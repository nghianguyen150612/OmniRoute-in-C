/*
 * OmniRoute native backend — bounded HTTP route matcher (Task 040).
 *
 * A small, pure routing-classification primitive over one COMPLETE Task 038
 * request view. It selects a caller-defined route token using exact
 * HTTP-method plus request-target matching:
 *
 *   Task 038 COMPLETE request
 *        -> omni_http_route_match()
 *        -> MATCH(route_token) | METHOD_NOT_ALLOWED | NOT_FOUND
 *
 * This primitive only classifies. It deliberately does NOT:
 * - invoke handlers, callbacks, or any application code;
 * - generate responses, serialize JSON, or touch sockets;
 * - parse queries, percent-decode, normalize paths, fold case, or match
 *   wildcards/parameters/regex (no normalization of any kind is performed);
 * - allocate heap, retain state, or own the route table or request.
 *
 * The route table is caller-owned and borrowed: an array of
 * `struct omni_http_route` with pointer+length method/target spans (no NUL
 * termination required) plus an opaque caller token per route. The table is
 * never modified and never retained; nothing is copied.
 *
 * Matching is exact and case-sensitive on both method and target:
 * `GET` != `get`, `/v1/models` != `/v1/models/`, `/v1/models?x=1`, or
 * `/V1/MODELS`. A target that exists under another method yields
 * METHOD_NOT_ALLOWED; an unknown target yields NOT_FOUND. Duplicate exact
 * route definitions are configuration ambiguity (AMBIGUOUS_ROUTE), never a
 * silent first/last choice. The table is bounded by
 * OMNI_HTTP_ROUTE_MAX_ROUTES; a larger table is rejected without scanning.
 *
 * Work is O(R * compared method/target bytes) with R <= MAX_ROUTES and O(1)
 * additional memory: no trie, no hash table, no route-index construction.
 */
#ifndef OMNIROUTE_HTTP_ROUTE_H
#define OMNIROUTE_HTTP_ROUTE_H

#include <stddef.h>
#include <stdint.h>

#include "omniroute/http_request.h"

/*
 * Maximum number of routes in a caller-owned route table. Conservative and
 * fixed: a larger table is rejected with ERR_TOO_MANY_ROUTES rather than
 * scanned, so matching cost is always bounded.
 */
#define OMNI_HTTP_ROUTE_MAX_ROUTES 64u

/*
 * One caller-owned route definition. All spans are borrowed pointer+length
 * pairs: no NUL termination is required or assumed, and this primitive never
 * reads beyond the stated lengths. `token` is an opaque caller value,
 * interpreted by the caller only — never by this primitive.
 */
struct omni_http_route {
  const unsigned char *method;
  size_t method_length;
  const unsigned char *target;
  size_t target_length;
  uint64_t token;
};

enum omni_http_route_status {
  OMNI_HTTP_ROUTE_MATCH = 0,
  /* No route has this exact method+target; the target is unknown. */
  OMNI_HTTP_ROUTE_NOT_FOUND,
  /* The target exists under at least one other method, but not this one. */
  OMNI_HTTP_ROUTE_METHOD_NOT_ALLOWED,
  /* The table contains duplicate exact method+target definitions. */
  OMNI_HTTP_ROUTE_AMBIGUOUS_ROUTE,
  /* NULL request, or a NULL table with a nonzero route count. */
  OMNI_HTTP_ROUTE_ERR_INVALID_ARGUMENT,
  /* The Task 038 result is not a logically COMPLETE request view. */
  OMNI_HTTP_ROUTE_ERR_INVALID_REQUEST,
  /* route_count exceeds OMNI_HTTP_ROUTE_MAX_ROUTES. */
  OMNI_HTTP_ROUTE_ERR_TOO_MANY_ROUTES,
  /* A route entry has a NULL/empty method or target span. */
  OMNI_HTTP_ROUTE_ERR_INVALID_ROUTE
};

struct omni_http_route_result {
  enum omni_http_route_status status;
  /*
   * Index of the matched route in the caller's table. Valid only on MATCH;
   * 0 on every other status.
   */
  size_t route_index;
  /*
   * The matched route's opaque caller token. Valid only on MATCH; 0 on
   * every other status.
   */
  uint64_t route_token;
};

/*
 * Classify one COMPLETE Task 038 request result against a caller-owned,
 * immutable route table.
 *
 * The request must be logically COMPLETE: status COMPLETE, a complete
 * request line with nonempty method and target spans, and a nonzero
 * consumed_bytes boundary. INCOMPLETE, parse-error, framing-error, and
 * malformed synthetic COMPLETE results are rejected with
 * ERR_INVALID_REQUEST — partial or fake requests are never routed. The
 * borrowed request view must still be valid (assemble -> route -> consume);
 * this primitive retains no request pointer after returning.
 *
 * The whole table is always scanned (no early METHOD_NOT_ALLOWED), so the
 * classification never depends on route order: an exact match anywhere in
 * the table wins over METHOD_NOT_ALLOWED, and a duplicate exact definition
 * anywhere yields AMBIGUOUS_ROUTE. On any non-MATCH status route_index and
 * route_token are 0.
 *
 * Pure metadata matching: headers and body bytes are never inspected, so
 * routing depends only on the exact method and target bytes. No heap, no
 * I/O, no retained state, no production wiring.
 */
struct omni_http_route_result omni_http_route_match(
    const struct omni_http_request_result *request,
    const struct omni_http_route *routes,
    size_t route_count);

#endif /* OMNIROUTE_HTTP_ROUTE_H */
