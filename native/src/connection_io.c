/*
 * OmniRoute native backend — bounded connection I/O state machine (Task 025).
 *
 * This module implements the first connection I/O lifecycle layer. It owns:
 * - read readiness handling
 * - write readiness handling
 * - I/O state transitions
 * - bounded buffer interaction (receive and send)
 *
 * It does NOT own:
 * - the socket descriptor (borrowed from connection, follows existing connection ownership rules)
 * - protocol parsing or application logic
 * - registry entries, reactor registrations, or external callbacks
 */

#include "omniroute/connection_io.h"

#include <errno.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/types.h>

static struct omni_connection_io_result make_result(enum omni_connection_io_status status,
                                                    int err, size_t count) {
  struct omni_connection_io_result out;
  out.status = status;
  out.sys_errno = err;
  out.count = count;
  return out;
}

static void mark_inert_fields(struct omni_connection_io *io) {
  struct omni_bytebuf empty_buffer = { 0 };
  io->accepted = NULL;
  io->receive = empty_buffer;
  io->send = empty_buffer;
  io->state = OMNI_CONNECTION_IO_NEW;
}

void omni_connection_io_make_inert(struct omni_connection_io *io) {
  if (io == NULL) {
    return;
  }
  mark_inert_fields(io);
}

struct omni_connection_io_result omni_connection_io_init(
    struct omni_connection_io *io, const struct omni_connection_io_config *config) {
  struct omni_bytebuf receive = { 0 };
  struct omni_bytebuf send = { 0 };

  if (io == NULL || config == NULL) {
    return make_result(OMNI_CONNECTION_IO_ERR_INVALID, EINVAL, 0u);
  }
  if (io->state != OMNI_CONNECTION_IO_NEW) {
    return make_result(OMNI_CONNECTION_IO_ERR_STATE, EINVAL, 0u);
  }
  if (config->accepted == NULL || !omni_accepted_is_live(config->accepted)) {
    mark_inert_fields(io);
    return make_result(OMNI_CONNECTION_IO_ERR_INVALID, EINVAL, 0u);
  }
  if (config->receive_storage == NULL || config->receive_capacity == 0u) {
    mark_inert_fields(io);
    return make_result(OMNI_CONNECTION_IO_ERR_INVALID, EINVAL, 0u);
  }
  if (config->send_storage == NULL || config->send_capacity == 0u) {
    mark_inert_fields(io);
    return make_result(OMNI_CONNECTION_IO_ERR_INVALID, EINVAL, 0u);
  }

  if (!omni_bytebuf_init_borrowed(&receive, config->receive_storage,
                                  config->receive_capacity)) {
    mark_inert_fields(io);
    return make_result(OMNI_CONNECTION_IO_ERR_INVALID, EINVAL, 0u);
  }
  if (!omni_bytebuf_init_borrowed(&send, config->send_storage,
                                  config->send_capacity)) {
    mark_inert_fields(io);
    return make_result(OMNI_CONNECTION_IO_ERR_INVALID, EINVAL, 0u);
  }

  io->accepted = config->accepted;
  io->receive = receive;
  io->send = send;
  io->state = OMNI_CONNECTION_IO_OPEN;
  return make_result(OMNI_CONNECTION_IO_OK, 0, 0u);
}

static bool io_ready(const struct omni_connection_io *io) {
  return io != NULL && io->state == OMNI_CONNECTION_IO_OPEN &&
         io->accepted != NULL && omni_accepted_is_live(io->accepted);
}

struct omni_connection_io_result omni_connection_io_readable(struct omni_connection_io *io) {
  if (!io_ready(io)) {
    if (io != NULL && (io->state == OMNI_CONNECTION_IO_CLOSING ||
                       io->state == OMNI_CONNECTION_IO_CLOSED)) {
      return make_result(OMNI_CONNECTION_IO_ERR_CLOSED, 0, 0u);
    }
    return make_result(OMNI_CONNECTION_IO_ERR_INVALID, EINVAL, 0u);
  }

  io->state = OMNI_CONNECTION_IO_READABLE;

  struct omni_recv_result recv_result = omni_recv_once(io->accepted, &io->receive);

