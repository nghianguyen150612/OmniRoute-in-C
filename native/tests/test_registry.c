/*
 * OmniRoute native backend — bounded connection registry tests (Task 020).
 *
 * Pure bookkeeping coverage only: no sockets, no poller waits, no payload
 * transfer, no threads, no heap expectations beyond the absence of growth.
 * Connection objects are borrowed opaque identities here, except for one
 * survival proof that uses fully initialized READY connections and shows
 * registry destroy leaves them usable. Every loop has an explicit finite
 * bound and every capacity is a small constant.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "omniroute/connection.h"
#include "omniroute/registry.h"

#define OMNI_TEST_REGISTRY_CAP 4u
#define OMNI_TEST_STRESS_CYCLES 1000u
#define OMNI_TEST_BACKING_SIZE 256u

static int check_count = 0;
static int failure_count = 0;

static void check(bool condition, const char *name) {
  ++check_count;
  if (condition) {
    printf("ok - %s\n", name);
  } else {
    ++failure_count;
    printf("NOT OK - %s\n", name);
  }
}

/* Opaque identities: never initialized, never dereferenced by the registry. */
static struct omni_connection pool[8];

static void make_registry(struct omni_connection_registry *registry,
                          struct omni_connection_registry_slot *slots, size_t capacity) {
  omni_connection_registry_make_inert(registry);
  (void)omni_connection_registry_init(registry, slots, capacity);
}

static void test_empty_registry(void) {
  struct omni_connection_registry registry;
  struct omni_connection_registry_slot slots[OMNI_TEST_REGISTRY_CAP];
  struct omni_connection_registry_handle probe =
      OMNI_CONNECTION_REGISTRY_HANDLE_INVALID;
  size_t cursor = 0u;
  struct omni_connection_registry_handle out_handle =
      OMNI_CONNECTION_REGISTRY_HANDLE_INVALID;
  struct omni_connection *out_connection = NULL;

  omni_connection_registry_make_inert(&registry);
  check(omni_connection_registry_capacity(&registry) == 0u,
        "inert registry reports zero capacity");
  check(omni_connection_registry_count(&registry) == 0u,
        "inert registry reports zero count");
  check(omni_connection_registry_find(&registry, probe) == NULL,
        "inert registry lookup returns NULL");

  check(omni_connection_registry_init(&registry, slots,
                                      (size_t)OMNI_TEST_REGISTRY_CAP)
            .status == OMNI_CONNECTION_REGISTRY_OK,
        "registry init succeeds with caller slots");
  check(omni_connection_registry_capacity(&registry) == (size_t)OMNI_TEST_REGISTRY_CAP,
        "empty registry reports fixed capacity");
  check(omni_connection_registry_count(&registry) == 0u,
        "empty registry reports zero members");
  check(omni_connection_registry_find(&registry, probe) == NULL,
        "empty registry rejects invalid handle");
  check(!omni_connection_registry_next(&registry, &cursor, &out_handle,
                                       &out_connection),
        "empty registry iteration reports no entry");
  check(cursor == (size_t)OMNI_TEST_REGISTRY_CAP,
        "empty iteration parks cursor at capacity");
  check(omni_connection_registry_remove(&registry, probe).status ==
            OMNI_CONNECTION_REGISTRY_ERR_NOT_FOUND,
        "empty registry remove reports not found");
  omni_connection_registry_destroy(&registry);
  check(omni_connection_registry_capacity(&registry) == 0u,
        "destroyed registry reports zero capacity");
}

