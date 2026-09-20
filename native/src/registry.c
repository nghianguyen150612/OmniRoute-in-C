/*
 * OmniRoute native backend — bounded connection registry implementation.
 *
 * Conversion strategy: capacities stay in size_t end to end and are
 * range-checked against the 32-bit handle position space at init, so the
 * single narrowing cast from scan position to handle position on add and
 * on iteration is always exact. Generations stay in 32 bits and wrap from
 * the maximum back to one, never zero, so zero keeps its never-used
 * meaning. Counts never exceed capacity, so no count conversion can
 * overflow and no published total exceeds the provided storage.
 *
 * Stale-handle audit: lookup and remove compare both position and
 * generation while the entry is marked occupied. Removal clears occupancy
 * and the borrowed reference but preserves the generation; the next add
 * into the same position advances it, so the previous handle can never
 * validate again. Destroy drops the array reference without touching any
 * connection, which retires every handle at once.
 *
 * Touch audit: this file holds no descriptor, readiness, payload, timer,
 * thread, or HTTP machinery by construction. It stores and compares
 * borrowed connection references only and never dereferences them, so no
 * connection lifetime, buffer, or poller detail can leak in here. The
 * network-boundary gate enforces the same property textually.
 */

#include "omniroute/registry.h"

#include <errno.h>
#include <stdint.h>

static void mark_inert(struct omni_connection_registry *registry) {
  registry->slots = NULL;
  registry->capacity = 0u;
  registry->count = 0u;
  registry->live = false;
}

static struct omni_connection_registry_result make_result(
    enum omni_connection_registry_status status, int err,
    struct omni_connection_registry_handle handle) {
  struct omni_connection_registry_result out;

  out.status = status;
  out.sys_errno = err;
  out.handle = handle;
  return out;
}

static struct omni_connection_registry_handle make_handle(uint32_t index,
                                                          uint32_t generation) {
  struct omni_connection_registry_handle handle;

  handle.index = index;
  handle.generation = generation;
  return handle;
}

static uint32_t next_generation(uint32_t current) {
  if (current == 0xFFFFFFFFu) {
    return 1u;
  }
  if (current == 0u) {
    return 1u;
  }
  return current + 1u;
}

static bool slot_matches(const struct omni_connection_registry_slot *slot,
                         struct omni_connection_registry_handle handle) {
  return slot->occupied && slot->generation == handle.generation &&
         handle.generation != 0u;
}

void omni_connection_registry_make_inert(struct omni_connection_registry *registry) {
  if (registry == NULL) {
    return;
  }
  mark_inert(registry);
}

struct omni_connection_registry_result omni_connection_registry_init(
    struct omni_connection_registry *registry,
    struct omni_connection_registry_slot *slots, size_t capacity) {
  size_t i = 0u;

  if (registry == NULL) {
    return make_result(OMNI_CONNECTION_REGISTRY_ERR_INVALID, EINVAL,
                       OMNI_CONNECTION_REGISTRY_HANDLE_INVALID);
  }
  /* A live registry holds memberships; refuse to discard them implicitly. */
  if (registry->live) {
    return make_result(OMNI_CONNECTION_REGISTRY_ERR_INVALID, EINVAL,
                       OMNI_CONNECTION_REGISTRY_HANDLE_INVALID);
  }
  if (slots == NULL || capacity == 0u ||
      capacity > (size_t)0xFFFFFFFFu) {
    mark_inert(registry);
    return make_result(OMNI_CONNECTION_REGISTRY_ERR_INVALID, EINVAL,
                       OMNI_CONNECTION_REGISTRY_HANDLE_INVALID);
  }
  registry->slots = slots;
  registry->capacity = capacity;
  registry->count = 0u;
  registry->live = true;
  for (i = 0u; i < capacity; ++i) {
    registry->slots[i].connection = NULL;
    registry->slots[i].generation = 0u;
    registry->slots[i].occupied = false;
  }
  return make_result(OMNI_CONNECTION_REGISTRY_OK, 0,
                     OMNI_CONNECTION_REGISTRY_HANDLE_INVALID);
}

