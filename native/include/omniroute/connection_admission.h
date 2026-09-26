/*
 * OmniRoute native backend — bounded listener-to-manager admission (Task 029).
 *
 * This layer borrows one live listener and one live connection manager. It
 * accepts at most one client per `once` call, or at most the explicit finite
 * attempt budget passed to `drain`. It composes the existing accepted-owner,
 * connection, and manager APIs; it does not wait for readiness, perform
 * payload I/O, or interpret application data.
 *
 * Every connection slot and byte range is caller-owned fixed storage. The
 * three byte pools are disjoint: connection receive backing, session receive
 * backing, and session send backing. Each pool is divided into equal
 * per-connection slices. The caller keeps all of this storage and the
 * listener/manager alive until every admission is released or destroyed.
 *
 * The manager's generation token is the admission identity. It is paired
 * with the local slot index only to find the connection object; a stale token
 * cannot release a later occupant of a reused admission slot.
 *
 * An admission is single-owner and externally synchronized. It has no heap
 * allocation, thread, worker, queue, or background activity.
 */

#ifndef OMNIROUTE_CONNECTION_ADMISSION_H
#define OMNIROUTE_CONNECTION_ADMISSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omniroute/accepted.h"
#include "omniroute/connection_manager.h"

#define OMNI_CONNECTION_ADMISSION_SLOT_INVALID SIZE_MAX

enum omni_connection_admission_state {
  OMNI_CONNECTION_ADMISSION_NEW = 0,
  OMNI_CONNECTION_ADMISSION_INITIALIZED,
  OMNI_CONNECTION_ADMISSION_ACTIVE,
  OMNI_CONNECTION_ADMISSION_STOPPING,
  OMNI_CONNECTION_ADMISSION_CLOSED
};

enum omni_connection_admission_status {
  OMNI_CONNECTION_ADMISSION_OK = 0,          /* one client admitted */
  OMNI_CONNECTION_ADMISSION_DRAINED,         /* listener queue would block */
  OMNI_CONNECTION_ADMISSION_LIMIT_REACHED,   /* explicit drain budget consumed */
  OMNI_CONNECTION_ADMISSION_SLOT_FULL,       /* no free caller connection slot */
  OMNI_CONNECTION_ADMISSION_MANAGER_FULL,    /* manager/runtime capacity exhausted */
  OMNI_CONNECTION_ADMISSION_INTERRUPTED,     /* accept was interrupted */
  OMNI_CONNECTION_ADMISSION_TRANSIENT,       /* pending handshake was consumed */
  OMNI_CONNECTION_ADMISSION_ERR_INVALID,
  OMNI_CONNECTION_ADMISSION_ERR_STATE,
  OMNI_CONNECTION_ADMISSION_ERR_ACCEPT,
  OMNI_CONNECTION_ADMISSION_ERR_CONNECTION,
  OMNI_CONNECTION_ADMISSION_ERR_MANAGER,
  OMNI_CONNECTION_ADMISSION_ERR_NOT_FOUND
};

struct omni_connection_admission_identity {
  size_t slot_index;
  uint64_t token; /* existing manager/reactor generation token */
};

struct omni_connection_admission_result {
  enum omni_connection_admission_status status;
  int sys_errno; /* Captured lower-layer errno when present; zero on success. */
  size_t attempts; /* calls to omni_accept_once made by this operation */
  size_t admitted; /* successful admissions in this operation */
  struct omni_connection_admission_identity identity; /* once success; otherwise invalid */
  enum omni_accept_status accept_status; /* lower-layer status at accept stop */
  enum omni_connection_status connection_status; /* connection init/adopt failure */
  enum omni_connection_manager_status manager_status; /* manager attach/release failure */
};

/* One caller-owned storage pool divided into equal per-connection slices. */
struct omni_connection_admission_buffer {
  void *storage;
  size_t storage_bytes;
  size_t per_connection_capacity;
};

