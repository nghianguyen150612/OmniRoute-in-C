/*
 * OmniRoute native backend — bounded HTTP header-line tests (Task 034).
 *
 * Deterministic, framework-free checks for grammar, incremental input,
 * limits, borrowed spans, byte classes, binary safety, and immutability.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omniroute/http_header_line.h"

static size_t check_count = 0u;
static size_t failure_count = 0u;

static void check(bool condition, const char *name) {
  ++check_count;
  if (!condition) {
    ++failure_count;
    fprintf(stderr, "NOT OK - %s\n", name);
  }
}

static bool is_test_token_byte(unsigned char byte) {
  if ((byte >= (unsigned char)'A' && byte <= (unsigned char)'Z') ||
      (byte >= (unsigned char)'a' && byte <= (unsigned char)'z') ||
      (byte >= (unsigned char)'0' && byte <= (unsigned char)'9')) {
    return true;
  }
  return byte == (unsigned char)'!' || byte == (unsigned char)'#' ||
         byte == (unsigned char)'$' || byte == (unsigned char)'%' ||
         byte == (unsigned char)'&' || byte == (unsigned char) '\'' ||
         byte == (unsigned char)'*' || byte == (unsigned char)'+' ||
         byte == (unsigned char)'-' || byte == (unsigned char)'.' ||
         byte == (unsigned char)'^' || byte == (unsigned char)'_' ||
         byte == (unsigned char)'`' || byte == (unsigned char)'|' ||
         byte == (unsigned char)'~';
}

static bool is_test_field_value_byte(unsigned char byte) {
  return byte == (unsigned char)'\t' || (byte >= 0x20u && byte <= 0x7eu);
}

static bool has_no_success_fields(struct omni_http_header_line_result result) {
  return result.name.data == NULL && result.name.length == 0u &&
         result.value.data == NULL && result.value.length == 0u &&
         result.consumed_bytes == 0u;
}

static struct omni_http_header_line_result parse_and_check_unchanged(
    const unsigned char *data, size_t length, const char *case_name) {
  unsigned char snapshot[OMNI_HTTP_HEADER_LINE_MAX_TOTAL_BYTES + 2u];
  struct omni_http_header_line_result result;

  if (data == NULL) {
    return omni_http_header_line_parse(data, length);
  }
  if (length > sizeof(snapshot)) {
    check(false, "immutability input fits the fixed snapshot");
    return omni_http_header_line_parse(data, length);
  }
  if (length > 0u) memcpy(snapshot, data, length);
  result = omni_http_header_line_parse(data, length);
  check(length == 0u || memcmp(snapshot, data, length) == 0, case_name);
  return result;
}

static struct omni_http_header_line_result expect_status(
    const unsigned char *data, size_t length,
    enum omni_http_header_line_status expected, const char *case_name) {
  struct omni_http_header_line_result result =
      parse_and_check_unchanged(data, length, "parser leaves input unchanged");

  check(result.status == expected, case_name);
  if (expected != OMNI_HTTP_HEADER_LINE_COMPLETE) {
    check(has_no_success_fields(result), "non-complete result exposes no spans or consumption");
  }
  return result;
}

static struct omni_http_header_line_result expect_complete(
    const unsigned char *data, size_t length, size_t name_offset,
    const char *expected_name, size_t value_offset, const char *expected_value,
    size_t expected_consumed, const char *case_name) {
  struct omni_http_header_line_result result =
      parse_and_check_unchanged(data, length, "parser leaves input unchanged");
  size_t name_length = strlen(expected_name);
  size_t value_length = strlen(expected_value);

  check(result.status == OMNI_HTTP_HEADER_LINE_COMPLETE, case_name);
  check(result.name.data == data + name_offset,
        "name span points into original input at the expected offset");
  check(result.name.length == name_length, "name span length is exact");
  check(name_length == 0u ||
            memcmp(result.name.data, expected_name, name_length) == 0,
        "name span bytes are exact and case-preserved");
  check(result.value.data == data + value_offset,
        "value span points into original input at the expected offset");
  check(result.value.length == value_length, "value span length is exact");
  check(value_length == 0u ||
            memcmp(result.value.data, expected_value, value_length) == 0,
        "value span bytes are exact");
  check(result.consumed_bytes == expected_consumed,
        "complete result consumes exactly the first header line");
  check(result.error_offset == 0u, "complete result has zero error offset");
  return result;
}

static void test_valid_lines_and_borrowed_spans(void) {
  static const unsigned char host_space[] = "Host: example.com\r\n";
  static const unsigned char host_no_space[] = "Host:example.com\r\n";
  static const unsigned char content_type[] =
      "Content-Type: application/json\r\n";
  static const unsigned char x_test[] = "X-Test: abc\r\n";
  static const unsigned char x_test_tab[] = "X-Test:\tabc\r\n";
  static const unsigned char x_test_trim[] = "X-Test: abc \t \r\n";
  static const unsigned char x_custom[] = "X_Custom: value\r\n";
  static const unsigned char opaque_value[] =
      "X-Format: text/plain; charset=utf-8, other\r\n";

  (void)expect_complete(host_space, sizeof(host_space) - 1u, 0u, "Host", 6u,
                        "example.com", sizeof(host_space) - 1u,
                        "Host with optional SP after colon");
  (void)expect_complete(host_no_space, sizeof(host_no_space) - 1u, 0u, "Host",
                        5u, "example.com", sizeof(host_no_space) - 1u,
                        "Host without optional whitespace");
  (void)expect_complete(content_type, sizeof(content_type) - 1u, 0u,
                        "Content-Type", 14u, "application/json",
                        sizeof(content_type) - 1u, "Content-Type field");
  (void)expect_complete(x_test, sizeof(x_test) - 1u, 0u, "X-Test", 8u, "abc",
                        sizeof(x_test) - 1u, "ordinary X-Test field");
  (void)expect_complete(x_test_tab, sizeof(x_test_tab) - 1u, 0u, "X-Test", 8u,
                        "abc", sizeof(x_test_tab) - 1u,
                        "HTAB after colon is leading OWS");
  (void)expect_complete(x_test_trim, sizeof(x_test_trim) - 1u, 0u, "X-Test",
                        8u, "abc", sizeof(x_test_trim) - 1u,
                        "trailing SP and HTAB are trimmed");
  (void)expect_complete(x_custom, sizeof(x_custom) - 1u, 0u, "X_Custom", 10u,
                        "value", sizeof(x_custom) - 1u,
                        "underscore is an HTTP token character");
  (void)expect_complete(opaque_value, sizeof(opaque_value) - 1u, 0u,
                        "X-Format", 10u, "text/plain; charset=utf-8, other",
                        sizeof(opaque_value) - 1u,
                        "field value punctuation remains opaque");
}

static void test_name_case_preservation(void) {
  static const struct {
    const unsigned char *line;
    size_t length;
    const char *name;
  } cases[] = {
      {(const unsigned char *)"HOST: x\r\n", sizeof("HOST: x\r\n") - 1u, "HOST"},
      {(const unsigned char *)"Host: x\r\n", sizeof("Host: x\r\n") - 1u, "Host"},
      {(const unsigned char *)"host: x\r\n", sizeof("host: x\r\n") - 1u, "host"},
  };

  for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    (void)expect_complete(cases[index].line, cases[index].length, 0u,
                          cases[index].name, 6u, "x", cases[index].length,
                          "field-name case is preserved byte-for-byte");
  }
}

static void test_extra_bytes_and_exact_consumption(void) {
  static const unsigned char lines[] = "Host: a\r\nNext: b\r\n";
  const size_t first_length = sizeof("Host: a\r\n") - 1u;
  struct omni_http_header_line_result result =
      expect_complete(lines, sizeof(lines) - 1u, 0u, "Host", 6u, "a",
                      first_length, "extra header bytes are ignored");

  check(result.consumed_bytes == first_length,
        "only bytes through the first CRLF are consumed");
  check(result.name.data == lines && result.value.data == lines + 6u,
        "extra-data result borrows the original combined input");
}

static void test_ows_and_empty_values(void) {
  static const unsigned char interior[] =
      "A:  hello \t world  \t\r\n";
  static const unsigned char empty_plain[] = "A:\r\n";
  static const unsigned char empty_space[] = "A: \r\n";
  static const unsigned char empty_tab[] = "A:\t\r\n";
  static const unsigned char empty_mixed[] = "A:   \t\r\n";
  static const unsigned char leading_mixed[] = "A: \t  b\r\n";
  static const unsigned char trailing_tab[] = "A:b\t\r\n";
  static const unsigned char trailing_mixed[] = "A:b \t \r\n";

  (void)expect_complete(interior, sizeof(interior) - 1u, 0u, "A", 4u,
                        "hello \t world", sizeof(interior) - 1u,
                        "interior OWS remains while edge OWS is trimmed");
  (void)expect_complete(empty_plain, sizeof(empty_plain) - 1u, 0u, "A", 2u,
                        "", sizeof(empty_plain) - 1u, "empty value without OWS");
  (void)expect_complete(empty_space, sizeof(empty_space) - 1u, 0u, "A", 3u,
                        "", sizeof(empty_space) - 1u,
                        "empty value containing leading SP");
  (void)expect_complete(empty_tab, sizeof(empty_tab) - 1u, 0u, "A", 3u, "",
                        sizeof(empty_tab) - 1u,
                        "empty value containing leading HTAB");
  (void)expect_complete(empty_mixed, sizeof(empty_mixed) - 1u, 0u, "A", 6u,
                        "", sizeof(empty_mixed) - 1u,
                        "all-OWS value produces an empty span");
  (void)expect_complete(leading_mixed, sizeof(leading_mixed) - 1u, 0u, "A",
                        6u, "b", sizeof(leading_mixed) - 1u,
                        "mixed leading OWS is excluded");
  (void)expect_complete(trailing_tab, sizeof(trailing_tab) - 1u, 0u, "A", 2u,
                        "b", sizeof(trailing_tab) - 1u,
                        "trailing HTAB is excluded");
  (void)expect_complete(trailing_mixed, sizeof(trailing_mixed) - 1u, 0u, "A",
                        2u, "b", sizeof(trailing_mixed) - 1u,
                        "multiple trailing OWS bytes are excluded");
}

static void test_colon_syntax_and_blank_line(void) {
  static const unsigned char no_space[] = "A:b\r\n";
  static const unsigned char with_space[] = "A: b\r\n";
  static const unsigned char before_colon[] = "A : b\r\n";
  static const unsigned char tab_before_colon[] = "A\t: b\r\n";
  static const unsigned char empty_name[] = ":b\r\n";
  static const unsigned char blank_line[] = "\r\n";

  (void)expect_complete(no_space, sizeof(no_space) - 1u, 0u, "A", 2u, "b",
                        sizeof(no_space) - 1u, "colon immediately after name");
  (void)expect_complete(with_space, sizeof(with_space) - 1u, 0u, "A", 3u, "b",
                        sizeof(with_space) - 1u, "OWS after colon accepted");
  (void)expect_status(before_colon, sizeof(before_colon) - 1u,
                      OMNI_HTTP_HEADER_LINE_INVALID,
                      "SP before colon is invalid");
  (void)expect_status(tab_before_colon, sizeof(tab_before_colon) - 1u,
                      OMNI_HTTP_HEADER_LINE_INVALID,
                      "HTAB before colon is invalid");
  (void)expect_status(empty_name, sizeof(empty_name) - 1u,
                      OMNI_HTTP_HEADER_LINE_INVALID,
                      "empty field-name is invalid");
  (void)expect_status(blank_line, sizeof(blank_line) - 1u,
                      OMNI_HTTP_HEADER_LINE_INVALID,
                      "header-block blank line is not a field line");
}

static void test_invalid_name_and_value_bytes(void) {
  static const unsigned char leading_space[] = " A: b\r\n";
  static const unsigned char leading_tab[] = "\tA: b\r\n";
  static const unsigned char embedded_nul_name[] = {'A', 0u, ':', 'b', '\r', '\n'};
  static const unsigned char del_name[] = {'A', 0x7fu, ':', 'b', '\r', '\n'};
  static const unsigned char high_name[] = {'A', 0x80u, ':', 'b', '\r', '\n'};
  static const unsigned char embedded_nul_value[] =
      {'A', ':', 'x', 0u, 'z', '\r', '\n'};
  static const unsigned char embedded_cr_value[] =
      {'A', ':', 'x', '\r', 'z', '\n'};
  static const unsigned char embedded_lf_value[] =
      {'A', ':', 'x', '\n', 'z', '\r', '\n'};
  static const unsigned char c0_value[] = {'A', ':', 'x', 0x01u, 'z', '\r', '\n'};
  static const unsigned char del_value[] = {'A', ':', 'x', 0x7fu, 'z', '\r', '\n'};
  static const unsigned char high_value[] = {'A', ':', 'x', 0x80u, 'z', '\r', '\n'};

  (void)expect_status(leading_space, sizeof(leading_space),
                      OMNI_HTTP_HEADER_LINE_INVALID,
                      "leading SP is not a field-name token");
  (void)expect_status(leading_tab, sizeof(leading_tab),
                      OMNI_HTTP_HEADER_LINE_INVALID,
                      "leading HTAB is not a field-name token");
  (void)expect_status(embedded_nul_name, sizeof(embedded_nul_name),
                      OMNI_HTTP_HEADER_LINE_INVALID,
                      "embedded NUL is invalid in field-name");
  (void)expect_status(del_name, sizeof(del_name), OMNI_HTTP_HEADER_LINE_INVALID,
                      "DEL is invalid in field-name");
  (void)expect_status(high_name, sizeof(high_name), OMNI_HTTP_HEADER_LINE_INVALID,
                      "high byte is invalid in field-name");
  (void)expect_status(embedded_nul_value, sizeof(embedded_nul_value),
                      OMNI_HTTP_HEADER_LINE_INVALID,
                      "embedded NUL is rejected without truncation");
  (void)expect_status(embedded_cr_value, sizeof(embedded_cr_value),
                      OMNI_HTTP_HEADER_LINE_INVALID,
                      "CR inside value must be followed immediately by LF");
  (void)expect_status(embedded_lf_value, sizeof(embedded_lf_value),
                      OMNI_HTTP_HEADER_LINE_INVALID,
                      "bare LF inside value is invalid");
  (void)expect_status(c0_value, sizeof(c0_value), OMNI_HTTP_HEADER_LINE_INVALID,
                      "other C0 control is invalid in value");
  (void)expect_status(del_value, sizeof(del_value), OMNI_HTTP_HEADER_LINE_INVALID,
                      "DEL is invalid in value");
  (void)expect_status(high_value, sizeof(high_value), OMNI_HTTP_HEADER_LINE_INVALID,
                      "high bytes are rejected by the ASCII contract");
}

static void test_line_endings_and_incomplete_prefixes(void) {
  static const unsigned char bare_lf[] = "A:x\n";
  static const unsigned char lone_cr[] = "A:x\r";
  static const unsigned char double_cr[] = "A:x\r\r\n";
  static const unsigned char cr_then_other[] = "A:x\rX";
  static const unsigned char lf_then_crlf[] = "A:x\n\r\n";
  static const unsigned char *const prefixes[] = {
      (const unsigned char *)"", (const unsigned char *)"H",
      (const unsigned char *)"Host", (const unsigned char *)"Host:",
      (const unsigned char *)"Host: ",
      (const unsigned char *)"Host: example.com",
      (const unsigned char *)"Host: example.com\r"};
  static const size_t prefix_lengths[] = {0u, 1u, 4u, 5u, 6u, 17u, 18u};

  (void)expect_status(bare_lf, sizeof(bare_lf) - 1u,
                      OMNI_HTTP_HEADER_LINE_INVALID,
                      "bare LF does not complete a header line");
  (void)expect_status(lone_cr, sizeof(lone_cr) - 1u,
                      OMNI_HTTP_HEADER_LINE_INCOMPLETE,
                      "trailing CR awaits LF");
  check(omni_http_header_line_parse(lone_cr, sizeof(lone_cr) - 1u).error_offset ==
            sizeof(lone_cr) - 1u,
        "trailing CR incomplete offset points after CR");
  (void)expect_status(double_cr, sizeof(double_cr) - 1u,
                      OMNI_HTTP_HEADER_LINE_INVALID,
                      "CR CR LF is invalid");
  (void)expect_status(cr_then_other, sizeof(cr_then_other) - 1u,
                      OMNI_HTTP_HEADER_LINE_INVALID,
                      "CR followed by a non-LF byte is invalid");
  (void)expect_status(lf_then_crlf, sizeof(lf_then_crlf) - 1u,
                      OMNI_HTTP_HEADER_LINE_INVALID,
                      "embedded LF is invalid even before a later CRLF");

  for (size_t index = 0u; index < sizeof(prefix_lengths) / sizeof(prefix_lengths[0]);
       ++index) {
    struct omni_http_header_line_result result = expect_status(
        prefixes[index], prefix_lengths[index], OMNI_HTTP_HEADER_LINE_INCOMPLETE,
        "documented viable prefix is incomplete");
    check(result.error_offset == prefix_lengths[index],
          "incomplete prefix offset marks the next required byte");
  }
}

static void test_every_prefix_of_valid_lines(void) {
  static const unsigned char *const lines[] = {
      (const unsigned char *)"Host: example.com\r\n",
      (const unsigned char *)"X-Test:\tabc \t\r\n",
      (const unsigned char *)"Content-Type: application/json\r\n"};
  static const size_t lengths[] = {
      sizeof("Host: example.com\r\n") - 1u,
      sizeof("X-Test:\tabc \t\r\n") - 1u,
      sizeof("Content-Type: application/json\r\n") - 1u};

  for (size_t line_index = 0u; line_index < sizeof(lengths) / sizeof(lengths[0]);
       ++line_index) {
    for (size_t prefix_length = 0u; prefix_length < lengths[line_index];
         ++prefix_length) {
      unsigned char *prefix = NULL;
      struct omni_http_header_line_result result;

      if (prefix_length > 0u) {
        prefix = (unsigned char *)malloc(prefix_length);
        check(prefix != NULL, "exact-size prefix allocation succeeds");
        if (prefix == NULL) continue;
        memcpy(prefix, lines[line_index], prefix_length);
      }
      result = expect_status(prefix, prefix_length,
                             OMNI_HTTP_HEADER_LINE_INCOMPLETE,
                             "every valid-line prefix before LF is incomplete");
      check(result.error_offset == prefix_length,
            "every prefix asks for its next byte at the prefix boundary");
      free(prefix);
    }
    (void)expect_status(lines[line_index], lengths[line_index],
                        OMNI_HTTP_HEADER_LINE_COMPLETE,
                        "full valid line completes after every prefix");
  }
}

static void test_deterministic_byte_classification(void) {
  size_t accepted_name_first = 0u;
  size_t accepted_name_later = 0u;
  size_t accepted_value = 0u;

  for (unsigned int value = 0u; value <= 255u; ++value) {
    unsigned char byte = (unsigned char)value;
    unsigned char name_first[] = {byte, ':', 'x', '\r', '\n'};
    unsigned char name_later[] = {'A', byte, ':', 'x', '\r', '\n'};
    unsigned char value_first[] = {'A', ':', byte, '\r', '\n'};
    unsigned char value_interior[] = {'A', ':', 'x', byte, 'y', '\r', '\n'};
    unsigned char value_trailing[] = {'A', ':', 'x', byte, '\r', '\n'};
    bool token = is_test_token_byte(byte);
    bool value_byte = is_test_field_value_byte(byte);
    enum omni_http_header_line_status name_first_expected =
        token ? OMNI_HTTP_HEADER_LINE_COMPLETE : OMNI_HTTP_HEADER_LINE_INVALID;
    enum omni_http_header_line_status name_later_expected =
        (token || byte == (unsigned char)':') ? OMNI_HTTP_HEADER_LINE_COMPLETE
                                              : OMNI_HTTP_HEADER_LINE_INVALID;
    enum omni_http_header_line_status value_expected =
        value_byte ? OMNI_HTTP_HEADER_LINE_COMPLETE : OMNI_HTTP_HEADER_LINE_INVALID;
    struct omni_http_header_line_result result;

    result = expect_status(name_first, sizeof(name_first), name_first_expected,
                           "first field-name byte matches HTTP token class");
    if (name_first_expected == OMNI_HTTP_HEADER_LINE_COMPLETE) {
      ++accepted_name_first;
      check(result.name.data == name_first && result.name.length == 1u,
            "first name-byte classification returns its borrowed token");
    }

    result = expect_status(name_later, sizeof(name_later), name_later_expected,
                           "later field-name byte follows token and colon syntax");
    if (name_later_expected == OMNI_HTTP_HEADER_LINE_COMPLETE) {
      ++accepted_name_later;
      check(result.name.data == name_later &&
                result.name.length == (token ? 2u : 1u),
            "later name-byte classification returns the preceding name");
    }

    result = expect_status(value_first, sizeof(value_first), value_expected,
                           "first field-value byte matches ASCII field-content");
    if (value_expected == OMNI_HTTP_HEADER_LINE_COMPLETE) {
      ++accepted_value;
      check(result.consumed_bytes == sizeof(value_first),
            "first value-byte classification consumes one complete line");
    }

    result = expect_status(value_interior, sizeof(value_interior), value_expected,
                           "interior field-value byte matches ASCII field-content");
    if (value_expected == OMNI_HTTP_HEADER_LINE_COMPLETE) {
      check(result.value.data == value_interior + 2u &&
                result.value.length == 3u,
            "interior value classification preserves OWS and visible bytes");
    }

    result = expect_status(value_trailing, sizeof(value_trailing), value_expected,
                           "trailing byte follows field-content then CRLF syntax");
    if (value_expected == OMNI_HTTP_HEADER_LINE_COMPLETE) {
      size_t expected_length =
          (byte == (unsigned char)' ' || byte == (unsigned char)'\t') ? 1u : 2u;
      check(result.value.data == value_trailing + 2u &&
                result.value.length == expected_length,
            "trailing classification trims only SP and HTAB");
    }
  }

  check(accepted_name_first == 77u,
        "all 77 HTTP token octets are accepted in first name position");
  check(accepted_name_later == 78u,
        "token octets and the name delimiter are accepted in later position");
  check(accepted_value == 96u,
        "HTAB, SP, and visible ASCII are accepted value byte classes");
  printf("byte classification: 256 values x 5 positions; accepted name-first=%zu, "
         "name-later=%zu, value-class=%zu\n",
         accepted_name_first, accepted_name_later, accepted_value);
}

static void test_limits(void) {
  unsigned char line[OMNI_HTTP_HEADER_LINE_MAX_TOTAL_BYTES + 1u];
  struct omni_http_header_line_result result;

  for (size_t name_length = OMNI_HTTP_HEADER_LINE_MAX_NAME_BYTES - 1u;
       name_length <= OMNI_HTTP_HEADER_LINE_MAX_NAME_BYTES; ++name_length) {
    for (size_t index = 0u; index < name_length; ++index) {
      line[index] = (unsigned char)'N';
    }
    line[name_length] = (unsigned char)':';
    line[name_length + 1u] = (unsigned char)'x';
    line[name_length + 2u] = (unsigned char)'\r';
    line[name_length + 3u] = (unsigned char)'\n';
    result = expect_status(line, name_length + 4u,
                           OMNI_HTTP_HEADER_LINE_COMPLETE,
                           "name limit minus one and exact limit are accepted");
    check(result.name.data == line && result.name.length == name_length,
          "name boundary result returns the exact name span");
  }
  memset(line, 'N', OMNI_HTTP_HEADER_LINE_MAX_NAME_BYTES + 1u);
  line[OMNI_HTTP_HEADER_LINE_MAX_NAME_BYTES + 1u] = (unsigned char)':';
  line[OMNI_HTTP_HEADER_LINE_MAX_NAME_BYTES + 2u] = (unsigned char)'x';
  line[OMNI_HTTP_HEADER_LINE_MAX_NAME_BYTES + 3u] = (unsigned char)'\r';
  line[OMNI_HTTP_HEADER_LINE_MAX_NAME_BYTES + 4u] = (unsigned char)'\n';
  result = expect_status(line, OMNI_HTTP_HEADER_LINE_MAX_NAME_BYTES + 5u,
                         OMNI_HTTP_HEADER_LINE_TOO_LARGE,
                         "name limit plus one is too large");
  check(result.error_offset == OMNI_HTTP_HEADER_LINE_MAX_NAME_BYTES,
        "name limit reports first name byte that cannot fit");

  for (size_t value_length = OMNI_HTTP_HEADER_LINE_MAX_VALUE_BYTES - 1u;
       value_length <= OMNI_HTTP_HEADER_LINE_MAX_VALUE_BYTES; ++value_length) {
    line[0] = (unsigned char)'A';
    line[1] = (unsigned char)':';
    memset(line + 2u, 'V', value_length);
    line[value_length + 2u] = (unsigned char)'\r';
    line[value_length + 3u] = (unsigned char)'\n';
    result = expect_status(line, value_length + 4u,
                           OMNI_HTTP_HEADER_LINE_COMPLETE,
                           "value limit minus one and exact limit are accepted");
    check(result.value.data == line + 2u && result.value.length == value_length,
          "value boundary result returns the exact span");
  }
  line[0] = (unsigned char)'A';
  line[1] = (unsigned char)':';
  memset(line + 2u, 'V', OMNI_HTTP_HEADER_LINE_MAX_VALUE_BYTES + 1u);
  line[OMNI_HTTP_HEADER_LINE_MAX_VALUE_BYTES + 3u] = (unsigned char)'\r';
  line[OMNI_HTTP_HEADER_LINE_MAX_VALUE_BYTES + 4u] = (unsigned char)'\n';
  result = expect_status(line, OMNI_HTTP_HEADER_LINE_MAX_VALUE_BYTES + 5u,
                         OMNI_HTTP_HEADER_LINE_TOO_LARGE,
                         "value limit plus one is too large");
  check(result.error_offset == OMNI_HTTP_HEADER_LINE_MAX_VALUE_BYTES + 2u,
        "value limit reports first retained value byte that cannot fit");

  for (size_t total = OMNI_HTTP_HEADER_LINE_MAX_TOTAL_BYTES - 1u;
       total <= OMNI_HTTP_HEADER_LINE_MAX_TOTAL_BYTES + 1u; ++total) {
    size_t trailing_ows = total - 5u;

    line[0] = (unsigned char)'A';
    line[1] = (unsigned char)':';
    line[2] = (unsigned char)'x';
    memset(line + 3u, ' ', trailing_ows);
    line[total - 2u] = (unsigned char)'\r';
    line[total - 1u] = (unsigned char)'\n';
    result = expect_status(line, total,
                           total <= OMNI_HTTP_HEADER_LINE_MAX_TOTAL_BYTES
                               ? OMNI_HTTP_HEADER_LINE_COMPLETE
                               : OMNI_HTTP_HEADER_LINE_TOO_LARGE,
                           "total line limit counts CRLF and has exact boundary");
    if (total <= OMNI_HTTP_HEADER_LINE_MAX_TOTAL_BYTES) {
      check(result.consumed_bytes == total,
            "total line limit and limit-minus-one consume exact line size");
      check(result.value.data == line + 2u && result.value.length == 1u,
            "total-line trailing OWS does not extend returned value");
    } else {
      check(result.error_offset == OMNI_HTTP_HEADER_LINE_MAX_TOTAL_BYTES,
            "total limit plus one fails at first byte beyond the cap");
    }
  }

  /* All three public bounds can be reached together without ambiguity. */
  memset(line, 'N', OMNI_HTTP_HEADER_LINE_MAX_NAME_BYTES);
  line[OMNI_HTTP_HEADER_LINE_MAX_NAME_BYTES] = (unsigned char)':';
  memset(line + OMNI_HTTP_HEADER_LINE_MAX_NAME_BYTES + 1u, 'V',
         OMNI_HTTP_HEADER_LINE_MAX_VALUE_BYTES);
  line[OMNI_HTTP_HEADER_LINE_MAX_TOTAL_BYTES - 2u] = (unsigned char)'\r';
  line[OMNI_HTTP_HEADER_LINE_MAX_TOTAL_BYTES - 1u] = (unsigned char)'\n';
  result = expect_status(line, OMNI_HTTP_HEADER_LINE_MAX_TOTAL_BYTES,
                         OMNI_HTTP_HEADER_LINE_COMPLETE,
                         "max name and max value fit exact total-line limit");
  check(result.name.length == OMNI_HTTP_HEADER_LINE_MAX_NAME_BYTES &&
            result.value.length == OMNI_HTTP_HEADER_LINE_MAX_VALUE_BYTES,
        "joint maximum spans have their documented lengths");
}

