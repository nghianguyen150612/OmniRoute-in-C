/*
 * OmniRoute native backend — connection/reactor lifecycle adapter (Task 022).
 *
 * This module is the explicit bridge between one caller-owned connection
 * registry and one generic reactor:
 *
 *   omni_connection -> omni_connection_reactor -> omni_reactor
 *                                      |
 *                                      +-> registry lookup
 *
 * The adapter registers an OPEN connection's borrowed descriptor and
 * interests, then translates the reactor's opaque token back into the
 * registry-owned connection pointer before invoking one caller-owned
 * readiness callback. It never reads or writes payload bytes, changes
 * connection state, closes a descriptor, or destroys a connection.
 *
 * The registry and reactor are borrowed. The caller keeps them, their fixed
 * backing arrays, every connection, the callback, and the callback context
 * alive for the adapter lifetime. A connection must be detached before its
 * storage is destroyed or reused. Destroying the adapter unregisters every
 * membership it can reach but never destroys the corresponding connections.
 *
 * Token model: a token is the registry handle encoded as
 *   (uint64_t)generation << 32 | index.
 * The registry generation changes when a slot is reused, so a readiness
 * record captured before detach resolves to no connection after detach and
 * cannot reach a later occupant of that slot. Tokens are meaningful only
 * while this adapter's registry membership exists. Registry destruction and
 * adapter destruction retire all outstanding tokens; callers must not retain
 * them for a later registry initialization. The existing registry's bounded
 * 32-bit generation wrap is the only eventual reuse rule.
 *
 * A connection/reactor adapter is single-owner and externally synchronized:
 * no mutexes, atomics, worker queues, timers, or background loop exist here.
 */

#ifndef OMNIROUTE_CONNECTION_REACTOR_H
#define OMNIROUTE_CONNECTION_REACTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omniroute/connection.h"
#include "omniroute/reactor.h"
#include "omniroute/registry.h"

typedef void (*omni_connection_reactor_callback)(struct omni_connection *connection,
                                                  uint64_t token,
                                                  uint32_t events,
                                                  void *context);

enum omni_connection_reactor_status {
  OMNI_CONNECTION_REACTOR_OK = 0,
  OMNI_CONNECTION_REACTOR_IGNORED,        /* stale token or unavailable registration */
  OMNI_CONNECTION_REACTOR_ERR_INVALID,   /* NULL or non-live dependency */
  OMNI_CONNECTION_REACTOR_ERR_STATE,     /* bad adapter/connection lifecycle state */
  OMNI_CONNECTION_REACTOR_ERR_DUPLICATE, /* connection is already attached */
  OMNI_CONNECTION_REACTOR_ERR_FULL,      /* registry or reactor capacity exhausted */
  OMNI_CONNECTION_REACTOR_ERR_NOT_FOUND, /* detach target is absent */
  OMNI_CONNECTION_REACTOR_ERR_REGISTRY,  /* registry operation failed */
  OMNI_CONNECTION_REACTOR_ERR_REACTOR   /* reactor registration failed */
};

struct omni_connection_reactor_result {
  enum omni_connection_reactor_status status;
  int sys_errno; /* errno-style failure detail; zero for success/ignored */
  uint64_t token; /* attach/detach target; zero when no token exists */
  size_t count;   /* callbacks dispatched; zero for lifecycle operations */
};

struct omni_connection_reactor {
  struct omni_reactor *reactor; /* borrowed; kept live until destroy */
  struct omni_connection_registry *registry; /* borrowed; kept live until destroy */
  omni_connection_reactor_callback callback; /* borrowed function */
  void *context;                              /* borrowed callback context */
  bool live;
};

/*
 * Set fresh caller-owned adapter storage to inert. NULL-safe. This is not a
 * cleanup operation for a live adapter: use destroy first so registrations
 * are removed and the reactor no longer retains the adapter context.
 */
void omni_connection_reactor_make_inert(struct omni_connection_reactor *adapter);

/*
 * Bind one live generic reactor to one live caller-owned connection registry.
 * The callback and context are shared by every attached connection. No
 * allocation occurs. Re-initializing a live adapter is rejected.
 */
struct omni_connection_reactor_result omni_connection_reactor_init(
    struct omni_connection_reactor *adapter,
    struct omni_reactor *reactor,
    struct omni_connection_registry *registry,
    omni_connection_reactor_callback callback,
    void *context);

/*
 * Detach all registry memberships reachable through the adapter and leave it
 * inert. Connections, descriptors, registry backing, reactor backing, and
 * callback storage remain caller-owned. Call this before releasing adapter
 * storage; the normal destruction order is adapter, reactor, registry, then
 * each connection's external storage.
 */
void omni_connection_reactor_destroy(struct omni_connection_reactor *adapter);

/*
 * Register one OPEN connection. The adapter adds the connection to the
 * registry first, derives its generation/index token, and then adds the
 * borrowed FD to the reactor. A reactor failure rolls the registry addition
 * back when possible; the connection is never destroyed or closed.
 */
struct omni_connection_reactor_result omni_connection_reactor_attach(
    struct omni_connection_reactor *adapter, struct omni_connection *connection);

/*
 * Remove one connection's reactor registration and registry membership.
 * Detach is idempotent only as a safe error: a missing second detach reports
 * NOT_FOUND. Registry membership is retired even when an already-live reactor
 * reports a removal failure, which makes any captured token stale and safe to
 * ignore. No connection lifecycle transition is performed.
 */
struct omni_connection_reactor_result omni_connection_reactor_detach(
    struct omni_connection_reactor *adapter, struct omni_connection *connection);

/*
 * Translate one reactor callback into the connection callback. The token is
 * resolved through the registry and the callback is invoked synchronously
 * only for an attached OPEN connection. Stale, removed, closed, or otherwise
 * unavailable tokens are ignored without invoking the connection callback.
 */
struct omni_connection_reactor_result omni_connection_reactor_dispatch(
    struct omni_connection_reactor *adapter, uint64_t token, uint32_t events);

/* Cheap local accounting through the borrowed registry. */
size_t omni_connection_reactor_count(const struct omni_connection_reactor *adapter);
size_t omni_connection_reactor_capacity(const struct omni_connection_reactor *adapter);

#endif /* OMNIROUTE_CONNECTION_REACTOR_H */
