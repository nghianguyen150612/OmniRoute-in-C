/* OmniRoute native backend — bounded request-head composition (Task 035). */
#ifndef OMNIROUTE_HTTP_REQUEST_HEAD_H
#define OMNIROUTE_HTTP_REQUEST_HEAD_H

#include "omniroute/http_header_line.h"
#include "omniroute/http_request_line.h"

#define OMNI_HTTP_REQUEST_HEAD_MAX_HEADERS 64u
#define OMNI_HTTP_REQUEST_HEAD_MAX_TOTAL_BYTES 16384u

struct omni_http_header {
  struct omni_http_header_line_span name;
  struct omni_http_header_line_span value;
};

enum omni_http_request_head_status {
  OMNI_HTTP_REQUEST_HEAD_COMPLETE = 0,
  OMNI_HTTP_REQUEST_HEAD_INCOMPLETE,
  OMNI_HTTP_REQUEST_HEAD_INVALID,
  OMNI_HTTP_REQUEST_HEAD_TOO_LARGE,
  OMNI_HTTP_REQUEST_HEAD_TOO_MANY_HEADERS,
  OMNI_HTTP_REQUEST_HEAD_UNSUPPORTED_VERSION,
  OMNI_HTTP_REQUEST_HEAD_ERR_INVALID_ARGUMENT
};

struct omni_http_request_head_result {
  enum omni_http_request_head_status status;
  struct omni_http_request_line_result request_line;
  const struct omni_http_header *headers;
  size_t header_count;
  size_t consumed_bytes;
  size_t error_offset;
};

/*
 * Compose Task 033 and Task 034 without allocation, I/O, or persistent state.
 * COMPLETE includes the final blank CRLF; later bytes are never inspected.
 * Header order, case, and duplicates are preserved; no semantics or body
 * framing are interpreted. Leading SP/HTAB header lines (obs-fold) are invalid.
 * Both the caller capacity and MAX_HEADERS apply; a complete extra field
 * returns TOO_MANY_HEADERS with its starting offset, before any storage write.
 * The total head limit includes the request line and final blank CRLF.
 *
 * All borrowed spans are valid only while the original input buffer remains
 * alive and unchanged. Header storage is caller-owned, must not overlap the
 * input, and is never retained. NULL storage is allowed only at capacity zero.
 * NULL data with length zero is INCOMPLETE; with nonzero length it is invalid.
 *
 * On non-success all logical outputs are zero/NULL (including request_line).
 * Previously touched caller entries may contain unspecified intermediate
 * values and are not valid output. No O(N) clearing is performed.
 * error_offset is global: child errors are translated from their subspan;
 * INCOMPLETE points where input is needed; total TOO_LARGE points at the
 * total limit; obs-fold points at its initial SP/HTAB; argument errors and
 * COMPLETE use zero. Child line limits retain their offending-byte offsets.
 */
struct omni_http_request_head_result omni_http_request_head_parse(const unsigned char *data,
                                                                  size_t length,
                                                                  struct omni_http_header *headers,
                                                                  size_t header_capacity);

#endif /* OMNIROUTE_HTTP_REQUEST_HEAD_H */
