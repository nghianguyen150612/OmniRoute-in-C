/*
 * OmniRoute native backend — runtime coordinator tests (Task 023).
 *
 * The suite uses the real bounded listener plus local pipe descriptors for
 * one externally owned connection. It verifies orchestration and ownership;
 * the production coordinator itself performs no accept or event-loop step.
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

#include "omniroute/connection.h"
#include "omniroute/runtime.h"

#define OMNI_TEST_CAPACITY 3u
#define OMNI_TEST_STRESS_CYCLES 1000u
#define OMNI_TEST_RECEIVE_CAPACITY 64u

static size_t check_count = 0u;
static size_t failure_count = 0u;

struct runtime_fixture {
  struct omni_runtime runtime;
  struct omni_listener listener;
  struct omni_connection_registry registry;
  struct omni_reactor reactor;
  struct pollfd poller_fds[OMNI_TEST_CAPACITY];
  uint64_t poller_tokens[OMNI_TEST_CAPACITY];
  struct omni_connection_registry_slot registry_slots[OMNI_TEST_CAPACITY];
  struct omni_reactor_registration reactor_registrations[OMNI_TEST_CAPACITY];
  struct omni_poller_event reactor_events[OMNI_TEST_CAPACITY];
};

struct connection_fixture {
  struct omni_accepted accepted;
  struct omni_connection connection;
  unsigned char receive_storage[OMNI_TEST_RECEIVE_CAPACITY];
  int peer_fd;
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

static void no_op_callback(uint64_t token, uint32_t events, void *context) {
  (void)token;
  (void)events;
  (void)context;
}

static void runtime_fixture_prepare(struct runtime_fixture *fixture) {
  if (fixture == NULL) {
    return;
  }
  omni_runtime_make_inert(&fixture->runtime);
  omni_listener_make_inert(&fixture->listener);
  omni_connection_registry_make_inert(&fixture->registry);
  omni_reactor_make_inert(&fixture->reactor);
}

static struct omni_runtime_config runtime_fixture_config(
    struct runtime_fixture *fixture) {
  struct omni_runtime_config config = { 0 };

  config.listener = &fixture->listener;
  config.registry = &fixture->registry;
  config.reactor = &fixture->reactor;
  config.registry_slots = fixture->registry_slots;
  config.registry_capacity = OMNI_TEST_CAPACITY;
  config.poller_fds = fixture->poller_fds;
  config.poller_tokens = fixture->poller_tokens;
  config.poller_capacity = OMNI_TEST_CAPACITY;
  config.reactor_registrations = fixture->reactor_registrations;
  config.reactor_events = fixture->reactor_events;
  config.reactor_capacity = OMNI_TEST_CAPACITY;
  config.listener_address = "127.0.0.1";
  config.listener_port = 0u;
  return config;
}

static bool runtime_fixture_init(struct runtime_fixture *fixture) {
  struct omni_runtime_config config;
  struct omni_runtime_result result;

  runtime_fixture_prepare(fixture);
  config = runtime_fixture_config(fixture);
  result = omni_runtime_init(&fixture->runtime, &config);
  return result.status == OMNI_RUNTIME_OK;
}

static void connection_fixture_prepare(struct connection_fixture *fixture) {
  if (fixture == NULL) {
    return;
  }
  fixture->peer_fd = -1;
  omni_accepted_make_inert(&fixture->accepted);
  omni_connection_make_inert(&fixture->connection);
}

static bool connection_fixture_init(struct connection_fixture *fixture) {
  struct omni_connection_config config;
  int pipe_fds[2] = { -1, -1 };

  connection_fixture_prepare(fixture);
  if (fixture == NULL || pipe(pipe_fds) != 0) {
    return false;
  }
  fixture->accepted.fd = pipe_fds[0];
  fixture->accepted.live = true;
  fixture->peer_fd = pipe_fds[1];
  config.receive_storage = fixture->receive_storage;
  config.receive_capacity = sizeof(fixture->receive_storage);
  config.poller_token = 0xA023u;
  config.poller_interests = OMNI_POLLER_INTEREST_READ;
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

static void connection_fixture_destroy(struct connection_fixture *fixture) {
  if (fixture == NULL) {
    return;
  }
  omni_connection_destroy(&fixture->connection);
  omni_accepted_destroy(&fixture->accepted);
  if (fixture->peer_fd >= 0) {
    (void)close(fixture->peer_fd);
    fixture->peer_fd = -1;
  }
}

static void test_make_inert_and_invalid_inputs(void) {
  struct runtime_fixture fixture = { 0 };
  struct omni_runtime_config config;
  struct omni_runtime_result result;

  runtime_fixture_prepare(&fixture);
  check(omni_runtime_state(&fixture.runtime) == OMNI_RUNTIME_INERT,
        "make inert establishes the INERT state");
  result = omni_runtime_start(&fixture.runtime);
  check(result.status == OMNI_RUNTIME_ERR_STATE && result.state == OMNI_RUNTIME_INERT,
        "start rejects an inert runtime");
  result = omni_runtime_stop(&fixture.runtime);
  check(result.status == OMNI_RUNTIME_ERR_STATE && result.state == OMNI_RUNTIME_INERT,
        "stop rejects an inert runtime");
  result = omni_runtime_init(&fixture.runtime, NULL);
  check(result.status == OMNI_RUNTIME_ERR_INVALID && result.state == OMNI_RUNTIME_INERT,
        "init rejects a NULL configuration");
  check(!fixture.listener.live && !fixture.registry.live && !fixture.reactor.live &&
            !fixture.runtime.poller.live,
        "invalid init leaves every subsystem inert");

  config = runtime_fixture_config(&fixture);
  config.registry_capacity = 0u;
  result = omni_runtime_init(&fixture.runtime, &config);
  check(result.status == OMNI_RUNTIME_ERR_INVALID && result.sys_errno == EINVAL,
        "init rejects zero registry capacity");
  check(omni_runtime_state(&fixture.runtime) == OMNI_RUNTIME_INERT,
        "invalid capacity keeps the runtime inert");
  omni_runtime_destroy(&fixture.runtime);
  omni_runtime_destroy(&fixture.runtime);
  check(omni_runtime_state(&fixture.runtime) == OMNI_RUNTIME_INERT,
        "destroy is safe and idempotent for an inert runtime");
}

static void test_lifecycle_and_transitions(void) {
  struct runtime_fixture fixture = { 0 };
  struct omni_runtime_config config;
  struct omni_runtime_result result;
  int listener_fd = -1;

  check(runtime_fixture_init(&fixture), "runtime init succeeds");
  if (!fixture.runtime.listener_initialized) {
    return;
  }
  config = runtime_fixture_config(&fixture);
  check(omni_runtime_state(&fixture.runtime) == OMNI_RUNTIME_INITIALIZED,
        "successful init enters INITIALIZED");
  check(fixture.registry.live && fixture.reactor.live && fixture.runtime.poller.live &&
            fixture.listener.live,
        "successful init makes registry, reactor, poller, and listener live");
  check(omni_connection_registry_capacity(&fixture.registry) == OMNI_TEST_CAPACITY &&
            omni_reactor_capacity(&fixture.reactor) == OMNI_TEST_CAPACITY &&
            omni_poller_capacity(&fixture.runtime.poller) == OMNI_TEST_CAPACITY,
        "runtime preserves each fixed subsystem capacity");
  listener_fd = omni_listener_fd(&fixture.listener);
  check(listener_fd >= 0 && fd_is_open(listener_fd) && omni_listener_port(&fixture.listener) != 0u,
        "runtime listener owns a live loopback descriptor and assigned port");

  result = omni_runtime_init(&fixture.runtime, &config);
  check(result.status == OMNI_RUNTIME_ERR_STATE && result.state == OMNI_RUNTIME_INITIALIZED,
        "reinitialization of a live runtime is rejected");
  result = omni_runtime_stop(&fixture.runtime);
  check(result.status == OMNI_RUNTIME_ERR_STATE && result.state == OMNI_RUNTIME_INITIALIZED,
        "stop before start is rejected");
  result = omni_runtime_start(&fixture.runtime);
  check(result.status == OMNI_RUNTIME_OK && result.state == OMNI_RUNTIME_RUNNING,
        "start enters RUNNING without starting an event loop");
  result = omni_runtime_start(&fixture.runtime);
  check(result.status == OMNI_RUNTIME_ERR_STATE && result.state == OMNI_RUNTIME_RUNNING,
        "repeated start is rejected");
  result = omni_runtime_stop(&fixture.runtime);
  check(result.status == OMNI_RUNTIME_OK && result.state == OMNI_RUNTIME_STOPPED,
        "stop completes the synchronous shutdown sequence");
  check(!fixture.listener.live && !fixture.registry.live && !fixture.reactor.live &&
            !fixture.runtime.poller.live && omni_listener_fd(&fixture.listener) == -1 &&
            !fd_is_open(listener_fd),
        "stop leaves all initialized subsystems inert and closes only the listener owner");
  check(!fixture.runtime.registry_initialized && !fixture.runtime.poller_initialized &&
            !fixture.runtime.reactor_initialized && !fixture.runtime.listener_initialized &&
            fixture.runtime.listener == NULL && fixture.runtime.registry == NULL &&
            fixture.runtime.reactor == NULL,
        "stop clears subsystem ownership tracking and borrowed references");
  result = omni_runtime_stop(&fixture.runtime);
  check(result.status == OMNI_RUNTIME_ERR_STATE && result.state == OMNI_RUNTIME_STOPPED,
        "repeated stop is rejected after the runtime is stopped");
  omni_runtime_destroy(&fixture.runtime);
  omni_runtime_destroy(&fixture.runtime);
  check(omni_runtime_state(&fixture.runtime) == OMNI_RUNTIME_STOPPED,
        "destroy is safe and idempotent after stop");
}

static void test_partial_initialization_cleanup(void) {
  struct runtime_fixture fixture = { 0 };
  struct omni_runtime_config config;
  struct omni_runtime_result result;

  runtime_fixture_prepare(&fixture);
  memset(fixture.registry_slots, 0xA5, sizeof(fixture.registry_slots));
  memset(fixture.poller_fds, 0xA5, sizeof(fixture.poller_fds));
  memset(fixture.reactor_registrations, 0xA5, sizeof(fixture.reactor_registrations));
  config = runtime_fixture_config(&fixture);
  config.listener_address = "not-an-ip-address";
  result = omni_runtime_init(&fixture.runtime, &config);
  check(result.status == OMNI_RUNTIME_ERR_LISTENER && result.sys_errno == EINVAL,
        "listener failure is reported after earlier subsystem initialization");
  check(omni_runtime_state(&fixture.runtime) == OMNI_RUNTIME_INERT &&
            !fixture.registry.live && !fixture.runtime.poller.live && !fixture.reactor.live &&
            !fixture.listener.live,
        "partial initialization cleanup returns every subsystem to inert");
  check(fixture.registry_slots[0].connection == NULL && fixture.registry_slots[0].generation == 0u &&
            !fixture.registry_slots[0].occupied && fixture.poller_fds[0].fd == -1 &&
            fixture.reactor_registrations[0].fd == -1 &&
            fixture.reactor_registrations[0].callback == NULL,
        "partial cleanup leaves caller arrays bounded and non-live");
  omni_runtime_destroy(&fixture.runtime);
  check(omni_runtime_state(&fixture.runtime) == OMNI_RUNTIME_INERT,
        "destroy after failed init remains safe");
}

static void test_borrowed_listener_not_closed_on_rejection(void) {
  struct runtime_fixture fixture = { 0 };
  struct omni_listener external_listener = { 0 };
  struct omni_runtime_config config;
  struct omni_runtime_result result;
  struct omni_listener_result listener_result;
  int external_fd = -1;

  runtime_fixture_prepare(&fixture);
  omni_listener_make_inert(&external_listener);
  listener_result = omni_listener_init(&external_listener, "127.0.0.1", 0u);
  check(listener_result.status == OMNI_LISTENER_OK, "external listener fixture initializes");
  if (listener_result.status != OMNI_LISTENER_OK) {
    return;
  }
  external_fd = omni_listener_fd(&external_listener);
  config = runtime_fixture_config(&fixture);
  config.listener = &external_listener;
  result = omni_runtime_init(&fixture.runtime, &config);
  check(result.status == OMNI_RUNTIME_ERR_INVALID &&
            omni_runtime_state(&fixture.runtime) == OMNI_RUNTIME_INERT,
        "runtime rejects a listener it did not initialize");
  check(fd_is_open(external_fd) && omni_listener_fd(&external_listener) == external_fd,
        "rejected borrowed listener remains externally live and open");
  omni_runtime_destroy(&fixture.runtime);
  omni_listener_destroy(&external_listener);
  check(!fd_is_open(external_fd), "external listener closes only under its own destroy call");
}

static void test_runtime_does_not_destroy_external_connections(void) {
  struct runtime_fixture fixture = { 0 };
  struct connection_fixture connection_fixture = { 0 };
  struct omni_connection_registry_result registry_result;
  struct omni_reactor_result reactor_result;
  int connection_fd = -1;

  check(runtime_fixture_init(&fixture), "ownership fixture initializes");
  if (!fixture.runtime.listener_initialized) {
    return;
  }
  check(omni_runtime_start(&fixture.runtime).status == OMNI_RUNTIME_OK,
        "ownership fixture starts");
  check(connection_fixture_init(&connection_fixture), "external connection fixture initializes");
  if (!omni_connection_is_live(&connection_fixture.connection)) {
    omni_runtime_destroy(&fixture.runtime);
    return;
  }
  connection_fd = omni_connection_fd(&connection_fixture.connection);
  registry_result = omni_connection_registry_add(&fixture.registry,
                                                 &connection_fixture.connection);
  reactor_result = omni_reactor_add(&fixture.reactor, connection_fd, 0x2301u,
                                    OMNI_POLLER_INTEREST_READ, no_op_callback, NULL);
  check(registry_result.status == OMNI_CONNECTION_REGISTRY_OK &&
            reactor_result.status == OMNI_REACTOR_OK,
        "external connection can use the runtime registry and reactor bounds");
  if (reactor_result.status != OMNI_REACTOR_OK &&
      registry_result.status == OMNI_CONNECTION_REGISTRY_OK) {
    (void)omni_connection_registry_remove(&fixture.registry, registry_result.handle);
  }
  omni_runtime_destroy(&fixture.runtime);
  check(omni_runtime_state(&fixture.runtime) == OMNI_RUNTIME_STOPPED &&
            omni_connection_state(&connection_fixture.connection) == OMNI_CONNECTION_OPEN &&
            omni_connection_fd(&connection_fixture.connection) == connection_fd &&
            fd_is_open(connection_fd),
        "runtime destroy removes registration metadata without destroying the connection");
  check(omni_connection_registry_count(&fixture.registry) == 0u &&
            omni_reactor_count(&fixture.reactor) == 0u,
        "runtime destroy leaves registry and reactor metadata inert");
  connection_fixture_destroy(&connection_fixture);
}

static void test_repeated_cycles(void) {
  struct runtime_fixture fixture = { 0 };
  bool cycles_ok = true;
  size_t cycle = 0u;

  runtime_fixture_prepare(&fixture);
  for (cycle = 0u; cycle < OMNI_TEST_STRESS_CYCLES; ++cycle) {
    struct omni_runtime_config config = runtime_fixture_config(&fixture);
    struct omni_runtime_result result;

    result = omni_runtime_init(&fixture.runtime, &config);
    if (result.status != OMNI_RUNTIME_OK ||
        omni_runtime_start(&fixture.runtime).status != OMNI_RUNTIME_OK ||
        omni_runtime_stop(&fixture.runtime).status != OMNI_RUNTIME_OK ||
        omni_runtime_state(&fixture.runtime) != OMNI_RUNTIME_STOPPED) {
      cycles_ok = false;
      break;
    }
    omni_runtime_destroy(&fixture.runtime);
    omni_runtime_make_inert(&fixture.runtime);
    if (omni_runtime_state(&fixture.runtime) != OMNI_RUNTIME_INERT) {
      cycles_ok = false;
      break;
    }
  }
  check(cycles_ok, "repeated init/start/stop/destroy cycles stay bounded");
  check(!fixture.listener.live && !fixture.registry.live && !fixture.reactor.live &&
            !fixture.runtime.poller.live,
        "stress cycles leave no live subsystem");
  omni_runtime_destroy(&fixture.runtime);
}

int main(void) {
  printf("=== Starting runtime coordinator unit tests (Task 023) ===\n");

  test_make_inert_and_invalid_inputs();
  test_lifecycle_and_transitions();
  test_partial_initialization_cleanup();
  test_borrowed_listener_not_closed_on_rejection();
  test_runtime_does_not_destroy_external_connections();
  test_repeated_cycles();

  printf("Total checks: %zu\n", check_count);
  printf("Failures: %zu\n", failure_count);
  return failure_count == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
