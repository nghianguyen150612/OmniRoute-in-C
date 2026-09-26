/*
 * OmniRoute native backend — bounded listener-to-manager admission (Task 029).
 *
 * This module coordinates existing listener, accepted-owner, connection, and
 * manager lifecycle APIs. It owns no descriptor syscall and allocates no
 * memory.
 */

#include "omniroute/connection_admission.h"

#include <errno.h>
#include <stdint.h>

#include "omniroute/poller.h"

static struct omni_connection_admission_result make_result(
    enum omni_connection_admission_status status, int sys_errno) {
  struct omni_connection_admission_result result;

  result.status = status;
  result.sys_errno = sys_errno;
  result.attempts = 0u;
  result.admitted = 0u;
  result.identity.slot_index = OMNI_CONNECTION_ADMISSION_SLOT_INVALID;
  result.identity.token = 0u;
  result.accept_status = OMNI_ACCEPT_ERR_INVALID;
  result.connection_status = OMNI_CONNECTION_ERR_INVALID;
  result.manager_status = OMNI_CONNECTION_MANAGER_ERR_INVALID;
  return result;
}

static bool multiply_size(size_t left, size_t right, size_t *product) {
  if (product == NULL || (right != 0u && left > SIZE_MAX / right)) {
    return false;
  }
  *product = left * right;
  return true;
}

static bool region_bounds(const void *storage, size_t bytes,
                          uintptr_t *start, uintptr_t *end) {
  uintptr_t address;

  if (storage == NULL || bytes == 0u || start == NULL || end == NULL) {
    return false;
  }
  address = (uintptr_t)storage;
  if ((uintmax_t)bytes > (uintmax_t)(UINTPTR_MAX - address)) {
    return false;
  }
  *start = address;
  *end = address + (uintptr_t)bytes;
  return true;
}

static bool regions_overlap(const void *left, size_t left_bytes,
                            const void *right, size_t right_bytes) {
  uintptr_t left_start;
  uintptr_t left_end;
  uintptr_t right_start;
  uintptr_t right_end;

  if (!region_bounds(left, left_bytes, &left_start, &left_end) ||
      !region_bounds(right, right_bytes, &right_start, &right_end)) {
    return true;
  }
  return left_start < right_end && right_start < left_end;
}

static unsigned char *buffer_slice(void *storage, size_t stride, size_t index) {
  if (storage == NULL) return NULL;
  return (unsigned char *)storage + (stride * index);
}

void omni_connection_admission_make_inert(struct omni_connection_admission *admission) {
  if (admission == NULL) return;
  admission->listener = NULL;
  admission->manager = NULL;
  admission->slots = NULL;
  admission->capacity = 0u;
  admission->count = 0u;
  admission->connection_receive_storage = NULL;
  admission->connection_receive_stride = 0u;
  admission->session_receive_storage = NULL;
  admission->session_receive_stride = 0u;
  admission->session_send_storage = NULL;
  admission->session_send_stride = 0u;
  admission->state = OMNI_CONNECTION_ADMISSION_NEW;
  admission->live = false;
}

void omni_connection_admission_slot_make_inert(struct omni_connection_admission_slot *slot) {
  if (slot == NULL) return;
  omni_connection_make_inert(&slot->connection);
  slot->token = 0u;
  slot->occupied = false;
}

