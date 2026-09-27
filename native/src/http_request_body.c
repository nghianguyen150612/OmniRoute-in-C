/* Task 037: zero-copy availability view over valid Task 035/036 metadata. */
#include <stdint.h>

#include "omniroute/http_request_body.h"

static struct omni_http_request_body_result
result_at(enum omni_http_request_body_status status) {
  struct omni_http_request_body_result result = {0};
  result.status = status;
  return result;
}

static int complete_head_metadata_is_valid(const struct omni_http_request_head_result *head) {
  return head->status == OMNI_HTTP_REQUEST_HEAD_COMPLETE && head->error_offset == 0u &&
         head->consumed_bytes != 0u &&
         head->consumed_bytes <= OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES &&
         head->request_line.status == OMNI_HTTP_REQUEST_LINE_COMPLETE &&
         head->request_line.error_offset == 0u && head->request_line.consumed_bytes != 0u &&
         head->request_line.consumed_bytes <= OMNI_HTTP_REQUEST_LINE_MAX_TOTAL_BYTES &&
         head->request_line.consumed_bytes <= head->consumed_bytes &&
         head->request_line.method.data != NULL && head->request_line.method.length != 0u &&
         head->request_line.method.length <= OMNI_HTTP_REQUEST_LINE_MAX_METHOD_BYTES &&
         head->request_line.target.data != NULL && head->request_line.target.length != 0u &&
         head->request_line.target.length <= OMNI_HTTP_REQUEST_LINE_MAX_TARGET_BYTES &&
         (head->request_line.version == OMNI_HTTP_VERSION_1_0 ||
          head->request_line.version == OMNI_HTTP_VERSION_1_1) &&
         head->header_count <= OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS &&
         (head->header_count == 0u || head->headers != NULL);
}

static int framing_failure_has_valid_shape(const struct omni_http_request_framing_result *framing,
                                           size_t header_count) {
  if (framing->framing != OMNI_HTTP_REQUEST_BODY_NONE || framing->content_length != 0u)
    return 0;

  switch (framing->status) {
  case OMNI_HTTP_REQUEST_FRAMING_INVALID_CONTENT_LENGTH:
  case OMNI_HTTP_REQUEST_FRAMING_CONFLICTING_CONTENT_LENGTH:
    return framing->content_length_field_count != 0u &&
           framing->content_length_field_count <= header_count &&
           framing->error_header_index < header_count;
  case OMNI_HTTP_REQUEST_FRAMING_AMBIGUOUS:
    return framing->content_length_field_count != 0u &&
           framing->content_length_field_count <= header_count &&
           framing->transfer_encoding_present && framing->error_header_index < header_count;
  case OMNI_HTTP_REQUEST_FRAMING_TRANSFER_ENCODING_UNSUPPORTED:
    return framing->content_length_field_count == 0u &&
           framing->transfer_encoding_present && framing->error_header_index < header_count;
  default:
    return 0;
  }
}

struct omni_http_request_body_result
omni_http_request_body_view(const unsigned char *data, size_t length,
                            const struct omni_http_request_head_result *head,
                            const struct omni_http_request_framing_result *framing) {
  struct omni_http_request_body_result result;
  size_t required_total_bytes;

  if ((data == NULL && length != 0u) || head == NULL || framing == NULL)
    return result_at(OMNI_HTTP_REQUEST_BODY_INVALID_ARGUMENT);
  if (!complete_head_metadata_is_valid(head) || head->consumed_bytes > length)
    return result_at(OMNI_HTTP_REQUEST_BODY_INVALID_STATE);

  if (framing->status != OMNI_HTTP_REQUEST_FRAMING_OK) {
    if (!framing_failure_has_valid_shape(framing, head->header_count))
      return result_at(OMNI_HTTP_REQUEST_BODY_INVALID_STATE);
    if (framing->status == OMNI_HTTP_REQUEST_FRAMING_TRANSFER_ENCODING_UNSUPPORTED)
      return result_at(OMNI_HTTP_REQUEST_BODY_UNSUPPORTED_FRAMING);
    return result_at(OMNI_HTTP_REQUEST_BODY_INVALID_FRAMING);
  }

  if (framing->error_header_index != OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX)
    return result_at(OMNI_HTTP_REQUEST_BODY_INVALID_STATE);

  if (framing->framing == OMNI_HTTP_REQUEST_BODY_NONE) {
    if (framing->content_length != 0u || framing->content_length_field_count != 0u ||
        framing->transfer_encoding_present) {
      return result_at(OMNI_HTTP_REQUEST_BODY_INVALID_STATE);
    }
    result = result_at(OMNI_HTTP_REQUEST_BODY_COMPLETE);
    result.body.data = data + head->consumed_bytes;
    result.required_total_bytes = head->consumed_bytes;
    result.consumed_bytes = head->consumed_bytes;
    return result;
  }

  if (framing->framing != OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH ||
      framing->content_length_field_count == 0u ||
      framing->content_length_field_count > head->header_count ||
      framing->transfer_encoding_present) {
    return result_at(OMNI_HTTP_REQUEST_BODY_INVALID_STATE);
  }

  if (framing->content_length > SIZE_MAX - head->consumed_bytes)
    return result_at(OMNI_HTTP_REQUEST_BODY_OVERFLOW);
  required_total_bytes = head->consumed_bytes + framing->content_length;

  if (length < required_total_bytes) {
    result = result_at(OMNI_HTTP_REQUEST_BODY_INCOMPLETE);
    result.required_total_bytes = required_total_bytes;
    return result;
  }

  result = result_at(OMNI_HTTP_REQUEST_BODY_COMPLETE);
  result.body.data = data + head->consumed_bytes;
  result.body.length = framing->content_length;
  result.required_total_bytes = required_total_bytes;
  result.consumed_bytes = required_total_bytes;
  return result;
}
