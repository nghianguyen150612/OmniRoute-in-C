/*
 * OmniRoute native backend — bounded nonblocking socket receive tests
 * (Task 017).
 *
 * Loopback-only, self-cleaning, deterministic: no external network, no
 * fixed ports (ephemeral assignment everywhere; the operator port 20128 is
 * never touched). This is the first suite allowed to move application
 * payload; only the test client transmits, and production receives through
 * omni_recv_*. No HTTP-shaped bytes are used. Payloads are arbitrary binary
 * data, including NUL and non-ASCII values, so counts rather than strings
 * define correctness.
 *
 * Timing discipline: blocking client connects and bounded poller waits
 * observe pre-existing kernel state. No sleeps or packetization assumptions
 * exist. A send may be observed through several receives; every test either
 * accumulates by count or deliberately bounds the receive tail.
 *
 * Kernel-level EINTR on a nonblocking receive is not forced: production
 * flags must not be changed merely to manufacture signal timing. The
 * receive errno classifier is kept deliberately small and its mapping is
 * documented in the public contract; the actual nonblocking WOULD_BLOCK,
 * EOF, and fatal paths are exercised below.
 */

#define _GNU_SOURCE /* sockets visibility under strict C11 */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "omniroute/accepted.h"
#include "omniroute/bytebuf.h"
#include "omniroute/listener.h"
#include "omniroute/poller.h"
#include "omniroute/recv.h"

static size_t payload_sent_by_client = 0u;
static int check_count = 0;
static int failure_count = 0;

static void check(bool condition, const char *name) {
  ++check_count;
  if (condition) {
    printf("ok - %s\n", name);
  } else {
    ++failure_count;
    printf("NOT OK - %s\n", name);
  }
}

/*
 * Count this process's open descriptors. The directory handle used for the
 * census appears in its own listing and is subtracted; both sides count
 * symmetrically. Linux-only: other platforms skip the census honestly while
 * still running all functional assertions.
 */
static int count_open_fds(bool *supported) {
#ifdef __linux__
  DIR *directory = NULL;
  struct dirent *entry = NULL;
  int count = 0;

  directory = opendir("/proc/self/fd");
  if (directory == NULL) {
    *supported = false;
    return -1;
  }
  while ((entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    ++count;
  }
  (void)closedir(directory);
  *supported = true;
  return count - 1;
#else
  *supported = false;
  return -1;
#endif
}

static bool fd_open(int fd) {
  return fd >= 0 && fcntl(fd, F_GETFD) != -1;
}

/* Every test uses an ephemeral loopback port; 20128 is never touched. */
static bool start_listener(struct omni_listener *listener, uint16_t *port_out) {
  struct omni_listener_result result;

  omni_listener_make_inert(listener);
  result = omni_listener_init(listener, "127.0.0.1", (uint16_t)0u);
  check(result.status == OMNI_LISTENER_OK && result.sys_errno == 0,
        "listener binds for receive test setup");
  if (result.status != OMNI_LISTENER_OK) {
    return false;
  }
  check(omni_listener_port(listener) != OMNI_LISTENER_PORT_INVALID &&
            omni_listener_port(listener) != (uint16_t)20128,
        "receive tests use an ephemeral non-20128 port");
  if (port_out != NULL) {
    *port_out = omni_listener_port(listener);
  }
  return true;
}

/* A blocking loopback connect completes its handshake before returning. */
static int open_client(uint16_t port) {
  struct sockaddr_in peer;
  int client_fd = socket(AF_INET, SOCK_STREAM, 0);

  check(client_fd != OMNI_ACCEPTED_FD_INVALID, "test client socket opens");
  if (client_fd == OMNI_ACCEPTED_FD_INVALID) {
    return OMNI_ACCEPTED_FD_INVALID;
  }
  memset(&peer, 0, sizeof(peer));
  peer.sin_family = AF_INET;
  peer.sin_port = htons(port);
  peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(client_fd, (const struct sockaddr *)&peer, (socklen_t)sizeof(peer)) != 0) {
    check(false, "test client connects");
    (void)close(client_fd);
    return OMNI_ACCEPTED_FD_INVALID;
  }
  check(true, "test client connects");
  return client_fd;
}

static bool accept_one(struct omni_listener *listener, struct omni_accepted *accepted) {
  struct omni_accept_result result;

  omni_accepted_make_inert(accepted);
  result = omni_accept_once(listener, accepted);
  check(result.status == OMNI_ACCEPT_OK && result.accepted == 1u,
        "accepted owner opens for receive test setup");
  return result.status == OMNI_ACCEPT_OK;
}

/* Test-side transmission only. Production has no send path in this task. */
static bool client_send_all(int client_fd, const unsigned char *data, size_t length) {
  size_t sent = 0u;

  while (sent < length) {
    ssize_t result = send(client_fd, data + sent, length - sent, 0);

    if (result <= 0) {
      check(false, "test client transmits controlled payload");
      return false;
    }
    sent += (size_t)result;
  }
  payload_sent_by_client += sent;
  check(true, "test client transmits controlled payload");
  return true;
}

static bool make_buffer(struct omni_bytebuf *buffer, size_t capacity) {
  bool result = omni_bytebuf_init_owned(buffer, capacity);

  check(result, "receive byte buffer initializes");
  return result;
}

static bool make_poller(struct omni_poller *poller, struct pollfd *slots,
                        uint64_t *tokens, size_t capacity) {
  struct omni_poller_result result;

  result = omni_poller_init_borrowed(poller, slots, tokens, capacity);
  check(result.status == OMNI_POLLER_OK, "receive poller initializes");
  return result.status == OMNI_POLLER_OK;
}

