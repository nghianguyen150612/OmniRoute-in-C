#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "omniroute/http_request_body.h"

#define RAW_CAPACITY 2048u

struct fixture {
  struct omni_http_header headers[OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS];
  struct omni_http_request_head_result head;
  struct omni_http_request_framing_result framing;
};

static size_t checks;
static size_t failures;

static void check(int condition, const char *label) {
  ++checks;
  if (!condition) {
    ++failures;
    (void)fprintf(stderr, "FAIL: %s\n", label);
  }
}

static int parse_request(const unsigned char *raw, size_t length, struct fixture *fixture) {
  (void)memset(fixture, 0, sizeof(*fixture));
  fixture->head = omni_http_request_head_parse(raw, length, fixture->headers,
                                                OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS);
  if (fixture->head.status != OMNI_HTTP_REQUEST_HEAD_COMPLETE)
    return 0;
  fixture->framing = omni_http_request_framing_analyze(&fixture->head);
  return 1;
}

static struct omni_http_request_body_result view_preserving_inputs(
    const unsigned char *raw, size_t snapshot_length, const struct fixture *fixture,
    const unsigned char *view_data, size_t available_length, const char *label) {
  unsigned char raw_before[RAW_CAPACITY];
  struct omni_http_header headers_before[OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS];
  struct omni_http_request_head_result head_before;
  struct omni_http_request_framing_result framing_before;
  struct omni_http_request_body_result result;

  check(snapshot_length <= sizeof(raw_before), "snapshot fits test raw capacity");
  if (snapshot_length > sizeof(raw_before))
    snapshot_length = sizeof(raw_before);
  if (snapshot_length != 0u)
    (void)memcpy(raw_before, raw, snapshot_length);
  (void)memcpy(headers_before, fixture->headers, sizeof(headers_before));
  (void)memcpy(&head_before, &fixture->head, sizeof(head_before));
  (void)memcpy(&framing_before, &fixture->framing, sizeof(framing_before));

  result = omni_http_request_body_view(view_data, available_length, &fixture->head,
                                       &fixture->framing);

  if (snapshot_length != 0u)
    check(memcmp(raw_before, raw, snapshot_length) == 0, label);
  check(memcmp(headers_before, fixture->headers, sizeof(headers_before)) == 0,
        "caller header storage remains byte-for-byte unchanged");
  check(memcmp(&head_before, &fixture->head, sizeof(head_before)) == 0,
        "Task 035 result remains byte-for-byte unchanged");
  check(memcmp(&framing_before, &fixture->framing, sizeof(framing_before)) == 0,
        "Task 036 result remains byte-for-byte unchanged");
  return result;
}

static void test_no_body_and_trailing(void) {
  unsigned char raw[] = "GET / HTTP/1.1\r\n\r\nXYZ";
  struct fixture fixture;
  struct omni_http_request_body_result result;
  size_t raw_length = sizeof(raw) - 1u;

  check(parse_request(raw, raw_length, &fixture),
        "NO_BODY raw request parses through Tasks 035/036");
  check(fixture.framing.status == OMNI_HTTP_REQUEST_FRAMING_OK &&
            fixture.framing.framing == OMNI_HTTP_REQUEST_BODY_NONE,
        "Task 036 reports NO_BODY");
  result = view_preserving_inputs(raw, raw_length, &fixture, raw, raw_length,
                                  "NO_BODY input remains unchanged");
  check(result.status == OMNI_HTTP_REQUEST_BODY_COMPLETE, "NO_BODY completes");
  check(result.body.length == 0u, "NO_BODY body is empty");
  check(result.consumed_bytes == fixture.head.consumed_bytes,
        "NO_BODY consumes exactly the request head");
  check(result.required_total_bytes == fixture.head.consumed_bytes,
        "NO_BODY required total ends at the request head");
  check(result.body.data == raw + fixture.head.consumed_bytes,
        "NO_BODY returns an empty span at the head boundary");
  check(memcmp(raw + result.consumed_bytes, "XYZ", 3u) == 0,
        "NO_BODY trailing bytes remain untouched");
}

