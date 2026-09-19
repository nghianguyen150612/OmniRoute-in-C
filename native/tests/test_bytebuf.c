/*
 * OmniRoute native backend — bounded reusable byte buffer tests (Task 013).
 *
 * Self-contained, deterministic, framework-free: a fixed sequence of checks
 * with TAP-style output and a nonzero exit on any failure. No randomness, no
 * threads, no network, no giant buffers (SIZE_MAX edge cases assert clean
 * failure without attempting real backing). Run under CTest, including the
 * ASan + UBSan configuration, which validates the exactly-once owned-free,
 * the absence of overflow UB, and (via the leak check at exit) that
 * steady-state operations perform no per-operation heap allocation.
 *
 * Structural no-growth argument: src/bytebuf.c contains exactly one malloc
 * (init_owned) and one free (destroy); every other operation is documented
 * allocation-free, and this suite drives thousands of steady-state
 * operations through ASan LeakSanitizer — any hidden per-op allocation
 * left reachable, or any double-free, fails the sanitizer run.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "omniroute/bytebuf.h"

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

static bool within_backing(const unsigned char *backing, size_t capacity,
                           const unsigned char *ptr, size_t size) {
  return ptr >= backing && size <= capacity && ptr + size <= backing + capacity;
}

/* Snapshot the full public state for preserve-on-failure assertions. */
struct buf_state {
  size_t capacity;
  size_t readable;
  size_t writable;
  size_t reclaimable;
  size_t high_water;
};

static struct buf_state snapshot(const struct omni_bytebuf *buf) {
  struct buf_state s;

  s.capacity = omni_bytebuf_capacity(buf);
  s.readable = omni_bytebuf_readable(buf);
  s.writable = omni_bytebuf_writable(buf);
  s.reclaimable = omni_bytebuf_reclaimable(buf);
  s.high_water = omni_bytebuf_high_water(buf);
  return s;
}

static bool same_state(struct buf_state a, struct buf_state b) {
  return a.capacity == b.capacity && a.readable == b.readable &&
         a.writable == b.writable && a.reclaimable == b.reclaimable &&
         a.high_water == b.high_water;
}

/* ------------------------------------------------------------------- init */

static void test_init_borrowed_valid(void) {
  struct omni_bytebuf buf;
  unsigned char storage[64];

  check(omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage)),
        "borrowed init accepts valid storage");
  check(omni_bytebuf_capacity(&buf) == 64u, "capacity matches storage");
  check(omni_bytebuf_readable(&buf) == 0u, "initial readable is zero");
  check(omni_bytebuf_writable(&buf) == 64u, "initial writable equals capacity");
  check(omni_bytebuf_reclaimable(&buf) == 0u, "initial reclaimable is zero");
  check(omni_bytebuf_high_water(&buf) == 0u, "initial high-water is zero");
  omni_bytebuf_destroy(&buf);
}

static void test_init_owned_valid(void) {
  struct omni_bytebuf buf;

  check(omni_bytebuf_init_owned(&buf, 128u), "owned init accepts nonzero capacity");
  check(omni_bytebuf_capacity(&buf) == 128u, "owned capacity matches request");
  check(omni_bytebuf_readable(&buf) == 0u, "owned buffer starts unreadable-empty");
  check(omni_bytebuf_writable(&buf) == 128u, "owned buffer starts fully writable");
  omni_bytebuf_destroy(&buf);
}

static void test_init_rejects_invalid(void) {
  struct omni_bytebuf buf;
  unsigned char storage[16];

  check(!omni_bytebuf_init_borrowed(NULL, storage, sizeof(storage)),
        "borrowed init rejects NULL buffer");
  check(!omni_bytebuf_init_borrowed(&buf, NULL, sizeof(storage)),
        "borrowed init rejects NULL storage");
  check(!omni_bytebuf_init_borrowed(&buf, storage, 0u),
        "borrowed init rejects zero capacity");
  check(omni_bytebuf_capacity(&buf) == 0u, "rejected borrowed buffer stays inert");
  check(!omni_bytebuf_init_owned(NULL, 64u), "owned init rejects NULL buffer");
  check(!omni_bytebuf_init_owned(&buf, 0u), "owned init rejects zero capacity");
  check(omni_bytebuf_capacity(&buf) == 0u, "rejected owned buffer stays inert");
  omni_bytebuf_destroy(&buf);
}

/* --------------------------------------------------------------- append */

