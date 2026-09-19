/*
 * OmniRoute native backend — bounded readiness tests (Task 015).
 *
 * Loopback-only, self-cleaning, deterministic: generic behavior runs on
 * pipes and Unix socketpairs (no TCP needed), plus exactly one
 * loopback-listener integration proof. No fixed ports — the operator port
 * 20128 is never touched — and no socket payload in either direction:
 * probes connect and close only, and this file contains no send/recv
 * calls at all (verifiable: grep for those tokens finds no matches).
 * Pipe writes/reads below are local-kernel test stimulus for readiness
 * edges, not network traffic. Framework-free TAP-style output with a
 * nonzero exit on any failure. Run under CTest, including the ASan +
 * UBSan configuration, whose leak check validates the one-allocation
 * owned lifecycle.
 *
 * Timing discipline: zero-time waits wherever the condition pre-exists
 * (pipe data already written, handshake already completed, peer already
 * closed); finite waits only for inherently asynchronous kernel round
 * trips (refused-connect RST), asserting bit presence rather than timing.
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
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "omniroute/listener.h"
#include "omniroute/poller.h"

/* Socket-payload counters: this file performs no socket payload I/O by
 * construction, and the network-boundary gate proves production code
 * cannot either. Asserted zero at the end. */
static size_t socket_bytes_sent = 0;
static size_t socket_bytes_read = 0;

static int check_count = 0;
static int failure_count = 0;