/*
 * Observe READ readiness once and remove the registration. This helper never
 * reads payload bytes; the receive primitive remains the only socket reader.
 */
static bool await_readable(struct omni_poller *poller, int fd, uint64_t token) {
  struct omni_poller_event event[4];
  struct omni_poller_result add_result;
  struct omni_poller_result wait_result;
  struct omni_poller_result remove_result;

  add_result = omni_poller_add(poller, fd, token, OMNI_POLLER_INTEREST_READ);
  if (add_result.status != OMNI_POLLER_OK) {
    check(false, "receive readiness registration succeeds");
    return false;
  }
  wait_result = omni_poller_wait(poller, (int64_t)2000, event, 4u);
  remove_result = omni_poller_remove(poller, fd);
  check(remove_result.status == OMNI_POLLER_OK,
        "receive readiness registration is removed by caller");
  check(wait_result.status == OMNI_POLLER_OK && wait_result.count == 1u &&
            (event[0].ready & OMNI_POLLER_READY_READ) != 0u &&
            event[0].token == token,
        "receive readiness observes pending input");
  return wait_result.status == OMNI_POLLER_OK && wait_result.count == 1u &&
         (event[0].ready & OMNI_POLLER_READY_READ) != 0u;
}

static void test_invalid_arguments(void) {
  struct omni_accepted accepted;
  struct omni_bytebuf buffer;
  struct omni_recv_result result;

  omni_accepted_make_inert(&accepted);
  memset(&buffer, 0, sizeof(buffer));

  result = omni_recv_once(NULL, &buffer);
  check(result.status == OMNI_RECV_ERR_INVALID && result.sys_errno == EINVAL &&
            result.received == 0u,
        "one-shot rejects a NULL accepted owner");
  result = omni_recv_once(&accepted, NULL);
  check(result.status == OMNI_RECV_ERR_INVALID && result.received == 0u,
        "one-shot rejects a NULL byte buffer");
  result = omni_recv_once(&accepted, &buffer);
  check(result.status == OMNI_RECV_ERR_INVALID && result.received == 0u,
        "one-shot rejects an inert accepted owner and buffer");
  result = omni_recv_drain(NULL, &buffer, 4u);
  check(result.status == OMNI_RECV_ERR_INVALID && result.received == 0u,
        "drain rejects a NULL accepted owner");
  result = omni_recv_drain(&accepted, NULL, 4u);
  check(result.status == OMNI_RECV_ERR_INVALID && result.received == 0u,
        "drain rejects a NULL byte buffer");
  result = omni_recv_drain(&accepted, &buffer, 0u);
  check(result.status == OMNI_RECV_ERR_INVALID && result.received == 0u,
        "drain rejects a zero call limit");
  check(!omni_accepted_is_live(&accepted) && omni_bytebuf_capacity(&buffer) == 0u,
        "invalid receive calls publish no borrowed state");
}

static void test_empty_would_block(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted;
  struct omni_bytebuf buffer;
  struct omni_recv_result result;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  int accepted_fd = OMNI_ACCEPTED_FD_INVALID;
  unsigned char *backing = NULL;
  size_t capacity = 0u;
  size_t read_offset = 0u;
  size_t write_offset = 0u;
  size_t high_water = 0u;
  bool owns_backing = false;

  omni_accepted_make_inert(&accepted);
  memset(&buffer, 0, sizeof(buffer));
  omni_listener_make_inert(&listener);
  if (!start_listener(&listener, &port)) {
    goto cleanup;
  }
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID || !accept_one(&listener, &accepted) ||
      !make_buffer(&buffer, 32u)) {
    goto cleanup;
  }
  accepted_fd = omni_accepted_fd(&accepted);
  backing = buffer.backing;
  capacity = buffer.capacity;
  read_offset = buffer.read;
  write_offset = buffer.write;
  high_water = buffer.high_water;
  owns_backing = buffer.owns_backing;

  result = omni_recv_once(&accepted, &buffer);
  check(result.status == OMNI_RECV_WOULD_BLOCK,
        "empty nonblocking socket reports WOULD_BLOCK");
  check(result.sys_errno == EAGAIN || result.sys_errno == EWOULDBLOCK,
        "WOULD_BLOCK preserves the kernel errno");
  check(result.received == 0u, "WOULD_BLOCK commits zero bytes");
  check(buffer.backing == backing && buffer.capacity == capacity &&
            buffer.read == read_offset && buffer.write == write_offset &&
            buffer.high_water == high_water && buffer.owns_backing == owns_backing &&
            omni_bytebuf_readable(&buffer) == 0u && omni_bytebuf_writable(&buffer) == 32u,
        "WOULD_BLOCK leaves every byte-buffer field unchanged");
  check(omni_accepted_is_live(&accepted) && omni_accepted_fd(&accepted) == accepted_fd &&
            fd_open(accepted_fd),
        "WOULD_BLOCK leaves accepted ownership and FD unchanged");

  result = omni_recv_drain(&accepted, &buffer, 4u);
  check(result.status == OMNI_RECV_WOULD_BLOCK && result.received == 0u,
        "empty bounded drain stops at WOULD_BLOCK");

 cleanup:
  omni_bytebuf_destroy(&buffer);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(client_fd);
  }
  omni_listener_destroy(&listener);
}

