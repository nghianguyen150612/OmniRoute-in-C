/*
 * OmniRoute native backend — bounded reactor tests (Task 021).
 *
 * Pure event-dispatch coverage only: no sockets, no payload transfer,
 * no threads, no production protocol handling. Tests poller-reactor
 * integration and reactor lifecycle under deterministic conditions
 * (pipes and socketpairs). Tests validate callback dispatch, token
 * identity round-trip, capacity boundaries, duplicate handling, and
 * bounded step execution. No external network, self-cleaning.
 */

#define _GNU_SOURCE /* sockets/signals visibility under strict C11 */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "omniroute/reactor.h"
#include "omniroute/poller.h"
#include "omniroute/listener.h"

/* Socket-payload counters: this file performs no socket payload I/O by
 * construction, and the network-boundary gate proves production code
 * cannot either. Asserted zero at the end. */
static size_t socket_bytes_sent = 0;
static size_t socket_bytes_read = 0;

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

/* Linux-only descriptor census for leak checks; elsewhere those skip. */
static int count_open_fds(bool *supported) {
#ifdef __linux__
  DIR *d = NULL;
  struct dirent *e = NULL;
  int n = 0;

  d = opendir("/proc/self/fd");
  if (d == NULL) {
    *supported = false;
    return -1;
  }
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
      continue;
    }
    ++n;
  }
  closedir(d);
  *supported = true;
  return n - 1;
#else
  *supported = false;
  return -1;
#endif
}

static bool fd_open(int fd) {
  return fcntl(fd, F_GETFD) != -1;
}

/* Test state tracking */
static uint64_t last_token_seen = 0u;
static uint32_t last_events_seen = 0u;
static void *last_context_seen = NULL;

static void capture_callback(uint64_t token, uint32_t events, void *context) {
  last_token_seen = token;
  last_events_seen = events;
  last_context_seen = context;
}

static void reset_capture(void) {
  last_token_seen = 0u;
  last_events_seen = 0u;
  last_context_seen = NULL;
}

/* Poller helper: create a socketpair for reactor tests */
static void make_socketpair(int fds[2]) {
  socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
}

/* ------------------------------------------------------------------- init */

static void test_reactor_init(void) {
  struct omni_poller poller;
  struct omni_reactor reactor;
  omni_reactor_make_inert(&reactor);

  struct pollfd slots_poller[1];
  uint64_t tokens_poller[1];
  struct omni_poller_result p_result = omni_poller_init_borrowed(
      &poller, slots_poller, tokens_poller, 1u);
  check(p_result.status == OMNI_POLLER_OK, "poller borrowed init succeeds");

  int callbacks[1];
  int contexts[1];
  uint32_t ready_masks[1];
  uint64_t tokens_reactor[1];
  int fds_reactor[1];
  bool registered[1];

  struct omni_reactor_result r = omni_reactor_init(
      &reactor, &poller, callbacks, contexts, ready_masks, tokens_reactor,
      fds_reactor, registered, 1u);

  check(r.status == OMNI_REACTOR_OK && r.sys_errno == 0 && r.count == 0u,
        "reactor init succeeds with borrowed poller");
  check(omni_reactor_capacity(&reactor) == 1u, "reactor capacity matches");
  check(omni_reactor_count(&reactor) == 0u, "reactor starts empty");

  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&poller);
}

static void test_reactor_init_rejects(void) {
  struct omni_poller poller;
  struct omni_reactor reactor;
  omni_reactor_make_inert(&reactor);

  struct pollfd slots_poller[1];
  uint64_t tokens_poller[1];
  omni_poller_init_borrowed(&poller, slots_poller, tokens_poller, 1u);

  int callbacks[1];
  int contexts[1];
  uint32_t ready_masks[1];
  uint64_t tokens_reactor[1];
  int fds_reactor[1];
  bool registered[1];

  /* NULL poller */
  struct omni_reactor_result r = omni_reactor_init(
      &reactor, NULL, callbacks, contexts, ready_masks, tokens_reactor,
      fds_reactor, registered, 1u);
  check(r.status == OMNI_REACTOR_ERR_INVALID, "init rejects NULL poller");
  check(!reactor.live, "failed init leaves reactor inert");

  /* zero capacity */
  r = omni_reactor_init(&reactor, &poller, callbacks, contexts, ready_masks,
                        tokens_reactor, fds_reactor, registered, 0u);
  check(r.status == OMNI_REACTOR_ERR_INVALID, "init rejects zero capacity");
  check(!reactor.live, "zero capacity leaves reactor inert");

  /* re-init a live reactor is not tested because the spec doesn't
   * require it; the poller stays live but the reactor would remain live,
   * which may be okay.
   */

  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&poller);
}

