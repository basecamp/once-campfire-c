/* src/integrations/push.h — Web Push delivery (I02).
 *
 * Port of the pinned `web_push.rs` and `web_push/{encryption,vapid,pool}.rs`
 * (tmp/rust-ref/crates/campfire/src/integrations/) using OpenSSL EVP
 * operations only. No new crypto format and no unauthenticated encryption:
 * HKDF inputs, P-256/ECDH, the AES-GCM record layout, VAPID JWT
 * claims/signature encoding and the request headers match the pinned
 * `web_push_expected.json` vectors (see tests/integrations/test_push.c).
 *
 * Ownership: I02 owns this header plus push.c and the push test. The HTTP
 * exchange itself belongs to I01: delivery takes an injected
 * cf_push_exchange_fn, so this module never touches sockets or libcurl. The
 * D02/J02 consumer wires the real helper (see the handoff note); the test
 * stubs the seam locally. Request timeouts (10 s connect/read inactivity,
 * 30 s total) are enforced by the exchange implementation.
 *
 * Subscription records, recipient selection (pushes_for), badges and the
 * title/body caps stay in the D01 push_subscription model, which this module
 * reads but never writes except through the invalidation flag: delivery
 * reports `invalidate=true` only for 404/410 and invalid P-256 keys, never
 * for TLS/config failures. Missing VAPID keys disable push with an explicit
 * error, never a silent drop. No function here logs endpoint or key
 * material; use cf_push_format_log for the sanitized line.
 */
#ifndef CF_INTEGRATIONS_PUSH_H
#define CF_INTEGRATIONS_PUSH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cf.h"
#include "config.h"
#include "models/push_subscription.h"

/* `WebPush::Request#default_options[:ttl]`: four weeks, in seconds. */
#define CF_PUSH_TTL_SECONDS 2419200u
/* Urgency header value for every delivery. */
#define CF_PUSH_URGENCY "high"
/* Content-Encoding header value for every delivery. */
#define CF_PUSH_CONTENT_ENCODING "aes128gcm"
/* `Rails.application.routes.url_helpers.account_logo_path`. */
#define CF_PUSH_ICON_PATH "/account/logo"
/* Test-notification title (`TestNotificationsController#create`). */
#define CF_PUSH_TEST_TITLE "Campfire Test"
/* Per-connect and per-read inactivity budget, enforced by the exchange. */
#define CF_PUSH_TIMEOUT_CONNECT_S 10
#define CF_PUSH_TIMEOUT_READ_S 10
/* Whole-delivery deadline, enforced by the exchange. */
#define CF_PUSH_TIMEOUT_TOTAL_S 30
/* `WebPush::Request#expiration`: VAPID JWT lifetime, in seconds. */
#define CF_PUSH_VAPID_EXPIRATION_S 43200
/* Largest encrypted record the gem accepts (and hence sends). */
#define CF_PUSH_MAX_RECORD_BYTES 4096

/* --- VAPID configuration ---------------------------------------------- */

/* `VapidError`: why push is disabled. Missing means "not configured". */
typedef enum {
    CF_PUSH_VAPID_OK = 0,
    CF_PUSH_VAPID_MISSING,
    CF_PUSH_VAPID_INVALID_PUBLIC_KEY,
    CF_PUSH_VAPID_INVALID_PRIVATE_KEY,
    CF_PUSH_VAPID_MISMATCHED
} cf_push_vapid_error;

/* Stable short name for sanitized diagnostics. Never returns NULL. */
const char *cf_push_vapid_error_name(cf_push_vapid_error error);

/* Parsed `VapidKey`: the subject plus the key pair. `pub`/`pub_len` hold the
 * decoded public key exactly as configured (sent as `k=`); `priv` holds the
 * left-padded 32-byte scalar that signs. */
typedef struct {
    char *subject; /* owned NUL text */
    unsigned char pub[65];
    size_t pub_len;
    unsigned char priv[32];
} cf_push_vapid;

void cf_push_vapid_dispose(cf_push_vapid *vapid);

/* `VapidKey.from_keys`: parse and cross-check one pair. `subject` may be
 * NULL (stored as ""). On failure `*out` stays empty and `*detail` (when
 * non-NULL) names the cause. Blank or malformed input maps to the Invalid*
 * variants exactly like the reference constructor. */
cf_err cf_push_vapid_init(const char *subject, const char *public_b64,
                           const char *private_b64, cf_push_vapid *out,
                           cf_push_vapid_error *detail);

/* `VapidConfig::from_config`: parse the configured pair, or report Missing
 * when any of the three fields is absent or empty. A bad pair is an
 * explicit error, never a silent drop. */
cf_err cf_push_vapid_from_config(const cf_config *config, cf_push_vapid *out,
                                  cf_push_vapid_error *detail);

/* `VapidConfig::authorization`: the `Authorization` header value for a push
 * service at `audience` (`scheme://host`) at `now_unix` (seconds). The
 * output is owned NUL text of the form `vapid t=<jwt>,k=<key>`. */
