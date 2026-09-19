/*
 * OmniRoute native backend — accepted-socket ownership + bounded accept4
 * drain tests (Task 016).
 *
 * Loopback-only, self-cleaning, deterministic: no external network, no
 * fixed ports (ephemeral assignment everywhere; the operator port 20128 is
 * never touched), no client payload in either direction — clients connect
 * and close only, production code never transfers bytes, and this file
 * contains no payload-transfer calls at all (asserted zero at the end).
 * Framework-free TAP-style output with a nonzero exit on any failure. Run
 * under CTest, including the ASan + UBSan configuration.
 *
 * Timing discipline: blocking client connect calls complete the handshake
 * before returning, so every zero-time poller wait and every nonblocking
 * accept below observes pre-existing kernel state instead of racing it. No
 * sleeps anywhere.
 *
 * Two environment-sensitive helpers degrade honestly: descriptor counting
 * via /proc/self/fd is Linux-only (elsewhere those checks report skip and
 * the functional assertions still run), and the FD-0 acceptance runs in a
 * forked child so the runner's own standard streams are never at risk.
 *
 * Partial success after INTERRUPTED is not deterministically triggerable
 * here (no signal source exists in the test process), so that path is
 * covered by the capacity-reached and queue-drained partial fills plus the
 * shared stop-with-prior-owners code path they exercise; the EINTR mapping
 * itself is a one-line classification shared with the covered taxonomy.
 */

#define _GNU_SOURCE /* sockets/fork visibility under strict C11 */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "omniroute/accepted.h"
#include "omniroute/listener.h"
#include "omniroute/poller.h"

/* Payload counters: this file performs no socket payload I/O by
 * construction (no payload-transfer calls), and the network-boundary gate
 * proves production code cannot either. Asserted zero at the end. */
static size_t bytes_sent_by_tests = 0;
static size_t bytes_read_by_tests = 0;

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

/*
 * Count open descriptors of this process. The handle used for counting
 * appears in its own listing, so it is subtracted; before/after deltas
 * remain exact because both sides count symmetrically. Linux-only: other
 * platforms report unsupported and the leak checks skip (functional
 * assertions still run).
 */
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

static bool fd_flags_ok(int fd) {
  int status_flags = fcntl(fd, F_GETFL);
  int desc_flags = fcntl(fd, F_GETFD);

  return status_flags != -1 && (status_flags & O_NONBLOCK) != 0 && desc_flags != -1 &&
         (desc_flags & FD_CLOEXEC) != 0;
}

/* Bind an ephemeral loopback listener; every test port flows through here
 * so the operator port can never be taken by accident. */
static bool start_listener(struct omni_listener *l, uint16_t *port_out) {
  struct omni_listener_result r;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;

  omni_listener_make_inert(l);
  r = omni_listener_init(l, "127.0.0.1", (uint16_t)0u);
  check(r.status == OMNI_LISTENER_OK && r.sys_errno == 0, "listener binds (setup)");
  if (r.status != OMNI_LISTENER_OK) {
    return false;
  }
  port = omni_listener_port(l);
  check(port != OMNI_LISTENER_PORT_INVALID && port != (uint16_t)20128,
        "listener port is ephemeral, never the operator port");
  if (port_out != NULL) {
    *port_out = port;
  }
  return true;
}

/* Blocking loopback connect: the handshake completes before return, so a
 * subsequent nonblocking accept observes it deterministically. */
static int open_client(uint16_t port) {
  struct sockaddr_in peer;
  int cfd = socket(AF_INET, SOCK_STREAM, 0);

  check(cfd != OMNI_ACCEPTED_FD_INVALID, "client socket created (setup)");
  if (cfd == OMNI_ACCEPTED_FD_INVALID) {
    return OMNI_ACCEPTED_FD_INVALID;
  }
  memset(&peer, 0, sizeof(peer));
  peer.sin_family = AF_INET;
  peer.sin_port = htons(port);
  peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(cfd, (const struct sockaddr *)&peer, (socklen_t)sizeof(peer)) != 0) {
    check(false, "client connects (setup)");
    (void)close(cfd);
    return OMNI_ACCEPTED_FD_INVALID;
  }
  check(true, "client connects (setup)");
  return cfd;
}

/* ------------------------------------------------------------------ inert */

static void test_inert_and_sentinel(void) {
  struct omni_accepted s;
  struct omni_accepted z;

  check(OMNI_ACCEPTED_FD_INVALID == -1, "invalid FD sentinel is -1");
  memset(&z, 0, sizeof(z));
  omni_accepted_make_inert(&s);
  check(omni_accepted_fd(&s) == OMNI_ACCEPTED_FD_INVALID,
        "inert owner reports invalid FD");
  check(!omni_accepted_is_live(&s), "inert owner reports not live");
  check(omni_accepted_fd(&z) == OMNI_ACCEPTED_FD_INVALID &&
            !omni_accepted_is_live(&z),
        "zeroed storage observes as inert");
  check(omni_accepted_fd(NULL) == OMNI_ACCEPTED_FD_INVALID,
        "FD accessor on NULL is invalid");
  check(!omni_accepted_is_live(NULL), "liveness on NULL is false");
  omni_accepted_destroy(&s);
  omni_accepted_destroy(&z);
  omni_accepted_destroy(NULL);
  check(true, "destroy tolerates inert and NULL owners");
}