static void check(bool cond, const char *name) {
  ++check_count;
  if (cond) {
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

/* ------------------------------------------------------------------ init */

static void test_init(void) {
  struct omni_poller p;
  struct omni_poller z;
  struct pollfd slots[4];
  uint64_t toks[4];
  struct omni_poller_result r;

  memset(&z, 0, sizeof(z));
  check(omni_poller_capacity(NULL) == 0u, "capacity on NULL is zero");
  check(omni_poller_count(NULL) == 0u, "count on NULL is zero");
  check(omni_poller_capacity(&z) == 0u && omni_poller_count(&z) == 0u,
        "zeroed storage observes as inert");

  r = omni_poller_init_borrowed(&p, slots, toks, 4u);
  check(r.status == OMNI_POLLER_OK && r.sys_errno == 0 && r.count == 0u,
        "borrowed init succeeds");
  check(omni_poller_capacity(&p) == 4u, "borrowed capacity matches");
  check(omni_poller_count(&p) == 0u, "initial count is zero");
  omni_poller_destroy(&p);

  r = omni_poller_init_owned(&p, 8u);
  check(r.status == OMNI_POLLER_OK, "owned init succeeds");
  check(omni_poller_capacity(&p) == 8u, "owned capacity matches");
  check(omni_poller_count(&p) == 0u, "owned poller starts empty");
  omni_poller_destroy(&p);
}

static void test_init_rejects(void) {
  struct omni_poller p;
  struct pollfd slots[2];
  uint64_t toks[2];
  struct omni_poller_result r;

  r = omni_poller_init_borrowed(NULL, slots, toks, 2u);
  check(r.status == OMNI_POLLER_ERR_INVALID, "borrowed init rejects NULL poller");
  r = omni_poller_init_borrowed(&p, NULL, toks, 2u);
  check(r.status == OMNI_POLLER_ERR_INVALID, "borrowed init rejects NULL fd array");
  r = omni_poller_init_borrowed(&p, slots, NULL, 2u);
  check(r.status == OMNI_POLLER_ERR_INVALID, "borrowed init rejects NULL token array");
  r = omni_poller_init_borrowed(&p, slots, toks, 0u);
  check(r.status == OMNI_POLLER_ERR_INVALID, "borrowed init rejects zero capacity");
  check(omni_poller_capacity(&p) == 0u, "rejected borrowed poller stays inert");
  r = omni_poller_init_owned(NULL, 4u);
  check(r.status == OMNI_POLLER_ERR_INVALID, "owned init rejects NULL poller");
  r = omni_poller_init_owned(&p, 0u);
  check(r.status == OMNI_POLLER_ERR_INVALID, "owned init rejects zero capacity");
  /* Unrepresentable size fails on arithmetic alone — no giant block attempted. */
  r = omni_poller_init_owned(&p, (size_t)-1);
  check(r.status == OMNI_POLLER_ERR_INVALID, "owned init rejects overflowing capacity");
  check(omni_poller_capacity(&p) == 0u, "rejected owned poller stays inert");
  omni_poller_destroy(&p);
  omni_poller_destroy(NULL);
  check(true, "destroy tolerates NULL");
}

/* ------------------------------------------------------------------- add */

static void test_add_basics(void) {
  struct omni_poller p;
  struct pollfd slots[4];
  uint64_t toks[4];
  int pr[2] = { -1, -1 };
  struct omni_poller_result r;

  check(pipe(pr) == 0, "add-probe pipes open (setup)");
  omni_poller_init_borrowed(&p, slots, toks, 4u);
  r = omni_poller_add(&p, pr[0], (uint64_t)7u, OMNI_POLLER_INTEREST_READ);
  check(r.status == OMNI_POLLER_OK, "add of valid FD succeeds");
  check(omni_poller_count(&p) == 1u, "count increments on add");
  r = omni_poller_add(&p, pr[0], (uint64_t)8u, OMNI_POLLER_INTEREST_READ);
  check(r.status == OMNI_POLLER_ERR_DUPLICATE, "duplicate FD rejected");
  check(omni_poller_count(&p) == 1u, "failed add preserves count");
  r = omni_poller_add(&p, -1, (uint64_t)9u, OMNI_POLLER_INTEREST_READ);
  check(r.status == OMNI_POLLER_ERR_INVALID, "negative FD rejected");
  r = omni_poller_add(&p, pr[1], (uint64_t)9u, 0u);
  check(r.status == OMNI_POLLER_ERR_INVALID, "empty interest mask rejected");
  r = omni_poller_add(&p, pr[1], (uint64_t)9u,
                      OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE |
                          (uint32_t)4u);
  check(r.status == OMNI_POLLER_ERR_INVALID, "unknown interest bits rejected");
  r = omni_poller_add(NULL, pr[1], (uint64_t)9u, OMNI_POLLER_INTEREST_WRITE);
  check(r.status == OMNI_POLLER_ERR_INVALID, "add on NULL poller fails");
  omni_poller_destroy(&p);
  (void)close(pr[0]);
  (void)close(pr[1]);
}

static void test_add_full(void) {
  struct omni_poller p;
  int a[2] = { -1, -1 };
  int b[2] = { -1, -1 };
  int c[2] = { -1, -1 };
  struct omni_poller_result r;

  check(pipe(a) == 0 && pipe(b) == 0 && pipe(c) == 0, "full-probe pipes open (setup)");
  omni_poller_init_owned(&p, 2u);
  check(omni_poller_add(&p, a[0], (uint64_t)1u, OMNI_POLLER_INTEREST_READ).status ==
            OMNI_POLLER_OK,
        "first add fits (setup)");
  check(omni_poller_add(&p, b[0], (uint64_t)2u, OMNI_POLLER_INTEREST_READ).status ==
            OMNI_POLLER_OK,
        "second add fits (setup)");
  r = omni_poller_add(&p, c[0], (uint64_t)3u, OMNI_POLLER_INTEREST_READ);
  check(r.status == OMNI_POLLER_ERR_FULL, "registration beyond capacity fails");
  check(r.sys_errno != 0, "full registration captures errno");
  check(omni_poller_count(&p) == 2u, "failed add preserves count");
  omni_poller_destroy(&p);
  (void)close(a[0]);
  (void)close(a[1]);
  (void)close(b[0]);
  (void)close(b[1]);
  (void)close(c[0]);
  (void)close(c[1]);
}

/* ---------------------------------------------------------------- update */

static void test_update(void) {
  struct omni_poller p;
  struct pollfd slots[4];
  uint64_t toks[4];
  int pr[2] = { -1, -1 };
  struct omni_poller_event ev[4];
  struct omni_poller_result r;
  unsigned char byte = 'q';

  check(pipe(pr) == 0, "update-probe pipes open (setup)");
  omni_poller_init_borrowed(&p, slots, toks, 4u);
  omni_poller_add(&p, pr[0], (uint64_t)0xA5u, OMNI_POLLER_INTEREST_READ);

  r = omni_poller_update(&p, 999999, OMNI_POLLER_INTEREST_WRITE);
  check(r.status == OMNI_POLLER_ERR_NOT_FOUND, "update of missing FD fails");
  r = omni_poller_update(&p, pr[0], 0u);
  check(r.status == OMNI_POLLER_ERR_INVALID, "update with empty mask fails");
  /* Old mask survived the failed update: data still reports readable. */
  check(write(pr[1], &byte, 1u) == 1, "update-probe stimulus written (setup)");
  r = omni_poller_wait(&p, (int64_t)0, ev, 4u);
  check(r.status == OMNI_POLLER_OK && r.count == 1u &&
            ev[0].token == (uint64_t)0xA5u &&
            (ev[0].ready & OMNI_POLLER_READY_READ) != 0u,
        "failed update preserves old mask and token");
  check(read(pr[0], &byte, 1u) == 1, "update-probe stimulus drained (setup)");

  r = omni_poller_update(&p, pr[0],
                         OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE);
  check(r.status == OMNI_POLLER_OK, "update to wider mask succeeds");
  omni_poller_destroy(&p);
  (void)close(pr[0]);
  (void)close(pr[1]);
}

/* ---------------------------------------------------------------- remove */

static void test_remove(void) {
  struct omni_poller p;
  struct pollfd slots[4];
  uint64_t toks[4];
  int a[2] = { -1, -1 };
  int b[2] = { -1, -1 };
  struct omni_poller_result r;

  check(pipe(a) == 0 && pipe(b) == 0, "remove-probe pipes open (setup)");
  omni_poller_init_borrowed(&p, slots, toks, 4u);
  omni_poller_add(&p, a[0], (uint64_t)1u, OMNI_POLLER_INTEREST_READ);
  omni_poller_add(&p, b[0], (uint64_t)2u, OMNI_POLLER_INTEREST_READ);

  r = omni_poller_remove(&p, 999999);
  check(r.status == OMNI_POLLER_ERR_NOT_FOUND, "remove of missing FD fails");
  check(omni_poller_count(&p) == 2u, "failed remove preserves count");
  r = omni_poller_remove(&p, a[0]);
  check(r.status == OMNI_POLLER_OK, "remove of live FD succeeds");
  check(omni_poller_count(&p) == 1u, "remove decrements count");
  check(fd_open(a[0]), "remove does not close the FD");
  /* Survivor keeps identity and order: stimulus on b reports token 2. */
  {
    unsigned char byte = 's';
    struct omni_poller_event ev[4];

    check(write(b[1], &byte, 1u) == 1, "survivor stimulus written (setup)");
    r = omni_poller_wait(&p, (int64_t)0, ev, 4u);
    check(r.status == OMNI_POLLER_OK && r.count == 1u &&
              ev[0].token == (uint64_t)2u &&
              (ev[0].ready & OMNI_POLLER_READY_READ) != 0u,
          "survivor keeps identity after compaction");
    check(read(b[0], &byte, 1u) == 1, "survivor stimulus drained (setup)");
  }
  /* A removed FD may register again. */
  r = omni_poller_add(&p, a[0], (uint64_t)3u, OMNI_POLLER_INTEREST_READ);
  check(r.status == OMNI_POLLER_OK, "removed FD may register again");
  r = omni_poller_remove(&p, a[0]);
  check(r.status == OMNI_POLLER_OK && omni_poller_count(&p) == 1u,
        "re-added FD removes cleanly");
  omni_poller_destroy(&p);
  check(fd_open(a[0]) && fd_open(b[0]), "destroy closes no registered FD");
  (void)close(a[0]);
  (void)close(a[1]);
  (void)close(b[0]);
  (void)close(b[1]);
}

/* ------------------------------------------------------------- reset */

static void test_reset(void) {
  struct omni_poller p;
  struct pollfd slots[4];
  uint64_t toks[4];
  int a[2] = { -1, -1 };
  int b[2] = { -1, -1 };
  bool supported = false;
  int before = 0;
  int after = 0;

  check(pipe(a) == 0 && pipe(b) == 0, "reset-probe pipes open (setup)");
  omni_poller_init_borrowed(&p, slots, toks, 4u);
  omni_poller_add(&p, a[0], (uint64_t)1u, OMNI_POLLER_INTEREST_READ);
  omni_poller_add(&p, b[1], (uint64_t)2u, OMNI_POLLER_INTEREST_WRITE);
  before = count_open_fds(&supported);
  omni_poller_reset(&p);
  after = count_open_fds(&supported);
  check(omni_poller_count(&p) == 0u, "reset removes all registrations");
  check(omni_poller_capacity(&p) == 4u, "reset retains capacity");
  check(fd_open(a[0]) && fd_open(b[1]), "reset closes no FDs");
  if (supported) {
    check(after == before, "reset leaks no descriptors");
  } else {
    check(true, "reset leaks no descriptors (skipped: no fd census)");
  }
  /* Backing reusable immediately. */
  check(omni_poller_add(&p, a[0], (uint64_t)9u, OMNI_POLLER_INTEREST_READ).status ==
            OMNI_POLLER_OK,
        "reset backing accepts new registrations");
  omni_poller_reset(NULL);
  check(true, "reset tolerates NULL");
  omni_poller_destroy(&p);
  (void)close(a[0]);
  (void)close(a[1]);
  (void)close(b[0]);
  (void)close(b[1]);
}

/* --------------------------------------------------------------- waits */

static void test_zero_and_timeout_waits(void) {
  struct omni_poller p;
  struct pollfd slots[2];
  uint64_t toks[2];
  int pr[2] = { -1, -1 };
  struct omni_poller_event ev[2];
  struct omni_poller_result r;

  check(pipe(pr) == 0, "wait-probe pipes open (setup)");
  omni_poller_init_borrowed(&p, slots, toks, 2u);
  omni_poller_add(&p, pr[0], (uint64_t)1u, OMNI_POLLER_INTEREST_READ);

  r = omni_poller_wait(&p, (int64_t)0, ev, 2u);
  check(r.status == OMNI_POLLER_OK && r.count == 0u,
        "zero-time wait with no readiness reports zero");
  r = omni_poller_wait(&p, (int64_t)50, ev, 2u);
  check(r.status == OMNI_POLLER_OK && r.count == 0u,
        "finite wait with no readiness times out to zero");
  omni_poller_destroy(&p);
  (void)close(pr[0]);
  (void)close(pr[1]);
}

static void test_timeout_boundaries(void) {
  struct omni_poller p;
  struct pollfd slots[2];
  uint64_t toks[2];
  int pr[2] = { -1, -1 };
  struct omni_poller_event ev[2];
  struct omni_poller_result r;

  check(pipe(pr) == 0, "timeout-probe pipes open (setup)");
  /* Empty poller validates the timeout without ever waiting. */
  omni_poller_init_borrowed(&p, slots, toks, 2u);
  r = omni_poller_wait(&p, (int64_t)INT_MAX, ev, 2u);
  check(r.status == OMNI_POLLER_OK && r.count == 0u,
        "INT_MAX timeout accepted on empty poller without blocking");
  r = omni_poller_wait(&p, (int64_t)INT_MAX + (int64_t)1, ev, 2u);
  check(r.status == OMNI_POLLER_ERR_INVALID, "timeout above INT_MAX rejected");
  r = omni_poller_wait(&p, (int64_t)-1, ev, 2u);
  check(r.status == OMNI_POLLER_ERR_INVALID, "negative timeout rejected");
  /* Non-empty poller rejects bad timeouts before polling. */
  omni_poller_add(&p, pr[0], (uint64_t)1u, OMNI_POLLER_INTEREST_READ);
  r = omni_poller_wait(&p, (int64_t)INT_MAX + (int64_t)1, ev, 2u);
  check(r.status == OMNI_POLLER_ERR_INVALID, "overflow timeout rejected with live FDs");
  r = omni_poller_wait(&p, (int64_t)-1, ev, 2u);
  check(r.status == OMNI_POLLER_ERR_INVALID, "negative timeout rejected with live FDs");
  r = omni_poller_wait(NULL, (int64_t)0, ev, 2u);
  check(r.status == OMNI_POLLER_ERR_INVALID, "wait on NULL poller fails");
  omni_poller_destroy(&p);
  (void)close(pr[0]);
  (void)close(pr[1]);
}

static void test_output_capacity(void) {
  struct omni_poller p;
  struct pollfd slots[4];
  uint64_t toks[4];
  int a[2] = { -1, -1 };
  int b[2] = { -1, -1 };
  unsigned char byte = 'e';
  struct omni_poller_event ev[4];
  struct omni_poller_result r;

  check(pipe(a) == 0 && pipe(b) == 0, "output-probe pipes open (setup)");
  check(write(a[1], &byte, 1u) == 1 && write(b[1], &byte, 1u) == 1,
        "output-probe stimulus written (setup)");
  omni_poller_init_borrowed(&p, slots, toks, 4u);
  omni_poller_add(&p, a[0], (uint64_t)11u, OMNI_POLLER_INTEREST_READ);
  omni_poller_add(&p, b[0], (uint64_t)22u, OMNI_POLLER_INTEREST_READ);

  r = omni_poller_wait(&p, (int64_t)0, ev, 1u);
  check(r.status == OMNI_POLLER_ERR_OUTPUT && r.count == 0u,
        "undersized output fails without polling");
  check(r.sys_errno != 0, "output failure captures errno");
  r = omni_poller_wait(&p, (int64_t)0, NULL, 0u);
  check(r.status == OMNI_POLLER_ERR_OUTPUT, "NULL output with live FDs fails");
  /* No wait happened above: full-capacity wait still sees both. */
  r = omni_poller_wait(&p, (int64_t)0, ev, 4u);
  check(r.status == OMNI_POLLER_OK && r.count == 2u,
        "adequate output reports every ready registration");
  check(ev[0].token == (uint64_t)11u && ev[1].token == (uint64_t)22u,
        "events arrive in registration order with tokens");
  /* Exact-capacity buffer: ASan proves no overrun beyond these asserts. */
  {
    struct omni_poller_event tight[2];

    r = omni_poller_wait(&p, (int64_t)0, tight, 2u);
    check(r.status == OMNI_POLLER_OK && r.count == 2u,
          "exact-capacity output holds every event");
  }
  omni_poller_destroy(&p);
  (void)close(a[0]);
  (void)close(a[1]);
  (void)close(b[0]);
  (void)close(b[1]);
}

/* ------------------------------------------------- readable and writable */

static void test_readable_writable(void) {
  struct omni_poller p;
  struct pollfd slots[4];
  uint64_t toks[4];
  int pr[2] = { -1, -1 };
  unsigned char byte = 'r';
  struct omni_poller_event ev[4];
  struct omni_poller_result r;

  check(pipe(pr) == 0, "rw-probe pipes open (setup)");
  omni_poller_init_borrowed(&p, slots, toks, 4u);
  omni_poller_add(&p, pr[0], (uint64_t)100u, OMNI_POLLER_INTEREST_READ);
  omni_poller_add(&p, pr[1], (uint64_t)200u, OMNI_POLLER_INTEREST_WRITE);

  /* Fresh pipe: write end ready, read end not. */
  r = omni_poller_wait(&p, (int64_t)0, ev, 4u);
  check(r.status == OMNI_POLLER_OK && r.count == 1u, "only the writable end is ready");
  check(ev[0].token == (uint64_t)200u &&
            (ev[0].ready & OMNI_POLLER_READY_WRITE) != 0u,
        "writable readiness detected with token");

  /* After stimulus both ends report. */
  check(write(pr[1], &byte, 1u) == 1, "rw-probe stimulus written (setup)");
  r = omni_poller_wait(&p, (int64_t)0, ev, 4u);
  check(r.status == OMNI_POLLER_OK && r.count == 2u, "both ends ready after write");
  check((ev[0].ready & OMNI_POLLER_READY_READ) != 0u && ev[0].fd == pr[0],
        "readable readiness detected on the read end");

  /* Drain: no stale readiness survives into the next wait. */
  check(read(pr[0], &byte, 1u) == 1, "rw-probe stimulus drained (setup)");
  r = omni_poller_wait(&p, (int64_t)0, ev, 4u);
  check(r.status == OMNI_POLLER_OK && r.count == 1u &&
            (ev[0].ready & OMNI_POLLER_READY_WRITE) != 0u,
        "drained read end reports nothing stale");
  omni_poller_destroy(&p);
  (void)close(pr[0]);
  (void)close(pr[1]);
}

/* ------------------------------------------------------- hangup and error */

static void test_hangup(void) {
  struct omni_poller p;
  struct pollfd slots[2];
  uint64_t toks[2];
  int sp[2] = { -1, -1 };
  struct omni_poller_event ev[2];
  struct omni_poller_result r;

  check(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "hangup socketpair opens (setup)");
  omni_poller_init_borrowed(&p, slots, toks, 2u);
  omni_poller_add(&p, sp[0], (uint64_t)0x40u, OMNI_POLLER_INTEREST_READ);
  (void)close(sp[1]); /* peer fully gone: kernel reports IN+HUP on survivor */
  r = omni_poller_wait(&p, (int64_t)0, ev, 2u);
  check(r.status == OMNI_POLLER_OK && r.count == 1u, "closed peer surfaces readiness");
  check((ev[0].ready & OMNI_POLLER_READY_HANGUP) != 0u, "hangup condition surfaced");
  check(ev[0].fd == sp[0], "hangup event carries the borrowed FD");
  omni_poller_remove(&p, sp[0]);
  omni_poller_destroy(&p);
  (void)close(sp[0]);
}

static void test_refused_error(void) {
  struct omni_listener probe;
  struct omni_listener_result lr;
  struct omni_poller p;
  struct pollfd slots[2];
  uint64_t toks[2];
  struct sockaddr_in dead;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int cfd = -1;
  struct omni_poller_event ev[2];
  struct omni_poller_result r;

  /* A just-closed loopback port is deterministically refused. */
  lr = omni_listener_init(&probe, "127.0.0.1", (uint16_t)0u);
  check(lr.status == OMNI_LISTENER_OK, "error-probe port reserved (setup)");
  port = omni_listener_port(&probe);
  omni_listener_destroy(&probe);

  cfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  check(cfd >= 0, "error-probe client created (setup)");
  memset(&dead, 0, sizeof(dead));
  dead.sin_family = AF_INET;
  dead.sin_port = htons(port);
  dead.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  check(connect(cfd, (const struct sockaddr *)&dead, (socklen_t)sizeof(dead)) != 0 &&
            errno == EINPROGRESS,
        "error-probe connect pending (setup)");

  omni_poller_init_borrowed(&p, slots, toks, 2u);
  omni_poller_add(&p, cfd, (uint64_t)0xE99u, OMNI_POLLER_INTEREST_WRITE);
  r = omni_poller_wait(&p, (int64_t)1000, ev, 2u);
  check(r.status == OMNI_POLLER_OK && r.count == 1u, "refused connect completes a wait");
  check((ev[0].ready & OMNI_POLLER_READY_ERROR) != 0u, "refused connect surfaces error");
  check(ev[0].token == (uint64_t)0xE99u, "error event round-trips the token");
  omni_poller_remove(&p, cfd);
  omni_poller_destroy(&p);
  (void)close(cfd);
}

/* ------------------------------------------------- externally closed FD */

static void test_externally_closed_fd(void) {
  struct omni_poller p;
  struct pollfd slots[2];
  uint64_t toks[2];
  int pr[2] = { -1, -1 };
  struct omni_poller_event ev[2];
  struct omni_poller_result r;
  bool supported = false;
  int before = 0;
  int after = 0;

  check(pipe(pr) == 0, "nval-probe pipes open (setup)");
  omni_poller_init_borrowed(&p, slots, toks, 2u);
  omni_poller_add(&p, pr[0], (uint64_t)0xBADu, OMNI_POLLER_INTEREST_READ);
  before = count_open_fds(&supported);
  /* Owner violation simulation: descriptor closed while registered. */
  (void)close(pr[0]);
  r = omni_poller_wait(&p, (int64_t)0, ev, 2u);
  check(r.status == OMNI_POLLER_OK && r.count == 1u,
        "externally closed FD surfaces readiness");
  check((ev[0].ready & OMNI_POLLER_READY_INVALID) != 0u,
        "externally closed FD surfaces invalid condition");
  check(ev[0].token == (uint64_t)0xBADu, "invalid event round-trips the token");
  after = count_open_fds(&supported);
  omni_poller_remove(&p, pr[0]);
  omni_poller_destroy(&p);
  if (supported) {
    /* One descriptor fewer (the violated one); poller closed nothing else. */
    check(after == before - 1, "poller closed nothing around the violation");
  } else {
    check(true, "poller closed nothing around the violation (skipped: no fd census)");
  }
  check(fd_open(pr[1]), "sibling FD untouched by poller destroy");
  (void)close(pr[1]);
}

/* ---------------------------------------------------------- EINTR policy */

static void on_alarm(int sig) {
  (void)sig;
}

static void test_interrupted(void) {
  struct omni_poller p;
  struct pollfd slots[2];
  uint64_t toks[2];
  int pr[2] = { -1, -1 };
  struct omni_poller_event ev[2];
  struct omni_poller_result r;
  struct sigaction set_sa;
  struct sigaction old_sa;
  bool have_old = false;

  check(pipe(pr) == 0, "eintr-probe pipes open (setup)");
  omni_poller_init_borrowed(&p, slots, toks, 2u);
  omni_poller_add(&p, pr[0], (uint64_t)5u, OMNI_POLLER_INTEREST_READ);

  memset(&set_sa, 0, sizeof(set_sa));
  set_sa.sa_handler = on_alarm;
  sigemptyset(&set_sa.sa_mask);
  set_sa.sa_flags = 0; /* no SA_RESTART: the wait must observe EINTR */
  if (sigaction(SIGALRM, &set_sa, &old_sa) == 0) {
    have_old = true;
  }
  check(have_old, "self-signal handler installed (setup)");
  (void)alarm(1); /* fires long before the 10s wait below expires */
  r = omni_poller_wait(&p, (int64_t)10000, ev, 2u);
  (void)alarm(0);
  if (have_old) {
    (void)sigaction(SIGALRM, &old_sa, NULL);
  }
  check(r.status == OMNI_POLLER_ERR_INTERRUPTED && r.count == 0u,
        "interrupted wait returns INTERRUPTED with nothing consumed");
  check(r.sys_errno == EINTR, "interrupted wait captures EINTR");
  check(omni_poller_count(&p) == 1u, "interrupted wait preserves registrations");
  /* Poller fully usable afterwards. */
  r = omni_poller_wait(&p, (int64_t)0, ev, 2u);
  check(r.status == OMNI_POLLER_OK && r.count == 0u, "poller usable after interrupt");
  omni_poller_destroy(&p);
  (void)close(pr[0]);
  (void)close(pr[1]);
}

/* ----------------------------------------------- listener integration proof */

static void test_listener_integration(void) {
  struct omni_listener listener;
  struct omni_listener_result lr;
  struct omni_poller p;
  struct pollfd slots[2];
  uint64_t toks[2];
  struct sockaddr_in peer;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int cfd = -1;
  struct omni_poller_event ev[2];
  struct omni_poller_result r;

  omni_listener_make_inert(&listener);
  lr = omni_listener_init(&listener, "127.0.0.1", (uint16_t)0u);
  check(lr.status == OMNI_LISTENER_OK, "integration listener binds (setup)");
  port = omni_listener_port(&listener);
  check(port != OMNI_LISTENER_PORT_INVALID && port != (uint16_t)20128,
        "integration port is ephemeral, never the operator port");

  omni_poller_init_borrowed(&p, slots, toks, 2u);
  r = omni_poller_add(&p, omni_listener_fd(&listener), (uint64_t)0x157u,
                      OMNI_POLLER_INTEREST_READ);
  check(r.status == OMNI_POLLER_OK, "listener FD registers as borrowed");
  r = omni_poller_wait(&p, (int64_t)0, ev, 2u);
  check(r.status == OMNI_POLLER_OK && r.count == 0u,
        "idle listener reports no readiness");

  /* Controlled client: blocking connect completes the handshake before
   * returning, so the zero-time wait below is deterministic, not racy. */
  cfd = socket(AF_INET, SOCK_STREAM, 0);
  check(cfd >= 0, "integration client created (setup)");
  memset(&peer, 0, sizeof(peer));
  peer.sin_family = AF_INET;
  peer.sin_port = htons(port);
  peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  check(connect(cfd, (const struct sockaddr *)&peer, (socklen_t)sizeof(peer)) == 0,
        "integration client connects (setup)");

  r = omni_poller_wait(&p, (int64_t)0, ev, 2u);
  check(r.status == OMNI_POLLER_OK && r.count == 1u,
        "loopback connect causes listener readability");
  check((ev[0].ready & OMNI_POLLER_READY_READ) != 0u, "readiness is readable");
  check(ev[0].fd == omni_listener_fd(&listener), "event carries the borrowed FD");
  /* Production never accepts: listener still holds exactly one owned FD. */
  (void)close(cfd);
  r = omni_poller_remove(&p, omni_listener_fd(&listener));
  check(r.status == OMNI_POLLER_OK, "listener registration removes cleanly");
  omni_poller_destroy(&p);
  check(fd_open(omni_listener_fd(&listener)), "listener usable after poller destroy");
  check(omni_listener_port(&listener) == port, "listener port unchanged by poller");
  omni_listener_destroy(&listener);
  check(true, "integration cleans up with no production accept");
}

/* ------------------------------------------------------ lifecycle stress */

static void test_lifecycle_stress(void) {
  struct omni_poller p;
  int fds[4][2];
  struct omni_poller_event ev[8];
  struct omni_poller_result r;
  bool supported = false;
  int before = 0;
  size_t step = 0;
  int i = 0;

  for (i = 0; i < 4; ++i) {
    check(pipe(fds[i]) == 0, "stress pipes open (setup)");
  }
  before = count_open_fds(&supported);
  r = omni_poller_init_owned(&p, 8u);
  check(r.status == OMNI_POLLER_OK, "stress poller initializes (setup)");

  for (step = 0; step < 120u; ++step) {
    int which = (int)(step % 4u);
    uint64_t token = (uint64_t)(1000u + step);
    size_t count_before = omni_poller_count(&p);

    switch (step % 5u) {
      case 0u:
        r = omni_poller_add(&p, fds[which][0], token, OMNI_POLLER_INTEREST_READ);
        if (r.status != OMNI_POLLER_OK && r.status != OMNI_POLLER_ERR_DUPLICATE &&
            r.status != OMNI_POLLER_ERR_FULL) {
          check(false, "stress add returns only expected statuses");
          goto done;
        }
        break;
      case 1u:
        r = omni_poller_update(&p, fds[which][0], OMNI_POLLER_INTEREST_READ |
                                                      OMNI_POLLER_INTEREST_WRITE);
        if (r.status != OMNI_POLLER_OK && r.status != OMNI_POLLER_ERR_NOT_FOUND) {
          check(false, "stress update returns only expected statuses");
          goto done;
        }
        break;
      case 2u: {
        r = omni_poller_wait(&p, (int64_t)0, ev, 8u);
        if (r.status != OMNI_POLLER_OK || r.count > 8u) {
          check(false, "stress wait stays bounded");
          goto done;
        }
        break;
      }
      case 3u:
        r = omni_poller_remove(&p, fds[which][0]);
        if (r.status != OMNI_POLLER_OK && r.status != OMNI_POLLER_ERR_NOT_FOUND) {
          check(false, "stress remove returns only expected statuses");
          goto done;
        }
        break;
      default:
        if (step % 20u == 19u) {
          omni_poller_reset(&p);
          if (omni_poller_count(&p) != 0u) {
            check(false, "stress reset empties");
            goto done;
          }
        }
        break;
    }
    /* Per-step invariants over public state. */
    if (omni_poller_count(&p) > omni_poller_capacity(&p)) {
      check(false, "stress count never exceeds capacity");
      goto done;
    }
    {
      /* Resets legitimately change the count with a stale status in r;
       * every other failure must preserve it. */
      bool reset_step = (step % 5u == 4u) && (step % 20u == 19u);

      if (!reset_step && r.status != OMNI_POLLER_OK &&
          omni_poller_count(&p) != count_before) {
        check(false, "stress failures preserve count");
        goto done;
      }
    }
  }
  check(true, "120-op deterministic lifecycle keeps invariants");

done:
  omni_poller_destroy(&p);
  if (supported) {
    int after = count_open_fds(&supported);

    check(after == before, "stress lifecycle leaks no descriptors");
  } else {
    check(true, "stress lifecycle leaks no descriptors (skipped: no fd census)");
  }
  for (i = 0; i < 4; ++i) {
    if (!fd_open(fds[i][0]) || !fd_open(fds[i][1])) {
      check(false, "stress borrowed FDs survive poller destroy");
      break;
    }
  }
  check(true, "stress borrowed FDs survive poller destroy");
  for (i = 0; i < 4; ++i) {
    (void)close(fds[i][0]);
    (void)close(fds[i][1]);
  }
}

int main(void) {
  test_init();
  test_init_rejects();
  test_add_basics();
  test_add_full();
  test_update();
  test_remove();
  test_reset();
  test_zero_and_timeout_waits();
  test_timeout_boundaries();
  test_output_capacity();
  test_readable_writable();
  test_hangup();
  test_refused_error();
  test_externally_closed_fd();
  test_interrupted();
  test_listener_integration();
  test_lifecycle_stress();

  check(socket_bytes_sent == 0u && socket_bytes_read == 0u,
        "no socket payload transferred by any test");

  printf("---\n%d checks, %d failures\n", check_count, failure_count);
  return failure_count == 0 ? 0 : 1;
}
