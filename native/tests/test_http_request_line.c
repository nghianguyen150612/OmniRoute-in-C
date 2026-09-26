/*
 * OmniRoute native backend — bounded HTTP request-line tests (Task 033).
 *
 * Deterministic, framework-free checks for grammar, incremental input,
 * limits, borrowed spans, binary safety, and parser immutability. The parser
 * itself remains allocation-free; exact-size prefix allocations here let
 * ASan detect reads beyond the supplied span.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omniroute/http_request_line.h"

static size_t check_count = 0u;
static size_t failure_count = 0u;

static void check(bool condition, const char *name) {
  ++check_count;
  if (!condition) {
    ++failure_count;
    fprintf(stderr, "NOT OK - %s\n", name);
  }
}

static bool is_test_token(unsigned char byte) {
  if ((byte >= (unsigned char)'A' && byte <= (unsigned char)'Z') ||
      (byte >= (unsigned char)'a' && byte <= (unsigned char)'z') ||
      (byte >= (unsigned char)'0' && byte <= (unsigned char)'9')) {
    return true;
  }
  return byte == (unsigned char)'!' || byte == (unsigned char)'#' ||
         byte == (unsigned char)'$' || byte == (unsigned char)'%' ||
         byte == (unsigned char)'&' || byte == (unsigned char)'\'' ||
         byte == (unsigned char)'*' || byte == (unsigned char)'+' ||
         byte == (unsigned char)'-' || byte == (unsigned char)'.' ||
         byte == (unsigned char)'^' || byte == (unsigned char)'_' ||
         byte == (unsigned char)'`' || byte == (unsigned char)'|' ||
         byte == (unsigned char)'~';
}

static bool has_no_success_fields(struct omni_http_request_line_result result) {
  return result.method.data == NULL && result.method.length == 0u &&
         result.target.data == NULL && result.target.length == 0u &&
         result.version == OMNI_HTTP_VERSION_UNKNOWN && result.consumed_bytes == 0u;
}

static struct omni_http_request_line_result expect_status(
    const unsigned char *data, size_t length,
    enum omni_http_request_line_status expected, const char *name) {
  struct omni_http_request_line_result result =
      omni_http_request_line_parse(data, length);

  check(result.status == expected, name);
  if (expected != OMNI_HTTP_REQUEST_LINE_COMPLETE) {
    check(has_no_success_fields(result), "non-complete result exposes no successful spans");
  }
  return result;
}

static size_t make_line(unsigned char *out,
                        const unsigned char *method, size_t method_length,
                        const unsigned char *target, size_t target_length,
                        const unsigned char *version, size_t version_length) {
  size_t length = 0u;

  if (method_length > 0u) {
    memcpy(out + length, method, method_length);
    length += method_length;
  }
  out[length++] = (unsigned char)' ';
  if (target_length > 0u) {
    memcpy(out + length, target, target_length);
    length += target_length;
  }
  out[length++] = (unsigned char)' ';
  if (version_length > 0u) {
    memcpy(out + length, version, version_length);
    length += version_length;
  }
  out[length++] = (unsigned char)'\r';
  out[length++] = (unsigned char)'\n';
  return length;
}

static struct omni_http_request_line_result parse_and_check_unchanged(
    const unsigned char *data, size_t length, const char *name) {
  unsigned char snapshot[OMNI_HTTP_REQUEST_LINE_MAX_TOTAL_BYTES + 1u];
  struct omni_http_request_line_result result;

  if (data == NULL || length > sizeof(snapshot)) {
    check(false, "immutability fixture fits fixed snapshot");
    return omni_http_request_line_parse(data, length);
  }
  if (length > 0u) memcpy(snapshot, data, length);
  result = omni_http_request_line_parse(data, length);
  check(length == 0u || memcmp(snapshot, data, length) == 0, name);
  return result;
}

static void test_valid_lines_and_borrowed_spans(void) {
  static const unsigned char get[] = "GET";
  static const unsigned char post[] = "POST";
  static const unsigned char options[] = "OPTIONS";
  static const unsigned char patch[] = "PATCH";
  static const unsigned char custom[] = "CUSTOM";
  static const unsigned char slash[] = "/";
  static const unsigned char models[] = "/v1/models";
  static const unsigned char chat[] = "/v1/chat/completions";
  static const unsigned char star[] = "*";
  static const unsigned char query[] = "/abc?x=1";
  static const unsigned char absolute[] = "http://example.test/x";
  static const unsigned char version_10[] = "HTTP/1.0";
  static const unsigned char version_11[] = "HTTP/1.1";
  unsigned char line[OMNI_HTTP_REQUEST_LINE_MAX_TOTAL_BYTES + 1u];
  unsigned char snapshot[OMNI_HTTP_REQUEST_LINE_MAX_TOTAL_BYTES + 1u];
  size_t length;
  struct omni_http_request_line_result result;

#define CHECK_VALID(method_, target_, version_, version_enum_, label_) do { \
    length = make_line(line, (method_), sizeof(method_) - 1u, (target_), \
                       sizeof(target_) - 1u, (version_), sizeof(version_) - 1u); \
    memcpy(snapshot, line, length); \
    result = omni_http_request_line_parse(line, length); \
    check(result.status == OMNI_HTTP_REQUEST_LINE_COMPLETE, (label_)); \
    check(result.method.data == line && \
              result.method.length == sizeof(method_) - 1u && \
              memcmp(result.method.data, (method_), result.method.length) == 0, \
          "method is an exact borrowed span"); \
    check(result.target.data == line + sizeof(method_) && \
              result.target.length == sizeof(target_) - 1u && \
              memcmp(result.target.data, (target_), result.target.length) == 0, \
          "target is an exact borrowed span"); \
    check(result.version == (version_enum_), "version enum matches request line"); \
    check(result.consumed_bytes == length, "consumed length ends exactly after CRLF"); \
    check(result.error_offset == 0u, "complete result has zero error offset"); \
    check(memcmp(snapshot, line, length) == 0, "successful parse leaves input unchanged"); \
  } while (0)

  CHECK_VALID(get, slash, version_11, OMNI_HTTP_VERSION_1_1, "GET slash HTTP/1.1 parses");
  CHECK_VALID(get, models, version_11, OMNI_HTTP_VERSION_1_1,
              "GET models HTTP/1.1 parses");
  CHECK_VALID(post, chat, version_11, OMNI_HTTP_VERSION_1_1,
              "POST chat path HTTP/1.1 parses");
  CHECK_VALID(options, star, version_11, OMNI_HTTP_VERSION_1_1,
              "OPTIONS asterisk target HTTP/1.1 parses");
  CHECK_VALID(patch, query, version_11, OMNI_HTTP_VERSION_1_1,
              "PATCH query target HTTP/1.1 parses");
  CHECK_VALID(get, slash, version_10, OMNI_HTTP_VERSION_1_0,
              "GET slash HTTP/1.0 parses");
  CHECK_VALID(custom, absolute, version_11, OMNI_HTTP_VERSION_1_1,
              "generic method and absolute target parse");

#undef CHECK_VALID
}

static void test_extra_bytes_are_ignored(void) {
  static const unsigned char input[] = "GET / HTTP/1.1\r\nHost: example\r\n";
  static const size_t request_length = sizeof("GET / HTTP/1.1\r\n") - 1u;
  struct omni_http_request_line_result result =
      parse_and_check_unchanged(input, sizeof(input) - 1u,
                                "parse with following header bytes leaves input unchanged");

  check(result.status == OMNI_HTTP_REQUEST_LINE_COMPLETE,
        "extra bytes after CRLF do not prevent completion");
  check(result.consumed_bytes == request_length,
        "extra bytes are not included in consumed request-line length");
  check(result.target.length == 1u && result.target.data[0] == (unsigned char)'/',
        "parser does not inspect or expose following header bytes");
}

static void test_incremental_prefixes(void) {
  static const unsigned char first[] = "GET /v1/models HTTP/1.1\r\n";
  static const unsigned char second[] = "OPTIONS * HTTP/1.0\r\n";
  static const unsigned char third[] = "PATCH /abc?x=1 HTTP/1.1\r\n";
  const unsigned char *const lines[] = {first, second, third};
  const size_t lengths[] = {sizeof(first) - 1u, sizeof(second) - 1u,
                            sizeof(third) - 1u};

  check(expect_status(NULL, 0u, OMNI_HTTP_REQUEST_LINE_INCOMPLETE,
                      "NULL empty span is incomplete").error_offset == 0u,
        "empty span requests input at offset zero");

  for (size_t line_index = 0u; line_index < sizeof(lengths) / sizeof(lengths[0]);
       ++line_index) {
    for (size_t prefix_length = 0u; prefix_length <= lengths[line_index];
         ++prefix_length) {
      struct omni_http_request_line_result result;
      unsigned char *exact_prefix = NULL;
      const enum omni_http_request_line_status expected =
          prefix_length == lengths[line_index]
              ? OMNI_HTTP_REQUEST_LINE_COMPLETE
              : OMNI_HTTP_REQUEST_LINE_INCOMPLETE;

      if (prefix_length == 0u) {
        result = omni_http_request_line_parse(NULL, 0u);
      } else {
        exact_prefix = (unsigned char *)malloc(prefix_length);
        check(exact_prefix != NULL, "allocate exact-size incremental prefix fixture");
        if (exact_prefix == NULL) return;
        memcpy(exact_prefix, lines[line_index], prefix_length);
        result = omni_http_request_line_parse(exact_prefix, prefix_length);
      }

      check(result.status == expected, "every viable request-line prefix has exact status");
      if (expected == OMNI_HTTP_REQUEST_LINE_INCOMPLETE) {
        check(has_no_success_fields(result), "incomplete prefix exposes no partial spans");
        check(result.consumed_bytes == 0u, "incomplete prefix consumes no bytes");
      } else {
        check(result.consumed_bytes == lengths[line_index],
              "final request-line prefix consumes all bytes");
      }
      free(exact_prefix);
    }
  }
}

static void test_invalid_spacing_and_line_endings(void) {
  static const unsigned char leading_space[] = " GET / HTTP/1.1\r\n";
  static const unsigned char empty_method[] = " / HTTP/1.1\r\n";
  static const unsigned char double_after_method[] = "GET  / HTTP/1.1\r\n";
  static const unsigned char empty_target[] = "GET  HTTP/1.1\r\n";
  static const unsigned char double_after_target[] = "GET /  HTTP/1.1\r\n";
  static const unsigned char trailing_space[] = "GET / HTTP/1.1 \r\n";
  static const unsigned char tab_separator[] = "GET\t/ HTTP/1.1\r\n";
  static const unsigned char bare_lf[] = "GET / HTTP/1.1\n";
  static const unsigned char extra_cr[] = "GET / HTTP/1.1\r\r\n";
  static const unsigned char lone_cr[] = "GET / HTTP/1.1\r";
  struct omni_http_request_line_result result;

  result = expect_status(leading_space, sizeof(leading_space) - 1u,
                         OMNI_HTTP_REQUEST_LINE_INVALID, "leading SP is invalid");
  check(result.error_offset == 0u, "leading SP error offset is zero");
  result = expect_status(empty_method, sizeof(empty_method) - 1u,
                         OMNI_HTTP_REQUEST_LINE_INVALID, "empty method is invalid");
  check(result.error_offset == 0u, "empty method error points to its delimiter");
  result = expect_status(double_after_method, sizeof(double_after_method) - 1u,
                         OMNI_HTTP_REQUEST_LINE_INVALID, "double SP after method is invalid");
  check(result.error_offset == 4u, "empty target error points to second SP");
  expect_status(empty_target, sizeof(empty_target) - 1u,
                OMNI_HTTP_REQUEST_LINE_INVALID, "empty request target is invalid");
  result = expect_status(double_after_target, sizeof(double_after_target) - 1u,
                         OMNI_HTTP_REQUEST_LINE_INVALID, "double SP before version is invalid");
  check(result.error_offset == 6u, "version spacing error points at unexpected SP");
  expect_status(trailing_space, sizeof(trailing_space) - 1u,
                OMNI_HTTP_REQUEST_LINE_INVALID, "trailing SP before CRLF is invalid");
  expect_status(tab_separator, sizeof(tab_separator) - 1u,
                OMNI_HTTP_REQUEST_LINE_INVALID, "HTAB is not a request-line separator");
  expect_status(bare_lf, sizeof(bare_lf) - 1u,
                OMNI_HTTP_REQUEST_LINE_INVALID, "bare LF is invalid");
  expect_status(extra_cr, sizeof(extra_cr) - 1u,
                OMNI_HTTP_REQUEST_LINE_INVALID, "extra CR is invalid");
  result = expect_status(lone_cr, sizeof(lone_cr) - 1u,
                         OMNI_HTTP_REQUEST_LINE_INCOMPLETE, "trailing CR is incomplete");
  check(result.error_offset == sizeof(lone_cr) - 1u,
        "trailing CR requests LF at the next offset");
}

static void test_method_and_target_validation(void) {
  static const unsigned char suffix[] = " / HTTP/1.1\r\n";
  static const unsigned char target_suffix[] = " HTTP/1.1\r\n";
  static const unsigned char invalid_method_bytes[] = {
      '(', ')', '<', '>', '@', ',', ';', ':', '\\', '"', '/', '[', ']', '?', '=', '{', '}',
      '\t', '\r', '\n', 0x00u, 0x01u, 0x7fu, 0x80u, 0xffu};
  unsigned char line[OMNI_HTTP_REQUEST_LINE_MAX_TOTAL_BYTES + 1u];
  unsigned char method[OMNI_HTTP_REQUEST_LINE_MAX_METHOD_BYTES + 1u];
  unsigned char target[OMNI_HTTP_REQUEST_LINE_MAX_TARGET_BYTES + 1u];
  static const unsigned char target_middle_bad_bytes[] = {
      0x00u, 0x01u, 0x7fu, 0x80u, 0xffu};
  size_t length;

  for (size_t index = 0u;
       index < sizeof(invalid_method_bytes) / sizeof(invalid_method_bytes[0]);
       ++index) {
    line[0] = invalid_method_bytes[index];
    memcpy(line + 1u, suffix, sizeof(suffix) - 1u);
    length = 1u + sizeof(suffix) - 1u;
    expect_status(line, length, OMNI_HTTP_REQUEST_LINE_INVALID,
                  "forbidden method byte is rejected");
  }

  for (size_t index = 0u; index < sizeof(method); ++index) {
    method[index] = (unsigned char)'A';
  }
  length = make_line(line, method, OMNI_HTTP_REQUEST_LINE_MAX_METHOD_BYTES - 1u,
                     (const unsigned char *)"/", 1u,
                     (const unsigned char *)"HTTP/1.1", 8u);
  expect_status(line, length, OMNI_HTTP_REQUEST_LINE_COMPLETE,
                "method limit minus one is accepted");
  length = make_line(line, method, OMNI_HTTP_REQUEST_LINE_MAX_METHOD_BYTES,
                     (const unsigned char *)"/", 1u,
                     (const unsigned char *)"HTTP/1.1", 8u);
  expect_status(line, length, OMNI_HTTP_REQUEST_LINE_COMPLETE,
                "method at the limit is accepted");
  length = make_line(line, method, OMNI_HTTP_REQUEST_LINE_MAX_METHOD_BYTES + 1u,
                     (const unsigned char *)"/", 1u,
                     (const unsigned char *)"HTTP/1.1", 8u);
  {
    struct omni_http_request_line_result result =
        expect_status(line, length, OMNI_HTTP_REQUEST_LINE_TOO_LARGE,
                      "method over the limit is too large");
    check(result.error_offset == OMNI_HTTP_REQUEST_LINE_MAX_METHOD_BYTES,
          "method too-large offset is the first excess byte");
  }

  for (size_t index = 0u; index < sizeof(target); ++index) {
    target[index] = (unsigned char)'x';
  }
  length = make_line(line, (const unsigned char *)"ABCDEFGHIJKLMNOPQRSTUVWX", 24u,
                     target, OMNI_HTTP_REQUEST_LINE_MAX_TARGET_BYTES - 1u,
                     (const unsigned char *)"HTTP/1.1", 8u);
  expect_status(line, length, OMNI_HTTP_REQUEST_LINE_COMPLETE,
                "target limit minus one is accepted");
  length = make_line(line, (const unsigned char *)"ABCDEFGHIJKLMNOPQRSTUVWX", 24u,
                     target, OMNI_HTTP_REQUEST_LINE_MAX_TARGET_BYTES,
                     (const unsigned char *)"HTTP/1.1", 8u);
  expect_status(line, length, OMNI_HTTP_REQUEST_LINE_COMPLETE,
                "target at the limit is accepted");
  length = make_line(line, (const unsigned char *)"ABCDEFGHIJKLMNOPQRSTUVWX", 24u,
                     target, OMNI_HTTP_REQUEST_LINE_MAX_TARGET_BYTES + 1u,
                     (const unsigned char *)"HTTP/1.1", 8u);
  {
    struct omni_http_request_line_result result =
        expect_status(line, length, OMNI_HTTP_REQUEST_LINE_TOO_LARGE,
                      "target over the limit is too large");
    check(result.error_offset == 24u + 1u + OMNI_HTTP_REQUEST_LINE_MAX_TARGET_BYTES,
          "target too-large offset is the first excess byte");
  }

  line[0] = (unsigned char)'G';
  line[1] = (unsigned char)'E';
  line[2] = (unsigned char)'T';
  line[3] = (unsigned char)' ';
  line[4] = (unsigned char)'/';
  line[5] = (unsigned char)'a';
  line[6] = (unsigned char)' ';
  memcpy(line + 7u, target_suffix, sizeof(target_suffix) - 1u);
  length = 7u + sizeof(target_suffix) - 1u;
  expect_status(line, length, OMNI_HTTP_REQUEST_LINE_INVALID,
                "embedded target SP is not accepted as target data");

  memcpy(line, "GET /ab HTTP/1.1\r\n", sizeof("GET /ab HTTP/1.1\r\n") - 1u);
  length = sizeof("GET /ab HTTP/1.1\r\n") - 1u;
  for (size_t index = 0u;
       index < sizeof(target_middle_bad_bytes) / sizeof(target_middle_bad_bytes[0]);
       ++index) {
    line[6] = target_middle_bad_bytes[index];
    expect_status(line, length, OMNI_HTTP_REQUEST_LINE_INVALID,
                  "binary and control bytes inside target are invalid");
  }
}

static void test_version_validation(void) {
  static const unsigned char get[] = "GET";
  static const unsigned char target[] = "/";
  static const unsigned char supported_10[] = "HTTP/1.0";
  static const unsigned char supported_11[] = "HTTP/1.1";
  static const unsigned char unsupported_12[] = "HTTP/1.2";
  static const unsigned char unsupported_20[] = "HTTP/2.0";
  static const unsigned char malformed_2[] = "HTTP/2";
  static const unsigned char malformed_3[] = "HTTP/3";
  static const unsigned char malformed_minor[] = "HTTP/1.a";
  static const unsigned char extra_digit[] = "HTTP/1.10";
  unsigned char line[64];
  static const unsigned char base_version[8] = {'H', 'T', 'T', 'P', '/', '1', '.', '1'};
  size_t length;
  struct omni_http_request_line_result result;

  length = make_line(line, get, sizeof(get) - 1u, target, sizeof(target) - 1u,
                     supported_10, sizeof(supported_10) - 1u);
  expect_status(line, length, OMNI_HTTP_REQUEST_LINE_COMPLETE, "HTTP/1.0 is supported");
  length = make_line(line, get, sizeof(get) - 1u, target, sizeof(target) - 1u,
                     supported_11, sizeof(supported_11) - 1u);
  expect_status(line, length, OMNI_HTTP_REQUEST_LINE_COMPLETE, "HTTP/1.1 is supported");
  length = make_line(line, get, sizeof(get) - 1u, target, sizeof(target) - 1u,
                     unsupported_12, sizeof(unsupported_12) - 1u);
  result = expect_status(line, length, OMNI_HTTP_REQUEST_LINE_UNSUPPORTED_VERSION,
                         "HTTP/1.2 is syntactically shaped but unsupported");
  check(result.error_offset == 11u, "unsupported version points at major digit");
  length = make_line(line, get, sizeof(get) - 1u, target, sizeof(target) - 1u,
                     unsupported_20, sizeof(unsupported_20) - 1u);
  expect_status(line, length, OMNI_HTTP_REQUEST_LINE_UNSUPPORTED_VERSION,
                "HTTP/2.0 is unsupported");
  length = make_line(line, get, sizeof(get) - 1u, target, sizeof(target) - 1u,
                     malformed_2, sizeof(malformed_2) - 1u);
  expect_status(line, length, OMNI_HTTP_REQUEST_LINE_INVALID,
                "HTTP/2 without minor version is malformed");
  length = make_line(line, get, sizeof(get) - 1u, target, sizeof(target) - 1u,
                     malformed_3, sizeof(malformed_3) - 1u);
  expect_status(line, length, OMNI_HTTP_REQUEST_LINE_INVALID,
                "HTTP/3 without minor version is malformed");
  length = make_line(line, get, sizeof(get) - 1u, target, sizeof(target) - 1u,
                     malformed_minor, sizeof(malformed_minor) - 1u);
  expect_status(line, length, OMNI_HTTP_REQUEST_LINE_INVALID,
                "non-digit minor version is invalid");
  length = make_line(line, get, sizeof(get) - 1u, target, sizeof(target) - 1u,
                     extra_digit, sizeof(extra_digit) - 1u);
  expect_status(line, length, OMNI_HTTP_REQUEST_LINE_INVALID,
                "extra version digit is invalid");

  for (size_t position = 0u; position < sizeof(base_version); ++position) {
    for (unsigned int value = 0u; value <= 255u; ++value) {
      enum omni_http_request_line_status expected;
      unsigned char candidate[sizeof(base_version)];

      memcpy(candidate, base_version, sizeof(candidate));
      candidate[position] = (unsigned char)value;
      length = make_line(line, get, sizeof(get) - 1u, target, sizeof(target) - 1u,
                         candidate, sizeof(candidate));

      if (position == 5u || position == 7u) {
        if (value < (unsigned int)'0' || value > (unsigned int)'9') {
          expected = OMNI_HTTP_REQUEST_LINE_INVALID;
        } else if (position == 5u && value != (unsigned int)'1') {
          expected = OMNI_HTTP_REQUEST_LINE_UNSUPPORTED_VERSION;
        } else if (position == 7u && value > (unsigned int)'1') {
          expected = OMNI_HTTP_REQUEST_LINE_UNSUPPORTED_VERSION;
        } else {
          expected = OMNI_HTTP_REQUEST_LINE_COMPLETE;
        }
      } else {
        static const unsigned char fixed_version[8] = {
            'H', 'T', 'T', 'P', '/', '1', '.', '1'};
        expected = candidate[position] == fixed_version[position]
                       ? OMNI_HTTP_REQUEST_LINE_COMPLETE
                       : OMNI_HTTP_REQUEST_LINE_INVALID;
      }

      result = omni_http_request_line_parse(line, length);
      check(result.status == expected,
            "all 256 values at each version byte match ASCII grammar");
    }
  }
}

static void test_exhaustive_method_and_target_bytes(void) {
  unsigned char method_line[sizeof("X / HTTP/1.1\r\n") - 1u];
  unsigned char target_line[sizeof("GET X HTTP/1.1\r\n") - 1u];

  memcpy(method_line, "X / HTTP/1.1\r\n", sizeof(method_line));
  memcpy(target_line, "GET X HTTP/1.1\r\n", sizeof(target_line));
  for (unsigned int value = 0u; value <= 255u; ++value) {
    const enum omni_http_request_line_status expected =
        is_test_token((unsigned char)value)
            ? OMNI_HTTP_REQUEST_LINE_COMPLETE
            : OMNI_HTTP_REQUEST_LINE_INVALID;
    struct omni_http_request_line_result result;

    method_line[0] = (unsigned char)value;
    result = omni_http_request_line_parse(method_line, sizeof(method_line));
    check(result.status == expected,
          "all 256 values in method position match HTTP token syntax");

    target_line[4] = (unsigned char)value;
    result = omni_http_request_line_parse(target_line, sizeof(target_line));
    check(result.status == (value >= 0x21u && value <= 0x7eu
                                ? OMNI_HTTP_REQUEST_LINE_COMPLETE
                                : OMNI_HTTP_REQUEST_LINE_INVALID),
          "all 256 values in target position match visible ASCII syntax");
  }
}

static void test_total_line_limit(void) {
  unsigned char method[25];
  unsigned char target[OMNI_HTTP_REQUEST_LINE_MAX_TARGET_BYTES];
  unsigned char line[OMNI_HTTP_REQUEST_LINE_MAX_TOTAL_BYTES + 1u];
  size_t length;
  struct omni_http_request_line_result result;

  for (size_t i = 0u; i < sizeof(method); ++i) method[i] = (unsigned char)'M';
  for (size_t i = 0u; i < sizeof(target); ++i) target[i] = (unsigned char)'x';

  length = make_line(line, method, 24u, target, sizeof(target),
                     (const unsigned char *)"HTTP/1.1", 8u);
  check(length == OMNI_HTTP_REQUEST_LINE_MAX_TOTAL_BYTES,
        "boundary fixture has exactly the total line limit bytes");
  result = expect_status(line, length - 1u, OMNI_HTTP_REQUEST_LINE_INCOMPLETE,
                         "total limit minus one remains an incomplete CR prefix");
  check(result.error_offset == length - 1u,
        "incomplete total-limit prefix requests its final LF");
  result = expect_status(line, length, OMNI_HTTP_REQUEST_LINE_COMPLETE,
                         "request line exactly at total limit is accepted");
  check(result.consumed_bytes == OMNI_HTTP_REQUEST_LINE_MAX_TOTAL_BYTES,
        "exact total-limit line consumes the full bound");

  line[length] = (unsigned char)'X';
  result = expect_status(line, length + 1u, OMNI_HTTP_REQUEST_LINE_COMPLETE,
                         "one extra byte after bounded CRLF is ignored");
  check(result.consumed_bytes == OMNI_HTTP_REQUEST_LINE_MAX_TOTAL_BYTES,
        "extra byte does not extend the consumed request line");

  length = make_line(line, method, sizeof(method), target, sizeof(target),
                     (const unsigned char *)"HTTP/1.1", 8u);
  check(length == OMNI_HTTP_REQUEST_LINE_MAX_TOTAL_BYTES + 1u,
        "combined-limit fixture exceeds total cap by one byte");
  result = expect_status(line, length, OMNI_HTTP_REQUEST_LINE_TOO_LARGE,
                         "viable request line beyond total cap is too large");
  check(result.error_offset == OMNI_HTTP_REQUEST_LINE_MAX_TOTAL_BYTES,
        "total too-large offset is the first byte beyond the cap");
}

static void test_null_arguments_and_failure_offsets(void) {
  static const unsigned char one_byte[] = {'G'};
  static const unsigned char invalid[] = "GET / HTTP/1.1\n";
  struct omni_http_request_line_result result;

  result = expect_status(NULL, 1u, OMNI_HTTP_REQUEST_LINE_ERR_INVALID_ARGUMENT,
                         "NULL with nonzero length is invalid argument");
  check(result.error_offset == 0u, "invalid-argument offset is zero");
  result = expect_status(one_byte, 0u, OMNI_HTTP_REQUEST_LINE_INCOMPLETE,
                         "non-NULL zero-length span is incomplete");
  check(result.error_offset == 0u, "zero-length span needs data at offset zero");
  result = expect_status(invalid, sizeof(invalid) - 1u,
                         OMNI_HTTP_REQUEST_LINE_INVALID,
                         "malformed syntax result has stable status");
  check(result.error_offset == sizeof(invalid) - 2u,
        "bare LF error points to LF byte");
}

static void test_failure_input_immutability(void) {
  unsigned char invalid[] = "GET / HTTP/1.1\n";
  unsigned char incomplete[] = "GET / HTTP/1.1\r";
  unsigned char unsupported[] = "GET / HTTP/2.0\r\n";
  unsigned char method[OMNI_HTTP_REQUEST_LINE_MAX_METHOD_BYTES + 1u];
  unsigned char oversized[OMNI_HTTP_REQUEST_LINE_MAX_TOTAL_BYTES + 1u];
  size_t length;
  struct omni_http_request_line_result result;

  result = parse_and_check_unchanged(invalid, sizeof(invalid) - 1u,
                                     "invalid parse leaves bytes unchanged");
  check(result.status == OMNI_HTTP_REQUEST_LINE_INVALID,
        "invalid immutability fixture is rejected");
  result = parse_and_check_unchanged(incomplete, sizeof(incomplete) - 1u,
                                     "incomplete parse leaves bytes unchanged");
  check(result.status == OMNI_HTTP_REQUEST_LINE_INCOMPLETE,
        "incomplete immutability fixture stays incomplete");
  result = parse_and_check_unchanged(unsupported, sizeof(unsupported) - 1u,
                                     "unsupported-version parse leaves bytes unchanged");
  check(result.status == OMNI_HTTP_REQUEST_LINE_UNSUPPORTED_VERSION,
        "unsupported immutability fixture is classified");

  for (size_t i = 0u; i < sizeof(method); ++i) method[i] = (unsigned char)'A';
  length = make_line(oversized, method, sizeof(method),
                     (const unsigned char *)"/", 1u,
                     (const unsigned char *)"HTTP/1.1", 8u);
  result = parse_and_check_unchanged(oversized, length,
                                     "too-large parse leaves bytes unchanged");
  check(result.status == OMNI_HTTP_REQUEST_LINE_TOO_LARGE,
        "too-large immutability fixture is rejected");
}

int main(void) {
  test_valid_lines_and_borrowed_spans();
  test_extra_bytes_are_ignored();
  test_incremental_prefixes();
  test_invalid_spacing_and_line_endings();
  test_method_and_target_validation();
  test_version_validation();
  test_exhaustive_method_and_target_bytes();
  test_total_line_limit();
  test_null_arguments_and_failure_offsets();
  test_failure_input_immutability();

  printf("---\n%zu checks, %zu failures\n", check_count, failure_count);
  printf("sizeof(omni_http_byte_span)=%zu; sizeof(omni_http_request_line_result)=%zu; "
         "persistent parser state=0 bytes\n",
         sizeof(struct omni_http_byte_span),
         sizeof(struct omni_http_request_line_result));
  return failure_count == 0u ? 0 : 1;
}
