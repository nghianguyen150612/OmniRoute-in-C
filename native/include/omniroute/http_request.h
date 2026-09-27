/* OmniRoute native backend — bounded incremental HTTP request view (Task 038). */
#ifndef OMNIROUTE_HTTP_REQUEST_H
#define OMNIROUTE_HTTP_REQUEST_H

#include <stddef.h>

#include "omniroute/bytebuf.h"
#include "omniroute/http_request_body.h"

enum omni_http_request_status {
  OMNI_HTTP_REQUEST_COMPLETE = 0,
  OMNI_HTTP_REQUEST_INCOMPLETE,
  OMNI_HTTP_REQUEST_INVALID_REQUEST_HEAD,
  OMNI_HTTP_REQUEST_UNSUPPORTED_VERSION,
  OMNI_HTTP_REQUEST_REQUEST_HEAD_TOO_LARGE,
  OMNI_HTTP_REQUEST_TOO_MANY_HEADERS,
  OMNI_HTTP_REQUEST_INVALID_FRAMING,
  OMNI_HTTP_REQUEST_UNSUPPORTED_FRAMING,
  OMNI_HTTP_REQUEST_BODY_OVERFLOW_ERROR,
  OMNI_HTTP_REQUEST_ERR_INVALID_ARGUMENT,
  OMNI_HTTP_REQUEST_ERR_INVALID_STATE
};

struct omni_http_request_result {
  enum omni_http_request_status status;
  struct omni_http_request_head_result request_head;
  struct omni_http_request_framing_result framing;
  struct omni_http_request_body_result body;
  size_t consumed_bytes;
  size_t required_total_bytes;
};

/*
 * Assemble at most one request from the bytebuf's current readable region.
 * The input, header array, and all returned spans are borrowed; this function
 * never allocates, retains state, or mutates the bytebuf. On COMPLETE,
 * consumed_bytes and required_total_bytes are the exact request length
 * relative to the readable-region start. On every other result,
 * consumed_bytes is zero and the child views are not a logical request view.
 *
 * A short request head reports INCOMPLETE with required_total_bytes zero. A
 * complete head with a short fixed body reports INCOMPLETE with the exact
 * required total from Task 037. Header storage is caller-owned and follows
 * Task 035's capacity rules; NULL storage is valid only with zero capacity.
 *
 * Returned spans become invalid if the caller later appends, consumes,
 * compacts, resets, destroys, or otherwise moves or modifies the bytebuf
 * backing storage. Task 038 does not perform any of those operations.
 */
struct omni_http_request_result omni_http_request_assemble(
    const struct omni_bytebuf *buffer, struct omni_http_header *headers, size_t header_capacity);

#endif /* OMNIROUTE_HTTP_REQUEST_H */