struct omni_connection_admission_result omni_connection_admission_init(
    struct omni_connection_admission *admission,
    const struct omni_connection_admission_config *config) {
  size_t slots_required = 0u;
  size_t connection_receive_required = 0u;
  size_t session_receive_required = 0u;
  size_t session_send_required = 0u;

  if (admission == NULL || config == NULL) {
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_INVALID, EINVAL);
  }
  if (admission->live || admission->state != OMNI_CONNECTION_ADMISSION_NEW) {
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_STATE, EINVAL);
  }
  if (config->listener == NULL || config->manager == NULL || config->slots == NULL ||
      config->capacity == 0u || omni_listener_fd(config->listener) == OMNI_LISTENER_FD_INVALID) {
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_INVALID, EINVAL);
  }
  if (omni_connection_manager_state(config->manager) !=
          OMNI_CONNECTION_MANAGER_INITIALIZED &&
      omni_connection_manager_state(config->manager) != OMNI_CONNECTION_MANAGER_RUNNING) {
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_INVALID, EINVAL);
  }
  if (config->capacity > omni_connection_manager_capacity(config->manager) ||
      !multiply_size(config->capacity, sizeof(*config->slots), &slots_required) ||
      config->slots_bytes < slots_required ||
      config->connection_receive.per_connection_capacity == 0u ||
      config->session_receive.per_connection_capacity == 0u ||
      config->session_send.per_connection_capacity == 0u ||
      !multiply_size(config->capacity,
                     config->connection_receive.per_connection_capacity,
                     &connection_receive_required) ||
      !multiply_size(config->capacity,
                     config->session_receive.per_connection_capacity,
                     &session_receive_required) ||
      !multiply_size(config->capacity, config->session_send.per_connection_capacity,
                     &session_send_required) ||
      config->connection_receive.storage == NULL ||
      config->connection_receive.storage_bytes < connection_receive_required ||
      config->session_receive.storage == NULL ||
      config->session_receive.storage_bytes < session_receive_required ||
      config->session_send.storage == NULL ||
      config->session_send.storage_bytes < session_send_required) {
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_INVALID, EINVAL);
  }
  if (regions_overlap(config->slots, slots_required,
                      config->connection_receive.storage, connection_receive_required) ||
      regions_overlap(config->slots, slots_required,
                      config->session_receive.storage, session_receive_required) ||
      regions_overlap(config->slots, slots_required,
                      config->session_send.storage, session_send_required) ||
      regions_overlap(config->connection_receive.storage, connection_receive_required,
                      config->session_receive.storage, session_receive_required) ||
      regions_overlap(config->connection_receive.storage, connection_receive_required,
                      config->session_send.storage, session_send_required) ||
      regions_overlap(config->session_receive.storage, session_receive_required,
                      config->session_send.storage, session_send_required)) {
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_INVALID, EINVAL);
  }
  for (size_t i = 0u; i < config->capacity; ++i) {
    const struct omni_connection_admission_slot *slot = &config->slots[i];
    enum omni_connection_state connection_state = omni_connection_state(&slot->connection);
    if (slot->occupied || slot->token != 0u ||
        (connection_state != OMNI_CONNECTION_INERT &&
         connection_state != OMNI_CONNECTION_CLOSED)) {
      return make_result(OMNI_CONNECTION_ADMISSION_ERR_INVALID, EINVAL);
    }
  }

  admission->listener = config->listener;
  admission->manager = config->manager;
  admission->slots = config->slots;
  admission->capacity = config->capacity;
  admission->count = 0u;
  admission->connection_receive_storage = config->connection_receive.storage;
  admission->connection_receive_stride = config->connection_receive.per_connection_capacity;
  admission->session_receive_storage = config->session_receive.storage;
  admission->session_receive_stride = config->session_receive.per_connection_capacity;
  admission->session_send_storage = config->session_send.storage;
  admission->session_send_stride = config->session_send.per_connection_capacity;
  admission->state = OMNI_CONNECTION_ADMISSION_INITIALIZED;
  admission->live = true;
  return make_result(OMNI_CONNECTION_ADMISSION_OK, 0);
}

static struct omni_connection_admission_result validate_active(
    struct omni_connection_admission *admission) {
  enum omni_connection_manager_state manager_state;

  if (admission == NULL || !admission->live) {
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_STATE, EINVAL);
  }
  if (admission->state != OMNI_CONNECTION_ADMISSION_INITIALIZED &&
      admission->state != OMNI_CONNECTION_ADMISSION_ACTIVE) {
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_STATE, EINVAL);
  }
  if (admission->listener == NULL ||
      omni_listener_fd(admission->listener) == OMNI_LISTENER_FD_INVALID ||
      admission->manager == NULL) {
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_INVALID, EINVAL);
  }
  manager_state = omni_connection_manager_state(admission->manager);
  if (manager_state != OMNI_CONNECTION_MANAGER_INITIALIZED &&
      manager_state != OMNI_CONNECTION_MANAGER_RUNNING) {
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_STATE, EINVAL);
  }
  return make_result(OMNI_CONNECTION_ADMISSION_OK, 0);
}

