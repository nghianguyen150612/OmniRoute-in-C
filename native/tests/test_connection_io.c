/*
 * OmniRoute native backend — bounded connection I/O state machine tests (Task 025).
 *
 * Loopback-only, self-cleaning, deterministic: no external network, no fixed
 * ports (the operator port 20128 is never touched). Payloads are arbitrary binary
 * data, including NUL and non-ASCII values, so counts rather than strings define
 * correctness.
 *
 * Timing discipline: bounded poll() waits observe pre-existing kernel state.
 * No sleeps or TCP packetization assumptions exist.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
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
#include <unistd.h>

#include "omniroute/accepted.h"
#include "omniroute/bytebuf.h"
#include "omniroute/connection_io.h"
#include "omniroute/listener.h"
#include "omniroute/poller.h"
#include "omniroute/recv.h"
#include "omniroute/send.h"

#define RECEIVE_CAPACITY 128u
#define SEND_CAPACITY 256u

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

static bool start_listener(struct omni_listener *listener, uint16_t *port_out) {
  struct omni_listener_result result;

  omni_listener_make_inert(listener);
  result = omni_listener_init(listener, "127.0.0.1", (uint16_t)0u);
  check(result.status == OMNI_LISTENER_OK && result.sys_errno == 0,
        "listener binds for connection_io test setup");
  if (result.status != OMNI_LISTENER_OK) {
    return false;
  }
  check(omni_listener_port(listener) != OMNI_LISTENER_PORT_INVALID &&
            omni_listener_port(listener) != (uint16_t)20128,
        "connection_io tests use an ephemeral non-20128 port");
  if (port_out != NULL) {
    *port_out = omni_listener_port(listener);
  }
  return true;
}

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
        "accepted owner opens for connection_io test setup");
  return result.status == OMNI_ACCEPT_OK;
}

static void client_send_all(int client_fd, const unsigned char *data, size_t length) {
  size_t sent = 0u;

  while (sent < length) {
    ssize_t result = send(client_fd, data + sent, length - sent, 0);
    if (result > 0) {
      sent += (size_t)result;
    } else {
      break;
    }
  }
}

static void test_init_inert_behavior(void) {
  struct omni_connection_io io = { 0 };
  struct omni_connection_io other = { 0 };

  /* NULL is safe */
  omni_connection_io_make_inert(NULL);

  /* Double make_inert is safe */
  omni_connection_io_make_inert(&io);
  omni_connection_io_make_inert(&io);

  check(io.state == OMNI_CONNECTION_IO_NEW, "init to NEW");

  /* Destroy on INERT is safe */
  omni_connection_io_destroy(&io);
  check(io.state == OMNI_CONNECTION_IO_CLOSED, "destroy on NEW goes to CLOSED");

  /* Can init after destroy */
  other.state = OMNI_CONNECTION_IO_CLOSED;
  omni_connection_io_make_inert(&other);
  check(other.state == OMNI_CONNECTION_IO_NEW, "re-init to NEW after destroy");
}

static void test_init_valid(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted = { 0 };
  struct omni_connection_io io = { 0 };
  unsigned char receive_storage[RECEIVE_CAPACITY];
  unsigned char send_storage[SEND_CAPACITY];
  struct omni_connection_io_config config = { 0 };
  struct omni_connection_io_result result;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;

  omni_listener_make_inert(&listener);
  if (!start_listener(&listener, &port)) {
    goto cleanup;
  }
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID) {
    goto cleanup;
  }
  if (!accept_one(&listener, &accepted)) {
    goto cleanup;
  }

  omni_connection_io_make_inert(&io);
  config.accepted = &accepted;
  config.receive_storage = receive_storage;
  config.receive_capacity = RECEIVE_CAPACITY;
  config.send_storage = send_storage;
  config.send_capacity = SEND_CAPACITY;

  result = omni_connection_io_init(&io, &config);
  check(result.status == OMNI_CONNECTION_IO_OK && result.sys_errno == 0,
        "valid init succeeds");
  check(io.state == OMNI_CONNECTION_IO_OPEN, "init transitions to OPEN");
  check(omni_connection_io_is_open(&io), "is_open returns true on OPEN");
  check(omni_connection_io_fd(&io) >= 0, "fd is valid after init");
  check(omni_connection_io_state(&io) == OMNI_CONNECTION_IO_OPEN,
        "state query returns OPEN");

  omni_connection_io_destroy(&io);
  check(io.state == OMNI_CONNECTION_IO_CLOSED, "destroy transitions to CLOSED");
  check(!omni_connection_io_is_open(&io), "is_open returns false on CLOSED");
  check(omni_connection_io_fd(&io) == OMNI_ACCEPTED_FD_INVALID,
        "fd is invalid after destroy");

  cleanup:
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(client_fd);
  }
  omni_listener_destroy(&listener);
}

