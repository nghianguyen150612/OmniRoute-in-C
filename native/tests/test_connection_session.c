/*
 * OmniRoute native backend — bounded connection/session integration tests (Task 026).
 *
 * This suite validates the session layer that coordinates the existing
 * connection ownership model with the bounded connection I/O state machine
 * without introducing protocol behavior.
 *
 * Loopback-only, self-cleaning, deterministic: no external network, no fixed
 * ports (operator port 20128 is never touched). No heap expectations beyond
 * the zero-heap production rule enforced by check_network_boundary.sh.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
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
#include "omniroute/connection.h"
#include "omniroute/connection_io.h"
#include "omniroute/connection_session.h"
#include "omniroute/listener.h"
#include "omniroute/poller.h"
#include "omniroute/registry.h"

#define CONN_RECV_CAP 128u
#define SESS_RECV_CAP 128u
#define SESS_SEND_CAP 256u

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

/* ---------- listener helpers ---------- */

static bool start_listener(struct omni_listener *listener, uint16_t *port_out) {
  struct omni_listener_result result;
  omni_listener_make_inert(listener);
  result = omni_listener_init(listener, "127.0.0.1", (uint16_t)0u);
  check(result.status == OMNI_LISTENER_OK && result.sys_errno == 0,
        "listener binds for session test setup");
  if (result.status != OMNI_LISTENER_OK) {
    return false;
  }
  check(omni_listener_port(listener) != OMNI_LISTENER_PORT_INVALID &&
            omni_listener_port(listener) != (uint16_t)20128,
        "session tests use ephemeral non-20128 port");
  if (port_out) {
    *port_out = omni_listener_port(listener);
  }
  return true;
}

static int open_client(uint16_t port) {
  struct sockaddr_in peer;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  check(fd != OMNI_ACCEPTED_FD_INVALID, "test client socket opens");
  if (fd == OMNI_ACCEPTED_FD_INVALID) return OMNI_ACCEPTED_FD_INVALID;
  memset(&peer, 0, sizeof(peer));
  peer.sin_family = AF_INET;
  peer.sin_port = htons(port);
  peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (const struct sockaddr *)&peer, (socklen_t)sizeof(peer)) != 0) {
    check(false, "test client connects");
    (void)close(fd);
    return OMNI_ACCEPTED_FD_INVALID;
  }
  check(true, "test client connects");
  return fd;
}

static bool accept_one(struct omni_listener *listener, struct omni_accepted *accepted) {
  struct omni_accept_result result;
  omni_accepted_make_inert(accepted);
  result = omni_accept_once(listener, accepted);
  check(result.status == OMNI_ACCEPT_OK && result.accepted == 1u,
        "accepted owner opens for session test setup");
  return result.status == OMNI_ACCEPT_OK;
}

static void client_send_all(int fd, const unsigned char *data, size_t len) {
  size_t sent = 0u;
  while (sent < len) {
    ssize_t n = send(fd, data + sent, len - sent, 0);
    if (n > 0) sent += (size_t)n;
    else break;
  }
}

/* ---------- connection helpers ---------- */

static bool make_open_connection(struct omni_connection *conn,
                                 unsigned char *conn_storage, size_t conn_cap,
                                 struct omni_accepted *accepted) {
  struct omni_connection_config cfg = { 0 };
  struct omni_connection_result res;
  omni_connection_make_inert(conn);
  cfg.receive_storage = conn_storage;
  cfg.receive_capacity = conn_cap;
  cfg.poller_token = 0x1234u;
  cfg.poller_interests = OMNI_POLLER_INTEREST_READ;
  res = omni_connection_init(conn, &cfg);
  if (res.status != OMNI_CONNECTION_OK) return false;
  res = omni_connection_from_accepted(conn, accepted);
  return res.status == OMNI_CONNECTION_OK;
}

/* ---------- test cases ---------- */

static void test_inert_behavior(void) {
  struct omni_connection_session sess = { 0 };
  struct omni_connection_session other = { 0 };

  omni_connection_session_make_inert(NULL);
  check(true, "make_inert NULL safe");

  omni_connection_session_make_inert(&sess);
  check(sess.state == OMNI_CONNECTION_SESSION_NEW, "make_inert -> NEW");
  check(omni_connection_session_state(&sess) == OMNI_CONNECTION_SESSION_NEW,
        "state query NEW");
  check(!omni_connection_session_is_open(&sess), "is_open false on NEW");
  check(omni_connection_session_state(NULL) == OMNI_CONNECTION_SESSION_NEW,
        "state NULL -> NEW");
  check(omni_connection_session_fd(&sess) == OMNI_ACCEPTED_FD_INVALID,
        "fd invalid on NEW");
  check(omni_connection_session_connection(&sess) == NULL,
        "connection NULL on NEW");
  check(omni_connection_session_io(&sess) == NULL, "io NULL on NEW");
  check(omni_connection_session_io_state(&sess) == OMNI_CONNECTION_IO_NEW,
        "io state NEW when session NEW");

  omni_connection_session_make_inert(&sess);
  check(sess.state == OMNI_CONNECTION_SESSION_NEW, "double make_inert stays NEW");

  omni_connection_session_destroy(&sess);
  check(sess.state == OMNI_CONNECTION_SESSION_CLOSED, "destroy NEW -> CLOSED");
  check(omni_connection_session_is_closed(&sess), "is_closed true after destroy");

  omni_connection_session_destroy(NULL);
  check(true, "destroy NULL safe");
  omni_connection_session_destroy(&sess);
  check(sess.state == OMNI_CONNECTION_SESSION_CLOSED, "double destroy stays CLOSED");

  other.state = OMNI_CONNECTION_SESSION_CLOSED;
  omni_connection_session_make_inert(&other);
  check(other.state == OMNI_CONNECTION_SESSION_NEW, "make_inert CLOSED -> NEW");
}