  io->state = OMNI_CONNECTION_IO_OPEN;

  switch (recv_result.status) {
    case OMNI_RECV_DATA:
      return make_result(OMNI_CONNECTION_IO_OK, 0, recv_result.received);
    case OMNI_RECV_WOULD_BLOCK:
      return make_result(OMNI_CONNECTION_IO_ERR_WOULD_BLOCK,
                         recv_result.sys_errno, 0u);
    case OMNI_RECV_EOF:
      return make_result(OMNI_CONNECTION_IO_ERR_EOF, 0, 0u);
    case OMNI_RECV_INTERRUPTED:
      return make_result(OMNI_CONNECTION_IO_ERR_INTERRUPTED,
                         recv_result.sys_errno, 0u);
    case OMNI_RECV_BUFFER_FULL:
      return make_result(OMNI_CONNECTION_IO_ERR_BUFFER_FULL, 0, 0u);
    case OMNI_RECV_LIMIT_REACHED:
      return make_result(OMNI_CONNECTION_IO_OK, 0, recv_result.received);
    case OMNI_RECV_ERR_INVALID:
      return make_result(OMNI_CONNECTION_IO_ERR_INVALID, EINVAL, 0u);
    case OMNI_RECV_ERR_FATAL:
      return make_result(OMNI_CONNECTION_IO_ERR_IO,
                         recv_result.sys_errno, 0u);
    case OMNI_RECV_ERR_INTERNAL:
      return make_result(OMNI_CONNECTION_IO_ERR_IO, EIO, 0u);
    default:
      return make_result(OMNI_CONNECTION_IO_ERR_IO, EIO, 0u);
  }
}

struct omni_connection_io_result omni_connection_io_writable(struct omni_connection_io *io) {
  if (!io_ready(io)) {
    if (io != NULL && (io->state == OMNI_CONNECTION_IO_CLOSING ||
                       io->state == OMNI_CONNECTION_IO_CLOSED)) {
      return make_result(OMNI_CONNECTION_IO_ERR_CLOSED, 0, 0u);
    }
    return make_result(OMNI_CONNECTION_IO_ERR_INVALID, EINVAL, 0u);
  }

  size_t readable = omni_bytebuf_readable(&io->send);
  if (readable == 0u) {
    return make_result(OMNI_CONNECTION_IO_OK, 0, 0u);
  }

  io->state = OMNI_CONNECTION_IO_WRITABLE;

  const unsigned char *data = omni_bytebuf_read_ptr(&io->send, &readable);
  struct omni_send_span span = { data, readable };
  struct omni_send_result send_result = omni_send_once(io->accepted, span);

  if (send_result.status == OMNI_SEND_PROGRESS) {
    bool consumed = omni_bytebuf_consume(&io->send, send_result.sent);
    (void)consumed;
  }

  io->state = OMNI_CONNECTION_IO_OPEN;

  switch (send_result.status) {
    case OMNI_SEND_PROGRESS:
      return make_result(OMNI_CONNECTION_IO_OK, 0, send_result.sent);
    case OMNI_SEND_COMPLETE:
      return make_result(OMNI_CONNECTION_IO_OK, 0, send_result.sent);
    case OMNI_SEND_WOULD_BLOCK:
      return make_result(OMNI_CONNECTION_IO_ERR_WOULD_BLOCK,
                         send_result.sys_errno, send_result.sent);
    case OMNI_SEND_INTERRUPTED:
      return make_result(OMNI_CONNECTION_IO_ERR_INTERRUPTED,
                         send_result.sys_errno, send_result.sent);
    case OMNI_SEND_PEER_CLOSED:
      return make_result(OMNI_CONNECTION_IO_ERR_IO,
                         send_result.sys_errno, send_result.sent);
    case OMNI_SEND_ERR_ZERO_PROGRESS:
      return make_result(OMNI_CONNECTION_IO_ERR_IO, EIO, 0u);
    case OMNI_SEND_LIMIT_REACHED:
      return make_result(OMNI_CONNECTION_IO_OK, 0, send_result.sent);
    case OMNI_SEND_ERR_INVALID:
      return make_result(OMNI_CONNECTION_IO_ERR_INVALID, EINVAL, 0u);
    case OMNI_SEND_ERR_FATAL:
      return make_result(OMNI_CONNECTION_IO_ERR_IO,
                         send_result.sys_errno, send_result.sent);
    case OMNI_SEND_ERR_INTERNAL:
      return make_result(OMNI_CONNECTION_IO_ERR_IO, EIO, 0u);
    default:
      return make_result(OMNI_CONNECTION_IO_ERR_IO, EIO, 0u);
  }
}

