/* Request context, dispatch, cookies/flash/format state (task A00,
 * docs/devel/implementation/03-application.md "A00: context and dispatch";
 * 00-contracts.md ownership/execution; api.h cf_ctx/cf_dispatch).
 *
 * One cf_ctx (frozen struct in cf.h) is created per admitted request on its
 * request worker. The context borrows app/reader/request, owns its route
 * match, the parsed merged parameter tree, and the private cookie/flash/format
 * state behind cf_ctx.private_state. It never closes a connection or writes a
 * socket; the worker hands the finished response to H01's task submit.
 *
 * Destruction order (03): context destruction happens after cookies and
 * response ownership are finalized.
 *
 * No callback registry, service locator, global current user or thread-local
 * request context: every helper below takes the cf_ctx explicitly. Actions
 * call the auth helpers directly; dispatch itself does not authenticate.
 */
#ifndef CF_CONTEXT_H
#define CF_CONTEXT_H

#include "auth/platform.h"
#include "cf.h"

/* ---- dispatch ---------------------------------------------------------- */

/* Implemented by H03's ordered route table (src/routes.c): the action bound to
 * a matched routes.json id, or NULL when the id has no action (dispatch then
 * answers H03's reference 404, the same public/404.html response the 40
 * reference_error rows use; 00-contracts.md "Error translation").
 *
 * A00 consumes this seam; A00's tests provide a double in
 * tests/app/support/route_double.c. Declared here (not in cf.h, which the
 * integrator owns) pending the integrator's confirmation; H03 may move the
 * declaration into its own header as long as this exact symbol/signature
 * remains. */
cf_action_fn cf_route_action(uint32_t route_id);

/* Run one request through the full A00 path: create the context, parse and
 * merge body/query params (cf_params_parse), match the route
 * (cf_route_match_request), merge the route's path params over them
 * (cf_params_merge, path wins), call the action, finish cookies and apply the
 * generic error mapping. On CF_OK *response (caller-owned, initialized here)
 * holds the finished response, ready for cf_http_task_submit; on any non-OK
 * return the response is empty and the worker abandons the task (transport
 * resource failure, 00-contracts.md).
 *
 * Rack::MethodOverride runs first (kit adapter.rs `method_override`, via H02's
 * cf_effective_method): when a POST's form `_method` field or
 * X-HTTP-Method-Override header names an overridable verb, the context
 * carries a shallow copy of the request with `method` replaced — spans still
 * borrow the caller's buffers and `original_method` keeps the wire verb — so
 * every later step (parameters, route matching, cf_before_actions/CSRF, the
 * action, error rendering) sees the effective method.
 *
 * The caller's request is not written for dispatch, with one H01-serialization
 * exception: when the override's effective verb is HEAD, cf_ctx_process
 * records CF_HEAD on the caller's request object before returning (H01 decides
 * HEAD body suppression from the request it serializes, exactly as adapter.rs
 * replaces `parts.method` on the request the server serializes; the object is
 * the task-owned storage the app received from cf_http_task_request, and its
 * `original_method` keeps the wire verb). No other override is written back. */
cf_err cf_ctx_process(cf_app *app, cf_db *reader, const cf_request *request,
                      cf_response *response);

/* Create/destroy one context. cf_ctx_create applies the method override,
 * parses the request's cookie headers and parameters and matches the route;
 * on failure *ctx is still safe to pass to cf_ctx_destroy. cf_ctx_destroy
 * releases the route match, the parameter tree and the private
 * cookie/flash/format state (including the override copy) and zeroes *ctx. */
cf_err cf_ctx_create(cf_ctx *ctx, cf_app *app, cf_db *reader,
                     const cf_request *request, cf_response *response);
void cf_ctx_destroy(cf_ctx *ctx);

/* The merged body/query/path parameter tree (Rails Hash#merge! order: body,
 * then query, then the route's path captures) and one exact-key lookup.
 * Borrowed until cf_ctx_destroy; cf_ctx_param is cf_param_get on that tree. */
const cf_params *cf_ctx_params(const cf_ctx *ctx);
const cf_param *cf_ctx_param(const cf_ctx *ctx, cf_span name);

/* The User-Agent platform A01's allow_browser parsed and stored on the
 * context (cf_platform_parse).  Step 7 fills it on every request, mirroring
 * the reference's `platform` helper: the readable User-Agent header value
 * when there is one, else the empty string (the gem's default-UA parse).
 * NULL when nothing was stored (before that step has run, or on a context
 * built by hand).  Stored in the context's A00-owned private state, so
 * cf_ctx (the frozen contract in cf.h) is untouched.  Borrowed until
 * cf_ctx_destroy; the layout presenter copies the facts. */
