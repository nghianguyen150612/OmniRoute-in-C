/*
 * Task 040: bounded exact HTTP route matching over one COMPLETE Task 038
 * request view.
 *
 * Validate every entry, check every route pair for duplicate definitions,
 * then classify the request. At most 64 routes and 2016 pairs are inspected,
 * using O(1) working memory. Duplicate detection is table-wide, including
 * routes unrelated to the current request. No target-only sighting returns
 * before a later exact method+target match.
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

/* Reuse Task 037's metadata contract without reparsing or reading body bytes. */
static bool complete_request_valid(const struct omni_http_request_result *request) {
  const struct omni_http_request_line_result *line = &request->request_head.request_line;
  struct omni_http_request_body_result expected;
  if (request->status != OMNI_HTTP_REQUEST_COMPLETE || request->source_read_ptr == NULL ||
      request->consumed_bytes == 0u || request->consumed_bytes > request->source_readable_length ||
      request->framing.status != OMNI_HTTP_REQUEST_FRAMING_OK ||
      request->body.status != OMNI_HTTP_REQUEST_BODY_COMPLETE || line->method.length == 0u ||
      line->method.length > OMNI_HTTP_REQUEST_LINE_MAX_METHOD_BYTES || line->target.length == 0u ||
      line->target.length > OMNI_HTTP_REQUEST_LINE_MAX_TARGET_BYTES) {
    return false;
  }
  /* Lengths are bounded, so these sums cannot overflow. A supported request
     line has two spaces, HTTP/1.x, and CRLF (12 bytes beyond method/target). */
  if (line->consumed_bytes != line->method.length + line->target.length + 12u ||
      line->consumed_bytes > request->source_readable_length ||
      line->method.data != request->source_read_ptr ||
      line->target.data != request->source_read_ptr + line->method.length + 1u ||
      request->request_head.consumed_bytes < line->consumed_bytes + 2u) {
    return false;
  }
  expected = omni_http_request_body_view(request->source_read_ptr, request->source_readable_length,
                                         &request->request_head, &request->framing);
  return expected.status == OMNI_HTTP_REQUEST_BODY_COMPLETE &&
         expected.consumed_bytes == request->consumed_bytes &&
         expected.required_total_bytes == request->required_total_bytes &&
         expected.consumed_bytes == request->body.consumed_bytes &&
         expected.required_total_bytes == request->body.required_total_bytes &&
         expected.body.data == request->body.body.data &&
         expected.body.length == request->body.body.length;
}

struct omni_http_route_result omni_http_route_match(const struct omni_http_request_result *request,
                                                    const struct omni_http_route *routes,
                                                    size_t route_count) {
  struct omni_http_route_result result;
  const struct omni_http_byte_span *method;
  const struct omni_http_byte_span *target;
  bool target_seen = false;
  bool exact_match = false;
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

  if (!complete_request_valid(request)) {
    result.status = OMNI_HTTP_ROUTE_ERR_INVALID_REQUEST;
    return result;
  }
  method = &request->request_head.request_line.method;
  target = &request->request_head.request_line.target;

  /* Validate all entries first: malformed entries precede ambiguity
     independent of position. No route bytes are read in this pass. */
  for (i = 0u; i < route_count; ++i) {
    if (routes[i].method == NULL || routes[i].method_length == 0u || routes[i].target == NULL ||
        routes[i].target_length == 0u) {
      result.status = OMNI_HTTP_ROUTE_ERR_INVALID_ROUTE;
      return result;
    }
  }
  /* Fixed maximum 64: at most 64*63/2 = 2016 pairs, no extra storage. */
  for (i = 0u; i < route_count; ++i) {
    for (size_t j = 0u; j < i; ++j) {
      if (spans_equal(routes[i].method, routes[i].method_length, routes[j].method,
                      routes[j].method_length) &&
          spans_equal(routes[i].target, routes[i].target_length, routes[j].target,
                      routes[j].target_length)) {
        result.status = OMNI_HTTP_ROUTE_AMBIGUOUS_ROUTE;
        return result;
      }
    }
  }

  for (i = 0u; i < route_count; ++i) {
    const struct omni_http_route *route = &routes[i];
    bool method_equal;
    bool target_equal;

    method_equal = spans_equal(route->method, route->method_length, method->data, method->length);
    target_equal = spans_equal(route->target, route->target_length, target->data, target->length);

    if (target_equal) {
      target_seen = true;
      if (method_equal) {
        exact_match = true;
        match_index = i;
        match_token = route->token;
      }
    }
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
