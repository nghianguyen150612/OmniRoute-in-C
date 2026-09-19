/*
 * OmniRoute native backend — accepted-socket owner + bounded accept4 drain
 * (Task 016).
 *
 * The smallest useful accepted-connection FD owner: it represents one
 * accepted client descriptor and nothing else. No input staging, no output
 * staging, no arena, no parser state, no HTTP state, no timestamps, no
 * routing or auth state, no linked-list links, no callbacks. A richer
 * connection runtime may arrive in a later task under its own name; this
 * deliberately narrow type must not be mistaken for it.
 *
 * Ownership split (see MIGRATION_PLAN.md section 1 and the Task 014/015
 * headers): the listener owns the listener descriptor, the poller only
 * borrows whatever descriptor it observes, and this object owns exactly one
 * accepted client descriptor. Higher protocol layers do not exist yet.
 *
 * Ownership and publication: on successful acceptance this object owns
 * exactly one client descriptor and the caller must not close it
 * independently afterwards. A poller registration of that descriptor later
 * only borrows it. Destroy releases the owned descriptor exactly once and
 * leaves the object inert; it never touches the listener descriptor or any
 * unrelated descriptor. A failed acceptance publishes nothing and the
 * destination stays inert, so failure can never leak or double-manage a
 * descriptor.
 *
 * Mechanism: Linux accept4 with atomic SOCK_NONBLOCK plus SOCK_CLOEXEC, so
 * no transient blocking or inheritable descriptor ever exists. Accepted
 * descriptors are verified after creation (O_NONBLOCK and FD_CLOEXEC both
 * probed); a verification failure closes the new descriptor at once, leaves
 * the destination inert, and reports a fatal status. A descriptor whose
 * required invariants were not established is never published. A POSIX
 * fallback for other platforms is deferred to the platform task — no
 * fallback is implemented here.
 *
 * Invalid-descriptor sentinel is -1. Descriptor 0 is valid and never
 * treated as invalid; liveness is the explicit live flag, never a numeric
 * comparison on the descriptor value.
 *
 * Result taxonomy: single acceptance and bounded drain share one status
 * shape plus the errno captured at the stop point and, for drain, the
 * number of published owners. Queue-drained (EAGAIN or EWOULDBLOCK on a
 * nonblocking listener) is normal control flow, never an error. EINTR
 * surfaces as INTERRUPTED with no internal retry — the caller decides
 * whether to continue draining; re-waiting policy needs no clock at this
 * layer. ECONNABORTED and EPROTO name a failed pending handshake (a
 * per-connection abort documented alongside accept semantics), so they
 * surface as TRANSIENT: the failed handshake is consumed and the caller may
 * drain again immediately. Every other accept failure stops the call as
 * ERR_FATAL with its errno preserved; only the two transient names above
 * have evidence as per-connection aborts worth classifying separately, so
 * the table stays small by design. Caller-contract violations (NULL or
 * inert listener, NULL destination, live destination, zero drain capacity)
 * report ERR_INVALID before any wait happens.
 *
 * Bounded drain: repeatedly accept from a live listener into caller-owned
 * storage until the queue drains, the caller capacity fills, or an
 * interruption or failure stops the call. The drain never exceeds the
 * caller capacity, never allocates, and preserves every successfully
 * accepted descriptor in the caller output. Capacity-reached is reported
 * distinctly from queue-drained: a full output means pending handshakes may
 * remain, and the future server runtime must revisit listener readiness.
 * Partial success keeps its owners: when a later attempt stops the drain,
 * the already published owners stay live and owned by the caller, and the
 * result still carries the accepted count with the terminal stop reason and
 * errno.
 *
 * Output contract: the caller provides storage for out_capacity owners and
 * every slot must already be inert on entry — the drain verifies this
 * before accepting anything, so a stray live owner can never be silently
 * overwritten and its descriptor lost. Only the first accepted entries
 * become live; the remaining capacity is left untouched by explicit
 * contract.
 *
 * Close policy (same as the Task 014 listener): destroy performs a single
 * close and never retries it. On Linux the descriptor is released even when
 * the close reports interruption, so a retry could release an unrelated
 * descriptor recycled into the same number.
 *
 * Zero heap allocation: single acceptance and drain call no allocator. The
 * kernel socket allocation itself is outside user-space heap accounting.
 * No peer-address capture (no numeric peer fields, no reverse lookup of any
 * kind), no payload input/output, no byte-buffer or arena involvement, no
 * server loop, no HTTP, no threads, no timing machinery.
 *
 * An accepted owner is single-owner and externally synchronized: no
 * mutexes, no atomics, no thread-safety machinery.
 */