static void test_append_and_readable_view(void) {
  struct omni_bytebuf buf;
  unsigned char storage[64];
  size_t len = 0;
  const unsigned char *view = NULL;
  static const unsigned char payload[] = { 'h', 'e', 'l', 'l', 'o' };

  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  check(omni_bytebuf_append(&buf, payload, sizeof(payload)), "append bytes succeeds");
  check(omni_bytebuf_readable(&buf) == sizeof(payload), "readable counts appended bytes");
  check(omni_bytebuf_writable(&buf) == sizeof(storage) - sizeof(payload),
        "writable shrinks by appended bytes");
  view = omni_bytebuf_read_ptr(&buf, &len);
  check(view != NULL && len == sizeof(payload), "readable view reports contents length");
  check(view == storage, "readable view is zero-copy at backing start");
  check(memcmp(view, payload, sizeof(payload)) == 0, "readable view contents match append");
  omni_bytebuf_destroy(&buf);
}

static void test_binary_data_with_nul(void) {
  struct omni_bytebuf buf;
  unsigned char storage[32];
  size_t len = 0;
  const unsigned char *view = NULL;
  /* Embedded zeros, high bytes, no terminator anywhere in the contract. */
  static const unsigned char payload[] = { 0x00u, 0xFFu, 0x00u, 0x41u,
                                           0x00u, 0x80u, 0x7Fu, 0x00u };

  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  check(omni_bytebuf_append(&buf, payload, sizeof(payload)),
        "binary append with embedded NUL succeeds");
  view = omni_bytebuf_read_ptr(&buf, &len);
  check(view != NULL && len == sizeof(payload), "binary readable length is exact");
  check(memcmp(view, payload, sizeof(payload)) == 0,
        "binary round-trip preserves embedded zeros");
  omni_bytebuf_destroy(&buf);
}

static void test_append_exact_and_beyond(void) {
  struct omni_bytebuf buf;
  unsigned char storage[16];
  struct buf_state before;
  unsigned char full[16];
  unsigned char extra = 0xAAu;
  size_t i = 0;

  for (i = 0; i < sizeof(full); ++i) {
    full[i] = (unsigned char)(0x30u + i);
  }
  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  check(omni_bytebuf_append(&buf, full, sizeof(full)),
        "append of exact remaining capacity succeeds");
  check(omni_bytebuf_readable(&buf) == 16u, "exact fill is fully readable");
  check(omni_bytebuf_writable(&buf) == 0u, "exact fill leaves no tail");
  before = snapshot(&buf);
  check(!omni_bytebuf_append(&buf, &extra, 1u), "append beyond capacity fails");
  check(same_state(before, snapshot(&buf)), "failed append leaves state unchanged");
  check(memcmp(storage, full, sizeof(full)) == 0, "failed append leaves bytes unchanged");
  omni_bytebuf_destroy(&buf);
}

static void test_zero_length_and_null_src(void) {
  struct omni_bytebuf buf;
  unsigned char storage[16];
  struct buf_state before;
  static const unsigned char payload[] = { 'x', 'y' };

  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  omni_bytebuf_append(&buf, payload, sizeof(payload));
  before = snapshot(&buf);
  check(omni_bytebuf_append(&buf, payload, 0u), "zero-length append succeeds");
  check(omni_bytebuf_append(&buf, NULL, 0u), "NULL source with zero length succeeds");
  check(same_state(before, snapshot(&buf)), "zero-length appends change nothing");
  check(!omni_bytebuf_append(&buf, NULL, 1u), "NULL source with nonzero length fails");
  check(same_state(before, snapshot(&buf)), "NULL-source failure preserves state");
  check(!omni_bytebuf_append(NULL, payload, 1u), "append on NULL buffer fails");
  omni_bytebuf_destroy(&buf);
}

/* ---------------------------------------------------------- writable view */

static void test_writable_view(void) {
  struct omni_bytebuf buf;
  unsigned char storage[16];
  unsigned char *wptr = NULL;
  size_t wlen = 0;
  static const unsigned char payload[] = { 'a', 'b', 'c', 'd' };

  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  wptr = omni_bytebuf_write_ptr(&buf, &wlen);
  check(wptr != NULL && wlen == 16u, "fresh writable view spans full capacity");
  check(wptr == storage, "writable view is zero-copy at backing start");
  check(within_backing(storage, sizeof(storage), wptr, wlen),
        "writable view stays inside backing");

  omni_bytebuf_append(&buf, payload, sizeof(payload));
  wptr = omni_bytebuf_write_ptr(&buf, &wlen);
  check(wptr == storage + sizeof(payload) && wlen == sizeof(storage) - sizeof(payload),
        "writable view advances past appended bytes");

  /* NULL out_len still yields the pointer. */
  wptr = omni_bytebuf_write_ptr(&buf, NULL);
  check(wptr == storage + sizeof(payload), "writable view tolerates NULL out_len");

  /* Fill exactly, then the full buffer reports NULL + zero. */
  {
    size_t tail = omni_bytebuf_writable(&buf);
    unsigned char *fill = omni_bytebuf_write_ptr(&buf, NULL);

    memset(fill, 0x5Au, tail);
    check(omni_bytebuf_commit(&buf, tail), "commit of exact tail succeeds (setup)");
  }
  wptr = omni_bytebuf_write_ptr(&buf, &wlen);
  check(wptr == NULL && wlen == 0u, "full buffer writable view is NULL with zero length");
  check(omni_bytebuf_write_ptr(NULL, &wlen) == NULL, "writable view on NULL buffer is NULL");
  omni_bytebuf_destroy(&buf);
}

