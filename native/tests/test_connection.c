/*
 * OmniRoute native backend — protocol-agnostic connection tests (Task 019).
 *
 * Loopback-only and self-cleaning. The production connection owns the
 * accepted server descriptor and delegates payload movement to the existing
 * recv/send primitives; the test client uses direct socket calls only as a
 * verification boundary. No HTTP-shaped bytes, fixed ports, sleeps, or
 * production event loop exist here. Every drain and readiness loop has an
 * explicit finite bound, and all listener ports are ephemeral rather than
 * the operator port 20128.
 *
 * The FD-0 ownership proof runs in a child so closing descriptor 0 cannot
 * disturb the CTest runner. The peer-failure proof also runs in a child with
 * the default SIGPIPE disposition: a successful child exit demonstrates that
 * the production MSG_NOSIGNAL path survived the reset without changing the
 * parent's process-wide signal policy.
 */

#define _GNU_SOURCE /* socket, fork, and MSG_DONTWAIT visibility under C11 */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

#include "omniroute/connection.h"

#define OMNI_TEST_RECEIVE_CAPACITY 4096u
#define OMNI_TEST_STRESS_LENGTH (256u * 1024u)
#define OMNI_TEST_LIFECYCLE_CYCLES 64u
#define OMNI_TEST_MAX_SEND_STEPS 8192u

static int check_count = 0;
static int failure_count = 0;
static unsigned char stress_source[OMNI_TEST_STRESS_LENGTH];
static unsigned char stress_snapshot[OMNI_TEST_STRESS_LENGTH];
static unsigned char stress_received[OMNI_TEST_STRESS_LENGTH];

static void check(bool condition, const char *name) {
  ++check_count;
  if (condition) {
    printf("ok - %s\n", name);
  } else {
    ++failure_count;
    printf("NOT OK - %s\n", name);
  }
}

static bool fd_open(int fd) {
  return fd >= 0 && fcntl(fd, F_GETFD) != -1;
}

static bool fd_flags_ok(int fd) {
  int status_flags = fcntl(fd, F_GETFL);
  int descriptor_flags = fcntl(fd, F_GETFD);

  return status_flags != -1 && (status_flags & O_NONBLOCK) != 0 &&
         descriptor_flags != -1 && (descriptor_flags & FD_CLOEXEC) != 0;
}

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

static bool start_listener(struct omni_listener *listener, uint16_t *port_out) {
  struct omni_listener_result result;

  omni_listener_make_inert(listener);
  result = omni_listener_init(listener, "127.0.0.1", (uint16_t)0u);
  if (result.status != OMNI_LISTENER_OK || result.sys_errno != 0) {
    return false;
  }
  if (omni_listener_port(listener) == OMNI_LISTENER_PORT_INVALID ||
      omni_listener_port(listener) == (uint16_t)20128) {
    omni_listener_destroy(listener);
    return false;
  }
  if (port_out != NULL) {
    *port_out = omni_listener_port(listener);
  }
  return true;
}

static int open_client(uint16_t port) {
  struct sockaddr_in peer;
  int client_fd = socket(AF_INET, SOCK_STREAM, 0);

  if (client_fd == OMNI_CONNECTION_FD_INVALID) {
    return OMNI_CONNECTION_FD_INVALID;
  }
  memset(&peer, 0, sizeof(peer));
  peer.sin_family = AF_INET;
  peer.sin_port = htons(port);
  peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(client_fd, (const struct sockaddr *)&peer, (socklen_t)sizeof(peer)) != 0) {
    (void)close(client_fd);
    return OMNI_CONNECTION_FD_INVALID;
  }
  return client_fd;
}

static bool accept_owner(struct omni_listener *listener, struct omni_accepted *accepted) {
  struct omni_accept_result result;

  omni_accepted_make_inert(accepted);
  result = omni_accept_once(listener, accepted);
  return result.status == OMNI_ACCEPT_OK && result.accepted == 1u &&
         omni_accepted_is_live(accepted);
}

struct connection_fixture {
  struct omni_listener listener;
  struct omni_accepted accepted;
  struct omni_connection connection;
  unsigned char receive_storage[OMNI_TEST_RECEIVE_CAPACITY];
  int client_fd;
  bool listener_started;
};

static bool fixture_start(struct connection_fixture *fixture, uint64_t token,
                          uint32_t interests) {
  struct omni_connection_config config;
  struct omni_connection_result connection_result;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;

  memset(fixture, 0, sizeof(*fixture));
  fixture->client_fd = OMNI_CONNECTION_FD_INVALID;
  omni_connection_make_inert(&fixture->connection);
  omni_accepted_make_inert(&fixture->accepted);
  omni_listener_make_inert(&fixture->listener);
  if (!start_listener(&fixture->listener, &port)) {
    return false;
  }
  fixture->listener_started = true;
  fixture->client_fd = open_client(port);
  if (fixture->client_fd == OMNI_CONNECTION_FD_INVALID ||
      !accept_owner(&fixture->listener, &fixture->accepted)) {
    return false;
  }
  config.receive_storage = fixture->receive_storage;
  config.receive_capacity = sizeof(fixture->receive_storage);
  config.poller_token = token;
  config.poller_interests = interests;
  connection_result = omni_connection_init(&fixture->connection, &config);
  if (connection_result.status != OMNI_CONNECTION_OK) {
    return false;
  }
  connection_result = omni_connection_from_accepted(&fixture->connection, &fixture->accepted);
  return connection_result.status == OMNI_CONNECTION_OK;
}

