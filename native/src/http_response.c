/*
 * OmniRoute native backend — bounded HTTP response-message view (Task 042).
 *
 * This module composes metadata only. It performs no content reads, allocation,
 * I/O, parsing, copying, or persistent state management.
 */

#include "omniroute/http_response.h"

static struct omni_http_response_result
empty_result(enum omni_http_response_status status) {
  struct omni_http_response_result result = {0};

  result.status = status;
  return result;
}

struct omni_http_response_result omni_http_response_view(
    const unsigned char *head_data, size_t head_available,
    const struct omni_http_response_head_result *head_result,
    const unsigned char *body_data, size_t body_length) {
  struct omni_http_response_result result;
  const size_t maximum = (size_t)-1;

  if (head_result == NULL) {
    return empty_result(OMNI_HTTP_RESPONSE_ERR_INVALID_ARGUMENT);
  }
  if (head_result->status != OMNI_HTTP_RESPONSE_HEAD_OK) {
    return empty_result(OMNI_HTTP_RESPONSE_ERR_INVALID_HEAD);
  }
  if (head_result->written_bytes == 0u ||
      head_result->required_bytes != head_result->written_bytes ||
      head_result->written_bytes > OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES ||
      head_result->written_bytes > head_available || head_data == NULL) {
    return empty_result(OMNI_HTTP_RESPONSE_ERR_INCONSISTENT_HEAD);
  }
  if (body_data == NULL && body_length != 0u) {
    return empty_result(OMNI_HTTP_RESPONSE_ERR_INVALID_ARGUMENT);
  }
  if (body_length > maximum - head_result->written_bytes) {
    return empty_result(OMNI_HTTP_RESPONSE_ERR_OVERFLOW);
  }

  result.status = OMNI_HTTP_RESPONSE_OK;
  result.head.data = head_data;
  result.head.length = head_result->written_bytes;
  result.body.data = body_data;
  result.body.length = body_length;
  result.total_bytes = head_result->written_bytes + body_length;
  return result;
}