static struct omni_connection_admission_slot *find_free_slot(
    struct omni_connection_admission *admission, size_t *index_out) {
  if (admission == NULL || admission->slots == NULL || index_out == NULL) return NULL;
  for (size_t i = 0u; i < admission->capacity; ++i) {
    if (!admission->slots[i].occupied) {
      *index_out = i;
      return &admission->slots[i];
    }
  }
  return NULL;
}

static struct omni_connection_admission_result admit_once_internal(
    struct omni_connection_admission *admission) {
  struct omni_connection_admission_result result =
      validate_active(admission);
  struct omni_connection_admission_slot *slot = NULL;
  struct omni_accepted accepted;
  struct omni_accept_result accept_result;
  struct omni_connection_config connection_config = {0};
  struct omni_connection_result connection_result;
  struct omni_connection_manager_attach_config attach_config = {0};
  struct omni_connection_manager_result manager_result;
  size_t slot_index = 0u;

  if (result.status != OMNI_CONNECTION_ADMISSION_OK) return result;
  if (admission->count >= admission->capacity) {
    return make_result(OMNI_CONNECTION_ADMISSION_SLOT_FULL, ENOSPC);
  }
  slot = find_free_slot(admission, &slot_index);
  if (slot == NULL) {
    return make_result(OMNI_CONNECTION_ADMISSION_SLOT_FULL, ENOSPC);
  }
  if (omni_connection_manager_count(admission->manager) >=
      omni_connection_manager_capacity(admission->manager)) {
    result = make_result(OMNI_CONNECTION_ADMISSION_MANAGER_FULL, ENOSPC);
    result.manager_status = OMNI_CONNECTION_MANAGER_ERR_FULL;
    return result;
  }

  omni_accepted_make_inert(&accepted);
  accept_result = omni_accept_once(admission->listener, &accepted);
  result.attempts = 1u;
  result.accept_status = accept_result.status;
  result.sys_errno = accept_result.sys_errno;
  if (accept_result.status == OMNI_ACCEPT_DRAINED) {
    result.status = OMNI_CONNECTION_ADMISSION_DRAINED;
    return result;
  }
  if (accept_result.status == OMNI_ACCEPT_INTERRUPTED) {
    result.status = OMNI_CONNECTION_ADMISSION_INTERRUPTED;
    return result;
  }
  if (accept_result.status == OMNI_ACCEPT_TRANSIENT) {
    result.status = OMNI_CONNECTION_ADMISSION_TRANSIENT;
    return result;
  }
  if (accept_result.status != OMNI_ACCEPT_OK || accept_result.accepted != 1u) {
    result.status = OMNI_CONNECTION_ADMISSION_ERR_ACCEPT;
    return result;
  }

  omni_connection_make_inert(&slot->connection);
  connection_config.receive_storage =
      buffer_slice(admission->connection_receive_storage,
                   admission->connection_receive_stride, slot_index);
  connection_config.receive_capacity = admission->connection_receive_stride;
  connection_config.poller_token = 0u;
  connection_config.poller_interests = OMNI_POLLER_INTEREST_READ;
  connection_result = omni_connection_init(&slot->connection, &connection_config);
  if (connection_result.status != OMNI_CONNECTION_OK) {
    omni_accepted_destroy(&accepted);
    omni_connection_destroy(&slot->connection);
    result.status = OMNI_CONNECTION_ADMISSION_ERR_CONNECTION;
    result.sys_errno = connection_result.sys_errno;
    result.connection_status = connection_result.status;
    return result;
  }

  connection_result = omni_connection_from_accepted(&slot->connection, &accepted);
  if (connection_result.status != OMNI_CONNECTION_OK) {
    omni_accepted_destroy(&accepted);
    omni_connection_destroy(&slot->connection);
    result.status = OMNI_CONNECTION_ADMISSION_ERR_CONNECTION;
    result.sys_errno = connection_result.sys_errno;
    result.connection_status = connection_result.status;
    return result;
  }

  attach_config.connection = &slot->connection;
  attach_config.receive_storage =
      buffer_slice(admission->session_receive_storage,
                   admission->session_receive_stride, slot_index);
  attach_config.receive_capacity = admission->session_receive_stride;
  attach_config.send_storage =
      buffer_slice(admission->session_send_storage, admission->session_send_stride,
                   slot_index);
  attach_config.send_capacity = admission->session_send_stride;
  manager_result = omni_connection_manager_add(admission->manager, &attach_config);
  if (manager_result.status != OMNI_CONNECTION_MANAGER_OK) {
    omni_connection_destroy(&slot->connection);
    result.status = manager_result.status == OMNI_CONNECTION_MANAGER_ERR_FULL
                        ? OMNI_CONNECTION_ADMISSION_MANAGER_FULL
                        : OMNI_CONNECTION_ADMISSION_ERR_MANAGER;
    result.sys_errno = manager_result.sys_errno;
    result.manager_status = manager_result.status;
    return result;
  }

  slot->token = manager_result.token;
  slot->occupied = true;
  admission->count += 1u;
  admission->state = OMNI_CONNECTION_ADMISSION_ACTIVE;
  result.status = OMNI_CONNECTION_ADMISSION_OK;
  result.sys_errno = 0;
  result.admitted = 1u;
  result.identity.slot_index = slot_index;
  result.identity.token = manager_result.token;
  result.manager_status = OMNI_CONNECTION_MANAGER_OK;
  return result;
}