static void fixture_stop(struct connection_fixture *fixture) {
  omni_connection_destroy(&fixture->connection);
  omni_accepted_destroy(&fixture->accepted);
  if (fixture->client_fd != OMNI_CONNECTION_FD_INVALID) {
    (void)close(fixture->client_fd);
    fixture->client_fd = OMNI_CONNECTION_FD_INVALID;
  }
  if (fixture->listener_started) {
    omni_listener_destroy(&fixture->listener);
    fixture->listener_started = false;
  }
}

static bool client_send_all(int fd, const unsigned char *data, size_t length) {
  size_t sent = 0u;
  size_t attempts = 0u;

  while (sent < length && attempts < OMNI_TEST_MAX_SEND_STEPS) {
    ssize_t produced = send(fd, data + sent, length - sent, 0);

    ++attempts;
    if (produced <= 0) {
      return false;
    }
    sent += (size_t)produced;
  }
  return sent == length;
}

static bool client_receive_exact(int fd, unsigned char *destination, size_t length) {
  size_t received = 0u;
  size_t waits = 0u;

  while (received < length && waits < OMNI_TEST_MAX_SEND_STEPS) {
    struct pollfd wait_fd;
    int waited = 0;
    ssize_t produced = 0;

    wait_fd.fd = fd;
    wait_fd.events = POLLIN | POLLERR | POLLHUP;
    wait_fd.revents = 0;
    waited = poll(&wait_fd, 1u, 1000);
    ++waits;
    if (waited <= 0 || (wait_fd.revents & (POLLERR | POLLHUP)) != 0) {
      /* POLLIN can accompany HUP with final bytes, so only reject HUP when
       * the following receive proves no data remains. */
      if ((wait_fd.revents & POLLIN) == 0) {
        return false;
      }
    }
    produced = recv(fd, destination + received, length - received, 0);
    if (produced <= 0) {
      return false;
    }
    received += (size_t)produced;
  }
  return received == length;
}

static bool client_drain_available(int fd, unsigned char *destination, size_t capacity,
                                   size_t *used) {
  size_t attempts = 0u;

  while (*used < capacity && attempts < 64u) {
    ssize_t produced = recv(fd, destination + *used, capacity - *used, MSG_DONTWAIT);

    ++attempts;
    if (produced > 0) {
      *used += (size_t)produced;
      continue;
    }
    if (produced == 0) {
      return false;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    }
    return false;
  }
  return true;
}

static bool client_receive_until(int fd, unsigned char *destination, size_t length,
                                 size_t *used) {
  size_t waits = 0u;

  while (*used < length && waits < OMNI_TEST_MAX_SEND_STEPS) {
    struct pollfd wait_fd;
    int waited = 0;

    if (!client_drain_available(fd, destination, length, used)) {
      return false;
    }
    if (*used == length) {
      return true;
    }
    wait_fd.fd = fd;
    wait_fd.events = POLLIN | POLLERR | POLLHUP;
    wait_fd.revents = 0;
    waited = poll(&wait_fd, 1u, 1000);
    ++waits;
    if (waited <= 0) {
      return false;
    }
  }
  return *used == length;
}

static void fill_payload(unsigned char *payload, size_t length, unsigned char seed) {
  size_t i = 0u;

  for (i = 0u; i < length; ++i) {
    if (i % 29u == 0u) {
      payload[i] = 0x00u;
    } else if (i % 31u == 0u) {
      payload[i] = 0x80u;
    } else if (i % 37u == 0u) {
      payload[i] = 0xffu;
    } else {
      payload[i] = (unsigned char)(seed + (unsigned char)(i * 13u));
    }
  }
}

