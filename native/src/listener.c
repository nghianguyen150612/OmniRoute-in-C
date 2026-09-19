/*
 * OmniRoute native backend — Linux-first TCP listener implementation.
 *
 * Overflow and conversion strategy: ports are uint16_t end to end, so no
 * range check can overflow — 0 means kernel-assigned, anything else fits
 * in_port_t exactly. Length arguments to the socket layer are formed by an
 * explicit narrowing cast from sizeof, on values far below SOCKLEN limits.
 * Every descriptor number stays in int; only -1 is special.
 *
 * EINTR audit: the one-shot setup calls that may report interruption on
 * Linux (bind, listen, getsockname) run through small bounded-retry
 * helpers. setsockopt for SO_REUSEADDR is a non-blocking option store
 * with no documented interruption path, so it is called once and any
 * failure surfaces immediately. No handler is installed by this backend,
 * so interruption is near-impossible; the bound exists so a pathological
 * signal storm fails setup cleanly instead of spinning. close is never
 * retried: on Linux the descriptor is released even when close reports
 * EINTR, and a retry could release an unrelated descriptor recycled into
 * the same number.
 *
 * Feature macro: _DEFAULT_SOURCE widens system-header visibility only
 * (atomic SOCK_NONBLOCK/SOCK_CLOEXEC on older glibc). It changes nothing
 * about the strict warning level applied to this file.
 */

#define _DEFAULT_SOURCE

#include "omniroute/listener.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Bounded interruption retries for one-shot setup calls (see above). */
#define OMNI_SETUP_ATTEMPTS 8

static void mark_inert_fields(struct omni_listener *listener) {
  listener->fd = OMNI_LISTENER_FD_INVALID;
  listener->port = OMNI_LISTENER_PORT_INVALID;
  listener->live = false;
}

/*
 * Parse dotted-decimal IPv4 text and enforce the loopback-only bind
 * policy: only 127.0.0.0/8 is accepted. Returns false for NULL or empty
 * input, unparsable text, or any non-loopback address.
 */
static bool parse_loopback(const char *text, struct in_addr *out) {
  uint32_t host = 0;
  struct in_addr v4;

  if (text == NULL || text[0] == '\0' || out == NULL) {
    return false;
  }
  memset(&v4, 0, sizeof(v4));
  if (inet_pton(AF_INET, text, &v4) != 1) {
    return false;
  }
  host = ntohl(v4.s_addr);
  if ((host >> 24u) != 127u) {
    return false;
  }
  *out = v4;
  return true;
}

/*
 * Creation requests atomic nonblocking + close-on-exec flags; this
 * verifies they actually hold on the live descriptor. Fails closed: any
 * verification failure tears the descriptor down instead of serving a
 * descriptor with the wrong blocking or inheritance behavior.
 */
static bool fd_state_ok(int fd) {
  int status_flags = 0;
  int desc_flags = 0;

  status_flags = fcntl(fd, F_GETFL);
  if (status_flags == -1 || (status_flags & O_NONBLOCK) == 0) {
    return false;
  }
  desc_flags = fcntl(fd, F_GETFD);
  if (desc_flags == -1 || (desc_flags & FD_CLOEXEC) == 0) {
    return false;
  }
  return true;
}

static int run_bind(int fd, const struct sockaddr_in *addr) {
  int attempt = 0;

  for (attempt = 0; attempt < OMNI_SETUP_ATTEMPTS; ++attempt) {
    if (bind(fd, (const struct sockaddr *)addr, (socklen_t)sizeof(*addr)) == 0) {
      return 0;
    }
    if (errno != EINTR) {
      return -1;
    }
  }
  return -1;
}

static int run_listen(int fd) {
  int attempt = 0;

  for (attempt = 0; attempt < OMNI_SETUP_ATTEMPTS; ++attempt) {
    if (listen(fd, OMNI_LISTENER_BACKLOG) == 0) {
      return 0;
    }
    if (errno != EINTR) {
      return -1;
    }
  }
  return -1;
}

static int run_getsockname(int fd, struct sockaddr_in *addr, socklen_t *len) {
  int attempt = 0;

  for (attempt = 0; attempt < OMNI_SETUP_ATTEMPTS; ++attempt) {
    if (getsockname(fd, (struct sockaddr *)addr, len) == 0) {
      return 0;
    }
    if (errno != EINTR) {
      return -1;
    }
  }
  return -1;
}