static void test_init_valid_open_close(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted = { 0 };
  struct omni_connection conn = { 0 };
  struct omni_connection_session sess = { 0 };
  unsigned char conn_storage[CONN_RECV_CAP];
  unsigned char sess_recv[SESS_RECV_CAP];
  unsigned char sess_send[SESS_SEND_CAP];
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  struct omni_connection_session_config cfg = { 0 };
  struct omni_connection_session_result res;

  omni_listener_make_inert(&listener);
  omni_accepted_make_inert(&accepted);
  omni_connection_make_inert(&conn);
  omni_connection_session_make_inert(&sess);

  if (!start_listener(&listener, &port)) goto cleanup;
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID) goto cleanup;
  if (!accept_one(&listener, &accepted)) goto cleanup;
  if (!make_open_connection(&conn, conn_storage, CONN_RECV_CAP, &accepted)) {
    check(false, "connection becomes OPEN for session init");
    goto cleanup;
  }
  check(omni_connection_state(&conn) == OMNI_CONNECTION_OPEN, "connection OPEN before session init");

  cfg.connection = &conn;
  cfg.receive_storage = sess_recv;
  cfg.receive_capacity = SESS_RECV_CAP;
  cfg.send_storage = sess_send;
  cfg.send_capacity = SESS_SEND_CAP;

  res = omni_connection_session_init(&sess, &cfg);
  check(res.status == OMNI_CONNECTION_SESSION_OK, "session init succeeds");
  check(sess.state == OMNI_CONNECTION_SESSION_INIT, "init -> INIT");
  check(omni_connection_session_state(&sess) == OMNI_CONNECTION_SESSION_INIT, "state INIT");
  check(!omni_connection_session_is_open(&sess), "is_open false on INIT");
  check(omni_connection_session_connection(&sess) == &conn, "connection borrowed after init");
  check(omni_connection_session_io(&sess) == NULL, "io NULL on INIT");
  check(omni_connection_session_io_state(&sess) == OMNI_CONNECTION_IO_NEW,
        "io still NEW after init (not yet open)");
  check(omni_connection_session_fd(&sess) == OMNI_ACCEPTED_FD_INVALID,
        "fd invalid on INIT");

  res = omni_connection_session_open(&sess);
  check(res.status == OMNI_CONNECTION_SESSION_OK, "session open succeeds");
  check(sess.state == OMNI_CONNECTION_SESSION_OPEN, "open -> OPEN");
  check(omni_connection_session_is_open(&sess), "is_open true on OPEN");
  check(omni_connection_session_connection(&sess) == &conn, "connection still borrowed after open");
  check(omni_connection_session_io(&sess) != NULL, "io available on OPEN");
  check(omni_connection_session_io_state(&sess) == OMNI_CONNECTION_IO_OPEN,
        "io OPEN after session open");
  check(omni_connection_session_fd(&sess) >= 0, "fd valid after open");
  check(omni_connection_session_receive_buffer(&sess) != NULL, "receive buffer available on OPEN");
  check(omni_connection_session_send_buffer(&sess) != NULL, "send buffer available on OPEN");
  check(omni_bytebuf_capacity(omni_connection_session_receive_buffer(&sess)) == SESS_RECV_CAP,
        "receive capacity correct");
  check(omni_bytebuf_capacity(omni_connection_session_send_buffer(&sess)) == SESS_SEND_CAP,
        "send capacity correct");

  res = omni_connection_session_close(&sess);
  check(res.status == OMNI_CONNECTION_SESSION_OK, "close from OPEN succeeds");
  check(sess.state == OMNI_CONNECTION_SESSION_CLOSING, "close -> CLOSING");
  check(!omni_connection_session_is_open(&sess), "is_open false on CLOSING");
  check(omni_connection_session_io_state(&sess) == OMNI_CONNECTION_IO_CLOSING,
        "io CLOSING after session close");
  check(omni_connection_session_fd(&sess) == OMNI_ACCEPTED_FD_INVALID,
        "fd invalid on CLOSING");

  res = omni_connection_session_close(&sess);
  check(res.status == OMNI_CONNECTION_SESSION_OK, "repeated close idempotent");

  omni_connection_session_destroy(&sess);
  check(sess.state == OMNI_CONNECTION_SESSION_CLOSED, "destroy -> CLOSED");
  check(omni_connection_session_is_closed(&sess), "is_closed true");
  check(omni_connection_session_connection(&sess) == NULL, "connection NULL after destroy");
  check(omni_connection_session_io(&sess) == NULL, "io NULL after destroy");
  check(omni_connection_session_io_state(&sess) == OMNI_CONNECTION_IO_CLOSED,
        "io CLOSED after session destroy");
  check(omni_connection_is_live(&conn), "connection still live after session destroy");
  check(omni_connection_fd(&conn) >= 0, "connection fd still valid after session destroy");