static void test_init_rejects(void) {
  struct omni_connection_registry registry;
  struct omni_connection_registry_slot slots[2];
  struct omni_connection_registry_result result;

  omni_connection_registry_make_inert(&registry);
  result = omni_connection_registry_init(&registry, slots, 0u);
  check(result.status == OMNI_CONNECTION_REGISTRY_ERR_INVALID,
        "init rejects zero capacity");
  check(!registry.live, "failed init leaves registry inert");
  check(omni_connection_registry_capacity(&registry) == 0u,
        "failed init reports zero capacity");

  omni_connection_registry_make_inert(&registry);
  result = omni_connection_registry_init(&registry, NULL, 2u);
  check(result.status == OMNI_CONNECTION_REGISTRY_ERR_INVALID,
        "init rejects NULL slot array");

  result = omni_connection_registry_init(NULL, slots, 2u);
  check(result.status == OMNI_CONNECTION_REGISTRY_ERR_INVALID,
        "init rejects NULL registry");

  /* Live registry is never discarded implicitly by a second init. */
  omni_connection_registry_make_inert(&registry);
  check(omni_connection_registry_init(&registry, slots, 2u).status ==
            OMNI_CONNECTION_REGISTRY_OK,
        "first init succeeds for re-init guard");
  {
    struct omni_connection_registry_result added =
        omni_connection_registry_add(&registry, &pool[0]);
    check(added.status == OMNI_CONNECTION_REGISTRY_OK,
          "member added before rejected re-init");
    result = omni_connection_registry_init(&registry, slots, 2u);
    check(result.status == OMNI_CONNECTION_REGISTRY_ERR_INVALID,
          "re-init of live registry is rejected");
    check(omni_connection_registry_count(&registry) == 1u,
          "rejected re-init preserves existing member");
    check(omni_connection_registry_find(&registry, added.handle) == &pool[0],
          "preserved member still resolves after rejected re-init");
  }
  omni_connection_registry_destroy(&registry);

  /* Oversized capacity cannot be represented in a 32-bit handle position. */
  omni_connection_registry_make_inert(&registry);
  result = omni_connection_registry_init(&registry, slots,
                                         (size_t)0xFFFFFFFFu + 1u);
  check(result.status == OMNI_CONNECTION_REGISTRY_ERR_INVALID,
        "init rejects capacity beyond handle position space");

  /* NULL destroy and double destroy are safe. */
  omni_connection_registry_destroy(NULL);
  check(true, "destroy of NULL registry is a safe no-op");
  omni_connection_registry_make_inert(&registry);
  omni_connection_registry_destroy(&registry);
  check(true, "destroy of inert registry is a safe no-op");
}

static void test_add_one(void) {
  struct omni_connection_registry registry;
  struct omni_connection_registry_slot slots[OMNI_TEST_REGISTRY_CAP];
  struct omni_connection_registry_result added;

  make_registry(&registry, slots, (size_t)OMNI_TEST_REGISTRY_CAP);
  added = omni_connection_registry_add(&registry, &pool[0]);
  check(added.status == OMNI_CONNECTION_REGISTRY_OK, "add one succeeds");
  check(added.sys_errno == 0, "add success carries zero errno");
  check(omni_connection_registry_handle_is_valid(added.handle),
        "add returns a valid handle");
  check(omni_connection_registry_count(&registry) == 1u,
        "count is one after single add");
  check(omni_connection_registry_capacity(&registry) ==
            (size_t)OMNI_TEST_REGISTRY_CAP,
        "capacity unchanged after add");
  check(omni_connection_registry_find(&registry, added.handle) == &pool[0],
        "lookup returns the exact added pointer");
  check(omni_connection_registry_add(&registry, NULL).status ==
            OMNI_CONNECTION_REGISTRY_ERR_INVALID,
        "add rejects NULL connection");
  check(omni_connection_registry_count(&registry) == 1u,
        "rejected NULL add leaves count unchanged");
  omni_connection_registry_destroy(&registry);
}

