/*
 * OmniRoute native backend — bounded HTTP request-line parser (Task 033).
 *
 * Stateless, single-pass ASCII syntax validation. All reads go through a
 * bounded byte accessor that enforces both the caller span and total-line
 * cap. No allocation, I/O, locale handling, input mutation, or retained
 * storage is used.
 */

#include "omniroute/http_request_line.h"

static struct omni_http_request_line_result result_at(
    enum omni_http_request_line_status status, size_t offset) {
  struct omni_http_request_line_result result = {0};

  result.status = status;
  result.error_offset = offset;
  return result;
}

/* A failed read reports whether the bounded input ended or the line cap did. */
static int request_byte(const unsigned char *data, size_t length, size_t offset,
                        unsigned char *byte,
                        struct omni_http_request_line_result *failure) {
  if (offset >= OMNI_HTTP_REQUEST_LINE_MAX_TOTAL_BYTES) {
    *failure = result_at(OMNI_HTTP_REQUEST_LINE_TOO_LARGE, offset);
    return 0;
  }
  if (offset >= length) {
    *failure = result_at(OMNI_HTTP_REQUEST_LINE_INCOMPLETE, offset);
    return 0;
  }

  *byte = data[offset];
  return 1;
}

static int is_ascii_digit(unsigned char byte) {
  return byte >= (unsigned char)'0' && byte <= (unsigned char)'9';
}

static int is_method_token(unsigned char byte) {
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

struct omni_http_request_line_result omni_http_request_line_parse(
    const unsigned char *data, size_t length) {
  static const unsigned char version_prefix[] = {'H', 'T', 'T', 'P', '/'};
  const size_t version_length = 8u;
  struct omni_http_request_line_result pending;
  struct omni_http_request_line_result result;
  size_t method_length = 0u;
  size_t method_start = 0u;
  size_t target_start = 0u;
  size_t target_length = 0u;
  size_t version_start = 0u;
  size_t offset = 0u;
  unsigned char byte = 0u;

  if (data == NULL && length != 0u) {
    return result_at(OMNI_HTTP_REQUEST_LINE_ERR_INVALID_ARGUMENT, 0u);
  }

  /* Find and validate METHOD SP. The delimiter itself may follow a max-sized method. */
  for (;;) {
    if (!request_byte(data, length, offset, &byte, &pending)) return pending;
    if (byte == (unsigned char)' ') {
      if (offset == method_start) {
        return result_at(OMNI_HTTP_REQUEST_LINE_INVALID, offset);
      }
      method_length = offset - method_start;
      ++offset;
      break;
    }
    if (!is_method_token(byte)) {
      return result_at(OMNI_HTTP_REQUEST_LINE_INVALID, offset);
    }
    if (offset - method_start >= OMNI_HTTP_REQUEST_LINE_MAX_METHOD_BYTES) {
      return result_at(OMNI_HTTP_REQUEST_LINE_TOO_LARGE, offset);
    }
    ++offset;
  }

  /* Find and validate a nonempty visible-ASCII request target followed by SP. */
  target_start = offset;
  for (;;) {
    if (!request_byte(data, length, offset, &byte, &pending)) return pending;
    if (byte == (unsigned char)' ') {
      if (offset == target_start) {
        return result_at(OMNI_HTTP_REQUEST_LINE_INVALID, offset);
      }
      target_length = offset - target_start;
      ++offset;
      break;
    }
    if (byte < 0x21u || byte > 0x7eu) {
      return result_at(OMNI_HTTP_REQUEST_LINE_INVALID, offset);
    }
    if (offset - target_start >= OMNI_HTTP_REQUEST_LINE_MAX_TARGET_BYTES) {
      return result_at(OMNI_HTTP_REQUEST_LINE_TOO_LARGE, offset);
    }
    ++offset;
  }

  /* HTTP-version has the fixed shape HTTP/ DIGIT . DIGIT. */
  version_start = offset;
  for (size_t index = 0u; index < version_length; ++index) {
    if (!request_byte(data, length, offset, &byte, &pending)) return pending;

    if (index < sizeof(version_prefix)) {
      if (byte != version_prefix[index]) {
        return result_at(OMNI_HTTP_REQUEST_LINE_INVALID, offset);
      }
    } else if (index == 5u || index == 7u) {
      if (!is_ascii_digit(byte)) {
        return result_at(OMNI_HTTP_REQUEST_LINE_INVALID, offset);
      }
    } else if (index == 6u) {
      if (byte != (unsigned char)'.') {
        return result_at(OMNI_HTTP_REQUEST_LINE_INVALID, offset);
      }
    }
    ++offset;
  }

  /* Only CRLF completes a request line; a lone CR may still be continued. */
  if (!request_byte(data, length, offset, &byte, &pending)) return pending;
  if (byte != (unsigned char)'\r') {
    return result_at(OMNI_HTTP_REQUEST_LINE_INVALID, offset);
  }
  ++offset;

  if (!request_byte(data, length, offset, &byte, &pending)) return pending;
  if (byte != (unsigned char)'\n') {
    return result_at(OMNI_HTTP_REQUEST_LINE_INVALID, offset);
  }
  ++offset;

  if (data[version_start + 5u] != (unsigned char)'1' ||
      (data[version_start + 7u] != (unsigned char)'0' &&
       data[version_start + 7u] != (unsigned char)'1')) {
    return result_at(OMNI_HTTP_REQUEST_LINE_UNSUPPORTED_VERSION,
                     version_start + 5u);
  }

  result = result_at(OMNI_HTTP_REQUEST_LINE_COMPLETE, 0u);
  result.method.data = data + method_start;
  result.method.length = method_length;
  result.target.data = data + target_start;
  result.target.length = target_length;
  result.version = data[version_start + 7u] == (unsigned char)'0'
                       ? OMNI_HTTP_VERSION_1_0
                       : OMNI_HTTP_VERSION_1_1;
  result.consumed_bytes = offset;
  return result;
}