cf_err cf_push_vapid_authorization(const cf_push_vapid *vapid,
                                    const char *audience, int64_t now_unix,
                                    char **out);

/* --- message encryption ------------------------------------------------- */

/* Why `encrypt` refused the inputs: Argument mirrors `ArgumentError`
 * (blank argument, bad Base64, oversized payload); InvalidKey mirrors
 * `OpenSSL::PKey::EC::Point::Error`. Only InvalidKey invalidates the
 * subscription. */
typedef enum {
    CF_PUSH_CRYPT_OK = 0,
    CF_PUSH_CRYPT_ARGUMENT,
    CF_PUSH_CRYPT_INVALID_KEY
} cf_push_crypt_error;

const char *cf_push_crypt_error_name(cf_push_crypt_error error);

/* `WebPush::Encryption.encrypt` (RFC 8291 `aes128gcm` framing as the gem
 * lays it out): encrypt `message` for the subscription keys (urlsafe Base64,
 * either alphabet, padding optional) with a fresh server key and salt from
 * the OS entropy source. The output is the owned record
 * `salt || u32be(ciphertext_len) || u8(key_len) || server_key ||
 * ciphertext`. */
cf_err cf_push_encrypt(const unsigned char *message, size_t message_len,
                        const char *p256dh_b64, const char *auth_b64,
                        unsigned char **out, size_t *out_len,
                        cf_push_crypt_error *detail);

/* Deterministic `encrypt_with` for fixtures: same as above but with the
 * caller's server key (32-byte scalar) and salt (16 bytes). `record_size`
 * < 0 writes the ciphertext length (the gem's framing); >= 0 writes the
 * given value (the RFC 8291 example vector's 4096). `padding` is appended to
 * the plaintext before encryption (the gem uses {0x02, 0x00}). */
cf_err cf_push_encrypt_with(const unsigned char *message, size_t message_len,
                             const char *p256dh_b64, const char *auth_b64,
                             const unsigned char server_priv[32],
                             const unsigned char salt[16], int64_t record_size,
                             const unsigned char *padding, size_t padding_len,
                             unsigned char **out, size_t *out_len,
                             cf_push_crypt_error *detail);

/* `WebPush.decode64`/`trim_encode64`: urlsafe Base64 without padding on
 * encode; either alphabet with optional padding and strict trailing bits
 * on decode (like Ruby's `urlsafe_decode64`). */
cf_err cf_push_b64u_encode(const unsigned char *in, size_t len, char **out);
cf_err cf_push_b64u_decode(const char *in, unsigned char **out,
                            size_t *out_len);

/* --- notification payload ------------------------------------------------- */

/* `Notification#encoded_message`: plain JSON, no HTML escaping, exact key
 * order `title/options(body,icon,data(path,badge))`. The output is owned
 * NUL text. The title/body caps (256/3072 JSON bytes with ellipsis) are
 * applied by the model's `payload_for`; this function sends the payload it
 * is given. */
cf_err cf_push_notification_json(const char *title, const char *body,
                                  const char *path, int64_t badge, char **out);

/* Test-notification body: a random UUIDv4 string (`Uuid::new_v4`). */
cf_err cf_push_test_body(char out[37]);

/* Audience for VAPID (`scheme://host` with a lowercased scheme). */
cf_err cf_push_audience(const char *scheme, const char *host, char **out);

/* --- request -------------------------------------------------------------- */

typedef struct {
    char *name; /* owned NUL text */
    char *value; /* owned NUL text */
} cf_push_header;

typedef struct {
    cf_push_header *items;
    size_t len;
} cf_push_headers;

void cf_push_headers_dispose(cf_push_headers *headers);

/* The six `payload_send` headers in order (Content-Type, Ttl, Urgency,
 * Content-Encoding, Content-Length, Authorization). */
cf_err cf_push_build_headers(const cf_push_vapid *vapid, const char *audience,
                              int64_t now_unix, size_t payload_len,
                              cf_push_headers *out);

/* One `payload_send` request: endpoint URL (borrowed by the caller, kept
 * alive only for the call), resolved delivery IP, host, port, origin-form
 * target, headers and encrypted body. */
typedef struct {
    const char *endpoint_url; /* borrowed */
    char *resolved_ip; /* owned NUL text */
    char *host; /* owned NUL text */
    uint16_t port;
    char *target; /* owned NUL text, origin-form */
    cf_push_headers headers; /* owned */
    unsigned char *body; /* owned */
    size_t body_len;
} cf_push_request;

void cf_push_request_dispose(cf_push_request *request);

/* Encrypt the notification JSON and frame the request for `endpoint`.
 * Fails with CF_INVALID for a blank/unparseable endpoint or bad keys.
 * `crypt_detail` (nullable) reports the encryption refusal class;
 * `error_text` (nullable) receives owned detail on failure (NULL on
 * success). The text may quote the endpoint: sanitize before logging. */