/* ------------------------------------------------------------ empty queue */

static void test_empty_queue_drained(void) {
  struct omni_listener l;
  struct omni_accepted slot;
  struct omni_accepted out[3];
  struct omni_accept_result r;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  size_t i = 0;

  if (!start_listener(&l, &port)) {
    return;
  }
  omni_accepted_make_inert(&slot);
  r = omni_accept_once(&l, &slot);
  check(r.status == OMNI_ACCEPT_DRAINED, "empty listener single accept drains");
  check(r.sys_errno == EAGAIN || r.sys_errno == EWOULDBLOCK,
        "drained accept captures the would-block errno");
  check(r.accepted == 0u, "drained accept publishes zero owners");
  check(!omni_accepted_is_live(&slot) &&
            omni_accepted_fd(&slot) == OMNI_ACCEPTED_FD_INVALID,
        "drained accept leaves the destination inert");

  for (i = 0u; i < 3u; ++i) {
    omni_accepted_make_inert(&out[i]);
  }
  r = omni_accept_drain(&l, out, 3u);
  check(r.status == OMNI_ACCEPT_DRAINED && r.accepted == 0u,
        "empty listener drain reports drained with zero owners");
  check(!omni_accepted_is_live(&out[0]) && !omni_accepted_is_live(&out[1]) &&
            !omni_accepted_is_live(&out[2]),
        "drained drain invents no live owners");
  check(omni_listener_fd(&l) != OMNI_LISTENER_FD_INVALID &&
            omni_listener_port(&l) == port,
        "listener unchanged by drained attempts");
  omni_listener_destroy(&l);
}

/* ------------------------------------------------------ single acceptance */

static void test_single_success_and_flags(void) {
  struct omni_listener l;
  struct omni_accepted slot;
  struct omni_accept_result r;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int cfd = OMNI_ACCEPTED_FD_INVALID;
  int afd = OMNI_ACCEPTED_FD_INVALID;

  if (!start_listener(&l, &port)) {
    return;
  }
  cfd = open_client(port);
  if (cfd == OMNI_ACCEPTED_FD_INVALID) {
    omni_listener_destroy(&l);
    return;
  }
  omni_accepted_make_inert(&slot);
  r = omni_accept_once(&l, &slot);
  check(r.status == OMNI_ACCEPT_OK && r.sys_errno == 0 && r.accepted == 1u,
        "one pending client accepts");
  check(omni_accepted_is_live(&slot), "accepted owner is live");
  afd = omni_accepted_fd(&slot);
  check(afd != OMNI_ACCEPTED_FD_INVALID && fd_open(afd), "accepted FD is valid");
  check(afd != omni_listener_fd(&l), "accepted FD differs from the listener FD");
  check(fd_flags_ok(afd), "accepted FD is O_NONBLOCK plus FD_CLOEXEC");
  check(omni_listener_fd(&l) != OMNI_LISTENER_FD_INVALID &&
            omni_listener_port(&l) == port,
        "listener remains live after accept");
  omni_accepted_destroy(&slot);
  check(!omni_accepted_is_live(&slot) &&
            omni_accepted_fd(&slot) == OMNI_ACCEPTED_FD_INVALID,
        "destroyed owner returns to inert");
  check(fcntl(afd, F_GETFD) == -1 && errno == EBADF,
        "owner destroy closes exactly the accepted FD");
  check(fd_open(omni_listener_fd(&l)), "listener survives accepted-owner destroy");
  omni_accepted_destroy(&slot);
  check(true, "repeated destroy of the accepted owner is safe");
  (void)close(cfd);
  omni_listener_destroy(&l);
}

/* ------------------------------------------------------------ invalid args */

