#define _POSIX_C_SOURCE 200809L

/*
 * OmniRoute native backend — bounded event-loop tests (Task 024).
 *
 * The suite uses the real Task 023 runtime and Task 021 reactor over local
 * pipes. It verifies synchronous orchestration and borrowed ownership; the
 * production event loop performs no payload I/O and owns no test descriptor.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "omniroute/connection.h"
#include "omniroute/connection_reactor.h"
#include "omniroute/event_loop.h"

#define OMNI_TEST_CAPACITY 4u
#define OMNI_TEST_RECEIVE_CAPACITY 64u
#define OMNI_TEST_STRESS_CYCLES 1000u

static size_t check_count = 0u;
static size_t failure_count = 0u;
static volatile sig_atomic_t alarm_count = 0;

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

struct stop_context {
  struct omni_event_loop *loop;
  size_t calls;
  uint64_t token;
  uint32_t events;
  enum omni_event_loop_status stop_status;
};

struct connection_stop_context {
  struct omni_event_loop *loop;
  struct omni_connection *seen_connection;
  size_t calls;
  uint64_t token;
  uint32_t events;
  enum omni_event_loop_status stop_status;
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

  runtime_fixture_prepare(fixture);
  config = runtime_fixture_config(fixture);
  return omni_runtime_init(&fixture->runtime, &config).status == OMNI_RUNTIME_OK;
}

static bool runtime_fixture_start(struct runtime_fixture *fixture) {
  return omni_runtime_start(&fixture->runtime).status == OMNI_RUNTIME_OK;
}

static void runtime_fixture_destroy(struct runtime_fixture *fixture) {
  if (fixture == NULL) {
    return;
  }
  omni_runtime_destroy(&fixture->runtime);
}

static bool event_loop_init_for(struct omni_event_loop *loop,
                                struct omni_runtime *runtime,
                                int64_t timeout_ms) {
  struct omni_event_loop_config config;

  config.runtime = runtime;
  config.timeout_ms = timeout_ms;
  return omni_event_loop_init(loop, &config).status == OMNI_EVENT_LOOP_OK;
}

static void no_op_callback(uint64_t token, uint32_t events, void *context) {
  (void)token;
  (void)events;
  (void)context;
}

static void stop_loop_callback(uint64_t token, uint32_t events, void *context) {
  struct stop_context *stop = (struct stop_context *)context;

  if (stop == NULL) {
    return;
  }
  stop->calls += 1u;
  stop->token = token;
  stop->events = events;
  stop->stop_status = omni_event_loop_stop(stop->loop).status;
}

static void connection_stop_callback(struct omni_connection *connection,
                                     uint64_t token,
                                     uint32_t events,
                                     void *context) {
  struct connection_stop_context *stop = (struct connection_stop_context *)context;

  if (stop == NULL) {
    return;
  }
  stop->calls += 1u;
  stop->seen_connection = connection;
  stop->token = token;
  stop->events = events;
  stop->stop_status = omni_event_loop_stop(stop->loop).status;
}

static void alarm_callback(int signal_number) {
  (void)signal_number;
  alarm_count += 1;
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
  config.poller_token = 0x2401u;
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

static void test_inert_and_invalid_initialization(void) {
  struct runtime_fixture fixture = { 0 };
  struct omni_event_loop loop = { 0 };
  struct omni_event_loop_config config = { 0 };
  struct omni_event_loop_result result;

  runtime_fixture_prepare(&fixture);
  omni_event_loop_make_inert(&loop);
  check(omni_event_loop_state(&loop) == OMNI_EVENT_LOOP_INERT,
        "make inert establishes the INERT event-loop state");
  check(omni_event_loop_state(NULL) == OMNI_EVENT_LOOP_INERT &&
            omni_event_loop_iterations(NULL) == 0u &&
            omni_event_loop_events_processed(NULL) == 0u,
        "NULL observers report inert and zero accounting");

  result = omni_event_loop_stop(&loop);
  check(result.status == OMNI_EVENT_LOOP_ERR_STATE && result.state == OMNI_EVENT_LOOP_INERT,
        "stop before initialization is rejected explicitly");
  result = omni_event_loop_run(&loop);
  check(result.status == OMNI_EVENT_LOOP_ERR_STATE && result.state == OMNI_EVENT_LOOP_INERT,
        "run on inert storage is rejected explicitly");
  result = omni_event_loop_init(&loop, NULL);
  check(result.status == OMNI_EVENT_LOOP_ERR_INVALID && result.sys_errno == EINVAL &&
            result.state == OMNI_EVENT_LOOP_INERT,
        "NULL configuration is rejected without leaving a live loop");

  check(runtime_fixture_init(&fixture), "event-loop invalid-input runtime initializes");
  config.runtime = &fixture.runtime;
  config.timeout_ms = -1;
  result = omni_event_loop_init(&loop, &config);
  check(result.status == OMNI_EVENT_LOOP_ERR_INVALID && result.sys_errno == EINVAL,
        "negative timeout is rejected");
  config.timeout_ms = OMNI_EVENT_LOOP_MAX_TIMEOUT_MS + 1;
  result = omni_event_loop_init(&loop, &config);
  check(result.status == OMNI_EVENT_LOOP_ERR_INVALID && result.sys_errno == EINVAL,
        "timeout above the documented maximum is rejected");
  config.timeout_ms = OMNI_EVENT_LOOP_DEFAULT_TIMEOUT_MS;
  result = omni_event_loop_init(&loop, &config);
  check(result.status == OMNI_EVENT_LOOP_OK && result.state == OMNI_EVENT_LOOP_INITIALIZED,
        "valid initialization borrows an initialized runtime without starting it");
  result = omni_event_loop_run(&loop);
  check(result.status == OMNI_EVENT_LOOP_ERR_RUNTIME &&
            omni_event_loop_state(&loop) == OMNI_EVENT_LOOP_INITIALIZED,
        "run requires the runtime lifecycle to be RUNNING");
  omni_event_loop_destroy(&loop);
  check(omni_event_loop_state(&loop) == OMNI_EVENT_LOOP_INERT,
        "destroy releases only the event-loop borrow and returns it to inert");
  runtime_fixture_destroy(&fixture);
}

static void test_empty_reactor_and_stop_safety(void) {
  struct runtime_fixture fixture = { 0 };
  struct omni_event_loop loop = { 0 };
  struct omni_event_loop_result result;

  check(runtime_fixture_init(&fixture) && runtime_fixture_start(&fixture),
        "empty-reactor runtime starts");
  check(event_loop_init_for(&loop, &fixture.runtime, 0),
        "zero timeout is accepted as a bounded probe");
  result = omni_event_loop_stop(&loop);
  check(result.status == OMNI_EVENT_LOOP_ERR_STATE &&
            omni_event_loop_state(&loop) == OMNI_EVENT_LOOP_INITIALIZED,
        "stop before run remains an explicit state error");
  result = omni_event_loop_run(&loop);
  check(result.status == OMNI_EVENT_LOOP_OK && result.state == OMNI_EVENT_LOOP_STOPPED,
        "run with an empty reactor exits cleanly instead of spinning");
  check(omni_event_loop_iterations(&loop) == 1u &&
            omni_event_loop_events_processed(&loop) == 0u &&
            omni_event_loop_last_status(&loop) == OMNI_REACTOR_OK,
        "empty execution accounts one bounded zero-event step");
  result = omni_event_loop_stop(&loop);
  check(result.status == OMNI_EVENT_LOOP_OK && result.state == OMNI_EVENT_LOOP_STOPPED,
        "stop after clean exit is idempotent");
  result = omni_event_loop_stop(&loop);
  check(result.status == OMNI_EVENT_LOOP_OK && result.state == OMNI_EVENT_LOOP_STOPPED,
        "repeated stop after clean exit is safe");
  result = omni_event_loop_run(&loop);
  check(result.status == OMNI_EVENT_LOOP_ERR_STATE,
        "repeated run requires a fresh initialization");
  omni_event_loop_destroy(&loop);
  runtime_fixture_destroy(&fixture);
}

static void test_ready_dispatch_and_callback_stop(void) {
  struct runtime_fixture fixture = { 0 };
  struct omni_event_loop loop = { 0 };
  struct stop_context stop = { 0 };
  unsigned char marker = 0x24u;
  int pipe_fds[2] = { -1, -1 };
  int read_fd = -1;
  struct omni_reactor_result reactor_result;
  struct omni_event_loop_result loop_result;

  check(runtime_fixture_init(&fixture) && runtime_fixture_start(&fixture),
        "readiness runtime starts");
  if (pipe(pipe_fds) != 0) {
    check(false, "readiness pipe creates");
    runtime_fixture_destroy(&fixture);
    return;
  }
  read_fd = pipe_fds[0];
  stop.loop = &loop;
  reactor_result = omni_reactor_add(&fixture.reactor, read_fd, 0x2402u,
                                    OMNI_POLLER_INTEREST_READ, stop_loop_callback, &stop);
  check(reactor_result.status == OMNI_REACTOR_OK,
        "event-loop readiness source registers through the existing reactor");
  if (reactor_result.status != OMNI_REACTOR_OK) {
    (void)close(pipe_fds[0]);
    (void)close(pipe_fds[1]);
    runtime_fixture_destroy(&fixture);
    return;
  }
  check(write(pipe_fds[1], &marker, sizeof(marker)) == (ssize_t)sizeof(marker),
        "test readiness source becomes readable");
  (void)close(pipe_fds[1]);
  pipe_fds[1] = -1;

  check(event_loop_init_for(&loop, &fixture.runtime, OMNI_EVENT_LOOP_DEFAULT_TIMEOUT_MS),
        "event loop accepts the documented default timeout");
  loop_result = omni_event_loop_run(&loop);
  check(loop_result.status == OMNI_EVENT_LOOP_OK &&
            loop_result.reactor_status == OMNI_REACTOR_OK &&
            omni_event_loop_state(&loop) == OMNI_EVENT_LOOP_STOPPED,
        "run returns cleanly after a callback requests stop");
  check(stop.calls == 1u && stop.stop_status == OMNI_EVENT_LOOP_OK &&
            stop.token == 0x2402u && (stop.events & OMNI_POLLER_READY_READ) != 0u,
        "one ready event reaches the callback with token and readiness mask");
  check(omni_event_loop_iterations(&loop) == 1u &&
            omni_event_loop_events_processed(&loop) == 1u,
        "ready execution accounts one iteration and one processed event");
  check(omni_reactor_count(&fixture.reactor) == 1u && fd_is_open(read_fd),
        "event-loop teardown does not remove or close the borrowed readiness descriptor");

  omni_event_loop_destroy(&loop);
  reactor_result = omni_reactor_remove(&fixture.reactor, 0x2402u);
  check(reactor_result.status == OMNI_REACTOR_OK,
        "the reactor owner removes its registration explicitly");
  (void)close(read_fd);
  runtime_fixture_destroy(&fixture);
}

static void test_interrupted_wait_propagates(void) {
  struct runtime_fixture fixture = { 0 };
  struct omni_event_loop loop = { 0 };
  struct sigaction action = { 0 };
  struct sigaction old_action = { 0 };
  struct itimerval timer = { { 0, 0 }, { 0, 10000 } };
  struct itimerval disabled_timer = { { 0, 0 }, { 0, 0 } };
  struct omni_reactor_result reactor_result;
  struct omni_event_loop_result loop_result;
  int pipe_fds[2] = { -1, -1 };
  bool handler_ok = false;
  bool timer_ok = false;

  check(runtime_fixture_init(&fixture) && runtime_fixture_start(&fixture),
        "interruption runtime starts");
  if (pipe(pipe_fds) != 0) {
    check(false, "interruption pipe creates");
    runtime_fixture_destroy(&fixture);
    return;
  }
  reactor_result = omni_reactor_add(&fixture.reactor, pipe_fds[0], 0x2403u,
                                    OMNI_POLLER_INTEREST_READ, no_op_callback, NULL);
  check(reactor_result.status == OMNI_REACTOR_OK,
        "interruption source registers without readiness");
  if (reactor_result.status != OMNI_REACTOR_OK) {
    (void)close(pipe_fds[0]);
    (void)close(pipe_fds[1]);
    runtime_fixture_destroy(&fixture);
    return;
  }
  action.sa_handler = alarm_callback;
  sigemptyset(&action.sa_mask);
  alarm_count = 0;
  handler_ok = sigaction(SIGALRM, &action, &old_action) == 0;
  timer_ok = handler_ok && setitimer(ITIMER_REAL, &timer, NULL) == 0;
  check(handler_ok && timer_ok,
        "test interruption timer installs");
  if (!timer_ok) {
    /* The check above can fail only in an unusual host; still restore state. */
    (void)setitimer(ITIMER_REAL, &disabled_timer, NULL);
    if (handler_ok) {
      (void)sigaction(SIGALRM, &old_action, NULL);
    }
    (void)omni_reactor_remove(&fixture.reactor, 0x2403u);
    (void)close(pipe_fds[0]);
    (void)close(pipe_fds[1]);
    runtime_fixture_destroy(&fixture);
    return;
  }

  check(event_loop_init_for(&loop, &fixture.runtime, OMNI_EVENT_LOOP_MAX_TIMEOUT_MS),
        "interruption loop initializes with a finite maximum-bound timeout");
  loop_result = omni_event_loop_run(&loop);
  (void)setitimer(ITIMER_REAL, &disabled_timer, NULL);
  (void)sigaction(SIGALRM, &old_action, NULL);
  check(loop_result.status == OMNI_EVENT_LOOP_ERR_INTERRUPTED &&
            loop_result.reactor_status == OMNI_REACTOR_ERR_INTERRUPTED &&
            omni_event_loop_state(&loop) == OMNI_EVENT_LOOP_STOPPED,
        "an interrupted reactor wait is returned without an internal retry");
  check(alarm_count == 1 && omni_event_loop_iterations(&loop) == 1u &&
            omni_event_loop_events_processed(&loop) == 0u,
        "interrupted execution accounts one wait and no callback");
  omni_event_loop_destroy(&loop);
  (void)omni_reactor_remove(&fixture.reactor, 0x2403u);
  (void)close(pipe_fds[0]);
  (void)close(pipe_fds[1]);
  runtime_fixture_destroy(&fixture);
}

