/*
 * OmniRoute native backend — bounded connection manager layer (Task 028).
 *
 * No heap allocation, no descriptor close, no accepted destroy, no
 * connection free, no poller ownership, no event loop run, no
 * automatic read/write, no protocol handling.
 */

#include "omniroute/connection_manager.h"

#include <errno.h>

static struct omni_connection_manager_result make_result(
    enum omni_connection_manager_status status, int err, uint64_t token) {
  struct omni_connection_manager_result out;
  out.status = status;
  out.sys_errno = err;
  out.token = token;
  return out;
}

static void mark_inert_entries(struct omni_connection_manager *mgr) {
  if (mgr->entries != NULL && mgr->capacity > 0) {
    for (size_t i = 0; i < mgr->capacity; ++i) {
      struct omni_connection_manager_entry *e = &mgr->entries[i];
      e->connection = NULL;
      e->token = 0u;
      e->occupied = false;
    }
  }
}

void omni_connection_manager_make_inert(struct omni_connection_manager *mgr) {
  if (mgr == NULL) return;
  mgr->runtime = NULL;
  mgr->entries = NULL;
  mgr->capacity = 0u;
  mgr->count = 0u;
  mgr->state = OMNI_CONNECTION_MANAGER_NEW;
  mgr->live = false;
}

struct omni_connection_manager_result omni_connection_manager_init(
    struct omni_connection_manager *mgr,
    const struct omni_connection_manager_config *config) {
  if (mgr == NULL || config == NULL) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_INVALID, EINVAL, 0u);
  }
  if (mgr->live || mgr->state != OMNI_CONNECTION_MANAGER_NEW) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_STATE, EINVAL, 0u);
  }
  if (config->runtime == NULL) {
    omni_connection_manager_make_inert(mgr);
    return make_result(OMNI_CONNECTION_MANAGER_ERR_INVALID, EINVAL, 0u);
  }
  if (config->entries == NULL || config->capacity == 0u) {
    omni_connection_manager_make_inert(mgr);
    return make_result(OMNI_CONNECTION_MANAGER_ERR_INVALID, EINVAL, 0u);
  }
  if (!config->runtime->live) {
    omni_connection_manager_make_inert(mgr);
    return make_result(OMNI_CONNECTION_MANAGER_ERR_INVALID, EINVAL, 0u);
  }
  /* Runtime must be at least INITIALIZED (it starts NEW then INITIALIZED). */
  if (config->runtime->state == OMNI_CONNECTION_RUNTIME_NEW ||
      config->runtime->state == OMNI_CONNECTION_RUNTIME_CLOSED) {
    omni_connection_manager_make_inert(mgr);
    return make_result(OMNI_CONNECTION_MANAGER_ERR_INVALID, EINVAL, 0u);
  }
  mgr->runtime = config->runtime;
  mgr->entries = config->entries;
  mgr->capacity = config->capacity;
  mgr->count = 0u;
  mgr->state = OMNI_CONNECTION_MANAGER_INITIALIZED;
  mgr->live = true;
  mark_inert_entries(mgr);
  return make_result(OMNI_CONNECTION_MANAGER_OK, 0, 0u);
}

struct omni_connection_manager_result omni_connection_manager_start(
    struct omni_connection_manager *mgr) {
  if (mgr == NULL) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_INVALID, EINVAL, 0u);
  }
  if (!mgr->live) {
    if (mgr->state == OMNI_CONNECTION_MANAGER_NEW ||
        mgr->state == OMNI_CONNECTION_MANAGER_CLOSED) {
      return make_result(OMNI_CONNECTION_MANAGER_ERR_STATE, EINVAL, 0u);
    }
    return make_result(OMNI_CONNECTION_MANAGER_ERR_INVALID, EINVAL, 0u);
  }
  if (mgr->state == OMNI_CONNECTION_MANAGER_INITIALIZED) {
    mgr->state = OMNI_CONNECTION_MANAGER_RUNNING;
    return make_result(OMNI_CONNECTION_MANAGER_OK, 0, 0u);
  }
  if (mgr->state == OMNI_CONNECTION_MANAGER_RUNNING) {
    return make_result(OMNI_CONNECTION_MANAGER_OK, 0, 0u);
  }
  if (mgr->state == OMNI_CONNECTION_MANAGER_STOPPING) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_STATE, EINVAL, 0u);
  }
  if (mgr->state == OMNI_CONNECTION_MANAGER_CLOSED) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_STATE, EINVAL, 0u);
  }
  return make_result(OMNI_CONNECTION_MANAGER_ERR_STATE, EINVAL, 0u);
}

