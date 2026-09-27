/* Task 038: bounded composition over the bytebuf readable region. */
#include "omniroute/http_request.h"

static struct omni_http_request_result result_at(enum omni_http_request_status status) {
  struct omni_http_request_result result = {0};
  result.status = status;
  return result;
}

static enum omni_http_request_status map_head_status(enum omni_http_request_head_status status) {
  switch (status) {
  case OMNI_HTTP_REQUEST_HEAD_INCOMPLETE:
    return OMNI_HTTP_REQUEST_INCOMPLETE;
  case OMNI_HTTP_REQUEST_HEAD_INVALID:
    return OMNI_HTTP_REQUEST_INVALID_REQUEST_HEAD;
  case OMNI_HTTP_REQUEST_HEAD_TOO_LARGE:
    return OMNI_HTTP_REQUEST_REQUEST_HEAD_TOO_LARGE;
  case OMNI_HTTP_REQUEST_HEAD_TOO_MANY_HEADERS:
    return OMNI_HTTP_REQUEST_TOO_MANY_HEADERS;
  case OMNI_HTTP_REQUEST_HEAD_UNSUPPORTED_VERSION:
    return OMNI_HTTP_REQUEST_UNSUPPORTED_VERSION;
  case OMNI_HTTP_REQUEST_HEAD_ERR_INVALID_ARGUMENT:
    return OMNI_HTTP_REQUEST_ERR_INVALID_ARGUMENT;
  case OMNI_HTTP_REQUEST_HEAD_COMPLETE:
    return OMNI_HTTP_REQUEST_ERR_INVALID_STATE;
  }
  return OMNI_HTTP_REQUEST_ERR_INVALID_STATE;
}

static enum omni_http_request_status map_framing_status(
    enum omni_http_request_framing_status status) {
  switch (status) {
  case OMNI_HTTP_REQUEST_FRAMING_INVALID_CONTENT_LENGTH:
  case OMNI_HTTP_REQUEST_FRAMING_CONFLICTING_CONTENT_LENGTH:
  case OMNI_HTTP_REQUEST_FRAMING_AMBIGUOUS:
    return OMNI_HTTP_REQUEST_INVALID_FRAMING;
  case OMNI_HTTP_REQUEST_FRAMING_TRANSFER_ENCODING_UNSUPPORTED:
    return OMNI_HTTP_REQUEST_UNSUPPORTED_FRAMING;
  case OMNI_HTTP_REQUEST_FRAMING_INVALID_ARGUMENT:
    return OMNI_HTTP_REQUEST_ERR_INVALID_ARGUMENT;
  case OMNI_HTTP_REQUEST_FRAMING_INVALID_HEAD:
    return OMNI_HTTP_REQUEST_ERR_INVALID_STATE;
  case OMNI_HTTP_REQUEST_FRAMING_OK:
    return OMNI_HTTP_REQUEST_ERR_INVALID_STATE;
  }
  return OMNI_HTTP_REQUEST_ERR_INVALID_STATE;
}

static enum omni_http_request_status map_body_status(enum omni_http_request_body_status status) {
  switch (status) {
  case OMNI_HTTP_REQUEST_BODY_INCOMPLETE:
    return OMNI_HTTP_REQUEST_INCOMPLETE;
  case OMNI_HTTP_REQUEST_BODY_UNSUPPORTED_FRAMING:
    return OMNI_HTTP_REQUEST_UNSUPPORTED_FRAMING;
  case OMNI_HTTP_REQUEST_BODY_INVALID_FRAMING:
    return OMNI_HTTP_REQUEST_INVALID_FRAMING;
  case OMNI_HTTP_REQUEST_BODY_INVALID_ARGUMENT:
    return OMNI_HTTP_REQUEST_ERR_INVALID_ARGUMENT;
  case OMNI_HTTP_REQUEST_BODY_INVALID_STATE:
    return OMNI_HTTP_REQUEST_ERR_INVALID_STATE;
  case OMNI_HTTP_REQUEST_BODY_OVERFLOW:
    return OMNI_HTTP_REQUEST_BODY_OVERFLOW_ERROR;
  case OMNI_HTTP_REQUEST_BODY_COMPLETE:
    return OMNI_HTTP_REQUEST_ERR_INVALID_STATE;
  }
  return OMNI_HTTP_REQUEST_ERR_INVALID_STATE;
}

struct omni_http_request_result omni_http_request_assemble(
    const struct omni_bytebuf *buffer, struct omni_http_header *headers, size_t header_capacity) {
  const unsigned char *data;
  size_t readable = 0u;
  struct omni_http_request_head_result head;
  struct omni_http_request_framing_result framing;
  struct omni_http_request_body_result body;
  struct omni_http_request_result result;

  if (buffer == NULL)
    return result_at(OMNI_HTTP_REQUEST_ERR_INVALID_ARGUMENT);
  if (omni_bytebuf_capacity(buffer) == 0u)
    return result_at(OMNI_HTTP_REQUEST_ERR_INVALID_STATE);

  data = omni_bytebuf_read_ptr(buffer, &readable);
  if (data == NULL && readable != 0u)
    return result_at(OMNI_HTTP_REQUEST_ERR_INVALID_STATE);

  head = omni_http_request_head_parse(data, readable, headers, header_capacity);
  if (head.status != OMNI_HTTP_REQUEST_HEAD_COMPLETE) {
    result = result_at(map_head_status(head.status));
    if (head.status == OMNI_HTTP_REQUEST_HEAD_INCOMPLETE)
      result.required_total_bytes = 0u;
    return result;
  }

  framing = omni_http_request_framing_analyze(&head);
  if (framing.status != OMNI_HTTP_REQUEST_FRAMING_OK)
    return result_at(map_framing_status(framing.status));

  body = omni_http_request_body_view(data, readable, &head, &framing);
  if (body.status != OMNI_HTTP_REQUEST_BODY_COMPLETE) {
    result = result_at(map_body_status(body.status));
    if (body.status == OMNI_HTTP_REQUEST_BODY_INCOMPLETE)
      result.required_total_bytes = body.required_total_bytes;
    return result;
  }

  result = result_at(OMNI_HTTP_REQUEST_COMPLETE);
  result.request_head = head;
  result.framing = framing;
  result.body = body;
  result.consumed_bytes = body.consumed_bytes;
  result.required_total_bytes = body.required_total_bytes;
  return result;
}