static void test_reactor_failure_propagates(void) {
  struct runtime_fixture fixture = { 0 };
  struct omni_event_loop loop = { 0 };
  struct omni_event_loop_result loop_result;
  struct omni_reactor_result reactor_result;
  int pipe_fds[2] = { -1, -1 };

  check(runtime_fixture_init(&fixture) && runtime_fixture_start(&fixture),
        "reactor-failure runtime starts");
  if (pipe(pipe_fds) != 0) {
    check(false, "reactor-failure pipe creates");
    runtime_fixture_destroy(&fixture);
    return;
  }
  reactor_result = omni_reactor_add(&fixture.reactor, pipe_fds[0], 0x2404u,
                                    OMNI_POLLER_INTEREST_READ, no_op_callback, NULL);
  check(reactor_result.status == OMNI_REACTOR_OK,
        "reactor-failure source registers before fault injection");
  if (reactor_result.status != OMNI_REACTOR_OK) {
    (void)close(pipe_fds[0]);
    (void)close(pipe_fds[1]);
    runtime_fixture_destroy(&fixture);
    return;
  }
  /* The test invalidates an existing dependency to exercise explicit error
   * propagation; production code never mutates this borrowed state. */
  fixture.runtime.poller.live = false;
  check(event_loop_init_for(&loop, &fixture.runtime, 10),
        "event loop initializes before a reactor dependency failure");
  loop_result = omni_event_loop_run(&loop);
  check(loop_result.status == OMNI_EVENT_LOOP_ERR_REACTOR &&
            loop_result.reactor_status == OMNI_REACTOR_ERR_INVALID &&
            omni_event_loop_state(&loop) == OMNI_EVENT_LOOP_STOPPED,
        "reactor failure is returned explicitly and stops the loop");
  check(omni_event_loop_iterations(&loop) == 1u &&
            omni_event_loop_events_processed(&loop) == 0u && fd_is_open(pipe_fds[0]),
        "reactor failure accounts one step without taking descriptor ownership");
  omni_event_loop_destroy(&loop);
  runtime_fixture_destroy(&fixture);
  (void)close(pipe_fds[0]);
  (void)close(pipe_fds[1]);
}