static void test_fixed_zero(void) {
  unsigned char raw[] = "POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\nXYZ";
  struct fixture fixture;
  struct omni_http_request_body_result result;
  size_t raw_length = sizeof(raw) - 1u;

  check(parse_request(raw, raw_length, &fixture),
        "Content-Length zero parses through Tasks 035/036");
  check(fixture.framing.status == OMNI_HTTP_REQUEST_FRAMING_OK &&
            fixture.framing.framing == OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH &&
            fixture.framing.content_length == 0u,
        "Task 036 preserves explicit FIXED_LENGTH(0)");
  result = view_preserving_inputs(raw, raw_length, &fixture, raw, raw_length,
                                  "FIXED_LENGTH(0) input remains unchanged");
  check(result.status == OMNI_HTTP_REQUEST_BODY_COMPLETE, "FIXED_LENGTH(0) completes immediately");
  check(result.body.length == 0u, "FIXED_LENGTH(0) body is empty");
  check(result.body.data == raw + fixture.head.consumed_bytes,
        "FIXED_LENGTH(0) span begins at the body boundary");
  check(result.consumed_bytes == fixture.head.consumed_bytes,
        "FIXED_LENGTH(0) consumes only the head");
  check(result.required_total_bytes == fixture.head.consumed_bytes,
        "FIXED_LENGTH(0) needs no byte after the head");
  check(memcmp(raw + result.consumed_bytes, "XYZ", 3u) == 0,
        "FIXED_LENGTH(0) leaves trailing bytes untouched");
}

static void test_exact_and_binary_body(void) {
  unsigned char raw[] = "POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\nABCDE";
  struct fixture fixture;
  struct omni_http_request_body_result result;
  size_t raw_length = sizeof(raw) - 1u;

  check(parse_request(raw, raw_length, &fixture), "fixed request parses through Tasks 035/036");
  result = view_preserving_inputs(raw, raw_length, &fixture, raw, raw_length,
                                  "fixed body input remains unchanged");
  check(result.status == OMNI_HTTP_REQUEST_BODY_COMPLETE, "exact fixed body completes");
  check(result.body.data == raw + fixture.head.consumed_bytes,
        "exact body uses the original input pointer");
  check(result.body.length == 5u, "exact body span has the declared length");
  check(memcmp(result.body.data, "ABCDE", 5u) == 0, "exact body bytes are preserved");
  check(result.consumed_bytes == fixture.head.consumed_bytes + 5u,
        "exact body consumption includes only the declared bytes");
  check(result.required_total_bytes == result.consumed_bytes,
        "complete exact body required total equals consumption");

  {
    static const unsigned char binary_body[5] = {0x41u, 0x00u, 0x42u, 0xffu, 0x43u};
    static const unsigned char binary_prefix[] =
        "POST /bin HTTP/1.1\r\nContent-Length: 5\r\n\r\n";
    unsigned char binary_raw[RAW_CAPACITY];
    size_t prefix_length = sizeof(binary_prefix) - 1u;
    size_t binary_length = prefix_length + sizeof(binary_body);

    (void)memcpy(binary_raw, binary_prefix, prefix_length);
    (void)memcpy(binary_raw + prefix_length, binary_body, sizeof(binary_body));
    check(parse_request(binary_raw, binary_length, &fixture),
          "binary body request parses through Tasks 035/036");
    result = view_preserving_inputs(binary_raw, binary_length, &fixture, binary_raw, binary_length,
                                    "binary body input remains unchanged");
    check(result.status == OMNI_HTTP_REQUEST_BODY_COMPLETE, "binary body completes");
    check(result.body.data == binary_raw + fixture.head.consumed_bytes,
          "binary body returns exact borrowed pointer");
    check(result.body.length == sizeof(binary_body), "binary body length is exact");
    check(memcmp(result.body.data, binary_body, sizeof(binary_body)) == 0,
          "A, NUL, B, 0xff, and C body bytes are opaque and preserved");
    check(result.consumed_bytes == binary_length, "binary body consumes exactly its request");
  }
}

