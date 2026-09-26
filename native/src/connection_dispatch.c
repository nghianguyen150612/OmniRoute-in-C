/* Task 031 bounded connection-readiness dispatch bridge. */

#include "omniroute/connection_dispatch.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static struct omni_connection_dispatch_result make_result(
    enum omni_connection_dispatch_status status, uint64_t token, uint32_t events) {
  struct omni_connection_dispatch_result result;

  (void)memset(&result, 0, sizeof(result));
  result.status = status;
  result.token = token;
  result.readiness_events = events;
  result.read_result.status = OMNI_CONNECTION_IO_ERR_STATE;
  result.write_result.status = OMNI_CONNECTION_IO_ERR_STATE;
  return result;
}

static uint64_t saturating_add(uint64_t value, uint64_t increment) {
  if (UINT64_MAX - value < increment) return UINT64_MAX;
  return value + increment;
}

static bool manager_usable(const struct omni_connection_manager *manager) {
  enum omni_connection_manager_state state = OMNI_CONNECTION_MANAGER_NEW;

  if (manager == NULL || !manager->live || manager->runtime == NULL ||
      manager->entries == NULL || manager->capacity == 0u ||
      !omni_connection_runtime_is_initialized(manager->runtime)) {
    return false;
  }
  state = omni_connection_manager_state(manager);
  return state == OMNI_CONNECTION_MANAGER_INITIALIZED ||
         state == OMNI_CONNECTION_MANAGER_RUNNING ||
         state == OMNI_CONNECTION_MANAGER_STOPPING;
}

static bool io_result_is_close_worthy(struct omni_connection_io_result result) {
  return result.status == OMNI_CONNECTION_IO_ERR_EOF ||
         result.status == OMNI_CONNECTION_IO_ERR_IO ||
         result.status == OMNI_CONNECTION_IO_ERR_CLOSED;
}

static struct omni_connection_dispatch_result result_with_events(uint64_t token,
                                                                  uint32_t events) {
  struct omni_connection_dispatch_result result =
      make_result(OMNI_CONNECTION_DISPATCH_IGNORED, token, events);

  result.error_observed = (events & OMNI_POLLER_READY_ERROR) != 0u;
  result.hangup_observed = (events & OMNI_POLLER_READY_HANGUP) != 0u;
  result.invalid_observed = (events & OMNI_POLLER_READY_INVALID) != 0u;
  result.close_worthy = result.error_observed || result.hangup_observed ||
                        result.invalid_observed;
  return result;
}

void omni_connection_dispatch_make_inert(struct omni_connection_dispatch *bridge) {
  if (bridge == NULL) return;
  (void)memset(bridge, 0, sizeof(*bridge));
  bridge->state = OMNI_CONNECTION_DISPATCH_INERT;
}

struct omni_connection_dispatch_result omni_connection_dispatch_init(
    struct omni_connection_dispatch *bridge,
    const struct omni_connection_dispatch_config *config) {
  if (bridge == NULL || config == NULL || config->manager == NULL) {
    return make_result(OMNI_CONNECTION_DISPATCH_ERR_INVALID, 0u, 0u);
  }
  if (bridge->state != OMNI_CONNECTION_DISPATCH_INERT) {
    return make_result(OMNI_CONNECTION_DISPATCH_ERR_STATE, 0u, 0u);
  }
  if (!manager_usable(config->manager)) {
    return make_result(OMNI_CONNECTION_DISPATCH_ERR_INVALID, 0u, 0u);
  }
  bridge->manager = config->manager;
  bridge->state = OMNI_CONNECTION_DISPATCH_ACTIVE;
  bridge->last_result = make_result(OMNI_CONNECTION_DISPATCH_OK, 0u, 0u);
  return bridge->last_result;
}

void omni_connection_dispatch_destroy(struct omni_connection_dispatch *bridge) {
  if (bridge == NULL) return;
  bridge->manager = NULL;
  bridge->state = OMNI_CONNECTION_DISPATCH_CLOSED;
}