static void test_add_until_full(void) {
  struct omni_connection_registry registry;
  struct omni_connection_registry_slot slots[OMNI_TEST_REGISTRY_CAP];
  struct omni_connection_registry_handle handles[OMNI_TEST_REGISTRY_CAP];
  size_t i = 0u;

  make_registry(&registry, slots, (size_t)OMNI_TEST_REGISTRY_CAP);
  for (i = 0u; i < (size_t)OMNI_TEST_REGISTRY_CAP; ++i) {
    struct omni_connection_registry_result added =
        omni_connection_registry_add(&registry, &pool[i]);

    check(added.status == OMNI_CONNECTION_REGISTRY_OK,
          "fill adds each member until full");
    check(omni_connection_registry_handle_is_valid(added.handle),
          "each fill handle is valid");
    handles[i] = added.handle;
  }
  check(omni_connection_registry_count(&registry) == (size_t)OMNI_TEST_REGISTRY_CAP,
        "count reaches capacity when full");
  for (i = 0u; i < (size_t)OMNI_TEST_REGISTRY_CAP; ++i) {
    char name[96];

    (void)snprintf(name, sizeof(name), "full registry lookup %u resolves",
                   (unsigned int)i);
    check(omni_connection_registry_find(&registry, handles[i]) == &pool[i], name);
  }
  /* Handles must be pairwise distinct. */
  for (i = 0u; i < (size_t)OMNI_TEST_REGISTRY_CAP; ++i) {
    size_t j = 0u;

    for (j = i + 1u; j < (size_t)OMNI_TEST_REGISTRY_CAP; ++j) {
      check(!omni_connection_registry_handle_equal(handles[i], handles[j]),
            "full registry handles are pairwise distinct");
    }
  }
  omni_connection_registry_destroy(&registry);
}

static void test_add_after_full_fails(void) {
  struct omni_connection_registry registry;
  struct omni_connection_registry_slot slots[2];
  struct omni_connection_registry_handle first;
  struct omni_connection_registry_handle second;
  struct omni_connection_registry_result overflow;

  make_registry(&registry, slots, 2u);
  first = omni_connection_registry_add(&registry, &pool[0]).handle;
  second = omni_connection_registry_add(&registry, &pool[1]).handle;
  check(omni_connection_registry_count(&registry) == 2u,
        "registry full at capacity two");
  overflow = omni_connection_registry_add(&registry, &pool[2]);
  check(overflow.status == OMNI_CONNECTION_REGISTRY_ERR_FULL,
        "add beyond capacity reports full");
  check(!omni_connection_registry_handle_is_valid(overflow.handle),
        "failed add returns no valid handle");
  check(omni_connection_registry_count(&registry) == 2u,
        "failed add leaves count unchanged");
  check(omni_connection_registry_find(&registry, first) == &pool[0],
        "first member survives failed add");
  check(omni_connection_registry_find(&registry, second) == &pool[1],
        "second member survives failed add");
  omni_connection_registry_destroy(&registry);
}

static void test_duplicate_rejected(void) {
  struct omni_connection_registry registry;
  struct omni_connection_registry_slot slots[OMNI_TEST_REGISTRY_CAP];
  struct omni_connection_registry_result first;
  struct omni_connection_registry_result again;

  make_registry(&registry, slots, (size_t)OMNI_TEST_REGISTRY_CAP);
  first = omni_connection_registry_add(&registry, &pool[3]);
  check(first.status == OMNI_CONNECTION_REGISTRY_OK, "first add succeeds");
  again = omni_connection_registry_add(&registry, &pool[3]);
  check(again.status == OMNI_CONNECTION_REGISTRY_ERR_DUPLICATE,
        "duplicate pointer add is rejected");
  check(omni_connection_registry_count(&registry) == 1u,
        "duplicate rejection leaves count unchanged");
  check(omni_connection_registry_find(&registry, first.handle) == &pool[3],
        "original handle still resolves after duplicate rejection");
  omni_connection_registry_destroy(&registry);
}

static void test_remove_existing(void) {
  struct omni_connection_registry registry;
  struct omni_connection_registry_slot slots[OMNI_TEST_REGISTRY_CAP];
  struct omni_connection_registry_handle ha;
  struct omni_connection_registry_handle hb;
  struct omni_connection_registry_result removed;

  make_registry(&registry, slots, (size_t)OMNI_TEST_REGISTRY_CAP);
  ha = omni_connection_registry_add(&registry, &pool[0]).handle;
  hb = omni_connection_registry_add(&registry, &pool[1]).handle;
  removed = omni_connection_registry_remove(&registry, ha);
  check(removed.status == OMNI_CONNECTION_REGISTRY_OK, "remove existing succeeds");
  check(omni_connection_registry_count(&registry) == 1u,
        "count decrements after remove");
  check(omni_connection_registry_find(&registry, ha) == NULL,
        "removed handle no longer resolves");
  check(omni_connection_registry_find(&registry, hb) == &pool[1],
        "remaining member still resolves after remove");
  check(omni_connection_registry_remove(&registry, ha).status ==
            OMNI_CONNECTION_REGISTRY_ERR_NOT_FOUND,
        "second remove of same handle reports not found");
  omni_connection_registry_destroy(&registry);
}

