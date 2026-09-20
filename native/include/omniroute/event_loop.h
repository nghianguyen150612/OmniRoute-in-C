/*
 * OmniRoute native backend — bounded synchronous event loop (Task 024).
 *
 * The event loop is the execution layer above the Task 023 runtime
 * coordinator and the Task 021 reactor:
 *
 *   runtime -> event_loop_run() -> reactor_step() -> callbacks
 *                                                             |
 *                                                             +-> connections
 *
 * It owns no listener, registry entry, reactor registration, descriptor,
 * connection, event record, queue, timer, worker, or protocol state. The
 * runtime remains the lifecycle owner; this object only borrows its live
 * reactor and repeats bounded synchronous steps until a stop request, a
 * reactor failure, an interrupted wait, or an empty reactor ends execution.
 *
 * Storage is explicit and fixed-size. Initialization does not allocate, and
 * run/stop/destroy do not allocate after initialization. The loop counters
 * saturate at UINT64_MAX instead of wrapping. A loop is single-owner and
 * externally synchronized: there are no mutexes, atomics, threads, signals,
 * hidden schedulers, or background execution paths.
 */

#ifndef OMNIROUTE_EVENT_LOOP_H
#define OMNIROUTE_EVENT_LOOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omniroute/reactor.h"
#include "omniroute/runtime.h"

/* A caller may use zero for a nonblocking probe. */
#define OMNI_EVENT_LOOP_DEFAULT_TIMEOUT_MS ((int64_t)1000)

/* Keep every individual wait bounded without narrowing to an unbounded wait. */
#define OMNI_EVENT_LOOP_MAX_TIMEOUT_MS ((int64_t)60000)

enum omni_event_loop_state {
  OMNI_EVENT_LOOP_INERT = 0,
  OMNI_EVENT_LOOP_INITIALIZED,
  OMNI_EVENT_LOOP_RUNNING,
  OMNI_EVENT_LOOP_STOPPING,
  OMNI_EVENT_LOOP_STOPPED
};

enum omni_event_loop_status {
  OMNI_EVENT_LOOP_OK = 0,
  OMNI_EVENT_LOOP_ERR_INVALID,      /* NULL, bad timeout, or unavailable runtime */
  OMNI_EVENT_LOOP_ERR_STATE,        /* invalid loop lifecycle transition */
  OMNI_EVENT_LOOP_ERR_RUNTIME,      /* runtime is not RUNNING during execution */
  OMNI_EVENT_LOOP_ERR_REACTOR,      /* reactor step failed */
  OMNI_EVENT_LOOP_ERR_INTERRUPTED   /* reactor wait was interrupted */
};

struct omni_event_loop_config {
  struct omni_runtime *runtime; /* borrowed; must outlive the event loop */
  int64_t timeout_ms;           /* inclusive range: 0..OMNI_EVENT_LOOP_MAX_TIMEOUT_MS */
};

struct omni_event_loop_result {
  enum omni_event_loop_status status;
  int sys_errno; /* errno-style detail; zero for success */
  enum omni_event_loop_state state; /* state after the operation */
  enum omni_reactor_status reactor_status; /* most recent reactor status */
};

struct omni_event_loop {
  struct omni_runtime *runtime; /* borrowed; runtime owns lifecycle */
  int64_t timeout_ms;
  enum omni_event_loop_state state;
  enum omni_reactor_status last_status;
  uint64_t iterations;
  uint64_t events_processed;
  bool stop_requested;
};

/*
 * Canonicalize fresh or stopped caller-owned storage to INERT. This is not
 * cleanup for INITIALIZED, RUNNING, or STOPPING storage; destroy after run
 * returns. NULL is a safe no-op.
 */
void omni_event_loop_make_inert(struct omni_event_loop *loop);

/*
 * Bind a loop to an initialized runtime and validate one finite step timeout.
 * The runtime may be INITIALIZED or RUNNING here; omni_event_loop_run still
 * requires the runtime to be RUNNING. No subsystem is started by this call.
 */
struct omni_event_loop_result omni_event_loop_init(
    struct omni_event_loop *loop, const struct omni_event_loop_config *config);

/*
 * Run synchronously. Each iteration calls exactly one omni_reactor_step with
 * the configured timeout. A RUNNING runtime is required. A stop requested by
 * a callback is observed after the current reactor dispatch returns. With no
 * reactor registrations, one zero-event step is performed and the loop exits
 * cleanly because the existing reactor has no wakeup source to wait on.
 * Runtime teardown is not performed here.
 */
struct omni_event_loop_result omni_event_loop_run(struct omni_event_loop *loop);

/*
 * Request a synchronous stop. INITIALIZED and INERT loops reject stop before
 * run; RUNNING transitions to STOPPING; STOPPING and STOPPED calls are safe
 * no-ops. No signal, wakeup descriptor, thread, or cross-owner mechanism is
 * installed, so another owner cannot interrupt a blocked step asynchronously.
 */
struct omni_event_loop_result omni_event_loop_stop(struct omni_event_loop *loop);

/*
 * Release the loop's borrowed references and return it to INERT. The runtime
 * is never stopped. Calling destroy while RUNNING or STOPPING is a safe no-op;
 * call it after run returns. NULL, INERT, and repeated destruction are safe.
 */
void omni_event_loop_destroy(struct omni_event_loop *loop);

/* NULL reports INERT. */
enum omni_event_loop_state omni_event_loop_state(const struct omni_event_loop *loop);

/* Fixed local accounting. INERT or NULL loops report zero. */
uint64_t omni_event_loop_iterations(const struct omni_event_loop *loop);
uint64_t omni_event_loop_events_processed(const struct omni_event_loop *loop);
enum omni_reactor_status omni_event_loop_last_status(const struct omni_event_loop *loop);

#endif /* OMNIROUTE_EVENT_LOOP_H */
