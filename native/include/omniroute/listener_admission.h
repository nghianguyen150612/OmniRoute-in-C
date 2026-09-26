/*
 * OmniRoute native backend — bounded listener readiness/admission bridge
 * (Task 030).
 *
 * The bridge borrows one listener, reactor, and initialized connection
 * admission object. It registers the listener for READ readiness and invokes
 * exactly one finite connection_admission_drain() per READ callback. The
 * caller supplies both the attempt budget and identity output storage.
 *
 * The bridge owns no descriptor, connection, dependency, event-loop step, or
 * allocation. It is single-owner and externally synchronized. Stop removes
 * its reactor registration before the bridge context may be destroyed.
 */

#ifndef OMNIROUTE_LISTENER_ADMISSION_H
#define OMNIROUTE_LISTENER_ADMISSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omniroute/connection_admission.h"
#include "omniroute/listener.h"
#include "omniroute/reactor.h"

/* Reserved by one live listener-admission bridge in a reactor at a time. */
#define OMNI_LISTENER_ADMISSION_REACTOR_TOKEN UINT64_MAX

enum omni_listener_admission_state {
  OMNI_LISTENER_ADMISSION_INERT = 0,
  OMNI_LISTENER_ADMISSION_INITIALIZED,
  OMNI_LISTENER_ADMISSION_ACTIVE,
  OMNI_LISTENER_ADMISSION_STOPPED,
  OMNI_LISTENER_ADMISSION_CLOSED
};

enum omni_listener_admission_status {
  OMNI_LISTENER_ADMISSION_OK = 0,
  OMNI_LISTENER_ADMISSION_ERR_INVALID,
  OMNI_LISTENER_ADMISSION_ERR_STATE,
  OMNI_LISTENER_ADMISSION_ERR_REACTOR_REGISTER,
  OMNI_LISTENER_ADMISSION_ERR_REACTOR_REMOVE,
  OMNI_LISTENER_ADMISSION_LISTENER_ERROR,
  OMNI_LISTENER_ADMISSION_ADMISSION_DRAINED,
  OMNI_LISTENER_ADMISSION_ADMISSION_LIMIT_REACHED,
  OMNI_LISTENER_ADMISSION_ADMISSION_CAPACITY,
  OMNI_LISTENER_ADMISSION_ADMISSION_INTERRUPTED,
  OMNI_LISTENER_ADMISSION_ADMISSION_TRANSIENT,
  OMNI_LISTENER_ADMISSION_ADMISSION_STOPPED,
  OMNI_LISTENER_ADMISSION_ADMISSION_FAILURE
};

struct omni_listener_admission_result {
  enum omni_listener_admission_status status;
  int sys_errno;
  enum omni_listener_admission_state state;
  uint32_t readiness_events;
  enum omni_reactor_status reactor_status;
  bool admission_attempted;
  struct omni_connection_admission_result admission;
};

struct omni_listener_admission_config {
  const struct omni_listener *listener; /* borrowed, must remain live */
  struct omni_reactor *reactor; /* borrowed, must remain live */
  struct omni_connection_admission *admission; /* borrowed, must remain usable */
  size_t max_admissions_per_dispatch; /* finite, nonzero attempt budget */
  struct omni_connection_admission_identity *identities; /* caller-owned, disjoint storage */
  size_t identity_capacity; /* must be >= max_admissions_per_dispatch */
};

struct omni_listener_admission {
  const struct omni_listener *listener;
  struct omni_reactor *reactor;
  struct omni_connection_admission *admission;
  struct omni_connection_admission_identity *identities;
  size_t identity_capacity;
  size_t max_admissions_per_dispatch;
  enum omni_listener_admission_state state;
  uint64_t dispatches;
  uint64_t total_admitted;
  struct omni_listener_admission_result last_result;
};

/* Canonicalize zero-initialized fresh, STOPPED, or CLOSED storage. NULL-safe.
 * INITIALIZED and ACTIVE objects require destroy/stop before reset.
 */
void omni_listener_admission_make_inert(struct omni_listener_admission *bridge);

/* Validate and borrow live dependencies plus sufficient fixed identity output.
 * Keep dependencies and the disjoint identity array alive until stop or
 * destroy has returned.
 */
struct omni_listener_admission_result omni_listener_admission_init(
    struct omni_listener_admission *bridge,
    const struct omni_listener_admission_config *config);

/* Register the listener FD for READ exactly once. */
struct omni_listener_admission_result omni_listener_admission_start(
    struct omni_listener_admission *bridge);

/* Unregister the listener token. Existing admissions remain live. */
struct omni_listener_admission_result omni_listener_admission_stop(
    struct omni_listener_admission *bridge);

/* Stop first if needed, then clear borrowed references; never destroys them. */
struct omni_listener_admission_result omni_listener_admission_destroy(
    struct omni_listener_admission *bridge);

/* NULL reports INERT. */
enum omni_listener_admission_state omni_listener_admission_state(
    const struct omni_listener_admission *bridge);

/* Last lifecycle or readiness result by value; NULL returns ERR_INVALID. */
struct omni_listener_admission_result omni_listener_admission_last_result(
    const struct omni_listener_admission *bridge);

/* Saturating callback and successful-admission counters. */
uint64_t omni_listener_admission_dispatches(
    const struct omni_listener_admission *bridge);
uint64_t omni_listener_admission_total_admitted(
    const struct omni_listener_admission *bridge);

#endif /* OMNIROUTE_LISTENER_ADMISSION_H */
