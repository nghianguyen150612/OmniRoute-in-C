/*
 * Task 031 bounded connection readiness dispatch tests. Test clients own
 * their socket calls; the production bridge only delegates through sessions.
 */
#define _POSIX_C_SOURCE 200809L

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
#include <sys/time.h>
#include <unistd.h>

#include "omniroute/accepted.h"
#include "omniroute/bytebuf.h"
#include "omniroute/connection.h"
#include "omniroute/connection_admission.h"
#include "omniroute/connection_dispatch.h"
#include "omniroute/connection_manager.h"
#include "omniroute/connection_reactor.h"
#include "omniroute/connection_runtime.h"
#include "omniroute/listener.h"
#include "omniroute/listener_admission.h"
#include "omniroute/poller.h"
#include "omniroute/reactor.h"
#include "omniroute/registry.h"

#define CAP 4u
#define REG_CAP 8u
#define REACTOR_CAP 16u
#define BUF_CAP 128u
#define LARGE_CAP 65536u
#define STRESS_CYCLES 1000u

static size_t checks;
static size_t failures;

struct fixture {
  struct omni_connection_registry registry;
  struct omni_connection_registry_slot registry_slots[REG_CAP];
  struct omni_poller poller;
  struct pollfd poller_fds[REACTOR_CAP];
  uint64_t poller_tokens[REACTOR_CAP];
  struct omni_reactor reactor;
  struct omni_reactor_registration registrations[REACTOR_CAP];
  struct omni_poller_event events[REACTOR_CAP];
  struct omni_connection_reactor adapter;
  struct omni_connection_runtime runtime;
  struct omni_connection_runtime_entry runtime_entries[CAP];
  struct omni_connection_manager manager;
  struct omni_connection_manager_entry manager_entries[CAP];
  struct omni_connection_dispatch dispatch;
  struct omni_listener listener;
  struct omni_connection_admission admission;
  struct omni_connection_admission_slot admission_slots[CAP];
  unsigned char conn_rx[CAP * BUF_CAP];
  unsigned char session_rx[CAP * BUF_CAP];
  unsigned char session_tx[CAP * BUF_CAP];
  struct omni_listener_admission listener_bridge;
  struct omni_connection_admission_identity identities[CAP];
};

struct managed {
  struct omni_accepted accepted;
  struct omni_connection connection;
  unsigned char conn_rx[BUF_CAP];
  unsigned char session_rx[BUF_CAP];
  unsigned char session_tx[LARGE_CAP];
  uint64_t token;
  int peer;
  bool attached;
};

static void check(bool ok, const char *message) {
  ++checks;
  if (ok) (void)printf("ok - %s\n", message);
  else { ++failures; (void)printf("NOT OK - %s\n", message); }
}

static bool fd_open(int fd) {
  if (fd < 0) return false;
  return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

static size_t fd_count(void) {
  DIR *dir = opendir("/proc/self/fd");
  struct dirent *entry = NULL;
  size_t count = 0u;
  if (dir == NULL) return SIZE_MAX;
  while ((entry = readdir(dir)) != NULL) if (entry->d_name[0] != '.') ++count;
  (void)closedir(dir);
  return count;
}

static void close_fd(int *fd) {
  if (fd != NULL && *fd >= 0) { (void)close(*fd); *fd = -1; }
}

static int open_client(uint16_t port) {
  struct sockaddr_in peer;
  struct timeval timeout = { 2, 0 };
  int receive_buffer = 1024;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                   (socklen_t)sizeof(receive_buffer));
  (void)memset(&peer, 0, sizeof(peer));
  peer.sin_family = AF_INET;
  peer.sin_port = htons(port);
  peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (const struct sockaddr *)&peer, (socklen_t)sizeof(peer)) != 0) {
    (void)close(fd);
    return -1;
  }
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, (socklen_t)sizeof(timeout));
  return fd;
}

static bool client_send(int fd, const unsigned char *data, size_t length) {
  size_t sent = 0u;
  for (size_t attempt = 0u; sent < length && attempt < 32u; ++attempt) {
    ssize_t n = send(fd, data + sent, length - sent, 0);
    if (n <= 0) return false;
    sent += (size_t)n;
  }
  return sent == length;
}

static bool client_receive(int fd, unsigned char *data, size_t length) {
  size_t received = 0u;
  for (size_t attempt = 0u; received < length && attempt < 32u; ++attempt) {
    ssize_t n = recv(fd, data + received, length - received, 0);
    if (n <= 0) return false;
    received += (size_t)n;
  }
  return received == length;
}

static bool buffer_equals(struct omni_bytebuf *buf,
                          const unsigned char *expected, size_t length) {
  size_t actual_length = 0u;
  const unsigned char *actual = omni_bytebuf_read_ptr(buf, &actual_length);
  return actual != NULL && actual_length == length &&
         memcmp(actual, expected, length) == 0;
}

static void fixture_prepare(struct fixture *f) {
  (void)memset(f, 0, sizeof(*f));
  omni_connection_registry_make_inert(&f->registry);
  omni_reactor_make_inert(&f->reactor);
  omni_connection_reactor_make_inert(&f->adapter);
  omni_connection_runtime_make_inert(&f->runtime);
  omni_connection_manager_make_inert(&f->manager);
  omni_connection_dispatch_make_inert(&f->dispatch);
  omni_listener_make_inert(&f->listener);
  omni_connection_admission_make_inert(&f->admission);
  omni_listener_admission_make_inert(&f->listener_bridge);
  for (size_t i = 0u; i < CAP; ++i)
    omni_connection_admission_slot_make_inert(&f->admission_slots[i]);
}