static void test_invalid_states_and_failed_initialization(void) {
  struct omni_connection connection;
  struct omni_connection_config config;
  struct omni_connection_result result;
  struct omni_accepted accepted;
  unsigned char storage[32];

  memset(storage, 0xa5, sizeof(storage));
  omni_connection_make_inert(&connection);
  omni_accepted_make_inert(&accepted);
  check(omni_connection_state(&connection) == OMNI_CONNECTION_INERT,
        "fresh connection is explicitly inert");
  check(!omni_connection_is_live(&connection) &&
            omni_connection_fd(&connection) == OMNI_CONNECTION_FD_INVALID,
        "inert connection has no live FD");
  check(omni_connection_receive_buffer(&connection) == NULL,
        "inert connection exposes no receive buffer");
  check(omni_connection_init(NULL, NULL).status == OMNI_CONNECTION_ERR_INVALID,
        "NULL connection initialization is rejected");
  check(omni_connection_init(&connection, NULL).status == OMNI_CONNECTION_ERR_INVALID,
        "NULL connection configuration is rejected");

  config.receive_storage = NULL;
  config.receive_capacity = sizeof(storage);
  config.poller_token = 1u;
  config.poller_interests = OMNI_POLLER_INTEREST_READ;
  result = omni_connection_init(&connection, &config);
  check(result.status == OMNI_CONNECTION_ERR_INVALID &&
            omni_connection_state(&connection) == OMNI_CONNECTION_INERT,
        "NULL receive storage fails without preparing state");

  config.receive_storage = storage;
  config.receive_capacity = 0u;
  result = omni_connection_init(&connection, &config);
  check(result.status == OMNI_CONNECTION_ERR_INVALID &&
            omni_connection_state(&connection) == OMNI_CONNECTION_INERT,
        "zero receive capacity fails without preparing state");

  config.receive_capacity = sizeof(storage);
  config.poller_interests = 0u;
  result = omni_connection_init(&connection, &config);
  check(result.status == OMNI_CONNECTION_ERR_INVALID &&
            omni_connection_state(&connection) == OMNI_CONNECTION_INERT,
        "empty poller interest metadata is rejected");
  config.poller_interests = OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE | 32u;
  result = omni_connection_init(&connection, &config);
  check(result.status == OMNI_CONNECTION_ERR_INVALID &&
            omni_connection_state(&connection) == OMNI_CONNECTION_INERT,
        "unknown poller interest metadata is rejected");

  config.poller_interests = OMNI_POLLER_INTEREST_READ;
  result = omni_connection_init(&connection, &config);
  check(result.status == OMNI_CONNECTION_OK &&
            omni_connection_state(&connection) == OMNI_CONNECTION_READY,
        "valid initialization prepares a connection without an FD");
  check(omni_connection_from_accepted(&connection, &accepted).status ==
            OMNI_CONNECTION_ERR_INVALID,
        "adoption of an inert accepted owner is rejected");
  check(omni_connection_begin_close(&connection).status == OMNI_CONNECTION_ERR_STATE,
        "closing a prepared connection is an invalid transition");
  check(omni_connection_recv_once(&connection).status == OMNI_RECV_ERR_INVALID &&
            omni_connection_send_once(&connection, (struct omni_send_span){ NULL, 0u }).status ==
                OMNI_SEND_ERR_INVALID,
        "payload access before OPEN is rejected without a syscall");
  result = omni_connection_init(&connection, &config);
  check(result.status == OMNI_CONNECTION_ERR_STATE,
        "reinitializing a prepared connection is rejected");
  omni_connection_destroy(&connection);
  check(omni_connection_state(&connection) == OMNI_CONNECTION_CLOSED &&
            omni_connection_fd(&connection) == OMNI_CONNECTION_FD_INVALID,
        "destroy of a prepared connection reaches CLOSED");
  omni_connection_destroy(&connection);
  check(omni_connection_state(&connection) == OMNI_CONNECTION_CLOSED,
        "repeated destroy of a closed connection is idempotent");
  omni_connection_make_inert(&connection);
  check(omni_connection_state(&connection) == OMNI_CONNECTION_INERT,
        "destroyed storage can be explicitly made inert for reuse");
}

static void test_failed_initialization_preserves_accepted_owner(void) {
  struct omni_listener listener;
  struct omni_accepted accepted;
  struct omni_connection connection;
  struct omni_connection_config config;
  struct omni_connection_result result;
  unsigned char storage[32];
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_CONNECTION_FD_INVALID;
  int accepted_fd = OMNI_CONNECTION_FD_INVALID;

  omni_listener_make_inert(&listener);
  omni_accepted_make_inert(&accepted);
  omni_connection_make_inert(&connection);
  check(start_listener(&listener, &port),
        "failed-initialization ownership fixture listener starts");
  if (omni_listener_fd(&listener) == OMNI_LISTENER_FD_INVALID) {
    return;
  }
  client_fd = open_client(port);
  check(client_fd != OMNI_CONNECTION_FD_INVALID && accept_owner(&listener, &accepted),
        "failed-initialization ownership fixture accepts a client");
  if (client_fd == OMNI_CONNECTION_FD_INVALID || !omni_accepted_is_live(&accepted)) {
    omni_accepted_destroy(&accepted);
    if (client_fd != OMNI_CONNECTION_FD_INVALID) {
      (void)close(client_fd);
    }
    omni_listener_destroy(&listener);
    return;
  }
  accepted_fd = omni_accepted_fd(&accepted);
  config.receive_storage = NULL;
  config.receive_capacity = sizeof(storage);
  config.poller_token = 91u;
  config.poller_interests = OMNI_POLLER_INTEREST_READ;
  result = omni_connection_init(&connection, &config);
  check(result.status == OMNI_CONNECTION_ERR_INVALID &&
            omni_connection_state(&connection) == OMNI_CONNECTION_INERT &&
            omni_accepted_is_live(&accepted) && omni_accepted_fd(&accepted) == accepted_fd &&
            fd_open(accepted_fd),
        "failed initialization leaves accepted ownership and FD untouched");
  omni_connection_destroy(&connection);
  omni_accepted_destroy(&accepted);
  check(fcntl(accepted_fd, F_GETFD) == -1 && errno == EBADF,
        "caller can clean up the preserved accepted owner exactly once");
  (void)close(client_fd);
  omni_listener_destroy(&listener);
}

