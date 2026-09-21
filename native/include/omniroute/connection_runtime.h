/*
 * OmniRoute native backend — bounded connection runtime binding layer (Task 027).
 *
 * This module is the lifecycle coordination point that connects:
 *
 *   event_loop
 *       |
 *   connection_reactor (registry + reactor)
 *       |
 *   connection_session (connection + io)
 *       |
 *   connection (accepted FD + receive buf)
 *
 * Expected attach flow:
 *
 *   connection
 *       |
 *   connection_session (create/open on attach)
 *       |
 *   connection_reactor registration (token)
 *       |
 *   event_loop dispatch (reactor_step -> dispatch)
 *
 * The binding layer owns session lifecycle bookkeeping and bounded attachment
 * metadata. It borrows connections, registry, reactor adapter, event loop, and
 * caller storage. It never closes descriptors, destroys accepted sockets, frees
 * connections, allocates memory, owns poller, or runs the event loop.
 * It never reads/writes automatically, never parses, never processes requests.
 *
 * Memory: fixed capacity only, no dynamic maps, no queues, caller-provided
 * entry array.
 *
 * State machine (runtime object):
 *
 *   NEW --init--> INITIALIZED --attach--> ATTACHED --detach--> INITIALIZED
 *    ^               |                      |   ^                  |
 *    |               +----destroy----------+   |                  |
 *    |                                      +--destroy------------+
 *    +-----------destroy-------------------- CLOSED (idempotent)
 *
 * Sub-states:
 *   - ATTACHED means at least one attachment is live.
 *   - DETACHING is not a distinct persistent state; detach is synchronous.
 *   - CLOSED is terminal after destroy.
 *   For compatibility with the task description, DETACHING is aliased to
 *   INITIALIZED during the synchronous detach step; the explicit enum keeps
 *   it distinct for future use.
 */

#ifndef OMNIROUTE_CONNECTION_RUNTIME_H
#define OMNIROUTE_CONNECTION_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omniroute/connection.h"
#include "omniroute/connection_reactor.h"
#include "omniroute/connection_session.h"
#include "omniroute/event_loop.h"
#include "omniroute/registry.h"

enum omni_connection_runtime_state {
  OMNI_CONNECTION_RUNTIME_NEW = 0,
  OMNI_CONNECTION_RUNTIME_INITIALIZED = 1,
  OMNI_CONNECTION_RUNTIME_ATTACHED = 2,
  OMNI_CONNECTION_RUNTIME_DETACHING = 3, /* transient detach marker, treated as INITIALIZED */
  OMNI_CONNECTION_RUNTIME_CLOSED = 4
};

enum omni_connection_runtime_status {
  OMNI_CONNECTION_RUNTIME_OK = 0,
  OMNI_CONNECTION_RUNTIME_ERR_INVALID = 1,   /* NULL, bad config, bad capacities, bad deps */
  OMNI_CONNECTION_RUNTIME_ERR_STATE = 2,     /* invalid lifecycle transition */
  OMNI_CONNECTION_RUNTIME_ERR_FULL = 3,      /* capacity exhausted */
  OMNI_CONNECTION_RUNTIME_ERR_DUPLICATE = 4, /* connection already attached */
  OMNI_CONNECTION_RUNTIME_ERR_NOT_FOUND = 5, /* detach target absent */
  OMNI_CONNECTION_RUNTIME_ERR_SESSION = 6,   /* session init/open failed */
  OMNI_CONNECTION_RUNTIME_ERR_REACTOR = 7,   /* reactor registration failed */
  OMNI_CONNECTION_RUNTIME_ERR_CLOSED = 8
};

struct omni_connection_runtime_result {
  enum omni_connection_runtime_status status;
  int sys_errno;   /* EINVAL or reactor/session errno, 0 on success */
  uint64_t token;  /* reactor token on attach success, else 0 */
};

