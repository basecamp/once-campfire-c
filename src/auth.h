/* src/auth.h — task A01: current-format authentication (02-data-auth.md "A01";
 * api.h cf_before_actions/cf_authenticate/cf_check_csrf/cf_authorize_room).
 *
 * Owned by A01.  This header carries the cross-module surface of the
 * src/auth module: the rails_compat current-JSON cookie/signed-ID layer, the
 * password service, the before-action chain and the session lifecycle helpers
 * the controller packets call.  Only symbols here may be used outside the
 * module; internal
 * helpers live in src/auth/internal.h.
 *
 * No compatibility machinery: no rotation list, no Marshal reader, no legacy
 * serializers, no rescue fallback to another format (D-C01).  Only the
 * configured SECRET_KEY_BASE is used.
 *
 * Chain-halt convention (proposed to the integrator; see evidence A01.md):
 * helpers that answer a request themselves (redirect, 429, 403, 422) set
 * ctx->response and return CF_OK.  Callers of cf_before_actions/helpers must
 * check cf_auth_halted(ctx) after CF_OK and return without running the action
 * body; dispatch then finishes cookies.  The alternative (a non-OK cf_err)
 * would make cf_dispatch replace the response with a status-only body, losing
 * the redirect Location.
 */
#ifndef CF_AUTH_H
#define CF_AUTH_H

#include "cf.h"

#include "models/session.h"
#include "models/types.h"
#include "models/user.h"

/* ---- current-format primitives (rails_compat) ---------------------------- */

/* Digest/encoding/serializer selectors named after the pinned source enums. */
typedef enum {
    CF_AUTH_DIGEST_SHA1 = 0,
    CF_AUTH_DIGEST_SHA256
} cf_auth_digest;

typedef enum {
    CF_AUTH_ENCODING_STRICT = 0,   /* Base64.strict_encode64 */
    CF_AUTH_ENCODING_URLSAFE,      /* urlsafe_encode64, no padding */
    CF_AUTH_ENCODING_URLSAFE_PADDED
} cf_auth_encoding;

typedef enum {
    /* MessageEncryptor::NullSerializer: the dumped value is a string of bytes;
     * the legacy {"_rails":{"message":..}} envelope. */
    CF_AUTH_SERIALIZER_NULL = 0,
    /* ::JSON (JSON.generate/JSON.load). */
    CF_AUTH_SERIALIZER_JSON,
    /* SerializerWithFallback[:json] with ActiveSupport::JSON escaping; the C
     * port never reads Marshal (D-C01), so it behaves as [:json]. */
    CF_AUTH_SERIALIZER_JSON_FALLBACK
} cf_auth_serializer;

typedef struct {
    cf_auth_digest digest;
    cf_auth_encoding encoding;
    cf_auth_serializer serializer;
} cf_auth_verifier_config;

/* ActiveSupport::MessageVerifier#generate for data the caller already encoded
 * in this config's serializer:
 *  - NULL serializer: `data` is the raw string to sign (e.g. the JSON text of
 *    a cookie value).
 *  - JSON serializers: `data` is JSON text (inserted verbatim as `data`).
 * Expiry is an absolute UTC microsecond value; the envelope stores the
 * reference `Time#iso8601(3)` form.  Output is the signed message. */
cf_err cf_auth_verifier_generate(cf_span key, const cf_auth_verifier_config *config,
                                 cf_span data, cf_span purpose, bool has_purpose,
                                 bool has_expiry, int64_t expires_us, cf_str *out);

/* ActiveSupport::MessageVerifier#verify.  `purpose` is matched when
 * has_purpose, else the no-purpose rule applies.  On success *out_json holds
 * (owned):
 *  - NULL serializer: the decoded message content (the JSON text of the
 *    value, exactly as `verify` returns Value::String(...) content);
 *  - JSON serializers: the loaded value re-encoded with the serializer's
 *    encoder (JSON.generate for JSON, ActiveSupport::JSON.encode for the
 *    fallback serializer).
 * Bad signature, bad purpose, expired and malformed input all leave
 * *found=false with CF_OK. */
