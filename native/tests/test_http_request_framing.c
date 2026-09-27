/* Task 036: framing semantics over real Task 035 parsed heads. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omniroute/http_request_framing.h"

static size_t checks;
static size_t failures;
static size_t integration_cases;
static size_t byte_cases;
static size_t fold_cases;

static void check(bool condition, const char *name) {
  ++checks;
  if (!condition) {
    ++failures;
    fprintf(stderr, "NOT OK: %s\n", name);
  }
}

static bool append(unsigned char *out, size_t capacity, size_t *used, const unsigned char *bytes,
                   size_t length) {
  if (length > capacity - *used)
    return false;
  if (length != 0u)
    memcpy(out + *used, bytes, length);
  *used += length;
  return true;
}

static bool make_request(unsigned char *out, size_t capacity, size_t *length,
                         const char *const *headers, size_t header_count, const unsigned char *body,
                         size_t body_length) {
  static const unsigned char request_line[] = "POST /x HTTP/1.1\r\n";
  static const unsigned char ending[] = "\r\n";
  size_t used = 0u;
  if (!append(out, capacity, &used, request_line, sizeof(request_line) - 1u))
    return false;
  for (size_t i = 0u; i < header_count; ++i) {
    size_t n = strlen(headers[i]);
    if (!append(out, capacity, &used, (const unsigned char *)headers[i], n) ||
        !append(out, capacity, &used, (const unsigned char *)"\r\n", 2u))
      return false;
  }
  if (!append(out, capacity, &used, ending, 2u) || !append(out, capacity, &used, body, body_length))
    return false;
  *length = used;
  return true;
}

static struct omni_http_request_framing_result analyze_headers(const char *const *headers,
                                                               size_t header_count) {
  unsigned char raw[OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES + 64u];
  struct omni_http_header parsed_headers[OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS];
  struct omni_http_request_head_result head;
  struct omni_http_request_framing_result result;
  size_t length = 0u;
  check(make_request(raw, sizeof(raw), &length, headers, header_count, NULL, 0u),
        "build integration request");
  head =
      omni_http_request_head_parse(raw, length, parsed_headers, OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS);
  check(head.status == OMNI_HTTP_REQUEST_HEAD_COMPLETE, "Task 035 integration parse");
  result = omni_http_request_framing_analyze(&head);
  ++integration_cases;
  return result;
}

static void expect_framing(const char *const *headers, size_t count,
                           enum omni_http_request_framing_status status,
                           enum omni_http_request_body_framing framing, size_t length,
                           size_t fields, bool te, size_t error_index, const char *name) {
  struct omni_http_request_framing_result r = analyze_headers(headers, count);
  check(r.status == status, name);
  check(r.framing == framing && r.content_length == length &&
            r.content_length_field_count == fields && r.transfer_encoding_present == te,
        "framing result fields");
  check(r.error_header_index == error_index, "error header index");
}

static void valid_and_invalid_lengths(void) {
  static const char *zero[] = {"Content-Length: 0"};
  static const char *one[] = {"Content-Length: 1"};
  static const char *nine[] = {"Content-Length: 9"};
  static const char *ten[] = {"Content-Length: 10"};
  static const char *zeros[] = {"Content-Length: 000"};
  static const char *padded[] = {"Content-Length: 00010"};
  static const char *invalid[] = {"",    "+",    "+1",  "-1",     "1.0",
                                  "1e3", "0x10", "abc", "\"10\"", "1 0"};
  unsigned char line[128];
  char header[256];
  struct omni_http_request_framing_result r;

  expect_framing(NULL, 0u, OMNI_HTTP_REQUEST_FRAMING_OK, OMNI_HTTP_REQUEST_BODY_NONE, 0u, 0u, false,
                 OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX, "POST with no framing fields");
  {
    static const unsigned char get[] = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
    struct omni_http_header get_headers[1];
    struct omni_http_request_head_result get_head =
        omni_http_request_head_parse(get, sizeof(get) - 1u, get_headers, 1u);
    struct omni_http_request_framing_result get_result =
        omni_http_request_framing_analyze(&get_head);
    check(get_head.status == OMNI_HTTP_REQUEST_HEAD_COMPLETE &&
              get_result.status == OMNI_HTTP_REQUEST_FRAMING_OK &&
              get_result.framing == OMNI_HTTP_REQUEST_BODY_NONE,
          "GET Host without framing is no body");
  }
  {
    const char *host[] = {"Host: a"};
    expect_framing(host, 1u, OMNI_HTTP_REQUEST_FRAMING_OK, OMNI_HTTP_REQUEST_BODY_NONE, 0u, 0u,
                   false, OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX,
                   "POST without framing is no body");
  }
  expect_framing(zero, 1u, OMNI_HTTP_REQUEST_FRAMING_OK, OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH, 0u,
                 1u, false, OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX,
                 "explicit zero remains fixed length");
  expect_framing(one, 1u, OMNI_HTTP_REQUEST_FRAMING_OK, OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH, 1u, 1u,
                 false, OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX, "decimal one");
  expect_framing(nine, 1u, OMNI_HTTP_REQUEST_FRAMING_OK, OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH, 9u,
                 1u, false, OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX, "decimal nine");
  expect_framing(ten, 1u, OMNI_HTTP_REQUEST_FRAMING_OK, OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH, 10u,
                 1u, false, OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX, "decimal ten");
  expect_framing(zeros, 1u, OMNI_HTTP_REQUEST_FRAMING_OK, OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH, 0u,
                 1u, false, OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX, "leading zero decimal");
  expect_framing(padded, 1u, OMNI_HTTP_REQUEST_FRAMING_OK, OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH, 10u,
                 1u, false, OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX, "padded decimal value");

  for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    (void)snprintf(header, sizeof(header), "Content-Length: %s", invalid[i]);
    {
      const char *fields[] = {header};
      r = analyze_headers(fields, 1u);
    }
    check(r.status == OMNI_HTTP_REQUEST_FRAMING_INVALID_CONTENT_LENGTH,
          "invalid Content-Length decimal/list grammar");
    check(r.error_header_index == 0u && r.content_length_field_count == 1u,
          "invalid Content-Length location/count");
  }

  /* Generate SIZE_MAX and SIZE_MAX + 1 as decimal without overflowing size_t. */
  {
    char max_text[sizeof(size_t) * 3u + 1u];
    char overflow_text[sizeof(size_t) * 3u + 2u];
    size_t n = 0u;
    size_t value = SIZE_MAX;
    char reversed[sizeof(size_t) * 3u + 1u];
    do {
      reversed[n++] = (char)('0' + (value % 10u));
      value /= 10u;
    } while (value != 0u);
    for (size_t i = 0u; i < n; ++i)
      max_text[i] = reversed[n - i - 1u];
    max_text[n] = '\0';
    memcpy(overflow_text, max_text, n + 1u);
    {
      size_t pos = n;
      while (pos != 0u && overflow_text[pos - 1u] == '9') {
        overflow_text[pos - 1u] = '0';
        --pos;
      }
      if (pos == 0u) {
        memmove(overflow_text + 1u, overflow_text, n + 1u);
        overflow_text[0] = '1';
      } else {
        ++overflow_text[pos - 1u];
      }
    }
    (void)snprintf(header, sizeof(header), "Content-Length: %s", max_text);
    {
      const char *fields[] = {header};
      r = analyze_headers(fields, 1u);
    }
    check(r.status == OMNI_HTTP_REQUEST_FRAMING_OK &&
              r.framing == OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH && r.content_length == SIZE_MAX,
          "SIZE_MAX decimal accepted");
    (void)snprintf(header, sizeof(header), "Content-Length: %s", overflow_text);
    {
      const char *fields[] = {header};
      r = analyze_headers(fields, 1u);
    }
    check(r.status == OMNI_HTTP_REQUEST_FRAMING_INVALID_CONTENT_LENGTH &&
              r.error_header_index == 0u,
          "SIZE_MAX plus one rejected");
    memset(line, '9', sizeof(line) - 1u);
    line[sizeof(line) - 1u] = '\0';
    (void)snprintf(header, sizeof(header), "Content-Length: %s", line);
    {
      const char *fields[] = {header};
      r = analyze_headers(fields, 1u);
    }
    check(r.status == OMNI_HTTP_REQUEST_FRAMING_INVALID_CONTENT_LENGTH,
          "long decimal overflow rejected");
  }
}

