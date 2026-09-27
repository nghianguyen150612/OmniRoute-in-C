/*
 * Task 040: bounded exact HTTP route matching over COMPLETE Task 038 views.
 *
 * Every case drives the real Task 038 assembler over a real bytebuf (or a
 * deliberately forged result for the invalid-request cases) and the real
 * route matcher: no synthetic-only matching paths, no networking, no heap,
 * no file descriptors. The suite proves exact method+target semantics,
 * case sensitivity, prefix/query rejection, order independence, duplicate
 * ambiguity, table bounds, invalid-request rejection, the real
 * assemble -> route -> consume -> next lifecycle, binary-body and header
 * independence, immutability, and 1,000 bounded lifecycle cycles.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "omniroute/http_request.h"
#include "omniroute/http_request_consume.h"
#include "omniroute/http_route.h"

#define STORAGE_CAPACITY 32768u
#define HEADER_CAPACITY 8u

static const unsigned char REQ_HEALTH[] = "GET /health HTTP/1.1\r\n\r\n";
static const unsigned char REQ_MODELS[] = "GET /v1/models HTTP/1.1\r\nHost: a\r\n\r\n";
static const unsigned char REQ_CHAT[] =
    "POST /v1/chat/completions HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}";
static const unsigned char REQ_LOWER_METHOD[] = "get /x HTTP/1.1\r\n\r\n";
static const unsigned char REQ_UPPER_TARGET[] = "GET /Health HTTP/1.1\r\n\r\n";
static const unsigned char REQ_QUERY[] = "GET /v1/models?x=1 HTTP/1.1\r\n\r\n";
static const unsigned char REQ_TRAILING_SLASH[] = "GET /v1/models/ HTTP/1.1\r\n\r\n";
static const unsigned char REQ_PREFIX_SHORT[] = "GET /v1/model HTTP/1.1\r\n\r\n";
static const unsigned char REQ_PREFIX_LONG[] = "GET /v1/models-extra HTTP/1.1\r\n\r\n";
static const unsigned char REQ_UNKNOWN[] = "GET /missing HTTP/1.1\r\n\r\n";
static const unsigned char REQ_WRONG_METHOD[] = "POST /health HTTP/1.1\r\n\r\n";
static const unsigned char REQ_BINARY_BODY[] =
    "POST /v1/responses HTTP/1.1\r\nContent-Length: 4\r\n\r\n"
    "\x00\x7f\x80\xff";
static const unsigned char REQ_EXTRA_HEADERS[] =
    "GET /health HTTP/1.1\r\nHost: a\r\nAccept: b\r\nX-Dup: 1\r\nX-Dup: 2\r\n\r\n";

static size_t checks;
static size_t failures;

static void check(bool condition, const char *label) {
  ++checks;
  if (!condition) {
    ++failures;
    (void)fprintf(stderr, "NOT OK: %s\n", label);
  }
}

struct fixture {
  unsigned char storage[STORAGE_CAPACITY];
  struct omni_bytebuf buffer;
  struct omni_http_header headers[HEADER_CAPACITY];
};

struct fixture_snapshot {
  struct omni_bytebuf buffer;
  unsigned char storage[STORAGE_CAPACITY];
};

static bool load_raw(struct fixture *fixture, const unsigned char *data, size_t length) {
  (void)memset(fixture->storage, 0xa5, sizeof(fixture->storage));
  (void)memset(fixture->headers, 0xcd, sizeof(fixture->headers));
  if (!omni_bytebuf_init_borrowed(&fixture->buffer, fixture->storage, sizeof(fixture->storage)))
    return false;
  if (!omni_bytebuf_append(&fixture->buffer, data, length)) {
    omni_bytebuf_destroy(&fixture->buffer);
    return false;
  }
  return true;
}

static struct fixture_snapshot snapshot_fixture(const struct fixture *fixture) {
  struct fixture_snapshot snapshot;

  snapshot.buffer = fixture->buffer;
  (void)memcpy(snapshot.storage, fixture->storage, sizeof(snapshot.storage));
  return snapshot;
}

static void check_unchanged(const struct fixture *fixture, const struct fixture_snapshot *snapshot,
                            const char *label) {
  check(memcmp(&fixture->buffer, &snapshot->buffer, sizeof(fixture->buffer)) == 0, label);
  check(memcmp(fixture->storage, snapshot->storage, sizeof(fixture->storage)) == 0,
        "bytebuf backing remains byte-identical");
}

static struct omni_http_request_result assemble(struct fixture *fixture) {
  return omni_http_request_assemble(&fixture->buffer, fixture->headers, HEADER_CAPACITY);
}

/* Build one route from C string literals (test fixtures only). */
static struct omni_http_route make_route(const char *method, const char *target, uint64_t token) {
  struct omni_http_route route;

  route.method = (const unsigned char *)method;
  route.method_length = strlen(method);
  route.target = (const unsigned char *)target;
  route.target_length = strlen(target);
  route.token = token;
  return route;
}

static const struct omni_http_route *empty_table(void) { return NULL; }

/* ------------------------------------------------------------ empty table */

