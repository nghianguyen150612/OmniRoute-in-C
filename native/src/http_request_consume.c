/*
 * Task 039: bounded consumption of one previously assembled HTTP request.
 *
 * Work is O(1) metadata validation plus exactly one existing bytebuf consume
 * call. This file never re-parses request bytes, never rescans headers or
 * body, never compares payload content, never allocates, never performs I/O,
 * never compacts, and never resets. Bytebuf read offsets are only ever moved
 * through omni_bytebuf_consume() (the shared bytebuf contract), so the
 * canonical empty-buffer collapse that primitive performs is inherited, not
 * reimplemented here.
 */
#include "omniroute/http_request_consume.h"

struct omni_http_request_consume_result omni_http_request_consume(
    struct omni_bytebuf *buffer, const struct omni_http_request_result *request) {
  struct omni_http_request_consume_result result;
  const unsigned char *current;
  size_t readable = 0u;
  size_t consumed;

  result.status = OMNI_HTTP_REQUEST_CONSUME_ERR_INVALID_ARGUMENT;
  result.consumed_bytes = 0u;
  result.remaining_readable_bytes = 0u;

  /* NULL bytebuf, NULL result, or a bytebuf that is not live (never
     initialized, destroyed, or failed init: capacity reports zero). */
  if (buffer == NULL || request == NULL)
    return result;
  if (omni_bytebuf_capacity(buffer) == 0u)
    return result;

  /* Complete-only rule: INCOMPLETE, parse/framing errors, head limits, and
     argument/state errors never consume. Checked before any read-view work so
     a non-COMPLETE result is rejected identically on any buffer state. */
  if (request->status != OMNI_HTTP_REQUEST_COMPLETE) {
    result.status = OMNI_HTTP_REQUEST_CONSUME_ERR_NOT_COMPLETE;
    return result;
  }

  /* An empty readable region cannot still be the source of a COMPLETE
     request: the readable-start identity check below has no non-NULL current
     pointer to compare against. Reject rather than underflow. */
  readable = omni_bytebuf_readable(buffer);
  if (readable == 0u) {
    result.status = OMNI_HTTP_REQUEST_CONSUME_ERR_STALE;
    return result;
  }

  /* Stale-result identity, part 1 — pointer: the request's assembly snapshot
     must name the CURRENT readable start. This rejects a consumed, reset, or
     compacted buffer, and a second consume of an already-consumed result,
     because all three move the read offset. A safe tail append leaves the
     readable start in place, so growth between assembly and consume is still
     accepted. */
  current = omni_bytebuf_read_ptr(buffer, NULL);
  if (current == NULL || request->source_read_ptr == NULL ||
      current != request->source_read_ptr) {
    result.status = OMNI_HTTP_REQUEST_CONSUME_ERR_STALE;
    return result;
  }

  /* Stale-result identity, part 2 — generation epoch: the pointer alone is
     not identity. A consume-to-empty canonicalization and a compaction both
     return the readable start to the same backing address for a DIFFERENT
     logical region, and a reset/refill can restore the exact old address.
     The snapshot's recorded epoch must still be the current one; any
     successful nonzero consume, meaningful compact, or reset advanced it.
     A live buffer always has a nonzero generation, so a zero snapshot epoch
     (a fabricated result) can never match here either. */
  if (omni_bytebuf_read_generation(buffer) != request->source_read_generation) {
    result.status = OMNI_HTTP_REQUEST_CONSUME_ERR_STALE;
    return result;
  }

  /* Internal consistency of the COMPLETE metadata. A complete HTTP request is
     never zero bytes (the shortest possible head is longer), so zero is
     rejected instead of consumed as a no-op success. The consumed boundary is
     taken only from the Task 038 result: it is never re-derived from the
     request line, headers, Content-Length, or body span. */
  consumed = request->consumed_bytes;
  if (consumed == 0u) {
    result.status = OMNI_HTTP_REQUEST_CONSUME_ERR_INCONSISTENT;
    return result;
  }
  if (consumed > readable) {
    result.status = OMNI_HTTP_REQUEST_CONSUME_ERR_INCONSISTENT;
    return result;
  }
  /* The request boundary must have been inside the readable region that
     produced it; a truncated snapshot is a fabricated result. */
  if (request->source_readable_length < consumed) {
    result.status = OMNI_HTTP_REQUEST_CONSUME_ERR_INCONSISTENT;
    return result;
  }

  /* Single bounded consumption. A failure here is reported, never retried and
     never worked around by adjusting offsets directly. */
  if (!omni_bytebuf_consume(buffer, consumed)) {
    result.status = OMNI_HTTP_REQUEST_CONSUME_ERR_BYTEBUF;
    return result;
  }

  result.status = OMNI_HTTP_REQUEST_CONSUME_OK;
  result.consumed_bytes = consumed;
  result.remaining_readable_bytes = omni_bytebuf_readable(buffer);
  return result;
}
