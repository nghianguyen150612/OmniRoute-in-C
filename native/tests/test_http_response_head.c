/*
 * OmniRoute native backend — bounded HTTP/1.1 response-head tests (Task 041).
 *
 * The suite is deterministic and framework-free. It verifies exact wire
 * bytes, bounded metadata validation, checked sizing, all-or-nothing output,
 * borrowed-input immutability, byte-classification policy, and stress reuse.
 */

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "omniroute/http_response_head.h"

static size_t check_count = 0u;
static size_t failure_count = 0u;

static void check(bool condition, const char *name) {
  ++check_count;
  if (!condition) {
    ++failure_count;
    fprintf(stderr, "NOT OK - %s\n", name);
  }
}

static struct omni_http_response_header make_header(const char *name,
                                                    const char *value) {
  struct omni_http_response_header header = {0};

  header.name = (const unsigned char *)name;
  header.name_length = strlen(name);
  header.value = (const unsigned char *)value;
  header.value_length = strlen(value);
  return header;
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

static bool is_test_field_byte(unsigned char byte) {
  return byte == (unsigned char)'\t' || (byte >= 0x20u && byte <= 0x7eu);
}

static void fill_canary(unsigned char *output, size_t length,
                        unsigned char value) {
  size_t index;

  for (index = 0u; index < length; ++index)
    output[index] = value;
}

static void expect_wire(unsigned int status_code, const char *reason,
                        const struct omni_http_response_header *headers,
                        size_t header_count, const unsigned char *expected,
                        size_t expected_length, const char *name) {
  unsigned char output[OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES + 8u];
  struct omni_http_response_head_result result;

  fill_canary(output, sizeof(output), 0xa5u);
  result = omni_http_response_head_build(
      status_code, (const unsigned char *)reason, strlen(reason), headers,
      header_count, output, sizeof(output));
  check(result.status == OMNI_HTTP_RESPONSE_HEAD_OK, name);
  check(result.written_bytes == expected_length &&
            result.required_bytes == expected_length,
        "successful result reports exact byte counts");
  check(memcmp(output, expected, expected_length) == 0,
        "successful output bytes are exact");
  check(output[expected_length] == 0xa5u,
        "successful output does not overwrite the tail");
  check(memchr(output, '\0', expected_length) == NULL,
        "successful output has no trailing NUL in its written range");
}

static void expect_failure_unchanged(
    unsigned int status_code, const unsigned char *reason, size_t reason_length,
    const struct omni_http_response_header *headers, size_t header_count,
    enum omni_http_response_head_status expected_status,
    size_t expected_required, const char *name) {
  unsigned char output[256];
  unsigned char snapshot[sizeof(output)];
  struct omni_http_response_head_result result;

  fill_canary(output, sizeof(output), 0xc3u);
  memcpy(snapshot, output, sizeof(output));
  result =
      omni_http_response_head_build(status_code, reason, reason_length, headers,
                                    header_count, output, sizeof(output));
  check(result.status == expected_status, name);
  check(result.written_bytes == 0u &&
            result.required_bytes == expected_required,
        "failure result reports zero writes and expected required bytes");
  check(memcmp(output, snapshot, sizeof(output)) == 0,
        "failure leaves every output byte unchanged");
}

static void test_zero_headers_and_status_fixtures(void) {
  static const struct {
    unsigned int status_code;
    const char *reason;
    const unsigned char *expected;
    size_t expected_length;
  } fixtures[] = {
      {100u, "Continue", (const unsigned char *)"HTTP/1.1 100 Continue\r\n\r\n",
       sizeof("HTTP/1.1 100 Continue\r\n\r\n") - 1u},
      {200u, "OK", (const unsigned char *)"HTTP/1.1 200 OK\r\n\r\n",
       sizeof("HTTP/1.1 200 OK\r\n\r\n") - 1u},
      {204u, "No Content",
       (const unsigned char *)"HTTP/1.1 204 No Content\r\n\r\n",
       sizeof("HTTP/1.1 204 No Content\r\n\r\n") - 1u},
      {400u, "Bad Request",
       (const unsigned char *)"HTTP/1.1 400 Bad Request\r\n\r\n",
       sizeof("HTTP/1.1 400 Bad Request\r\n\r\n") - 1u},
      {404u, "Not Found",
       (const unsigned char *)"HTTP/1.1 404 Not Found\r\n\r\n",
       sizeof("HTTP/1.1 404 Not Found\r\n\r\n") - 1u},
      {405u, "Method Not Allowed",
       (const unsigned char *)"HTTP/1.1 405 Method Not Allowed\r\n\r\n",
       sizeof("HTTP/1.1 405 Method Not Allowed\r\n\r\n") - 1u},
      {500u, "Internal Server Error",
       (const unsigned char *)"HTTP/1.1 500 Internal Server Error\r\n\r\n",
       sizeof("HTTP/1.1 500 Internal Server Error\r\n\r\n") - 1u},
      {999u, "Custom", (const unsigned char *)"HTTP/1.1 999 Custom\r\n\r\n",
       sizeof("HTTP/1.1 999 Custom\r\n\r\n") - 1u},
  };
  size_t index;
  struct omni_http_response_head_result measured;

  for (index = 0u; index < sizeof(fixtures) / sizeof(fixtures[0]); ++index) {
    expect_wire(fixtures[index].status_code, fixtures[index].reason, NULL, 0u,
                fixtures[index].expected, fixtures[index].expected_length,
                "common status fixture serializes");
  }

  measured = omni_http_response_head_build(200u, (const unsigned char *)"OK",
                                           2u, NULL, 0u, NULL, 0u);
  check(measured.status == OMNI_HTTP_RESPONSE_HEAD_ERR_OUTPUT_TOO_SMALL &&
            measured.written_bytes == 0u && measured.required_bytes == 19u,
        "measure-only zero-header query reports exact required bytes");
}

static void test_empty_reason_and_headers(void) {
  static const unsigned char expected[] = "HTTP/1.1 204 \r\n\r\n";
  static const unsigned char empty_header_expected[] =
      "HTTP/1.1 200 OK\r\nX-Empty: \r\n\r\n";
  struct omni_http_response_header header = make_header("X-Empty", "");

  expect_wire(204u, "", NULL, 0u, expected, sizeof(expected) - 1u,
              "empty reason preserves status-line space");
  expect_wire(200u, "OK", &header, 1u, empty_header_expected,
              sizeof(empty_header_expected) - 1u,
              "empty header value keeps canonical colon-space separator");
}

static void test_multi_header_order_and_casing(void) {
  static const unsigned char expected[] = "HTTP/1.1 200 OK\r\n"
                                          "Content-Length: 2\r\n"
                                          "Content-Type: application/json\r\n"
                                          "Connection: keep-alive\r\n"
                                          "x-Test: exact\r\n"
                                          "X-Test: duplicate\r\n"
                                          "\r\n";
  struct omni_http_response_header headers[5];

  headers[0] = make_header("Content-Length", "2");
  headers[1] = make_header("Content-Type", "application/json");
  headers[2] = make_header("Connection", "keep-alive");
  headers[3] = make_header("x-Test", "exact");
  headers[4] = make_header("X-Test", "duplicate");
  expect_wire(200u, "OK", headers, 5u, expected, sizeof(expected) - 1u,
              "multi-header order casing and duplicates are preserved");
}

static void test_capacity_and_argument_contracts(void) {
  static const unsigned char expected[] = "HTTP/1.1 200 OK\r\n\r\n";
  struct omni_http_response_head_result result;
  struct omni_http_response_header header = make_header("X-Test", "value");
  unsigned char output[sizeof(expected) + 4u];
  unsigned char snapshot[sizeof(output)];

  fill_canary(output, sizeof(output), 0x5au);
  result =
      omni_http_response_head_build(200u, (const unsigned char *)"OK", 2u, NULL,
                                    0u, output, sizeof(expected) - 1u);
  check(result.status == OMNI_HTTP_RESPONSE_HEAD_OK &&
            result.written_bytes == sizeof(expected) - 1u,
        "exact output capacity succeeds");
  check(memcmp(output, expected, sizeof(expected) - 1u) == 0,
        "exact-capacity bytes are correct");

  fill_canary(output, sizeof(output), 0x5au);
  memcpy(snapshot, output, sizeof(output));
  result =
      omni_http_response_head_build(200u, (const unsigned char *)"OK", 2u, NULL,
                                    0u, output, sizeof(expected) - 2u);
  check(result.status == OMNI_HTTP_RESPONSE_HEAD_ERR_OUTPUT_TOO_SMALL &&
            result.written_bytes == 0u &&
            result.required_bytes == sizeof(expected) - 1u,
        "one-byte-short capacity reports exact required size");
  check(memcmp(output, snapshot, sizeof(output)) == 0,
        "one-byte-short capacity leaves output unchanged");

  result = omni_http_response_head_build(200u, (const unsigned char *)"OK", 2u,
                                         NULL, 0u, NULL, 1u);
  check(result.status == OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_ARGUMENT,
        "NULL output with positive capacity is invalid");
  expect_failure_unchanged(200u, NULL, 1u, NULL, 0u,
                           OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_REASON, 0u,
                           "NULL reason with positive length is invalid");
  expect_failure_unchanged(200u, (const unsigned char *)"OK", 2u, NULL, 1u,
                           OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_ARGUMENT, 0u,
                           "NULL header array with positive count is invalid");

  header.name = NULL;
  header.name_length = 1u;
  expect_failure_unchanged(200u, (const unsigned char *)"OK", 2u, &header, 1u,
                           OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_HEADER, 0u,
                           "NULL header name with positive length is invalid");
  header = make_header("X-Test", "value");
  header.value = NULL;
  header.value_length = 1u;
  expect_failure_unchanged(200u, (const unsigned char *)"OK", 2u, &header, 1u,
                           OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_HEADER, 0u,
                           "NULL header value with positive length is invalid");
}

static void test_header_count_boundaries(void) {
  struct omni_http_response_header
      headers[OMNI_HTTP_RESPONSE_HEAD_MAX_HEADERS + 1u];
  unsigned char output[OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES];
  unsigned char snapshot[sizeof(output)];
  struct omni_http_response_head_result result;
  size_t index;

  for (index = 0u; index < sizeof(headers) / sizeof(headers[0]); ++index) {
    headers[index] = make_header("X", "v");
  }
  result = omni_http_response_head_build(
      200u, (const unsigned char *)"OK", 2u, headers,
      OMNI_HTTP_RESPONSE_HEAD_MAX_HEADERS, output, sizeof(output));
  check(result.status == OMNI_HTTP_RESPONSE_HEAD_OK &&
            result.written_bytes == 403u,
        "maximum response-header count succeeds");

  fill_canary(output, sizeof(output), 0x6bu);
  memcpy(snapshot, output, sizeof(output));
  result = omni_http_response_head_build(
      200u, (const unsigned char *)"OK", 2u, headers,
      OMNI_HTTP_RESPONSE_HEAD_MAX_HEADERS + 1u, output, sizeof(output));
  check(result.status == OMNI_HTTP_RESPONSE_HEAD_ERR_TOO_MANY_HEADERS &&
            result.written_bytes == 0u && result.required_bytes == 0u,
        "maximum response-header count plus one is rejected before scanning");
  check(memcmp(output, snapshot, sizeof(output)) == 0,
        "too many headers leaves output unchanged");
}

/* Exact capacity, one byte short, and the tail canary for one representative. */
static void check_capacity_contract(
    unsigned int status_code, const char *reason,
    const struct omni_http_response_header *headers, size_t header_count,
    const unsigned char *expected, size_t expected_length,
    const char *case_name) {
  static unsigned char output[OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES + 8u];
  unsigned char snapshot[sizeof(output)];
  struct omni_http_response_head_result result;

  printf("case: %s\n", case_name);

  result = omni_http_response_head_build(
      status_code, (const unsigned char *)reason, strlen(reason), headers,
      header_count, NULL, 0u);
  check(result.status == OMNI_HTTP_RESPONSE_HEAD_ERR_OUTPUT_TOO_SMALL &&
            result.written_bytes == 0u &&
            result.required_bytes == expected_length,
        "measure-only query agrees with the expected head length");

  fill_canary(output, sizeof(output), 0x2eu);
  result = omni_http_response_head_build(
      status_code, (const unsigned char *)reason, strlen(reason), headers,
      header_count, output, expected_length);
  check(result.status == OMNI_HTTP_RESPONSE_HEAD_OK &&
            result.written_bytes == expected_length,
        "exact capacity succeeds");
  check(memcmp(output, expected, expected_length) == 0 &&
            output[expected_length] == 0x2eu,
        "exact capacity writes exact bytes and preserves the tail canary");

  fill_canary(output, sizeof(output), 0x2eu);
  memcpy(snapshot, output, sizeof(output));
  result = omni_http_response_head_build(
      status_code, (const unsigned char *)reason, strlen(reason), headers,
      header_count, output, expected_length - 1u);
  check(result.status == OMNI_HTTP_RESPONSE_HEAD_ERR_OUTPUT_TOO_SMALL &&
            result.written_bytes == 0u &&
            result.required_bytes == expected_length,
        "one-byte-short capacity reports the exact required size");
  check(memcmp(output, snapshot, sizeof(output)) == 0,
        "one-byte-short capacity leaves output fully unchanged");
}

static void test_capacity_sweep(void) {
  static const unsigned char empty_reason[] = "HTTP/1.1 204 \r\n\r\n";
  static const unsigned char zero_headers[] = "HTTP/1.1 200 OK\r\n\r\n";
  static const unsigned char custom_status[] = "HTTP/1.1 999 Custom\r\n\r\n";
  static const unsigned char server_error[] =
      "HTTP/1.1 500 Internal Server Error\r\n\r\n";
  static const unsigned char empty_value[] =
      "HTTP/1.1 200 OK\r\nX-Empty: \r\n\r\n";
  static const unsigned char multi[] = "HTTP/1.1 200 OK\r\n"
                                       "Content-Length: 2\r\n"
                                       "Content-Type: application/json\r\n"
                                       "Connection: keep-alive\r\n"
                                       "\r\n";
  struct omni_http_response_header headers[3];
  struct omni_http_response_header single;

  single = make_header("X-Empty", "");
  check_capacity_contract(200u, "OK", NULL, 0u, zero_headers,
                          sizeof(zero_headers) - 1u, "200 OK with zero headers");
  check_capacity_contract(204u, "", NULL, 0u, empty_reason,
                          sizeof(empty_reason) - 1u, "204 with empty reason");
  check_capacity_contract(999u, "Custom", NULL, 0u, custom_status,
                          sizeof(custom_status) - 1u, "999 with custom reason");
  check_capacity_contract(500u, "Internal Server Error", NULL, 0u, server_error,
                          sizeof(server_error) - 1u, "500 long reason phrase");
  check_capacity_contract(200u, "OK", &single, 1u, empty_value,
                          sizeof(empty_value) - 1u, "empty header value");
  headers[0] = make_header("Content-Length", "2");
  headers[1] = make_header("Content-Type", "application/json");
  headers[2] = make_header("Connection", "keep-alive");
  check_capacity_contract(200u, "OK", headers, 3u, multi, sizeof(multi) - 1u,
                          "three ordered headers");
}

static void test_invalid_status_and_injection(void) {
  struct omni_http_response_header header;
  const unsigned char reason_injection[] = "OK\r\nX-Evil: yes";
  const unsigned char name_injection[] = "X-Test\r\nInjected";
  const unsigned char value_injection[] = "safe\r\nInjected: yes";

  expect_failure_unchanged(99u, (const unsigned char *)"Bad", 3u, NULL, 0u,
                           OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_STATUS, 0u,
                           "two-digit status is invalid");
  expect_failure_unchanged(0u, (const unsigned char *)"Bad", 3u, NULL, 0u,
                           OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_STATUS, 0u,
                           "zero status is invalid");
  expect_failure_unchanged(1000u, (const unsigned char *)"Bad", 3u, NULL, 0u,
                           OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_STATUS, 0u,
                           "four-digit status is invalid");

  expect_failure_unchanged(200u, reason_injection,
                           sizeof(reason_injection) - 1u, NULL, 0u,
                           OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_REASON, 32u,
                           "reason CRLF injection is rejected");
  header.name = name_injection;
  header.name_length = sizeof(name_injection) - 1u;
  header.value = (const unsigned char *)"value";
  header.value_length = 5u;
  expect_failure_unchanged(200u, (const unsigned char *)"OK", 2u, &header, 1u,
                           OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_HEADER, 44u,
                           "header-name CRLF injection is rejected");
  header.name = (const unsigned char *)"X-Test";
  header.name_length = 6u;
  header.value = value_injection;
  header.value_length = sizeof(value_injection) - 1u;
  expect_failure_unchanged(200u, (const unsigned char *)"OK", 2u, &header, 1u,
                           OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_HEADER, 48u,
                           "header-value CRLF injection is rejected");
}

static void test_byte_classification(void) {
  unsigned char single_byte = 0u;
  struct omni_http_response_header header;
  unsigned char output[128];
  unsigned char snapshot[sizeof(output)];
  struct omni_http_response_head_result result;
  unsigned int value;

  header.name = &single_byte;
  header.name_length = 1u;
  header.value = (const unsigned char *)"v";
  header.value_length = 1u;
  for (value = 0u; value <= UCHAR_MAX; ++value) {
    const bool accepted = is_test_token((unsigned char)value);

    single_byte = (unsigned char)value;
    fill_canary(output, sizeof(output), 0x91u);
    memcpy(snapshot, output, sizeof(output));
    result =
        omni_http_response_head_build(200u, (const unsigned char *)"OK", 2u,
                                      &header, 1u, output, sizeof(output));
    check((result.status == OMNI_HTTP_RESPONSE_HEAD_OK) == accepted,
          "every header-name byte follows strict HTTP token policy");
    if (!accepted) {
      check(memcmp(output, snapshot, sizeof(output)) == 0,
            "invalid header-name byte leaves output unchanged");
    }
  }

  header.name = (const unsigned char *)"X";
  header.name_length = 1u;
  header.value = &single_byte;
  for (value = 0u; value <= UCHAR_MAX; ++value) {
    const bool accepted = is_test_field_byte((unsigned char)value);

    single_byte = (unsigned char)value;
    fill_canary(output, sizeof(output), 0x92u);
    memcpy(snapshot, output, sizeof(output));
    result =
        omni_http_response_head_build(200u, (const unsigned char *)"OK", 2u,
                                      &header, 1u, output, sizeof(output));
    check((result.status == OMNI_HTTP_RESPONSE_HEAD_OK) == accepted,
          "every header-value byte follows conservative field policy");
    if (!accepted) {
      check(memcmp(output, snapshot, sizeof(output)) == 0,
            "invalid header-value byte leaves output unchanged");
    }
  }

  header.value = (const unsigned char *)"v";
  header.value_length = 1u;
  for (value = 0u; value <= UCHAR_MAX; ++value) {
    const bool accepted = is_test_field_byte((unsigned char)value);
    const unsigned char reason_byte = (unsigned char)value;

    fill_canary(output, sizeof(output), 0x93u);
    memcpy(snapshot, output, sizeof(output));
    result = omni_http_response_head_build(200u, &reason_byte, 1u, &header, 1u,
                                           output, sizeof(output));
    check((result.status == OMNI_HTTP_RESPONSE_HEAD_OK) == accepted,
          "every reason byte follows conservative reason policy");
    if (!accepted) {
      check(memcmp(output, snapshot, sizeof(output)) == 0,
            "invalid reason byte leaves output unchanged");
    }
  }
}

static void test_size_boundaries_and_overflow(void) {
  static unsigned char large_value[OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES];
  static unsigned char output[OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES];
  static unsigned char snapshot[OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES];
  struct omni_http_response_header header = make_header("X", "");
  struct omni_http_response_head_result result;
  size_t index;

  for (index = 0u; index < sizeof(large_value); ++index)
    large_value[index] = 'a';
  header.value = large_value;
  header.value_length = OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES - 24u;
  result = omni_http_response_head_build(200u, (const unsigned char *)"OK", 2u,
                                         &header, 1u, output, sizeof(output));
  check(result.status == OMNI_HTTP_RESPONSE_HEAD_OK &&
            result.written_bytes == OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES &&
            result.required_bytes == OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES,
        "exact maximum response-head size succeeds");
  check(output[OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES - 1u] == '\n',
        "exact maximum response-head ends with final LF");

  header.value_length = OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES - 23u;
  fill_canary(output, sizeof(output), 0x7du);
  memcpy(snapshot, output, sizeof(output));
  result = omni_http_response_head_build(200u, (const unsigned char *)"OK", 2u,
                                         &header, 1u, output, sizeof(output));
  check(result.status == OMNI_HTTP_RESPONSE_HEAD_ERR_TOO_LARGE &&
            result.written_bytes == 0u &&
            result.required_bytes == OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES + 1u,
        "one byte over maximum is too large with exact measured size");
  check(memcmp(output, snapshot, sizeof(output)) == 0,
        "too-large response leaves output unchanged");

  header.value = (const unsigned char *)"v";
  header.value_length = SIZE_MAX;
  expect_failure_unchanged(200u, (const unsigned char *)"OK", 2u, &header, 1u,
                           OMNI_HTTP_RESPONSE_HEAD_ERR_OVERFLOW, 0u,
                           "SIZE_MAX header value is rejected before scanning");
  header.value = (const unsigned char *)"v";
  header.value_length = 1u;
  header.name = (const unsigned char *)"X";
  header.name_length = SIZE_MAX;
  expect_failure_unchanged(200u, (const unsigned char *)"OK", 2u, &header, 1u,
                           OMNI_HTTP_RESPONSE_HEAD_ERR_OVERFLOW, 0u,
                           "SIZE_MAX header name is rejected before scanning");
  expect_failure_unchanged(200u, (const unsigned char *)"OK", SIZE_MAX, NULL,
                           0u, OMNI_HTTP_RESPONSE_HEAD_ERR_OVERFLOW, 0u,
                           "SIZE_MAX reason is rejected before scanning");
  header.name = (const unsigned char *)"X";
  header.name_length = 1u;
  header.value = (const unsigned char *)"v";
  header.value_length = SIZE_MAX - 16u;
  expect_failure_unchanged(200u, (const unsigned char *)"OK", 2u, &header, 1u,
                           OMNI_HTTP_RESPONSE_HEAD_ERR_OVERFLOW, 0u,
                           "near-SIZE_MAX header value overflows checked sums");
  header.name_length = SIZE_MAX - 16u;
  expect_failure_unchanged(200u, (const unsigned char *)"OK", 2u, &header, 1u,
                           OMNI_HTTP_RESPONSE_HEAD_ERR_OVERFLOW, 0u,
                           "near-SIZE_MAX header name overflows checked sums");
  expect_failure_unchanged(200u, (const unsigned char *)"OK", SIZE_MAX - 16u,
                           NULL, 0u, OMNI_HTTP_RESPONSE_HEAD_ERR_OVERFLOW, 0u,
                           "near-SIZE_MAX reason overflows checked sums");

  /*
   * A length that is large but not yet overflowing must be rejected by the
   * protocol bound before any byte is read, so a fabricated span paired with a
   * real, small backing buffer can never be scanned. A regressed order would
   * read out of bounds here and trip the sanitizers.
   */
  header.name_length = 1u;
  header.value_length = 20000u;
  expect_failure_unchanged(200u, (const unsigned char *)"OK", 2u, &header, 1u,
                           OMNI_HTTP_RESPONSE_HEAD_ERR_TOO_LARGE, 20024u,
                           "over-limit length is bounded before scanning bytes");
  header.value_length = 1u;
  expect_failure_unchanged(200u, (const unsigned char *)"OK", 20000u, &header,
                           1u, OMNI_HTTP_RESPONSE_HEAD_ERR_TOO_LARGE, 20023u,
                           "over-limit reason is bounded before scanning bytes");
}

static void test_extra_status_and_array_rules(void) {
  static const unsigned char expected[] = "HTTP/1.1 100 Continue\r\n\r\n";
  struct omni_http_response_header unused = make_header("X", "v");
  struct omni_http_response_head_result result;

  result = omni_http_response_head_build(100u, (const unsigned char *)"Continue",
                                         8u, NULL, 0u, NULL, 0u);
  check(result.status == OMNI_HTTP_RESPONSE_HEAD_ERR_OUTPUT_TOO_SMALL &&
            result.required_bytes == sizeof(expected) - 1u,
        "lowest valid three-digit status is accepted");
  result = omni_http_response_head_build((unsigned int)SIZE_MAX,
                                         (const unsigned char *)"OK", 2u, NULL,
                                         0u, NULL, 0u);
  check(result.status == OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_STATUS &&
            result.written_bytes == 0u,
        "SIZE_MAX-truncated status code is rejected");
  result = omni_http_response_head_build(
      200u, (const unsigned char *)"OK", 2u, &unused, 0u, NULL, 0u);
  check(result.status == OMNI_HTTP_RESPONSE_HEAD_ERR_OUTPUT_TOO_SMALL &&
            result.required_bytes == 19u,
        "non-NULL header array with zero count is valid");
}

static void test_input_immutability_and_determinism(void) {
  unsigned char reason[] = "OK";
  unsigned char name[] = "x-Test";
  unsigned char value[] = "exact value";
  unsigned char reason_snapshot[sizeof(reason)];
  unsigned char name_snapshot[sizeof(name)];
  unsigned char value_snapshot[sizeof(value)];
  unsigned char first[128];
  unsigned char second[128];
  struct omni_http_response_header header;
  struct omni_http_response_head_result first_result;
  struct omni_http_response_head_result second_result;

  memcpy(reason_snapshot, reason, sizeof(reason));
  memcpy(name_snapshot, name, sizeof(name));
  memcpy(value_snapshot, value, sizeof(value));
  header.name = name;
  header.name_length = sizeof(name) - 1u;
  header.value = value;
  header.value_length = sizeof(value) - 1u;
  first_result = omni_http_response_head_build(
      200u, reason, sizeof(reason) - 1u, &header, 1u, first, sizeof(first));
  second_result = omni_http_response_head_build(
      200u, reason, sizeof(reason) - 1u, &header, 1u, second, sizeof(second));
  check(first_result.status == OMNI_HTTP_RESPONSE_HEAD_OK &&
            second_result.status == OMNI_HTTP_RESPONSE_HEAD_OK,
        "repeated deterministic builds succeed");
  check(first_result.written_bytes == second_result.written_bytes &&
            memcmp(first, second, first_result.written_bytes) == 0,
        "repeated deterministic builds produce identical bytes");
  check(memcmp(reason, reason_snapshot, sizeof(reason)) == 0 &&
            memcmp(name, name_snapshot, sizeof(name)) == 0 &&
            memcmp(value, value_snapshot, sizeof(value)) == 0,
        "successful build does not mutate borrowed metadata");
}

static void test_stress(void) {
  static const unsigned int statuses[] = {200u, 204u, 400u, 404u, 405u, 500u};
  static const char *const reasons[] = {
      "OK",        "No Content",         "Bad Request",
      "Not Found", "Method Not Allowed", "Internal Server Error"};
  static const char *const names[] = {"X-A", "X-B", "X-C"};
  static const char *const values[] = {"a", "bb", "ccc"};
  struct omni_http_response_header headers[3];
  unsigned char output[256];
  size_t iteration;

  for (iteration = 0u; iteration < 10000u; ++iteration) {
    const size_t header_count = iteration % 4u;
    const size_t fixture = iteration % 6u;
    struct omni_http_response_head_result result;
    size_t index;

    for (index = 0u; index < 3u; ++index) {
      headers[index] =
          make_header(names[index], values[(iteration + index) % 3u]);
    }
    fill_canary(output, sizeof(output), 0xe1u);
    result = omni_http_response_head_build(
        statuses[fixture], (const unsigned char *)reasons[fixture],
        strlen(reasons[fixture]), headers, header_count, output,
        sizeof(output));
    check(result.status == OMNI_HTTP_RESPONSE_HEAD_OK &&
              result.written_bytes == result.required_bytes &&
              output[result.written_bytes] == 0xe1u,
          "10,000 bounded builds remain deterministic and within the tail "
          "canary");
  }
}

int main(void) {
  test_zero_headers_and_status_fixtures();
  test_empty_reason_and_headers();
  test_multi_header_order_and_casing();
  test_capacity_and_argument_contracts();
  test_header_count_boundaries();
  test_capacity_sweep();
  test_invalid_status_and_injection();
  test_byte_classification();
  test_size_boundaries_and_overflow();
  test_extra_status_and_array_rules();
  test_input_immutability_and_determinism();
  test_stress();

  printf("http-response-head checks: %zu; failures: %zu; ", check_count,
         failure_count);
  printf("sizeof(omni_http_response_header)=%zu; ",
         sizeof(struct omni_http_response_header));
  printf("sizeof(omni_http_response_head_result)=%zu\n",
         sizeof(struct omni_http_response_head_result));
  return failure_count == 0u ? 0 : 1;
}