static void test_connection_and_descriptor_ownership(void) {
  struct runtime_fixture fixture = { 0 };
  struct connection_fixture connection = { 0 };
  struct omni_connection_reactor adapter = { 0 };
  struct omni_event_loop loop = { 0 };
  struct connection_stop_context stop = { 0 };
  struct omni_connection_reactor_result adapter_result;
  unsigned char marker = 0x25u;
  int connection_fd = -1;

  check(runtime_fixture_init(&fixture) && runtime_fixture_start(&fixture),
        "connection-ownership runtime starts");
  check(connection_fixture_init(&connection), "borrowed connection fixture initializes");
  if (!omni_connection_is_live(&connection.connection)) {
    runtime_fixture_destroy(&fixture);
    return;
  }
  connection_fd = omni_connection_fd(&connection.connection);
  stop.loop = &loop;
  adapter_result = omni_connection_reactor_init(&adapter, &fixture.reactor, &fixture.registry,
                                                connection_stop_callback, &stop);
  check(adapter_result.status == OMNI_CONNECTION_REACTOR_OK,
        "connection/reactor adapter initializes over the runtime-owned primitives");
  adapter_result = omni_connection_reactor_attach(&adapter, &connection.connection);
  check(adapter_result.status == OMNI_CONNECTION_REACTOR_OK,
        "open connection attaches through the existing adapter");
  if (adapter_result.status != OMNI_CONNECTION_REACTOR_OK) {
    omni_connection_reactor_destroy(&adapter);
    connection_fixture_destroy(&connection);
    runtime_fixture_destroy(&fixture);
    return;
  }
  check(write(connection.peer_fd, &marker, sizeof(marker)) == (ssize_t)sizeof(marker),
        "connection readiness source becomes readable");
  check(event_loop_init_for(&loop, &fixture.runtime, 100),
        "event loop binds without copying connection state");
  check(omni_event_loop_run(&loop).status == OMNI_EVENT_LOOP_OK,
        "event loop dispatches through the connection/reactor adapter");
  check(stop.calls == 1u && stop.seen_connection == &connection.connection &&
            stop.stop_status == OMNI_EVENT_LOOP_OK && stop.token != 0u &&
            (stop.events & OMNI_POLLER_READY_READ) != 0u,
        "adapter callback receives the live connection and stop request");
  check(omni_connection_state(&connection.connection) == OMNI_CONNECTION_OPEN &&
            omni_connection_fd(&connection.connection) == connection_fd &&
            fd_is_open(connection_fd) && omni_connection_reactor_count(&adapter) == 1u,
        "event loop does not destroy or close the attached connection");
  omni_event_loop_destroy(&loop);
  omni_connection_reactor_destroy(&adapter);
  check(omni_connection_state(&connection.connection) == OMNI_CONNECTION_OPEN &&
            fd_is_open(connection_fd),
        "adapter teardown also leaves connection ownership with the connection owner");
  connection_fixture_destroy(&connection);
  runtime_fixture_destroy(&fixture);
}

