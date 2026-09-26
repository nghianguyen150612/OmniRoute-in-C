/*
 * OmniRoute native backend — bounded HTTP header-line parser (Task 034).
 *
 * This standalone protocol primitive parses exactly one normal field line.
 * It does not parse a header block, perform I/O, allocate memory, or retain
 * state. Returned name/value spans borrow the input storage and remain valid
 * only while that storage remains alive and unchanged.
 */

#ifndef OMNIROUTE_HTTP_HEADER_LINE_H
#define OMNIROUTE_HTTP_HEADER_LINE_H

#include <stddef.h>

#define OMNI_HTTP_HEADER_LINE_MAX_NAME_BYTES 256u
#define OMNI_HTTP_HEADER_LINE_MAX_VALUE_BYTES 3837u
#define OMNI_HTTP_HEADER_LINE_MAX_TOTAL_BYTES 4096u

struct omni_http_header_line_span {
  const unsigned char *data;
  size_t length;
};

enum omni_http_header_line_status {
  OMNI_HTTP_HEADER_LINE_COMPLETE = 0,
  OMNI_HTTP_HEADER_LINE_INCOMPLETE,
  OMNI_HTTP_HEADER_LINE_INVALID,
  OMNI_HTTP_HEADER_LINE_TOO_LARGE,
  OMNI_HTTP_HEADER_LINE_ERR_INVALID_ARGUMENT
};

struct omni_http_header_line_result {
  enum omni_http_header_line_status status;
  struct omni_http_header_line_span name;
  struct omni_http_header_line_span value;
  size_t consumed_bytes;
  size_t error_offset;
};

/*
 * Parse one bounded field-name ":" OWS field-value OWS CRLF line. The total
 * byte limit includes CRLF. The value limit counts bytes in the returned
 * value span after leading and trailing OWS are removed. NULL with zero
 * length is an incomplete prefix; NULL with nonzero length is invalid.
 * Non-success results expose no spans and consume zero bytes.
 *
 * error_offset is zero on COMPLETE and ERR_INVALID_ARGUMENT, the offending
 * byte for INVALID, the first byte that cannot fit for TOO_LARGE, and the
 * offset where more input is needed for INCOMPLETE.
 */
struct omni_http_header_line_result omni_http_header_line_parse(
    const unsigned char *data, size_t length);

#endif /* OMNIROUTE_HTTP_HEADER_LINE_H */
