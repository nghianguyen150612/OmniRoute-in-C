/*
 * Task 039: bounded consumption of exactly one assembled HTTP request.
 *
 * Every case drives the real Task 038 assembler over a real bytebuf and the
 * real bytebuf consume primitive: no synthetic-only paths, no networking, no
 * heap, no file descriptors. Assertions compare the borrowed request view
 * before consuming and the resulting readable region afterwards, so the suite
 * proves the assemble -> inspect -> consume -> next-region lifecycle rather
 * than just the returned status.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "omniroute/http_request.h"
#include "omniroute/http_request_consume.h"

#define STORAGE_CAPACITY 32768u
#define HEADER_CAPACITY 8u

static const unsigned char REQ_A[] = "GET /a HTTP/1.1\r\n\r\n";
/* Body is exactly five opaque bytes: 'a', 'b', 0x00, 'c', 0xff. The NUL uses an
   octal escape so the following 'c' is not swallowed as a hex digit, and the
   declared Content-Length matches the byte count exactly. */
static const unsigned char REQ_B_BODY[] = {'a', 'b', 0x00u, 'c', 0xffu};
static const unsigned char REQ_B[] = "POST /b HTTP/1.1\r\nContent-Length: 5\r\n\r\n"
                                     "ab\0c\xff";
static const unsigned char REQ_C[] = "GET /c HTTP/1.1\r\nHost: example.test\r\n\r\n";

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

static size_t raw_length(const unsigned char *raw) {
  return strlen((const char *)raw);
}

static bool load_fixture(struct fixture *fixture, const unsigned char *prefix, size_t prefix_length,
                         const unsigned char *data, size_t length) {
  (void)memset(fixture->storage, 0xa5, sizeof(fixture->storage));
  (void)memset(fixture->headers, 0xcd, sizeof(fixture->headers));
  if (!omni_bytebuf_init_borrowed(&fixture->buffer, fixture->storage, sizeof(fixture->storage)))
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

/*
 * Structural equality of the bytebuf accounting plus a byte-for-byte backing
 * comparison: used where a rejected call must leave NOTHING changed.
 */
static void check_unchanged(const struct fixture *fixture,
                            const struct fixture_snapshot *snapshot, const char *label) {
  check(memcmp(&fixture->buffer, &snapshot->buffer, sizeof(fixture->buffer)) == 0, label);
  check(memcmp(fixture->storage, snapshot->storage, sizeof(fixture->storage)) == 0,
        "bytebuf backing remains byte-identical");
}

static struct omni_http_request_result assemble(struct fixture *fixture) {
  return omni_http_request_assemble(&fixture->buffer, fixture->headers, HEADER_CAPACITY);
}

/* Build "POST / HTTP/1.1\r\nContent-Length: N\r\n\r\n" + body. */
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

/* ---------------------------------------------------------------- arguments */

static void test_arguments_and_empty(void) {
  static const unsigned char request[] = "GET / HTTP/1.1\r\n\r\n";
  struct fixture fixture;
  struct omni_http_request_result result;
  struct omni_http_request_consume_result consumed;
  struct fixture_snapshot snapshot;

  (void)memset(&fixture, 0, sizeof(fixture));
  check(!omni_bytebuf_init_borrowed(&fixture.buffer, fixture.storage, sizeof(fixture.storage))
            ? false
            : true,
        "empty fixture initializes");

  /* NULL bytebuf / NULL result, on an empty and on a populated buffer. */
  check(load_raw(&fixture, request, sizeof(request) - 1u), "argument fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "argument fixture completes");
  snapshot = snapshot_fixture(&fixture);
  consumed = omni_http_request_consume(NULL, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_INVALID_ARGUMENT,
        "NULL bytebuf is invalid argument");
  check(consumed.consumed_bytes == 0u && consumed.remaining_readable_bytes == 0u,
        "NULL bytebuf reports no consumption and no remaining bytes");
  consumed = omni_http_request_consume(&fixture.buffer, NULL);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_INVALID_ARGUMENT,
        "NULL request is invalid argument");
  check(consumed.consumed_bytes == 0u, "NULL request consumes zero");
  check_unchanged(&fixture, &snapshot, "argument errors leave the bytebuf untouched");

  /* A destroyed buffer is not live: capacity reports zero. */
  omni_bytebuf_destroy(&fixture.buffer);
  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_INVALID_ARGUMENT,
        "destroyed bytebuf is invalid argument");
  check(consumed.consumed_bytes == 0u, "destroyed bytebuf consumes zero");

  /* A never-initialized (all-zero) buffer is inert, not consumable. */
  (void)memset(&fixture, 0, sizeof(fixture));
  (void)memset(fixture.headers, 0xcd, sizeof(fixture.headers));
  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_INVALID_ARGUMENT,
        "inert bytebuf is invalid argument");

  /* Empty live buffer: a real INCOMPLETE result, then a fake COMPLETE one. */
  check(!omni_bytebuf_init_borrowed(&fixture.buffer, fixture.storage, sizeof(fixture.storage))
            ? false
            : true,
        "empty live buffer re-initializes");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_INCOMPLETE, "empty buffer assembles INCOMPLETE");
  snapshot = snapshot_fixture(&fixture);
  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_NOT_COMPLETE,
        "empty buffer with INCOMPLETE result is not consumable");
  check(consumed.consumed_bytes == 0u, "empty buffer consumes zero");
  check_unchanged(&fixture, &snapshot, "empty buffer stays empty after rejection");

  {
    struct omni_http_request_result fake = {0};
    fake.status = OMNI_HTTP_REQUEST_COMPLETE;
    fake.consumed_bytes = 8u;
    fake.source_read_ptr = fixture.storage;
    fake.source_readable_length = 8u;
    consumed = omni_http_request_consume(&fixture.buffer, &fake);
    check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_STALE,
          "fake COMPLETE result on an empty buffer is stale");
    check(consumed.consumed_bytes == 0u, "fake COMPLETE result on empty buffer consumes zero");
    check(omni_bytebuf_readable(&fixture.buffer) == 0u,
          "empty buffer readable count cannot underflow");
    check_unchanged(&fixture, &snapshot, "empty buffer unchanged after fake result");
  }
  omni_bytebuf_destroy(&fixture.buffer);
}

/* ------------------------------------------------------------- no-body path */

static void test_basic_no_body_consume(void) {
  static const unsigned char request[] = "GET /v1/models HTTP/1.1\r\nHost: a\r\nAccept: b\r\n\r\n";
  struct fixture fixture;
  struct omni_http_request_result result;
  struct omni_http_request_consume_result consumed;
  struct fixture_snapshot snapshot;
  const size_t length = sizeof(request) - 1u;

  (void)memset(&fixture, 0, sizeof(fixture));
  check(load_raw(&fixture, request, length), "no-body fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "no-body request completes");
  check(result.consumed_bytes == length, "no-body consumed boundary is the request length");
  check(result.source_read_ptr == omni_bytebuf_read_ptr(&fixture.buffer, NULL),
        "snapshot names the current readable start");
  check(result.source_readable_length == length, "snapshot records the readable length");
  check(result.source_read_generation == omni_bytebuf_read_generation(&fixture.buffer) &&
            result.source_read_generation != 0u,
        "snapshot records the current readable generation");
  check(result.framing.framing == OMNI_HTTP_REQUEST_BODY_NONE, "no-body framing is exposed");
  check(result.body.body.length == 0u, "no-body span is empty");

  snapshot = snapshot_fixture(&fixture);
  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "no-body consume succeeds");
  check(consumed.consumed_bytes == length, "no-body consume reports exact bytes");
  check(consumed.remaining_readable_bytes == 0u, "no-body consume empties the buffer");
  check(omni_bytebuf_readable(&fixture.buffer) == 0u, "buffer is empty after no-body consume");
  /* The bytebuf consume primitive canonicalizes a fully drained buffer; that
     inherited behavior is the only offset change, and no byte is erased. */
  check(fixture.buffer.read == 0u && fixture.buffer.write == 0u,
        "fully drained buffer is canonically empty via bytebuf semantics");
  check(memcmp(fixture.storage, snapshot.storage, sizeof(fixture.storage)) == 0,
        "no-body consume rewrites no backing byte");
  omni_bytebuf_destroy(&fixture.buffer);
}

