/*
 * OmniRoute native backend — bounded listener readiness/admission bridge.
 *
 * This module only composes the listener descriptor accessor, generic
 * reactor registration, and Task 029 bounded admission drain. It performs no
 * descriptor operation and allocates no memory.
 */

#include "omniroute/listener_admission.h"

#include <errno.h>
#include <stdint.h>

#include "omniroute/poller.h"

static struct omni_connection_admission_result empty_admission_result(void) {
  struct omni_connection_admission_result result;

  result.status = OMNI_CONNECTION_ADMISSION_ERR_INVALID;
  result.sys_errno = 0;
  result.attempts = 0u;
  result.admitted = 0u;
  result.identity.slot_index = OMNI_CONNECTION_ADMISSION_SLOT_INVALID;
  result.identity.token = 0u;
  result.accept_status = OMNI_ACCEPT_ERR_INVALID;
  result.connection_status = OMNI_CONNECTION_ERR_INVALID;
  result.manager_status = OMNI_CONNECTION_MANAGER_ERR_INVALID;
  return result;
}

static struct omni_listener_admission_result make_result(
    enum omni_listener_admission_status status,
    int sys_errno,
    enum omni_listener_admission_state state) {
  struct omni_listener_admission_result result;

  result.status = status;
  result.sys_errno = sys_errno;
  result.state = state;
  result.readiness_events = 0u;
  result.reactor_status = OMNI_REACTOR_OK;
  result.admission_attempted = false;
  result.admission = empty_admission_result();
  return result;
}

static void record_result(struct omni_listener_admission *bridge,
                          struct omni_listener_admission_result result) {
  if (bridge == NULL) return;
  result.state = bridge->state;
  bridge->last_result = result;
}

static void add_saturating(uint64_t *counter, size_t amount) {
  uintmax_t current;
  uintmax_t increment;
  uintmax_t maximum = (uintmax_t)UINT64_MAX;

  if (counter == NULL || *counter == UINT64_MAX) return;
  current = (uintmax_t)*counter;
  increment = (uintmax_t)amount;
  if (increment >= maximum - current) {
    *counter = UINT64_MAX;
    return;
  }
  *counter = (uint64_t)(current + increment);
}

static bool admission_state_usable(const struct omni_connection_admission *admission) {
  enum omni_connection_admission_state state =
      omni_connection_admission_state(admission);

  return admission != NULL && admission->live &&
         (state == OMNI_CONNECTION_ADMISSION_INITIALIZED ||
          state == OMNI_CONNECTION_ADMISSION_ACTIVE);
}

static bool dependencies_usable(const struct omni_listener_admission *bridge) {
  return bridge != NULL && bridge->listener != NULL && bridge->reactor != NULL &&
         bridge->reactor->live &&
         omni_listener_fd(bridge->listener) != OMNI_LISTENER_FD_INVALID &&
         admission_state_usable(bridge->admission) &&
         bridge->admission->listener == bridge->listener;
}

static enum omni_listener_admission_status map_admission_status(
    enum omni_connection_admission_status status) {
  switch (status) {
    case OMNI_CONNECTION_ADMISSION_OK:
      return OMNI_LISTENER_ADMISSION_OK;
    case OMNI_CONNECTION_ADMISSION_DRAINED:
      return OMNI_LISTENER_ADMISSION_ADMISSION_DRAINED;
    case OMNI_CONNECTION_ADMISSION_LIMIT_REACHED:
      return OMNI_LISTENER_ADMISSION_ADMISSION_LIMIT_REACHED;
    case OMNI_CONNECTION_ADMISSION_SLOT_FULL:
    case OMNI_CONNECTION_ADMISSION_MANAGER_FULL:
      return OMNI_LISTENER_ADMISSION_ADMISSION_CAPACITY;
    case OMNI_CONNECTION_ADMISSION_INTERRUPTED:
      return OMNI_LISTENER_ADMISSION_ADMISSION_INTERRUPTED;
    case OMNI_CONNECTION_ADMISSION_TRANSIENT:
      return OMNI_LISTENER_ADMISSION_ADMISSION_TRANSIENT;
    case OMNI_CONNECTION_ADMISSION_ERR_STATE:
      return OMNI_LISTENER_ADMISSION_ADMISSION_STOPPED;
    case OMNI_CONNECTION_ADMISSION_ERR_INVALID:
    case OMNI_CONNECTION_ADMISSION_ERR_ACCEPT:
    case OMNI_CONNECTION_ADMISSION_ERR_CONNECTION:
    case OMNI_CONNECTION_ADMISSION_ERR_MANAGER:
    case OMNI_CONNECTION_ADMISSION_ERR_NOT_FOUND:
      return OMNI_LISTENER_ADMISSION_ADMISSION_FAILURE;
  }
  return OMNI_LISTENER_ADMISSION_ADMISSION_FAILURE;
}

