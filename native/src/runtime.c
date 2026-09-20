/*
 * OmniRoute native backend — bounded runtime coordinator implementation.
 *
 * This file only sequences existing lifecycle primitives. It contains no
 * descriptor calls, readiness waits, payload operations, allocator calls,
 * queues, timers, threads, or protocol handling.
 */

#include "omniroute/runtime.h"

#include <errno.h>
#include <stdint.h>

static void clear_context(struct omni_runtime *runtime, enum omni_runtime_state state) {
  struct omni_poller empty_poller = { 0 };

  runtime->listener = NULL;
  runtime->registry = NULL;
  runtime->reactor = NULL;
  runtime->poller = empty_poller;
  runtime->state = state;
  runtime->registry_initialized = false;
  runtime->poller_initialized = false;
  runtime->reactor_initialized = false;
  runtime->listener_initialized = false;
}

static struct omni_runtime_result make_result(enum omni_runtime_status status,
                                              int sys_errno,
                                              enum omni_runtime_state state) {
  struct omni_runtime_result result;

  result.status = status;
  result.sys_errno = sys_errno;
  result.state = state;
  return result;
}

static bool capacity_fits(size_t capacity, size_t element_size) {
  return capacity <= SIZE_MAX / element_size;
}

static bool config_valid(const struct omni_runtime_config *config) {
  if (config == NULL || config->listener == NULL || config->registry == NULL ||
      config->reactor == NULL || config->registry_slots == NULL ||
      config->poller_fds == NULL || config->poller_tokens == NULL ||
      config->reactor_registrations == NULL || config->reactor_events == NULL ||
      config->registry_capacity == 0u || config->poller_capacity == 0u ||
      config->reactor_capacity == 0u) {
    return false;
  }
  if (config->registry->live || config->reactor->live || config->listener->live) {
    return false;
  }
  if (config->registry_capacity > (size_t)UINT32_MAX ||
      !capacity_fits(config->registry_capacity, sizeof(*config->registry_slots)) ||
      !capacity_fits(config->poller_capacity, sizeof(*config->poller_fds)) ||
      !capacity_fits(config->poller_capacity, sizeof(*config->poller_tokens)) ||
      !capacity_fits(config->reactor_capacity, sizeof(*config->reactor_registrations)) ||
      !capacity_fits(config->reactor_capacity, sizeof(*config->reactor_events))) {
    return false;
  }
  return true;
}

static bool dependencies_live(const struct omni_runtime *runtime) {
  return runtime != NULL && runtime->registry_initialized && runtime->poller_initialized &&
         runtime->reactor_initialized && runtime->listener_initialized &&
         runtime->registry != NULL && runtime->registry->live && runtime->poller.live &&
         runtime->reactor != NULL && runtime->reactor->live && runtime->listener != NULL &&
         runtime->listener->live;
}

static void cleanup_initialized(struct omni_runtime *runtime) {
  if (runtime == NULL) {
    return;
  }

  /* The coordinator has no accept loop to interrupt; entering STOPPING is
   * the explicit stop-accepting boundary before subsystem teardown. */
  runtime->state = OMNI_RUNTIME_STOPPING;

  /* Reverse of init: remove readiness registrations before forgetting the
   * registry identities they may refer to. */
  if (runtime->reactor_initialized && runtime->reactor != NULL) {
    omni_reactor_destroy(runtime->reactor);
  }
  runtime->reactor_initialized = false;

  if (runtime->registry_initialized && runtime->registry != NULL) {
    omni_connection_registry_destroy(runtime->registry);
  }
  runtime->registry_initialized = false;

  if (runtime->poller_initialized) {
    omni_poller_destroy(&runtime->poller);
  }
  runtime->poller_initialized = false;

  if (runtime->listener_initialized && runtime->listener != NULL) {
    omni_listener_destroy(runtime->listener);
  }
  runtime->listener_initialized = false;

  runtime->listener = NULL;
  runtime->registry = NULL;
  runtime->reactor = NULL;
  runtime->poller = (struct omni_poller){ 0 };
  runtime->state = OMNI_RUNTIME_STOPPED;
}

void omni_runtime_make_inert(struct omni_runtime *runtime) {
  if (runtime == NULL) {
    return;
  }
  if (runtime->state == OMNI_RUNTIME_INITIALIZED ||
      runtime->state == OMNI_RUNTIME_RUNNING ||
      runtime->state == OMNI_RUNTIME_STOPPING) {
    return;
  }
  clear_context(runtime, OMNI_RUNTIME_INERT);
}

