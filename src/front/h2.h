/* P01 HTTP/2 front (05-storage-integrations.md P01, FRONT-02).
 *
 * nghttp2 owns framing and HPACK; this module owns the server-side policy
 * that maps streams onto the application's request/response model. One
 * stream is one owned request plus one response: it is never emulated as an
 * HTTP/1 socket.
 *
 * Transport-private completion key: cf_front_h2_key extends the loop's
 * (connection generation, request sequence) pair with the stream ID, so a
 * stale completion (closed connection, reused slot, finished sequence,
 * reset stream) releases its resources without touching a live stream.
 *
 * Policy (binding, from 05 P01):
 * - SETTINGS_MAX_CONCURRENT_STREAMS is 100.
 * - Required and duplicate pseudo-headers are validated; HTTP/1-only
 *   connection headers are rejected; :authority goes through the same
 *   PUBLIC_ORIGIN check the HTTP/1 loop applies to Host.
 * - Per-stream header/body/queued-byte limits match the HTTP/1 budgets and
 *   connection-global limits are enforced too.
 * - RST_STREAM cancels queued output, never committed (already framed)
 *   writes; GOAWAY drains accepted streams and refuses new ones.
 * - Response DATA is sent only under nghttp2 flow-control allowance (the
 *   data-source read callback fires only when the window allows).
 *
 * Threading mirrors the loop contract: a session and every stream it owns
 * are confined to the owning loop thread. nghttp2 is driven with the
 * memory-transport API (mem_send/mem_recv) so the loop never blocks: wire
 * bytes move between the TLS/plain socket buffers and the session only on
 * epoll readiness.
 *
 * Wiring (integrator-owned, proposed as patch snippets in the handoff):
 * create one session per TLS+h2 connection after ALPN selects "h2", feed it
 * from the TLS recv step, flush mem_send bytes through the TLS send step,
 * and admit each validated stream through the same admit hook workers use
 * for HTTP/1 (key carried as the transport-private completion token). */
#ifndef CF_FRONT_H2_H
#define CF_FRONT_H2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cf.h"

/* Forward declaration; nghttp2_session is nghttp2's opaque session type.
 * Including <nghttp2/nghttp2.h> is only required to drive the handle. */
typedef struct nghttp2_session nghttp2_session;

typedef struct cf_front_h2_session cf_front_h2_session;

/* Fixed budgets: stream limits match the HTTP/1 budgets (http_internal.h);
 * connection-global limits match the loop-wide input/output reservations. */
#define CF_FRONT_H2_MAX_CONCURRENT_STREAMS ((uint32_t)100)
#define CF_FRONT_H2_HEADER_MAX ((size_t)32768)
#define CF_FRONT_H2_HEADER_COUNT_MAX ((size_t)100)
#define CF_FRONT_H2_TARGET_MAX ((size_t)8192)
#define CF_FRONT_H2_BODY_MAX ((size_t)16 * 1024 * 1024)
#define CF_FRONT_H2_QUEUED_MAX ((size_t)8 * 1024 * 1024)
#define CF_FRONT_H2_CONN_INPUT_MAX ((size_t)64 * 1024 * 1024)
#define CF_FRONT_H2_CONN_OUTPUT_MAX ((size_t)64 * 1024 * 1024)

/* Transport-private completion key: the loop's connection generation and
 * request sequence plus the h2 stream ID. Retains generation + sequence so
 * stale completions are detectable exactly like HTTP/1 slot reuse. */
typedef struct {
    cf_conn_id conn;
    uint64_t sequence;
    int32_t stream_id;
} cf_front_h2_key;

/* True when the key still names a live stream: the connection id matches
 * (slot + generation), the request sequence matches, and the stream is
 * still open. Anything else is a stale completion: release resources,
 * never write. */
bool cf_front_h2_key_valid(const cf_front_h2_key *key, cf_conn_id live_conn,
                            uint64_t live_sequence, bool stream_open);

/* Header-block validation outcome. */
typedef enum {
    CF_FRONT_H2_HEADERS_OK = 0,
    CF_FRONT_H2_HEADERS_MISSING_PSEUDO,
    CF_FRONT_H2_HEADERS_DUPLICATE_PSEUDO,
    CF_FRONT_H2_HEADERS_CONN_HEADER,
    CF_FRONT_H2_HEADERS_BAD_AUTHORITY,
    CF_FRONT_H2_HEADERS_BAD_TARGET,
    CF_FRONT_H2_HEADERS_TOO_MANY,
    CF_FRONT_H2_HEADERS_TOO_LARGE,
    CF_FRONT_H2_HEADERS_BAD_NAME
} cf_front_h2_headers_result;