const cf_platform *cf_ctx_platform(const cf_ctx *ctx);

/* Store a parsed platform on the context (the same value cf_ctx_platform
 * returns).  The copy owns no memory: spans that pointed into the source's
 * version_text/os_text buffers are rebased onto the stored copy's buffers.
 * `platform` NULL clears the stored value (cf_ctx_platform then returns
 * NULL).  CF_INVALID for a NULL/incomplete context. */
cf_err cf_ctx_set_platform(cf_ctx *ctx, const cf_platform *platform);

/* Explicit error status override (00-contracts.md: "A reference handler may
 * deliberately map a validation error differently; its explicit mapping wins
 * over the generic fallback"). When an action or helper records a status and
 * then returns a non-OK cf_err, cf_dispatch answers that status with an empty
 * body instead of the generic table mapping. A01 uses this for CSRF (422) and
 * banned-IP (429); A00's format helpers record 406 themselves. */
void cf_ctx_set_error_status(cf_ctx *ctx, unsigned status);

/* ---- cookies (A00 mechanism; values/policies are A01's) ---------------- */

/* Rails' `cookies_same_site_protection = :lax` default. */
typedef enum {
    CF_COOKIE_SAMESITE_DEFAULT = 0, /* Lax */
    CF_COOKIE_SAMESITE_LAX,
    CF_COOKIE_SAMESITE_STRICT,
    CF_COOKIE_SAMESITE_NONE
} cf_cookie_samesite;

/* Options accepted by cf_ctx_cookie_set/delete. A zeroed struct means the
 * Rails defaults: path "/" and SameSite=Lax, no domain, not permanent, not
 * secure/httponly/partitioned, no explicit expiry. A zero-length span means
 * absent (path "/", no domain). expires_us is absolute UTC microseconds;
 * permanent overrides expires_us (20 calendar years from the request clock,
 * like Rails `cookies.permanent`). */
typedef struct {
    cf_span path;
    cf_span domain;
    bool has_expires;
    int64_t expires_us;
    bool permanent;
    bool secure;
    bool httponly;
    bool partitioned;
    cf_cookie_samesite samesite;
} cf_cookie_options;

/* `cookies[name] = value`: records the value in the request's jar and the
 * pending Set-Cookie queue. Like ActionDispatch::Cookies, a header is emitted
 * only when the value changed or an expiry was given; a later set replaces an
 * earlier pending set for the same name in place and cancels a pending delete.
 * The value is the wire value (A01 signs/encrypts before calling); it is
 * copied. A plain set has no 4096-byte check (Rack has none); A01 owns the
 * signed/encrypted CookieOverflow rule (02-data-auth.md A01). */
cf_err cf_ctx_cookie_set(cf_ctx *ctx, cf_span name, cf_span value,
                         const cf_cookie_options *options);

/* `cookies.delete(name, **options)`: a no-op unless the cookie was present in
 * the request or set earlier; emits the reference delete header when it was. */
cf_err cf_ctx_cookie_delete(cf_ctx *ctx, cf_span name,
                            const cf_cookie_options *options);

/* `cookies[name]` from the request's Cookie header(s): first occurrence wins,
 * form-unescaped (`+` becomes space, `%XX` decoded; a malformed escape or
 * non-UTF-8 result keeps the raw wire value). A Cookie header carrying
 * obs-text is skipped whole, like the kit's
 * `get_all(COOKIE).filter_map(to_str)` (the only unreadable class H01 lets
 * through; DEL and the other controls are rejected at the protocol layer).
 * CF_NOT_FOUND when absent. */
cf_err cf_ctx_cookie_get(const cf_ctx *ctx, cf_span name, cf_span *out);

/* ---- flash ------------------------------------------------------------- */

/* The private flash map (string values; the reference only stores notice and
 * alert). A00 owns this state; A01's cf_auth_flash_persist serializes it and
 * views read it. The map is lazy: the first cf_ctx_flash_get/set/now/delete
 * loads the request's persisted flash through cf_auth_flash_load, and every
 * such access marks the flash as accessed so cf_finish_cookies serializes it
 * at the end of a successful response (the reference's commit_flash).
 *
 * cf_ctx_flash_set is FlashHash#set (shown on the next request);
 * cf_ctx_flash_get returns CF_NOT_FOUND when absent; cf_ctx_flash_now is
 * FlashHash#now (this request only — what the loader installs a persisted
 * value as, so a notice is rendered once); cf_ctx_flash_delete is
 * FlashHash#delete. An overwrite keeps the entry's insertion position. */