static void test_lifecycle_binary_io_and_poller(void) {
  struct connection_fixture fixture;
  struct omni_poller poller;
  struct pollfd poller_fds[1];
  uint64_t poller_tokens[1];
  struct omni_poller_event events[1];
  struct omni_poller_result poller_result;
  struct omni_send_result send_result;
  struct omni_recv_result recv_result;
  const unsigned char outbound[] = { 0x00u, 0x7fu, 0x80u, 0xffu, 0x41u, 0x00u };
  const unsigned char inbound[] = { 0xffu, 0x00u, 0x80u, 0x42u, 0xc3u, 0x28u };
  unsigned char outbound_copy[sizeof(outbound)];
  unsigned char outbound_received[sizeof(outbound)];
  unsigned char inbound_copy[sizeof(inbound)];
  size_t readable = 0u;
  const unsigned char *received = NULL;
  int accepted_fd = OMNI_CONNECTION_FD_INVALID;
  bool poller_live = false;

  memcpy(outbound_copy, outbound, sizeof(outbound));
  memcpy(inbound_copy, inbound, sizeof(inbound));
  check(fixture_start(&fixture, UINT64_C(0x0190c0ffee),
                      OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE),
        "accepted socket transfers into a connection");
  if (!omni_connection_is_live(&fixture.connection)) {
    fixture_stop(&fixture);
    return;
  }
  accepted_fd = omni_connection_fd(&fixture.connection);
  check(omni_connection_state(&fixture.connection) == OMNI_CONNECTION_OPEN,
        "adopted connection enters OPEN");
  check(accepted_fd != OMNI_CONNECTION_FD_INVALID && fd_open(accepted_fd) &&
            fd_flags_ok(accepted_fd),
        "connection owns a live nonblocking close-on-exec accepted FD");
  check(!omni_accepted_is_live(&fixture.accepted) &&
            omni_accepted_fd(&fixture.accepted) == OMNI_ACCEPTED_FD_INVALID,
        "accepted source is inert after ownership transfer");
  check(omni_connection_poller_token(&fixture.connection) == UINT64_C(0x0190c0ffee) &&
            omni_connection_poller_interests(&fixture.connection) ==
                (OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE),
        "connection retains only opaque poller metadata");

  poller_result = omni_poller_init_borrowed(&poller, poller_fds, poller_tokens, 1u);
  check(poller_result.status == OMNI_POLLER_OK, "external poller initializes");
  poller_live = poller_result.status == OMNI_POLLER_OK;
  if (poller_live) {
    poller_result = omni_poller_add(&poller, accepted_fd,
                                    omni_connection_poller_token(&fixture.connection),
                                    OMNI_POLLER_INTEREST_WRITE);
    check(poller_result.status == OMNI_POLLER_OK && omni_poller_count(&poller) == 1u,
          "caller registers connection FD for WRITE");
    poller_result = omni_poller_wait(&poller, (int64_t)2000, events, 1u);
    check(poller_result.status == OMNI_POLLER_OK && poller_result.count == 1u &&
              (events[0].ready & OMNI_POLLER_READY_WRITE) != 0u &&
              events[0].token == omni_connection_poller_token(&fixture.connection),
          "external poller observes connection WRITE readiness");
  }

  send_result = omni_connection_send_drain(
      &fixture.connection, (struct omni_send_span){ outbound, sizeof(outbound) }, 4u);
  check(send_result.status == OMNI_SEND_COMPLETE && send_result.sent == sizeof(outbound),
        "connection delegates a complete binary send without an output queue");
  check(omni_poller_count(&poller) == (poller_live ? 1u : 0u),
        "send does not update external poller registration");
  check(client_receive_exact(fixture.client_fd, outbound_received, sizeof(outbound)) &&
            memcmp(outbound_received, outbound, sizeof(outbound)) == 0,
        "client receives exact binary send bytes and ordering");
  check(memcmp(outbound, outbound_copy, sizeof(outbound)) == 0,
        "caller send span remains byte-for-byte unchanged");

  if (poller_live) {
    poller_result = omni_poller_remove(&poller, accepted_fd);
    check(poller_result.status == OMNI_POLLER_OK && omni_poller_count(&poller) == 0u,
          "caller removes registration before connection destroy");
    omni_poller_destroy(&poller);
    poller_live = false;
  }

  check(client_send_all(fixture.client_fd, inbound, sizeof(inbound)),
        "client sends binary receive payload");
  recv_result = omni_connection_recv_drain(&fixture.connection, 8u);
  check(recv_result.status == OMNI_RECV_WOULD_BLOCK && recv_result.received == sizeof(inbound),
        "connection receive drain reports data then bounded would-block");
  received = omni_bytebuf_read_ptr(omni_connection_receive_buffer(&fixture.connection), &readable);
  check(received != NULL && readable == sizeof(inbound) &&
            memcmp(received, inbound, sizeof(inbound)) == 0,
        "connection receive buffer preserves exact binary bytes");
  check(memcmp(inbound, inbound_copy, sizeof(inbound)) == 0,
        "caller receive source remains unchanged");
  check(omni_bytebuf_consume(omni_connection_receive_buffer(&fixture.connection), readable),
        "caller explicitly consumes connection receive bytes");

  (void)close(fixture.client_fd);
  fixture.client_fd = OMNI_CONNECTION_FD_INVALID;
  recv_result = omni_connection_recv_once(&fixture.connection);
  check(recv_result.status == OMNI_RECV_EOF && recv_result.received == 0u,
        "client disconnect is surfaced as EOF through the connection");
  check(omni_connection_state(&fixture.connection) == OMNI_CONNECTION_OPEN &&
            omni_connection_is_live(&fixture.connection) &&
            fd_open(accepted_fd),
        "EOF does not silently destroy connection ownership");
  check(omni_connection_begin_close(&fixture.connection).status == OMNI_CONNECTION_OK &&
            omni_connection_state(&fixture.connection) == OMNI_CONNECTION_CLOSING,
        "OPEN transitions explicitly to CLOSING");
  check(omni_connection_is_live(&fixture.connection) &&
            omni_connection_fd(&fixture.connection) == accepted_fd,
        "CLOSING still owns the accepted FD until destroy");
  check(omni_connection_begin_close(&fixture.connection).status == OMNI_CONNECTION_ERR_STATE,
        "repeated begin-close is rejected");
  check(omni_connection_recv_once(&fixture.connection).status == OMNI_RECV_ERR_INVALID &&
            omni_connection_send_once(&fixture.connection,
                                      (struct omni_send_span){ outbound, sizeof(outbound) })
                    .status == OMNI_SEND_ERR_INVALID,
        "CLOSING rejects new payload operations");
  omni_connection_destroy(&fixture.connection);
  check(omni_connection_state(&fixture.connection) == OMNI_CONNECTION_CLOSED &&
            omni_connection_fd(&fixture.connection) == OMNI_CONNECTION_FD_INVALID &&
            fcntl(accepted_fd, F_GETFD) == -1 && errno == EBADF,
        "connection destroy closes the owned FD exactly once");
  omni_connection_destroy(&fixture.connection);
  check(fd_open(omni_listener_fd(&fixture.listener)),
        "connection destroy leaves the listener lifecycle untouched");
  fixture_stop(&fixture);
}

