/*
 * OmniRoute native backend — bounded runtime coordinator foundation (Task 023).
 *
 * The runtime coordinates the lifetime of the existing listener, connection
 * registry, and reactor primitives:
 *
 *   caller storage -> registry -> poller support -> reactor -> listener
 *
 * It owns lifecycle ordering and records which subsystem initializers have
 * completed. It does not own protocol state, application callbacks, provider
 * state, HTTP state, or any connection object. The separate event-loop layer
 * calls the existing bounded reactor step explicitly after runtime_start;
 * this coordinator itself still does not run a wait or accept loop.
 *
 * The runtime object, listener, registry, reactor, and every backing array
 * are caller-owned storage. The coordinator borrows those objects for the
 * duration of initialization and calls their existing lifecycle functions;
 * it never allocates, grows, queues, or frees caller memory. The listener
 * descriptor created by a successful runtime initialization remains owned by
 * the listener primitive and is released through omni_listener_destroy during
 * the coordinator shutdown sequence. Descriptors belonging to external
 * connections are never closed here.
 *
 * A runtime is single-owner and externally synchronized: no mutexes,
 * atomics, threads, timers, workers, or background storage exist here.
 */

#ifndef OMNIROUTE_RUNTIME_H
#define OMNIROUTE_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omniroute/listener.h"
#include "omniroute/poller.h"
#include "omniroute/reactor.h"
#include "omniroute/registry.h"

enum omni_runtime_state {
  OMNI_RUNTIME_INERT = 0,
  OMNI_RUNTIME_INITIALIZED,
  OMNI_RUNTIME_RUNNING,
  OMNI_RUNTIME_STOPPING,
  OMNI_RUNTIME_STOPPED
};

enum omni_runtime_status {
  OMNI_RUNTIME_OK = 0,
  OMNI_RUNTIME_ERR_INVALID,  /* bad config, NULL, live borrowed object, or bad storage */
  OMNI_RUNTIME_ERR_STATE,    /* invalid lifecycle transition */
  OMNI_RUNTIME_ERR_REGISTRY, /* registry initialization failed */
  OMNI_RUNTIME_ERR_POLLER,   /* poller support initialization failed */
  OMNI_RUNTIME_ERR_REACTOR,  /* reactor initialization failed */
  OMNI_RUNTIME_ERR_LISTENER  /* listener initialization failed */
};

struct omni_runtime_result {
  enum omni_runtime_status status;
  int sys_errno; /* errno-style detail; zero for success */
  enum omni_runtime_state state; /* state after the operation */
};

/*
 * Caller-owned inputs for one bounded coordinator instance.
 *
 * The three subsystem objects must be inert before init. Their backing
 * arrays remain caller-owned and must stay alive until runtime_destroy has
 * returned. The capacities are independent fixed bounds; the coordinator
 * never allocates or resizes one subsystem to match another.
 *
 * listener_address is borrowed only during omni_runtime_init. Port zero is
 * passed through to the listener primitive for an ephemeral loopback port.
 */
struct omni_runtime_config {
  struct omni_listener *listener; /* borrowed object; initialized by runtime */
  struct omni_connection_registry *registry; /* borrowed object */
  struct omni_reactor *reactor; /* borrowed object */

  struct omni_connection_registry_slot *registry_slots; /* [registry_capacity] */
  size_t registry_capacity;

  struct pollfd *poller_fds; /* [poller_capacity] */
  uint64_t *poller_tokens; /* [poller_capacity] */
  size_t poller_capacity;

  struct omni_reactor_registration *reactor_registrations; /* [reactor_capacity] */
  struct omni_poller_event *reactor_events; /* [reactor_capacity] */
  size_t reactor_capacity;

  const char *listener_address; /* loopback IPv4 text, borrowed during init */
  uint16_t listener_port;       /* host order; zero requests an ephemeral port */
};

/*
 * Caller-owned coordinator context. The embedded poller is support state for
 * the borrowed reactor; its descriptor/token arrays come from the config.
 * Initialization flags are coordinator-owned bookkeeping, not additional
 * resource arrays.
 */
struct omni_runtime {
  struct omni_listener *listener;
  struct omni_connection_registry *registry;
  struct omni_reactor *reactor;
  struct omni_poller poller;
  enum omni_runtime_state state;
  bool registry_initialized;
  bool poller_initialized;
  bool reactor_initialized;
  bool listener_initialized;
};

/*
 * Canonicalize fresh or stopped caller-owned runtime storage to INERT. This
 * is not cleanup for INITIALIZED, RUNNING, or STOPPING storage: use
 * omni_runtime_stop or omni_runtime_destroy first. NULL is a safe no-op.
 */
void omni_runtime_make_inert(struct omni_runtime *runtime);

/*
 * Initialize the fixed subsystem graph in deterministic order:
 * registry, poller support, reactor, listener. No event wait or accept work
 * occurs. On any failure, every subsystem initialized by this call is
 * cleaned up in reverse order and the runtime returns to INERT.
 *
 * INERT and STOPPED runtimes may be initialized. INITIALIZED, RUNNING, and
 * STOPPING runtimes reject reinitialization without changing state.
 */
struct omni_runtime_result omni_runtime_init(struct omni_runtime *runtime,
                                             const struct omni_runtime_config *config);

/*
 * Enter RUNNING after successful initialization. This is a lifecycle marker
 * only: it does not start an event loop, wait for readiness, or accept a
 * socket. The fixed subsystem graph must still be live when called.
 */
struct omni_runtime_result omni_runtime_start(struct omni_runtime *runtime);

/*
 * Stop a RUNNING runtime synchronously. The sequence is STOPPING, reactor
 * registrations, registry metadata, poller support, listener, then STOPPED.
 * No connection is destroyed and no external connection descriptor is
 * closed. Calling stop in any other state returns ERR_STATE.
 */
struct omni_runtime_result omni_runtime_stop(struct omni_runtime *runtime);

/*
 * Idempotent teardown for a partially or fully initialized runtime. An
 * INITIALIZED runtime may be destroyed without start; a RUNNING runtime is
 * stopped as part of destruction. INERT and STOPPED runtimes are unchanged.
 */
void omni_runtime_destroy(struct omni_runtime *runtime);

/* NULL reports INERT. */
enum omni_runtime_state omni_runtime_state(const struct omni_runtime *runtime);

#endif /* OMNIROUTE_RUNTIME_H */
