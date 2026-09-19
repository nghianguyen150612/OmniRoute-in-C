/*
 * OmniRoute native backend — bounded readiness implementation.
 *
 * Conversion strategy: interests arrive as a validated project mask and
 * leave as a single explicit narrowing cast to the platform event field;
 * the values carried are two small bits, so the cast is exact. Timeout
 * arrives as int64 milliseconds and is range-checked to 0..INT_MAX before
 * the single explicit narrowing cast to the platform wait call — larger
 * values are rejected, never clamped. Registration counts stay in
 * size_t; the count handed to the wait call is at most the caller-chosen
 * capacity. Event output is bounded by the caller's own capacity, checked
 * before any waiting happens, so translation cannot overrun it.
 *
 * Interruption audit: the readiness wait may report EINTR. The mapping
 * turns that into INTERRUPTED immediately with nothing consumed — no
 * silent original-timeout retry (which could stretch a finite wait
 * without bound under repeated interruption) and no deadline clock (no
 * consumer needs one yet). The empty-poller fast path performs no wait
 * call at all, so it cannot be interrupted.
 *
 * Descriptor-touch audit: this file contains no close, no dup, no flag
 * change, and no payload input/output calls — by construction there is
 * nothing here that could take ownership. The network-boundary gate
 * enforces the same property textually.
 *
 * Feature macro: _DEFAULT_SOURCE widens system-header visibility only
 * (extended readiness constants on older glibc). It changes nothing
 * about the strict warning level applied to this file.
 */

#define _DEFAULT_SOURCE

#include "omniroute/poller.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void mark_inert(struct omni_poller *poller) {
  poller->fds = NULL;
  poller->tokens = NULL;
  poller->capacity = 0;
  poller->count = 0;
  poller->owns_backing = false;
  poller->live = false;
}