static void test_remove_unknown(void) {
  struct omni_connection_registry registry;
  struct omni_connection_registry_slot slots[2];
  struct omni_connection_registry_handle good;
  struct omni_connection_registry_handle bad_index;
  struct omni_connection_registry_handle bad_generation;

  make_registry(&registry, slots, 2u);
  good = omni_connection_registry_add(&registry, &pool[0]).handle;
  bad_index.index = 7u;
  bad_index.generation = 1u;
  check(omni_connection_registry_remove(&registry, bad_index).status ==
            OMNI_CONNECTION_REGISTRY_ERR_NOT_FOUND,
        "remove with out-of-range position fails safely");
  bad_generation.index = good.index;
  bad_generation.generation = (uint32_t)(good.generation + 100u);
  if (bad_generation.generation == 0u) {
    bad_generation.generation = 77u;
  }
  check(omni_connection_registry_remove(&registry, bad_generation).status ==
            OMNI_CONNECTION_REGISTRY_ERR_NOT_FOUND,
        "remove with wrong generation fails safely");
  check(omni_connection_registry_remove(
            &registry, OMNI_CONNECTION_REGISTRY_HANDLE_INVALID)
            .status == OMNI_CONNECTION_REGISTRY_ERR_NOT_FOUND,
        "remove with invalid sentinel fails safely");
  check(omni_connection_registry_remove(NULL, good).status ==
            OMNI_CONNECTION_REGISTRY_ERR_INVALID,
        "remove on NULL registry is invalid");
  check(omni_connection_registry_count(&registry) == 1u,
        "failed removes leave count unchanged");
  check(omni_connection_registry_find(&registry, good) == &pool[0],
        "good member survives failed removes");
  omni_connection_registry_destroy(&registry);
  check(omni_connection_registry_remove(&registry, good).status ==
            OMNI_CONNECTION_REGISTRY_ERR_INVALID,
        "remove on destroyed registry is invalid");
}

static void test_stale_handle_rejection(void) {
  struct omni_connection_registry registry;
  struct omni_connection_registry_slot slots[2];
  struct omni_connection_registry_handle first;
  struct omni_connection_registry_handle second;

  make_registry(&registry, slots, 2u);
  first = omni_connection_registry_add(&registry, &pool[0]).handle;
  check(omni_connection_registry_remove(&registry, first).status ==
            OMNI_CONNECTION_REGISTRY_OK,
        "stale test removes the original member");
  check(omni_connection_registry_find(&registry, first) == NULL,
        "removed handle is stale for lookup");
  /* First free position is reused, so the next add lands on the same slot
   * with an advanced generation. Either way the old handle must stay dead. */
  second = omni_connection_registry_add(&registry, &pool[1]).handle;
  check(omni_connection_registry_handle_is_valid(second),
        "replacement add after remove succeeds");
  check(omni_connection_registry_find(&registry, first) == NULL,
        "old handle stays stale after slot reuse");
  check(omni_connection_registry_find(&registry, second) == &pool[1],
        "new handle resolves to the new member");
  if (second.index == first.index) {
    check(second.generation != first.generation,
          "reused position advances generation");
  } else {
    check(true, "reused position advances generation");
  }
  check(!omni_connection_registry_handle_equal(first, second),
        "replacement handle differs from stale handle");
  omni_connection_registry_destroy(&registry);
  check(omni_connection_registry_find(&registry, second) == NULL,
        "handles do not survive registry destroy");
}