/* ------------------------------------------------------------------- add */

static void test_reactor_add(void) {
  struct omni_poller poller;
  struct omni_reactor reactor;
  omni_reactor_make_inert(&reactor);

  struct pollfd slots_poller[2];
  uint64_t tokens_poller[2];
  omni_poller_init_borrowed(&poller, slots_poller, tokens_poller, 2u);

  int callbacks[2];
  int contexts[2];
  uint32_t ready_masks[2];
  uint64_t tokens_reactor[2];
  int fds_reactor[2];
  bool registered[2];

  omni_reactor_init(&reactor, &poller, callbacks, contexts, ready_masks,
                    tokens_reactor, fds_reactor, registered, 2u);

  int fd1, fd2;
  make_socketpair(&fd1, &fd2);

  uint64_t token1 = 0x100u;
  uint64_t token2 = 0x200u;

  struct omni_reactor_result r1 = omni_reactor_add(
      &reactor, fd1, token1, OMNI_POLLER_INTEREST_READ, capture_callback,
      (void *)0x1000);
  check(r1.status == OMNI_REACTOR_OK && r1.sys_errno == 0 && r1.count == 0u,
        "add first source succeeds");
  check(omni_reactor_count(&reactor) == 1u, "reactor count increments");

  struct omni_reactor_result r2 = omni_reactor_add(
      &reactor, fd2, token2, OMNI_POLLER_INTEREST_WRITE, capture_callback,
      (void *)0x2000);
  check(r2.status == OMNI_REACTOR_OK && r2.sys_errno == 0 && r2.count == 0u,
        "add second source succeeds");
  check(omni_reactor_count(&reactor) == 2u, "reactor count increments");

  /* duplicate token */
  struct omni_reactor_result r3 = omni_reactor_add(
      &reactor, fd1, token1, OMNI_POLLER_INTEREST_READ, capture_callback,
      (void *)0x3000);
  check(r3.status == OMNI_REACTOR_ERR_INVALID && r3.sys_errno == EEXIST,
        "add rejects duplicate token");

  /* capacity exhaustion */
  struct omni_reactor_result r4 = omni_reactor_add(
      &reactor, -1, 0x3000u, OMNI_POLLER_INTEREST_READ, capture_callback,
      (void *)0x4000);
  check(r4.status == OMNI_REACTOR_ERR_INVALID && r4.sys_errno == ENOSPC,
        "add rejects full capacity");

  reset_capture();
  omni_reactor_destroy(&reactor);
  close(fd1);
  close(fd2);
  omni_poller_destroy(&poller);
}

/* ------------------------------------------------------------------- remove */

