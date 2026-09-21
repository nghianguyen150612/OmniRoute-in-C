/*
 * OmniRoute native backend — bounded connection runtime binding layer (Task 027).
 *
 * No heap allocation, no descriptor close, no accepted destroy, no connection
 * free, no poller ownership, no event loop run, no automatic read/write.
 */

#include "omniroute/connection_runtime.h"

#include <errno.h>
#include <string.h>

static struct omni_connection_runtime_result make_result(
    enum omni_connection_runtime_status status, int err, uint64_t token) {
  struct omni_connection_runtime_result out;
  out.status = status;
  out.sys_errno = err;
  out.token = token;
  return out;
}

static void mark_inert_entries(struct omni_connection_runtime *rt) {
  if (rt->entries != NULL && rt->capacity > 0) {
    for (size_t i = 0; i < rt->capacity; ++i) {
      struct omni_connection_runtime_entry *e = &rt->entries[i];
      e->connection = NULL;
      omni_connection_session_make_inert(&e->session);
      e->token = 0u;
      e->occupied = false;
    }
  }
}

void omni_connection_runtime_make_inert(struct omni_connection_runtime *rt) {
  if (rt == NULL) return;
  rt->registry = NULL;
  rt->adapter = NULL;
  rt->event_loop = NULL;
  rt->entries = NULL;
  rt->capacity = 0u;
  rt->count = 0u;
  rt->state = OMNI_CONNECTION_RUNTIME_NEW;
  rt->live = false;
}

struct omni_connection_runtime_result omni_connection_runtime_init(
    struct omni_connection_runtime *rt,
    const struct omni_connection_runtime_config *config) {
  if (rt == NULL || config == NULL) {
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_INVALID, EINVAL, 0u);
  }
  if (rt->live || rt->state != OMNI_CONNECTION_RUNTIME_NEW) {
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_STATE, EINVAL, 0u);
  }
  if (config->registry == NULL || config->adapter == NULL) {
    omni_connection_runtime_make_inert(rt);
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_INVALID, EINVAL, 0u);
  }
  if (config->entries == NULL || config->capacity == 0u) {
    omni_connection_runtime_make_inert(rt);
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_INVALID, EINVAL, 0u);
  }
  /* Basic liveness checks: registry and adapter must be live.  Event loop may be NULL. */
  if (!config->registry->live) {
    omni_connection_runtime_make_inert(rt);
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_INVALID, EINVAL, 0u);
  }
  if (!config->adapter->live) {
    omni_connection_runtime_make_inert(rt);
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_INVALID, EINVAL, 0u);
  }
  /* Capacity must fit in registry handling (adapter already checks). */

  rt->registry = config->registry;
  rt->adapter = config->adapter;
  rt->event_loop = config->event_loop;
  rt->entries = config->entries;
  rt->capacity = config->capacity;
  rt->count = 0u;
  rt->state = OMNI_CONNECTION_RUNTIME_INITIALIZED;
  rt->live = true;
  mark_inert_entries(rt);
  return make_result(OMNI_CONNECTION_RUNTIME_OK, 0, 0u);
}

static struct omni_connection_runtime_entry *find_entry(
    struct omni_connection_runtime *rt, struct omni_connection *conn) {
  if (rt->entries == NULL) return NULL;
  for (size_t i = 0; i < rt->capacity; ++i) {
    struct omni_connection_runtime_entry *e = &rt->entries[i];
    if (e->occupied && e->connection == conn) return e;
  }
  return NULL;
}

static struct omni_connection_runtime_entry *find_free(
    struct omni_connection_runtime *rt) {
  if (rt->entries == NULL) return NULL;
  for (size_t i = 0; i < rt->capacity; ++i) {
    struct omni_connection_runtime_entry *e = &rt->entries[i];
    if (!e->occupied) return e;
  }
  return NULL;
}

