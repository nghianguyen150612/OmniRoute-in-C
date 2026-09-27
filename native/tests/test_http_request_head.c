/* Task 035: syntax composition, exact input allocations, and storage guards. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omniroute/http_request_head.h"

static size_t checks;
static size_t failures;
static size_t prefix_cases;
static size_t mutation_cases;

static void check(bool condition, const char *name) {
  ++checks;
  if (!condition) {
    ++failures;
    fprintf(stderr, "NOT OK: %s\n", name);
  }
}

static void failure_output(struct omni_http_request_head_result r) {
  check(r.header_count == 0u && r.consumed_bytes == 0u && r.headers == NULL &&
            r.request_line.method.data == NULL && r.request_line.method.length == 0u &&
            r.request_line.target.data == NULL && r.request_line.target.length == 0u &&
            r.request_line.version == OMNI_HTTP_VERSION_UNKNOWN &&
            r.request_line.consumed_bytes == 0u && r.request_line.error_offset == 0u,
        "no partial logical output");
}

static struct omni_http_request_head_result expect(const unsigned char *data, size_t length,
                                                   size_t capacity,
                                                   enum omni_http_request_head_status status,
                                                   size_t error) {
  struct omni_http_header headers[OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS];
  struct omni_http_request_head_result r =
      omni_http_request_head_parse(data, length, headers, capacity);
  check(r.status == status, "expected status");
  check(r.error_offset == error, "expected global error offset");
  if (r.status != OMNI_HTTP_REQUEST_HEAD_COMPLETE)
    failure_output(r);
  /* Do not return the borrowed local headers pointer. */
  r.headers = NULL;
  return r;
}

static void span(struct omni_http_header_line_span s, const unsigned char *ptr, const char *value) {
  check(s.data == ptr && s.length == strlen(value), "borrowed header span offset/length");
  check(s.length == strlen(value) && memcmp(s.data, value, s.length) == 0,
        "header bytes preserved");
}

static void examples(void) {
  const unsigned char zero[] = "GET / HTTP/1.1\r\n\r\n";
  const unsigned char one[] = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
  const unsigned char multi[] = "GET /v1/models HTTP/1.1\r\nHost: localhost\r\nAccept: "
                                "application/json\r\nX-Test: abc\r\n\r\n";
  const unsigned char cases[] =
      "GET / HTTP/1.1\r\nHost: a\r\nHOST: b\r\nhost: c\r\nX-Test: a\r\nX-Test: b\r\n"
      "X-Empty:\r\nX-OWS: \t a\t b \t\r\n\r\n";
  struct omni_http_header headers[8];
  struct omni_http_request_head_result r =
      omni_http_request_head_parse(zero, sizeof(zero) - 1u, NULL, 0u);
  check(r.status == OMNI_HTTP_REQUEST_HEAD_COMPLETE && r.header_count == 0u &&
            r.consumed_bytes == sizeof(zero) - 1u,
        "zero headers and zero capacity");
  r = omni_http_request_head_parse(zero, SIZE_MAX, headers, SIZE_MAX);
  check(r.status == OMNI_HTTP_REQUEST_HEAD_COMPLETE && r.consumed_bytes == sizeof(zero) - 1u,
        "huge declared length/capacity do not overflow or inspect trailing bytes");
  r = omni_http_request_head_parse(one, sizeof(one) - 1u, headers, 8u);
  check(r.status == OMNI_HTTP_REQUEST_HEAD_COMPLETE && r.header_count == 1u &&
            r.headers == headers && r.consumed_bytes == sizeof(one) - 1u,
        "one header output");
  check(r.request_line.method.data == one && r.request_line.method.length == 3u &&
            memcmp(r.request_line.method.data, "GET", 3u) == 0 &&
            r.request_line.target.data == one + 4u && r.request_line.target.length == 1u &&
            *r.request_line.target.data == '/' && r.request_line.version == OMNI_HTTP_VERSION_1_0 &&
            r.request_line.consumed_bytes == 16u,
        "request-line zero-copy fields");
  span(headers[0].name, one + 16u, "Host");
  span(headers[0].value, one + 22u, "example.com");
  r = omni_http_request_head_parse(multi, sizeof(multi) - 1u, headers, 8u);
  check(r.status == OMNI_HTTP_REQUEST_HEAD_COMPLETE && r.header_count == 3u &&
            r.consumed_bytes == sizeof(multi) - 1u &&
            r.request_line.version == OMNI_HTTP_VERSION_1_1,
        "multiple headers");
  check(r.request_line.target.data == multi + 4u && r.request_line.target.length == 10u &&
            memcmp(r.request_line.target.data, "/v1/models", 10u) == 0,
        "multi target");
  span(headers[0].name, multi + 25u, "Host");
  span(headers[0].value, multi + 31u, "localhost");
  span(headers[1].name, multi + 42u, "Accept");
  span(headers[1].value, multi + 50u, "application/json");
  span(headers[2].name, multi + 68u, "X-Test");
  span(headers[2].value, multi + 76u, "abc");
  r = omni_http_request_head_parse(cases, sizeof(cases) - 1u, headers, 8u);
  check(r.status == OMNI_HTTP_REQUEST_HEAD_COMPLETE && r.header_count == 7u,
        "case/duplicates/empty/OWS");
  span(headers[0].name, cases + 16u, "Host");
  span(headers[1].name, cases + 25u, "HOST");
  span(headers[2].name, cases + 34u, "host");
  span(headers[3].name, cases + 43u, "X-Test");
  span(headers[4].name, cases + 54u, "X-Test");
  span(headers[5].value, cases + 73u, "");
  span(headers[6].value, cases + 84u, "a\t b");
  {
    const unsigned char semantic[] =
        "GET / HTTP/1.1\r\nContent-Length: 10\r\nContent-Length: 20\r\n"
        "Transfer-Encoding: chunked\r\nHost: a\r\nHost: b\r\n\r\n";
    r = omni_http_request_head_parse(semantic, sizeof(semantic) - 1u, headers, 8u);
    check(r.status == OMNI_HTTP_REQUEST_HEAD_COMPLETE && r.header_count == 5u,
          "no semantic interpretation");
  }
}