static int run_fd_zero_child(void) {
  struct connection_fixture fixture;
  struct omni_send_result send_result;
  const unsigned char payload[] = { 0x00u, 0x80u, 0xffu, 0x17u };
  unsigned char received[sizeof(payload)];
  int original_fd = OMNI_CONNECTION_FD_INVALID;
  int guard_fd = OMNI_CONNECTION_FD_INVALID;
  bool had_stdin = fcntl(STDIN_FILENO, F_GETFD) != -1;

  if (!had_stdin) {
    guard_fd = open("/dev/null", O_RDWR);
    if (guard_fd != STDIN_FILENO) {
      if (guard_fd >= 0) {
        (void)close(guard_fd);
      }
      return 10;
    }
  }
  if (!fixture_start(&fixture, 9u, OMNI_POLLER_INTEREST_READ)) {
    fixture_stop(&fixture);
    return 11;
  }
  original_fd = omni_connection_fd(&fixture.connection);
  if (dup2(original_fd, STDIN_FILENO) == -1) {
    fixture_stop(&fixture);
    return 12;
  }
  /* Release the original accepted owner, then publish the duplicate as the
   * same owner shape. This keeps the test's ownership transition explicit. */
  omni_accepted_destroy(&fixture.accepted);
  fixture.accepted.fd = STDIN_FILENO;
  fixture.accepted.live = true;
  omni_connection_destroy(&fixture.connection);
  omni_connection_make_inert(&fixture.connection);
  {
    struct omni_connection_config config;
    struct omni_connection_result result;

    config.receive_storage = fixture.receive_storage;
    config.receive_capacity = sizeof(fixture.receive_storage);
    config.poller_token = 9u;
    config.poller_interests = OMNI_POLLER_INTEREST_READ;
    result = omni_connection_init(&fixture.connection, &config);
    if (result.status != OMNI_CONNECTION_OK ||
        omni_connection_from_accepted(&fixture.connection, &fixture.accepted).status !=
            OMNI_CONNECTION_OK) {
      fixture_stop(&fixture);
      return 13;
    }
  }
  if (omni_connection_fd(&fixture.connection) != STDIN_FILENO ||
      !omni_connection_is_live(&fixture.connection)) {
    fixture_stop(&fixture);
    return 14;
  }
  send_result = omni_connection_send_once(
      &fixture.connection, (struct omni_send_span){ payload, sizeof(payload) });
  if (send_result.status != OMNI_SEND_PROGRESS || send_result.sent != sizeof(payload) ||
      !client_receive_exact(fixture.client_fd, received, sizeof(received)) ||
      memcmp(received, payload, sizeof(payload)) != 0) {
    fixture_stop(&fixture);
    return 15;
  }
  omni_connection_destroy(&fixture.connection);
  if (fcntl(STDIN_FILENO, F_GETFD) != -1 || errno != EBADF) {
    fixture_stop(&fixture);
    return 16;
  }
  (void)close(fixture.client_fd);
  fixture.client_fd = OMNI_CONNECTION_FD_INVALID;
  omni_listener_destroy(&fixture.listener);
  fixture.listener_started = false;
  return 0;
}