struct omni_connection_runtime_result omni_connection_runtime_attach(
    struct omni_connection_runtime *rt,
    const struct omni_connection_runtime_attach_config *attach_config) {
  struct omni_connection_runtime_entry *slot = NULL;
  struct omni_connection_session_config sess_cfg = {0};
  struct omni_connection_session_result sess_res;
  struct omni_connection_reactor_result react_res;

  if (rt == NULL || attach_config == NULL) {
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_INVALID, EINVAL, 0u);
  }
  if (!rt->live ||
      (rt->state != OMNI_CONNECTION_RUNTIME_INITIALIZED &&
       rt->state != OMNI_CONNECTION_RUNTIME_ATTACHED)) {
    if (rt->state == OMNI_CONNECTION_RUNTIME_CLOSED ||
        rt->state == OMNI_CONNECTION_RUNTIME_NEW) {
      return make_result(OMNI_CONNECTION_RUNTIME_ERR_STATE, EINVAL, 0u);
    }
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_INVALID, EINVAL, 0u);
  }
  if (attach_config->connection == NULL) {
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_INVALID, EINVAL, 0u);
  }
  if (omni_connection_state(attach_config->connection) != OMNI_CONNECTION_OPEN ||
      !omni_connection_is_live(attach_config->connection)) {
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_INVALID, EINVAL, 0u);
  }
  if (attach_config->receive_storage == NULL || attach_config->receive_capacity == 0u ||
      attach_config->send_storage == NULL || attach_config->send_capacity == 0u) {
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_INVALID, EINVAL, 0u);
  }
  if (rt->count >= rt->capacity) {
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_FULL, ENOSPC, 0u);
  }
  if (find_entry(rt, attach_config->connection) != NULL) {
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_DUPLICATE, EEXIST, 0u);
  }
  slot = find_free(rt);
  if (slot == NULL) {
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_FULL, ENOSPC, 0u);
  }

  /* Prepare session in the free slot. */
  omni_connection_session_make_inert(&slot->session);
  sess_cfg.connection = attach_config->connection;
  sess_cfg.receive_storage = attach_config->receive_storage;
  sess_cfg.receive_capacity = attach_config->receive_capacity;
  sess_cfg.send_storage = attach_config->send_storage;
  sess_cfg.send_capacity = attach_config->send_capacity;

  sess_res = omni_connection_session_init(&slot->session, &sess_cfg);
  if (sess_res.status != OMNI_CONNECTION_SESSION_OK) {
    omni_connection_session_make_inert(&slot->session);
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_SESSION, sess_res.sys_errno, 0u);
  }
  sess_res = omni_connection_session_open(&slot->session);
  if (sess_res.status != OMNI_CONNECTION_SESSION_OK) {
    omni_connection_session_destroy(&slot->session);
    omni_connection_session_make_inert(&slot->session);
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_SESSION, sess_res.sys_errno, 0u);
  }

  /* Session is now OPEN; register via adapter. */
  react_res = omni_connection_reactor_attach(rt->adapter, attach_config->connection);
  if (react_res.status != OMNI_CONNECTION_REACTOR_OK) {
    /* Roll back session. */
    omni_connection_session_destroy(&slot->session);
    omni_connection_session_make_inert(&slot->session);
    if (react_res.status == OMNI_CONNECTION_REACTOR_ERR_DUPLICATE) {
      return make_result(OMNI_CONNECTION_RUNTIME_ERR_DUPLICATE, react_res.sys_errno, 0u);
    }
    if (react_res.status == OMNI_CONNECTION_REACTOR_ERR_FULL) {
      return make_result(OMNI_CONNECTION_RUNTIME_ERR_FULL, react_res.sys_errno, 0u);
    }
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_REACTOR, react_res.sys_errno, 0u);
  }

  slot->connection = attach_config->connection;
  slot->token = react_res.token;
  slot->occupied = true;
  rt->count += 1u;
  rt->state = OMNI_CONNECTION_RUNTIME_ATTACHED;
  return make_result(OMNI_CONNECTION_RUNTIME_OK, 0, react_res.token);
}

