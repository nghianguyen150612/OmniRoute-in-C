/* Task 036: bounded semantics over the borrowed Task 035 header view. */
#include <stdint.h>

#include "omniroute/http_request_framing.h"

struct content_length_parse {
  bool valid;
  bool has_value;
  bool conflict;
  size_t value;
};

static struct omni_http_request_framing_result
result_at(enum omni_http_request_framing_status status) {
  struct omni_http_request_framing_result result = {0};
  result.status = status;
  result.error_header_index = OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX;
  return result;
}

static unsigned char ascii_lower(unsigned char byte) {
  if (byte >= (unsigned char)'A' && byte <= (unsigned char)'Z') {
    return (unsigned char)(byte + ((unsigned char)'a' - (unsigned char)'A'));
  }
  return byte;
}

static bool header_name_is(const struct omni_http_header_line_span *name, const char *literal,
                           size_t literal_length) {
  if (name->length != literal_length)
    return false;
  for (size_t i = 0u; i < literal_length; ++i) {
    if (ascii_lower(name->data[i]) != (unsigned char)literal[i])
      return false;
  }
  return true;
}

static bool is_ows(unsigned char byte) {
  return byte == (unsigned char)' ' || byte == (unsigned char)'\t';
}

static struct content_length_parse parse_content_length(struct omni_http_header_line_span value) {
  struct content_length_parse parsed = {0};
  size_t offset = 0u;
  size_t common_value = 0u;

  parsed.valid = true;
  for (;;) {
    size_t member_value = 0u;
    size_t digits = 0u;

    size_t whitespace_start = offset;
    while (offset < value.length && is_ows(value.data[offset]))
      ++offset;
    if (offset != whitespace_start && !parsed.has_value) {
      parsed.valid = false;
      return parsed;
    }
    while (offset < value.length && value.data[offset] >= (unsigned char)'0' &&
           value.data[offset] <= (unsigned char)'9') {
      size_t digit = (size_t)(value.data[offset] - (unsigned char)'0');
      if (member_value > (SIZE_MAX - digit) / 10u) {
        parsed.valid = false;
        return parsed;
      }
      member_value = member_value * 10u + digit;
      ++digits;
      ++offset;
    }
    if (digits == 0u) {
      parsed.valid = false;
      return parsed;
    }
    whitespace_start = offset;
    while (offset < value.length && is_ows(value.data[offset]))
      ++offset;
    if (offset == value.length && offset != whitespace_start) {
      parsed.valid = false;
      return parsed;
    }

    if (!parsed.has_value) {
      common_value = member_value;
      parsed.has_value = true;
    } else if (member_value != common_value) {
      parsed.conflict = true;
    }

    if (offset == value.length) {
      parsed.value = common_value;
      return parsed;
    }
    if (value.data[offset] != (unsigned char)',') {
      parsed.valid = false;
      return parsed;
    }
    ++offset;
    /* The next iteration rejects trailing/doubled commas and OWS-only items. */
  }
}

static bool complete_head_is_valid(const struct omni_http_request_head_result *head) {
  if (head->status != OMNI_HTTP_REQUEST_HEAD_COMPLETE || head->error_offset != 0u ||
      head->consumed_bytes == 0u || head->consumed_bytes > OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES ||
      head->request_line.status != OMNI_HTTP_REQUEST_LINE_COMPLETE ||
      head->request_line.error_offset != 0u || head->request_line.consumed_bytes == 0u ||
      head->request_line.consumed_bytes > OMNI_HTTP_REQUEST_LINE_MAX_TOTAL_BYTES ||
      head->request_line.consumed_bytes > head->consumed_bytes ||
      head->request_line.method.data == NULL || head->request_line.method.length == 0u ||
      head->request_line.method.length > OMNI_HTTP_REQUEST_LINE_MAX_METHOD_BYTES ||
      head->request_line.target.data == NULL || head->request_line.target.length == 0u ||
      head->request_line.target.length > OMNI_HTTP_REQUEST_LINE_MAX_TARGET_BYTES ||
      (head->request_line.version != OMNI_HTTP_VERSION_1_0 &&
       head->request_line.version != OMNI_HTTP_VERSION_1_1) ||
      head->header_count > OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS ||
      (head->header_count != 0u && head->headers == NULL)) {
    return false;
  }
  return true;
}