static bool fixture_init(struct fixture *f) {
  struct omni_connection_runtime_config rcfg = {0};
  struct omni_connection_manager_config mcfg = {0};
  struct omni_connection_dispatch_config dcfg = {0};
  struct omni_connection_admission_config acfg = {0};
  struct omni_listener_admission_config lcfg = {0};
  fixture_prepare(f);
  if (omni_connection_registry_init(&f->registry, f->registry_slots, REG_CAP).status !=
      OMNI_CONNECTION_REGISTRY_OK) return false;
  if (omni_poller_init_borrowed(&f->poller, f->poller_fds, f->poller_tokens,
                                REACTOR_CAP).status != OMNI_POLLER_OK) return false;
  if (omni_reactor_init(&f->reactor, &f->poller, f->registrations, f->events,
                        REACTOR_CAP).status != OMNI_REACTOR_OK) return false;
  if (omni_connection_reactor_init(&f->adapter, &f->reactor, &f->registry,
                                   omni_connection_dispatch_callback,
                                   &f->dispatch).status != OMNI_CONNECTION_REACTOR_OK)
    return false;
  rcfg.registry = &f->registry;
  rcfg.adapter = &f->adapter;
  rcfg.entries = f->runtime_entries;
  rcfg.capacity = CAP;
  if (omni_connection_runtime_init(&f->runtime, &rcfg).status !=
      OMNI_CONNECTION_RUNTIME_OK) return false;
  mcfg.runtime = &f->runtime;
  mcfg.entries = f->manager_entries;
  mcfg.capacity = CAP;
  if (omni_connection_manager_init(&f->manager, &mcfg).status !=
      OMNI_CONNECTION_MANAGER_OK) return false;
  dcfg.manager = &f->manager;
  if (omni_connection_dispatch_init(&f->dispatch, &dcfg).status !=
      OMNI_CONNECTION_DISPATCH_OK) return false;
  if (omni_listener_init(&f->listener, "127.0.0.1", 0u).status != OMNI_LISTENER_OK)
    return false;

  acfg.listener = &f->listener;
  acfg.manager = &f->manager;
  acfg.slots = f->admission_slots;
  acfg.slots_bytes = sizeof(f->admission_slots);
  acfg.capacity = CAP;
  acfg.connection_receive.storage = f->conn_rx;
  acfg.connection_receive.storage_bytes = sizeof(f->conn_rx);
  acfg.connection_receive.per_connection_capacity = BUF_CAP;
  acfg.session_receive.storage = f->session_rx;
  acfg.session_receive.storage_bytes = sizeof(f->session_rx);
  acfg.session_receive.per_connection_capacity = BUF_CAP;
  acfg.session_send.storage = f->session_tx;
  acfg.session_send.storage_bytes = sizeof(f->session_tx);
  acfg.session_send.per_connection_capacity = BUF_CAP;
  if (omni_connection_admission_init(&f->admission, &acfg).status !=
      OMNI_CONNECTION_ADMISSION_OK) return false;

  lcfg.listener = &f->listener;
  lcfg.reactor = &f->reactor;
  lcfg.admission = &f->admission;
  lcfg.max_admissions_per_dispatch = 1u;
  lcfg.identities = f->identities;
  lcfg.identity_capacity = CAP;
  return omni_listener_admission_init(&f->listener_bridge, &lcfg).status ==
             OMNI_LISTENER_ADMISSION_OK &&
         omni_listener_admission_start(&f->listener_bridge).status ==
             OMNI_LISTENER_ADMISSION_OK;
}

static void fixture_destroy(struct fixture *f) {
  (void)omni_listener_admission_destroy(&f->listener_bridge);
  (void)omni_connection_admission_destroy(&f->admission);
  omni_connection_manager_destroy(&f->manager);
  omni_connection_dispatch_destroy(&f->dispatch);
  omni_connection_runtime_destroy(&f->runtime);
  omni_connection_reactor_destroy(&f->adapter);
  omni_reactor_destroy(&f->reactor);
  omni_poller_destroy(&f->poller);
  omni_connection_registry_destroy(&f->registry);
  omni_listener_destroy(&f->listener);
}

static bool admission_step(struct fixture *f,
                           struct omni_connection_admission_identity *identity) {
  struct omni_reactor_result step = omni_reactor_step(&f->reactor, 0);
  struct omni_listener_admission_result result =
      omni_listener_admission_last_result(&f->listener_bridge);
  if (step.status != OMNI_REACTOR_OK ||
      result.status != OMNI_LISTENER_ADMISSION_ADMISSION_LIMIT_REACHED ||
      result.admission.admitted != 1u) return false;
  *identity = f->identities[0];
  return identity->token != 0u;
}

static bool release_admission(struct fixture *f,
                              struct omni_connection_admission_identity identity) {
  return omni_connection_admission_release(&f->admission, identity).status ==
         OMNI_CONNECTION_ADMISSION_OK;
}