static void duplicates_and_lists(void) {
  static const char *equal[] = {"Content-Length: 10", "Content-Length: 10"};
  static const char *different_repr[] = {"Content-Length: 010", "Content-Length: 10"};
  static const char *conflict[] = {"Content-Length: 10", "Content-Length: 11"};
  static const char *zero_conflict[] = {"Content-Length: 0", "Content-Length: 1"};
  static const char *leading_conflict[] = {"Content-Length: 001", "Content-Length: 2"};
  static const char *same_combo[] = {"Content-Length: 10, 10"};
  static const char *list_forms[][1] = {
      {"Content-Length: 10,10"},   {"Content-Length: 10, 10"},  {"Content-Length: 10 ,10"},
      {"Content-Length: 10 , 10"}, {"Content-Length: 10,\t10"}, {"Content-Length: 010,10,0010"}};
  static const char *bad_lists[][1] = {{"Content-Length: 10,11"},   {"Content-Length: 10,"},
                                       {"Content-Length: ,10"},     {"Content-Length: 10,,10"},
                                       {"Content-Length: 10, abc"}, {"Content-Length: 10,+10"},
                                       {"Content-Length: 10,1 0"}};
  static const char *combo_duplicate[] = {"Content-Length: 10,10", "Content-Length: 010"};
  static const char *combo_conflict[] = {"Content-Length: 10,10", "Content-Length: 11"};
  struct omni_http_request_framing_result r;

  expect_framing(equal, 2u, OMNI_HTTP_REQUEST_FRAMING_OK, OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH, 10u,
                 2u, false, OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX, "equal duplicate lengths");
  expect_framing(different_repr, 2u, OMNI_HTTP_REQUEST_FRAMING_OK,
                 OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH, 10u, 2u, false,
                 OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX, "equal duplicate numeric values");
  for (size_t i = 0u; i < 3u; ++i) {
    const char *const *items = i == 0u ? conflict : (i == 1u ? zero_conflict : leading_conflict);
    r = analyze_headers(items, 2u);
    check(r.status == OMNI_HTTP_REQUEST_FRAMING_CONFLICTING_CONTENT_LENGTH,
          "conflicting duplicate lengths");
    check(r.error_header_index == 1u && r.content_length_field_count == 2u,
          "conflict points to later field");
  }
  for (size_t i = 0u; i < sizeof(list_forms) / sizeof(list_forms[0]); ++i) {
    r = analyze_headers(list_forms[i], 1u);
    check(r.status == OMNI_HTTP_REQUEST_FRAMING_OK &&
              r.framing == OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH && r.content_length == 10u &&
              r.content_length_field_count == 1u,
          "coalesced equal list form");
  }
  for (size_t i = 0u; i < sizeof(bad_lists) / sizeof(bad_lists[0]); ++i) {
    r = analyze_headers(bad_lists[i], 1u);
    check(r.status == (i == 0u ? OMNI_HTTP_REQUEST_FRAMING_CONFLICTING_CONTENT_LENGTH
                               : OMNI_HTTP_REQUEST_FRAMING_INVALID_CONTENT_LENGTH) &&
              r.error_header_index == 0u,
          "invalid or conflicting coalesced list");
  }
  expect_framing(combo_duplicate, 2u, OMNI_HTTP_REQUEST_FRAMING_OK,
                 OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH, 10u, 2u, false,
                 OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX, "duplicate plus coalesced equal");
  r = analyze_headers(combo_conflict, 2u);
  check(r.status == OMNI_HTTP_REQUEST_FRAMING_CONFLICTING_CONTENT_LENGTH &&
            r.error_header_index == 1u,
        "duplicate plus coalesced conflict");
  (void)same_combo;
}