static void test_every_body_prefix(const unsigned char *head_prefix, size_t body_length) {
  unsigned char raw[RAW_CAPACITY];
  struct fixture fixture;
  size_t prefix_length = strlen((const char *)head_prefix);
  size_t full_length = prefix_length + body_length;

  check(full_length <= sizeof(raw), "prefix fixture fits raw capacity");
  if (full_length > sizeof(raw))
    return;
  (void)memcpy(raw, head_prefix, prefix_length);
  for (size_t i = 0u; i < body_length; ++i)
    raw[prefix_length + i] = (unsigned char)(i * 13u);
  check(parse_request(raw, full_length, &fixture), "prefix request parses through Tasks 035/036");
  check(fixture.framing.content_length == body_length, "prefix fixture framing length is exact");
  check(fixture.head.consumed_bytes == prefix_length, "head boundary matches fixture prefix");

  for (size_t available_body = 0u; available_body <= body_length; ++available_body) {
    struct omni_http_request_body_result result = view_preserving_inputs(
        raw, full_length, &fixture, raw, prefix_length + available_body,
        "body prefix input remains unchanged");
    if (available_body < body_length) {
      check(result.status == OMNI_HTTP_REQUEST_BODY_INCOMPLETE,
            "every incomplete body prefix is INCOMPLETE");
      check(result.body.data == NULL && result.body.length == 0u,
            "incomplete body exposes no partial span");
      check(result.consumed_bytes == 0u, "incomplete body consumes zero bytes");
      check(result.required_total_bytes == full_length,
            "incomplete body reports its exact required total");
    } else {
      check(result.status == OMNI_HTTP_REQUEST_BODY_COMPLETE,
            "the exact final body prefix is COMPLETE");
      check(result.body.data == raw + prefix_length, "complete prefix returns the borrowed body");
      check(result.body.length == body_length, "complete prefix returns exact body length");
      check(result.consumed_bytes == full_length, "complete prefix consumes the full request");
    }
  }
}

static void test_all_prefixes(void) {
  static const unsigned char one[] = "POST / HTTP/1.1\r\nContent-Length: 1\r\n\r\n";
  static const unsigned char two[] = "POST / HTTP/1.1\r\nContent-Length: 2\r\n\r\n";
  static const unsigned char five[] = "POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\n";
  static const unsigned char sixteen[] = "POST / HTTP/1.1\r\nContent-Length: 16\r\n\r\n";

  test_every_body_prefix(one, 1u);
  test_every_body_prefix(two, 2u);
  test_every_body_prefix(five, 5u);
  test_every_body_prefix(sixteen, 16u);
}

static void test_trailing_and_pipeline(void) {
  unsigned char raw[] = "POST /1 HTTP/1.1\r\nContent-Length: 3\r\n\r\nABC"
                        "GET /2 HTTP/1.1\r\n\r\n";
  struct fixture fixture;
  struct omni_http_request_body_result result;
  size_t raw_length = sizeof(raw) - 1u;
  static const unsigned char body_and_next[] = "ABCGET /2 HTTP/1.1\r\n\r\n";

  check(parse_request(raw, raw_length, &fixture), "pipelined request parses through Tasks 035/036");
  result = view_preserving_inputs(raw, raw_length, &fixture, raw, raw_length,
                                  "pipelined input remains unchanged");
  check(result.status == OMNI_HTTP_REQUEST_BODY_COMPLETE, "fixed body before pipeline completes");
  check(result.body.length == 3u && memcmp(result.body.data, "ABC", 3u) == 0,
        "pipelined request body is exactly ABC");
  check(result.consumed_bytes == fixture.head.consumed_bytes + 3u,
        "pipeline consumption stops immediately after ABC");
  check(memcmp(raw + result.consumed_bytes, body_and_next + 3u,
               sizeof(body_and_next) - 1u - 3u) == 0,
        "next pipelined request remains untouched");
}