static bool managed_open(struct fixture *f, struct managed *m, uint32_t interests,
                        size_t rx_capacity, size_t tx_capacity) {
  struct omni_connection_config ccfg = {0};
  struct omni_connection_manager_attach_config mcfg = {0};
  struct omni_accept_result accept_result;
  struct omni_connection_manager_result add_result;
  if (rx_capacity == 0u || rx_capacity > sizeof(m->session_rx) ||
      tx_capacity == 0u || tx_capacity > sizeof(m->session_tx)) return false;
  (void)memset(m, 0, sizeof(*m));
  m->peer = -1;
  omni_accepted_make_inert(&m->accepted);
  omni_connection_make_inert(&m->connection);
  m->peer = open_client(omni_listener_port(&f->listener));
  if (m->peer < 0) return false;
  accept_result = omni_accept_once(&f->listener, &m->accepted);
  if (accept_result.status != OMNI_ACCEPT_OK || accept_result.accepted != 1u)
    return false;
  ccfg.receive_storage = m->conn_rx;
  ccfg.receive_capacity = sizeof(m->conn_rx);
  ccfg.poller_interests = interests;
  if (omni_connection_init(&m->connection, &ccfg).status != OMNI_CONNECTION_OK ||
      omni_connection_from_accepted(&m->connection, &m->accepted).status !=
          OMNI_CONNECTION_OK) return false;
  mcfg.connection = &m->connection;
  mcfg.receive_storage = m->session_rx;
  mcfg.receive_capacity = rx_capacity;
  mcfg.send_storage = m->session_tx;
  mcfg.send_capacity = tx_capacity;
  add_result = omni_connection_manager_add(&f->manager, &mcfg);
  if (add_result.status != OMNI_CONNECTION_MANAGER_OK) return false;
  m->token = add_result.token;
  m->attached = true;
  return true;
}

static void managed_destroy(struct fixture *f, struct managed *m) {
  if (m->attached) {
    (void)omni_connection_manager_remove(&f->manager, &m->connection);
    m->attached = false;
  }
  omni_connection_destroy(&m->connection);
  omni_accepted_destroy(&m->accepted);
  close_fd(&m->peer);
}

static struct omni_connection_session *managed_session(struct fixture *f,
                                                        struct managed *m) {
  return omni_connection_runtime_find_session(&f->runtime, &m->connection);
}

static struct omni_connection_dispatch_result last(struct fixture *f) {
  return omni_connection_dispatch_last_result(&f->dispatch);
}

static void test_lifecycle(void) {
  struct omni_connection_dispatch bridge = {0};
  struct omni_connection_dispatch_config config = {0};
  struct omni_connection_manager unavailable = {0};
  struct fixture f = {0};
  uint64_t callbacks;
  omni_connection_dispatch_make_inert(NULL);
  omni_connection_dispatch_make_inert(&bridge);
  check(omni_connection_dispatch_state(&bridge) == OMNI_CONNECTION_DISPATCH_INERT,
        "fresh bridge is inert");
  check(omni_connection_dispatch_init(&bridge, &config).status ==
            OMNI_CONNECTION_DISPATCH_ERR_INVALID,
        "NULL manager is rejected");
  omni_connection_manager_make_inert(&unavailable);
  config.manager = &unavailable;
  check(omni_connection_dispatch_init(&bridge, &config).status ==
            OMNI_CONNECTION_DISPATCH_ERR_INVALID &&
            omni_connection_dispatch_state(&bridge) == OMNI_CONNECTION_DISPATCH_INERT,
        "unavailable manager is rejected");
  check(fixture_init(&f), "valid dependencies initialize dispatch fixture");
  config.manager = &f.manager;
  check(omni_connection_dispatch_init(&f.dispatch, &config).status ==
            OMNI_CONNECTION_DISPATCH_ERR_STATE,
        "duplicate init is rejected");
  omni_connection_dispatch_destroy(&f.dispatch);
  omni_connection_dispatch_destroy(&f.dispatch);
  callbacks = omni_connection_dispatch_dispatches(&f.dispatch);
  omni_connection_dispatch_callback(NULL, 1u, OMNI_POLLER_READY_READ, &f.dispatch);
  check(omni_connection_dispatch_state(&f.dispatch) == OMNI_CONNECTION_DISPATCH_CLOSED &&
            omni_connection_dispatch_dispatches(&f.dispatch) == callbacks,
        "repeated destroy is safe and closed bridge does not dispatch");
  fixture_destroy(&f);
}

static void test_admission_read_integration(void) {
  struct fixture f = {0};
  struct omni_connection_admission_identity id = {0};
  struct omni_connection *conn = NULL;
  struct omni_connection_session *session = NULL;
  struct omni_connection_dispatch_result result;
  const unsigned char payload[] = { 0x00u, 0x80u, 0xffu, 0x41u, 0x00u, 0x7fu };
  int client = -1;
  check(fixture_init(&f), "listener-to-connection dispatch fixture initializes");
  client = open_client(omni_listener_port(&f.listener));
  check(client >= 0 && client_send(client, payload, sizeof(payload)),
        "client sends binary payload before listener callback");
  check(admission_step(&f, &id),
        "Task 030 readiness performs Task 029 admission into manager");
  check(omni_connection_manager_count(&f.manager) == 1u &&
            omni_connection_admission_count(&f.admission) == 1u,
        "listener callback creates live bounded manager membership");
  conn = &f.admission_slots[id.slot_index].connection;
  session = omni_connection_runtime_find_session(&f.runtime, conn);
  check(session != NULL && omni_connection_session_is_open(session),
        "existing runtime entry resolves the admitted open session");
  (void)omni_reactor_step(&f.reactor, 0);
  result = last(&f);
  check(result.status == OMNI_CONNECTION_DISPATCH_OK && result.token == id.token &&
            result.read_attempted && result.bytes_read == sizeof(payload) &&
            result.read_result.status == OMNI_CONNECTION_IO_OK,
        "connection reactor READ callback performs one bounded session read");
  check(buffer_equals(omni_connection_session_receive_buffer(session), payload,
                      sizeof(payload)),
        "receive buffer contains exact NUL/high-byte payload");
  check(omni_connection_dispatch_bytes_read(&f.dispatch) == sizeof(payload) &&
            omni_connection_manager_count(&f.manager) == 1u,
        "integration read accounting advances without removing connection");
  check(release_admission(&f, id), "integration admission releases by generation identity");
  close_fd(&client);
  fixture_destroy(&f);
}