static void test_binary_data(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted;
  struct omni_bytebuf buffer;
  struct omni_poller poller;
  struct pollfd slots[4];
  uint64_t tokens[4];
  struct omni_recv_result result = { OMNI_RECV_ERR_INVALID, EINVAL, 0u };
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  int accepted_fd = OMNI_ACCEPTED_FD_INVALID;
  static const unsigned char payload[] = {
    0x00u, 0xFFu, 0x68u, 0x01u, 0x7Fu, 0x80u, 0x00u, 0xFEu, 0x5Au, 0x02u,
  };
  size_t total = 0u;
  int attempts = 0;

  omni_accepted_make_inert(&accepted);
  memset(&buffer, 0, sizeof(buffer));
  memset(&poller, 0, sizeof(poller));
  omni_listener_make_inert(&listener);
  if (!start_listener(&listener, &port)) {
    goto cleanup;
  }
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID || !accept_one(&listener, &accepted) ||
      !make_buffer(&buffer, 32u) || !make_poller(&poller, slots, tokens, 4u)) {
    goto cleanup;
  }
  accepted_fd = omni_accepted_fd(&accepted);
  if (!client_send_all(client_fd, payload, sizeof(payload)) ||
      !await_readable(&poller, accepted_fd, (uint64_t)0xD47Au)) {
    goto cleanup;
  }

  for (attempts = 0; attempts < 8 && total < sizeof(payload); ++attempts) {
    size_t writable_before = omni_bytebuf_writable(&buffer);

    result = omni_recv_once(&accepted, &buffer);
    if (result.status == OMNI_RECV_WOULD_BLOCK) {
      if (!await_readable(&poller, accepted_fd,
                          (uint64_t)0xD47Au + (uint64_t)attempts + UINT64_C(1))) {
        goto cleanup;
      }
      continue;
    }
    if (result.status != OMNI_RECV_DATA || result.received == 0u ||
        result.received > writable_before || result.sys_errno != 0) {
      check(false, "positive receive publishes only bounded DATA");
      goto cleanup;
    }
    total += result.received;
  }
  check(total == sizeof(payload), "binary receive reports the exact byte count");
  check(result.status == OMNI_RECV_DATA && result.sys_errno == 0,
        "positive receive reports DATA with zero errno");
  {
    size_t readable = 0u;
    const unsigned char *view = omni_bytebuf_read_ptr(&buffer, &readable);

    check(view != NULL && readable == sizeof(payload) &&
              memcmp(view, payload, sizeof(payload)) == 0,
          "binary receive preserves NUL and non-ASCII bytes exactly");
  }
  check(omni_bytebuf_readable(&buffer) == sizeof(payload),
        "readable count advances through commit");
  check(omni_bytebuf_high_water(&buffer) == sizeof(payload),
        "high-water advances through commit");
  check(omni_accepted_is_live(&accepted) && omni_accepted_fd(&accepted) == accepted_fd &&
            fd_open(accepted_fd),
        "DATA leaves the accepted owner live with the same FD");
  check(buffer.owns_backing && buffer.backing != NULL && buffer.capacity == 32u,
        "DATA leaves byte-buffer backing ownership unchanged");
  result = omni_recv_once(&accepted, &buffer);
  check(result.status == OMNI_RECV_WOULD_BLOCK,
        "socket with all payload consumed returns WOULD_BLOCK");

 cleanup:
  omni_poller_destroy(&poller);
  omni_bytebuf_destroy(&buffer);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(client_fd);
  }
  omni_listener_destroy(&listener);
}

static void test_buffer_full_preserves_pending(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted;
  struct omni_bytebuf buffer;
  struct omni_poller poller;
  struct pollfd slots[4];
  uint64_t tokens[4];
  struct omni_recv_result result = { OMNI_RECV_ERR_INVALID, EINVAL, 0u };
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  int accepted_fd = OMNI_ACCEPTED_FD_INVALID;
  unsigned char payload[20];
  size_t i = 0u;
  size_t total = 0u;
  int attempts = 0;

  for (i = 0u; i < sizeof(payload); ++i) {
    payload[i] = (unsigned char)((i * 37u + 11u) & 0xFFu);
  }
  omni_accepted_make_inert(&accepted);
  memset(&buffer, 0, sizeof(buffer));
  memset(&poller, 0, sizeof(poller));
  omni_listener_make_inert(&listener);
  if (!start_listener(&listener, &port)) {
    goto cleanup;
  }
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID || !accept_one(&listener, &accepted) ||
      !make_buffer(&buffer, 8u) || !make_poller(&poller, slots, tokens, 4u)) {
    goto cleanup;
  }
  accepted_fd = omni_accepted_fd(&accepted);
  if (!client_send_all(client_fd, payload, sizeof(payload)) ||
      !await_readable(&poller, accepted_fd, (uint64_t)0xB00Fu)) {
    goto cleanup;
  }
  result = omni_recv_once(&accepted, &buffer);
  check(result.status == OMNI_RECV_DATA && result.received == 8u,
        "writable-tail bound forces an eight-byte partial receive");
  check(omni_bytebuf_readable(&buffer) == 8u && omni_bytebuf_writable(&buffer) == 0u,
        "partial receive fills the byte buffer exactly");
  check(memcmp(omni_bytebuf_read_ptr(&buffer, NULL), payload, 8u) == 0,
        "partial receive keeps the stream prefix in order");

  result = omni_recv_once(&accepted, &buffer);
  check(result.status == OMNI_RECV_BUFFER_FULL && result.sys_errno == 0 &&
            result.received == 0u,
        "full buffer returns BUFFER_FULL without a receive attempt");
  check(omni_bytebuf_readable(&buffer) == 8u && omni_bytebuf_writable(&buffer) == 0u,
        "BUFFER_FULL leaves byte-buffer state unchanged");

  /* The second phase proves the BUFFER_FULL call did not consume the tail. */
  omni_bytebuf_destroy(&buffer);
  if (!make_buffer(&buffer, 16u) ||
      !await_readable(&poller, accepted_fd, (uint64_t)0xB010u)) {
    goto cleanup;
  }
  total = 0u;
  for (attempts = 0; attempts < 8; ++attempts) {
    result = omni_recv_once(&accepted, &buffer);
    if (result.status == OMNI_RECV_DATA) {
      total += result.received;
      continue;
    }
    if (result.status == OMNI_RECV_WOULD_BLOCK) {
      break;
    }
    check(false, "pending bytes stop only at DATA or WOULD_BLOCK");
    goto cleanup;
  }
  check(result.status == OMNI_RECV_WOULD_BLOCK && total == 12u,
        "pending bytes survive BUFFER_FULL and drain later");
  check(omni_bytebuf_readable(&buffer) == 12u &&
            memcmp(omni_bytebuf_read_ptr(&buffer, NULL), payload + 8u, 12u) == 0,
        "preserved pending bytes retain exact stream order");

 cleanup:
  omni_poller_destroy(&poller);
  omni_bytebuf_destroy(&buffer);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(client_fd);
  }
  omni_listener_destroy(&listener);
}