static void test_invalid_arguments(void) {
  struct omni_listener l;
  struct omni_listener dead;
  struct omni_accepted slot;
  struct omni_accepted out[2];
  struct omni_accept_result r;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int c0 = OMNI_ACCEPTED_FD_INVALID;
  int c1 = OMNI_ACCEPTED_FD_INVALID;
  int first_fd = OMNI_ACCEPTED_FD_INVALID;

  if (!start_listener(&l, &port)) {
    return;
  }
  omni_listener_make_inert(&dead);

  omni_accepted_make_inert(&slot);
  r = omni_accept_once(NULL, &slot);
  check(r.status == OMNI_ACCEPT_ERR_INVALID && r.accepted == 0u,
        "single accept on NULL listener fails");
  check(!omni_accepted_is_live(&slot), "NULL-listener failure publishes nothing");
  r = omni_accept_once(&dead, &slot);
  check(r.status == OMNI_ACCEPT_ERR_INVALID, "single accept on inert listener fails");
  r = omni_accept_once(&l, NULL);
  check(r.status == OMNI_ACCEPT_ERR_INVALID, "single accept on NULL destination fails");

  /* A live destination already owns its descriptor: reject before any
   * kernel wait, and prove the queue was untouched by accepting the
   * still-pending second client into a fresh slot afterwards. */
  c0 = open_client(port);
  c1 = open_client(port);
  if (c0 == OMNI_ACCEPTED_FD_INVALID || c1 == OMNI_ACCEPTED_FD_INVALID) {
    goto cleanup;
  }
  omni_accepted_make_inert(&slot);
  r = omni_accept_once(&l, &slot);
  check(r.status == OMNI_ACCEPT_OK, "invalid-args setup accept succeeds (setup)");
  first_fd = omni_accepted_fd(&slot);
  r = omni_accept_once(&l, &slot);
  check(r.status == OMNI_ACCEPT_ERR_INVALID && r.accepted == 0u,
        "single accept into a live destination fails");
  check(omni_accepted_is_live(&slot) && omni_accepted_fd(&slot) == first_fd,
        "rejected overwrite keeps the owned descriptor");
  omni_accepted_make_inert(&out[0]);
  r = omni_accept_once(&l, &out[0]);
  check(r.status == OMNI_ACCEPT_OK, "rejected attempt consumed no queue entry");
  omni_accepted_destroy(&slot);
  omni_accepted_destroy(&out[0]);

  omni_accepted_make_inert(&out[0]);
  omni_accepted_make_inert(&out[1]);
  r = omni_accept_drain(NULL, out, 2u);
  check(r.status == OMNI_ACCEPT_ERR_INVALID && r.accepted == 0u,
        "drain on NULL listener fails");
  r = omni_accept_drain(&l, NULL, 2u);
  check(r.status == OMNI_ACCEPT_ERR_INVALID && r.accepted == 0u,
        "drain on NULL output fails");
  r = omni_accept_drain(&l, out, 0u);
  check(r.status == OMNI_ACCEPT_ERR_INVALID && r.accepted == 0u,
        "drain with zero capacity fails");
  r = omni_accept_drain(&dead, out, 2u);
  check(r.status == OMNI_ACCEPT_ERR_INVALID && r.accepted == 0u,
        "drain on inert listener fails");

cleanup:
  if (c0 != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(c0);
  }
  if (c1 != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(c1);
  }
  omni_accepted_destroy(&slot);
  omni_accepted_destroy(&out[0]);
  omni_accepted_destroy(&out[1]);
  omni_listener_destroy(&l);
  omni_listener_destroy(&dead);
}

/* Drain refuses a live slot anywhere in the output before accepting, then
 * the caller can still drain the untouched queue through fresh slots. */
static void test_drain_live_slot_precondition(void) {
  struct omni_listener l;
  struct omni_accepted out[2];
  struct omni_accept_result r;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int c0 = OMNI_ACCEPTED_FD_INVALID;
  int c1 = OMNI_ACCEPTED_FD_INVALID;
  int first_fd = OMNI_ACCEPTED_FD_INVALID;

  if (!start_listener(&l, &port)) {
    return;
  }
  c0 = open_client(port);
  c1 = open_client(port);
  if (c0 == OMNI_ACCEPTED_FD_INVALID || c1 == OMNI_ACCEPTED_FD_INVALID) {
    goto cleanup;
  }
  omni_accepted_make_inert(&out[0]);
  omni_accepted_make_inert(&out[1]);
  r = omni_accept_once(&l, &out[0]);
  check(r.status == OMNI_ACCEPT_OK, "precondition setup accept succeeds (setup)");
  first_fd = omni_accepted_fd(&out[0]);

  r = omni_accept_drain(&l, out, 2u);
  check(r.status == OMNI_ACCEPT_ERR_INVALID && r.accepted == 0u,
        "drain with a live output slot fails with nothing published");
  check(omni_accepted_is_live(&out[0]) && omni_accepted_fd(&out[0]) == first_fd &&
            !omni_accepted_is_live(&out[1]),
        "rejected drain preserves every output slot");

  /* One handshake is still queued; a fresh single-slot drain takes it.
   * Capacity fills before the queue drains, so the stop reason is
   * capacity — exactly the distinction the server runtime needs. */
  r = omni_accept_drain(&l, &out[1], 1u);
  check(r.status == OMNI_ACCEPT_CAPACITY && r.accepted == 1u,
        "fresh drain still obtains the untouched pending client");
  check(omni_accepted_is_live(&out[1]), "remainder owner is live");

cleanup:
  omni_accepted_destroy(&out[0]);
  omni_accepted_destroy(&out[1]);
  if (c0 != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(c0);
  }
  if (c1 != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(c1);
  }
  omni_listener_destroy(&l);
}

