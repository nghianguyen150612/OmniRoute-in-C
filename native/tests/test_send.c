/*
 * OmniRoute native backend — bounded nonblocking socket send tests
 * (Task 018).
 *
 * Loopback-only, self-cleaning, deterministic: the test client receives
 * through controlled test-side recv() calls, while production output always
 * goes through omni_send_*. No external network, no fixed ports (the
 * operator port 20128 is never touched), no HTTP-shaped payloads, and no
 * production server loop. Payloads are arbitrary binary data, including NUL
 * and bytes at or above 0x80.
 *
 * Timing discipline: bounded poll() waits observe pre-existing kernel state;
 * no sleeps or TCP packetization assumptions exist. Backpressure uses a
 * test-only send-buffer limit and a caller-controlled finite send budget.
 * Peer-failure SIGPIPE proof is isolated in a child process so a broken
 * production flag cannot terminate the main CTest runner.
 */

#define _GNU_SOURCE /* sockets, fork, and MSG_NOSIGNAL visibility in tests */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "omniroute/accepted.h"
#include "omniroute/listener.h"
#include "omniroute/poller.h"
#include "omniroute/send.h"

#define LARGE_PAYLOAD_SIZE (1024u * 1024u)
#define TRANSFER_PAYLOAD_SIZE (256u * 1024u)
#define STRESS_ITERATIONS 32u

static unsigned char large_payload[LARGE_PAYLOAD_SIZE];
static unsigned char large_snapshot[LARGE_PAYLOAD_SIZE];
static unsigned char transfer_payload[TRANSFER_PAYLOAD_SIZE];
static unsigned char transfer_snapshot[TRANSFER_PAYLOAD_SIZE];

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

static bool accepted_flags(int fd, int *status_flags, int *descriptor_flags) {
  int status = fcntl(fd, F_GETFL);
  int descriptor = fcntl(fd, F_GETFD);

  if (status == -1 || descriptor == -1) {
    return false;
  }
  if (status_flags != NULL) {
    *status_flags = status;
  }
  if (descriptor_flags != NULL) {
    *descriptor_flags = descriptor;
  }
  return (status & O_NONBLOCK) != 0 && (descriptor & FD_CLOEXEC) != 0;
}

/* Every test uses an ephemeral loopback port; 20128 is never touched. */
static bool start_listener(struct omni_listener *listener, uint16_t *port_out) {
  struct omni_listener_result result;

  omni_listener_make_inert(listener);
  result = omni_listener_init(listener, "127.0.0.1", (uint16_t)0u);
  check(result.status == OMNI_LISTENER_OK && result.sys_errno == 0,
        "listener binds for send test setup");
  if (result.status != OMNI_LISTENER_OK) {
    return false;
  }
  check(omni_listener_port(listener) != OMNI_LISTENER_PORT_INVALID &&
            omni_listener_port(listener) != (uint16_t)20128,
        "send tests use an ephemeral non-20128 port");
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
        "accepted owner opens for send test setup");
  return result.status == OMNI_ACCEPT_OK;
}

static bool setup_connection(struct omni_listener *listener, struct omni_accepted *accepted,
                             int *client_out) {
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;

  omni_listener_make_inert(listener);
  omni_accepted_make_inert(accepted);
  if (!start_listener(listener, &port)) {
    return false;
  }
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID || !accept_one(listener, accepted)) {
    if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
      (void)close(client_fd);
    }
    omni_accepted_destroy(accepted);
    omni_listener_destroy(listener);
    return false;
  }
  if (client_out != NULL) {
    *client_out = client_fd;
  }
  return true;
}

static void cleanup_connection(struct omni_listener *listener,
                               struct omni_accepted *accepted, int *client_fd) {
  omni_accepted_destroy(accepted);
  if (client_fd != NULL && *client_fd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(*client_fd);
    *client_fd = OMNI_ACCEPTED_FD_INVALID;
  }
  omni_listener_destroy(listener);
}

