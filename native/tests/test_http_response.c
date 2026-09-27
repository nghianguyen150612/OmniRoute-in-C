/*
 * OmniRoute native backend — bounded HTTP response-message view tests (Task
 * 042). The suite proves zero-copy composition, metadata-only validation,
 * opaque binary bodies, checked sizing, and the borrowed lifetime contract.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "omniroute/http_response.h"

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
  struct omni_http_response_header header;

  header.name = (const unsigned char *)name;
  header.name_length = strlen(name);
  header.value = (const unsigned char *)value;
  header.value_length = strlen(value);
  return header;
}

static struct omni_http_response_head_result
build_head(unsigned int status_code, const char *reason,
           const struct omni_http_response_header *headers, size_t header_count,
           unsigned char *output, size_t output_capacity) {
  return omni_http_response_head_build(
      status_code, (const unsigned char *)reason, strlen(reason), headers,
      header_count, output, output_capacity);
}

static void check_empty_result(struct omni_http_response_result result,
                               enum omni_http_response_status status,
                               const char *name) {
  check(result.status == status, name);
  check(result.head.data == NULL && result.head.length == 0u &&
            result.body.data == NULL && result.body.length == 0u &&
            result.total_bytes == 0u,
        "failure returns one deterministic empty view");
}

static struct omni_http_response_result
compose_success(unsigned int status_code, const char *reason,
                const struct omni_http_response_header *headers,
                size_t header_count, unsigned char *head_output,
                size_t head_capacity, const unsigned char *body,
                size_t body_length) {
  const struct omni_http_response_head_result head_result = build_head(
      status_code, reason, headers, header_count, head_output, head_capacity);

  check(head_result.status == OMNI_HTTP_RESPONSE_HEAD_OK,
        "Task 041 real build succeeds before composition");
  return omni_http_response_view(head_output, head_capacity, &head_result, body,
                                 body_length);
}

static void test_real_zero_body_integration(void) {
  static const unsigned char expected_200[] = "HTTP/1.1 200 OK\r\n\r\n";
  static const unsigned char expected_204[] = "HTTP/1.1 204 No Content\r\n\r\n";
  unsigned char head_200[128];
  unsigned char head_204[128];
  struct omni_http_response_head_result head_result;
  struct omni_http_response_result result;

  head_result = build_head(200u, "OK", NULL, 0u, head_200, sizeof(head_200));
  result = omni_http_response_view(head_200, sizeof(head_200), &head_result,
                                   NULL, 0u);
  check(result.status == OMNI_HTTP_RESPONSE_OK,
        "real Task 041 200 head composes with zero body");
  check(result.head.data == head_200 &&
            result.head.length == sizeof(expected_200) - 1u &&
            memcmp(result.head.data, expected_200, result.head.length) == 0,
        "200 head span is exact and borrowed");
  check(result.body.data == NULL && result.body.length == 0u &&
            result.total_bytes == result.head.length,
        "200 zero body has empty span and head-only total");

  head_result =
      build_head(204u, "No Content", NULL, 0u, head_204, sizeof(head_204));
  result = omni_http_response_view(head_204, sizeof(head_204), &head_result,
                                   NULL, 0u);
  check(result.status == OMNI_HTTP_RESPONSE_OK && result.body.data == NULL &&
            result.body.length == 0u &&
            result.total_bytes == sizeof(expected_204) - 1u,
        "204 uses ordinary zero-body semantics without status special-casing");
}

static void test_text_binary_and_opaque_bodies(void) {
  static const unsigned char text_body[] = {'h', 'e', 'l', 'l', 'o'};
  static const unsigned char binary_body[] = {0x00u, 0x01u, 0x7fu, 0x80u,
                                              0xffu};
  static const unsigned char http_like_body[] =
      "HTTP/1.1 500 Fake\r\nContent-Length: 999\r\n\r\n";
  unsigned char all_bytes[256];
  unsigned char head[256];
  struct omni_http_response_header headers[2];
  struct omni_http_response_result result;
  size_t index;

  headers[0] = make_header("Content-Length", "5");
  headers[1] = make_header("Content-Type", "text/plain");
  result = compose_success(200u, "OK", headers, 2u, head, sizeof(head),
                           text_body, sizeof(text_body));
  check(result.status == OMNI_HTTP_RESPONSE_OK &&
            result.body.data == text_body &&
            result.body.length == sizeof(text_body) &&
            result.total_bytes == result.head.length + sizeof(text_body),
        "text body is composed without copying");

  headers[0] = make_header("Content-Length", "100");
  result = compose_success(200u, "OK", headers, 1u, head, sizeof(head),
                           (const unsigned char *)"abc", 3u);
  check(result.status == OMNI_HTTP_RESPONSE_OK && result.body.length == 3u &&
            result.body.data[0] == (unsigned char)'a',
        "Content-Length mismatch remains outside Task 042 policy");

  result = compose_success(200u, "OK", NULL, 0u, head, sizeof(head),
                           binary_body, sizeof(binary_body));
  check(result.status == OMNI_HTTP_RESPONSE_OK &&
            result.body.data == binary_body &&
            result.body.length == sizeof(binary_body) &&
            result.body.data[0] == 0x00u && result.body.data[4] == 0xffu,
        "binary body preserves embedded NUL and high bytes");

  for (index = 0u; index < sizeof(all_bytes); ++index) {
    all_bytes[index] = (unsigned char)index;
  }
  result = compose_success(200u, "OK", NULL, 0u, head, sizeof(head), all_bytes,
                           sizeof(all_bytes));
  check(result.status == OMNI_HTTP_RESPONSE_OK &&
            result.body.data == all_bytes &&
            result.body.length == sizeof(all_bytes),
        "all 256 byte values remain one opaque borrowed span");
  check(memcmp(result.body.data, all_bytes, sizeof(all_bytes)) == 0,
        "all 256 body bytes remain unchanged");

  result = compose_success(200u, "OK", NULL, 0u, head, sizeof(head),
                           http_like_body, sizeof(http_like_body) - 1u);
  check(result.status == OMNI_HTTP_RESPONSE_OK &&
            result.body.data == http_like_body &&
            result.body.length == sizeof(http_like_body) - 1u,
        "HTTP-looking body bytes remain opaque data");
}

static void test_storage_identity_and_capacity(void) {
  static const unsigned char static_body[] = {0xa5u, 0x00u, 0xffu};
  unsigned char head[512];
  unsigned char head_snapshot[sizeof(head)];
  unsigned char body[] = {'s', 't', 'a', 'c', 'k'};
  unsigned char body_snapshot[sizeof(body)];
  unsigned char sentinel_body = 0x5au;
  struct omni_http_response_head_result head_result;
  struct omni_http_response_result result;
  size_t index;

  memset(head, 0xa5, sizeof(head));
  head_result = build_head(200u, "OK", NULL, 0u, head, sizeof(head));
  for (index = head_result.written_bytes; index < sizeof(head); ++index) {
    head[index] = 0xc7u;
  }
  memcpy(head_snapshot, head, sizeof(head));
  memcpy(body_snapshot, body, sizeof(body));
  result = omni_http_response_view(head, sizeof(head), &head_result, body,
                                   sizeof(body));
  check(result.status == OMNI_HTTP_RESPONSE_OK && result.head.data == head &&
            result.body.data == body,
        "head and body pointers retain exact caller identity");
  check(result.head.length == head_result.written_bytes &&
            result.total_bytes == head_result.written_bytes + sizeof(body) &&
            head[result.head.length] == 0xc7u,
        "unused head capacity is excluded from the view and total");
  check(memcmp(head, head_snapshot, sizeof(head)) == 0 &&
            memcmp(body, body_snapshot, sizeof(body)) == 0,
        "composition does not mutate head or body storage");

  result = compose_success(200u, "OK", NULL, 0u, head, sizeof(head),
                           static_body, sizeof(static_body));
  check(result.status == OMNI_HTTP_RESPONSE_OK &&
            result.body.data == static_body &&
            result.body.length == sizeof(static_body),
        "static const body storage is borrowed without ownership");

  result = compose_success(200u, "OK", NULL, 0u, head, sizeof(head),
                           &sentinel_body, 1u);
  check(result.status == OMNI_HTTP_RESPONSE_OK &&
            result.body.data == &sentinel_body,
        "stack body storage is borrowed during its valid lifetime");
}

static void test_failed_head_results_are_rejected(void) {
  static unsigned char large_value[OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES];
  static unsigned char head[OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES];
  static const unsigned char sentinel = 0x3cu;
  struct omni_http_response_header header;
  struct omni_http_response_head_result failed;
  struct omni_http_response_head_result valid;
  struct omni_http_response_result result;

  memset(large_value, 'a', sizeof(large_value));
  header = make_header("X", "v");
  failed = build_head(200u, "OK", &header, 1u, head, 1u);
  check_empty_result(
      omni_http_response_view(head, sizeof(head), &failed, &sentinel, 1u),
      OMNI_HTTP_RESPONSE_ERR_INVALID_HEAD,
      "Task 041 ERR_OUTPUT_TOO_SMALL result is rejected");

  failed = omni_http_response_head_build(99u, (const unsigned char *)"Bad", 3u,
                                         NULL, 0u, head, sizeof(head));
  check_empty_result(
      omni_http_response_view(head, sizeof(head), &failed, &sentinel, 1u),
      OMNI_HTTP_RESPONSE_ERR_INVALID_HEAD,
      "Task 041 ERR_INVALID_STATUS result is rejected");

  failed = omni_http_response_head_build(200u, (const unsigned char *)"Bad\r",
                                         4u, NULL, 0u, head, sizeof(head));
  check_empty_result(
      omni_http_response_view(head, sizeof(head), &failed, &sentinel, 1u),
      OMNI_HTTP_RESPONSE_ERR_INVALID_HEAD,
      "Task 041 ERR_INVALID_REASON result is rejected");

  header.name = (const unsigned char *)"Bad Name";
  header.name_length = 8u;
  failed = omni_http_response_head_build(200u, (const unsigned char *)"OK", 2u,
                                         &header, 1u, head, sizeof(head));
  check_empty_result(
      omni_http_response_view(head, sizeof(head), &failed, &sentinel, 1u),
      OMNI_HTTP_RESPONSE_ERR_INVALID_HEAD,
      "Task 041 ERR_INVALID_HEADER result is rejected");

  header.name = (const unsigned char *)"X";
  header.name_length = 1u;
  header.value = large_value;
  header.value_length = sizeof(large_value);
  failed = omni_http_response_head_build(200u, (const unsigned char *)"OK", 2u,
                                         &header, 1u, head, sizeof(head));
  check_empty_result(
      omni_http_response_view(head, sizeof(head), &failed, &sentinel, 1u),
      OMNI_HTTP_RESPONSE_ERR_INVALID_HEAD,
      "Task 041 ERR_TOO_LARGE result is rejected");

  header.value = (const unsigned char *)"v";
  header.value_length = 1u;
  header.name_length = SIZE_MAX;
  failed = omni_http_response_head_build(200u, (const unsigned char *)"OK", 2u,
                                         &header, 1u, head, sizeof(head));
  check_empty_result(
      omni_http_response_view(head, sizeof(head), &failed, &sentinel, 1u),
      OMNI_HTTP_RESPONSE_ERR_INVALID_HEAD,
      "Task 041 ERR_OVERFLOW result is rejected");

  valid = build_head(200u, "OK", NULL, 0u, head, sizeof(head));
  result =
      omni_http_response_view(head, valid.written_bytes, &valid, &sentinel, 1u);
  check(result.status == OMNI_HTTP_RESPONSE_OK,
        "valid Task 041 result remains composable after failed builds");
}

static void test_inconsistent_metadata_and_arguments(void) {
  unsigned char head[128];
  static const unsigned char body = 0x41u;
  struct omni_http_response_head_result valid;
  struct omni_http_response_head_result synthetic;

  valid = build_head(200u, "OK", NULL, 0u, head, sizeof(head));
  check_empty_result(
      omni_http_response_view(head, sizeof(head), NULL, &body, 1u),
      OMNI_HTTP_RESPONSE_ERR_INVALID_ARGUMENT,
      "NULL Task 041 result is rejected");

  synthetic = valid;
  synthetic.written_bytes = 0u;
  check_empty_result(
      omni_http_response_view(head, sizeof(head), &synthetic, &body, 1u),
      OMNI_HTTP_RESPONSE_ERR_INCONSISTENT_HEAD,
      "successful zero-byte head metadata is rejected");

  synthetic = valid;
  synthetic.required_bytes = valid.required_bytes + 1u;
  check_empty_result(
      omni_http_response_view(head, sizeof(head), &synthetic, &body, 1u),
      OMNI_HTTP_RESPONSE_ERR_INCONSISTENT_HEAD,
      "successful mismatched required/written metadata is rejected");

  synthetic = valid;
  synthetic.written_bytes = OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES + 1u;
  synthetic.required_bytes = synthetic.written_bytes;
  check_empty_result(
      omni_http_response_view(&body, SIZE_MAX, &synthetic, &body, 1u),
      OMNI_HTTP_RESPONSE_ERR_INCONSISTENT_HEAD,
      "successful head beyond Task 041 maximum is rejected");

  synthetic = valid;
  check_empty_result(
      omni_http_response_view(head, valid.written_bytes - 1u, &synthetic, &body,
                              1u),
      OMNI_HTTP_RESPONSE_ERR_INCONSISTENT_HEAD,
      "written bytes beyond available head storage are rejected");

  check_empty_result(
      omni_http_response_view(NULL, valid.written_bytes, &valid, &body, 1u),
      OMNI_HTTP_RESPONSE_ERR_INCONSISTENT_HEAD,
      "nonempty successful head requires non-NULL storage");

  check_empty_result(
      omni_http_response_view(head, sizeof(head), &valid, NULL, 1u),
      OMNI_HTTP_RESPONSE_ERR_INVALID_ARGUMENT,
      "nonzero body length requires non-NULL storage");
  check(omni_http_response_view(head, sizeof(head), &valid, NULL, 0u).status ==
            OMNI_HTTP_RESPONSE_OK,
        "NULL body with zero length is accepted");
}

static void test_total_overflow(void) {
  unsigned char head[128];
  static const unsigned char sentinel = 0x71u;
  struct omni_http_response_head_result valid;
  struct omni_http_response_result result;
  const size_t overflowing_length = SIZE_MAX - 1u + 1u;

  valid = build_head(200u, "OK", NULL, 0u, head, sizeof(head));
  result = omni_http_response_view(head, sizeof(head), &valid, &sentinel,
                                   SIZE_MAX - valid.written_bytes + 1u);
  check_empty_result(result, OMNI_HTTP_RESPONSE_ERR_OVERFLOW,
                     "body length that exceeds SIZE_MAX is rejected");
  result = omni_http_response_view(head, sizeof(head), &valid, &sentinel,
                                   overflowing_length);
  check_empty_result(result, OMNI_HTTP_RESPONSE_ERR_OVERFLOW,
                     "SIZE_MAX body sentinel is rejected without dereference");
}

static void test_max_head_composition(void) {
  static unsigned char value[OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES];
  static unsigned char output[OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES];
  static const unsigned char body = 0x7fu;
  struct omni_http_response_header header;
  struct omni_http_response_head_result head_result;
  struct omni_http_response_result result;

  memset(value, 'a', sizeof(value));
  header.name = (const unsigned char *)"X";
  header.name_length = 1u;
  header.value = value;
  header.value_length = OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES - 24u;
  head_result =
      omni_http_response_head_build(200u, (const unsigned char *)"OK", 2u,
                                    &header, 1u, output, sizeof(output));
  check(head_result.status == OMNI_HTTP_RESPONSE_HEAD_OK &&
            head_result.written_bytes == OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES,
        "Task 041 exact maximum response head is successful");
  result =
      omni_http_response_view(output, sizeof(output), &head_result, NULL, 0u);
  check(result.status == OMNI_HTTP_RESPONSE_OK &&
            result.head.length == OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES &&
            result.total_bytes == OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES,
        "maximum head composes with zero body");
  result =
      omni_http_response_view(output, sizeof(output), &head_result, &body, 1u);
  check(result.status == OMNI_HTTP_RESPONSE_OK &&
            result.total_bytes == OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES + 1u &&
            result.body.data == &body,
        "maximum head composes with a small body when size arithmetic fits");
}

static void test_max_header_count_composition(void) {
  struct omni_http_response_header headers[OMNI_HTTP_RESPONSE_HEAD_MAX_HEADERS];
  static const unsigned char body[] = {0x01u, 0x02u};
  unsigned char head[OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES];
  struct omni_http_response_head_result head_result;
  struct omni_http_response_result result;
  size_t index;

  for (index = 0u; index < OMNI_HTTP_RESPONSE_HEAD_MAX_HEADERS; ++index) {
    headers[index] = make_header("X", "v");
  }
  head_result =
      build_head(200u, "OK", headers, OMNI_HTTP_RESPONSE_HEAD_MAX_HEADERS, head,
                 sizeof(head));
  result = omni_http_response_view(head, sizeof(head), &head_result, body,
                                   sizeof(body));
  check(head_result.status == OMNI_HTTP_RESPONSE_HEAD_OK &&
            result.status == OMNI_HTTP_RESPONSE_OK &&
            result.body.data == body && result.body.length == sizeof(body),
        "maximum Task 041 header count composes without Task 042 sensitivity");
}

static void test_stress(void) {
  static const unsigned int statuses[] = {200u, 204u, 400u, 404u, 500u};
  static const char *const reasons[] = {"OK", "No Content", "Bad Request",
                                        "Not Found", "Internal Server Error"};
  static const unsigned char text_body[] = "hello";
  static const unsigned char binary_body[] = {0x00u, 0xffu, 0x7fu};
  struct omni_http_response_header header;
  unsigned char head[256];
  size_t iteration;

  for (iteration = 0u; iteration < 10000u; ++iteration) {
    const size_t body_kind = iteration % 3u;
    const unsigned char *body = NULL;
    size_t body_length = 0u;
    struct omni_http_response_head_result head_result;
    struct omni_http_response_result result;

    header = make_header("X-Cycle", "bounded");
    if (body_kind == 1u) {
      body = text_body;
      body_length = sizeof(text_body) - 1u;
    } else if (body_kind == 2u) {
      body = binary_body;
      body_length = sizeof(binary_body);
    }
    head_result =
        build_head(statuses[iteration % 5u], reasons[iteration % 5u],
                   iteration % 2u == 0u ? &header : NULL,
                   iteration % 2u == 0u ? 1u : 0u, head, sizeof(head));
    result = omni_http_response_view(head, sizeof(head), &head_result, body,
                                     body_length);
    check(result.status == OMNI_HTTP_RESPONSE_OK && result.head.data == head &&
              result.body.data == body && result.body.length == body_length &&
              result.total_bytes == head_result.written_bytes + body_length,
          "10,000 real Task 041 to Task 042 compositions remain deterministic");
  }
}

int main(void) {
  test_real_zero_body_integration();
  test_text_binary_and_opaque_bodies();
  test_storage_identity_and_capacity();
  test_failed_head_results_are_rejected();
  test_inconsistent_metadata_and_arguments();
  test_total_overflow();
  test_max_head_composition();
  test_max_header_count_composition();
  test_stress();

  printf("http-response checks: %zu; failures: %zu; ", check_count,
         failure_count);
  printf("sizeof(omni_http_response_span)=%zu; ",
         sizeof(struct omni_http_response_span));
  printf("sizeof(omni_http_response_result)=%zu\n",
         sizeof(struct omni_http_response_result));
  return failure_count == 0u ? 0 : 1;
}