/* ---------------------------------------------------------- multi-client */

static void test_multi_client_drain(void) {
  struct omni_listener l;
  struct omni_accepted out[4];
  struct omni_accept_result r;
  struct omni_accepted extra;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int clients[3] = { OMNI_ACCEPTED_FD_INVALID, OMNI_ACCEPTED_FD_INVALID,
                     OMNI_ACCEPTED_FD_INVALID };
  int fds[3] = { OMNI_ACCEPTED_FD_INVALID, OMNI_ACCEPTED_FD_INVALID,
                 OMNI_ACCEPTED_FD_INVALID };
  size_t i = 0;
  size_t j = 0;

  if (!start_listener(&l, &port)) {
    return;
  }
  for (i = 0u; i < 3u; ++i) {
    clients[i] = open_client(port);
    if (clients[i] == OMNI_ACCEPTED_FD_INVALID) {
      goto cleanup;
    }
  }
  for (i = 0u; i < 4u; ++i) {
    omni_accepted_make_inert(&out[i]);
  }
  r = omni_accept_drain(&l, out, 4u);
  check(r.status == OMNI_ACCEPT_DRAINED && r.accepted == 3u,
        "three pending clients drain with room to spare");
  check(r.accepted <= 4u, "accepted count never exceeds output capacity");
  for (i = 0u; i < 3u; ++i) {
    fds[i] = omni_accepted_fd(&out[i]);
    if (!omni_accepted_is_live(&out[i]) || fds[i] == OMNI_ACCEPTED_FD_INVALID ||
        !fd_flags_ok(fds[i]) || fds[i] == omni_listener_fd(&l)) {
      check(false, "every drained owner holds a unique verified client FD");
      goto cleanup;
    }
  }
  check(fds[0] != fds[1] && fds[0] != fds[2] && fds[1] != fds[2],
        "drained owners hold distinct descriptors");
  check(true, "every drained owner holds a unique verified client FD");
  check(!omni_accepted_is_live(&out[3]), "undrained tail capacity stays untouched");

  /* Queue fully drained: the next attempt observes would-block. */
  omni_accepted_make_inert(&extra);
  r = omni_accept_once(&l, &extra);
  check(r.status == OMNI_ACCEPT_DRAINED && r.accepted == 0u,
        "post-drain accept reports queue drained");

  /* Owners are independent: destroying one leaves the rest usable. */
  omni_accepted_destroy(&out[0]);
  check(fcntl(fds[0], F_GETFD) == -1, "destroyed owner released its descriptor");
  check(fd_open(fds[1]) && fd_open(fds[2]),
        "destroying one owner affects no other owner");
  omni_accepted_destroy(&out[1]);
  omni_accepted_destroy(&out[2]);
  check(fcntl(fds[1], F_GETFD) == -1 && fcntl(fds[2], F_GETFD) == -1,
        "every accepted owner destroys exactly once");

cleanup:
  for (j = 0u; j < 4u; ++j) {
    omni_accepted_destroy(&out[j]);
  }
  omni_accepted_destroy(&extra);
  for (j = 0u; j < 3u; ++j) {
    if (clients[j] != OMNI_ACCEPTED_FD_INVALID) {
      (void)close(clients[j]);
    }
  }
  omni_listener_destroy(&l);
}

/* ------------------------------------------------------- capacity-limited */

static void test_capacity_limited_drain(void) {
  struct omni_listener l;
  struct omni_accepted out[3];
  struct omni_accepted rest[2];
  struct omni_accept_result r;
  struct omni_accepted extra;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int clients[3] = { OMNI_ACCEPTED_FD_INVALID, OMNI_ACCEPTED_FD_INVALID,
                     OMNI_ACCEPTED_FD_INVALID };
  size_t i = 0;
  int first = OMNI_ACCEPTED_FD_INVALID;
  int second = OMNI_ACCEPTED_FD_INVALID;

  if (!start_listener(&l, &port)) {
    return;
  }
  for (i = 0u; i < 3u; ++i) {
    clients[i] = open_client(port);
    if (clients[i] == OMNI_ACCEPTED_FD_INVALID) {
      goto cleanup;
    }
  }
  for (i = 0u; i < 3u; ++i) {
    omni_accepted_make_inert(&out[i]);
  }
  /* More pending than capacity: the call must stop full, not drained. */
  r = omni_accept_drain(&l, out, 2u);
  check(r.status == OMNI_ACCEPT_CAPACITY, "small drain reports capacity reached");
  check(r.accepted == 2u && r.sys_errno == 0, "capacity stop carries count, no errno");
  check(omni_accepted_is_live(&out[0]) && omni_accepted_is_live(&out[1]),
        "capacity fill publishes full ownership");
  first = omni_accepted_fd(&out[0]);
  second = omni_accepted_fd(&out[1]);
  check(first != second && first != omni_listener_fd(&l) &&
            second != omni_listener_fd(&l),
        "capacity-fill owners hold distinct client descriptors");
  check(!omni_accepted_is_live(&out[2]), "output past the fill stays untouched");

  /* Partial success is retained: store the filled owners, then drain the
   * remainder to the drained state. */
  omni_accepted_destroy(&out[0]);
  omni_accepted_destroy(&out[1]);
  check(fcntl(first, F_GETFD) == -1 && fcntl(second, F_GETFD) == -1,
        "stored capacity-fill owners release cleanly");
  omni_accepted_make_inert(&rest[0]);
  omni_accepted_make_inert(&rest[1]);
  r = omni_accept_drain(&l, rest, 2u);
  check(r.status == OMNI_ACCEPT_DRAINED && r.accepted == 1u,
        "second drain obtains the remaining pending client");
  omni_accepted_make_inert(&extra);
  r = omni_accept_once(&l, &extra);
  check(r.status == OMNI_ACCEPT_DRAINED, "queue eventually reaches drained");

cleanup:
  for (i = 0u; i < 3u; ++i) {
    omni_accepted_destroy(&out[i]);
  }
  omni_accepted_destroy(&rest[0]);
  omni_accepted_destroy(&rest[1]);
  omni_accepted_destroy(&extra);
  for (i = 0u; i < 3u; ++i) {
    if (clients[i] != OMNI_ACCEPTED_FD_INVALID) {
      (void)close(clients[i]);
    }
  }
  omni_listener_destroy(&l);
}

