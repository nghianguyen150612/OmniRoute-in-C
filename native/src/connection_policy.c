/* Task 032 bounded connection lifecycle and interest policy. */

#include "omniroute/connection_policy.h"

#include <errno.h>
#include <string.h>

#include "omniroute/bytebuf.h"
#include "omniroute/connection_manager.h"
#include "omniroute/connection_runtime.h"
#include "omniroute/connection_session.h"
#include "omniroute/poller.h"

static struct omni_connection_policy_result inert_result(void) {
  struct omni_connection_policy_result result;

  (void)memset(&result, 0, sizeof(result));
  result.status = OMNI_CONNECTION_POLICY_ERR_STATE;
  result.action = OMNI_CONNECTION_POLICY_ACTION_NONE;
  result.reactor_status = OMNI_CONNECTION_REACTOR_ERR_INVALID;
  result.release_status = OMNI_CONNECTION_ADMISSION_ERR_STATE;
  result.release_manager_status = OMNI_CONNECTION_MANAGER_ERR_STATE;
  return result;
}

static struct omni_connection_policy_result make_result(
    enum omni_connection_policy_status status,
    enum omni_connection_policy_action action) {
  struct omni_connection_policy_result result = inert_result();

  result.status = status;
  result.action = action;
  result.reactor_status = OMNI_CONNECTION_REACTOR_OK;
  result.release_status = OMNI_CONNECTION_ADMISSION_OK;
  result.release_manager_status = OMNI_CONNECTION_MANAGER_OK;
  return result;
}

static uint64_t saturating_add(uint64_t value, uint64_t increment) {
  if (UINT64_MAX - value < increment) return UINT64_MAX;
  return value + increment;
}

static bool admission_state_usable(const struct omni_connection_admission *admission) {
  enum omni_connection_admission_state state = OMNI_CONNECTION_ADMISSION_CLOSED;

  if (admission == NULL || !admission->live || admission->manager == NULL ||
      admission->slots == NULL || admission->capacity == 0u) {
    return false;
  }
  state = omni_connection_admission_state(admission);
  return state == OMNI_CONNECTION_ADMISSION_INITIALIZED ||
         state == OMNI_CONNECTION_ADMISSION_ACTIVE ||
         state == OMNI_CONNECTION_ADMISSION_STOPPING;
}

static bool dependencies_usable(const struct omni_connection_admission *admission,
                                const struct omni_connection_dispatch *dispatch) {
  struct omni_connection_manager *manager = NULL;
  struct omni_connection_runtime *runtime = NULL;

  if (!admission_state_usable(admission) || dispatch == NULL ||
      dispatch->state != OMNI_CONNECTION_DISPATCH_ACTIVE ||
      dispatch->manager != admission->manager) {
    return false;
  }
  manager = admission->manager;
  if (!manager->live || manager->entries == NULL || manager->capacity == 0u ||
      manager->runtime == NULL) {
    return false;
  }
  runtime = manager->runtime;
  return omni_connection_runtime_is_initialized(runtime) && runtime->adapter != NULL &&
         runtime->adapter->live && runtime->adapter->registry != NULL &&
         runtime->adapter->registry->live && runtime->adapter->reactor != NULL &&
         runtime->adapter->reactor->live && runtime->adapter->reactor->poller != NULL &&
         runtime->adapter->reactor->poller->live;
}

static bool find_admitted_connection(struct omni_connection_policy *policy, uint64_t token,
                                     size_t *slot_index,
                                     struct omni_connection **connection) {
  struct omni_connection_manager *manager = NULL;

  if (policy == NULL || token == 0u || !admission_state_usable(policy->admission) ||
      slot_index == NULL || connection == NULL) {
    return false;
  }
  manager = policy->admission->manager;
  for (size_t i = 0u; i < policy->admission->capacity; ++i) {
    struct omni_connection_admission_slot *slot = &policy->admission->slots[i];
    const struct omni_connection_manager_entry *entry = NULL;

    if (!slot->occupied || slot->token != token) continue;
    entry = omni_connection_manager_find(manager, &slot->connection);
    if (entry == NULL || entry->token != token ||
        omni_connection_state(&slot->connection) != OMNI_CONNECTION_OPEN ||
        !omni_connection_is_live(&slot->connection)) {
      return false;
    }
    *slot_index = i;
    *connection = &slot->connection;
    return true;
  }
  return false;
}

static bool io_result_close_worthy(const struct omni_connection_io_result *io_result) {
  if (io_result == NULL) return false;
  return io_result->status == OMNI_CONNECTION_IO_ERR_EOF ||
         io_result->status == OMNI_CONNECTION_IO_ERR_IO ||
         io_result->status == OMNI_CONNECTION_IO_ERR_CLOSED;
}