static void names_and_transfer_encoding(void) {
  static const char *cl_cases[] = {"Content-Length: 7", "content-length: 7", "CONTENT-LENGTH: 7",
                                   "CoNtEnT-LeNgTh: 7"};
  static const char *te_cases[] = {
      "Transfer-Encoding: chunked",       "transfer-encoding: gzip",
      "TRANSFER-ENCODING: gzip, chunked", "TrAnSfEr-EnCoDiNg: identity",
      "Transfer-Encoding: x-custom",      "Transfer-Encoding:"};
  static const char *similar[] = {
      "X-Content-Length: 7",       "Content-Length-X: 7",          "Content_Length: 7",
      "ContentLength: 7",          "X-Transfer-Encoding: chunked", "Transfer-Encoding-X: chunked",
      "Transfer_Encoding: chunked"};
  struct omni_http_request_framing_result r;
  for (size_t i = 0u; i < sizeof(cl_cases) / sizeof(cl_cases[0]); ++i) {
    const char *fields[] = {cl_cases[i]};
    r = analyze_headers(fields, 1u);
    check(r.status == OMNI_HTTP_REQUEST_FRAMING_OK && r.content_length == 7u &&
              r.content_length_field_count == 1u,
          "case-insensitive Content-Length");
  }
  for (size_t i = 0u; i < sizeof(te_cases) / sizeof(te_cases[0]); ++i) {
    const char *fields[] = {te_cases[i]};
    r = analyze_headers(fields, 1u);
    check(r.status == OMNI_HTTP_REQUEST_FRAMING_TRANSFER_ENCODING_UNSUPPORTED &&
              r.transfer_encoding_present && r.framing == OMNI_HTTP_REQUEST_BODY_NONE &&
              r.error_header_index == 0u,
          "Transfer-Encoding detected but unsupported");
  }
  for (size_t i = 0u; i < sizeof(similar) / sizeof(similar[0]); ++i) {
    const char *fields[] = {similar[i]};
    r = analyze_headers(fields, 1u);
    check(r.status == OMNI_HTTP_REQUEST_FRAMING_OK && r.framing == OMNI_HTTP_REQUEST_BODY_NONE &&
              r.content_length_field_count == 0u && !r.transfer_encoding_present,
          "similar unrelated header name ignored");
  }
  {
    const char *multiple_te[] = {"Transfer-Encoding: gzip", "transfer-encoding: chunked"};
    r = analyze_headers(multiple_te, 2u);
    check(r.status == OMNI_HTTP_REQUEST_FRAMING_TRANSFER_ENCODING_UNSUPPORTED &&
              r.transfer_encoding_present,
          "multiple Transfer-Encoding fields remain unsupported");
  }
  {
    const char *both_a[] = {"Content-Length: 7", "Transfer-Encoding: chunked"};
    const char *both_b[] = {"Transfer-Encoding: chunked", "Content-Length: 7"};
    const char *malformed_a[] = {"Content-Length: +7", "Transfer-Encoding: chunked"};
    const char *malformed_b[] = {"Transfer-Encoding: chunked", "Content-Length: +7"};
    r = analyze_headers(both_a, 2u);
    check(r.status == OMNI_HTTP_REQUEST_FRAMING_AMBIGUOUS && r.transfer_encoding_present &&
              r.content_length_field_count == 1u && r.error_header_index == 0u,
          "TE and CL ambiguity in CL-first order");
    r = analyze_headers(both_b, 2u);
    check(r.status == OMNI_HTTP_REQUEST_FRAMING_AMBIGUOUS && r.transfer_encoding_present &&
              r.content_length_field_count == 1u && r.error_header_index == 1u,
          "TE and CL ambiguity in TE-first order");
    r = analyze_headers(malformed_a, 2u);
    check(r.status == OMNI_HTTP_REQUEST_FRAMING_INVALID_CONTENT_LENGTH &&
              r.error_header_index == 0u,
          "malformed CL precedence with TE");
    r = analyze_headers(malformed_b, 2u);
    check(r.status == OMNI_HTTP_REQUEST_FRAMING_INVALID_CONTENT_LENGTH &&
              r.error_header_index == 1u,
          "malformed CL precedence independent of order");
  }
  {
    const char *unrelated[] = {"Host: a", "Host: b", "X-Test: 1", "X-Test: 2", "Accept: */*"};
    expect_framing(unrelated, 5u, OMNI_HTTP_REQUEST_FRAMING_OK, OMNI_HTTP_REQUEST_BODY_NONE, 0u, 0u,
                   false, OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX,
                   "unrelated duplicates ignored");
  }
}