cf_err cf_auth_verifier_verify(cf_span key, const cf_auth_verifier_config *config,
                               cf_span message, cf_span purpose, bool has_purpose,
                               int64_t now_us, cf_str *out_json, bool *found);

/* ActiveSupport::MessageEncryptor (aes-256-gcm, 12-byte IV, 16-byte tag, empty
 * AAD) with the Null serializer, exactly as the encrypted cookie jar builds
 * it: `dumped` is the JSON text of the cookie value. */
cf_err cf_auth_encryptor_encrypt(cf_span key32, cf_span dumped, cf_span purpose,
                                 bool has_purpose, bool has_expiry,
                                 int64_t expires_us, cf_str *out);

/* decrypt_and_verify; *out_json is the decrypted JSON text (any JSON value).
 * Malformed/tampered/wrong-purpose/expired input leaves *found=false. */
cf_err cf_auth_encryptor_decrypt(cf_span key32, cf_span message, cf_span purpose,
                                 bool has_purpose, int64_t now_us,
                                 cf_str *out_json, bool *found);

/* ---- cookie jars (cookies.rs) ------------------------------------------- */

/* cookies.signed[name] = { value:, expires: }: purpose `cookie.<name>`, the
 * legacy envelope, HMAC-SHA1 over the strict-Base64 payload. */
cf_err cf_auth_signed_cookie_generate(cf_span secret_key_base, cf_span name,
                                      cf_span value, bool has_expiry,
                                      int64_t expires_us, cf_str *out);
/* cookies.signed[name] read: purpose is tried first, then the no-purpose rule
 * (the source's `verify(purpose).or_else(verify(None))`).  Only JSON string
 * values are returned; anything else reads as absent. */
cf_err cf_auth_signed_cookie_verify(cf_span secret_key_base, cf_span name,
                                    cf_span raw, int64_t now_us,
                                    cf_str *out_value, bool *found);

/* cookies.encrypted[name]: aes-256-gcm with purpose `cookie.<name>`;
 * `value_json` is the JSON text of the value (`ActiveSupport::JSON.encode`). */
cf_err cf_auth_cookie_encrypt(cf_span secret_key_base, cf_span name,
                              cf_span value_json, bool has_expiry,
                              int64_t expires_us, cf_str *out);
/* Decrypt; the result is the JSON text of the stored value.  Marshal payloads
 * are rejected (D-C01), as are all malformed inputs. */
cf_err cf_auth_cookie_decrypt(cf_span secret_key_base, cf_span name,
                              cf_span raw, int64_t now_us,
                              cf_str *out_value_json, bool *found);

/* ---- Active Record signed ids (signed_id.rs) ----------------------------- */

/* `record.signed_id(purpose:, expires_in:)`: `user/avatar`, SHA256, URL-safe
 * unpadded Base64, JSON serializer.  Purpose is compact-joined ("" = none). */
cf_err cf_auth_signed_id_generate(cf_span secret_key_base, cf_span model_name,
                                  int64_t id, cf_span purpose, bool has_purpose,
                                  bool has_expiry, int64_t expires_us, cf_str *out);
/* find_signed: the id, or found=false.  The reading legacy fallback is out of
 * scope (D-C01). */
cf_err cf_auth_signed_id_verify(cf_span secret_key_base, cf_span model_name,
                                cf_span signed_id, cf_span purpose, bool has_purpose,
                                int64_t now_us, cf_optional_i64 *out, bool *found);

/* ---- Rails.application.message_verifier(name) (active storage) ----------- */

/* name is the verifier name/key salt ("ActiveStorage"); SHA1, strict Base64,
 * the fallback serializer without Marshal.  `data_json`/`out_json` are the
 * already-encoded data (`generate_raw`/`verify_raw`). */
cf_err cf_auth_app_verifier_generate(cf_span secret_key_base, cf_span name,
                                     cf_span data_json, cf_span purpose,
                                     bool has_purpose, bool has_expiry,
                                     int64_t expires_us, cf_str *out);
cf_err cf_auth_app_verifier_verify(cf_span secret_key_base, cf_span name,
                                   cf_span message, cf_span purpose, bool has_purpose,
                                   int64_t now_us, cf_str *out_json, bool *found);

