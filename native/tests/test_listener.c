/*
 * OmniRoute native backend — TCP listener lifecycle tests (Task 014).
 *
 * Loopback-only, self-cleaning, deterministic: no external network, no
 * fixed ports (ephemeral assignment everywhere; the operator port 20128 is
 * never touched), no client payload in either direction — probes connect
 * and close only, and production code never accepts. Framework-free
 * TAP-style output with a nonzero exit on any failure. Run under CTest,
 * including the ASan + UBSan configuration.
 *
 * Two environment-sensitive helpers degrade honestly: descriptor counting
 * via /proc/self/fd is Linux-only (elsewhere those checks report skip and
 * the functional assertions still run), and the FD-0 lifecycle runs in a
 * forked child so the runner's own standard streams are never at risk.
 */

#define _GNU_SOURCE /* dirent/fork/sockets visibility under strict C11 */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "omniroute/listener.h"

/* Payload counters: this file performs no socket payload I/O by
 * construction (no send/recv-family calls), and the network-boundary gate
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

static bool fds_supported(void) {
  bool supported = false;

  (void)count_open_fds(&supported);
  return supported;
}

/* ------------------------------------------------------------------ inert */

static void test_inert_state(void) {
  struct omni_listener l;
  struct omni_listener z;

  memset(&z, 0, sizeof(z));
  omni_listener_make_inert(&l);
  check(omni_listener_fd(&l) == OMNI_LISTENER_FD_INVALID,
        "inert listener reports invalid FD");
  check(omni_listener_port(&l) == OMNI_LISTENER_PORT_INVALID,
        "inert listener reports invalid port");
  check(omni_listener_fd(&z) == OMNI_LISTENER_FD_INVALID &&
            omni_listener_port(&z) == OMNI_LISTENER_PORT_INVALID,
        "zeroed storage observes as inert");
  check(omni_listener_fd(NULL) == OMNI_LISTENER_FD_INVALID,
        "FD accessor on NULL is invalid");
  check(omni_listener_port(NULL) == OMNI_LISTENER_PORT_INVALID,
        "port accessor on NULL is invalid");
  omni_listener_destroy(&l);
  omni_listener_destroy(&z);
  omni_listener_destroy(NULL);
  check(true, "destroy tolerates inert and NULL listeners");
}

/* ---------------------------------------------------------- ephemeral bind */

static void test_ephemeral_bind(void) {
  struct omni_listener l;
  struct omni_listener_result r;

  omni_listener_make_inert(&l);
  r = omni_listener_init(&l, "127.0.0.1", (uint16_t)0u);
  check(r.status == OMNI_LISTENER_OK && r.sys_errno == 0,
        "bind to 127.0.0.1 port 0 succeeds");
  check(omni_listener_port(&l) != OMNI_LISTENER_PORT_INVALID,
        "ephemeral bind yields a nonzero actual port");
  check(omni_listener_port(&l) != (uint16_t)20128,
        "ephemeral bind never takes the operator port");
  check(omni_listener_fd(&l) != OMNI_LISTENER_FD_INVALID,
        "raw listener FD is valid after success");
  omni_listener_destroy(&l);
}

/* --------------------------------------------------------------- FD flags */

static void test_fd_flags(void) {
  struct omni_listener l;
  struct omni_listener_result r;
  int fd = OMNI_LISTENER_FD_INVALID;
  int status_flags = 0;
  int desc_flags = 0;
  int socktype = 0;
  socklen_t socktype_len = (socklen_t)sizeof(socktype);
  int listening = 0;
  socklen_t listening_len = (socklen_t)sizeof(listening);

  r = omni_listener_init(&l, "127.0.0.1", (uint16_t)0u);
  check(r.status == OMNI_LISTENER_OK, "flag-probe listener binds (setup)");
  fd = omni_listener_fd(&l);

  status_flags = fcntl(fd, F_GETFL);
  check(status_flags != -1 && (status_flags & O_NONBLOCK) != 0,
        "listener FD is O_NONBLOCK");
  desc_flags = fcntl(fd, F_GETFD);
  check(desc_flags != -1 && (desc_flags & FD_CLOEXEC) != 0,
        "listener FD is FD_CLOEXEC");
  check(getsockopt(fd, SOL_SOCKET, SO_TYPE, &socktype, &socktype_len) == 0 &&
            socktype == SOCK_STREAM,
        "listener socket type is TCP stream");
  check(getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &listening_len) == 0 &&
            listening != 0,
        "listening state is kernel-visible");
  omni_listener_destroy(&l);
}