static void precedence_and_contract(void) {
  const char *conflict_malformed[] = {"Content-Length: 10", "Content-Length: 11",
                                      "Content-Length: 1 0"};
  const char *conflict_te[] = {"Content-Length: 10", "Content-Length: 11",
                               "Transfer-Encoding: chunked"};
  const char *cl_te[] = {"Content-Length: 10", "Transfer-Encoding: chunked"};
  struct omni_http_header fields[2];
  struct omni_http_request_head_result head;
  struct omni_http_request_head_result bad;
  struct omni_http_request_framing_result r;
  unsigned char raw[128];
  size_t length;

  r = analyze_headers(conflict_malformed, 3u);
  check(r.status == OMNI_HTTP_REQUEST_FRAMING_INVALID_CONTENT_LENGTH && r.error_header_index == 2u,
        "invalid Content-Length outranks prior conflict");
  r = analyze_headers(conflict_te, 3u);
  check(r.status == OMNI_HTTP_REQUEST_FRAMING_CONFLICTING_CONTENT_LENGTH &&
            r.error_header_index == 1u,
        "conflict outranks TE ambiguity");

  (void)make_request(raw, sizeof(raw), &length, cl_te, 2u, NULL, 0u);
  head = omni_http_request_head_parse(raw, length, fields, 2u);
  bad = head;
  bad.status = OMNI_HTTP_REQUEST_HEAD_INCOMPLETE;
  check(omni_http_request_framing_analyze(&bad).status == OMNI_HTTP_REQUEST_FRAMING_INVALID_HEAD,
        "incomplete Task 035 head rejected");
  bad = head;
  bad.headers = NULL;
  check(omni_http_request_framing_analyze(&bad).status == OMNI_HTTP_REQUEST_FRAMING_INVALID_HEAD,
        "nonzero count without header view rejected");
  bad = head;
  bad.header_count = OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS + 1u;
  check(omni_http_request_framing_analyze(&bad).status == OMNI_HTTP_REQUEST_FRAMING_INVALID_HEAD,
        "header count above Task 035 bound rejected");
  check(omni_http_request_framing_analyze(NULL).status ==
            OMNI_HTTP_REQUEST_FRAMING_INVALID_ARGUMENT,
        "NULL head rejected");
  bad = head;
  bad.request_line.status = OMNI_HTTP_REQUEST_LINE_INVALID;
  check(omni_http_request_framing_analyze(&bad).status == OMNI_HTTP_REQUEST_FRAMING_INVALID_HEAD,
        "inconsistent request line rejected");
  bad = head;
  bad.request_line.target.length = OMNI_HTTP_REQUEST_LINE_MAX_TARGET_BYTES + 1u;
  check(omni_http_request_framing_analyze(&bad).status == OMNI_HTTP_REQUEST_FRAMING_INVALID_HEAD,
        "request-line span beyond Task 033 bound rejected");
  bad = head;
  bad.error_offset = 1u;
  check(omni_http_request_framing_analyze(&bad).status == OMNI_HTTP_REQUEST_FRAMING_INVALID_HEAD,
        "nonzero successful head error offset rejected");

  {
    const char *nonempty[] = {"Content-Length: 1"};
    static const unsigned char leading_ows[] = " 1";
    static const unsigned char trailing_ows[] = "1 ";
    (void)make_request(raw, sizeof(raw), &length, nonempty, 1u, NULL, 0u);
    head = omni_http_request_head_parse(raw, length, fields, 2u);
    check(head.status == OMNI_HTTP_REQUEST_HEAD_COMPLETE, "strict decimal base head");
    bad = head;
    fields[0].value.data = leading_ows;
    fields[0].value.length = sizeof(leading_ows) - 1u;
    check(omni_http_request_framing_analyze(&bad).status ==
              OMNI_HTTP_REQUEST_FRAMING_INVALID_CONTENT_LENGTH,
          "leading OWS outside a comma list rejected");
    fields[0].value.data = trailing_ows;
    fields[0].value.length = sizeof(trailing_ows) - 1u;
    check(omni_http_request_framing_analyze(&bad).status ==
              OMNI_HTTP_REQUEST_FRAMING_INVALID_CONTENT_LENGTH,
          "trailing OWS outside a comma list rejected");
  }
  {
    const char *nonempty[] = {"Content-Length: 10"};
    (void)make_request(raw, sizeof(raw), &length, nonempty, 1u, NULL, 0u);
    head = omni_http_request_head_parse(raw, length, fields, 2u);
    bad = head;
    bad.headers = NULL;
    bad.header_count = 0u;
    check(omni_http_request_framing_analyze(&bad).status == OMNI_HTTP_REQUEST_FRAMING_OK &&
              omni_http_request_framing_analyze(&bad).framing == OMNI_HTTP_REQUEST_BODY_NONE,
          "consistent zero-header result accepted");
    fields[0].name.data = NULL;
    check(omni_http_request_framing_analyze(&head).status == OMNI_HTTP_REQUEST_FRAMING_INVALID_HEAD,
          "invalid borrowed span shape rejected");
  }
}

