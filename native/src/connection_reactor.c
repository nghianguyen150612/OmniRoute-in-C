/*
 * OmniRoute native backend — connection/reactor lifecycle adapter.
 *
 * The adapter composes two existing bounded ownership layers. The registry
 * owns membership metadata and the reactor owns the readiness registration;
 * the connection remains the sole owner of its descriptor and buffers. No
 * syscall, payload operation, allocator, queue, or lifecycle side effect is
 * introduced here.
 */

#include "omniroute/connection_reactor.h"

#include <errno.h>
#include <stdint.h>

static void mark_inert(struct omni_connection_reactor *adapter) {
  if (adapter == NULL) {
    return;
  }
  adapter->reactor = NULL;
  adapter->registry = NULL;
  adapter->callback = NULL;
  adapter->context = NULL;
  adapter->live = false;
}

static struct omni_connection_reactor_result make_result(
    enum omni_connection_reactor_status status, int sys_errno, uint64_t token,
    size_t count) {
  struct omni_connection_reactor_result result;

  result.status = status;
  result.sys_errno = sys_errno;
  result.token = token;
  result.count = count;
  return result;
}

static uint64_t token_from_handle(struct omni_connection_registry_handle handle) {
  return ((uint64_t)handle.generation << 32) | (uint64_t)handle.index;
}

static bool handle_from_token(uint64_t token,
                              struct omni_connection_registry_handle *handle) {
  uint32_t index = (uint32_t)(token & UINT64_C(0xFFFFFFFF));
  uint32_t generation = (uint32_t)(token >> 32);

  if (handle == NULL || generation == 0u || index == UINT32_MAX) {
    return false;
  }
  handle->index = index;
  handle->generation = generation;
  return omni_connection_registry_handle_is_valid(*handle);
}

static bool find_connection_handle(
    const struct omni_connection_registry *registry,
    const struct omni_connection *connection,
    struct omni_connection_registry_handle *out_handle) {
  size_t cursor = 0u;
  struct omni_connection_registry_handle handle;
  struct omni_connection *member = NULL;

  if (registry == NULL || connection == NULL || out_handle == NULL) {
    return false;
  }
  while (omni_connection_registry_next(registry, &cursor, &handle, &member)) {
    if (member == connection) {
      *out_handle = handle;
      return true;
    }
  }
  return false;
}

static enum omni_connection_reactor_status map_registry_status(
    enum omni_connection_registry_status status) {
  switch (status) {
    case OMNI_CONNECTION_REGISTRY_ERR_DUPLICATE:
      return OMNI_CONNECTION_REACTOR_ERR_DUPLICATE;
    case OMNI_CONNECTION_REGISTRY_ERR_FULL:
      return OMNI_CONNECTION_REACTOR_ERR_FULL;
    case OMNI_CONNECTION_REGISTRY_ERR_NOT_FOUND:
      return OMNI_CONNECTION_REACTOR_ERR_NOT_FOUND;
    case OMNI_CONNECTION_REGISTRY_ERR_INVALID:
      return OMNI_CONNECTION_REACTOR_ERR_REGISTRY;
    case OMNI_CONNECTION_REGISTRY_OK:
      return OMNI_CONNECTION_REACTOR_OK;
  }
  return OMNI_CONNECTION_REACTOR_ERR_REGISTRY;
}

static void connection_reactor_bridge(uint64_t token, uint32_t events, void *context) {
  struct omni_connection_reactor *adapter = (struct omni_connection_reactor *)context;

  (void)omni_connection_reactor_dispatch(adapter, token, events);
}

void omni_connection_reactor_make_inert(struct omni_connection_reactor *adapter) {
  mark_inert(adapter);
}

struct omni_connection_reactor_result omni_connection_reactor_init(
    struct omni_connection_reactor *adapter,
    struct omni_reactor *reactor,
    struct omni_connection_registry *registry,
    omni_connection_reactor_callback callback,
    void *context) {
  if (adapter == NULL) {
    return make_result(OMNI_CONNECTION_REACTOR_ERR_INVALID, EINVAL, 0u, 0u);
  }
  if (adapter->live) {
    return make_result(OMNI_CONNECTION_REACTOR_ERR_STATE, EINVAL, 0u, 0u);
  }
  if (reactor == NULL || !reactor->live || registry == NULL || !registry->live ||
      callback == NULL) {
    mark_inert(adapter);
    return make_result(OMNI_CONNECTION_REACTOR_ERR_INVALID, EINVAL, 0u, 0u);
  }
  adapter->reactor = reactor;
  adapter->registry = registry;
  adapter->callback = callback;
  adapter->context = context;
  adapter->live = true;
  return make_result(OMNI_CONNECTION_REACTOR_OK, 0, 0u, 0u);
}