struct omni_connection_io_result omni_connection_io_close(struct omni_connection_io *io) {
  if (io == NULL) {
    return make_result(OMNI_CONNECTION_IO_ERR_INVALID, EINVAL, 0u);
  }
  if (io->state == OMNI_CONNECTION_IO_NEW) {
    return make_result(OMNI_CONNECTION_IO_ERR_STATE, EINVAL, 0u);
  }
  if (io->state == OMNI_CONNECTION_IO_CLOSING ||
      io->state == OMNI_CONNECTION_IO_CLOSED) {
    return make_result(OMNI_CONNECTION_IO_OK, 0, 0u);
  }
  if (io->state != OMNI_CONNECTION_IO_OPEN &&
      io->state != OMNI_CONNECTION_IO_READABLE &&
      io->state != OMNI_CONNECTION_IO_WRITABLE) {
    return make_result(OMNI_CONNECTION_IO_ERR_STATE, EINVAL, 0u);
  }
  io->state = OMNI_CONNECTION_IO_CLOSING;
  return make_result(OMNI_CONNECTION_IO_OK, 0, 0u);
}

void omni_connection_io_destroy(struct omni_connection_io *io) {
  if (io == NULL || io->state == OMNI_CONNECTION_IO_CLOSED) {
    return;
  }
  if (io->state == OMNI_CONNECTION_IO_OPEN ||
      io->state == OMNI_CONNECTION_IO_READABLE ||
      io->state == OMNI_CONNECTION_IO_WRITABLE ||
      io->state == OMNI_CONNECTION_IO_CLOSING) {
    omni_bytebuf_destroy(&io->receive);
    omni_bytebuf_destroy(&io->send);
  } else {
    /* NEW carries no resource by construction. */
  }
  io->accepted = NULL;
  io->receive.backing = NULL;
  io->receive.capacity = 0u;
  io->receive.read = 0u;
  io->receive.write = 0u;
  io->receive.high_water = 0u;
  io->receive.owns_backing = false;
  io->receive.live = false;
  io->send.backing = NULL;
  io->send.capacity = 0u;
  io->send.read = 0u;
  io->send.write = 0u;
  io->send.high_water = 0u;
  io->send.owns_backing = false;
  io->send.live = false;
  io->state = OMNI_CONNECTION_IO_CLOSED;
}

enum omni_connection_io_state omni_connection_io_state(const struct omni_connection_io *io) {
  if (io == NULL) {
    return OMNI_CONNECTION_IO_NEW;
  }
  return io->state;
}

bool omni_connection_io_is_open(const struct omni_connection_io *io) {
  return io != NULL && io->state == OMNI_CONNECTION_IO_OPEN;
}

struct omni_bytebuf *omni_connection_io_receive_buffer(struct omni_connection_io *io) {
  if (io == NULL || (io->state != OMNI_CONNECTION_IO_OPEN &&
                     io->state != OMNI_CONNECTION_IO_READABLE)) {
    return NULL;
  }
  return &io->receive;
}

struct omni_bytebuf *omni_connection_io_send_buffer(struct omni_connection_io *io) {
  if (io == NULL || (io->state != OMNI_CONNECTION_IO_OPEN &&
                     io->state != OMNI_CONNECTION_IO_WRITABLE)) {
    return NULL;
  }
  return &io->send;
}

int omni_connection_io_fd(const struct omni_connection_io *io) {
  if (!io_ready(io)) {
    return OMNI_ACCEPTED_FD_INVALID;
  }
  return omni_accepted_fd(io->accepted);
}