static void listener_ready(uint64_t token, uint32_t events, void *context) {
  struct omni_listener_admission *bridge =
      (struct omni_listener_admission *)context;
  struct omni_listener_admission_result result;
  const uint32_t error_events = OMNI_POLLER_READY_ERROR |
                                OMNI_POLLER_READY_HANGUP |
                                OMNI_POLLER_READY_INVALID;

  if (bridge == NULL || bridge->state != OMNI_LISTENER_ADMISSION_ACTIVE ||
      token != OMNI_LISTENER_ADMISSION_REACTOR_TOKEN) {
    return;
  }

  add_saturating(&bridge->dispatches, 1u);
  result = make_result(OMNI_LISTENER_ADMISSION_OK, 0,
                       OMNI_LISTENER_ADMISSION_ACTIVE);
  result.readiness_events = events;

  if ((events & error_events) != 0u) {
    result.status = OMNI_LISTENER_ADMISSION_LISTENER_ERROR;
    record_result(bridge, result);
    return;
  }
  if ((events & OMNI_POLLER_READY_READ) == 0u) {
    /* WRITE and unknown non-error conditions never trigger admission. */
    record_result(bridge, result);
    return;
  }

  result.admission_attempted = true;
  result.admission = omni_connection_admission_drain(
      bridge->admission, bridge->max_admissions_per_dispatch,
      bridge->identities, bridge->identity_capacity);
  add_saturating(&bridge->total_admitted, result.admission.admitted);
  result.status = map_admission_status(result.admission.status);
  result.sys_errno = result.admission.sys_errno;
  record_result(bridge, result);
}

void omni_listener_admission_make_inert(struct omni_listener_admission *bridge) {
  if (bridge == NULL || bridge->state == OMNI_LISTENER_ADMISSION_INITIALIZED ||
      bridge->state == OMNI_LISTENER_ADMISSION_ACTIVE) {
    return;
  }
  bridge->listener = NULL;
  bridge->reactor = NULL;
  bridge->admission = NULL;
  bridge->identities = NULL;
  bridge->identity_capacity = 0u;
  bridge->max_admissions_per_dispatch = 0u;
  bridge->state = OMNI_LISTENER_ADMISSION_INERT;
  bridge->dispatches = 0u;
  bridge->total_admitted = 0u;
  bridge->last_result = make_result(OMNI_LISTENER_ADMISSION_OK, 0,
                                    OMNI_LISTENER_ADMISSION_INERT);
}