void omni_connection_reactor_destroy(struct omni_connection_reactor *adapter) {
  if (adapter == NULL || !adapter->live || adapter->registry == NULL) {
    return;
  }

  while (adapter->registry->live && omni_connection_registry_count(adapter->registry) > 0u) {
    size_t cursor = 0u;
    struct omni_connection_registry_handle handle;
    struct omni_connection *connection = NULL;
    uint64_t token = 0u;

    if (!omni_connection_registry_next(adapter->registry, &cursor, &handle, &connection)) {
      break;
    }
    (void)connection;
    token = token_from_handle(handle);
    if (adapter->reactor != NULL && adapter->reactor->live) {
      (void)omni_reactor_remove(adapter->reactor, token);
    }
    (void)omni_connection_registry_remove(adapter->registry, handle);
  }
  mark_inert(adapter);
}

struct omni_connection_reactor_result omni_connection_reactor_attach(
    struct omni_connection_reactor *adapter, struct omni_connection *connection) {
  struct omni_connection_registry_result registry_result;
  struct omni_reactor_result reactor_result;
  uint64_t token = 0u;

  if (adapter == NULL || !adapter->live || adapter->reactor == NULL ||
      adapter->registry == NULL || !adapter->reactor->live || !adapter->registry->live ||
      connection == NULL) {
    return make_result(OMNI_CONNECTION_REACTOR_ERR_INVALID, EINVAL, 0u, 0u);
  }
  if (omni_connection_state(connection) != OMNI_CONNECTION_OPEN ||
      !omni_connection_is_live(connection)) {
    return make_result(OMNI_CONNECTION_REACTOR_ERR_STATE, EINVAL, 0u, 0u);
  }

  registry_result = omni_connection_registry_add(adapter->registry, connection);
  if (registry_result.status != OMNI_CONNECTION_REGISTRY_OK) {
    return make_result(map_registry_status(registry_result.status),
                       registry_result.sys_errno, 0u, 0u);
  }
  token = token_from_handle(registry_result.handle);
  reactor_result = omni_reactor_add(adapter->reactor, omni_connection_fd(connection), token,
                                    omni_connection_poller_interests(connection),
                                    connection_reactor_bridge, adapter);
  if (reactor_result.status != OMNI_REACTOR_OK) {
    (void)omni_connection_registry_remove(adapter->registry, registry_result.handle);
    return make_result(OMNI_CONNECTION_REACTOR_ERR_REACTOR, reactor_result.sys_errno, 0u, 0u);
  }
  return make_result(OMNI_CONNECTION_REACTOR_OK, 0, token, 0u);
}

struct omni_connection_reactor_result omni_connection_reactor_detach(
    struct omni_connection_reactor *adapter, struct omni_connection *connection) {
  struct omni_connection_registry_handle handle;
  struct omni_connection_registry_result registry_result;
  struct omni_reactor_result reactor_result;
  uint64_t token = 0u;

  if (adapter == NULL || !adapter->live || adapter->reactor == NULL ||
      adapter->registry == NULL || !adapter->registry->live || connection == NULL) {
    return make_result(OMNI_CONNECTION_REACTOR_ERR_INVALID, EINVAL, 0u, 0u);
  }
  if (!find_connection_handle(adapter->registry, connection, &handle)) {
    return make_result(OMNI_CONNECTION_REACTOR_ERR_NOT_FOUND, ENOENT, 0u, 0u);
  }
  token = token_from_handle(handle);

  if (adapter->reactor->live) {
    reactor_result = omni_reactor_remove(adapter->reactor, token);
    if (reactor_result.status != OMNI_REACTOR_OK &&
        reactor_result.status != OMNI_REACTOR_ERR_NOT_FOUND) {
      /* Retire the registry identity even when a broken reactor cannot
       * remove its borrowed descriptor. Any captured event is now stale. */
      (void)omni_connection_registry_remove(adapter->registry, handle);
      return make_result(OMNI_CONNECTION_REACTOR_ERR_REACTOR, reactor_result.sys_errno,
                         token, 0u);
    }
  }

  registry_result = omni_connection_registry_remove(adapter->registry, handle);
  if (registry_result.status != OMNI_CONNECTION_REGISTRY_OK) {
    return make_result(OMNI_CONNECTION_REACTOR_ERR_REGISTRY, registry_result.sys_errno,
                       token, 0u);
  }
  return make_result(OMNI_CONNECTION_REACTOR_OK, 0, token, 0u);
}