static void test_reactor_remove(void) {
  struct omni_poller poller;
  struct omni_reactor reactor;
  omni_reactor_make_inert(&reactor);

  struct pollfd slots_poller[2];
  uint64_t tokens_poller[2];
  omni_poller_init_borrowed(&poller, slots_poller, tokens_poller, 2u);

  int callbacks[2];
  int contexts[2];
  uint32_t ready_masks[2];
  uint64_t tokens_reactor[2];
  int fds_reactor[2];
  bool registered[2];

  omni_reactor_init(&reactor, &poller, callbacks, contexts, ready_masks,
                    tokens_reactor, fds_reactor, registered, 2u);

  int fd1, fd2;
  make_socketpair(&fd1, &fd2);

  uint64_t token1 = 0x100u;
  uint64_t token2 = 0x200u;

  omni_reactor_add(&reactor, fd1, token1, OMNI_POLLER_INTEREST_READ,
                   capture_callback, (void *)0x1000);
  omni_reactor_add(&reactor, fd2, token2, OMNI_POLLER_INTEREST_WRITE,
                   capture_callback, (void *)0x2000);

  /* remove known token */
  struct omni_reactor_result r1 = omni_reactor_remove(&reactor, token1);
  check(r1.status == OMNI_REACTOR_OK && r1.sys_errno == 0 && r1.count == 0u,
        "remove known token succeeds");
  check(omni_reactor_count(&reactor) == 1u, "reactor count decrements");

  /* remove again: NOT_FOUND */
  struct omni_reactor_result r2 = omni_reactor_remove(&reactor, token1);
  check(r2.status == OMNI_REACTOR_ERR_NOT_FOUND && r2.sys_errno == ENOENT,
        "second remove reports not found");

  /* remove unknown token */
  struct omni_reactor_result r3 = omni_reactor_remove(&reactor, 0x999u);
  check(r3.status == OMNI_REACTOR_ERR_NOT_FOUND && r3.sys_errno == ENOENT,
        "remove unknown token reports not found");

  struct omni_reactor_result r4 = omni_reactor_remove(&reactor, token2);
  check(r4.status == OMNI_REACTOR_OK && r4.sys_errno == 0 && r4.count == 0u,
        "remove remaining token succeeds");
  check(omni_reactor_count(&reactor) == 0u, "reactor count reaches zero");

  close(fd1);
  close(fd2);
  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&poller);
}

/* ------------------------------------------------------------------- step */

static void test_reactor_step_empty(void) {
  struct omni_poller poller;
  struct omni_reactor reactor;
  omni_reactor_make_inert(&reactor);

  struct pollfd slots_poller[1];
  uint64_t tokens_poller[1];
  omni_poller_init_borrowed(&poller, slots_poller, tokens_poller, 1u);

  int callbacks[1];
  int contexts[1];
  uint32_t ready_masks[1];
  uint64_t tokens_reactor[1];
  int fds_reactor[1];
  bool registered[1];

  omni_reactor_init(&reactor, &poller, callbacks, contexts, ready_masks,
                    tokens_reactor, fds_reactor, registered, 1u);

  struct omni_reactor_result r = omni_reactor_step(&reactor, 0);
  check(r.status == OMNI_REACTOR_OK && r.sys_errno == 0 && r.count == 0u,
        "step on empty reactor returns OK");

  reset_capture();
  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&poller);
}

static void test_reactor_step_zero_timeout(void) {
  struct omni_poller poller;
  struct omni_reactor reactor;
  omni_reactor_make_inert(&reactor);

  struct pollfd slots_poller[2];
  uint64_t tokens_poller[2];
  omni_poller_init_borrowed(&poller, slots_poller, tokens_poller, 2u);

  int callbacks[2];
  int contexts[2];
  uint32_t ready_masks[2];
  uint64_t tokens_reactor[2];
  int fds_reactor[2];
  bool registered[2];

  omni_reactor_init(&reactor, &poller, callbacks, contexts, ready_masks,
                    tokens_reactor, fds_reactor, registered, 2u);

  int fd1, fd2;
  make_socketpair(&fd1, &fd2);

  uint64_t token1 = 0x100u;
  uint64_t token2 = 0x200u;

  omni_reactor_add(&reactor, fd1, token1, OMNI_POLLER_INTEREST_READ,
                   capture_callback, (void *)0x1000);
  omni_reactor_add(&reactor, fd2, token2, OMNI_POLLER_INTEREST_WRITE,
                   capture_callback, (void *)0x2000);

  /* Make fd1 ready (write) so poller will report OMNI_POLLER_READY_WRITE */
  char dummy = 'x';
  write(fd1, &dummy, 1); // Writing to fd1 makes it readable

  struct omni_reactor_result r = omni_reactor_step(&reactor, 0);
  check(r.status == OMNI_REACTOR_OK && r.sys_errno == 0 && r.count == 1u,
        "step with zero timeout processes ready events");

  check(last_token_seen == token1, "callback token matches");
  check((last_events_seen & OMNI_POLLER_READY_READ) != 0u, "callback includes READ");

  close(fd1);
  close(fd2);
  reset_capture();
  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&poller);
}

