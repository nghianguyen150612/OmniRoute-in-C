/*
 * OmniRoute native backend — bounded readiness abstraction (Task 015).
 *
 * A small POSIX poll-based readiness layer that observes borrowed
 * descriptors without owning them. It answers one question for a future
 * server runtime: "which of these registered descriptors is ready, and
 * for what?" Why this backend first (see MIGRATION_PLAN.md section 1):
 * the plan sketches an epoll/kqueue-abstracted poller under a future
 * platform/ directory, but mandates nothing for this layer — a portable
 * readiness wait establishes exact registration, timeout, ordering, and
 * ownership semantics with the smallest auditable surface before any
 * optimized backend arrives. A later task may replace or augment the
 * mechanism after profiling; the registration contract stays.
 *
 * This module owns readiness bookkeeping ONLY:
 *
 * - no accept path (no client descriptor is ever produced here);
 * - no connection objects, no per-connection buffers or arenas;
 * - no payload input/output of any kind;
 * - no callbacks, no task queues, no timers beyond the wait timeout;
 * - no wakeup pipe/eventfd (no threads submit work yet);
 * - no event loop, no epoll/select/io_uring/kqueue, no threads;
 * - no HTTP, no TLS, no signal disposition changes.
 *
 * Storage is explicit and bounded: either caller-provided parallel arrays
 * or exactly one owned block allocated at initialization (symmetric with
 * the arena/byte-buffer dual model). Initialization-time allocation is
 * the only heap activity in the owned lifecycle — add, remove, update,
 * wait, reset, and destroy never allocate. Capacity never grows; beyond
 * capacity, registration fails cleanly with state unchanged.
 *
 * Borrowed-descriptor contract (critical): the poller never closes, dups,
 * or reconfigures a registered descriptor. The owner keeps lifetime
 * responsibility — for a Task 014 listener, omni_listener owns its
 * descriptor and the poller only observes the borrowed value. Destroying
 * the poller, removing a registration, or resetting closes nothing.
 * The owner MUST remove a registration before closing or recycling its
 * descriptor; descriptor numbers can be reused by the OS, so a raw
 * descriptor integer is never treated as a permanent identity.
 *
 * Identity: each registration carries a caller-provided opaque 64-bit
 * token round-tripped in every readiness record, so future dispatch can
 * identify registrations without owning pointers and without trusting
 * recycled descriptor numbers. Tokens are never validated — any value,
 * including zero, is accepted and echoed back.
 *
 * Interests use a small project-level mask (readable, writable) rather
 * than leaking platform constants. Empty or out-of-mask interests are
 * rejected: there is no temporarily-disabled entry state yet. Readiness
 * records use a project-level mask covering readable, writable, error,
 * hangup, and invalid-descriptor. Error, hangup, and invalid conditions
 * surface even when not requested — the underlying wait reports them
 * independently of interests, and a future connection layer must see a
 * dead peer. Each record also carries the borrowed descriptor value for
 * the owner's own bookkeeping; the value is valid only while the owner
 * keeps the descriptor alive.
 *
 * Timeout is an int64 count of milliseconds: 0 probes without blocking,
 * positive values bound the wait, and negatives are rejected — there is
 * no infinite wait at this layer (without a wakeup mechanism it would be
 * a hang risk). Values above INT_MAX are rejected rather than narrowed.
 *
 * Interruption policy: an interrupted wait returns INTERRUPTED
 * immediately with nothing consumed and state unchanged. It is never
 * retried internally — re-waiting with the original timeout after
 * repeated interruptions could extend a finite wait without bound, and
 * deadline-preserving retry would need a clock for no current consumer.
 * A caller that owns a clock may re-wait against its own remaining
 * budget; tests cover the mapping with a self-signal.
 *
 * Output capacity is explicit: the caller supplies the event array and
 * its size, and a wait requires room for every live registration before
 * doing any work — readiness is never silently truncated. Events emit in
 * live-registration order, which removal preserves, so dispatch order is
 * deterministic. An empty poller returns zero events immediately without
 * waiting, even with a nonzero timeout.
 *
 * Memory errors use the same shape as Task 014: a status plus the errno
 * captured at the failure point. No heap-allocated diagnostics.
 *
 * A poller is single-owner and externally synchronized: no mutexes, no
 * atomics, no thread-safety machinery.
 */

#ifndef OMNIROUTE_POLLER_H
#define OMNIROUTE_POLLER_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Registration interests (project mask, not platform constants). */
#define OMNI_POLLER_INTEREST_READ ((uint32_t)1u)
#define OMNI_POLLER_INTEREST_WRITE ((uint32_t)2u)

/* Readiness conditions reported per event (project mask). */
#define OMNI_POLLER_READY_READ ((uint32_t)1u)
#define OMNI_POLLER_READY_WRITE ((uint32_t)2u)
#define OMNI_POLLER_READY_ERROR ((uint32_t)4u)
#define OMNI_POLLER_READY_HANGUP ((uint32_t)8u)
#define OMNI_POLLER_READY_INVALID ((uint32_t)16u)

