/*
 * OmniRoute native backend — bounded connection lifecycle policy (Task 032).
 *
 * This test-only policy consumes one already-produced Task 031 dispatch
 * result, synchronizes READ/WRITE interests, or releases a close-worthy
 * connection through Task 029 admission ownership. It owns no connection,
 * registration, descriptor, or borrowed dependency and keeps no per-connection
 * table. All work is synchronous, bounded by admission capacity, and
 * externally synchronized.
 */

#ifndef OMNIROUTE_CONNECTION_POLICY_H
#define OMNIROUTE_CONNECTION_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "omniroute/connection_admission.h"
#include "omniroute/connection_dispatch.h"

enum omni_connection_policy_state {
  OMNI_CONNECTION_POLICY_INERT = 0,
  OMNI_CONNECTION_POLICY_ACTIVE,
  OMNI_CONNECTION_POLICY_CLOSED
};

enum omni_connection_policy_status {
  OMNI_CONNECTION_POLICY_OK = 0,
  OMNI_CONNECTION_POLICY_IGNORED,
  OMNI_CONNECTION_POLICY_ERR_INVALID,
  OMNI_CONNECTION_POLICY_ERR_STATE,
  OMNI_CONNECTION_POLICY_ERR_DEPENDENCY,
  OMNI_CONNECTION_POLICY_ERR_INTEREST_UPDATE,
  OMNI_CONNECTION_POLICY_ERR_RELEASE
};

enum omni_connection_policy_action {
  OMNI_CONNECTION_POLICY_ACTION_NONE = 0,
  OMNI_CONNECTION_POLICY_ACTION_KEEP,
  OMNI_CONNECTION_POLICY_ACTION_UPDATE_INTERESTS,
  OMNI_CONNECTION_POLICY_ACTION_CLOSE,
  OMNI_CONNECTION_POLICY_ACTION_IGNORE,
  OMNI_CONNECTION_POLICY_ACTION_ERROR
};

struct omni_connection_policy_result {
  enum omni_connection_policy_status status;
  enum omni_connection_policy_action action;
  bool has_dispatch;
  struct omni_connection_dispatch_result dispatch;
  uint32_t old_interests;
  uint32_t desired_interests;
  uint32_t new_interests;
  bool interest_updated;
  bool connection_released;
  enum omni_connection_reactor_status reactor_status;
  enum omni_connection_admission_status release_status;
  enum omni_connection_manager_status release_manager_status;
  int sys_errno;
};

struct omni_connection_policy_config {
  struct omni_connection_admission *admission; /* borrowed; must outlive use */
  struct omni_connection_dispatch *dispatch;   /* borrowed; must outlive use */
};

struct omni_connection_policy {
  struct omni_connection_admission *admission; /* borrowed */
  struct omni_connection_dispatch *dispatch;   /* borrowed */
  enum omni_connection_policy_state state;
  uint64_t apply_calls;
  uint64_t sync_calls;
  uint64_t actions;
  uint64_t interest_updates;
  uint64_t closed_count;
  uint64_t ignored_count;
  uint64_t close_worthy_observations;
  uint64_t policy_errors;
  struct omni_connection_policy_result last_result;
};

/* Canonicalize fresh or CLOSED storage to INERT. NULL-safe. */
void omni_connection_policy_make_inert(struct omni_connection_policy *policy);

/* Borrow a live admission/dispatch pair. Duplicate or unavailable init fails. */
struct omni_connection_policy_result omni_connection_policy_init(
    struct omni_connection_policy *policy,
    const struct omni_connection_policy_config *config);

/*
 * Clear borrowed references only; never releases a connection. Repeat-safe.
 * Keep the policy storage alive until registrations using it as callback
 * context have been detached, even after destroy.
 */
void omni_connection_policy_destroy(struct omni_connection_policy *policy);

/* Apply one already-produced dispatch result. Does not step or dispatch I/O. */
struct omni_connection_policy_result omni_connection_policy_apply(
    struct omni_connection_policy *policy,
    const struct omni_connection_dispatch_result *dispatch_result);

/* Explicit post-buffer-append synchronization point, keyed by reactor token. */
struct omni_connection_policy_result omni_connection_policy_sync_interests(
    struct omni_connection_policy *policy, uint64_t token);

/*
 * Optional connection-reactor callback adapter: one dispatch, then one apply.
 * ERROR/HANGUP without INVALID causes one bounded READ attempt before policy.
 */
void omni_connection_policy_callback(struct omni_connection *connection,
                                     uint64_t token,
                                     uint32_t events,
                                     void *context);

enum omni_connection_policy_state omni_connection_policy_state(
    const struct omni_connection_policy *policy);
struct omni_connection_policy_result omni_connection_policy_last_result(
    const struct omni_connection_policy *policy);
uint64_t omni_connection_policy_actions(const struct omni_connection_policy *policy);
uint64_t omni_connection_policy_closed_count(const struct omni_connection_policy *policy);
uint64_t omni_connection_policy_interest_updates(const struct omni_connection_policy *policy);
uint64_t omni_connection_policy_ignored_count(const struct omni_connection_policy *policy);
uint64_t omni_connection_policy_close_worthy_observations(
    const struct omni_connection_policy *policy);
uint64_t omni_connection_policy_errors(const struct omni_connection_policy *policy);

#endif /* OMNIROUTE_CONNECTION_POLICY_H */
