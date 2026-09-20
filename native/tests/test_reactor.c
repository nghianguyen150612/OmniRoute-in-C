/*
 * OmniRoute native backend — bounded reactor tests (Task 021).
 *
 * The suite uses local pipes only. Pipe writes/readbacks are test stimulus
 * and assertions that the reactor reports readiness without processing
 * payloads; the production reactor itself contains no I/O calls. Each test
 * uses caller-owned fixed arrays and closes every descriptor it creates.
 */

#define _GNU_SOURCE /* setitimer/sigaction visibility under strict C11 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "omniroute/poller.h"
#include "omniroute/reactor.h"

static size_t check_count = 0u;
static size_t failure_count = 0u;

struct callback_state {
  size_t calls;
  uint64_t token;
  uint32_t events;
  void *context;
};

static void check(bool condition, const char *message) {
  check_count += 1u;
  if (condition) {
    return;
  }
  failure_count += 1u;
  fprintf(stderr, "FAIL: %s\n", message);
}

static void capture_callback(uint64_t token, uint32_t events, void *context) {
  struct callback_state *state = (struct callback_state *)context;

  if (state == NULL) {
    return;
  }
  state->calls += 1u;
  state->token = token;
  state->events = events;
  state->context = context;
}

static bool make_pipe(int fds[2]) {
  fds[0] = -1;
  fds[1] = -1;
  return pipe(fds) == 0;
}

static bool fd_is_open(int fd) {
  int result = fcntl(fd, F_GETFD);

  return result != -1 || errno != EBADF;
}

static void on_alarm(int signal_number) {
  (void)signal_number;
}

static bool init_poller(struct omni_poller *poller,
                        struct pollfd *poll_slots,
                        uint64_t *poll_tokens,
                        size_t capacity) {
  struct omni_poller_result result =
      omni_poller_init_borrowed(poller, poll_slots, poll_tokens, capacity);

  return result.status == OMNI_POLLER_OK;
}

static void test_lifecycle(void) {
  struct omni_poller poller = { 0 };
  struct omni_reactor reactor = { 0 };
  struct pollfd poll_slots[2];
  uint64_t poll_tokens[2];
  struct omni_reactor_registration registrations[2];
  struct omni_poller_event events[2];
  struct omni_reactor_result result;

  omni_reactor_make_inert(&reactor);
  check(init_poller(&poller, poll_slots, poll_tokens, 2u),
        "lifecycle poller init succeeds");

  result = omni_reactor_init(&reactor, &poller, registrations, events, 2u);
  check(result.status == OMNI_REACTOR_OK && result.sys_errno == 0 && result.count == 0u,
        "reactor init succeeds");
  check(omni_reactor_capacity(&reactor) == 2u, "reactor capacity is fixed");
  check(omni_reactor_count(&reactor) == 0u, "reactor starts empty");

  omni_reactor_destroy(&reactor);
  check(omni_reactor_capacity(&reactor) == 0u && omni_reactor_count(&reactor) == 0u,
        "destroy leaves reactor inert");
  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&poller);
}

static void test_invalid_state_and_inputs(void) {
  struct omni_poller inert_poller = { 0 };
  struct omni_reactor reactor = { 0 };
  struct omni_reactor_registration registrations[1];
  struct omni_poller_event events[1];
  struct pollfd poll_slots[1];
  uint64_t poll_tokens[1];
  int fds[2];
  struct omni_reactor_result result;

  omni_reactor_make_inert(&reactor);
  result = omni_reactor_step(&reactor, 0);
  check(result.status == OMNI_REACTOR_ERR_INVALID, "step on inert reactor is invalid");
  result = omni_reactor_init(&reactor, &inert_poller, registrations, events, 1u);
  check(result.status == OMNI_REACTOR_ERR_INVALID, "init rejects inert poller");

  check(init_poller(&inert_poller, poll_slots, poll_tokens, 1u),
        "input-probe poller init succeeds");
  result = omni_reactor_init(&reactor, &inert_poller, NULL, events, 1u);
  check(result.status == OMNI_REACTOR_ERR_INVALID, "init rejects NULL registrations");
  result = omni_reactor_init(&reactor, &inert_poller, registrations, NULL, 1u);
  check(result.status == OMNI_REACTOR_ERR_INVALID, "init rejects NULL event storage");
  result = omni_reactor_init(&reactor, &inert_poller, registrations, events, 0u);
  check(result.status == OMNI_REACTOR_ERR_INVALID, "init rejects zero capacity");

  result = omni_reactor_init(&reactor, &inert_poller, registrations, events, 1u);
  check(result.status == OMNI_REACTOR_OK, "input-probe reactor init succeeds");
  check(make_pipe(fds), "input-probe pipe opens");
  result = omni_reactor_add(&reactor, fds[0], 1u, OMNI_POLLER_INTEREST_READ, NULL, NULL);
  check(result.status == OMNI_REACTOR_ERR_INVALID && result.sys_errno == EINVAL,
        "add rejects NULL callback");
  result = omni_reactor_add(&reactor, -1, 1u, OMNI_POLLER_INTEREST_READ,
                            capture_callback, NULL);
  check(result.status == OMNI_REACTOR_ERR_INVALID, "add rejects negative descriptor");
  result = omni_reactor_add(&reactor, fds[0], 1u, 0u, capture_callback, NULL);
  check(result.status == OMNI_REACTOR_ERR_INVALID, "add rejects empty interest mask");
  result = omni_reactor_step(&reactor, -1);
  check(result.status == OMNI_REACTOR_ERR_INVALID, "step rejects negative timeout");
  result = omni_reactor_step(&reactor, (int64_t)INT_MAX + (int64_t)1);
  check(result.status == OMNI_REACTOR_ERR_INVALID, "step rejects timeout overflow");

  close(fds[0]);
  close(fds[1]);
  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&inert_poller);
}