void omni_connection_dispatch_callback(struct omni_connection *connection,
                                       uint64_t token,
                                       uint32_t events,
                                       void *context) {
  struct omni_connection_dispatch *bridge = (struct omni_connection_dispatch *)context;
  struct omni_connection_dispatch_result result;
  const struct omni_connection_manager_entry *entry = NULL;
  struct omni_connection_session *session = NULL;
  uint32_t io_events = OMNI_POLLER_READY_READ | OMNI_POLLER_READY_WRITE;

  if (bridge == NULL || bridge->state != OMNI_CONNECTION_DISPATCH_ACTIVE) return;
  bridge->dispatches = saturating_add(bridge->dispatches, 1u);
  result = result_with_events(token, events);
  if (result.error_observed || result.hangup_observed || result.invalid_observed) {
    bridge->error_observations = saturating_add(bridge->error_observations, 1u);
  }
  if (connection == NULL) {
    result.status = OMNI_CONNECTION_DISPATCH_ERR_INVALID;
    bridge->last_result = result;
    return;
  }

  /* Manager membership and its generation token are authoritative. */
  if (!manager_usable(bridge->manager)) {
    result.status = OMNI_CONNECTION_DISPATCH_STALE;
    result.stale = true;
    result.ignored = true;
    bridge->stale_dispatches = saturating_add(bridge->stale_dispatches, 1u);
    bridge->ignored_dispatches = saturating_add(bridge->ignored_dispatches, 1u);
    bridge->last_result = result;
    return;
  }
  entry = omni_connection_manager_find(bridge->manager, connection);
  if (entry == NULL || entry->token != token || entry->token == 0u ||
      omni_connection_state(connection) != OMNI_CONNECTION_OPEN ||
      !omni_connection_is_live(connection)) {
    result.status = OMNI_CONNECTION_DISPATCH_STALE;
    result.stale = true;
    result.ignored = true;
    bridge->stale_dispatches = saturating_add(bridge->stale_dispatches, 1u);
    bridge->ignored_dispatches = saturating_add(bridge->ignored_dispatches, 1u);
    bridge->last_result = result;
    return;
  }
  session = omni_connection_runtime_find_session(bridge->manager->runtime, connection);
  if (session == NULL || omni_connection_session_connection(session) != connection ||
      !omni_connection_session_is_open(session)) {
    result.status = OMNI_CONNECTION_DISPATCH_STALE;
    result.stale = true;
    result.ignored = true;
    bridge->stale_dispatches = saturating_add(bridge->stale_dispatches, 1u);
    bridge->ignored_dispatches = saturating_add(bridge->ignored_dispatches, 1u);
    bridge->last_result = result;
    return;
  }

  /* POLLNVAL means the descriptor cannot be used safely, even if READ/WRITE
   * bits are also present in the same poll record. */
  if (result.invalid_observed) {
    result.status = OMNI_CONNECTION_DISPATCH_INVALID_EVENT;
    bridge->last_result = result;
    return;
  }

  if ((events & io_events) == 0u) {
    result.status = OMNI_CONNECTION_DISPATCH_IGNORED;
    result.ignored = true;
    bridge->ignored_dispatches = saturating_add(bridge->ignored_dispatches, 1u);
    bridge->last_result = result;
    return;
  }

  /* Deterministic combined-event order: one READ, then one WRITE. */
  if ((events & OMNI_POLLER_READY_READ) != 0u) {
    result.read_attempted = true;
    bridge->read_dispatches = saturating_add(bridge->read_dispatches, 1u);
    result.read_result = omni_connection_session_readable(session);
    result.bytes_read = (uint64_t)result.read_result.count;
    result.close_worthy = result.close_worthy ||
                          io_result_is_close_worthy(result.read_result);
    bridge->bytes_read = saturating_add(bridge->bytes_read, result.bytes_read);
  }
  if ((events & OMNI_POLLER_READY_WRITE) != 0u) {
    result.write_attempted = true;
    bridge->write_dispatches = saturating_add(bridge->write_dispatches, 1u);
    result.write_result = omni_connection_session_writable(session);
    result.bytes_written = (uint64_t)result.write_result.count;
    result.close_worthy = result.close_worthy ||
                          io_result_is_close_worthy(result.write_result);
    bridge->bytes_written = saturating_add(bridge->bytes_written, result.bytes_written);
  }
  result.status = OMNI_CONNECTION_DISPATCH_OK;
  bridge->last_result = result;
}

enum omni_connection_dispatch_state omni_connection_dispatch_state(
    const struct omni_connection_dispatch *bridge) {
  return bridge == NULL ? OMNI_CONNECTION_DISPATCH_INERT : bridge->state;
}

struct omni_connection_dispatch_result omni_connection_dispatch_last_result(
    const struct omni_connection_dispatch *bridge) {
  if (bridge == NULL) return make_result(OMNI_CONNECTION_DISPATCH_ERR_INVALID, 0u, 0u);
  return bridge->last_result;
}

#define OMNI_DISPATCH_COUNTER_ACCESSOR(name, field) \
  uint64_t name(const struct omni_connection_dispatch *bridge) { \
    return bridge == NULL ? 0u : bridge->field; \
  }

OMNI_DISPATCH_COUNTER_ACCESSOR(omni_connection_dispatch_dispatches, dispatches)
OMNI_DISPATCH_COUNTER_ACCESSOR(omni_connection_dispatch_ignored_dispatches, ignored_dispatches)
OMNI_DISPATCH_COUNTER_ACCESSOR(omni_connection_dispatch_stale_dispatches, stale_dispatches)
OMNI_DISPATCH_COUNTER_ACCESSOR(omni_connection_dispatch_read_dispatches, read_dispatches)
OMNI_DISPATCH_COUNTER_ACCESSOR(omni_connection_dispatch_write_dispatches, write_dispatches)
OMNI_DISPATCH_COUNTER_ACCESSOR(omni_connection_dispatch_error_observations, error_observations)
OMNI_DISPATCH_COUNTER_ACCESSOR(omni_connection_dispatch_bytes_read, bytes_read)
OMNI_DISPATCH_COUNTER_ACCESSOR(omni_connection_dispatch_bytes_written, bytes_written)

#undef OMNI_DISPATCH_COUNTER_ACCESSOR
