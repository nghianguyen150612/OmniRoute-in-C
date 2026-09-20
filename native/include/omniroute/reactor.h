/*
 * OmniRoute native backend — bounded reactor foundation (Task 021).
 *
 * The reactor is the small coordination layer between the existing poller
 * and application callbacks:
 *
 *   poller_wait() -> reactor_step() -> callback(token, events, context)
 *
 * It waits for readiness and reports it. It does not perform payload reads or
 * writes, accept descriptors, close descriptors, or process protocol/parser
 * code. It does not own sockets,
 * connection objects, registry entries, or external buffers.
 *
 * Storage is explicit and fixed-capacity. The caller supplies one registration
 * array and one poller-event array at initialization; the reactor never
 * allocates, grows, queues, or allocates per event. The caller keeps those
 * arrays, the poller, every callback, and every callback context valid until
 * the corresponding reactor operation has removed or destroyed the
 * registration. Removal and destruction clear callback/context fields before
 * releasing the registration, so the reactor does not retain those pointers.
 *
 * A reactor is single-owner and externally synchronized. Callback code may
 * inspect or coordinate application state, and may remove registrations, but
 * the reactor does not infer ownership or perform application cleanup.
 */

#ifndef OMNIROUTE_REACTOR_H
#define OMNIROUTE_REACTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omniroute/poller.h"

typedef void (*omni_reactor_callback)(uint64_t token, uint32_t events, void *context);

struct omni_reactor_registration {
  int fd;
  uint64_t token;
  uint32_t interests;
  omni_reactor_callback callback;
  void *context;
};

enum omni_reactor_status {
  OMNI_REACTOR_OK = 0,
  OMNI_REACTOR_ERR_INVALID,     /* NULL, inert, bad descriptor/mask/timeout */
  OMNI_REACTOR_ERR_DUPLICATE,   /* token or descriptor already registered */
  OMNI_REACTOR_ERR_FULL,        /* fixed registration capacity exhausted */
  OMNI_REACTOR_ERR_NOT_FOUND,   /* remove target is absent */
  OMNI_REACTOR_ERR_OUTPUT,      /* poller output cannot represent its live set */
  OMNI_REACTOR_ERR_WAIT,        /* readiness wait failed */
  OMNI_REACTOR_ERR_INTERRUPTED  /* readiness wait was interrupted */
};

struct omni_reactor_result {
  enum omni_reactor_status status;
  int sys_errno; /* errno at the failure point; 0 on success */
  size_t count;  /* callbacks dispatched on OK; 0 for failures */
};

struct omni_reactor {
  struct omni_poller *poller; /* borrowed; caller destroys it separately */
  struct omni_reactor_registration *registrations; /* [capacity], caller-owned */
  struct omni_poller_event *events;                /* [capacity], caller-owned */
  size_t capacity;
  size_t count;
  bool live;
};

/*
 * Set a fresh caller-owned object to inert. NULL-safe. This is not cleanup for
 * a live reactor: destroy a live reactor first so its poller registrations are
 * removed and callback pointers are cleared.
 */
void omni_reactor_make_inert(struct omni_reactor *reactor);

/*
 * Initialize a live reactor over a live, externally owned poller. The poller
 * is borrowed and must remain live until reactor_destroy. Registration and
 * event arrays are borrowed fixed-capacity storage and must remain alive until
 * reactor_destroy. No allocation occurs. The reactor manages poller entries
 * added through omni_reactor_add; callers must not mutate that poller set
 * behind the reactor.
 */
struct omni_reactor_result omni_reactor_init(
    struct omni_reactor *reactor,
    struct omni_poller *poller,
    struct omni_reactor_registration *registrations,
    struct omni_poller_event *events,
    size_t capacity);

/*
 * Remove all reactor registrations and leave the reactor inert. Descriptors,
 * poller backing, registration arrays, callbacks, and contexts remain owned
 * by their callers. NULL, inert, and repeated destruction are safe no-ops.
 */
void omni_reactor_destroy(struct omni_reactor *reactor);

/*
 * Register one borrowed descriptor. Token identity is unique within a live
 * reactor; callback must be non-NULL. Duplicate token/descriptor and full
 * capacity errors leave both reactor and poller state unchanged.
 */
struct omni_reactor_result omni_reactor_add(
    struct omni_reactor *reactor,
    int fd,
    uint64_t token,
    uint32_t interests,
    omni_reactor_callback callback,
    void *context);

/*
 * Remove a registration by token. Missing tokens return NOT_FOUND with state
 * unchanged. Successful removal unregisters the borrowed descriptor and
 * clears the callback/context before compacting the fixed array. No descriptor
 * is closed and no connection or registry object is destroyed.
 */
struct omni_reactor_result omni_reactor_remove(struct omni_reactor *reactor,
                                               uint64_t token);

/*
 * Process one bounded wait/dispatch step. timeout_ms=0 probes; a positive
 * timeout bounds the single poller wait. Negative and >INT_MAX timeouts are
 * invalid. The step dispatches each event returned by the poller at most once,
 * then returns; it never runs an event loop. A callback receives the poller's
 * readiness mask, including READ, WRITE, ERROR, HANGUP, or INVALID bits.
 *
 * If a callback removes a later registration, that later event is skipped.
 * If a callback destroys the reactor, dispatch stops after that callback.
 * Interrupted waits return ERR_INTERRUPTED with the poller registration set
 * unchanged and no callback dispatched.
 */
struct omni_reactor_result omni_reactor_step(struct omni_reactor *reactor,
                                             int64_t timeout_ms);

/* Cheap local accounting. NULL or non-live reactors report zero. */
size_t omni_reactor_capacity(const struct omni_reactor *reactor);
size_t omni_reactor_count(const struct omni_reactor *reactor);

#endif /* OMNIROUTE_REACTOR_H */
