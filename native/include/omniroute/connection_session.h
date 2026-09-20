/*
 * OmniRoute native backend — bounded connection/session integration (Task 026).
 *
 * This module connects the existing connection ownership model with the
 * bounded connection I/O state machine without introducing protocol behavior.
 *
 * It represents "one live connection with its bounded I/O state" and only
 * coordinates lifecycle. It is NOT an HTTP connection, API session,
 * authentication session, or protocol parser.
 *
 * Relationship:
 *
 *   listener -> accepted -> connection (owns FD + receive buf)
 *                              |
 *                              v
 *                    connection_session (borrows connection,
 *                              |         owns connection_io)
 *                              v
 *                    connection_io (owns receive/send bytebufs,
 *                              |     borrows accepted FD)
 *                              +-- receive buffer (borrowed backing)
 *                              +-- send buffer (borrowed backing)
 *                              +-- socket read/write via recv/send primitives
 *
 * Ownership:
 *   session OWNS: connection_io object lifecycle (including its two
 *                 bytebuf objects with borrowed backing).
 *   session BORROWS: connection object, accepted socket ownership,
 *                    registry membership, reactor registration, caller
 *                    receive/send backing storage.
 *
 * The session MUST NOT:
 *   - close file descriptors directly
 *   - destroy accepted sockets
 *   - remove registry entries
 *   - manage poller registrations
 *   - run event loops
 *   - allocate memory
 *   - read/write automatically, retry, or loop
 *
 * State machine (explicit, bounded):
 *
 *   NEW --init--> INIT --open--> OPEN --close--> CLOSING --destroy--> CLOSED
 *    ^               |            |   ^              |                 ^
 *    |               |            |   |              |                 |
 *    +--make_inert---+            +---readable/writable (OPEN->READABLE/WRITABLE->OPEN transient via io)
 *    |                            |
 *    +-----------destroy----------+
 *
 * Rules:
 *   - invalid transitions return explicit errors (ERR_STATE / ERR_INVALID)
 *   - destroy is safe from any state, idempotent
 *   - repeated close is idempotent, repeated destroy is safe
 *   - failed init leaves session in NEW (inert), failed open leaves it in INIT
 *   - readable/writable are valid only from OPEN and delegate once to io
 */

#ifndef OMNIROUTE_CONNECTION_SESSION_H
#define OMNIROUTE_CONNECTION_SESSION_H

#include <stdbool.h>
#include <stddef.h>

#include "omniroute/bytebuf.h"
#include "omniroute/connection.h"
#include "omniroute/connection_io.h"

enum omni_connection_session_state {
  OMNI_CONNECTION_SESSION_NEW = 0, /* inert, no io init, no connection bound */
  OMNI_CONNECTION_SESSION_INIT,    /* connection bound, buffers recorded, io still NEW */
  OMNI_CONNECTION_SESSION_OPEN,    /* io is OPEN, ready for readable/writable */
  OMNI_CONNECTION_SESSION_CLOSING, /* close requested, io is CLOSING */
  OMNI_CONNECTION_SESSION_CLOSED   /* all session-owned resources released */
};

enum omni_connection_session_status {
  OMNI_CONNECTION_SESSION_OK = 0,
  OMNI_CONNECTION_SESSION_ERR_INVALID, /* NULL, bad connection, bad buffers, bad capacities */
  OMNI_CONNECTION_SESSION_ERR_STATE,   /* invalid lifecycle transition */
  OMNI_CONNECTION_SESSION_ERR_CLOSED   /* operation on closing/closed session */
};

struct omni_connection_session_result {
  enum omni_connection_session_status status;
  int sys_errno; /* EINVAL for contract errors, 0 on success */
};

struct omni_connection_session_config {
  struct omni_connection *connection; /* borrowed, must be OPEN, caller keeps alive */
  void *receive_storage;              /* caller-owned recv backing, retained until destroy */
  size_t receive_capacity;            /* finite, nonzero hard cap */
  void *send_storage;                 /* caller-owned send backing, retained until destroy */
  size_t send_capacity;               /* finite, nonzero hard cap */
};

