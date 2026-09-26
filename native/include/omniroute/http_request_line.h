/*
 * OmniRoute native backend — bounded HTTP request-line parser (Task 033).
 *
 * This standalone protocol primitive parses exactly one
 * METHOD SP REQUEST-TARGET SP HTTP-VERSION CRLF request line. It does not
 * parse headers or a body, perform I/O, allocate memory, or retain state.
 * Parsing is limited by the public method, target, and total-line bounds.
 *
 * Method and target are borrowed spans into the supplied input. They remain
 * valid only while the original input storage remains alive and unchanged.
 * They are not NUL-terminated. The parser stops at the first CRLF and ignores
 * every byte after it, allowing a later layer to parse subsequent data.
 */

#ifndef OMNIROUTE_HTTP_REQUEST_LINE_H
#define OMNIROUTE_HTTP_REQUEST_LINE_H

#include <stddef.h>

#define OMNI_HTTP_REQUEST_LINE_MAX_METHOD_BYTES 32u
#define OMNI_HTTP_REQUEST_LINE_MAX_TARGET_BYTES 4060u
#define OMNI_HTTP_REQUEST_LINE_MAX_TOTAL_BYTES 4096u

struct omni_http_byte_span {
  const unsigned char *data;
  size_t length;
};

enum omni_http_version {
  OMNI_HTTP_VERSION_UNKNOWN = 0,
  OMNI_HTTP_VERSION_1_0,
  OMNI_HTTP_VERSION_1_1
};

enum omni_http_request_line_status {
  OMNI_HTTP_REQUEST_LINE_COMPLETE = 0,
  OMNI_HTTP_REQUEST_LINE_INCOMPLETE,
  OMNI_HTTP_REQUEST_LINE_INVALID,
  OMNI_HTTP_REQUEST_LINE_TOO_LARGE,
  OMNI_HTTP_REQUEST_LINE_UNSUPPORTED_VERSION,
  OMNI_HTTP_REQUEST_LINE_ERR_INVALID_ARGUMENT
};

struct omni_http_request_line_result {
  enum omni_http_request_line_status status;
  struct omni_http_byte_span method;
  struct omni_http_byte_span target;
  enum omni_http_version version;
  size_t consumed_bytes;
  size_t error_offset;
};

/*
 * Parse one bounded request line from a caller-owned byte span. NULL with
 * zero length is an empty, incomplete prefix; NULL with nonzero length is an
 * invalid argument. The result contains successful spans only for COMPLETE.
 *
 * error_offset is zero on COMPLETE, the offending byte for INVALID, the
 * first unsupported version digit for UNSUPPORTED_VERSION, and the first
 * byte that cannot fit for TOO_LARGE. For INCOMPLETE it identifies the byte
 * offset at which more input is needed.
 */
struct omni_http_request_line_result omni_http_request_line_parse(
    const unsigned char *data, size_t length);

#endif /* OMNIROUTE_HTTP_REQUEST_LINE_H */
