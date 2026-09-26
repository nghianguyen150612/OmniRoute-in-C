/*
 * OmniRoute native backend — bounded managed connection readiness dispatch
 * bridge (Task 031).
 *
 * The bridge borrows a live connection manager. It validates callback
 * membership and generation token, resolves the session through the existing
 * runtime entry, then delegates readiness to the session's bounded I/O
 * methods. It owns only local counters and the last fixed-size result.
 *
 * One callback processes at most one READ before at most one WRITE. ERROR,
 * HANGUP, and INVALID are recorded before payload readiness; INVALID forbids
 * ordinary I/O for that callback. EOF and fatal I/O are surfaced and never
 * trigger manager removal, connection/session destruction, or descriptor
 * closure. Reactor interests are not changed here.
 *
 * The bridge is single-owner and externally synchronized. Its storage must
 * remain alive while any connection-reactor registration can still invoke
 * the borrowed callback context, including after bridge destruction.
 */

#ifndef OMNIROUTE_CONNECTION_DISPATCH_H
#define OMNIROUTE_CONNECTION_DISPATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "omniroute/connection_manager.h"
#include "omniroute/connection_reactor.h"
#include "omniroute/connection_session.h"

enum omni_connection_dispatch_state {
  OMNI_CONNECTION_DISPATCH_INERT = 0,
  OMNI_CONNECTION_DISPATCH_ACTIVE,
  OMNI_CONNECTION_DISPATCH_CLOSED
};

enum omni_connection_dispatch_status {
  OMNI_CONNECTION_DISPATCH_OK = 0,
  OMNI_CONNECTION_DISPATCH_IGNORED,
  OMNI_CONNECTION_DISPATCH_STALE,
  OMNI_CONNECTION_DISPATCH_INVALID_EVENT,
  OMNI_CONNECTION_DISPATCH_ERR_INVALID,
  OMNI_CONNECTION_DISPATCH_ERR_STATE
};

struct omni_connection_dispatch_result {
  enum omni_connection_dispatch_status status;
  uint32_t readiness_events;
  uint64_t token;
  struct omni_connection_io_result read_result;
  struct omni_connection_io_result write_result;
  uint64_t bytes_read;
  uint64_t bytes_written;
  bool read_attempted;
  bool write_attempted;
  bool ignored;
  bool stale;
  bool error_observed;
  bool hangup_observed;
  bool invalid_observed;
  bool close_worthy;
};

struct omni_connection_dispatch_config {
  struct omni_connection_manager *manager; /* borrowed; must outlive use */
};

struct omni_connection_dispatch {
  struct omni_connection_manager *manager;
  enum omni_connection_dispatch_state state;
  uint64_t dispatches;
  uint64_t ignored_dispatches;
  uint64_t stale_dispatches;
  uint64_t read_dispatches;
  uint64_t write_dispatches;
  uint64_t error_observations;
  uint64_t bytes_read;
  uint64_t bytes_written;
  struct omni_connection_dispatch_result last_result;
};

/* Canonicalize fresh or CLOSED storage to INERT. NULL-safe. */
void omni_connection_dispatch_make_inert(struct omni_connection_dispatch *bridge);

/* Borrow a live manager. Duplicate init and unavailable managers are rejected. */
struct omni_connection_dispatch_result omni_connection_dispatch_init(
    struct omni_connection_dispatch *bridge,
    const struct omni_connection_dispatch_config *config);

/* Clear borrowed references; never changes manager, session, connection or reactor. */
void omni_connection_dispatch_destroy(struct omni_connection_dispatch *bridge);

/* Callback shape accepted directly by omni_connection_reactor_init(). */
void omni_connection_dispatch_callback(struct omni_connection *connection,
                                       uint64_t token,
                                       uint32_t events,
                                       void *context);

enum omni_connection_dispatch_state omni_connection_dispatch_state(
    const struct omni_connection_dispatch *bridge);
struct omni_connection_dispatch_result omni_connection_dispatch_last_result(
    const struct omni_connection_dispatch *bridge);
uint64_t omni_connection_dispatch_dispatches(const struct omni_connection_dispatch *bridge);
uint64_t omni_connection_dispatch_ignored_dispatches(
    const struct omni_connection_dispatch *bridge);
uint64_t omni_connection_dispatch_stale_dispatches(
    const struct omni_connection_dispatch *bridge);
uint64_t omni_connection_dispatch_read_dispatches(
    const struct omni_connection_dispatch *bridge);
uint64_t omni_connection_dispatch_write_dispatches(
    const struct omni_connection_dispatch *bridge);
uint64_t omni_connection_dispatch_error_observations(
    const struct omni_connection_dispatch *bridge);
uint64_t omni_connection_dispatch_bytes_read(const struct omni_connection_dispatch *bridge);
uint64_t omni_connection_dispatch_bytes_written(const struct omni_connection_dispatch *bridge);

#endif /* OMNIROUTE_CONNECTION_DISPATCH_H */