static bool dispatch_close_worthy(const struct omni_connection_dispatch_result *dispatch) {
  if (dispatch == NULL) return false;
  if (dispatch->invalid_observed ||
      dispatch->status == OMNI_CONNECTION_DISPATCH_INVALID_EVENT) {
    return true;
  }
  if ((dispatch->read_attempted && io_result_close_worthy(&dispatch->read_result)) ||
      (dispatch->write_attempted && io_result_close_worthy(&dispatch->write_result))) {
    return true;
  }
  /* Task 031 marks ERROR/HANGUP close-worthy at observation time. Those bits
   * alone are hints: its direct I/O result determines whether to keep or close. */
  return dispatch->close_worthy && !dispatch->error_observed &&
         !dispatch->hangup_observed;
}

static struct omni_connection_policy_result ignore_result(
    struct omni_connection_policy *policy,
    struct omni_connection_policy_result result) {
  result.status = OMNI_CONNECTION_POLICY_IGNORED;
  result.action = OMNI_CONNECTION_POLICY_ACTION_IGNORE;
  if (policy != NULL) {
    policy->ignored_count = saturating_add(policy->ignored_count, 1u);
    policy->actions = saturating_add(policy->actions, 1u);
    policy->last_result = result;
  }
  return result;
}

static struct omni_connection_policy_result error_result(
    struct omni_connection_policy *policy,
    struct omni_connection_policy_result result,
    enum omni_connection_policy_status status,
    int sys_errno) {
  result.status = status;
  result.action = OMNI_CONNECTION_POLICY_ACTION_ERROR;
  result.sys_errno = sys_errno;
  if (policy != NULL) {
    policy->policy_errors = saturating_add(policy->policy_errors, 1u);
    policy->actions = saturating_add(policy->actions, 1u);
    policy->last_result = result;
  }
  return result;
}

static struct omni_connection_policy_result synchronize_connection(
    struct omni_connection_policy *policy,
    struct omni_connection_policy_result result,
    uint64_t token,
    struct omni_connection *connection) {
  struct omni_connection_session *session = NULL;
  struct omni_bytebuf *send_buffer = NULL;
  struct omni_connection_reactor *adapter = NULL;
  size_t pending = 0u;

  result.old_interests = omni_connection_poller_interests(connection);
  result.new_interests = result.old_interests;
  session = omni_connection_runtime_find_session(policy->admission->manager->runtime,
                                                  connection);
  if (session == NULL) {
    return error_result(policy, result, OMNI_CONNECTION_POLICY_ERR_DEPENDENCY, EINVAL);
  }
  send_buffer = omni_connection_session_send_buffer(session);
  if (send_buffer == NULL) {
    return error_result(policy, result, OMNI_CONNECTION_POLICY_ERR_DEPENDENCY, EINVAL);
  }
  pending = omni_bytebuf_readable(send_buffer);
  result.desired_interests = OMNI_POLLER_INTEREST_READ;
  if (pending != 0u) result.desired_interests |= OMNI_POLLER_INTEREST_WRITE;

  if (result.old_interests == result.desired_interests) {
    result.status = OMNI_CONNECTION_POLICY_OK;
    result.action = OMNI_CONNECTION_POLICY_ACTION_KEEP;
  } else {
    struct omni_connection_reactor_result update;
    adapter = policy->admission->manager->runtime->adapter;
    update = omni_connection_reactor_update_interests(adapter, token,
                                                       result.desired_interests);
    result.reactor_status = update.status;
    result.sys_errno = update.sys_errno;
    if (update.status != OMNI_CONNECTION_REACTOR_OK) {
      return error_result(policy, result, OMNI_CONNECTION_POLICY_ERR_INTEREST_UPDATE,
                          update.sys_errno);
    }
    result.status = OMNI_CONNECTION_POLICY_OK;
    result.action = OMNI_CONNECTION_POLICY_ACTION_UPDATE_INTERESTS;
    result.new_interests = result.desired_interests;
    result.interest_updated = true;
    policy->interest_updates = saturating_add(policy->interest_updates, 1u);
  }
  policy->actions = saturating_add(policy->actions, 1u);
  policy->last_result = result;
  return result;
}

void omni_connection_policy_make_inert(struct omni_connection_policy *policy) {
  if (policy == NULL) return;
  (void)memset(policy, 0, sizeof(*policy));
  policy->state = OMNI_CONNECTION_POLICY_INERT;
  policy->last_result = inert_result();
}

