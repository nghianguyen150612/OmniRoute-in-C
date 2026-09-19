/*
 * OmniRoute native backend — bounded nonblocking socket receive.
 *
 * Conversion strategy: the writable tail arrives as size_t and the single
 * request length is capped at SSIZE_MAX before the call, so the value
 * handed to the kernel always fits the signed return range and no request
 * relies on implementation-defined behavior for oversized lengths. A
 * positive signed return converts to size_t only after the sign proof, and
 * the commit length can never exceed the observed tail, so the publish
 * step is exact. Drain totals accumulate in size_t behind a defensive
 * overflow guard; the buffer invariant already bounds every total by the
 * hard capacity, and the guard documents that proof in code. Errno is
 * captured immediately after a failed call, before any other operation
 * can overwrite it. If the defensive commit check ever fails after a
 * positive recv, the result reports that positive syscall count under
 * ERR_INTERNAL while refusing to call it DATA; the logical write side is
 * not advanced, and the caller must treat that backing as poisoned rather
 * than retrying.
 *
 * Single-attempt audit: the one-shot primitive performs zero kernel waits
 * for invalid arguments or an empty writable tail, and exactly one
 * otherwise — never a retry, never a peek, never a wait-all. Interruption
 * surfaces as INTERRUPTED with the buffer unchanged. The drain performs at
 * most max_calls attempts, so a peer that keeps producing cannot hold the
 * call forever; a zero limit is rejected as invalid, mirroring the
 * zero-capacity rule of the accept drain.
 *
 * Publication audit: ownership of new bytes escapes exactly once, through
 * the commit of the produced count after a positive return. The request
 * length never exceeds the observed tail, so under valid buffer state the
 * commit cannot fail; the defensive internal status exists fail-closed for
 * the unreachable case only, reporting the syscall count without publishing
 * DATA and leaving the buffer logically unchanged. EOF changes nothing at all:
 * the accepted owner stays live, its descriptor stays open, and buffered bytes stay
 * buffered. Fatal failures likewise leave the descriptor open for a later
 * lifecycle layer to judge.
 *
 * Borrowing audit: this file holds no descriptor lifecycle calls and no
 * buffer management calls beyond the writable view plus the single commit
 * — no flag changes, no compaction, no reset, no reallocation, no destroy.
 * The network-boundary gate enforces the same property textually.
 *
 * Feature macro: _DEFAULT_SOURCE widens system-header visibility only
 * (socket receive declarations on older glibc). It changes nothing about
 * the strict warning level applied to this file.
 */

#define _DEFAULT_SOURCE

#include "omniroute/recv.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

static struct omni_recv_result make_result(enum omni_recv_status status, int err,
                                           size_t received) {
  struct omni_recv_result out;

  out.status = status;
  out.sys_errno = err;
  out.received = received;
  return out;
}

/*
 * Classify the failure of one receive attempt. Would-block and
 * interruption are control flow; everything else stops the caller as
 * fatal so a later lifecycle layer can apply its own close or backoff
 * policy. No transient class exists at this layer: unlike accept, no
 * receive errno names a consumable per-connection event worth surfacing
 * separately, so the table stays at this minimum.
 */
static enum omni_recv_status classify_attempt_errno(int err) {
  if (err == EAGAIN || err == EWOULDBLOCK) {
    return OMNI_RECV_WOULD_BLOCK;
  }
  if (err == EINTR) {
    return OMNI_RECV_INTERRUPTED;
  }
  return OMNI_RECV_ERR_FATAL;
}

