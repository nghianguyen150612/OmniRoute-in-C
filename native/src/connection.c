/*
 * OmniRoute native backend — protocol-agnostic connection owner.
 *
 * This file deliberately contains no socket syscall. It composes the
 * accepted owner, bounded byte buffer, receive primitive, and send primitive
 * while keeping the lifecycle and ownership transition in one place.
 *
 * Memory audit: init borrows caller storage and performs no allocation;
 * destroy delegates to the byte buffer, whose borrowed backing is never
 * freed. The embedded accepted owner is the sole FD owner after adoption and
 * performs the single close through omni_accepted_destroy. No queue, output
 * storage, retained send span, or dynamic growth exists here.
 *
 * Transition audit: only INERT -> READY -> OPEN -> CLOSING -> CLOSED is
 * published. Failed preparation stays INERT; failed adoption stays READY and
 * leaves the source accepted owner unchanged. EOF and payload failures are
 * deliberately not lifecycle transitions — the caller owns that policy.
 */

#include "omniroute/connection.h"

#include <errno.h>

static void mark_inert_fields(struct omni_connection *connection) {
  struct omni_bytebuf empty_buffer = { 0 };

  omni_accepted_make_inert(&connection->accepted);
  connection->receive = empty_buffer;
  connection->poller_token = 0u;
  connection->poller_interests = 0u;
  connection->state = OMNI_CONNECTION_INERT;
}

static struct omni_connection_result make_result(enum omni_connection_status status, int err) {
  struct omni_connection_result result;

  result.status = status;
  result.sys_errno = err;
  return result;
}

static bool interests_valid(uint32_t interests) {
  if (interests == 0u) {
    return false;
  }
  return (interests & ~(OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE)) == 0u;
}

static bool io_ready(const struct omni_connection *connection) {
  return connection != NULL && connection->state == OMNI_CONNECTION_OPEN &&
         omni_accepted_is_live(&connection->accepted) &&
         omni_bytebuf_capacity(&connection->receive) != 0u;
}

static struct omni_recv_result invalid_recv_result(void) {
  struct omni_recv_result result;

  result.status = OMNI_RECV_ERR_INVALID;
  result.sys_errno = EINVAL;
  result.received = 0u;
  return result;
}

static struct omni_send_result invalid_send_result(void) {
  struct omni_send_result result;

  result.status = OMNI_SEND_ERR_INVALID;
  result.sys_errno = EINVAL;
  result.sent = 0u;
  return result;
}

void omni_connection_make_inert(struct omni_connection *connection) {
  if (connection == NULL) {
    return;
  }
  mark_inert_fields(connection);
}

struct omni_connection_result omni_connection_init(
    struct omni_connection *connection, const struct omni_connection_config *config) {
  struct omni_bytebuf receive = { 0 };

  if (connection == NULL || config == NULL) {
    return make_result(OMNI_CONNECTION_ERR_INVALID, EINVAL);
  }
  if (connection->state != OMNI_CONNECTION_INERT) {
    return make_result(OMNI_CONNECTION_ERR_STATE, EINVAL);
  }
  if (config->receive_storage == NULL || config->receive_capacity == 0u ||
      !interests_valid(config->poller_interests)) {
    mark_inert_fields(connection);
    return make_result(OMNI_CONNECTION_ERR_INVALID, EINVAL);
  }
  /* Borrowed initialization cannot fail after the validation above. */
  if (!omni_bytebuf_init_borrowed(&receive, config->receive_storage,
                                  config->receive_capacity)) {
    mark_inert_fields(connection);
    return make_result(OMNI_CONNECTION_ERR_INVALID, EINVAL);
  }
  omni_accepted_make_inert(&connection->accepted);
  connection->receive = receive;
  connection->poller_token = config->poller_token;
  connection->poller_interests = config->poller_interests;
  connection->state = OMNI_CONNECTION_READY;
  return make_result(OMNI_CONNECTION_OK, 0);
}

struct omni_connection_result omni_connection_from_accepted(
    struct omni_connection *connection, struct omni_accepted *accepted) {
  int fd = OMNI_CONNECTION_FD_INVALID;

  if (connection == NULL || accepted == NULL) {
    return make_result(OMNI_CONNECTION_ERR_INVALID, EINVAL);
  }
  if (connection->state != OMNI_CONNECTION_READY) {
    return make_result(OMNI_CONNECTION_ERR_STATE, EINVAL);
  }
  if (!omni_accepted_is_live(accepted)) {
    return make_result(OMNI_CONNECTION_ERR_INVALID, EINVAL);
  }
  fd = omni_accepted_fd(accepted);
  if (fd == OMNI_CONNECTION_FD_INVALID || !connection->receive.live) {
    return make_result(OMNI_CONNECTION_ERR_INVALID, EINVAL);
  }
  /* Publish the destination owner before making the source inert. */
  connection->accepted = *accepted;
  omni_accepted_make_inert(accepted);
  connection->state = OMNI_CONNECTION_OPEN;
  return make_result(OMNI_CONNECTION_OK, 0);
}

