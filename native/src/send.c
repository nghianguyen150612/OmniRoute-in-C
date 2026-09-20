/*
 * OmniRoute native backend — bounded nonblocking socket send.
 *
 * Conversion strategy: the caller's size_t length is capped at SSIZE_MAX
 * before each syscall, so the positive ssize_t result is compared against a
 * safely representable request before conversion to size_t. A drain's local
 * total is proven not to exceed the original length before every subtraction
 * and byte-pointer advance; the explicit overflow guard keeps that proof
 * fail-closed if the contract is ever changed.
 *
 * EINTR audit: one-shot send never retries, and the bounded drain stops on
 * the first interruption. EAGAIN/EWOULDBLOCK are normal would-block control
 * flow. EPIPE and ECONNRESET are surfaced as peer-closed failures; every
 * other failed syscall preserves errno under ERR_FATAL. A zero return for a
 * nonzero request has no defined errno, so it is an explicit zero-progress
 * internal outcome with EIO and no retry.
 *
 * SIGPIPE audit: Linux MSG_NOSIGNAL is supplied on every production send
 * syscall. No signal handler or process-wide disposition is installed. A
 * future non-Linux portability layer must replace this per-call mechanism
 * with an equivalent safe policy before this source is widened.
 *
 * Borrowing audit: this file has no descriptor lifecycle, flag, poller, or
 * allocator calls. The accepted owner and source bytes remain caller-owned;
 * no pointer or offset escapes the synchronous function call.
 *
 * Feature macro: _GNU_SOURCE exposes MSG_NOSIGNAL on the Linux-first glibc
 * baseline. The compile-time check below fails closed rather than silently
 * permitting an unsafe plain send on a platform without that mechanism.
 */

#define _GNU_SOURCE

#include "omniroute/send.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifndef MSG_NOSIGNAL
#error "Task 018 requires a per-call SIGPIPE-safe send flag"
#endif

static struct omni_send_result make_result(enum omni_send_status status, int err,
                                           size_t sent) {
  struct omni_send_result out;

  out.status = status;
  out.sys_errno = err;
  out.sent = sent;
  return out;
}

static enum omni_send_status classify_attempt_errno(int err) {
  if (err == EAGAIN || err == EWOULDBLOCK) {
    return OMNI_SEND_WOULD_BLOCK;
  }
  if (err == EINTR) {
    return OMNI_SEND_INTERRUPTED;
  }
  if (err == EPIPE || err == ECONNRESET) {
    return OMNI_SEND_PEER_CLOSED;
  }
  return OMNI_SEND_ERR_FATAL;
}

struct omni_send_result omni_send_once(const struct omni_accepted *conn,
                                       struct omni_send_span span) {
  int fd = OMNI_ACCEPTED_FD_INVALID;
  const unsigned char *source = NULL;
  size_t request = 0u;
  ssize_t produced = 0;

  if (conn == NULL) {
    return make_result(OMNI_SEND_ERR_INVALID, EINVAL, 0u);
  }
  /* Borrowed descriptor only; accepted ownership never moves. */
  fd = omni_accepted_fd(conn);
  if (fd == OMNI_ACCEPTED_FD_INVALID) {
    return make_result(OMNI_SEND_ERR_INVALID, EINVAL, 0u);
  }
  if (span.data == NULL && span.length != 0u) {
    return make_result(OMNI_SEND_ERR_INVALID, EINVAL, 0u);
  }
  if (span.length == 0u) {
    /* Successful no-op: do not probe connection state with a zero send. */
    return make_result(OMNI_SEND_COMPLETE, 0, 0u);
  }
  /* The nonzero-length validation above proves this cast is non-NULL. */
  source = (const unsigned char *)span.data;
  request = span.length;
  if (request > (size_t)SSIZE_MAX) {
    request = (size_t)SSIZE_MAX;
  }
  /* Linux per-call SIGPIPE suppression; accepted FDs are already nonblocking. */
  produced = send(fd, source, request, MSG_NOSIGNAL);
  if (produced > 0) {
    /* The request cast is safe because request <= SSIZE_MAX. */
    if (produced > (ssize_t)request) {
      return make_result(OMNI_SEND_ERR_INTERNAL, EIO, 0u);
    }
    return make_result(OMNI_SEND_PROGRESS, 0, (size_t)produced);
  }
  if (produced == 0) {
    /* A nonzero request must not be reported as completed or progress. */
    return make_result(OMNI_SEND_ERR_ZERO_PROGRESS, EIO, 0u);
  }
  {
    int err = errno;

    return make_result(classify_attempt_errno(err), err, 0u);
  }
}

struct omni_send_result omni_send_drain(const struct omni_accepted *conn,
                                        struct omni_send_span span,
                                        size_t max_calls) {
  const unsigned char *source = NULL;
  size_t total = 0u;
  size_t i = 0u;

  if (conn == NULL || max_calls == 0u) {
    return make_result(OMNI_SEND_ERR_INVALID, EINVAL, 0u);
  }
  if (omni_accepted_fd(conn) == OMNI_ACCEPTED_FD_INVALID) {
    return make_result(OMNI_SEND_ERR_INVALID, EINVAL, 0u);
  }
  if (span.data == NULL && span.length != 0u) {
    return make_result(OMNI_SEND_ERR_INVALID, EINVAL, 0u);
  }
  if (span.length == 0u) {
    return make_result(OMNI_SEND_COMPLETE, 0, 0u);
  }
  /* The nonzero-length validation above proves this cast is non-NULL. */
  source = (const unsigned char *)span.data;
  for (i = 0u; i < max_calls; ++i) {
    size_t remaining = 0u;
    struct omni_send_span tail;
    struct omni_send_result one;

    /* This guard proves both subtraction and pointer arithmetic below. */
    if (total > span.length) {
      return make_result(OMNI_SEND_ERR_INTERNAL, EIO, total);
    }
    remaining = span.length - total;
    if (remaining == 0u) {
      return make_result(OMNI_SEND_COMPLETE, 0, total);
    }
    tail.data = source + total;
    tail.length = remaining;
    one = omni_send_once(conn, tail);
    if (one.status != OMNI_SEND_PROGRESS) {
      return make_result(one.status, one.sys_errno, total);
    }
    /* Positive progress must fit the remaining span and the total. */
    if (one.sent == 0u || one.sent > remaining || one.sent > SIZE_MAX - total) {
      return make_result(OMNI_SEND_ERR_INTERNAL, EIO, total);
    }
    total += one.sent;
    if (total == span.length) {
      return make_result(OMNI_SEND_COMPLETE, 0, total);
    }
  }
  /* The finite syscall budget ended while an unsent tail remains. */
  return make_result(OMNI_SEND_LIMIT_REACHED, 0, total);
}