struct omni_connection_policy_result omni_connection_policy_init(
    struct omni_connection_policy *policy,
    const struct omni_connection_policy_config *config) {
  if (policy == NULL || config == NULL || config->admission == NULL ||
      config->dispatch == NULL) {
    return make_result(OMNI_CONNECTION_POLICY_ERR_INVALID,
                       OMNI_CONNECTION_POLICY_ACTION_ERROR);
  }
  if (policy->state != OMNI_CONNECTION_POLICY_INERT) {
    return make_result(OMNI_CONNECTION_POLICY_ERR_STATE,
                       OMNI_CONNECTION_POLICY_ACTION_ERROR);
  }
  if (!dependencies_usable(config->admission, config->dispatch)) {
    return make_result(OMNI_CONNECTION_POLICY_ERR_DEPENDENCY,
                       OMNI_CONNECTION_POLICY_ACTION_ERROR);
  }
  policy->admission = config->admission;
  policy->dispatch = config->dispatch;
  policy->state = OMNI_CONNECTION_POLICY_ACTIVE;
  policy->last_result = make_result(OMNI_CONNECTION_POLICY_OK,
                                    OMNI_CONNECTION_POLICY_ACTION_NONE);
  return policy->last_result;
}

void omni_connection_policy_destroy(struct omni_connection_policy *policy) {
  if (policy == NULL || policy->state == OMNI_CONNECTION_POLICY_CLOSED) return;
  policy->admission = NULL;
  policy->dispatch = NULL;
  policy->state = OMNI_CONNECTION_POLICY_CLOSED;
}

struct omni_connection_policy_result omni_connection_policy_sync_interests(
    struct omni_connection_policy *policy, uint64_t token) {
  struct omni_connection_policy_result result =
      make_result(OMNI_CONNECTION_POLICY_ERR_STATE, OMNI_CONNECTION_POLICY_ACTION_NONE);
  struct omni_connection *connection = NULL;
  size_t slot_index = 0u;

  if (policy == NULL) {
    return make_result(OMNI_CONNECTION_POLICY_ERR_INVALID,
                       OMNI_CONNECTION_POLICY_ACTION_ERROR);
  }
  if (policy->state != OMNI_CONNECTION_POLICY_ACTIVE) {
    return make_result(OMNI_CONNECTION_POLICY_ERR_STATE,
                       OMNI_CONNECTION_POLICY_ACTION_NONE);
  }
  if (!dependencies_usable(policy->admission, policy->dispatch)) {
    return error_result(policy, result, OMNI_CONNECTION_POLICY_ERR_STATE, EINVAL);
  }
  policy->sync_calls = saturating_add(policy->sync_calls, 1u);
  if (token == 0u) {
    return error_result(policy, result, OMNI_CONNECTION_POLICY_ERR_INVALID, EINVAL);
  }
  if (!find_admitted_connection(policy, token, &slot_index, &connection)) {
    result.dispatch.token = token;
    (void)slot_index;
    return ignore_result(policy, result);
  }
  return synchronize_connection(policy, result, token, connection);
}

