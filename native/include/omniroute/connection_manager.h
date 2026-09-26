/*
 * OmniRoute native backend — bounded connection manager layer (Task 028).
 *
 * This module manages a fixed collection of active connection_runtime
 * attachments. It is the bounded membership layer above the runtime
 * binding:
 *
 *   accepted connection
 *         |
 *   connection creation (connection_from_accepted, OPEN)
 *         |
 *   connection_runtime_attach()  (session init/open + reactor registration)
 *         |
 *   connection_manager stores bounded membership (this layer)
 *
 * Expected add flow:
 *
 *   caller creates OPEN connection (borrowed backing)
 *       |
 *   manager_add(manager, attach_config)  -> runtime_attach -> store entry
 *       |
 *   manager tracks token + occupancy
 *
 * Expected remove flow:
 *
 *   manager_remove(manager, connection) -> runtime_detach -> slot reusable
 *       |
 *   entry cleared, count--, token retired
 *
 * Ownership:
 *
 *   Manager OWNS:  fixed entry array bookkeeping (occupied + token) and
 *                  active membership count.
 *   Manager BORROWS: runtime (which itself borrows registry, reactor,
 *                    event loop), registry, reactor, event loop, caller
 *                    entry storage, caller connection objects, caller
 *                    receive/send backing. The manager never owns the
 *                    runtime object, never owns registry/reactor/poller
 *                    storage, never owns accepted descriptors, never owns
 *                    connections, never owns poller registrations.
 *
 *   Manager MUST NOT: close sockets, destroy accepted objects, free
 *                     connections, allocate memory, run the event loop,
 *                     process payloads, parse protocols, handle HTTP,
 *                     manage poller directly, allocate per-connection
 *                     heap memory, or change connection lifecycle beyond
 *                     the delegated runtime attach/detach.
 *
 * Memory:
 *
 *   - zero malloc/free: all steady-state operations are bounded and use
 *     only caller-provided storage.
 *   - fixed capacity: provided at init, never grows.
 *   - caller supplied storage: the entry array and the manager object
 *     itself are caller-owned.
 *
 *   Documented sizes (64-bit Linux ABI, C11, no packing):
 *
 *     sizeof(struct omni_connection_manager)       — manager object
 *     sizeof(struct omni_connection_manager_entry) — one membership slot
 *     sizeof(struct omni_connection_manager_config) — transient init view
 *
 *   Measured by the focused native test: manager 40 bytes, entry 24 bytes,
 *   config 24 bytes. For capacity N, manager-owned logical storage is
 *   40 + N * 24 bytes on this ABI; runtime/session and recv/send backing are
 *   separate storage and are not part of the manager-only formula.
 *
 *   Capacity formula (caller reservation):
 *
 *     total = sizeof(struct omni_connection_manager)
 *           + capacity * sizeof(struct omni_connection_manager_entry)
 *
 *   The entry array is a flat dense array scanned linearly; occupancy
 *   determines liveness. No chaining, no spare lists, no hidden blocks.
 *
 * Token / identity:
 *
 *   The manager preserves the existing generation/token rules from the
 *   underlying registry/reactor. The token stored per entry is the
 *   reactor token (generation << 32 | index) produced by
 *   connection_runtime_attach / connection_reactor_attach. Detach
 *   retires the generation via the registry, so a captured token
 *   becomes stale and cannot reach a later occupant of the same slot.
 *   No pointer-as-ID, no heap map, no custom generation.
 *
 * State machine (manager object):
 *
 *   NEW --init--> INITIALIZED --start--> RUNNING --stop--> STOPPING --destroy--> CLOSED
 *    ^               |   ^                    |               |                 ^
 *    |               |   |                    |               |                 |
 *    |               +---add (auto RUNNING)---+               |                 |
 *    |               |                        |               |                 |
 *    |               +-------destroy----------+------destroy--+                 |
 *    |               |                        |               |                 |
 *    +-----------destroy----------------------+----------------+                 |
 *    |               |                        |               |                 |
 *    +--make_inert---+                        +--remove------+                 |
 *                    |                        |   (last)     |                 |
 *                    +------------------------+--------------+                 |
 *                                                             |                 |
 *   Add is valid from INITIALIZED or RUNNING. Remove is valid
 *   from INITIALIZED, RUNNING, or STOPPING. Stop moves RUNNING
 *   or INITIALIZED to STOPPING and forbids further add. Destroy
 *   is idempotent from any state and reaches CLOSED. Failed add
 *   leaves count and state unchanged and never leaves a partial
 *   entry. Operations before init are rejected.
 */

#ifndef OMNIROUTE_CONNECTION_MANAGER_H
#define OMNIROUTE_CONNECTION_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omniroute/connection.h"
#include "omniroute/connection_runtime.h"

enum omni_connection_manager_state {
  OMNI_CONNECTION_MANAGER_NEW = 0,
  OMNI_CONNECTION_MANAGER_INITIALIZED = 1,
  OMNI_CONNECTION_MANAGER_RUNNING = 2,
  OMNI_CONNECTION_MANAGER_STOPPING = 3,
  OMNI_CONNECTION_MANAGER_CLOSED = 4
};

