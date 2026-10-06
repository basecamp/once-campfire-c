/* src/integrations/webhook.h — I01 webhook delivery (05-storage-integrations.md I01).
 *
 * Reference: tmp/rust-ref/crates/campfire/src/integrations/webhook.rs
 * (`Webhook#deliver`): a bot's webhook gets the message as JSON, and its
 * answer becomes the bot's reply.
 *
 * Contract (mirrored):
 *  - Intentionally UNGUARDED (unlike unfurl): only an administrator sets the
 *    URL and it may point at internal services. No guard, no pinning.
 *  - 7 s connect/read inactivity (ENDPOINT_TIMEOUT), 60 s total
 *    (DELIVERY_DEADLINE); the reply is read up to 100 MB decoded
 *    (MAX_REPLY_SIZE). A larger reply fails the delivery.
 *  - Exact request: POST of the payload bytes with Content-Type
 *    application/json (+ Net::HTTP defaults Accept-Encoding/Accept/
 *    User-Agent: Ruby/Host/Connection: close/Content-Length).
 *  - Reply: no Content-Type => none; 200 + text/html|text/plain => text
 *    (lossy UTF-8); anything else => attachment via Mime::Type.lookup
 *    semantics ("attachment.<symbol>", registered spelling; unregistered
 *    types must still match MIME_REGEXP; invalid => failed delivery).
 *  - Timeouts (connect, read, or overall) are answered with the
 *    "Failed to respond within N seconds" text reply (N = 7 or 60), not a
 *    failure. Any other transport failure fails the delivery.
 *  - No automatic retry of POST deliveries; a successful request with a
 *    malformed response is not retried. Failures are counted per job kind
 *    by the J02 consumer (jobs.h accounting), never here.
 */
#ifndef CF_INTEGRATIONS_WEBHOOK_H
#define CF_INTEGRATIONS_WEBHOOK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "integrations/http.h"

/* Webhook::ENDPOINT_TIMEOUT (db model constant, 7 s). */
#define CF_WEBHOOK_ENDPOINT_TIMEOUT_SECS 7
/* Whole-delivery deadline (60 s). */
#define CF_WEBHOOK_DELIVERY_DEADLINE_SECS 60
/* Largest reply read after decompression (100 MB). */
#define CF_WEBHOOK_MAX_REPLY_SIZE ((size_t)100 * 1024 * 1024)

typedef struct {
    cf_http_config http; /* timeouts/caps preset by cf_webhook_default_config */
} cf_webhook_config;

void cf_webhook_default_config(cf_webhook_config *c);

typedef enum {
    CF_WEBHOOK_REPLY_NONE = 0,
    CF_WEBHOOK_REPLY_TEXT,
    CF_WEBHOOK_REPLY_ATTACHMENT
} cf_webhook_reply_kind;

typedef enum {
    CF_WEBHOOK_OK = 0,       /* delivered (reply kind says what came back) */
    CF_WEBHOOK_NOMEM,
    CF_WEBHOOK_INVALID_URL,  /* bad URI / non-HTTP / missing host/bad port */
    CF_WEBHOOK_TRANSPORT,    /* connection/TLS/read failure (not a timeout) */
    CF_WEBHOOK_INVALID_MIME, /* Mime::Type::InvalidMimeType */
    CF_WEBHOOK_REPLY_LARGE,  /* reply past MAX_REPLY_SIZE */
    CF_WEBHOOK_IO            /* other failure */
} cf_webhook_err;

const char *cf_webhook_err_name(cf_webhook_err e);

typedef struct {
    bool has_status;   /* false when the delivery timed out */
    unsigned status;
    cf_webhook_reply_kind kind;
    /* TEXT: text is the lossy-UTF-8 body. ATTACHMENT: data/filename/content_type. */
    char *text;
    size_t text_len;
    unsigned char *data;
    size_t data_len;
    char *filename;
    char *content_type;
    /* Timeout outcome: caller formats "Failed to respond within N seconds". */
    bool timed_out;
    unsigned timeout_secs; /* 7 or 60 */
} cf_webhook_delivery;

void cf_webhook_delivery_dispose(cf_webhook_delivery *d);

/* webhook.deliver(url, payload): POST once, classify the reply. Never
 * retries. Out is empty unless CF_WEBHOOK_OK. */
cf_webhook_err cf_webhook_deliver(const cf_webhook_config *cfg, const char *url,
                                  const unsigned char *payload, size_t payload_len,
                                  cf_webhook_delivery *out);

/* Pure reply classifier (status + stripped Content-Type + body) for tests
 * and for the J02 consumer. content_type may be NULL. */
cf_webhook_err cf_webhook_classify(unsigned status, const char *content_type,
                                    const unsigned char *body, size_t body_len,
                                    cf_webhook_delivery *out);

/* Mime::Type.lookup: registered exact string (or synonym-stripped form) sets
 * *symbol (may be NULL for unregistered-but-valid) and *registered_spelling;
 * returns false for Mime::Type::InvalidMimeType. Outputs are borrowed
 * statics / point into content_type; the caller copies what it keeps. */
bool cf_webhook_mime_lookup(const char *content_type, const char **symbol,
                            const char **registered_spelling);

/* J02 applies delivery replies with fresh writer authorization checks.
 * After CF_WEBHOOK_OK the consumer must, in order:
 *  1. re-check the bot row (and its webhook) still exists — a bot removed
 *     while the request was in flight must NOT be resurrected by its reply;
 *  2. for TEXT: canonicalize via cf_richtext_canonical_body, then create the
 *     message (room.messages.create!) in a write transaction with the
 *     normal writer checks, then broadcast_create;
 *  3. for ATTACHMENT: stage the blob (ActiveStorage create_and_upload!),
 *     create_with_attachment!, process the attachment, broadcast_create;
 *  4. for NONE: nothing.
 * Returns true when the delivery carries a reply that needs applying. */
static inline bool cf_webhook_needs_apply(const cf_webhook_delivery *d) {
    return d && (d->kind == CF_WEBHOOK_REPLY_TEXT || d->kind == CF_WEBHOOK_REPLY_ATTACHMENT);
}

#endif /* CF_INTEGRATIONS_WEBHOOK_H */