static void test_reclaimable_prefix_requires_compact(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted;
  struct omni_bytebuf buffer;
  struct omni_poller poller;
  struct pollfd slots[4];
  uint64_t tokens[4];
  struct omni_recv_result result = { OMNI_RECV_ERR_INVALID, EINVAL, 0u };
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  int accepted_fd = OMNI_ACCEPTED_FD_INVALID;
  unsigned char first[10];
  unsigned char second[12];
  unsigned char expected[16];
  size_t first_received = 0u;
  size_t second_received = 0u;
  size_t i = 0u;
  int attempts = 0;

  for (i = 0u; i < sizeof(first); ++i) {
    first[i] = (unsigned char)((i * 53u + 3u) & 0xFFu);
  }
  for (i = 0u; i < sizeof(second); ++i) {
    second[i] = (unsigned char)((i * 29u + 91u) & 0xFFu);
  }
  memcpy(expected, first + 6u, 4u);
  memcpy(expected + 4u, second, sizeof(second));

  omni_accepted_make_inert(&accepted);
  memset(&buffer, 0, sizeof(buffer));
  memset(&poller, 0, sizeof(poller));
  omni_listener_make_inert(&listener);
  if (!start_listener(&listener, &port)) {
    goto cleanup;
  }
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID || !accept_one(&listener, &accepted) ||
      !make_buffer(&buffer, 16u) || !make_poller(&poller, slots, tokens, 4u)) {
    goto cleanup;
  }
  accepted_fd = omni_accepted_fd(&accepted);
  if (!client_send_all(client_fd, first, sizeof(first)) ||
      !await_readable(&poller, accepted_fd, (uint64_t)0xC001u)) {
    goto cleanup;
  }
  for (attempts = 0; attempts < 8 && first_received < sizeof(first); ++attempts) {
    result = omni_recv_once(&accepted, &buffer);
    if (result.status == OMNI_RECV_DATA) {
      first_received += result.received;
    } else if (result.status == OMNI_RECV_WOULD_BLOCK) {
      if (!await_readable(&poller, accepted_fd,
                          (uint64_t)0xC001u + (uint64_t)attempts + UINT64_C(1))) {
        goto cleanup;
      }
    } else {
      check(false, "first reclaimable-prefix block receives as DATA");
      goto cleanup;
    }
  }
  check(first_received == sizeof(first), "first block accumulates by byte count");
  check(omni_bytebuf_consume(&buffer, 6u), "test consumes a prefix without compacting");
  check(omni_bytebuf_readable(&buffer) == 4u && omni_bytebuf_writable(&buffer) == 6u &&
            omni_bytebuf_reclaimable(&buffer) == 6u,
        "reclaimable prefix and contiguous tail remain distinct");

  if (!client_send_all(client_fd, second, sizeof(second)) ||
      !await_readable(&poller, accepted_fd, (uint64_t)0xC002u)) {
    goto cleanup;
  }
  for (attempts = 0; attempts < 8 && second_received < sizeof(second); ++attempts) {
    size_t writable_before = omni_bytebuf_writable(&buffer);

    result = omni_recv_once(&accepted, &buffer);
    if (result.status == OMNI_RECV_DATA) {
      check(result.received <= writable_before,
            "receive never exceeds the reclaimable buffer tail");
      second_received += result.received;
    } else if (result.status == OMNI_RECV_WOULD_BLOCK) {
      if (!await_readable(&poller, accepted_fd,
                          (uint64_t)0xC002u + (uint64_t)attempts + UINT64_C(1))) {
        goto cleanup;
      }
    } else {
      break;
    }
  }
  check(second_received == 6u, "tail bound receives only six second-block bytes");
  result = omni_recv_once(&accepted, &buffer);
  check(result.status == OMNI_RECV_BUFFER_FULL && omni_bytebuf_reclaimable(&buffer) == 6u,
        "reclaimable prefix does not trigger automatic compaction");

  omni_bytebuf_compact(&buffer);
  check(omni_bytebuf_writable(&buffer) == 6u && omni_bytebuf_reclaimable(&buffer) == 0u,
        "explicit compact reopens the writable tail");
  if (!await_readable(&poller, accepted_fd, (uint64_t)0xC003u)) {
    goto cleanup;
  }
  for (attempts = 0; attempts < 8 && second_received < sizeof(second); ++attempts) {
    result = omni_recv_once(&accepted, &buffer);
    if (result.status == OMNI_RECV_DATA) {
      second_received += result.received;
    } else if (result.status == OMNI_RECV_WOULD_BLOCK) {
      if (!await_readable(&poller, accepted_fd,
                          (uint64_t)0xC003u + (uint64_t)attempts + UINT64_C(1))) {
        goto cleanup;
      }
    } else {
      check(false, "post-compact receive obtains the remaining bytes");
      goto cleanup;
    }
  }
  check(second_received == sizeof(second),
        "explicit compact permits the remaining second-block bytes");
  {
    size_t readable = 0u;
    const unsigned char *view = omni_bytebuf_read_ptr(&buffer, &readable);

    check(readable == sizeof(expected) && memcmp(view, expected, sizeof(expected)) == 0,
          "consume, compact, and receive preserve byte ordering");
  }

 cleanup:
  omni_poller_destroy(&poller);
  omni_bytebuf_destroy(&buffer);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(client_fd);
  }
  omni_listener_destroy(&listener);
}