/* ----------------------------------------------------------------- commit */

static void test_commit(void) {
  struct omni_bytebuf buf;
  unsigned char storage[16];
  struct buf_state before;
  unsigned char *wptr = NULL;
  size_t wlen = 0;
  size_t len = 0;
  const unsigned char *view = NULL;

  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  wptr = omni_bytebuf_write_ptr(&buf, &wlen);
  memcpy(wptr, "hello", 5u);
  check(omni_bytebuf_commit(&buf, 5u), "commit of valid produced length succeeds");
  check(omni_bytebuf_readable(&buf) == 5u, "commit grows readable");
  view = omni_bytebuf_read_ptr(&buf, &len);
  check(view != NULL && len == 5u && memcmp(view, "hello", 5u) == 0,
        "committed bytes are readable");

  before = snapshot(&buf);
  check(!omni_bytebuf_commit(&buf, wlen), "commit beyond writable tail fails");
  check(same_state(before, snapshot(&buf)), "failed commit preserves state");
  check(!omni_bytebuf_commit(NULL, 1u), "commit on NULL buffer fails");
  check(!omni_bytebuf_commit(&buf, (size_t)-1), "commit of SIZE_MAX fails safely");
  check(same_state(before, snapshot(&buf)), "SIZE_MAX commit preserves state");

  before = snapshot(&buf);
  check(omni_bytebuf_commit(&buf, 0u), "zero commit is a successful no-op");
  check(same_state(before, snapshot(&buf)), "zero commit changes nothing");
  omni_bytebuf_destroy(&buf);
}

/* ---------------------------------------------------------------- consume */

static void test_consume_partial(void) {
  struct omni_bytebuf buf;
  unsigned char storage[32];
  size_t len = 0;
  const unsigned char *view = NULL;

  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  omni_bytebuf_append(&buf, "0123456789", 10u);
  check(omni_bytebuf_consume(&buf, 4u), "consume of readable prefix succeeds");
  check(omni_bytebuf_readable(&buf) == 6u, "partial consume shrinks readable");
  check(omni_bytebuf_reclaimable(&buf) == 4u, "partial consume exposes reclaimable prefix");
  view = omni_bytebuf_read_ptr(&buf, &len);
  check(view == storage + 4u && len == 6u, "readable view advances past consumed bytes");
  check(memcmp(view, "456789", 6u) == 0, "unread bytes keep order after consume");
  omni_bytebuf_destroy(&buf);
}

static void test_consume_exact_canonical_empty(void) {
  struct omni_bytebuf buf;
  unsigned char storage[32];
  size_t rlen = 0;
  size_t wlen = 0;

  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  omni_bytebuf_append(&buf, "abcdefgh", 8u);
  omni_bytebuf_consume(&buf, 3u);
  check(omni_bytebuf_consume(&buf, 5u), "consume of exact readable amount succeeds");
  check(omni_bytebuf_readable(&buf) == 0u, "full consume empties readable");
  check(omni_bytebuf_reclaimable(&buf) == 0u, "full consume leaves no prefix");
  check(omni_bytebuf_writable(&buf) == 32u, "full consume restores canonical empty state");
  check(omni_bytebuf_read_ptr(&buf, &rlen) == NULL && rlen == 0u,
        "empty readable view is NULL with zero length");
  check(omni_bytebuf_write_ptr(&buf, &wlen) == storage && wlen == 32u,
        "canonical empty writable view restarts at backing");
  omni_bytebuf_destroy(&buf);
}

static void test_consume_beyond_fails(void) {
  struct omni_bytebuf buf;
  unsigned char storage[32];
  struct buf_state before;

  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  omni_bytebuf_append(&buf, "12345", 5u);
  before = snapshot(&buf);
  check(!omni_bytebuf_consume(&buf, 6u), "consume beyond readable fails");
  check(same_state(before, snapshot(&buf)), "failed consume preserves state");
  check(!omni_bytebuf_consume(&buf, (size_t)-1), "consume of SIZE_MAX fails safely");
  check(same_state(before, snapshot(&buf)), "SIZE_MAX consume preserves state");
  check(!omni_bytebuf_consume(NULL, 1u), "consume on NULL buffer fails");
  check(omni_bytebuf_consume(&buf, 0u), "zero consume is a successful no-op");
  check(same_state(before, snapshot(&buf)), "zero consume changes nothing");

  /* Consuming from an empty buffer fails (except the zero no-op). */
  omni_bytebuf_consume(&buf, 5u);
  before = snapshot(&buf);
  check(!omni_bytebuf_consume(&buf, 1u), "consume from empty buffer fails");
  check(same_state(before, snapshot(&buf)), "empty consume failure preserves state");
  omni_bytebuf_destroy(&buf);
}

