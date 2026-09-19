/*
 * OmniRoute native backend — bounded reusable byte buffer implementation.
 *
 * Overflow strategy: no expression of the form `write + len` or
 * `read + count` is ever computed speculatively. Every admission uses
 * subtraction-first bounds (`len <= capacity - write`,
 * `count <= write - read`), so each sum the code does compute was already
 * proven not to exceed capacity (hence not SIZE_MAX). Pointer formation
 * (`backing + read`, `backing + write`) happens only after the integer
 * proof, keeping every pointer within [backing, backing + capacity].
 *
 * Growth audit: the ONLY malloc in this file is the single owned backing
 * allocation in init_owned; the ONLY free is its counterpart in destroy.
 * Steady-state operations (views, append, commit, consume, compact, reset,
 * accounting) perform zero heap calls — verified structurally (no other
 * allocation token appears below) and at runtime by the ASan leak check
 * over the stress tests, which would flag any per-operation leak at exit.
 *
 * Debug aid: asserts guard the one true invariant
 * (read <= write <= capacity) — impossible corruption, not caller-controlled
 * exhaustion or misuse, which fail cleanly with false/NULL. Sanitizers
 * remain the primary validation.
 */

#include "omniroute/bytebuf.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void mark_inert(struct omni_bytebuf *buf) {
  buf->backing = NULL;
  buf->capacity = 0;
  buf->read = 0;
  buf->write = 0;
  buf->high_water = 0;
  buf->owns_backing = false;
  buf->live = false;
}

/* The structural invariant. Callers never establish it; corruption only. */
static bool offsets_valid(const struct omni_bytebuf *buf) {
  return buf->read <= buf->write && buf->write <= buf->capacity;
}

static void note_growth(struct omni_bytebuf *buf) {
  size_t readable = 0;

  assert(offsets_valid(buf));
  if (!offsets_valid(buf)) {
    return;
  }
  readable = buf->write - buf->read;
  if (readable > buf->high_water) {
    buf->high_water = readable;
  }
}

bool omni_bytebuf_init_borrowed(struct omni_bytebuf *buf, void *storage, size_t capacity) {
  if (buf == NULL || storage == NULL || capacity == 0) {
    if (buf != NULL) {
      mark_inert(buf);
    }
    return false;
  }
  buf->backing = (unsigned char *)storage;
  buf->capacity = capacity;
  buf->read = 0;
  buf->write = 0;
  buf->high_water = 0;
  buf->owns_backing = false;
  buf->live = true;
  return true;
}

bool omni_bytebuf_init_owned(struct omni_bytebuf *buf, size_t capacity) {
  unsigned char *backing = NULL;

  if (buf == NULL || capacity == 0) {
    if (buf != NULL) {
      mark_inert(buf);
    }
    return false;
  }
  /* The single owned backing allocation of this buffer's lifetime. */
  backing = (unsigned char *)malloc(capacity);
  if (backing == NULL) {
    mark_inert(buf);
    return false;
  }
  buf->backing = backing;
  buf->capacity = capacity;
  buf->read = 0;
  buf->write = 0;
  buf->high_water = 0;
  buf->owns_backing = true;
  buf->live = true;
  return true;
}

const unsigned char *omni_bytebuf_read_ptr(const struct omni_bytebuf *buf, size_t *out_len) {
  size_t readable = 0;

  if (buf == NULL || !buf->live || !offsets_valid(buf)) {
    if (out_len != NULL) {
      *out_len = 0;
    }
    return NULL;
  }
  readable = buf->write - buf->read;
  if (out_len != NULL) {
    *out_len = readable;
  }
  /* Empty readable region: no byte to point at; NULL keeps callers from
   * forming a view they cannot dereference. */
  if (readable == 0) {
    return NULL;
  }
  return buf->backing + buf->read;
}

unsigned char *omni_bytebuf_write_ptr(struct omni_bytebuf *buf, size_t *out_len) {
  size_t tail = 0;

  if (buf == NULL || !buf->live || !offsets_valid(buf)) {
    if (out_len != NULL) {
      *out_len = 0;
    }
    return NULL;
  }
  tail = buf->capacity - buf->write;
  if (out_len != NULL) {
    *out_len = tail;
  }
  /* Full buffer: no writable byte to point at. */
  if (tail == 0) {
    return NULL;
  }
  return buf->backing + buf->write;
}