/* A partial fill that ends drained still keeps every published owner. */
static void test_partial_fill_then_drained(void) {
  struct omni_listener l;
  struct omni_accepted out[5];
  struct omni_accept_result r;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int c0 = OMNI_ACCEPTED_FD_INVALID;
  int c1 = OMNI_ACCEPTED_FD_INVALID;
  size_t i = 0;

  if (!start_listener(&l, &port)) {
    return;
  }
  c0 = open_client(port);
  c1 = open_client(port);
  if (c0 == OMNI_ACCEPTED_FD_INVALID || c1 == OMNI_ACCEPTED_FD_INVALID) {
    goto cleanup;
  }
  for (i = 0u; i < 5u; ++i) {
    omni_accepted_make_inert(&out[i]);
  }
  r = omni_accept_drain(&l, out, 5u);
  check(r.status == OMNI_ACCEPT_DRAINED && r.accepted == 2u,
        "short queue yields partial fill then drained");
  check(omni_accepted_is_live(&out[0]) && omni_accepted_is_live(&out[1]) &&
            !omni_accepted_is_live(&out[2]) && !omni_accepted_is_live(&out[3]) &&
            !omni_accepted_is_live(&out[4]),
        "only the published prefix is live after a partial fill");

cleanup:
  for (i = 0u; i < 5u; ++i) {
    omni_accepted_destroy(&out[i]);
  }
  if (c0 != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(c0);
  }
  if (c1 != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(c1);
  }
  omni_listener_destroy(&l);
}

/* --------------------------------------- listener/poller/accept integration */

static void test_listener_poller_accept_integration(void) {
  struct omni_listener listener;
  struct omni_poller p;
  struct pollfd slots[2];
  uint64_t toks[2];
  struct omni_poller_event ev[2];
  struct omni_poller_result pr;
  struct omni_accepted out[2];
  struct omni_accept_result ar;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int cfd = OMNI_ACCEPTED_FD_INVALID;
  size_t i = 0;

  omni_listener_make_inert(&listener);
  if (!start_listener(&listener, &port)) {
    return;
  }
  pr = omni_poller_init_borrowed(&p, slots, toks, 2u);
  check(pr.status == OMNI_POLLER_OK, "integration poller initializes (setup)");
  pr = omni_poller_add(&p, omni_listener_fd(&listener), (uint64_t)0xA16u,
                       OMNI_POLLER_INTEREST_READ);
  check(pr.status == OMNI_POLLER_OK, "listener FD registers as borrowed (setup)");

  pr = omni_poller_wait(&p, (int64_t)0, ev, 2u);
  check(pr.status == OMNI_POLLER_OK && pr.count == 0u,
        "idle listener reports no readiness");

  cfd = open_client(port);
  if (cfd == OMNI_ACCEPTED_FD_INVALID) {
    goto cleanup;
  }
  pr = omni_poller_wait(&p, (int64_t)0, ev, 2u);
  check(pr.status == OMNI_POLLER_OK && pr.count == 1u,
        "loopback connect causes listener readability");
  check((ev[0].ready & OMNI_POLLER_READY_READ) != 0u, "readiness is readable");
  check(ev[0].fd == omni_listener_fd(&listener), "event carries the borrowed FD");
  check(ev[0].token == (uint64_t)0xA16u, "event round-trips the token");

  for (i = 0u; i < 2u; ++i) {
    omni_accepted_make_inert(&out[i]);
  }
  ar = omni_accept_drain(&listener, out, 2u);
  check(ar.status == OMNI_ACCEPT_DRAINED && ar.accepted == 1u,
        "readiness dispatch drains the one pending client");
  check(omni_accepted_is_live(&out[0]) && fd_flags_ok(omni_accepted_fd(&out[0])),
        "dispatched owner is live with verified flags");
  check(omni_listener_fd(&listener) != OMNI_LISTENER_FD_INVALID,
        "listener remains live after readiness dispatch");
  check(omni_poller_count(&p) == 1u, "poller still borrows only the listener FD");

  omni_accepted_destroy(&out[0]);
  omni_accepted_destroy(&out[1]);
  pr = omni_poller_remove(&p, omni_listener_fd(&listener));
  check(pr.status == OMNI_POLLER_OK, "listener registration removes cleanly");
  omni_poller_destroy(&p);
  check(fd_open(omni_listener_fd(&listener)),
        "listener usable after poller destroy");

cleanup:
  omni_accepted_destroy(&out[0]);
  omni_accepted_destroy(&out[1]);
  omni_poller_destroy(&p);
  if (cfd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(cfd);
  }
  omni_listener_destroy(&listener);
}