static void test_read_status_and_no_compaction(void) {
  struct fixture f = {0};
  struct managed m = {0};
  struct omni_connection_dispatch_result result;
  struct omni_connection_session *session;
  struct omni_bytebuf *buf;
  const unsigned char payload[] = { 0x00u, 0x80u, 0xfeu, 0x11u, 0x22u,
                                    0x33u, 0x44u, 0x55u, 0x66u, 0x77u };
  check(fixture_init(&f), "read status fixture initializes");
  check(managed_open(&f, &m, OMNI_POLLER_INTEREST_READ, 8u, BUF_CAP),
        "managed read connection uses eight-byte receive capacity");
  session = managed_session(&f, &m);
  buf = omni_connection_session_receive_buffer(session);
  omni_connection_dispatch_callback(&m.connection, m.token, OMNI_POLLER_READY_READ, &f.dispatch);
  result = last(&f);
  check(result.read_result.status == OMNI_CONNECTION_IO_ERR_WOULD_BLOCK &&
            (result.read_result.sys_errno == EAGAIN ||
             result.read_result.sys_errno == EWOULDBLOCK),
        "readiness race surfaces WOULD_BLOCK and errno");
  check(client_send(m.peer, payload, sizeof(payload)), "peer queues ten binary bytes");
  omni_connection_dispatch_callback(&m.connection, m.token, OMNI_POLLER_READY_READ, &f.dispatch);
  result = last(&f);
  check(result.bytes_read == 8u && result.read_result.count == 8u &&
            omni_bytebuf_readable(buf) == 8u,
        "one callback reads only its single eight-byte bounded tail");
  check(buffer_equals(buf, payload, 8u), "bounded read preserves raw binary bytes");
  check(omni_bytebuf_consume(buf, 4u) && omni_bytebuf_reclaimable(buf) == 4u &&
            omni_bytebuf_writable(buf) == 0u,
        "test leaves a consumed prefix and no contiguous writable tail");
  omni_connection_dispatch_callback(&m.connection, m.token, OMNI_POLLER_READY_READ, &f.dispatch);
  result = last(&f);
  check(result.read_result.status == OMNI_CONNECTION_IO_ERR_BUFFER_FULL &&
            result.bytes_read == 0u && omni_bytebuf_readable(buf) == 4u &&
            omni_bytebuf_reclaimable(buf) == 4u && omni_bytebuf_writable(buf) == 0u,
        "buffer-full callback does not compact or consume prior bytes");
  check(memcmp(omni_bytebuf_read_ptr(buf, NULL), payload + 4u, 4u) == 0 &&
            omni_connection_dispatch_read_dispatches(&f.dispatch) == 3u,
        "unread suffix is intact and each readiness made one read dispatch");
  managed_destroy(&f, &m);
  fixture_destroy(&f);
}

static void test_error_eof_and_invalid(void) {
  struct fixture f = {0};
  struct managed m = {0};
  struct managed invalid_fd = {0};
  struct omni_connection_dispatch_result result;
  uint64_t reads;
  uint64_t writes;
  size_t registrations;
  int fd;
  check(fixture_init(&f), "error-event fixture initializes");
  check(managed_open(&f, &m, OMNI_POLLER_INTEREST_READ, BUF_CAP, BUF_CAP),
        "managed connection opens for EOF and error callbacks");
  fd = omni_connection_fd(&m.connection);
  check(shutdown(m.peer, SHUT_WR) == 0, "peer half-closes to produce EOF");
  (void)omni_reactor_step(&f.reactor, 0);
  result = last(&f);
  check(result.read_attempted && result.read_result.status == OMNI_CONNECTION_IO_ERR_EOF &&
            result.close_worthy,
        "reactor callback surfaces EOF without automatic removal");
  check(omni_connection_manager_count(&f.manager) == 1u &&
            omni_connection_state(&m.connection) == OMNI_CONNECTION_OPEN && fd_open(fd),
        "EOF leaves membership, connection state and descriptor ownership intact");
  omni_connection_dispatch_callback(&m.connection, m.token,
      OMNI_POLLER_READY_ERROR | OMNI_POLLER_READY_READ, &f.dispatch);
  result = last(&f);
  check(result.error_observed && result.close_worthy && result.read_attempted &&
            result.read_result.status == OMNI_CONNECTION_IO_ERR_EOF,
        "ERROR is classified before one READ and both outcomes are recorded");
  omni_connection_dispatch_callback(&m.connection, m.token,
                                    OMNI_POLLER_READY_HANGUP, &f.dispatch);
  result = last(&f);
  check(result.hangup_observed && result.close_worthy && !result.read_attempted,
        "HANGUP is surfaced without synthesizing payload I/O");
  reads = omni_connection_dispatch_read_dispatches(&f.dispatch);
  writes = omni_connection_dispatch_write_dispatches(&f.dispatch);
  registrations = omni_reactor_count(&f.reactor);
  omni_connection_dispatch_callback(&m.connection, m.token,
      OMNI_POLLER_READY_INVALID | OMNI_POLLER_READY_READ | OMNI_POLLER_READY_WRITE,
      &f.dispatch);
  result = last(&f);
  check(result.status == OMNI_CONNECTION_DISPATCH_INVALID_EVENT &&
            result.invalid_observed && result.close_worthy &&
            !result.read_attempted && !result.write_attempted,
        "INVALID suppresses normal I/O even when READ and WRITE are also set");
  check(omni_connection_dispatch_read_dispatches(&f.dispatch) == reads &&
            omni_connection_dispatch_write_dispatches(&f.dispatch) == writes &&
            omni_reactor_count(&f.reactor) == registrations &&
            omni_connection_manager_count(&f.manager) == 1u && fd_open(fd),
        "error handling never mutates registration, membership or FD ownership");

  check(managed_open(&f, &invalid_fd, OMNI_POLLER_INTEREST_READ, BUF_CAP, BUF_CAP),
        "second connection is registered for real invalid-FD poll proof");
  {
    int invalid = omni_connection_fd(&invalid_fd.connection);
    (void)close(invalid); /* Deliberate test fault: force poll() to report POLLNVAL. */
    (void)omni_reactor_step(&f.reactor, 0);
    result = last(&f);
    check(result.token == invalid_fd.token && result.invalid_observed &&
              result.status == OMNI_CONNECTION_DISPATCH_INVALID_EVENT &&
              !result.read_attempted && !result.write_attempted,
          "real invalid-FD readiness reaches callback and performs no payload I/O");
    check(omni_connection_manager_count(&f.manager) == 2u,
          "real INVALID observation does not automatically remove membership");
  }
  managed_destroy(&f, &invalid_fd);
  managed_destroy(&f, &m);
  fixture_destroy(&f);
}