static void errors(void) {
  unsigned char large[OMNI_HTTP_HEADER_LINE_MAX_TOTAL_BYTES + 32u];
  struct omni_http_header h[1];
  struct omni_http_request_head_result r;
  const unsigned char missing[] = "GET / HTTP/1.1\r\nHost: a\r\n";
  const unsigned char fold[] = "GET / HTTP/1.1\r\nX-Test: abc\r\n def\r\n\r\n";
  const unsigned char tab[] = "GET / HTTP/1.1\r\nX-Test: abc\r\n\tdef\r\n\r\n";
  expect(NULL, 0u, 0u, OMNI_HTTP_REQUEST_HEAD_INCOMPLETE, 0u);
  expect(NULL, 1u, 0u, OMNI_HTTP_REQUEST_HEAD_ERR_INVALID_ARGUMENT, 0u);
  r = omni_http_request_head_parse(NULL, 0u, NULL, 1u);
  check(r.status == OMNI_HTTP_REQUEST_HEAD_ERR_INVALID_ARGUMENT, "NULL storage positive capacity");
  failure_output(r);
  r = omni_http_request_head_parse((const unsigned char *)"GET / HTTP/1.1\r\n\r\n", 18u, NULL, 1u);
  check(r.status == OMNI_HTTP_REQUEST_HEAD_ERR_INVALID_ARGUMENT, "storage validation before parse");
  failure_output(r);
  expect((const unsigned char *)"GET", 3u, 1u, OMNI_HTTP_REQUEST_HEAD_INCOMPLETE, 3u);
  expect((const unsigned char *)"GET  /", 6u, 1u, OMNI_HTTP_REQUEST_HEAD_INVALID, 4u);
  expect((const unsigned char *)"GET / HTTP/2.0\r\n", 16u, 1u,
         OMNI_HTTP_REQUEST_HEAD_UNSUPPORTED_VERSION, 11u);
  memset(large, 'A', 33u);
  expect(large, 33u, 1u, OMNI_HTTP_REQUEST_HEAD_TOO_LARGE, 32u);
  memcpy(large, "GET / HTTP/1.1\r\n", 16u);
  memset(large + 16u, 'A', 257u);
  expect(large, 273u, 1u, OMNI_HTTP_REQUEST_HEAD_TOO_LARGE, 272u);
  expect((const unsigned char *)"GET / HTTP/1.1\r\nBad : a\r\n", 25u, 1u,
         OMNI_HTTP_REQUEST_HEAD_INVALID, 19u);
  expect(missing, sizeof(missing) - 1u, 1u, OMNI_HTTP_REQUEST_HEAD_INCOMPLETE,
         sizeof(missing) - 1u);
  expect((const unsigned char *)"GET / HTTP/1.1\r\nHost: a\r", 24u, 1u,
         OMNI_HTTP_REQUEST_HEAD_INCOMPLETE, 24u);
  expect((const unsigned char *)"GET / HTTP/1.1\r\nHost: a\r\n\r", 26u, 1u,
         OMNI_HTTP_REQUEST_HEAD_INCOMPLETE, 26u);
  expect((const unsigned char *)"GET / HTTP/1.1\r\n\rx", 18u, 1u, OMNI_HTTP_REQUEST_HEAD_INVALID,
         17u);
  expect(fold, sizeof(fold) - 1u, 1u, OMNI_HTTP_REQUEST_HEAD_INVALID, 29u);
  expect(tab, sizeof(tab) - 1u, 1u, OMNI_HTTP_REQUEST_HEAD_INVALID, 29u);
  expect(fold, 30u, 1u, OMNI_HTTP_REQUEST_HEAD_INVALID, 29u);
  expect(tab, 30u, 1u, OMNI_HTTP_REQUEST_HEAD_INVALID, 29u);
  expect(fold, 30u, 0u, OMNI_HTTP_REQUEST_HEAD_TOO_MANY_HEADERS, 16u);
  /* Capacity is checked only for valid complete fields. */
  expect((const unsigned char *)"GET / HTTP/1.1\r\nX", 17u, 0u, OMNI_HTTP_REQUEST_HEAD_INCOMPLETE,
         17u);
  expect((const unsigned char *)"GET / HTTP/1.1\r\n:", 17u, 0u, OMNI_HTTP_REQUEST_HEAD_INVALID,
         16u);
  r = omni_http_request_head_parse(missing, sizeof(missing) - 1u, h, 0u);
  check(r.status == OMNI_HTTP_REQUEST_HEAD_TOO_MANY_HEADERS, "zero capacity with header");
  failure_output(r);
}