static void test_all_byte_values(void) {
  static const unsigned char prefix[] =
      "POST /octets HTTP/1.1\r\nContent-Length: 256\r\n\r\n";
  unsigned char raw[RAW_CAPACITY];
  struct fixture fixture;
  struct omni_http_request_body_result result;
  size_t prefix_length = sizeof(prefix) - 1u;
  size_t raw_length = prefix_length + 256u;

  (void)memcpy(raw, prefix, prefix_length);
  for (size_t i = 0u; i < 256u; ++i)
    raw[prefix_length + i] = (unsigned char)i;
  check(parse_request(raw, raw_length, &fixture), "all-octet request parses through Tasks 035/036");
  result = view_preserving_inputs(raw, raw_length, &fixture, raw, raw_length,
                                  "all-octet input remains unchanged");
  check(result.status == OMNI_HTTP_REQUEST_BODY_COMPLETE, "256-byte opaque body completes");
  check(result.body.data == raw + fixture.head.consumed_bytes,
        "all-octet body pointer is zero-copy");
  check(result.body.length == 256u, "all-octet body length is 256");
  check(result.consumed_bytes == raw_length, "all-octet body consumption is exact");
  for (size_t i = 0u; i < 256u; ++i)
    check(result.body.data[i] == (unsigned char)i, "body octets 0x00 through 0xff stay opaque");
}

static void test_large_metadata_and_overflow(void) {
  unsigned char raw[] = "POST /large HTTP/1.1\r\nContent-Length: 1\r\n\r\nX";
  struct fixture fixture;
  struct omni_http_request_body_result result;
  size_t raw_length = sizeof(raw) - 1u;

  check(parse_request(raw, raw_length, &fixture),
        "large-metadata request parses through Tasks 035/036");
  fixture.framing.content_length = SIZE_MAX - fixture.head.consumed_bytes;
  result = view_preserving_inputs(raw, raw_length, &fixture, raw, raw_length,
                                  "large metadata input remains unchanged");
  check(result.status == OMNI_HTTP_REQUEST_BODY_INCOMPLETE,
        "largest representable total reports INCOMPLETE without allocation");
  check(result.body.data == NULL && result.body.length == 0u && result.consumed_bytes == 0u,
        "large incomplete body exposes no span or consumption");
  check(result.required_total_bytes == SIZE_MAX,
        "largest valid total is calculated without imposing a body cap");

  fixture.framing.content_length = SIZE_MAX - fixture.head.consumed_bytes + 1u;
  result = omni_http_request_body_view(raw, raw_length, &fixture.head, &fixture.framing);
  check(result.status == OMNI_HTTP_REQUEST_BODY_OVERFLOW,
        "head plus fixed length overflow is rejected explicitly");
  check(result.body.data == NULL && result.body.length == 0u && result.consumed_bytes == 0u &&
            result.required_total_bytes == 0u,
        "overflow exposes no partial result");
}