struct omni_connection_session {
  struct omni_connection *connection; /* borrowed; NULL when NEW/CLOSED */
  struct omni_connection_io io;       /* owned io object; valid only OPEN/CLOSING */
  void *receive_storage;              /* borrowed, stored at init for open */
  size_t receive_capacity;
  void *send_storage;
  size_t send_capacity;
  enum omni_connection_session_state state;
};

/*
 * Canonicalize fresh caller-owned session storage to NEW without touching any
 * descriptor or freeing anything. NULL-safe. Call this before first init when
 * the struct is not statically initialized. Safe for NEW or CLOSED. Not a
 * cleanup for INIT/OPEN/CLOSING; use destroy first.
 */
void omni_connection_session_make_inert(struct omni_connection_session *session);

/*
 * Bind a live OPEN connection and record borrowed buffer backing. Requires a
 * NEW session. Transitions NEW -> INIT on success. No heap allocation,
 * no descriptor operation, no registry/poller interaction. Invalid input
 * leaves the session NEW. The caller must keep the connection and both
 * backing ranges alive until after destroy.
 */
struct omni_connection_session_result omni_connection_session_init(
    struct omni_connection_session *session,
    const struct omni_connection_session_config *config);

/*
 * Initialize the owned connection_io object from the stored borrowed
 * connection and buffer references. Requires INIT. Transitions INIT -> OPEN
 * on success. On failure the session remains INIT and the io remains NEW
 * (caller may retry or destroy). No heap allocation.
 */
struct omni_connection_session_result omni_connection_session_open(
    struct omni_connection_session *session);

/*
 * Begin closing the session I/O. Valid from OPEN. Transitions OPEN -> CLOSING
 * via io_close. Idempotent from CLOSING and CLOSED. Rejected from NEW/INIT.
 * The socket is NOT closed here; the connection still owns the FD.
 */
struct omni_connection_session_result omni_connection_session_close(
    struct omni_connection_session *session);

/*
 * Release the owned connection_io object (both bytebufs) and return the
 * session to CLOSED. Safe for NULL, NEW, INIT, OPEN, CLOSING, CLOSED.
 * The borrowed connection, accepted socket, registry, reactor, and caller
 * buffers are NOT destroyed or closed. The caller must remove any external
 * poller registration before destroy.
 */
void omni_connection_session_destroy(struct omni_connection_session *session);

/* State queries. NULL reports NEW / false / invalid. */
enum omni_connection_session_state omni_connection_session_state(
    const struct omni_connection_session *session);
bool omni_connection_session_is_open(const struct omni_connection_session *session);
bool omni_connection_session_is_closed(const struct omni_connection_session *session);

/* Borrowed connection view. NULL for NULL or NEW/CLOSED sessions. */
struct omni_connection *omni_connection_session_connection(
    struct omni_connection_session *session);

/* Borrowed io view. NULL for NULL or NEW/INIT/CLOSED sessions. */
struct omni_connection_io *omni_connection_session_io(
    struct omni_connection_session *session);

/* Expose io state even when session is INIT (then reports NEW). */
enum omni_connection_io_state omni_connection_session_io_state(
    const struct omni_connection_session *session);

/* Buffer views through the owned io, valid only when session is OPEN. */
struct omni_bytebuf *omni_connection_session_receive_buffer(
    struct omni_connection_session *session);
struct omni_bytebuf *omni_connection_session_send_buffer(
    struct omni_connection_session *session);

/* Borrowed FD view for external poller registration, valid only when OPEN. */
int omni_connection_session_fd(const struct omni_connection_session *session);

/*
 * Perform one bounded readable/writable operation through the session
 * boundary. Valid only when session is OPEN; delegates once to the owned
 * connection_io readable/writable. Handles WOULD_BLOCK, EINTR, EOF,
 * BUFFER_FULL, fatal error as the io layer does. No retry, no loop.
 */
struct omni_connection_io_result omni_connection_session_readable(
    struct omni_connection_session *session);
struct omni_connection_io_result omni_connection_session_writable(
    struct omni_connection_session *session);

#endif /* OMNIROUTE_CONNECTION_SESSION_H */