/* --------------------------------------------------------- fixed body paths */

static void test_fixed_body_consume(void) {
  static const unsigned char body[] = {'A', 'B', 'C'};
  unsigned char raw[512];
  struct fixture fixture;
  struct omni_http_request_result result;
  struct omni_http_request_consume_result consumed;
  struct fixture_snapshot snapshot;
  const size_t length = make_fixed_request(raw, sizeof(raw), sizeof(body), body);

  (void)memset(&fixture, 0, sizeof(fixture));
  check(length != 0u, "fixed-body fixture builds");
  check(load_raw(&fixture, raw, length), "fixed-body fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "fixed-body request completes");
  check(result.framing.framing == OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH &&
            result.framing.content_length == sizeof(body),
        "fixed-body framing is exposed");
  check(result.body.body.data == fixture.storage + result.request_head.consumed_bytes &&
            result.body.body.length == sizeof(body) &&
            memcmp(result.body.body.data, body, sizeof(body)) == 0,
        "borrowed body view is exact before consumption");
  check(result.consumed_bytes == result.request_head.consumed_bytes + sizeof(body),
        "fixed-body boundary is head plus declared body");

  snapshot = snapshot_fixture(&fixture);
  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "fixed-body consume succeeds");
  check(consumed.consumed_bytes == result.consumed_bytes,
        "fixed-body consume reports the exact head+body boundary");
  check(consumed.remaining_readable_bytes == 0u, "fixed-body consume empties the buffer");
  check(memcmp(fixture.storage, snapshot.storage, sizeof(fixture.storage)) == 0,
        "fixed-body consume leaves request bytes in place");
  check(fixture.storage[result.request_head.consumed_bytes] == 'A' &&
            fixture.storage[result.request_head.consumed_bytes + 2u] == 'C',
        "consumed body bytes are not wiped or overwritten");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_binary_body_consume(void) {
  static const unsigned char body[] = {0x00u, 0x01u, 0x7fu, 0x80u, 0xffu};
  unsigned char raw[512];
  unsigned char pipeline[1024];
  struct fixture fixture;
  struct omni_http_request_result result;
  struct omni_http_request_consume_result consumed;
  const size_t length = make_fixed_request(raw, sizeof(raw), sizeof(body), body);
  const size_t tail = sizeof(REQ_C) - 1u;

  (void)memset(&fixture, 0, sizeof(fixture));
  (void)memcpy(pipeline, raw, length);
  (void)memcpy(pipeline + length, REQ_C, tail);
  check(load_raw(&fixture, pipeline, length + tail), "binary-body pipeline loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "binary-body request completes");
  check(result.body.body.length == sizeof(body) &&
            memcmp(result.body.body.data, body, sizeof(body)) == 0,
        "binary body stays opaque, including the embedded NUL and high bytes");
  check(result.consumed_bytes == result.request_head.consumed_bytes + sizeof(body),
        "boundary depends only on the declared length, not on body byte values");

  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "binary-body consume succeeds");
  check(consumed.consumed_bytes == length, "binary-body consume stops exactly at the body end");
  check(consumed.remaining_readable_bytes == tail, "next request remains readable");
  check(memcmp(fixture.storage + fixture.buffer.read, REQ_C, tail) == 0,
        "bytes after the binary body are untouched trailing data");
  omni_bytebuf_destroy(&fixture.buffer);
}

/* ------------------------------------------------- trailing / pipelined data */

static void test_trailing_data_only_first_consumed(void) {
  static const unsigned char trailing[] = "RAW-TRAILING-BYTES";
  unsigned char pipeline[256];
  struct fixture fixture;
  struct omni_http_request_result result;
  struct omni_http_request_consume_result consumed;
  const size_t tail = sizeof(trailing) - 1u;
  const size_t first = sizeof(REQ_A) - 1u;

  (void)memset(&fixture, 0, sizeof(fixture));
  (void)memcpy(pipeline, REQ_A, first);
  (void)memcpy(pipeline + first, trailing, tail);
  check(load_raw(&fixture, pipeline, first + tail), "trailing-data fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "trailing-data first request completes");
  check(result.consumed_bytes == first, "first request boundary excludes trailing bytes");

  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "trailing-data consume succeeds");
  check(consumed.consumed_bytes == first, "only the first request is consumed");
  check(consumed.remaining_readable_bytes == tail, "trailing bytes stay readable");
  check(memcmp(omni_bytebuf_read_ptr(&fixture.buffer, NULL), trailing, tail) == 0,
        "trailing bytes are byte-identical after consumption");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_two_pipelined_requests(void) {
  unsigned char pipeline[256];
  struct fixture fixture;
  struct omni_http_request_result first;
  struct omni_http_request_result second;
  struct omni_http_request_consume_result consumed;
  const size_t first_length = sizeof(REQ_A) - 1u;
  const size_t second_length = sizeof(REQ_B) - 1u;

  (void)memset(&fixture, 0, sizeof(fixture));
  (void)memcpy(pipeline, REQ_A, first_length);
  (void)memcpy(pipeline + first_length, REQ_B, second_length);
  check(load_raw(&fixture, pipeline, first_length + second_length), "two-request pipeline loads");

  first = assemble(&fixture);
  check(first.status == OMNI_HTTP_REQUEST_COMPLETE, "pipelined request A completes");
  check(first.request_head.request_line.target.data == fixture.storage + 4u,
        "request A view starts at the readable region");
  consumed = omni_http_request_consume(&fixture.buffer, &first);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "request A consumes");
  check(consumed.consumed_bytes == first_length, "request A consumed exactly once");
  check(consumed.remaining_readable_bytes == second_length, "request B remains readable");

  second = assemble(&fixture);
  check(second.status == OMNI_HTTP_REQUEST_COMPLETE, "pipelined request B completes");
  /* "POST " is five bytes, so request B's target span sits at first_length + 5. */
  check(second.request_head.request_line.target.data == fixture.storage + first_length + 5u,
        "request B view follows the advanced read offset");
  check(second.request_head.request_line.target.length == 2u &&
            memcmp(second.request_head.request_line.target.data, "/b", 2u) == 0,
        "request B target is exact");
  consumed = omni_http_request_consume(&fixture.buffer, &second);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "request B consumes");
  check(consumed.consumed_bytes == second_length, "request B consumed exactly once");
  check(consumed.remaining_readable_bytes == 0u, "two-request pipeline drains exactly");
  check(omni_bytebuf_readable(&fixture.buffer) == 0u, "nothing is skipped or duplicated");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_three_pipelined_requests(void) {
  unsigned char pipeline[512];
  const unsigned char *parts[3];
  size_t lengths[3];
  struct fixture fixture;
  size_t expected_offset = 0u;
  size_t total;

  (void)memset(&fixture, 0, sizeof(fixture));
  parts[0] = REQ_A;
  parts[1] = REQ_B;
  parts[2] = REQ_C;
  lengths[0] = sizeof(REQ_A) - 1u;
  lengths[1] = sizeof(REQ_B) - 1u;
  lengths[2] = sizeof(REQ_C) - 1u;
  total = lengths[0] + lengths[1] + lengths[2];
  expected_offset = 0u;
  for (size_t i = 0u; i < 3u; ++i) {
    (void)memcpy(pipeline + expected_offset, parts[i], lengths[i]);
    expected_offset += lengths[i];
  }
  check(total == expected_offset, "three-request pipeline length math");
  check(load_raw(&fixture, pipeline, total), "three-request pipeline loads");

  for (size_t i = 0u; i < 3u; ++i) {
    struct omni_http_request_result result = assemble(&fixture);
    struct omni_http_request_consume_result consumed;
    size_t preceding = i == 0u ? 0u : lengths[0] + (i == 1u ? 0u : lengths[1]);

    check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "each pipelined request completes");
    check(result.consumed_bytes == lengths[i], "each request consumes its own length only");
    check(fixture.buffer.read == preceding,
          "read offset progresses to the current request exactly");
    check(memcmp(omni_bytebuf_read_ptr(&fixture.buffer, NULL), parts[i], lengths[i]) == 0,
          "current readable region is the expected request");
    consumed = omni_http_request_consume(&fixture.buffer, &result);
    check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "each pipelined request consumes");
    check(consumed.consumed_bytes == lengths[i], "no skipped or duplicated bytes");
    check(consumed.remaining_readable_bytes == total - (preceding + lengths[i]),
          "remaining readable count is exact after each consume");
    check(i < 2u ? omni_bytebuf_reclaimable(&fixture.buffer) > 0u
                 : omni_bytebuf_readable(&fixture.buffer) == 0u,
          "trailing bytes remain reclaimed-prefix-only, no compaction needed");
  }
  check(omni_bytebuf_readable(&fixture.buffer) == 0u, "three-request pipeline ends empty");
  check(omni_bytebuf_high_water(&fixture.buffer) == total, "high water matches the pipeline size");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_fixed_body_pipelined_request(void) {
  static const unsigned char body[] = {'A', 'B', 'C'};
  unsigned char raw[512];
  unsigned char pipeline[1024];
  struct fixture fixture;
  struct omni_http_request_result first;
  struct omni_http_request_result second;
  struct omni_http_request_consume_result consumed;
  const size_t length = make_fixed_request(raw, sizeof(raw), sizeof(body), body);
  const size_t tail = sizeof(REQ_A) - 1u;

  (void)memset(&fixture, 0, sizeof(fixture));
  (void)memcpy(pipeline, raw, length);
  (void)memcpy(pipeline + length, REQ_A, tail);
  check(load_raw(&fixture, pipeline, length + tail), "fixed-body pipeline loads");

  first = assemble(&fixture);
  check(first.status == OMNI_HTTP_REQUEST_COMPLETE, "fixed-body pipeline first completes");
  check(first.consumed_bytes == length, "first boundary stops after the declared body");
  consumed = omni_http_request_consume(&fixture.buffer, &first);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "fixed-body pipeline consume succeeds");
  check(consumed.consumed_bytes == length, "consumed exactly through the body bytes");
  check(consumed.remaining_readable_bytes == tail, "the GET request remains readable");

  second = assemble(&fixture);
  check(second.status == OMNI_HTTP_REQUEST_COMPLETE, "next request after a body is a GET");
  check(second.framing.framing == OMNI_HTTP_REQUEST_BODY_NONE, "next request has no body");
  check(memcmp(omni_bytebuf_read_ptr(&fixture.buffer, NULL), REQ_A, tail) == 0,
        "next request bytes are the bytes after the consumed body");
  omni_bytebuf_destroy(&fixture.buffer);
}