/* ------------------------------------------- no automatic prefix reclaim */

static void test_prefix_not_auto_reclaimed(void) {
  struct omni_bytebuf buf;
  unsigned char storage[16];
  struct buf_state before;

  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  omni_bytebuf_append(&buf, "0123456789ABCDEF", 16u);
  omni_bytebuf_consume(&buf, 12u); /* 4 readable, 12 prefix, 0 tail */
  check(omni_bytebuf_writable(&buf) == 0u, "consumed prefix is not tail yet");
  before = snapshot(&buf);
  check(!omni_bytebuf_append(&buf, "Z", 1u),
        "append fails despite logical free space (no auto-compact)");
  check(same_state(before, snapshot(&buf)), "tail-only admission preserves state");
  omni_bytebuf_destroy(&buf);
}

/* -------------------------------------------------------------- compaction */

static void test_compact(void) {
  struct omni_bytebuf buf;
  unsigned char storage[16];
  size_t len = 0;
  const unsigned char *view = NULL;

  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  omni_bytebuf_append(&buf, "0123456789ABCDEF", 16u);
  omni_bytebuf_consume(&buf, 10u); /* unread "ABCDEF" at [10,16) */
  check(omni_bytebuf_reclaimable(&buf) == 10u, "prefix is reclaimable before compact");
  omni_bytebuf_compact(&buf);
  check(omni_bytebuf_reclaimable(&buf) == 0u, "compact clears the prefix");
  check(omni_bytebuf_readable(&buf) == 6u, "compact keeps readable count");
  check(omni_bytebuf_writable(&buf) == 10u, "compact restores writable tail");
  view = omni_bytebuf_read_ptr(&buf, &len);
  check(view == storage && len == 6u, "compact moves unread bytes to backing start");
  check(memcmp(view, "ABCDEF", 6u) == 0, "compaction preserves unread byte order");

  /* Post-compact append proves the recovered tail is usable in order. */
  omni_bytebuf_append(&buf, "0123456789", 10u);
  view = omni_bytebuf_read_ptr(&buf, &len);
  check(len == 16u && memcmp(view, "ABCDEF0123456789", 16u) == 0,
        "append after compact extends in order to full capacity");
  omni_bytebuf_destroy(&buf);
}

static void test_compact_noop_cases(void) {
  struct omni_bytebuf buf;
  unsigned char storage[16];
  struct buf_state before;

  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  omni_bytebuf_compact(&buf);
  check(omni_bytebuf_writable(&buf) == 16u, "compact on empty buffer stays empty");

  omni_bytebuf_append(&buf, "abcd", 4u);
  before = snapshot(&buf);
  omni_bytebuf_compact(&buf);
  check(same_state(before, snapshot(&buf)), "compact on already-compact buffer is a no-op");

  /* Empty with a consumed prefix canonicalizes without moving bytes. */
  omni_bytebuf_append(&buf, "efghijkl", 8u);
  omni_bytebuf_consume(&buf, 12u);
  omni_bytebuf_compact(&buf);
  check(omni_bytebuf_writable(&buf) == 16u && omni_bytebuf_reclaimable(&buf) == 0u,
        "compact on drained buffer canonicalizes to empty");

  omni_bytebuf_compact(NULL);
  check(true, "compact tolerates NULL");
  omni_bytebuf_destroy(&buf);
}

static void test_overlap_snapshot_semantics(void) {
  struct omni_bytebuf buf;
  unsigned char storage[32];
  size_t i = 0;
  /* Deterministic stale tail so the overlapped snapshot is fully known. */
  static const unsigned char expect[] = { 'C', 'D', 'E', 'F', 0x5Au, 0x5Au, 0x5Au, 0x5Au };
  size_t len = 0;
  const unsigned char *view = NULL;

  memset(storage, 0x5A, sizeof(storage));
  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  omni_bytebuf_append(&buf, "0123456789ABCDEF", 16u);
  /* src [12,20) overlaps dst [16,24): with memmove the destination sees the
   * pre-copy bytes, exactly `expect`. A memcpy implementation could smear
   * the bytes it just wrote into the overlapped region instead. */
  check(omni_bytebuf_append(&buf, storage + 12u, 8u), "overlapping append succeeds");
  view = omni_bytebuf_read_ptr(&buf, &len);
  check(len == 24u, "overlapping append grows readable");
  check(memcmp(view + 16u, expect, sizeof(expect)) == 0,
        "overlapping append has snapshot (memmove) semantics");
  for (i = 0; i < 16u; ++i) {
    if (view[i] != (unsigned char)("0123456789ABCDEF"[i])) {
      check(false, "overlapping append leaves earlier bytes intact");
      omni_bytebuf_destroy(&buf);
      return;
    }
  }
  check(true, "overlapping append leaves earlier bytes intact");
  omni_bytebuf_destroy(&buf);
}

