/*
 * OmniRoute native backend — bounded reusable byte buffer (Task 013).
 *
 * A small, hard-cap byte staging primitive for future network input/output
 * and incremental parser staging (MIGRATION_PLAN.md section 2.1 buffering,
 * MEMORY_MODEL.md sections 2.4/2.5/5). Capacity is finite and fixed at
 * initialization: no operation grows it, ever. Exhaustion fails cleanly and
 * leaves state unchanged — higher layers later choose among reject,
 * consume/compact, stream, or explicit spill policy. This primitive decides
 * none of that.
 *
 * State model (linear, NOT a ring — see below): two offsets into one flat
 * backing store with the invariant read <= write <= capacity.
 *
 *   [0, read)          consumed prefix (unreadable, reclaimable by compact)
 *   [read, write)      readable bytes (zero-copy inspectable)
 *   [write, capacity)  contiguous writable tail (zero-copy fillable)
 *
 * A linear read/write-offset buffer with explicit compaction is sufficient
 * for initial incremental-parse work and easier to audit than a ring:
 * MEMORY_MODEL.md section 2.5 mentions a "fixed ring/chain buffer" only as
 * a future C-implication sketch ("pump upstream->downstream"), and
 * MIGRATION_PLAN.md section 1 lists "buf/ring" as a layout placeholder —
 * neither mandates ring semantics for this primitive, so this task keeps it
 * linear. Revisit only with a real consumer that proves wraparound wins.
 *
 * Ownership is explicit, never inferred (symmetric with Task 012 arena):
 * - borrowed buffers use caller-owned storage (`init_borrowed`); destroy
 *   never frees it;
 * - owned buffers hold exactly one libc allocation made at init and released
 *   at destroy (`init_owned`); append/consume/compact/commit/reset never
 *   call malloc.
 *
 * Zero-capacity buffers are rejected (symmetric with the arena): both init
 * forms return false and leave the structure inert. This keeps every live
 * buffer's backing a valid non-NULL range and every view check branchless.
 *
 * Zero-copy view lifetime: any pointer returned into backing storage is
 * borrowed and may be invalidated — or semantically moved, in the case of
 * compact — by append, commit, consume, compact, reset, or destroy. Never
 * retain a view across those calls; never treat one as a stable owned or
 * NUL-terminated string.
 *
 * Byte semantics: raw bytes, no NUL appended, no text/UTF-8 assumption, no
 * strlen-based API. Embedded zero bytes are ordinary payload.
 *
 * Append never compacts implicitly: it requires contiguous tail space
 * (len <= capacity - write). A consumed prefix is NOT reclaimed as tail
 * until an explicit compact. Append is all-or-nothing — no partial writes.
 * Source/destination overlap is supported via memmove (appending from inside
 * the buffer's own backing behaves as a snapshot copy).
 *
 * A buffer is single-owner and externally synchronized: no mutexes, no
 * atomics, no thread-safety machinery. Failed operations return false/NULL
 * and never abort.
 *
 * Reset reuses backing without clearing it (no secure-erasure guarantee;
 * secrets need an explicit wipe mechanism later). High-water keeps its
 * lifetime maximum across reset.
 */

#ifndef OMNIROUTE_BYTEBUF_H
#define OMNIROUTE_BYTEBUF_H

#include <stdbool.h>
#include <stddef.h>

struct omni_bytebuf {
  unsigned char *backing;
  size_t capacity;
  size_t read;       /* offset of first readable byte; reclaimable prefix */
  size_t write;      /* one-past-last readable byte; start of free tail */
  size_t high_water; /* lifetime maximum of readable bytes; reset preserves */
  bool owns_backing;
  bool live; /* false before init, after destroy, or after failed init */
};

/*
 * Borrow caller-owned storage of exactly capacity bytes. The caller keeps
 * storage alive until after destroy; reset/destroy never free it. Returns
 * false (buffer left inert) for NULL inputs or zero capacity.
 */
bool omni_bytebuf_init_borrowed(struct omni_bytebuf *buf, void *storage, size_t capacity);

/*
 * Take exactly one owned backing allocation of capacity bytes, released by
 * destroy. Returns false (buffer left inert) for zero capacity or when the
 * backing allocation fails. This is the only heap allocation in the owned
 * lifecycle.
 */