static void test_fd_zero_ownership(void) {
  pid_t child = fork();
  int status = 0;

  check(child >= 0, "FD-0 ownership child starts");
  if (child == 0) {
    _exit(run_fd_zero_child());
  }
  if (child < 0) {
    return;
  }
  check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "connection owns and closes descriptor 0 without confusing it with invalid");
}

static void test_partial_progress_and_resume(void) {
  struct connection_fixture fixture;
  struct omni_send_result first;
  struct omni_send_result limited;
  struct omni_send_span tail;
  size_t offset = 0u;
  size_t received = 0u;
  size_t steps = 0u;
  int send_buffer = 1024;
  bool transfer_ok = true;

  fill_payload(stress_source, sizeof(stress_source), 0x11u);
  memcpy(stress_snapshot, stress_source, sizeof(stress_source));
  check(fixture_start(&fixture, 17u, OMNI_POLLER_INTEREST_WRITE),
        "partial-send fixture starts");
  if (!omni_connection_is_live(&fixture.connection)) {
    fixture_stop(&fixture);
    return;
  }
  check(setsockopt(omni_connection_fd(&fixture.connection), SOL_SOCKET, SO_SNDBUF,
                   &send_buffer, (socklen_t)sizeof(send_buffer)) == 0,
        "test-only send buffer constraint applies");
  first = omni_connection_send_once(
      &fixture.connection, (struct omni_send_span){ stress_source, sizeof(stress_source) });
  check(first.status == OMNI_SEND_PROGRESS && first.sent > 0u &&
            first.sent < sizeof(stress_source),
        "one-shot connection send produces deterministic partial progress");
  if (first.status != OMNI_SEND_PROGRESS || first.sent == 0u ||
      first.sent >= sizeof(stress_source)) {
    fixture_stop(&fixture);
    return;
  }
  offset = first.sent;
  check(client_receive_exact(fixture.client_fd, stress_received, first.sent),
        "client drains the exact first partial prefix");
  check(memcmp(stress_received, stress_source, first.sent) == 0,
        "first partial prefix is exact and ordered");

  tail.data = stress_source + offset;
  tail.length = sizeof(stress_source) - offset;
  received = first.sent;
  limited = omni_connection_send_drain(&fixture.connection, tail, 1u);
  check(limited.status == OMNI_SEND_LIMIT_REACHED && limited.sent > 0u &&
            limited.sent < tail.length,
        "bounded connection drain exposes LIMIT_REACHED with partial progress");
  offset += limited.sent;
  check(client_drain_available(fixture.client_fd, stress_received, sizeof(stress_received),
                               &received),
        "client drains bytes after bounded partial send");
  transfer_ok = received >= first.sent;
  steps = 0u;
  while (transfer_ok && offset < sizeof(stress_source) && steps < OMNI_TEST_MAX_SEND_STEPS) {
    struct omni_send_result result;

    tail.data = stress_source + offset;
    tail.length = sizeof(stress_source) - offset;
    result = omni_connection_send_drain(&fixture.connection, tail, 8u);
    if (result.status == OMNI_SEND_ERR_INVALID || result.status == OMNI_SEND_ERR_FATAL ||
        result.status == OMNI_SEND_PEER_CLOSED || result.status == OMNI_SEND_INTERRUPTED ||
        result.status == OMNI_SEND_ERR_ZERO_PROGRESS || result.status == OMNI_SEND_ERR_INTERNAL) {
      transfer_ok = false;
      break;
    }
    if (result.sent > tail.length) {
      transfer_ok = false;
      break;
    }
    offset += result.sent;
    if (!client_drain_available(fixture.client_fd, stress_received, sizeof(stress_received),
                                &received)) {
      transfer_ok = false;
      break;
    }
    if (result.sent == 0u) {
      struct pollfd wait_fd;
      int waited = 0;

      wait_fd.fd = omni_connection_fd(&fixture.connection);
      wait_fd.events = POLLOUT | POLLERR | POLLHUP;
      wait_fd.revents = 0;
      waited = poll(&wait_fd, 1u, 1000);
      if (waited <= 0 || (wait_fd.revents & (POLLERR | POLLHUP)) != 0) {
        transfer_ok = false;
        break;
      }
    }
    ++steps;
  }
  check(transfer_ok && offset == sizeof(stress_source) && steps < OMNI_TEST_MAX_SEND_STEPS,
        "caller resumes from each exact returned offset without resend");
  check(client_receive_until(fixture.client_fd, stress_received, sizeof(stress_source),
                             &received),
        "client drains the complete resumed byte stream");
  check(received == sizeof(stress_source) &&
            memcmp(stress_received, stress_source, sizeof(stress_source)) == 0,
        "partial-send resume preserves exact total ordering and contents");
  check(memcmp(stress_source, stress_snapshot, sizeof(stress_source)) == 0,
        "partial and bounded sends never mutate the caller source");
  fixture_stop(&fixture);
}