struct omni_listener_admission_result omni_listener_admission_init(
    struct omni_listener_admission *bridge,
    const struct omni_listener_admission_config *config) {
  struct omni_listener_admission_result result;

  if (bridge == NULL) {
    return make_result(OMNI_LISTENER_ADMISSION_ERR_INVALID, EINVAL,
                       OMNI_LISTENER_ADMISSION_INERT);
  }
  if (bridge->state != OMNI_LISTENER_ADMISSION_INERT &&
      bridge->state != OMNI_LISTENER_ADMISSION_CLOSED) {
    result = make_result(OMNI_LISTENER_ADMISSION_ERR_STATE, EINVAL, bridge->state);
    record_result(bridge, result);
    return result;
  }

  omni_listener_admission_make_inert(bridge);
  if (config == NULL || config->listener == NULL || config->reactor == NULL ||
      !config->reactor->live || config->admission == NULL ||
      !admission_state_usable(config->admission) ||
      config->admission->listener != config->listener ||
      omni_listener_fd(config->listener) == OMNI_LISTENER_FD_INVALID ||
      config->max_admissions_per_dispatch == 0u || config->identities == NULL ||
      config->identity_capacity < config->max_admissions_per_dispatch ||
      config->max_admissions_per_dispatch >
          SIZE_MAX / sizeof(*config->identities)) {
    result = make_result(OMNI_LISTENER_ADMISSION_ERR_INVALID, EINVAL,
                         OMNI_LISTENER_ADMISSION_INERT);
    record_result(bridge, result);
    return result;
  }

  bridge->listener = config->listener;
  bridge->reactor = config->reactor;
  bridge->admission = config->admission;
  bridge->identities = config->identities;
  bridge->identity_capacity = config->identity_capacity;
  bridge->max_admissions_per_dispatch = config->max_admissions_per_dispatch;
  bridge->state = OMNI_LISTENER_ADMISSION_INITIALIZED;
  result = make_result(OMNI_LISTENER_ADMISSION_OK, 0,
                       OMNI_LISTENER_ADMISSION_INITIALIZED);
  record_result(bridge, result);
  return result;
}

struct omni_listener_admission_result omni_listener_admission_start(
    struct omni_listener_admission *bridge) {
  struct omni_reactor_result reactor_result;
  struct omni_listener_admission_result result;

  if (bridge == NULL) {
    return make_result(OMNI_LISTENER_ADMISSION_ERR_INVALID, EINVAL,
                       OMNI_LISTENER_ADMISSION_INERT);
  }
  if (bridge->state != OMNI_LISTENER_ADMISSION_INITIALIZED) {
    result = make_result(OMNI_LISTENER_ADMISSION_ERR_STATE, EINVAL, bridge->state);
    record_result(bridge, result);
    return result;
  }
  if (!dependencies_usable(bridge)) {
    result = make_result(OMNI_LISTENER_ADMISSION_ERR_INVALID, EINVAL, bridge->state);
    record_result(bridge, result);
    return result;
  }

  reactor_result = omni_reactor_add(
      bridge->reactor, omni_listener_fd(bridge->listener),
      OMNI_LISTENER_ADMISSION_REACTOR_TOKEN, OMNI_POLLER_INTEREST_READ,
      listener_ready, bridge);
  if (reactor_result.status != OMNI_REACTOR_OK) {
    result = make_result(OMNI_LISTENER_ADMISSION_ERR_REACTOR_REGISTER,
                         reactor_result.sys_errno, bridge->state);
    result.reactor_status = reactor_result.status;
    record_result(bridge, result);
    return result;
  }

  bridge->state = OMNI_LISTENER_ADMISSION_ACTIVE;
  result = make_result(OMNI_LISTENER_ADMISSION_OK, 0, bridge->state);
  record_result(bridge, result);
  return result;
}