static void test_repeated_cycles(void) {
  struct runtime_fixture fixture = { 0 };
  bool cycles_ok = true;
  size_t cycle = 0u;

  check(runtime_fixture_init(&fixture) && runtime_fixture_start(&fixture),
        "stress runtime starts once for repeated loop cycles");
  for (cycle = 0u; cycle < OMNI_TEST_STRESS_CYCLES; ++cycle) {
    struct omni_event_loop loop = { 0 };

    if (!event_loop_init_for(&loop, &fixture.runtime, 0) ||
        omni_event_loop_run(&loop).status != OMNI_EVENT_LOOP_OK ||
        omni_event_loop_iterations(&loop) != 1u ||
        omni_event_loop_stop(&loop).status != OMNI_EVENT_LOOP_OK) {
      cycles_ok = false;
      break;
    }
    omni_event_loop_destroy(&loop);
    if (omni_event_loop_state(&loop) != OMNI_EVENT_LOOP_INERT) {
      cycles_ok = false;
      break;
    }
  }
  check(cycles_ok, "repeated init/run/stop/destroy cycles stay bounded");
  check(omni_reactor_count(&fixture.reactor) == 0u &&
            omni_poller_count(&fixture.runtime.poller) == 0u,
        "stress cycles leave reactor and poller registrations unchanged");
  runtime_fixture_destroy(&fixture);
}

int main(void) {
  printf("=== Starting bounded event-loop unit tests (Task 024) ===\n");

  test_inert_and_invalid_initialization();
  test_empty_reactor_and_stop_safety();
  test_ready_dispatch_and_callback_stop();
  test_interrupted_wait_propagates();
  test_reactor_failure_propagates();
  test_connection_and_descriptor_ownership();
  test_repeated_cycles();

  printf("Total checks: %zu\n", check_count);
  printf("Failures: %zu\n", failure_count);
  return failure_count == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