static void test_registration_and_duplicates(void) {
  struct omni_poller poller = { 0 };
  struct omni_reactor reactor = { 0 };
  struct pollfd poll_slots[2];
  uint64_t poll_tokens[2];
  struct omni_reactor_registration registrations[2];
  struct omni_poller_event events[2];
  struct callback_state first = { 0 };
  struct callback_state second = { 0 };
  int first_pipe[2];
  int second_pipe[2];
  struct omni_reactor_result result;

  omni_reactor_make_inert(&reactor);
  check(init_poller(&poller, poll_slots, poll_tokens, 2u),
        "registration poller init succeeds");
  result = omni_reactor_init(&reactor, &poller, registrations, events, 2u);
  check(result.status == OMNI_REACTOR_OK, "registration reactor init succeeds");
  check(make_pipe(first_pipe) && make_pipe(second_pipe), "registration pipes open");

  result = omni_reactor_add(&reactor, first_pipe[0], 0x10u, OMNI_POLLER_INTEREST_READ,
                            capture_callback, &first);
  check(result.status == OMNI_REACTOR_OK && omni_reactor_count(&reactor) == 1u,
        "first event source registers");
  result = omni_reactor_add(&reactor, second_pipe[0], 0x20u, OMNI_POLLER_INTEREST_READ,
                            capture_callback, &second);
  check(result.status == OMNI_REACTOR_OK && omni_reactor_count(&reactor) == 2u,
        "second event source registers");

  result = omni_reactor_add(&reactor, second_pipe[1], 0x10u, OMNI_POLLER_INTEREST_WRITE,
                            capture_callback, &second);
  check(result.status == OMNI_REACTOR_ERR_DUPLICATE && result.sys_errno == EEXIST,
        "duplicate token is rejected");
  result = omni_reactor_add(&reactor, first_pipe[0], 0x30u, OMNI_POLLER_INTEREST_READ,
                            capture_callback, &second);
  check(result.status == OMNI_REACTOR_ERR_DUPLICATE && result.sys_errno == EEXIST,
        "duplicate descriptor is rejected");
  result = omni_reactor_add(&reactor, second_pipe[1], 0x30u, OMNI_POLLER_INTEREST_WRITE,
                            capture_callback, &second);
  check(result.status == OMNI_REACTOR_ERR_FULL && result.sys_errno == ENOSPC,
        "full fixed capacity is rejected");
  check(omni_reactor_count(&reactor) == 2u && omni_poller_count(&poller) == 2u,
        "failed registrations leave state unchanged");

  result = omni_reactor_remove(&reactor, 0x10u);
  check(result.status == OMNI_REACTOR_OK && omni_reactor_count(&reactor) == 1u &&
            omni_poller_count(&poller) == 1u,
        "known registration removes from reactor and poller");
  check(registrations[1].callback == NULL && registrations[1].context == NULL &&
            registrations[1].fd == -1,
        "removed registration clears callback context and descriptor");
  result = omni_reactor_remove(&reactor, 0x10u);
  check(result.status == OMNI_REACTOR_ERR_NOT_FOUND && result.sys_errno == ENOENT,
        "second removal reports missing entry");
  result = omni_reactor_remove(&reactor, 0x99u);
  check(result.status == OMNI_REACTOR_ERR_NOT_FOUND, "unknown removal reports missing entry");

  result = omni_reactor_remove(&reactor, 0x20u);
  check(result.status == OMNI_REACTOR_OK && omni_reactor_count(&reactor) == 0u,
        "remaining registration removes");
  close(first_pipe[0]);
  close(first_pipe[1]);
  close(second_pipe[0]);
  close(second_pipe[1]);
  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&poller);
}