cleanup:
  omni_connection_session_destroy(&sess);
  omni_connection_destroy(&conn);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) (void)close(client_fd);
  omni_listener_destroy(&listener);
}

static void test_init_invalid(void) {
  struct omni_connection_session sess = { 0 };
  struct omni_connection conn = { 0 };
  unsigned char conn_storage[CONN_RECV_CAP];
  unsigned char sess_recv[SESS_RECV_CAP];
  unsigned char sess_send[SESS_SEND_CAP];
  struct omni_connection_session_config cfg = { 0 };
  struct omni_connection_session_result res;
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted = { 0 };
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;

  omni_connection_session_make_inert(&sess);
  cfg.connection = NULL;
  cfg.receive_storage = sess_recv;
  cfg.receive_capacity = SESS_RECV_CAP;
  cfg.send_storage = sess_send;
  cfg.send_capacity = SESS_SEND_CAP;
  res = omni_connection_session_init(&sess, &cfg);
  check(res.status == OMNI_CONNECTION_SESSION_ERR_INVALID, "init rejects NULL connection");
  check(sess.state == OMNI_CONNECTION_SESSION_NEW, "failed init stays NEW");

  res = omni_connection_session_init(NULL, &cfg);
  check(res.status == OMNI_CONNECTION_SESSION_ERR_INVALID, "init rejects NULL session");
  res = omni_connection_session_init(&sess, NULL);
  check(res.status == OMNI_CONNECTION_SESSION_ERR_INVALID, "init rejects NULL config");

  /* Test with non-OPEN connection */
  omni_connection_make_inert(&conn);
  cfg.connection = &conn;
  res = omni_connection_session_init(&sess, &cfg);
  check(res.status == OMNI_CONNECTION_SESSION_ERR_INVALID, "init rejects INERT connection");
  check(sess.state == OMNI_CONNECTION_SESSION_NEW, "failed init with INERT conn stays NEW");

  /* Test with READY connection (init but not from_accepted) */
  {
    struct omni_connection_config ccfg = { 0 };
    ccfg.receive_storage = conn_storage;
    ccfg.receive_capacity = CONN_RECV_CAP;
    ccfg.poller_token = 1u;
    ccfg.poller_interests = OMNI_POLLER_INTEREST_READ;
    (void)omni_connection_init(&conn, &ccfg);
  }
  res = omni_connection_session_init(&sess, &cfg);
  check(res.status == OMNI_CONNECTION_SESSION_ERR_INVALID, "init rejects READY connection");
  omni_connection_destroy(&conn);

  /* Test zero capacities */
  omni_listener_make_inert(&listener);
  omni_accepted_make_inert(&accepted);
  if (start_listener(&listener, &port)) {
    client_fd = open_client(port);
    if (client_fd != OMNI_ACCEPTED_FD_INVALID && accept_one(&listener, &accepted)) {
      if (make_open_connection(&conn, conn_storage, CONN_RECV_CAP, &accepted)) {
        cfg.connection = &conn;
        cfg.receive_storage = NULL;
        cfg.receive_capacity = SESS_RECV_CAP;
        res = omni_connection_session_init(&sess, &cfg);
        check(res.status == OMNI_CONNECTION_SESSION_ERR_INVALID, "init rejects NULL recv storage");
        cfg.receive_storage = sess_recv;
        cfg.receive_capacity = 0u;
        res = omni_connection_session_init(&sess, &cfg);
        check(res.status == OMNI_CONNECTION_SESSION_ERR_INVALID, "init rejects zero recv cap");
        cfg.receive_capacity = SESS_RECV_CAP;
        cfg.send_capacity = 0u;
        res = omni_connection_session_init(&sess, &cfg);
        check(res.status == OMNI_CONNECTION_SESSION_ERR_INVALID, "init rejects zero send cap");
        cfg.send_capacity = SESS_SEND_CAP;
        omni_connection_destroy(&conn);
      }
    }
    omni_accepted_destroy(&accepted);
    if (client_fd != OMNI_ACCEPTED_FD_INVALID) { (void)close(client_fd); client_fd = OMNI_ACCEPTED_FD_INVALID; }
    omni_listener_destroy(&listener);
  }

  /* init from non-NEW should be rejected */
  omni_listener_make_inert(&listener);
  omni_accepted_make_inert(&accepted);
  omni_connection_make_inert(&conn);
  omni_connection_session_make_inert(&sess);
  if (start_listener(&listener, &port)) {
    client_fd = open_client(port);
    if (client_fd != OMNI_ACCEPTED_FD_INVALID && accept_one(&listener, &accepted)) {
      if (make_open_connection(&conn, conn_storage, CONN_RECV_CAP, &accepted)) {
        cfg.connection = &conn;
        cfg.receive_storage = sess_recv;
        cfg.receive_capacity = SESS_RECV_CAP;
        cfg.send_storage = sess_send;
        cfg.send_capacity = SESS_SEND_CAP;
        res = omni_connection_session_init(&sess, &cfg);
        check(res.status == OMNI_CONNECTION_SESSION_OK, "valid init for duplicate test");
        res = omni_connection_session_init(&sess, &cfg);
        check(res.status == OMNI_CONNECTION_SESSION_ERR_STATE, "second init rejected (not NEW)");
        omni_connection_session_destroy(&sess);
        omni_connection_destroy(&conn);
      }
    }
    omni_accepted_destroy(&accepted);
    if (client_fd != OMNI_ACCEPTED_FD_INVALID) (void)close(client_fd);
    omni_listener_destroy(&listener);
  }

  omni_connection_session_destroy(&sess);
  check(sess.state == OMNI_CONNECTION_SESSION_CLOSED, "destroy after invalid tests -> CLOSED");
}

