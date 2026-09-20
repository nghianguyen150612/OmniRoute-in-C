/*
 * OmniRoute native backend — bounded nonblocking socket-send primitive
 * (Task 018).
 *
 * This is the first production socket payload output primitive: one
 * allocation-free layer sends bytes from a caller-owned immutable span
 * through one live accepted nonblocking socket. It covers output bytes only
 * — no output queue, no byte buffer repurposing, no response object, no HTTP,
 * and no server loop.
 *
 * Ownership split: the send layer borrows the accepted owner and borrows the
 * source span. The accepted owner keeps sole descriptor ownership; this
 * layer never closes, destroys, reconfigures, transfers, or retains the FD.
 * The caller keeps the source bytes alive and unchanged for the synchronous
 * call; this layer never allocates, copies, mutates, or retains the source
 * pointer, length, or offset after return. The poller is independent: send
 * never registers, removes, or updates readiness entries.
 *
 * Mechanism: one nonblocking socket send per one-shot call, with the Linux
 * MSG_NOSIGNAL per-call flag. A future portability layer must provide an
 * equivalent SIGPIPE-safe mechanism where that Linux flag is unavailable;
 * this module deliberately does not install a process-wide signal policy.
 * Accepted descriptors already carry O_NONBLOCK, so this layer never calls
 * fcntl or adds a per-call wait override.
 *
 * Counts are authoritative and binary-safe: no NUL termination, text,
 * encoding, HTTP, or string assumption exists. A positive return is reported
 * as PROGRESS even when it is partial. A zero-length span is a successful
 * COMPLETE no-op and performs no kernel call. A nonzero span with a zero
 * syscall result is an explicit internal zero-progress outcome, never full
 * success.
 *
 * The bounded drain advances through the span only with local arithmetic and
 * performs at most max_calls one-shot attempts. It returns COMPLETE when the
 * span is exhausted, LIMIT_REACHED when the call budget ends with bytes left,
 * or the terminal one-shot status with all prior progress preserved.
 */

#ifndef OMNIROUTE_SEND_H
#define OMNIROUTE_SEND_H

#include <stddef.h>

#include "omniroute/accepted.h"

/*
 * Minimal immutable byte span. The pointer may be NULL only when length is
 * zero. The caller owns the storage and keeps it valid for the synchronous
 * send call; the send layer retains no reference after return.
 */
struct omni_send_span {
  const void *data;
  size_t length;
};

enum omni_send_status {
  OMNI_SEND_PROGRESS = 0,     /* one positive syscall result, possibly partial */
  OMNI_SEND_COMPLETE,         /* zero-length one-shot or fully sent drain */
  OMNI_SEND_WOULD_BLOCK,      /* no progress for this attempt: normal control flow */
  OMNI_SEND_INTERRUPTED,      /* EINTR: no internal retry */
  OMNI_SEND_PEER_CLOSED,      /* EPIPE or ECONNRESET, errno preserved */
  OMNI_SEND_ERR_ZERO_PROGRESS, /* nonzero request unexpectedly returned zero */
  OMNI_SEND_LIMIT_REACHED,    /* drain call budget used with bytes remaining */
  OMNI_SEND_ERR_INVALID,      /* caller-contract violation: no syscall */
  OMNI_SEND_ERR_FATAL,        /* other socket failure, accepted FD left open */
  OMNI_SEND_ERR_INTERNAL      /* impossible result/progress invariant */
};

struct omni_send_result {
  enum omni_send_status status;
  int sys_errno; /* errno at the stop point; 0 on progress/complete/limit */
  size_t sent;  /* one-shot: this attempt; drain: total local progress */
};

/*
 * Perform at most one socket send from an immutable caller-owned span. A
 * live accepted owner is borrowed, not destroyed or reconfigured. Nonzero
 * input with a NULL data pointer is invalid; zero-length input, including a
 * NULL pointer, returns COMPLETE with zero bytes and no syscall. A positive
 * result reports exactly the bytes accepted by the kernel; the caller owns
 * advancement for any unsent tail.
 */
struct omni_send_result omni_send_once(const struct omni_accepted *conn,
                                       struct omni_send_span span);

/*
 * Send at most max_calls one-shot attempts from one immutable span. The
 * offset is local to this call and is never retained. A zero call limit is
 * invalid, even for an empty span. Completion is reported without an extra
 * syscall; if the limit ends with bytes remaining, LIMIT_REACHED carries all
 * progress made so far. No internal retry occurs after WOULD_BLOCK,
 * INTERRUPTED, peer failure, zero progress, or any fatal status.
 */
struct omni_send_result omni_send_drain(const struct omni_accepted *conn,
                                        struct omni_send_span span,
                                        size_t max_calls);

#endif /* OMNIROUTE_SEND_H */