/*
 * Capture errno first, then release the setup-owned descriptor and reset
 * state. The cleanup close cannot overwrite the reported failure: the
 * value is already stored.
 */
static struct omni_listener_result fail_at(struct omni_listener *listener,
                                           enum omni_listener_status status, int fd) {
  struct omni_listener_result out;

  out.status = status;
  out.sys_errno = errno;
  if (fd != OMNI_LISTENER_FD_INVALID) {
    (void)close(fd);
  }
  mark_inert_fields(listener);
  return out;
}

void omni_listener_make_inert(struct omni_listener *listener) {
  if (listener == NULL) {
    return;
  }
  mark_inert_fields(listener);
}

struct omni_listener_result omni_listener_init(struct omni_listener *listener,
                                               const char *address,
                                               uint16_t port) {
  struct omni_listener_result ok = { OMNI_LISTENER_OK, 0 };
  struct sockaddr_in bound;
  struct sockaddr_in actual;
  socklen_t actual_len = 0;
  struct in_addr v4;
  uint16_t actual_port = 0;
  int fd = OMNI_LISTENER_FD_INVALID;
  int reuse = 0;

  if (listener == NULL) {
    struct omni_listener_result invalid = { OMNI_LISTENER_ERR_INVALID, EINVAL };

    return invalid;
  }
  /* Define caller storage first: every failure below, including argument
   * validation, leaves the listener inert rather than untouched. */
  mark_inert_fields(listener);
  if (!parse_loopback(address, &v4)) {
    struct omni_listener_result invalid = { OMNI_LISTENER_ERR_INVALID, EINVAL };

    return invalid;
  }

  /* Atomic flags first: no transient blocking or inheritable state. Older
   * kernels without these flags fail here with EINVAL and surface as
   * ERR_SOCKET; verification below would fail closed regardless. */
  fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd == OMNI_LISTENER_FD_INVALID) {
    return fail_at(listener, OMNI_LISTENER_ERR_SOCKET, fd);
  }
  if (!fd_state_ok(fd)) {
    return fail_at(listener, OMNI_LISTENER_ERR_OPTION, fd);
  }
  reuse = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, (socklen_t)sizeof(reuse)) != 0) {
    return fail_at(listener, OMNI_LISTENER_ERR_OPTION, fd);
  }
  memset(&bound, 0, sizeof(bound));
  bound.sin_family = AF_INET;
  bound.sin_port = htons(port);
  bound.sin_addr = v4;
  if (run_bind(fd, &bound) != 0) {
    return fail_at(listener, OMNI_LISTENER_ERR_BIND, fd);
  }
  if (run_listen(fd) != 0) {
    return fail_at(listener, OMNI_LISTENER_ERR_LISTEN, fd);
  }
  /* Actual bound identity comes from kernel state, never from the request:
   * ephemeral (0) assignments resolve here, and the byte-order conversion
   * is validated by tests binding explicit ports. */
  memset(&actual, 0, sizeof(actual));
  actual_len = (socklen_t)sizeof(actual);
  if (run_getsockname(fd, &actual, &actual_len) != 0) {
    return fail_at(listener, OMNI_LISTENER_ERR_ADDR, fd);
  }
  if (actual.sin_family != AF_INET) {
    errno = EAFNOSUPPORT;
    return fail_at(listener, OMNI_LISTENER_ERR_ADDR, fd);
  }
  actual_port = ntohs(actual.sin_port);
  /* Publish success only now: the owned descriptor escapes exactly here. */
  listener->fd = fd;
  listener->port = actual_port;
  listener->live = true;
  return ok;
}

void omni_listener_destroy(struct omni_listener *listener) {
  if (listener == NULL || !listener->live) {
    return;
  }
  if (listener->fd != OMNI_LISTENER_FD_INVALID) {
    /* Single close, never retried; see the policy note at the top. */
    (void)close(listener->fd);
  }
  mark_inert_fields(listener);
}

int omni_listener_fd(const struct omni_listener *listener) {
  if (listener == NULL || !listener->live) {
    return OMNI_LISTENER_FD_INVALID;
  }
  return listener->fd;
}

uint16_t omni_listener_port(const struct omni_listener *listener) {
  if (listener == NULL || !listener->live) {
    return OMNI_LISTENER_PORT_INVALID;
  }
  return listener->port;
}