struct omni_runtime_result omni_runtime_init(struct omni_runtime *runtime,
                                             const struct omni_runtime_config *config) {
  struct omni_connection_registry_result registry_result;
  struct omni_poller_result poller_result;
  struct omni_reactor_result reactor_result;
  struct omni_listener_result listener_result;
  enum omni_runtime_state prior_state = OMNI_RUNTIME_INERT;

  if (runtime == NULL) {
    return make_result(OMNI_RUNTIME_ERR_INVALID, EINVAL, OMNI_RUNTIME_INERT);
  }
  prior_state = runtime->state;
  if (prior_state == OMNI_RUNTIME_INITIALIZED || prior_state == OMNI_RUNTIME_RUNNING ||
      prior_state == OMNI_RUNTIME_STOPPING) {
    return make_result(OMNI_RUNTIME_ERR_STATE, EINVAL, prior_state);
  }
  if (prior_state != OMNI_RUNTIME_INERT && prior_state != OMNI_RUNTIME_STOPPED) {
    return make_result(OMNI_RUNTIME_ERR_STATE, EINVAL, prior_state);
  }
  if (!config_valid(config)) {
    clear_context(runtime, OMNI_RUNTIME_INERT);
    return make_result(OMNI_RUNTIME_ERR_INVALID, EINVAL, OMNI_RUNTIME_INERT);
  }

  clear_context(runtime, OMNI_RUNTIME_INERT);
  runtime->listener = config->listener;
  runtime->registry = config->registry;
  runtime->reactor = config->reactor;

  /* Startup order is deliberate and kept explicit for partial-failure
   * cleanup: registry, poller support, reactor, listener. */
  registry_result = omni_connection_registry_init(runtime->registry,
                                                   config->registry_slots,
                                                   config->registry_capacity);
  if (registry_result.status != OMNI_CONNECTION_REGISTRY_OK) {
    clear_context(runtime, OMNI_RUNTIME_INERT);
    return make_result(OMNI_RUNTIME_ERR_REGISTRY, registry_result.sys_errno,
                       OMNI_RUNTIME_INERT);
  }
  runtime->registry_initialized = true;

  poller_result = omni_poller_init_borrowed(&runtime->poller, config->poller_fds,
                                            config->poller_tokens, config->poller_capacity);
  if (poller_result.status != OMNI_POLLER_OK) {
    cleanup_initialized(runtime);
    clear_context(runtime, OMNI_RUNTIME_INERT);
    return make_result(OMNI_RUNTIME_ERR_POLLER, poller_result.sys_errno,
                       OMNI_RUNTIME_INERT);
  }
  runtime->poller_initialized = true;

  reactor_result = omni_reactor_init(runtime->reactor, &runtime->poller,
                                     config->reactor_registrations,
                                     config->reactor_events, config->reactor_capacity);
  if (reactor_result.status != OMNI_REACTOR_OK) {
    cleanup_initialized(runtime);
    clear_context(runtime, OMNI_RUNTIME_INERT);
    return make_result(OMNI_RUNTIME_ERR_REACTOR, reactor_result.sys_errno,
                       OMNI_RUNTIME_INERT);
  }
  runtime->reactor_initialized = true;

  listener_result = omni_listener_init(runtime->listener, config->listener_address,
                                       config->listener_port);
  if (listener_result.status != OMNI_LISTENER_OK) {
    cleanup_initialized(runtime);
    clear_context(runtime, OMNI_RUNTIME_INERT);
    return make_result(OMNI_RUNTIME_ERR_LISTENER, listener_result.sys_errno,
                       OMNI_RUNTIME_INERT);
  }
  runtime->listener_initialized = true;
  runtime->state = OMNI_RUNTIME_INITIALIZED;
  return make_result(OMNI_RUNTIME_OK, 0, runtime->state);
}

struct omni_runtime_result omni_runtime_start(struct omni_runtime *runtime) {
  if (runtime == NULL) {
    return make_result(OMNI_RUNTIME_ERR_INVALID, EINVAL, OMNI_RUNTIME_INERT);
  }
  if (runtime->state != OMNI_RUNTIME_INITIALIZED) {
    return make_result(OMNI_RUNTIME_ERR_STATE, EINVAL, runtime->state);
  }
  if (!dependencies_live(runtime)) {
    return make_result(OMNI_RUNTIME_ERR_INVALID, EINVAL, runtime->state);
  }
  runtime->state = OMNI_RUNTIME_RUNNING;
  return make_result(OMNI_RUNTIME_OK, 0, runtime->state);
}

struct omni_runtime_result omni_runtime_stop(struct omni_runtime *runtime) {
  if (runtime == NULL) {
    return make_result(OMNI_RUNTIME_ERR_INVALID, EINVAL, OMNI_RUNTIME_INERT);
  }
  if (runtime->state != OMNI_RUNTIME_RUNNING) {
    return make_result(OMNI_RUNTIME_ERR_STATE, EINVAL, runtime->state);
  }
  cleanup_initialized(runtime);
  return make_result(OMNI_RUNTIME_OK, 0, runtime->state);
}

void omni_runtime_destroy(struct omni_runtime *runtime) {
  if (runtime == NULL) {
    return;
  }
  if (runtime->state == OMNI_RUNTIME_INITIALIZED ||
      runtime->state == OMNI_RUNTIME_RUNNING ||
      runtime->state == OMNI_RUNTIME_STOPPING) {
    cleanup_initialized(runtime);
  }
}

enum omni_runtime_state omni_runtime_state(const struct omni_runtime *runtime) {
  if (runtime == NULL) {
    return OMNI_RUNTIME_INERT;
  }
  return runtime->state;
}