static void fill_payload(unsigned char *payload, size_t length, unsigned int salt) {
  size_t i = 0u;

  for (i = 0u; i < length; ++i) {
    payload[i] = (unsigned char)((i * 37u + (size_t)salt * 11u + 0x80u) & 0xffu);
  }
  if (length > 0u) {
    payload[0] = 0x00u;
  }
  if (length > 1u) {
    payload[1] = 0x80u;
  }
  if (length > 2u) {
    payload[2] = 0xffu;
  }
}

static bool set_send_buffer(int fd, int bytes) {
  int value = bytes;

  return setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &value, (socklen_t)sizeof(value)) == 0;
}

static bool set_nonblocking_for_test(int fd) {
  int flags = fcntl(fd, F_GETFL);

  if (flags == -1) {
    return false;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/* Receive exactly one expected byte range through the test client. */
static bool client_receive_expected(int client_fd, const unsigned char *expected,
                                    size_t length) {
  unsigned char chunk[8192];
  size_t received = 0u;
  size_t waits = 0u;

  while (received < length && waits < 4096u) {
    struct pollfd probe;
    int ready = 0;
    size_t remaining = length - received;
    size_t request = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
    ssize_t got = 0;

    probe.fd = client_fd;
    probe.events = POLLIN;
    probe.revents = 0;
    ready = poll(&probe, (nfds_t)1, 2000);
    if (ready <= 0 || (probe.revents & (POLLIN | POLLERR | POLLHUP)) == 0) {
      return false;
    }
    got = recv(client_fd, chunk, request, 0);
    if (got <= 0) {
      return false;
    }
    if ((size_t)got > remaining ||
        memcmp(expected + received, chunk, (size_t)got) != 0) {
      return false;
    }
    received += (size_t)got;
    ++waits;
  }
  return received == length;
}

static bool client_has_no_pending_data(int client_fd) {
  struct pollfd probe;
  int ready = 0;

  probe.fd = client_fd;
  probe.events = POLLIN;
  probe.revents = 0;
  ready = poll(&probe, (nfds_t)1, 0);
  return ready == 0;
}

static bool wait_client_readable(int client_fd) {
  struct pollfd probe;
  int ready = 0;

  probe.fd = client_fd;
  probe.events = POLLIN;
  probe.revents = 0;
  ready = poll(&probe, (nfds_t)1, 2000);
  return ready > 0 && (probe.revents & (POLLIN | POLLERR | POLLHUP)) != 0;
}

/* Drain a nonblocking test client while checking stream order. */
static bool drain_client_checked(int client_fd, const unsigned char *expected,
                                 size_t length, size_t *received_out) {
  unsigned char chunk[8192];
  size_t received = *received_out;
  size_t reads = 0u;

  if (received > length) {
    return false;
  }
  while (reads < 128u) {
    ssize_t got = recv(client_fd, chunk, sizeof(chunk), 0);

    if (got > 0) {
      size_t count = (size_t)got;

      if (count > length - received || memcmp(expected + received, chunk, count) != 0) {
        return false;
      }
      received += count;
      ++reads;
      continue;
    }
    if (got == 0) {
      return received == length;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      *received_out = received;
      return true;
    }
    return false;
  }
  *received_out = received;
  return true;
}

static bool await_writable(struct omni_poller *poller, int fd, uint64_t token) {
  struct omni_poller_event event[2];
  struct omni_poller_result result;

  result = omni_poller_add(poller, fd, token, OMNI_POLLER_INTEREST_WRITE);
  if (result.status != OMNI_POLLER_OK) {
    return false;
  }
  result = omni_poller_wait(poller, (int64_t)2000, event, 2u);
  if (result.status != OMNI_POLLER_OK || result.count != 1u || event[0].fd != fd ||
      event[0].token != token || (event[0].ready & OMNI_POLLER_READY_WRITE) == 0u) {
    (void)omni_poller_remove(poller, fd);
    return false;
  }
  return true;
}

static void test_invalid_and_zero_length(void) {
  struct omni_listener listener;
  struct omni_accepted accepted;
  struct omni_accepted inert;
  struct omni_accepted malformed = { OMNI_ACCEPTED_FD_INVALID, true };
  struct omni_send_span span;
  struct omni_send_result result;
  unsigned char payload[4] = { 0x00u, 0x80u, 0xffu, 0x01u };
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  int bystander[2] = { OMNI_ACCEPTED_FD_INVALID, OMNI_ACCEPTED_FD_INVALID };
  int accepted_fd = OMNI_ACCEPTED_FD_INVALID;
  int status_before = 0;
  int descriptor_before = 0;

  if (!setup_connection(&listener, &accepted, &client_fd)) {
    return;
  }
  (void)pipe(bystander);
  span.data = payload;
  span.length = sizeof(payload);
  omni_accepted_make_inert(&inert);
  result = omni_send_once(NULL, span);
  check(result.status == OMNI_SEND_ERR_INVALID && result.sys_errno == EINVAL &&
            result.sent == 0u,
        "one-shot rejects a NULL accepted owner");
  result = omni_send_once(&inert, span);
  check(result.status == OMNI_SEND_ERR_INVALID && result.sent == 0u,
        "one-shot rejects an inert accepted owner");
  result = omni_send_once(&malformed, span);
  check(result.status == OMNI_SEND_ERR_INVALID && result.sent == 0u,
        "one-shot rejects a live owner with an invalid descriptor");
  accepted_fd = omni_accepted_fd(&accepted);
  check(accepted_flags(accepted_fd, &status_before, &descriptor_before),
        "accepted owner exposes expected nonblocking and close-on-exec flags");
  span.data = NULL;
  result = omni_send_once(&accepted, span);
  check(result.status == OMNI_SEND_ERR_INVALID && result.sys_errno == EINVAL &&
            result.sent == 0u,
        "one-shot rejects NULL source with nonzero length");
  span.length = 0u;
  result = omni_send_once(&accepted, span);
  check(result.status == OMNI_SEND_COMPLETE && result.sys_errno == 0 && result.sent == 0u,
        "zero-length one-shot is a successful no-op");
  result = omni_send_drain(&accepted, span, 4u);
  check(result.status == OMNI_SEND_COMPLETE && result.sent == 0u,
        "zero-length drain completes without a syscall");
  result = omni_send_drain(&accepted, span, 0u);
  check(result.status == OMNI_SEND_ERR_INVALID && result.sys_errno == EINVAL &&
            result.sent == 0u,
        "zero max_calls is rejected explicitly");
  span.data = payload;
  span.length = sizeof(payload);
  result = omni_send_drain(&accepted, (struct omni_send_span){ NULL, 1u }, 4u);
  check(result.status == OMNI_SEND_ERR_INVALID && result.sent == 0u,
        "drain rejects NULL source with nonzero length");
  check(client_has_no_pending_data(client_fd),
        "zero-length calls do not place bytes on the client");
  check(omni_accepted_is_live(&accepted) && omni_accepted_fd(&accepted) == accepted_fd,
        "invalid and zero-length outcomes preserve accepted ownership");
  check(fcntl(accepted_fd, F_GETFL) == status_before &&
            fcntl(accepted_fd, F_GETFD) == descriptor_before,
        "invalid and zero-length outcomes preserve accepted FD flags");
  check(fd_open(bystander[0]) && fd_open(bystander[1]),
        "bystander descriptors survive invalid and zero-length outcomes");
  omni_accepted_destroy(&accepted);
  check(!omni_accepted_is_live(&accepted) && fcntl(accepted_fd, F_GETFD) == -1 &&
            errno == EBADF,
        "caller can destroy the accepted owner exactly once after send calls");
  omni_accepted_destroy(&accepted);
  check(fd_open(omni_listener_fd(&listener)), "listener survives accepted-owner destroy");
  cleanup_connection(&listener, &accepted, &client_fd);
  if (bystander[0] != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(bystander[0]);
  }
  if (bystander[1] != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(bystander[1]);
  }
}

static void test_poller_write_and_binary_payload(void) {
  struct omni_listener listener;
  struct omni_accepted accepted;
  struct omni_poller poller;
  struct pollfd slots[2];
  uint64_t tokens[2];
  struct omni_send_span span;
  struct omni_send_result send_result;
  struct omni_poller_result poller_result;
  unsigned char payload[] = { 0x00u, 0x80u, 0xffu, 0x11u, 0x00u,
                              0xfeu, 0x7fu, 0x90u, 0x01u, 0xc0u };
  unsigned char snapshot[sizeof(payload)];
  uint16_t unused_port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  int accepted_fd = OMNI_ACCEPTED_FD_INVALID;
  int status_before = 0;
  int descriptor_before = 0;

  (void)unused_port;
  memset(&poller, 0, sizeof(poller));
  if (!setup_connection(&listener, &accepted, &client_fd)) {
    return;
  }
  accepted_fd = omni_accepted_fd(&accepted);
  (void)accepted_flags(accepted_fd, &status_before, &descriptor_before);
  memcpy(snapshot, payload, sizeof(payload));
  poller_result = omni_poller_init_borrowed(&poller, slots, tokens, 2u);
  check(poller_result.status == OMNI_POLLER_OK, "send poller initializes");
  if (poller_result.status != OMNI_POLLER_OK ||
      !await_writable(&poller, accepted_fd, (uint64_t)0x5E1Du)) {
    goto cleanup;
  }
  check(omni_poller_count(&poller) == 1u,
        "poller retains the caller-owned WRITE registration before send");
  span.data = payload;
  span.length = sizeof(payload);
  send_result = omni_send_drain(&accepted, span, 4u);
  check(send_result.status == OMNI_SEND_COMPLETE && send_result.sys_errno == 0 &&
            send_result.sent == sizeof(payload),
        "bounded send drain completes a small binary span");
  check(client_receive_expected(client_fd, payload, sizeof(payload)),
        "client receives the exact binary payload length and bytes");
  check(memcmp(payload, snapshot, sizeof(payload)) == 0,
        "successful send leaves the immutable source unchanged");
  check(omni_accepted_is_live(&accepted) && omni_accepted_fd(&accepted) == accepted_fd,
        "successful send preserves the accepted owner");
  check(fcntl(accepted_fd, F_GETFL) == status_before &&
            fcntl(accepted_fd, F_GETFD) == descriptor_before,
        "successful send does not change accepted FD flags");
  poller_result = omni_poller_remove(&poller, accepted_fd);
  check(poller_result.status == OMNI_POLLER_OK && omni_poller_count(&poller) == 0u,
        "caller removes WRITE registration after send; send did not update poller");

cleanup:
  omni_poller_destroy(&poller);
  check(fd_open(accepted_fd), "poller destroy closes neither accepted descriptor nor client");
  cleanup_connection(&listener, &accepted, &client_fd);
}

static void test_one_shot_partial_and_resume(void) {
  struct omni_listener listener;
  struct omni_accepted accepted;
  struct omni_send_result first;
  struct omni_send_result second;
  struct omni_send_span span;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  int accepted_fd = OMNI_ACCEPTED_FD_INVALID;
  int status_before = 0;
  int descriptor_before = 0;
  size_t remaining = 0u;

  fill_payload(large_payload, sizeof(large_payload), 17u);
  memcpy(large_snapshot, large_payload, sizeof(large_payload));
  if (!setup_connection(&listener, &accepted, &client_fd)) {
    return;
  }
  accepted_fd = omni_accepted_fd(&accepted);
  check(accepted_flags(accepted_fd, &status_before, &descriptor_before),
        "partial-send accepted flags are initially valid");
  check(set_send_buffer(accepted_fd, 1024),
        "test-only socket send buffer constrains partial-send setup");
  span.data = large_payload;
  span.length = sizeof(large_payload);
  first = omni_send_once(&accepted, span);
  check(first.status == OMNI_SEND_PROGRESS && first.sent > 0u &&
            first.sent < sizeof(large_payload),
        "one-shot reports a deterministic positive partial send");
  check(first.sent <= sizeof(large_payload),
        "one-shot progress count never exceeds the requested span");
  check(memcmp(large_payload, large_snapshot, sizeof(large_payload)) == 0,
        "partial send leaves the full caller source unchanged");
  check(omni_accepted_is_live(&accepted) && omni_accepted_fd(&accepted) == accepted_fd,
        "partial send preserves accepted ownership");
  check(fcntl(accepted_fd, F_GETFL) == status_before &&
            fcntl(accepted_fd, F_GETFD) == descriptor_before,
        "partial send preserves accepted FD flags");
  if (first.status == OMNI_SEND_PROGRESS && first.sent > 0u &&
      first.sent < sizeof(large_payload)) {
    check(client_receive_expected(client_fd, large_payload, first.sent),
          "client verifies the exact first partial-send prefix");
    remaining = sizeof(large_payload) - first.sent;
    second = omni_send_once(&accepted,
                            (struct omni_send_span){ large_payload + first.sent, remaining });
    if (second.status == OMNI_SEND_WOULD_BLOCK) {
      struct pollfd probe;

      probe.fd = accepted_fd;
      probe.events = POLLOUT;
      probe.revents = 0;
      check(poll(&probe, (nfds_t)1, 2000) > 0, "caller can await writable readiness before resume");
      second = omni_send_once(
          &accepted, (struct omni_send_span){ large_payload + first.sent, remaining });
    }
    check(second.status == OMNI_SEND_PROGRESS && second.sent > 0u && second.sent <= remaining,
          "caller resumes one-shot send at the exact returned offset");
    if (second.status == OMNI_SEND_PROGRESS && second.sent > 0u) {
      check(client_receive_expected(client_fd, large_payload + first.sent, second.sent),
            "resumed send delivers bytes after the returned offset without duplication");
    }
  }
  cleanup_connection(&listener, &accepted, &client_fd);
}

static void test_bounded_limit(void) {
  struct omni_listener listener;
  struct omni_accepted accepted;
  struct omni_send_result first;
  struct omni_send_result second;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  size_t remaining = 0u;

  fill_payload(large_payload, sizeof(large_payload), 23u);
  memcpy(large_snapshot, large_payload, sizeof(large_payload));
  if (!setup_connection(&listener, &accepted, &client_fd)) {
    return;
  }
  check(set_send_buffer(omni_accepted_fd(&accepted), 1024),
        "test-only socket send buffer constrains bounded-limit setup");
  first = omni_send_drain(&accepted,
                          (struct omni_send_span){ large_payload, sizeof(large_payload) }, 1u);
  check(first.status == OMNI_SEND_LIMIT_REACHED && first.sys_errno == 0 && first.sent > 0u &&
            first.sent < sizeof(large_payload),
        "bounded drain reports LIMIT_REACHED after exactly one send budget unit");
  check(first.sent <= sizeof(large_payload),
        "LIMIT_REACHED preserves a bounded total progress count");
  check(memcmp(large_payload, large_snapshot, sizeof(large_payload)) == 0,
        "LIMIT_REACHED leaves the immutable source unchanged");
  if (first.status == OMNI_SEND_LIMIT_REACHED && first.sent > 0u) {
    check(client_receive_expected(client_fd, large_payload, first.sent),
          "client drains the exact LIMIT_REACHED prefix");
    remaining = sizeof(large_payload) - first.sent;
    second = omni_send_drain(
        &accepted, (struct omni_send_span){ large_payload + first.sent, remaining }, 1u);
    check(second.sent <= remaining && second.status != OMNI_SEND_ERR_INVALID,
          "caller can resume bounded drain from the returned offset");
    if (second.sent > 0u) {
      check(client_receive_expected(client_fd, large_payload + first.sent, second.sent),
            "resumed bounded drain does not resend the already-sent prefix");
    }
  }
  cleanup_connection(&listener, &accepted, &client_fd);
}

static void test_would_block_backpressure(void) {
  struct omni_listener listener;
  struct omni_accepted accepted;
  struct omni_send_result result;
  struct omni_send_result one_shot;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  int accepted_fd = OMNI_ACCEPTED_FD_INVALID;
  int status_before = 0;
  int descriptor_before = 0;

  fill_payload(large_payload, sizeof(large_payload), 31u);
  memcpy(large_snapshot, large_payload, sizeof(large_payload));
  if (!setup_connection(&listener, &accepted, &client_fd)) {
    return;
  }
  accepted_fd = omni_accepted_fd(&accepted);
  (void)accepted_flags(accepted_fd, &status_before, &descriptor_before);
  check(set_send_buffer(accepted_fd, 1024),
        "test-only socket send buffer constrains WOULD_BLOCK setup");
  /* The client intentionally does not drain while this finite call runs. */
  result = omni_send_drain(&accepted,
                           (struct omni_send_span){ large_payload, sizeof(large_payload) },
                           256u);
  check(result.status == OMNI_SEND_WOULD_BLOCK &&
            (result.sys_errno == EAGAIN || result.sys_errno == EWOULDBLOCK),
        "bounded send drain reaches explicit WOULD_BLOCK under backpressure");
  check(result.sent > 0u && result.sent < sizeof(large_payload),
        "WOULD_BLOCK preserves prior positive progress and leaves an unsent tail");
  if (result.status == OMNI_SEND_WOULD_BLOCK && result.sent < sizeof(large_payload)) {
    one_shot = omni_send_once(
        &accepted,
        (struct omni_send_span){ large_payload + result.sent,
                                 sizeof(large_payload) - result.sent });
    check(one_shot.status == OMNI_SEND_WOULD_BLOCK && one_shot.sent == 0u &&
              (one_shot.sys_errno == EAGAIN || one_shot.sys_errno == EWOULDBLOCK),
          "one-shot WOULD_BLOCK reports zero progress for that attempt");
  }
  check(omni_accepted_is_live(&accepted) && omni_accepted_fd(&accepted) == accepted_fd,
        "WOULD_BLOCK preserves accepted ownership");
  check(fcntl(accepted_fd, F_GETFL) == status_before &&
            fcntl(accepted_fd, F_GETFD) == descriptor_before,
        "WOULD_BLOCK does not change accepted FD flags");
  check(fd_open(client_fd), "WOULD_BLOCK leaves the caller-owned peer descriptor live");
  check(memcmp(large_payload, large_snapshot, sizeof(large_payload)) == 0,
        "WOULD_BLOCK leaves the immutable source unchanged");
  cleanup_connection(&listener, &accepted, &client_fd);
}

static int run_peer_failure_child(void) {
  struct omni_listener listener;
  struct omni_accepted accepted;
  struct omni_send_result result = { OMNI_SEND_ERR_FATAL, EIO, 0u };
  struct pollfd probe;
  struct linger reset = { 1, 0 };
  unsigned char payload = 0xa5u;
  int bystander[2] = { OMNI_ACCEPTED_FD_INVALID, OMNI_ACCEPTED_FD_INVALID };
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  int accepted_fd = OMNI_ACCEPTED_FD_INVALID;
  int status_before = 0;
  int descriptor_before = 0;
  bool observed_failure = false;
  unsigned int attempts = 0u;

  if (pipe(bystander) != 0) {
    return 2;
  }
  if (!setup_connection(&listener, &accepted, &client_fd)) {
    (void)close(bystander[0]);
    (void)close(bystander[1]);
    return 2;
  }
  accepted_fd = omni_accepted_fd(&accepted);
  if (!accepted_flags(accepted_fd, &status_before, &descriptor_before)) {
    cleanup_connection(&listener, &accepted, &client_fd);
    (void)close(bystander[0]);
    (void)close(bystander[1]);
    return 3;
  }
  if (setsockopt(client_fd, SOL_SOCKET, SO_LINGER, &reset, (socklen_t)sizeof(reset)) != 0) {
    cleanup_connection(&listener, &accepted, &client_fd);
    (void)close(bystander[0]);
    (void)close(bystander[1]);
    return 3;
  }
  (void)close(client_fd);
  client_fd = OMNI_ACCEPTED_FD_INVALID;
  /* A bounded wait lets the reset become visible without sleeping. */
  probe.fd = accepted_fd;
  probe.events = POLLIN | POLLOUT;
  probe.revents = 0;
  (void)poll(&probe, (nfds_t)1, 2000);
  for (attempts = 0u; attempts < 8u; ++attempts) {
    result = omni_send_once(&accepted, (struct omni_send_span){ &payload, 1u });
    if (result.status == OMNI_SEND_PEER_CLOSED || result.status == OMNI_SEND_ERR_FATAL) {
      observed_failure = true;
      break;
    }
    if (result.status == OMNI_SEND_WOULD_BLOCK) {
      probe.revents = 0;
      (void)poll(&probe, (nfds_t)1, 2000);
    }
  }
  if (!observed_failure || result.sys_errno == 0 ||
      (result.status == OMNI_SEND_PEER_CLOSED &&
       result.sys_errno != EPIPE && result.sys_errno != ECONNRESET)) {
    cleanup_connection(&listener, &accepted, &client_fd);
    (void)close(bystander[0]);
    (void)close(bystander[1]);
    return 4;
  }
  if (!omni_accepted_is_live(&accepted) || omni_accepted_fd(&accepted) != accepted_fd ||
      fcntl(accepted_fd, F_GETFL) != status_before ||
      fcntl(accepted_fd, F_GETFD) != descriptor_before || payload != 0xa5u ||
      !fd_open(bystander[0]) || !fd_open(bystander[1])) {
    cleanup_connection(&listener, &accepted, &client_fd);
    (void)close(bystander[0]);
    (void)close(bystander[1]);
    return 5;
  }
  omni_accepted_destroy(&accepted);
  if (omni_accepted_is_live(&accepted) || omni_accepted_fd(&accepted) != OMNI_ACCEPTED_FD_INVALID) {
    omni_listener_destroy(&listener);
    (void)close(bystander[0]);
    (void)close(bystander[1]);
    return 6;
  }
  omni_accepted_destroy(&accepted);
  if (!fd_open(omni_listener_fd(&listener)) || !fd_open(bystander[0]) ||
      !fd_open(bystander[1])) {
    omni_listener_destroy(&listener);
    (void)close(bystander[0]);
    (void)close(bystander[1]);
    return 7;
  }
  omni_listener_destroy(&listener);
  (void)close(bystander[0]);
  (void)close(bystander[1]);
  return 0;
}

static void test_peer_failure_and_sigpipe(void) {
  pid_t child = fork();
  int status = 0;
  pid_t waited = (pid_t)-1;

  check(child >= (pid_t)0, "peer-failure SIGPIPE test process forks");
  if (child < (pid_t)0) {
    return;
  }
  if (child == (pid_t)0) {
    int code = run_peer_failure_child();

    _exit(code);
  }
  waited = waitpid(child, &status, 0);
  check(waited == child, "peer-failure child is reaped");
  check(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "peer/socket failure is explicit and SIGPIPE cannot terminate the process");
  check(!(waited == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE),
        "peer-close send does not deliver SIGPIPE");
}

static void test_transfer_stress(void) {
  struct omni_listener listener;
  struct omni_accepted accepted;
  struct omni_send_result result;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  int accepted_fd = OMNI_ACCEPTED_FD_INVALID;
  int status_before = 0;
  int descriptor_before = 0;
  size_t sent = 0u;
  size_t received = 0u;
  size_t iterations = 0u;
  bool transfer_ok = true;

  fill_payload(transfer_payload, sizeof(transfer_payload), 43u);
  memcpy(transfer_snapshot, transfer_payload, sizeof(transfer_payload));
  if (!setup_connection(&listener, &accepted, &client_fd)) {
    return;
  }
  accepted_fd = omni_accepted_fd(&accepted);
  (void)accepted_flags(accepted_fd, &status_before, &descriptor_before);
  transfer_ok = set_nonblocking_for_test(client_fd);
  check(transfer_ok, "stress client becomes nonblocking for bounded draining");
  while (transfer_ok && sent < sizeof(transfer_payload) && iterations < 2048u) {
    size_t remaining = sizeof(transfer_payload) - sent;

    result = omni_send_drain(
        &accepted, (struct omni_send_span){ transfer_payload + sent, remaining }, 8u);
    if (result.sent > remaining) {
      transfer_ok = false;
      break;
    }
    sent += result.sent;
    if (result.status != OMNI_SEND_COMPLETE && result.status != OMNI_SEND_LIMIT_REACHED &&
        result.status != OMNI_SEND_WOULD_BLOCK) {
      transfer_ok = false;
      break;
    }
    if (!drain_client_checked(client_fd, transfer_payload, sizeof(transfer_payload),
                              &received)) {
      transfer_ok = false;
      break;
    }
    if (result.status == OMNI_SEND_WOULD_BLOCK &&
        !wait_client_readable(client_fd)) {
      transfer_ok = false;
      break;
    }
    ++iterations;
  }
  while (transfer_ok && received < sizeof(transfer_payload) && iterations < 4096u) {
    if (!wait_client_readable(client_fd) ||
        !drain_client_checked(client_fd, transfer_payload, sizeof(transfer_payload),
                              &received)) {
      transfer_ok = false;
      break;
    }
    ++iterations;
  }
  check(transfer_ok && sent == sizeof(transfer_payload),
        "bounded send stress reaches the exact total source length");
  check(transfer_ok && received == sizeof(transfer_payload),
        "bounded send stress receives the exact total byte count");
  check(transfer_ok && memcmp(transfer_payload, transfer_snapshot,
                              sizeof(transfer_payload)) == 0,
        "bounded send stress leaves the source byte-for-byte unchanged");
  check(transfer_ok && sent == received,
        "bounded send stress preserves TCP stream ordering without duplicates or gaps");
  check(omni_accepted_is_live(&accepted) && omni_accepted_fd(&accepted) == accepted_fd,
        "transfer stress preserves accepted ownership");
  check(fcntl(accepted_fd, F_GETFL) == status_before &&
            fcntl(accepted_fd, F_GETFD) == descriptor_before,
        "transfer stress preserves accepted FD flags");
  cleanup_connection(&listener, &accepted, &client_fd);
}

static void test_descriptor_lifecycle_stress(void) {
  bool supported = false;
  int before = -1;
  int bystander[2] = { OMNI_ACCEPTED_FD_INVALID, OMNI_ACCEPTED_FD_INVALID };
  unsigned int iteration = 0u;

  check(pipe(bystander) == 0, "bystander descriptors open for lifecycle stress");
  before = count_open_fds(&supported);
  for (iteration = 0u; iteration < STRESS_ITERATIONS; ++iteration) {
    struct omni_listener listener;
    struct omni_accepted accepted;
    unsigned char payload[37];
    struct omni_send_result result;
    int client_fd = OMNI_ACCEPTED_FD_INVALID;

    fill_payload(payload, sizeof(payload), iteration + 71u);
    if (!setup_connection(&listener, &accepted, &client_fd)) {
      break;
    }
    result = omni_send_drain(
        &accepted, (struct omni_send_span){ payload, sizeof(payload) }, 4u);
    check(result.status == OMNI_SEND_COMPLETE && result.sent == sizeof(payload),
          "lifecycle stress sends one complete binary message");
    check(client_receive_expected(client_fd, payload, sizeof(payload)),
          "lifecycle stress verifies exact message bytes");
    check(fd_open(bystander[0]) && fd_open(bystander[1]),
          "bystander descriptors survive lifecycle iteration");
    cleanup_connection(&listener, &accepted, &client_fd);
    check(!omni_accepted_is_live(&accepted),
          "lifecycle stress leaves accepted owner inert after destroy");
  }
  check(iteration == STRESS_ITERATIONS, "lifecycle stress completes its finite iteration bound");
  check(fd_open(bystander[0]) && fd_open(bystander[1]),
        "bystander descriptors survive the complete lifecycle stress");
  if (supported) {
    int after = count_open_fds(&supported);

    check(after == before, "descriptor census returns to its pre-stress baseline");
  } else {
    check(true, "descriptor census skipped honestly on unsupported platforms");
  }
  if (bystander[0] != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(bystander[0]);
  }
  if (bystander[1] != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(bystander[1]);
  }
}

int main(void) {
  test_invalid_and_zero_length();
  test_poller_write_and_binary_payload();
  test_one_shot_partial_and_resume();
  test_bounded_limit();
  test_would_block_backpressure();
  test_peer_failure_and_sigpipe();
  test_transfer_stress();
  test_descriptor_lifecycle_stress();

  printf("---\n%d checks, %d failures\n", check_count, failure_count);
  return failure_count == 0 ? 0 : 1;
}
