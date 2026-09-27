/*
 * OmniRoute native backend — bounded HTTP request consumption (Task 039).
 *
 * Task 038 assembles a borrowed COMPLETE request view from a bytebuf's current
 * readable region. This primitive closes that half of the lifecycle: it
 * validates that one assembled result is still consumable and then advances
 * the bytebuf's read offset by exactly that request's length, leaving any
 * trailing/pipelined bytes readable for an explicit next assemble call.
 *
 *   receive bytebuf
 *        -> omni_http_request_assemble()   [Task 038, borrowed view]
 *        -> caller processes the borrowed view
 *        -> omni_http_request_consume()    [Task 039, this module]
 *        -> next readable region / next pipelined request
 *
 * What this primitive deliberately does NOT do:
 * - no parsing, routing, request handling, or response generation;
 * - no re-derivation of the request length (Task 038's COMPLETE boundary is
 *   authoritative once validated);
 * - no automatic second assemble: only ONE request is consumed per call even
 *   when further complete requests are already buffered;
 * - no omni_bytebuf_compact() and no omni_bytebuf_reset(): compaction stays an
 *   explicit caller policy, so repeated pipelined consumption progresses
 *   purely through read offsets. The canonical empty-buffer collapse already
 *   documented for omni_bytebuf_consume() is inherited from that primitive.
 * - no heap, no I/O, no retained state, no request registry, no generation
 *   counter, and no writes to caller-owned memory.
 *
 * Work and persistent state are O(1) and zero bytes.
 */
#ifndef OMNIROUTE_HTTP_REQUEST_CONSUME_H
#define OMNIROUTE_HTTP_REQUEST_CONSUME_H

#include <stddef.h>

#include "omniroute/bytebuf.h"
#include "omniroute/http_request.h"

enum omni_http_request_consume_status {
  OMNI_HTTP_REQUEST_CONSUME_OK = 0,
  /* NULL bytebuf/result, or a bytebuf that is not live. */
  OMNI_HTTP_REQUEST_CONSUME_ERR_INVALID_ARGUMENT,
  /* The Task 038 result is not COMPLETE; no bytes were consumed. */
  OMNI_HTTP_REQUEST_CONSUME_ERR_NOT_COMPLETE,
  /* The result's assembly snapshot no longer names the current readable
     start, or the readable region is empty. */
  OMNI_HTTP_REQUEST_CONSUME_ERR_STALE,
  /* COMPLETE metadata is internally inconsistent (zero length, boundary past
     the readable region, or a snapshot shorter than the boundary). */
  OMNI_HTTP_REQUEST_CONSUME_ERR_INCONSISTENT,
  /* omni_bytebuf_consume() itself refused the operation. */
  OMNI_HTTP_REQUEST_CONSUME_ERR_BYTEBUF
};

struct omni_http_request_consume_result {
  enum omni_http_request_consume_status status;
  size_t consumed_bytes;           /* bytes removed; 0 on every failure */
  size_t remaining_readable_bytes; /* readable bytes after consumption */
};

/*
 * Consume exactly one previously assembled COMPLETE request from `buffer`,
 * the same bytebuf the request was assembled from.
 *
 * Permitted only when request->status is OMNI_HTTP_REQUEST_COMPLETE and the
 * result is consistent with the CURRENT readable state:
 *   - the current readable start equals the assembly snapshot
 *     (request->source_read_ptr, recorded by Task 038);
 *   - the current readable-view generation equals the assembly snapshot
 *     (request->source_read_generation, recorded by Task 038 from
 *     omni_bytebuf_read_generation) — the pointer alone is not identity,
 *     because consume-to-empty and compact can return the readable start to
 *     the same backing address for a different region;
 *   - the readable region is non-empty;
 *   - request->consumed_bytes is nonzero and no larger than the current
 *     readable length;
 *   - request->source_readable_length covers the consumed boundary.
 *
 * On OK: exactly request->consumed_bytes are removed and
 * remaining_readable_bytes is the bytebuf's readable length afterwards. Any
 * trailing or pipelined bytes stay readable, untouched, at their existing
 * backing offsets. Caller header storage, the request result, and request
 * bytes are never modified.
 *
 * On any failure: status is set, consumed_bytes is 0, and the bytebuf is left
 * exactly as it was — no best-effort or partial consumption happens.
 *
 * Stale-result safety contract (precise, not a blanket claim): this primitive
 * verifies that the COMPLETE view still refers to the current readable-region
 * start under the same readable-view generation epoch, and that its boundary
 * is still available. The generation check is what makes same-address reuse
 * detectable: after a consume-to-empty or a compaction, the readable start
 * can equal the old snapshot pointer while naming a completely different
 * region, and only the epoch distinguishes them. This catches the ordinary
 * lifecycle mistakes: consuming a result twice, consuming an old result after
 * a pipelined consume, consuming after a compaction or reset (including a
 * reset/refill that restores the same readable address), and consuming after
 * an external partial consume. Callers must not otherwise modify or move the
 * underlying bytebuf contents between assembly and consumption; an arbitrary
 * same-address overwrite in place is outside the borrowed-view contract.
 * Appending to the tail between assembly and consumption is safe: neither the
 * readable start nor the generation moves, so the same result still consumes
 * correctly and the newly arrived bytes become the next request.
 *
 * Borrowed-view lifetime: after a successful consume the request result's
 * borrowed spans must no longer be used as a current request view. The
 * result struct itself is not cleared or rewritten here — the caller owns it.
 */
struct omni_http_request_consume_result omni_http_request_consume(
    struct omni_bytebuf *buffer, const struct omni_http_request_result *request);

#endif /* OMNIROUTE_HTTP_REQUEST_CONSUME_H */
