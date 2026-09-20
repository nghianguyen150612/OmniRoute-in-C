/*
 * OmniRoute native backend — bounded connection I/O state machine (Task 025).
 *
 * This module is the first connection I/O lifecycle layer. It owns:
 * - read readiness handling
 * - write readiness handling
 * - I/O state transitions
 * - bounded buffer interaction (receive and send)
 *
 * It does NOT own:
 * - the socket descriptor (borrowed from connection, follows existing connection ownership rules)
 * - protocol parsing or application logic
 * - registry entries, reactor registrations, or external callbacks
 *
 * Target architecture:
 *
 *   event_loop
 *         |
 *         v
 *   connection_reactor
 *         |
 *         v
 *   connection_io
 *         |
 *         +-- receive buffer (omni_bytebuf)
 *         |
 *         +-- send buffer (omni_bytebuf)
 *         |
 *         +-- socket read/write
 *
 * Relationship with connection:
 *
 *   connection
 *        |
 *        v
 *   connection_io
 *        |
 *        +-- omni_bytebuf receive
 *        |
 *        +-- omni_bytebuf send
 *        |
 *        +-- socket fd (borrowed)
 *
 * The I/O layer owns data movement only. It does not understand protocol messages.
 */

#ifndef OMNIROUTE_CONNECTION_IO_H
#define OMNIROUTE_CONNECTION_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omniroute/accepted.h"
#include "omniroute/bytebuf.h"
#include "omniroute/recv.h"
#include "omniroute/send.h"

/* I/O state machine states. */
enum omni_connection_io_state {
  OMNI_CONNECTION_IO_NEW = 0,      /* not initialized */
  OMNI_CONNECTION_IO_OPEN,         /* initialized, ready for I/O */
  OMNI_CONNECTION_IO_READABLE,     /* read readiness handling in progress */
  OMNI_CONNECTION_IO_WRITABLE,     /* write readiness handling in progress */
  OMNI_CONNECTION_IO_CLOSING,      /* close requested, draining */
  OMNI_CONNECTION_IO_CLOSED        /* all resources released */
};

/* I/O operation statuses. */
enum omni_connection_io_status {
  OMNI_CONNECTION_IO_OK = 0,           /* operation completed successfully */
  OMNI_CONNECTION_IO_ERR_INVALID,      /* NULL args, bad storage, bad fd, zero capacity */
  OMNI_CONNECTION_IO_ERR_STATE,        /* invalid lifecycle transition */
  OMNI_CONNECTION_IO_ERR_CLOSED,       /* I/O attempted on closed connection */
  OMNI_CONNECTION_IO_ERR_BUFFER_FULL,  /* receive buffer full, no kernel wait performed */
  OMNI_CONNECTION_IO_ERR_WOULD_BLOCK,  /* socket would block: normal control flow */
  OMNI_CONNECTION_IO_ERR_INTERRUPTED,  /* interrupted syscall: nothing committed */
  OMNI_CONNECTION_IO_ERR_EOF,          /* orderly peer shutdown */
  OMNI_CONNECTION_IO_ERR_IO            /* fatal socket error, errno preserved */
};

/* Result structure for I/O operations. */
struct omni_connection_io_result {
  enum omni_connection_io_status status;
  int sys_errno;  /* errno at the stop point; 0 on success */
  size_t count;   /* bytes read or written */
};

/* Configuration for initialization. */
struct omni_connection_io_config {
  struct omni_accepted *accepted;  /* borrowed accepted owner; caller keeps ownership */
  void *receive_storage;           /* caller-owned receive buffer backing; retained until destroy */
  size_t receive_capacity;         /* finite, nonzero hard receive cap */
  void *send_storage;              /* caller-owned send buffer backing; retained until destroy */
  size_t send_capacity;            /* finite, nonzero hard send cap */
};