static void test_fatal_read_and_closed_session(void) {
  struct fixture f = {0};
  struct managed reset_peer = {0};
  struct managed closing = {0};
  struct managed closing_connection = {0};
  struct omni_connection_dispatch_result result;
  struct omni_connection_session *session;
  struct linger reset = { 1, 0 };
  struct pollfd probe;
  int fd;
  check(fixture_init(&f), "fatal-read fixture initializes");
  check(managed_open(&f, &reset_peer, OMNI_POLLER_INTEREST_READ, BUF_CAP, BUF_CAP),
        "managed connection opens for reset read");
  fd = omni_connection_fd(&reset_peer.connection);
  check(setsockopt(reset_peer.peer, SOL_SOCKET, SO_LINGER, &reset,
                   (socklen_t)sizeof(reset)) == 0, "peer reset is enabled");
  close_fd(&reset_peer.peer);
  probe.fd = fd;
  probe.events = POLLIN | POLLOUT;
  probe.revents = 0;
  (void)poll(&probe, (nfds_t)1, 2000);
  omni_connection_dispatch_callback(&reset_peer.connection, reset_peer.token,
                                    OMNI_POLLER_READY_READ, &f.dispatch);
  result = last(&f);
  check(result.read_result.status == OMNI_CONNECTION_IO_ERR_IO &&
            result.read_result.sys_errno != 0 && result.close_worthy,
        "fatal receive errno is preserved and marked close-worthy");
  check(omni_connection_manager_count(&f.manager) == 1u && fd_open(fd),
        "fatal read leaves cleanup and descriptor closure to owner");
  managed_destroy(&f, &reset_peer);

  check(managed_open(&f, &closing, OMNI_POLLER_INTEREST_READ, BUF_CAP, BUF_CAP),
        "second managed connection opens for closed-session guard");
  session = managed_session(&f, &closing);
  check(omni_connection_session_close(session).status == OMNI_CONNECTION_SESSION_OK,
        "session can enter CLOSING while membership remains live");
  omni_connection_dispatch_callback(&closing.connection, closing.token,
                                    OMNI_POLLER_READY_READ, &f.dispatch);
  result = last(&f);
  check(result.status == OMNI_CONNECTION_DISPATCH_STALE && result.stale &&
            !result.read_attempted && omni_connection_manager_count(&f.manager) == 1u,
        "non-OPEN session is rejected before I/O");
  managed_destroy(&f, &closing);

  check(managed_open(&f, &closing_connection, OMNI_POLLER_INTEREST_READ,
                     BUF_CAP, BUF_CAP),
        "second managed connection opens for connection-closing guard");
  fd = omni_connection_fd(&closing_connection.connection);
  check(omni_connection_begin_close(&closing_connection.connection).status ==
            OMNI_CONNECTION_OK &&
            omni_connection_session_is_open(managed_session(&f, &closing_connection)),
        "connection enters CLOSING while its managed session remains OPEN");
  omni_connection_dispatch_callback(&closing_connection.connection,
                                    closing_connection.token,
                                    OMNI_POLLER_READY_READ, &f.dispatch);
  result = last(&f);
  check(result.status == OMNI_CONNECTION_DISPATCH_STALE && result.stale &&
            !result.read_attempted && omni_connection_manager_count(&f.manager) == 1u &&
            fd_open(fd),
        "CLOSING connection is rejected without I/O or ownership cleanup");
  managed_destroy(&f, &closing_connection);
  fixture_destroy(&f);
}