static void maximum_headers(void) {
  for (size_t position = 0u; position < 2u; ++position) {
    unsigned char raw[1024];
    struct omni_http_header headers[OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS];
    struct omni_http_request_head_result head;
    struct omni_http_request_framing_result r;
    size_t used = 0u;
    const char *line = "POST /x HTTP/1.1\r\n";
    (void)append(raw, sizeof(raw), &used, (const unsigned char *)line, strlen(line));
    for (size_t i = 0u; i < OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS; ++i) {
      const char *field = ((position == 0u && i == 0u) ||
                           (position == 1u && i == OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS - 1u))
                              ? "Content-Length: 5\r\n"
                              : "X: a\r\n";
      (void)append(raw, sizeof(raw), &used, (const unsigned char *)field, strlen(field));
    }
    (void)append(raw, sizeof(raw), &used, (const unsigned char *)"\r\n", 2u);
    head = omni_http_request_head_parse(raw, used, headers, OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS);
    check(head.status == OMNI_HTTP_REQUEST_HEAD_COMPLETE &&
              head.header_count == OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS,
          "Task 035 maximum header count parsed");
    r = omni_http_request_framing_analyze(&head);
    check(r.status == OMNI_HTTP_REQUEST_FRAMING_OK &&
              r.framing == OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH && r.content_length == 5u,
          "framing at first/last maximum header position");
  }
}