static void test_eof_preserves_buffered_bytes(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted;
  struct omni_bytebuf buffer;
  struct omni_poller poller;
  struct pollfd slots[4];
  uint64_t tokens[4];
  struct omni_recv_result result = { OMNI_RECV_ERR_INVALID, EINVAL, 0u };
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  int accepted_fd = OMNI_ACCEPTED_FD_INVALID;
  static const unsigned char payload[] = {
    0xDEu, 0x00u, 0xADu, 0xBEu, 0xEFu, 0x00u, 0x01u, 0xFFu, 0x42u,
  };
  size_t total = 0u;
  int attempts = 0;

  omni_accepted_make_inert(&accepted);
  memset(&buffer, 0, sizeof(buffer));
  memset(&poller, 0, sizeof(poller));
  omni_listener_make_inert(&listener);
  if (!start_listener(&listener, &port)) {
    goto cleanup;
  }
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID || !accept_one(&listener, &accepted) ||
      !make_buffer(&buffer, 32u) || !make_poller(&poller, slots, tokens, 4u)) {
    goto cleanup;
  }
  accepted_fd = omni_accepted_fd(&accepted);
  if (!client_send_all(client_fd, payload, sizeof(payload))) {
    goto cleanup;
  }
  check(shutdown(client_fd, SHUT_WR) == 0, "test client performs a write half-close");
  if (!await_readable(&poller, accepted_fd, (uint64_t)0xE0Fu)) {
    goto cleanup;
  }
  for (attempts = 0; attempts < 8; ++attempts) {
    result = omni_recv_once(&accepted, &buffer);
    if (result.status == OMNI_RECV_DATA) {
      total += result.received;
      continue;
    }
    if (result.status == OMNI_RECV_WOULD_BLOCK) {
      if (!await_readable(&poller, accepted_fd,
                          (uint64_t)0xE0Fu + (uint64_t)attempts + UINT64_C(1))) {
        goto cleanup;
      }
      continue;
    }
    break;
  }
  check(total == sizeof(payload), "EOF test receives every byte before shutdown");
  check(result.status == OMNI_RECV_EOF && result.sys_errno == 0 && result.received == 0u,
        "orderly write shutdown surfaces explicit EOF");
  {
    size_t readable = 0u;
    const unsigned char *view = omni_bytebuf_read_ptr(&buffer, &readable);

    check(readable == sizeof(payload) && memcmp(view, payload, sizeof(payload)) == 0,
          "EOF preserves already-buffered binary bytes");
  }
  {
    unsigned char *backing = buffer.backing;
    size_t read_offset = buffer.read;
    size_t write_offset = buffer.write;
    size_t high_water = buffer.high_water;

    result = omni_recv_once(&accepted, &buffer);
    check(result.status == OMNI_RECV_EOF && buffer.backing == backing &&
              buffer.read == read_offset && buffer.write == write_offset &&
              buffer.high_water == high_water,
          "repeated EOF changes neither ownership nor byte-buffer state");
  }
  check(omni_accepted_is_live(&accepted) && omni_accepted_fd(&accepted) == accepted_fd &&
            fd_open(accepted_fd),
        "EOF does not close or destroy the accepted owner");

 cleanup:
  omni_poller_destroy(&poller);
  omni_bytebuf_destroy(&buffer);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(client_fd);
  }
  omni_listener_destroy(&listener);
}