struct omni_connection_manager_result omni_connection_manager_stop(
    struct omni_connection_manager *mgr) {
  if (mgr == NULL) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_INVALID, EINVAL, 0u);
  }
  if (!mgr->live) {
    if (mgr->state == OMNI_CONNECTION_MANAGER_NEW ||
        mgr->state == OMNI_CONNECTION_MANAGER_CLOSED) {
      return make_result(OMNI_CONNECTION_MANAGER_ERR_STATE, EINVAL, 0u);
    }
    return make_result(OMNI_CONNECTION_MANAGER_ERR_INVALID, EINVAL, 0u);
  }
  if (mgr->state == OMNI_CONNECTION_MANAGER_INITIALIZED ||
      mgr->state == OMNI_CONNECTION_MANAGER_RUNNING) {
    mgr->state = OMNI_CONNECTION_MANAGER_STOPPING;
    return make_result(OMNI_CONNECTION_MANAGER_OK, 0, 0u);
  }
  if (mgr->state == OMNI_CONNECTION_MANAGER_STOPPING) {
    return make_result(OMNI_CONNECTION_MANAGER_OK, 0, 0u);
  }
  return make_result(OMNI_CONNECTION_MANAGER_ERR_STATE, EINVAL, 0u);
}

static struct omni_connection_manager_entry *find_entry(
    struct omni_connection_manager *mgr, struct omni_connection *conn) {
  if (mgr->entries == NULL) return NULL;
  for (size_t i = 0; i < mgr->capacity; ++i) {
    struct omni_connection_manager_entry *e = &mgr->entries[i];
    if (e->occupied && e->connection == conn) return e;
  }
  return NULL;
}

static struct omni_connection_manager_entry *find_free(
    struct omni_connection_manager *mgr) {
  if (mgr->entries == NULL) return NULL;
  for (size_t i = 0; i < mgr->capacity; ++i) {
    struct omni_connection_manager_entry *e = &mgr->entries[i];
    if (!e->occupied) return e;
  }
  return NULL;
}