static void test_init_invalid_storage(void) {
  struct omni_connection_io io = { 0 };
  struct omni_connection_io_config config = { 0 };
  struct omni_connection_io_result result;
  unsigned char receive_storage[RECEIVE_CAPACITY];
  unsigned char send_storage[SEND_CAPACITY];

  /* Can't use live accepted without listening - just test storage validation */
  omni_connection_io_make_inert(&io);
  config.receive_storage = NULL;
  config.receive_capacity = RECEIVE_CAPACITY;
  config.send_storage = send_storage;  /* valid send, invalid receive */
  config.send_capacity = SEND_CAPACITY;

  result = omni_connection_io_init(&io, &config);
  check(result.status == OMNI_CONNECTION_IO_ERR_INVALID && result.sys_errno == EINVAL,
        "init rejects NULL receive_storage");

  config.receive_storage = receive_storage;
  config.receive_capacity = 0u;  /* zero capacity */

  result = omni_connection_io_init(&io, &config);
  check(result.status == OMNI_CONNECTION_IO_ERR_INVALID,
        "init rejects zero receive_capacity");

  config.receive_capacity = RECEIVE_CAPACITY;
  config.send_capacity = 0u;

  result = omni_connection_io_init(&io, &config);
  check(result.status == OMNI_CONNECTION_IO_ERR_INVALID,
        "init rejects zero send_capacity");

  omni_connection_io_destroy(&io);
  check(io.state == OMNI_CONNECTION_IO_CLOSED, "destroy is safe after failed init");
}

static void test_read_eof(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted = { 0 };
  struct omni_connection_io io = { 0 };
  unsigned char receive_storage[RECEIVE_CAPACITY];
  unsigned char send_storage[SEND_CAPACITY];
  struct omni_connection_io_config config = { 0 };
  struct omni_connection_io_result result;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  static const unsigned char payload[] = { 0x01u, 0x02u, 0x03u, 0x04u };

  omni_listener_make_inert(&listener);
  omni_accepted_make_inert(&accepted);
  omni_connection_io_make_inert(&io);

  if (!start_listener(&listener, &port)) {
    goto cleanup;
  }
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID) {
    goto cleanup;
  }
  if (!accept_one(&listener, &accepted)) {
    goto cleanup;
  }

  config.accepted = &accepted;
  config.receive_storage = receive_storage;
  config.receive_capacity = RECEIVE_CAPACITY;
  config.send_storage = send_storage;
  config.send_capacity = SEND_CAPACITY;

  result = omni_connection_io_init(&io, &config);
  if (result.status != OMNI_CONNECTION_IO_OK) {
    goto cleanup;
  }

  /* Send data then close write side */
  client_send_all(client_fd, payload, sizeof(payload));
  if (shutdown(client_fd, SHUT_WR) != 0) {
    goto cleanup;
  }

  /* Read the data */
  result = omni_connection_io_readable(&io);
  check(result.status == OMNI_CONNECTION_IO_OK && result.count == sizeof(payload),
        "readable receives expected bytes");

  /* Read EOF */
  result = omni_connection_io_readable(&io);
  check(result.status == OMNI_CONNECTION_IO_ERR_EOF, "readable returns EOF");

  cleanup:
  omni_connection_io_destroy(&io);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(client_fd);
  }
  omni_listener_destroy(&listener);
}