/* ------------------------------------------------------ non-consumable state */

static void test_incomplete_never_consumes(void) {
  static const unsigned char partial_line[] = "GET / HTTP/1.1";
  static const unsigned char partial_head[] = "GET / HTTP/1.1\r\nHost: a\r\n";
  unsigned char raw[512];
  const size_t full = make_fixed_request(raw, sizeof(raw), 5u,
                                         (const unsigned char *)"abcde");
  const size_t partial_body = full - 2u;
  const unsigned char *inputs[3];
  size_t lengths[3];
  struct fixture fixture;

  (void)memset(&fixture, 0, sizeof(fixture));
  inputs[0] = partial_line;
  lengths[0] = sizeof(partial_line) - 1u;
  inputs[1] = partial_head;
  lengths[1] = sizeof(partial_head) - 1u;
  inputs[2] = raw;
  lengths[2] = partial_body;

  for (size_t i = 0u; i < 3u; ++i) {
    struct omni_http_request_result result;
    struct omni_http_request_consume_result consumed;
    struct fixture_snapshot snapshot;

    check(load_raw(&fixture, inputs[i], lengths[i]), "incomplete fixture loads");
    snapshot = snapshot_fixture(&fixture);
    result = assemble(&fixture);
    check(result.status == OMNI_HTTP_REQUEST_INCOMPLETE, "every incomplete case stays INCOMPLETE");
    check(result.consumed_bytes == 0u, "incomplete result exposes no consumable boundary");
    check(result.source_read_ptr == NULL && result.source_readable_length == 0u &&
              result.source_read_generation == 0u,
          "incomplete result exposes no usable source snapshot");
    consumed = omni_http_request_consume(&fixture.buffer, &result);
    check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_NOT_COMPLETE,
          "incomplete request is never consumable");
    check(consumed.consumed_bytes == 0u, "incomplete request consumes zero");
    check(consumed.remaining_readable_bytes == 0u, "incomplete request reports no remainder");
    check_unchanged(&fixture, &snapshot, "incomplete request leaves metadata and backing intact");
    omni_bytebuf_destroy(&fixture.buffer);
  }
}

static void test_terminal_errors_never_consume(void) {
  static const unsigned char invalid_head[] = "GET / HTTP/1.1\r\nBad Header\r\n\r\n";
  static const unsigned char unsupported[] = "GET / HTTP/2.0\r\n\r\n";
  static const unsigned char two_headers[] = "GET / HTTP/1.1\r\nHost: a\r\nAccept: b\r\n\r\n";
  static const unsigned char bad_length[] = "POST / HTTP/1.1\r\nContent-Length: nope\r\n\r\n";
  static const unsigned char conflicting[] =
      "POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nAB";
  static const unsigned char ambiguous[] =
      "POST / HTTP/1.1\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\nAB";
  static const unsigned char transfer_encoding[] =
      "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
  const unsigned char *inputs[7];
  enum omni_http_request_status statuses[7];
  size_t capacities[7];
  struct fixture fixture;

  (void)memset(&fixture, 0, sizeof(fixture));
  inputs[0] = invalid_head;
  statuses[0] = OMNI_HTTP_REQUEST_INVALID_REQUEST_HEAD;
  capacities[0] = HEADER_CAPACITY;
  inputs[1] = unsupported;
  statuses[1] = OMNI_HTTP_REQUEST_UNSUPPORTED_VERSION;
  capacities[1] = HEADER_CAPACITY;
  inputs[2] = two_headers;
  statuses[2] = OMNI_HTTP_REQUEST_TOO_MANY_HEADERS;
  /* Two headers with room for one: a caller-capacity limit, not a parse error. */
  capacities[2] = 1u;
  inputs[3] = bad_length;
  statuses[3] = OMNI_HTTP_REQUEST_INVALID_FRAMING;
  capacities[3] = HEADER_CAPACITY;
  inputs[4] = conflicting;
  statuses[4] = OMNI_HTTP_REQUEST_INVALID_FRAMING;
  capacities[4] = HEADER_CAPACITY;
  inputs[5] = ambiguous;
  statuses[5] = OMNI_HTTP_REQUEST_INVALID_FRAMING;
  capacities[5] = HEADER_CAPACITY;
  inputs[6] = transfer_encoding;
  statuses[6] = OMNI_HTTP_REQUEST_UNSUPPORTED_FRAMING;
  capacities[6] = HEADER_CAPACITY;

  for (size_t i = 0u; i < 7u; ++i) {
    struct omni_http_request_result result;
    struct omni_http_request_consume_result consumed;
    struct fixture_snapshot snapshot;

    check(load_raw(&fixture, inputs[i], raw_length(inputs[i])), "terminal-error fixture loads");
    snapshot = snapshot_fixture(&fixture);
    result = omni_http_request_assemble(&fixture.buffer, fixture.headers, capacities[i]);
    check(result.status == statuses[i], "terminal error maps to the expected Task 038 status");
    check(result.consumed_bytes == 0u, "terminal error exposes no consumable boundary");
    check(result.source_read_ptr == NULL && result.source_readable_length == 0u &&
              result.source_read_generation == 0u,
          "terminal error exposes no usable source snapshot");
    consumed = omni_http_request_consume(&fixture.buffer, &result);
    check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_NOT_COMPLETE,
          "terminal error is never consumable");
    check(consumed.consumed_bytes == 0u, "terminal error consumes zero");
    check_unchanged(&fixture, &snapshot, "terminal error leaves metadata and backing intact");
    omni_bytebuf_destroy(&fixture.buffer);
  }
}

