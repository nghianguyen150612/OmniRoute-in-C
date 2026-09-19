/*
 * OmniRoute native backend — accepted-socket owner + bounded accept4 drain.
 *
 * Conversion strategy: descriptors stay in int end to end and only the
 * value -1 is special, so descriptor 0 flows through every path as an
 * ordinary valid value. Drain counts stay in size_t and the loop bound is
 * the caller capacity itself, so no count conversion can overflow and the
 * published total never exceeds the provided storage. Errno is captured
 * into the result before any cleanup call can overwrite it.
 *
 * EINTR audit: the single acceptance attempt is never retried internally —
 * interruption surfaces as INTERRUPTED and the caller decides whether to
 * continue draining. The destroy path performs a single close that is never
 * retried: on Linux the descriptor is released even when the close reports
 * interruption, and a retry could release an unrelated descriptor recycled
 * into the same number. No handler is installed by this backend.
 *
 * Transient audit: ECONNABORTED and EPROTO name a failed pending handshake
 * rather than a broken listener, so they surface as TRANSIENT with nothing
 * published for the attempt; the failed handshake is consumed and the
 * caller may drain again at once. EAGAIN and EWOULDBLOCK on the
 * nonblocking listener mean the queue is drained — normal control flow.
 * Every other failure stops the call as fatal with its errno preserved;
 * the transient set stays at these two names because only they carry
 * per-connection-abort evidence at this layer.
 *
 * Publication audit: ownership escapes exactly once per success, after the
 * new descriptor verifies nonblocking and close-on-exec. The
 * success-but-unverified path closes the new descriptor exactly once and
 * leaves the destination inert. Failure paths publish nothing.
 *
 * Feature macro: _GNU_SOURCE widens system-header visibility only
 * (accept4 and its atomic creation flags on glibc). It changes nothing
 * about the strict warning level applied to this file.
 */

#define _GNU_SOURCE

#include "omniroute/accepted.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

static void mark_inert_fields(struct omni_accepted *slot) {
  slot->fd = OMNI_ACCEPTED_FD_INVALID;
  slot->live = false;
}

static struct omni_accept_result make_result(enum omni_accept_status status, int err,
                                             size_t accepted) {
  struct omni_accept_result out;

  out.status = status;
  out.sys_errno = err;
  out.accepted = accepted;
  return out;
}

/*
 * Classify the failure of one acceptance attempt. Queue-drained and
 * interruption are control flow; the two transient names cover a failed
 * pending handshake; everything else stops the caller as fatal so a future
 * server layer can apply its own backoff or abort policy.
 */
static enum omni_accept_status classify_attempt_errno(int err) {
  if (err == EAGAIN || err == EWOULDBLOCK) {
    return OMNI_ACCEPT_DRAINED;
  }
  if (err == EINTR) {
    return OMNI_ACCEPT_INTERRUPTED;
  }
  if (err == ECONNABORTED || err == EPROTO) {
    return OMNI_ACCEPT_TRANSIENT;
  }
  return OMNI_ACCEPT_ERR_FATAL;
}

/*
 * Creation requests atomic nonblocking plus close-on-exec flags; this
 * verifies they actually hold on the new descriptor. Fails closed: any
 * verification failure tears the descriptor down instead of publishing a
 * client descriptor with the wrong blocking or inheritance behavior. The
 * failure errno is reported precisely: a probing-call failure carries its
 * own errno, while present-but-wrong flags carry EIO since no probing call
 * failed.
 */
static bool new_fd_state_ok(int fd, int *err_out) {
  int status_flags = 0;
  int desc_flags = 0;

  status_flags = fcntl(fd, F_GETFL);
  if (status_flags == -1) {
    *err_out = errno;
    return false;
  }
  if ((status_flags & O_NONBLOCK) == 0) {
    *err_out = EIO;
    return false;
  }
  desc_flags = fcntl(fd, F_GETFD);
  if (desc_flags == -1) {
    *err_out = errno;
    return false;
  }
  if ((desc_flags & FD_CLOEXEC) == 0) {
    *err_out = EIO;
    return false;
  }
  return true;
}

void omni_accepted_make_inert(struct omni_accepted *slot) {
  if (slot == NULL) {
    return;
  }
  mark_inert_fields(slot);
}