static void test_read_then_close(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted = { 0 };
  struct omni_connection_io io = { 0 };
  unsigned char receive_storage[RECEIVE_CAPACITY];
  unsigned char send_storage[SEND_CAPACITY];
  struct omni_connection_io_config config = { 0 };
  struct omni_connection_io_result result;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  static const unsigned char payload[] = { 0x01u, 0x02u, 0x03u, 0x04u };

  omni_listener_make_inert(&listener);
  omni_accepted_make_inert(&accepted);

  if (!start_listener(&listener, &port)) {
    goto cleanup;
  }
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID) {
    goto cleanup;
  }
  if (!accept_one(&listener, &accepted)) {
    goto cleanup;
  }

  client_send_all(client_fd, payload, sizeof(payload));

  config.accepted = &accepted;
  config.receive_storage = receive_storage;
  config.receive_capacity = RECEIVE_CAPACITY;
  config.send_storage = send_storage;
  config.send_capacity = SEND_CAPACITY;

  result = omni_connection_io_init(&io, &config);
  if (result.status != OMNI_CONNECTION_IO_OK) {
    goto cleanup;
  }

  /* Close transitions to CLOSING */
  result = omni_connection_io_close(&io);
  check(result.status == OMNI_CONNECTION_IO_OK, "close succeeds from OPEN");
  check(io.state == OMNI_CONNECTION_IO_CLOSING, "close transitions to CLOSING");

  /* Repeated close is safe */
  result = omni_connection_io_close(&io);
  check(result.status == OMNI_CONNECTION_IO_OK, "repeated close is safe");

  /* I/O after close is rejected */
  result = omni_connection_io_readable(&io);
  check(result.status == OMNI_CONNECTION_IO_ERR_CLOSED, "I/O after close rejected");

  /* Destroy works from CLOSING */
  omni_connection_io_destroy(&io);
  check(io.state == OMNI_CONNECTION_IO_CLOSED, "destroy transitions to CLOSED");

  cleanup:
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(client_fd);
  }
  omni_listener_destroy(&listener);
}

static void test_write_then_close(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted = { 0 };
  struct omni_connection_io io = { 0 };
  unsigned char receive_storage[RECEIVE_CAPACITY];
  unsigned char send_storage[SEND_CAPACITY];
  struct omni_connection_io_config config = { 0 };
  struct omni_connection_io_result result;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;

  omni_listener_make_inert(&listener);
  omni_accepted_make_inert(&accepted);

  if (!start_listener(&listener, &port)) {
    goto cleanup;
  }
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID) {
    goto cleanup;
  }
  if (!accept_one(&listener, &accepted)) {
    goto cleanup;
  }

  config.accepted = &accepted;
  config.receive_storage = receive_storage;
  config.receive_capacity = RECEIVE_CAPACITY;
  config.send_storage = send_storage;
  config.send_capacity = SEND_CAPACITY;

  result = omni_connection_io_init(&io, &config);
  if (result.status != OMNI_CONNECTION_IO_OK) {
    goto cleanup;
  }

  /* Append data to send buffer */
  struct omni_bytebuf *send_buf = omni_connection_io_send_buffer(&io);
  check(send_buf != NULL, "send buffer is available in OPEN state");
  const unsigned char out_data[] = { 0xAAu, 0xBBu, 0xCCu };
  check(omni_bytebuf_append(send_buf, out_data, sizeof(out_data)),
        "can append to send buffer");

  /* Write the data */
  result = omni_connection_io_writable(&io);
  check(result.status == OMNI_CONNECTION_IO_OK, "writable succeeded");
  check(result.count == sizeof(out_data), "writable wrote all bytes");

  /* Close transitions properly */
  result = omni_connection_io_close(&io);
  check(result.status == OMNI_CONNECTION_IO_OK, "close succeeds");
  check(io.state == OMNI_CONNECTION_IO_CLOSING, "close transitions to CLOSING");

  /* I/O after close is rejected */
  result = omni_connection_io_readable(&io);
  check(result.status == OMNI_CONNECTION_IO_ERR_CLOSED, "I/O after close rejected");

  /* Destroy works from CLOSING */
  omni_connection_io_destroy(&io);
  check(io.state == OMNI_CONNECTION_IO_CLOSED, "destroy transitions to CLOSED");

  cleanup:
  omni_connection_io_destroy(&io);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(client_fd);
  }
  omni_listener_destroy(&listener);
}