static void ascii_case_and_bytes(void) {
  static const char cl_name[] = "Content-Length";
  static const char te_name[] = "Transfer-Encoding";
  unsigned char raw[128];
  struct omni_http_header parsed_headers[1];
  struct omni_http_request_head_result base;
  size_t raw_length;
  const char *header[] = {"Content-Length: 3"};
  (void)make_request(raw, sizeof(raw), &raw_length, header, 1u, NULL, 0u);
  base = omni_http_request_head_parse(raw, raw_length, parsed_headers, 1u);
  check(base.status == OMNI_HTTP_REQUEST_HEAD_COMPLETE, "case fold base head");

  /* Toggle each ASCII letter independently, then all-lower/all-upper names. */
  for (size_t i = 0u; i < sizeof(cl_name) - 1u; ++i) {
    if (!((cl_name[i] >= 'A' && cl_name[i] <= 'Z') || (cl_name[i] >= 'a' && cl_name[i] <= 'z')))
      continue;
    struct omni_http_header changed_header = parsed_headers[0];
    unsigned char changed_name[32];
    struct omni_http_request_head_result changed_head = base;
    memcpy(changed_name, parsed_headers[0].name.data, parsed_headers[0].name.length);
    if (changed_name[i] >= (unsigned char)'A' && changed_name[i] <= (unsigned char)'Z')
      changed_name[i] = (unsigned char)(changed_name[i] + ('a' - 'A'));
    else if (changed_name[i] >= (unsigned char)'a' && changed_name[i] <= (unsigned char)'z')
      changed_name[i] = (unsigned char)(changed_name[i] - ('a' - 'A'));
    changed_header.name.data = changed_name;
    changed_head.headers = &changed_header;
    check(omni_http_request_framing_analyze(&changed_head).status == OMNI_HTTP_REQUEST_FRAMING_OK &&
              omni_http_request_framing_analyze(&changed_head).content_length == 3u,
          "single-letter Content-Length case toggle");
    ++fold_cases;
  }
  for (unsigned int byte = 0u; byte < 256u; ++byte) {
    struct omni_http_header changed_header = parsed_headers[0];
    unsigned char changed_name[32];
    struct omni_http_request_head_result changed_head = base;
    bool matches;
    memcpy(changed_name, parsed_headers[0].name.data, parsed_headers[0].name.length);
    changed_name[0] = (unsigned char)byte;
    changed_header.name.data = changed_name;
    changed_head.headers = &changed_header;
    matches = (byte == (unsigned int)'C' || byte == (unsigned int)'c');
    check(
        (omni_http_request_framing_analyze(&changed_head).status == OMNI_HTTP_REQUEST_FRAMING_OK &&
         omni_http_request_framing_analyze(&changed_head).content_length_field_count == 1u) ==
            matches,
        "ASCII-only first-name-byte fold");
    ++byte_cases;
  }

  {
    const char *te_field[] = {"Transfer-Encoding: chunked"};
    struct omni_http_header te_headers[1];
    struct omni_http_request_head_result te_base;
    (void)make_request(raw, sizeof(raw), &raw_length, te_field, 1u, NULL, 0u);
    te_base = omni_http_request_head_parse(raw, raw_length, te_headers, 1u);
    check(te_base.status == OMNI_HTTP_REQUEST_HEAD_COMPLETE, "TE case-fold base head");
    for (size_t i = 0u; i < sizeof(te_name) - 1u; ++i) {
      if (!((te_name[i] >= 'A' && te_name[i] <= 'Z') || (te_name[i] >= 'a' && te_name[i] <= 'z')))
        continue;
      struct omni_http_header changed_header = te_headers[0];
      unsigned char changed_name[32];
      struct omni_http_request_head_result changed_head = te_base;
      memcpy(changed_name, te_headers[0].name.data, te_headers[0].name.length);
      if (changed_name[i] >= (unsigned char)'A' && changed_name[i] <= (unsigned char)'Z')
        changed_name[i] = (unsigned char)(changed_name[i] + ('a' - 'A'));
      else if (changed_name[i] >= (unsigned char)'a' && changed_name[i] <= (unsigned char)'z')
        changed_name[i] = (unsigned char)(changed_name[i] - ('a' - 'A'));
      changed_header.name.data = changed_name;
      changed_head.headers = &changed_header;
      check(omni_http_request_framing_analyze(&changed_head).status ==
                OMNI_HTTP_REQUEST_FRAMING_TRANSFER_ENCODING_UNSUPPORTED,
            "single-letter Transfer-Encoding case toggle");
      ++fold_cases;
    }
    for (size_t position = 0u; position < 2u; ++position) {
      for (unsigned int byte = 0u; byte < 256u; ++byte) {
        struct omni_http_header changed_header = te_headers[0];
        unsigned char changed_name[32];
        struct omni_http_request_head_result changed_head = te_base;
        struct omni_http_request_framing_result changed_result;
        memcpy(changed_name, te_headers[0].name.data, te_headers[0].name.length);
        changed_name[position == 0u ? 0u : 8u] = (unsigned char)byte;
        changed_header.name.data = changed_name;
        changed_head.headers = &changed_header;
        changed_result = omni_http_request_framing_analyze(&changed_head);
        check((changed_result.status == OMNI_HTTP_REQUEST_FRAMING_TRANSFER_ENCODING_UNSUPPORTED) ==
                  (position == 0u ? (byte == (unsigned int)'T' || byte == (unsigned int)'t')
                                  : byte == (unsigned int)'-'),
              "ASCII-only Transfer-Encoding comparison");
        ++byte_cases;
      }
    }
  }

  /* Decimal, list separator, and OWS positions exercise all 256 octets. */
  {
    const char *list[] = {"Content-Length: 1,1"};
    struct omni_http_header h[1];
    struct omni_http_request_head_result list_head;
    (void)make_request(raw, sizeof(raw), &raw_length, list, 1u, NULL, 0u);
    list_head = omni_http_request_head_parse(raw, raw_length, h, 1u);
    check(list_head.status == OMNI_HTTP_REQUEST_HEAD_COMPLETE, "list mutation base head");
    for (size_t position = 0u; position < 4u; ++position) {
      for (unsigned int byte = 0u; byte < 256u; ++byte) {
        unsigned char value[4] = {(unsigned char)'1', (unsigned char)',', (unsigned char)' ',
                                  (unsigned char)'1'};
        struct omni_http_header field = h[0];
        struct omni_http_request_head_result mutated = list_head;
        struct omni_http_request_framing_result result;
        if (position == 0u)
          value[0] = (unsigned char)byte;
        else if (position == 1u)
          value[1] = (unsigned char)byte;
        else if (position == 2u)
          value[2] = (unsigned char)byte;
        else
          value[3] = (unsigned char)byte;
        field.value.data = value;
        field.value.length = 4u;
        mutated.headers = &field;
        result = omni_http_request_framing_analyze(&mutated);
        if (position == 0u || position == 3u) {
          enum omni_http_request_framing_status expected =
              byte < (unsigned int)'0' || byte > (unsigned int)'9'
                  ? OMNI_HTTP_REQUEST_FRAMING_INVALID_CONTENT_LENGTH
                  : (byte == (unsigned int)'1'
                         ? OMNI_HTTP_REQUEST_FRAMING_OK
                         : OMNI_HTTP_REQUEST_FRAMING_CONFLICTING_CONTENT_LENGTH);
          check(result.status == expected, "all octets at decimal digit position");
        } else if (position == 1u) {
          check((result.status == OMNI_HTTP_REQUEST_FRAMING_OK) == (byte == (unsigned int)','),
                "all octets at comma position");
        } else {
          check((result.status == OMNI_HTTP_REQUEST_FRAMING_OK) ==
                    (byte == (unsigned int)' ' || byte == (unsigned int)'\t' ||
                     byte == (unsigned int)'0'),
                "all octets at list OWS/member start position");
        }
        ++byte_cases;
      }
    }
  }
}

