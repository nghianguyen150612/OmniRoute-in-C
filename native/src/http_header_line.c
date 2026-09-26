/*
 * OmniRoute native backend — bounded HTTP header-line parser (Task 034).
 *
 * Stateless, single-pass ASCII syntax validation. Reads are bounded by both
 * the caller span and the public total-line limit. No allocation, I/O,
 * locale handling, input mutation, or retained storage is used.
 */

#include "omniroute/http_header_line.h"

static struct omni_http_header_line_result result_at(
    enum omni_http_header_line_status status, size_t offset) {
  struct omni_http_header_line_result result = {0};

  result.status = status;
  result.error_offset = offset;
  return result;
}

static int header_byte(const unsigned char *data, size_t length, size_t offset,
                       unsigned char *byte,
                       struct omni_http_header_line_result *failure) {
  if (offset >= OMNI_HTTP_HEADER_LINE_MAX_TOTAL_BYTES) {
    *failure = result_at(OMNI_HTTP_HEADER_LINE_TOO_LARGE, offset);
    return 0;
  }
  if (offset >= length) {
    *failure = result_at(OMNI_HTTP_HEADER_LINE_INCOMPLETE, offset);
    return 0;
  }

  *byte = data[offset];
  return 1;
}

static int is_token_byte(unsigned char byte) {
  if ((byte >= (unsigned char)'A' && byte <= (unsigned char)'Z') ||
      (byte >= (unsigned char)'a' && byte <= (unsigned char)'z') ||
      (byte >= (unsigned char)'0' && byte <= (unsigned char)'9')) {
    return 1;
  }

  switch (byte) {
    case (unsigned char)'!':
    case (unsigned char)'#':
    case (unsigned char)'$':
    case (unsigned char)'%':
    case (unsigned char)'&':
    case (unsigned char)'\'':
    case (unsigned char)'*':
    case (unsigned char)'+':
    case (unsigned char)'-':
    case (unsigned char)'.':
    case (unsigned char)'^':
    case (unsigned char)'_':
    case (unsigned char)'`':
    case (unsigned char)'|':
    case (unsigned char)'~':
      return 1;
    default:
      return 0;
  }
}

static int is_ows(unsigned char byte) {
  return byte == (unsigned char)' ' || byte == (unsigned char)'\t';
}

static int is_field_value_byte(unsigned char byte) {
  return byte == (unsigned char)'\t' ||
         (byte >= 0x20u && byte <= 0x7eu);
}

struct omni_http_header_line_result omni_http_header_line_parse(
    const unsigned char *data, size_t length) {
  struct omni_http_header_line_result pending;
  struct omni_http_header_line_result result;
  size_t name_length = 0u;
  size_t value_start = 0u;
  size_t value_end = 0u;
  size_t offset = 0u;
  unsigned char byte = 0u;

  if (data == NULL && length != 0u) {
    return result_at(OMNI_HTTP_HEADER_LINE_ERR_INVALID_ARGUMENT, 0u);
  }

  /* Parse a nonempty HTTP token field-name; the colon must touch the name. */
  for (;;) {
    if (!header_byte(data, length, offset, &byte, &pending)) return pending;
    if (byte == (unsigned char)':') {
      if (offset == 0u) {
        return result_at(OMNI_HTTP_HEADER_LINE_INVALID, offset);
      }
      name_length = offset;
      ++offset;
      break;
    }
    if (!is_token_byte(byte)) {
      return result_at(OMNI_HTTP_HEADER_LINE_INVALID, offset);
    }
    if (offset >= OMNI_HTTP_HEADER_LINE_MAX_NAME_BYTES) {
      return result_at(OMNI_HTTP_HEADER_LINE_TOO_LARGE, offset);
    }
    ++offset;
  }

  /* Discard leading OWS after the colon. It is not part of the value span. */
  for (;;) {
    if (!header_byte(data, length, offset, &byte, &pending)) return pending;
    if (!is_ows(byte)) break;
    ++offset;
    if (!header_byte(data, length, offset, &byte, &pending)) return pending;
  }

  value_start = offset;
  value_end = value_start;

  /*
   * Validate value bytes once. value_end advances only at a non-OWS byte, so
   * the final span excludes trailing OWS while interior OWS remains included.
   */
  for (;;) {
    if (byte == (unsigned char)'\r') {
      ++offset;
      break;
    }
    if (byte == (unsigned char)'\n' || !is_field_value_byte(byte)) {
      return result_at(OMNI_HTTP_HEADER_LINE_INVALID, offset);
    }
    if (!is_ows(byte)) {
      if (offset - value_start >= OMNI_HTTP_HEADER_LINE_MAX_VALUE_BYTES) {
        return result_at(OMNI_HTTP_HEADER_LINE_TOO_LARGE, offset);
      }
      value_end = offset + 1u;
    }
    ++offset;
    if (!header_byte(data, length, offset, &byte, &pending)) return pending;
  }

  /* CR is only a viable ending until the required LF is present. */
  if (!header_byte(data, length, offset, &byte, &pending)) return pending;
  if (byte != (unsigned char)'\n') {
    return result_at(OMNI_HTTP_HEADER_LINE_INVALID, offset);
  }
  ++offset;

  result = result_at(OMNI_HTTP_HEADER_LINE_COMPLETE, 0u);
  result.name.data = data;
  result.name.length = name_length;
  result.value.data = data + value_start;
  result.value.length = value_end - value_start;
  result.consumed_bytes = offset;
  return result;
}