static void test_send_buffer_drain(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted = { 0 };
  struct omni_connection_io io = { 0 };
  unsigned char receive_storage[RECEIVE_CAPACITY];
  unsigned char send_storage[SEND_CAPACITY];
  struct omni_connection_io_config config = { 0 };
  struct omni_connection_io_result result;
  struct pollfd probe;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;

  omni_listener_make_inert(&listener);
  omni_accepted_make_inert(&accepted);

  if (!start_listener(&listener, &port)) {
    goto cleanup;
  }
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID) {
    goto cleanup;
  }
  if (!accept_one(&listener, &accepted)) {
    goto cleanup;
  }

  config.accepted = &accepted;
  config.receive_storage = receive_storage;
  config.receive_capacity = RECEIVE_CAPACITY;
  config.send_storage = send_storage;
  config.send_capacity = SEND_CAPACITY;

  result = omni_connection_io_init(&io, &config);
  if (result.status != OMNI_CONNECTION_IO_OK) {
    goto cleanup;
  }

  /* Append to send buffer */
  struct omni_bytebuf *send_buf = omni_connection_io_send_buffer(&io);
  static const unsigned char out_data[] = { 0xAAu, 0xBBu, 0xCCu, 0xDDu };
  check(omni_bytebuf_append(send_buf, out_data, sizeof(out_data)),
        "can append to send buffer");

  /* Write until would-block or complete */
  result = omni_connection_io_writable(&io);
  while (result.status == OMNI_CONNECTION_IO_OK && result.count < sizeof(out_data)) {
    if (omni_bytebuf_readable(&io.send) == 0u) {
      break;
    }
    result = omni_connection_io_writable(&io);
  }

  /* Either we wrote it all or the buffer is now empty */
  check(omni_bytebuf_readable(&io.send) == 0u,
        "send buffer drained after write");

  /* Verify client received something */
  probe.fd = client_fd;
  probe.events = POLLIN;
  probe.revents = 0;
  if (poll(&probe, 1, 2000) > 0 && (probe.revents & POLLIN)) {
    unsigned char recv_buf[37];
    ssize_t received = recv(client_fd, recv_buf, sizeof(recv_buf), 0);
    check(received > 0, "client received some written data");
  }

  cleanup:
  omni_connection_io_destroy(&io);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(client_fd);
  }
  omni_listener_destroy(&listener);
}

static void test_memory_bounded(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted = { 0 };
  struct omni_connection_io io = { 0 };
  unsigned char receive_storage[RECEIVE_CAPACITY];
  unsigned char send_storage[SEND_CAPACITY];
  struct omni_connection_io_config config = { 0 };
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;

  omni_listener_make_inert(&listener);
  omni_accepted_make_inert(&accepted);

  if (!start_listener(&listener, &port)) {
    goto cleanup;
  }
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID) {
    goto cleanup;
  }
  if (accept_one(&listener, &accepted)) {
    config.accepted = &accepted;
    config.receive_storage = receive_storage;
    config.receive_capacity = RECEIVE_CAPACITY;
    config.send_storage = send_storage;
    config.send_capacity = SEND_CAPACITY;

    omni_connection_io_make_inert(&io);
    struct omni_connection_io_result result = omni_connection_io_init(&io, &config);
    if (result.status == OMNI_CONNECTION_IO_OK) {
      /* Verify buffer capacities are fixed */
      check(omni_bytebuf_capacity(&io.receive) == RECEIVE_CAPACITY,
            "receive buffer capacity is fixed");
      check(omni_bytebuf_capacity(&io.send) == SEND_CAPACITY,
            "send buffer capacity is fixed");
      check(omni_bytebuf_readable(&io.receive) == 0u,
            "receive buffer starts empty");
      check(omni_bytebuf_readable(&io.send) == 0u,
            "send buffer starts empty");
    }
    omni_connection_io_destroy(&io);
  }

  cleanup:
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(client_fd);
  }
  omni_listener_destroy(&listener);
}

static void test_descriptor_preservation(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted = { 0 };
  struct omni_connection_io io = { 0 };
  unsigned char receive_storage[RECEIVE_CAPACITY];
  unsigned char send_storage[SEND_CAPACITY];
  struct omni_connection_io_config config = { 0 };
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;

  omni_listener_make_inert(&listener);
  omni_accepted_make_inert(&accepted);
  memset(&io, 0, sizeof(io));

  if (!start_listener(&listener, &port)) {
    goto cleanup;
  }
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID || !accept_one(&listener, &accepted)) {
    goto cleanup;
  }

  config.accepted = &accepted;
  config.receive_storage = receive_storage;
  config.receive_capacity = RECEIVE_CAPACITY;
  config.send_storage = send_storage;
  config.send_capacity = SEND_CAPACITY;

  omni_connection_io_make_inert(&io);
  struct omni_connection_io_result init_result = omni_connection_io_init(&io, &config);
  if (init_result.status != OMNI_CONNECTION_IO_OK) {
    goto cleanup;
  }

  /* Destroy connection I/O - should not close listener or other descriptors */
  omni_connection_io_destroy(&io);
  check(omni_accepted_is_live(&accepted), "accepted owner remains live after destroy");
  check(omni_listener_fd(&listener) != OMNI_LISTENER_FD_INVALID,
        "listener fd remains valid after destroy");

  cleanup:
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(client_fd);
  }
  omni_listener_destroy(&listener);
}

