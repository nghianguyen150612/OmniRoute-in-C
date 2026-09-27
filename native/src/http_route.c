/*
 * Task 040: bounded exact HTTP route matching over one COMPLETE Task 038
 * request view.
 *
 * The whole matcher is a single bounded scan over the caller's route table
 * with O(1) working memory: one pass records whether an exact method+target
 * match was seen (with its index and token), whether the target was seen
 * under any other method, and whether a second exact definition appears
 * anywhere in the table. Because the scan never stops early, the result is
 * independent of route order and METHOD_NOT_ALLOWED is never reported
 * before a later exact match could be found.
 *
 * Only exact byte comparisons are used: method and target spans are compared
 * by length first, then memcmp only when the lengths are already proven
 * equal. No NUL-terminated string API, no case folding, no query handling,
 * no path normalization, no prefix matching. Headers and body bytes are
 * never inspected.
 *
 * Every table entry is validated (nonempty, non-NULL method and target
 * spans) before its bytes are compared; a malformed entry is an explicit
 * configuration error, never a silently skipped row.
 *
 * No heap allocation, no I/O, no retained state, no production wiring.
 */

#include "omniroute/http_route.h"

#include <string.h>

/* Length-first exact span comparison; memcmp only on proven-equal lengths. */
static bool spans_equal(const unsigned char *a, size_t a_length, const unsigned char *b,
                         size_t b_length) {
  if (a_length != b_length) {
    return false;
  }
  return memcmp(a, b, a_length) == 0;
}

struct omni_http_route_result omni_http_route_match(
    const struct omni_http_request_result *request, const struct omni_http_route *routes,
    size_t route_count) {
  struct omni_http_route_result result;
  const struct omni_http_byte_span *method;
  const struct omni_http_byte_span *target;
  bool target_seen = false;
  bool exact_match = false;
  bool duplicate_match = false;
  size_t match_index = 0u;
  uint64_t match_token = 0u;
  size_t i = 0u;

  result.status = OMNI_HTTP_ROUTE_ERR_INVALID_ARGUMENT;
  result.route_index = 0u;
  result.route_token = 0u;

  if (request == NULL) {
    return result;
  }
  /* Bounded table: reject before scanning anything. */
  if (route_count > OMNI_HTTP_ROUTE_MAX_ROUTES) {
    result.status = OMNI_HTTP_ROUTE_ERR_TOO_MANY_ROUTES;
    return result;
  }
  if (route_count > 0u && routes == NULL) {
    return result;
  }

  /* Only a logically COMPLETE Task 038 result is routable. The request line
     must be complete with nonempty method and target spans, and the consumed
     boundary must be nonzero. Anything else — INCOMPLETE, parse/framing
     errors, or a fabricated COMPLETE shape — is rejected before routing. */
  if (request->status != OMNI_HTTP_REQUEST_COMPLETE ||
      request->request_head.status != OMNI_HTTP_REQUEST_HEAD_COMPLETE ||
      request->request_head.request_line.status != OMNI_HTTP_REQUEST_LINE_COMPLETE ||
      request->consumed_bytes == 0u) {
    result.status = OMNI_HTTP_ROUTE_ERR_INVALID_REQUEST;
    return result;
  }
  method = &request->request_head.request_line.method;
  target = &request->request_head.request_line.target;
  if (method->data == NULL || method->length == 0u || target->data == NULL ||
      target->length == 0u) {
    result.status = OMNI_HTTP_ROUTE_ERR_INVALID_REQUEST;
    return result;
  }

  /* Single bounded scan: no early returns inside the loop, so an exact
     match later in the table always beats an earlier target-only sighting,
     and a duplicate anywhere is still found. */
  for (i = 0u; i < route_count; ++i) {
    const struct omni_http_route *route = &routes[i];
    bool method_equal;
    bool target_equal;

    if (route->method == NULL || route->method_length == 0u || route->target == NULL ||
        route->target_length == 0u) {
      result.status = OMNI_HTTP_ROUTE_ERR_INVALID_ROUTE;
      return result;
    }

    method_equal =
        spans_equal(route->method, route->method_length, method->data, method->length);
    target_equal =
        spans_equal(route->target, route->target_length, target->data, target->length);

    if (target_equal) {
      target_seen = true;
      if (method_equal) {
        if (exact_match) {
          /* A second exact definition: configuration ambiguity. Keep
             scanning so a later malformed entry is still reported as
             ERR_INVALID_ROUTE rather than masked. */
          duplicate_match = true;
        } else {
          exact_match = true;
          match_index = i;
          match_token = route->token;
        }
      }
    }
  }

  if (duplicate_match) {
    result.status = OMNI_HTTP_ROUTE_AMBIGUOUS_ROUTE;
    return result;
  }
  if (exact_match) {
    result.status = OMNI_HTTP_ROUTE_MATCH;
    result.route_index = match_index;
    result.route_token = match_token;
    return result;
  }
  if (target_seen) {
    result.status = OMNI_HTTP_ROUTE_METHOD_NOT_ALLOWED;
    return result;
  }
  result.status = OMNI_HTTP_ROUTE_NOT_FOUND;
  return result;
}
