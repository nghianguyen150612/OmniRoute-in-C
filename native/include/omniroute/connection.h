/*
 * OmniRoute native backend — protocol-agnostic connection owner (Task 019).
 *
 * This is the first long-lived native connection object. It owns one
 * accepted client descriptor only after an explicit adoption step, and it
 * owns the lifetime of one connection-local receive byte-buffer object. The
 * byte-buffer backing is caller-provided storage: the connection never
 * allocates, grows, or replaces it. There is deliberately no output buffer
 * or queue, no HTTP state, no parser state, no response state, no TLS state,
 * and no event loop.
 *
 * Preparation and ownership are separate on purpose:
 *
 *   INERT --init--> READY --from_accepted--> OPEN --begin_close--> CLOSING
 *     ^                 |                    |                     |
 *     |                 +--failure----------+----------destroy-----+
 *     |                                      |
 *     +--------------------destroy---------- CLOSED
 *
 * `omni_connection_init` prepares the bounded receive storage and poller
 * metadata but owns no socket. `omni_connection_from_accepted` then moves
 * the live accepted owner into the connection. A failed adoption leaves the
 * accepted owner with its original caller and leaves the prepared
 * connection usable for a later attempt. The source accepted owner is made
 * inert only after the connection has published its own copy of the owner.
 *
 * The destination must be fresh or have been made inert before init. Never
 * call make_inert on a READY, OPEN, or CLOSING connection: that would discard
 * state without performing the required cleanup. Use destroy for every live
 * or prepared connection. Destroy is idempotent and reaches CLOSED; it
 * destroys the embedded byte-buffer object before releasing the accepted FD.
 * The accepted FD remains connection-owned until destroy, including while
 * CLOSING. Descriptor 0 is valid because liveness is explicit and the only
 * invalid FD sentinel is -1.
 *
 * Payload operations are synchronous delegations. Receive writes into the
 * embedded bounded byte buffer through the existing receive primitive; send
 * borrows an immutable caller span through the existing SIGPIPE-safe send
 * primitive. Both operations are available only in OPEN, never retain a
 * caller pointer, never alter poller registration, and never change
 * lifecycle state automatically after EOF or peer failure. The caller still
 * chooses when to begin closing and must remove any external poller
 * registration before destroy.
 *
 * Poller token and interest mask are metadata only. This object never calls
 * the poller, registers itself, updates interest, or removes a registration.
 * The caller keeps the receive backing storage alive until after destroy.
 *
 * A connection is single-owner and externally synchronized: no mutexes,
 * atomics, or thread-safety machinery exist here.
 */

#ifndef OMNIROUTE_CONNECTION_H
#define OMNIROUTE_CONNECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omniroute/accepted.h"
#include "omniroute/bytebuf.h"
#include "omniroute/poller.h"
#include "omniroute/recv.h"
#include "omniroute/send.h"

#define OMNI_CONNECTION_FD_INVALID OMNI_ACCEPTED_FD_INVALID

enum omni_connection_state {
  OMNI_CONNECTION_INERT = 0,    /* no buffer and no owned descriptor */
  OMNI_CONNECTION_READY,        /* buffer/metadata prepared; no FD yet */
  OMNI_CONNECTION_OPEN,         /* owns one accepted FD; I/O is allowed */
  OMNI_CONNECTION_CLOSING,      /* owns one accepted FD; I/O is stopped */
  OMNI_CONNECTION_CLOSED        /* all owned resources released */
};

enum omni_connection_status {
  OMNI_CONNECTION_OK = 0,
  OMNI_CONNECTION_ERR_INVALID, /* NULL, bad storage, bad metadata, bad owner */
  OMNI_CONNECTION_ERR_STATE     /* invalid lifecycle transition */
};

struct omni_connection_result {
  enum omni_connection_status status;
  int sys_errno; /* EINVAL for contract errors; 0 on success */
};

struct omni_connection_config {
  void *receive_storage;     /* caller-owned bytes retained until destroy */
  size_t receive_capacity;   /* finite, nonzero hard receive cap */
  uint64_t poller_token;     /* opaque caller identity; zero is valid */
  uint32_t poller_interests; /* nonzero OMNI_POLLER_INTEREST_* mask */
};