struct omni_http_request_framing_result
omni_http_request_framing_analyze(const struct omni_http_request_head_result *head) {
  struct omni_http_request_framing_result result = result_at(OMNI_HTTP_REQUEST_FRAMING_OK);
  bool content_length_seen = false;
  bool content_length_invalid = false;
  bool content_length_conflict = false;
  size_t first_content_length_index = OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX;
  size_t invalid_content_length_index = OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX;
  size_t conflict_index = OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX;
  size_t first_transfer_encoding_index = OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX;
  size_t common_value = 0u;
  bool invalid_head = false;

  if (head == NULL)
    return result_at(OMNI_HTTP_REQUEST_FRAMING_INVALID_ARGUMENT);
  if (!complete_head_is_valid(head))
    return result_at(OMNI_HTTP_REQUEST_FRAMING_INVALID_HEAD);

  for (size_t i = 0u; i < head->header_count; ++i) {
    const struct omni_http_header *header = &head->headers[i];
    bool is_content_length;
    bool is_transfer_encoding;

    /* These shape checks keep malformed caller-constructed results bounded. */
    if (header->name.data == NULL || header->name.length == 0u ||
        header->name.length > OMNI_HTTP_HEADER_LINE_MAX_NAME_BYTES || header->value.data == NULL ||
        header->value.length > OMNI_HTTP_HEADER_LINE_MAX_VALUE_BYTES) {
      invalid_head = true;
      continue;
    }

    is_content_length = header_name_is(&header->name, "content-length", 14u);
    is_transfer_encoding = header_name_is(&header->name, "transfer-encoding", 17u);
    if (is_transfer_encoding) {
      result.transfer_encoding_present = true;
      if (first_transfer_encoding_index == OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX) {
        first_transfer_encoding_index = i;
      }
    }
    if (is_content_length) {
      struct content_length_parse parsed;
      ++result.content_length_field_count;
      if (!content_length_seen)
        first_content_length_index = i;
      content_length_seen = true;
      parsed = parse_content_length(header->value);
      if (!parsed.valid) {
        if (!content_length_invalid)
          invalid_content_length_index = i;
        content_length_invalid = true;
        continue;
      }
      if (i == first_content_length_index) {
        common_value = parsed.value;
      } else if (parsed.value != common_value) {
        if (!content_length_conflict)
          conflict_index = i;
        content_length_conflict = true;
      }
      if (parsed.conflict) {
        if (!content_length_conflict)
          conflict_index = i;
        content_length_conflict = true;
      }
    }
  }

  if (invalid_head)
    return result_at(OMNI_HTTP_REQUEST_FRAMING_INVALID_HEAD);
  if (content_length_invalid) {
    result.status = OMNI_HTTP_REQUEST_FRAMING_INVALID_CONTENT_LENGTH;
    result.error_header_index = invalid_content_length_index;
    return result;
  }
  if (content_length_conflict) {
    result.status = OMNI_HTTP_REQUEST_FRAMING_CONFLICTING_CONTENT_LENGTH;
    result.error_header_index = conflict_index;
    return result;
  }
  if (content_length_seen && result.transfer_encoding_present) {
    result.status = OMNI_HTTP_REQUEST_FRAMING_AMBIGUOUS;
    result.error_header_index = first_content_length_index;
    return result;
  }
  if (result.transfer_encoding_present) {
    result.status = OMNI_HTTP_REQUEST_FRAMING_TRANSFER_ENCODING_UNSUPPORTED;
    result.error_header_index = first_transfer_encoding_index;
    return result;
  }
  if (content_length_seen) {
    result.framing = OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH;
    result.content_length = common_value;
  } else {
    result.framing = OMNI_HTTP_REQUEST_BODY_NONE;
  }
  return result;
}