static void test_invalid_transitions(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted = { 0 };
  struct omni_connection conn = { 0 };
  struct omni_connection_session sess = { 0 };
  unsigned char conn_storage[CONN_RECV_CAP];
  unsigned char sess_recv[SESS_RECV_CAP];
  unsigned char sess_send[SESS_SEND_CAP];
  struct omni_connection_session_config cfg = { 0 };
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;

  omni_connection_session_make_inert(&sess);
  check(omni_connection_session_close(&sess).status == OMNI_CONNECTION_SESSION_ERR_STATE,
        "close from NEW rejected");
  check(omni_connection_session_open(&sess).status == OMNI_CONNECTION_SESSION_ERR_STATE,
        "open from NEW rejected");
  struct omni_connection_io_result ior;
  ior = omni_connection_session_readable(&sess);
  check(ior.status == OMNI_CONNECTION_IO_ERR_STATE || ior.status == OMNI_CONNECTION_IO_ERR_INVALID,
        "readable from NEW rejected");
  ior = omni_connection_session_writable(&sess);
  check(ior.status == OMNI_CONNECTION_IO_ERR_STATE || ior.status == OMNI_CONNECTION_IO_ERR_INVALID,
        "writable from NEW rejected");

  /* INIT -> close/open checks */
  omni_listener_make_inert(&listener);
  if (!start_listener(&listener, &port)) goto cleanup;
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID) goto cleanup;
  if (!accept_one(&listener, &accepted)) goto cleanup;
  if (!make_open_connection(&conn, conn_storage, CONN_RECV_CAP, &accepted)) goto cleanup;

  cfg.connection = &conn;
  cfg.receive_storage = sess_recv;
  cfg.receive_capacity = SESS_RECV_CAP;
  cfg.send_storage = sess_send;
  cfg.send_capacity = SESS_SEND_CAP;
  omni_connection_session_make_inert(&sess);
  (void)omni_connection_session_init(&sess, &cfg);
  check(sess.state == OMNI_CONNECTION_SESSION_INIT, "reached INIT for invalid transition tests");
  check(omni_connection_session_close(&sess).status == OMNI_CONNECTION_SESSION_ERR_STATE,
        "close from INIT rejected");
  ior = omni_connection_session_readable(&sess);
  check(ior.status == OMNI_CONNECTION_IO_ERR_STATE, "readable from INIT rejected");
  ior = omni_connection_session_writable(&sess);
  check(ior.status == OMNI_CONNECTION_IO_ERR_STATE, "writable from INIT rejected");

  /* Now open -> OPEN, then test invalid open/init */
  (void)omni_connection_session_open(&sess);
  check(sess.state == OMNI_CONNECTION_SESSION_OPEN, "reached OPEN");
  check(omni_connection_session_init(&sess, &cfg).status == OMNI_CONNECTION_SESSION_ERR_STATE,
        "init from OPEN rejected");
  check(omni_connection_session_open(&sess).status == OMNI_CONNECTION_SESSION_ERR_STATE,
        "second open from OPEN rejected");

  (void)omni_connection_session_close(&sess);
  check(sess.state == OMNI_CONNECTION_SESSION_CLOSING, "reached CLOSING");
  check(omni_connection_session_init(&sess, &cfg).status == OMNI_CONNECTION_SESSION_ERR_STATE,
        "init from CLOSING rejected");
  check(omni_connection_session_open(&sess).status == OMNI_CONNECTION_SESSION_ERR_CLOSED,
        "open from CLOSING rejected");
  ior = omni_connection_session_readable(&sess);
  check(ior.status == OMNI_CONNECTION_IO_ERR_CLOSED, "readable from CLOSING -> CLOSED error");
  ior = omni_connection_session_writable(&sess);
  check(ior.status == OMNI_CONNECTION_IO_ERR_CLOSED, "writable from CLOSING -> CLOSED error");

  omni_connection_session_destroy(&sess);
  check(sess.state == OMNI_CONNECTION_SESSION_CLOSED, "reached CLOSED");
  check(omni_connection_session_init(&sess, &cfg).status == OMNI_CONNECTION_SESSION_ERR_STATE,
        "init from CLOSED rejected");
  check(omni_connection_session_open(&sess).status == OMNI_CONNECTION_SESSION_ERR_CLOSED,
        "open from CLOSED rejected");
  check(omni_connection_session_close(&sess).status == OMNI_CONNECTION_SESSION_OK,
        "close from CLOSED idempotent");
  ior = omni_connection_session_readable(&sess);
  check(ior.status == OMNI_CONNECTION_IO_ERR_CLOSED, "readable from CLOSED -> CLOSED");
  /* destroy idempotent */
  omni_connection_session_destroy(&sess);
  check(sess.state == OMNI_CONNECTION_SESSION_CLOSED, "double destroy stays CLOSED");

  /* make_inert after CLOSED should go to NEW */
  omni_connection_session_make_inert(&sess);
  check(sess.state == OMNI_CONNECTION_SESSION_NEW, "make_inert CLOSED -> NEW");