/* ------------------------------------------------------------ reset/reuse */

static void test_reset_and_reuse(void) {
  struct omni_bytebuf buf;
  unsigned char storage[32];
  size_t hw_before = 0;
  size_t len = 0;
  const unsigned char *view = NULL;

  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  omni_bytebuf_append(&buf, "some-data-here", 14u);
  hw_before = omni_bytebuf_high_water(&buf);
  check(hw_before == 14u, "high-water tracks appended readable (setup)");
  omni_bytebuf_reset(&buf);
  check(omni_bytebuf_readable(&buf) == 0u, "reset empties readable");
  check(omni_bytebuf_writable(&buf) == 32u, "reset restores full writable tail");
  check(omni_bytebuf_reclaimable(&buf) == 0u, "reset clears the prefix");
  check(omni_bytebuf_capacity(&buf) == 32u, "reset retains capacity");
  check(omni_bytebuf_high_water(&buf) == hw_before, "reset preserves high-water");

  check(omni_bytebuf_append(&buf, "reused-capacity!!", 16u), "reuse after reset succeeds");
  view = omni_bytebuf_read_ptr(&buf, &len);
  check(view == storage && len == 16u, "reuse after reset restarts at backing");
  check(memcmp(view, "reused-capacity!!", 16u) == 0, "reused contents are exact");
  omni_bytebuf_reset(NULL);
  check(true, "reset tolerates NULL");
  omni_bytebuf_destroy(&buf);
}

static void test_deterministic_reuse_after_consume_compact(void) {
  struct omni_bytebuf buf;
  unsigned char storage[64];
  size_t len = 0;
  const unsigned char *view = NULL;
  unsigned char second[32];
  size_t i = 0;

  for (i = 0; i < sizeof(second); ++i) {
    second[i] = (unsigned char)(0x80u + i);
  }
  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  /* 48 byte first fill with a recognizable pattern. */
  for (i = 0; i < 48u; ++i) {
    unsigned char v = (unsigned char)i;

    check(omni_bytebuf_append(&buf, &v, 1u), "reuse-cycle fill append succeeds");
  }
  check(omni_bytebuf_consume(&buf, 16u), "reuse-cycle consume succeeds");
  omni_bytebuf_compact(&buf);
  check(omni_bytebuf_append(&buf, second, sizeof(second)),
        "reuse-cycle append fits recovered tail exactly");
  check(omni_bytebuf_readable(&buf) == 64u, "reuse cycle ends exactly full");
  view = omni_bytebuf_read_ptr(&buf, &len);
  for (i = 0; i < 32u; ++i) {
    if (view[i] != (unsigned char)(16u + i)) {
      check(false, "reuse cycle preserves original order");
      omni_bytebuf_destroy(&buf);
      return;
    }
  }
  check(true, "reuse cycle preserves original order");
  check(memcmp(view + 32u, second, sizeof(second)) == 0,
        "reuse cycle appends second fill in order");
  omni_bytebuf_destroy(&buf);
}

/* ------------------------------------------------------------ ownership */

static void test_borrowed_never_freed(void) {
  struct omni_bytebuf buf;
  unsigned char storage[32];
  size_t i = 0;

  for (i = 0; i < sizeof(storage); ++i) {
    storage[i] = (unsigned char)(0xA0u + i);
  }
  check(omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage)),
        "borrowed lifecycle init succeeds");
  check(omni_bytebuf_append(&buf, "0123456789ABCDEF", 16u),
        "borrowed lifecycle append succeeds");
  omni_bytebuf_destroy(&buf);
  /* Destroy must not have freed (or cleared) caller storage: the unwritten
   * tail is bit-exact, and the storage is immediately reusable by us. */
  for (i = 16u; i < sizeof(storage); ++i) {
    if (storage[i] != (unsigned char)(0xA0u + i)) {
      check(false, "destroy never frees or clears borrowed storage");
      return;
    }
  }
  check(true, "destroy never frees or clears borrowed storage");
  memset(storage, 0xCC, sizeof(storage));
  check(storage[0] == 0xCCu, "borrowed storage stays caller-usable after destroy");
}

