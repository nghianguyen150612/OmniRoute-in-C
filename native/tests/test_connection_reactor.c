/*
 * OmniRoute native backend — connection/reactor adapter tests (Task 022).
 *
 * These tests use caller-owned pipe descriptors as accepted-owner stand-ins.
 * The production connection accepts any already-owned descriptor and does
 * not inspect its origin; using pipes keeps the adapter suite focused on
 * registration, token lifetime, callback forwarding, and ownership. The
 * listener/accept/connection integration is already covered by the earlier
 * native suites. No production payload read or write is performed here.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "omniroute/connection_reactor.h"

#define OMNI_TEST_CAPACITY 4u
#define OMNI_TEST_STRESS_CYCLES 1000u
#define OMNI_TEST_RECEIVE_CAPACITY 64u

static size_t check_count = 0u;
static size_t failure_count = 0u;

struct connection_fixture {
  struct omni_accepted accepted;
  struct omni_connection connection;
  unsigned char receive_storage[OMNI_TEST_RECEIVE_CAPACITY];
  int peer_fd;
};

struct adapter_fixture {
  struct omni_poller poller;
  struct omni_reactor reactor;
  struct omni_connection_registry registry;
  struct omni_connection_reactor adapter;
  struct pollfd poll_slots[OMNI_TEST_CAPACITY];
  uint64_t poll_tokens[OMNI_TEST_CAPACITY];
  struct omni_reactor_registration registrations[OMNI_TEST_CAPACITY];
  struct omni_poller_event events[OMNI_TEST_CAPACITY];
  struct omni_connection_registry_slot registry_slots[OMNI_TEST_CAPACITY];
};

struct callback_state {
  size_t calls;
  struct omni_connection *connections[OMNI_TEST_CAPACITY];
  uint64_t tokens[OMNI_TEST_CAPACITY];
  uint32_t events[OMNI_TEST_CAPACITY];
  void *last_context;
  struct omni_connection_reactor *adapter;
  struct omni_connection *detach_on_connection;
  struct omni_connection *detach_target;
  struct omni_connection_reactor_result detach_result;
};

static void check(bool condition, const char *message) {
  ++check_count;
  if (condition) {
    printf("ok - %s\n", message);
  } else {
    ++failure_count;
    printf("NOT OK - %s\n", message);
  }
}

static bool fd_is_open(int fd) {
  if (fd < 0) {
    return false;
  }
  return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

static void connection_fixture_make_inert(struct connection_fixture *fixture) {
  if (fixture == NULL) {
    return;
  }
  fixture->peer_fd = -1;
  omni_accepted_make_inert(&fixture->accepted);
  omni_connection_make_inert(&fixture->connection);
}

static bool connection_fixture_start(struct connection_fixture *fixture,
                                     bool own_write_end,
                                     uint64_t metadata_token,
                                     uint32_t interests) {
  struct omni_connection_config config;
  int pipe_fds[2] = { -1, -1 };
  int owned_index = own_write_end ? 1 : 0;
  int peer_index = own_write_end ? 0 : 1;

  if (fixture == NULL || pipe(pipe_fds) != 0) {
    return false;
  }
  memset(fixture, 0, sizeof(*fixture));
  connection_fixture_make_inert(fixture);
  fixture->accepted.fd = pipe_fds[owned_index];
  fixture->accepted.live = true;
  fixture->peer_fd = pipe_fds[peer_index];
  config.receive_storage = fixture->receive_storage;
  config.receive_capacity = sizeof(fixture->receive_storage);
  config.poller_token = metadata_token;
  config.poller_interests = interests;
  if (omni_connection_init(&fixture->connection, &config).status != OMNI_CONNECTION_OK ||
      omni_connection_from_accepted(&fixture->connection, &fixture->accepted).status !=
          OMNI_CONNECTION_OK) {
    omni_connection_destroy(&fixture->connection);
    omni_accepted_destroy(&fixture->accepted);
    (void)close(fixture->peer_fd);
    fixture->peer_fd = -1;
    return false;
  }
  return true;
}

static void connection_fixture_close_peer(struct connection_fixture *fixture) {
  if (fixture == NULL || fixture->peer_fd < 0) {
    return;
  }
  (void)close(fixture->peer_fd);
  fixture->peer_fd = -1;
}

static void connection_fixture_stop(struct connection_fixture *fixture) {
  if (fixture == NULL) {
    return;
  }
  omni_connection_destroy(&fixture->connection);
  omni_accepted_destroy(&fixture->accepted);
  connection_fixture_close_peer(fixture);
}

static void capture_callback(struct omni_connection *connection,
                             uint64_t token,
                             uint32_t events,
                             void *context) {
  struct callback_state *state = (struct callback_state *)context;
  size_t index = 0u;

  if (state == NULL) {
    return;
  }
  index = state->calls;
  if (index < OMNI_TEST_CAPACITY) {
    state->connections[index] = connection;
    state->tokens[index] = token;
    state->events[index] = events;
  }
  state->calls += 1u;
  state->last_context = context;
  if (connection == state->detach_on_connection && state->detach_target != NULL &&
      state->adapter != NULL) {
    state->detach_result =
        omni_connection_reactor_detach(state->adapter, state->detach_target);
    state->detach_target = NULL;
  }
}

static bool adapter_fixture_start(struct adapter_fixture *fixture,
                                  size_t reactor_capacity,
                                  size_t registry_capacity,
                                  struct callback_state *state) {
  struct omni_poller_result poller_result;
  struct omni_reactor_result reactor_result;
  struct omni_connection_registry_result registry_result;
  struct omni_connection_reactor_result adapter_result;

  if (fixture == NULL || reactor_capacity == 0u ||
      reactor_capacity > OMNI_TEST_CAPACITY || registry_capacity == 0u ||
      registry_capacity > OMNI_TEST_CAPACITY) {
    return false;
  }
  memset(fixture, 0, sizeof(*fixture));
  omni_reactor_make_inert(&fixture->reactor);
  omni_connection_registry_make_inert(&fixture->registry);
  omni_connection_reactor_make_inert(&fixture->adapter);
  poller_result = omni_poller_init_borrowed(&fixture->poller, fixture->poll_slots,
                                            fixture->poll_tokens, reactor_capacity);
  if (poller_result.status != OMNI_POLLER_OK) {
    return false;
  }
  reactor_result = omni_reactor_init(&fixture->reactor, &fixture->poller,
                                     fixture->registrations, fixture->events,
                                     reactor_capacity);
  if (reactor_result.status != OMNI_REACTOR_OK) {
    omni_poller_destroy(&fixture->poller);
    return false;
  }
  registry_result = omni_connection_registry_init(&fixture->registry,
                                                   fixture->registry_slots,
                                                   registry_capacity);
  if (registry_result.status != OMNI_CONNECTION_REGISTRY_OK) {
    omni_reactor_destroy(&fixture->reactor);
    omni_poller_destroy(&fixture->poller);
    return false;
  }
  adapter_result = omni_connection_reactor_init(&fixture->adapter, &fixture->reactor,
                                                &fixture->registry, capture_callback, state);
  if (adapter_result.status != OMNI_CONNECTION_REACTOR_OK) {
    omni_connection_registry_destroy(&fixture->registry);
    omni_reactor_destroy(&fixture->reactor);
    omni_poller_destroy(&fixture->poller);
    return false;
  }
  return true;
}

static void adapter_fixture_stop(struct adapter_fixture *fixture) {
  if (fixture == NULL) {
    return;
  }
  omni_connection_reactor_destroy(&fixture->adapter);
  omni_reactor_destroy(&fixture->reactor);
  omni_connection_registry_destroy(&fixture->registry);
  omni_poller_destroy(&fixture->poller);
}

static bool callback_seen(const struct callback_state *state,
                          const struct omni_connection *connection,
                          uint32_t required_events) {
  size_t i = 0u;

  if (state == NULL) {
    return false;
  }
  for (i = 0u; i < state->calls && i < OMNI_TEST_CAPACITY; ++i) {
    if (state->connections[i] == connection &&
        (state->events[i] & required_events) == required_events) {
      return true;
    }
  }
  return false;
}

static void test_invalid_inputs_and_lifecycle(void) {
  struct adapter_fixture runtime;
  struct callback_state state = { 0 };
  struct omni_connection_reactor adapter = { 0 };
  struct omni_reactor reactor = { 0 };
  struct omni_connection_registry registry = { 0 };
  struct omni_connection_reactor_result result;

  omni_connection_reactor_make_inert(&adapter);
  result = omni_connection_reactor_init(NULL, NULL, NULL, NULL, NULL);
  check(result.status == OMNI_CONNECTION_REACTOR_ERR_INVALID,
        "NULL adapter initialization is rejected");
  result = omni_connection_reactor_init(&adapter, &reactor, &registry, capture_callback,
                                        &state);
  check(result.status == OMNI_CONNECTION_REACTOR_ERR_INVALID,
        "inert reactor and registry are rejected");
  check(adapter_fixture_start(&runtime, 2u, 2u, &state),
        "adapter lifecycle fixture initializes");
  if (!runtime.adapter.live) {
    return;
  }
  result = omni_connection_reactor_init(&runtime.adapter, &runtime.reactor,
                                        &runtime.registry, capture_callback, &state);
  check(result.status == OMNI_CONNECTION_REACTOR_ERR_STATE,
        "duplicate adapter initialization is rejected");
  check(omni_connection_reactor_capacity(&runtime.adapter) == 2u &&
            omni_connection_reactor_count(&runtime.adapter) == 0u,
        "adapter exposes fixed registry accounting");
  adapter_fixture_stop(&runtime);
}

static void test_attach_duplicate_and_detach(void) {
  struct adapter_fixture runtime;
  struct callback_state state = { 0 };
  struct connection_fixture connection_fixture;
  struct omni_connection_reactor_result first;
  struct omni_connection_reactor_result duplicate;
  struct omni_connection_reactor_result detached;
  struct omni_connection_reactor_result missing;
  struct omni_connection_reactor_result invalid;

  check(adapter_fixture_start(&runtime, 2u, 2u, &state),
        "attach fixture initializes");
  invalid = omni_connection_reactor_attach(&runtime.adapter, NULL);
  check(invalid.status == OMNI_CONNECTION_REACTOR_ERR_INVALID,
        "attach rejects a NULL connection pointer");
  invalid = omni_connection_reactor_detach(&runtime.adapter, NULL);
  check(invalid.status == OMNI_CONNECTION_REACTOR_ERR_INVALID,
        "detach rejects a NULL connection pointer");
  check(connection_fixture_start(&connection_fixture, false, 0x101u,
                                 OMNI_POLLER_INTEREST_READ),
        "attach connection prepares as OPEN");
  first = omni_connection_reactor_attach(&runtime.adapter, &connection_fixture.connection);
  check(first.status == OMNI_CONNECTION_REACTOR_OK && first.token != 0u &&
            first.count == 0u && omni_connection_reactor_count(&runtime.adapter) == 1u &&
            omni_reactor_count(&runtime.reactor) == 1u,
        "valid connection attaches with a nonzero registry token");
  duplicate =
      omni_connection_reactor_attach(&runtime.adapter, &connection_fixture.connection);
  check(duplicate.status == OMNI_CONNECTION_REACTOR_ERR_DUPLICATE &&
            omni_connection_reactor_count(&runtime.adapter) == 1u &&
            omni_reactor_count(&runtime.reactor) == 1u,
        "duplicate attach is rejected without changing either registration set");
  detached = omni_connection_reactor_detach(&runtime.adapter, &connection_fixture.connection);
  check(detached.status == OMNI_CONNECTION_REACTOR_OK && detached.token == first.token &&
            omni_connection_reactor_count(&runtime.adapter) == 0u &&
            omni_reactor_count(&runtime.reactor) == 0u &&
            omni_connection_is_live(&connection_fixture.connection),
        "valid detach unregisters without changing connection ownership");
  missing = omni_connection_reactor_detach(&runtime.adapter, &connection_fixture.connection);
  check(missing.status == OMNI_CONNECTION_REACTOR_ERR_NOT_FOUND && missing.sys_errno == ENOENT,
        "detach of a missing connection reports NOT_FOUND");
  connection_fixture_stop(&connection_fixture);
  adapter_fixture_stop(&runtime);
}

static void test_read_write_dispatch_and_context(void) {
  struct adapter_fixture runtime;
  struct callback_state state = { 0 };
  struct connection_fixture readable;
  struct connection_fixture writable;
  struct omni_connection_reactor_result readable_attach;
  struct omni_connection_reactor_result writable_attach;
  struct omni_reactor_result step_result;
  unsigned char byte = 0xA5u;
  unsigned char received = 0u;

  check(adapter_fixture_start(&runtime, 2u, 2u, &state),
        "read/write fixture initializes");
  state.adapter = &runtime.adapter;
  check(connection_fixture_start(&readable, false, 0x201u, OMNI_POLLER_INTEREST_READ),
        "readable connection prepares");
  check(connection_fixture_start(&writable, true, 0x202u, OMNI_POLLER_INTEREST_WRITE),
        "writable connection prepares");
  readable_attach = omni_connection_reactor_attach(&runtime.adapter, &readable.connection);
  writable_attach = omni_connection_reactor_attach(&runtime.adapter, &writable.connection);
  check(readable_attach.status == OMNI_CONNECTION_REACTOR_OK &&
            writable_attach.status == OMNI_CONNECTION_REACTOR_OK,
        "readable and writable connections attach");
  check(write(readable.peer_fd, &byte, sizeof(byte)) == (ssize_t)sizeof(byte),
        "readability stimulus is written by the test peer");
  step_result = omni_reactor_step(&runtime.reactor, 0);
  check(step_result.status == OMNI_REACTOR_OK && step_result.count == 2u && state.calls == 2u,
        "one reactor step reaches both attached connection callbacks");
  check(callback_seen(&state, &readable.connection, OMNI_POLLER_READY_READ),
        "readable readiness is forwarded to the matching connection");
  check(callback_seen(&state, &writable.connection, OMNI_POLLER_READY_WRITE),
        "writable readiness is forwarded to the matching connection");
  check(state.last_context == &state,
        "adapter callback context is propagated without replacement");
  check(read(readable.connection.accepted.fd, &received, sizeof(received)) ==
                (ssize_t)sizeof(received) &&
            received == byte,
        "adapter callback does not read payload bytes automatically");
  check(fd_is_open(omni_connection_fd(&readable.connection)) &&
            fd_is_open(omni_connection_fd(&writable.connection)),
        "readiness dispatch does not close connection descriptors");
  check(omni_connection_reactor_detach(&runtime.adapter, &readable.connection).status ==
            OMNI_CONNECTION_REACTOR_OK &&
            omni_connection_reactor_detach(&runtime.adapter, &writable.connection).status ==
                OMNI_CONNECTION_REACTOR_OK,
        "read/write connections detach cleanly");
  connection_fixture_stop(&readable);
  connection_fixture_stop(&writable);
  adapter_fixture_stop(&runtime);
}

static void test_error_dispatch(void) {
  struct adapter_fixture runtime;
  struct callback_state state = { 0 };
  struct connection_fixture connection_fixture;
  struct omni_reactor_result step_result;

  check(adapter_fixture_start(&runtime, 1u, 1u, &state),
        "error fixture initializes");
  check(connection_fixture_start(&connection_fixture, true, 0x301u,
                                 OMNI_POLLER_INTEREST_WRITE),
        "error connection prepares");
  check(omni_connection_reactor_attach(&runtime.adapter, &connection_fixture.connection).status ==
            OMNI_CONNECTION_REACTOR_OK,
        "error connection attaches");
  connection_fixture_close_peer(&connection_fixture);
  step_result = omni_reactor_step(&runtime.reactor, 0);
  check(step_result.status == OMNI_REACTOR_OK && step_result.count == 1u && state.calls == 1u,
        "error readiness produces one connection callback");
  check(callback_seen(&state, &connection_fixture.connection, OMNI_POLLER_READY_ERROR),
        "error readiness is forwarded without connection cleanup");
  check(omni_connection_is_live(&connection_fixture.connection),
        "error callback preserves the connection owner");
  check(omni_connection_reactor_detach(&runtime.adapter, &connection_fixture.connection).status ==
            OMNI_CONNECTION_REACTOR_OK,
        "error connection detaches after notification");
  connection_fixture_stop(&connection_fixture);
  adapter_fixture_stop(&runtime);
}

static void test_stale_tokens_and_removed_events(void) {
  struct adapter_fixture runtime;
  struct callback_state state = { 0 };
  struct connection_fixture first;
  struct connection_fixture second;
  struct omni_connection_reactor_result first_attach;
  struct omni_connection_reactor_result second_attach;
  struct omni_connection_reactor_result stale;
  struct omni_reactor_result step_result;
  unsigned char byte = 0x31u;

  check(adapter_fixture_start(&runtime, 2u, 2u, &state),
        "stale-token fixture initializes");
  state.adapter = &runtime.adapter;
  check(connection_fixture_start(&first, false, 0x401u, OMNI_POLLER_INTEREST_READ),
        "first stale-token connection prepares");
  first_attach = omni_connection_reactor_attach(&runtime.adapter, &first.connection);
  check(first_attach.status == OMNI_CONNECTION_REACTOR_OK,
        "first stale-token connection attaches");
  check(omni_connection_reactor_detach(&runtime.adapter, &first.connection).status ==
            OMNI_CONNECTION_REACTOR_OK,
        "detaching retires the first token");
  stale = omni_connection_reactor_dispatch(&runtime.adapter, first_attach.token,
                                           OMNI_POLLER_READY_READ);
  check(stale.status == OMNI_CONNECTION_REACTOR_IGNORED && state.calls == 0u,
        "a detached token is safely ignored");

  check(connection_fixture_start(&second, false, 0x402u, OMNI_POLLER_INTEREST_READ),
        "second stale-token connection prepares");
  second_attach = omni_connection_reactor_attach(&runtime.adapter, &second.connection);
  check(second_attach.status == OMNI_CONNECTION_REACTOR_OK &&
            second_attach.token != first_attach.token,
        "slot reuse receives a different generation token");
  stale = omni_connection_reactor_dispatch(&runtime.adapter, first_attach.token,
                                           OMNI_POLLER_READY_READ);
  check(stale.status == OMNI_CONNECTION_REACTOR_IGNORED && state.calls == 0u,
        "an old token cannot reach a later slot occupant");
  check(write(second.peer_fd, &byte, sizeof(byte)) == (ssize_t)sizeof(byte),
        "second connection receives readiness stimulus");
  step_result = omni_reactor_step(&runtime.reactor, 0);
  check(step_result.status == OMNI_REACTOR_OK && step_result.count == 1u && state.calls == 1u,
        "only the current registration dispatches after token reuse");
  check(omni_connection_reactor_detach(&runtime.adapter, &second.connection).status ==
            OMNI_CONNECTION_REACTOR_OK,
        "current slot occupant detaches cleanly");
  connection_fixture_stop(&first);
  connection_fixture_stop(&second);
  adapter_fixture_stop(&runtime);
}

static void test_remove_later_registration_during_dispatch(void) {
  struct adapter_fixture runtime;
  struct callback_state state = { 0 };
  struct connection_fixture first;
  struct connection_fixture second;
  struct omni_reactor_result step_result;
  unsigned char byte = 0x52u;

  check(adapter_fixture_start(&runtime, 2u, 2u, &state),
        "remove-during-dispatch fixture initializes");
  check(connection_fixture_start(&first, false, 0x501u, OMNI_POLLER_INTEREST_READ),
        "first removal-order connection prepares");
  check(connection_fixture_start(&second, false, 0x502u, OMNI_POLLER_INTEREST_READ),
        "second removal-order connection prepares");
  state.adapter = &runtime.adapter;
  state.detach_on_connection = &first.connection;
  state.detach_target = &second.connection;
  check(omni_connection_reactor_attach(&runtime.adapter, &first.connection).status ==
            OMNI_CONNECTION_REACTOR_OK &&
            omni_connection_reactor_attach(&runtime.adapter, &second.connection).status ==
                OMNI_CONNECTION_REACTOR_OK,
        "both removal-order connections attach in deterministic order");
  check(write(first.peer_fd, &byte, sizeof(byte)) == (ssize_t)sizeof(byte) &&
            write(second.peer_fd, &byte, sizeof(byte)) == (ssize_t)sizeof(byte),
        "both removal-order connections become readable");
  step_result = omni_reactor_step(&runtime.reactor, 0);
  check(step_result.status == OMNI_REACTOR_OK && step_result.count == 1u &&
            state.calls == 1u &&
            state.detach_result.status == OMNI_CONNECTION_REACTOR_OK,
        "removing a later connection during dispatch skips its stale event");
  check(omni_connection_reactor_count(&runtime.adapter) == 1u &&
            omni_reactor_count(&runtime.reactor) == 1u &&
            omni_connection_is_live(&second.connection),
        "removed connection remains owned but receives no callback");
  check(omni_connection_reactor_detach(&runtime.adapter, &first.connection).status ==
            OMNI_CONNECTION_REACTOR_OK,
        "first connection detaches after in-callback removal");
  connection_fixture_stop(&first);
  connection_fixture_stop(&second);
  adapter_fixture_stop(&runtime);
}

static void test_reactor_failure_and_closed_connection(void) {
  struct adapter_fixture runtime;
  struct callback_state state = { 0 };
  struct connection_fixture first;
  struct connection_fixture second;
  struct connection_fixture closed;
  struct omni_connection_reactor_result result;

  check(adapter_fixture_start(&runtime, 1u, 2u, &state),
        "reactor-failure fixture initializes with asymmetric capacities");
  check(connection_fixture_start(&first, false, 0x601u, OMNI_POLLER_INTEREST_READ),
        "first reactor-failure connection prepares");
  check(connection_fixture_start(&second, false, 0x602u, OMNI_POLLER_INTEREST_READ),
        "second reactor-failure connection prepares");
  check(omni_connection_reactor_attach(&runtime.adapter, &first.connection).status ==
            OMNI_CONNECTION_REACTOR_OK,
        "first reactor-failure connection attaches");
  result = omni_connection_reactor_attach(&runtime.adapter, &second.connection);
  check(result.status == OMNI_CONNECTION_REACTOR_ERR_REACTOR &&
            omni_connection_reactor_count(&runtime.adapter) == 1u &&
            omni_reactor_count(&runtime.reactor) == 1u,
        "reactor capacity failure rolls back registry membership");
  check(omni_connection_reactor_detach(&runtime.adapter, &first.connection).status ==
            OMNI_CONNECTION_REACTOR_OK,
        "first reactor-failure connection detaches");

  check(connection_fixture_start(&closed, false, 0x603u, OMNI_POLLER_INTEREST_READ),
        "closed-connection fixture prepares");
  omni_connection_destroy(&closed.connection);
  result = omni_connection_reactor_attach(&runtime.adapter, &closed.connection);
  check(result.status == OMNI_CONNECTION_REACTOR_ERR_STATE &&
            omni_connection_reactor_count(&runtime.adapter) == 0u,
        "closed connections cannot attach");
  connection_fixture_stop(&first);
  connection_fixture_stop(&second);
  connection_fixture_stop(&closed);
  adapter_fixture_stop(&runtime);
}

static void test_reactor_destroy_preserves_connections(void) {
  struct adapter_fixture runtime;
  struct callback_state state = { 0 };
  struct connection_fixture connection_fixture;
  struct omni_connection_reactor_result attached;
  struct omni_connection_reactor_result dispatch_result;
  int connection_fd = -1;

  check(adapter_fixture_start(&runtime, 1u, 1u, &state),
        "reactor-destroy fixture initializes");
  check(connection_fixture_start(&connection_fixture, false, 0x701u,
                                 OMNI_POLLER_INTEREST_READ),
        "reactor-destroy connection prepares");
  attached = omni_connection_reactor_attach(&runtime.adapter, &connection_fixture.connection);
  connection_fd = omni_connection_fd(&connection_fixture.connection);
  check(attached.status == OMNI_CONNECTION_REACTOR_OK && fd_is_open(connection_fd),
        "reactor-destroy connection attaches with an open descriptor");
  omni_reactor_destroy(&runtime.reactor);
  check(omni_connection_is_live(&connection_fixture.connection) &&
            omni_connection_fd(&connection_fixture.connection) == connection_fd &&
            fd_is_open(connection_fd),
        "reactor destruction does not destroy or close connections");
  dispatch_result = omni_connection_reactor_dispatch(&runtime.adapter, attached.token,
                                                     OMNI_POLLER_READY_READ);
  check(dispatch_result.status == OMNI_CONNECTION_REACTOR_IGNORED && state.calls == 0u,
        "adapter ignores tokens after the generic reactor is destroyed");
  omni_connection_reactor_destroy(&runtime.adapter);
  check(omni_connection_reactor_count(&runtime.adapter) == 0u &&
            omni_connection_is_live(&connection_fixture.connection) &&
            fd_is_open(connection_fd),
        "adapter cleanup retires membership without destroying the connection");
  connection_fixture_stop(&connection_fixture);
  adapter_fixture_stop(&runtime);
}

static void test_repeated_attach_detach(void) {
  struct adapter_fixture runtime;
  struct callback_state state = { 0 };
  struct connection_fixture connection_fixture;
  uint64_t previous_token = 0u;
  uint32_t cycle = 0u;
  bool cycles_ok = true;

  check(adapter_fixture_start(&runtime, 1u, 1u, &state),
        "stress fixture initializes");
  check(connection_fixture_start(&connection_fixture, false, 0x801u,
                                 OMNI_POLLER_INTEREST_READ),
        "stress connection prepares");
  for (cycle = 0u; cycle < OMNI_TEST_STRESS_CYCLES; ++cycle) {
    struct omni_connection_reactor_result attached =
        omni_connection_reactor_attach(&runtime.adapter, &connection_fixture.connection);
    struct omni_connection_reactor_result detached;

    if (attached.status != OMNI_CONNECTION_REACTOR_OK || attached.token == 0u ||
        (cycle > 0u && attached.token == previous_token) ||
        omni_connection_reactor_count(&runtime.adapter) != 1u) {
      cycles_ok = false;
      break;
    }
    previous_token = attached.token;
    detached = omni_connection_reactor_detach(&runtime.adapter, &connection_fixture.connection);
    if (detached.status != OMNI_CONNECTION_REACTOR_OK ||
        omni_connection_reactor_count(&runtime.adapter) != 0u ||
        omni_reactor_count(&runtime.reactor) != 0u) {
      cycles_ok = false;
      break;
    }
  }
  check(cycles_ok, "repeated attach/detach retires every bounded registration");
  check(omni_connection_is_live(&connection_fixture.connection) &&
            state.calls == 0u,
        "stress cycles preserve the connection and do not synthesize callbacks");
  connection_fixture_stop(&connection_fixture);
  adapter_fixture_stop(&runtime);
}

int main(void) {
  printf("=== Starting connection reactor unit tests (Task 022) ===\n");

  test_invalid_inputs_and_lifecycle();
  test_attach_duplicate_and_detach();
  test_read_write_dispatch_and_context();
  test_error_dispatch();
  test_stale_tokens_and_removed_events();
  test_remove_later_registration_during_dispatch();
  test_reactor_failure_and_closed_connection();
  test_reactor_destroy_preserves_connections();
  test_repeated_attach_detach();

  printf("Total checks: %zu\n", check_count);
  printf("Failures: %zu\n", failure_count);
  return failure_count == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
