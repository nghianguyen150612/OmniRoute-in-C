/*
 * OmniRoute native backend — bounded connection registry (Task 020).
 *
 * A small bookkeeping layer that tracks multiple live connection objects
 * for a future event loop. It stores borrowed pointers only: connection
 * objects stay owned by their caller, and this layer never creates,
 * destroys, resets, closes, reads from, writes to, polls, or registers
 * any connection. It performs no payload input/output, no accept work,
 * no readiness observation, no timer work, no thread work, and no HTTP
 * work. Later tasks add the event loop itself; this task only records
 * membership.
 *
 * Storage is explicit and bounded. The caller provides a fixed slot array
 * plus the registry object itself; capacity is fixed at initialization
 * and never grows. There is no owned-backing form and no steady-state
 * allocation of any kind. Beyond capacity, addition fails cleanly with
 * state unchanged.
 *
 * Ownership split: the registry owns slots and handle metadata only.
 * Connections remain externally owned from add until after remove or
 * registry destroy. Removal forgets the entry without touching the
 * connection. Registry destroy forgets every entry without touching any
 * connection and without releasing the caller slot array. The caller
 * keeps both the connection objects and the slot array alive until after
 * registry destroy.
 *
 * Handle model: each membership is named by an opaque slot-plus-generation
 * pair. The slot position selects the storage, the generation distinguishes
 * successive occupants of the same position. A handle is valid only while
 * the registry is live and its entry is still present with the same
 * generation. Removal preserves the stored generation so a later occupant
 * of the same position carries a different generation; the old handle then
 * fails lookup and fails a second remove. Handles never survive registry
 * destroy: after destroy every old handle is invalid and must be dropped
 * by the caller, even if the same slot array is reused for a later init.
 * Raw positions are never exposed as identities; iteration publishes
 * handles alongside pointers.
 *
 * Memory cost: the registry object itself holds one borrowed array
 * reference plus two counts and a liveness flag. Each slot holds one
 * borrowed connection reference plus one generation counter plus one
 * occupancy flag. See the slot and registry struct comments for the exact
 * layout. No per-entry chaining, no spare lists, no hidden blocks.
 *
 * Iteration: live entries visit in increasing slot order, which is fully
 * determined by the sequence of add and remove calls. Iteration needs no
 * extra storage and never mutates the registry, so removing an entry
 * during a scan only hides that entry from later steps.
 *
 * A registry is single-owner and externally synchronized: no mutexes, no
 * atomics, no thread-safety machinery.
 */

#ifndef OMNIROUTE_REGISTRY_H
#define OMNIROUTE_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration only: the registry never dereferences a connection. */
struct omni_connection;

/* Opaque membership identity: storage position plus occupant generation. */
struct omni_connection_registry_handle {
  uint32_t index;
  uint32_t generation;
};

/* Invalid handle sentinel: never produced by a successful add. */
#define OMNI_CONNECTION_REGISTRY_HANDLE_INVALID \
  ((struct omni_connection_registry_handle){ 0xFFFFFFFFu, 0u })

/* Caller-provided membership storage, one element per capacity slot. */
struct omni_connection_registry_slot {
  struct omni_connection *connection; /* borrowed, never dereferenced here */
  uint32_t generation;                /* occupant epoch, zero means never used */
  bool occupied;                      /* true exactly while holding a member */
};

/* Registry object itself, also caller-owned. */
struct omni_connection_registry {
  struct omni_connection_registry_slot *slots; /* borrowed array, see init */
  size_t capacity;                             /* fixed slot count */
  size_t count;                                /* live members, never above cap */
  bool live; /* false before init, after destroy, or after failed init */
};

enum omni_connection_registry_status {
  OMNI_CONNECTION_REGISTRY_OK = 0,
  OMNI_CONNECTION_REGISTRY_ERR_INVALID,   /* bad args or non-live registry */
  OMNI_CONNECTION_REGISTRY_ERR_FULL,      /* no free slot, state unchanged */
  OMNI_CONNECTION_REGISTRY_ERR_DUPLICATE, /* pointer already tracked */
  OMNI_CONNECTION_REGISTRY_ERR_NOT_FOUND  /* unknown or stale handle */
};