static void test_empty_and_timeout_steps(void) {
  struct omni_poller poller = { 0 };
  struct omni_reactor reactor = { 0 };
  struct pollfd poll_slots[1];
  uint64_t poll_tokens[1];
  struct omni_reactor_registration registrations[1];
  struct omni_poller_event events[1];
  int fds[2];
  struct omni_reactor_result result;

  omni_reactor_make_inert(&reactor);
  check(init_poller(&poller, poll_slots, poll_tokens, 1u),
        "timeout poller init succeeds");
  result = omni_reactor_init(&reactor, &poller, registrations, events, 1u);
  check(result.status == OMNI_REACTOR_OK, "timeout reactor init succeeds");

  result = omni_reactor_step(&reactor, 0);
  check(result.status == OMNI_REACTOR_OK && result.count == 0u,
        "empty reactor zero-time step returns immediately");
  result = omni_reactor_step(&reactor, 25);
  check(result.status == OMNI_REACTOR_OK && result.count == 0u,
        "empty reactor positive-time step returns immediately");

  check(make_pipe(fds), "timeout pipe opens");
  result = omni_reactor_add(&reactor, fds[0], 0x55u, OMNI_POLLER_INTEREST_READ,
                            capture_callback, NULL);
  check(result.status == OMNI_REACTOR_OK, "timeout source registers");
  result = omni_reactor_step(&reactor, 25);
  check(result.status == OMNI_REACTOR_OK && result.count == 0u,
        "positive timeout with no readiness returns without callback");
  result = omni_reactor_step(&reactor, 0);
  check(result.status == OMNI_REACTOR_OK && result.count == 0u,
        "zero timeout with no readiness returns without callback");

  close(fds[0]);
  close(fds[1]);
  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&poller);
}

static void test_read_write_dispatch(void) {
  struct omni_poller poller = { 0 };
  struct omni_reactor reactor = { 0 };
  struct pollfd poll_slots[2];
  uint64_t poll_tokens[2];
  struct omni_reactor_registration registrations[2];
  struct omni_poller_event events[2];
  struct callback_state readable = { 0 };
  struct callback_state writable = { 0 };
  int pipe_fds[2];
  unsigned char byte = 0xA5u;
  unsigned char received = 0u;
  struct omni_reactor_result result;

  omni_reactor_make_inert(&reactor);
  check(init_poller(&poller, poll_slots, poll_tokens, 2u),
        "dispatch poller init succeeds");
  result = omni_reactor_init(&reactor, &poller, registrations, events, 2u);
  check(result.status == OMNI_REACTOR_OK, "dispatch reactor init succeeds");
  check(make_pipe(pipe_fds), "dispatch pipe opens");

  result = omni_reactor_add(&reactor, pipe_fds[0], 0x101u, OMNI_POLLER_INTEREST_READ,
                            capture_callback, &readable);
  check(result.status == OMNI_REACTOR_OK, "read source registers");
  result = omni_reactor_add(&reactor, pipe_fds[1], 0x202u, OMNI_POLLER_INTEREST_WRITE,
                            capture_callback, &writable);
  check(result.status == OMNI_REACTOR_OK, "write source registers");

  check(write(pipe_fds[1], &byte, sizeof(byte)) == (ssize_t)sizeof(byte),
        "dispatch stimulus writes one byte");
  result = omni_reactor_step(&reactor, 0);
  check(result.status == OMNI_REACTOR_OK && result.count == 2u,
        "one step dispatches every ready source");
  check(readable.calls == 1u && readable.token == 0x101u &&
            (readable.events & OMNI_POLLER_READY_READ) != 0u &&
            readable.context == &readable,
        "read callback receives token mask and context");
  check(writable.calls == 1u && writable.token == 0x202u &&
            (writable.events & OMNI_POLLER_READY_WRITE) != 0u &&
            writable.context == &writable,
        "write callback receives token mask and context");
  check(read(pipe_fds[0], &received, sizeof(received)) == (ssize_t)sizeof(received) &&
            received == byte,
        "reactor reports readiness without reading payload");
  check(fd_is_open(pipe_fds[0]) && fd_is_open(pipe_fds[1]),
        "reactor does not close event-source descriptors");

  result = omni_reactor_remove(&reactor, 0x101u);
  check(result.status == OMNI_REACTOR_OK, "read source removes after dispatch");
  result = omni_reactor_remove(&reactor, 0x202u);
  check(result.status == OMNI_REACTOR_OK, "write source removes after dispatch");
  close(pipe_fds[0]);
  close(pipe_fds[1]);
  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&poller);
}