static void test_owned_lifecycle(void) {
  struct omni_bytebuf buf;
  size_t wlen = 0;
  unsigned char *wptr = NULL;

  check(omni_bytebuf_init_owned(&buf, 64u), "owned lifecycle init succeeds");
  wptr = omni_bytebuf_write_ptr(&buf, &wlen);
  check(wptr != NULL && wlen == 64u, "owned backing is writable end to end");
  memset(wptr, 0xAB, wlen);
  check(omni_bytebuf_commit(&buf, 64u), "owned lifecycle fills to capacity");
  check(omni_bytebuf_readable(&buf) == 64u, "owned lifecycle readable is full");
  omni_bytebuf_destroy(&buf);
  check(omni_bytebuf_capacity(&buf) == 0u, "destroyed owned buffer is inert");
  /* ASan proves the exactly-once free; repeated destroy must stay silent. */
  omni_bytebuf_destroy(&buf);
  check(true, "repeated destroy of owned buffer is safe");
}

static void test_repeated_destroy_borrowed(void) {
  struct omni_bytebuf buf;
  unsigned char storage[16];

  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  omni_bytebuf_destroy(&buf);
  omni_bytebuf_destroy(&buf);
  check(true, "repeated destroy of borrowed buffer is safe");
}

/* ------------------------------------------------------------- high-water */

static void test_high_water(void) {
  struct omni_bytebuf buf;
  unsigned char storage[48];

  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  omni_bytebuf_append(&buf, "0123456789", 10u);
  check(omni_bytebuf_high_water(&buf) == 10u, "high-water follows append growth");
  omni_bytebuf_consume(&buf, 4u);
  check(omni_bytebuf_high_water(&buf) == 10u, "consume never lowers high-water");
  omni_bytebuf_compact(&buf);
  check(omni_bytebuf_high_water(&buf) == 10u, "compact never lowers high-water");
  omni_bytebuf_append(&buf, "ABCDEFGH", 8u); /* readable 6 + 8 = 14 */
  check(omni_bytebuf_readable(&buf) == 14u, "readable reaches new peak (setup)");
  check(omni_bytebuf_high_water(&buf) == 14u, "high-water tracks new readable peak");
  /* Commit path updates high-water too. */
  {
    unsigned char *wptr = omni_bytebuf_write_ptr(&buf, NULL);

    memset(wptr, 0x51, 6u);
    omni_bytebuf_commit(&buf, 6u); /* readable 20 */
  }
  check(omni_bytebuf_high_water(&buf) == 20u, "high-water tracks commit growth");
  /* Failed operations never move high-water. */
  {
    struct buf_state before = snapshot(&buf);
    unsigned char v = 0;

    omni_bytebuf_append(&buf, &v, omni_bytebuf_writable(&buf) + 1u);
    check(omni_bytebuf_high_water(&buf) == before.high_water,
          "failed append leaves high-water unchanged");
  }
  omni_bytebuf_destroy(&buf);
}

/* ---------------------------------------------------------- overflow-safe */

static void test_size_max_fails_safely(void) {
  struct omni_bytebuf buf;
  unsigned char storage[64];
  struct buf_state before;
  unsigned char seed = 0x5Eu;

  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  check(omni_bytebuf_append(&buf, &seed, 1u), "seed append succeeds");
  before = snapshot(&buf);
  /* No giant buffer is ever attempted: each call must fail on bounds alone
   * via subtraction-first checks (len > capacity - write). */
  check(!omni_bytebuf_append(&buf, &seed, (size_t)-1), "append of SIZE_MAX fails");
  check(!omni_bytebuf_append(&buf, &seed, (size_t)-1 - 5u), "append near SIZE_MAX fails");
  check(!omni_bytebuf_commit(&buf, (size_t)-1), "commit of SIZE_MAX fails");
  check(!omni_bytebuf_consume(&buf, (size_t)-1), "consume of SIZE_MAX fails");
  check(same_state(before, snapshot(&buf)), "overflow-path failures preserve state");
  check(omni_bytebuf_readable(&buf) == 1u, "seed byte still readable after overflow attempts");
  omni_bytebuf_destroy(&buf);
}

/* ---------------------------------------------------------- null and inert */