/* --------------------------------------------------------- stale-result path */

static void test_double_consume_is_rejected(void) {
  unsigned char pipeline[256];
  struct fixture fixture;
  struct omni_http_request_result first;
  struct omni_http_request_consume_result consumed;
  struct fixture_snapshot snapshot;
  const size_t first_length = sizeof(REQ_A) - 1u;
  const size_t second_length = sizeof(REQ_C) - 1u;

  (void)memset(&fixture, 0, sizeof(fixture));
  (void)memcpy(pipeline, REQ_A, first_length);
  (void)memcpy(pipeline + first_length, REQ_C, second_length);
  check(load_raw(&fixture, pipeline, first_length + second_length), "double-consume fixture loads");

  first = assemble(&fixture);
  check(first.status == OMNI_HTTP_REQUEST_COMPLETE, "double-consume request completes");
  consumed = omni_http_request_consume(&fixture.buffer, &first);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "first consume succeeds");

  snapshot = snapshot_fixture(&fixture);
  consumed = omni_http_request_consume(&fixture.buffer, &first);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_STALE,
        "consuming the same result twice is stale");
  check(consumed.consumed_bytes == 0u, "the stale second consume removes nothing");
  check(consumed.remaining_readable_bytes == 0u, "the stale second consume reports no remainder");
  check_unchanged(&fixture, &snapshot, "the stale second consume leaves the buffer untouched");
  check(omni_bytebuf_readable(&fixture.buffer) == second_length,
        "the next pipelined request is still fully readable");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_old_result_cannot_consume_next_request(void) {
  unsigned char pipeline[256];
  struct fixture fixture;
  struct omni_http_request_result old_result;
  struct omni_http_request_consume_result consumed;
  struct fixture_snapshot snapshot;
  const size_t first_length = sizeof(REQ_A) - 1u;
  const size_t second_length = sizeof(REQ_C) - 1u;

  (void)memset(&fixture, 0, sizeof(fixture));
  (void)memcpy(pipeline, REQ_A, first_length);
  (void)memcpy(pipeline + first_length, REQ_C, second_length);
  check(load_raw(&fixture, pipeline, first_length + second_length),
        "old-result fixture loads");

  old_result = assemble(&fixture);
  check(old_result.status == OMNI_HTTP_REQUEST_COMPLETE, "old-result request A completes");
  consumed = omni_http_request_consume(&fixture.buffer, &old_result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "old-result consume A succeeds");
  {
    struct omni_http_request_result fresh = assemble(&fixture);
    check(fresh.status == OMNI_HTTP_REQUEST_COMPLETE, "request B assembles after consume A");
    check(fresh.source_read_ptr == old_result.source_read_ptr + first_length,
          "request B snapshot points past the consumed boundary");
  }

  snapshot = snapshot_fixture(&fixture);
  consumed = omni_http_request_consume(&fixture.buffer, &old_result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_STALE,
        "the old result cannot consume the next pipelined request");
  check(consumed.consumed_bytes == 0u, "the old result removes nothing");
  check_unchanged(&fixture, &snapshot, "request B remains untouched by the stale attempt");
  check(memcmp(omni_bytebuf_read_ptr(&fixture.buffer, NULL), REQ_C, second_length) == 0,
        "request B is byte-identical after the stale attempt");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_compaction_after_assembly_is_rejected(void) {
  static const unsigned char prefix[] = "CONSUMED-PREFIX";
  unsigned char pipeline[256];
  struct fixture fixture;
  struct omni_http_request_result result;
  struct omni_http_request_consume_result consumed;
  struct fixture_snapshot snapshot;
  const size_t first_length = sizeof(REQ_A) - 1u;
  const size_t second_length = sizeof(REQ_C) - 1u;
  const size_t start = sizeof(prefix) - 1u;

  (void)memset(&fixture, 0, sizeof(fixture));
  (void)memcpy(pipeline, REQ_A, first_length);
  (void)memcpy(pipeline + first_length, REQ_C, second_length);
  /* A consumed prefix is required: compaction only moves bytes when read > 0. */
  check(load_fixture(&fixture, prefix, start, pipeline, first_length + second_length),
        "compaction fixture loads");
  check(fixture.buffer.read == start, "compaction fixture starts with a consumed prefix");

  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "compaction fixture request completes");
  check(result.source_read_ptr == fixture.storage + start, "snapshot is at the nonzero read offset");
  /* Caller policy compacts, moving the readable start under the borrowed view;
     the stale snapshot must catch it. */
  omni_bytebuf_compact(&fixture.buffer);
  check(fixture.buffer.read == 0u, "explicit compaction moved the readable start to zero");
  check(memcmp(omni_bytebuf_read_ptr(&fixture.buffer, NULL), REQ_A, first_length) == 0,
        "the request itself survived the memmove");
  snapshot = snapshot_fixture(&fixture);
  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_STALE,
        "a result assembled before compaction is stale");
  check(consumed.consumed_bytes == 0u, "post-compaction stale consume removes nothing");
  check_unchanged(&fixture, &snapshot, "post-compaction stale consume changes nothing");

  /* The caller must reassemble after compaction. */
  {
    struct omni_http_request_result fresh = assemble(&fixture);
    struct omni_http_request_consume_result retry;
    check(fresh.status == OMNI_HTTP_REQUEST_COMPLETE, "reassembled request still completes");
    check(fresh.source_read_ptr == fixture.storage, "reassembled snapshot matches the moved start");
    retry = omni_http_request_consume(&fixture.buffer, &fresh);
    check(retry.status == OMNI_HTTP_REQUEST_CONSUME_OK, "reassembled result consumes");
    check(retry.consumed_bytes == first_length, "reassembled consume uses the same boundary");
    check(retry.remaining_readable_bytes == second_length, "the moved request B remains readable");
  }
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_reset_after_assembly_is_rejected(void) {
  unsigned char pipeline[256];
  struct fixture fixture;
  struct omni_http_request_result result;
  struct omni_http_request_consume_result consumed;
  const size_t first_length = sizeof(REQ_A) - 1u;

  (void)memset(&fixture, 0, sizeof(fixture));
  (void)memcpy(pipeline, REQ_A, first_length);
  (void)memcpy(pipeline + first_length, REQ_C, sizeof(REQ_C) - 1u);
  check(load_raw(&fixture, pipeline, sizeof(REQ_A) + sizeof(REQ_C) - 2u), "reset fixture loads");

  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "reset fixture request completes");
  omni_bytebuf_reset(&fixture.buffer);
  check(omni_bytebuf_readable(&fixture.buffer) == 0u, "reset empties the readable region");
  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_STALE,
        "a result assembled before reset is stale");
  check(consumed.consumed_bytes == 0u, "post-reset stale consume removes nothing");
  check(omni_bytebuf_readable(&fixture.buffer) == 0u, "post-reset readable count cannot underflow");

  /* Reuse after reset with a fresh request is the supported path. */
  check(omni_bytebuf_append(&fixture.buffer, REQ_C, sizeof(REQ_C) - 1u), "reuse append succeeds");
  {
    struct omni_http_request_result fresh = assemble(&fixture);
    struct omni_http_request_consume_result retry;
    check(fresh.status == OMNI_HTTP_REQUEST_COMPLETE, "reused buffer assembles a fresh request");
    retry = omni_http_request_consume(&fixture.buffer, &fresh);
    check(retry.status == OMNI_HTTP_REQUEST_CONSUME_OK, "fresh request after reset consumes");
    check(retry.consumed_bytes == sizeof(REQ_C) - 1u, "fresh consume is exact after reset");
  }
  omni_bytebuf_destroy(&fixture.buffer);
}