struct omni_connection_admission_result omni_connection_admission_once(
    struct omni_connection_admission *admission) {
  return admit_once_internal(admission);
}

struct omni_connection_admission_result omni_connection_admission_drain(
    struct omni_connection_admission *admission,
    size_t max_attempts,
    struct omni_connection_admission_identity *identities,
    size_t identity_capacity) {
  struct omni_connection_admission_result result =
      make_result(OMNI_CONNECTION_ADMISSION_ERR_INVALID, EINVAL);

  if (max_attempts == 0u || identities == NULL || identity_capacity < max_attempts ||
      max_attempts > SIZE_MAX / sizeof(*identities)) {
    return result;
  }
  result.status = OMNI_CONNECTION_ADMISSION_OK;
  result.sys_errno = 0;
  for (size_t i = 0u; i < max_attempts; ++i) {
    struct omni_connection_admission_result one = admit_once_internal(admission);
    result.attempts += one.attempts;
    if (one.status != OMNI_CONNECTION_ADMISSION_OK) {
      result.status = one.status;
      result.sys_errno = one.sys_errno;
      result.accept_status = one.accept_status;
      result.connection_status = one.connection_status;
      result.manager_status = one.manager_status;
      return result;
    }
    identities[result.admitted] = one.identity;
    result.admitted += 1u;
  }
  result.status = OMNI_CONNECTION_ADMISSION_LIMIT_REACHED;
  return result;
}

struct omni_connection_admission_result omni_connection_admission_release(
    struct omni_connection_admission *admission,
    struct omni_connection_admission_identity identity) {
  struct omni_connection_admission_result result;
  struct omni_connection_admission_slot *slot = NULL;
  struct omni_connection_manager_result manager_result;

  if (admission == NULL || !admission->live) {
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_STATE, EINVAL);
  }
  if (admission->state != OMNI_CONNECTION_ADMISSION_INITIALIZED &&
      admission->state != OMNI_CONNECTION_ADMISSION_ACTIVE &&
      admission->state != OMNI_CONNECTION_ADMISSION_STOPPING) {
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_STATE, EINVAL);
  }
  if (identity.slot_index >= admission->capacity || identity.token == 0u) {
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_NOT_FOUND, ENOENT);
  }
  slot = &admission->slots[identity.slot_index];
  if (!slot->occupied || slot->token != identity.token) {
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_NOT_FOUND, ENOENT);
  }

  manager_result = omni_connection_manager_remove(admission->manager, &slot->connection);
  if (manager_result.status != OMNI_CONNECTION_MANAGER_OK) {
    result = make_result(OMNI_CONNECTION_ADMISSION_ERR_MANAGER, manager_result.sys_errno);
    result.manager_status = manager_result.status;
    return result;
  }
  omni_connection_destroy(&slot->connection);
  slot->token = 0u;
  slot->occupied = false;
  if (admission->count > 0u) admission->count -= 1u;
  if (admission->state == OMNI_CONNECTION_ADMISSION_ACTIVE && admission->count == 0u) {
    admission->state = OMNI_CONNECTION_ADMISSION_INITIALIZED;
  }
  result = make_result(OMNI_CONNECTION_ADMISSION_OK, 0);
  result.manager_status = OMNI_CONNECTION_MANAGER_OK;
  return result;
}