static void test_write_and_combined_readiness(void) {
  struct fixture f = {0};
  struct managed write_only = {0};
  struct managed combined = {0};
  struct omni_connection_session *session;
  struct omni_bytebuf *tx;
  struct omni_bytebuf *rx;
  struct omni_connection_dispatch_result result;
  unsigned char peer_bytes[32];
  const unsigned char outbound[] = { 0x00u, 0x81u, 0x22u, 0xffu, 0x7eu };
  const unsigned char inbound[] = { 0x91u, 0x00u, 0xabu, 0xcdu };
  uint64_t reads;
  uint64_t writes;
  bool found = false;
  uint32_t interests = 0u;
  check(fixture_init(&f), "write fixture initializes");
  check(managed_open(&f, &write_only, OMNI_POLLER_INTEREST_WRITE, BUF_CAP, BUF_CAP),
        "test connection explicitly subscribes to WRITE readiness");
  session = managed_session(&f, &write_only);
  tx = omni_connection_session_send_buffer(session);
  check(omni_bytebuf_append(tx, outbound, sizeof(outbound)),
        "binary bytes are appended to the existing session send buffer");
  for (size_t i = 0u; i < f.reactor.count; ++i) {
    if (f.registrations[i].token == write_only.token) {
      found = true;
      interests = f.registrations[i].interests;
    }
  }
  (void)omni_reactor_step(&f.reactor, 0);
  result = last(&f);
  check(result.write_attempted && result.write_result.status == OMNI_CONNECTION_IO_OK &&
            result.bytes_written == sizeof(outbound),
        "WRITE callback moves one buffered span toward peer");
  check(client_receive(write_only.peer, peer_bytes, sizeof(outbound)) &&
            memcmp(peer_bytes, outbound, sizeof(outbound)) == 0,
        "peer receives exact binary buffered output");
  check(found && interests == OMNI_POLLER_INTEREST_WRITE,
        "bridge preserves configured WRITE interest instead of updating reactor");
  (void)omni_reactor_step(&f.reactor, 0);
  result = last(&f);
  check(result.write_attempted && result.bytes_written == 0u &&
            result.write_result.status == OMNI_CONNECTION_IO_OK,
        "empty send buffer is safe under explicitly configured WRITE readiness");
  managed_destroy(&f, &write_only);

  check(managed_open(&f, &combined,
      OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE, BUF_CAP, BUF_CAP),
      "combined test connection explicitly subscribes to READ and WRITE");
  session = managed_session(&f, &combined);
  tx = omni_connection_session_send_buffer(session);
  rx = omni_connection_session_receive_buffer(session);
  check(omni_bytebuf_append(tx, outbound, sizeof(outbound)) &&
            client_send(combined.peer, inbound, sizeof(inbound)),
        "combined event has pending session output and peer input");
  reads = omni_connection_dispatch_read_dispatches(&f.dispatch);
  writes = omni_connection_dispatch_write_dispatches(&f.dispatch);
  (void)omni_reactor_step(&f.reactor, 0);
  result = last(&f);
  check((result.readiness_events & 3u) == 3u && result.read_attempted &&
            result.write_attempted && result.read_result.status == OMNI_CONNECTION_IO_OK &&
            result.write_result.status == OMNI_CONNECTION_IO_OK,
        "combined readiness deterministically processes READ before WRITE");
  check(omni_connection_dispatch_read_dispatches(&f.dispatch) == reads + 1u &&
            omni_connection_dispatch_write_dispatches(&f.dispatch) == writes + 1u,
        "combined callback performs at most one operation in each direction");
  check(buffer_equals(rx, inbound, sizeof(inbound)) &&
            client_receive(combined.peer, peer_bytes, sizeof(outbound)) &&
            memcmp(peer_bytes, outbound, sizeof(outbound)) == 0,
        "combined callback preserves exact independent input and output payloads");
  managed_destroy(&f, &combined);
  fixture_destroy(&f);
}