struct omni_connection_reactor_result omni_connection_reactor_update_interests(
    struct omni_connection_reactor *adapter, uint64_t token, uint32_t interests) {
  struct omni_connection_registry_handle handle;
  struct omni_connection *connection = NULL;
  struct omni_reactor_result reactor_result;

  if (adapter == NULL || !adapter->live || adapter->reactor == NULL ||
      adapter->registry == NULL || !adapter->reactor->live ||
      !adapter->registry->live || token == 0u) {
    return make_result(OMNI_CONNECTION_REACTOR_ERR_INVALID, EINVAL, token, 0u);
  }
  if (!handle_from_token(token, &handle)) {
    return make_result(OMNI_CONNECTION_REACTOR_ERR_NOT_FOUND, ENOENT, token, 0u);
  }
  connection = omni_connection_registry_find(adapter->registry, handle);
  if (connection == NULL) {
    return make_result(OMNI_CONNECTION_REACTOR_ERR_NOT_FOUND, ENOENT, token, 0u);
  }
  if (omni_connection_state(connection) != OMNI_CONNECTION_OPEN ||
      !omni_connection_is_live(connection)) {
    return make_result(OMNI_CONNECTION_REACTOR_ERR_STATE, EINVAL, token, 0u);
  }

  reactor_result = omni_reactor_update_interests(adapter->reactor, token, interests);
  if (reactor_result.status != OMNI_REACTOR_OK) {
    return make_result(OMNI_CONNECTION_REACTOR_ERR_REACTOR,
                       reactor_result.sys_errno, token, 0u);
  }
  connection->poller_interests = interests;
  return make_result(OMNI_CONNECTION_REACTOR_OK, 0, token, 0u);
}

struct omni_connection_reactor_result omni_connection_reactor_dispatch(
    struct omni_connection_reactor *adapter, uint64_t token, uint32_t events) {
  struct omni_connection_registry_handle handle;
  struct omni_connection *connection = NULL;
  omni_connection_reactor_callback callback = NULL;
  void *context = NULL;

  if (adapter == NULL || !adapter->live || adapter->reactor == NULL ||
      adapter->registry == NULL || !adapter->registry->live) {
    return make_result(OMNI_CONNECTION_REACTOR_ERR_INVALID, EINVAL, 0u, 0u);
  }
  if (!adapter->reactor->live || !handle_from_token(token, &handle)) {
    return make_result(OMNI_CONNECTION_REACTOR_IGNORED, 0, token, 0u);
  }
  connection = omni_connection_registry_find(adapter->registry, handle);
  if (connection == NULL || omni_connection_state(connection) != OMNI_CONNECTION_OPEN ||
      !omni_connection_is_live(connection)) {
    return make_result(OMNI_CONNECTION_REACTOR_IGNORED, 0, token, 0u);
  }

  callback = adapter->callback;
  context = adapter->context;
  if (callback == NULL) {
    return make_result(OMNI_CONNECTION_REACTOR_IGNORED, 0, token, 0u);
  }
  callback(connection, token, events, context);
  return make_result(OMNI_CONNECTION_REACTOR_OK, 0, token, 1u);
}

size_t omni_connection_reactor_count(const struct omni_connection_reactor *adapter) {
  if (adapter == NULL || !adapter->live || adapter->registry == NULL) {
    return 0u;
  }
  return omni_connection_registry_count(adapter->registry);
}

size_t omni_connection_reactor_capacity(const struct omni_connection_reactor *adapter) {
  if (adapter == NULL || !adapter->live || adapter->registry == NULL) {
    return 0u;
  }
  return omni_connection_registry_capacity(adapter->registry);
}