static size_t make_count(unsigned char *data, size_t count) {
  size_t used = 16u;
  memcpy(data, "GET / HTTP/1.1\r\n", used);
  for (size_t i = 0u; i < count; ++i) {
    memcpy(data + used, "X: a\r\n", 6u);
    used += 6u;
  }
  memcpy(data + used, "\r\n", 2u);
  return used + 2u;
}

static void capacity(void) {
  unsigned char data[512];
  struct {
    uint64_t before;
    struct omni_http_header h[2];
    uint64_t after;
  } guarded;
  struct omni_http_header all[OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS + 1u];
  struct omni_http_request_head_result r;
  for (size_t n = 1u; n <= 3u; ++n) {
    size_t length = make_count(data, n);
    memset(&guarded, 0xa5, sizeof(guarded));
    r = omni_http_request_head_parse(data, length, guarded.h, 2u);
    check(r.status ==
              (n <= 2u ? OMNI_HTTP_REQUEST_HEAD_COMPLETE : OMNI_HTTP_REQUEST_HEAD_TOO_MANY_HEADERS),
          "N-1/N/N+1 capacity");
    check(guarded.before == UINT64_C(0xa5a5a5a5a5a5a5a5) &&
              guarded.after == UINT64_C(0xa5a5a5a5a5a5a5a5),
          "capacity canaries unchanged");
    if (n > 2u) {
      failure_output(r);
      check(r.error_offset == 28u, "capacity error start");
    } else
      check(r.header_count == n, "accepted capacity count");
  }
  for (size_t n = OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS; n <= OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS + 1u;
       ++n) {
    size_t length = make_count(data, n);
    memset(all, 0xa5, sizeof(all));
    r = omni_http_request_head_parse(data, length, all, OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS + 1u);
    check(r.status == (n == OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS
                           ? OMNI_HTTP_REQUEST_HEAD_COMPLETE
                           : OMNI_HTTP_REQUEST_HEAD_TOO_MANY_HEADERS),
          "global count exact/over");
    for (size_t b = 0u; b < sizeof(all[0]); ++b)
      check(((unsigned char *)&all[OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS])[b] == 0xa5u,
            "global cap no write");
    if (r.status != OMNI_HTTP_REQUEST_HEAD_COMPLETE)
      failure_output(r);
  }
}