struct omni_connection_runtime_result omni_connection_runtime_detach(
    struct omni_connection_runtime *rt,
    struct omni_connection *connection) {
  struct omni_connection_runtime_entry *slot = NULL;
  struct omni_connection_reactor_result react_res;

  if (rt == NULL || connection == NULL) {
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_INVALID, EINVAL, 0u);
  }
  if (!rt->live ||
      (rt->state != OMNI_CONNECTION_RUNTIME_INITIALIZED &&
       rt->state != OMNI_CONNECTION_RUNTIME_ATTACHED &&
       rt->state != OMNI_CONNECTION_RUNTIME_DETACHING)) {
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_STATE, EINVAL, 0u);
  }
  slot = find_entry(rt, connection);
  if (slot == NULL) {
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_NOT_FOUND, ENOENT, 0u);
  }

  /* Unregister via adapter; always destroy session and free slot even if reactor reports NOT_FOUND. */
  react_res = omni_connection_reactor_detach(rt->adapter, connection);
  /* Destroy session owned bookkeeping. */
  omni_connection_session_destroy(&slot->session);
  omni_connection_session_make_inert(&slot->session);
  slot->connection = NULL;
  slot->token = 0u;
  slot->occupied = false;
  if (rt->count > 0) rt->count -= 1u;
  if (rt->count == 0u) {
    rt->state = OMNI_CONNECTION_RUNTIME_INITIALIZED;
  } else {
    rt->state = OMNI_CONNECTION_RUNTIME_ATTACHED;
  }
  /* If reactor detach failed with NOT_FOUND, propagate that, but slot is already freed. */
  if (react_res.status == OMNI_CONNECTION_REACTOR_ERR_NOT_FOUND) {
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_NOT_FOUND, react_res.sys_errno, 0u);
  }
  if (react_res.status != OMNI_CONNECTION_REACTOR_OK) {
    return make_result(OMNI_CONNECTION_RUNTIME_ERR_REACTOR, react_res.sys_errno, 0u);
  }
  return make_result(OMNI_CONNECTION_RUNTIME_OK, 0, 0u);
}

void omni_connection_runtime_destroy(struct omni_connection_runtime *rt) {
  if (rt == NULL || !rt->live) {
    if (rt != NULL && rt->state == OMNI_CONNECTION_RUNTIME_CLOSED) return;
    if (rt != NULL && rt->state == OMNI_CONNECTION_RUNTIME_NEW) {
      rt->state = OMNI_CONNECTION_RUNTIME_CLOSED;
      return;
    }
    return;
  }
  /* Detach all occupied entries synchronously. */
  if (rt->entries != NULL) {
    for (size_t i = 0; i < rt->capacity; ++i) {
      struct omni_connection_runtime_entry *e = &rt->entries[i];
      if (e->occupied && e->connection != NULL) {
        /* Best-effort reactor detach; ignore result, we are tearing down. */
        (void)omni_connection_reactor_detach(rt->adapter, e->connection);
        omni_connection_session_destroy(&e->session);
        omni_connection_session_make_inert(&e->session);
        e->connection = NULL;
        e->token = 0u;
        e->occupied = false;
      } else if (e->occupied) {
        omni_connection_session_destroy(&e->session);
        omni_connection_session_make_inert(&e->session);
        e->occupied = false;
        e->token = 0u;
      } else {
        /* Ensure session is inert. */
        omni_connection_session_make_inert(&e->session);
      }
    }
  }
  rt->count = 0u;
  rt->registry = NULL;
  rt->adapter = NULL;
  rt->event_loop = NULL;
  rt->entries = NULL;
  rt->capacity = 0u;
  rt->state = OMNI_CONNECTION_RUNTIME_CLOSED;
  rt->live = false;
}

enum omni_connection_runtime_state omni_connection_runtime_state(
    const struct omni_connection_runtime *rt) {
  if (rt == NULL) return OMNI_CONNECTION_RUNTIME_NEW;
  return rt->state;
}

size_t omni_connection_runtime_count(const struct omni_connection_runtime *rt) {
  if (rt == NULL || !rt->live) return 0u;
  return rt->count;
}

size_t omni_connection_runtime_capacity(const struct omni_connection_runtime *rt) {
  if (rt == NULL || !rt->live) return 0u;
  return rt->capacity;
}

bool omni_connection_runtime_is_initialized(const struct omni_connection_runtime *rt) {
  return rt != NULL && rt->live &&
         (rt->state == OMNI_CONNECTION_RUNTIME_INITIALIZED ||
          rt->state == OMNI_CONNECTION_RUNTIME_ATTACHED);
}

struct omni_connection_session *omni_connection_runtime_find_session(
    struct omni_connection_runtime *rt,
    struct omni_connection *connection) {
  struct omni_connection_runtime_entry *e = NULL;
  if (rt == NULL || connection == NULL || !rt->live) return NULL;
  e = find_entry(rt, connection);
  if (e == NULL) return NULL;
  return &e->session;
}

int omni_connection_runtime_fd(struct omni_connection_runtime *rt,
                               struct omni_connection *connection) {
  struct omni_connection_session *sess = NULL;
  sess = omni_connection_runtime_find_session(rt, connection);
  if (sess == NULL) return OMNI_ACCEPTED_FD_INVALID;
  return omni_connection_session_fd(sess);
}
