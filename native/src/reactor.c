/*
 * OmniRoute native backend — bounded reactor implementation (Task 021).
 *
 * Conversion strategy: capacity stays in size_t end to end and is
 * range-checked against the 32-bit position space at init, so the
 * single narrowing cast from position to handle on lookup is always
 * exact. Ownership is explicit: the reactor borrows everything from its
 * caller (poller and parallel arrays), never allocates beyond init
 * (which passes caller-provided storage). Registration maps tokens
 * to array positions via linear scan — capacity is intended to stay
 * small so this is acceptable. Callbacks and contexts are borrowed
 * and never dereferenced after their registration is removed.
 *
 * Descriptor-touch audit: this file performs no close, no dup, no flag
 * change, and no payload input/output calls — by construction there is
 * nothing here that could take ownership. The network-boundary gate
 * enforces the same property textually.
 *
 * Result taxonomy: similar to other layers: status, errno, count (of
 * events processed). Duplicates and capacity exceedances are rejected
 * with status changed but state unchanged. Interrupted waits surface
 * immediately with ERR_INTERRUPTED and zero events processed.
 */

#include "omniroute/reactor.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void mark_inert(struct omni_reactor *reactor) {
  if (reactor == NULL) {
    return;
  }
  reactor->poller = NULL;
  reactor->callbacks = NULL;
  reactor->contexts = NULL;
  reactor->ready_masks = NULL;
  reactor->tokens = NULL;
  reactor->fds = NULL;
  reactor->registered = NULL;
  reactor->capacity = 0u;
  reactor->count = 0u;
  reactor->live = false;
}

static struct omni_reactor_result make_result(enum omni_reactor_status status, int err,
                                             size_t count) {
  struct omni_reactor_result out;

  out.status = status;
  out.sys_errno = err;
  out.count = count;
  return out;
}

static bool interests_valid(uint32_t interests) {
  if (interests == 0u) {
    return false;
  }
  return (interests & ~(OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE)) == 0u;
}

static size_t find_by_token(const struct omni_reactor *reactor, uint64_t token) {
  if (reactor == NULL || !reactor->live) {
    return (size_t)-1;
  }
  for (size_t i = 0u; i < reactor->count; ++i) {
    if (reactor->registered[i] && reactor->tokens[i] == token) {
      return i;
    }
  }
  return (size_t)-1;
}

void omni_reactor_make_inert(struct omni_reactor *reactor) {
  if (reactor == NULL) {
    return;
  }
  mark_inert(reactor);
}

struct omni_reactor_result omni_reactor_init(struct omni_reactor *reactor,
                                             struct omni_poller *poller,
                                             void *callbacks,
                                             void *contexts,
                                             void *ready_masks,
                                             void *tokens,
                                             void *fds,
                                             bool *registered,
                                             size_t capacity) {
  if (reactor == NULL) {
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }
  if (reactor->live) {
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }
  if (poller == NULL || callbacks == NULL || contexts == NULL ||
      ready_masks == NULL || tokens == NULL || fds == NULL ||
      registered == NULL || capacity == 0u) {
    mark_inert(reactor);
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }
  if (capacity > SIZE_MAX / sizeof(omni_reactor_callback)) {
    mark_inert(reactor);
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }
  if (capacity > SIZE_MAX / sizeof(void *)) {
    mark_inert(reactor);
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }

  reactor->poller = poller;
  reactor->callbacks = (omni_reactor_callback *)callbacks;
  reactor->contexts = (void **)contexts;
  reactor->ready_masks = (uint32_t *)ready_masks;
  reactor->tokens = (uint64_t *)tokens;
  reactor->fds = (int *)fds;
  reactor->registered = (bool *)registered;
  reactor->capacity = capacity;
  reactor->count = 0u;
  reactor->live = true;

  for (size_t i = 0u; i < capacity; ++i) {
    reactor->registered[i] = false;
    reactor->tokens[i] = 0u;
    reactor->fds[i] = -1;
  }

  return make_result(OMNI_REACTOR_OK, 0, 0u);
}

void omni_reactor_destroy(struct omni_reactor *reactor) {
  if (reactor == NULL || !reactor->live) {
    return;
  }
  mark_inert(reactor);
}

struct omni_reactor_result omni_reactor_add(struct omni_reactor *reactor,
                                           int fd,
                                           uint64_t token,
                                           uint32_t interests,
                                           omni_reactor_callback callback,
                                           void *context) {
  if (reactor == NULL || !reactor->live) {
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }
  if (fd < 0) {
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }
  if (!interests_valid(interests)) {
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }
  if (callback == NULL) {
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }

  size_t i = 0u;
  for (; i < reactor->count; ++i) {
    if (reactor->tokens[i] == token) {
      return make_result(OMNI_REACTOR_ERR_INVALID, EEXIST, 0u);
    }
  }

  if (reactor->count >= reactor->capacity) {
    return make_result(OMNI_REACTOR_ERR_INVALID, ENOSPC, 0u);
  }

  reactor->fds[reactor->count] = fd;
  reactor->tokens[reactor->count] = token;
  reactor->registered[reactor->count] = true;

  struct omni_poller_result poller_result = omni_poller_add(
      reactor->poller, fd, token, interests);

  if (poller_result.status != OMNI_POLLER_OK) {
    reactor->registered[reactor->count] = false;
    reactor->tokens[reactor->count] = 0u;
    reactor->fds[reactor->count] = -1;
    return make_result(OMNI_REACTOR_ERR_INVALID, poller_result.sys_errno, 0u);
  }

  reactor->callbacks[reactor->count] = callback;
  reactor->contexts[reactor->count] = context;

  ++reactor->count;
  return make_result(OMNI_REACTOR_OK, 0, 0u);
}

