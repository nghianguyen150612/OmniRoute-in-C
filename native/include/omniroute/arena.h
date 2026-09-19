/*
 * OmniRoute native backend — bounded arena allocator (Task 012).
 *
 * A small, explicit, overflow-safe bump allocator for future request-scoped
 * native allocations (MIGRATION_PLAN.md section 2.1). Capacity is finite and
 * fixed at initialization: exhaustion fails cleanly with NULL, never grows,
 * never falls back to the heap, never chains hidden blocks.
 *
 * Lifetime contract: every pointer returned for a nonzero size remains valid
 * only until its arena is reset or destroyed. There is no individual free.
 *
 * Ownership is explicit, never inferred:
 * - borrowed arenas use caller-owned storage (`init_borrowed`); destroy
 *   never frees it;
 * - owned arenas hold exactly one libc allocation made at init and released
 *   at destroy (`init_owned`); steady-state allocation never calls malloc.
 *
 * An arena is single-owner and externally synchronized: no mutexes, no
 * atomics, no thread-safety machinery. Failed allocations (exhaustion,
 * invalid alignment, dead arena) return NULL or zero and never abort.
 *
 * Zero-size allocation returns the aligned current position without
 * consuming space (NULL when even the alignment padding does not fit).
 * The returned pointer may be one-past-end; dereference nothing from it.
 */

#ifndef OMNIROUTE_ARENA_H
#define OMNIROUTE_ARENA_H

#include <stdbool.h>
#include <stddef.h>

/* Default alignment for callers with no stricter need: the strictest scalar. */
#define OMNI_ARENA_ALIGN_DEFAULT _Alignof(max_align_t)

struct omni_arena {
  unsigned char *backing;
  size_t capacity;
  size_t used;
  size_t high_water; /* lifetime maximum of used; reset does not clear it */
  bool owns_backing;
  bool live; /* false before init, after destroy, or after failed init */
};

/*
 * Borrow caller-owned storage. The caller keeps backing alive until after
 * destroy; reset/destroy never free it. Returns false (arena left inert)
 * for NULL inputs or zero size.
 */
bool omni_arena_init_borrowed(struct omni_arena *arena, void *buffer, size_t size);

/*
 * Take exactly one owned backing allocation of capacity bytes, released by
 * destroy. Returns false (arena left inert) for zero capacity or when the
 * backing allocation fails.
 */
bool omni_arena_init_owned(struct omni_arena *arena, size_t capacity);

/*
 * Bump-allocate size bytes with the given alignment (power of two, nonzero).
 * Returns NULL on exhaustion, invalid alignment, or a non-live/NULL arena.
 * Padding counts against capacity. No heap fallback, no metadata headers.
 */
void *omni_arena_alloc(struct omni_arena *arena, size_t size, size_t align);

/* Like alloc, but zero-fills the returned range. */
void *omni_arena_alloc_zeroed(struct omni_arena *arena, size_t size, size_t align);

/*
 * Rewind usage to zero and keep the backing allocation: capacity unchanged,
 * contents NOT cleared (no data-erasure guarantee; secrets need an explicit
 * wipe mechanism later). All outstanding pointers invalidate immediately.
 * NULL or non-live arenas: no-op.
 */
void omni_arena_reset(struct omni_arena *arena);

/*
 * Release owned backing exactly once and leave the arena inert
 * (backing NULL, zero accounting, live false). Borrowed storage is never
 * freed. NULL, non-live, and repeated destroy are safe no-ops.
 */
void omni_arena_destroy(struct omni_arena *arena);

/* Cheap local accounting. NULL or non-live arenas report zero. */
size_t omni_arena_capacity(const struct omni_arena *arena);
size_t omni_arena_used(const struct omni_arena *arena);
size_t omni_arena_remaining(const struct omni_arena *arena);
size_t omni_arena_high_water(const struct omni_arena *arena);

#endif /* OMNIROUTE_ARENA_H */
