/* OmniRoute native backend — bounded HTTP response-message view (Task 042). */
#ifndef OMNIROUTE_HTTP_RESPONSE_H
#define OMNIROUTE_HTTP_RESPONSE_H

#include <stddef.h>

#include "omniroute/http_response_head.h"

/* Borrowed immutable bytes. The span does not own or copy its storage. */
struct omni_http_response_span {
  const unsigned char *data;
  size_t length;
};

enum omni_http_response_status {
  OMNI_HTTP_RESPONSE_OK = 0,
  OMNI_HTTP_RESPONSE_ERR_INVALID_ARGUMENT,
  OMNI_HTTP_RESPONSE_ERR_INVALID_HEAD,
  OMNI_HTTP_RESPONSE_ERR_INCONSISTENT_HEAD,
  OMNI_HTTP_RESPONSE_ERR_OVERFLOW
};

struct omni_http_response_result {
  enum omni_http_response_status status;
  struct omni_http_response_span head;
  struct omni_http_response_span body;
  size_t total_bytes;
};

/*
 * Compose a successful Task 041 head result and borrowed body bytes into one
 * immutable logical response description. The caller must pair head_result
 * with the same head storage produced by that Task 041 invocation. This
 * function validates metadata consistency but does not reparse or authenticate
 * the head bytes. Neither storage is copied or retained.
 *
 * A successful view borrows both storages, which must remain alive and
 * unchanged while the view is used. A NULL body is valid only with length 0.
 * Body bytes are opaque: no HTTP headers, content length, or byte contents are
 * inspected. Head and body storage may be separate and non-contiguous.
 */
struct omni_http_response_result omni_http_response_view(
    const unsigned char *head_data, size_t head_available,
    const struct omni_http_response_head_result *head_result,
    const unsigned char *body_data, size_t body_length);

#endif /* OMNIROUTE_HTTP_RESPONSE_H */