struct omni_recv_result omni_recv_once(const struct omni_accepted *conn,
                                       struct omni_bytebuf *buf) {
  int fd = OMNI_ACCEPTED_FD_INVALID;
  unsigned char *dst = NULL;
  size_t tail = 0u;
  size_t req = 0u;
  ssize_t produced = 0;

  if (conn == NULL || buf == NULL) {
    return make_result(OMNI_RECV_ERR_INVALID, EINVAL, 0u);
  }
  /* Borrowed descriptor only; accepted ownership never moves. */
  fd = omni_accepted_fd(conn);
  if (fd == OMNI_ACCEPTED_FD_INVALID) {
    return make_result(OMNI_RECV_ERR_INVALID, EINVAL, 0u);
  }
  /* Only live buffers report nonzero capacity, so this single check
   * rejects every non-live buffer before any kernel wait happens. */
  if (omni_bytebuf_capacity(buf) == 0u) {
    return make_result(OMNI_RECV_ERR_INVALID, EINVAL, 0u);
  }
  dst = omni_bytebuf_write_ptr(buf, &tail);
  if (dst == NULL || tail == 0u) {
    /* Empty writable tail: perform no kernel wait, so pending socket
     * bytes stay queued for a later call after the caller compacts or
     * resets explicitly. A NULL view on a live buffer would mean
     * corrupted offsets, which no API path can establish; treating it
     * the same way stays fail-closed either way. */
    return make_result(OMNI_RECV_BUFFER_FULL, 0, 0u);
  }
  /* Cap the single request at the signed return range: lengths beyond it
   * would leave the return value implementation-defined. Ordinary tails
   * are far smaller; the cap only binds hypothetical giant backings. */
  req = tail;
  if (req > (size_t)SSIZE_MAX) {
    req = (size_t)SSIZE_MAX;
  }
  /* Flags are zero: accepted sockets are already nonblocking by
   * construction, so no per-call override is needed and none is added. */
  produced = recv(fd, dst, req, 0);
  if (produced > 0) {
    size_t got = (size_t)produced;

    /* Proven: got <= req <= tail, so the publish step cannot fail for
     * lack of room. The defensive branch below is unreachable under
     * valid buffer state; it reports the syscall count under ERR_INTERNAL
     * while leaving the buffer logically unchanged rather than pretending
     * the bytes were committed. */
    if (!omni_bytebuf_commit(buf, got)) {
      return make_result(OMNI_RECV_ERR_INTERNAL, EIO, got);
    }
    return make_result(OMNI_RECV_DATA, 0, got);
  }
  if (produced == 0) {
    /* Orderly peer shutdown of its sending side: socket and buffer both
     * untouched, buffered bytes preserved for later consumption. */
    return make_result(OMNI_RECV_EOF, 0, 0u);
  }
  {
    int err = errno;

    return make_result(classify_attempt_errno(err), err, 0u);
  }
}

struct omni_recv_result omni_recv_drain(const struct omni_accepted *conn,
                                        struct omni_bytebuf *buf,
                                        size_t max_calls) {
  size_t total = 0u;
  size_t i = 0u;

  if (conn == NULL || buf == NULL || max_calls == 0u) {
    return make_result(OMNI_RECV_ERR_INVALID, EINVAL, 0u);
  }
  if (omni_accepted_fd(conn) == OMNI_ACCEPTED_FD_INVALID ||
      omni_bytebuf_capacity(buf) == 0u) {
    return make_result(OMNI_RECV_ERR_INVALID, EINVAL, 0u);
  }
  for (i = 0u; i < max_calls; ++i) {
    struct omni_recv_result one = omni_recv_once(conn, buf);

    if (one.status != OMNI_RECV_DATA) {
      /* Terminal reason with every byte committed so far retained in
       * the buffer: nothing rolls back, nothing resets. */
      return make_result(one.status, one.sys_errno, total);
    }
    /* Defensive only: committed bytes only advance the write side, so
     * the total can never exceed the hard capacity — the guard writes
     * that proof down instead of assuming it silently. */
    if (one.received > SIZE_MAX - total) {
      return make_result(OMNI_RECV_ERR_INTERNAL, EIO, total);
    }
    total += one.received;
  }
  /* Call budget used before the input drained: pending bytes may remain,
   * so the limit reports distinctly from would-block. */
  return make_result(OMNI_RECV_LIMIT_REACHED, 0, total);
}