struct omni_connection_registry_result omni_connection_registry_add(
    struct omni_connection_registry *registry, struct omni_connection *connection) {
  size_t i = 0u;
  size_t free_at = 0u;
  bool found_free = false;

  if (registry == NULL || !registry->live) {
    return make_result(OMNI_CONNECTION_REGISTRY_ERR_INVALID, EINVAL,
                       OMNI_CONNECTION_REGISTRY_HANDLE_INVALID);
  }
  if (connection == NULL) {
    return make_result(OMNI_CONNECTION_REGISTRY_ERR_INVALID, EINVAL,
                       OMNI_CONNECTION_REGISTRY_HANDLE_INVALID);
  }
  /* Duplicate check first: failure must not consume a free position. */
  for (i = 0u; i < registry->capacity; ++i) {
    if (registry->slots[i].occupied && registry->slots[i].connection == connection) {
      return make_result(OMNI_CONNECTION_REGISTRY_ERR_DUPLICATE, EEXIST,
                         OMNI_CONNECTION_REGISTRY_HANDLE_INVALID);
    }
  }
  /* Deterministic placement: first free position in increasing order. */
  for (i = 0u; i < registry->capacity; ++i) {
    if (!registry->slots[i].occupied) {
      free_at = i;
      found_free = true;
      break;
    }
  }
  if (!found_free) {
    return make_result(OMNI_CONNECTION_REGISTRY_ERR_FULL, ENOSPC,
                       OMNI_CONNECTION_REGISTRY_HANDLE_INVALID);
  }
  registry->slots[free_at].connection = connection;
  registry->slots[free_at].generation = next_generation(registry->slots[free_at].generation);
  registry->slots[free_at].occupied = true;
  registry->count += 1u;
  return make_result(OMNI_CONNECTION_REGISTRY_OK, 0,
                     make_handle((uint32_t)free_at,
                                 registry->slots[free_at].generation));
}

struct omni_connection_registry_result omni_connection_registry_remove(
    struct omni_connection_registry *registry,
    struct omni_connection_registry_handle handle) {
  struct omni_connection_registry_slot *slot = NULL;

  if (registry == NULL || !registry->live) {
    return make_result(OMNI_CONNECTION_REGISTRY_ERR_INVALID, EINVAL,
                       OMNI_CONNECTION_REGISTRY_HANDLE_INVALID);
  }
  if (handle.generation == 0u || (size_t)handle.index >= registry->capacity) {
    return make_result(OMNI_CONNECTION_REGISTRY_ERR_NOT_FOUND, ENOENT,
                       OMNI_CONNECTION_REGISTRY_HANDLE_INVALID);
  }
  slot = &registry->slots[handle.index];
  if (!slot_matches(slot, handle)) {
    return make_result(OMNI_CONNECTION_REGISTRY_ERR_NOT_FOUND, ENOENT,
                       OMNI_CONNECTION_REGISTRY_HANDLE_INVALID);
  }
  /* Forget the entry only: the connection object is left entirely alone. */
  slot->connection = NULL;
  slot->occupied = false;
  registry->count -= 1u;
  return make_result(OMNI_CONNECTION_REGISTRY_OK, 0,
                     OMNI_CONNECTION_REGISTRY_HANDLE_INVALID);
}

struct omni_connection *omni_connection_registry_find(
    const struct omni_connection_registry *registry,
    struct omni_connection_registry_handle handle) {
  const struct omni_connection_registry_slot *slot = NULL;

  if (registry == NULL || !registry->live) {
    return NULL;
  }
  if (handle.generation == 0u || (size_t)handle.index >= registry->capacity) {
    return NULL;
  }
  slot = &registry->slots[handle.index];
  if (!slot_matches(slot, handle)) {
    return NULL;
  }
  return slot->connection;
}

bool omni_connection_registry_next(const struct omni_connection_registry *registry,
                                   size_t *cursor,
                                   struct omni_connection_registry_handle *out_handle,
                                   struct omni_connection **out_connection) {
  size_t i = 0u;

  if (registry == NULL || !registry->live || cursor == NULL || out_handle == NULL ||
      out_connection == NULL) {
    return false;
  }
  for (i = *cursor; i < registry->capacity; ++i) {
    if (registry->slots[i].occupied) {
      *out_handle = make_handle((uint32_t)i, registry->slots[i].generation);
      *out_connection = registry->slots[i].connection;
      *cursor = i + 1u;
      return true;
    }
  }
  *cursor = registry->capacity;
  return false;
}

bool omni_connection_registry_handle_equal(struct omni_connection_registry_handle a,
                                           struct omni_connection_registry_handle b) {
  return a.index == b.index && a.generation == b.generation;
}

bool omni_connection_registry_handle_is_valid(
    struct omni_connection_registry_handle handle) {
  if (omni_connection_registry_handle_equal(handle,
                                            OMNI_CONNECTION_REGISTRY_HANDLE_INVALID)) {
    return false;
  }
  return handle.generation != 0u;
}

void omni_connection_registry_destroy(struct omni_connection_registry *registry) {
  if (registry == NULL || !registry->live) {
    return;
  }
  /* Borrowed array and borrowed connections are both released by reference
   * only; no entry is touched and no connection detail is involved. */
  mark_inert(registry);
}

size_t omni_connection_registry_capacity(const struct omni_connection_registry *registry) {
  if (registry == NULL || !registry->live) {
    return 0u;
  }
  return registry->capacity;
}

size_t omni_connection_registry_count(const struct omni_connection_registry *registry) {
  if (registry == NULL || !registry->live) {
    return 0u;
  }
  return registry->count;
}