/* Connection I/O object. */
struct omni_connection_io {
  struct omni_accepted *accepted;  /* borrowed; caller owns lifecycle */
  struct omni_bytebuf receive;     /* connection-local receive buffer; borrowed backing */
  struct omni_bytebuf send;        /* connection-local send buffer; borrowed backing */
  enum omni_connection_io_state state;
};

/*
 * Canonicalize fresh caller-owned connection I/O storage to NEW without
 * touching any descriptor or freeing anything. NULL-safe. Call this before
 * the first init when the struct is not statically initialized. It is also
 * safe for an already NEW or CLOSED object. It is not a cleanup operation
 * for OPEN, READABLE, WRITABLE, or CLOSING objects; destroy those first.
 */
void omni_connection_io_make_inert(struct omni_connection_io *io);

/*
 * Prepare bounded borrowed receive and send buffers, and bind the borrowed
 * accepted owner. Requires a NEW destination; transitions it to OPEN.
 * No heap allocation occurs. Invalid input leaves the destination NEW.
 * The caller must keep the accepted owner, receive storage, and send storage
 * alive until after destroy.
 */
struct omni_connection_io_result omni_connection_io_init(
    struct omni_connection_io *io, const struct omni_connection_io_config *config);

/*
 * Handle a read readiness event. Only valid in OPEN state.
 * Reads from the borrowed socket into the receive buffer.
 * Handles:
 *   - successful data receipt
 *   - EAGAIN/EWOULDBLOCK
 *   - EINTR
 *   - EOF
 *   - fatal error
 * Does not parse received bytes. State transitions: OPEN -> READABLE -> OPEN.
 */
struct omni_connection_io_result omni_connection_io_readable(struct omni_connection_io *io);

/*
 * Handle a write readiness event. Only valid in OPEN state.
 * Writes buffered data from the send buffer to the socket.
 * Consumes sent bytes through the bytebuf.
 * Handles:
 *   - full write
 *   - partial write
 *   - EAGAIN/EWOULDBLOCK
 *   - EINTR
 *   - fatal error
 * No automatic retry loop. No blocking writes. State transitions: OPEN -> WRITABLE -> OPEN.
 */
struct omni_connection_io_result omni_connection_io_writable(struct omni_connection_io *io);

/*
 * Begin closing the I/O layer. Transitions from OPEN, READABLE, or WRITABLE
 * to CLOSING. Idempotent from CLOSING and CLOSED. Rejected from NEW.
 * The socket is NOT closed here; the connection owns that.
 */
struct omni_connection_io_result omni_connection_io_close(struct omni_connection_io *io);

/*
 * Release the receive and send buffer objects and return the object to CLOSED.
 * NULL, NEW, and repeated destroy are safe no-ops. The caller must remove any
 * external poller registration before destroy. The borrowed accepted owner is
 * NOT destroyed or closed here.
 */
void omni_connection_io_destroy(struct omni_connection_io *io);

/* State and buffer views. NULL reports NEW / false. */
enum omni_connection_io_state omni_connection_io_state(const struct omni_connection_io *io);
bool omni_connection_io_is_open(const struct omni_connection_io *io);

/*
 * Borrow the connection-local receive buffer object for explicit read-side
 * inspection/consume/compact operations while OPEN/READABLE. The caller must
 * not destroy or reinitialize it, and the pointer expires at connection I/O
 * destroy. NULL is returned for NULL or non-OPEN/READABLE connections.
 */
struct omni_bytebuf *omni_connection_io_receive_buffer(struct omni_connection_io *io);

/*
 * Borrow the connection-local send buffer object for explicit write-side
 * fill/commit operations while OPEN/WRITABLE. The caller must not destroy or
 * reinitialize it, and the pointer expires at connection I/O destroy. NULL is
 * returned for NULL or non-OPEN/WRITABLE connections.
 */
struct omni_bytebuf *omni_connection_io_send_buffer(struct omni_connection_io *io);

/* Borrowed FD view for external poller registration. NULL reports invalid. */
int omni_connection_io_fd(const struct omni_connection_io *io);

#endif /* OMNIROUTE_CONNECTION_IO_H */