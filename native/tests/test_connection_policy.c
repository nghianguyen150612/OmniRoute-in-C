/*
 * Task 032 bounded connection lifecycle and reactor-interest policy tests.
 * Test clients own socket syscalls; production policy only coordinates the
 * existing admission, dispatch, session, and reactor APIs.
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

#include "omniroute/bytebuf.h"
#include "omniroute/connection_admission.h"
#include "omniroute/connection_dispatch.h"
#include "omniroute/connection_manager.h"
#include "omniroute/connection_policy.h"
#include "omniroute/connection_reactor.h"
#include "omniroute/connection_runtime.h"
#include "omniroute/listener.h"
#include "omniroute/listener_admission.h"
#include "omniroute/poller.h"
#include "omniroute/reactor.h"
#include "omniroute/registry.h"

#define TEST_CAPACITY 3u
#define REGISTRY_CAPACITY 8u
#define REACTOR_CAPACITY 8u
#define BUFFER_CAPACITY 256u
#define STRESS_CYCLES 1000u

static size_t checks;
static size_t failures;

struct fixture {
  struct omni_connection_registry registry;
  struct omni_connection_registry_slot registry_slots[REGISTRY_CAPACITY];
  struct omni_poller poller;
  struct pollfd poller_fds[REACTOR_CAPACITY];
  uint64_t poller_tokens[REACTOR_CAPACITY];
  struct omni_reactor reactor;
  struct omni_reactor_registration registrations[REACTOR_CAPACITY];
  struct omni_poller_event events[REACTOR_CAPACITY];
  struct omni_connection_reactor adapter;
  struct omni_connection_runtime runtime;
  struct omni_connection_runtime_entry runtime_entries[TEST_CAPACITY];
  struct omni_connection_manager manager;
  struct omni_connection_manager_entry manager_entries[TEST_CAPACITY];
  struct omni_connection_dispatch dispatch;
  struct omni_connection_policy policy;
  struct omni_listener listener;
  struct omni_connection_admission admission;
  struct omni_connection_admission_slot admission_slots[TEST_CAPACITY];
  unsigned char connection_receive[TEST_CAPACITY * BUFFER_CAPACITY];
  unsigned char session_receive[TEST_CAPACITY * BUFFER_CAPACITY];
  unsigned char session_send[TEST_CAPACITY * BUFFER_CAPACITY];
  struct omni_listener_admission listener_admission;
  struct omni_connection_admission_identity identities[TEST_CAPACITY];
};

static void check(bool condition, const char *description) {
  ++checks;
  if (condition) {
    (void)printf("ok - %s\n", description);
  } else {
    ++failures;
    (void)printf("NOT OK - %s\n", description);
  }
}

static bool fd_open(int fd) {
  if (fd < 0) return false;
  return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

static size_t fd_count(void) {
  DIR *directory = opendir("/proc/self/fd");
  struct dirent *entry = NULL;
  size_t count = 0u;

  if (directory == NULL) return SIZE_MAX;
  while ((entry = readdir(directory)) != NULL) {
    if (entry->d_name[0] != '.') ++count;
  }
  (void)closedir(directory);
  return count;
}

static void close_fd(int *fd) {
  if (fd != NULL && *fd >= 0) {
    (void)close(*fd);
    *fd = -1;
  }
}

static int open_client(uint16_t port) {
  struct sockaddr_in address;
  struct timeval timeout = { 2, 0 };
  int receive_buffer = 1024;
  int fd = socket(AF_INET, SOCK_STREAM, 0);

  if (fd < 0) return -1;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                   (socklen_t)sizeof(receive_buffer));
  (void)memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (const struct sockaddr *)&address, (socklen_t)sizeof(address)) != 0) {
    (void)close(fd);
    return -1;
  }
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   (socklen_t)sizeof(timeout));
  return fd;
}

static bool client_send(int fd, const unsigned char *bytes, size_t length) {
  size_t sent = 0u;

  for (size_t attempt = 0u; sent < length && attempt < 32u; ++attempt) {
    ssize_t amount = send(fd, bytes + sent, length - sent, 0);
    if (amount <= 0) return false;
    sent += (size_t)amount;
  }
  return sent == length;
}

static bool client_receive(int fd, unsigned char *bytes, size_t length) {
  size_t received = 0u;

  for (size_t attempt = 0u; received < length && attempt < 32u; ++attempt) {
    ssize_t amount = recv(fd, bytes + received, length - received, 0);
    if (amount <= 0) return false;
    received += (size_t)amount;
  }
  return received == length;
}

static bool buffer_equals(struct omni_bytebuf *buffer,
                          const unsigned char *expected,
                          size_t length) {
  size_t actual_length = 0u;
  const unsigned char *actual = omni_bytebuf_read_ptr(buffer, &actual_length);

  return actual != NULL && actual_length == length &&
         memcmp(actual, expected, length) == 0;
}

static void fixture_prepare(struct fixture *fixture) {
  (void)memset(fixture, 0, sizeof(*fixture));
  omni_connection_registry_make_inert(&fixture->registry);
  omni_reactor_make_inert(&fixture->reactor);
  omni_connection_reactor_make_inert(&fixture->adapter);
  omni_connection_runtime_make_inert(&fixture->runtime);
  omni_connection_manager_make_inert(&fixture->manager);
  omni_connection_dispatch_make_inert(&fixture->dispatch);
  omni_connection_policy_make_inert(&fixture->policy);
  omni_listener_make_inert(&fixture->listener);
  omni_connection_admission_make_inert(&fixture->admission);
  omni_listener_admission_make_inert(&fixture->listener_admission);
  for (size_t i = 0u; i < TEST_CAPACITY; ++i) {
    omni_connection_admission_slot_make_inert(&fixture->admission_slots[i]);
  }
}

static bool fixture_init(struct fixture *fixture) {
  struct omni_connection_runtime_config runtime_config = {0};
  struct omni_connection_manager_config manager_config = {0};
  struct omni_connection_dispatch_config dispatch_config = {0};
  struct omni_connection_policy_config policy_config = {0};
  struct omni_connection_admission_config admission_config = {0};
  struct omni_listener_admission_config listener_config = {0};

  fixture_prepare(fixture);
  if (omni_connection_registry_init(&fixture->registry, fixture->registry_slots,
                                   REGISTRY_CAPACITY).status != OMNI_CONNECTION_REGISTRY_OK) {
    return false;
  }
  if (omni_poller_init_borrowed(&fixture->poller, fixture->poller_fds,
                                fixture->poller_tokens, REACTOR_CAPACITY).status !=
      OMNI_POLLER_OK) {
    return false;
  }
  if (omni_reactor_init(&fixture->reactor, &fixture->poller,
                        fixture->registrations, fixture->events,
                        REACTOR_CAPACITY).status != OMNI_REACTOR_OK) {
    return false;
  }
  if (omni_connection_reactor_init(&fixture->adapter, &fixture->reactor,
                                   &fixture->registry,
                                   omni_connection_policy_callback,
                                   &fixture->policy).status != OMNI_CONNECTION_REACTOR_OK) {
    return false;
  }

  runtime_config.registry = &fixture->registry;
  runtime_config.adapter = &fixture->adapter;
  runtime_config.entries = fixture->runtime_entries;
  runtime_config.capacity = TEST_CAPACITY;
  if (omni_connection_runtime_init(&fixture->runtime, &runtime_config).status !=
      OMNI_CONNECTION_RUNTIME_OK) {
    return false;
  }
  manager_config.runtime = &fixture->runtime;
  manager_config.entries = fixture->manager_entries;
  manager_config.capacity = TEST_CAPACITY;
  if (omni_connection_manager_init(&fixture->manager, &manager_config).status !=
      OMNI_CONNECTION_MANAGER_OK) {
    return false;
  }
  dispatch_config.manager = &fixture->manager;
  if (omni_connection_dispatch_init(&fixture->dispatch, &dispatch_config).status !=
      OMNI_CONNECTION_DISPATCH_OK) {
    return false;
  }
  if (omni_listener_init(&fixture->listener, "127.0.0.1", 0u).status !=
      OMNI_LISTENER_OK) {
    return false;
  }

  admission_config.listener = &fixture->listener;
  admission_config.manager = &fixture->manager;
  admission_config.slots = fixture->admission_slots;
  admission_config.slots_bytes = sizeof(fixture->admission_slots);
  admission_config.capacity = TEST_CAPACITY;
  admission_config.connection_receive.storage = fixture->connection_receive;
  admission_config.connection_receive.storage_bytes = sizeof(fixture->connection_receive);
  admission_config.connection_receive.per_connection_capacity = BUFFER_CAPACITY;
  admission_config.session_receive.storage = fixture->session_receive;
  admission_config.session_receive.storage_bytes = sizeof(fixture->session_receive);
  admission_config.session_receive.per_connection_capacity = BUFFER_CAPACITY;
  admission_config.session_send.storage = fixture->session_send;
  admission_config.session_send.storage_bytes = sizeof(fixture->session_send);
  admission_config.session_send.per_connection_capacity = BUFFER_CAPACITY;
  if (omni_connection_admission_init(&fixture->admission, &admission_config).status !=
      OMNI_CONNECTION_ADMISSION_OK) {
    return false;
  }
  policy_config.admission = &fixture->admission;
  policy_config.dispatch = &fixture->dispatch;
  if (omni_connection_policy_init(&fixture->policy, &policy_config).status !=
      OMNI_CONNECTION_POLICY_OK) {
    return false;
  }

  listener_config.listener = &fixture->listener;
  listener_config.reactor = &fixture->reactor;
  listener_config.admission = &fixture->admission;
  listener_config.max_admissions_per_dispatch = 1u;
  listener_config.identities = fixture->identities;
  listener_config.identity_capacity = TEST_CAPACITY;
  return omni_listener_admission_init(&fixture->listener_admission,
                                      &listener_config).status ==
             OMNI_LISTENER_ADMISSION_OK &&
         omni_listener_admission_start(&fixture->listener_admission).status ==
             OMNI_LISTENER_ADMISSION_OK;
}

static void fixture_destroy(struct fixture *fixture) {
  (void)omni_listener_admission_destroy(&fixture->listener_admission);
  (void)omni_connection_admission_destroy(&fixture->admission);
  omni_connection_policy_destroy(&fixture->policy);
  omni_connection_manager_destroy(&fixture->manager);
  omni_connection_dispatch_destroy(&fixture->dispatch);
  omni_connection_runtime_destroy(&fixture->runtime);
  omni_connection_reactor_destroy(&fixture->adapter);
  omni_reactor_destroy(&fixture->reactor);
  omni_poller_destroy(&fixture->poller);
  omni_connection_registry_destroy(&fixture->registry);
  omni_listener_destroy(&fixture->listener);
}

static bool admission_step(struct fixture *fixture,
                           struct omni_connection_admission_identity *identity) {
  struct omni_reactor_result reactor_result = omni_reactor_step(&fixture->reactor, 0);
  struct omni_listener_admission_result admission_result =
      omni_listener_admission_last_result(&fixture->listener_admission);

  if (reactor_result.status != OMNI_REACTOR_OK ||
      admission_result.status != OMNI_LISTENER_ADMISSION_ADMISSION_LIMIT_REACHED ||
      admission_result.admission.admitted != 1u) {
    return false;
  }
  *identity = fixture->identities[0];
  return identity->token != 0u;
}

static bool connect_and_admit(struct fixture *fixture, int *client,
                              struct omni_connection_admission_identity *identity) {
  *client = open_client(omni_listener_port(&fixture->listener));
  if (*client < 0) return false;
  if (!admission_step(fixture, identity)) {
    close_fd(client);
    return false;
  }
  return true;
}

static struct omni_connection *connection_for(
    struct fixture *fixture, struct omni_connection_admission_identity identity) {
  if (identity.slot_index >= TEST_CAPACITY) return NULL;
  if (!fixture->admission_slots[identity.slot_index].occupied ||
      fixture->admission_slots[identity.slot_index].token != identity.token) {
    return NULL;
  }
  return &fixture->admission_slots[identity.slot_index].connection;
}

static struct omni_connection_session *session_for(
    struct fixture *fixture, struct omni_connection_admission_identity identity) {
  struct omni_connection *connection = connection_for(fixture, identity);
  if (connection == NULL) return NULL;
  return omni_connection_runtime_find_session(&fixture->runtime, connection);
}

static bool registration_interests(struct fixture *fixture, uint64_t token,
                                   uint32_t *reactor_interests,
                                   short *poller_events,
                                   uint64_t *poller_token) {
  for (size_t i = 0u; i < fixture->reactor.count; ++i) {
    const struct omni_reactor_registration *registration =
        &fixture->reactor.registrations[i];
    if (registration->token == token) {
      if (reactor_interests != NULL) *reactor_interests = registration->interests;
      for (size_t j = 0u; j < fixture->poller.count; ++j) {
        if (fixture->poller.fds[j].fd == registration->fd) {
          if (poller_events != NULL) *poller_events = fixture->poller.fds[j].events;
          if (poller_token != NULL) *poller_token = fixture->poller.tokens[j];
          return true;
        }
      }
      return false;
    }
  }
  return false;
}

static struct omni_connection_dispatch_result dispatch_once(
    struct fixture *fixture, struct omni_connection_admission_identity identity,
    uint32_t events) {
  struct omni_connection *connection = connection_for(fixture, identity);

  if (connection != NULL) {
    omni_connection_dispatch_callback(connection, identity.token, events,
                                      &fixture->dispatch);
  }
  return omni_connection_dispatch_last_result(&fixture->dispatch);
}

static bool release_identity(struct fixture *fixture,
                             struct omni_connection_admission_identity identity) {
  return omni_connection_admission_release(&fixture->admission, identity).status ==
         OMNI_CONNECTION_ADMISSION_OK;
}

static void test_lifecycle_and_init(void) {
  struct fixture fixture = {0};
  struct omni_connection_policy inert = {0};
  struct omni_connection_dispatch unavailable_dispatch = {0};
  struct omni_connection_policy_config bad_config = {0};
  uint64_t manager_count = 0u;

  omni_connection_policy_make_inert(NULL);
  omni_connection_policy_destroy(NULL);
  check(omni_connection_policy_state(NULL) == OMNI_CONNECTION_POLICY_INERT,
        "NULL policy state is inert and lifecycle helpers tolerate NULL");
  omni_connection_policy_make_inert(&inert);
  check(omni_connection_policy_state(&inert) == OMNI_CONNECTION_POLICY_INERT,
        "fresh policy storage becomes inert");
  check(omni_connection_policy_init(&inert, NULL).status ==
            OMNI_CONNECTION_POLICY_ERR_INVALID,
        "NULL initialization config is rejected");
  check(fixture_init(&fixture), "lifecycle fixture initializes all borrowed owners");
  bad_config.admission = NULL;
  bad_config.dispatch = &fixture.dispatch;
  check(omni_connection_policy_init(&inert, &bad_config).status ==
            OMNI_CONNECTION_POLICY_ERR_INVALID &&
            omni_connection_policy_state(&inert) == OMNI_CONNECTION_POLICY_INERT,
        "NULL admission dependency is rejected without changing inert state");
  bad_config.admission = &fixture.admission;
  bad_config.dispatch = &unavailable_dispatch;
  omni_connection_dispatch_make_inert(&unavailable_dispatch);
  check(omni_connection_policy_init(&inert, &bad_config).status ==
            OMNI_CONNECTION_POLICY_ERR_DEPENDENCY,
        "unavailable dispatch dependency is rejected");
  check(omni_connection_policy_state(&fixture.policy) == OMNI_CONNECTION_POLICY_ACTIVE,
        "valid policy init enters ACTIVE");
  check(omni_connection_policy_init(&fixture.policy, &bad_config).status ==
            OMNI_CONNECTION_POLICY_ERR_STATE,
        "duplicate initialization is rejected");
  manager_count = (uint64_t)omni_connection_manager_count(&fixture.manager);
  omni_connection_policy_destroy(&fixture.policy);
  omni_connection_policy_destroy(&fixture.policy);
  check(omni_connection_policy_state(&fixture.policy) == OMNI_CONNECTION_POLICY_CLOSED,
        "destroy enters CLOSED and is repeat-safe");
  check(fixture.manager.live && fixture.admission.live && fixture.adapter.live &&
            (uint64_t)omni_connection_manager_count(&fixture.manager) == manager_count,
        "policy destroy leaves borrowed manager, admission and reactor owners alive");
  omni_connection_policy_make_inert(&fixture.policy);
  fixture_destroy(&fixture);
}

static void test_read_keep_and_write_interest_cycle(void) {
  struct fixture fixture = {0};
  struct omni_connection_admission_identity identity = {0};
  struct omni_connection *connection = NULL;
  struct omni_connection_session *session = NULL;
  struct omni_bytebuf *send_buffer = NULL;
  struct omni_connection_dispatch_result dispatched;
  struct omni_connection_policy_result policy_result;
  struct omni_reactor_result reactor_result;
  const unsigned char outgoing[] = { 0x00u, 0x81u, 0x7fu, 0x2au, 0xffu };
  const unsigned char incoming[] = { 0x33u, 0x00u, 0xf0u };
  unsigned char received[sizeof(outgoing)];
  uint32_t reactor_interests = 0u;
  uint64_t poller_token = 0u;
  short poller_events = 0;
  uint64_t updates_before = 0u;
  size_t manager_count_before = 0u;
  int client = -1;

  check(fixture_init(&fixture), "interest fixture initializes");
  check(connect_and_admit(&fixture, &client, &identity),
        "listener readiness admits a managed connection");
  connection = connection_for(&fixture, identity);
  session = session_for(&fixture, identity);
  check(connection != NULL && session != NULL &&
            omni_connection_poller_interests(connection) == OMNI_POLLER_INTEREST_READ &&
            registration_interests(&fixture, identity.token, &reactor_interests,
                                   &poller_events, &poller_token) &&
            reactor_interests == OMNI_POLLER_INTEREST_READ && poller_events == POLLIN &&
            poller_token == identity.token,
        "new idle registration has READ only with the same generation token");

  dispatched = dispatch_once(&fixture, identity, OMNI_POLLER_READY_READ);
  policy_result = omni_connection_policy_apply(&fixture.policy, &dispatched);
  check(dispatched.read_attempted &&
            dispatched.read_result.status == OMNI_CONNECTION_IO_ERR_WOULD_BLOCK &&
            policy_result.status == OMNI_CONNECTION_POLICY_OK &&
            policy_result.action == OMNI_CONNECTION_POLICY_ACTION_KEEP &&
            !policy_result.connection_released &&
            omni_connection_admission_count(&fixture.admission) == 1u,
        "READ WOULD_BLOCK keeps the live connection and does not retry");

  check(client_send(client, incoming, sizeof(incoming)) &&
            omni_reactor_step(&fixture.reactor, 1000).status == OMNI_REACTOR_OK,
        "real READ readiness dispatch completes once");
  policy_result = omni_connection_policy_last_result(&fixture.policy);
  session = session_for(&fixture, identity);
  check(policy_result.has_dispatch && policy_result.dispatch.read_attempted &&
            policy_result.dispatch.bytes_read == sizeof(incoming) && session != NULL &&
            buffer_equals(omni_connection_session_receive_buffer(session), incoming,
                          sizeof(incoming)) &&
            omni_connection_manager_count(&fixture.manager) == 1u,
        "successful READ keeps raw binary payload buffered without protocol parsing");

  session = session_for(&fixture, identity);
  send_buffer = session == NULL ? NULL : omni_connection_session_send_buffer(session);
  check(send_buffer != NULL && omni_bytebuf_append(send_buffer, outgoing, sizeof(outgoing)),
        "application can append bounded outbound bytes to the existing session buffer");
  updates_before = omni_connection_policy_interest_updates(&fixture.policy);
  manager_count_before = omni_connection_manager_count(&fixture.manager);
  policy_result = omni_connection_policy_sync_interests(&fixture.policy, identity.token);
  check(policy_result.status == OMNI_CONNECTION_POLICY_OK &&
            policy_result.action == OMNI_CONNECTION_POLICY_ACTION_UPDATE_INTERESTS &&
            policy_result.old_interests == OMNI_POLLER_INTEREST_READ &&
            policy_result.desired_interests ==
                (OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE) &&
            policy_result.new_interests ==
                (OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE) &&
            policy_result.interest_updated &&
            omni_connection_policy_interest_updates(&fixture.policy) == updates_before + 1u,
        "pending output enables exactly READ|WRITE at the explicit sync point");
  check(registration_interests(&fixture, identity.token, &reactor_interests,
                               &poller_events, &poller_token) &&
            reactor_interests ==
                (OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE) &&
            poller_events == (POLLIN | POLLOUT) && poller_token == identity.token &&
            omni_connection_poller_interests(connection) == reactor_interests &&
            omni_connection_manager_count(&fixture.manager) == manager_count_before &&
            omni_connection_reactor_count(&fixture.adapter) == manager_count_before,
        "interest update preserves token, membership, and synchronized metadata");
  policy_result = omni_connection_policy_sync_interests(&fixture.policy, identity.token);
  check(policy_result.action == OMNI_CONNECTION_POLICY_ACTION_KEEP &&
            !policy_result.interest_updated &&
            omni_connection_policy_interest_updates(&fixture.policy) == updates_before + 1u,
        "repeated synchronization avoids a redundant poller update");

  (void)memset(&dispatched, 0, sizeof(dispatched));
  dispatched.status = OMNI_CONNECTION_DISPATCH_OK;
  dispatched.token = identity.token;
  dispatched.write_attempted = true;
  dispatched.write_result.status = OMNI_CONNECTION_IO_ERR_WOULD_BLOCK;
  dispatched.write_result.sys_errno = EWOULDBLOCK;
  policy_result = omni_connection_policy_apply(&fixture.policy, &dispatched);
  check(policy_result.action == OMNI_CONNECTION_POLICY_ACTION_KEEP &&
            policy_result.old_interests ==
                (OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE) &&
            policy_result.desired_interests ==
                (OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE) &&
            !policy_result.connection_released &&
            omni_connection_admission_count(&fixture.admission) == 1u &&
            omni_connection_policy_interest_updates(&fixture.policy) == updates_before + 1u,
        "WRITE WOULD_BLOCK keeps WRITE enabled and returns without a retry or close");

  reactor_result = omni_reactor_step(&fixture.reactor, 1000);
  check(reactor_result.status == OMNI_REACTOR_OK && reactor_result.count == 1u,
        "WRITE readiness dispatches one bounded callback");
  policy_result = omni_connection_policy_last_result(&fixture.policy);
  check(policy_result.dispatch.write_attempted &&
            policy_result.dispatch.bytes_written == sizeof(outgoing) &&
            policy_result.action == OMNI_CONNECTION_POLICY_ACTION_UPDATE_INTERESTS &&
            policy_result.old_interests ==
                (OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE) &&
            policy_result.desired_interests == OMNI_POLLER_INTEREST_READ &&
            policy_result.new_interests == OMNI_POLLER_INTEREST_READ &&
            omni_bytebuf_readable(omni_connection_session_send_buffer(
                session_for(&fixture, identity))) == 0u,
        "drained output disables WRITE and retains READ");
  check(client_receive(client, received, sizeof(received)) &&
            memcmp(received, outgoing, sizeof(outgoing)) == 0,
        "peer receives the exact binary output bytes");
  {
    uint64_t apply_calls = fixture.policy.apply_calls;
    reactor_result = omni_reactor_step(&fixture.reactor, 0);
    check(reactor_result.status == OMNI_REACTOR_OK && reactor_result.count == 0u &&
              fixture.policy.apply_calls == apply_calls,
          "idle probe produces no repeated writable callback");
  }
  check(release_identity(&fixture, identity), "healthy test connection releases through admission");
  close_fd(&client);
  fixture_destroy(&fixture);
}

static void test_hangup_error_and_buffered_data(void) {
  struct fixture fixture = {0};
  struct omni_connection_admission_identity identity = {0};
  struct omni_connection_admission_identity sibling_identity = {0};
  struct omni_connection *connection = NULL;
  struct omni_connection *sibling = NULL;
  struct omni_connection_session *session = NULL;
  struct omni_connection_policy_result policy_result;
  const unsigned char payload[] = { 0x00u, 0x44u, 0x80u, 0xffu };
  struct omni_reactor_result reactor_result;
  int client = -1;
  int sibling_client = -1;

  check(fixture_init(&fixture), "hangup fixture initializes");
  check(connect_and_admit(&fixture, &client, &identity),
        "connection is admitted before orderly peer close");
  check(connect_and_admit(&fixture, &sibling_client, &sibling_identity),
        "unrelated live connection is admitted alongside the close target");
  check(client_send(client, payload, sizeof(payload)),
        "peer queues payload before an orderly close");
  close_fd(&client);
  {
    struct omni_connection_reactor_result callback_result =
        omni_connection_reactor_dispatch(&fixture.adapter, identity.token,
                                         OMNI_POLLER_READY_HANGUP);
    check(callback_result.status == OMNI_CONNECTION_REACTOR_OK &&
              callback_result.count == 1u,
          "HANGUP readiness is forwarded once to the policy callback");
  }
  policy_result = omni_connection_policy_last_result(&fixture.policy);
  connection = connection_for(&fixture, identity);
  session = session_for(&fixture, identity);
  check(connection != NULL &&
            session != NULL &&
            policy_result.dispatch.hangup_observed &&
            policy_result.dispatch.read_attempted &&
            (policy_result.dispatch.readiness_events & OMNI_POLLER_READY_READ) != 0u &&
            policy_result.dispatch.read_result.status == OMNI_CONNECTION_IO_OK &&
            !policy_result.connection_released &&
            omni_connection_admission_count(&fixture.admission) == 2u &&
            buffer_equals(omni_connection_session_receive_buffer(session), payload,
                          sizeof(payload)),
        "HANGUP with readable bytes preserves them and keeps the connection for this dispatch");

  reactor_result = omni_reactor_step(&fixture.reactor, 1000);
  policy_result = omni_connection_policy_last_result(&fixture.policy);
  check(reactor_result.status == OMNI_REACTOR_OK &&
            policy_result.dispatch.read_attempted &&
            policy_result.dispatch.read_result.status == OMNI_CONNECTION_IO_ERR_EOF &&
            policy_result.action == OMNI_CONNECTION_POLICY_ACTION_CLOSE &&
            policy_result.connection_released &&
            omni_connection_manager_count(&fixture.manager) == 1u &&
            omni_connection_admission_count(&fixture.admission) == 1u &&
            omni_connection_reactor_count(&fixture.adapter) == 1u &&
            omni_reactor_count(&fixture.reactor) == 2u,
        "a later bounded READ surfaces EOF and ordered policy release removes the client registration");
  sibling = connection_for(&fixture, sibling_identity);
  check(sibling != NULL && omni_connection_is_live(sibling) &&
            fd_open(omni_connection_fd(sibling)),
        "close cleanup preserves the unrelated managed connection and descriptor");
  check(fixture.listener.live && fd_open(omni_listener_fd(&fixture.listener)),
        "EOF cleanup leaves the listener owner open");

  {
    struct omni_connection_policy_result before =
        omni_connection_policy_last_result(&fixture.policy);
    struct omni_connection_admission_identity replacement = {0};
    uint64_t dispatches_before =
        omni_connection_dispatch_dispatches(&fixture.dispatch);
    uint64_t policy_calls_before = fixture.policy.apply_calls;
    int replacement_client = -1;
    struct omni_connection_reactor_result stale_result;

    stale_result = omni_connection_reactor_dispatch(&fixture.adapter, identity.token,
                                                    OMNI_POLLER_READY_INVALID);
    check(stale_result.status == OMNI_CONNECTION_REACTOR_IGNORED &&
              omni_connection_dispatch_dispatches(&fixture.dispatch) == dispatches_before &&
              fixture.policy.apply_calls == policy_calls_before &&
              omni_connection_policy_last_result(&fixture.policy).action == before.action,
          "detached old reactor token cannot invoke policy for the sibling connection");
    check(connect_and_admit(&fixture, &replacement_client, &replacement),
          "a new client is admitted into capacity released by EOF policy");
    check(replacement.slot_index == identity.slot_index &&
              replacement.token != identity.token &&
              omni_connection_admission_count(&fixture.admission) == 2u,
          "released slot is reusable with a different generation token");
    {
      dispatches_before = omni_connection_dispatch_dispatches(&fixture.dispatch);
      policy_calls_before = fixture.policy.apply_calls;
      stale_result = omni_connection_reactor_dispatch(&fixture.adapter, identity.token,
                                                       OMNI_POLLER_READY_INVALID);
      struct omni_connection *replacement_connection =
          connection_for(&fixture, replacement);
      check(stale_result.status == OMNI_CONNECTION_REACTOR_IGNORED &&
                omni_connection_dispatch_dispatches(&fixture.dispatch) == dispatches_before &&
                fixture.policy.apply_calls == policy_calls_before &&
                replacement_connection != NULL &&
                omni_connection_is_live(replacement_connection),
        "old token after slot reuse cannot reach the new live connection");
    }
    close_fd(&replacement_client);
  }
  check(release_identity(&fixture, sibling_identity),
        "unrelated connection remains independently releasable");
  close_fd(&sibling_client);
  fixture_destroy(&fixture);

  {
    struct fixture error_fixture = {0};
    struct omni_connection_admission_identity error_identity = {0};
    struct omni_connection *error_connection = NULL;
    struct omni_connection_policy_result error_policy_result;
    struct omni_connection_dispatch_result error_dispatch;
    int error_client = -1;
    check(fixture_init(&error_fixture), "ERROR-readiness fixture initializes");
    check(connect_and_admit(&error_fixture, &error_client, &error_identity),
          "connection is available for ERROR readiness classification");
    error_connection = connection_for(&error_fixture, error_identity);
    error_dispatch = dispatch_once(&error_fixture, error_identity,
                                   OMNI_POLLER_READY_ERROR | OMNI_POLLER_READY_READ);
    error_policy_result = omni_connection_policy_apply(&error_fixture.policy,
                                                        &error_dispatch);
    check(error_connection != NULL && error_dispatch.error_observed &&
              error_dispatch.read_attempted &&
              error_dispatch.read_result.status == OMNI_CONNECTION_IO_ERR_WOULD_BLOCK &&
              error_policy_result.action == OMNI_CONNECTION_POLICY_ACTION_KEEP &&
              !error_policy_result.connection_released &&
              omni_connection_admission_count(&error_fixture.admission) == 1u,
          "ERROR alone does not close when the bounded READ reports normal WOULD_BLOCK");
    check(release_identity(&error_fixture, error_identity),
          "ERROR probe connection remains safely releasable by its owner");
    close_fd(&error_client);
    fixture_destroy(&error_fixture);
  }

  {
    struct fixture fatal_fixture = {0};
    struct omni_connection_admission_identity fatal_identity = {0};
    struct omni_connection_policy_result fatal_result;
    struct linger reset = { 1, 0 };
    int fatal_client = -1;
    check(fixture_init(&fatal_fixture), "fatal-read cleanup fixture initializes");
    check(connect_and_admit(&fatal_fixture, &fatal_client, &fatal_identity),
          "fatal-read cleanup client is admitted");
    check(setsockopt(fatal_client, SOL_SOCKET, SO_LINGER, &reset,
                     (socklen_t)sizeof(reset)) == 0,
          "test peer enables a reset close");
    close_fd(&fatal_client);
    reactor_result = omni_reactor_step(&fatal_fixture.reactor, 1000);
    fatal_result = omni_connection_policy_last_result(&fatal_fixture.policy);
    check(reactor_result.status == OMNI_REACTOR_OK &&
              fatal_result.dispatch.read_attempted &&
              fatal_result.dispatch.read_result.status == OMNI_CONNECTION_IO_ERR_IO &&
              fatal_result.action == OMNI_CONNECTION_POLICY_ACTION_CLOSE &&
              fatal_result.connection_released &&
              omni_connection_manager_count(&fatal_fixture.manager) == 0u &&
              omni_connection_admission_count(&fatal_fixture.admission) == 0u &&
              omni_reactor_count(&fatal_fixture.reactor) == 1u,
          "fatal reset read reaches policy and releases registration and admission ownership");
    fixture_destroy(&fatal_fixture);
  }
}

static struct omni_connection_dispatch_result synthetic_close_result(
    uint64_t token, bool read_side, enum omni_connection_io_status io_status, int sys_errno) {
  struct omni_connection_dispatch_result result = {0};

  result.status = OMNI_CONNECTION_DISPATCH_OK;
  result.token = token;
  result.close_worthy = true;
  if (read_side) {
    result.read_attempted = true;
    result.read_result.status = io_status;
    result.read_result.sys_errno = sys_errno;
  } else {
    result.write_attempted = true;
    result.write_result.status = io_status;
    result.write_result.sys_errno = sys_errno;
  }
  return result;
}

static void test_close_worthy_statuses_and_stale_reuse(void) {
  struct fixture fixture = {0};
  const enum omni_connection_io_status statuses[] = {
    OMNI_CONNECTION_IO_ERR_EOF,
    OMNI_CONNECTION_IO_ERR_IO,
    OMNI_CONNECTION_IO_ERR_IO,
    OMNI_CONNECTION_IO_ERR_IO
  };
  const int errors[] = { 0, ECONNRESET, EPIPE, EIO };
  const bool read_sides[] = { true, true, false, false };
  const char *descriptions[] = {
    "synthetic Task 031 receive EOF releases through admission",
    "synthetic fatal receive result releases through admission",
    "synthetic peer-closed send result releases through admission",
    "synthetic fatal send result releases through admission"
  };
  struct omni_connection_admission_identity prior = {0};
  bool have_prior = false;

  check(fixture_init(&fixture), "close classification fixture initializes");
  for (size_t i = 0u; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
    struct omni_connection_admission_identity identity = {0};
    struct omni_connection_dispatch_result dispatched;
    struct omni_connection_policy_result result;
    int client = -1;

    check(connect_and_admit(&fixture, &client, &identity),
          "close-worthy connection is admitted");
    if (have_prior) {
      check(identity.slot_index == prior.slot_index && identity.token != prior.token,
            "released admission slot is reused with a new generation token");
    }
    dispatched = synthetic_close_result(identity.token, read_sides[i], statuses[i], errors[i]);
    result = omni_connection_policy_apply(&fixture.policy, &dispatched);
    check(result.status == OMNI_CONNECTION_POLICY_OK &&
              result.action == OMNI_CONNECTION_POLICY_ACTION_CLOSE &&
              result.connection_released && result.release_status ==
                  OMNI_CONNECTION_ADMISSION_OK &&
              omni_connection_manager_count(&fixture.manager) == 0u &&
              omni_connection_admission_count(&fixture.admission) == 0u &&
              omni_connection_reactor_count(&fixture.adapter) == 0u &&
              omni_reactor_count(&fixture.reactor) == 1u,
          descriptions[i]);
    prior = identity;
    have_prior = true;
    close_fd(&client);
  }

  {
    struct omni_connection_admission_identity current = {0};
    struct omni_connection_dispatch_result stale = {0};
    struct omni_connection_dispatch_result invalid = {0};
    struct omni_connection_policy_result result;
    struct omni_connection *current_connection = NULL;
    int client = -1;

    check(connect_and_admit(&fixture, &client, &current),
          "slot can be admitted after repeated close/release cycles");
    check(current.slot_index == prior.slot_index && current.token != prior.token,
          "latest occupant has a fresh token after slot reuse");
    stale.status = OMNI_CONNECTION_DISPATCH_STALE;
    stale.token = prior.token;
    stale.ignored = true;
    stale.stale = true;
    stale.invalid_observed = true;
    stale.close_worthy = true;
    result = omni_connection_policy_apply(&fixture.policy, &stale);
    current_connection = connection_for(&fixture, current);
    check(result.status == OMNI_CONNECTION_POLICY_IGNORED &&
              result.action == OMNI_CONNECTION_POLICY_ACTION_IGNORE &&
              !result.connection_released && current_connection != NULL &&
              omni_connection_is_live(current_connection) &&
              omni_connection_manager_count(&fixture.manager) == 1u &&
              omni_connection_admission_count(&fixture.admission) == 1u,
          "stale INVALID result is ignored before close policy and cannot reach reused storage");

    invalid.status = OMNI_CONNECTION_DISPATCH_INVALID_EVENT;
    invalid.token = current.token;
    invalid.invalid_observed = true;
    invalid.close_worthy = true;
    result = omni_connection_policy_apply(&fixture.policy, &invalid);
    check(result.action == OMNI_CONNECTION_POLICY_ACTION_CLOSE &&
              result.connection_released &&
              omni_connection_admission_count(&fixture.admission) == 0u &&
              omni_reactor_count(&fixture.reactor) == 1u,
          "reactor INVALID releases the current connection without raw descriptor cleanup");
    check(fixture.listener.live && fd_open(omni_listener_fd(&fixture.listener)),
          "INVALID cleanup preserves the listener");
    close_fd(&client);
  }
  fixture_destroy(&fixture);
}

static void test_interest_update_failure_preserves_state(void) {
  struct fixture fixture = {0};
  struct omni_connection_admission_identity identity = {0};
  struct omni_connection *connection = NULL;
  struct omni_connection_session *session = NULL;
  struct omni_bytebuf *send_buffer = NULL;
  struct omni_connection_policy_result result;
  const unsigned char payload[] = { 1u, 2u, 3u };
  uint32_t reactor_interests = 0u;
  short poller_events = 0;
  uint64_t poller_token = 0u;
  size_t poller_count = 0u;
  int client = -1;

  check(fixture_init(&fixture), "interest failure fixture initializes");
  check(connect_and_admit(&fixture, &client, &identity),
        "interest failure client is admitted");
  connection = connection_for(&fixture, identity);
  session = session_for(&fixture, identity);
  send_buffer = session == NULL ? NULL : omni_connection_session_send_buffer(session);
  check(send_buffer != NULL && omni_bytebuf_append(send_buffer, payload, sizeof(payload)),
        "interest failure queues bounded output before synchronization");

  poller_count = fixture.poller.count;
  fixture.poller.count = 0u;
  result = omni_connection_policy_sync_interests(&fixture.policy, identity.token);
  fixture.poller.count = poller_count;
  check(result.status == OMNI_CONNECTION_POLICY_ERR_INTEREST_UPDATE &&
            result.action == OMNI_CONNECTION_POLICY_ACTION_ERROR &&
            result.reactor_status == OMNI_CONNECTION_REACTOR_ERR_REACTOR &&
            result.sys_errno == ENOENT &&
            !result.interest_updated && !result.connection_released &&
            result.old_interests == OMNI_POLLER_INTEREST_READ &&
            result.new_interests == OMNI_POLLER_INTEREST_READ &&
            result.desired_interests ==
                (OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE),
        "failed poller update is surfaced and leaves the healthy connection live");
  check(connection != NULL && omni_connection_poller_interests(connection) ==
            OMNI_POLLER_INTEREST_READ &&
            registration_interests(&fixture, identity.token, &reactor_interests,
                                   &poller_events, &poller_token) &&
            reactor_interests == OMNI_POLLER_INTEREST_READ && poller_events == POLLIN &&
            poller_token == identity.token &&
            omni_connection_manager_count(&fixture.manager) == 1u &&
            omni_connection_admission_count(&fixture.admission) == 1u,
        "interest failure preserves connection metadata, reactor mask, poller mask and membership");
  result = omni_connection_policy_sync_interests(&fixture.policy, identity.token);
  check(result.action == OMNI_CONNECTION_POLICY_ACTION_UPDATE_INTERESTS &&
            result.new_interests ==
                (OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE),
        "interest synchronization succeeds after the lower poller is available again");
  check(release_identity(&fixture, identity),
        "connection remains owner-releasable after interest failure recovery");
  close_fd(&client);
  fixture_destroy(&fixture);
}

static void test_release_failure_preserves_owner_state(void) {
  struct fixture fixture = {0};
  struct omni_connection_admission_identity identity = {0};
  struct omni_connection_dispatch_result dispatched;
  struct omni_connection_policy_result result;
  enum omni_connection_manager_state saved_state;
  struct omni_connection *connection = NULL;
  int accepted_fd = -1;
  int client = -1;

  check(fixture_init(&fixture), "release failure fixture initializes");
  check(connect_and_admit(&fixture, &client, &identity),
        "release failure connection is admitted");
  connection = connection_for(&fixture, identity);
  accepted_fd = connection == NULL ? -1 : omni_connection_fd(connection);
  dispatched = synthetic_close_result(identity.token, true,
                                      OMNI_CONNECTION_IO_ERR_EOF, 0);
  saved_state = fixture.manager.state;
  fixture.manager.state = OMNI_CONNECTION_MANAGER_CLOSED;
  result = omni_connection_policy_apply(&fixture.policy, &dispatched);
  fixture.manager.state = saved_state;
  check(result.status == OMNI_CONNECTION_POLICY_ERR_RELEASE &&
            result.action == OMNI_CONNECTION_POLICY_ACTION_ERROR &&
            result.release_status == OMNI_CONNECTION_ADMISSION_ERR_MANAGER &&
            result.release_manager_status == OMNI_CONNECTION_MANAGER_ERR_STATE &&
            !result.connection_released && connection != NULL &&
            omni_connection_is_live(connection) && fd_open(accepted_fd) &&
            omni_connection_admission_count(&fixture.admission) == 1u,
        "failed owner release is reported without claiming release or raw-closing the FD");
  check(omni_connection_manager_count(&fixture.manager) == 1u &&
            omni_connection_reactor_count(&fixture.adapter) == 1u &&
            omni_reactor_count(&fixture.reactor) == 2u,
        "precondition release failure preserves manager, adapter and reactor membership");
  result = omni_connection_policy_apply(&fixture.policy, &dispatched);
  check(result.action == OMNI_CONNECTION_POLICY_ACTION_CLOSE &&
            result.connection_released &&
            omni_connection_admission_count(&fixture.admission) == 0u,
        "restored owner accepts the same close decision and releases exactly once");
  close_fd(&client);
  fixture_destroy(&fixture);
}

static void test_saturating_counters_and_destroyed_policy(void) {
  struct fixture fixture = {0};
  struct omni_connection_admission_identity identity = {0};
  struct omni_connection_dispatch_result close_result;
  struct omni_connection_policy_result result;
  int client = -1;

  check(fixture_init(&fixture), "counter fixture initializes");
  check(connect_and_admit(&fixture, &client, &identity),
        "counter test connection is admitted");
  fixture.policy.actions = UINT64_MAX - 1u;
  fixture.policy.close_worthy_observations = UINT64_MAX - 1u;
  fixture.policy.closed_count = UINT64_MAX - 1u;
  close_result = synthetic_close_result(identity.token, true,
                                       OMNI_CONNECTION_IO_ERR_IO, EIO);
  result = omni_connection_policy_apply(&fixture.policy, &close_result);
  check(result.connection_released && omni_connection_policy_actions(&fixture.policy) ==
            UINT64_MAX &&
            omni_connection_policy_close_worthy_observations(&fixture.policy) ==
                UINT64_MAX &&
            omni_connection_policy_closed_count(&fixture.policy) == UINT64_MAX,
        "policy accounting counters saturate instead of wrapping");
  omni_connection_policy_destroy(&fixture.policy);
  result = omni_connection_policy_sync_interests(&fixture.policy, identity.token);
  check(result.status == OMNI_CONNECTION_POLICY_ERR_STATE &&
            result.action == OMNI_CONNECTION_POLICY_ACTION_NONE &&
            omni_connection_policy_closed_count(&fixture.policy) == UINT64_MAX,
        "destroyed policy performs no further interest or lifecycle action");
  close_fd(&client);
  fixture_destroy(&fixture);
}

static void test_1000_interest_release_cycles(void) {
  struct fixture fixture = {0};
  size_t baseline = SIZE_MAX;
  size_t completed = 0u;
  uint64_t previous_token = 0u;
  bool ok = true;

  check(fixture_init(&fixture), "1,000-cycle policy stress fixture initializes");
  baseline = fd_count();
  for (size_t cycle = 0u; cycle < STRESS_CYCLES && ok; ++cycle) {
    struct omni_connection_admission_identity identity = {0};
    struct omni_connection_policy_result policy_result;
    struct omni_connection_session *session = NULL;
    struct omni_bytebuf *send_buffer = NULL;
    struct omni_connection *connection = NULL;
    struct omni_reactor_result reactor_result;
    unsigned char payload[4] = {
      (unsigned char)(cycle & 0xffu),
      (unsigned char)((cycle >> 4u) & 0xffu),
      0u,
      (unsigned char)(0x80u | (cycle & 0x7fu))
    };
    unsigned char echoed[sizeof(payload)];
    int client = open_client(omni_listener_port(&fixture.listener));

    if (client < 0 || !client_send(client, payload, sizeof(payload)) ||
        !admission_step(&fixture, &identity) ||
        (cycle != 0u && identity.token == previous_token)) {
      ok = false;
      close_fd(&client);
      break;
    }
    previous_token = identity.token;
    connection = connection_for(&fixture, identity);
    session = session_for(&fixture, identity);
    send_buffer = session == NULL ? NULL : omni_connection_session_send_buffer(session);
    if (connection == NULL || send_buffer == NULL ||
        !omni_bytebuf_append(send_buffer, payload, sizeof(payload))) {
      ok = false;
    }
    if (ok &&
        (omni_connection_manager_count(&fixture.manager) >
             omni_connection_manager_capacity(&fixture.manager) ||
         omni_connection_admission_count(&fixture.admission) >
             omni_connection_admission_capacity(&fixture.admission) ||
         omni_connection_reactor_count(&fixture.adapter) > TEST_CAPACITY ||
         fixture.reactor.count > TEST_CAPACITY + 1u ||
         fixture.poller.count > TEST_CAPACITY + 1u)) {
      ok = false;
    }
    if (ok) {
      policy_result = omni_connection_policy_sync_interests(&fixture.policy,
                                                             identity.token);
      if (policy_result.action != OMNI_CONNECTION_POLICY_ACTION_UPDATE_INTERESTS ||
          policy_result.desired_interests !=
              (OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE)) {
        ok = false;
      }
    }
    if (ok) {
      reactor_result = omni_reactor_step(&fixture.reactor, 1000);
      policy_result = omni_connection_policy_last_result(&fixture.policy);
      session = session_for(&fixture, identity);
      if (reactor_result.status != OMNI_REACTOR_OK ||
          !policy_result.dispatch.read_attempted ||
          !policy_result.dispatch.write_attempted ||
          policy_result.dispatch.bytes_read != sizeof(payload) ||
          policy_result.dispatch.bytes_written != sizeof(payload) ||
          policy_result.action != OMNI_CONNECTION_POLICY_ACTION_UPDATE_INTERESTS ||
          policy_result.new_interests != OMNI_POLLER_INTEREST_READ || session == NULL ||
          !buffer_equals(omni_connection_session_receive_buffer(session), payload,
                         sizeof(payload)) ||
          !client_receive(client, echoed, sizeof(echoed)) ||
          memcmp(echoed, payload, sizeof(payload)) != 0) {
        ok = false;
      }
    }
    if (ok && shutdown(client, SHUT_WR) != 0) ok = false;
    if (ok) {
      reactor_result = omni_reactor_step(&fixture.reactor, 1000);
      policy_result = omni_connection_policy_last_result(&fixture.policy);
      if (reactor_result.status != OMNI_REACTOR_OK ||
          policy_result.dispatch.read_result.status != OMNI_CONNECTION_IO_ERR_EOF ||
          policy_result.action != OMNI_CONNECTION_POLICY_ACTION_CLOSE ||
          !policy_result.connection_released) {
        ok = false;
      } else {
        ++completed;
      }
    }
    if (omni_connection_manager_count(&fixture.manager) >
            omni_connection_manager_capacity(&fixture.manager) ||
        omni_connection_admission_count(&fixture.admission) >
            omni_connection_admission_capacity(&fixture.admission) ||
        omni_connection_reactor_count(&fixture.adapter) > TEST_CAPACITY ||
        omni_reactor_count(&fixture.reactor) != 1u) {
      ok = false;
    }
    close_fd(&client);
    if (fd_count() != baseline) ok = false;
  }
  check(ok && completed == STRESS_CYCLES,
        "1,000 admit/dispatch/WRITE-enable/drain/EOF-release cycles complete");
  check(ok && omni_connection_policy_closed_count(&fixture.policy) >= STRESS_CYCLES &&
            omni_connection_policy_interest_updates(&fixture.policy) >=
                (uint64_t)(STRESS_CYCLES * 2u) &&
            omni_connection_manager_count(&fixture.manager) == 0u &&
            omni_connection_admission_count(&fixture.admission) == 0u &&
            omni_reactor_count(&fixture.reactor) == 1u,
        "stress leaves bounded registrations and records both interest transitions per cycle");
  check(ok && fd_count() == baseline,
        "Linux FD census returns to baseline after 1,000 policy release cycles");
  fixture_destroy(&fixture);
}

int main(void) {
  test_lifecycle_and_init();
  test_read_keep_and_write_interest_cycle();
  test_hangup_error_and_buffered_data();
  test_close_worthy_statuses_and_stale_reuse();
  test_interest_update_failure_preserves_state();
  test_release_failure_preserves_owner_state();
  test_saturating_counters_and_destroyed_policy();
  test_1000_interest_release_cycles();
  (void)printf("connection-policy-unit: %zu checks, %zu failures\n", checks, failures);
  (void)printf("connection-policy memory: policy=%zu config=%zu result=%zu bytes\n",
               sizeof(struct omni_connection_policy),
               sizeof(struct omni_connection_policy_config),
               sizeof(struct omni_connection_policy_result));
  return failures == 0u ? 0 : 1;
}