static void test_write_backpressure_and_peer_close(void) {
  struct fixture f = {0};
  struct managed pressured = {0};
  struct managed reset_peer = {0};
  struct omni_connection_session *session;
  struct omni_bytebuf *tx;
  struct omni_connection_dispatch_result result;
  struct linger reset = { 1, 0 };
  struct pollfd probe;
  unsigned char payload[LARGE_CAP];
  bool partial = false;
  bool would_block = false;
  bool fatal = false;
  int fd;
  for (size_t i = 0u; i < sizeof(payload); ++i)
    payload[i] = (unsigned char)((i * 37u) & 0xffu);
  check(fixture_init(&f), "backpressure fixture initializes");
  check(managed_open(&f, &pressured, OMNI_POLLER_INTEREST_WRITE, BUF_CAP,
                     sizeof(payload)), "large fixed send buffer is initialized");
  fd = omni_connection_fd(&pressured.connection);
  {
    int sndbuf = 1024;
    check(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, (socklen_t)sizeof(sndbuf)) == 0,
          "small test socket send buffer creates partial progress");
  }
  session = managed_session(&f, &pressured);
  tx = omni_connection_session_send_buffer(session);
  check(omni_bytebuf_append(tx, payload, sizeof(payload)), "bounded send storage accepts test bytes");
  for (size_t i = 0u; i < 256u && !would_block; ++i) {
    if (omni_bytebuf_readable(tx) == 0u &&
        !omni_bytebuf_append(tx, payload, sizeof(payload))) break;
    size_t before = omni_bytebuf_readable(tx);
    omni_connection_dispatch_callback(&pressured.connection, pressured.token,
                                     OMNI_POLLER_READY_WRITE, &f.dispatch);
    result = last(&f);
    if (result.bytes_written > 0u && result.bytes_written < before) partial = true;
    if (result.bytes_written <= before &&
        omni_bytebuf_readable(tx) == before - (size_t)result.bytes_written) {
      if (result.write_result.status == OMNI_CONNECTION_IO_ERR_WOULD_BLOCK &&
          (result.write_result.sys_errno == EAGAIN ||
           result.write_result.sys_errno == EWOULDBLOCK) && result.bytes_written == 0u)
        would_block = true;
    } else {
      break;
    }
  }
  check(partial && omni_bytebuf_readable(tx) < sizeof(payload),
        "single partial write consumes exactly its reported prefix");
  check(would_block, "write readiness race surfaces WOULD_BLOCK with errno");
  check(omni_connection_manager_count(&f.manager) == 1u && fd_open(fd),
        "backpressure preserves live manager membership and FD");
  managed_destroy(&f, &pressured);

  check(managed_open(&f, &reset_peer, OMNI_POLLER_INTEREST_WRITE, BUF_CAP, BUF_CAP),
        "peer-close write fixture is managed");
  fd = omni_connection_fd(&reset_peer.connection);
  check(setsockopt(reset_peer.peer, SOL_SOCKET, SO_LINGER, &reset,
                   (socklen_t)sizeof(reset)) == 0, "peer reset is enabled for write");
  close_fd(&reset_peer.peer);
  probe.fd = fd;
  probe.events = POLLIN | POLLOUT;
  probe.revents = 0;
  (void)poll(&probe, (nfds_t)1, 2000);
  session = managed_session(&f, &reset_peer);
  tx = omni_connection_session_send_buffer(session);
  for (size_t i = 0u; i < 8u && !fatal; ++i) {
    unsigned char one = (unsigned char)(0xa0u + (unsigned char)i);
    if (!omni_bytebuf_append(tx, &one, 1u)) break;
    omni_connection_dispatch_callback(&reset_peer.connection, reset_peer.token,
                                     OMNI_POLLER_READY_WRITE, &f.dispatch);
    result = last(&f);
    fatal = result.write_result.status == OMNI_CONNECTION_IO_ERR_IO &&
            result.write_result.sys_errno != 0 && result.close_worthy;
  }
  check(fatal, "peer-closed/fatal write errno is surfaced");
  check(omni_connection_manager_count(&f.manager) == 1u && fd_open(fd),
        "fatal write never removes manager membership or closes descriptor");
  managed_destroy(&f, &reset_peer);
  fixture_destroy(&f);
}

static void test_stale_and_reused_generation(void) {
  struct fixture f = {0};
  struct omni_connection_admission_identity first = {0};
  struct omni_connection_admission_identity second = {0};
  struct omni_connection_dispatch_result result;
  struct omni_connection *connection;
  struct omni_connection_session *session;
  struct managed removed = {0};
  const unsigned char p1[] = { 0x00u, 0x11u };
  const unsigned char p2[] = { 0x80u, 0xffu, 0x22u };
  int client1 = -1;
  int client2 = -1;
  check(fixture_init(&f), "stale-token fixture initializes");
  client1 = open_client(omni_listener_port(&f.listener));
  check(client1 >= 0 && client_send(client1, p1, sizeof(p1)) && admission_step(&f, &first),
        "first connection admitted by listener callback");
  connection = &f.admission_slots[first.slot_index].connection;
  check(release_admission(&f, first), "first slot occupant is released");
  close_fd(&client1);
  client2 = open_client(omni_listener_port(&f.listener));
  check(client2 >= 0 && client_send(client2, p2, sizeof(p2)) && admission_step(&f, &second),
        "second connection admitted after slot reuse");
  check(first.slot_index == second.slot_index && first.token != second.token &&
            connection == &f.admission_slots[second.slot_index].connection,
        "reused slot has a fresh generation token");
  session = omni_connection_runtime_find_session(&f.runtime, connection);
  omni_connection_dispatch_callback(connection, first.token, OMNI_POLLER_READY_READ, &f.dispatch);
  result = last(&f);
  check(result.status == OMNI_CONNECTION_DISPATCH_STALE && result.stale &&
            !result.read_attempted && session != NULL &&
            omni_bytebuf_readable(omni_connection_session_receive_buffer(session)) == 0u,
        "old token cannot read from the reused slot occupant");
  (void)omni_reactor_step(&f.reactor, 0);
  result = last(&f);
  check(result.token == second.token && buffer_equals(
            omni_connection_session_receive_buffer(session), p2, sizeof(p2)),
        "current callback reads exact bytes from the current occupant");
  check(release_admission(&f, second), "second reused occupant releases");
  close_fd(&client2);

  check(managed_open(&f, &removed, OMNI_POLLER_INTEREST_READ, BUF_CAP, BUF_CAP),
        "separate managed entry is added for remove-stale check");
  {
    uint64_t stale_token = removed.token;
    int owned_fd = omni_connection_fd(&removed.connection);
    struct omni_connection *ptr = &removed.connection;
    struct omni_connection_manager_result removed_result =
        omni_connection_manager_remove(&f.manager, ptr);
    removed.attached = false;
    omni_connection_dispatch_callback(ptr, stale_token, OMNI_POLLER_READY_READ, &f.dispatch);
    result = last(&f);
    check(removed_result.status == OMNI_CONNECTION_MANAGER_OK &&
              result.status == OMNI_CONNECTION_DISPATCH_STALE && result.stale &&
              !result.read_attempted && fd_open(owned_fd),
          "removed entry is stale and callback leaves connection FD open");
  }
  managed_destroy(&f, &removed);
  fixture_destroy(&f);
}