/* Caller-owned entry; initialize with slot_make_inert before admission_init. */
struct omni_connection_admission_slot {
  struct omni_connection connection;
  uint64_t token; /* manager generation token while occupied; zero otherwise */
  bool occupied;
};

struct omni_connection_admission_config {
  const struct omni_listener *listener; /* borrowed, must remain live */
  struct omni_connection_manager *manager; /* borrowed, must remain live */
  struct omni_connection_admission_slot *slots; /* caller-owned [capacity] */
  size_t slots_bytes;
  size_t capacity; /* fixed admission slots; cannot exceed manager capacity */
  struct omni_connection_admission_buffer connection_receive;
  struct omni_connection_admission_buffer session_receive;
  struct omni_connection_admission_buffer session_send;
};

struct omni_connection_admission {
  const struct omni_listener *listener;
  struct omni_connection_manager *manager;
  struct omni_connection_admission_slot *slots;
  size_t capacity;
  size_t count;
  void *connection_receive_storage;
  size_t connection_receive_stride;
  void *session_receive_storage;
  size_t session_receive_stride;
  void *session_send_storage;
  size_t session_send_stride;
  enum omni_connection_admission_state state;
  bool live;
};

/* Reset fresh or CLOSED admission storage to NEW. NULL-safe. */
void omni_connection_admission_make_inert(struct omni_connection_admission *admission);

/*
 * Prepare a fixed slot/buffer pool over live borrowed dependencies. Every
 * slot must first be made inert or be CLOSED with zero token/unoccupied.
 * Pool products are overflow-checked and the required memory regions must
 * not overlap. Invalid configuration leaves `admission` NEW.
 */
struct omni_connection_admission_result omni_connection_admission_init(
    struct omni_connection_admission *admission,
    const struct omni_connection_admission_config *config);

/*
 * Attempt at most one accept and admission. The manager token and slot index
 * are returned in `identity` only on success. Capacity/state errors are
 * returned before consuming a pending client.
 */
struct omni_connection_admission_result omni_connection_admission_once(
    struct omni_connection_admission *admission);

/*
 * Make no more than `max_attempts` calls to the existing one-shot accept
 * primitive. `identities` must hold at least max_attempts items; only the
 * first result.admitted entries are written. Earlier admissions stay live if
 * a later accept/admission stops the drain. A fully consumed budget returns
 * LIMIT_REACHED, even when its final attempt admitted a client.
 */
struct omni_connection_admission_result omni_connection_admission_drain(
    struct omni_connection_admission *admission,
    size_t max_attempts,
    struct omni_connection_admission_identity *identities,
    size_t identity_capacity);

/*
 * Detach the manager/reactor/session membership, then destroy the connection
 * owner and make its slot reusable. A stale/missing identity changes nothing.
 * Release remains valid in STOPPING.
 */
struct omni_connection_admission_result omni_connection_admission_release(
    struct omni_connection_admission *admission,
    struct omni_connection_admission_identity identity);

/* Forbid new accepts; existing identities remain releasable. Idempotent in STOPPING. */
struct omni_connection_admission_result omni_connection_admission_stop(
    struct omni_connection_admission *admission);

/* Release memberships before connections; on lower-layer failure retain storage. */
struct omni_connection_admission_result omni_connection_admission_destroy(
    struct omni_connection_admission *admission);

/* NULL reports NEW / zero. */
enum omni_connection_admission_state omni_connection_admission_state(
    const struct omni_connection_admission *admission);
size_t omni_connection_admission_count(const struct omni_connection_admission *admission);
size_t omni_connection_admission_capacity(const struct omni_connection_admission *admission);

/*
 * Initialize a fresh or CLOSED slot. This is not cleanup for READY/OPEN or
 * otherwise live connection storage; release/destroy it through admission.
 */
void omni_connection_admission_slot_make_inert(struct omni_connection_admission_slot *slot);

#endif /* OMNIROUTE_CONNECTION_ADMISSION_H */