struct omni_connection {
  struct omni_accepted accepted; /* owned only in OPEN/CLOSING */
  struct omni_bytebuf receive;   /* connection-local object; borrowed backing */
  uint64_t poller_token;
  uint32_t poller_interests;
  enum omni_connection_state state;
};

/*
 * Canonicalize fresh caller-owned connection storage to INERT without
 * touching any descriptor or freeing anything. NULL-safe. Call this before
 * the first init when the struct is not statically initialized. It is also
 * safe for an already INERT or CLOSED object. It is not a cleanup operation
 * for READY, OPEN, or CLOSING objects; destroy those first.
 */
void omni_connection_make_inert(struct omni_connection *connection);

/*
 * Prepare one bounded borrowed receive buffer and poller metadata. Requires
 * an INERT destination; transitions it to READY. No descriptor is acquired,
 * and no heap allocation occurs. Invalid input leaves the destination INERT.
 * A nonzero interest mask must contain only READ and/or WRITE bits.
 */
struct omni_connection_result omni_connection_init(
    struct omni_connection *connection, const struct omni_connection_config *config);

/*
 * Move one live accepted owner into a READY connection. On success the
 * connection becomes OPEN and the source owner becomes inert; the caller
 * must not destroy or use that source owner for the FD afterward. On failure
 * no ownership moves and the source remains unchanged. The connection must
 * be destroyed before its caller-provided receive storage is released.
 */
struct omni_connection_result omni_connection_from_accepted(
    struct omni_connection *connection, struct omni_accepted *accepted);

/*
 * Mark an OPEN connection CLOSING. No shutdown is performed and the FD is
 * still owned until destroy. Repeated calls, and calls from INERT/READY/
 * CLOSED, return ERR_STATE without changing state.
 */
struct omni_connection_result omni_connection_begin_close(struct omni_connection *connection);

/*
 * Release the receive buffer object and then the owned accepted descriptor,
 * returning the object to CLOSED. NULL, INERT, and repeated destroy are
 * safe. The caller must remove any external poller registration first.
 */
void omni_connection_destroy(struct omni_connection *connection);

/* State and borrowed FD views. NULL reports INERT / false / invalid FD. */
enum omni_connection_state omni_connection_state(const struct omni_connection *connection);
bool omni_connection_is_live(const struct omni_connection *connection);
int omni_connection_fd(const struct omni_connection *connection);

/* Poller metadata views only; these functions never touch a poller. */
uint64_t omni_connection_poller_token(const struct omni_connection *connection);
uint32_t omni_connection_poller_interests(const struct omni_connection *connection);

/*
 * Borrow the connection-local receive buffer object for explicit read-side
 * inspection/consume/compact operations while OPEN. The caller must not
 * destroy or reinitialize it, and the pointer expires at connection destroy.
 * NULL is returned for NULL or non-OPEN connections.
 */
struct omni_bytebuf *omni_connection_receive_buffer(struct omni_connection *connection);

/*
 * Delegate one-shot/bounded receive into the connection-owned buffer. Only
 * OPEN connections permit I/O; other states return RECV_ERR_INVALID with no
 * syscall. EOF and socket failures remain primitive results and do not
 * change lifecycle state.
 */
struct omni_recv_result omni_connection_recv_once(struct omni_connection *connection);
struct omni_recv_result omni_connection_recv_drain(struct omni_connection *connection,
                                                   size_t max_calls);

/*
 * Delegate one-shot/bounded send from an immutable caller-owned span. Only
 * OPEN connections permit I/O; all Task 018 partial-progress, would-block,
 * interruption, peer-failure, and no-retention semantics remain unchanged.
 */
struct omni_send_result omni_connection_send_once(const struct omni_connection *connection,
                                                  struct omni_send_span span);
struct omni_send_result omni_connection_send_drain(const struct omni_connection *connection,
                                                   struct omni_send_span span,
                                                   size_t max_calls);

#endif /* OMNIROUTE_CONNECTION_H */