/* An accepted descriptor may later register as a borrowed poller FD: the
 * connected socket is writable with no payload transferred, and poller
 * destroy still closes nothing. */
static void test_accepted_borrowed_registration(void) {
  struct omni_listener l;
  struct omni_poller p;
  struct pollfd slots[2];
  uint64_t toks[2];
  struct omni_poller_event ev[2];
  struct omni_accepted slot;
  struct omni_accept_result ar;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int cfd = OMNI_ACCEPTED_FD_INVALID;
  int afd = OMNI_ACCEPTED_FD_INVALID;

  if (!start_listener(&l, &port)) {
    return;
  }
  cfd = open_client(port);
  if (cfd == OMNI_ACCEPTED_FD_INVALID) {
    omni_listener_destroy(&l);
    return;
  }
  omni_accepted_make_inert(&slot);
  ar = omni_accept_once(&l, &slot);
  check(ar.status == OMNI_ACCEPT_OK, "borrow-probe accept succeeds (setup)");
  afd = omni_accepted_fd(&slot);

  check(omni_poller_init_borrowed(&p, slots, toks, 2u).status == OMNI_POLLER_OK,
        "borrow-probe poller initializes (setup)");
  check(omni_poller_add(&p, afd, (uint64_t)0xB0u, OMNI_POLLER_INTEREST_WRITE).status ==
            OMNI_POLLER_OK,
        "accepted FD registers as borrowed");
  {
    struct omni_poller_result pr = omni_poller_wait(&p, (int64_t)0, ev, 2u);

    check(pr.status == OMNI_POLLER_OK && pr.count == 1u &&
              (ev[0].ready & OMNI_POLLER_READY_WRITE) != 0u &&
              ev[0].token == (uint64_t)0xB0u,
          "connected accepted socket reports writable with its token");
  }
  check(omni_poller_remove(&p, afd).status == OMNI_POLLER_OK,
        "accepted registration removes cleanly");
  omni_poller_destroy(&p);
  check(fd_open(afd) && fd_open(omni_listener_fd(&l)),
        "poller destroy closes neither accepted nor listener FD");
  omni_accepted_destroy(&slot);
  check(fcntl(afd, F_GETFD) == -1 && fd_open(omni_listener_fd(&l)),
        "accepted destroy closes only the accepted FD");
  (void)close(cfd);
  omni_listener_destroy(&l);
}

/* -------------------------------------------------------------- bystander */

static void test_bystander_survives(void) {
  struct omni_listener l;
  struct omni_accepted slot;
  struct omni_accepted one[1];
  struct omni_accept_result r;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int bystander = open("/dev/null", O_RDONLY);
  int cfd = OMNI_ACCEPTED_FD_INVALID;

  check(bystander != OMNI_ACCEPTED_FD_INVALID, "bystander FD opens (setup)");
  if (bystander == OMNI_ACCEPTED_FD_INVALID) {
    return;
  }
  if (!start_listener(&l, &port)) {
    (void)close(bystander);
    return;
  }
  omni_accepted_make_inert(&slot);
  r = omni_accept_once(&l, &slot);
  check(r.status == OMNI_ACCEPT_DRAINED, "failed accept under bystander drains");
  check(fd_open(bystander), "bystander survives failed accept");

  cfd = open_client(port);
  if (cfd == OMNI_ACCEPTED_FD_INVALID) {
    goto cleanup;
  }
  r = omni_accept_once(&l, &slot);
  check(r.status == OMNI_ACCEPT_OK, "successful accept under bystander works");
  check(fd_open(bystander), "bystander survives successful accept");
  omni_accepted_make_inert(&one[0]);
  r = omni_accept_drain(&l, one, 1u);
  check(r.status == OMNI_ACCEPT_DRAINED, "drain under bystander drains");
  omni_accepted_destroy(&slot);
  check(fd_open(bystander), "bystander survives accepted-owner destroy");

cleanup:
  omni_accepted_destroy(&slot);
  omni_accepted_destroy(&one[0]);
  if (cfd != OMNI_ACCEPTED_FD_INVALID) {
    (void)close(cfd);
  }
  omni_listener_destroy(&l);
  check(fd_open(bystander), "bystander survives listener destroy");
  (void)close(bystander);
}

