/* OmniRoute native backend — bounded request framing analysis (Task 036). */
#ifndef OMNIROUTE_HTTP_REQUEST_FRAMING_H
#define OMNIROUTE_HTTP_REQUEST_FRAMING_H

#include <stdbool.h>
#include <stddef.h>

#include "omniroute/http_request_head.h"

#define OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX ((size_t)-1)

enum omni_http_request_framing_status {
  OMNI_HTTP_REQUEST_FRAMING_OK = 0,
  OMNI_HTTP_REQUEST_FRAMING_INVALID_ARGUMENT,
  OMNI_HTTP_REQUEST_FRAMING_INVALID_HEAD,
  OMNI_HTTP_REQUEST_FRAMING_INVALID_CONTENT_LENGTH,
  OMNI_HTTP_REQUEST_FRAMING_CONFLICTING_CONTENT_LENGTH,
  OMNI_HTTP_REQUEST_FRAMING_AMBIGUOUS,
  OMNI_HTTP_REQUEST_FRAMING_TRANSFER_ENCODING_UNSUPPORTED
};

enum omni_http_request_body_framing {
  OMNI_HTTP_REQUEST_BODY_NONE = 0,
  OMNI_HTTP_REQUEST_BODY_FIXED_LENGTH
};

struct omni_http_request_framing_result {
  enum omni_http_request_framing_status status;
  enum omni_http_request_body_framing framing;
  size_t content_length;
  size_t content_length_field_count;
  bool transfer_encoding_present;
  size_t error_header_index;
};

/*
 * Analyze framing metadata from a COMPLETE Task 035 result. This scans its
 * bounded headers only; it never reads the request body or reparses raw syntax.
 * Equal duplicate/coalesced Content-Length values are accepted. Any malformed
 * or overflowing value, disagreement, or Content-Length + Transfer-Encoding
 * pair is rejected. Transfer-Encoding alone is explicitly unsupported.
 * Content-Length: 0 remains FIXED_LENGTH(0). No method body policy is applied.
 *
 * Failure precedence is: invalid argument/head, invalid Content-Length,
 * conflicting Content-Length, Content-Length/Transfer-Encoding ambiguity,
 * unsupported Transfer-Encoding, then fixed length or no body. Therefore
 * malformed Content-Length wins over Transfer-Encoding ambiguity regardless
 * of header order. error_header_index identifies the first relevant offending
 * Content-Length (or first Transfer-Encoding for unsupported-only results),
 * otherwise it is OMNI_HTTP_REQUEST_FRAMING_NO_HEADER_INDEX.
 */
struct omni_http_request_framing_result
omni_http_request_framing_analyze(const struct omni_http_request_head_result *head);

#endif /* OMNIROUTE_HTTP_REQUEST_FRAMING_H */