struct omni_connection_manager_result omni_connection_manager_add(
    struct omni_connection_manager *mgr,
    const struct omni_connection_manager_attach_config *attach_config) {
  struct omni_connection_manager_entry *slot = NULL;
  struct omni_connection_runtime_attach_config rt_cfg = {0};
  struct omni_connection_runtime_result rt_res;

  if (mgr == NULL || attach_config == NULL) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_INVALID, EINVAL, 0u);
  }
  if (!mgr->live) {
    if (mgr->state == OMNI_CONNECTION_MANAGER_NEW ||
        mgr->state == OMNI_CONNECTION_MANAGER_CLOSED ||
        mgr->state == OMNI_CONNECTION_MANAGER_STOPPING) {
      return make_result(OMNI_CONNECTION_MANAGER_ERR_STATE, EINVAL, 0u);
    }
    return make_result(OMNI_CONNECTION_MANAGER_ERR_INVALID, EINVAL, 0u);
  }
  if (mgr->state == OMNI_CONNECTION_MANAGER_NEW ||
      mgr->state == OMNI_CONNECTION_MANAGER_CLOSED ||
      mgr->state == OMNI_CONNECTION_MANAGER_STOPPING) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_STATE, EINVAL, 0u);
  }
  if (mgr->state != OMNI_CONNECTION_MANAGER_INITIALIZED &&
      mgr->state != OMNI_CONNECTION_MANAGER_RUNNING) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_STATE, EINVAL, 0u);
  }
  if (attach_config->connection == NULL) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_INVALID, EINVAL, 0u);
  }
  if (omni_connection_state(attach_config->connection) != OMNI_CONNECTION_OPEN ||
      !omni_connection_is_live(attach_config->connection)) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_INVALID, EINVAL, 0u);
  }
  if (attach_config->receive_storage == NULL || attach_config->receive_capacity == 0u ||
      attach_config->send_storage == NULL || attach_config->send_capacity == 0u) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_INVALID, EINVAL, 0u);
  }
  if (find_entry(mgr, attach_config->connection) != NULL) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_DUPLICATE, EEXIST, 0u);
  }
  if (mgr->count >= mgr->capacity) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_FULL, ENOSPC, 0u);
  }
  slot = find_free(mgr);
  if (slot == NULL) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_FULL, ENOSPC, 0u);
  }

  /* Delegate to runtime. This will create session and register reactor. */
  rt_cfg.connection = attach_config->connection;
  rt_cfg.receive_storage = attach_config->receive_storage;
  rt_cfg.receive_capacity = attach_config->receive_capacity;
  rt_cfg.send_storage = attach_config->send_storage;
  rt_cfg.send_capacity = attach_config->send_capacity;

  rt_res = omni_connection_runtime_attach(mgr->runtime, &rt_cfg);
  if (rt_res.status != OMNI_CONNECTION_RUNTIME_OK) {
    if (rt_res.status == OMNI_CONNECTION_RUNTIME_ERR_DUPLICATE) {
      return make_result(OMNI_CONNECTION_MANAGER_ERR_DUPLICATE, rt_res.sys_errno, 0u);
    }
    if (rt_res.status == OMNI_CONNECTION_RUNTIME_ERR_FULL) {
      return make_result(OMNI_CONNECTION_MANAGER_ERR_FULL, rt_res.sys_errno, 0u);
    }
    if (rt_res.status == OMNI_CONNECTION_RUNTIME_ERR_INVALID) {
      return make_result(OMNI_CONNECTION_MANAGER_ERR_INVALID, rt_res.sys_errno, 0u);
    }
    if (rt_res.status == OMNI_CONNECTION_RUNTIME_ERR_STATE) {
      return make_result(OMNI_CONNECTION_MANAGER_ERR_STATE, rt_res.sys_errno, 0u);
    }
    return make_result(OMNI_CONNECTION_MANAGER_ERR_RUNTIME, rt_res.sys_errno, 0u);
  }

  /* Runtime succeeded — record membership. */
  slot->connection = attach_config->connection;
  slot->token = rt_res.token;
  slot->occupied = true;
  mgr->count += 1u;
  if (mgr->state == OMNI_CONNECTION_MANAGER_INITIALIZED) {
    mgr->state = OMNI_CONNECTION_MANAGER_RUNNING;
  }
  return make_result(OMNI_CONNECTION_MANAGER_OK, 0, rt_res.token);
}

