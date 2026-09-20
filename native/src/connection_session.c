/*
 * OmniRoute native backend — bounded connection/session integration (Task 026).
 *
 * This file coordinates the connection ownership model with the bounded
 * connection I/O state machine without introducing protocol behavior.
 * No heap allocation, no descriptor close, no registry/poller management,
 * no automatic read/write or retry loops.
 */

#include "omniroute/connection_session.h"

#include <errno.h>

static struct omni_connection_session_result make_result(
    enum omni_connection_session_status status, int err) {
  struct omni_connection_session_result out;
  out.status = status;
  out.sys_errno = err;
  return out;
}

static void mark_inert_fields(struct omni_connection_session *session) {
  struct omni_connection_io empty_io = { 0 };
  omni_connection_io_make_inert(&empty_io);
  session->connection = NULL;
  session->io = empty_io;
  session->receive_storage = NULL;
  session->receive_capacity = 0u;
  session->send_storage = NULL;
  session->send_capacity = 0u;
  session->state = OMNI_CONNECTION_SESSION_NEW;
}

void omni_connection_session_make_inert(struct omni_connection_session *session) {
  if (session == NULL) {
    return;
  }
  mark_inert_fields(session);
}

struct omni_connection_session_result omni_connection_session_init(
    struct omni_connection_session *session,
    const struct omni_connection_session_config *config) {
  if (session == NULL || config == NULL) {
    return make_result(OMNI_CONNECTION_SESSION_ERR_INVALID, EINVAL);
  }
  if (session->state != OMNI_CONNECTION_SESSION_NEW) {
    return make_result(OMNI_CONNECTION_SESSION_ERR_STATE, EINVAL);
  }
  if (config->connection == NULL) {
    mark_inert_fields(session);
    return make_result(OMNI_CONNECTION_SESSION_ERR_INVALID, EINVAL);
  }
  if (omni_connection_state(config->connection) != OMNI_CONNECTION_OPEN) {
    mark_inert_fields(session);
    return make_result(OMNI_CONNECTION_SESSION_ERR_INVALID, EINVAL);
  }
  if (!omni_connection_is_live(config->connection)) {
    mark_inert_fields(session);
    return make_result(OMNI_CONNECTION_SESSION_ERR_INVALID, EINVAL);
  }
  if (config->receive_storage == NULL || config->receive_capacity == 0u) {
    mark_inert_fields(session);
    return make_result(OMNI_CONNECTION_SESSION_ERR_INVALID, EINVAL);
  }
  if (config->send_storage == NULL || config->send_capacity == 0u) {
    mark_inert_fields(session);
    return make_result(OMNI_CONNECTION_SESSION_ERR_INVALID, EINVAL);
  }

  /* Ensure io is in NEW before we claim INIT, so open can init it cleanly. */
  omni_connection_io_make_inert(&session->io);
  session->connection = config->connection;
  session->receive_storage = config->receive_storage;
  session->receive_capacity = config->receive_capacity;
  session->send_storage = config->send_storage;
  session->send_capacity = config->send_capacity;
  session->state = OMNI_CONNECTION_SESSION_INIT;
  return make_result(OMNI_CONNECTION_SESSION_OK, 0);
}