static int run_peer_failure_child(void) {
  struct connection_fixture fixture;
  struct omni_send_result result;
  struct linger reset_linger;
  unsigned char payload[OMNI_TEST_STRESS_LENGTH];
  size_t offset = 0u;
  size_t attempts = 0u;
  int bystander = OMNI_CONNECTION_FD_INVALID;
  bool failed = false;

  fill_payload(payload, sizeof(payload), 0x5au);
  if (!fixture_start(&fixture, 23u, OMNI_POLLER_INTEREST_WRITE)) {
    fixture_stop(&fixture);
    return 20;
  }
  bystander = open("/dev/null", O_RDONLY);
  if (bystander < 0) {
    fixture_stop(&fixture);
    return 21;
  }
  reset_linger.l_onoff = 1;
  reset_linger.l_linger = 0;
  if (setsockopt(fixture.client_fd, SOL_SOCKET, SO_LINGER, &reset_linger,
                 (socklen_t)sizeof(reset_linger)) != 0) {
    (void)close(bystander);
    fixture_stop(&fixture);
    return 22;
  }
  (void)close(fixture.client_fd);
  fixture.client_fd = OMNI_CONNECTION_FD_INVALID;
  while (attempts < 32u && offset < sizeof(payload)) {
    struct omni_send_span tail;

    tail.data = payload + offset;
    tail.length = sizeof(payload) - offset;
    result = omni_connection_send_drain(&fixture.connection, tail, 8u);
    if (result.sent > tail.length) {
      (void)close(bystander);
      fixture_stop(&fixture);
      return 23;
    }
    offset += result.sent;
    if (result.status == OMNI_SEND_PEER_CLOSED || result.status == OMNI_SEND_ERR_FATAL) {
      failed = true;
      break;
    }
    if (result.status == OMNI_SEND_ERR_INVALID || result.status == OMNI_SEND_ERR_INTERNAL ||
        result.status == OMNI_SEND_ERR_ZERO_PROGRESS || result.status == OMNI_SEND_INTERRUPTED) {
      break;
    }
    ++attempts;
  }
  if (!failed || (result.sys_errno != EPIPE && result.sys_errno != ECONNRESET) ||
      !omni_connection_is_live(&fixture.connection) ||
      !fd_flags_ok(omni_connection_fd(&fixture.connection)) || !fd_open(bystander)) {
    (void)close(bystander);
    fixture_stop(&fixture);
    return 24;
  }
  omni_connection_destroy(&fixture.connection);
  (void)close(bystander);
  if (fixture.listener_started) {
    omni_listener_destroy(&fixture.listener);
    fixture.listener_started = false;
  }
  return 0;
}

static void test_peer_failure_and_sigpipe_survival(void) {
  pid_t child = fork();
  int status = 0;

  check(child >= 0, "peer-failure child starts");
  if (child == 0) {
    _exit(run_peer_failure_child());
  }
  if (child < 0) {
    return;
  }
  check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "peer failure preserves errno/ownership and cannot terminate through SIGPIPE");
}