static void test_framing_rejections(void) {
  struct fixture fixture;
  struct omni_http_request_body_result result;
  unsigned char te_raw[] = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
  unsigned char ambiguous_raw[] = "POST / HTTP/1.1\r\nContent-Length: 3\r\n"
                                  "Transfer-Encoding: chunked\r\n\r\nABC";
  unsigned char invalid_raw[] = "POST / HTTP/1.1\r\nContent-Length: 3x\r\n\r\nABC";
  unsigned char conflict_raw[] = "POST / HTTP/1.1\r\nContent-Length: 3\r\n"
                                 "Content-Length: 4\r\n\r\nABCD";

  check(parse_request(te_raw, sizeof(te_raw) - 1u, &fixture),
        "TE request parses through Tasks 035/036");
  check(fixture.framing.status == OMNI_HTTP_REQUEST_FRAMING_TRANSFER_ENCODING_UNSUPPORTED,
        "Task 036 marks TE unsupported");
  result = view_preserving_inputs(te_raw, sizeof(te_raw) - 1u, &fixture, te_raw,
                                  sizeof(te_raw) - 1u, "unsupported TE input remains unchanged");
  check(result.status == OMNI_HTTP_REQUEST_BODY_UNSUPPORTED_FRAMING,
        "Task 037 rejects chunked framing without decoding it");
  check(result.body.data == NULL && result.body.length == 0u && result.consumed_bytes == 0u,
        "unsupported framing exposes no body span");

  check(parse_request(ambiguous_raw, sizeof(ambiguous_raw) - 1u, &fixture),
        "ambiguous request head parses through Task 035");
  check(fixture.framing.status == OMNI_HTTP_REQUEST_FRAMING_AMBIGUOUS,
        "Task 036 identifies TE plus Content-Length ambiguity");
  result = view_preserving_inputs(ambiguous_raw, sizeof(ambiguous_raw) - 1u, &fixture,
                                  ambiguous_raw, sizeof(ambiguous_raw) - 1u,
                                  "ambiguous framing input remains unchanged");
  check(result.status == OMNI_HTTP_REQUEST_BODY_INVALID_FRAMING,
        "Task 037 rejects ambiguous framing");

  check(parse_request(invalid_raw, sizeof(invalid_raw) - 1u, &fixture),
        "malformed-length request head parses through Task 035");
  check(fixture.framing.status == OMNI_HTTP_REQUEST_FRAMING_INVALID_CONTENT_LENGTH,
        "Task 036 identifies malformed Content-Length");
  result = view_preserving_inputs(invalid_raw, sizeof(invalid_raw) - 1u, &fixture, invalid_raw,
                                  sizeof(invalid_raw) - 1u,
                                  "malformed framing input remains unchanged");
  check(result.status == OMNI_HTTP_REQUEST_BODY_INVALID_FRAMING,
        "Task 037 rejects malformed Content-Length framing");

  check(parse_request(conflict_raw, sizeof(conflict_raw) - 1u, &fixture),
        "conflicting-length request head parses through Task 035");
  check(fixture.framing.status == OMNI_HTTP_REQUEST_FRAMING_CONFLICTING_CONTENT_LENGTH,
        "Task 036 identifies conflicting Content-Length");
  result = view_preserving_inputs(conflict_raw, sizeof(conflict_raw) - 1u, &fixture, conflict_raw,
                                  sizeof(conflict_raw) - 1u,
                                  "conflicting framing input remains unchanged");
  check(result.status == OMNI_HTTP_REQUEST_BODY_INVALID_FRAMING,
        "Task 037 rejects conflicting Content-Length framing");
}