struct omni_connection_session_result omni_connection_session_open(
    struct omni_connection_session *session) {
  struct omni_connection_io_config io_config = { 0 };
  struct omni_connection_io_result io_result;

  if (session == NULL) {
    return make_result(OMNI_CONNECTION_SESSION_ERR_INVALID, EINVAL);
  }
  if (session->state != OMNI_CONNECTION_SESSION_INIT) {
    if (session->state == OMNI_CONNECTION_SESSION_CLOSING ||
        session->state == OMNI_CONNECTION_SESSION_CLOSED) {
      return make_result(OMNI_CONNECTION_SESSION_ERR_CLOSED, 0);
    }
    return make_result(OMNI_CONNECTION_SESSION_ERR_STATE, EINVAL);
  }
  if (session->connection == NULL ||
      omni_connection_state(session->connection) != OMNI_CONNECTION_OPEN ||
      !omni_connection_is_live(session->connection)) {
    return make_result(OMNI_CONNECTION_SESSION_ERR_INVALID, EINVAL);
  }
  if (session->receive_storage == NULL || session->receive_capacity == 0u ||
      session->send_storage == NULL || session->send_capacity == 0u) {
    return make_result(OMNI_CONNECTION_SESSION_ERR_INVALID, EINVAL);
  }

  /* Borrow the connection's accepted owner for the io. */
  io_config.accepted = &session->connection->accepted;
  io_config.receive_storage = session->receive_storage;
  io_config.receive_capacity = session->receive_capacity;
  io_config.send_storage = session->send_storage;
  io_config.send_capacity = session->send_capacity;

  /* io is NEW here, so init should succeed with valid inputs. */
  io_result = omni_connection_io_init(&session->io, &io_config);
  if (io_result.status != OMNI_CONNECTION_IO_OK) {
    /* io_init leaves io in NEW on failure; keep session in INIT for retry/destroy. */
    return make_result(OMNI_CONNECTION_SESSION_ERR_INVALID, io_result.sys_errno);
  }

  session->state = OMNI_CONNECTION_SESSION_OPEN;
  return make_result(OMNI_CONNECTION_SESSION_OK, 0);
}

struct omni_connection_session_result omni_connection_session_close(
    struct omni_connection_session *session) {
  struct omni_connection_io_result io_result;

  if (session == NULL) {
    return make_result(OMNI_CONNECTION_SESSION_ERR_INVALID, EINVAL);
  }
  if (session->state == OMNI_CONNECTION_SESSION_NEW ||
      session->state == OMNI_CONNECTION_SESSION_INIT) {
    return make_result(OMNI_CONNECTION_SESSION_ERR_STATE, EINVAL);
  }
  if (session->state == OMNI_CONNECTION_SESSION_CLOSING ||
      session->state == OMNI_CONNECTION_SESSION_CLOSED) {
    return make_result(OMNI_CONNECTION_SESSION_OK, 0);
  }
  if (session->state != OMNI_CONNECTION_SESSION_OPEN) {
    return make_result(OMNI_CONNECTION_SESSION_ERR_STATE, EINVAL);
  }

  io_result = omni_connection_io_close(&session->io);
  if (io_result.status == OMNI_CONNECTION_IO_OK) {
    session->state = OMNI_CONNECTION_SESSION_CLOSING;
    return make_result(OMNI_CONNECTION_SESSION_OK, 0);
  }
  /* Propagate state errors if io close is rejected. */
  if (io_result.status == OMNI_CONNECTION_IO_ERR_STATE) {
    return make_result(OMNI_CONNECTION_SESSION_ERR_STATE, io_result.sys_errno);
  }
  return make_result(OMNI_CONNECTION_SESSION_ERR_INVALID, io_result.sys_errno);
}

void omni_connection_session_destroy(struct omni_connection_session *session) {
  if (session == NULL) {
    return;
  }
  if (session->state == OMNI_CONNECTION_SESSION_CLOSED) {
    return;
  }
  if (session->state == OMNI_CONNECTION_SESSION_NEW) {
    /* No io resources to release, but canonicalize to CLOSED. */
    mark_inert_fields(session);
    session->state = OMNI_CONNECTION_SESSION_CLOSED;
    return;
  }
  /* For INIT, io is still NEW, so destroy is safe (goes to CLOSED).
   * For OPEN/CLOSING, io is OPEN/CLOSING and destroy releases both bytebufs. */
  omni_connection_io_destroy(&session->io);
  session->connection = NULL;
  session->receive_storage = NULL;
  session->receive_capacity = 0u;
  session->send_storage = NULL;
  session->send_capacity = 0u;
  session->state = OMNI_CONNECTION_SESSION_CLOSED;
}