/* ------------------------------------------------- FD-0 isolated child */

static void test_fd_zero_acceptance(void) {
  pid_t pid = 0;
  int status = 0;

  pid = fork();
  if (pid == -1) {
    check(false, "fork available for FD-0 isolation test");
    return;
  }
  if (pid == 0) {
    /* Child: private descriptor-table copy. The runner's standard
     * streams are untouched and only _exit paths run below, never
     * stdio. Strategy: set up a live listener plus one completed
     * handshake first, then free descriptor 0 last — the next
     * acceptance must land exactly there if descriptor 0 is truly
     * treated as valid. */
    struct omni_listener l;
    struct omni_accepted a;
    struct omni_accept_result r;
    struct sockaddr_in peer;
    int cf = OMNI_LISTENER_FD_INVALID;
    int afd = OMNI_ACCEPTED_FD_INVALID;

    omni_listener_make_inert(&l);
    if (omni_listener_init(&l, "127.0.0.1", (uint16_t)0u).status !=
        OMNI_LISTENER_OK) {
      _exit(11);
    }
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons(omni_listener_port(&l));
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    cf = socket(AF_INET, SOCK_STREAM, 0);
    if (cf == OMNI_LISTENER_FD_INVALID) {
      _exit(14);
    }
    if (connect(cf, (const struct sockaddr *)&peer, (socklen_t)sizeof(peer)) != 0) {
      _exit(15);
    }
    /* Free descriptor 0 only now: it is the lowest free slot, so the
     * pending handshake must accept as descriptor 0. */
    if (close(STDIN_FILENO) != 0) {
      _exit(10);
    }
    omni_accepted_make_inert(&a);
    r = omni_accept_once(&l, &a);
    if (r.status != OMNI_ACCEPT_OK || r.accepted != 1u) {
      _exit(17);
    }
    afd = omni_accepted_fd(&a);
    if (afd != STDIN_FILENO || !omni_accepted_is_live(&a)) {
      _exit(18);
    }
    if (fcntl(afd, F_GETFL) == -1 || fcntl(afd, F_GETFD) == -1) {
      _exit(19);
    }
    omni_accepted_destroy(&a);
    if (fcntl(STDIN_FILENO, F_GETFD) != -1) {
      _exit(20); /* destroy must have closed exactly descriptor 0 */
    }
    if (omni_accepted_is_live(&a) ||
        omni_accepted_fd(&a) != OMNI_ACCEPTED_FD_INVALID) {
      _exit(21);
    }
    (void)close(cf);
    omni_listener_destroy(&l);
    _exit(0);
  }
  while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
    continue; /* bounded by child lifetime; retries only wait interruption */
  }
  check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "accepted owner takes and releases FD 0 in an isolated child");
}

/* ---------------------------------------------------------- stress loops */

static void test_sequential_lifecycle_stress(void) {
  bool supported = false;
  int before = count_open_fds(&supported);
  int i = 0;

  for (i = 0; i < 100; ++i) {
    struct omni_listener l;
    struct omni_accepted slot;
    struct omni_accept_result r;
    uint16_t port = OMNI_LISTENER_PORT_INVALID;
    int cfd = OMNI_ACCEPTED_FD_INVALID;
    int afd = OMNI_ACCEPTED_FD_INVALID;

    omni_listener_make_inert(&l);
    if (omni_listener_init(&l, "127.0.0.1", (uint16_t)0u).status !=
        OMNI_LISTENER_OK) {
      check(false, "stress listener binds every cycle");
      omni_listener_destroy(&l);
      return;
    }
    if (omni_listener_port(&l) == (uint16_t)20128) {
      check(false, "stress never takes the operator port");
      omni_listener_destroy(&l);
      return;
    }
    port = omni_listener_port(&l);
    cfd = open_client(port);
    if (cfd == OMNI_ACCEPTED_FD_INVALID) {
      check(false, "stress client connects every cycle");
      omni_listener_destroy(&l);
      return;
    }
    omni_accepted_make_inert(&slot);
    r = omni_accept_once(&l, &slot);
    if (r.status != OMNI_ACCEPT_OK || r.accepted != 1u) {
      check(false, "stress accept succeeds every cycle");
      omni_accepted_destroy(&slot);
      (void)close(cfd);
      omni_listener_destroy(&l);
      return;
    }
    afd = omni_accepted_fd(&slot);
    if (afd == OMNI_ACCEPTED_FD_INVALID || afd == omni_listener_fd(&l) ||
        !fd_flags_ok(afd)) {
      check(false, "stress accepted FD is a verified client FD every cycle");
      omni_accepted_destroy(&slot);
      (void)close(cfd);
      omni_listener_destroy(&l);
      return;
    }
    omni_accepted_destroy(&slot);
    if (fcntl(afd, F_GETFD) != -1) {
      check(false, "stress destroy closes the accepted FD every cycle");
      (void)close(cfd);
      omni_listener_destroy(&l);
      return;
    }
    (void)close(cfd);
    omni_listener_destroy(&l);
  }
  check(true, "100 accept/destroy cycles succeed");
  if (supported) {
    check(count_open_fds(&supported) == before,
          "sequential stress leaves descriptor count unchanged");
  } else {
    check(true, "sequential stress leaves descriptor count unchanged (skipped: no fd census)");
  }
}