static void test_fatal_reset_preserves_ownership(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted;
  struct omni_bytebuf buffer;
  struct omni_poller poller;
  struct pollfd slots[4];
  uint64_t tokens[4];
  struct omni_poller_event event[4];
  struct omni_recv_result result = { OMNI_RECV_ERR_INVALID, EINVAL, 0u };
  struct omni_poller_result wait_result;
  struct linger linger_value;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  int accepted_fd = OMNI_ACCEPTED_FD_INVALID;

  omni_accepted_make_inert(&accepted);
  memset(&buffer, 0, sizeof(buffer));
  memset(&poller, 0, sizeof(poller));
  omni_listener_make_inert(&listener);
  if (!start_listener(&listener, &port)) {
    goto cleanup;
  }
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID || !accept_one(&listener, &accepted) ||
      !make_buffer(&buffer, 32u) || !make_poller(&poller, slots, tokens, 4u)) {
    goto cleanup;
  }
  accepted_fd = omni_accepted_fd(&accepted);
  memset(&linger_value, 0, sizeof(linger_value));
  linger_value.l_onoff = 1;
  linger_value.l_linger = 0;
  check(setsockopt(client_fd, SOL_SOCKET, SO_LINGER, &linger_value,
                   (socklen_t)sizeof(linger_value)) == 0,
        "test client arms a controlled reset");
  (void)close(client_fd);
  client_fd = OMNI_ACCEPTED_FD_INVALID;
  check(omni_poller_add(&poller, accepted_fd, (uint64_t)0xF00Du,
                        OMNI_POLLER_INTEREST_READ).status == OMNI_POLLER_OK,
        "reset probe registers the accepted FD");
  wait_result = omni_poller_wait(&poller, (int64_t)2000, event, 4u);
  check(wait_result.status == OMNI_POLLER_OK && wait_result.count == 1u,
        "controlled reset produces readiness");
  (void)omni_poller_remove(&poller, accepted_fd);

  result = omni_recv_once(&accepted, &buffer);
  check(result.status == OMNI_RECV_ERR_FATAL && result.sys_errno != 0 &&
            result.received == 0u,
        "controlled reset surfaces FATAL with captured errno");
  check(omni_bytebuf_readable(&buffer) == 0u && omni_bytebuf_writable(&buffer) == 32u,
        "FATAL leaves the byte buffer unchanged");
  check(omni_accepted_is_live(&accepted) && omni_accepted_fd(&accepted) == accepted_fd &&
            fd_open(accepted_fd),
        "FATAL leaves accepted-FD ownership with the caller");

 cleanup:
  omni_poller_destroy(&poller);
  omni_bytebuf_destroy(&buffer);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(client_fd);
  }
  omni_listener_destroy(&listener);
}

static void test_poller_receive_integration(void) {
  struct omni_listener listener = { 0 };
  struct omni_poller poller;
  struct pollfd slots[4];
  uint64_t tokens[4];
  struct omni_poller_event event[4];
  struct omni_poller_result result;
  struct omni_accepted accepted;
  struct omni_bytebuf buffer;
  struct omni_recv_result receive_result = { OMNI_RECV_ERR_INVALID, EINVAL, 0u };
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int listener_fd = OMNI_LISTENER_FD_INVALID;
  int accepted_fd = OMNI_ACCEPTED_FD_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  static const unsigned char payload[] = {
    0x10u, 0x00u, 0x20u, 0xFFu, 0x30u, 0x00u,
    0x40u, 0x80u, 0x50u, 0x01u, 0x60u, 0x7Fu,
  };
  size_t total = 0u;
  int attempts = 0;

  omni_accepted_make_inert(&accepted);
  memset(&buffer, 0, sizeof(buffer));
  memset(&poller, 0, sizeof(poller));
  omni_listener_make_inert(&listener);
  if (!start_listener(&listener, &port) ||
      !make_poller(&poller, slots, tokens, 4u)) {
    goto cleanup;
  }
  listener_fd = omni_listener_fd(&listener);
  result = omni_poller_add(&poller, listener_fd, (uint64_t)0x1EC5u,
                           OMNI_POLLER_INTEREST_READ);
  check(result.status == OMNI_POLLER_OK, "listener FD registers as a borrowed poller entry");
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID) {
    goto cleanup;
  }
  result = omni_poller_wait(&poller, (int64_t)2000, event, 4u);
  check(result.status == OMNI_POLLER_OK && result.count == 1u &&
            (event[0].ready & OMNI_POLLER_READY_READ) != 0u,
        "poller observes listener readiness before acceptance");
  omni_accepted_make_inert(&accepted);
  check(omni_accept_once(&listener, &accepted).status == OMNI_ACCEPT_OK,
        "listener readiness is dispatched to accepted owner");
  accepted_fd = omni_accepted_fd(&accepted);
  check(omni_poller_remove(&poller, listener_fd).status == OMNI_POLLER_OK,
        "listener registration is removed before accepted registration");
  check(omni_poller_add(&poller, accepted_fd, (uint64_t)0x8EADu,
                        OMNI_POLLER_INTEREST_READ).status == OMNI_POLLER_OK,
        "accepted FD registers as a borrowed poller entry");
  if (!make_buffer(&buffer, 32u)) {
    goto cleanup;
  }
  result = omni_poller_wait(&poller, (int64_t)0, event, 4u);
  check(result.status == OMNI_POLLER_OK && result.count == 0u,
        "accepted FD is not readable before payload");
  if (!client_send_all(client_fd, payload, sizeof(payload))) {
    goto cleanup;
  }
  result = omni_poller_wait(&poller, (int64_t)2000, event, 4u);
  check(result.status == OMNI_POLLER_OK && result.count == 1u &&
            (event[0].ready & OMNI_POLLER_READY_READ) != 0u &&
            event[0].token == (uint64_t)0x8EADu,
        "accepted FD becomes readable for binary payload");
  for (attempts = 0; attempts < 8 && total < sizeof(payload); ++attempts) {
    receive_result = omni_recv_once(&accepted, &buffer);
    if (receive_result.status == OMNI_RECV_DATA) {
      total += receive_result.received;
    } else if (receive_result.status == OMNI_RECV_WOULD_BLOCK) {
      result = omni_poller_wait(&poller, (int64_t)2000, event, 4u);
      if (result.status != OMNI_POLLER_OK || result.count == 0u) {
        break;
      }
    } else {
      break;
    }
  }
  check(total == sizeof(payload), "poller-to-receive path commits the exact byte count");
  check(memcmp(omni_bytebuf_read_ptr(&buffer, NULL), payload, sizeof(payload)) == 0,
        "poller-to-receive path preserves exact binary bytes");
  check(omni_poller_count(&poller) == 1u,
        "receive does not register or remove poller entries");
  check(omni_poller_remove(&poller, accepted_fd).status == OMNI_POLLER_OK,
        "caller removes accepted registration before lifecycle teardown");
  omni_poller_destroy(&poller);
  check(fd_open(listener_fd) && fd_open(accepted_fd),
        "poller destroy closes neither borrowed descriptor");
  omni_accepted_destroy(&accepted);
  check(!fd_open(accepted_fd) && fd_open(listener_fd),
        "accepted destroy closes only its owned descriptor");

 cleanup:
  omni_poller_destroy(&poller);
  omni_bytebuf_destroy(&buffer);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(client_fd);
  }
  omni_listener_destroy(&listener);
}

