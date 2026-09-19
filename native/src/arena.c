/*
 * OmniRoute native backend — bounded arena allocator implementation.
 *
 * Overflow strategy: no expression of the form `offset + padding + size` is
 * ever computed speculatively. Padding derives from a bitmask over the live
 * absolute address (valid since used + pad <= capacity keeps the pointer
 * formable), and admission uses subtraction-first bounds (`pad <= remaining`, `size <= remaining - pad`), so every sum the
 * code does compute is already proven not to exceed capacity (hence not
 * SIZE_MAX). Pointer formation happens only after the integer proof, keeping
 * every pointer within [backing, backing + capacity].
 *
 * Debug aid: a single assert guards the one true invariant (used never
 * exceeds capacity) — impossible corruption, not caller-controlled
 * exhaustion, which returns NULL. Sanitizers remain the primary validation.
 */

#include "omniroute/arena.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void mark_inert(struct omni_arena *arena) {
  arena->backing = NULL;
  arena->capacity = 0;
  arena->used = 0;
  arena->high_water = 0;
  arena->owns_backing = false;
  arena->live = false;
}

static bool alignment_valid(size_t align) {
  /* Power of two and nonzero. align - 1 cannot underflow (align >= 1 here
   * is checked first via short-circuit on align == 0). */
  if (align == 0) {
    return false;
  }
  return (align & (align - 1u)) == 0u;
}

bool omni_arena_init_borrowed(struct omni_arena *arena, void *buffer, size_t size) {
  if (arena == NULL || buffer == NULL || size == 0) {
    if (arena != NULL) {
      mark_inert(arena);
    }
    return false;
  }
  arena->backing = (unsigned char *)buffer;
  arena->capacity = size;
  arena->used = 0;
  arena->high_water = 0;
  arena->owns_backing = false;
  arena->live = true;
  return true;
}

bool omni_arena_init_owned(struct omni_arena *arena, size_t capacity) {
  unsigned char *backing = NULL;

  if (arena == NULL || capacity == 0) {
    if (arena != NULL) {
      mark_inert(arena);
    }
    return false;
  }
  /* The single owned backing allocation of this arena's lifetime. */
  backing = (unsigned char *)malloc(capacity);
  if (backing == NULL) {
    mark_inert(arena);
    return false;
  }
  arena->backing = backing;
  arena->capacity = capacity;
  arena->used = 0;
  arena->high_water = 0;
  arena->owns_backing = true;
  arena->live = true;
  return true;
}

void *omni_arena_alloc(struct omni_arena *arena, size_t size, size_t align) {
  size_t mask = 0;
  size_t misalign = 0;
  size_t pad = 0;
  size_t remaining = 0;
  size_t new_used = 0;
  uintptr_t addr = 0;
  unsigned char *result = NULL;

  if (arena == NULL || !arena->live || !alignment_valid(align)) {
    return NULL;
  }
  /* Invariant, not caller input: corruption if it ever fires. */
  assert(arena->used <= arena->capacity);

  /* Padding from the absolute address, not the offset: caller buffers carry
   * no alignment guarantee, so offset-relative padding could hand out a
   * misaligned pointer. used + pad <= capacity keeps (backing + used) a
   * valid pointer, and converting a valid pointer to uintptr_t is defined.
   * mask fits uintptr_t wherever addresses and sizes share a width (all
   * supported targets: a power-of-two align <= SIZE_MAX leaves
   * mask <= SIZE_MAX/2 on every platform width). */
  mask = align - 1u;
  addr = (uintptr_t)(arena->backing + arena->used);
  misalign = (size_t)(addr & (uintptr_t)mask);
  pad = (misalign == 0u) ? 0u : (align - misalign);

  /* Admission, subtraction-first: remaining is exact (used <= capacity),
   * and both comparisons avoid forming used + pad + size. */
  remaining = arena->capacity - arena->used;
  if (pad > remaining) {
    return NULL;
  }
  if (size > remaining - pad) {
    return NULL;
  }
  /* Proven: used + pad <= capacity and (used + pad) + size <= capacity, so
   * neither sum wraps and the pointer below stays within
   * [backing, backing + capacity]. */
  new_used = arena->used + pad;
  new_used += size;
  result = arena->backing + arena->used + pad;
  arena->used = new_used;
  if (new_used > arena->high_water) {
    arena->high_water = new_used;
  }
  return (void *)result;
}

void *omni_arena_alloc_zeroed(struct omni_arena *arena, size_t size, size_t align) {
  void *result = omni_arena_alloc(arena, size, align);

  if (result != NULL && size != 0) {
    memset(result, 0, size);
  }
  return result;
}

void omni_arena_reset(struct omni_arena *arena) {
  if (arena == NULL || !arena->live) {
    return;
  }
  /* Rewind only: backing retained, capacity unchanged, contents kept,
   * high_water keeps its lifetime maximum. Outstanding pointers invalidate
   * by contract from this point on. */
  arena->used = 0;
}

void omni_arena_destroy(struct omni_arena *arena) {
  if (arena == NULL || !arena->live) {
    return;
  }
  if (arena->owns_backing) {
    free(arena->backing);
  }
  mark_inert(arena);
}

size_t omni_arena_capacity(const struct omni_arena *arena) {
  if (arena == NULL || !arena->live) {
    return 0;
  }
  return arena->capacity;
}

size_t omni_arena_used(const struct omni_arena *arena) {
  if (arena == NULL || !arena->live) {
    return 0;
  }
  return arena->used;
}

size_t omni_arena_remaining(const struct omni_arena *arena) {
  if (arena == NULL || !arena->live) {
    return 0;
  }
  assert(arena->used <= arena->capacity);
  return arena->capacity - arena->used;
}

size_t omni_arena_high_water(const struct omni_arena *arena) {
  if (arena == NULL || !arena->live) {
    return 0;
  }
  return arena->high_water;
}