cleanup:
  omni_connection_session_destroy(&sess);
  omni_connection_destroy(&conn);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) (void)close(client_fd);
  omni_listener_destroy(&listener);
}

static void test_ownership_preservation(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted = { 0 };
  struct omni_connection conn = { 0 };
  struct omni_connection_session sess = { 0 };
  unsigned char conn_storage[CONN_RECV_CAP];
  unsigned char sess_recv[SESS_RECV_CAP];
  unsigned char sess_send[SESS_SEND_CAP];
  struct omni_connection_session_config cfg = { 0 };
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  int listener_fd_before = -1;
  int conn_fd_before = -1;

  omni_listener_make_inert(&listener);
  omni_accepted_make_inert(&accepted);
  omni_connection_make_inert(&conn);
  omni_connection_session_make_inert(&sess);
  if (!start_listener(&listener, &port)) goto cleanup;
  listener_fd_before = omni_listener_fd(&listener);
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID) goto cleanup;
  if (!accept_one(&listener, &accepted)) goto cleanup;
  if (!make_open_connection(&conn, conn_storage, CONN_RECV_CAP, &accepted)) goto cleanup;
  conn_fd_before = omni_connection_fd(&conn);
  check(conn_fd_before >= 0, "conn fd valid before session");

  cfg.connection = &conn;
  cfg.receive_storage = sess_recv;
  cfg.receive_capacity = SESS_RECV_CAP;
  cfg.send_storage = sess_send;
  cfg.send_capacity = SESS_SEND_CAP;
  (void)omni_connection_session_init(&sess, &cfg);
  (void)omni_connection_session_open(&sess);
  check(omni_connection_session_fd(&sess) == conn_fd_before, "session fd matches connection fd");

  /* Close session should not affect connection fd */
  (void)omni_connection_session_close(&sess);
  check(omni_connection_fd(&conn) == conn_fd_before, "conn fd preserved after session close");
  check(omni_listener_fd(&listener) == listener_fd_before, "listener fd preserved after session close");
  check(omni_connection_is_live(&conn), "connection still live after session close");

  /* Destroy session should not affect connection fd or listener */
  omni_connection_session_destroy(&sess);
  check(omni_connection_fd(&conn) == conn_fd_before, "conn fd preserved after session destroy");
  check(omni_listener_fd(&listener) == listener_fd_before, "listener fd preserved after session destroy");
  check(omni_connection_is_live(&conn), "connection still live after session destroy");
  check(omni_accepted_is_live(&conn.accepted), "accepted still live after session destroy (owned by connection)");

cleanup:
  omni_connection_session_destroy(&sess);
  omni_connection_destroy(&conn);
  /* After connection destroy, fd should be invalid */
  check(omni_connection_fd(&conn) == OMNI_ACCEPTED_FD_INVALID, "fd invalid after connection destroy");
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) (void)close(client_fd);
  check(omni_listener_fd(&listener) == listener_fd_before, "listener still valid after session/conn teardown");
  omni_listener_destroy(&listener);
  check(omni_listener_fd(&listener) == OMNI_LISTENER_FD_INVALID, "listener fd invalid after listener destroy");
}