cf_err cf_ctx_flash_set(cf_ctx *ctx, cf_span key, cf_span value);
cf_err cf_ctx_flash_get(const cf_ctx *ctx, cf_span key, cf_span *out);
cf_err cf_ctx_flash_now(cf_ctx *ctx, cf_span key, cf_span value);
cf_err cf_ctx_flash_delete(cf_ctx *ctx, cf_span key);

/* Insertion-order access to the map (FlashHash#keys order: first insertion
 * position, an overwrite replaces in place, delete removes). cf_ctx_flash_at
 * walks every entry the map holds, loaded or set, consumed or not — what the
 * messages ETag joins; like cf_ctx_flash_get it loads the session's flash on
 * first use and marks it accessed. cf_ctx_flash_pending_at is the commit's
 * view and never loads: it walks only the entries that survive the request
 * (set, not consumed), what cf_auth_flash_persist serializes. Both return
 * false past the end; ctx, key or value may be NULL (a NULL key/value is
 * ignored), and the entries stay borrowed until cf_ctx_destroy. */
bool cf_ctx_flash_at(const cf_ctx *ctx, size_t index, cf_span *key,
                     cf_span *value);
bool cf_ctx_flash_pending_at(const cf_ctx *ctx, size_t index, cf_span *key,
                             cf_span *value);

/* reset_session drops the flash (the reference sets flash = None); A01's
 * terminate_current_session calls it. */
void cf_ctx_flash_reset(cf_ctx *ctx);

/* commit_flash: serializes the surviving flash map into session["flash"]
 * (`{"discard":[],"flashes":{...}}`, reference key order) through the
 * encrypted `_campfire_session` cookie, and removes the key when nothing
 * survives. Implemented by A01 (src/auth/session.c); declared here next to
 * its caller cf_finish_cookies, the way cf_route_action above names H03's
 * handler. */
cf_err cf_auth_flash_persist(cf_ctx *ctx);

/* ---- formats (format.rs / ctx.rs::respond_to groundwork) --------------- */

/* One registered MIME type (format.rs `Mime`). Descriptors are process
 * static; compare by symbol. */
typedef struct cf_format cf_format;
struct cf_format {
    const char *symbol; /* "html", "turbo_stream", ... */
    const char *string; /* "text/html", ... */
};

/* The registered descriptors A00 currently exposes to consumers. The full
 * format.rs table is internal; these cover the formats the pinned controllers
 * negotiate. */
extern const cf_format cf_format_html;
extern const cf_format cf_format_text;
extern const cf_format cf_format_js;
extern const cf_format cf_format_json;
extern const cf_format cf_format_turbo_stream;
extern const cf_format cf_format_xml;
extern const cf_format cf_format_all; /* the `*` `/` `*` wildcard */

/* `Mime::Type.lookup_by_extension` for the descriptors above; NULL when the
 * extension is not registered. */
const cf_format *cf_format_lookup_symbol(const char *symbol);

/* `request.formats` (ctx.rs::formats): requested `format` param, else the
 * Accept header, else the path extension, else HTML (JS for XHR). Borrowed
 * until cf_ctx_destroy, in reference order; CF_OK with *count == 0 is an empty
 * result, CF_INVALID is the reference InvalidMimeType (dispatch answers 406
 * when an action propagates it). */
cf_err cf_ctx_formats(cf_ctx *ctx, const cf_format ***out, size_t *count);

/* `request.format`: the first negotiated format, or NULL (Mime::NullType). */
const cf_format *cf_ctx_format(cf_ctx *ctx);

/* `respond_to`: first client format among `offered` in the reference
 * negotiation order. On CF_OK *out is the choice (never the wildcard);
 * CF_NOT_FOUND when nothing is acceptable, after recording 406 through
 * cf_ctx_set_error_status so the generic error mapping answers 406. */
cf_err cf_ctx_respond_to(cf_ctx *ctx, const cf_format *const *offered,
                         size_t count, const cf_format **out);

/* The format a render uses: the respond_to choice, else the first request
 * format, else HTML. Never NULL. */
const cf_format *cf_ctx_rendered_format(cf_ctx *ctx);

/* request.should_apply_vary_header?: the format came from the Accept header
 * (not a format param) and the header is one Rails uses for negotiation. */
bool cf_ctx_vary_accept(const cf_ctx *ctx);

#endif /* CF_CONTEXT_H */