struct omni_connection_registry_result {
  enum omni_connection_registry_status status;
  int sys_errno; /* errno-style code at the stop point, zero on success */
  struct omni_connection_registry_handle handle; /* valid only on add success */
};

/*
 * Canonicalize fresh caller-owned registry storage to inert without
 * touching any slot array. NULL-safe. Call before first init when the
 * struct is not statically zeroed. Not a cleanup path for a live
 * registry; destroy live registries first.
 */
void omni_connection_registry_make_inert(struct omni_connection_registry *registry);

/*
 * Borrow a caller-owned slot array of exactly capacity entries. The caller
 * keeps the array alive until after destroy; destroy never releases it.
 * Capacity must be nonzero and fit in 32 bits so every position stays
 * representable in a handle. Re-initializing a live registry is rejected
 * with state preserved. All other invalid input leaves the registry inert.
 * On success every slot starts empty with a fresh occupant epoch.
 */
struct omni_connection_registry_result omni_connection_registry_init(
    struct omni_connection_registry *registry,
    struct omni_connection_registry_slot *slots, size_t capacity);

/*
 * Track one caller-owned connection. The pointer must be non-NULL and not
 * already tracked. The first free position in increasing order is used, so
 * the placement is deterministic for any fixed call sequence. On success
 * the result carries the new stable handle. Every failure leaves existing
 * entries unchanged.
 */
struct omni_connection_registry_result omni_connection_registry_add(
    struct omni_connection_registry *registry, struct omni_connection *connection);

/*
 * Forget one membership without touching its connection. The connection
 * stays owned by its caller and its descriptor, if any, stays open. A
 * second remove of the same handle reports NOT_FOUND. Unknown positions
 * and generation mismatches report NOT_FOUND with state unchanged.
 */
struct omni_connection_registry_result omni_connection_registry_remove(
    struct omni_connection_registry *registry,
    struct omni_connection_registry_handle handle);

/*
 * Resolve a handle to its borrowed connection, or NULL for any stale,
 * unknown, or non-live case. Never mutates the registry. The returned
 * pointer stays externally owned and expires at remove or destroy.
 */
struct omni_connection *omni_connection_registry_find(
    const struct omni_connection_registry *registry,
    struct omni_connection_registry_handle handle);

/*
 * Visit live entries in increasing slot order without extra storage.
 * The caller starts with cursor zero; each successful call publishes the
 * next live handle plus its borrowed connection and advances the cursor
 * past the published position. Returns false when no live entry remains
 * at or after the cursor, with the cursor parked at capacity. Never
 * mutates the registry; entries removed mid-scan are simply skipped.
 */
bool omni_connection_registry_next(const struct omni_connection_registry *registry,
                                   size_t *cursor,
                                   struct omni_connection_registry_handle *out_handle,
                                   struct omni_connection **out_connection);

/* Handle helpers: equality and sentinel test. */
bool omni_connection_registry_handle_equal(struct omni_connection_registry_handle a,
                                           struct omni_connection_registry_handle b);
bool omni_connection_registry_handle_is_valid(
    struct omni_connection_registry_handle handle);

/*
 * Release the borrowed slot reference and forget every membership.
 * Connections are untouched: no destroy, no descriptor release, no buffer
 * work. NULL, inert, and repeated destroy are safe no-ops. All handles
 * become invalid at destroy and must be dropped by the caller.
 */
void omni_connection_registry_destroy(struct omni_connection_registry *registry);

/* Cheap local accounting. NULL or non-live registries report zero. */
size_t omni_connection_registry_capacity(const struct omni_connection_registry *registry);
size_t omni_connection_registry_count(const struct omni_connection_registry *registry);

#endif /* OMNIROUTE_REGISTRY_H */