static void test_bounded_drain(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted;
  struct omni_bytebuf buffer;
  struct omni_poller poller;
  struct pollfd slots[4];
  uint64_t tokens[4];
  struct omni_recv_result result = { OMNI_RECV_ERR_INVALID, EINVAL, 0u };
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  int accepted_fd = OMNI_ACCEPTED_FD_INVALID;
  static const unsigned char payload[] = {
    0xA0u, 0x00u, 0xA1u, 0xA2u, 0xA3u, 0xFFu, 0xA4u, 0x80u, 0xA5u, 0xA6u,
  };
  static const unsigned char eof_payload[] = { 0x55u, 0x00u, 0xAAu, 0xFEu, 0x7Fu };

  omni_accepted_make_inert(&accepted);
  memset(&buffer, 0, sizeof(buffer));
  memset(&poller, 0, sizeof(poller));
  omni_listener_make_inert(&listener);
  if (!start_listener(&listener, &port)) {
    goto cleanup;
  }
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID || !accept_one(&listener, &accepted) ||
      !make_buffer(&buffer, 4u) || !make_poller(&poller, slots, tokens, 4u)) {
    goto cleanup;
  }
  accepted_fd = omni_accepted_fd(&accepted);
  result = omni_recv_drain(&accepted, &buffer, 8u);
  check(result.status == OMNI_RECV_WOULD_BLOCK && result.received == 0u,
        "empty bounded drain stops at WOULD_BLOCK");

  if (!client_send_all(client_fd, payload, sizeof(payload)) ||
      !await_readable(&poller, accepted_fd, (uint64_t)0xD001u)) {
    goto cleanup;
  }
  result = omni_recv_drain(&accepted, &buffer, 1u);
  check(result.status == OMNI_RECV_LIMIT_REACHED && result.received == 4u &&
            result.sys_errno == 0,
        "bounded drain stops at one explicit call with partial success");
  check(memcmp(omni_bytebuf_read_ptr(&buffer, NULL), payload, 4u) == 0,
        "limit-reached drain keeps its committed prefix");

  omni_bytebuf_reset(&buffer);
  if (!await_readable(&poller, accepted_fd, (uint64_t)0xD002u)) {
    goto cleanup;
  }
  result = omni_recv_drain(&accepted, &buffer, 8u);
  check(result.status == OMNI_RECV_BUFFER_FULL && result.received == 4u,
        "bounded drain reports BUFFER_FULL with partial-success total");
  check(memcmp(omni_bytebuf_read_ptr(&buffer, NULL), payload + 4u, 4u) == 0,
        "buffer-full drain keeps the next committed stream segment");

  omni_bytebuf_reset(&buffer);
  if (!await_readable(&poller, accepted_fd, (uint64_t)0xD003u)) {
    goto cleanup;
  }
  result = omni_recv_drain(&accepted, &buffer, 8u);
  check(result.status == OMNI_RECV_WOULD_BLOCK && result.received == 2u,
        "bounded drain reports WOULD_BLOCK after partial success");
  check(memcmp(omni_bytebuf_read_ptr(&buffer, NULL), payload + 8u, 2u) == 0,
        "would-block drain preserves its final committed segment");

  omni_bytebuf_destroy(&buffer);
  if (!make_buffer(&buffer, 32u) ||
      !client_send_all(client_fd, eof_payload, sizeof(eof_payload))) {
    goto cleanup;
  }
  check(shutdown(client_fd, SHUT_WR) == 0, "bounded-drain client performs a write half-close");
  if (!await_readable(&poller, accepted_fd, (uint64_t)0xD004u)) {
    goto cleanup;
  }
  result = omni_recv_drain(&accepted, &buffer, 8u);
  check(result.status == OMNI_RECV_EOF && result.received == sizeof(eof_payload),
        "bounded drain reports EOF after retaining prior DATA");
  check(memcmp(omni_bytebuf_read_ptr(&buffer, NULL), eof_payload, sizeof(eof_payload)) == 0,
        "bounded drain preserves bytes before EOF");

 cleanup:
  omni_poller_destroy(&poller);
  omni_bytebuf_destroy(&buffer);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(client_fd);
  }
  omni_listener_destroy(&listener);
}

