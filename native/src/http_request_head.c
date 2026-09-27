/* Task 035: monotonic, bounded composition; child parsers own line grammar. */
#include "omniroute/http_request_head.h"

static struct omni_http_request_head_result result_at(enum omni_http_request_head_status status,
                                                      size_t offset) {
  struct omni_http_request_head_result result = {0};
  result.status = status;
  result.error_offset = offset;
  return result;
}

static enum omni_http_request_head_status
request_status(enum omni_http_request_line_status status) {
  switch (status) {
  case OMNI_HTTP_REQUEST_LINE_COMPLETE:
    return OMNI_HTTP_REQUEST_HEAD_COMPLETE;
  case OMNI_HTTP_REQUEST_LINE_INCOMPLETE:
    return OMNI_HTTP_REQUEST_HEAD_INCOMPLETE;
  case OMNI_HTTP_REQUEST_LINE_INVALID:
    return OMNI_HTTP_REQUEST_HEAD_INVALID;
  case OMNI_HTTP_REQUEST_LINE_TOO_LARGE:
    return OMNI_HTTP_REQUEST_HEAD_TOO_LARGE;
  case OMNI_HTTP_REQUEST_LINE_UNSUPPORTED_VERSION:
    return OMNI_HTTP_REQUEST_HEAD_UNSUPPORTED_VERSION;
  case OMNI_HTTP_REQUEST_LINE_ERR_INVALID_ARGUMENT:
    return OMNI_HTTP_REQUEST_HEAD_ERR_INVALID_ARGUMENT;
  }
  return OMNI_HTTP_REQUEST_HEAD_INVALID;
}

static enum omni_http_request_head_status header_status(enum omni_http_header_line_status status) {
  switch (status) {
  case OMNI_HTTP_HEADER_LINE_COMPLETE:
    return OMNI_HTTP_REQUEST_HEAD_COMPLETE;
  case OMNI_HTTP_HEADER_LINE_INCOMPLETE:
    return OMNI_HTTP_REQUEST_HEAD_INCOMPLETE;
  case OMNI_HTTP_HEADER_LINE_INVALID:
    return OMNI_HTTP_REQUEST_HEAD_INVALID;
  case OMNI_HTTP_HEADER_LINE_TOO_LARGE:
    return OMNI_HTTP_REQUEST_HEAD_TOO_LARGE;
  case OMNI_HTTP_HEADER_LINE_ERR_INVALID_ARGUMENT:
    return OMNI_HTTP_REQUEST_HEAD_ERR_INVALID_ARGUMENT;
  }
  return OMNI_HTTP_REQUEST_HEAD_INVALID;
}

struct omni_http_request_head_result omni_http_request_head_parse(const unsigned char *data,
                                                                  size_t length,
                                                                  struct omni_http_header *headers,
                                                                  size_t header_capacity) {
  struct omni_http_request_line_result line;
  size_t bounded_length = length;
  size_t offset;
  size_t count = 0u;

  if ((data == NULL && length != 0u) || (headers == NULL && header_capacity != 0u)) {
    return result_at(OMNI_HTTP_REQUEST_HEAD_ERR_INVALID_ARGUMENT, 0u);
  }
  if (bounded_length > OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES) {
    bounded_length = OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES;
  }
  line = omni_http_request_line_parse(data, bounded_length);
  if (line.status != OMNI_HTTP_REQUEST_LINE_COMPLETE) {
    return result_at(request_status(line.status), line.error_offset);
  }
  offset = line.consumed_bytes;

  for (;;) {
    struct omni_http_header_line_result field;
    enum omni_http_request_head_status status;
    size_t remaining = bounded_length - offset;

    /* All offsets stay <= bounded_length <= the finite head limit. */
    if (remaining == 0u) {
      return result_at(offset == OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES
                           ? OMNI_HTTP_REQUEST_HEAD_TOO_LARGE
                           : OMNI_HTTP_REQUEST_HEAD_INCOMPLETE,
                       offset);
    }
    /* This layer owns the blank line; never send it to the field parser. */
    if (data[offset] == (unsigned char)'\r') {
      struct omni_http_request_head_result result;
      if (remaining < 2u) {
        return result_at(bounded_length == OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES
                             ? OMNI_HTTP_REQUEST_HEAD_TOO_LARGE
                             : OMNI_HTTP_REQUEST_HEAD_INCOMPLETE,
                         bounded_length);
      }
      if (data[offset + 1u] != (unsigned char)'\n') {
        return result_at(OMNI_HTTP_REQUEST_HEAD_INVALID, offset + 1u);
      }
      result = result_at(OMNI_HTTP_REQUEST_HEAD_COMPLETE, 0u);
      result.request_line = line;
      result.headers = headers;
      result.header_count = count;
      result.consumed_bytes = offset + 2u;
      return result;
    }
    /* Reject folded lines, including a truncated initial SP/HTAB. */
    if (data[offset] == (unsigned char)' ' || data[offset] == (unsigned char)'\t') {
      return result_at(OMNI_HTTP_REQUEST_HEAD_INVALID, offset);
    }
    field = omni_http_header_line_parse(data + offset, remaining);
    if (field.status != OMNI_HTTP_HEADER_LINE_COMPLETE) {
      status = header_status(field.status);
      if (status == OMNI_HTTP_REQUEST_HEAD_INCOMPLETE &&
          bounded_length == OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES) {
        status = OMNI_HTTP_REQUEST_HEAD_TOO_LARGE;
      }
      /* Child error offsets are <= remaining; addition cannot overflow. */
      return result_at(status, offset + field.error_offset);
    }
    if (count >= header_capacity || count >= OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS) {
      return result_at(OMNI_HTTP_REQUEST_HEAD_TOO_MANY_HEADERS, offset);
    }
    headers[count].name = field.name;
    headers[count].value = field.value;
    ++count;
    /* COMPLETE always consumes a nonempty line within remaining. */
    offset += field.consumed_bytes;
  }
}
