/* OmniRoute native backend — bounded HTTP body view (Task 037). */
#ifndef OMNIROUTE_HTTP_REQUEST_BODY_H
#define OMNIROUTE_HTTP_REQUEST_BODY_H

#include <stddef.h>

#include "omniroute/http_request_framing.h"

struct omni_http_request_body_span {
  const unsigned char *data;
  size_t length;
};

enum omni_http_request_body_status {
  OMNI_HTTP_REQUEST_BODY_COMPLETE = 0,
  OMNI_HTTP_REQUEST_BODY_INCOMPLETE,
  OMNI_HTTP_REQUEST_BODY_UNSUPPORTED_FRAMING,
  OMNI_HTTP_REQUEST_BODY_INVALID_FRAMING,
  OMNI_HTTP_REQUEST_BODY_INVALID_ARGUMENT,
  OMNI_HTTP_REQUEST_BODY_INVALID_STATE,
  OMNI_HTTP_REQUEST_BODY_OVERFLOW
};

struct omni_http_request_body_result {
  enum omni_http_request_body_status status;
  struct omni_http_request_body_span body;
  size_t consumed_bytes;
  size_t required_total_bytes;
};

/*
 * View the complete body described by valid Task 035 and Task 036 results.
 * `data` must be the same contiguous request input used for those results.
 * The body span borrows that storage and remains valid only while the storage
 * stays alive and unchanged. This function retains no pointers or state.
 *
 * NO_BODY completes at the request-head boundary. FIXED_LENGTH requires the
 * entire declared body and returns exactly that span; incomplete results
 * expose no body and consume no bytes. Bytes after the body remain untouched.
 * Body bytes are opaque binary data. Transfer-Encoding and invalid framing
 * are rejected. No body-size policy or arbitrary body limit is applied here.
 *
 * The function does not reparse headers or body bytes, allocate memory, or
 * perform I/O. Its work and persistent state are O(1) and zero bytes.
 */
struct omni_http_request_body_result
omni_http_request_body_view(const unsigned char *data, size_t length,
                            const struct omni_http_request_head_result *head,
                            const struct omni_http_request_framing_result *framing);

#endif /* OMNIROUTE_HTTP_REQUEST_BODY_H */
