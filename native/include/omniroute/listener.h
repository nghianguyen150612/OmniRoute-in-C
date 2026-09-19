/*
 * OmniRoute native backend — Linux-first TCP listener primitive (Task 014).
 *
 * A small loopback-IPv4 listening-socket abstraction with explicit FD
 * ownership: create, configure, bind, listen, query, destroy. This module
 * owns socket creation and lifecycle ONLY:
 *
 * - no accept path (no client FD is ever produced or configured here);
 * - no event loop, no epoll/poll/select, no threads;
 * - no payload input/output of any kind;
 * - no HTTP, no TLS, no signal disposition changes.
 *
 * The borrowed-FD accessor exists so a future event-loop task can register
 * the descriptor without taking ownership.
 *
 * Scope (see MIGRATION_PLAN.md section 1): the future home for FD and
 * poller code is platform/linux, with shared pieces under platform/posix.
 * This task keeps one small module with documented Linux/POSIX assumptions
 * instead of a speculative portability layer: AF_INET stream sockets with
 * atomic SOCK_NONBLOCK/SOCK_CLOEXEC creation flags, verified after
 * creation. IPv6 is deferred, not supported — a dual-stack story arrives
 * with a real consumer.
 *
 * Bind policy: loopback only. Initialization accepts any address in
 * 127.0.0.0/8 and rejects everything else, including 0.0.0.0 — the
 * unfinished backend must never be exposed to the LAN by construction.
 * External-exposure semantics belong to an explicit later task.
 *
 * Ownership: on success the listener owns exactly one kernel descriptor
 * and the caller must never close it directly; destroy releases it exactly
 * once. Every failure after descriptor creation closes the descriptor
 * before returning, so a failed init never leaves a live owned FD.
 * The raw-FD accessor is a borrowed view: no transfer, caller must not
 * close it. Descriptor 0 is valid; only -1 means invalid.
 *
 * Close policy: destroy performs a single close and never retries it. On
 * Linux the descriptor is released even when close reports EINTR, so a
 * retry could close an unrelated descriptor recycled into the same number.
 *
 * Error model: every init returns a status plus the errno captured at the
 * failure point, before cleanup calls can overwrite it. No heap-allocated
 * diagnostics; tests assert statuses, never localized message text.
 *
 * Memory: the listener is a caller-provided struct holding one owned
 * kernel FD. No heap allocation, no arena or byte-buffer involvement —
 * those primitives solve different problems and are not coupled here.
 *
 * A listener is single-owner and externally synchronized: no mutexes, no
 * atomics, no thread-safety machinery.
 */

#ifndef OMNIROUTE_LISTENER_H
#define OMNIROUTE_LISTENER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Invalid-descriptor sentinel. Descriptor 0 is valid and never treated as
 * invalid; only this value means "no descriptor". */
#define OMNI_LISTENER_FD_INVALID (-1)

/* Returned by omni_listener_port for a non-live listener. Port 0 is never
 * a valid actual bound port: requesting 0 asks the kernel for an ephemeral
 * assignment, and the stored value is always the assigned one. */
#define OMNI_LISTENER_PORT_INVALID ((uint16_t)0)

/*
 * Listen backlog: small and explicit. Pending handshakes beyond this wait
 * in the kernel queue only up to the kernel's own clamp
 * (net.core.somaxconn and friends); this constant is the application's
 * ask, not a guarantee. No runtime configuration exists yet — later
 * server work can promote this to a vetted setting with its own bounds.
 */
#define OMNI_LISTENER_BACKLOG 16

enum omni_listener_status {
  OMNI_LISTENER_OK = 0,
  OMNI_LISTENER_ERR_INVALID, /* NULL/empty/malformed args, non-loopback address */
  OMNI_LISTENER_ERR_SOCKET,  /* descriptor creation failed */
  OMNI_LISTENER_ERR_OPTION,  /* flag verification or SO_REUSEADDR setup failed */
  OMNI_LISTENER_ERR_BIND,    /* address bind failed, including conflicts */
  OMNI_LISTENER_ERR_LISTEN,  /* marking the descriptor listening failed */
  OMNI_LISTENER_ERR_ADDR     /* bound-address query failed */
};

struct omni_listener_result {
  enum omni_listener_status status;
  int sys_errno; /* errno at the failure point; 0 on success */
};

struct omni_listener {
  int fd;         /* owned descriptor; OMNI_LISTENER_FD_INVALID when inert */
  uint16_t port;  /* actual bound port, host order; 0 when inert */
  bool live;      /* false before init, after destroy, or after failed init */
};

/*
 * Put a listener into the inert state without touching any descriptor:
 * fd invalid, port invalid, not live. All observers treat this exactly
 * like a destroyed listener. Call this before first use rather than
 * relying on zeroed storage. NULL-safe no-op.
 */
void omni_listener_make_inert(struct omni_listener *listener);

/*
 * Create a nonblocking, close-on-exec TCP listener bound to the given
 * IPv4 address text and port, both validated before any descriptor is
 * created. Address must parse as IPv4 and lie in 127.0.0.0/8; port is in
 * host order with 0 requesting a kernel-assigned ephemeral port. Applies
 * SO_REUSEADDR, then the bind/listen steps with bounded EINTR retries.
 * Returns OMNI_LISTENER_OK with the owned descriptor and the actual bound
 * port stored, or a failure status with errno captured — the listener left
 * inert and no descriptor leaked either way. Ordinary bind conflicts fail
 * here without aborting the process.
 */
struct omni_listener_result omni_listener_init(struct omni_listener *listener,
                                               const char *address,
                                               uint16_t port);

/*
 * Release the owned descriptor exactly once and return the listener to
 * the inert state. Never closes unrelated descriptors: only the stored
 * owned descriptor is released, and only when live. NULL, inert, and
 * repeated destroy are safe no-ops. The single close is never retried;
 * see the close policy above.
 */
void omni_listener_destroy(struct omni_listener *listener);

/*
 * Borrowed view of the owned descriptor for future event-loop
 * registration. No ownership transfer — the caller must not close it, and
 * must not use it past destroy. Returns OMNI_LISTENER_FD_INVALID for NULL
 * or non-live listeners.
 */
int omni_listener_fd(const struct omni_listener *listener);

/*
 * Actual bound port in host order, from kernel socket state after the
 * bind/listen steps — never the requested value echoed back. Returns
 * OMNI_LISTENER_PORT_INVALID for NULL or non-live listeners.
 */
uint16_t omni_listener_port(const struct omni_listener *listener);

#endif /* OMNIROUTE_LISTENER_H */