static void body_untouched_and_immutability(void) {
  static const unsigned char body[] = {'A', 0u, 'B', 0xffu, 'C'};
  const char *headers[] = {"Content-Length: 5"};
  unsigned char raw[128];
  unsigned char raw_snapshot[128];
  struct omni_http_header headers_out[1];
  struct omni_http_header headers_snapshot[1];
  struct omni_http_request_head_result head;
  struct omni_http_request_head_result head_snapshot;
  struct omni_http_request_framing_result result;
  size_t length = 0u;
  check(make_request(raw, sizeof(raw), &length, headers, 1u, body, sizeof(body)),
        "build raw body request");
  memcpy(raw_snapshot, raw, sizeof(raw));
  head = omni_http_request_head_parse(raw, length, headers_out, 1u);
  check(head.status == OMNI_HTTP_REQUEST_HEAD_COMPLETE && head.consumed_bytes < length &&
            head.consumed_bytes + sizeof(body) == length,
        "Task 035 stops before body");
  memcpy(headers_snapshot, headers_out, sizeof(headers_out));
  memcpy(&head_snapshot, &head, sizeof(head));
  result = omni_http_request_framing_analyze(&head);
  check(result.status == OMNI_HTTP_REQUEST_FRAMING_OK &&
            result.framing == OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH && result.content_length == 5u,
        "body length classified without body access");
  check(memcmp(raw, raw_snapshot, length) == 0, "raw bytes including binary body unchanged");
  check(memcmp(headers_out, headers_snapshot, sizeof(headers_out)) == 0,
        "Task 035 header storage unchanged");
  check(memcmp(&head, &head_snapshot, sizeof(head)) == 0, "Task 035 result unchanged");
}

int main(void) {
  valid_and_invalid_lengths();
  duplicates_and_lists();
  names_and_transfer_encoding();
  precedence_and_contract();
  maximum_headers();
  ascii_case_and_bytes();
  body_untouched_and_immutability();
  printf("http-request-framing: %zu checks, %zu failures\n", checks, failures);
  printf("coverage: %zu framing integration cases, %zu byte cases, %zu case-fold toggles\n",
         integration_cases, byte_cases, fold_cases);
  printf("memory: result=%zu bytes, persistent=0 bytes, additional working memory=O(1)\n",
         sizeof(struct omni_http_request_framing_result));
  return failures == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
