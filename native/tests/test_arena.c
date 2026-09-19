/*
 * OmniRoute native backend — bounded arena allocator tests (Task 012).
 *
 * Self-contained, deterministic, framework-free: a fixed sequence of checks
 * with TAP-style output and a nonzero exit on any failure. No randomness, no
 * threads, no network, no giant buffers (SIZE_MAX edge cases assert failure
 * without attempting real backing). Run under CTest, including the
 * ASan + UBSan configuration, which validates the exactly-once owned-free
 * and the absence of overflow UB far better than any hand assertion.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "omniroute/arena.h"

static int check_count = 0;
static int failure_count = 0;

static void check(bool cond, const char *name) {
  ++check_count;
  if (cond) {
    printf("ok - %s\n", name);
  } else {
    ++failure_count;
    printf("NOT OK - %s\n", name);
  }
}

static bool ptr_aligned(const void *ptr, size_t align) {
  return ((uintptr_t)ptr % align) == 0u;
}

static bool within_backing(const unsigned char *backing, size_t capacity,
                           const unsigned char *ptr, size_t size) {
  return ptr >= backing && size <= capacity && ptr + size <= backing + capacity;
}

/* ------------------------------------------------------------------ init */

static void test_init_borrowed_valid(void) {
  struct omni_arena arena;
  unsigned char buffer[64];

  check(omni_arena_init_borrowed(&arena, buffer, sizeof(buffer)),
        "borrowed init accepts a valid buffer");
  check(omni_arena_capacity(&arena) == 64u, "borrowed capacity matches buffer");
  check(omni_arena_used(&arena) == 0u, "fresh arena reports zero used");
  check(omni_arena_remaining(&arena) == 64u, "fresh arena reports full remaining");
  check(omni_arena_high_water(&arena) == 0u, "fresh arena reports zero high-water");
  omni_arena_destroy(&arena);
}

static void test_init_rejects_invalid(void) {
  struct omni_arena arena;
  unsigned char buffer[16];

  check(!omni_arena_init_borrowed(NULL, buffer, sizeof(buffer)),
        "borrowed init rejects NULL arena");
  check(!omni_arena_init_borrowed(&arena, NULL, sizeof(buffer)),
        "borrowed init rejects NULL buffer");
  check(!omni_arena_init_borrowed(&arena, buffer, 0u),
        "borrowed init rejects zero size");
  check(omni_arena_capacity(&arena) == 0u, "rejected arena stays inert");
  check(!omni_arena_init_owned(NULL, 64u), "owned init rejects NULL arena");
  check(!omni_arena_init_owned(&arena, 0u), "owned init rejects zero capacity");
  omni_arena_destroy(&arena);
}

static void test_init_owned_valid(void) {
  struct omni_arena arena;

  check(omni_arena_init_owned(&arena, 128u), "owned init accepts nonzero capacity");
  check(omni_arena_capacity(&arena) == 128u, "owned capacity matches request");
  check(omni_arena_remaining(&arena) == 128u, "owned arena starts empty");
  omni_arena_destroy(&arena);
}

/* ---------------------------------------------------------- alloc basics */

static void test_first_and_sequential_allocs(void) {
  struct omni_arena arena;
  unsigned char buffer[256];
  unsigned char *seen[8];
  size_t seen_sizes[8];
  size_t i = 0;
  size_t j = 0;
  bool overlap = false;

  omni_arena_init_borrowed(&arena, buffer, sizeof(buffer));
  for (i = 0; i < 8u; ++i) {
    seen[i] = (unsigned char *)omni_arena_alloc(&arena, 16u, 8u);
    seen_sizes[i] = 16u;
    if (seen[i] == NULL || !ptr_aligned(seen[i], 8u) ||
        !within_backing(buffer, sizeof(buffer), seen[i], seen_sizes[i])) {
      check(false, "sequential allocs land aligned inside backing");
      omni_arena_destroy(&arena);
      return;
    }
  }
  for (i = 0; i < 8u; ++i) {
    for (j = i + 1u; j < 8u; ++j) {
      const unsigned char *a_start = seen[i];
      const unsigned char *a_end = seen[i] + seen_sizes[i];
      const unsigned char *b_start = seen[j];
      if (!(a_end <= b_start || seen[j] + seen_sizes[j] <= a_start)) {
        overlap = true;
      }
    }
  }
  check(!overlap, "sequential allocations never overlap");
  check(omni_arena_used(&arena) == 8u * 16u, "used accounts every allocation");
  check(omni_arena_remaining(&arena) == 256u - 8u * 16u, "remaining complements used");
  check(omni_arena_high_water(&arena) == 8u * 16u, "high-water tracks peak used");
  omni_arena_destroy(&arena);
}