static void test_invalid_metadata_and_arguments(void) {
  unsigned char raw[] = "POST / HTTP/1.1\r\nContent-Length: 1\r\n\r\nX";
  struct fixture fixture;
  struct omni_http_request_body_result result;
  size_t raw_length = sizeof(raw) - 1u;
  struct omni_http_request_head_result bad_head;
  struct omni_http_request_framing_result bad_framing;

  check(parse_request(raw, raw_length, &fixture),
        "invalid-state request parses through Tasks 035/036");

  bad_head = fixture.head;
  bad_head.status = OMNI_HTTP_REQUEST_HEAD_INCOMPLETE;
  result = omni_http_request_body_view(raw, raw_length, &bad_head, &fixture.framing);
  check(result.status == OMNI_HTTP_REQUEST_BODY_INVALID_STATE, "non-COMPLETE head is rejected");

  bad_head = fixture.head;
  bad_head.consumed_bytes = raw_length + 1u;
  result = omni_http_request_body_view(raw, raw_length, &bad_head, &fixture.framing);
  check(result.status == OMNI_HTTP_REQUEST_BODY_INVALID_STATE,
        "head consumption beyond current input is rejected");

  bad_head = fixture.head;
  bad_head.header_count = OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS + 1u;
  result = omni_http_request_body_view(raw, raw_length, &bad_head, &fixture.framing);
  check(result.status == OMNI_HTTP_REQUEST_BODY_INVALID_STATE,
        "inconsistent Task 035 header count is rejected");

  bad_head = fixture.head;
  bad_head.request_line.consumed_bytes = bad_head.consumed_bytes + 1u;
  result = omni_http_request_body_view(raw, raw_length, &bad_head, &fixture.framing);
  check(result.status == OMNI_HTTP_REQUEST_BODY_INVALID_STATE,
        "request-line boundary beyond head boundary is rejected");

  bad_framing = fixture.framing;
  bad_framing.framing = (enum omni_http_request_body_framing)7;
  result = omni_http_request_body_view(raw, raw_length, &fixture.head, &bad_framing);
  check(result.status == OMNI_HTTP_REQUEST_BODY_INVALID_STATE,
        "unknown framing kind with OK status is rejected");

  bad_framing = fixture.framing;
  bad_framing.content_length_field_count = 0u;
  result = omni_http_request_body_view(raw, raw_length, &fixture.head, &bad_framing);
  check(result.status == OMNI_HTTP_REQUEST_BODY_INVALID_STATE,
        "FIXED_LENGTH without a Content-Length field count is rejected");

  bad_framing = fixture.framing;
  bad_framing.framing = OMNI_HTTP_REQUEST_BODY_NONE;
  result = omni_http_request_body_view(raw, raw_length, &fixture.head, &bad_framing);
  check(result.status == OMNI_HTTP_REQUEST_BODY_INVALID_STATE,
        "NO_BODY with Content-Length metadata is rejected");

  bad_framing = fixture.framing;
  bad_framing.transfer_encoding_present = true;
  result = omni_http_request_body_view(raw, raw_length, &fixture.head, &bad_framing);
  check(result.status == OMNI_HTTP_REQUEST_BODY_INVALID_STATE,
        "FIXED_LENGTH combined with Transfer-Encoding metadata is rejected");

  result = omni_http_request_body_view(NULL, raw_length, &fixture.head, &fixture.framing);
  check(result.status == OMNI_HTTP_REQUEST_BODY_INVALID_ARGUMENT,
        "NULL input with nonzero length is invalid");
  result = omni_http_request_body_view(raw, raw_length, NULL, &fixture.framing);
  check(result.status == OMNI_HTTP_REQUEST_BODY_INVALID_ARGUMENT, "NULL head is invalid");
  result = omni_http_request_body_view(raw, raw_length, &fixture.head, NULL);
  check(result.status == OMNI_HTTP_REQUEST_BODY_INVALID_ARGUMENT, "NULL framing is invalid");
  result = omni_http_request_body_view(NULL, 0u, &fixture.head, &fixture.framing);
  check(result.status == OMNI_HTTP_REQUEST_BODY_INVALID_STATE,
        "NULL plus zero cannot satisfy the complete parsed-head boundary");

  bad_head = fixture.head;
  bad_head.headers = NULL;
  result = omni_http_request_body_view(raw, raw_length, &bad_head, &fixture.framing);
  check(result.status == OMNI_HTTP_REQUEST_BODY_INVALID_STATE,
        "nonzero Task 035 header count with NULL storage is rejected");

  bad_head = fixture.head;
  bad_head.request_line.target.data = NULL;
  result = omni_http_request_body_view(raw, raw_length, &bad_head, &fixture.framing);
  check(result.status == OMNI_HTTP_REQUEST_BODY_INVALID_STATE,
        "inconsistent Task 035 request-line span metadata is rejected");
}

int main(void) {
  test_no_body_and_trailing();
  test_fixed_zero();
  test_exact_and_binary_body();
  test_all_prefixes();
  test_trailing_and_pipeline();
  test_all_byte_values();
  test_large_metadata_and_overflow();
  test_framing_rejections();
  test_invalid_metadata_and_arguments();

  (void)printf("http-request-body-unit: %zu checks, %zu failures\n", checks, failures);
  (void)printf("sizeof(omni_http_request_body_span)=%zu; "
               "sizeof(omni_http_request_body_result)=%zu; persistent body-view state=0 bytes\n",
               sizeof(struct omni_http_request_body_span),
               sizeof(struct omni_http_request_body_result));
  return failures == 0u ? 0 : 1;
}