static void test_error_dispatch(void) {
  struct omni_poller poller = { 0 };
  struct omni_reactor reactor = { 0 };
  struct pollfd poll_slots[1];
  uint64_t poll_tokens[1];
  struct omni_reactor_registration registrations[1];
  struct omni_poller_event events[1];
  struct callback_state error_state = { 0 };
  int pipe_fds[2];
  struct omni_reactor_result result;

  omni_reactor_make_inert(&reactor);
  check(init_poller(&poller, poll_slots, poll_tokens, 1u),
        "error poller init succeeds");
  result = omni_reactor_init(&reactor, &poller, registrations, events, 1u);
  check(result.status == OMNI_REACTOR_OK, "error reactor init succeeds");
  check(make_pipe(pipe_fds), "error pipe opens");
  close(pipe_fds[0]);

  result = omni_reactor_add(&reactor, pipe_fds[1], 0xE01u, OMNI_POLLER_INTEREST_WRITE,
                            capture_callback, &error_state);
  check(result.status == OMNI_REACTOR_OK, "error source registers");
  result = omni_reactor_step(&reactor, 0);
  check(result.status == OMNI_REACTOR_OK && result.count == 1u,
        "error step dispatches one event");
  check(error_state.calls == 1u && error_state.token == 0xE01u &&
            (error_state.events & OMNI_POLLER_READY_ERROR) != 0u,
        "error readiness reaches callback");

  result = omni_reactor_remove(&reactor, 0xE01u);
  check(result.status == OMNI_REACTOR_OK, "error source removes");
  close(pipe_fds[1]);
  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&poller);
}

static void test_interrupted_step(void) {
  struct omni_poller poller = { 0 };
  struct omni_reactor reactor = { 0 };
  struct pollfd poll_slots[1];
  uint64_t poll_tokens[1];
  struct omni_reactor_registration registrations[1];
  struct omni_poller_event events[1];
  struct callback_state state = { 0 };
  struct sigaction action;
  struct sigaction old_action;
  struct itimerval timer;
  int pipe_fds[2];
  struct omni_reactor_result result;
  bool handler_installed = false;

  omni_reactor_make_inert(&reactor);
  check(init_poller(&poller, poll_slots, poll_tokens, 1u),
        "interrupt poller init succeeds");
  result = omni_reactor_init(&reactor, &poller, registrations, events, 1u);
  check(result.status == OMNI_REACTOR_OK, "interrupt reactor init succeeds");
  check(make_pipe(pipe_fds), "interrupt pipe opens");
  result = omni_reactor_add(&reactor, pipe_fds[0], 0x1Eu, OMNI_POLLER_INTEREST_READ,
                            capture_callback, &state);
  check(result.status == OMNI_REACTOR_OK, "interrupt source registers");

  memset(&action, 0, sizeof(action));
  action.sa_handler = on_alarm;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  if (sigaction(SIGALRM, &action, &old_action) == 0) {
    handler_installed = true;
  }
  check(handler_installed, "interrupt signal handler installs");
  if (handler_installed) {
    memset(&timer, 0, sizeof(timer));
    timer.it_value.tv_usec = 20000;
    check(setitimer(ITIMER_REAL, &timer, NULL) == 0, "interrupt timer starts");
    result = omni_reactor_step(&reactor, 1000);
    memset(&timer, 0, sizeof(timer));
    (void)setitimer(ITIMER_REAL, &timer, NULL);
    (void)sigaction(SIGALRM, &old_action, NULL);
    check(result.status == OMNI_REACTOR_ERR_INTERRUPTED && result.count == 0u &&
              result.sys_errno == EINTR,
          "interrupted step returns bounded interrupted status");
    check(state.calls == 0u && omni_reactor_count(&reactor) == 1u &&
              omni_poller_count(&poller) == 1u,
          "interrupted step preserves registrations and dispatches nothing");
  }

  result = omni_reactor_remove(&reactor, 0x1Eu);
  check(result.status == OMNI_REACTOR_OK, "interrupt source removes");
  close(pipe_fds[0]);
  close(pipe_fds[1]);
  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&poller);
}