static void test_lookup_correctness(void) {
  struct omni_connection_registry registry;
  struct omni_connection_registry_slot slots[OMNI_TEST_REGISTRY_CAP];
  struct omni_connection_registry_handle handles[OMNI_TEST_REGISTRY_CAP];
  size_t i = 0u;

  make_registry(&registry, slots, (size_t)OMNI_TEST_REGISTRY_CAP);
  for (i = 0u; i < (size_t)OMNI_TEST_REGISTRY_CAP; ++i) {
    handles[i] = omni_connection_registry_add(&registry, &pool[i]).handle;
  }
  for (i = 0u; i < (size_t)OMNI_TEST_REGISTRY_CAP; ++i) {
    struct omni_connection *found = omni_connection_registry_find(&registry,
                                                                  handles[i]);

    check(found == &pool[i], "lookup maps every handle to its own member");
  }
  check(omni_connection_registry_find(NULL, handles[0]) == NULL,
        "lookup on NULL registry returns NULL");
  {
    struct omni_connection_registry_handle cross;

    cross.index = handles[0].index;
    cross.generation = handles[1].generation;
    /* When both generations happen to match, the cross handle equals the
     * first handle and must resolve; otherwise it must be rejected. */
    if (omni_connection_registry_handle_equal(
            cross, handles[0])) {
      check(omni_connection_registry_find(&registry, cross) == &pool[0],
            "identical cross handle resolves consistently");
    } else {
      check(omni_connection_registry_find(&registry, cross) == NULL,
            "cross-generation handle is rejected");
    }
  }
  omni_connection_registry_destroy(&registry);
}

static void test_iteration(void) {
  struct omni_connection_registry registry;
  struct omni_connection_registry_slot slots[OMNI_TEST_REGISTRY_CAP];
  struct omni_connection_registry_handle ha;
  struct omni_connection_registry_handle hb;
  struct omni_connection_registry_handle hc;
  size_t cursor = 0u;
  struct omni_connection_registry_handle got;
  struct omni_connection *found = NULL;
  size_t visited = 0u;
  uint32_t last_index = 0u;
  bool first_step = true;

  make_registry(&registry, slots, (size_t)OMNI_TEST_REGISTRY_CAP);
  ha = omni_connection_registry_add(&registry, &pool[0]).handle;
  hb = omni_connection_registry_add(&registry, &pool[2]).handle;
  hc = omni_connection_registry_add(&registry, &pool[5]).handle;
  (void)ha;
  (void)hb;
  (void)hc;
  cursor = 0u;
  visited = 0u;
  first_step = true;
  while (omni_connection_registry_next(&registry, &cursor, &got, &found)) {
    check(found != NULL, "iteration publishes a borrowed connection");
    check(omni_connection_registry_find(&registry, got) == found,
          "iterated handle resolves through lookup");
    if (first_step) {
      first_step = false;
    } else {
      check(got.index > last_index, "iteration visits increasing positions");
    }
    last_index = got.index;
    ++visited;
    if (visited > (size_t)OMNI_TEST_REGISTRY_CAP + 1u) {
      break;
    }
  }
  check(visited == 3u, "iteration visits exactly the live members");
  check(cursor == (size_t)OMNI_TEST_REGISTRY_CAP,
        "finished iteration parks cursor at capacity");

  /* Removing the middle member keeps iteration deterministic and safe. */
  check(omni_connection_registry_remove(&registry, hb).status ==
            OMNI_CONNECTION_REGISTRY_OK,
        "iteration test removes the middle member");
  cursor = 0u;
  visited = 0u;
  first_step = true;
  while (omni_connection_registry_next(&registry, &cursor, &got, &found)) {
    check(found == &pool[0] || found == &pool[5],
          "post-remove iteration skips the forgotten entry");
    if (!first_step) {
      check(got.index > last_index,
            "post-remove iteration stays in increasing order");
    }
    first_step = false;
    last_index = got.index;
    ++visited;
    if (visited > (size_t)OMNI_TEST_REGISTRY_CAP + 1u) {
      break;
    }
  }
  check(visited == 2u, "post-remove iteration visits two survivors");
  check(!omni_connection_registry_next(NULL, &cursor, &got, &found),
        "iteration on NULL registry reports false");
  omni_connection_registry_destroy(&registry);
}