static void test_deterministic_transfer_stress(void) {
  struct connection_fixture fixture;
  size_t offset = 0u;
  size_t received = 0u;
  size_t steps = 0u;
  bool transfer_ok = true;

  fill_payload(stress_source, sizeof(stress_source), 0x33u);
  memcpy(stress_snapshot, stress_source, sizeof(stress_source));
  memset(stress_received, 0, sizeof(stress_received));
  check(fixture_start(&fixture, 31u, OMNI_POLLER_INTEREST_WRITE),
        "deterministic transfer fixture starts");
  if (!omni_connection_is_live(&fixture.connection)) {
    fixture_stop(&fixture);
    return;
  }
  while (transfer_ok && offset < sizeof(stress_source) && steps < OMNI_TEST_MAX_SEND_STEPS) {
    struct omni_send_result result;
    struct omni_send_span tail;

    tail.data = stress_source + offset;
    tail.length = sizeof(stress_source) - offset;
    result = omni_connection_send_drain(&fixture.connection, tail, 4u);
    if (result.status == OMNI_SEND_ERR_INVALID || result.status == OMNI_SEND_ERR_FATAL ||
        result.status == OMNI_SEND_PEER_CLOSED || result.status == OMNI_SEND_INTERRUPTED ||
        result.status == OMNI_SEND_ERR_ZERO_PROGRESS || result.status == OMNI_SEND_ERR_INTERNAL ||
        result.sent > tail.length) {
      transfer_ok = false;
      break;
    }
    offset += result.sent;
    if (!client_drain_available(fixture.client_fd, stress_received, sizeof(stress_received),
                                &received)) {
      transfer_ok = false;
      break;
    }
    if (result.sent == 0u) {
      struct pollfd wait_fd;
      int waited = 0;

      wait_fd.fd = omni_connection_fd(&fixture.connection);
      wait_fd.events = POLLOUT | POLLERR | POLLHUP;
      wait_fd.revents = 0;
      waited = poll(&wait_fd, 1u, 1000);
      if (waited <= 0) {
        transfer_ok = false;
        break;
      }
    }
    ++steps;
  }
  check(transfer_ok && offset == sizeof(stress_source) && steps < OMNI_TEST_MAX_SEND_STEPS,
        "bounded connection drain transfers deterministic large stream");
  check(client_receive_until(fixture.client_fd, stress_received, sizeof(stress_source),
                             &received),
        "transfer stress drains all client-side TCP fragments");
  check(received == sizeof(stress_source) &&
            memcmp(stress_received, stress_source, sizeof(stress_source)) == 0,
        "transfer stress has exact count, ordering, and no duplicates or gaps");
  check(memcmp(stress_source, stress_snapshot, sizeof(stress_source)) == 0,
        "transfer stress leaves immutable source bytes unchanged");
  fixture_stop(&fixture);
}

static void test_lifecycle_descriptor_stress(void) {
  bool supported = false;
  int before = count_open_fds(&supported);
  unsigned char payload[48];
  unsigned int cycle = 0u;

  fill_payload(payload, sizeof(payload), 0x71u);
  for (cycle = 0u; cycle < OMNI_TEST_LIFECYCLE_CYCLES; ++cycle) {
    struct connection_fixture fixture;
    struct omni_send_result send_result;
    unsigned char received[sizeof(payload)];

    if (!fixture_start(&fixture, (uint64_t)cycle + 100u, OMNI_POLLER_INTEREST_WRITE)) {
      check(false, "lifecycle stress cycle setup succeeds");
      fixture_stop(&fixture);
      break;
    }
    send_result = omni_connection_send_drain(
        &fixture.connection, (struct omni_send_span){ payload, sizeof(payload) }, 4u);
    if (send_result.status != OMNI_SEND_COMPLETE || send_result.sent != sizeof(payload) ||
        !client_receive_exact(fixture.client_fd, received, sizeof(received)) ||
        memcmp(received, payload, sizeof(payload)) != 0) {
      check(false, "lifecycle stress cycle transfers exact bytes");
      fixture_stop(&fixture);
      break;
    }
    omni_connection_destroy(&fixture.connection);
    if (omni_connection_state(&fixture.connection) != OMNI_CONNECTION_CLOSED) {
      check(false, "lifecycle stress cycle reaches CLOSED");
      fixture_stop(&fixture);
      break;
    }
    fixture_stop(&fixture);
  }
  check(cycle == OMNI_TEST_LIFECYCLE_CYCLES,
        "bounded lifecycle stress completes every loopback cycle");
  if (supported) {
    check(count_open_fds(&supported) == before,
          "descriptor census returns to baseline after lifecycle stress");
  } else {
    check(true, "descriptor census unsupported outside Linux; lifecycle checks still ran");
  }
}

int main(void) {
  test_invalid_states_and_failed_initialization();
  test_failed_initialization_preserves_accepted_owner();
  test_lifecycle_binary_io_and_poller();
  test_fd_zero_ownership();
  test_partial_progress_and_resume();
  test_peer_failure_and_sigpipe_survival();
  test_deterministic_transfer_stress();
  test_lifecycle_descriptor_stress();

  printf("%d checks, %d failures\n", check_count, failure_count);
  return failure_count == 0 ? 0 : 1;
}
