/* OmniRoute native backend — bounded HTTP/1.1 response-head builder (Task 041).
 */
#ifndef OMNIROUTE_HTTP_RESPONSE_HEAD_H
#define OMNIROUTE_HTTP_RESPONSE_HEAD_H

#include <stddef.h>

#define OMNI_HTTP_RESPONSE_HEAD_MAX_HEADERS 64u
#define OMNI_HTTP_RESPONSE_HEAD_MAX_BYTES 16384u

/* Borrowed response metadata. Neither span requires NUL termination. */
struct omni_http_response_header {
  const unsigned char *name;
  size_t name_length;
  const unsigned char *value;
  size_t value_length;
};

enum omni_http_response_head_status {
  OMNI_HTTP_RESPONSE_HEAD_OK = 0,
  OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_ARGUMENT,
  OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_STATUS,
  OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_REASON,
  OMNI_HTTP_RESPONSE_HEAD_ERR_TOO_MANY_HEADERS,
  OMNI_HTTP_RESPONSE_HEAD_ERR_INVALID_HEADER,
  OMNI_HTTP_RESPONSE_HEAD_ERR_TOO_LARGE,
  OMNI_HTTP_RESPONSE_HEAD_ERR_OUTPUT_TOO_SMALL,
  OMNI_HTTP_RESPONSE_HEAD_ERR_OVERFLOW
};

struct omni_http_response_head_result {
  enum omni_http_response_head_status status;
  size_t written_bytes;
  size_t required_bytes;
};

/*
 * Build one complete HTTP/1.1 response head into caller-owned storage.
 *
 * The status code must be exactly representable as three decimal digits
 * (100..999). Reason and header metadata are borrowed and are validated as
 * conservative ASCII HTTP syntax: reason/value bytes are HTAB or visible
 * ASCII (SP through '~'), while names use HTTP token bytes. CR, LF, NUL,
 * other controls, and high bytes are rejected. Empty reason and value spans
 * are valid; an empty reason still emits the required space before CRLF.
 *
 * Header order, spelling/case, and duplicates are preserved. No headers are
 * added and no header semantics, body bytes, Content-Length policy, or
 * networking are involved. `headers == NULL, header_count == 0` is valid;
 * NULL with a positive count is invalid. `output == NULL, output_capacity ==
 * 0` is a measure-only query that returns OUTPUT_TOO_SMALL for valid metadata;
 * NULL with positive capacity is invalid. On every failure output is untouched
 * and written_bytes is zero. Output storage must not overlap borrowed input.
 */
struct omni_http_response_head_result omni_http_response_head_build(
    unsigned int status_code, const unsigned char *reason, size_t reason_length,
    const struct omni_http_response_header *headers, size_t header_count,
    unsigned char *output, size_t output_capacity);

#endif /* OMNIROUTE_HTTP_RESPONSE_HEAD_H */