/* ---- GlobalID / SignedGlobalID (global_id.rs) ---------------------------- */

/* `gid://campfire/Model/id[?params]`; query params are dropped. */
cf_err cf_auth_global_id_parse(cf_span gid, cf_str *out_app, cf_str *out_model,
                               cf_str *out_id, bool *found);
/* GlobalID#to_param: URL-safe Base64 without padding of the gid text. */
cf_err cf_auth_global_id_param(cf_span gid_uri, cf_str *out);

/* record.attachable_sgid: purpose "attachable", data "<gid>?expires_in". */
cf_err cf_auth_sgid_generate_attachable(cf_span secret_key_base, cf_span gid_uri,
                                        cf_str *out);
/* SignedGlobalID.new(gid_uri, for: purpose, expires_at:). */
cf_err cf_auth_sgid_generate(cf_span secret_key_base, cf_span gid_uri,
                             cf_span purpose, bool has_expiry,
                             int64_t expires_us, cf_str *out);
/* SignedGlobalID.new(gid_uri, for: purpose, expires_at:); the C port parses
 * only the current verifier (no legacy self-validated metadata, D-C01).  On
 * success *out_gid is the gid URI from the message. */
cf_err cf_auth_sgid_locate(cf_span secret_key_base, cf_span sgid, cf_span purpose,
                           int64_t now_us, cf_str *out_gid, bool *found);

/* ---- Turbo stream names (turbo.rs) --------------------------------------- */

/* Turbo::StreamsChannel.signed_stream_name: parts joined with ':', SHA256,
 * strict Base64, JSON serializer, no envelope. */
cf_err cf_auth_turbo_signed_stream_name(cf_span secret_key_base,
                                        const cf_span *parts, size_t parts_len,
                                        cf_str *out);
/* verified_stream_name; a validly signed number reads as its to_s. */
cf_err cf_auth_turbo_verified_stream_name(cf_span secret_key_base, cf_span message,
                                          cf_str *out, bool *found);

/* ---- passwords (rails_compat::password; libxcrypt) ----------------------- */

#define CF_AUTH_BCRYPT_MIN_COST 4
#define CF_AUTH_BCRYPT_MAX_COST 31

/* BCrypt::Password.new(digest).is_password?(password): false for a malformed
 * digest; libxcrypt's bcrypt counts only the first 72 password bytes.  Also
 * the ratified A01 boundary used by cf_user_authenticate/authenticated
 * (declared locally in src/models/user.c).
 *
 * Bounded by the crypto queue once one has been started (P12-06): a job that
 * cannot be prepared, a refusal while stopping, and a discard at shutdown all
 * return false without running bcrypt on the calling thread (the bool
 * boundary has no error channel; false is a fail-closed non-match, and the
 * counts are in cf_crypto_queue_stats).  Only a process that never starts a
 * queue (unit tests, tools) verifies inline. */
bool cf_password_verify(cf_str password, cf_str digest);

/* BCrypt::Password.create(password, cost:) in the `$2a$` format bcrypt-ruby
 * writes; the salt comes from cf_random_bytes (OS entropy).  cost is 4..31.
 * Same bound: once a queue has been started, an unprepared job returns its
 * CF_NOMEM/CF_INTERNAL and a refusal/discard returns CF_BUSY, with *out
 * cleared and no inline hash; only the never-started-queue mode hashes on the
 * calling thread. */
cf_err cf_auth_password_digest(cf_str password, int cost, cf_str *out);

/* ---- bounded crypto queue (00-contracts execution/waiting; P12-04) ------- */

/* `CF_CRYPTO_WORKERS` worker threads run every bcrypt job; the pending queue
 * is the fixed 32 of 01-foundation-http.md.  A request worker submits an
 * owned-input job and waits for its completion, with its reader idle and no
 * continuation scheduling (00-contracts).  Sign-in verification, the constant
 * dummy verification for unknown accounts and setup hashing all pass through
 * here while the queue runs.
 *
 * The bound is unconditional once a queue has been started (P12-06): the
 * requirement is sticky for the process, and nothing (a stop, a refusal, a
 * discarded job, or a job that cannot be prepared) runs bcrypt on the calling
 * thread afterwards.  Only a process that never started the queue at all
 * (unit tests, tools) has an inline mode, counted in inline_fallbacks.  The
 * application wires start/stop through cf_app_start/cf_app_stop/
 * cf_app_destroy. */