struct omni_reactor_result omni_reactor_remove(struct omni_reactor *reactor,
                                              uint64_t token) {
  if (reactor == NULL || !reactor->live) {
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }

  size_t i = find_by_token(reactor, token);
  if (i == (size_t)-1) {
    return make_result(OMNI_REACTOR_ERR_NOT_FOUND, ENOENT, 0u);
  }

  if (!reactor->registered[i]) {
    return make_result(OMNI_REACTOR_ERR_NOT_FOUND, ENOENT, 0u);
  }

  reactor->registered[i] = false;

  struct omni_poller_result poller_result = omni_poller_remove(
      reactor->poller, reactor->fds[i]);

  if (poller_result.status != OMNI_POLLER_OK) {
    reactor->registered[i] = true;
    return make_result(OMNI_REACTOR_ERR_NOT_FOUND, poller_result.sys_errno, 0u);
  }

  if (i < reactor->count - 1) {
    memmove(&reactor->fds[i], &reactor->fds[i + 1],
            (reactor->count - i - 1) * sizeof(int));
    memmove(&reactor->tokens[i], &reactor->tokens[i + 1],
            (reactor->count - i - 1) * sizeof(uint64_t));
    memmove(&reactor->registered[i], &reactor->registered[i + 1],
            (reactor->count - i - 1) * sizeof(bool));
    memmove(&reactor->callbacks[i], &reactor->callbacks[i + 1],
            (reactor->count - i - 1) * sizeof(omni_reactor_callback));
    memmove(&reactor->contexts[i], &reactor->contexts[i + 1],
            (reactor->count - i - 1) * sizeof(void *));
    memmove(&reactor->ready_masks[i], &reactor->ready_masks[i + 1],
            (reactor->count - i - 1) * sizeof(uint32_t));
  }

  --reactor->count;
  reactor->fds[reactor->count] = -1;
  reactor->tokens[reactor->count] = 0u;
  reactor->registered[reactor->count] = false;

  return make_result(OMNI_REACTOR_OK, 0, 0u);
}

struct omni_reactor_result omni_reactor_update(struct omni_reactor *reactor,
                                              uint64_t token,
                                              uint32_t interests) {
  if (reactor == NULL || !reactor->live) {
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }
  if (!interests_valid(interests)) {
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }

  size_t i = find_by_token(reactor, token);
  if (i == (size_t)-1) {
    return make_result(OMNI_REACTOR_ERR_NOT_FOUND, ENOENT, 0u);
  }

  if (!reactor->registered[i]) {
    return make_result(OMNI_REACTOR_ERR_NOT_FOUND, ENOENT, 0u);
  }

  struct omni_poller_result poller_result = omni_poller_update(
      reactor->poller, reactor->fds[i], interests);

  if (poller_result.status != OMNI_POLLER_OK) {
    return make_result(OMNI_REACTOR_ERR_NOT_FOUND, poller_result.sys_errno, 0u);
  }

  return make_result(OMNI_REACTOR_OK, 0, 0u);
}

struct omni_reactor_result omni_reactor_step(struct omni_reactor *reactor,
                                            int64_t timeout_ms) {
  if (reactor == NULL || !reactor->live) {
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }

  if (reactor->count == 0u) {
    return make_result(OMNI_REACTOR_OK, 0, 0u);
  }

  if (timeout_ms < 0 || timeout_ms > INT_MAX) {
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }

  /* Create temporary event storage for the poller wait */
  struct omni_poller_event events[1];

  struct omni_poller_result poller_result = omni_poller_wait(
      reactor->poller, timeout_ms, events, 1u);

  size_t processed = 0u;

  if (poller_result.status == OMNI_POLLER_OK) {
    for (size_t i = 0u; i < poller_result.count; ++i) {
      size_t idx = find_by_token(reactor, events[i].token);

      if (idx != (size_t)-1 && reactor->registered[idx]) {
        uint32_t ready = reactor->ready_masks[idx];
        ready |= events[i].ready;
        reactor->ready_masks[idx] = ready;

        omni_reactor_callback callback = reactor->callbacks[idx];
        void *context = reactor->contexts[idx];

        callback(events[i].token, ready, context);
        ++processed;
      }
    }

    return make_result(OMNI_REACTOR_OK, 0, processed);
  } else if (poller_result.status == OMNI_POLLER_ERR_INTERRUPTED) {
    return make_result(OMNI_REACTOR_ERR_INTERRUPTED, 0, 0u);
  } else {
    return make_result(OMNI_REACTOR_ERR_INVALID, poller_result.sys_errno, 0u);
  }
}

void omni_reactor_reset(struct omni_reactor *reactor) {
  if (reactor == NULL || !reactor->live) {
    return;
  }

  for (size_t i = 0u; i < reactor->count; ++i) {
    reactor->registered[i] = false;
    reactor->tokens[i] = 0u;
    reactor->fds[i] = -1;
  }

  reactor->count = 0u;
}

size_t omni_reactor_capacity(const struct omni_reactor *reactor) {
  if (reactor == NULL || !reactor->live) {
    return 0u;
  }
  return reactor->capacity;
}

size_t omni_reactor_count(const struct omni_reactor *reactor) {
  if (reactor == NULL || !reactor->live) {
    return 0u;
  }
  return reactor->count;
}