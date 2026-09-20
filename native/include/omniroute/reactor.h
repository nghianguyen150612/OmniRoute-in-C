/*
 * OmniRoute native backend — bounded reactor foundation (Task 021).
 *
 * The reactor connects the poller to application callbacks:
 *
 *   poller -> reactor -> user callbacks
 *
 * It is a thin event dispatcher that never processes payloads, never
 * creates sockets, never owns descriptors, never closes connections,
 * never destroys connection objects, never allocates memory per event,
 * never owns external resources. Its sole responsibility is:
 *
 *   - waiting for events
 *   - dispatching ready notifications
 *   - coordinating existing primitives
 *
 * This layer provides the first bounded reactor/event-loop foundation
 * for future server runtime components. Subsequent tasks will use this
 * reactor to build HTTP, JSON, routing, authentication, provider handling,
 * TLS, threads, worker pools, timers, background tasks, etc.
 *
 * Memory is explicit and bounded: fixed-capacity storage allocated at
 * initialization only. No per-event allocation, no dynamic growth,
 * no unbounded queues. All ownership is explicit and contract-driven.
 *
 * Callback contract:
 *   - Token: opaque uint64 identity (round-trip from registration)
 *   - Events: readiness mask (OMNI_REACTOR_READY_*)
 *   - Context: user-provided pointer (lifetime managed by caller)
 *   - Lifetime: user must keep callback and context valid as long as
 *     registration exists; reactor never dereferences invalid pointers
 *
 * Registration contract:
 *   - Token must be unique (duplicate registration rejected)
 *   - Capacity must not be exceeded (calls fail with unchanged state)
 *   - Descriptor ownership never transferred (reactor borrows only)
 *
 * Event flow:
 *   poller_wait() -> reactor_step() -> callback(token, events, context)
 *   The reactor only reports readiness, never calls recv/send, closes
 *   sockets, destroys connections, or processes protocols.
 *
 * Step behavior:
 *   reactor_step(timeout_ms) processes at most one bounded step:
 *     - returns after processing current events
 *     - supports timeout=0 (probe) and positive timeout
 *     - returns when no more events are ready within timeout
 *     - never loops infinitely
 *
 * This is not a complete server runtime.
 */

#ifndef OMNIROUTE_REACTOR_H
#define OMNIROUTE_REACTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omniroute/poller.h"

typedef void (*omni_reactor_callback)(uint64_t token, uint32_t events, void *context);

enum omni_reactor_status {
  OMNI_REACTOR_OK = 0,
  OMNI_REACTOR_ERR_INVALID,    /* NULL args, poller not live, full capacity, duplicate token */
  OMNI_REACTOR_ERR_NOT_FOUND,  /* remove on unknown token */
  OMNI_REACTOR_ERR_INTERRUPTED /* poller wait interrupted; state unchanged */
};

struct omni_reactor_result {
  enum omni_reactor_status status;
  int sys_errno; /* errno at the failure point; 0 on success */
  size_t count;  /* events processed on OK */
};

struct omni_reactor {
  struct omni_poller *poller;          /* borrowed reactor poller */
  omni_reactor_callback *callbacks;    /* [capacity] callback table */
  void **contexts;                     /* [capacity] user context table */
  uint32_t *ready_masks;               /* [capacity] preallocated ready masks */
  uint64_t *tokens;                    /* [capacity] identity tokens */
  int *fds;                           /* [capacity] descriptor table */
  bool *registered;                   /* [capacity] registration state */
  size_t capacity;
  size_t count; /* live registrations */
  bool live;
};

/*
 * Canonicalize fresh caller-owned reactor storage to inert without touching
 * any owned poller reference. NULL-safe. Call before first init when the
 * struct is not statically zeroed.
 */
void omni_reactor_make_inert(struct omni_reactor *reactor);

/*
 * Borrow a live poller and fixed-capacity parallel storage. The caller keeps
 * all arrays alive until after destroy. The poller must be live and externally
 * owned. Capacity must be nonzero and fit in 32 bits so every position stays
 * representable in a handle. Returns ERR_INVALID for NULL, non-live poller,
 * zero capacity, or overrun capacity. Registration capacity must be a
 * positive power of two for future optimization.
 */
struct omni_reactor_result omni_reactor_init(struct omni_reactor *reactor,
                                             struct omni_poller *poller,
                                             void *callbacks,
                                             void *contexts,
                                             void *ready_masks,
                                             void *tokens,
                                             void *fds,
                                             bool *registered,
                                             size_t capacity);

/*
 * Release the borrowed poller reference and all parallel storage. The poller
 * remains owned by its caller and must be destroyed separately. NULL, inert,
 * and repeated destroy are safe no-ops.
 */
void omni_reactor_destroy(struct omni_reactor *reactor);

/*
 * Register an event source for the given token and descriptor. The callback
 * and context are stored in the parallel arrays for the reactor's lifetime.
 * Descriptor ownership never transfers: the poller only observes a borrowed
 * descriptor, and its owner keeps lifetime responsibility. Duplicate tokens
 * and full capacity are rejected with ERR_INVALID and state unchanged.
 */
struct omni_reactor_result omni_reactor_add(struct omni_reactor *reactor,
                                           int fd,
                                           uint64_t token,
                                           uint32_t interests,
                                           omni_reactor_callback callback,
                                           void *context);

/*
 * Remove a registration. Unknown tokens are rejected with ERR_NOT_FOUND and
 * state unchanged. The registration is cleared but the callback and context
 * remain stored in the parallel arrays (caller may reuse token later).
 */
struct omni_reactor_result omni_reactor_remove(struct omni_reactor *reactor,
                                              uint64_t token);

/*
 * Replace one registration's interest mask. Unknown tokens are ERR_NOT_FOUND,
 * invalid masks are ERR_INVALID, and state is preserved. Never allocates.
 */
struct omni_reactor_result omni_reactor_update(struct omni_reactor *reactor,
                                              uint64_t token,
                                              uint32_t interests);

/*
 * Process events for a bounded step.
 *
 * Process events for a bounded step. Waits up to timeout_ms (0 probes, positive
 * bounds the wait; negative and values above INT_MAX are ERR_INVALID). After
 * poller_wait returns, this function processes all ready events from the
 * poller's last result, calling callbacks in the order they appeared.
 * Returns immediately after processing all current events; no infinite loop.
 *
 * Supports interrupted wait: when poller_wait returns INTERRUPTED, the
 * reactor returns ERR_INTERRUPTED with zero events processed and state
 * unchanged (the poller is still registered).
 *
 * Empty reactor: when the reactor has no registrations, returns OK with zero
 * count and no wait.
 */
struct omni_reactor_result omni_reactor_step(struct omni_reactor *reactor,
                                            int64_t timeout_ms);

/*
 * Drop all registrations while retaining backing capacity. The poller remains
 * live and owned by its caller. Callbacks and contexts are not cleared.
 */
void omni_reactor_reset(struct omni_reactor *reactor);

/* Cheap local accounting. NULL or non-live reactors report zero. */
size_t omni_reactor_capacity(const struct omni_reactor *reactor);
size_t omni_reactor_count(const struct omni_reactor *reactor);

#endif /* OMNIROUTE_REACTOR_H */