enum omni_connection_session_state omni_connection_session_state(
    const struct omni_connection_session *session) {
  if (session == NULL) {
    return OMNI_CONNECTION_SESSION_NEW;
  }
  return session->state;
}

bool omni_connection_session_is_open(const struct omni_connection_session *session) {
  return session != NULL && session->state == OMNI_CONNECTION_SESSION_OPEN;
}

bool omni_connection_session_is_closed(const struct omni_connection_session *session) {
  return session != NULL && session->state == OMNI_CONNECTION_SESSION_CLOSED;
}

struct omni_connection *omni_connection_session_connection(
    struct omni_connection_session *session) {
  if (session == NULL) {
    return NULL;
  }
  if (session->state == OMNI_CONNECTION_SESSION_NEW ||
      session->state == OMNI_CONNECTION_SESSION_CLOSED) {
    return NULL;
  }
  return session->connection;
}

struct omni_connection_io *omni_connection_session_io(
    struct omni_connection_session *session) {
  if (session == NULL) {
    return NULL;
  }
  if (session->state != OMNI_CONNECTION_SESSION_OPEN &&
      session->state != OMNI_CONNECTION_SESSION_CLOSING) {
    return NULL;
  }
  return &session->io;
}

enum omni_connection_io_state omni_connection_session_io_state(
    const struct omni_connection_session *session) {
  if (session == NULL) {
    return OMNI_CONNECTION_IO_NEW;
  }
  return omni_connection_io_state(&session->io);
}

struct omni_bytebuf *omni_connection_session_receive_buffer(
    struct omni_connection_session *session) {
  if (session == NULL || session->state != OMNI_CONNECTION_SESSION_OPEN) {
    return NULL;
  }
  return omni_connection_io_receive_buffer(&session->io);
}

struct omni_bytebuf *omni_connection_session_send_buffer(
    struct omni_connection_session *session) {
  if (session == NULL || session->state != OMNI_CONNECTION_SESSION_OPEN) {
    return NULL;
  }
  return omni_connection_io_send_buffer(&session->io);
}

int omni_connection_session_fd(const struct omni_connection_session *session) {
  if (session == NULL || session->state != OMNI_CONNECTION_SESSION_OPEN) {
    return OMNI_ACCEPTED_FD_INVALID;
  }
  return omni_connection_io_fd(&session->io);
}

struct omni_connection_io_result omni_connection_session_readable(
    struct omni_connection_session *session) {
  struct omni_connection_io_result out;
  out.status = OMNI_CONNECTION_IO_ERR_INVALID;
  out.sys_errno = EINVAL;
  out.count = 0u;
  if (session == NULL) {
    return out;
  }
  if (session->state == OMNI_CONNECTION_SESSION_CLOSING ||
      session->state == OMNI_CONNECTION_SESSION_CLOSED) {
    out.status = OMNI_CONNECTION_IO_ERR_CLOSED;
    out.sys_errno = 0;
    return out;
  }
  if (session->state != OMNI_CONNECTION_SESSION_OPEN) {
    out.status = OMNI_CONNECTION_IO_ERR_STATE;
    out.sys_errno = EINVAL;
    return out;
  }
  return omni_connection_io_readable(&session->io);
}

struct omni_connection_io_result omni_connection_session_writable(
    struct omni_connection_session *session) {
  struct omni_connection_io_result out;
  out.status = OMNI_CONNECTION_IO_ERR_INVALID;
  out.sys_errno = EINVAL;
  out.count = 0u;
  if (session == NULL) {
    return out;
  }
  if (session->state == OMNI_CONNECTION_SESSION_CLOSING ||
      session->state == OMNI_CONNECTION_SESSION_CLOSED) {
    out.status = OMNI_CONNECTION_IO_ERR_CLOSED;
    out.sys_errno = 0;
    return out;
  }
  if (session->state != OMNI_CONNECTION_SESSION_OPEN) {
    out.status = OMNI_CONNECTION_IO_ERR_STATE;
    out.sys_errno = EINVAL;
    return out;
  }
  return omni_connection_io_writable(&session->io);
}
