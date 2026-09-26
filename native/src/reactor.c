/*
 * OmniRoute native backend — bounded reactor implementation (Task 021).
 *
 * The implementation borrows a live poller and two caller-provided fixed
 * arrays. Registration entries are kept dense so lookup and compaction are
 * deterministic. There is no heap call, no descriptor syscall, and no
 * payload I/O in this layer. A step asks the poller for the complete live
 * event set that fits in the fixed event array, then dispatches callbacks
 * synchronously in poller order.
 */

#include "omniroute/reactor.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

static void mark_inert(struct omni_reactor *reactor) {
  if (reactor == NULL) {
    return;
  }
  reactor->poller = NULL;
  reactor->registrations = NULL;
  reactor->events = NULL;
  reactor->capacity = 0u;
  reactor->count = 0u;
  reactor->live = false;
}

static void clear_registration(struct omni_reactor_registration *registration) {
  registration->fd = -1;
  registration->token = 0u;
  registration->interests = 0u;
  registration->callback = NULL;
  registration->context = NULL;
}

static struct omni_reactor_result make_result(enum omni_reactor_status status,
                                              int sys_errno,
                                              size_t count) {
  struct omni_reactor_result result;

  result.status = status;
  result.sys_errno = sys_errno;
  result.count = count;
  return result;
}

static bool interests_valid(uint32_t interests) {
  if (interests == 0u) {
    return false;
  }
  return (interests & ~(OMNI_POLLER_INTEREST_READ | OMNI_POLLER_INTEREST_WRITE)) == 0u;
}

static size_t find_token(const struct omni_reactor *reactor, uint64_t token) {
  size_t i = 0u;

  if (reactor == NULL || !reactor->live) {
    return SIZE_MAX;
  }
  for (i = 0u; i < reactor->count; ++i) {
    if (reactor->registrations[i].token == token) {
      return i;
    }
  }
  return SIZE_MAX;
}

static size_t find_fd(const struct omni_reactor *reactor, int fd) {
  size_t i = 0u;

  if (reactor == NULL || !reactor->live) {
    return SIZE_MAX;
  }
  for (i = 0u; i < reactor->count; ++i) {
    if (reactor->registrations[i].fd == fd) {
      return i;
    }
  }
  return SIZE_MAX;
}

static size_t find_event_registration(const struct omni_reactor *reactor,
                                      const struct omni_poller_event *event) {
  size_t i = 0u;

  if (reactor == NULL || event == NULL || !reactor->live) {
    return SIZE_MAX;
  }
  for (i = 0u; i < reactor->count; ++i) {
    if (reactor->registrations[i].fd == event->fd &&
        reactor->registrations[i].token == event->token) {
      return i;
    }
  }
  return SIZE_MAX;
}

static enum omni_reactor_status map_add_error(enum omni_poller_status status) {
  switch (status) {
    case OMNI_POLLER_ERR_DUPLICATE:
      return OMNI_REACTOR_ERR_DUPLICATE;
    case OMNI_POLLER_ERR_FULL:
      return OMNI_REACTOR_ERR_FULL;
    case OMNI_POLLER_ERR_INVALID:
      return OMNI_REACTOR_ERR_INVALID;
    default:
      return OMNI_REACTOR_ERR_WAIT;
  }
}

static enum omni_reactor_status map_remove_error(enum omni_poller_status status) {
  switch (status) {
    case OMNI_POLLER_ERR_NOT_FOUND:
      return OMNI_REACTOR_ERR_NOT_FOUND;
    case OMNI_POLLER_ERR_INVALID:
      return OMNI_REACTOR_ERR_INVALID;
    default:
      return OMNI_REACTOR_ERR_WAIT;
  }
}

static enum omni_reactor_status map_update_error(enum omni_poller_status status) {
  switch (status) {
    case OMNI_POLLER_ERR_NOT_FOUND:
      return OMNI_REACTOR_ERR_NOT_FOUND;
    case OMNI_POLLER_ERR_INVALID:
      return OMNI_REACTOR_ERR_INVALID;
    default:
      return OMNI_REACTOR_ERR_WAIT;
  }
}