static void test_null_arguments_and_error_offsets(void) {
  static const unsigned char one_byte[] = {'A'};
  static const unsigned char invalid_name[] = {'A', ' ', ':', 'x', '\r', '\n'};
  static const unsigned char invalid_value[] = {'A', ':', 'x', 0u, '\r', '\n'};
  struct omni_http_header_line_result result;

  result = expect_status(NULL, 0u, OMNI_HTTP_HEADER_LINE_INCOMPLETE,
                         "NULL plus zero length is an incomplete prefix");
  check(result.error_offset == 0u, "empty NULL prefix needs byte zero");
  (void)expect_status(NULL, 1u, OMNI_HTTP_HEADER_LINE_ERR_INVALID_ARGUMENT,
                      "NULL plus nonzero length is invalid argument");
  (void)expect_status(one_byte, 0u, OMNI_HTTP_HEADER_LINE_INCOMPLETE,
                      "nonnull pointer plus zero length is incomplete");

  result = expect_status(invalid_name, sizeof(invalid_name),
                         OMNI_HTTP_HEADER_LINE_INVALID,
                         "invalid field-name has deterministic offset");
  check(result.error_offset == 1u, "name syntax offset identifies offending SP");
  result = expect_status(invalid_value, sizeof(invalid_value),
                         OMNI_HTTP_HEADER_LINE_INVALID,
                         "invalid field-value has deterministic offset");
  check(result.error_offset == 3u, "value syntax offset identifies embedded NUL");
}

int main(void) {
  test_valid_lines_and_borrowed_spans();
  test_name_case_preservation();
  test_extra_bytes_and_exact_consumption();
  test_ows_and_empty_values();
  test_colon_syntax_and_blank_line();
  test_invalid_name_and_value_bytes();
  test_line_endings_and_incomplete_prefixes();
  test_every_prefix_of_valid_lines();
  test_deterministic_byte_classification();
  test_limits();
  test_null_arguments_and_error_offsets();

  printf("http-header-line-unit: %zu checks, %zu failures\n", check_count,
         failure_count);
  printf("sizeof(omni_http_header_line_span)=%zu; "
         "sizeof(omni_http_header_line_result)=%zu; "
         "persistent parser state=0 bytes\n",
         sizeof(struct omni_http_header_line_span),
         sizeof(struct omni_http_header_line_result));
  return failure_count == 0u ? 0 : 1;
}