typedef struct {
    size_t workers;            /* configured worker threads */
    size_t pending_capacity;   /* fixed pending bound (32) */
    uint64_t submitted;        /* jobs accepted for asynchronous execution */
    uint64_t completed;        /* accepted jobs executed by a worker */
    uint64_t refused;          /* jobs the queue declined because it was stopping or already stopped; the caller got the documented failure and no bcrypt ran */
    uint64_t saturated;        /* wait events: a submit found the queue full and slept (one submission can wait more than once, so this is not a per-submission count) */
    uint64_t discarded;        /* queued jobs discarded by a stop; each waiter got CF_BUSY (documented failure), no bcrypt ran */
    uint64_t inline_fallbacks; /* bcrypt-boundary executions on the calling thread because no queue was ever started in this process (the unit-test/tool mode); never incremented once a queue has been started */
    uint64_t unprepared;       /* submissions that could not be prepared for the queue (owned-input copy or job sync init failed); the caller got CF_NOMEM/CF_INTERNAL (digest) or false (verify), and no bcrypt ran */
    size_t pending_max;        /* high-water queued jobs */
    size_t running_max;        /* high-water concurrent executions */
    bool running;              /* started and not stopping */
} cf_crypto_queue_stats;

/* Starts `workers` bcrypt worker threads and resets the counters.  The queue
 * is one per process: concurrent app instances (test fixtures, embedders)
 * share it, so a start while it runs is reference-counted and CF_OK when
 * `workers` matches the first start (CF_BUSY otherwise, CF_INVALID for 0).
 * A successful start makes queue-or-fail sticky for the process (P12-06):
 * after it, and after the stop below, no caller runs bcrypt inline. */
cf_err cf_crypto_queue_start(size_t workers);
/* Release this instance's reference.  The last release stops accepting, wakes
 * every waiting submitter (a queued job is discarded and counted; its caller
 * receives CF_BUSY, the documented failure, and never hashes inline) and
 * joins the workers.  Idempotent per started instance; counters survive for
 * inspection. */
void cf_crypto_queue_stop(void);
/* Snapshot of the counters, including the last run's values after a stop. */
bool cf_crypto_queue_stats_get(cf_crypto_queue_stats *out);

/* Test-only execution observer (same injection style as
 * cf_auth_rate_limit_test_reset): invoked on the worker while the job holds
 * its concurrency slot, before bcrypt runs.  NULL by default. */
typedef void (*cf_crypto_queue_observer)(void *ctx);
void cf_crypto_queue_set_test_observer(cf_crypto_queue_observer fn, void *ctx);

/* Test-only allocation-failure injection: makes the next `count` job
 * owned-input copies fail (job preparation returns CF_NOMEM as if the copy
 * allocation had failed), so the failure arms above can be exercised
 * deterministically.  0 disarms; default 0. */
void cf_crypto_queue_set_test_copy_failure(unsigned count);

/* Test-only: clears the sticky queue-or-fail requirement, restoring the
 * never-started-queue inline mode so a case can exercise it after another
 * case started a queue in the same process.  The production requirement is
 * never cleared. */
void cf_crypto_queue_test_reset_required(void);

/* ---- sign-in rate limit (sessions.rs) ------------------------------------ */

#define CF_AUTH_SIGN_IN_LIMIT 10
#define CF_AUTH_SIGN_IN_WINDOW_US INT64_C(180000000)
#define CF_AUTH_SIGN_IN_MAP_CAP 4096
#define CF_AUTH_SIGN_IN_REJECTION "Too many requests or unauthorized."

/* `rate_limit to: 10, within: 3.minutes, by: request.remote_ip`: a fixed
 * per-IP window started by the first attempt.  *limited is true for attempt
 * 11 and later and whenever a new IP arrives at CF_AUTH_SIGN_IN_MAP_CAP
 * (expired entries are removed first; there is no eviction bypass).  When
 * limited, the 429 status is recorded through cf_ctx_set_error_status so a
 * caller that renders the rejection page can use it. */