cf_err cf_push_build_request(const char *endpoint,
                              const char *p256dh_b64, const char *auth_b64,
                              const char *resolved_ip, const char *title,
                              const char *body, const char *path, int64_t badge,
                              const cf_push_vapid *vapid, int64_t now_unix,
                              cf_push_request *out,
                              cf_push_crypt_error *crypt_detail,
                              char **error_text);

/* Recipient order: `pool.queue` delivers in id order (`find_each`). Sorts
 * the vector in place by subscription id. */
void cf_push_sort_by_id(cf_push_subscription_vector *subscriptions);

/* --- delivery --------------------------------------------------------------- */

/* What the exchange (I01's helper) reported. Timeouts and TLS map to the
 * gem's `Net::OpenTimeout`/`Net::ReadTimeout`/`OpenSSL::SSL::SSLError`
 * classes; anything else transport-level is `SystemCallError`. None of
 * them invalidates the subscription. */
typedef enum {
    CF_PUSH_TRANSPORT_OK = 0,
    CF_PUSH_TRANSPORT_TLS,
    CF_PUSH_TRANSPORT_OPEN_TIMEOUT,
    CF_PUSH_TRANSPORT_READ_TIMEOUT,
    CF_PUSH_TRANSPORT_IO
} cf_push_transport_error;

/* I01-owned exchange seam (10 s connect/read inactivity, 30 s total,
 * pinned-IP TLS POST). Writes the push service status and its reason
 * phrase ("" when absent, truncated to the buffer). Returns CF_OK unless
 * the call itself was misused; transport trouble arrives via
 * `*transport_err`. Never called when delivery is skipped. */
typedef cf_err (*cf_push_exchange_fn)(
    void *ctx, const cf_push_request *request, unsigned *out_status,
    char *reason_buf, size_t reason_cap,
    cf_push_transport_error *transport_err);

/* Determined delivery outcome. `GONE` (404/410) and `INVALID_KEY` destroy
 * the subscription; every other error preserves it. `OK` covers both a
 * 2xx delivery (`delivered`) and a skipped endpoint (`skipped`, the
 * reference `Ok(None)`). `class_name` is the Ruby exception class for the
 * pool's log line; `host`/`status` identify the service. `detail` carries
 * the sanitized message and never contains endpoint or key material. */
typedef enum {
    CF_PUSH_DELIVERY_OK = 0,
    CF_PUSH_DELIVERY_GONE,
    CF_PUSH_DELIVERY_RESPONSE,
    CF_PUSH_DELIVERY_INVALID_KEY,
    CF_PUSH_DELIVERY_ARGUMENT,
    CF_PUSH_DELIVERY_TLS,
    CF_PUSH_DELIVERY_HTTP
} cf_push_delivery_kind;

typedef struct {
    cf_push_delivery_kind kind;
    char *class_name; /* owned NUL text */
    char *host; /* owned NUL text, may be "" */
    unsigned status; /* 0 when no HTTP status exists */
    char *detail; /* owned NUL text, sanitized */
    bool delivered;
    bool skipped;
    bool invalidate;
} cf_push_delivery;

void cf_push_delivery_dispose(cf_push_delivery *delivery);

/* `invalidates_subscription`: true only for GONE and INVALID_KEY. */
bool cf_push_delivery_invalidates(const cf_push_delivery *delivery);

/* Sanitized one-line log: `<class>: host: <host>, status: <n>` (or the
 * class plus the sanitized detail when no status exists). Never contains
 * endpoint or key material. Always NUL-terminates. */
void cf_push_format_log(const cf_push_delivery *delivery, char *buf,
                         size_t cap);

/* `verify_response` plus the transport mapping: classify one push service
 * status/reason for `host`. Reason may be NULL (= ""). */
cf_err cf_push_classify_response(unsigned status, const char *reason,
                                  const char *host, cf_push_delivery *out);

/* `Notification#deliver`: resolve the endpoint through the model's
 * `resolved_endpoint_ip` (permitted `https://…:443` push service, public
 * address), then `payload_send`. A non-deliverable endpoint yields kind OK
 * with `skipped=true` and never calls the exchange. A NULL `vapid` is an
 * explicit ARGUMENT error (push disabled), not a silent drop. Returns
 * CF_OK whenever an outcome was determined; only resource/misuse failures
 * (NULL outputs, allocation failure, exchange misuse) return otherwise. */
cf_err cf_push_deliver(const cf_push_subscription *subscription,
                        const char *title, const char *body, const char *path,
                        int64_t badge, const cf_push_vapid *vapid,
                        cf_push_resolve_fn resolve, void *resolve_arg,
                        cf_push_exchange_fn exchange, void *exchange_ctx,
                        int64_t now_unix, cf_push_delivery *out);

#endif /* CF_INTEGRATIONS_PUSH_H */