static void test_add_remove_stress(void) {
  struct omni_connection_registry registry;
  struct omni_connection_registry_slot slots[OMNI_TEST_REGISTRY_CAP];
  size_t cycle = 0u;

  make_registry(&registry, slots, (size_t)OMNI_TEST_REGISTRY_CAP);
  for (cycle = 0u; cycle < OMNI_TEST_STRESS_CYCLES; ++cycle) {
    struct omni_connection *member = &pool[cycle % 8u];
    struct omni_connection_registry_result added;
    struct omni_connection_registry_result removed;

    /* Keep at most two live members so add/remove interleave deterministically. */
    if (omni_connection_registry_count(&registry) >= 2u) {
      size_t cursor = 0u;
      struct omni_connection_registry_handle oldest;
      struct omni_connection *oldest_ptr = NULL;

      if (!omni_connection_registry_next(&registry, &cursor, &oldest,
                                         &oldest_ptr)) {
        check(false, "stress iteration finds an entry to retire");
        break;
      }
      removed = omni_connection_registry_remove(&registry, oldest);
      if (removed.status != OMNI_CONNECTION_REGISTRY_OK) {
        check(false, "stress remove succeeds");
        break;
      }
      if (omni_connection_registry_find(&registry, oldest) != NULL) {
        check(false, "stress removed handle stays stale");
        break;
      }
    }
    /* The rotating pool may still hold this member; retire it first. */
    {
      size_t cursor = 0u;
      struct omni_connection_registry_handle each;
      struct omni_connection *each_ptr = NULL;
      bool present = false;

      while (omni_connection_registry_next(&registry, &cursor, &each,
                                           &each_ptr)) {
        if (each_ptr == member) {
          present = true;
          if (omni_connection_registry_remove(&registry, each).status !=
              OMNI_CONNECTION_REGISTRY_OK) {
            check(false, "stress duplicate retire succeeds");
            present = false;
            break;
          }
          break;
        }
      }
      (void)present;
    }
    added = omni_connection_registry_add(&registry, member);
    if (added.status != OMNI_CONNECTION_REGISTRY_OK) {
      check(false, "stress add succeeds within capacity");
      break;
    }
    if (omni_connection_registry_find(&registry, added.handle) != member) {
      check(false, "stress lookup resolves fresh handle");
      break;
    }
    if (omni_connection_registry_count(&registry) > (size_t)OMNI_TEST_REGISTRY_CAP) {
      check(false, "stress count never exceeds capacity");
      break;
    }
  }
  check(cycle == OMNI_TEST_STRESS_CYCLES, "deterministic add/remove stress completes");
  check(omni_connection_registry_count(&registry) <= (size_t)OMNI_TEST_REGISTRY_CAP,
        "stress count stays within capacity");
  omni_connection_registry_destroy(&registry);
}

static void test_no_memory_growth(void) {
  struct omni_connection_registry registry;
  struct omni_connection_registry_slot slots[OMNI_TEST_REGISTRY_CAP];
  const struct omni_connection_registry_slot *slots_before = NULL;
  size_t cycle = 0u;

  make_registry(&registry, slots, (size_t)OMNI_TEST_REGISTRY_CAP);
  slots_before = registry.slots;
  for (cycle = 0u; cycle < 200u; ++cycle) {
    struct omni_connection_registry_handle handle =
        omni_connection_registry_add(&registry, &pool[cycle % 4u]).handle;

    if (!omni_connection_registry_handle_is_valid(handle)) {
      /* Pool alias already tracked; retire it and retry once. */
      size_t cursor = 0u;
      struct omni_connection_registry_handle each;
      struct omni_connection *each_ptr = NULL;

      while (omni_connection_registry_next(&registry, &cursor, &each,
                                           &each_ptr)) {
        if (each_ptr == &pool[cycle % 4u]) {
          (void)omni_connection_registry_remove(&registry, each);
          break;
        }
      }
      handle = omni_connection_registry_add(&registry, &pool[cycle % 4u]).handle;
      if (!omni_connection_registry_handle_is_valid(handle)) {
        check(false, "growth test add eventually succeeds");
        break;
      }
    }
    if (omni_connection_registry_remove(&registry, handle).status !=
        OMNI_CONNECTION_REGISTRY_OK) {
      check(false, "growth test remove succeeds");
      break;
    }
    if (registry.slots != slots_before) {
      check(false, "registry keeps caller slot array across cycles");
      break;
    }
    if (omni_connection_registry_capacity(&registry) !=
        (size_t)OMNI_TEST_REGISTRY_CAP) {
      check(false, "registry capacity stays fixed across cycles");
      break;
    }
  }
  check(cycle == 200u, "repeated add/remove cycles keep storage fixed");
  check(registry.slots == slots_before, "slot array address never moves");
  check(registry.slots == slots, "slot array is still the caller array");
  check(omni_connection_registry_count(&registry) == 0u,
        "growth test ends with zero members");
  omni_connection_registry_destroy(&registry);
}

