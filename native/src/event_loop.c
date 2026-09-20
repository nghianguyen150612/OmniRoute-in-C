/*
 * OmniRoute native backend — bounded synchronous event loop implementation.
 *
 * This layer deliberately composes, rather than reimplements, the runtime
 * and reactor. It performs no descriptor operation, no wait syscall, no
 * allocation, and no payload or protocol work. One loop iteration is one
 * omni_reactor_step call followed by local counter/state handling.
 */

#include "omniroute/event_loop.h"

#include <errno.h>
#include <stdint.h>

static void clear_loop(struct omni_event_loop *loop, enum omni_event_loop_state state) {
  if (loop == NULL) {
    return;
  }
  loop->runtime = NULL;
  loop->timeout_ms = 0;
  loop->state = state;
  loop->last_status = OMNI_REACTOR_OK;
  loop->iterations = 0u;
  loop->events_processed = 0u;
  loop->stop_requested = false;
}

static struct omni_event_loop_result make_result(enum omni_event_loop_status status,
                                                 int sys_errno,
                                                 enum omni_event_loop_state state,
                                                 enum omni_reactor_status reactor_status) {
  struct omni_event_loop_result result;

  result.status = status;
  result.sys_errno = sys_errno;
  result.state = state;
  result.reactor_status = reactor_status;
  return result;
}

static struct omni_event_loop_result result_from_loop(const struct omni_event_loop *loop,
                                                      enum omni_event_loop_status status,
                                                      int sys_errno) {
  if (loop == NULL) {
    return make_result(status, sys_errno, OMNI_EVENT_LOOP_INERT,
                       OMNI_REACTOR_ERR_INVALID);
  }
  return make_result(status, sys_errno, loop->state, loop->last_status);
}

static bool runtime_bindable(const struct omni_runtime *runtime) {
  if (runtime == NULL ||
      (omni_runtime_state(runtime) != OMNI_RUNTIME_INITIALIZED &&
       omni_runtime_state(runtime) != OMNI_RUNTIME_RUNNING)) {
    return false;
  }
  return runtime->reactor_initialized && runtime->reactor != NULL && runtime->reactor->live;
}

static bool runtime_running(const struct omni_runtime *runtime) {
  return runtime != NULL && omni_runtime_state(runtime) == OMNI_RUNTIME_RUNNING &&
         runtime->reactor_initialized && runtime->reactor != NULL && runtime->reactor->live;
}

static bool timeout_valid(int64_t timeout_ms) {
  return timeout_ms >= (int64_t)0 && timeout_ms <= OMNI_EVENT_LOOP_MAX_TIMEOUT_MS;
}

/* Counter policy: finite counters saturate, so accounting never wraps. */
static void counter_add(uint64_t *counter, size_t amount) {
  uintmax_t current = 0u;
  uintmax_t addition = 0u;

  if (counter == NULL || *counter == UINT64_MAX) {
    return;
  }
  current = (uintmax_t)*counter;
  addition = (uintmax_t)amount;
  if (addition >= (uintmax_t)UINT64_MAX - current) {
    *counter = UINT64_MAX;
    return;
  }
  *counter = (uint64_t)(current + addition);
}

static enum omni_event_loop_status map_reactor_status(enum omni_reactor_status status) {
  if (status == OMNI_REACTOR_ERR_INTERRUPTED) {
    return OMNI_EVENT_LOOP_ERR_INTERRUPTED;
  }
  return OMNI_EVENT_LOOP_ERR_REACTOR;
}

void omni_event_loop_make_inert(struct omni_event_loop *loop) {
  if (loop == NULL) {
    return;
  }
  if (loop->state == OMNI_EVENT_LOOP_INITIALIZED ||
      loop->state == OMNI_EVENT_LOOP_RUNNING ||
      loop->state == OMNI_EVENT_LOOP_STOPPING) {
    return;
  }
  clear_loop(loop, OMNI_EVENT_LOOP_INERT);
}

struct omni_event_loop_result omni_event_loop_init(
    struct omni_event_loop *loop, const struct omni_event_loop_config *config) {
  if (loop == NULL) {
    return make_result(OMNI_EVENT_LOOP_ERR_INVALID, EINVAL, OMNI_EVENT_LOOP_INERT,
                       OMNI_REACTOR_ERR_INVALID);
  }
  if (loop->state != OMNI_EVENT_LOOP_INERT && loop->state != OMNI_EVENT_LOOP_STOPPED) {
    return result_from_loop(loop, OMNI_EVENT_LOOP_ERR_STATE, EINVAL);
  }
  if (config == NULL || !timeout_valid(config->timeout_ms) ||
      !runtime_bindable(config->runtime)) {
    clear_loop(loop, OMNI_EVENT_LOOP_INERT);
    return result_from_loop(loop, OMNI_EVENT_LOOP_ERR_INVALID, EINVAL);
  }

  clear_loop(loop, OMNI_EVENT_LOOP_INERT);
  loop->runtime = config->runtime;
  loop->timeout_ms = config->timeout_ms;
  loop->state = OMNI_EVENT_LOOP_INITIALIZED;
  return result_from_loop(loop, OMNI_EVENT_LOOP_OK, 0);
}

