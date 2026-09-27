/* Task 038: bounded incremental request assembly over a real bytebuf. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "omniroute/http_request.h"

#define STORAGE_CAPACITY 32768u
#define HEADER_CAPACITY 8u

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

static bool load_fixture(struct fixture *fixture, const unsigned char *prefix, size_t prefix_length,
                         const unsigned char *data, size_t length) {
  (void)memset(fixture->storage, 0xa5, sizeof(fixture->storage));
  (void)memset(fixture->headers, 0xcd, sizeof(fixture->headers));
  if (!omni_bytebuf_init_borrowed(&fixture->buffer, fixture->storage,
                                  sizeof(fixture->storage)))
    return false;
  if (!omni_bytebuf_append(&fixture->buffer, prefix, prefix_length) ||
      !omni_bytebuf_append(&fixture->buffer, data, length) ||
      !omni_bytebuf_consume(&fixture->buffer, prefix_length)) {
    omni_bytebuf_destroy(&fixture->buffer);
    return false;
  }
  return true;
}

static bool load_raw(struct fixture *fixture, const unsigned char *data, size_t length) {
  return load_fixture(fixture, NULL, 0u, data, length);
}

static struct fixture_snapshot snapshot_fixture(const struct fixture *fixture) {
  struct fixture_snapshot snapshot;

  snapshot.buffer = fixture->buffer;
  (void)memcpy(snapshot.storage, fixture->storage, sizeof(snapshot.storage));
  return snapshot;
}

static void check_unchanged(const struct fixture *fixture,
                            const struct fixture_snapshot *snapshot, const char *label) {
  check(memcmp(&fixture->buffer, &snapshot->buffer, sizeof(fixture->buffer)) == 0, label);
  check(memcmp(fixture->storage, snapshot->storage, sizeof(fixture->storage)) == 0,
        "bytebuf backing remains unchanged");
}

static struct omni_http_request_result assemble(struct fixture *fixture, size_t header_capacity) {
  return omni_http_request_assemble(&fixture->buffer, fixture->headers, header_capacity);
}

static size_t raw_length(const unsigned char *raw) {
  return strlen((const char *)raw);
}

static void test_arguments_and_empty(void) {
  static const unsigned char zero_header[] = "GET / HTTP/1.1\r\n\r\n";
  struct fixture fixture;
  struct omni_http_request_result result;
  struct fixture_snapshot snapshot;

  (void)memset(&fixture, 0, sizeof(fixture));
  check(!omni_bytebuf_init_borrowed(&fixture.buffer, fixture.storage, sizeof(fixture.storage))
            ? false
            : true,
        "empty fixture initializes");
  (void)memset(fixture.headers, 0xcd, sizeof(fixture.headers));
  snapshot = snapshot_fixture(&fixture);
  result = omni_http_request_assemble(NULL, fixture.headers, HEADER_CAPACITY);
  check(result.status == OMNI_HTTP_REQUEST_ERR_INVALID_ARGUMENT,
        "NULL bytebuf is invalid argument");
  check(result.consumed_bytes == 0u, "NULL bytebuf consumes zero");
  result = omni_http_request_assemble(&fixture.buffer, NULL, 1u);
  check(result.status == OMNI_HTTP_REQUEST_ERR_INVALID_ARGUMENT,
        "NULL headers with positive capacity is invalid argument");
  check(result.consumed_bytes == 0u, "invalid header storage consumes zero");
  check_unchanged(&fixture, &snapshot, "empty bytebuf unchanged after argument errors");

  result = assemble(&fixture, 0u);
  check(result.status == OMNI_HTTP_REQUEST_INCOMPLETE, "empty readable buffer is incomplete");
  check(result.required_total_bytes == 0u, "empty head required total is unknown");
  check(result.request_head.header_count == 0u, "incomplete result has no logical headers");
  check(result.consumed_bytes == 0u, "empty buffer consumes zero");
  check_unchanged(&fixture, &snapshot, "empty bytebuf remains unchanged");
  omni_bytebuf_destroy(&fixture.buffer);

  check(load_raw(&fixture, zero_header, sizeof(zero_header) - 1u),
        "zero-header fixture loads");
  result = omni_http_request_assemble(&fixture.buffer, NULL, 0u);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE,
        "NULL header storage with zero capacity is valid for zero headers");
  check(result.request_head.header_count == 0u && result.consumed_bytes == sizeof(zero_header) - 1u,
        "zero-header request exposes exact boundary");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_request_line_prefixes(void) {
  static const unsigned char raw[] = "GET / HTTP/1.1\r\n\r\n";
  struct fixture fixture;

  for (size_t length = 0u; length < sizeof(raw) - 1u; ++length) {
    struct fixture_snapshot snapshot;
    struct omni_http_request_result result;
    check(load_raw(&fixture, raw, length), "request-line prefix fixture loads");
    snapshot = snapshot_fixture(&fixture);
    result = assemble(&fixture, HEADER_CAPACITY);
    check(result.status == OMNI_HTTP_REQUEST_INCOMPLETE, "request-line prefix is incomplete");
    check(result.consumed_bytes == 0u, "request-line prefix consumes zero");
    check(result.required_total_bytes == 0u, "partial head has unknown total");
    check_unchanged(&fixture, &snapshot, "request-line prefix leaves bytebuf unchanged");
    omni_bytebuf_destroy(&fixture.buffer);
  }
}

static void test_complete_views(void) {
  static const unsigned char raw[] = "GET /v1/models HTTP/1.1\r\n"
                                     "Host: localhost\r\n"
                                     "Accept: application/json\r\n"
                                     "\r\n"
                                     "NEXT";
  const size_t length = sizeof(raw) - 1u;
  struct fixture fixture;
  struct fixture_snapshot snapshot;
  struct omni_http_request_result result;
  const unsigned char *readable;
  size_t readable_length = 0u;

  check(load_raw(&fixture, raw, length), "complete header fixture loads");
  snapshot = snapshot_fixture(&fixture);
  readable = omni_bytebuf_read_ptr(&fixture.buffer, &readable_length);
  result = assemble(&fixture, HEADER_CAPACITY);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "complete no-body request assembles");
  check(result.request_head.request_line.method.data == readable + 0u,
        "method span points into readable backing");
  check(result.request_head.request_line.method.length == 3u &&
            memcmp(result.request_head.request_line.method.data, "GET", 3u) == 0,
        "method span is exact");
  check(result.request_head.request_line.target.data == readable + 4u,
        "target span points into readable backing");
  check(result.request_head.request_line.target.length == 10u &&
            memcmp(result.request_head.request_line.target.data, "/v1/models", 10u) == 0,
        "target span is exact");
  check(result.request_head.header_count == 2u, "parsed header count is exact");
  check(result.request_head.headers == fixture.headers, "header view uses caller storage");
  check(result.request_head.headers[0].name.data == readable + 25u &&
            result.request_head.headers[0].name.length == 4u,
        "first header name is zero-copy");
  check(result.request_head.headers[0].value.data == readable + 31u &&
            result.request_head.headers[0].value.length == 9u,
        "first header value is exact");
  check(result.request_head.headers[1].name.data == readable + 42u &&
            result.request_head.headers[1].value.data == readable + 50u,
        "second header spans are zero-copy");
  check(result.framing.framing == OMNI_HTTP_REQUEST_BODY_NONE,
        "no-body framing is exposed on success");
  check(result.body.body.data == readable + result.consumed_bytes,
        "empty body span points at request boundary");
  check(result.body.body.length == 0u, "empty body span has zero length");
  check(result.consumed_bytes == 70u && result.required_total_bytes == 70u,
        "no-body total and consumed boundary are exact");
  check(result.consumed_bytes < readable_length, "trailing bytes are outside first request");
  check(memcmp(readable + result.consumed_bytes, "NEXT", 4u) == 0,
        "trailing bytes remain readable");
  check_unchanged(&fixture, &snapshot, "successful no-body assembly does not mutate bytebuf");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_incomplete_head_and_errors(void) {
  static const unsigned char incomplete[] = "GET / HTTP/1.1\r\nHost: a\r\n";
  static const unsigned char invalid[] = "GET / HTTP/1.1\r\nBad Header\r\n\r\n";
  static const unsigned char unsupported[] = "GET / HTTP/2.0\r\n\r\n";
  struct fixture fixture;
  struct fixture_snapshot snapshot;
  struct omni_http_request_result result;

  check(load_raw(&fixture, incomplete, sizeof(incomplete) - 1u),
        "incomplete header fixture loads");
  snapshot = snapshot_fixture(&fixture);
  result = assemble(&fixture, HEADER_CAPACITY);
  check(result.status == OMNI_HTTP_REQUEST_INCOMPLETE, "incomplete header section is incomplete");
  check(result.required_total_bytes == 0u && result.consumed_bytes == 0u,
        "incomplete header has unknown total and consumes zero");
  check(result.request_head.header_count == 0u, "incomplete head has no logical headers");
  check_unchanged(&fixture, &snapshot, "incomplete header leaves bytebuf unchanged");
  omni_bytebuf_destroy(&fixture.buffer);

  check(load_raw(&fixture, invalid, sizeof(invalid) - 1u), "invalid header fixture loads");
  snapshot = snapshot_fixture(&fixture);
  result = assemble(&fixture, HEADER_CAPACITY);
  check(result.status == OMNI_HTTP_REQUEST_INVALID_REQUEST_HEAD,
        "invalid header propagates as invalid request head");
  check(result.consumed_bytes == 0u && result.request_head.header_count == 0u,
        "invalid header exposes no consumable view");
  check_unchanged(&fixture, &snapshot, "invalid header leaves bytebuf unchanged");
  omni_bytebuf_destroy(&fixture.buffer);

  check(load_raw(&fixture, unsupported, sizeof(unsupported) - 1u),
        "unsupported version fixture loads");
  snapshot = snapshot_fixture(&fixture);
  result = assemble(&fixture, HEADER_CAPACITY);
  check(result.status == OMNI_HTTP_REQUEST_UNSUPPORTED_VERSION,
        "unsupported version propagates distinctly");
  check(result.consumed_bytes == 0u, "unsupported version consumes zero");
  check_unchanged(&fixture, &snapshot, "unsupported version leaves bytebuf unchanged");
  omni_bytebuf_destroy(&fixture.buffer);
}

static size_t make_fixed_request(unsigned char *raw, size_t capacity, size_t body_length,
                                 const unsigned char *body) {
  static const unsigned char prefix[] = "POST / HTTP/1.1\r\nContent-Length: ";
  static const unsigned char suffix[] = "\r\n\r\n";
  char length_text[32];
  int written = snprintf(length_text, sizeof(length_text), "%zu", body_length);
  size_t used = sizeof(prefix) - 1u;
  size_t length_text_size;

  if (written < 0)
    return 0u;
  length_text_size = (size_t)written;
  if (used > capacity || length_text_size > capacity - used)
    return 0u;
  (void)memcpy(raw, prefix, used);
  (void)memcpy(raw + used, length_text, length_text_size);
  used += length_text_size;
  if (sizeof(suffix) - 1u > capacity - used)
    return 0u;
  (void)memcpy(raw + used, suffix, sizeof(suffix) - 1u);
  used += sizeof(suffix) - 1u;
  if (body_length > capacity - used)
    return 0u;
  if (body_length != 0u)
    (void)memcpy(raw + used, body, body_length);
  return used + body_length;
}

static void test_fixed_bodies(void) {
  unsigned char raw[512];
  unsigned char body[256];

  for (size_t i = 0u; i < sizeof(body); ++i)
    body[i] = (unsigned char)i;

  for (size_t body_length = 0u; body_length <= 5u; ++body_length) {
    size_t length = make_fixed_request(raw, sizeof(raw), body_length, body);
    struct fixture full;
    struct omni_http_request_result full_result;
    check(length != 0u, "fixed request fixture builds");
    check(load_raw(&full, raw, length), "fixed request fixture loads");
    full_result = assemble(&full, HEADER_CAPACITY);
    check(full_result.status == OMNI_HTTP_REQUEST_COMPLETE, "fixed body exact bytes complete");
    check(full_result.framing.framing == OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH &&
              full_result.framing.content_length == body_length,
          "fixed body framing is exposed");
    check(full_result.body.body.data == full.storage + full.buffer.read +
                                         full_result.request_head.consumed_bytes,
          "fixed body points into readable backing");
    check(full_result.body.body.length == body_length &&
              memcmp(full_result.body.body.data, body, body_length) == 0,
          "fixed body bytes are exact");
    check(full_result.consumed_bytes == full_result.request_head.consumed_bytes + body_length &&
              full_result.required_total_bytes == full_result.consumed_bytes,
          "fixed body consumed boundary is exact");
    omni_bytebuf_destroy(&full.buffer);

    for (size_t available_body = 0u; available_body < body_length; ++available_body) {
      const size_t head_length = length - body_length;
      struct fixture partial;
      struct fixture_snapshot snapshot;
      struct omni_http_request_result result;
      check(load_raw(&partial, raw, head_length + available_body),
            "fixed body prefix fixture loads");
      snapshot = snapshot_fixture(&partial);
      result = assemble(&partial, HEADER_CAPACITY);
      check(result.status == OMNI_HTTP_REQUEST_INCOMPLETE,
            "fixed body prefix is incomplete");
      check(result.required_total_bytes == length && result.consumed_bytes == 0u,
            "fixed body prefix reports exact total and consumes zero");
      check(result.request_head.header_count == 0u, "partial body exposes no logical head");
      check_unchanged(&partial, &snapshot, "partial body leaves bytebuf unchanged");
      omni_bytebuf_destroy(&partial.buffer);
    }
  }

  {
    struct fixture fixture;
    struct fixture_snapshot snapshot;
    struct omni_http_request_result result;
    const size_t length = make_fixed_request(raw, sizeof(raw), sizeof(body), body);
    check(load_raw(&fixture, raw, length), "all-byte body fixture loads");
    snapshot = snapshot_fixture(&fixture);
    result = assemble(&fixture, HEADER_CAPACITY);
    check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "all 256 body bytes complete");
    check(result.body.body.length == sizeof(body), "all-byte body length is 256");
    check(memcmp(result.body.body.data, body, sizeof(body)) == 0,
          "all 256 body values remain opaque and exact");
    check_unchanged(&fixture, &snapshot, "binary body assembly leaves bytebuf unchanged");
    omni_bytebuf_destroy(&fixture.buffer);
  }
}

static void test_framing_errors(void) {
  static const unsigned char transfer_encoding[] = "POST / HTTP/1.1\r\n"
                                                    "Transfer-Encoding: chunked\r\n\r\n"
                                                    "5\r\nhello\r\n0\r\n\r\n";
  static const unsigned char conflicting[] = "POST / HTTP/1.1\r\n"
                                             "Content-Length: 1\r\n"
                                             "Content-Length: 2\r\n\r\nAB";
  static const unsigned char ambiguous[] = "POST / HTTP/1.1\r\n"
                                           "Content-Length: 2\r\n"
                                           "Transfer-Encoding: chunked\r\n\r\nAB";
  static const unsigned char malformed[] = "POST / HTTP/1.1\r\n"
                                           "Content-Length: nope\r\n\r\n";
  const unsigned char *raws[] = {transfer_encoding, conflicting, ambiguous, malformed};
  const enum omni_http_request_status statuses[] = {
      OMNI_HTTP_REQUEST_UNSUPPORTED_FRAMING, OMNI_HTTP_REQUEST_INVALID_FRAMING,
      OMNI_HTTP_REQUEST_INVALID_FRAMING, OMNI_HTTP_REQUEST_INVALID_FRAMING};
  struct fixture fixture;

  for (size_t i = 0u; i < sizeof(raws) / sizeof(raws[0]); ++i) {
    const size_t length = raw_length(raws[i]);
    struct fixture_snapshot snapshot;
    struct omni_http_request_result result;
    check(load_raw(&fixture, raws[i], length), "framing error fixture loads");
    snapshot = snapshot_fixture(&fixture);
    result = assemble(&fixture, HEADER_CAPACITY);
    check(result.status == statuses[i], "framing error maps to terminal status");
    check(result.consumed_bytes == 0u && result.request_head.header_count == 0u,
          "framing error exposes no consumable view");
    check_unchanged(&fixture, &snapshot, "framing error leaves bytebuf unchanged");
    omni_bytebuf_destroy(&fixture.buffer);
  }
}

static void test_pipeline_and_offsets(void) {
  static const unsigned char first[] = "GET /1 HTTP/1.1\r\n\r\n";
  static const unsigned char second[] = "GET /2 HTTP/1.1\r\n\r\n";
  static const unsigned char prefix[] = "OLDOLD";
  unsigned char pipeline[sizeof(first) + sizeof(second) + 32u];
  unsigned char fixed[256];
  const size_t first_length = sizeof(first) - 1u;
  const size_t second_length = sizeof(second) - 1u;
  struct fixture fixture;
  struct fixture_snapshot snapshot;
  struct omni_http_request_result result;

  (void)memcpy(pipeline, first, first_length);
  (void)memcpy(pipeline + first_length, second, second_length);
  check(load_fixture(&fixture, prefix, sizeof(prefix) - 1u, pipeline,
                     first_length + second_length),
        "offset pipeline fixture loads");
  snapshot = snapshot_fixture(&fixture);
  result = assemble(&fixture, HEADER_CAPACITY);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "offset no-body pipeline completes");
  check(result.consumed_bytes == first_length,
        "offset consumed bytes are relative to readable start");
  check(result.request_head.request_line.method.data == fixture.storage + sizeof(prefix) - 1u,
        "parser begins at bytebuf read offset");
  check(memcmp(fixture.storage + sizeof(prefix) - 1u + result.consumed_bytes, second,
               second_length) == 0,
        "second pipelined request remains trailing data");
  check_unchanged(&fixture, &snapshot, "offset pipeline leaves metadata and storage unchanged");
  omni_bytebuf_destroy(&fixture.buffer);

  {
    static const unsigned char binary_body[] = {'A', 0x00u, 'B', 0x80u, 0xffu};
    size_t length = make_fixed_request(fixed, sizeof(fixed), sizeof(binary_body), binary_body);
    const size_t second_offset = length;
    check(load_raw(&fixture, fixed, length), "fixed pipeline first request loads");
    check(omni_bytebuf_append(&fixture.buffer, second, second_length),
          "fixed pipeline second request appends");
    snapshot = snapshot_fixture(&fixture);
    result = assemble(&fixture, HEADER_CAPACITY);
    check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "fixed pipeline first request completes");
    check(result.body.body.length == sizeof(binary_body) &&
              memcmp(result.body.body.data, binary_body, sizeof(binary_body)) == 0,
          "fixed pipeline body is exact");
    check(result.consumed_bytes == second_offset, "fixed pipeline stops after first body");
    check(memcmp(fixture.storage + result.consumed_bytes, second, second_length) == 0,
          "fixed pipeline second request remains readable");
    check_unchanged(&fixture, &snapshot, "fixed pipeline assembly does not mutate bytebuf");
    omni_bytebuf_destroy(&fixture.buffer);
  }
}

static void test_header_capacity_and_limits(void) {
  static const unsigned char two_headers[] = "GET / HTTP/1.1\r\nHost: a\r\nAccept: b\r\n\r\n";
  struct fixture fixture;
  struct omni_http_header canary[HEADER_CAPACITY + 1u];
  struct fixture_snapshot snapshot;
  struct omni_http_request_result result;

  check(load_raw(&fixture, two_headers, sizeof(two_headers) - 1u),
        "header capacity fixture loads");
  (void)memset(canary, 0xe7, sizeof(canary));
  snapshot = snapshot_fixture(&fixture);
  result = omni_http_request_assemble(&fixture.buffer, canary, 1u);
  check(result.status == OMNI_HTTP_REQUEST_TOO_MANY_HEADERS,
        "caller header capacity overflow is terminal");
  check(result.consumed_bytes == 0u && result.request_head.header_count == 0u,
        "header capacity overflow consumes zero");
  for (size_t i = 1u; i < sizeof(canary) / sizeof(canary[0]); ++i) {
    for (size_t byte = 0u; byte < sizeof(canary[i]); ++byte)
      check(((const unsigned char *)&canary[i])[byte] == 0xe7u,
            "header capacity canary remains outside caller capacity");
  }
  check_unchanged(&fixture, &snapshot, "header capacity failure leaves bytebuf unchanged");
  omni_bytebuf_destroy(&fixture.buffer);

  {
    unsigned char raw[OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES + 1u];
    size_t used = 0u;
    const size_t totals[] = {OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES,
                             OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES + 1u};
    for (size_t case_index = 0u; case_index < sizeof(totals) / sizeof(totals[0]); ++case_index) {
      const size_t total = totals[case_index];
      size_t field_budget = total - 18u;
      used = 0u;
      (void)memcpy(raw + used, "GET / HTTP/1.1\r\n", 16u);
      used += 16u;
      for (size_t field = 0u; field < 5u; ++field) {
        size_t line_length = field_budget / (5u - field);
        if (field == 4u)
          line_length = field_budget;
        (void)memset(raw + used, 'X', line_length);
        raw[used + 1u] = ':';
        raw[used + line_length - 2u] = '\r';
        raw[used + line_length - 1u] = '\n';
        used += line_length;
        field_budget -= line_length;
      }
      raw[used++] = '\r';
      raw[used++] = '\n';
      check(used == total, "head-limit fixture reaches requested total");
      check(load_raw(&fixture, raw, total), "head-limit fixture loads");
      snapshot = snapshot_fixture(&fixture);
      result = assemble(&fixture, 5u);
      check(result.status == (case_index == 0u
                                  ? OMNI_HTTP_REQUEST_COMPLETE
                                  : OMNI_HTTP_REQUEST_REQUEST_HEAD_TOO_LARGE),
            "Task 035 exact and over head limit propagate through assembler");
      if (case_index == 0u)
        check(result.consumed_bytes == total && result.required_total_bytes == total,
              "exact head limit is a complete no-body request");
      else
        check(result.consumed_bytes == 0u, "over-limit head consumes zero");
      check_unchanged(&fixture, &snapshot, "head-limit assembly leaves bytebuf unchanged");
      omni_bytebuf_destroy(&fixture.buffer);
    }
  }
}

static void test_tail_capacity_and_prefix_robustness(void) {
  static const unsigned char raw[] = "GET / HTTP/1.1\r\n\r\n";
  static const unsigned char malformed_tail[] = "GET / HTTP/1.1\r\n";
  struct fixture fixture;

  check(load_raw(&fixture, malformed_tail, sizeof(malformed_tail) - 1u),
        "tail-capacity fixture loads");
  (void)memset(fixture.storage + fixture.buffer.write, 0x00,
               fixture.buffer.capacity - fixture.buffer.write);
  fixture.storage[fixture.buffer.capacity - 1u] = '\n';
  {
    struct fixture_snapshot snapshot = snapshot_fixture(&fixture);
    struct omni_http_request_result result = assemble(&fixture, HEADER_CAPACITY);
    check(result.status == OMNI_HTTP_REQUEST_INCOMPLETE,
          "tail garbage cannot complete request beyond write offset");
    check(result.consumed_bytes == 0u && result.required_total_bytes == 0u,
          "tail garbage reports incomplete without consuming");
    check_unchanged(&fixture, &snapshot, "tail garbage is not inspected or mutated");
  }
  omni_bytebuf_destroy(&fixture.buffer);

  for (size_t length = 0u; length <= sizeof(raw) - 1u; ++length) {
    check(load_raw(&fixture, raw, length), "deterministic prefix fixture loads");
    {
      struct fixture_snapshot snapshot = snapshot_fixture(&fixture);
      struct omni_http_request_result result = assemble(&fixture, HEADER_CAPACITY);
      if (length < sizeof(raw) - 1u) {
        check(result.status == OMNI_HTTP_REQUEST_INCOMPLETE,
              "every valid request prefix remains incomplete");
        check(result.consumed_bytes == 0u, "every valid request prefix consumes zero");
      } else {
        check(result.status == OMNI_HTTP_REQUEST_COMPLETE,
              "complete prefix becomes a request");
      }
      check_unchanged(&fixture, &snapshot, "prefix robustness leaves bytebuf unchanged");
    }
    omni_bytebuf_destroy(&fixture.buffer);
  }
}

static void test_header_mutations(void) {
  static const unsigned char original[] = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
  unsigned char mutated[sizeof(original) - 1u];
  struct fixture fixture;

  for (size_t i = 0u; i < sizeof(mutated); ++i) {
    (void)memcpy(mutated, original, sizeof(mutated));
    mutated[i] ^= 0x01u;
    check(load_raw(&fixture, mutated, sizeof(mutated)), "mutated head fixture loads");
    {
      struct fixture_snapshot snapshot = snapshot_fixture(&fixture);
      struct omni_http_request_result result = assemble(&fixture, HEADER_CAPACITY);
      check(result.consumed_bytes == 0u || result.consumed_bytes <= sizeof(mutated),
            "mutated head has bounded consumed result");
      if (result.status != OMNI_HTTP_REQUEST_COMPLETE)
        check(result.consumed_bytes == 0u, "mutated non-success consumes zero");
      check_unchanged(&fixture, &snapshot, "mutated head leaves bytebuf unchanged");
    }
    omni_bytebuf_destroy(&fixture.buffer);
  }
}

/*
 * Task 039 adds two metadata fields (source_read_ptr / source_readable_length)
 * to the COMPLETE result so a later consumption step can prove the borrowed
 * view still describes the CURRENT readable region. These checks pin that
 * contract here, in the assembler that produces it: snapshot identity at a
 * nonzero read offset, COMPLETE-only population, no usable snapshot on
 * INCOMPLETE or any terminal error, and stability of the readable start
 * across a safe tail append.
 */