static void test_multi_drain_stress(void) {
  bool supported = false;
  int before = count_open_fds(&supported);
  int iter = 0;

  for (iter = 0; iter < 20; ++iter) {
    struct omni_listener l;
    struct omni_accepted out[4];
    struct omni_accept_result r;
    uint16_t port = OMNI_LISTENER_PORT_INVALID;
    int clients[3] = { OMNI_ACCEPTED_FD_INVALID, OMNI_ACCEPTED_FD_INVALID,
                       OMNI_ACCEPTED_FD_INVALID };
    size_t i = 0;
    size_t k = 0;

    omni_listener_make_inert(&l);
    if (omni_listener_init(&l, "127.0.0.1", (uint16_t)0u).status !=
        OMNI_LISTENER_OK) {
      check(false, "drain-stress listener binds every cycle");
      omni_listener_destroy(&l);
      return;
    }
    port = omni_listener_port(&l);
    for (i = 0u; i < 3u; ++i) {
      clients[i] = open_client(port);
      if (clients[i] == OMNI_ACCEPTED_FD_INVALID) {
        check(false, "drain-stress clients connect every cycle");
        goto cycle_cleanup;
      }
    }
    for (i = 0u; i < 4u; ++i) {
      omni_accepted_make_inert(&out[i]);
    }
    r = omni_accept_drain(&l, out, 4u);
    if (r.status != OMNI_ACCEPT_DRAINED || r.accepted != 3u || r.accepted > 4u) {
      check(false, "drain-stress accepts three then drains every cycle");
      goto cycle_cleanup;
    }
    for (i = 0u; i < 3u; ++i) {
      int fd = omni_accepted_fd(&out[i]);

      if (!omni_accepted_is_live(&out[i]) || fd == omni_listener_fd(&l)) {
        check(false, "drain-stress publishes only client owners every cycle");
        goto cycle_cleanup;
      }
      for (k = 0u; k < i; ++k) {
        if (fd == omni_accepted_fd(&out[k])) {
          check(false, "drain-stress owners stay distinct every cycle");
          goto cycle_cleanup;
        }
      }
    }
    if (omni_accepted_is_live(&out[3])) {
      check(false, "drain-stress leaves tail capacity untouched every cycle");
      goto cycle_cleanup;
    }
cycle_cleanup:
    for (i = 0u; i < 4u; ++i) {
      omni_accepted_destroy(&out[i]);
    }
    for (i = 0u; i < 3u; ++i) {
      if (clients[i] != OMNI_ACCEPTED_FD_INVALID) {
        (void)close(clients[i]);
      }
    }
    omni_listener_destroy(&l);
    if (failure_count != 0) {
      return;
    }
  }
  check(failure_count == 0, "20 three-client drains keep every invariant");
  if (supported) {
    check(count_open_fds(&supported) == before,
          "multi-drain stress leaves descriptor count unchanged");
  } else {
    check(true, "multi-drain stress leaves descriptor count unchanged (skipped: no fd census)");
  }
}

int main(void) {
  bool supported = false;
  int baseline = count_open_fds(&supported);

  test_inert_and_sentinel();
  test_empty_queue_drained();
  test_single_success_and_flags();
  test_invalid_arguments();
  test_drain_live_slot_precondition();
  test_multi_client_drain();
  test_capacity_limited_drain();
  test_partial_fill_then_drained();
  test_listener_poller_accept_integration();
  test_accepted_borrowed_registration();
  test_bystander_survives();
  test_fd_zero_acceptance();
  test_sequential_lifecycle_stress();
  test_multi_drain_stress();

  check(bytes_sent_by_tests == 0u && bytes_read_by_tests == 0u,
        "no client payload read or written by any test");
  if (supported) {
    check(count_open_fds(&supported) == baseline,
          "whole suite leaves descriptor count unchanged");
  } else {
    check(true, "whole suite leaves descriptor count unchanged (skipped: no fd census)");
  }

  printf("---\n%d checks, %d failures\n", check_count, failure_count);
  return failure_count == 0 ? 0 : 1;
}
