/* src/auth/internal.h — private helpers of the A01 auth module.
 * Nothing here may be used outside the auth module. */
#ifndef CF_AUTH_INTERNAL_H
#define CF_AUTH_INTERNAL_H

#include "cf.h"

#include "auth.h"

#include "context.h"

#include <yyjson.h>

/* ---- spans / allocations ------------------------------------------------- */

/* Owned copy of `span` as a NUL-terminated cf_str ({NULL,0} on failure for an
 * empty span is a valid owned empty value; callers may dispose it). */
cf_err auth_str_dup(cf_span span, cf_str *out);
bool auth_span_equal(cf_span a, cf_span b);
cf_span auth_cstr_span(const char *text);

/* The kit's `request.header(name)`: the first request header with this name
 * (case-insensitive) whose value passes http 1.5.0's `HeaderValue::to_str`
 * (HTAB or visible ASCII); false when absent or unreadable (obs-text, DEL and
 * other controls read as absent). */
bool auth_request_header(const cf_request *request, const char *name,
                         cf_span *out);

/* ---- random / crypto primitives (crypto.c) ------------------------------- */

cf_err auth_pbkdf2_sha256(cf_span password, cf_span salt, size_t length,
                          unsigned char *out);
cf_err auth_hmac(cf_auth_digest digest, cf_span key, cf_span data,
                 unsigned char *out, size_t *out_len);
cf_err auth_hmac_hex(cf_auth_digest digest, cf_span key, cf_span data,
                     cf_str *out);
bool auth_constant_time_equal(cf_span a, cf_span b);

/* Base64 flavors (Ruby semantics; see encoding.rs). */
cf_err auth_base64_encode(cf_span data, cf_auth_encoding encoding, cf_str *out);
/* Strict decode: standard alphabet, canonical padding.  urlsafe decode accepts
 * both alphabets, optional padding, and canonical trailing bits. */
cf_err auth_base64_strict_decode(cf_span encoded, cf_buf **out);
cf_err auth_base64_urlsafe_decode(cf_span encoded, cf_buf **out);
cf_err auth_hex_encode(cf_span data, cf_str *out);

/* aes-256-gcm with a 12-byte iv and 16-byte tag; iv from cf_random_bytes. */
cf_err auth_aes256gcm_encrypt(cf_span key32, cf_span iv12, cf_span plaintext,
                              cf_buf **out_ciphertext, unsigned char tag[16]);
bool auth_aes256gcm_decrypt(cf_span key32, cf_span iv12, cf_span ciphertext,
                            cf_span tag16, cf_buf **out_plaintext);

/* ---- JSON / timestamps (json.c) ------------------------------------------ */

/* ActiveSupport::JSON.encode of a string, including the quotes and the
 * `<`/`>`/`&` -> </>/& escapes. */
cf_err auth_json_string_encode(cf_builder *builder, cf_span raw);
/* `::JSON.generate` of a string (no HTML-entity escaping). */
cf_err auth_json_string_generate(cf_builder *builder, cf_span raw);
/* JSON.generate semantics for a parsed value (compact, insertion order) and
 * ActiveSupport::JSON.encode (the same, then re-escaping <>&). */
cf_err auth_json_write_value(yyjson_val *value, bool active_support_escape,
                             cf_str *out);
bool auth_utf8_valid(cf_span bytes);

/* Time#iso8601(3) in UTC and its parse; microsecond precision in, nanosecond
 * comparison possible. */
cf_err auth_iso8601_millis(int64_t us, char out[32]);
/* Parses the canonical `YYYY-MM-DDTHH:MM:SS(.f+)Z` form into nanoseconds;
 * CF_INVALID otherwise. */
cf_err auth_iso8601_parse_ns(cf_span text, int64_t *out_ns);

/* `20.years.from_now` with the Feb-29 clamp (jiff years_from; same rule as
 * src/context.c's cookie helper). */
int64_t auth_years_from_us(int64_t us, int years);

/* ---- metadata envelopes (message.c) -------------------------------------- */

typedef enum {
    AUTH_META_OK = 0,
    AUTH_META_INVALID,
    AUTH_META_EXPIRED,
    AUTH_META_PURPOSE
} auth_meta_status;

/* The value returned by a verifier/encryptor read.  For the NULL serializer
 * only `payload` is set (the decoded message bytes: the JSON text of the
 * value).  For JSON serializers `doc` is set and `payload` holds the JSON
 * text the document was parsed from (for extract_target/"data" handling). */
typedef struct {
    cf_auth_serializer serializer;
    cf_buf *payload; /* owned; maybe NULL for the empty JSON value */
    yyjson_doc *doc; /* owned; NULL for the NULL serializer */
    yyjson_val *root; /* value within doc; may be NULL (absent "data") */
    bool found;
} auth_value;

void auth_value_dispose(auth_value *value);

/* serialize_with_metadata. */
cf_err auth_metadata_serialize(cf_auth_serializer serializer, cf_span dumped,
                               cf_span purpose, bool has_purpose,
                               bool has_expiry, int64_t expires_us,
                               cf_builder *out);

/* deserialize_with_metadata.  `strict_message` selects strict Base64 for the
 * legacy message (the encryptor) instead of the verifier's lenient urlsafe
 * decode. */
auth_meta_status auth_metadata_deserialize(cf_auth_serializer serializer,
                                           cf_span bytes, cf_span purpose,
                                           bool has_purpose, int64_t now_us,
                                           bool strict_message,
                                           auth_value *out);

/* Verifier/encryptor read paths shared by the wrappers. */
cf_err auth_verifier_read(const cf_auth_verifier_config *config, cf_span key,
                          cf_span message, cf_span purpose, bool has_purpose,
                          int64_t now_us, auth_value *out, bool *found);

cf_err auth_signed_cookie_key(cf_span secret_key_base, unsigned char *out64);
cf_err auth_encrypted_cookie_key(cf_span secret_key_base, unsigned char *out32);

/* ---- session internals (session.c) --------------------------------------- */

#define AUTH_SESSION_COOKIE "_campfire_session"
#define AUTH_SESSION_TOKEN_COOKIE "session_token"

/* Cookie option helpers used by the cookie values A01 owns. */
cf_err auth_permanent_options(cf_ctx *ctx, cf_cookie_options *out);
cf_err auth_delete_options(cf_cookie_options *out);

/* CookieOverflow (kit cookies.rs check_for_overflow): name + wire value over
 * 4096 bytes is the reference's unrescued 500, mapped to CF_INTERNAL. */
cf_err auth_check_cookie_overflow(cf_span name, cf_span wire);

/* Writes the signed session_token cookie (permanent/HttpOnly/SameSite=Lax). */
cf_err auth_set_session_token_cookie(cf_ctx *ctx, cf_span token);

/* Test-only injection for src/auth/rate.c (like core/testclock.h): forget
 * every rate-limit window.  Application code never calls this. */
void cf_auth_rate_limit_test_reset(void);

/* restore_authentication without the bot/redirect fallbacks: resumes the
 * session named by the session_token cookie.  *restored is true when the
 * session existed; *signed_in is true when its user row was found (what
 * redirect_signed_in_user_to_root checks). */
cf_err auth_restore_session_only(cf_ctx *ctx, bool *restored, bool *signed_in);

#endif /* CF_AUTH_INTERNAL_H */