static void test_source_snapshot(void) {
  static const unsigned char first[] = "GET /1 HTTP/1.1\r\n\r\n";
  static const unsigned char second[] = "POST /2 HTTP/1.1\r\nContent-Length: 3\r\n\r\nABC";
  static const unsigned char partial[] = "GET /1 HTTP/1.1\r\nHost: a\r\n";
  static const unsigned char invalid[] = "GET /1 HTTP/1.1\r\nBad Header\r\n\r\n";
  static const unsigned char unsupported[] = "GET /1 HTTP/2.0\r\n\r\n";
  static const unsigned char ambiguous[] = "POST /1 HTTP/1.1\r\n"
                                          "Content-Length: 2\r\n"
                                          "Transfer-Encoding: chunked\r\n\r\nAB";
  static const unsigned char prefix[] = "OLDOLD";
  unsigned char pipeline[sizeof(first) + sizeof(second)];
  const unsigned char *readable;
  size_t readable_length = 0u;
  struct fixture fixture;
  struct fixture_snapshot snapshot;
  struct omni_http_request_result result;

  /* Snapshot identity, including from a nonzero read offset. */
  (void)memcpy(pipeline, first, sizeof(first) - 1u);
  (void)memcpy(pipeline + sizeof(first) - 1u, second, sizeof(second) - 1u);
  check(load_fixture(&fixture, prefix, sizeof(prefix) - 1u, pipeline,
                     sizeof(first) + sizeof(second) - 1u),
        "snapshot offset fixture loads");
  snapshot = snapshot_fixture(&fixture);
  readable = omni_bytebuf_read_ptr(&fixture.buffer, &readable_length);
  result = assemble(&fixture, HEADER_CAPACITY);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "snapshot fixture completes");
  check(result.source_read_ptr == readable, "COMPLETE snapshot is the current readable start");
  check(result.source_read_ptr == fixture.storage + sizeof(prefix) - 1u,
        "COMPLETE snapshot is relative to the read offset, not backing offset zero");
  check(result.source_readable_length == readable_length,
        "COMPLETE snapshot length is the current readable length");
  check(result.source_readable_length == sizeof(first) + sizeof(second) - 1u,
        "COMPLETE snapshot length covers both pipelined requests");
  check_unchanged(&fixture, &snapshot, "snapshot fields do not mutate the bytebuf");
  omni_bytebuf_destroy(&fixture.buffer);

  /* A safe tail append does not move the readable start. */
  check(load_raw(&fixture, first, sizeof(first) - 1u), "snapshot append fixture loads");
  readable = omni_bytebuf_read_ptr(&fixture.buffer, &readable_length);
  result = assemble(&fixture, HEADER_CAPACITY);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "append fixture completes");
  check(omni_bytebuf_append(&fixture.buffer, second, sizeof(second) - 1u),
        "append fixture tail append succeeds");
  check(omni_bytebuf_read_ptr(&fixture.buffer, NULL) == result.source_read_ptr,
        "tail append leaves the assembly source start in place");
  check(omni_bytebuf_readable(&fixture.buffer) > result.source_readable_length,
        "tail append grows the readable region past the snapshot length");
  check(result.consumed_bytes == sizeof(first) - 1u,
        "appending after assembly does not change the recorded boundary");
  omni_bytebuf_destroy(&fixture.buffer);

  /* INCOMPLETE and terminal results expose no usable snapshot. */
  {
    const unsigned char *inputs[4] = {partial, invalid, unsupported, ambiguous};
    const enum omni_http_request_status statuses[4] = {OMNI_HTTP_REQUEST_INCOMPLETE,
                                                      OMNI_HTTP_REQUEST_INVALID_REQUEST_HEAD,
                                                      OMNI_HTTP_REQUEST_UNSUPPORTED_VERSION,
                                                      OMNI_HTTP_REQUEST_INVALID_FRAMING};
    for (size_t i = 0u; i < 4u; ++i) {
      check(load_raw(&fixture, inputs[i], raw_length(inputs[i])), "snapshot error fixture loads");
      result = assemble(&fixture, HEADER_CAPACITY);
      check(result.status == statuses[i], "snapshot error fixture maps to its Task 038 status");
      check(result.consumed_bytes == 0u, "non-COMPLETE result still consumes zero");
      check(result.source_read_ptr == NULL && result.source_readable_length == 0u,
            "non-COMPLETE result exposes no usable source snapshot");
      omni_bytebuf_destroy(&fixture.buffer);
    }
  }

  /* A zero-capacity error path also exposes no snapshot. */
  check(load_raw(&fixture, first, sizeof(first) - 1u), "snapshot argument fixture loads");
  result = omni_http_request_assemble(NULL, fixture.headers, HEADER_CAPACITY);
  check(result.status == OMNI_HTTP_REQUEST_ERR_INVALID_ARGUMENT,
        "NULL buffer still reports invalid argument");
  check(result.source_read_ptr == NULL && result.source_readable_length == 0u,
        "NULL buffer exposes no usable source snapshot");
  result = omni_http_request_assemble(&fixture.buffer, NULL, 1u);
  check(result.status == OMNI_HTTP_REQUEST_ERR_INVALID_ARGUMENT,
        "NULL header storage still reports invalid argument");
  check(result.source_read_ptr == NULL && result.source_readable_length == 0u,
        "invalid header storage exposes no usable source snapshot");
  omni_bytebuf_destroy(&fixture.buffer);
}

int main(void) {
  test_arguments_and_empty();
  test_request_line_prefixes();
  test_complete_views();
  test_incomplete_head_and_errors();
  test_fixed_bodies();
  test_framing_errors();
  test_pipeline_and_offsets();
  test_header_capacity_and_limits();
  test_tail_capacity_and_prefix_robustness();
  test_header_mutations();
  test_source_snapshot();
  (void)printf("http-request-unit: %zu checks, %zu failures\n", checks, failures);
  return failures == 0u ? 0 : 1;
}