/* ----------------------------------------------------------- bound identity */

static void test_bound_identity(void) {
  struct omni_listener l;
  struct omni_listener_result r;
  struct sockaddr_in got;
  socklen_t got_len = (socklen_t)sizeof(got);
  int fd = OMNI_LISTENER_FD_INVALID;

  r = omni_listener_init(&l, "127.0.0.1", (uint16_t)0u);
  check(r.status == OMNI_LISTENER_OK, "identity-probe listener binds (setup)");
  fd = omni_listener_fd(&l);
  memset(&got, 0, sizeof(got));
  check(getsockname(fd, (struct sockaddr *)&got, &got_len) == 0,
        "bound socket answers kernel identity query");
  check(got.sin_family == AF_INET, "bound family is IPv4");
  check(got.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "bound address is loopback");
  check(got.sin_port == htons(omni_listener_port(&l)),
        "kernel port matches the accessor (byte order correct)");
  omni_listener_destroy(&l);
}

/* --------------------------------------------------------------- destroy */

static void test_destroy_closes_exactly(void) {
  struct omni_listener l;
  struct omni_listener_result r;
  int owned = OMNI_LISTENER_FD_INVALID;
  int stable = OMNI_LISTENER_FD_INVALID;

  r = omni_listener_init(&l, "127.0.0.1", (uint16_t)0u);
  check(r.status == OMNI_LISTENER_OK, "destroy-probe listener binds (setup)");
  owned = omni_listener_fd(&l);
  stable = omni_listener_fd(&l);
  check(owned == stable, "borrowed FD accessor is stable without transfer");
  omni_listener_destroy(&l);
  check(omni_listener_fd(&l) == OMNI_LISTENER_FD_INVALID,
        "FD accessor after destroy returns invalid sentinel");
  check(omni_listener_port(&l) == OMNI_LISTENER_PORT_INVALID,
        "port accessor after destroy returns invalid value");
  check(fcntl(owned, F_GETFD) == -1 && errno == EBADF,
        "destroy closes exactly the owned listener FD");
  omni_listener_destroy(&l);
  check(true, "repeated destroy is safe");
}

/* ---------------------------------------------------------- explicit port */

static void test_explicit_port_and_rebind(void) {
  struct omni_listener a;
  struct omni_listener b;
  struct omni_listener_result r;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;

  r = omni_listener_init(&a, "127.0.0.1", (uint16_t)0u);
  check(r.status == OMNI_LISTENER_OK, "rebind setup listener binds (setup)");
  port = omni_listener_port(&a);
  check(port != OMNI_LISTENER_PORT_INVALID && port != (uint16_t)20128,
        "rebind setup port is usable and not the operator port");
  omni_listener_destroy(&a);

  r = omni_listener_init(&b, "127.0.0.1", port);
  check(r.status == OMNI_LISTENER_OK, "explicit available port bind succeeds");
  check(omni_listener_port(&b) == port, "explicit bind keeps the requested port");
  omni_listener_destroy(&b);
}

/* ------------------------------------------------------ conflict + releak */