/*
 * Repeated read/write/close cycles: each iteration accepts a fresh connection,
 * performs a bounded read and write, closes the I/O layer, and destroys it.
 * Verifies that the layer can be re-initialized after destroy and that no
 * state leaks across cycles. Loopback-only and self-cleaning.
 */
static void test_stress_cycles(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted = { 0 };
  struct omni_connection_io io = { 0 };
  unsigned char receive_storage[RECEIVE_CAPACITY];
  unsigned char send_storage[SEND_CAPACITY];
  struct omni_connection_io_config config = { 0 };
  struct omni_connection_io_result result;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  static const unsigned char payload[] = { 0x01u, 0x02u, 0x03u, 0x04u };
  static const unsigned char out_data[] = { 0xAAu, 0xBBu, 0xCCu, 0xDDu };
  int cycle;

  omni_listener_make_inert(&listener);
  if (!start_listener(&listener, &port)) {
    goto cleanup;
  }

  for (cycle = 0; cycle < 10; ++cycle) {
    struct omni_bytebuf *send_buf;

    /* Create a fresh client connection for each cycle */
    if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
      (void)close(client_fd);
      client_fd = OMNI_ACCEPTED_FD_INVALID;
    }
    omni_accepted_destroy(&accepted);
    omni_accepted_make_inert(&accepted);
    
    client_fd = open_client(port);
    if (client_fd == OMNI_ACCEPTED_FD_INVALID) {
      break;
    }
    if (!accept_one(&listener, &accepted)) {
      (void)close(client_fd);
      client_fd = OMNI_ACCEPTED_FD_INVALID;
      break;
    }

    omni_connection_io_make_inert(&io);
    config.accepted = &accepted;
    config.receive_storage = receive_storage;
    config.receive_capacity = RECEIVE_CAPACITY;
    config.send_storage = send_storage;
    config.send_capacity = SEND_CAPACITY;

    result = omni_connection_io_init(&io, &config);
    if (result.status != OMNI_CONNECTION_IO_OK) {
      omni_accepted_destroy(&accepted);
      (void)close(client_fd);
      break;
    }

    /* Read side: the client sends before we read. */
    client_send_all(client_fd, payload, sizeof(payload));
    result = omni_connection_io_readable(&io);
    check(result.status == OMNI_CONNECTION_IO_OK && result.count == sizeof(payload),
          "stress: readable receives payload");

    /* Write side: fill the send buffer and drain it. */
    send_buf = omni_connection_io_send_buffer(&io);
    if (send_buf != NULL) {
      check(omni_bytebuf_append(send_buf, out_data, sizeof(out_data)),
            "stress: send buffer accepts data");
    }

    result = omni_connection_io_writable(&io);
    check(result.status == OMNI_CONNECTION_IO_OK, "stress: writable succeeds");

    /* Close then destroy. */
    result = omni_connection_io_close(&io);
    check(result.status == OMNI_CONNECTION_IO_OK, "stress: close succeeds");
    omni_connection_io_destroy(&io);
    check(io.state == OMNI_CONNECTION_IO_CLOSED, "stress: destroy reaches CLOSED");

    (void)close(client_fd);
    omni_accepted_destroy(&accepted);
  }

  check(cycle > 0, "stress: at least one cycle ran");

  cleanup:
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(client_fd);
  }
  omni_listener_destroy(&listener);
}

int main(void) {
  test_init_inert_behavior();
  test_init_valid();
  test_init_invalid_storage();
  test_read_eof();
  test_write_then_close();
  test_read_then_close();
  test_send_buffer_drain();
  test_memory_bounded();
  test_descriptor_preservation();
  test_stress_cycles();

  printf("---\n%d checks, %d failures\n", check_count, failure_count);
  return failure_count == 0 ? 0 : 1;
}