static void test_counter_saturation(void) {
  struct fixture f = {0};
  struct managed m = {0};
  struct omni_connection_session *session;
  struct omni_bytebuf *tx;
  struct omni_connection_dispatch_result result;
  const unsigned char one = 0x80u;
  check(fixture_init(&f), "counter fixture initializes");
  check(managed_open(&f, &m, OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE,
                     BUF_CAP, BUF_CAP), "combined connection is managed");
  session = managed_session(&f, &m);
  tx = omni_connection_session_send_buffer(session);
  check(omni_bytebuf_append(tx, &one, 1u) && client_send(m.peer, &one, 1u),
        "one byte is queued in each direction");
  f.dispatch.dispatches = UINT64_MAX;
  f.dispatch.read_dispatches = UINT64_MAX;
  f.dispatch.write_dispatches = UINT64_MAX;
  f.dispatch.bytes_read = UINT64_MAX;
  f.dispatch.bytes_written = UINT64_MAX;
  f.dispatch.error_observations = UINT64_MAX;
  omni_connection_dispatch_callback(&m.connection, m.token,
      OMNI_POLLER_READY_ERROR | OMNI_POLLER_READY_READ | OMNI_POLLER_READY_WRITE,
      &f.dispatch);
  result = last(&f);
  check(result.bytes_read == 1u && result.bytes_written == 1u,
        "per-dispatch byte results remain exact at cumulative counter limit");
  check(f.dispatch.dispatches == UINT64_MAX && f.dispatch.read_dispatches == UINT64_MAX &&
            f.dispatch.write_dispatches == UINT64_MAX &&
            f.dispatch.bytes_read == UINT64_MAX && f.dispatch.bytes_written == UINT64_MAX &&
            f.dispatch.error_observations == UINT64_MAX,
        "dispatch, byte and event totals saturate instead of wrapping");
  f.dispatch.ignored_dispatches = UINT64_MAX;
  omni_connection_dispatch_callback(&m.connection, m.token, 0u, &f.dispatch);
  check(f.dispatch.ignored_dispatches == UINT64_MAX, "ignored count also saturates");
  managed_destroy(&f, &m);
  fixture_destroy(&f);
}

static void test_stress(void) {
  struct fixture f = {0};
  size_t baseline = SIZE_MAX;
  size_t completed = 0u;
  bool ok = true;
  check(fixture_init(&f), "1,000-cycle stress fixture initializes");
  baseline = fd_count();
  for (size_t cycle = 0u; cycle < STRESS_CYCLES && ok; ++cycle) {
    struct omni_connection_admission_identity id = {0};
    struct omni_connection_dispatch_result result;
    unsigned char payload[3] = {
      (unsigned char)(cycle & 0xffu),
      (unsigned char)((cycle >> 2u) & 0xffu),
      (unsigned char)(0x80u | (cycle & 0x7fu))
    };
    int client = open_client(omni_listener_port(&f.listener));
    if (client < 0 || !client_send(client, payload, sizeof(payload)) ||
        !admission_step(&f, &id) ||
        omni_connection_manager_count(&f.manager) >
            omni_connection_manager_capacity(&f.manager) ||
        omni_connection_admission_count(&f.admission) >
            omni_connection_admission_capacity(&f.admission)) {
      ok = false;
    } else {
      struct omni_reactor_result step = omni_reactor_step(&f.reactor, 0);
      struct omni_connection *conn = &f.admission_slots[id.slot_index].connection;
      struct omni_connection_session *session =
          omni_connection_runtime_find_session(&f.runtime, conn);
      result = last(&f);
      if (step.status != OMNI_REACTOR_OK || result.token != id.token ||
          !result.read_attempted || result.bytes_read != sizeof(payload) ||
          session == NULL ||
          !buffer_equals(omni_connection_session_receive_buffer(session),
                         payload, sizeof(payload)) ||
          !release_admission(&f, id)) ok = false;
      else ++completed;
    }
    close_fd(&client);
    if (omni_connection_manager_count(&f.manager) >
            omni_connection_manager_capacity(&f.manager) ||
        omni_connection_admission_count(&f.admission) >
            omni_connection_admission_capacity(&f.admission) ||
        fd_count() != baseline) ok = false;
  }
  check(ok && completed == STRESS_CYCLES,
        "1,000 admit/read/release callback cycles preserve payload and capacity bounds");
  check(ok && omni_connection_dispatch_read_dispatches(&f.dispatch) >= STRESS_CYCLES &&
            omni_connection_manager_count(&f.manager) == 0u &&
            omni_connection_admission_count(&f.admission) == 0u,
        "stress leaves manager and admission empty after at least 1,000 reads");
  check(ok && fd_count() == baseline, "stress descriptor census returns to baseline");
  fixture_destroy(&f);
}

int main(void) {
  test_lifecycle();
  test_admission_read_integration();
  test_read_status_and_no_compaction();
  test_error_eof_and_invalid();
  test_fatal_read_and_closed_session();
  test_write_and_combined_readiness();
  test_write_backpressure_and_peer_close();
  test_stale_and_reused_generation();
  test_counter_saturation();
  test_stress();
  (void)printf("sizeof(struct omni_connection_dispatch)=%zu\n",
               sizeof(struct omni_connection_dispatch));
  (void)printf("sizeof(struct omni_connection_dispatch_config)=%zu\n",
               sizeof(struct omni_connection_dispatch_config));
  (void)printf("sizeof(struct omni_connection_dispatch_result)=%zu\n",
               sizeof(struct omni_connection_dispatch_result));
  (void)printf("checks=%zu failures=%zu\n", checks, failures);
  return failures == 0u ? 0 : 1;
}