struct omni_connection_admission_result omni_connection_admission_stop(
    struct omni_connection_admission *admission) {
  if (admission == NULL || !admission->live) {
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_STATE, EINVAL);
  }
  if (admission->state == OMNI_CONNECTION_ADMISSION_STOPPING) {
    return make_result(OMNI_CONNECTION_ADMISSION_OK, 0);
  }
  if (admission->state != OMNI_CONNECTION_ADMISSION_INITIALIZED &&
      admission->state != OMNI_CONNECTION_ADMISSION_ACTIVE) {
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_STATE, EINVAL);
  }
  admission->state = OMNI_CONNECTION_ADMISSION_STOPPING;
  return make_result(OMNI_CONNECTION_ADMISSION_OK, 0);
}

struct omni_connection_admission_result omni_connection_admission_destroy(
    struct omni_connection_admission *admission) {
  if (admission == NULL) {
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_INVALID, EINVAL);
  }
  if (admission->state == OMNI_CONNECTION_ADMISSION_CLOSED) {
    return make_result(OMNI_CONNECTION_ADMISSION_OK, 0);
  }
  if (!admission->live) {
    if (admission->state == OMNI_CONNECTION_ADMISSION_NEW) {
      admission->state = OMNI_CONNECTION_ADMISSION_CLOSED;
      return make_result(OMNI_CONNECTION_ADMISSION_OK, 0);
    }
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_STATE, EINVAL);
  }

  admission->state = OMNI_CONNECTION_ADMISSION_STOPPING;
  for (size_t i = 0u; i < admission->capacity; ++i) {
    struct omni_connection_admission_slot *slot = &admission->slots[i];
    struct omni_connection_admission_identity identity;
    struct omni_connection_admission_result release_result;
    if (!slot->occupied) continue;
    identity.slot_index = i;
    identity.token = slot->token;
    release_result = omni_connection_admission_release(admission, identity);
    if (release_result.status != OMNI_CONNECTION_ADMISSION_OK) return release_result;
  }
  if (admission->count != 0u) {
    return make_result(OMNI_CONNECTION_ADMISSION_ERR_STATE, EINVAL);
  }
  admission->listener = NULL;
  admission->manager = NULL;
  admission->slots = NULL;
  admission->capacity = 0u;
  admission->connection_receive_storage = NULL;
  admission->connection_receive_stride = 0u;
  admission->session_receive_storage = NULL;
  admission->session_receive_stride = 0u;
  admission->session_send_storage = NULL;
  admission->session_send_stride = 0u;
  admission->state = OMNI_CONNECTION_ADMISSION_CLOSED;
  admission->live = false;
  return make_result(OMNI_CONNECTION_ADMISSION_OK, 0);
}

enum omni_connection_admission_state omni_connection_admission_state(
    const struct omni_connection_admission *admission) {
  if (admission == NULL) return OMNI_CONNECTION_ADMISSION_NEW;
  return admission->state;
}

size_t omni_connection_admission_count(const struct omni_connection_admission *admission) {
  if (admission == NULL || !admission->live) return 0u;
  return admission->count;
}

size_t omni_connection_admission_capacity(const struct omni_connection_admission *admission) {
  if (admission == NULL || !admission->live) return 0u;
  return admission->capacity;
}