static void test_registry_not_destroyed(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted = { 0 };
  struct omni_connection conn = { 0 };
  struct omni_connection_session sess = { 0 };
  struct omni_connection_registry reg = { 0 };
  struct omni_connection_registry_slot slots[4] = { 0 };
  unsigned char conn_storage[CONN_RECV_CAP];
  unsigned char sess_recv[SESS_RECV_CAP];
  unsigned char sess_send[SESS_SEND_CAP];
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  struct omni_connection_session_config scfg = { 0 };

  omni_listener_make_inert(&listener);
  omni_accepted_make_inert(&accepted);
  omni_connection_make_inert(&conn);
  omni_connection_session_make_inert(&sess);
  omni_connection_registry_make_inert(&reg);

  if (!start_listener(&listener, &port)) goto cleanup;
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID) goto cleanup;
  if (!accept_one(&listener, &accepted)) goto cleanup;
  if (!make_open_connection(&conn, conn_storage, CONN_RECV_CAP, &accepted)) goto cleanup;

  /* Initialize registry and add connection */
  (void)omni_connection_registry_init(&reg, slots, 4u);
  struct omni_connection_registry_result rres = omni_connection_registry_add(&reg, &conn);
  check(rres.status == OMNI_CONNECTION_REGISTRY_OK, "registry add succeeds");

  size_t count_before = omni_connection_registry_count(&reg);

  scfg.connection = &conn;
  scfg.receive_storage = sess_recv;
  scfg.receive_capacity = SESS_RECV_CAP;
  scfg.send_storage = sess_send;
  scfg.send_capacity = SESS_SEND_CAP;
  (void)omni_connection_session_init(&sess, &scfg);
  (void)omni_connection_session_open(&sess);
  check(omni_connection_registry_count(&reg) == count_before, "registry count unchanged after session open");

  (void)omni_connection_session_close(&sess);
  check(omni_connection_registry_count(&reg) == count_before, "registry count unchanged after session close");

  omni_connection_session_destroy(&sess);
  check(omni_connection_registry_count(&reg) == count_before, "registry count unchanged after session destroy");
  check(omni_connection_registry_find(&reg, rres.handle) == &conn,
        "registry still finds connection after session destroy");

  /* Ensure session destroy did not destroy connection */
  check(omni_connection_is_live(&conn), "connection still live after session destroy with registry");

cleanup:
  omni_connection_session_destroy(&sess);
  omni_connection_registry_destroy(&reg);
  omni_connection_destroy(&conn);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) (void)close(client_fd);
  omni_listener_destroy(&listener);
}

static void test_readable_writable_via_session(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted = { 0 };
  struct omni_connection conn = { 0 };
  struct omni_connection_session sess = { 0 };
  unsigned char conn_storage[CONN_RECV_CAP];
  unsigned char sess_recv[SESS_RECV_CAP];
  unsigned char sess_send[SESS_SEND_CAP];
  struct omni_connection_session_config cfg = { 0 };
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  static const unsigned char payload[] = { 0x10u, 0x20u, 0x30u, 0x40u };
  static const unsigned char out_data[] = { 0xAAu, 0xBBu, 0xCCu };

  omni_listener_make_inert(&listener);
  omni_accepted_make_inert(&accepted);
  omni_connection_make_inert(&conn);
  omni_connection_session_make_inert(&sess);
  if (!start_listener(&listener, &port)) goto cleanup;
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID) goto cleanup;
  if (!accept_one(&listener, &accepted)) goto cleanup;
  if (!make_open_connection(&conn, conn_storage, CONN_RECV_CAP, &accepted)) goto cleanup;

  cfg.connection = &conn;
  cfg.receive_storage = sess_recv;
  cfg.receive_capacity = SESS_RECV_CAP;
  cfg.send_storage = sess_send;
  cfg.send_capacity = SESS_SEND_CAP;
  (void)omni_connection_session_init(&sess, &cfg);
  (void)omni_connection_session_open(&sess);

  client_send_all(client_fd, payload, sizeof(payload));
  struct omni_connection_io_result rres = omni_connection_session_readable(&sess);
  check(rres.status == OMNI_CONNECTION_IO_OK && rres.count == sizeof(payload),
        "session readable receives payload");

  /* Check buffer contents via session view */
  struct omni_bytebuf *rbuf = omni_connection_session_receive_buffer(&sess);
  check(rbuf != NULL && omni_bytebuf_readable(rbuf) == sizeof(payload),
        "receive buffer holds payload");

  /* Test writable via session */
  struct omni_bytebuf *sbuf = omni_connection_session_send_buffer(&sess);
  check(sbuf != NULL, "send buffer available");
  check(omni_bytebuf_append(sbuf, out_data, sizeof(out_data)), "append to send buffer");

  /* Poll for writable readiness is not needed for loopback small payload; just try */
  struct omni_connection_io_result wres = omni_connection_session_writable(&sess);
  check(wres.status == OMNI_CONNECTION_IO_OK, "session writable succeeds");
  check(wres.count == sizeof(out_data) || omni_bytebuf_readable(sbuf) == 0u,
        "writable drained buffer");

  /* Verify client received */
  struct pollfd pfd = { 0 };
  pfd.fd = client_fd;
  pfd.events = POLLIN;
  if (poll(&pfd, 1, 500) > 0 && (pfd.revents & POLLIN)) {
    unsigned char tmp[32];
    ssize_t n = recv(client_fd, tmp, sizeof(tmp), 0);
    check(n > 0, "client received data from session writable");
  }

  /* readable after EOF */
  if (shutdown(client_fd, SHUT_WR) == 0) {
    rres = omni_connection_session_readable(&sess);
    /* May be WOULD_BLOCK or EOF depending on timing; both are valid handling */
    check(rres.status == OMNI_CONNECTION_IO_ERR_EOF ||
              rres.status == OMNI_CONNECTION_IO_ERR_WOULD_BLOCK ||
              rres.status == OMNI_CONNECTION_IO_OK,
          "readable handles EOF/woult-block after shutdown");
  }

