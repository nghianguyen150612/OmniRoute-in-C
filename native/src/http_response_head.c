/*
 * OmniRoute native backend — bounded HTTP/1.1 response-head builder (Task 041).
 *
 * The builder validates and measures all borrowed metadata before it writes a
 * single output byte. It has no heap, I/O, locale, formatting, or persistent
 * state and uses only caller-owned output storage.
 */

#include "omniroute/http_response_head.h"

static struct omni_http_response_head_result
result_with(enum omni_http_response_head_status status, size_t required_bytes) {
  struct omni_http_response_head_result result = {0};

  result.status = status;
  result.required_bytes = required_bytes;
  return result;
}

/*
 * Task 034 owns the canonical HTTP token-byte set in a file-static helper of a
 * separate static library, so it cannot be linked here without publishing a new
 * internal shared surface. This predicate is intentionally byte-identical to
 * it; treat any divergence between the two as a routing/serialization mismatch.
 */
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

static int is_reason_or_value_byte(unsigned char byte) {
  return byte == (unsigned char)'\t' || (byte >= 0x20u && byte <= 0x7eu);
}

static int add_size(size_t *total, size_t amount) {
  const size_t maximum = (size_t)-1;

  if (amount > maximum - *total)
    return 0;
  *total += amount;
  return 1;
}

static enum omni_http_response_head_status
measure(const unsigned char *reason, size_t reason_length,
        const struct omni_http_response_header *headers, size_t header_count,
        size_t *required_bytes) {
  size_t total = 15u;
  size_t index;

  /* Error paths report a deterministic zero size unless a size was proven. */
  *required_bytes = 0u;

  if (reason == NULL && reason_length != 0u) {
    return OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_REASON;
  }
  if (headers == NULL && header_count != 0u) {
    return OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_ARGUMENT;
  }
  if (header_count > OMNI_HTTP_RESPONSE_HEAD_MAX_HEADERS) {
    return OMNI_HTTP_RESPONSE_HEAD_ERR_TOO_MANY_HEADERS;
  }

  if (!add_size(&total, reason_length)) {
    return OMNI_HTTP_RESPONSE_HEAD_ERR_OVERFLOW;
  }

  for (index = 0u; index < header_count; ++index) {
    const struct omni_http_response_header *header = &headers[index];

    if ((header->name == NULL && header->name_length != 0u) ||
        (header->value == NULL && header->value_length != 0u)) {
      return OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_HEADER;
    }
    if (!add_size(&total, header->name_length) || !add_size(&total, 4u) ||
        !add_size(&total, header->value_length)) {
      return OMNI_HTTP_RESPONSE_HEAD_ERR_OVERFLOW;
    }
  }
  if (!add_size(&total, 2u)) {
    return OMNI_HTTP_RESPONSE_HEAD_ERR_OVERFLOW;
  }

  *required_bytes = total;
  if (total > OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES) {
    return OMNI_HTTP_RESPONSE_HEAD_ERR_TOO_LARGE;
  }
  return OMNI_HTTP_RESPONSE_HEAD_OK;
}

static int valid_bytes(const unsigned char *data, size_t length,
                       int token_bytes) {
  size_t index;

  for (index = 0u; index < length; ++index) {
    const unsigned char byte = data[index];
    if (token_bytes != 0 ? !is_token_byte(byte)
                         : !is_reason_or_value_byte(byte)) {
      return 0;
    }
  }
  return 1;
}

static void append_bytes(unsigned char *output, size_t *offset,
                         const unsigned char *data, size_t length) {
  size_t index;

  for (index = 0u; index < length; ++index) {
    output[*offset] = data[index];
    ++*offset;
  }
}

struct omni_http_response_head_result omni_http_response_head_build(
    unsigned int status_code, const unsigned char *reason, size_t reason_length,
    const struct omni_http_response_header *headers, size_t header_count,
    unsigned char *output, size_t output_capacity) {
  struct omni_http_response_head_result result = {0};
  size_t required_bytes = 0u;
  size_t offset = 0u;
  size_t index;
  enum omni_http_response_head_status measured;

  if (status_code < 100u || status_code > 999u) {
    result.status = OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_STATUS;
    return result;
  }
  if (output == NULL && output_capacity != 0u) {
    result.status = OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_ARGUMENT;
    return result;
  }

  measured =
      measure(reason, reason_length, headers, header_count, &required_bytes);
  if (measured != OMNI_HTTP_RESPONSE_HEAD_OK) {
    return result_with(measured, required_bytes);
  }
  if (!valid_bytes(reason, reason_length, 0)) {
    return result_with(OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_REASON,
                       required_bytes);
  }
  for (index = 0u; index < header_count; ++index) {
    const struct omni_http_response_header *header = &headers[index];

    if (header->name_length == 0u ||
        !valid_bytes(header->name, header->name_length, 1)) {
      return result_with(OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_HEADER,
                         required_bytes);
    }
    if (!valid_bytes(header->value, header->value_length, 0)) {
      return result_with(OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_HEADER,
                         required_bytes);
    }
  }
  if (output == NULL || output_capacity < required_bytes) {
    return result_with(OMNI_HTTP_RESPONSE_HEAD_ERR_OUTPUT_TOO_SMALL,
                       required_bytes);
  }

  output[offset++] = (unsigned char)'H';
  output[offset++] = (unsigned char)'T';
  output[offset++] = (unsigned char)'T';
  output[offset++] = (unsigned char)'P';
  output[offset++] = (unsigned char)'/';
  output[offset++] = (unsigned char)'1';
  output[offset++] = (unsigned char)'.';
  output[offset++] = (unsigned char)'1';
  output[offset++] = (unsigned char)' ';
  output[offset++] = (unsigned char)('0' + status_code / 100u);
  output[offset++] = (unsigned char)('0' + (status_code / 10u) % 10u);
  output[offset++] = (unsigned char)('0' + status_code % 10u);
  output[offset++] = (unsigned char)' ';
  append_bytes(output, &offset, reason, reason_length);
  output[offset++] = (unsigned char)'\r';
  output[offset++] = (unsigned char)'\n';

  for (index = 0u; index < header_count; ++index) {
    const struct omni_http_response_header *header = &headers[index];

    append_bytes(output, &offset, header->name, header->name_length);
    output[offset++] = (unsigned char)':';
    output[offset++] = (unsigned char)' ';
    append_bytes(output, &offset, header->value, header->value_length);
    output[offset++] = (unsigned char)'\r';
    output[offset++] = (unsigned char)'\n';
  }
  output[offset++] = (unsigned char)'\r';
  output[offset++] = (unsigned char)'\n';

  result.status = OMNI_HTTP_RESPONSE_HEAD_OK;
  result.written_bytes = offset;
  result.required_bytes = required_bytes;
  return result;
}