static enum omni_reactor_status map_wait_error(enum omni_poller_status status) {
  switch (status) {
    case OMNI_POLLER_ERR_INVALID:
      return OMNI_REACTOR_ERR_INVALID;
    case OMNI_POLLER_ERR_OUTPUT:
      return OMNI_REACTOR_ERR_OUTPUT;
    case OMNI_POLLER_ERR_INTERRUPTED:
      return OMNI_REACTOR_ERR_INTERRUPTED;
    case OMNI_POLLER_ERR_WAIT:
      return OMNI_REACTOR_ERR_WAIT;
    default:
      return OMNI_REACTOR_ERR_WAIT;
  }
}

void omni_reactor_make_inert(struct omni_reactor *reactor) {
  mark_inert(reactor);
}

struct omni_reactor_result omni_reactor_init(
    struct omni_reactor *reactor,
    struct omni_poller *poller,
    struct omni_reactor_registration *registrations,
    struct omni_poller_event *events,
    size_t capacity) {
  size_t i = 0u;

  if (reactor == NULL) {
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }
  if (reactor->live) {
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }
  if (poller == NULL || !poller->live || registrations == NULL || events == NULL ||
      capacity == 0u) {
    mark_inert(reactor);
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }
  if (capacity > SIZE_MAX / sizeof(*registrations) ||
      capacity > SIZE_MAX / sizeof(*events)) {
    mark_inert(reactor);
    return make_result(OMNI_REACTOR_ERR_INVALID, EOVERFLOW, 0u);
  }

  reactor->poller = poller;
  reactor->registrations = registrations;
  reactor->events = events;
  reactor->capacity = capacity;
  reactor->count = 0u;
  reactor->live = true;

  for (i = 0u; i < capacity; ++i) {
    clear_registration(&registrations[i]);
  }

  return make_result(OMNI_REACTOR_OK, 0, 0u);
}

void omni_reactor_destroy(struct omni_reactor *reactor) {
  if (reactor == NULL || !reactor->live) {
    return;
  }

  while (reactor->count > 0u) {
    size_t last = reactor->count - 1u;

    /* A failed or externally altered poller still cannot justify retaining
     * callback/context pointers. Destruction never closes the borrowed FD. */
    if (reactor->poller != NULL && reactor->poller->live) {
      (void)omni_poller_remove(reactor->poller, reactor->registrations[last].fd);
    }
    clear_registration(&reactor->registrations[last]);
    reactor->count = last;
  }

  mark_inert(reactor);
}

struct omni_reactor_result omni_reactor_add(
    struct omni_reactor *reactor,
    int fd,
    uint64_t token,
    uint32_t interests,
    omni_reactor_callback callback,
    void *context) {
  struct omni_poller_result poller_result;

  if (reactor == NULL || !reactor->live || fd < 0 || callback == NULL ||
      !interests_valid(interests)) {
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }
  if (find_token(reactor, token) != SIZE_MAX) {
    return make_result(OMNI_REACTOR_ERR_DUPLICATE, EEXIST, 0u);
  }
  if (find_fd(reactor, fd) != SIZE_MAX) {
    return make_result(OMNI_REACTOR_ERR_DUPLICATE, EEXIST, 0u);
  }
  if (reactor->count >= reactor->capacity) {
    return make_result(OMNI_REACTOR_ERR_FULL, ENOSPC, 0u);
  }

  poller_result = omni_poller_add(reactor->poller, fd, token, interests);
  if (poller_result.status != OMNI_POLLER_OK) {
    return make_result(map_add_error(poller_result.status), poller_result.sys_errno, 0u);
  }

  reactor->registrations[reactor->count].fd = fd;
  reactor->registrations[reactor->count].token = token;
  reactor->registrations[reactor->count].interests = interests;
  reactor->registrations[reactor->count].callback = callback;
  reactor->registrations[reactor->count].context = context;
  reactor->count += 1u;

  return make_result(OMNI_REACTOR_OK, 0, 0u);
}