/* Build valid heads at exact total lengths, using four individually valid lines. */
static void total_limits(void) {
  unsigned char data[OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES + 1u];
  struct omni_http_header h[4];
  for (size_t total = OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES - 1u;
       total <= OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES + 1u; ++total) {
    size_t used = 16u;
    size_t budget = total - 18u;
    struct omni_http_request_head_result r;
    memcpy(data, "GET / HTTP/1.1\r\n", used);
    for (size_t i = 0u; i < 4u; ++i) {
      size_t line = budget / (4u - i);
      memset(data + used, 'X', 256u);
      data[used + 256u] = ':';
      memset(data + used + 257u, 'a', line - 259u);
      data[used + line - 2u] = '\r';
      data[used + line - 1u] = '\n';
      used += line;
      budget -= line;
    }
    data[used++] = '\r';
    data[used++] = '\n';
    check(used == total, "exact total fixture size");
    r = omni_http_request_head_parse(data, total, h, 4u);
    check(r.status == (total <= OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES
                           ? OMNI_HTTP_REQUEST_HEAD_COMPLETE
                           : OMNI_HTTP_REQUEST_HEAD_TOO_LARGE),
          "total limit -1/exact/+1");
    if (r.status == OMNI_HTTP_REQUEST_HEAD_COMPLETE)
      check(r.consumed_bytes == total, "exact total consumed");
    else {
      failure_output(r);
      check(r.error_offset == OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES, "total error offset");
    }
    /* Missing final LF at the hard limit can never become a valid head. */
    if (total == OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES + 1u) {
      expect(data, total - 1u, 4u, OMNI_HTTP_REQUEST_HEAD_TOO_LARGE, total - 1u);
      expect(data, total - 2u, 4u, OMNI_HTTP_REQUEST_HEAD_INCOMPLETE, total - 2u);
    }
  }
  /* Limit reached inside a viable child line, with no blank terminator. */
  memcpy(data, "GET / HTTP/1.1\r\n", 16u);
  for (size_t i = 16u; i < sizeof(data); ++i)
    data[i] = ' ';
  /* Three max-length fields followed by a partial fourth field. */
  for (size_t i = 0u; i < 3u; ++i) {
    size_t off = 16u + i * 4096u;
    data[off] = 'X';
    data[off + 1u] = ':';
    data[off + 4094u] = '\r';
    data[off + 4095u] = '\n';
  }
  data[12304u] = 'X';
  data[12305u] = ':';
  expect(data, OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES - 1u, 4u, OMNI_HTTP_REQUEST_HEAD_INCOMPLETE,
         OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES - 1u);
  expect(data, sizeof(data), 4u, OMNI_HTTP_REQUEST_HEAD_TOO_LARGE,
         OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES);
}

static void prefixes(const unsigned char *data, size_t length) {
  struct omni_http_header h[8];
  for (size_t n = 0u; n <= length; ++n) {
    ++prefix_cases;
    unsigned char *exact = n == 0u ? NULL : malloc(n);
    struct omni_http_request_head_result r;
    if (n != 0u && exact == NULL) {
      check(false, "test allocation");
      return;
    }
    if (n != 0u)
      memcpy(exact, data, n);
    r = omni_http_request_head_parse(exact, n, h, 8u);
    check(r.status ==
              (n == length ? OMNI_HTTP_REQUEST_HEAD_COMPLETE : OMNI_HTTP_REQUEST_HEAD_INCOMPLETE),
          "every viable exact-allocation prefix");
    check(r.error_offset == (n == length ? 0u : n), "prefix offset");
    if (n < length)
      failure_output(r);
    if (n != 0u)
      check(memcmp(exact, data, n) == 0, "prefix immutable");
    free(exact);
  }
}