/* ----------------------------------- generation-epoch stale-view aliases */
/*
 * The pointer-only identity check is insufficient: consume-to-empty and
 * compact both return the readable start to the same backing address for a
 * DIFFERENT logical region, and a reset/refill can restore the exact old
 * address. These regressions prove the readable-view generation epoch
 * rejects every same-address alias while leaving the valid lifecycle
 * (safe tail append, zero consume) working.
 */
static void test_stale_generation_aliases(void) {
  unsigned char pipeline[256];
  struct fixture fixture;
  struct omni_http_request_result result;
  struct omni_http_request_consume_result consumed;
  struct fixture_snapshot snapshot;
  const size_t first_length = sizeof(REQ_A) - 1u;
  const size_t second_length = sizeof(REQ_C) - 1u;

  /* 1. Consume-to-empty, then refill: the readable start returns to backing
     offset zero — the exact address A's snapshot recorded — but it now
     names request B. */
  (void)memset(&fixture, 0, sizeof(fixture));
  check(load_raw(&fixture, REQ_A, first_length), "consume-empty-refill fixture loads A");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "consume-empty-refill A completes");
  check(result.source_read_ptr == fixture.storage, "A snapshot is at backing offset zero");
  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "consume-empty-refill A consumes");
  check(omni_bytebuf_readable(&fixture.buffer) == 0u, "buffer is canonically empty after A");
  check(omni_bytebuf_read_ptr(&fixture.buffer, NULL) == NULL, "empty readable view is NULL");

  check(omni_bytebuf_append(&fixture.buffer, REQ_C, second_length), "request B appends after drain");
  check(omni_bytebuf_read_ptr(&fixture.buffer, NULL) == result.source_read_ptr,
        "refill restores the exact old readable address (the alias)");
  check(omni_bytebuf_read_generation(&fixture.buffer) != result.source_read_generation,
        "but the generation epoch moved on");
  snapshot = snapshot_fixture(&fixture);
  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_STALE,
        "old A result cannot consume the refilled request at the same address");
  check(consumed.consumed_bytes == 0u, "same-address stale consume removes nothing");
  check_unchanged(&fixture, &snapshot, "request B remains untouched by the stale attempt");
  check(memcmp(fixture.storage, REQ_C, second_length) == 0,
        "request B is byte-identical after the rejected consume");
  omni_bytebuf_destroy(&fixture.buffer);

  /* 2. Pipeline then compact with no consumed prefix: after consuming A and
     compacting, B sits at backing offset zero — A's old snapshot address. */
  (void)memset(&fixture, 0, sizeof(fixture));
  (void)memcpy(pipeline, REQ_A, first_length);
  (void)memcpy(pipeline + first_length, REQ_C, second_length);
  check(load_raw(&fixture, pipeline, first_length + second_length),
        "compact-alias fixture loads A+B");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "compact-alias A completes");
  check(result.source_read_ptr == fixture.storage, "compact-alias A snapshot is at offset zero");
  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "compact-alias A consumes");
  omni_bytebuf_compact(&fixture.buffer);
  check(fixture.buffer.read == 0u, "compaction moved B to backing offset zero");
  check(omni_bytebuf_read_ptr(&fixture.buffer, NULL) == result.source_read_ptr,
        "compaction restored the exact old readable address (the alias)");
  check(omni_bytebuf_read_generation(&fixture.buffer) != result.source_read_generation,
        "but the generation epoch advanced");
  snapshot = snapshot_fixture(&fixture);
  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_STALE,
        "old A result cannot consume compacted B at the same address");
  check(consumed.consumed_bytes == 0u, "compact-alias stale consume removes nothing");
  check_unchanged(&fixture, &snapshot, "compacted B remains untouched by the stale attempt");
  check(memcmp(fixture.storage, REQ_C, second_length) == 0,
        "compacted B is byte-identical after the rejected consume");
  omni_bytebuf_destroy(&fixture.buffer);

  /* 3. Reset then refill: reset returns every offset to zero, so a refilled
     request reuses A's exact snapshot address. */
  (void)memset(&fixture, 0, sizeof(fixture));
  check(load_raw(&fixture, REQ_A, first_length), "reset-refill fixture loads A");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "reset-refill A completes");
  omni_bytebuf_reset(&fixture.buffer);
  check(omni_bytebuf_readable(&fixture.buffer) == 0u, "reset empties the buffer");
  check(omni_bytebuf_append(&fixture.buffer, REQ_C, second_length), "request B appends after reset");
  check(omni_bytebuf_read_ptr(&fixture.buffer, NULL) == result.source_read_ptr,
        "refill after reset restores the exact old readable address (the alias)");
  check(omni_bytebuf_read_generation(&fixture.buffer) != result.source_read_generation,
        "but the generation epoch advanced");
  snapshot = snapshot_fixture(&fixture);
  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_STALE,
        "old A result cannot consume the reset-refilled request");
  check(consumed.consumed_bytes == 0u, "reset-refill stale consume removes nothing");
  check_unchanged(&fixture, &snapshot, "reset-refilled B remains untouched");
  check(memcmp(fixture.storage, REQ_C, second_length) == 0,
        "reset-refilled B is byte-identical after the rejected consume");
  omni_bytebuf_destroy(&fixture.buffer);

  /* 4. External partial consume invalidates the assembled view even though
     the request bytes themselves are untouched. */
  (void)memset(&fixture, 0, sizeof(fixture));
  check(load_raw(&fixture, REQ_A, first_length), "partial-consume fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "partial-consume A completes");
  check(omni_bytebuf_consume(&fixture.buffer, 4u), "external partial consume succeeds");
  snapshot = snapshot_fixture(&fixture);
  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_STALE,
        "a result assembled before an external partial consume is stale");
  check(consumed.consumed_bytes == 0u, "partial-consume stale attempt removes nothing");
  check_unchanged(&fixture, &snapshot, "buffer unchanged after the stale attempt");
  check(omni_bytebuf_readable(&fixture.buffer) == first_length - 4u,
        "the externally consumed prefix stays consumed");
  omni_bytebuf_destroy(&fixture.buffer);

  /* 5. Zero consume is a true no-op: the assembled view stays consumable. */
  (void)memset(&fixture, 0, sizeof(fixture));
  check(load_raw(&fixture, REQ_A, first_length), "zero-consume fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "zero-consume A completes");
  check(omni_bytebuf_consume(&fixture.buffer, 0u), "zero consume succeeds as a no-op");
  check(omni_bytebuf_read_generation(&fixture.buffer) == result.source_read_generation,
        "zero consume does not advance the generation");
  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK,
        "zero consume does not invalidate the assembled request");
  check(consumed.consumed_bytes == first_length, "zero-consume lifecycle consumes exactly A");
  omni_bytebuf_destroy(&fixture.buffer);

  /* 6. Safe tail append remains valid across the generation check: the
     readable start AND the epoch are both unchanged by tail growth. */
  (void)memset(&fixture, 0, sizeof(fixture));
  check(load_raw(&fixture, REQ_A, first_length), "safe-append generation fixture loads A");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "safe-append A completes");
  check(omni_bytebuf_append(&fixture.buffer, REQ_C, second_length), "tail append succeeds");
  check(omni_bytebuf_read_ptr(&fixture.buffer, NULL) == result.source_read_ptr,
        "tail append leaves the readable start in place");
  check(omni_bytebuf_read_generation(&fixture.buffer) == result.source_read_generation,
        "tail append leaves the generation in place");
  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK,
        "safe tail append still consumes under the generation check");
  check(consumed.consumed_bytes == first_length, "only A is consumed");
  check(consumed.remaining_readable_bytes == second_length, "appended B stays readable");
  omni_bytebuf_destroy(&fixture.buffer);
}