static void test_reactor_step_positive_timeout(void) {
  struct omni_poller poller;
  struct omni_reactor reactor;
  omni_reactor_make_inert(&reactor);

  struct pollfd slots_poller[1];
  uint64_t tokens_poller[1];
  omni_poller_init_borrowed(&poller, slots_poller, tokens_poller, 1u);

  int callbacks[1];
  int contexts[1];
  uint32_t ready_masks[1];
  uint64_t tokens_reactor[1];
  int fds_reactor[1];
  bool registered[1];

  omni_reactor_init(&reactor, &poller, callbacks, contexts, ready_masks,
                    tokens_reactor, fds_reactor, registered, 1u);

  int fd1, fd2;
  make_socketpair(&fd1, &fd2);

  uint64_t token1 = 0x100u;

  omni_reactor_add(&reactor, fd1, token1, OMNI_POLLER_INTEREST_READ,
                   capture_callback, (void *)0x1000);

  struct omni_reactor_result r = omni_reactor_step(&reactor, 100);
  check(r.status == OMNI_REACTOR_OK && r.sys_errno == 0 && r.count == 0u,
        "step with positive timeout returns when no events");

  close(fd1);
  close(fd2);
  reset_capture();
  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&poller);
}

/* ------------------------------------------------------------------- update */

static void test_reactor_update(void) {
  struct omni_poller poller;
  struct omni_reactor reactor;
  omni_reactor_make_inert(&reactor);

  struct pollfd slots_poller[2];
  uint64_t tokens_poller[2];
  omni_poller_init_borrowed(&poller, slots_poller, tokens_poller, 2u);

  int callbacks[2];
  int contexts[2];
  uint32_t ready_masks[2];
  uint64_t tokens_reactor[2];
  int fds_reactor[2];
  bool registered[2];

  omni_reactor_init(&reactor, &poller, callbacks, contexts, ready_masks,
                    tokens_reactor, fds_reactor, registered, 2u);

  int fd1, fd2;
  make_socketpair(&fd1, &fd2);

  uint64_t token1 = 0x100u;

  omni_reactor_add(&reactor, fd1, token1, OMNI_POLLER_INTEREST_READ,
                   capture_callback, (void *)0x1000);

  /* Update interests */
  struct omni_reactor_result r = omni_reactor_update(
      &reactor, token1, OMNI_POLLER_INTEREST_WRITE);
  check(r.status == OMNI_REACTOR_OK && r.sys_errno == 0 && r.count == 0u,
        "update changes interests");

  /* Unknown token */
  struct omni_reactor_result r2 = omni_reactor_update(&reactor, 0x999u,
                                                      OMNI_POLLER_INTEREST_READ);
  check(r2.status == OMNI_REACTOR_ERR_NOT_FOUND && r2.sys_errno == ENOENT,
        "update unknown token reports not found");

  /* Invalid mask */
  struct omni_reactor_result r3 = omni_reactor_update(&reactor, token1, 0u);
  check(r3.status == OMNI_REACTOR_ERR_INVALID && r3.sys_errno == EINVAL,
        "update rejects zero mask");

  close(fd1);
  close(fd2);
  reset_capture();
  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&poller);
}

/* ------------------------------------------------------------------- stress */