static void test_exact_capacity_and_overflow(void) {
  struct omni_arena arena;
  unsigned char buffer[64];
  size_t used_before = 0;

  omni_arena_init_borrowed(&arena, buffer, sizeof(buffer));
  check(omni_arena_alloc(&arena, 64u, 1u) != NULL, "exact-capacity allocation succeeds");
  check(omni_arena_used(&arena) == 64u, "exact-capacity allocation consumes all");
  check(omni_arena_remaining(&arena) == 0u, "no remaining after exact fill");
  used_before = omni_arena_used(&arena);
  check(omni_arena_alloc(&arena, 1u, 1u) == NULL, "one byte over capacity fails");
  check(omni_arena_used(&arena) == used_before, "failed alloc preserves used");
  check(omni_arena_remaining(&arena) == 0u, "failed alloc preserves remaining");
  omni_arena_destroy(&arena);
}

/* ----------------------------------------------------------------- reset */

static void test_reset_reuse(void) {
  struct omni_arena arena;
  unsigned char buffer[64];
  void *first = NULL;
  void *second = NULL;

  omni_arena_init_borrowed(&arena, buffer, sizeof(buffer));
  first = omni_arena_alloc(&arena, 32u, 8u);
  check(first != NULL, "pre-reset allocation succeeds");
  omni_arena_reset(&arena);
  check(omni_arena_used(&arena) == 0u, "reset returns used to zero");
  check(omni_arena_capacity(&arena) == 64u, "reset keeps capacity");
  check(omni_arena_remaining(&arena) == 64u, "reset restores remaining");
  second = omni_arena_alloc(&arena, 32u, 8u);
  check(second == first, "reset deterministically reuses backing");
  omni_arena_destroy(&arena);
}

static void test_reset_invalidates_old_pointers(void) {
  struct omni_arena arena;
  unsigned char buffer[64];
  unsigned char *old = NULL;
  unsigned char *fresh = NULL;

  omni_arena_init_borrowed(&arena, buffer, sizeof(buffer));
  old = (unsigned char *)omni_arena_alloc(&arena, 16u, 1u);
  memset(old, 'A', 16u);
  omni_arena_reset(&arena);
  fresh = (unsigned char *)omni_arena_alloc(&arena, 16u, 1u);
  memset(fresh, 'B', 16u);
  /* Reading `old` is defined (backing storage is still live); the changed
   * contents prove the logical ownership moved on. */
  check(old[0] == 'B', "reset invalidates outstanding pointers by reuse");
  omni_arena_destroy(&arena);
}

static void test_high_water_survives_reset(void) {
  struct omni_arena arena;
  unsigned char buffer[128];

  omni_arena_init_borrowed(&arena, buffer, sizeof(buffer));
  check(omni_arena_alloc(&arena, 100u, 1u) != NULL, "large alloc succeeds");
  omni_arena_reset(&arena);
  check(omni_arena_alloc(&arena, 40u, 1u) != NULL, "smaller post-reset alloc succeeds");
  check(omni_arena_high_water(&arena) == 100u, "high-water keeps lifetime maximum");
  omni_arena_destroy(&arena);
}

/* -------------------------------------------------------------- alignment */