enum omni_connection_manager_status {
  OMNI_CONNECTION_MANAGER_OK = 0,
  OMNI_CONNECTION_MANAGER_ERR_INVALID = 1,   /* NULL, bad config, bad capacities */
  OMNI_CONNECTION_MANAGER_ERR_STATE = 2,     /* invalid lifecycle transition */
  OMNI_CONNECTION_MANAGER_ERR_FULL = 3,      /* capacity exhausted */
  OMNI_CONNECTION_MANAGER_ERR_DUPLICATE = 4, /* connection already managed */
  OMNI_CONNECTION_MANAGER_ERR_NOT_FOUND = 5, /* remove target absent */
  OMNI_CONNECTION_MANAGER_ERR_RUNTIME = 6,   /* underlying runtime failure */
  OMNI_CONNECTION_MANAGER_ERR_CLOSED = 7
};

struct omni_connection_manager_result {
  enum omni_connection_manager_status status;
  int sys_errno;   /* EINVAL, ENOSPC, EEXIST, ENOENT or runtime errno, 0 on success */
  uint64_t token;  /* reactor token on add success, else 0 */
};

/* Reuse runtime attach config shape; alias for caller convenience. */
struct omni_connection_manager_attach_config {
  struct omni_connection *connection; /* borrowed, must be OPEN */
  void *receive_storage;              /* caller-owned recv backing */
  size_t receive_capacity;            /* nonzero */
  void *send_storage;                 /* caller-owned send backing */
  size_t send_capacity;               /* nonzero */
};

/* Bounded membership entry, caller-owned array element. */
struct omni_connection_manager_entry {
  struct omni_connection *connection; /* borrowed */
  uint64_t token; /* reactor token, 0 when not occupied */
  bool occupied;
};

/* Init config — all pointers borrowed, array is caller-owned [capacity]. */
struct omni_connection_manager_config {
  struct omni_connection_runtime *runtime;          /* borrowed, must be live */
  struct omni_connection_manager_entry *entries;    /* caller-owned array [capacity] */
  size_t capacity; /* fixed, nonzero */
};

struct omni_connection_manager {
  struct omni_connection_runtime *runtime;       /* borrowed */
  struct omni_connection_manager_entry *entries; /* borrowed array */
  size_t capacity;
  size_t count;
  enum omni_connection_manager_state state;
  bool live;
};

/* Canonicalize fresh caller-owned manager storage to NEW. NULL-safe. */
void omni_connection_manager_make_inert(struct omni_connection_manager *mgr);

/*
 * Bind manager to a live runtime and a fixed entry array.
 * Requires NEW. Transitions NEW -> INITIALIZED. No heap allocation.
 * Invalid input leaves manager NEW.
 */
struct omni_connection_manager_result omni_connection_manager_init(
    struct omni_connection_manager *mgr,
    const struct omni_connection_manager_config *config);

/*
 * Move manager from INITIALIZED to RUNNING. Valid from INITIALIZED.
 * Idempotent from RUNNING. Rejected from NEW/STOPPING/CLOSED.
 */
struct omni_connection_manager_result omni_connection_manager_start(
    struct omni_connection_manager *mgr);

/*
 * Move manager from INITIALIZED or RUNNING to STOPPING. Valid from
 * INITIALIZED or RUNNING. Idempotent from STOPPING. Rejected from
 * NEW/CLOSED. After STOPPING, add is rejected.
 */
struct omni_connection_manager_result omni_connection_manager_stop(
    struct omni_connection_manager *mgr);

/*
 * Add one OPEN connection: delegate to connection_runtime_attach with
 * caller-provided recv/send backing, then store bounded membership.
 * Valid from INITIALIZED or RUNNING. On success count++ and state
 * becomes RUNNING (auto-start if from INITIALIZED). Failed add leaves
 * manager consistent (no partial entry, count and state unchanged).
 * Duplicate or full returns explicit error without changing state.
 */
struct omni_connection_manager_result omni_connection_manager_add(
    struct omni_connection_manager *mgr,
    const struct omni_connection_manager_attach_config *attach_config);

/*
 * Remove one managed connection: delegate to connection_runtime_detach,
 * then free the entry. Valid from INITIALIZED, RUNNING, or STOPPING.
 * Missing target returns NOT_FOUND without changing state. If last
 * entry is removed while RUNNING, state returns to INITIALIZED;
 * while STOPPING it stays STOPPING. Remove is synchronous.
 */
struct omni_connection_manager_result omni_connection_manager_remove(
    struct omni_connection_manager *mgr,
    struct omni_connection *connection);

/*
 * Detach all memberships and return manager to CLOSED. Safe for NULL,
 * NEW, INITIALIZED, RUNNING, STOPPING, CLOSED (idempotent). Borrowed
 * runtime, registry, reactor, event loop, caller arrays and connections
 * are not freed. Descriptors remain open. Underlying runtime entries
 * are detached via runtime_detach best-effort.
 */
void omni_connection_manager_destroy(struct omni_connection_manager *mgr);

/* State and accounting. NULL reports NEW / 0. */
enum omni_connection_manager_state omni_connection_manager_state(
    const struct omni_connection_manager *mgr);
size_t omni_connection_manager_count(const struct omni_connection_manager *mgr);
size_t omni_connection_manager_capacity(const struct omni_connection_manager *mgr);

/* Find entry for a given connection, or NULL if not managed. */
const struct omni_connection_manager_entry *omni_connection_manager_find(
    const struct omni_connection_manager *mgr,
    struct omni_connection *connection);

#endif /* OMNIROUTE_CONNECTION_MANAGER_H */