int omni_accepted_fd(const struct omni_accepted *slot) {
  if (slot == NULL || !slot->live) {
    return OMNI_ACCEPTED_FD_INVALID;
  }
  return slot->fd;
}

bool omni_accepted_is_live(const struct omni_accepted *slot) {
  if (slot == NULL) {
    return false;
  }
  return slot->live;
}

void omni_accepted_destroy(struct omni_accepted *slot) {
  if (slot == NULL || !slot->live) {
    return;
  }
  if (slot->fd != OMNI_ACCEPTED_FD_INVALID) {
    /* Single close, never retried; see the policy note at the top. */
    (void)close(slot->fd);
  }
  mark_inert_fields(slot);
}

struct omni_accept_result omni_accept_once(const struct omni_listener *listener,
                                           struct omni_accepted *slot) {
  int listener_fd = OMNI_LISTENER_FD_INVALID;
  int client_fd = OMNI_ACCEPTED_FD_INVALID;
  int verify_err = 0;

  if (listener == NULL || slot == NULL) {
    return make_result(OMNI_ACCEPT_ERR_INVALID, EINVAL, 0u);
  }
  /* Borrowed descriptor only; listener ownership never moves. */
  listener_fd = omni_listener_fd(listener);
  if (listener_fd == OMNI_LISTENER_FD_INVALID) {
    return make_result(OMNI_ACCEPT_ERR_INVALID, EINVAL, 0u);
  }
  /* A live destination already owns a descriptor; overwriting it would
   * lose that descriptor. Reject before any kernel wait happens. */
  if (slot->live) {
    return make_result(OMNI_ACCEPT_ERR_INVALID, EINVAL, 0u);
  }
  /* Atomic flags first: no transient blocking or inheritable state. No
   * peer capture: the handshake identity is unneeded at this layer. */
  client_fd = accept4(listener_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (client_fd == OMNI_ACCEPTED_FD_INVALID) {
    int err = errno;

    return make_result(classify_attempt_errno(err), err, 0u);
  }
  /* Only values below zero fail; descriptor 0 is a valid acceptance. */
  if (!new_fd_state_ok(client_fd, &verify_err)) {
    /* Capture first: the cleanup close must not overwrite the report. */
    (void)close(client_fd);
    mark_inert_fields(slot);
    return make_result(OMNI_ACCEPT_ERR_FATAL, verify_err, 0u);
  }
  /* Publish success only now: the owned descriptor escapes exactly here. */
  slot->fd = client_fd;
  slot->live = true;
  return make_result(OMNI_ACCEPT_OK, 0, 1u);
}

struct omni_accept_result omni_accept_drain(const struct omni_listener *listener,
                                            struct omni_accepted *slot,
                                            size_t out_capacity) {
  size_t accepted = 0u;
  size_t i = 0u;

  if (listener == NULL || slot == NULL || out_capacity == 0u) {
    return make_result(OMNI_ACCEPT_ERR_INVALID, EINVAL, 0u);
  }
  if (omni_listener_fd(listener) == OMNI_LISTENER_FD_INVALID) {
    return make_result(OMNI_ACCEPT_ERR_INVALID, EINVAL, 0u);
  }
  /* Precondition scan before any kernel wait: a stray live owner in the
   * output would lose its descriptor on overwrite, so refuse the whole
   * call with nothing published instead. */
  for (i = 0u; i < out_capacity; ++i) {
    if (slot[i].live) {
      return make_result(OMNI_ACCEPT_ERR_INVALID, EINVAL, 0u);
    }
  }
  for (i = 0u; i < out_capacity; ++i) {
    struct omni_accept_result one = omni_accept_once(listener, &slot[i]);

    if (one.status == OMNI_ACCEPT_OK) {
      accepted += 1u;
      continue;
    }
    /* Queue drained, interrupted, transient, or fatal: stop with the
     * terminal reason while the caller keeps every owner published so
     * far. Only the first accepted entries are live; the remaining
     * capacity is left untouched. */
    return make_result(one.status, one.sys_errno, accepted);
  }
  /* Output filled before the queue drained: pending handshakes may
   * remain, so report capacity distinctly from drained. */
  return make_result(OMNI_ACCEPT_CAPACITY, 0, accepted);
}