static void test_null_and_inert(void) {
  struct omni_bytebuf buf;
  struct omni_bytebuf zeroed;
  unsigned char storage[16];
  size_t len = 99u;

  memset(&zeroed, 0, sizeof(zeroed)); /* never initialized: inert by construction */
  check(omni_bytebuf_capacity(NULL) == 0u, "capacity on NULL is zero");
  check(omni_bytebuf_readable(NULL) == 0u, "readable on NULL is zero");
  check(omni_bytebuf_writable(NULL) == 0u, "writable on NULL is zero");
  check(omni_bytebuf_reclaimable(NULL) == 0u, "reclaimable on NULL is zero");
  check(omni_bytebuf_high_water(NULL) == 0u, "high-water on NULL is zero");
  check(omni_bytebuf_read_ptr(NULL, &len) == NULL && len == 0u,
        "readable view on NULL is NULL with zero length");
  check(omni_bytebuf_read_ptr(NULL, NULL) == NULL, "readable view tolerates NULL out_len");
  check(omni_bytebuf_capacity(&zeroed) == 0u, "capacity on inert buffer is zero");
  check(omni_bytebuf_read_ptr(&zeroed, &len) == NULL, "readable view on inert is NULL");
  check(omni_bytebuf_write_ptr(&zeroed, &len) == NULL && len == 0u,
        "writable view on inert is NULL with zero length");
  check(!omni_bytebuf_append(&zeroed, "x", 1u), "append on inert buffer fails");
  check(!omni_bytebuf_commit(&zeroed, 1u), "commit on inert buffer fails");
  check(!omni_bytebuf_consume(&zeroed, 1u), "consume on inert buffer fails");
  omni_bytebuf_reset(&zeroed);
  omni_bytebuf_compact(&zeroed);
  omni_bytebuf_destroy(&zeroed);
  check(true, "reset/compact/destroy tolerate inert buffers");
  omni_bytebuf_destroy(NULL);
  check(true, "destroy tolerates NULL");

  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  omni_bytebuf_destroy(&buf);
  check(omni_bytebuf_append(&buf, "x", 1u) == false, "append on destroyed buffer fails");
  check(omni_bytebuf_capacity(&buf) == 0u, "destroyed buffer reports zero capacity");
  omni_bytebuf_reset(&buf);
  check(true, "reset tolerates destroyed buffers");
}

/* ----------------------------------------------- deterministic stress test */

static size_t pseudo_size(size_t step) {
  return ((step * 37u + 13u) % 91u) + 1u;
}