static void trailing(void) {
  unsigned char data[] = "POST /x HTTP/1.1\r\nHost: a\r\n\r\nBODY\0\xff";
  unsigned char pipeline[] = "GET /1 HTTP/1.1\r\n\r\nGET /2 HTTP/1.1\r\n\r\n";
  unsigned char snapshot[sizeof(data)];
  struct omni_http_header h[1];
  struct omni_http_request_head_result r;
  memcpy(snapshot, data, sizeof(data));
  r = omni_http_request_head_parse(data, sizeof(data), h, 1u);
  check(r.status == OMNI_HTTP_REQUEST_HEAD_COMPLETE && r.consumed_bytes == 29u,
        "binary body ignored");
  check(memcmp(snapshot, data, sizeof(data)) == 0, "whole input/body immutable");
  r = omni_http_request_head_parse(pipeline, sizeof(pipeline) - 1u, NULL, 0u);
  check(r.status == OMNI_HTTP_REQUEST_HEAD_COMPLETE && r.consumed_bytes == 19u,
        "pipeline stops at first head");
}

static void robustness(void) {
  const unsigned char original[] = "GET / HTTP/1.1\r\nX: \ta b\t\r\n\r\n";
  unsigned char data[sizeof(original)];
  unsigned char snapshot[sizeof(original)];
  /* Every position includes composition CRLF, colon, OWS, value, and terminator. */
  for (size_t pos = 0u; pos < sizeof(original) - 1u; ++pos) {
    for (unsigned int byte = 0u; byte < 256u; ++byte) {
      ++mutation_cases;
      struct {
        struct omni_http_header h[1];
        uint64_t canary;
      } storage;
      struct omni_http_request_head_result r;
      memcpy(data, original, sizeof(data));
      data[pos] = (unsigned char)byte;
      memcpy(snapshot, data, sizeof(data));
      storage.canary = UINT64_C(0x123456789abcdef0);
      r = omni_http_request_head_parse(data, sizeof(data) - 1u, storage.h, 1u);
      check(storage.canary == UINT64_C(0x123456789abcdef0), "mutation storage canary");
      check(memcmp(data, snapshot, sizeof(data)) == 0, "mutation immutable");
      check(r.error_offset <= sizeof(data) - 1u, "mutation bounded error");
      if (r.status != OMNI_HTTP_REQUEST_HEAD_COMPLETE)
        failure_output(r);
      else {
        check(r.consumed_bytes <= sizeof(data) - 1u && r.header_count <= 1u,
              "mutation bounded success");
        check(r.request_line.method.data == data && r.request_line.target.data >= data &&
                  r.request_line.target.data + r.request_line.target.length <=
                      data + r.consumed_bytes,
              "mutation request spans bounded");
        if (r.header_count != 0u)
          check(storage.h[0].name.data >= data &&
                    storage.h[0].value.data + storage.h[0].value.length <= data + r.consumed_bytes,
                "mutation field spans bounded");
      }
    }
  }
}

int main(void) {
  const unsigned char zero[] = "GET / HTTP/1.1\r\n\r\n";
  const unsigned char one[] = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
  const unsigned char multi[] = "GET /v1/models HTTP/1.1\r\nHost: localhost\r\nAccept: "
                                "application/json\r\nX: \ta b \t\r\n\r\n";
  examples();
  errors();
  capacity();
  total_limits();
  trailing();
  prefixes(zero, sizeof(zero) - 1u);
  prefixes(one, sizeof(one) - 1u);
  prefixes(multi, sizeof(multi) - 1u);
  robustness();
  printf("http-request-head: %zu checks, %zu failures\n", checks, failures);
  printf("coverage: %zu exact-allocation prefixes, %zu octet mutations\n", prefix_cases,
         mutation_cases);
  printf("request span=%zu bytes\n", sizeof(struct omni_http_byte_span));
  printf("memory: span=%zu header=%zu result=%zu persistent=0 bytes\n",
         sizeof(struct omni_http_header_line_span), sizeof(struct omni_http_header),
         sizeof(struct omni_http_request_head_result));
  return failures == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