static void test_repeated_add_remove_and_steps(void) {
  struct omni_poller poller = { 0 };
  struct omni_reactor reactor = { 0 };
  struct pollfd poll_slots[2];
  uint64_t poll_tokens[2];
  struct omni_reactor_registration registrations[2];
  struct omni_poller_event events[2];
  struct callback_state state = { 0 };
  int pipe_fds[2];
  struct omni_reactor_result result;
  bool cycles_ok = true;
  bool steps_ok = true;
  uint32_t cycle = 0u;
  uint32_t step = 0u;

  omni_reactor_make_inert(&reactor);
  check(init_poller(&poller, poll_slots, poll_tokens, 2u),
        "stress poller init succeeds");
  result = omni_reactor_init(&reactor, &poller, registrations, events, 2u);
  check(result.status == OMNI_REACTOR_OK, "stress reactor init succeeds");
  check(make_pipe(pipe_fds), "stress pipe opens");

  for (cycle = 0u; cycle < 1000u; ++cycle) {
    uint64_t token = (uint64_t)cycle + 1u;

    result = omni_reactor_add(&reactor, pipe_fds[0], token, OMNI_POLLER_INTEREST_READ,
                              capture_callback, &state);
    if (result.status != OMNI_REACTOR_OK || omni_reactor_count(&reactor) != 1u) {
      cycles_ok = false;
      break;
    }
    result = omni_reactor_remove(&reactor, token);
    if (result.status != OMNI_REACTOR_OK || omni_reactor_count(&reactor) != 0u ||
        omni_poller_count(&poller) != 0u) {
      cycles_ok = false;
      break;
    }
  }
  check(cycles_ok, "repeated add/remove stays bounded and reusable");

  result = omni_reactor_add(&reactor, pipe_fds[0], 0x777u, OMNI_POLLER_INTEREST_READ,
                            capture_callback, &state);
  check(result.status == OMNI_REACTOR_OK, "repeated-step source registers");
  for (step = 0u; step < 1000u; ++step) {
    result = omni_reactor_step(&reactor, 0);
    if (result.status != OMNI_REACTOR_OK || result.count != 0u) {
      steps_ok = false;
      break;
    }
  }
  check(steps_ok, "repeated zero-time steps remain bounded");
  check(write(pipe_fds[1], "r", (size_t)1) == (ssize_t)1,
        "repeated-step stimulus writes one byte");
  result = omni_reactor_step(&reactor, 0);
  check(result.status == OMNI_REACTOR_OK && result.count == 1u && state.calls == 1u,
        "repeated-step reactor still dispatches after stress");
  result = omni_reactor_remove(&reactor, 0x777u);
  check(result.status == OMNI_REACTOR_OK, "repeated-step source removes");

  close(pipe_fds[0]);
  close(pipe_fds[1]);
  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&poller);
}

static void test_destroy_unregisters_without_closing(void) {
  struct omni_poller poller = { 0 };
  struct omni_reactor reactor = { 0 };
  struct pollfd poll_slots[1];
  uint64_t poll_tokens[1];
  struct omni_reactor_registration registrations[1];
  struct omni_poller_event events[1];
  struct callback_state state = { 0 };
  int pipe_fds[2];
  struct omni_reactor_result result;

  omni_reactor_make_inert(&reactor);
  check(init_poller(&poller, poll_slots, poll_tokens, 1u),
        "destroy poller init succeeds");
  result = omni_reactor_init(&reactor, &poller, registrations, events, 1u);
  check(result.status == OMNI_REACTOR_OK, "destroy reactor init succeeds");
  check(make_pipe(pipe_fds), "destroy pipe opens");
  result = omni_reactor_add(&reactor, pipe_fds[0], 0xD01u, OMNI_POLLER_INTEREST_READ,
                            capture_callback, &state);
  check(result.status == OMNI_REACTOR_OK, "destroy source registers");

  omni_reactor_destroy(&reactor);
  check(omni_poller_count(&poller) == 0u && fd_is_open(pipe_fds[0]) &&
            registrations[0].callback == NULL && registrations[0].context == NULL,
        "destroy unregisters without closing or retaining callback state");
  close(pipe_fds[0]);
  close(pipe_fds[1]);
  omni_poller_destroy(&poller);
}

int main(void) {
  printf("=== Starting reactor unit tests (Task 021) ===\n");

  test_lifecycle();
  test_invalid_state_and_inputs();
  test_registration_and_duplicates();
  test_empty_and_timeout_steps();
  test_read_write_dispatch();
  test_error_dispatch();
  test_interrupted_step();
  test_repeated_add_remove_and_steps();
  test_destroy_unregisters_without_closing();

  printf("Total checks: %zu\n", check_count);
  printf("Failures: %zu\n", failure_count);
  return failure_count == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