/* ------------------------------------------------ append between the two ops */

static void test_append_between_assemble_and_consume(void) {
  unsigned char pipeline[512];
  struct fixture fixture;
  struct omni_http_request_result first;
  struct omni_http_request_result second;
  struct omni_http_request_consume_result consumed;
  const size_t first_length = sizeof(REQ_A) - 1u;
  const size_t second_length = sizeof(REQ_C) - 1u;

  (void)memset(&fixture, 0, sizeof(fixture));
  (void)memcpy(pipeline, REQ_A, first_length);
  (void)memcpy(pipeline + first_length, REQ_C, second_length);
  check(load_raw(&fixture, pipeline, first_length), "append-after-assembly fixture loads A only");

  first = assemble(&fixture);
  check(first.status == OMNI_HTTP_REQUEST_COMPLETE, "request A completes before the append");
  check(first.source_readable_length == first_length, "snapshot length is A only");
  check(omni_bytebuf_readable(&fixture.buffer) == first_length, "buffer holds A only");

  /* More network data arrives between assembly and consumption. */
  check(omni_bytebuf_append(&fixture.buffer, REQ_C, second_length), "request B appends to the tail");
  check(omni_bytebuf_readable(&fixture.buffer) == first_length + second_length,
        "buffer now holds both requests");
  check(omni_bytebuf_read_ptr(&fixture.buffer, NULL) == first.source_read_ptr,
        "tail append leaves the readable start where the snapshot recorded it");

  consumed = omni_http_request_consume(&fixture.buffer, &first);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK,
        "safe tail growth does not invalidate the assembled request");
  check(consumed.consumed_bytes == first_length, "only request A is consumed after the append");
  check(consumed.remaining_readable_bytes == second_length, "the appended request stays readable");

  second = assemble(&fixture);
  check(second.status == OMNI_HTTP_REQUEST_COMPLETE, "the appended request assembles next");
  check(second.consumed_bytes == second_length, "appended request boundary is exact");
  omni_bytebuf_destroy(&fixture.buffer);
}

/* ---------------------------------------------- read offset and no compaction */

static void test_read_offset_nonzero(void) {
  static const unsigned char prefix[] = "CONSUMED-PREFIX";
  unsigned char pipeline[512];
  struct fixture fixture;
  struct omni_http_request_result result;
  struct omni_http_request_consume_result consumed;
  const size_t prefix_length = sizeof(prefix) - 1u;
  const size_t first_length = sizeof(REQ_A) - 1u;
  const size_t second_length = sizeof(REQ_C) - 1u;
  const size_t start = prefix_length;

  (void)memset(&fixture, 0, sizeof(fixture));
  (void)memcpy(pipeline, REQ_A, first_length);
  (void)memcpy(pipeline + first_length, REQ_C, second_length);
  check(load_fixture(&fixture, prefix, prefix_length, pipeline, first_length + second_length),
        "read-offset fixture loads");
  check(fixture.buffer.read == prefix_length, "fixture read offset is nonzero");
  check(fixture.buffer.read != 0u, "consumption must not assume backing offset zero");

  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "request after a prefix completes");
  check(result.request_head.request_line.target.data == fixture.storage + start + 4u,
        "parsing begins at the current readable start, not backing offset zero");
  check(result.source_read_ptr == fixture.storage + start, "snapshot is relative to read offset");
  check(result.consumed_bytes == first_length, "consumed boundary is relative to read offset");
  check(memcmp(fixture.storage + start + first_length, REQ_C, second_length) == 0,
        "the request after the boundary sits at start + consumed_bytes");

  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "consume works from a nonzero read offset");
  check(consumed.consumed_bytes == first_length, "consumed bytes are relative to read offset");
  check(fixture.buffer.read == start + first_length, "read offset advanced by the request length");
  check(fixture.buffer.write == start + first_length + second_length, "write offset is unchanged");
  check(omni_bytebuf_readable(&fixture.buffer) == second_length, "only the next request remains");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_no_auto_compaction(void) {
  unsigned char pipeline[256];
  struct fixture fixture;
  struct omni_http_request_result result;
  struct omni_http_request_consume_result consumed;
  const size_t first_length = sizeof(REQ_A) - 1u;
  const size_t second_length = sizeof(REQ_C) - 1u;

  (void)memset(&fixture, 0, sizeof(fixture));
  (void)memcpy(pipeline, REQ_A, first_length);
  (void)memcpy(pipeline + first_length, REQ_C, second_length);
  check(load_raw(&fixture, pipeline, first_length + second_length), "no-compaction fixture loads");

  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "no-compaction request completes");
  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "no-compaction consume succeeds");

  /* No memmove toward offset zero: the consumed request's bytes are still at
     their original backing addresses and the read offset simply advanced. */
  check(fixture.buffer.read == first_length, "read offset advanced instead of compacting");
  check(fixture.buffer.read != 0u, "consume did not canonicalize a still-populated buffer");
  check(fixture.buffer.write == first_length + second_length, "write offset untouched");
  check(memcmp(fixture.storage, REQ_A, first_length) == 0,
        "consumed request bytes stay at their original backing offset");
  check(memcmp(fixture.storage + first_length, REQ_C, second_length) == 0,
        "trailing request stays at its original backing offset");
  check(omni_bytebuf_reclaimable(&fixture.buffer) == first_length,
        "the consumed prefix is reclaimable, which is the caller's compaction policy");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_tail_capacity_isolation(void) {
  struct fixture fixture;
  struct omni_http_request_result result;
  struct omni_http_request_consume_result consumed;
  const size_t length = sizeof(REQ_A) - 1u;

  (void)memset(&fixture, 0, sizeof(fixture));
  check(load_raw(&fixture, REQ_A, length), "tail-capacity fixture loads");
  /* Sentinel data beyond the write offset must stay invisible to consumption. */
  (void)memset(fixture.storage + fixture.buffer.write, 0x5a,
               fixture.buffer.capacity - fixture.buffer.write);
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "request completes with sentinel tail");
  check(result.source_readable_length == length, "snapshot ignores the unused tail capacity");
  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "sentinel tail does not affect consume");
  check(consumed.consumed_bytes == length, "consume stops at the write offset, never the capacity");
  check(consumed.remaining_readable_bytes == 0u, "unused capacity never becomes readable");
  for (size_t i = 0u; i < fixture.buffer.capacity; ++i)
    check(fixture.storage[i] == (i < length ? REQ_A[i] : 0x5au),
          "unused capacity sentinel bytes are neither read nor written");
  omni_bytebuf_destroy(&fixture.buffer);
}

/* --------------------------------------------------------- ownership/immut. */