struct omni_connection_runtime_attach_config {
  struct omni_connection *connection; /* borrowed, must be OPEN */
  void *receive_storage;              /* caller-owned recv backing for session io */
  size_t receive_capacity;            /* nonzero */
  void *send_storage;                 /* caller-owned send backing for session io */
  size_t send_capacity;               /* nonzero */
};

/* Bounded attachment entry, caller-owned array element. */
struct omni_connection_runtime_entry {
  struct omni_connection *connection; /* borrowed */
  struct omni_connection_session session; /* owned session bookkeeping */
  uint64_t token; /* reactor token, 0 when not attached */
  bool occupied;
};

struct omni_connection_runtime_config {
  struct omni_connection_registry *registry;   /* borrowed, must be live */
  struct omni_connection_reactor *adapter;    /* borrowed, must be live */
  struct omni_event_loop *event_loop;         /* borrowed, may be NULL for this layer */
  struct omni_connection_runtime_entry *entries; /* caller-owned array [capacity] */
  size_t capacity; /* fixed, nonzero */
};

struct omni_connection_runtime {
  struct omni_connection_registry *registry;
  struct omni_connection_reactor *adapter;
  struct omni_event_loop *event_loop;
  struct omni_connection_runtime_entry *entries;
  size_t capacity;
  size_t count;
  enum omni_connection_runtime_state state;
  bool live;
};

/* Canonicalize fresh caller-owned runtime storage to NEW. NULL-safe. */
void omni_connection_runtime_make_inert(struct omni_connection_runtime *rt);

/*
 * Bind runtime to registry, reactor adapter, event loop and fixed entry array.
 * Requires NEW. Transitions NEW -> INITIALIZED. No heap allocation.
 * Invalid input leaves runtime NEW.
 */
struct omni_connection_runtime_result omni_connection_runtime_init(
    struct omni_connection_runtime *rt,
    const struct omni_connection_runtime_config *config);

/*
 * Attach one OPEN connection: create/open a session in a free entry using
 * caller-provided recv/send backing, then register via the connection reactor.
 * Valid from INITIALIZED or ATTACHED. On success count++ and state -> ATTACHED.
 * Failed attach leaves runtime consistent (no partial entry, count unchanged).
 * Duplicate or full returns explicit error without changing runtime state.
 */
struct omni_connection_runtime_result omni_connection_runtime_attach(
    struct omni_connection_runtime *rt,
    const struct omni_connection_runtime_attach_config *attach_config);

/*
 * Detach one attached connection: unregister via adapter, destroy session,
 * free entry. Valid from INITIALIZED or ATTACHED. Missing target returns
 * NOT_FOUND without changing state. If last entry is removed, state returns
 * to INITIALIZED. Detach is synchronous; DETACHING is not observable as a
 * persistent state.
 */
struct omni_connection_runtime_result omni_connection_runtime_detach(
    struct omni_connection_runtime *rt,
    struct omni_connection *connection);

/*
 * Detach all attachments and return runtime to CLOSED. Safe for NULL, NEW,
 * INITIALIZED, ATTACHED, CLOSED (idempotent). Borrowed connections, registry,
 * reactor, event loop and caller arrays are not freed. Descriptors remain
 * open.
 */
void omni_connection_runtime_destroy(struct omni_connection_runtime *rt);

/* State and accounting. NULL reports NEW / 0. */
enum omni_connection_runtime_state omni_connection_runtime_state(
    const struct omni_connection_runtime *rt);
size_t omni_connection_runtime_count(const struct omni_connection_runtime *rt);
size_t omni_connection_runtime_capacity(const struct omni_connection_runtime *rt);
bool omni_connection_runtime_is_initialized(const struct omni_connection_runtime *rt);

/* Find session for a given connection, or NULL if not attached. */
struct omni_connection_session *omni_connection_runtime_find_session(
    struct omni_connection_runtime *rt,
    struct omni_connection *connection);

/* Borrowed FD view for an attached connection, or FD_INVALID. */
int omni_connection_runtime_fd(struct omni_connection_runtime *rt,
                               struct omni_connection *connection);

#endif /* OMNIROUTE_CONNECTION_RUNTIME_H */