#ifndef OMNIROUTE_ACCEPTED_H
#define OMNIROUTE_ACCEPTED_H

#include <stdbool.h>
#include <stddef.h>

#include "omniroute/listener.h"

/* Invalid-descriptor sentinel. Descriptor 0 is valid and never treated as
 * invalid; only this value means "no descriptor". */
#define OMNI_ACCEPTED_FD_INVALID (-1)

enum omni_accept_status {
  OMNI_ACCEPT_OK = 0,
  OMNI_ACCEPT_DRAINED,     /* queue drained / would block: normal, no owner made */
  OMNI_ACCEPT_INTERRUPTED, /* interrupted: nothing published for the attempt */
  OMNI_ACCEPT_TRANSIENT,   /* failed handshake consumed: nothing published */
  OMNI_ACCEPT_CAPACITY,    /* drain output full: pending handshakes may remain */
  OMNI_ACCEPT_ERR_INVALID, /* caller-contract violation: nothing published */
  OMNI_ACCEPT_ERR_FATAL    /* system failure: nothing published for the attempt */
};

struct omni_accept_result {
  enum omni_accept_status status;
  int sys_errno;   /* errno at the stop point; 0 on OK and on CAPACITY */
  size_t accepted; /* single: 1 on OK else 0; drain: owners published */
};

struct omni_accepted {
  int fd;    /* owned client descriptor; OMNI_ACCEPTED_FD_INVALID when inert */
  bool live; /* false before use, after destroy, or after failed acceptance */
};

/*
 * Put an accepted owner into the inert state without touching any
 * descriptor: descriptor invalid, not live. All observers treat this
 * exactly like a destroyed owner. Call this before first use rather than
 * relying on zeroed storage. NULL-safe no-op.
 */
void omni_accepted_make_inert(struct omni_accepted *slot);

/*
 * Borrowed view of the owned client descriptor for future event-loop
 * registration. No ownership transfer — the caller must not close it, and
 * must not use it past destroy. Returns OMNI_ACCEPTED_FD_INVALID for NULL
 * or non-live owners. Descriptor 0 is reported as the valid value 0.
 */
int omni_accepted_fd(const struct omni_accepted *slot);

/*
 * True exactly while the owner holds a live owned descriptor. NULL
 * reports false. Liveness is this flag, never a numeric test on the
 * descriptor value.
 */
bool omni_accepted_is_live(const struct omni_accepted *slot);

/*
 * Release the owned client descriptor exactly once and return the owner to
 * the inert state. Never closes the listener descriptor or unrelated
 * descriptors: only the stored owned descriptor is released, and only when
 * live. NULL, inert, and repeated destroy are safe no-ops. The single
 * close is never retried; see the close policy above.
 */
void omni_accepted_destroy(struct omni_accepted *slot);

/*
 * Attempt exactly one nonblocking acceptance from a live listener. Borrows
 * the listener descriptor and never modifies listener ownership. The
 * destination must already be inert; a live destination is rejected before
 * any wait happens so its descriptor cannot be lost. Ownership publishes
 * only after accept4 succeeds with atomic nonblocking plus close-on-exec
 * flags and both flags verify on the new descriptor. Any failure path
 * leaves the destination inert with no descriptor published.
 */
struct omni_accept_result omni_accept_once(const struct omni_listener *listener,
                                           struct omni_accepted *slot);

/*
 * Bounded drain from a live listener into caller-owned storage: repeat
 * single acceptance until the queue drains, out_capacity owners publish,
 * or an interruption or failure stops the call. Never exceeds the caller
 * capacity and never allocates. Every slot in out[0, out_capacity) must
 * already be inert; otherwise the call fails as ERR_INVALID with nothing
 * published. Only the first result.accepted entries become live owned
 * descriptors; the rest of the capacity is left untouched. The caller owns
 * all published owners regardless of the terminal stop reason.
 */
struct omni_accept_result omni_accept_drain(const struct omni_listener *listener,
                                            struct omni_accepted *slot,
                                            size_t out_capacity);

#endif /* OMNIROUTE_ACCEPTED_H */