cf_err cf_auth_sign_in_rate_limit(cf_ctx *ctx, bool *limited);

/* ---- before-action chain (concerns.rs) ----------------------------------- */

/* ApplicationController's chain in the pinned order: version headers
 * (X-Version/X-Rev from the build-time CF_APP_VERSION/CF_GIT_REVISION
 * defines), current request, banned IP on unsafe methods (429), the
 * controller's auth policy, bot restrictions (403), CSRF (422), browser check
 * (allow_browser; parses the UA into ctx->platform and renders the
 * incompatible-browser page), then the require-unauthenticated
 * restore/redirect.  See the halt convention above. */
cf_err cf_before_actions(cf_ctx *ctx, cf_before policy);

/* require_authentication: restore the session, else bot-key authentication,
 * else store the return-to URL and redirect to PUBLIC_ORIGIN `/session/new`. */
cf_err cf_authenticate(cf_ctx *ctx);

/* verify_authenticity_token by Sec-Fetch-Site rather than tokens; bot_exempt
 * is the chain's `authenticated_by == BotKey` guard. */
cf_err cf_check_csrf(cf_ctx *ctx, bool bot_exempt);

/* RoomScoped access: the user must have a membership in the room
 * (Room::find_for_user).  CF_NOT_FOUND when the room is not reachable. */
cf_err cf_authorize_room(cf_db *db, int64_t user_id, int64_t room_id);

/* True when a helper already produced the response (halted the chain). */
bool cf_auth_halted(const cf_ctx *ctx);

/* ---- session lifecycle (Authentication concern) -------------------------- */

/* Current user record for the authenticated identity, queried through
 * ctx->reader on every call (no auth cache). */
cf_err cf_auth_current_user(cf_ctx *ctx, bool *found, cf_user *out);

/* User.active.authenticate_by(email_address:, password:) and
 * User::authenticated: a blank password returns false before any lookup, an
 * unknown user still runs the dummy bcrypt verification, and the single
 * bcrypt check runs with no statement active.  On success *out_user is the
 * authenticated active user. */
cf_err cf_auth_authenticate_by(cf_ctx *ctx, cf_span email_address,
                               cf_span password, bool *authenticated,
                               cf_user *out_user);

/* start_new_session_for(user): writes the session row, sets the signed
 * `session_token` cookie (HttpOnly, SameSite=Lax, 20-year expiry) and installs
 * the SESSION identity. */
cf_err cf_auth_start_new_session_for(cf_ctx *ctx, const cf_user *user,
                                     cf_session *out_session);

/* post_authenticating_url: the stored return-to when it stays under
 * PUBLIC_ORIGIN, else `<PUBLIC_ORIGIN>/`.  Removes the session value. */
cf_err cf_auth_post_authenticating_url(cf_ctx *ctx, cf_str *out_url);

/* destroy: removes the push subscription named by the
 * push_subscription_endpoint param, destroys the session, clears the cookies,
 * and asks the writer for DISCONNECT_USER(reconnect=true). */
cf_err cf_auth_terminate_current_session(cf_ctx *ctx);

/* Encrypted `_campfire_session` cookie state (Rails cookie store, 20-year
 * expiry, HttpOnly).  Values are JSON texts; each call decodes the cookie at
 * most once and writes it back only when the data changed. */
cf_err cf_auth_session_read(cf_ctx *ctx, cf_span key, cf_str *out_json,
                            bool *found);
cf_err cf_auth_session_write(cf_ctx *ctx, cf_span key, cf_span value_json);
cf_err cf_auth_session_remove(cf_ctx *ctx, cf_span key);

/* Load the persisted flash (session["flash"]["flashes"]) into the context's
 * flash map and discard it from the session, so a notice is shown once.
 * Views render after this; cf_ctx_flash_set/now remain A00's map. */
cf_err cf_auth_flash_load(cf_ctx *ctx);

#endif /* CF_AUTH_H */