/* Validate one decoded header block. names/values are raw (not
 * NUL-terminated) pairs in received order. Pseudo-headers must precede
 * regular headers; :method, :scheme, :path and :authority are required
 * exactly once; HTTP/1-only connection headers (connection, keep-alive,
 * proxy-authenticate, proxy-authorization, te, trailer, transfer-encoding,
 * upgrade) are rejected; :authority must pass cf_front_h2_authority_allowed
 * against public_origin; :path must be origin-form within target budget.
 * On OK, *authority_out (when non-NULL) borrows the :authority value for
 * the call only. */
cf_front_h2_headers_result cf_front_h2_check_headers(
    const unsigned char **names, const size_t *name_lens,
    const unsigned char **values, const size_t *value_lens, size_t count,
    const char *public_origin, const unsigned char **authority_out,
    size_t *authority_len_out);

/* The same PUBLIC_ORIGIN check the HTTP/1 loop applies to Host, applied to
 * :authority: host (or [literal]/host:port, no userinfo/whitespace) matches
 * the origin host case-insensitively; an explicit port must equal the
 * origin's; a portless authority matches only when the origin's port is the
 * scheme default. */
bool cf_front_h2_authority_allowed(const char *authority, size_t authority_len,
                                    const char *public_origin);

/* Create a server session bound to one connection. public_origin is
 * borrowed for the session lifetime (the caller keeps the loop's copy
 * alive). Advertises MAX_CONCURRENT_STREAMS 100 in SETTINGS. */
cf_err cf_front_h2_session_create(const char *public_origin,
                                   cf_front_h2_session **out);

/* Release a session and every stream record (accepts NULL). Queued response
 * bytes are released; committed (already framed) bytes already left through
 * mem_send and are never recalled. */
void cf_front_h2_session_destroy(cf_front_h2_session *session);

/* Borrowed nghttp2 handle for the memory-transport pump
 * (nghttp2_session_mem_send/mem_recv) and for test-driven flow-control
 * (window updates, settings). Valid until destroy. */
nghttp2_session *cf_front_h2_handle(cf_front_h2_session *session);

/* Observations for the loop seam and tests. refused_total counts every
 * stream denied admission: our validation/budget/drain refusals plus
 * nghttp2's own enforcement (observed on the send path, never double
 * counted). resets_total counts RST-driven cancellations of admitted
 * streams, both directions. */
size_t cf_front_h2_open_count(const cf_front_h2_session *session);
uint64_t cf_front_h2_admitted_total(const cf_front_h2_session *session);
uint64_t cf_front_h2_refused_total(const cf_front_h2_session *session);
uint64_t cf_front_h2_resets_total(const cf_front_h2_session *session);
bool cf_front_h2_stream_open(const cf_front_h2_session *session,
                              int32_t stream_id);
bool cf_front_h2_stream_cancelled(const cf_front_h2_session *session,
                                   int32_t stream_id);
size_t cf_front_h2_stream_body_len(const cf_front_h2_session *session,
                                    int32_t stream_id);
/* Queued (submitted, not yet framed-out) response bytes on the stream. */
size_t cf_front_h2_stream_queued(const cf_front_h2_session *session,
                                  int32_t stream_id);
/* Bytes already handed to nghttp2 framing (committed; RST cannot recall). */
size_t cf_front_h2_stream_sent(const cf_front_h2_session *session,
                                int32_t stream_id);
/* True once a response was submitted for the stream (committed point). */
bool cf_front_h2_stream_committed(const cf_front_h2_session *session,
                                   int32_t stream_id);
/* True after GOAWAY was submitted: accepted streams drain, new streams are
 * refused. */
bool cf_front_h2_draining(const cf_front_h2_session *session);

/* True when the session wants to write but no flow-control allowance
 * remains (connection or any stream with queued bytes has a zero remote
 * window): the loop must wait for WINDOW_UPDATE, never busy-spin. */
bool cf_front_h2_flow_stalled(cf_front_h2_session *session);

/* Queue a response (status + optional body) on an admitted stream. Copies
 * the body; DATA frames leave only under flow-control allowance via the
 * data-source read callback. First call commits the stream: later
 * RST_STREAM cancels delivery, not committed bytes. */
cf_err cf_front_h2_submit_response(cf_front_h2_session *session,
                                    int32_t stream_id, unsigned status,
                                    const unsigned char *body, size_t body_len);

/* Cancel a stream (RST_STREAM CANCEL). Drops queued-but-unframed output;
 * committed bytes already left through mem_send. Idempotent. */
cf_err cf_front_h2_rst_stream(cf_front_h2_session *session, int32_t stream_id,
                               uint32_t error_code);

/* Submit GOAWAY and enter draining: accepted streams run to completion,
 * new streams are refused. Idempotent. */
cf_err cf_front_h2_goaway(cf_front_h2_session *session);

#endif /* CF_FRONT_H2_H */