static void test_bind_conflict(void) {
  struct omni_listener a;
  struct omni_listener b;
  struct omni_listener c;
  struct omni_listener_result r;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  bool supported = false;
  int before = 0;
  int after = 0;

  r = omni_listener_init(&a, "127.0.0.1", (uint16_t)0u);
  check(r.status == OMNI_LISTENER_OK, "conflict holder binds (setup)");
  port = omni_listener_port(&a);

  before = count_open_fds(&supported);
  omni_listener_make_inert(&b);
  r = omni_listener_init(&b, "127.0.0.1", port);
  after = count_open_fds(&supported);
  check(r.status == OMNI_LISTENER_ERR_BIND, "duplicate bind fails cleanly");
  check(r.sys_errno != 0, "duplicate bind captures errno");
  check(omni_listener_fd(&b) == OMNI_LISTENER_FD_INVALID &&
            omni_listener_port(&b) == OMNI_LISTENER_PORT_INVALID,
        "failed duplicate bind leaves the listener inert");
  if (supported) {
    check(after == before, "failed duplicate bind leaks no FD");
  } else {
    check(true, "failed duplicate bind leaks no FD (skipped: no fd census)");
  }

  /* Restart semantics: once the holder is gone the port binds again. */
  omni_listener_destroy(&a);
  r = omni_listener_init(&c, "127.0.0.1", port);
  check(r.status == OMNI_LISTENER_OK, "port rebinds after holder destroy");
  check(omni_listener_port(&c) == port, "rebound port matches");
  omni_listener_destroy(&c);
}

/* -------------------------------------------------------- invalid address */

static void test_invalid_addresses(void) {
  static const char *const bad[] = {
    NULL, "", "not-an-ip", "256.300.1.1", "0.0.0.0", "192.0.2.1", "::1", "127.0.0.1:80"
  };
  size_t i = 0;
  bool supported = fds_supported();
  int before = count_open_fds(&supported);

  for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    struct omni_listener l;
    struct omni_listener_result r;

    omni_listener_make_inert(&l);
    r = omni_listener_init(&l, bad[i], (uint16_t)0u);
    if (r.status != OMNI_LISTENER_ERR_INVALID ||
        omni_listener_fd(&l) != OMNI_LISTENER_FD_INVALID ||
        omni_listener_port(&l) != OMNI_LISTENER_PORT_INVALID) {
      check(false, "invalid bind address fails cleanly and stays inert");
      omni_listener_destroy(&l);
      return;
    }
    omni_listener_destroy(&l);
  }
  check(true, "invalid bind address fails cleanly and stays inert");
  if (supported) {
    int after = count_open_fds(&supported);

    check(after == before, "invalid addresses create no descriptor");
  } else {
    check(true, "invalid addresses create no descriptor (skipped: no fd census)");
  }
  check(omni_listener_init(NULL, "127.0.0.1", (uint16_t)0u).status ==
            OMNI_LISTENER_ERR_INVALID,
        "init on NULL listener fails without crashing");
}

/* -------------------------------------------------------------- connect */

static void test_connect_probe(void) {
  struct omni_listener l;
  struct omni_listener_result r;
  struct sockaddr_in peer;
  uint16_t port = OMNI_LISTENER_PORT_INVALID;
  int cfd = OMNI_LISTENER_FD_INVALID;

  r = omni_listener_init(&l, "127.0.0.1", (uint16_t)0u);
  check(r.status == OMNI_LISTENER_OK, "probe listener binds (setup)");
  port = omni_listener_port(&l);

  cfd = socket(AF_INET, SOCK_STREAM, 0);
  check(cfd != OMNI_LISTENER_FD_INVALID, "probe client socket created");
  memset(&peer, 0, sizeof(peer));
  peer.sin_family = AF_INET;
  peer.sin_port = htons(port);
  peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  check(connect(cfd, (const struct sockaddr *)&peer, (socklen_t)sizeof(peer)) == 0,
        "loopback connect reaches the listener backlog");
  /* No payload either way: the queued handshake is torn down here and the
   * listener never accepts. Production code has no path that could have
   * touched these bytes (network-boundary gate). */
  check(bytes_sent_by_tests == 0u && bytes_read_by_tests == 0u,
        "probe transfers zero payload bytes");
  (void)close(cfd);
  check(omni_listener_fd(&l) != OMNI_LISTENER_FD_INVALID &&
            omni_listener_port(&l) == port,
        "listener survives the connect probe unchanged");
  omni_listener_destroy(&l);
}

/* -------------------------------------------------------- bystander FD */

static void test_bystander_survives(void) {
  struct omni_listener l;
  struct omni_listener_result r;
  int bystander = open("/dev/null", O_RDONLY);

  check(bystander != OMNI_LISTENER_FD_INVALID, "bystander FD opens (setup)");
  r = omni_listener_init(&l, "127.0.0.1", (uint16_t)0u);
  check(r.status == OMNI_LISTENER_OK, "bystander-probe listener binds (setup)");
  omni_listener_destroy(&l);
  check(fcntl(bystander, F_GETFD) != -1, "unrelated bystander FD survives destroy");
  (void)close(bystander);
}