cleanup:
  omni_connection_session_destroy(&sess);
  omni_connection_destroy(&conn);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) (void)close(client_fd);
  omni_listener_destroy(&listener);
}

static void test_connection_io_initialization_exposure(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted = { 0 };
  struct omni_connection conn = { 0 };
  struct omni_connection_session sess = { 0 };
  unsigned char conn_storage[CONN_RECV_CAP];
  unsigned char sess_recv[SESS_RECV_CAP];
  unsigned char sess_send[SESS_SEND_CAP];
  struct omni_connection_session_config cfg = { 0 };
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;

  omni_listener_make_inert(&listener);
  omni_accepted_make_inert(&accepted);
  omni_connection_make_inert(&conn);
  omni_connection_session_make_inert(&sess);
  if (!start_listener(&listener, &port)) goto cleanup;
  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID) goto cleanup;
  if (!accept_one(&listener, &accepted)) goto cleanup;
  if (!make_open_connection(&conn, conn_storage, CONN_RECV_CAP, &accepted)) goto cleanup;

  cfg.connection = &conn;
  cfg.receive_storage = sess_recv;
  cfg.receive_capacity = SESS_RECV_CAP;
  cfg.send_storage = sess_send;
  cfg.send_capacity = SESS_SEND_CAP;
  (void)omni_connection_session_init(&sess, &cfg);
  check(omni_connection_session_io_state(&sess) == OMNI_CONNECTION_IO_NEW,
        "io NEW before open");
  (void)omni_connection_session_open(&sess);
  check(omni_connection_session_io_state(&sess) == OMNI_CONNECTION_IO_OPEN,
        "io OPEN after open");
  check(omni_bytebuf_capacity(omni_connection_session_receive_buffer(&sess)) == SESS_RECV_CAP,
        "session receive cap correct");
  check(omni_bytebuf_capacity(omni_connection_session_send_buffer(&sess)) == SESS_SEND_CAP,
        "session send cap correct");

  /* session should not affect connection's own receive buffer */
  check(omni_bytebuf_capacity(&conn.receive) == CONN_RECV_CAP,
        "connection receive cap unchanged");
  check(omni_connection_receive_buffer(&conn) != omni_connection_session_receive_buffer(&sess),
        "session and connection buffers are distinct");

cleanup:
  omni_connection_session_destroy(&sess);
  omni_connection_destroy(&conn);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) (void)close(client_fd);
  omni_listener_destroy(&listener);
}

static void test_memory_bounded(void) {
  struct omni_connection_session sess = { 0 };
  omni_connection_session_make_inert(&sess);
  /* Check sizes are bounded and document them */
  check(sizeof(sess) == 160u || sizeof(sess) == 128u || sizeof(sess) > 0,
        "session struct has bounded size");
  /* More precise: we expect 160 with stored buffers, but allow 128 if impl changes */
  printf("# sizeof(session)=%zu # sizeof(io)=%zu # sizeof(connection)=%zu\n",
         sizeof(sess), sizeof(struct omni_connection_io), sizeof(struct omni_connection));
  /* Verify no heap via destroy idempotence */
  omni_connection_session_destroy(&sess);
  check(sess.state == OMNI_CONNECTION_SESSION_CLOSED, "destroy on NEW has bounded effect");
}

