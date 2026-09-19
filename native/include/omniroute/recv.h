/*
 * OmniRoute native backend — bounded nonblocking socket receive (Task 017).
 *
 * The first production socket payload input: a small allocation-free layer
 * that moves available bytes from a live accepted nonblocking socket
 * directly into the writable tail of an existing bounded byte buffer. This
 * task covers receive only — no send path exists anywhere in production,
 * no parsing, no server loop.
 *
 * Ownership split: the receive layer borrows both resources and owns
 * neither. The accepted owner keeps sole descriptor ownership (this layer
 * never closes, destroys, reconfigures, or transfers it), the byte buffer
 * keeps sole backing ownership (this layer never destroys, resets,
 * compacts, reallocates, or replaces it), and the poller stays fully
 * independent (this layer never registers, removes, or updates readiness
 * entries — a future dispatcher will compose readiness observation with
 * these primitives). Only a successful receive plus commit advances the
 * buffer write side; every other outcome leaves both borrowed objects
 * exactly as found.
 *
 * Mechanism: exactly one socket receive per one-shot call, placed straight
 * into the buffer writable tail obtained through the Task 013
 * writable-view contract — no intermediate staging, no copy. Flags are
 * zero: accepted sockets are already guaranteed nonblocking, so no extra
 * wait override is needed; speculative peeking and wait-all behavior are
 * explicitly out of scope. A POSIX fallback for other platforms is
 * deferred to the platform task, like the accept layer before it.
 *
 * Result taxonomy: one status shape plus the errno captured at the stop
 * point and the committed byte count. DATA carries the exact committed
 * count with a zero errno field. Would-block and EOF are normal control
 * flow, never errors. Interruption surfaces as INTERRUPTED with no
 * internal retry, matching the poller and accept layers. Every other
 * failure stops as ERR_FATAL with its errno preserved; the accepted
 * descriptor is never closed automatically — a later lifecycle layer
 * decides close and backoff policy. Caller-contract violations report
 * ERR_INVALID before any kernel wait happens. A zero writable tail
 * reports BUFFER_FULL without any kernel wait, so pending socket bytes
 * are never consumed by a call that has nowhere to put them.
 *
 * Impossible-commit policy: the requested length never exceeds the
 * observed writable tail, so under valid buffer state the commit that
 * publishes received bytes cannot fail — the call sequence is shaped to
 * make that path unreachable rather than merely unlikely. A defensive
 * ERR_INTERNAL status still exists fail-closed for the unreachable case:
 * it reports the positive recv count while refusing to publish DATA, so
 * the caller cannot mistake the syscall result for a committed buffer
 * range. The buffer's logical write side remains unchanged, but its
 * backing contains bytes that are no longer safe to treat as unpublished
 * scratch; the caller must stop using that buffer and choose its own
 * recovery. The receive layer does not retry, reset, or abort. This path
 * is never used for caller-controlled exhaustion.
 *
 * EOF policy: an orderly peer shutdown of its sending side reports EOF
 * and changes nothing — the accepted owner is not destroyed, the
 * descriptor is not closed, the buffer is not reset, and already-buffered
 * bytes are not discarded. Draining buffered input before closing is a
 * later layer's decision.
 *
 * Bounded drain: repeat the one-shot primitive up to an explicit finite
 * call limit, accumulating committed bytes, until would-block, EOF,
 * buffer-full, interruption, failure, or the limit itself stops the call.
 * The drain never loops merely because the peer keeps producing; the
 * limit protects event-loop fairness. Partial success is retained: bytes
 * committed before the stop stay valid in the buffer and the result still
 * carries the total with the terminal reason. A zero limit is rejected as
 * invalid, mirroring the zero-capacity rule of the accept drain.
 *
 * No automatic compaction and no automatic reset: a zero writable tail
 * reports BUFFER_FULL even when a reclaimable prefix exists, because
 * compaction cost belongs to the caller's explicit decision. The linear
 * buffer may hold unread data plus a consumed prefix while the tail is
 * empty — the receive layer reports that state honestly instead of hiding
 * it. Callers compact or reset explicitly, then receive again.
 *
 * Payload bytes stay opaque: no framing, no text, no NUL-termination, no
 * encoding assumption. Counts are authoritative and binary-safe.
 *
 * Zero heap allocation: receive calls no allocator; bytes land directly
 * in existing buffer backing. The kernel socket allocation itself is
 * outside user-space heap accounting, as in earlier tasks.
 *
 * A receive call is externally synchronized like every primitive before
 * it: no mutexes, no atomics, no thread-safety machinery.
 */

#ifndef OMNIROUTE_RECV_H
#define OMNIROUTE_RECV_H

#include <stddef.h>

#include "omniroute/accepted.h"
#include "omniroute/bytebuf.h"

enum omni_recv_status {
  OMNI_RECV_DATA = 0,
  OMNI_RECV_WOULD_BLOCK, /* nothing available now: normal, nothing committed */
  OMNI_RECV_EOF,         /* orderly peer shutdown: socket and buffer untouched */
  OMNI_RECV_INTERRUPTED, /* interrupted: nothing committed, state unchanged */
  OMNI_RECV_BUFFER_FULL, /* zero writable tail: no kernel wait performed */
  OMNI_RECV_LIMIT_REACHED, /* drain call limit used: pending input may remain */
  OMNI_RECV_ERR_INVALID,   /* caller-contract violation: nothing committed */
  OMNI_RECV_ERR_FATAL,     /* system failure: nothing committed, FD left open */
  OMNI_RECV_ERR_INTERNAL   /* unreachable commit failure: fail-closed, see above */
};

struct omni_recv_result {
  enum omni_recv_status status;
  int sys_errno;   /* errno at the stop point; 0 on DATA, EOF, BUFFER_FULL, LIMIT */
  size_t received; /* one-shot: recv/commit count; drain: total committed */
};

/*
 * Perform at most one socket receive from a live accepted owner directly
 * into the writable tail of a live byte buffer, committing exactly the
 * produced byte count. Borrows both objects and mutates neither except
 * through the single commit on DATA. Makes zero kernel waits when the
 * arguments are invalid or the writable tail is empty, and exactly one
 * otherwise. Never compacts, resets, reallocates, closes, or destroys
 * anything it borrows.
 */
struct omni_recv_result omni_recv_once(const struct omni_accepted *conn,
                                       struct omni_bytebuf *buf);

/*
 * Bounded drain from a live accepted owner into a live byte buffer: repeat
 * the one-shot primitive until would-block, EOF, buffer-full,
 * interruption, failure, or max_calls one-shot attempts. Never performs
 * more than max_calls kernel waits; a zero limit is invalid. Bytes
 * committed before the stop stay valid in the buffer and the result
 * carries their total with the terminal status and errno.
 */
struct omni_recv_result omni_recv_drain(const struct omni_accepted *conn,
                                        struct omni_bytebuf *buf,
                                        size_t max_calls);

#endif /* OMNIROUTE_RECV_H */