static void test_alignments(void) {
  static const size_t aligns[] = {1u, 2u, 4u, 8u, 16u};
  struct omni_arena arena;
  unsigned char buffer[256];
  size_t i = 0;

  omni_arena_init_borrowed(&arena, buffer, sizeof(buffer));
  for (i = 0; i < sizeof(aligns) / sizeof(aligns[0]); ++i) {
    void *ptr = omni_arena_alloc(&arena, 8u, aligns[i]);
    if (ptr == NULL || !ptr_aligned(ptr, aligns[i])) {
      check(false, "every power-of-two alignment is honored");
      omni_arena_destroy(&arena);
      return;
    }
  }
  check(true, "every power-of-two alignment is honored");
  check(omni_arena_alloc(&arena, 8u, OMNI_ARENA_ALIGN_DEFAULT) != NULL,
        "default alignment allocates");
  check(ptr_aligned(omni_arena_alloc(&arena, sizeof(double), _Alignof(double)), _Alignof(double)),
        "double alignment representable");
  omni_arena_destroy(&arena);
}

static void test_padding_accounted(void) {
  struct omni_arena arena;
  unsigned char buffer[32];
  unsigned char *seed = NULL;
  unsigned char *ptr = NULL;

  omni_arena_init_borrowed(&arena, buffer, sizeof(buffer));
  seed = (unsigned char *)omni_arena_alloc(&arena, 1u, 1u);
  check(seed != NULL, "unaligned seed byte succeeds");
  ptr = (unsigned char *)omni_arena_alloc(&arena, 8u, 8u);
  check(ptr != NULL && ptr_aligned(ptr, 8u), "aligned follow-up lands aligned");
  /* Base-independent invariant: used equals the payload offset plus size. */
  check(omni_arena_used(&arena) == (size_t)(ptr - buffer) + 8u,
        "padding counts against capacity");
  omni_arena_destroy(&arena);
}

static void test_misaligned_backing(void) {
  struct omni_arena arena;
  unsigned char raw[80];
  unsigned char *ptr = NULL;

  /* Deliberately odd backing: alignment must come from the address, never
   * be assumed from the offset. */
  check(omni_arena_init_borrowed(&arena, raw + 1u, 72u), "odd backing accepted");
  ptr = (unsigned char *)omni_arena_alloc(&arena, 8u, 8u);
  check(ptr != NULL && ptr_aligned(ptr, 8u), "odd backing still yields aligned pointers");
  check(omni_arena_used(&arena) == (size_t)(ptr - (raw + 1u)) + 8u,
        "odd-backing padding accounted");
  check(omni_arena_used(&arena) >= 8u && omni_arena_used(&arena) <= 15u,
        "odd-backing padding bounded by alignment");
  omni_arena_destroy(&arena);
}

static void test_invalid_alignment(void) {
  static const size_t bad[] = {0u, 3u, 6u, 24u};
  struct omni_arena arena;
  unsigned char buffer[64];
  size_t i = 0;

  omni_arena_init_borrowed(&arena, buffer, sizeof(buffer));
  for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    if (omni_arena_alloc(&arena, 8u, bad[i]) != NULL) {
      check(false, "zero and non-power-of-two alignments rejected");
      omni_arena_destroy(&arena);
      return;
    }
  }
  check(omni_arena_used(&arena) == 0u, "rejected alignments consume nothing");
  check(true, "zero and non-power-of-two alignments rejected");
  omni_arena_destroy(&arena);
}

/* ------------------------------------------------------- overflow safety */

static void test_size_max_fails_safely(void) {
  struct omni_arena arena;
  unsigned char buffer[64];
  size_t used_before = 0;

  omni_arena_init_borrowed(&arena, buffer, sizeof(buffer));
  check(omni_arena_alloc(&arena, 32u, 1u) != NULL, "seed allocation succeeds");
  used_before = omni_arena_used(&arena);
  /* No giant buffer is ever attempted: each call must fail on bounds alone. */
  check(omni_arena_alloc(&arena, (size_t)-1, 1u) == NULL, "SIZE_MAX size fails");
  check(omni_arena_alloc(&arena, (size_t)-1 - 5u, 8u) == NULL, "near-SIZE_MAX size fails");
  check(omni_arena_alloc(&arena, 8u, (size_t)1 << 63) == NULL,
        "huge power-of-two alignment fails without overflow");
  check(omni_arena_used(&arena) == used_before, "overflow-path failures preserve state");
  check(omni_arena_remaining(&arena) == 64u - used_before, "remaining preserved");
  omni_arena_destroy(&arena);
}