static void test_callers_data_is_never_mutated(void) {
  unsigned char pipeline[256];
  struct fixture fixture;
  struct omni_http_request_result result;
  struct omni_http_request_result result_before;
  struct omni_http_request_consume_result consumed;
  struct omni_http_header headers_before[HEADER_CAPACITY];
  const size_t first_length = sizeof(REQ_B) - 1u;
  const size_t second_length = sizeof(REQ_C) - 1u;
  size_t head_length;

  (void)memset(&fixture, 0, sizeof(fixture));
  (void)memcpy(pipeline, REQ_B, first_length);
  (void)memcpy(pipeline + first_length, REQ_C, second_length);
  check(load_raw(&fixture, pipeline, first_length + second_length), "ownership fixture loads");

  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "ownership fixture request completes");
  head_length = result.request_head.consumed_bytes;
  check(result.body.body.data == fixture.storage + head_length,
        "the borrowed body view starts inside the bytebuf backing");
  result_before = result;
  (void)memcpy(headers_before, fixture.headers, sizeof(headers_before));
  check(result.request_head.headers == fixture.headers, "view uses the caller's header storage");
  check(result.request_head.header_count == 1u, "the request exposes exactly one header");

  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "ownership fixture consume succeeds");
  check(memcmp(&result, &result_before, sizeof(result)) == 0,
        "the caller's request result is not rewritten or cleared");
  check(memcmp(fixture.headers, headers_before, sizeof(headers_before)) == 0,
        "caller header storage is byte-identical after consumption");
  check(memcmp(fixture.storage, REQ_B, first_length) == 0, "request head and body bytes are untouched");
  check(memcmp(result.body.body.data, REQ_B_BODY, sizeof(REQ_B_BODY)) == 0,
        "consume does not wipe or overwrite the consumed body bytes");
  check(result.body.body.data == fixture.storage + head_length,
        "the borrowed body view is not relocated by consume");
  check(omni_bytebuf_read_ptr(&fixture.buffer, NULL) == fixture.storage + first_length,
        "the old view no longer points at the current readable start");
  omni_bytebuf_destroy(&fixture.buffer);
}

static void test_inconsistent_metadata_is_rejected(void) {
  struct fixture fixture;
  struct omni_http_request_result result;
  struct omni_http_request_result forged;
  struct omni_http_request_consume_result consumed;
  const size_t length = sizeof(REQ_A) - 1u;

  (void)memset(&fixture, 0, sizeof(fixture));
  check(load_raw(&fixture, REQ_A, length), "forged-metadata fixture loads");
  result = assemble(&fixture);
  check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "forged-metadata fixture completes");

  /* A fabricated COMPLETE result with no source snapshot at all. */
  forged = result;
  forged.source_read_ptr = NULL;
  consumed = omni_http_request_consume(&fixture.buffer, &forged);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_STALE,
        "a COMPLETE result without a source snapshot is rejected");
  check(consumed.consumed_bytes == 0u, "snapshot-less result consumes zero");
  check(omni_bytebuf_readable(&fixture.buffer) == length, "the real request is still readable");

  /* A snapshot naming a different address than the current readable start. */
  forged = result;
  forged.source_read_ptr = fixture.storage + 1u;
  consumed = omni_http_request_consume(&fixture.buffer, &forged);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_STALE,
        "a snapshot from another address is rejected");
  check(consumed.consumed_bytes == 0u, "foreign snapshot consumes zero");
  check(omni_bytebuf_readable(&fixture.buffer) == length,
        "a foreign snapshot cannot advance the real request");

  /* A snapshot with the right address but a stale generation epoch: the
     pointer matches, so only the generation check can reject it. */
  forged = result;
  forged.source_read_generation = result.source_read_generation + 1u;
  consumed = omni_http_request_consume(&fixture.buffer, &forged);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_STALE,
        "a snapshot with a stale generation epoch is rejected");
  check(consumed.consumed_bytes == 0u, "stale-generation snapshot consumes zero");
  check(omni_bytebuf_readable(&fixture.buffer) == length,
        "a stale-generation snapshot cannot advance the real request");

  /* A fabricated COMPLETE result with no generation recorded at all. */
  forged = result;
  forged.source_read_generation = 0u;
  consumed = omni_http_request_consume(&fixture.buffer, &forged);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_STALE,
        "a snapshot with a zero generation epoch is rejected");
  check(consumed.consumed_bytes == 0u, "zero-generation snapshot consumes zero");

  /* Zero-length COMPLETE. */
  forged = result;
  forged.consumed_bytes = 0u;
  consumed = omni_http_request_consume(&fixture.buffer, &forged);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_INCONSISTENT,
        "a zero-length COMPLETE result is inconsistent");
  check(consumed.consumed_bytes == 0u, "zero-length COMPLETE consumes zero");

  /* Boundary past the readable region. */
  forged = result;
  forged.consumed_bytes = length + 1u;
  consumed = omni_http_request_consume(&fixture.buffer, &forged);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_INCONSISTENT,
        "a boundary larger than the readable region is rejected");
  check(consumed.consumed_bytes == 0u, "oversized boundary consumes zero");

  /* A far-future size_t boundary must not wrap into acceptance. */
  forged = result;
  forged.consumed_bytes = SIZE_MAX;
  consumed = omni_http_request_consume(&fixture.buffer, &forged);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_INCONSISTENT,
        "SIZE_MAX boundary is rejected without wrapping");
  check(omni_bytebuf_readable(&fixture.buffer) == length, "readable count survives SIZE_MAX");

  /* Snapshot shorter than the boundary. */
  forged = result;
  forged.source_readable_length = forged.consumed_bytes - 1u;
  consumed = omni_http_request_consume(&fixture.buffer, &forged);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_ERR_INCONSISTENT,
        "a snapshot shorter than the boundary is rejected");
  check(omni_bytebuf_readable(&fixture.buffer) == length, "no forged variant consumed anything");

  /* The unmodified real result still consumes. */
  consumed = omni_http_request_consume(&fixture.buffer, &result);
  check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "the genuine result still consumes");
  check(consumed.consumed_bytes == length, "genuine consume stays exact");
  omni_bytebuf_destroy(&fixture.buffer);
}

/* ----------------------------------------- full assemble->process->consume */