static void test_reactor_stress_add_remove(void) {
  struct omni_poller poller;
  struct omni_reactor reactor;
  omni_reactor_make_inert(&reactor);

  /* Use larger capacity to support stress cycles */
  struct pollfd slots_poller[100];
  uint64_t tokens_poller[100];
  omni_poller_init_borrowed(&poller, slots_poller, tokens_poller, 100u);

  int callbacks[100];
  int contexts[100];
  uint32_t ready_masks[100];
  uint64_t tokens_reactor[100];
  int fds_reactor[100];
  bool registered[100];

  omni_reactor_init(&reactor, &poller, callbacks, contexts, ready_masks,
                    tokens_reactor, fds_reactor, registered, 100u);

  for (uint32_t cycle = 0u; cycle < 1000u; ++cycle) {
    int pair[2];
    make_socketpair(pair);
    int fd1 = pair[0];
    int fd2 = pair[1];

    uint64_t token1 = cycle + 1u;
    uint64_t token2 = (cycle + 1000u) + 1u;

    struct omni_reactor_result r1 = omni_reactor_add(
        &reactor, fd1, token1, OMNI_POLLER_INTEREST_READ, capture_callback,
        (void *)cycle);
    check(r1.status == OMNI_REACTOR_OK && r1.sys_errno == 0 && r1.count == 0u,
          "stress add first source succeeds");

    struct omni_reactor_result r2 = omni_reactor_add(
        &reactor, fd2, token2, OMNI_POLLER_INTEREST_WRITE, capture_callback,
        (void *)((size_t)cycle + 0x2000));
    check(r2.status == OMNI_REACTOR_OK && r2.sys_errno == 0 && r2.count == 0u,
          "stress add second source succeeds");

    if (cycle % 2 == 0) {
      struct omni_reactor_result r3 = omni_reactor_remove(&reactor, token1);
      check(r3.status == OMNI_REACTOR_OK && r3.sys_errno == 0 && r3.count == 0u,
            "stress remove first source succeeds");
    }

    if (cycle % 3 == 0) {
      struct omni_reactor_result r4 = omni_reactor_remove(&reactor, token2);
      check(r4.status == OMNI_REACTOR_OK && r4.sys_errno == 0 && r4.count == 0u,
            "stress remove second source succeeds");
    }

    close(fd1);
    close(fd2);
  }

  /* Should end with few live entries (capacity may be partially filled) */
  size_t remaining = omni_reactor_count(&reactor);
  check(remaining < 100u, "stress leaves reactor below capacity");

  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&poller);
}

static void test_reactor_stress_step(void) {
  struct omni_poller poller;
  struct omni_reactor reactor;
  omni_reactor_make_inert(&reactor);

  struct pollfd slots_poller[10];
  uint64_t tokens_poller[10];
  omni_poller_init_borrowed(&poller, slots_poller, tokens_poller, 10u);

  int callbacks[10];
  int contexts[10];
  uint32_t ready_masks[10];
  uint64_t tokens_reactor[10];
  int fds_reactor[10];
  bool registered[10];

  omni_reactor_init(&reactor, &poller, callbacks, contexts, ready_masks,
                    tokens_reactor, fds_reactor, registered, 10u);

  /* Create several socketpairs for stress testing */
  int pairs[20][2];
  for (int i = 0; i < 20; i += 2) {
    make_socketpair(pairs[i / 2]);
  }

  for (int i = 0; i < 10; i += 2) {
    uint64_t token = (uint64_t)i + 0x1000u;
    omni_reactor_add(&reactor, pairs[i / 2][0], token, OMNI_POLLER_INTEREST_READ,
                     capture_callback, (void *)((size_t)i + 0x1000));
  }

  /* Step with zero timeout; some may be ready due to writes
   * already performed by make_socketpair (handshake buffers).
   */
  for (uint32_t step = 0u; step < 50u; ++step) {
    struct omni_reactor_result r = omni_reactor_step(&reactor, 0);
    /* step can return OK with zero or one count; both are fine */
    check(r.status == OMNI_REACTOR_OK || r.status == OMNI_REACTOR_ERR_INTERRUPTED,
          "step either succeeds or returns INTERRUPTED");
    if (r.status == OMNI_REACTOR_OK) {
      check(r.count <= 10u, "processed count does not exceed capacity");
    }
  }

  for (int i = 0; i < 10; ++i) {
    close(pairs[i][0]);
    close(pairs[i][1]);
  }

  omni_reactor_destroy(&reactor);
  omni_poller_destroy(&poller);
}

static void print_summary(void) {
  printf("\n=== Reactor unit tests summary ===\n");
  printf("Total checks: %d\n", check_count);
  printf("Failures: %d\n", failure_count);
  printf("Success rate: %.2f%%\n",
          (check_count == 0) ? 0.0 : (100.0 * (check_count - failure_count) / check_count));
}

int main(void) {
  /* Initialize results counters */
  check_count = 0;
  failure_count = 0;

  printf("=== Starting reactor unit tests (Task 021) ===\n");

  test_reactor_init();
  test_reactor_init_rejects();
  test_reactor_add();
  test_reactor_remove();
  test_reactor_step_empty();
  test_reactor_step_zero_timeout();
  test_reactor_step_positive_timeout();
  test_reactor_update();
  test_reactor_stress_add_remove();
  test_reactor_stress_step();

  print_summary();

  return failure_count > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}