struct omni_connection_policy_result omni_connection_policy_apply(
    struct omni_connection_policy *policy,
    const struct omni_connection_dispatch_result *dispatch_result) {
  struct omni_connection_policy_result result =
      make_result(OMNI_CONNECTION_POLICY_ERR_STATE, OMNI_CONNECTION_POLICY_ACTION_NONE);
  struct omni_connection *connection = NULL;
  struct omni_connection_admission_identity identity;
  struct omni_connection_admission_result release_result;
  size_t slot_index = 0u;

  if (policy == NULL) {
    return make_result(OMNI_CONNECTION_POLICY_ERR_INVALID,
                       OMNI_CONNECTION_POLICY_ACTION_ERROR);
  }
  if (policy->state != OMNI_CONNECTION_POLICY_ACTIVE) {
    return make_result(OMNI_CONNECTION_POLICY_ERR_STATE,
                       OMNI_CONNECTION_POLICY_ACTION_NONE);
  }
  if (!dependencies_usable(policy->admission, policy->dispatch)) {
    return error_result(policy, result, OMNI_CONNECTION_POLICY_ERR_STATE, EINVAL);
  }
  policy->apply_calls = saturating_add(policy->apply_calls, 1u);
  if (dispatch_result == NULL) {
    return error_result(policy, result, OMNI_CONNECTION_POLICY_ERR_INVALID, EINVAL);
  }
  result.has_dispatch = true;
  result.dispatch = *dispatch_result;
  if (dispatch_result->ignored || dispatch_result->stale ||
      dispatch_result->status == OMNI_CONNECTION_DISPATCH_IGNORED ||
      dispatch_result->status == OMNI_CONNECTION_DISPATCH_STALE) {
    return ignore_result(policy, result);
  }
  if (dispatch_result->status != OMNI_CONNECTION_DISPATCH_OK &&
      dispatch_result->status != OMNI_CONNECTION_DISPATCH_INVALID_EVENT) {
    return error_result(policy, result, OMNI_CONNECTION_POLICY_ERR_DEPENDENCY,
                        dispatch_result->status == OMNI_CONNECTION_DISPATCH_ERR_INVALID ?
                            EINVAL : EPROTO);
  }
  if (dispatch_result->token == 0u) {
    return error_result(policy, result, OMNI_CONNECTION_POLICY_ERR_INVALID, EINVAL);
  }
  if (!find_admitted_connection(policy, dispatch_result->token, &slot_index,
                                &connection)) {
    return ignore_result(policy, result);
  }
  result.old_interests = omni_connection_poller_interests(connection);
  result.new_interests = result.old_interests;

  if (dispatch_close_worthy(dispatch_result)) {
    identity.slot_index = slot_index;
    identity.token = dispatch_result->token;
    result.desired_interests = 0u;
    policy->close_worthy_observations =
        saturating_add(policy->close_worthy_observations, 1u);
    release_result = omni_connection_admission_release(policy->admission, identity);
    result.release_status = release_result.status;
    result.release_manager_status = release_result.manager_status;
    result.sys_errno = release_result.sys_errno;
    if (release_result.status != OMNI_CONNECTION_ADMISSION_OK) {
      return error_result(policy, result, OMNI_CONNECTION_POLICY_ERR_RELEASE,
                          release_result.sys_errno);
    }
    result.status = OMNI_CONNECTION_POLICY_OK;
    result.action = OMNI_CONNECTION_POLICY_ACTION_CLOSE;
    result.new_interests = 0u;
    result.connection_released = true;
    policy->closed_count = saturating_add(policy->closed_count, 1u);
    policy->actions = saturating_add(policy->actions, 1u);
    policy->last_result = result;
    return result;
  }

  return synchronize_connection(policy, result, dispatch_result->token, connection);
}

void omni_connection_policy_callback(struct omni_connection *connection,
                                     uint64_t token,
                                     uint32_t events,
                                     void *context) {
  struct omni_connection_policy *policy = (struct omni_connection_policy *)context;
  struct omni_connection_dispatch_result dispatch_result;
  uint32_t dispatch_events = events;

  if (policy == NULL || policy->state != OMNI_CONNECTION_POLICY_ACTIVE ||
      policy->dispatch == NULL ||
      policy->dispatch->state != OMNI_CONNECTION_DISPATCH_ACTIVE) {
    return;
  }
  /* HANGUP/ERROR may coexist with unread payload bytes. Make one bounded READ
   * attempt (unless INVALID forbids I/O) so EOF is not inferred ahead of data. */
  if ((events & OMNI_POLLER_READY_INVALID) == 0u &&
      (events & (OMNI_POLLER_READY_ERROR | OMNI_POLLER_READY_HANGUP)) != 0u) {
    dispatch_events |= OMNI_POLLER_READY_READ;
  }
  omni_connection_dispatch_callback(connection, token, dispatch_events,
                                    policy->dispatch);
  dispatch_result = omni_connection_dispatch_last_result(policy->dispatch);
  (void)omni_connection_policy_apply(policy, &dispatch_result);
}

enum omni_connection_policy_state omni_connection_policy_state(
    const struct omni_connection_policy *policy) {
  return policy == NULL ? OMNI_CONNECTION_POLICY_INERT : policy->state;
}

struct omni_connection_policy_result omni_connection_policy_last_result(
    const struct omni_connection_policy *policy) {
  return policy == NULL ? inert_result() : policy->last_result;
}

#define OMNI_POLICY_COUNTER_ACCESSOR(name, field) \
  uint64_t name(const struct omni_connection_policy *policy) { \
    return policy == NULL ? 0u : policy->field; \
  }

OMNI_POLICY_COUNTER_ACCESSOR(omni_connection_policy_actions, actions)
OMNI_POLICY_COUNTER_ACCESSOR(omni_connection_policy_closed_count, closed_count)
OMNI_POLICY_COUNTER_ACCESSOR(omni_connection_policy_interest_updates, interest_updates)
OMNI_POLICY_COUNTER_ACCESSOR(omni_connection_policy_ignored_count, ignored_count)
OMNI_POLICY_COUNTER_ACCESSOR(omni_connection_policy_close_worthy_observations,
                             close_worthy_observations)
OMNI_POLICY_COUNTER_ACCESSOR(omni_connection_policy_errors, policy_errors)

#undef OMNI_POLICY_COUNTER_ACCESSOR