enum omni_poller_status {
  OMNI_POLLER_OK = 0,
  OMNI_POLLER_ERR_INVALID,     /* NULL args, bad descriptor, bad mask, bad timeout, inert */
  OMNI_POLLER_ERR_NOMEM,       /* owned backing allocation failed */
  OMNI_POLLER_ERR_FULL,        /* registration capacity exhausted */
  OMNI_POLLER_ERR_DUPLICATE,   /* descriptor already registered */
  OMNI_POLLER_ERR_NOT_FOUND,   /* remove/update target absent */
  OMNI_POLLER_ERR_OUTPUT,      /* caller event storage too small; no wait performed */
  OMNI_POLLER_ERR_WAIT,        /* readiness wait failed */
  OMNI_POLLER_ERR_INTERRUPTED  /* wait interrupted; nothing consumed, state unchanged */
};

struct omni_poller_result {
  enum omni_poller_status status;
  int sys_errno; /* errno at the failure point; 0 on success */
  size_t count;  /* events written on OK; 0 otherwise (and always 0 from init) */
};

struct omni_poller_event {
  int fd;           /* borrowed descriptor value; see lifetime contract above */
  uint64_t token;   /* caller identity round-trip */
  uint32_t ready;   /* readiness mask */
};

struct omni_poller {
  struct pollfd *fds; /* [capacity] dense in [0, count); fd < 0 marks free */
  uint64_t *tokens;   /* [capacity] parallel caller identity */
  size_t capacity;
  size_t count; /* live registrations, always compact in fds[0, count) */
  bool owns_backing;
  bool live; /* false before init, after destroy, or after failed init */
};

/*
 * Borrow caller-owned parallel arrays of capacity slots. The caller keeps
 * both arrays alive until after destroy; reset/destroy never free them.
 * Returns a non-OK result (poller left inert) for NULL inputs or zero
 * capacity. The arrays need no pre-initialization.
 */
struct omni_poller_result omni_poller_init_borrowed(struct omni_poller *poller,
                                                    struct pollfd *fds,
                                                    uint64_t *tokens,
                                                    size_t capacity);

/*
 * Take exactly one owned backing block for capacity slots, released by
 * destroy. Returns a non-OK result (poller left inert) for zero capacity,
 * unrepresentable sizes, or allocation failure. The only heap allocation
 * in the owned lifecycle.
 */
struct omni_poller_result omni_poller_init_owned(struct omni_poller *poller,
                                                 size_t capacity);

/*
 * Register a borrowed descriptor with interests and identity token.
 * Rejects negative descriptors, empty/unknown interest bits, duplicates
 * of an already-registered descriptor, and full capacity — every failure
 * leaves registration state unchanged. Never allocates, never touches
 * descriptor flags. On success the registration appends in registration
 * order.
 */
struct omni_poller_result omni_poller_add(struct omni_poller *poller, int fd,
                                          uint64_t token, uint32_t interests);

/*
 * Drop a registration without touching its descriptor: the descriptor
 * stays open and owned by its owner. Returns NOT_FOUND with state
 * unchanged for an absent descriptor. Remaining registrations keep their
 * relative order. Never allocates.
 */
struct omni_poller_result omni_poller_remove(struct omni_poller *poller, int fd);

/*
 * Replace one registration's interest mask, keeping its token. Validates
 * the new mask first: NOT_FOUND or INVALID leave the old mask and token
 * untouched. Never allocates, never touches descriptor flags.
 */
struct omni_poller_result omni_poller_update(struct omni_poller *poller, int fd,
                                             uint32_t interests);

/*
 * Wait up to timeout_ms milliseconds (0 probes, positive bounds the
 * wait; negatives and values above INT_MAX are INVALID) and translate
 * readiness into at most out_capacity caller-owned event records in
 * live-registration order. Requires out_capacity >= live count when any
 * registration exists — otherwise OUTPUT with no wait performed, so
 * readiness is never silently truncated. An empty poller returns OK with
 * zero events immediately. Never allocates; never returns pointers to
 * transient storage. INTERRUPTED reports an interrupted wait with nothing
 * consumed.
 */
struct omni_poller_result omni_poller_wait(struct omni_poller *poller,
                                           int64_t timeout_ms,
                                           struct omni_poller_event *out,
                                           size_t out_capacity);

/*
 * Drop all registrations while retaining backing capacity. Closes no
 * descriptors. NULL or non-live pollers: no-op. Never allocates.
 */
void omni_poller_reset(struct omni_poller *poller);

/*
 * Release owned backing exactly once and leave the poller inert. Closes
 * no registered descriptors — owners keep full lifetime control. NULL,
 * non-live, and repeated destroy are safe no-ops.
 */
void omni_poller_destroy(struct omni_poller *poller);

/* Cheap local accounting. NULL or non-live pollers report zero. */
size_t omni_poller_capacity(const struct omni_poller *poller);
size_t omni_poller_count(const struct omni_poller *poller);

#endif /* OMNIROUTE_POLLER_H */