struct omni_connection_result omni_connection_begin_close(struct omni_connection *connection) {
  if (connection == NULL) {
    return make_result(OMNI_CONNECTION_ERR_INVALID, EINVAL);
  }
  if (connection->state != OMNI_CONNECTION_OPEN) {
    return make_result(OMNI_CONNECTION_ERR_STATE, EINVAL);
  }
  connection->state = OMNI_CONNECTION_CLOSING;
  return make_result(OMNI_CONNECTION_OK, 0);
}

void omni_connection_destroy(struct omni_connection *connection) {
  if (connection == NULL || connection->state == OMNI_CONNECTION_CLOSED) {
    return;
  }
  if (connection->state == OMNI_CONNECTION_OPEN ||
      connection->state == OMNI_CONNECTION_CLOSING) {
    /* The buffer has no FD dependency; release its object before the owner. */
    omni_bytebuf_destroy(&connection->receive);
    omni_accepted_destroy(&connection->accepted);
  } else if (connection->state == OMNI_CONNECTION_READY) {
    omni_bytebuf_destroy(&connection->receive);
    omni_accepted_make_inert(&connection->accepted);
  } else {
    /* INERT carries no resource by construction. */
    omni_accepted_make_inert(&connection->accepted);
  }
  connection->receive.backing = NULL;
  connection->receive.capacity = 0u;
  connection->receive.read = 0u;
  connection->receive.write = 0u;
  connection->receive.high_water = 0u;
  connection->receive.owns_backing = false;
  connection->receive.live = false;
  connection->poller_token = 0u;
  connection->poller_interests = 0u;
  connection->state = OMNI_CONNECTION_CLOSED;
}

enum omni_connection_state omni_connection_state(const struct omni_connection *connection) {
  if (connection == NULL) {
    return OMNI_CONNECTION_INERT;
  }
  return connection->state;
}

bool omni_connection_is_live(const struct omni_connection *connection) {
  return connection != NULL &&
         (connection->state == OMNI_CONNECTION_OPEN ||
          connection->state == OMNI_CONNECTION_CLOSING) &&
         omni_accepted_is_live(&connection->accepted);
}

int omni_connection_fd(const struct omni_connection *connection) {
  if (!omni_connection_is_live(connection)) {
    return OMNI_CONNECTION_FD_INVALID;
  }
  return omni_accepted_fd(&connection->accepted);
}

uint64_t omni_connection_poller_token(const struct omni_connection *connection) {
  if (connection == NULL || connection->state == OMNI_CONNECTION_INERT ||
      connection->state == OMNI_CONNECTION_CLOSED) {
    return 0u;
  }
  return connection->poller_token;
}

uint32_t omni_connection_poller_interests(const struct omni_connection *connection) {
  if (connection == NULL || connection->state == OMNI_CONNECTION_INERT ||
      connection->state == OMNI_CONNECTION_CLOSED) {
    return 0u;
  }
  return connection->poller_interests;
}

struct omni_bytebuf *omni_connection_receive_buffer(struct omni_connection *connection) {
  if (!io_ready(connection)) {
    return NULL;
  }
  return &connection->receive;
}

struct omni_recv_result omni_connection_recv_once(struct omni_connection *connection) {
  if (!io_ready(connection)) {
    return invalid_recv_result();
  }
  return omni_recv_once(&connection->accepted, &connection->receive);
}

struct omni_recv_result omni_connection_recv_drain(struct omni_connection *connection,
                                                   size_t max_calls) {
  if (!io_ready(connection)) {
    return invalid_recv_result();
  }
  return omni_recv_drain(&connection->accepted, &connection->receive, max_calls);
}

struct omni_send_result omni_connection_send_once(const struct omni_connection *connection,
                                                  struct omni_send_span span) {
  if (!io_ready(connection)) {
    return invalid_send_result();
  }
  return omni_send_once(&connection->accepted, span);
}

struct omni_send_result omni_connection_send_drain(const struct omni_connection *connection,
                                                   struct omni_send_span span,
                                                   size_t max_calls) {
  if (!io_ready(connection)) {
    return invalid_send_result();
  }
  return omni_send_drain(&connection->accepted, span, max_calls);
}