bool omni_bytebuf_commit(struct omni_bytebuf *buf, size_t count) {
  size_t tail = 0;

  if (buf == NULL || !buf->live || !offsets_valid(buf)) {
    return false;
  }
  assert(offsets_valid(buf));
  /* Subtraction-first: tail is exact, and count is admitted without ever
   * forming write + count speculatively. */
  tail = buf->capacity - buf->write;
  if (count > tail) {
    return false;
  }
  /* Proven: write + count <= capacity, so the sum cannot wrap. */
  buf->write += count;
  note_growth(buf);
  return true;
}

bool omni_bytebuf_append(struct omni_bytebuf *buf, const void *src, size_t len) {
  size_t tail = 0;

  if (buf == NULL || !buf->live || !offsets_valid(buf)) {
    return false;
  }
  /* Zero-length append is a no-op success even with NULL src, so text-style
   * "append maybe-empty slice" call sites need no special case. */
  if (len == 0) {
    return true;
  }
  if (src == NULL) {
    return false;
  }
  assert(offsets_valid(buf));
  /* Tail-only admission: a consumed prefix is NOT reclaimed here — callers
   * compact explicitly first. All-or-nothing: failure changes nothing. */
  tail = buf->capacity - buf->write;
  if (len > tail) {
    return false;
  }
  /* memmove, not memcpy: src may alias backing (e.g. re-staging bytes
   * already in the buffer); snapshot semantics keep that defined. */
  memmove(buf->backing + buf->write, src, len);
  /* Proven: write + len <= capacity, so the sum cannot wrap. */
  buf->write += len;
  note_growth(buf);
  return true;
}

bool omni_bytebuf_consume(struct omni_bytebuf *buf, size_t count) {
  size_t readable = 0;

  if (buf == NULL || !buf->live || !offsets_valid(buf)) {
    return false;
  }
  assert(offsets_valid(buf));
  readable = buf->write - buf->read;
  if (count > readable) {
    return false;
  }
  /* Proven: read + count <= write <= capacity, so the sum cannot wrap. */
  buf->read += count;
  if (buf->read == buf->write) {
    /* Canonical empty: collapse both offsets so the next append sees the
     * full backing without needing a compact. */
    buf->read = 0;
    buf->write = 0;
  }
  return true;
}

void omni_bytebuf_compact(struct omni_bytebuf *buf) {
  size_t unread = 0;

  if (buf == NULL || !buf->live || !offsets_valid(buf)) {
    return;
  }
  assert(offsets_valid(buf));
  if (buf->read == 0) {
    return; /* already compact (empty included: read == write == 0) */
  }
  if (buf->read == buf->write) {
    /* Empty with a consumed prefix: no bytes to move, just canonicalize. */
    buf->read = 0;
    buf->write = 0;
    return;
  }
  /* read > 0 and read < write here, so unread > 0: the memmove length is
   * nonzero and both ranges lie inside backing. Overlap is the common
   * case — memmove keeps byte order exact. */
  unread = buf->write - buf->read;
  memmove(buf->backing, buf->backing + buf->read, unread);
  buf->write = unread;
  buf->read = 0;
}

void omni_bytebuf_reset(struct omni_bytebuf *buf) {
  if (buf == NULL || !buf->live) {
    return;
  }
  /* Reuse only: backing retained, capacity unchanged, contents kept (no
   * erasure guarantee), high_water keeps its lifetime maximum. Outstanding
   * zero-copy views invalidate by contract from this point on. */
  buf->read = 0;
  buf->write = 0;
}

void omni_bytebuf_destroy(struct omni_bytebuf *buf) {
  if (buf == NULL || !buf->live) {
    return;
  }
  if (buf->owns_backing) {
    free(buf->backing);
  }
  mark_inert(buf);
}

size_t omni_bytebuf_capacity(const struct omni_bytebuf *buf) {
  if (buf == NULL || !buf->live) {
    return 0;
  }
  return buf->capacity;
}

size_t omni_bytebuf_readable(const struct omni_bytebuf *buf) {
  if (buf == NULL || !buf->live || !offsets_valid(buf)) {
    return 0;
  }
  return buf->write - buf->read;
}

size_t omni_bytebuf_writable(const struct omni_bytebuf *buf) {
  if (buf == NULL || !buf->live || !offsets_valid(buf)) {
    return 0;
  }
  return buf->capacity - buf->write;
}

size_t omni_bytebuf_reclaimable(const struct omni_bytebuf *buf) {
  if (buf == NULL || !buf->live || !offsets_valid(buf)) {
    return 0;
  }
  return buf->read;
}

size_t omni_bytebuf_high_water(const struct omni_bytebuf *buf) {
  if (buf == NULL || !buf->live) {
    return 0;
  }
  return buf->high_water;
}