static void test_receive_stress(void) {
  enum { STREAM_LENGTH = 256u, CHUNK_LENGTH = 37u, CAPACITY = 16u };
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted;
  struct omni_bytebuf buffer;
  struct omni_poller poller;
  struct pollfd slots[4];
  uint64_t tokens[4];
  struct omni_recv_result result = { OMNI_RECV_ERR_INVALID, EINVAL, 0u };
  unsigned char expected[STREAM_LENGTH];
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  int accepted_fd = OMNI_ACCEPTED_FD_INVALID;
  size_t offset = 0u;
  size_t consumed = 0u;
  size_t i = 0u;
  bool census_supported = false;
  int census_before = count_open_fds(&census_supported);

  for (i = 0u; i < sizeof(expected); ++i) {
    expected[i] = (unsigned char)((i * 31u + 7u) & 0xFFu);
  }
  omni_accepted_make_inert(&accepted);
  memset(&buffer, 0, sizeof(buffer));
  memset(&poller, 0, sizeof(poller));
  omni_listener_make_inert(&listener);
  if (!start_listener(&listener, &port)) {
    goto cleanup;
  }
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID || !accept_one(&listener, &accepted) ||
      !make_buffer(&buffer, CAPACITY) || !make_poller(&poller, slots, tokens, 4u)) {
    goto cleanup;
  }
  accepted_fd = omni_accepted_fd(&accepted);

  for (offset = 0u; offset < sizeof(expected); offset += CHUNK_LENGTH) {
    size_t remaining = sizeof(expected) - offset;
    size_t chunk = remaining < CHUNK_LENGTH ? remaining : CHUNK_LENGTH;
    size_t target = consumed + chunk;
    int attempts = 0;

    if (!client_send_all(client_fd, expected + offset, chunk) ||
        !await_readable(&poller, accepted_fd, (uint64_t)(0x5700u + offset))) {
      goto cleanup;
    }
    while (consumed < target && attempts < 16) {
      size_t readable_before = omni_bytebuf_readable(&buffer);

      result = omni_recv_once(&accepted, &buffer);
      if (result.status == OMNI_RECV_DATA) {
        size_t readable = 0u;
        const unsigned char *view = omni_bytebuf_read_ptr(&buffer, &readable);

        check(readable_before == 0u && view != NULL && result.received <= target - consumed &&
                  readable == result.received &&
                  memcmp(view, expected + consumed, result.received) == 0,
              "stress verifies each bounded receive in stream order");
        check(result.received > 0u && result.received <= CAPACITY &&
                  buffer.read <= buffer.write && buffer.write <= buffer.capacity &&
                  omni_bytebuf_writable(&buffer) <= CAPACITY &&
                  omni_bytebuf_reclaimable(&buffer) <= CAPACITY,
              "stress preserves byte-buffer accounting invariants");
        check(omni_accepted_is_live(&accepted) &&
                  omni_accepted_fd(&accepted) == accepted_fd && fd_open(accepted_fd),
              "stress keeps accepted ownership live");
        check(omni_bytebuf_consume(&buffer, result.received),
              "stress consumes only the bytes just verified");
        consumed += result.received;
      } else if (result.status == OMNI_RECV_WOULD_BLOCK) {
        if (!await_readable(&poller, accepted_fd,
                            (uint64_t)0x5700u + (uint64_t)offset +
                              (uint64_t)attempts + UINT64_C(1))) {
          goto cleanup;
        }
      } else {
        check(false, "stress receive stops only at DATA or WOULD_BLOCK");
        goto cleanup;
      }
      ++attempts;
    }
    check(consumed == target, "stress drains each controlled chunk exactly");
    if (consumed != target) {
      goto cleanup;
    }
    omni_bytebuf_compact(&buffer);
  }
  check(consumed == sizeof(expected), "stress consumes the complete known stream");
  check(shutdown(client_fd, SHUT_WR) == 0, "stress client performs a write half-close");
  if (!await_readable(&poller, accepted_fd, (uint64_t)0x5E0Fu)) {
    goto cleanup;
  }
  result = omni_recv_once(&accepted, &buffer);
  check(result.status == OMNI_RECV_EOF && result.received == 0u,
        "stress ends with EOF after all bytes are delivered");

 cleanup:
  omni_poller_destroy(&poller);
  omni_bytebuf_destroy(&buffer);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(client_fd);
  }
  omni_listener_destroy(&listener);
  if (census_supported) {
    check(count_open_fds(&census_supported) == census_before,
          "receive stress returns descriptor census to baseline");
  } else {
    check(true, "receive stress descriptor census skipped outside Linux");
  }
}

int main(void) {
  bool census_supported = false;
  int census_before = count_open_fds(&census_supported);

  test_invalid_arguments();
  test_empty_would_block();
  test_binary_data();
  test_buffer_full_preserves_pending();
  test_reclaimable_prefix_requires_compact();
  test_eof_preserves_buffered_bytes();
  test_fatal_reset_preserves_ownership();
  test_poller_receive_integration();
  test_bounded_drain();
  test_receive_stress();

  check(payload_sent_by_client > 0u,
        "controlled test-client payload actually reached production receive");
  if (census_supported) {
    check(count_open_fds(&census_supported) == census_before,
          "complete receive suite returns descriptor census to baseline");
  } else {
    check(true, "complete receive-suite descriptor census skipped outside Linux");
  }
  printf("---\n%d checks, %d failures\n", check_count, failure_count);
  return failure_count == 0 ? 0 : 1;
}