/* -------------------------------------------------------------- zero size */

static void test_zero_size(void) {
  struct omni_arena arena;
  unsigned char buffer[32];
  void *first = NULL;
  void *second = NULL;

  omni_arena_init_borrowed(&arena, buffer, sizeof(buffer));
  first = omni_arena_alloc(&arena, 0u, 8u);
  second = omni_arena_alloc(&arena, 0u, 8u);
  check(first != NULL, "zero-size allocation returns a position");
  check(first == second, "repeated zero-size allocations share the position");
  check(omni_arena_used(&arena) == 0u, "zero-size allocations consume nothing");
  check(ptr_aligned(first, 8u), "zero-size result honors alignment");
  omni_arena_destroy(&arena);
}

/* ------------------------------------------------------- null/inert arena */

static void test_null_and_inert_arena(void) {
  struct omni_arena arena;
  unsigned char buffer[16];

  check(omni_arena_alloc(NULL, 8u, 1u) == NULL, "alloc on NULL arena fails");
  check(omni_arena_capacity(NULL) == 0u, "capacity on NULL arena is zero");
  check(omni_arena_used(NULL) == 0u, "used on NULL arena is zero");
  check(omni_arena_remaining(NULL) == 0u, "remaining on NULL arena is zero");
  check(omni_arena_high_water(NULL) == 0u, "high-water on NULL arena is zero");
  omni_arena_reset(NULL);
  omni_arena_destroy(NULL);
  check(true, "reset/destroy tolerate NULL");

  omni_arena_init_borrowed(&arena, buffer, sizeof(buffer));
  omni_arena_destroy(&arena);
  check(omni_arena_alloc(&arena, 8u, 1u) == NULL, "alloc on destroyed arena fails");
  check(omni_arena_capacity(&arena) == 0u, "destroyed arena reports zero capacity");
  omni_arena_reset(&arena);
  check(true, "reset tolerates destroyed arenas");
}

/* ----------------------------------------------------------------- destroy */

static void test_borrowed_destroy_keeps_buffer(void) {
  struct omni_arena arena;
  unsigned char buffer[32];
  size_t i = 0;

  for (i = 0; i < sizeof(buffer); ++i) {
    buffer[i] = (unsigned char)(0xA0u + i);
  }
  omni_arena_init_borrowed(&arena, buffer, sizeof(buffer));
  check(omni_arena_alloc(&arena, 16u, 1u) != NULL, "borrowed alloc succeeds");
  omni_arena_destroy(&arena);
  for (i = 0; i < sizeof(buffer); ++i) {
    if (i < 16u) {
      continue; /* written region: arena-owned logically, buffer still ours */
    }
    if (buffer[i] != (unsigned char)(0xA0u + i)) {
      check(false, "destroy never touches borrowed storage");
      return;
    }
  }
  check(true, "destroy never touches borrowed storage");
}

static void test_repeated_destroy_safe(void) {
  struct omni_arena borrowed;
  struct omni_arena owned;
  unsigned char buffer[32];

  omni_arena_init_borrowed(&borrowed, buffer, sizeof(buffer));
  omni_arena_destroy(&borrowed);
  omni_arena_destroy(&borrowed);
  check(true, "repeated destroy of borrowed arena is safe");

  omni_arena_init_owned(&owned, 64u);
  omni_arena_destroy(&owned);
  omni_arena_destroy(&owned);
  check(true, "repeated destroy of owned arena is safe (ASan proves single free)");
}

/* --------------------------------------------------------------- zeroed */

static void test_alloc_zeroed(void) {
  struct omni_arena arena;
  unsigned char buffer[64];
  unsigned char *ptr = NULL;
  size_t i = 0;

  omni_arena_init_borrowed(&arena, buffer, sizeof(buffer));
  memset(buffer, 0xFF, sizeof(buffer));
  ptr = (unsigned char *)omni_arena_alloc_zeroed(&arena, 24u, 8u);
  check(ptr != NULL, "zeroed allocation succeeds");
  for (i = 0; i < 24u; ++i) {
    if (ptr[i] != 0u) {
      check(false, "zeroed allocation returns cleared memory");
      omni_arena_destroy(&arena);
      return;
    }
  }
  check(true, "zeroed allocation returns cleared memory");
  check(omni_arena_used(&arena) >= 24u, "zeroed allocation consumes capacity");
  omni_arena_destroy(&arena);
}