struct omni_listener_admission_result omni_listener_admission_stop(
    struct omni_listener_admission *bridge) {
  struct omni_listener_admission_result result;
  struct omni_reactor_result reactor_result;

  if (bridge == NULL) {
    return make_result(OMNI_LISTENER_ADMISSION_ERR_INVALID, EINVAL,
                       OMNI_LISTENER_ADMISSION_INERT);
  }
  if (bridge->state == OMNI_LISTENER_ADMISSION_STOPPED) {
    result = make_result(OMNI_LISTENER_ADMISSION_OK, 0, bridge->state);
    record_result(bridge, result);
    return result;
  }
  if (bridge->state == OMNI_LISTENER_ADMISSION_INITIALIZED) {
    bridge->state = OMNI_LISTENER_ADMISSION_STOPPED;
    result = make_result(OMNI_LISTENER_ADMISSION_OK, 0, bridge->state);
    record_result(bridge, result);
    return result;
  }
  if (bridge->state != OMNI_LISTENER_ADMISSION_ACTIVE) {
    result = make_result(OMNI_LISTENER_ADMISSION_ERR_STATE, EINVAL, bridge->state);
    record_result(bridge, result);
    return result;
  }

  reactor_result = omni_reactor_remove(
      bridge->reactor, OMNI_LISTENER_ADMISSION_REACTOR_TOKEN);
  if (reactor_result.status != OMNI_REACTOR_OK) {
    result = make_result(OMNI_LISTENER_ADMISSION_ERR_REACTOR_REMOVE,
                         reactor_result.sys_errno, bridge->state);
    result.reactor_status = reactor_result.status;
    record_result(bridge, result);
    return result;
  }

  bridge->state = OMNI_LISTENER_ADMISSION_STOPPED;
  result = make_result(OMNI_LISTENER_ADMISSION_OK, 0, bridge->state);
  record_result(bridge, result);
  return result;
}

struct omni_listener_admission_result omni_listener_admission_destroy(
    struct omni_listener_admission *bridge) {
  struct omni_listener_admission_result result;

  if (bridge == NULL) {
    return make_result(OMNI_LISTENER_ADMISSION_ERR_INVALID, EINVAL,
                       OMNI_LISTENER_ADMISSION_INERT);
  }
  if (bridge->state == OMNI_LISTENER_ADMISSION_ACTIVE) {
    result = omni_listener_admission_stop(bridge);
    if (result.status != OMNI_LISTENER_ADMISSION_OK) return result;
  }
  if (bridge->state != OMNI_LISTENER_ADMISSION_INERT &&
      bridge->state != OMNI_LISTENER_ADMISSION_INITIALIZED &&
      bridge->state != OMNI_LISTENER_ADMISSION_STOPPED &&
      bridge->state != OMNI_LISTENER_ADMISSION_CLOSED) {
    result = make_result(OMNI_LISTENER_ADMISSION_ERR_STATE, EINVAL, bridge->state);
    record_result(bridge, result);
    return result;
  }

  bridge->listener = NULL;
  bridge->reactor = NULL;
  bridge->admission = NULL;
  bridge->identities = NULL;
  bridge->identity_capacity = 0u;
  bridge->max_admissions_per_dispatch = 0u;
  bridge->state = OMNI_LISTENER_ADMISSION_CLOSED;
  result = make_result(OMNI_LISTENER_ADMISSION_OK, 0, bridge->state);
  record_result(bridge, result);
  return result;
}

enum omni_listener_admission_state omni_listener_admission_state(
    const struct omni_listener_admission *bridge) {
  return bridge == NULL ? OMNI_LISTENER_ADMISSION_INERT : bridge->state;
}

struct omni_listener_admission_result omni_listener_admission_last_result(
    const struct omni_listener_admission *bridge) {
  if (bridge == NULL) {
    return make_result(OMNI_LISTENER_ADMISSION_ERR_INVALID, EINVAL,
                       OMNI_LISTENER_ADMISSION_INERT);
  }
  return bridge->last_result;
}

uint64_t omni_listener_admission_dispatches(
    const struct omni_listener_admission *bridge) {
  if (bridge == NULL || bridge->state == OMNI_LISTENER_ADMISSION_INERT) return 0u;
  return bridge->dispatches;
}

uint64_t omni_listener_admission_total_admitted(
    const struct omni_listener_admission *bridge) {
  if (bridge == NULL || bridge->state == OMNI_LISTENER_ADMISSION_INERT) return 0u;
  return bridge->total_admitted;
}