struct omni_reactor_result omni_reactor_remove(struct omni_reactor *reactor,
                                               uint64_t token) {
  struct omni_poller_result poller_result;
  size_t index = 0u;
  size_t tail_count = 0u;

  if (reactor == NULL || !reactor->live) {
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }

  index = find_token(reactor, token);
  if (index == SIZE_MAX) {
    return make_result(OMNI_REACTOR_ERR_NOT_FOUND, ENOENT, 0u);
  }

  poller_result = omni_poller_remove(reactor->poller, reactor->registrations[index].fd);
  if (poller_result.status != OMNI_POLLER_OK) {
    return make_result(map_remove_error(poller_result.status), poller_result.sys_errno, 0u);
  }

  tail_count = reactor->count - index - 1u;
  if (tail_count > 0u) {
    memmove(&reactor->registrations[index], &reactor->registrations[index + 1u],
            tail_count * sizeof(reactor->registrations[0]));
  }
  reactor->count -= 1u;
  clear_registration(&reactor->registrations[reactor->count]);

  return make_result(OMNI_REACTOR_OK, 0, 0u);
}

struct omni_reactor_result omni_reactor_update_interests(
    struct omni_reactor *reactor, uint64_t token, uint32_t interests) {
  struct omni_poller_result poller_result;
  size_t index = 0u;

  if (reactor == NULL || !reactor->live || reactor->poller == NULL ||
      !reactor->poller->live || !interests_valid(interests)) {
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }
  index = find_token(reactor, token);
  if (index == SIZE_MAX) {
    return make_result(OMNI_REACTOR_ERR_NOT_FOUND, ENOENT, 0u);
  }

  /* The poller validates and commits first; the fixed reactor record follows
   * only on success, keeping both masks unchanged on a recoverable failure. */
  poller_result = omni_poller_update(reactor->poller,
                                    reactor->registrations[index].fd, interests);
  if (poller_result.status != OMNI_POLLER_OK) {
    return make_result(map_update_error(poller_result.status),
                       poller_result.sys_errno, 0u);
  }
  reactor->registrations[index].interests = interests;
  return make_result(OMNI_REACTOR_OK, 0, 0u);
}

struct omni_reactor_result omni_reactor_step(struct omni_reactor *reactor,
                                             int64_t timeout_ms) {
  struct omni_poller_result poller_result;
  struct omni_poller_event *events = NULL;
  size_t event_count = 0u;
  size_t dispatched = 0u;
  size_t i = 0u;

  if (reactor == NULL || !reactor->live) {
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }
  if (timeout_ms < 0 || timeout_ms > INT_MAX) {
    return make_result(OMNI_REACTOR_ERR_INVALID, EINVAL, 0u);
  }
  if (reactor->count == 0u) {
    return make_result(OMNI_REACTOR_OK, 0, 0u);
  }

  events = reactor->events;
  poller_result = omni_poller_wait(reactor->poller, timeout_ms, events, reactor->capacity);
  if (poller_result.status != OMNI_POLLER_OK) {
    return make_result(map_wait_error(poller_result.status), poller_result.sys_errno, 0u);
  }

  event_count = poller_result.count;
  for (i = 0u; i < event_count; ++i) {
    struct omni_poller_event event;
    size_t index = 0u;
    omni_reactor_callback callback = NULL;
    void *context = NULL;

    /* A callback may destroy the reactor. Stop before touching its backing
     * arrays in that case; the local event pointer is never dereferenced. */
    if (!reactor->live) {
      break;
    }

    event = events[i];
    index = find_event_registration(reactor, &event);
    if (index == SIZE_MAX) {
      continue;
    }

    callback = reactor->registrations[index].callback;
    context = reactor->registrations[index].context;
    if (callback == NULL) {
      continue;
    }

    callback(event.token, event.ready, context);
    dispatched += 1u;
  }

  return make_result(OMNI_REACTOR_OK, 0, dispatched);
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