/* ---------------------------------------------- deterministic stress test */

static size_t pseudo_size(size_t step) {
  return ((step * 37u + 13u) % 91u) + 1u;
}

static void test_deterministic_stress(void) {
  static const size_t aligns[] = {1u, 2u, 4u, 8u, 16u};
  struct omni_arena arena;
  unsigned char buffer[1024];
  const unsigned char *starts[320];
  size_t sizes[320];
  size_t successes = 0;
  size_t step = 0;
  size_t i = 0;
  size_t j = 0;

  omni_arena_init_borrowed(&arena, buffer, sizeof(buffer));
  for (step = 0; step < 320u; ++step) {
    size_t size = (step % 7u == 6u) ? ((size_t)-1 / 2u) : pseudo_size(step);
    size_t align = aligns[step % (sizeof(aligns) / sizeof(aligns[0]))];
    size_t used_before = omni_arena_used(&arena);
    unsigned char *ptr = (unsigned char *)omni_arena_alloc(&arena, size, align);

    /* Per-step invariants, success or failure. */
    if (omni_arena_used(&arena) > omni_arena_capacity(&arena) ||
        omni_arena_remaining(&arena) != omni_arena_capacity(&arena) - omni_arena_used(&arena)) {
      check(false, "stress invariants hold every step");
      omni_arena_destroy(&arena);
      return;
    }
    if (ptr == NULL) {
      if (omni_arena_used(&arena) != used_before) {
        check(false, "failed allocations preserve accounting");
        omni_arena_destroy(&arena);
        return;
      }
      continue;
    }
    if (!ptr_aligned(ptr, align) || !within_backing(buffer, sizeof(buffer), ptr, size)) {
      check(false, "successful allocations land aligned inside backing");
      omni_arena_destroy(&arena);
      return;
    }
    for (i = 0; i < successes; ++i) {
      const unsigned char *a_end = starts[i] + sizes[i];
      const unsigned char *b_end = ptr + size;
      if (size != 0u && sizes[i] != 0u && !(a_end <= ptr || b_end <= starts[i])) {
        check(false, "successful allocations never overlap");
        omni_arena_destroy(&arena);
        return;
      }
    }
    starts[successes] = ptr;
    sizes[successes] = size;
    ++successes;
    if (step == 160u) {
      omni_arena_reset(&arena);
      successes = 0;
    }
  }
  check(successes > 0u, "deterministic stress keeps invariants (with mid-run reset)");
  for (i = 0; i < successes; ++i) {
    for (j = i + 1u; j < successes; ++j) {
      const unsigned char *a_end = starts[i] + sizes[i];
      const unsigned char *b_end = starts[j] + sizes[j];
      if (sizes[i] != 0u && sizes[j] != 0u && !(a_end <= starts[j] || b_end <= starts[i])) {
        check(false, "post-reset allocations never overlap");
        omni_arena_destroy(&arena);
        return;
      }
    }
  }
  check(true, "post-reset allocations never overlap");
  omni_arena_destroy(&arena);
}

int main(void) {
  test_init_borrowed_valid();
  test_init_rejects_invalid();
  test_init_owned_valid();
  test_first_and_sequential_allocs();
  test_exact_capacity_and_overflow();
  test_reset_reuse();
  test_reset_invalidates_old_pointers();
  test_high_water_survives_reset();
  test_alignments();
  test_padding_accounted();
  test_misaligned_backing();
  test_invalid_alignment();
  test_size_max_fails_safely();
  test_zero_size();
  test_null_and_inert_arena();
  test_borrowed_destroy_keeps_buffer();
  test_repeated_destroy_safe();
  test_alloc_zeroed();
  test_deterministic_stress();

  printf("---\n%d checks, %d failures\n", check_count, failure_count);
  return failure_count == 0 ? 0 : 1;
}