/* ------------------------------------------------- FD-0 isolated child */

static void test_fd_zero_lifecycle(void) {
  pid_t pid = 0;
  int status = 0;

  pid = fork();
  if (pid == -1) {
    check(false, "fork available for FD-0 isolation test");
    return;
  }
  if (pid == 0) {
    /* Child: private descriptor-table copy. Standard streams of the
     * runner are untouched; only _exit paths below, never stdio. */
    struct omni_listener l;
    bool ok = false;

    if (close(STDIN_FILENO) != 0) {
      _exit(10);
    }
    omni_listener_make_inert(&l);
    if (omni_listener_init(&l, "127.0.0.1", (uint16_t)0u).status != OMNI_LISTENER_OK) {
      _exit(11);
    }
    /* Descriptor 0 is valid and must not be confused with invalid. */
    ok = (omni_listener_fd(&l) == STDIN_FILENO) &&
         (omni_listener_port(&l) != OMNI_LISTENER_PORT_INVALID);
    omni_listener_destroy(&l);
    ok = ok && (omni_listener_fd(&l) == OMNI_LISTENER_FD_INVALID);
    if (fcntl(STDIN_FILENO, F_GETFD) != -1) {
      _exit(13); /* destroy must have closed exactly fd 0 */
    }
    _exit(ok ? 0 : 12);
  }
  while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
    continue; /* bounded by child lifetime; retries only wait interruption */
  }
  check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "listener owns and releases FD 0 in an isolated child");
}

/* ---------------------------------------------------------- cycle stress */

static void test_sequential_cycles(void) {
  int i = 0;

  for (i = 0; i < 50; ++i) {
    struct omni_listener l;
    struct omni_listener_result r = omni_listener_init(&l, "127.0.0.1", (uint16_t)0u);

    if (r.status != OMNI_LISTENER_OK ||
        omni_listener_port(&l) == OMNI_LISTENER_PORT_INVALID ||
        omni_listener_fd(&l) == OMNI_LISTENER_FD_INVALID) {
      check(false, "sequential create/destroy cycles succeed");
      omni_listener_destroy(&l);
      return;
    }
    omni_listener_destroy(&l);
  }
  check(true, "sequential create/destroy cycles succeed");
}

static void test_descriptor_leak_stress(void) {
  bool supported = false;
  int before = count_open_fds(&supported);
  int i = 0;

  if (!supported) {
    check(true, "descriptor leak stress (skipped: no fd census)");
    return;
  }
  for (i = 0; i < 200; ++i) {
    struct omni_listener l;
    struct omni_listener_result r = omni_listener_init(&l, "127.0.0.1", (uint16_t)0u);

    if (r.status != OMNI_LISTENER_OK) {
      check(false, "descriptor leak stress binds every cycle");
      omni_listener_destroy(&l);
      return;
    }
    if (omni_listener_fd(&l) == OMNI_LISTENER_FD_INVALID) {
      check(false, "descriptor leak stress holds a valid FD every cycle");
      omni_listener_destroy(&l);
      return;
    }
    omni_listener_destroy(&l);
  }
  check(count_open_fds(&supported) == before,
        "200 lifecycles leave descriptor count unchanged");
}

int main(void) {
  test_inert_state();
  test_ephemeral_bind();
  test_fd_flags();
  test_bound_identity();
  test_destroy_closes_exactly();
  test_explicit_port_and_rebind();
  test_bind_conflict();
  test_invalid_addresses();
  test_connect_probe();
  test_bystander_survives();
  test_fd_zero_lifecycle();
  test_sequential_cycles();
  test_descriptor_leak_stress();

  check(bytes_sent_by_tests == 0u && bytes_read_by_tests == 0u,
        "no client payload read or written by any test");

  printf("---\n%d checks, %d failures\n", check_count, failure_count);
  return failure_count == 0 ? 0 : 1;
}