static void test_empty_table(void) {
  struct fixture fixture;
  struct omni_http_request_result result;
  struct omni_http_route_result matched;

  (void)memset(&fixture, 0, sizeof(fixture));
  check(load_raw(&fixture, REQ_HEALTH, sizeof(REQ_HEALTH) - 1u), "empty-table fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "empty-table request completes");
  matched = omni_http_route_match(&result, empty_table(), 0u);
  check(matched.status == OMNI_HTTP_ROUTE_NOT_FOUND, "empty table classifies as NOT_FOUND");
  check(matched.route_index == 0u && matched.route_token == 0u,
        "empty table reports no index or token");
  omni_bytebuf_destroy(&fixture.buffer);
}

/* ---------------------------------------------------------- exact matching */

static void test_single_exact_match(void) {
  struct fixture fixture;
  struct omni_http_request_result result;
  struct omni_http_route table[1];
  struct omni_http_route_result matched;

  (void)memset(&fixture, 0, sizeof(fixture));
  table[0] = make_route("GET", "/health", 1u);
  check(load_raw(&fixture, REQ_HEALTH, sizeof(REQ_HEALTH) - 1u), "single-match fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "single-match request completes");
  matched = omni_http_route_match(&result, table, 1u);
  check(matched.status == OMNI_HTTP_ROUTE_MATCH, "single exact route matches");
  check(matched.route_index == 0u, "match reports the route index");
  check(matched.route_token == 1u, "match reports the caller token");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_multiple_routes_all_match(void) {
  struct fixture fixture;
  struct omni_http_route table[4];
  static const unsigned char *const requests[4] = {REQ_HEALTH, REQ_MODELS, REQ_CHAT,
                                                   REQ_BINARY_BODY};
  static const size_t lengths[4] = {
      sizeof(REQ_HEALTH) - 1u,
      sizeof(REQ_MODELS) - 1u,
      sizeof(REQ_CHAT) - 1u,
      sizeof(REQ_BINARY_BODY) - 1u,
  };
  static const uint64_t tokens[4] = {1u, 2u, 3u, 4u};
  size_t i;

  (void)memset(&fixture, 0, sizeof(fixture));
  table[0] = make_route("GET", "/health", 1u);
  table[1] = make_route("GET", "/v1/models", 2u);
  table[2] = make_route("POST", "/v1/chat/completions", 3u);
  table[3] = make_route("POST", "/v1/responses", 4u);
  for (i = 0u; i < 4u; ++i) {
    struct omni_http_request_result result;
    struct omni_http_route_result matched;

    check(load_raw(&fixture, requests[i], lengths[i]), "multi-route fixture loads");
    result = assemble(&fixture);
    check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "multi-route request completes");
    matched = omni_http_route_match(&result, table, 4u);
    check(matched.status == OMNI_HTTP_ROUTE_MATCH, "multi-route exact request matches");
    check(matched.route_index == i, "multi-route match reports the right index");
    check(matched.route_token == tokens[i], "multi-route match reports the right token");
    omni_bytebuf_destroy(&fixture.buffer);
  }
}

/* ------------------------------------------------- method/target semantics */

static void test_method_mismatch(void) {
  struct fixture fixture;
  struct omni_http_route table[1];
  struct omni_http_request_result result;
  struct omni_http_route_result matched;

  (void)memset(&fixture, 0, sizeof(fixture));
  table[0] = make_route("GET", "/health", 1u);
  check(load_raw(&fixture, REQ_WRONG_METHOD, sizeof(REQ_WRONG_METHOD) - 1u),
        "method-mismatch fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "method-mismatch request completes");
  matched = omni_http_route_match(&result, table, 1u);
  check(matched.status == OMNI_HTTP_ROUTE_METHOD_NOT_ALLOWED,
        "existing target under another method is METHOD_NOT_ALLOWED");
  check(matched.route_index == 0u && matched.route_token == 0u,
        "METHOD_NOT_ALLOWED reports no index or token");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_unknown_target(void) {
  struct fixture fixture;
  struct omni_http_route table[2];
  struct omni_http_request_result result;
  struct omni_http_route_result matched;

  (void)memset(&fixture, 0, sizeof(fixture));
  table[0] = make_route("GET", "/health", 1u);
  table[1] = make_route("POST", "/v1/chat/completions", 3u);
  check(load_raw(&fixture, REQ_UNKNOWN, sizeof(REQ_UNKNOWN) - 1u), "unknown-target fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "unknown-target request completes");
  matched = omni_http_route_match(&result, table, 2u);
  check(matched.status == OMNI_HTTP_ROUTE_NOT_FOUND, "unknown target is NOT_FOUND");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_method_case_sensitivity(void) {
  struct fixture fixture;
  struct omni_http_route table[1];
  struct omni_http_request_result result;
  struct omni_http_route_result matched;

  (void)memset(&fixture, 0, sizeof(fixture));
  table[0] = make_route("GET", "/x", 7u);
  check(load_raw(&fixture, REQ_LOWER_METHOD, sizeof(REQ_LOWER_METHOD) - 1u),
        "method-case fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "method-case request completes");
  matched = omni_http_route_match(&result, table, 1u);
  check(matched.status == OMNI_HTTP_ROUTE_METHOD_NOT_ALLOWED,
        "lowercase method never exact-matches GET (target exists)");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_target_case_sensitivity(void) {
  struct fixture fixture;
  struct omni_http_route table[1];
  struct omni_http_request_result result;
  struct omni_http_route_result matched;

  (void)memset(&fixture, 0, sizeof(fixture));
  table[0] = make_route("GET", "/health", 1u);
  check(load_raw(&fixture, REQ_UPPER_TARGET, sizeof(REQ_UPPER_TARGET) - 1u),
        "target-case fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "target-case request completes");
  matched = omni_http_route_match(&result, table, 1u);
  check(matched.status == OMNI_HTTP_ROUTE_NOT_FOUND,
        "differently-cased target is a different target (NOT_FOUND)");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_prefix_and_query_rejection(void) {
  struct fixture fixture;
  struct omni_http_route table[1];
  static const unsigned char *const requests[4] = {REQ_PREFIX_SHORT, REQ_PREFIX_LONG,
                                                   REQ_TRAILING_SLASH, REQ_QUERY};
  static const size_t lengths[4] = {
      sizeof(REQ_PREFIX_SHORT) - 1u,
      sizeof(REQ_PREFIX_LONG) - 1u,
      sizeof(REQ_TRAILING_SLASH) - 1u,
      sizeof(REQ_QUERY) - 1u,
  };
  size_t i;

  (void)memset(&fixture, 0, sizeof(fixture));
  table[0] = make_route("GET", "/v1/models", 2u);
  for (i = 0u; i < 4u; ++i) {
    struct omni_http_request_result result;
    struct omni_http_route_result matched;

    check(load_raw(&fixture, requests[i], lengths[i]), "prefix/query fixture loads");
    result = assemble(&fixture);
    check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "prefix/query request completes");
    matched = omni_http_route_match(&result, table, 1u);
    check(matched.status == OMNI_HTTP_ROUTE_NOT_FOUND,
          "prefix, suffix, trailing slash, and query are not exact matches");
    omni_bytebuf_destroy(&fixture.buffer);
  }
}

/* ------------------------------------------------------ duplicate handling */

static void test_duplicate_exact_routes(void) {
  struct fixture fixture;
  struct omni_http_route table[2];
  struct omni_http_request_result result;
  struct omni_http_route_result matched;

  (void)memset(&fixture, 0, sizeof(fixture));
  table[0] = make_route("GET", "/x", 1u);
  table[1] = make_route("GET", "/x", 2u);
  check(load_raw(&fixture, (const unsigned char *)"GET /x HTTP/1.1\r\n\r\n", 19u),
        "duplicate-adjacent fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "duplicate-adjacent request completes");
  matched = omni_http_route_match(&result, table, 2u);
  check(matched.status == OMNI_HTTP_ROUTE_AMBIGUOUS_ROUTE,
        "adjacent duplicate exact routes are ambiguous");
  check(matched.route_index == 0u && matched.route_token == 0u,
        "ambiguous table reports no index or token");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_duplicate_separated_by_unrelated_routes(void) {
  struct fixture fixture;
  struct omni_http_route table[3];
  struct omni_http_request_result result;
  struct omni_http_route_result matched;

  (void)memset(&fixture, 0, sizeof(fixture));
  table[0] = make_route("GET", "/x", 1u);
  table[1] = make_route("POST", "/v1/chat/completions", 3u);
  table[2] = make_route("GET", "/x", 2u);
  check(load_raw(&fixture, (const unsigned char *)"GET /x HTTP/1.1\r\n\r\n", 19u),
        "duplicate-separated fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "duplicate-separated request completes");
  matched = omni_http_route_match(&result, table, 3u);
  check(matched.status == OMNI_HTTP_ROUTE_AMBIGUOUS_ROUTE,
        "duplicates separated by unrelated routes are ambiguous");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_duplicate_identical_tokens_still_ambiguous(void) {
  struct fixture fixture;
  struct omni_http_route table[2];
  struct omni_http_request_result result;
  struct omni_http_route_result matched;

  (void)memset(&fixture, 0, sizeof(fixture));
  table[0] = make_route("GET", "/x", 5u);
  table[1] = make_route("GET", "/x", 5u);
  check(load_raw(&fixture, (const unsigned char *)"GET /x HTTP/1.1\r\n\r\n", 19u),
        "duplicate-identical fixture loads");
  result = assemble(&fixture);
  matched = omni_http_route_match(&result, table, 2u);
  check(matched.status == OMNI_HTTP_ROUTE_AMBIGUOUS_ROUTE,
        "even identical-token duplicates are rejected as ambiguous");
  omni_bytebuf_destroy(&fixture.buffer);
}

/* --------------------------------------------------------- order independence */

static void test_route_order_independence(void) {
  struct fixture fixture;
  struct omni_http_route forward[2];
  struct omni_http_route reversed[2];
  struct omni_http_request_result result;
  struct omni_http_route_result matched;

  (void)memset(&fixture, 0, sizeof(fixture));
  forward[0] = make_route("POST", "/x", 9u);
  forward[1] = make_route("GET", "/x", 8u);
  reversed[0] = make_route("GET", "/x", 8u);
  reversed[1] = make_route("POST", "/x", 9u);

  /* POST /x must match in both orders (no early METHOD_NOT_ALLOWED). */
  check(load_raw(&fixture, (const unsigned char *)"POST /x HTTP/1.1\r\n\r\n", 20u),
        "order fixture loads POST /x");
  result = assemble(&fixture);
  matched = omni_http_route_match(&result, forward, 2u);
  check(matched.status == OMNI_HTTP_ROUTE_MATCH && matched.route_token == 9u,
        "POST /x matches with POST-first order");
  omni_bytebuf_destroy(&fixture.buffer);

  check(load_raw(&fixture, (const unsigned char *)"POST /x HTTP/1.1\r\n\r\n", 20u),
        "order fixture reloads POST /x");
  result = assemble(&fixture);
  matched = omni_http_route_match(&result, reversed, 2u);
  check(matched.status == OMNI_HTTP_ROUTE_MATCH && matched.route_token == 9u,
        "POST /x matches with GET-first order");
  omni_bytebuf_destroy(&fixture.buffer);

  /* GET /x must match in both orders too. */
  check(load_raw(&fixture, (const unsigned char *)"GET /x HTTP/1.1\r\n\r\n", 19u),
        "order fixture loads GET /x");
  result = assemble(&fixture);
  matched = omni_http_route_match(&result, forward, 2u);
  check(matched.status == OMNI_HTTP_ROUTE_MATCH && matched.route_token == 8u,
        "GET /x matches with POST-first order");
  omni_bytebuf_destroy(&fixture.buffer);

  check(load_raw(&fixture, (const unsigned char *)"GET /x HTTP/1.1\r\n\r\n", 19u),
        "order fixture reloads GET /x");
  result = assemble(&fixture);
  matched = omni_http_route_match(&result, reversed, 2u);
  check(matched.status == OMNI_HTTP_ROUTE_MATCH && matched.route_token == 8u,
        "GET /x matches with GET-first order");
  omni_bytebuf_destroy(&fixture.buffer);

  /* A method-only mismatch must be METHOD_NOT_ALLOWED in both orders. */
  check(load_raw(&fixture, (const unsigned char *)"DELETE /x HTTP/1.1\r\n\r\n", 22u),
        "order fixture loads DELETE /x");
  result = assemble(&fixture);
  matched = omni_http_route_match(&result, forward, 2u);
  check(matched.status == OMNI_HTTP_ROUTE_METHOD_NOT_ALLOWED,
        "DELETE /x is METHOD_NOT_ALLOWED with POST-first order");
  omni_bytebuf_destroy(&fixture.buffer);

  check(load_raw(&fixture, (const unsigned char *)"DELETE /x HTTP/1.1\r\n\r\n", 22u),
        "order fixture reloads DELETE /x");
  result = assemble(&fixture);
  matched = omni_http_route_match(&result, reversed, 2u);
  check(matched.status == OMNI_HTTP_ROUTE_METHOD_NOT_ALLOWED,
        "DELETE /x is METHOD_NOT_ALLOWED with GET-first order");
  omni_bytebuf_destroy(&fixture.buffer);
}

/* --------------------------------------------------------- table validation */

static void test_invalid_route_entries(void) {
  struct fixture fixture;
  struct omni_http_route table[1];
  struct omni_http_request_result result;
  struct omni_http_route_result matched;

  (void)memset(&fixture, 0, sizeof(fixture));
  check(load_raw(&fixture, REQ_HEALTH, sizeof(REQ_HEALTH) - 1u), "invalid-route fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "invalid-route request completes");

  table[0] = make_route("GET", "/health", 1u);
  table[0].method = NULL;
  matched = omni_http_route_match(&result, table, 1u);
  check(matched.status == OMNI_HTTP_ROUTE_ERR_INVALID_ROUTE, "NULL method is an invalid route");

  table[0] = make_route("GET", "/health", 1u);
  table[0].method_length = 0u;
  matched = omni_http_route_match(&result, table, 1u);
  check(matched.status == OMNI_HTTP_ROUTE_ERR_INVALID_ROUTE,
        "zero method length is an invalid route");

  table[0] = make_route("GET", "/health", 1u);
  table[0].target = NULL;
  matched = omni_http_route_match(&result, table, 1u);
  check(matched.status == OMNI_HTTP_ROUTE_ERR_INVALID_ROUTE, "NULL target is an invalid route");

  table[0] = make_route("GET", "/health", 1u);
  table[0].target_length = 0u;
  matched = omni_http_route_match(&result, table, 1u);
  check(matched.status == OMNI_HTTP_ROUTE_ERR_INVALID_ROUTE,
        "zero target length is an invalid route");

  /* A NULL table with a nonzero count is an argument error. */
  matched = omni_http_route_match(&result, NULL, 1u);
  check(matched.status == OMNI_HTTP_ROUTE_ERR_INVALID_ARGUMENT,
        "NULL table with nonzero count is invalid argument");

  /* A NULL request is an argument error. */
  matched = omni_http_route_match(NULL, table, 1u);
  check(matched.status == OMNI_HTTP_ROUTE_ERR_INVALID_ARGUMENT, "NULL request is invalid argument");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_route_count_bound(void) {
  struct fixture fixture;
  struct omni_http_route table[OMNI_HTTP_ROUTE_MAX_ROUTES + 1u];
  struct omni_http_request_result result;
  struct omni_http_route_result matched;
  char methods[OMNI_HTTP_ROUTE_MAX_ROUTES + 1u][8];
  char targets[OMNI_HTTP_ROUTE_MAX_ROUTES + 1u][24];
  size_t i;

  (void)memset(&fixture, 0, sizeof(fixture));
  for (i = 0u; i < OMNI_HTTP_ROUTE_MAX_ROUTES + 1u; ++i) {
    int written = snprintf(methods[i], sizeof(methods[i]), "M%zu", i % 4u);
    int twritten = snprintf(targets[i], sizeof(targets[i]), "/r/%zu", i);

    if (written < 0 || twritten < 0) {
      check(false, "route-count fixture formats");
      return;
    }
    table[i] = make_route(methods[i], targets[i], (uint64_t)(1000u + i));
  }
  check(load_raw(&fixture, REQ_HEALTH, sizeof(REQ_HEALTH) - 1u), "route-count fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "route-count request completes");

  /* Exactly MAX_ROUTES entries: the last one must be reachable. */
  matched = omni_http_route_match(&result, table, OMNI_HTTP_ROUTE_MAX_ROUTES);
  check(matched.status == OMNI_HTTP_ROUTE_NOT_FOUND,
        "MAX_ROUTES entries scan fine (no match for GET /health)");
  table[OMNI_HTTP_ROUTE_MAX_ROUTES - 1u] = make_route("GET", "/health", 4242u);
  matched = omni_http_route_match(&result, table, OMNI_HTTP_ROUTE_MAX_ROUTES);
  check(matched.status == OMNI_HTTP_ROUTE_MATCH && matched.route_token == 4242u &&
            matched.route_index == OMNI_HTTP_ROUTE_MAX_ROUTES - 1u,
        "exact MAX_ROUTES bound works, including the last entry");

  /* One past the bound: rejected without scanning. */
  matched = omni_http_route_match(&result, table, OMNI_HTTP_ROUTE_MAX_ROUTES + 1u);
  check(matched.status == OMNI_HTTP_ROUTE_ERR_TOO_MANY_ROUTES,
        "MAX_ROUTES+1 is rejected as too many routes");
  check(matched.route_index == 0u && matched.route_token == 0u,
        "too-many-routes reports no index or token");
  matched = omni_http_route_match(&result, table, SIZE_MAX);
  check(matched.status == OMNI_HTTP_ROUTE_ERR_TOO_MANY_ROUTES,
        "SIZE_MAX count rejects before any table arithmetic");
  {
    const struct omni_http_route one = make_route("GET", "/health", 7u);
    matched = omni_http_route_match(&result, &one, OMNI_HTTP_ROUTE_MAX_ROUTES + 1u);
    check(matched.status == OMNI_HTTP_ROUTE_ERR_TOO_MANY_ROUTES,
          "over-limit count does not scan even a one-entry allocation");
  }
  omni_bytebuf_destroy(&fixture.buffer);
}

/* ------------------------------------------------------ invalid requests */

static void test_invalid_request_results(void) {
  struct fixture fixture;
  struct omni_http_route table[1];
  static const unsigned char partial[] = "GET /health HTTP/1.1";
  static const unsigned char bad_head[] = "GET / HTTP/1.1\r\nBad Header\r\n\r\n";
  static const unsigned char bad_framing[] = "POST / HTTP/1.1\r\nContent-Length: nope\r\n\r\n";
  static const unsigned char unsupported[] =
      "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
  static const unsigned char *const inputs[4] = {partial, bad_head, bad_framing, unsupported};
  static const size_t lengths[4] = {
      sizeof(partial) - 1u,
      sizeof(bad_head) - 1u,
      sizeof(bad_framing) - 1u,
      sizeof(unsupported) - 1u,
  };
  size_t i;

  (void)memset(&fixture, 0, sizeof(fixture));
  table[0] = make_route("GET", "/health", 1u);
  for (i = 0u; i < 4u; ++i) {
    struct omni_http_request_result result;
    struct omni_http_route_result matched;

    check(load_raw(&fixture, inputs[i], lengths[i]), "invalid-request fixture loads");
    result = assemble(&fixture);
    check(result.status != OMNI_HTTP_REQUEST_COMPLETE, "invalid-request case is not COMPLETE");
    matched = omni_http_route_match(&result, table, 1u);
    check(matched.status == OMNI_HTTP_ROUTE_ERR_INVALID_REQUEST,
          "non-COMPLETE results are never routed");
    omni_bytebuf_destroy(&fixture.buffer);
  }

  /* Forged COMPLETE shapes: status says COMPLETE but the metadata is not a
     logical request view. */
  check(load_raw(&fixture, REQ_HEALTH, sizeof(REQ_HEALTH) - 1u), "forged-request fixture loads");
  {
    struct omni_http_request_result result = assemble(&fixture);
    struct omni_http_route_result matched;

    check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "forged baseline completes");

    /* Complete status, but the request line is not complete. */
    result.request_head.status = OMNI_HTTP_REQUEST_HEAD_INCOMPLETE;
    matched = omni_http_route_match(&result, table, 1u);
    check(matched.status == OMNI_HTTP_ROUTE_ERR_INVALID_REQUEST,
          "COMPLETE status with an incomplete head is rejected");
    result = assemble(&fixture);

    /* Complete everything, but an empty method span. */
    result.request_head.request_line.method.length = 0u;
    matched = omni_http_route_match(&result, table, 1u);
    check(matched.status == OMNI_HTTP_ROUTE_ERR_INVALID_REQUEST,
          "COMPLETE status with an empty method span is rejected");
    result = assemble(&fixture);

    /* Complete everything, but an empty target span. */
    result.request_head.request_line.target.length = 0u;
    matched = omni_http_route_match(&result, table, 1u);
    check(matched.status == OMNI_HTTP_ROUTE_ERR_INVALID_REQUEST,
          "COMPLETE status with an empty target span is rejected");
    result = assemble(&fixture);

    /* Complete spans, but a zero consumed boundary. */
    result.consumed_bytes = 0u;
    matched = omni_http_route_match(&result, table, 1u);
    check(matched.status == OMNI_HTTP_ROUTE_ERR_INVALID_REQUEST,
          "COMPLETE status with a zero consumed boundary is rejected");
  }
  omni_bytebuf_destroy(&fixture.buffer);
}

/* Contradictory COMPLETE child metadata must fail before byte comparisons. */
static void test_complete_metadata_consistency(void) {
  struct fixture fixture = {0};
  struct omni_http_route route = make_route("GET", "/health", 1u);
  struct omni_http_request_result baseline;
  check(load_raw(&fixture, REQ_HEALTH, sizeof(REQ_HEALTH) - 1u), "metadata fixture loads");
  baseline = assemble(&fixture);
  for (size_t variant = 0u; variant < 16u; ++variant) {
    struct omni_http_request_result forged = baseline;
    switch (variant) {
    case 0:
      forged.framing.status = OMNI_HTTP_REQUEST_FRAMING_AMBIGUOUS;
      break;
    case 1:
      forged.framing.transfer_encoding_present = true;
      break;
    case 2:
      forged.body.status = OMNI_HTTP_REQUEST_BODY_INCOMPLETE;
      break;
    case 3:
      forged.consumed_bytes += 1u;
      break;
    case 4:
      forged.required_total_bytes += 1u;
      break;
    case 5:
      forged.source_readable_length = 0u;
      break;
    case 6:
      forged.source_read_ptr = NULL;
      break;
    case 7:
      forged.request_head.request_line.method.length = SIZE_MAX;
      break;
    case 8:
      forged.request_head.request_line.target.length = SIZE_MAX;
      break;
    case 9:
      forged.request_head.request_line.version = OMNI_HTTP_VERSION_UNKNOWN;
      break;
    case 10:
      forged.request_head.request_line.consumed_bytes = 0u;
      break;
    case 11:
      forged.request_head.consumed_bytes = 0u;
      break;
    case 12:
      forged.body.body.length = 1u;
      break;
    case 13:
      forged.body.body.data = NULL;
      break;
    case 14:
      forged.request_head.request_line.method.data += 1u;
      break;
    case 15:
      forged.request_head.request_line.target.data += 1u;
      break;
    }
    struct omni_http_route_result matched = omni_http_route_match(&forged, &route, 1u);
    check(matched.status == OMNI_HTTP_ROUTE_ERR_INVALID_REQUEST,
          "contradictory COMPLETE metadata is rejected");
    check(matched.route_index == 0u && matched.route_token == 0u,
          "invalid COMPLETE metadata exposes no match");
  }
  omni_bytebuf_destroy(&fixture.buffer);
}

/* Duplicate definitions invalidate the table even for another request. */
static void test_unrelated_duplicates(void) {
  struct fixture fixture = {0};
  struct omni_http_route routes[3] = {make_route("GET", "/x", 1u), make_route("GET", "/health", 2u),
                                      make_route("GET", "/x", 3u)};
  const unsigned char *inputs[] = {REQ_HEALTH, REQ_WRONG_METHOD, REQ_UNKNOWN};
  const size_t lengths[] = {sizeof(REQ_HEALTH) - 1u, sizeof(REQ_WRONG_METHOD) - 1u,
                            sizeof(REQ_UNKNOWN) - 1u};
  for (size_t i = 0u; i < 3u; ++i) {
    check(load_raw(&fixture, inputs[i], lengths[i]), "unrelated-duplicate fixture loads");
    struct omni_http_request_result request = assemble(&fixture);
    struct omni_http_route_result result = omni_http_route_match(&request, routes, 3u);
    check(result.status == OMNI_HTTP_ROUTE_AMBIGUOUS_ROUTE,
          "unrelated duplicate invalidates every request classification");
    check(result.route_token == 0u && result.route_index == 0u,
          "unrelated ambiguity exposes no match");
    omni_bytebuf_destroy(&fixture.buffer);
  }
}

/* ------------------------------------------- real Task038 -> Task040 flow */

static void test_task038_integration(void) {
  struct fixture fixture;
  struct omni_http_route table[3];
  struct omni_http_request_result result;
  struct omni_http_route_result matched;

  (void)memset(&fixture, 0, sizeof(fixture));
  table[0] = make_route("GET", "/health", 1u);
  table[1] = make_route("GET", "/v1/models", 2u);
  table[2] = make_route("POST", "/v1/chat/completions", 3u);

  check(load_raw(&fixture, REQ_HEALTH, sizeof(REQ_HEALTH) - 1u), "integration loads GET /health");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "integration GET /health completes");
  matched = omni_http_route_match(&result, table, 3u);
  check(matched.status == OMNI_HTTP_ROUTE_MATCH && matched.route_token == 1u,
        "integration routes GET /health");
  omni_bytebuf_destroy(&fixture.buffer);

  check(load_raw(&fixture, REQ_MODELS, sizeof(REQ_MODELS) - 1u),
        "integration loads GET /v1/models");
  result = assemble(&fixture);
  matched = omni_http_route_match(&result, table, 3u);
  check(matched.status == OMNI_HTTP_ROUTE_MATCH && matched.route_token == 2u,
        "integration routes GET /v1/models");
  omni_bytebuf_destroy(&fixture.buffer);

  check(load_raw(&fixture, REQ_CHAT, sizeof(REQ_CHAT) - 1u),
        "integration loads POST /v1/chat/completions");
  result = assemble(&fixture);
  matched = omni_http_route_match(&result, table, 3u);
  check(matched.status == OMNI_HTTP_ROUTE_MATCH && matched.route_token == 3u,
        "integration routes POST /v1/chat/completions");
  omni_bytebuf_destroy(&fixture.buffer);

  check(load_raw(&fixture, REQ_UNKNOWN, sizeof(REQ_UNKNOWN) - 1u),
        "integration loads GET /missing");
  result = assemble(&fixture);
  matched = omni_http_route_match(&result, table, 3u);
  check(matched.status == OMNI_HTTP_ROUTE_NOT_FOUND, "integration classifies unknown target");
  omni_bytebuf_destroy(&fixture.buffer);

  check(load_raw(&fixture, REQ_WRONG_METHOD, sizeof(REQ_WRONG_METHOD) - 1u),
        "integration loads POST /health");
  result = assemble(&fixture);
  matched = omni_http_route_match(&result, table, 3u);
  check(matched.status == OMNI_HTTP_ROUTE_METHOD_NOT_ALLOWED,
        "integration classifies wrong method");
  omni_bytebuf_destroy(&fixture.buffer);
}

/* ------------------------------------------- Task039 lifecycle integration */

static void test_task039_lifecycle_integration(void) {
  unsigned char pipeline[256];
  struct fixture fixture;
  struct omni_http_route table[2];
  struct omni_http_request_result first;
  struct omni_http_request_result second;
  struct omni_http_route_result matched;
  struct omni_http_request_consume_result consumed;
  const size_t first_length = sizeof(REQ_HEALTH) - 1u;
  const size_t second_length = sizeof(REQ_MODELS) - 1u;

  (void)memset(&fixture, 0, sizeof(fixture));
  table[0] = make_route("GET", "/health", 1u);
  table[1] = make_route("GET", "/v1/models", 2u);
  (void)memcpy(pipeline, REQ_HEALTH, first_length);
  (void)memcpy(pipeline + first_length, REQ_MODELS, second_length);
  check(load_raw(&fixture, pipeline, first_length + second_length), "lifecycle pipeline loads");

  /* assemble -> route -> consume -> assemble -> route -> consume, with no
     compaction and no stale usage. */
  first = assemble(&fixture);
  check(first.status == OMNI_HTTP_REQUEST_COMPLETE, "lifecycle A completes");
  matched = omni_http_route_match(&first, table, 2u);
  check(matched.status == OMNI_HTTP_ROUTE_MATCH && matched.route_token == 1u,
        "lifecycle routes A before consuming");
  consumed = omni_http_request_consume(&fixture.buffer, &first);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK && consumed.consumed_bytes == first_length,
        "lifecycle consumes A exactly");
  check(consumed.remaining_readable_bytes == second_length, "lifecycle leaves B readable");

  second = assemble(&fixture);
  check(second.status == OMNI_HTTP_REQUEST_COMPLETE, "lifecycle B completes");
  matched = omni_http_route_match(&second, table, 2u);
  check(matched.status == OMNI_HTTP_ROUTE_MATCH && matched.route_token == 2u,
        "lifecycle routes the next request after consuming the first");
  consumed = omni_http_request_consume(&fixture.buffer, &second);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK && consumed.consumed_bytes == second_length,
        "lifecycle consumes B exactly");
  check(omni_bytebuf_readable(&fixture.buffer) == 0u, "lifecycle drains the pipeline");

  /* The old A result is stale now: routing it again would be a caller bug, but
     consuming it must be rejected (guarded by the generation epoch). */
  consumed = omni_http_request_consume(&fixture.buffer, &first);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_STALE,
        "lifecycle never allows consuming a stale result");
  omni_bytebuf_destroy(&fixture.buffer);
}

/* ------------------------------------------- body and header independence */

static void test_binary_body_independence(void) {
  struct fixture fixture;
  struct omni_http_route table[1];
  struct omni_http_request_result result;
  struct omni_http_route_result matched;

  (void)memset(&fixture, 0, sizeof(fixture));
  table[0] = make_route("POST", "/v1/responses", 4u);
  check(load_raw(&fixture, REQ_BINARY_BODY, sizeof(REQ_BINARY_BODY) - 1u),
        "binary-body fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "binary-body request completes");
  check(result.body.body.length == 4u && memcmp(result.body.body.data, "\x00\x7f\x80\xff", 4u) == 0,
        "binary body bytes are intact");
  matched = omni_http_route_match(&result, table, 1u);
  check(matched.status == OMNI_HTTP_ROUTE_MATCH && matched.route_token == 4u,
        "routing depends only on method+target, never on body bytes");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_header_independence(void) {
  struct fixture fixture;
  struct omni_http_route table[1];
  struct omni_http_request_result bare;
  struct omni_http_request_result headered;
  struct omni_http_route_result matched;

  (void)memset(&fixture, 0, sizeof(fixture));
  table[0] = make_route("GET", "/health", 1u);

  check(load_raw(&fixture, REQ_HEALTH, sizeof(REQ_HEALTH) - 1u), "header-independence bare loads");
  bare = assemble(&fixture);
  matched = omni_http_route_match(&bare, table, 1u);
  check(matched.status == OMNI_HTTP_ROUTE_MATCH, "bare request routes");
  omni_bytebuf_destroy(&fixture.buffer);

  check(load_raw(&fixture, REQ_EXTRA_HEADERS, sizeof(REQ_EXTRA_HEADERS) - 1u),
        "header-independence headered loads");
  headered = assemble(&fixture);
  check(headered.request_head.header_count == 4u, "unrelated and duplicate headers parse");
  matched = omni_http_route_match(&headered, table, 1u);
  check(matched.status == OMNI_HTTP_ROUTE_MATCH && matched.route_token == 1u,
        "unrelated and duplicate headers never change routing");
  omni_bytebuf_destroy(&fixture.buffer);
}

/* ------------------------------------------------------------- immutability */

static void test_immutability(void) {
  struct fixture fixture;
  struct omni_http_route table[1];
  struct omni_http_request_result result;
  struct omni_http_request_result result_before;
  struct omni_http_route table_before;
  struct fixture_snapshot snapshot;
  struct omni_http_header headers_before[HEADER_CAPACITY];
  struct omni_http_route_result matched;

  (void)memset(&fixture, 0, sizeof(fixture));
  table[0] = make_route("GET", "/v1/models", 2u);
  check(load_raw(&fixture, REQ_MODELS, sizeof(REQ_MODELS) - 1u), "immutability fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "immutability request completes");
  result_before = result;
  table_before = table[0];
  (void)memcpy(headers_before, fixture.headers, sizeof(headers_before));
  snapshot = snapshot_fixture(&fixture);

  matched = omni_http_route_match(&result, table, 1u);
  check(matched.status == OMNI_HTTP_ROUTE_MATCH && matched.route_token == 2u,
        "immutability request matches");
  check(memcmp(&result, &result_before, sizeof(result)) == 0,
        "the request result is never modified");
  check(memcmp(&table[0], &table_before, sizeof(table[0])) == 0,
        "the route table is never modified");
  check(memcmp(fixture.headers, headers_before, sizeof(headers_before)) == 0,
        "caller header storage is never modified");
  check_unchanged(&fixture, &snapshot, "immutability leaves bytebuf and backing untouched");
  omni_bytebuf_destroy(&fixture.buffer);
}

/* ---------------------------------------------------- deterministic bytes */

static void test_deterministic_byte_mutations(void) {
  struct fixture fixture;
  struct omni_http_route table[1];
  struct omni_http_request_result result;
  struct omni_http_route_result matched;
  unsigned char raw[64];
  size_t length = sizeof("GET /health HTTP/1.1\r\n\r\n") - 1u;
  size_t pos;

  (void)memset(&fixture, 0, sizeof(fixture));
  table[0] = make_route("GET", "/health", 1u);
  (void)memcpy(raw, "GET /health HTTP/1.1\r\n\r\n", length);

  /* Every single-byte mutation of the method or target span must break the
     exact match (the request line still parses, so routing still runs). */
  for (pos = 0u; pos < 14u; ++pos) {
    unsigned char original = raw[pos];
    int value;

    for (value = 0; value < 256; ++value) {
      if ((unsigned char)value == original) {
        continue;
      }
      raw[pos] = (unsigned char)value;
      if (!load_raw(&fixture, raw, length)) {
        check(false, "mutation fixture loads");
        return;
      }
      result = assemble(&fixture);
      if (result.status != OMNI_HTTP_REQUEST_COMPLETE) {
        /* Some mutations break the request line itself (e.g. NUL or CR);
           those are rejected as invalid requests, never misrouted. */
        matched = omni_http_route_match(&result, table, 1u);
        check(matched.status == OMNI_HTTP_ROUTE_ERR_INVALID_REQUEST,
              "unparsable mutation is an invalid request, never misrouted");
      } else {
        matched = omni_http_route_match(&result, table, 1u);
        check(matched.status != OMNI_HTTP_ROUTE_MATCH,
              "any method/target byte mutation breaks the exact match");
      }
      omni_bytebuf_destroy(&fixture.buffer);
    }
    raw[pos] = original;
  }

  /* The unmutated request still matches. */
  check(load_raw(&fixture, raw, length), "unmutated request loads");
  result = assemble(&fixture);
  matched = omni_http_route_match(&result, table, 1u);
  check(matched.status == OMNI_HTTP_ROUTE_MATCH && matched.route_token == 1u,
        "unmutated request still matches after the mutation sweep");
  omni_bytebuf_destroy(&fixture.buffer);
}

/* ------------------------------------------------------------- memory model */

static void test_memory_model(void) {
  check(sizeof(struct omni_http_route) == 4u * sizeof(size_t) + sizeof(uint64_t),
        "route entry stays a tiny fixed-size value type (two spans plus a token)");
  check(sizeof(struct omni_http_route_result) <= 3u * sizeof(size_t),
        "route result stays a tiny fixed-size value type (status, index, token)");
  check(OMNI_HTTP_ROUTE_MAX_ROUTES == 64u, "the public route bound is 64");
}

/* ------------------------------------------------------------------- stress */

static void test_stress_1000_lifecycle_cycles(void) {
  enum { CYCLES = 1000 };
  struct fixture fixture;
  struct omni_http_route table[3];
  static const unsigned char *const requests[5] = {REQ_HEALTH, REQ_WRONG_METHOD, REQ_UNKNOWN,
                                                   REQ_MODELS, REQ_CHAT};
  static const size_t lengths[5] = {
      sizeof(REQ_HEALTH) - 1u, sizeof(REQ_WRONG_METHOD) - 1u, sizeof(REQ_UNKNOWN) - 1u,
      sizeof(REQ_MODELS) - 1u, sizeof(REQ_CHAT) - 1u,
  };
  static const enum omni_http_route_status expected[5] = {
      OMNI_HTTP_ROUTE_MATCH,     OMNI_HTTP_ROUTE_METHOD_NOT_ALLOWED,
      OMNI_HTTP_ROUTE_NOT_FOUND, OMNI_HTTP_ROUTE_MATCH,
      OMNI_HTTP_ROUTE_MATCH,
  };
  static const uint64_t tokens[5] = {1u, 0u, 0u, 2u, 3u};
  struct omni_http_route reversed[3];
  size_t cycles_ok = 0u;
  size_t bytes_total = 0u;
  size_t bytes_expected;
  size_t i;

  (void)memset(&fixture, 0, sizeof(fixture));
  table[0] = make_route("GET", "/health", 1u);
  table[1] = make_route("GET", "/v1/models", 2u);
  table[2] = make_route("POST", "/v1/chat/completions", 3u);
  for (i = 0u; i < 3u; ++i) {
    reversed[i] = table[2u - i];
  }
  check(omni_bytebuf_init_borrowed(&fixture.buffer, fixture.storage, sizeof(fixture.storage)),
        "stress buffer initializes");
  (void)memset(fixture.storage, 0xa5, sizeof(fixture.storage));
  (void)memset(fixture.headers, 0xcd, sizeof(fixture.headers));

  for (i = 0u; i < (size_t)CYCLES; ++i) {
    const size_t variant = i % 5u;
    struct omni_http_request_result result;
    struct omni_http_route_result matched;
    struct omni_http_request_consume_result consumed;

    if (!omni_bytebuf_append(&fixture.buffer, requests[variant], lengths[variant])) {
      check(false, "stress append succeeds");
      break;
    }
    result = assemble(&fixture);
    if (result.status != OMNI_HTTP_REQUEST_COMPLETE) {
      check(false, "stress request completes");
      break;
    }
    matched = omni_http_route_match(&result, table, 3u);
    if (matched.status != expected[variant] || matched.route_token != tokens[variant]) {
      check(false, "stress classification is exact for every variant");
      break;
    }
    {
      struct omni_http_route_result reordered = omni_http_route_match(&result, reversed, 3u);
      check(reordered.status == matched.status && reordered.route_token == matched.route_token,
            "stress classification is independent of route order");
    }
    /* Routing status does not affect consumability: every COMPLETE view is
       consumed exactly once, so no byte is lost or double-consumed. */
    consumed = omni_http_request_consume(&fixture.buffer, &result);
    if (consumed.status != OMNI_HTTP_REQUEST_CONSUME_OK ||
        consumed.consumed_bytes != lengths[variant]) {
      check(false, "stress consume is exact for every variant");
      break;
    }
    if (consumed.remaining_readable_bytes != 0u) {
      check(false, "stress consume drains each request fully");
      break;
    }
    {
      struct omni_http_request_consume_result stale =
          omni_http_request_consume(&fixture.buffer, &result);
      check(stale.status == OMNI_HTTP_REQUEST_CONSUME_ERR_STALE && stale.consumed_bytes == 0u,
            "stress rejects a second consume without byte loss");
    }
    ++cycles_ok;
    bytes_total += lengths[variant];
  }
  check(cycles_ok == (size_t)CYCLES, "all 1000 assemble/route/consume cycles succeed");
  /* Each of the 5 variants appears exactly CYCLES/5 times. */
  bytes_expected =
      ((size_t)CYCLES / 5u) * (lengths[0] + lengths[1] + lengths[2] + lengths[3] + lengths[4]);
  check(bytes_total == bytes_expected, "stress consumed every appended byte exactly once");
  check(omni_bytebuf_readable(&fixture.buffer) == 0u, "stress ends with an empty readable region");
  check(omni_bytebuf_high_water(&fixture.buffer) <=
            lengths[0] + lengths[1] + lengths[2] + lengths[3] + lengths[4],
        "stress high water stays bounded: no heap growth, no byte loss");
  check(omni_bytebuf_capacity(&fixture.buffer) == sizeof(fixture.storage),
        "stress never reallocates or grows the buffer");
  omni_bytebuf_destroy(&fixture.buffer);
}

int main(void) {
  test_empty_table();
  test_single_exact_match();
  test_multiple_routes_all_match();
  test_method_mismatch();
  test_unknown_target();
  test_method_case_sensitivity();
  test_target_case_sensitivity();
  test_prefix_and_query_rejection();
  test_duplicate_exact_routes();
  test_duplicate_separated_by_unrelated_routes();
  test_duplicate_identical_tokens_still_ambiguous();
  test_route_order_independence();
  test_invalid_route_entries();
  test_route_count_bound();
  test_invalid_request_results();
  test_complete_metadata_consistency();
  test_unrelated_duplicates();
  test_task038_integration();
  test_task039_lifecycle_integration();
  test_binary_body_independence();
  test_header_independence();
  test_immutability();
  test_deterministic_byte_mutations();
  test_memory_model();
  test_stress_1000_lifecycle_cycles();
  (void)printf("http-route-unit: %zu checks, %zu failures (route=%zu bytes, result=%zu bytes, "
               "max_routes=%zu)\n",
               checks, failures, sizeof(struct omni_http_route),
               sizeof(struct omni_http_route_result), (size_t)OMNI_HTTP_ROUTE_MAX_ROUTES);
  return failures == 0u ? 0 : 1;
}