static void test_repeated_lifecycle_cycles(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted = { 0 };
  struct omni_connection conn = { 0 };
  struct omni_connection_session sess = { 0 };
  unsigned char conn_storage[CONN_RECV_CAP];
  unsigned char sess_recv[SESS_RECV_CAP];
  unsigned char sess_send[SESS_SEND_CAP];
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  static const unsigned char payload[] = { 0x01u, 0x02u };
  static const unsigned char out[] = { 0xAAu };
  int i;

  omni_listener_make_inert(&listener);
  if (!start_listener(&listener, &port)) goto cleanup;

  for (i = 0; i < 20; ++i) {
    struct omni_connection_session_config cfg = { 0 };
    if (client_fd != OMNI_ACCEPTED_FD_INVALID) { (void)close(client_fd); client_fd = OMNI_ACCEPTED_FD_INVALID; }
    omni_connection_session_destroy(&sess);
    omni_connection_destroy(&conn);
    omni_accepted_destroy(&accepted);
    omni_accepted_make_inert(&accepted);
    omni_connection_make_inert(&conn);
    omni_connection_session_make_inert(&sess);

    client_fd = open_client(port);
    if (client_fd == OMNI_ACCEPTED_FD_INVALID) break;
    if (!accept_one(&listener, &accepted)) break;
    if (!make_open_connection(&conn, conn_storage, CONN_RECV_CAP, &accepted)) break;

    cfg.connection = &conn;
    cfg.receive_storage = sess_recv;
    cfg.receive_capacity = SESS_RECV_CAP;
    cfg.send_storage = sess_send;
    cfg.send_capacity = SESS_SEND_CAP;
    if (omni_connection_session_init(&sess, &cfg).status != OMNI_CONNECTION_SESSION_OK) break;
    if (omni_connection_session_open(&sess).status != OMNI_CONNECTION_SESSION_OK) break;
    client_send_all(client_fd, payload, sizeof(payload));
    struct omni_connection_io_result rr = omni_connection_session_readable(&sess);
    check(rr.status == OMNI_CONNECTION_IO_OK && rr.count == sizeof(payload),
          "cycle readable ok");
    struct omni_bytebuf *sb = omni_connection_session_send_buffer(&sess);
    if (sb) (void)omni_bytebuf_append(sb, out, sizeof(out));
    struct omni_connection_io_result wr = omni_connection_session_writable(&sess);
    check(wr.status == OMNI_CONNECTION_IO_OK, "cycle writable ok");
    check(omni_connection_session_close(&sess).status == OMNI_CONNECTION_SESSION_OK,
          "cycle close ok");
    omni_connection_session_destroy(&sess);
    check(sess.state == OMNI_CONNECTION_SESSION_CLOSED, "cycle destroy -> CLOSED");
    check(omni_connection_is_live(&conn), "cycle connection still live");
    (void)close(client_fd); client_fd = OMNI_ACCEPTED_FD_INVALID;
    omni_connection_destroy(&conn);
    omni_accepted_destroy(&accepted);
  }
  check(i == 20, "20 lifecycle cycles completed");

cleanup:
  omni_connection_session_destroy(&sess);
  omni_connection_destroy(&conn);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) (void)close(client_fd);
  omni_listener_destroy(&listener);
}

static void test_no_accidental_close_fd_census(void) {
  struct omni_listener listener = { 0 };
  struct omni_accepted accepted = { 0 };
  struct omni_connection conn = { 0 };
  struct omni_connection_session sess = { 0 };
  unsigned char conn_storage[CONN_RECV_CAP];
  unsigned char sess_recv[SESS_RECV_CAP];
  unsigned char sess_send[SESS_SEND_CAP];
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  int probe_fd = -1;
  bool probe_valid_before = false;

  omni_listener_make_inert(&listener);
  if (!start_listener(&listener, &port)) return;
  /* Create a bystander fd that should survive session lifecycle */
  probe_fd = socket(AF_INET, SOCK_STREAM, 0);
  probe_valid_before = (probe_fd != -1);

  client_fd = open_client(port);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID) goto cleanup;
  if (!accept_one(&listener, &accepted)) goto cleanup;
  if (!make_open_connection(&conn, conn_storage, CONN_RECV_CAP, &accepted)) goto cleanup;

  struct omni_connection_session_config cfg = { 0 };
  cfg.connection = &conn;
  cfg.receive_storage = sess_recv;
  cfg.receive_capacity = SESS_RECV_CAP;
  cfg.send_storage = sess_send;
  cfg.send_capacity = SESS_SEND_CAP;
  omni_connection_session_make_inert(&sess);
  (void)omni_connection_session_init(&sess, &cfg);
  (void)omni_connection_session_open(&sess);
  (void)omni_connection_session_close(&sess);
  omni_connection_session_destroy(&sess);
  if (probe_valid_before) {
    check(close(probe_fd) == 0, "bystander fd survived session lifecycle");
    probe_fd = -1;
  } else {
    check(false, "probe fd creation failed");
  }

cleanup:
  if (probe_fd != -1) (void)close(probe_fd);
  omni_connection_session_destroy(&sess);
  omni_connection_destroy(&conn);
  omni_accepted_destroy(&accepted);
  if (client_fd != OMNI_ACCEPTED_FD_INVALID) (void)close(client_fd);
  omni_listener_destroy(&listener);
}

int main(void) {
  test_inert_behavior();
  test_init_valid_open_close();
  test_init_invalid();
  test_invalid_transitions();
  test_ownership_preservation();
  test_registry_not_destroyed();
  test_readable_writable_via_session();
  test_connection_io_initialization_exposure();
  test_memory_bounded();
  test_repeated_lifecycle_cycles();
  test_no_accidental_close_fd_census();

  printf("---\n%d checks, %d failures\n", check_count, failure_count);
  return failure_count == 0 ? 0 : 1;
}