struct omni_event_loop_result omni_event_loop_run(struct omni_event_loop *loop) {
  if (loop == NULL) {
    return make_result(OMNI_EVENT_LOOP_ERR_INVALID, EINVAL, OMNI_EVENT_LOOP_INERT,
                       OMNI_REACTOR_ERR_INVALID);
  }
  if (loop->state != OMNI_EVENT_LOOP_INITIALIZED) {
    return result_from_loop(loop, OMNI_EVENT_LOOP_ERR_STATE, EINVAL);
  }
  if (!runtime_running(loop->runtime)) {
    return result_from_loop(loop, OMNI_EVENT_LOOP_ERR_RUNTIME, EINVAL);
  }

  loop->state = OMNI_EVENT_LOOP_RUNNING;
  while (loop->state == OMNI_EVENT_LOOP_RUNNING) {
    struct omni_reactor_result step_result;
    bool was_empty = omni_reactor_count(loop->runtime->reactor) == 0u;

    counter_add(&loop->iterations, 1u);
    step_result = omni_reactor_step(loop->runtime->reactor, loop->timeout_ms);
    loop->last_status = step_result.status;
    counter_add(&loop->events_processed, step_result.count);

    if (step_result.status != OMNI_REACTOR_OK) {
      loop->state = OMNI_EVENT_LOOP_STOPPED;
      return result_from_loop(loop, map_reactor_status(step_result.status),
                              step_result.sys_errno);
    }
    if (loop->stop_requested || loop->state == OMNI_EVENT_LOOP_STOPPING) {
      loop->state = OMNI_EVENT_LOOP_STOPPED;
      return result_from_loop(loop, OMNI_EVENT_LOOP_OK, 0);
    }
    if (!runtime_running(loop->runtime)) {
      loop->state = OMNI_EVENT_LOOP_STOPPED;
      return result_from_loop(loop, OMNI_EVENT_LOOP_ERR_RUNTIME, EINVAL);
    }
    /* The reactor's empty fast path cannot block, so repeating it would spin
     * forever in this no-wakeup foundation. Treat it as a clean idle exit. */
    if (was_empty || omni_reactor_count(loop->runtime->reactor) == 0u) {
      loop->state = OMNI_EVENT_LOOP_STOPPED;
      return result_from_loop(loop, OMNI_EVENT_LOOP_OK, 0);
    }
  }

  /* The loop only leaves the body through a stop request or an error. */
  loop->state = OMNI_EVENT_LOOP_STOPPED;
  return result_from_loop(loop, OMNI_EVENT_LOOP_OK, 0);
}

struct omni_event_loop_result omni_event_loop_stop(struct omni_event_loop *loop) {
  if (loop == NULL) {
    return make_result(OMNI_EVENT_LOOP_ERR_INVALID, EINVAL, OMNI_EVENT_LOOP_INERT,
                       OMNI_REACTOR_ERR_INVALID);
  }
  switch (loop->state) {
    case OMNI_EVENT_LOOP_RUNNING:
      loop->stop_requested = true;
      loop->state = OMNI_EVENT_LOOP_STOPPING;
      return result_from_loop(loop, OMNI_EVENT_LOOP_OK, 0);
    case OMNI_EVENT_LOOP_STOPPING:
    case OMNI_EVENT_LOOP_STOPPED:
      return result_from_loop(loop, OMNI_EVENT_LOOP_OK, 0);
    case OMNI_EVENT_LOOP_INERT:
    case OMNI_EVENT_LOOP_INITIALIZED:
      return result_from_loop(loop, OMNI_EVENT_LOOP_ERR_STATE, EINVAL);
  }
  return result_from_loop(loop, OMNI_EVENT_LOOP_ERR_STATE, EINVAL);
}

void omni_event_loop_destroy(struct omni_event_loop *loop) {
  if (loop == NULL || loop->state == OMNI_EVENT_LOOP_RUNNING ||
      loop->state == OMNI_EVENT_LOOP_STOPPING) {
    return;
  }
  clear_loop(loop, OMNI_EVENT_LOOP_INERT);
}

enum omni_event_loop_state omni_event_loop_state(const struct omni_event_loop *loop) {
  if (loop == NULL) {
    return OMNI_EVENT_LOOP_INERT;
  }
  return loop->state;
}

uint64_t omni_event_loop_iterations(const struct omni_event_loop *loop) {
  if (loop == NULL || loop->state == OMNI_EVENT_LOOP_INERT) {
    return 0u;
  }
  return loop->iterations;
}

uint64_t omni_event_loop_events_processed(const struct omni_event_loop *loop) {
  if (loop == NULL || loop->state == OMNI_EVENT_LOOP_INERT) {
    return 0u;
  }
  return loop->events_processed;
}

enum omni_reactor_status omni_event_loop_last_status(const struct omni_event_loop *loop) {
  if (loop == NULL || loop->state == OMNI_EVENT_LOOP_INERT) {
    return OMNI_REACTOR_ERR_INVALID;
  }
  return loop->last_status;
}