static struct omni_poller_result make_result(enum omni_poller_status status, int err,
                                             size_t count) {
  struct omni_poller_result out;

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

/* Project interests to the platform event field. Input pre-validated, so
 * the result always carries at least one bit. */
static short project_interests(uint32_t interests) {
  int events = 0;

  if ((interests & OMNI_POLLER_INTEREST_READ) != 0u) {
    events |= POLLIN;
  }
  if ((interests & OMNI_POLLER_INTEREST_WRITE) != 0u) {
    events |= POLLOUT;
  }
  return (short)events;
}

/*
 * Platform result bits to the project readiness mask. The extended
 * readable/writable variants fold into readable/writable; urgent-data
 * signaling has no project bit yet and is not surfaced — a future layer
 * can add it without changing this shape. Error, hangup, and invalid
 * conditions map one-to-one and are reported even when the caller never
 * asked for them, matching underlying wait semantics.
 */
static uint32_t translate_revents(int revents) {
  uint32_t ready = 0;

  if ((revents & (POLLIN | POLLRDNORM | POLLRDBAND)) != 0) {
    ready |= OMNI_POLLER_READY_READ;
  }
  if ((revents & (POLLOUT | POLLWRNORM | POLLWRBAND)) != 0) {
    ready |= OMNI_POLLER_READY_WRITE;
  }
  if ((revents & POLLERR) != 0) {
    ready |= OMNI_POLLER_READY_ERROR;
  }
  if ((revents & POLLHUP) != 0) {
    ready |= OMNI_POLLER_READY_HANGUP;
  }
  if ((revents & POLLNVAL) != 0) {
    ready |= OMNI_POLLER_READY_INVALID;
  }
  return ready;
}

static bool find_slot(const struct omni_poller *poller, int fd, size_t *at) {
  size_t i = 0;

  for (i = 0; i < poller->count; ++i) {
    if (poller->fds[i].fd == fd) {
      if (at != NULL) {
        *at = i;
      }
      return true;
    }
  }
  return false;
}

static void clear_slot(struct omni_poller *poller, size_t i) {
  poller->fds[i].fd = -1;
  poller->fds[i].events = 0;
  poller->fds[i].revents = 0;
  poller->tokens[i] = 0u;
}

struct omni_poller_result omni_poller_init_borrowed(struct omni_poller *poller,
                                                    struct pollfd *fds,
                                                    uint64_t *tokens,
                                                    size_t capacity) {
  size_t i = 0;

  if (poller == NULL || fds == NULL || tokens == NULL || capacity == 0u) {
    if (poller != NULL) {
      mark_inert(poller);
    }
    return make_result(OMNI_POLLER_ERR_INVALID, EINVAL, 0u);
  }
  poller->fds = fds;
  poller->tokens = tokens;
  poller->capacity = capacity;
  poller->count = 0u;
  poller->owns_backing = false;
  poller->live = true;
  for (i = 0; i < capacity; ++i) {
    clear_slot(poller, i);
  }
  return make_result(OMNI_POLLER_OK, 0, 0u);
}

struct omni_poller_result omni_poller_init_owned(struct omni_poller *poller,
                                                 size_t capacity) {
  size_t tok_bytes = 0;
  size_t fds_bytes = 0;
  size_t total = 0;
  size_t i = 0;
  unsigned char *block = NULL;

  /*
   * Single owned block, caller tokens first: the token array needs the
   * strictest alignment, and malloc already satisfies it at offset zero;
   * the descriptor array follows at a multiple of eight, which the
   * assertion below proves sufficient for its (weaker) alignment on
   * every conforming target. Revisit the layout if this ever fires.
   */
  _Static_assert(sizeof(uint64_t) % _Alignof(struct pollfd) == 0u,
                 "descriptor array fits after token array");
  if (poller == NULL || capacity == 0u) {
    if (poller != NULL) {
      mark_inert(poller);
    }
    return make_result(OMNI_POLLER_ERR_INVALID, EINVAL, 0u);
  }
  /* Overflow-checked sizing: no product is formed before its bound proof. */
  if (capacity > SIZE_MAX / sizeof(uint64_t)) {
    mark_inert(poller);
    return make_result(OMNI_POLLER_ERR_INVALID, EINVAL, 0u);
  }
  tok_bytes = capacity * sizeof(uint64_t);
  if (capacity > SIZE_MAX / sizeof(struct pollfd)) {
    mark_inert(poller);
    return make_result(OMNI_POLLER_ERR_INVALID, EINVAL, 0u);
  }
  fds_bytes = capacity * sizeof(struct pollfd);
  if (tok_bytes > SIZE_MAX - fds_bytes) {
    mark_inert(poller);
    return make_result(OMNI_POLLER_ERR_INVALID, EINVAL, 0u);
  }
  total = tok_bytes + fds_bytes;
  /* The single owned backing allocation of this poller's lifetime. */
  block = (unsigned char *)malloc(total);
  if (block == NULL) {
    mark_inert(poller);
    return make_result(OMNI_POLLER_ERR_NOMEM, errno, 0u);
  }
  poller->tokens = (uint64_t *)(void *)block;
  poller->fds = (struct pollfd *)(void *)(block + tok_bytes);
  poller->capacity = capacity;
  poller->count = 0u;
  poller->owns_backing = true;
  poller->live = true;
  for (i = 0; i < capacity; ++i) {
    clear_slot(poller, i);
  }
  return make_result(OMNI_POLLER_OK, 0, 0u);
}

struct omni_poller_result omni_poller_add(struct omni_poller *poller, int fd,
                                          uint64_t token, uint32_t interests) {
  if (poller == NULL || !poller->live) {
    return make_result(OMNI_POLLER_ERR_INVALID, EINVAL, 0u);
  }
  if (fd < 0 || !interests_valid(interests)) {
    return make_result(OMNI_POLLER_ERR_INVALID, EINVAL, 0u);
  }
  if (find_slot(poller, fd, NULL)) {
    return make_result(OMNI_POLLER_ERR_DUPLICATE, EEXIST, 0u);
  }
  if (poller->count >= poller->capacity) {
    return make_result(OMNI_POLLER_ERR_FULL, ENOSPC, 0u);
  }
  /* Appended in registration order; the dense prefix stays compact. */
  poller->fds[poller->count].fd = fd;
  poller->fds[poller->count].events = project_interests(interests);
  poller->fds[poller->count].revents = 0;
  poller->tokens[poller->count] = token;
  poller->count += 1u;
  return make_result(OMNI_POLLER_OK, 0, 0u);
}

struct omni_poller_result omni_poller_remove(struct omni_poller *poller, int fd) {
  size_t at = 0;
  size_t tail = 0;

  if (poller == NULL || !poller->live) {
    return make_result(OMNI_POLLER_ERR_INVALID, EINVAL, 0u);
  }
  if (!find_slot(poller, fd, &at)) {
    return make_result(OMNI_POLLER_ERR_NOT_FOUND, ENOENT, 0u);
  }
  /* Shift down to keep the live prefix dense and order-preserving. */
  tail = poller->count - 1u;
  if (at < tail) {
    memmove(&poller->fds[at], &poller->fds[at + 1u],
            (tail - at) * sizeof(struct pollfd));
    memmove(&poller->tokens[at], &poller->tokens[at + 1u],
            (tail - at) * sizeof(uint64_t));
  }
  poller->count = tail;
  clear_slot(poller, tail);
  return make_result(OMNI_POLLER_OK, 0, 0u);
}

struct omni_poller_result omni_poller_update(struct omni_poller *poller, int fd,
                                             uint32_t interests) {
  size_t at = 0;

  if (poller == NULL || !poller->live) {
    return make_result(OMNI_POLLER_ERR_INVALID, EINVAL, 0u);
  }
  if (!find_slot(poller, fd, &at)) {
    return make_result(OMNI_POLLER_ERR_NOT_FOUND, ENOENT, 0u);
  }
  /* Mask validated before anything is touched: failure keeps the old
   * mask and token exactly. */
  if (!interests_valid(interests)) {
    return make_result(OMNI_POLLER_ERR_INVALID, EINVAL, 0u);
  }
  poller->fds[at].events = project_interests(interests);
  poller->fds[at].revents = 0;
  return make_result(OMNI_POLLER_OK, 0, 0u);
}

struct omni_poller_result omni_poller_wait(struct omni_poller *poller,
                                           int64_t timeout_ms,
                                           struct omni_poller_event *out,
                                           size_t out_capacity) {
  int rc = 0;
  size_t i = 0;
  size_t emitted = 0;

  if (poller == NULL || !poller->live) {
    return make_result(OMNI_POLLER_ERR_INVALID, EINVAL, 0u);
  }
  /* Timeout validated first so even misuses fail fast and deterministically. */
  if (timeout_ms < (int64_t)0 || timeout_ms > (int64_t)INT_MAX) {
    return make_result(OMNI_POLLER_ERR_INVALID, EINVAL, 0u);
  }
  /* Empty poller: nothing to wait for, so return at once however long the
   * caller offered to wait. The output array may be NULL here. */
  if (poller->count == 0u) {
    return make_result(OMNI_POLLER_OK, 0, 0u);
  }
  /* Output capacity gates the wait itself: with room for every live
   * registration, translation cannot truncate or overrun. */
  if (out == NULL || out_capacity < poller->count) {
    return make_result(OMNI_POLLER_ERR_OUTPUT, ENOBUFS, 0u);
  }
  rc = poll(poller->fds, (nfds_t)poller->count, (int)timeout_ms);
  if (rc < 0) {
    int err = errno;

    if (err == EINTR) {
      return make_result(OMNI_POLLER_ERR_INTERRUPTED, err, 0u);
    }
    return make_result(OMNI_POLLER_ERR_WAIT, err, 0u);
  }
  if (rc == 0) {
    return make_result(OMNI_POLLER_OK, 0, 0u);
  }
  /* Registration order: the dense prefix scans front to back, so dispatch
   * order is deterministic for any fixed op sequence. Emitted records
   * cannot exceed the live count, which the capacity check already fit. */
  for (i = 0; i < poller->count; ++i) {
    int revents = (int)poller->fds[i].revents;

    if (revents == 0) {
      continue;
    }
    out[emitted].fd = poller->fds[i].fd;
    out[emitted].token = poller->tokens[i];
    out[emitted].ready = translate_revents(revents);
    emitted += 1u;
  }
  return make_result(OMNI_POLLER_OK, 0, emitted);
}

void omni_poller_reset(struct omni_poller *poller) {
  size_t i = 0;

  if (poller == NULL || !poller->live) {
    return;
  }
  /* Registrations dropped, backing retained, descriptors untouched. */
  for (i = 0; i < poller->capacity; ++i) {
    clear_slot(poller, i);
  }
  poller->count = 0u;
}

void omni_poller_destroy(struct omni_poller *poller) {
  if (poller == NULL || !poller->live) {
    return;
  }
  if (poller->owns_backing) {
    /* Block base is the token array by construction; the descriptor
     * array is interior. Registered descriptors are never freed here. */
    free(poller->tokens);
  }
  mark_inert(poller);
}

size_t omni_poller_capacity(const struct omni_poller *poller) {
  if (poller == NULL || !poller->live) {
    return 0u;
  }
  return poller->capacity;
}

size_t omni_poller_count(const struct omni_poller *poller) {
  if (poller == NULL || !poller->live) {
    return 0u;
  }
  return poller->count;
}