bool omni_bytebuf_init_owned(struct omni_bytebuf *buf, size_t capacity);

/*
 * Zero-copy readable view: pointer to the first readable byte plus its
 * length in *out_len (out_len may be NULL). Empty or non-live buffer:
 * returns NULL with *out_len set to 0 when out_len is non-NULL. The
 * pointer is borrowed (see lifetime notes above). NULL buffer: NULL.
 */
const unsigned char *omni_bytebuf_read_ptr(const struct omni_bytebuf *buf, size_t *out_len);

/*
 * Zero-copy writable view: pointer to the contiguous free tail plus its
 * length in *out_len (out_len may be NULL). Full, empty-capacity
 * (impossible for live buffers — capacity is nonzero), or non-live buffer:
 * returns NULL with *out_len set to 0 when out_len is non-NULL. Intended
 * for a future socket read placed directly into storage; this function
 * itself performs no I/O. The pointer is borrowed (see lifetime notes).
 * NULL buffer: NULL.
 */
unsigned char *omni_bytebuf_write_ptr(struct omni_bytebuf *buf, size_t *out_len);

/*
 * Advance the write side by count bytes previously placed through the
 * writable view. Requires count <= contiguous tail space; zero is a
 * successful no-op. Returns false with state unchanged on a non-live/NULL
 * buffer or when count exceeds the tail. Never allocates. Updates
 * high-water when readable grows.
 */
bool omni_bytebuf_commit(struct omni_bytebuf *buf, size_t count);

/*
 * Copy len bytes from src into the writable tail. Requires contiguous tail
 * space (len <= capacity - write): NO implicit compaction — compact first
 * when a consumed prefix holds the space you need. All-or-nothing: failure
 * leaves state unchanged. Zero length is a successful no-op even with NULL
 * src; NULL src with nonzero len fails. Overlap with backing is safe
 * (memmove snapshot semantics). Never allocates. Updates high-water when
 * readable grows.
 */
bool omni_bytebuf_append(struct omni_bytebuf *buf, const void *src, size_t len);

/*
 * Advance the read side by count bytes after the caller processed them.
 * Requires count <= readable bytes; zero is a successful no-op. Returns
 * false with state unchanged otherwise. Consuming exactly the readable
 * amount reaches the canonical empty state (read == write == 0). Never
 * erases bytes. Never allocates.
 */
bool omni_bytebuf_consume(struct omni_bytebuf *buf, size_t count);

/*
 * Move unread bytes toward the start of backing storage (memmove,
 * order-preserving) to recover a contiguous writable tail. No-op when
 * already compact (read == 0) or empty (then also canonicalizes to
 * read == write == 0). NULL or non-live buffers: no-op. Never allocates.
 * This is the ONLY operation with cost proportional to the unread bytes —
 * every other steady-state operation is O(1) — so callers compact
 * explicitly, never per-consume.
 */
void omni_bytebuf_compact(struct omni_bytebuf *buf);

/*
 * Make the buffer logically empty (read == write == 0) while retaining
 * backing storage and capacity for reuse. Contents are NOT cleared and
 * high-water is preserved. No heap activity. NULL or non-live: no-op.
 */
void omni_bytebuf_reset(struct omni_bytebuf *buf);

/*
 * Release owned backing exactly once and leave the buffer inert
 * (backing NULL, zero accounting, live false). Borrowed storage is never
 * freed. NULL, non-live, and repeated destroy are safe no-ops.
 */
void omni_bytebuf_destroy(struct omni_bytebuf *buf);

/* Cheap local accounting. NULL or non-live buffers report zero. */
size_t omni_bytebuf_capacity(const struct omni_bytebuf *buf);
size_t omni_bytebuf_readable(const struct omni_bytebuf *buf);
size_t omni_bytebuf_writable(const struct omni_bytebuf *buf);   /* contiguous tail */
size_t omni_bytebuf_reclaimable(const struct omni_bytebuf *buf); /* prefix compact would move */
size_t omni_bytebuf_high_water(const struct omni_bytebuf *buf);

#endif /* OMNIROUTE_BYTEBUF_H */