static void test_full_lifecycle_integration(void) {
  unsigned char raw[512];
  unsigned char pipeline[1024];
  static const unsigned char body[] = {0x00u, 0x01u, 0x7fu, 0x80u, 0xffu};
  struct fixture fixture;
  const size_t body_length = make_fixed_request(raw, sizeof(raw), sizeof(body), body);

  (void)memset(&fixture, 0, sizeof(fixture));

  /* 1. no-body request: inspect the borrowed view, then consume. */
  (void)memcpy(pipeline, REQ_A, sizeof(REQ_A) - 1u);
  (void)memcpy(pipeline + sizeof(REQ_A) - 1u, REQ_C, sizeof(REQ_C) - 1u);
  check(load_raw(&fixture, pipeline, sizeof(REQ_A) + sizeof(REQ_C) - 2u),
        "lifecycle no-body pipeline loads");
  {
    struct omni_http_request_result result = assemble(&fixture);
    struct omni_http_request_consume_result consumed;
    check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "lifecycle: no-body assembles");
    check(result.request_head.request_line.method.length == 3u &&
              memcmp(result.request_head.request_line.method.data, "GET", 3u) == 0,
          "lifecycle: caller inspects the borrowed method before consuming");
    check(result.request_head.request_line.target.length == 2u &&
              memcmp(result.request_head.request_line.target.data, "/a", 2u) == 0,
          "lifecycle: caller inspects the borrowed target before consuming");
    consumed = omni_http_request_consume(&fixture.buffer, &result);
    check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "lifecycle: no-body consumes");
    check(consumed.remaining_readable_bytes == sizeof(REQ_C) - 1u,
          "lifecycle: the next region is exactly the next request");
  }
  omni_bytebuf_destroy(&fixture.buffer);

  /* 2. fixed-length binary body: inspect the borrowed body, then consume. */
  check(load_raw(&fixture, raw, body_length), "lifecycle binary-body fixture loads");
  {
    struct omni_http_request_result result = assemble(&fixture);
    struct omni_http_request_consume_result consumed;
    check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "lifecycle: binary body assembles");
    check(result.framing.content_length == sizeof(body), "lifecycle: declared length is exact");
    check(result.body.body.length == sizeof(body) &&
              memcmp(result.body.body.data, body, sizeof(body)) == 0,
          "lifecycle: caller inspects the borrowed binary body before consuming");
    check(result.consumed_bytes == result.request_head.consumed_bytes + sizeof(body),
          "lifecycle: the boundary covers the whole body");
    consumed = omni_http_request_consume(&fixture.buffer, &result);
    check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "lifecycle: binary body consumes");
    check(consumed.consumed_bytes == body_length, "lifecycle: binary consume is exact");
    check(consumed.remaining_readable_bytes == 0u, "lifecycle: nothing trails the binary body");
  }
  omni_bytebuf_destroy(&fixture.buffer);

  /* 3. pipelined: the caller drives assemble/consume one request at a time. */
  (void)memcpy(pipeline, REQ_A, sizeof(REQ_A) - 1u);
  (void)memcpy(pipeline + sizeof(REQ_A) - 1u, raw, body_length);
  check(load_raw(&fixture, pipeline, sizeof(REQ_A) - 1u + body_length),
        "lifecycle mixed pipeline loads");
  for (size_t step = 0u; step < 2u; ++step) {
    struct omni_http_request_result result = assemble(&fixture);
    struct omni_http_request_consume_result consumed;
    check(result.status == OMNI_HTTP_REQUEST_COMPLETE, "lifecycle: each pipeline step assembles");
    check(step == 0u ? result.framing.framing == OMNI_HTTP_REQUEST_BODY_NONE
                       : result.framing.framing == OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH,
          "lifecycle: each step exposes its own framing");
    consumed = omni_http_request_consume(&fixture.buffer, &result);
    check(consumed.status == OMNI_HTTP_REQUEST_CONSUME_OK, "lifecycle: each step consumes");
    check(consumed.consumed_bytes == result.consumed_bytes,
          "lifecycle: each step consumes its own assembled boundary");
  }
  check(omni_bytebuf_readable(&fixture.buffer) == 0u, "lifecycle: pipeline fully drained");
  omni_bytebuf_destroy(&fixture.buffer);
}

/* ------------------------------------------------------------------- stress */

static void test_repeated_assemble_consume_stress(void) {
  enum { CYCLES = 1000 };
  struct fixture fixture;
  const size_t first_length = sizeof(REQ_A) - 1u;
  const size_t second_length = sizeof(REQ_B) - 1u;
  const size_t pair = first_length + second_length;
  size_t cycles_ok = 0u;
  size_t bytes_total = 0u;

  (void)memset(&fixture, 0, sizeof(fixture));
  check(!omni_bytebuf_init_borrowed(&fixture.buffer, fixture.storage, sizeof(fixture.storage))
            ? false
            : true,
        "stress buffer initializes");
  (void)memset(fixture.storage, 0xa5, sizeof(fixture.storage));
  (void)memset(fixture.headers, 0xcd, sizeof(fixture.headers));

  for (size_t cycle = 0u; cycle < (size_t)CYCLES; ++cycle) {
    struct omni_http_request_result first;
    struct omni_http_request_result second;
    struct omni_http_request_consume_result consumed;
    bool cycle_ok = true;

    if (!omni_bytebuf_append(&fixture.buffer, REQ_A, first_length)) {
      check(false, "stress append of request A succeeds");
      break;
    }
    if (!omni_bytebuf_append(&fixture.buffer, REQ_B, second_length)) {
      check(false, "stress append of request B succeeds");
      break;
    }
    if (omni_bytebuf_readable(&fixture.buffer) != pair)
      cycle_ok = false;

    first = assemble(&fixture);
    if (first.status != OMNI_HTTP_REQUEST_COMPLETE || first.consumed_bytes != first_length)
      cycle_ok = false;
    else {
      /* Inspect the borrowed view the way a real caller would. */
      if (first.request_head.request_line.target.length != 2u ||
          memcmp(first.request_head.request_line.target.data, "/a", 2u) != 0)
        cycle_ok = false;
      consumed = omni_http_request_consume(&fixture.buffer, &first);
      if (consumed.status != OMNI_HTTP_REQUEST_CONSUME_OK || consumed.consumed_bytes != first_length ||
          consumed.remaining_readable_bytes != second_length)
        cycle_ok = false;
    }

    second = assemble(&fixture);
    if (second.status != OMNI_HTTP_REQUEST_COMPLETE || second.consumed_bytes != second_length)
      cycle_ok = false;
    else if (second.framing.content_length != 5u ||
             memcmp(second.body.body.data, REQ_B_BODY, sizeof(REQ_B_BODY)) != 0)
      cycle_ok = false;
    else {
      consumed = omni_http_request_consume(&fixture.buffer, &second);
      if (consumed.status != OMNI_HTTP_REQUEST_CONSUME_OK ||
          consumed.consumed_bytes != second_length || consumed.remaining_readable_bytes != 0u)
        cycle_ok = false;
    }

    if (omni_bytebuf_readable(&fixture.buffer) != 0u)
      cycle_ok = false;
    if (!cycle_ok) {
      check(false, "stress cycle stays consistent");
      break;
    }
    ++cycles_ok;
    bytes_total += pair;
  }
  check(cycles_ok == (size_t)CYCLES, "all 1000 assemble/consume cycles succeed");
  check(bytes_total == (size_t)CYCLES * pair, "stress consumed every appended byte exactly once");
  check(omni_bytebuf_readable(&fixture.buffer) == 0u, "stress ends with an empty readable region");
  check(omni_bytebuf_high_water(&fixture.buffer) == pair,
        "stress high water stays at one pipeline pair: bounded storage, no growth");
  check(omni_bytebuf_capacity(&fixture.buffer) == sizeof(fixture.storage),
        "stress never reallocates or grows the buffer");
  omni_bytebuf_destroy(&fixture.buffer);
}

/* ------------------------------------------------------------ memory model */

static void test_memory_model(void) {
  check(sizeof(struct omni_http_request_consume_result) <= 4u * sizeof(size_t),
        "consume result stays a tiny fixed-size value type (status plus two counters)");
  check(sizeof(struct omni_http_request_result) <= 256u,
        "the Task 038 result grew only by a pointer and a size_t snapshot");
}

int main(void) {
  test_arguments_and_empty();
  test_basic_no_body_consume();
  test_fixed_body_consume();
  test_binary_body_consume();
  test_trailing_data_only_first_consumed();
  test_two_pipelined_requests();
  test_three_pipelined_requests();
  test_fixed_body_pipelined_request();
  test_incomplete_never_consumes();
  test_terminal_errors_never_consume();
  test_double_consume_is_rejected();
  test_old_result_cannot_consume_next_request();
  test_compaction_after_assembly_is_rejected();
  test_reset_after_assembly_is_rejected();
  test_stale_generation_aliases();
  test_append_between_assemble_and_consume();
  test_read_offset_nonzero();
  test_no_auto_compaction();
  test_tail_capacity_isolation();
  test_callers_data_is_never_mutated();
  test_inconsistent_metadata_is_rejected();
  test_full_lifecycle_integration();
  test_repeated_assemble_consume_stress();
  test_memory_model();
  (void)printf("http-request-consume-unit: %zu checks, %zu failures "
               "(consume_result=%zu bytes, request_result=%zu bytes)\n",
               checks, failures, sizeof(struct omni_http_request_consume_result),
               sizeof(struct omni_http_request_result));
  return failures == 0u ? 0 : 1;
}