struct omni_connection_manager_result omni_connection_manager_remove(
    struct omni_connection_manager *mgr,
    struct omni_connection *connection) {
  struct omni_connection_manager_entry *slot = NULL;
  struct omni_connection_runtime_result rt_res;

  if (mgr == NULL || connection == NULL) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_INVALID, EINVAL, 0u);
  }
  if (!mgr->live) {
    if (mgr->state == OMNI_CONNECTION_MANAGER_NEW ||
        mgr->state == OMNI_CONNECTION_MANAGER_CLOSED) {
      return make_result(OMNI_CONNECTION_MANAGER_ERR_STATE, EINVAL, 0u);
    }
    return make_result(OMNI_CONNECTION_MANAGER_ERR_INVALID, EINVAL, 0u);
  }
  if (mgr->state == OMNI_CONNECTION_MANAGER_NEW ||
      mgr->state == OMNI_CONNECTION_MANAGER_CLOSED) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_STATE, EINVAL, 0u);
  }
  if (mgr->state != OMNI_CONNECTION_MANAGER_INITIALIZED &&
      mgr->state != OMNI_CONNECTION_MANAGER_RUNNING &&
      mgr->state != OMNI_CONNECTION_MANAGER_STOPPING) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_STATE, EINVAL, 0u);
  }
  slot = find_entry(mgr, connection);
  if (slot == NULL) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_NOT_FOUND, ENOENT, 0u);
  }

  rt_res = omni_connection_runtime_detach(mgr->runtime, connection);
  /* Always clear manager bookkeeping, even if runtime reports NOT_FOUND. */
  slot->connection = NULL;
  slot->token = 0u;
  slot->occupied = false;
  if (mgr->count > 0) mgr->count -= 1u;
  if (mgr->count == 0u) {
    if (mgr->state == OMNI_CONNECTION_MANAGER_RUNNING) {
      mgr->state = OMNI_CONNECTION_MANAGER_INITIALIZED;
    } else if (mgr->state == OMNI_CONNECTION_MANAGER_STOPPING) {
      /* stay STOPPING — no add allowed, removals still allowed */
    }
  } else {
    /* with remaining entries, stay in current RUNNING/STOPPING/INITIALIZED */
    if (mgr->state == OMNI_CONNECTION_MANAGER_INITIALIZED && mgr->count > 0) {
      mgr->state = OMNI_CONNECTION_MANAGER_RUNNING;
    }
  }

  if (rt_res.status == OMNI_CONNECTION_RUNTIME_ERR_NOT_FOUND) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_NOT_FOUND, rt_res.sys_errno, 0u);
  }
  if (rt_res.status != OMNI_CONNECTION_RUNTIME_OK) {
    return make_result(OMNI_CONNECTION_MANAGER_ERR_RUNTIME, rt_res.sys_errno, 0u);
  }
  return make_result(OMNI_CONNECTION_MANAGER_OK, 0, 0u);
}

void omni_connection_manager_destroy(struct omni_connection_manager *mgr) {
  if (mgr == NULL) return;
  if (!mgr->live) {
    if (mgr->state == OMNI_CONNECTION_MANAGER_CLOSED) return;
    if (mgr->state == OMNI_CONNECTION_MANAGER_NEW) {
      mgr->state = OMNI_CONNECTION_MANAGER_CLOSED;
      return;
    }
    /* For STOPPING but !live shouldn't happen; handle idempotent. */
    if (mgr->state == OMNI_CONNECTION_MANAGER_STOPPING) {
      mgr->state = OMNI_CONNECTION_MANAGER_CLOSED;
      return;
    }
    return;
  }
  if (mgr->entries != NULL) {
    for (size_t i = 0; i < mgr->capacity; ++i) {
      struct omni_connection_manager_entry *e = &mgr->entries[i];
      if (e->occupied && e->connection != NULL) {
        (void)omni_connection_runtime_detach(mgr->runtime, e->connection);
        e->connection = NULL;
        e->token = 0u;
        e->occupied = false;
      } else if (e->occupied) {
        e->occupied = false;
        e->token = 0u;
        e->connection = NULL;
      } else {
        e->connection = NULL;
        e->token = 0u;
      }
    }
  }
  mgr->count = 0u;
  mgr->runtime = NULL;
  mgr->entries = NULL;
  mgr->capacity = 0u;
  mgr->state = OMNI_CONNECTION_MANAGER_CLOSED;
  mgr->live = false;
}

enum omni_connection_manager_state omni_connection_manager_state(
    const struct omni_connection_manager *mgr) {
  if (mgr == NULL) return OMNI_CONNECTION_MANAGER_NEW;
  return mgr->state;
}

size_t omni_connection_manager_count(const struct omni_connection_manager *mgr) {
  if (mgr == NULL || !mgr->live) return 0u;
  return mgr->count;
}

size_t omni_connection_manager_capacity(const struct omni_connection_manager *mgr) {
  if (mgr == NULL || !mgr->live) return 0u;
  return mgr->capacity;
}

const struct omni_connection_manager_entry *omni_connection_manager_find(
    const struct omni_connection_manager *mgr,
    struct omni_connection *connection) {
  if (mgr == NULL || connection == NULL || !mgr->live || mgr->entries == NULL) return NULL;
  for (size_t i = 0u; i < mgr->capacity; ++i) {
    const struct omni_connection_manager_entry *entry = &mgr->entries[i];
    if (entry->occupied && entry->connection == connection) return entry;
  }
  return NULL;
}