static void test_deterministic_stress(void) {
  struct omni_bytebuf buf;
  unsigned char storage[256];
  /* Reference model: the readable region must always equal this byte run. */
  size_t model_readable = 0;
  size_t expected_base = 0; /* value of first readable byte */
  size_t produced = 0;      /* next byte value to emit */
  size_t step = 0;
  size_t i = 0;

  omni_bytebuf_init_borrowed(&buf, storage, sizeof(storage));
  for (step = 0; step < 400u; ++step) {
    struct buf_state before = snapshot(&buf);
    size_t want = pseudo_size(step);
    size_t op = step % 6u;

    if (op == 0u || op == 3u) {
      /* Append path (op 3 uses view + commit instead). */
      unsigned char tmp[96];
      size_t n = want > sizeof(tmp) ? sizeof(tmp) : want;
      size_t k = 0;

      for (k = 0; k < n; ++k) {
        tmp[k] = (unsigned char)((produced + k) & 0xFFu);
      }
      if (op == 0u) {
        if (omni_bytebuf_append(&buf, tmp, n)) {
          produced += n;
          model_readable += n;
        } else {
          /* Must have failed for lack of tail only; state preserved. */
          if (!(n > before.writable) || !same_state(before, snapshot(&buf))) {
            check(false, "stress append fails only on short tail, preserving state");
            omni_bytebuf_destroy(&buf);
            return;
          }
          /* Recover deterministically: drain half, compact, retry once. */
          if (model_readable > 0u) {
            size_t drain = model_readable / 2u;

            omni_bytebuf_consume(&buf, drain);
            model_readable -= drain;
            expected_base += drain;
          }
          omni_bytebuf_compact(&buf);
          if (omni_bytebuf_append(&buf, tmp, n)) {
            produced += n;
            model_readable += n;
          }
        }
      } else {
        size_t tail = 0;
        unsigned char *wptr = omni_bytebuf_write_ptr(&buf, &tail);
        size_t fill = n > tail ? tail : n;

        if (wptr == NULL) {
          fill = 0;
        } else {
          for (k = 0; k < fill; ++k) {
            wptr[k] = (unsigned char)((produced + k) & 0xFFu);
          }
        }
        if (!omni_bytebuf_commit(&buf, fill)) {
          check(false, "stress commit of view-clamped length succeeds");
          omni_bytebuf_destroy(&buf);
          return;
        }
        produced += fill;
        model_readable += fill;
        /* A deliberately over-wide commit must fail preserving state. */
        before = snapshot(&buf);
        if (omni_bytebuf_commit(&buf, omni_bytebuf_writable(&buf) + 1u)) {
          check(false, "stress over-commit fails");
          omni_bytebuf_destroy(&buf);
          return;
        }
        if (!same_state(before, snapshot(&buf))) {
          check(false, "stress over-commit preserves state");
          omni_bytebuf_destroy(&buf);
          return;
        }
      }
    } else if (op == 1u || op == 4u) {
      /* Consume path, including a provoked over-consume. */
      size_t take = want % (model_readable + 2u);

      if (take <= model_readable) {
        if (!omni_bytebuf_consume(&buf, take)) {
          check(false, "stress consume within readable succeeds");
          omni_bytebuf_destroy(&buf);
          return;
        }
        model_readable -= take;
        expected_base += take;
        if (model_readable == 0u) {
          expected_base = produced; /* canonical empty: base rejoins head */
        }
      }
      before = snapshot(&buf);
      if (omni_bytebuf_consume(&buf, model_readable + 1u)) {
        check(false, "stress over-consume fails");
        omni_bytebuf_destroy(&buf);
        return;
      }
      if (!same_state(before, snapshot(&buf))) {
        check(false, "stress over-consume preserves state");
        omni_bytebuf_destroy(&buf);
        return;
      }
    } else if (op == 2u) {
      omni_bytebuf_compact(&buf);
    } else {
      /* op == 5: periodic reset keeps the lifetime bounded. */
      if (step % 30u == 29u) {
        size_t hw = omni_bytebuf_high_water(&buf);

        omni_bytebuf_reset(&buf);
        model_readable = 0;
        expected_base = produced;
        if (omni_bytebuf_high_water(&buf) != hw) {
          check(false, "stress reset preserves high-water");
          omni_bytebuf_destroy(&buf);
          return;
        }
      }
    }

    /* Per-step public invariants. */
    if (omni_bytebuf_readable(&buf) != model_readable ||
        omni_bytebuf_readable(&buf) + omni_bytebuf_writable(&buf) +
                omni_bytebuf_reclaimable(&buf) !=
            omni_bytebuf_capacity(&buf)) {
      check(false, "stress accounting partitions capacity every step");
      omni_bytebuf_destroy(&buf);
      return;
    }
    if (omni_bytebuf_capacity(&buf) != sizeof(storage)) {
      check(false, "stress capacity never changes");
      omni_bytebuf_destroy(&buf);
      return;
    }
    {
      size_t rlen = 0;
      size_t wlen = 0;
      const unsigned char *rptr = omni_bytebuf_read_ptr(&buf, &rlen);
      unsigned char *wptr = omni_bytebuf_write_ptr(&buf, &wlen);

      if (rlen != model_readable || wlen != omni_bytebuf_writable(&buf)) {
        check(false, "stress view lengths match accounting");
        omni_bytebuf_destroy(&buf);
        return;
      }
      if (model_readable > 0u) {
        if (rptr == NULL ||
            !within_backing(storage, sizeof(storage), rptr, rlen)) {
          check(false, "stress readable view stays inside backing");
          omni_bytebuf_destroy(&buf);
          return;
        }
        if (rptr[0] != (unsigned char)(expected_base & 0xFFu) ||
            rptr[rlen - 1u] != (unsigned char)((produced - 1u) & 0xFFu)) {
          check(false, "stress byte order holds end to end");
          omni_bytebuf_destroy(&buf);
          return;
        }
      } else if (rptr != NULL) {
        check(false, "stress empty readable view is NULL");
        omni_bytebuf_destroy(&buf);
        return;
      }
      if (wlen > 0u) {
        if (wptr == NULL ||
            !within_backing(storage, sizeof(storage), wptr, wlen)) {
          check(false, "stress writable view stays inside backing");
          omni_bytebuf_destroy(&buf);
          return;
        }
      }
    }
  }
  check(model_readable == omni_bytebuf_readable(&buf),
        "deterministic stress keeps invariants across 400 mixed ops");
  /* Final full-content sweep: every remaining byte in emission order. */
  {
    size_t rlen = 0;
    const unsigned char *rptr = omni_bytebuf_read_ptr(&buf, &rlen);

    for (i = 0; i < rlen; ++i) {
      if (rptr[i] != (unsigned char)((expected_base + i) & 0xFFu)) {
        check(false, "stress final bytes match emission order");
        omni_bytebuf_destroy(&buf);
        return;
      }
    }
  }
  check(true, "stress final bytes match emission order");
  omni_bytebuf_destroy(&buf);
}

int main(void) {
  test_init_borrowed_valid();
  test_init_owned_valid();
  test_init_rejects_invalid();
  test_append_and_readable_view();
  test_binary_data_with_nul();
  test_append_exact_and_beyond();
  test_zero_length_and_null_src();
  test_writable_view();
  test_commit();
  test_consume_partial();
  test_consume_exact_canonical_empty();
  test_consume_beyond_fails();
  test_prefix_not_auto_reclaimed();
  test_compact();
  test_compact_noop_cases();
  test_overlap_snapshot_semantics();
  test_reset_and_reuse();
  test_deterministic_reuse_after_consume_compact();
  test_borrowed_never_freed();
  test_owned_lifecycle();
  test_repeated_destroy_borrowed();
  test_high_water();
  test_size_max_fails_safely();
  test_null_and_inert();
  test_deterministic_stress();

  printf("---\n%d checks, %d failures\n", check_count, failure_count);
  return failure_count == 0 ? 0 : 1;
}