static void test_caller_owned_survives_destroy(void) {
  struct omni_connection_registry registry;
  struct omni_connection_registry_slot slots[2];
  struct omni_connection first;
  struct omni_connection second;
  unsigned char first_storage[OMNI_TEST_BACKING_SIZE];
  unsigned char second_storage[OMNI_TEST_BACKING_SIZE];
  struct omni_connection_config first_config;
  struct omni_connection_config second_config;
  struct omni_connection_registry_handle first_handle;
  struct omni_connection_registry_handle second_handle;

  memset(&first, 0, sizeof(first));
  memset(&second, 0, sizeof(second));
  omni_connection_make_inert(&first);
  omni_connection_make_inert(&second);
  memset(&first_config, 0, sizeof(first_config));
  memset(&second_config, 0, sizeof(second_config));
  first_config.receive_storage = first_storage;
  first_config.receive_capacity = sizeof(first_storage);
  first_config.poller_token = 11u;
  first_config.poller_interests = OMNI_POLLER_INTEREST_READ;
  second_config.receive_storage = second_storage;
  second_config.receive_capacity = sizeof(second_storage);
  second_config.poller_token = 12u;
  second_config.poller_interests = OMNI_POLLER_INTEREST_WRITE;
  check(omni_connection_init(&first, &first_config).status == OMNI_CONNECTION_OK,
        "survival fixture prepares first connection");
  check(omni_connection_init(&second, &second_config).status == OMNI_CONNECTION_OK,
        "survival fixture prepares second connection");

  make_registry(&registry, slots, 2u);
  first_handle = omni_connection_registry_add(&registry, &first).handle;
  second_handle = omni_connection_registry_add(&registry, &second).handle;
  check(omni_connection_registry_handle_is_valid(first_handle),
        "survival fixture tracks first connection");
  check(omni_connection_registry_handle_is_valid(second_handle),
        "survival fixture tracks second connection");
  omni_connection_registry_destroy(&registry);
  check(omni_connection_state(&first) == OMNI_CONNECTION_READY,
        "first caller connection survives registry destroy");
  check(omni_connection_state(&second) == OMNI_CONNECTION_READY,
        "second caller connection survives registry destroy");
  check(omni_connection_poller_token(&first) == 11u,
        "first connection metadata survives registry destroy");
  check(omni_connection_poller_token(&second) == 12u,
        "second connection metadata survives registry destroy");
  check(omni_connection_registry_find(&registry, first_handle) == NULL,
        "handles retire with the registry, not the connections");
  omni_connection_destroy(&first);
  omni_connection_destroy(&second);
  check(omni_connection_state(&first) == OMNI_CONNECTION_CLOSED,
        "first connection still destroys cleanly after registry");
  check(omni_connection_state(&second) == OMNI_CONNECTION_CLOSED,
        "second connection still destroys cleanly after registry");
}

int main(void) {
  test_empty_registry();
  test_init_rejects();
  test_add_one();
  test_add_until_full();
  test_add_after_full_fails();
  test_duplicate_rejected();
  test_remove_existing();
  test_remove_unknown();
  test_stale_handle_rejection();
  test_lookup_correctness();
  test_iteration();
  test_add_remove_stress();
  test_no_memory_growth();
  test_caller_owned_survives_destroy();

  printf("---\n%d checks, %d failures\n", check_count, failure_count);
  return failure_count == 0 ? 0 : 1;
}
