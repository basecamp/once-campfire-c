/* tests/actions/sessions_test.c — A-sessions acceptance: `sessions#new`
 * (route ID 12), `sessions#destroy` (route ID 17) and `sessions#create`
 * (route ID 18), per docs/devel/implementation/contracts/controller-packets.md
 * "A-sessions" and 03-application.md's "sessions#new/create/destroy" slice
 * row:
 *
 *   - public success: the sign-in page renders (also with the submitted
 *     email_address), compared byte-for-byte against A02's
 *     golden/a/sessions_new.html and sessions_new_email.html;
 *   - setup redirect: no users at all redirects to /first_run;
 *   - unauthorized/failure: wrong or missing credentials render the 401
 *     sign-in page with flash.now[:alert] (golden
 *     sessions_new_rejected.html); an unacceptable format is 406;
 *   - parameter failure: a non-string email_address is the reference's
 *     `param_str -> None` failure arm (401 page, no echoed value);
 *   - the fixed-window rate limit: attempt 11 is the 429 rejection, a new
 *     window starts at exactly three minutes;
 *   - create success: a session row plus the signed session_token cookie,
 *     redirecting to the return-to URL (else root);
 *   - destroy: authenticated; removes the session row, clears both cookies,
 *     removes the push subscription of the current user only, and emits
 *     DISCONNECT_USER(reconnect=true) through the writer (C03's registered
 *     control handler makes the mandatory write succeed); an unauthenticated
 *     DELETE is redirected to sign-in by the chain.
 *
 * The route double (tests/app/support/route_double.c) binds rows 12/17/18
 * (and a local protected probe on row 1 for the return-to leg) to the real
 * actions, so every case runs the real A00 dispatch path (params, cookies,
 * before-actions, format negotiation, generic error mapping, cookie finish).
 * The writer is started and a capture control handler is registered, which
 * is how the mandated DISCONNECT_USER event is asserted (the same shape as
 * A01's auth_env).
 *
 * cf.h is frozen and no packet-action header exists yet, so the three
 * symbols are declared here; the integrator's routes.c rebind
 * (c_symbols) needs the same declarations.
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "auth/internal.h" /* cf_auth_rate_limit_test_reset */
#include "cf.h"
#include "config.h"
#include "context.h"
#include "core/testclock.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "http/http_internal.h"
#include "models/user.h"
#include "views.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"
#include "../views/support/golden.h"

#include <limits.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

cf_err cf_action_sessions_new(cf_ctx *ctx);
cf_err cf_action_sessions_destroy(cf_ctx *ctx);
cf_err cf_action_sessions_create(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
/* golden/a facts.json root vapid_public_key */
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="
#define PASSWORD "secret123456"
/* golden/a facts.json sessions cases: the account's updated_at is what
 * produces the fixture's /account/logo?v=20260926130020; David is the first
 * administrator (accounts/_help_contact). */
#define GOLDEN_ACCOUNT_ID INT64_C(873240054)
#define GOLDEN_ADMIN_ID INT64_C(127326141)
#define GOLDEN_ADMIN_EMAIL "david@37signals.com"
#define GOLDEN_ACCOUNT_UPDATED_AT "2026-09-26 13:00:20.000000"
/* The first fixed-window instant for the rate-limit cases. */
#define RATE_T0 INT64_C(1767272400000000)

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* Borrowed cf_str over a literal (the model/password calls take cf_str). */
static cf_str CSTR(const char *text) {
    return (cf_str){(char *)(uintptr_t)text, strlen(text)};
}

/* --- scratch app + started writer + capture control handler ---------------- */

#define SESSIONS_MAX_EVENTS 64

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
    cf_event events[SESSIONS_MAX_EVENTS];
    size_t event_count;
} sess_env;

static cf_err sess_capture(void *ctx, const cf_event *event) {
    sess_env *env = ctx;
    if (env->event_count < SESSIONS_MAX_EVENTS) {
        env->events[env->event_count++] = *event;
    }
    return CF_OK;
}

/* The return-to leg's protected target: the production chain with
 * authentication required, so the unauthenticated request stores
 * return_to_after_authenticating and is redirected to sign-in. */
static cf_err sessions_protected_probe(cf_ctx *ctx) {
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;
    ctx->response->status = 200;
    return CF_OK;
}

static bool env_open(sess_env *env) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;
    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", ORIGIN},
        {"SECRET_KEY_BASE", HEX64},
        {"VAPID_PUBLIC_KEY", VAPID_PUBLIC_KEY},
        {"DATABASE_PATH", env->scratch.path},
    };
    if (cf_config_parse(entries, 4, NULL, &env->config) != CF_OK) return false;
    cf_config_test_set_bcrypt_cost(env->config, 4);
    if (cf_app_create(env->config, &env->app) != CF_OK) {
        cf_config_destroy(env->config);
        env->config = NULL;
        return false;
    }
    if (cf_writer_start(env->app, env->config) != CF_OK) return false;
    env->writer_started = true;
    if (cf_writer_set_control_handler(env->app, sess_capture, env) != CF_OK) {
        return false;
    }
    cf_test_routes_reset();
    /* routes.json ids 12/17/18.  The double's `(.:format)` grammar needs an
     * extension for a literal last segment, so the bare form is registered as
     * well (reported in the evidence). */
    if (cf_test_routes_add("GET", "/session/new(.:format)", 12,
                           cf_action_sessions_new) != CF_OK ||
        cf_test_routes_add("GET", "/session/new", 12,
                           cf_action_sessions_new) != CF_OK ||
        cf_test_routes_add("POST", "/session(.:format)", 18,
                           cf_action_sessions_create) != CF_OK ||
        cf_test_routes_add("POST", "/session", 18,
                           cf_action_sessions_create) != CF_OK ||
        cf_test_routes_add("DELETE", "/session(.:format)", 17,
                           cf_action_sessions_destroy) != CF_OK ||
        cf_test_routes_add("DELETE", "/session", 17,
                           cf_action_sessions_destroy) != CF_OK ||
        cf_test_routes_add("GET", "/rooms/1", 1,
                           sessions_protected_probe) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(sess_env *env) {
    if (env->writer_started) cf_writer_stop(env->app);
    if (env->app != NULL) cf_app_destroy(env->app); /* frees the config too */
    env->config = NULL;
    cf_db_scratch_close(&env->scratch);
    memset(env, 0, sizeof *env);
}

static void exec_sql(cf_db *db, const char *sql) {
    char *message = NULL;
    int rc = sqlite3_exec(cf_db_handle(db), sql, NULL, NULL, &message);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  sql failed: %s\n  %s\n",
                message != NULL ? message : "?", sql);
    }
    CF_REQUIRE(rc == SQLITE_OK);
}

static int64_t count_rows(cf_db *db, const char *sql) {
    bool ok = false;
    int64_t value = cf_db_test_i64(cf_db_handle(db), sql, &ok);
    CF_REQUIRE(ok);
    return value;
}

/* --- seeding --------------------------------------------------------------- */

static void seed_user(cf_db *db, int64_t id, const char *name,
                      const char *email, const char *digest, int role,
                      int status) {
    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, updated_at) "
             "VALUES (%lld, NULL, NULL, '2026-01-02 03:04:05', %s, '%s', %s, "
             "%d, %d, '2026-01-02 03:04:05')",
             (long long)id,
             email != NULL ? "?" : "NULL", name,
             digest != NULL ? "?" : "NULL", role, status);
    if (email == NULL && digest == NULL) {
        exec_sql(db, sql);
        return;
    }
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) == SQLITE_OK);
    int bind = 1;
    if (email != NULL) {
        sqlite3_bind_text(stmt, bind++, email, -1, SQLITE_TRANSIENT);
    }
    if (digest != NULL) {
        sqlite3_bind_text(stmt, bind++, digest, -1, SQLITE_TRANSIENT);
    }
    int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (step != SQLITE_DONE) {
        fprintf(stderr, "  seed_user failed (%d): %s\n", step, sql);
    }
    CF_REQUIRE(step == SQLITE_DONE);
}

static void seed_golden_account(cf_db *db) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
             "name, settings, singleton_guard, updated_at) VALUES "
             "(%lld, '2026-09-26 13:00:20.000000', NULL, "
             "'CRMu-l8Ge-KB9B', '37signals', NULL, 0, '%s')",
             (long long)GOLDEN_ACCOUNT_ID, GOLDEN_ACCOUNT_UPDATED_AT);
    exec_sql(db, sql);
}

/* A session row whose last_active_at is in the future, so restoring it never
 * needs the writer (cf_session_needs_resume is false). */
static void seed_session(cf_db *db, const char *token, int64_t user_id) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO sessions (created_at, ip_address, last_active_at, "
             "token, updated_at, user_agent, user_id) VALUES "
             "('2040-01-01 00:00:00.000000', NULL, "
             "'2040-01-01 00:00:00.000000', '%s', "
             "'2040-01-01 00:00:00.000000', NULL, %lld)",
             token, (long long)user_id);
    exec_sql(db, sql);
}

static void seed_push_subscription(cf_db *db, int64_t id, int64_t user_id,
                                   const char *endpoint) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO push_subscriptions (id, auth_key, created_at, "
             "endpoint, p256dh_key, updated_at, user_agent, user_id) VALUES "
             "(%lld, NULL, '2026-01-02 03:04:05', '%s', NULL, "
             "'2026-01-02 03:04:05', NULL, %lld)",
             (long long)id, endpoint, (long long)user_id);
    exec_sql(db, sql);
}

/* "session_token=<wire>" with the Rack form-escaping a base64 value needs. */
static void make_session_cookie(const cf_config *config, cf_db *db, char *out,
                                size_t cap, const char *token,
                                int64_t user_id) {
    seed_session(db, token, user_id);
    cf_str wire = {0};
    CF_REQUIRE(cf_auth_signed_cookie_generate(
                   (cf_span){(const unsigned char *)config->secret_key_base,
                             config->secret_key_base_len},
                   SP("session_token"),
                   (cf_span){(const unsigned char *)token, strlen(token)},
                   false, 0, &wire) == CF_OK);
    size_t at = (size_t)snprintf(out, cap, "session_token=");
    for (size_t i = 0; i < wire.len && at + 4 < cap; i++) {
        unsigned char c = (unsigned char)wire.ptr[i];
        if (c == '+' || c == '%') {
            at += (size_t)snprintf(out + at, cap - at, "%%%02X", c);
        } else {
            out[at++] = (char)c;
        }
    }
    out[at] = '\0';
    cf_str_dispose(&wire);
}

/* Percent-encode a query value (only the endpoint host characters matter). */
static void query_encode(char *out, size_t cap, const char *text) {
    size_t at = 0;
    for (size_t i = 0; text[i] != '\0' && at + 4 < cap; i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out[at++] = (char)c;
        } else {
            at += (size_t)snprintf(out + at, cap - at, "%%%02X", c);
        }
    }
    out[at] = '\0';
}

/* --- request/response helpers ---------------------------------------------- */

static bool run_request(sess_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void get_session_new(cf_request *req, cf_span query,
                            const char *cookie_header) {
    cf_test_req_init(req);
    req->path = SP("/session/new");
    req->query = query;
    if (query.len != 0) {
        static char target[1024];
        snprintf(target, sizeof target, "/session/new?%.*s", (int)query.len,
                 (const char *)query.ptr);
        req->target = SP(target);
    } else {
        req->target = SP("/session/new");
    }
    if (cookie_header != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"),
                                      SP(cookie_header)) == CF_OK);
    }
}

/* POST /session with a form body.  sec_fetch_site is the chain's CSRF input
 * (the reference verifies by Sec-Fetch-Site, not tokens). */
static void post_session(cf_request *req, const char *body,
                         const char *sec_fetch_site,
                         const char *cookie_header) {
    cf_test_req_init(req);
    req->method = CF_POST;
    req->original_method = CF_POST;
    req->path = SP("/session");
    req->target = SP("/session");
    req->body = SP(body);
    CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                  SP("application/x-www-form-urlencoded")) ==
               CF_OK);
    CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                  SP(sec_fetch_site)) == CF_OK);
    if (cookie_header != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"),
                                      SP(cookie_header)) == CF_OK);
    }
}

/* DELETE /session (?push_subscription_endpoint=...). */
static void delete_session(cf_request *req, const char *endpoint,
                           const char *cookie_header) {
    static char target[2048];
    cf_test_req_init(req);
    req->method = CF_DELETE;
    req->original_method = CF_DELETE;
    req->path = SP("/session");
    req->target = SP("/session");
    if (endpoint != NULL) {
        char encoded[1024];
        query_encode(encoded, sizeof encoded, endpoint);
        snprintf(target, sizeof target,
                 "/session?push_subscription_endpoint=%s", encoded);
        req->target = SP(target);
        req->query = SP(target + strlen("/session?"));
    }
    CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                  SP("same-origin")) == CF_OK);
    if (cookie_header != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"),
                                      SP(cookie_header)) == CF_OK);
    }
}

/* GET /rooms/1 through the protected probe (return-to leg). */
static void get_protected_probe(cf_request *req) {
    cf_test_req_init(req);
    req->path = SP("/rooms/1");
    req->target = SP("/rooms/1");
}

static bool span_contains(cf_span span, const char *needle) {
    size_t len = strlen(needle);
    if (len == 0) return true;
    if (span.len < len) return false;
    for (size_t i = 0; i + len <= span.len; i++) {
        if (memcmp(span.ptr + i, needle, len) == 0) return true;
    }
    return false;
}

static bool buf_contains(const cf_buf *buf, const char *needle) {
    return buf != NULL && span_contains(cf_buf_span(buf), needle);
}

static bool head_contains(cf_response *resp, cf_request *req,
                          const char *needle) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    bool found = buf_contains(ser.headers, needle);
    cf_buf_release(ser.headers);
    return found;
}

/* Copy the raw value after "Set-Cookie: <name>=" (up to ';' or CR).  The
 * value is already form-escaped by A00, exactly as a client would send it
 * back. */
static bool extract_set_cookie(cf_response *resp, cf_request *req,
                               const char *name, char *out, size_t cap) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    cf_span head = cf_buf_span(ser.headers);
    char needle[128];
    snprintf(needle, sizeof needle, "Set-Cookie: %s=", name);
    size_t len = strlen(needle);
    bool found = false;
    for (size_t i = 0; i + len <= head.len; i++) {
        if (memcmp(head.ptr + i, needle, len) != 0) continue;
        size_t at = i + len;
        size_t end = at;
        while (end < head.len && head.ptr[end] != ';' &&
               head.ptr[end] != '\r' && head.ptr[end] != '\n') {
            end++;
        }
        size_t copy = end - at;
        if (copy >= cap) copy = cap - 1;
        memcpy(out, head.ptr + at, copy);
        out[copy] = '\0';
        found = true;
        break;
    }
    cf_buf_release(ser.headers);
    return found;
}

/* --- assets for the exact golden renders ----------------------------------- */

static int g_assets_ready = 0;

static bool write_whole_file(const char *path, const char *bytes, size_t len) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    size_t wrote = len == 0 ? 0 : fwrite(bytes, 1, len, file);
    return wrote == len && fclose(file) == 0;
}

/* The asset build and the goldens were rendered at different reference
 * revisions: the loadable importmap-tags.html differs from the golden's
 * importmap block in three digests (A02-foundation.md reports this).  The
 * action reads the configured asset module, so configure it with a scratch
 * root whose importmap-tags.html is the golden's own block and whose public/
 * tree is the pinned manifest (the same technique as
 * tests/actions/welcome_test.c). */
static bool sessions_assets_setup(void) {
    if (g_assets_ready) return true;
    char dir[] = "/tmp/cf_sessions_assets_XXXXXX";
    if (mkdtemp(dir) == NULL) return false;

    char importmap_path[PATH_MAX];
    char link_path[PATH_MAX];
    int n = snprintf(importmap_path, sizeof importmap_path,
                     "%s/importmap-tags.html", dir);
    int m = snprintf(link_path, sizeof link_path, "%s/public", dir);
    if (n < 0 || (size_t)n >= sizeof importmap_path || m < 0 ||
        (size_t)m >= sizeof link_path) {
        rmdir(dir);
        return false;
    }
    bool ok = false;
    char *golden = NULL;
    size_t golden_len = 0;

    golden = cf_golden_read("a", "sessions_new", "html", &golden_len);
    if (golden == NULL) goto out;
    {
        static const char head[] = "<script type=\"importmap\"";
        static const char tail[] = "import \"application\"</script>";
        char *start = memmem(golden, golden_len, head, sizeof head - 1);
        char *end = start != NULL
                        ? memmem(start, golden_len - (size_t)(start - golden),
                                 tail, sizeof tail - 1)
                        : NULL;
        if (start == NULL || end == NULL) goto out;
        size_t block_len = (size_t)(end - start) + (sizeof tail - 1);
        if (!write_whole_file(importmap_path, start, block_len)) goto out;
    }
    {
        char resolved[PATH_MAX];
        if (realpath("tests/fixtures/assets/public", resolved) == NULL) {
            goto out;
        }
        if (symlink(resolved, link_path) != 0) goto out;
    }
    ok = cf_views_assets_configure(dir) == CF_OK;

out:
    free(golden);
    remove(importmap_path);
    remove(link_path);
    rmdir(dir);
    if (ok) g_assets_ready = 1;
    return ok;
}

/* The route-level sign-in render cases share the golden fixture state. */
static void seed_golden_login_state(sess_env *env) {
    seed_golden_account(env->scratch.db);
    seed_user(env->scratch.db, GOLDEN_ADMIN_ID, "David",
              GOLDEN_ADMIN_EMAIL, NULL, 1, 0);
}

/* --- acceptance ------------------------------------------------------------ */

CF_TEST(sessions_route_ids_bind_the_actions) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/session/new", 12,
                                  cf_action_sessions_new) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("POST", "/session", 18,
                                  cf_action_sessions_create) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("DELETE", "/session", 17,
                                  cf_action_sessions_destroy) == CF_OK);
    CF_CHECK(cf_route_action(12) == cf_action_sessions_new);
    CF_CHECK(cf_route_action(18) == cf_action_sessions_create);
    CF_CHECK(cf_route_action(17) == cf_action_sessions_destroy);
}

/* No users: `redirect_to first_run_url if User.none?` (and no page). */
CF_TEST(sessions_new_without_users_redirects_to_first_run) {
    sess_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    cf_response resp;
    get_session_new(&req, (cf_span){NULL, 0}, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/first_run\r\n"));
    CF_CHECK(resp.body == NULL);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Public success: the exact sign-in page of golden/a/sessions_new.html. */
CF_TEST(sessions_new_matches_the_golden) {
    sess_env env;
    CF_REQUIRE(env_open(&env));
    CF_REQUIRE(sessions_assets_setup());
    seed_golden_login_state(&env);

    cf_request req;
    cf_response resp;
    get_session_new(&req, (cf_span){NULL, 0}, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("sessions_new", (const char *)body.ptr, body.len);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* The submitted root-level email_address is echoed into the form. */
CF_TEST(sessions_new_echoes_the_email_matches_the_golden) {
    sess_env env;
    CF_REQUIRE(env_open(&env));
    CF_REQUIRE(sessions_assets_setup());
    seed_golden_login_state(&env);

    cf_request req;
    cf_response resp;
    get_session_new(&req, SP("email_address=x@y.com"), NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("sessions_new_email", (const char *)body.ptr, body.len);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Parameter failure: email_address of the wrong type is `param_str -> None`
 * (kit params.rs Param::as_str), so the page renders without a value. */
CF_TEST(sessions_new_wrong_typed_email_renders_without_value) {
    sess_env env;
    CF_REQUIRE(env_open(&env));
    CF_REQUIRE(sessions_assets_setup());
    seed_golden_login_state(&env);

    cf_request req;
    cf_response resp;
    get_session_new(&req, SP("email_address[]=x@y.com"), NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("sessions_new", (const char *)body.ptr, body.len);
    CF_CHECK(!span_contains(body, "x@y.com"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A client that cannot take HTML is ActionController::UnknownFormat (406). */
CF_TEST(sessions_new_unacceptable_format_is_406) {
    sess_env env;
    CF_REQUIRE(env_open(&env));
    seed_golden_login_state(&env);

    cf_request req;
    cf_response resp;
    get_session_new(&req, (cf_span){NULL, 0}, NULL);
    CF_REQUIRE(cf_test_req_header(&req, SP("Accept"),
                                  SP("application/json")) == CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Turbo-Frame request: turbo-rails' frame layout (head and content only). */
CF_TEST(sessions_new_turbo_frame_renders_the_frame_layout) {
    sess_env env;
    CF_REQUIRE(env_open(&env));
    CF_REQUIRE(sessions_assets_setup());
    seed_golden_login_state(&env);

    cf_request req;
    cf_response resp;
    get_session_new(&req, (cf_span){NULL, 0}, NULL);
    CF_REQUIRE(cf_test_req_header(&req, SP("Turbo-Frame"),
                                  SP("session_form")) == CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    static const char frame_prefix[] = "<html>\n  <head>\n    ";
    cf_span body = cf_buf_span(resp.body);
    CF_CHECK(body.len >= sizeof frame_prefix - 1 &&
             memcmp(body.ptr, frame_prefix, sizeof frame_prefix - 1) == 0);
    CF_CHECK(span_contains(body, "turbo-visit-control"));
    CF_CHECK(!span_contains(body, "<!DOCTYPE html>"));
    CF_CHECK(!span_contains(body, "<body class="));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A non-ASCII `Turbo-Frame` value fails http 1.5.0's `HeaderValue::to_str`,
 * so the pin reads the header as absent: the page layout renders. */
CF_TEST(sessions_new_turbo_frame_non_ascii_is_page) {
    sess_env env;
    CF_REQUIRE(env_open(&env));
    CF_REQUIRE(sessions_assets_setup());
    seed_golden_login_state(&env);

    cf_request req;
    cf_response resp;
    get_session_new(&req, (cf_span){NULL, 0}, NULL);
    CF_REQUIRE(cf_test_req_header(
                   &req, SP("Turbo-Frame"),
                   (cf_span){(const unsigned char *)"\xC3\xA9", 2}) == CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    cf_span body = cf_buf_span(resp.body);
    CF_CHECK(body.len > 15 && memcmp(body.ptr, "<!DOCTYPE html>", 15) == 0);
    CF_CHECK(span_contains(body, "<body class="));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Forged cross-site POST: the chain's CSRF check answers 422 (before the
 * action body and before any rate-limit counting). */
CF_TEST(sessions_create_cross_site_post_is_422) {
    sess_env env;
    CF_REQUIRE(env_open(&env));
    seed_golden_login_state(&env);

    cf_request req;
    cf_response resp;
    post_session(&req, "email_address=david%4037signals.com&password=x",
                 "cross-site", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Success: the session row and signed cookie, redirect to root (no stored
 * return-to). */
CF_TEST(sessions_create_authenticates_and_redirects_root) {
    sess_env env;
    CF_REQUIRE(env_open(&env));
    cf_auth_rate_limit_test_reset();
    cf_str digest = {0};
    CF_REQUIRE(cf_auth_password_digest(CSTR(PASSWORD), 4, &digest) == CF_OK);
    seed_user(env.scratch.db, 1, "Active", "a@example.com", digest.ptr, 0, 0);
    cf_str_dispose(&digest);

    cf_request req;
    cf_response resp;
    post_session(&req, "email_address=a%40example.com&password=" PASSWORD,
                 "same-origin", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, "Location: " ORIGIN "/\r\n"));

    /* State: exactly one sessions row for the user, and the emitted
     * session_token cookie verifies to that row's token. */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions WHERE user_id=1") == 1);
    char token[256];
    CF_REQUIRE(cf_db_test_text(cf_db_handle(env.scratch.db),
                               "SELECT token FROM sessions WHERE user_id=1",
                               token, sizeof token) != NULL);
    char header[4096];
    CF_REQUIRE(extract_set_cookie(&resp, &req, "session_token", header,
                                  sizeof header));
    cf_str verified = {0};
    bool found = false;
    CF_REQUIRE(cf_auth_signed_cookie_verify(
                   (cf_span){(const unsigned char *)env.config
                                 ->secret_key_base,
                             env.config->secret_key_base_len},
                   SP("session_token"),
                   (cf_span){(const unsigned char *)header, strlen(header)}, 0,
                   &verified, &found) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(verified.len == strlen(token) &&
             memcmp(verified.ptr, token, verified.len) == 0);
    cf_str_dispose(&verified);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* post_authenticating_url: an unauthenticated hit on a protected page stores
 * the return-to in the encrypted session cookie; the next successful sign-in
 * redirects there and removes it. */
CF_TEST(sessions_create_returns_to_the_stored_url) {
    sess_env env;
    CF_REQUIRE(env_open(&env));
    cf_auth_rate_limit_test_reset();
    cf_str digest = {0};
    CF_REQUIRE(cf_auth_password_digest(CSTR(PASSWORD), 4, &digest) == CF_OK);
    seed_user(env.scratch.db, 1, "Active", "a@example.com", digest.ptr, 0, 0);
    cf_str_dispose(&digest);

    /* Leg 1: GET /rooms/1 unauthenticated -> sign-in redirect that stores the
     * return-to in _campfire_session. */
    cf_request req1;
    cf_response resp1;
    get_protected_probe(&req1);
    CF_REQUIRE(run_request(&env, &req1, &resp1));
    CF_CHECK(resp1.status == 302);
    CF_CHECK(head_contains(&resp1, &req1,
                           "Location: " ORIGIN "/session/new\r\n"));
    char session_cookie[4096];
    CF_REQUIRE(extract_set_cookie(&resp1, &req1, "_campfire_session",
                                  session_cookie, sizeof session_cookie));
    cf_response_dispose(&resp1);

    /* Leg 2: POST /session with the return-to cookie. */
    char cookie_header[4200];
    snprintf(cookie_header, sizeof cookie_header, "_campfire_session=%s",
             session_cookie);
    cf_request req2;
    cf_response resp2;
    post_session(&req2, "email_address=a%40example.com&password=" PASSWORD,
                 "same-origin", cookie_header);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 302);
    CF_CHECK(head_contains(&resp2, &req2,
                           "Location: " ORIGIN "/rooms/1\r\n"));
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions WHERE user_id=1") == 1);
    cf_response_dispose(&resp2);
    env_close(&env);
}

/* Failure: the 401 sign-in page with flash.now[:alert], exactly
 * golden/a/sessions_new_rejected.html (submitted email echoed). */
CF_TEST(sessions_create_wrong_password_matches_the_rejected_golden) {
    sess_env env;
    CF_REQUIRE(env_open(&env));
    cf_auth_rate_limit_test_reset();
    CF_REQUIRE(sessions_assets_setup());
    seed_golden_login_state(&env);

    cf_request req;
    cf_response resp;
    post_session(&req,
                 "email_address=david%4037signals.com&password=wrong-password",
                 "same-origin", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 401);
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("sessions_new_rejected", (const char *)body.ptr,
                     body.len);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Parameter failure: no email_address/password at all is still the reference
 * failure arm (no require/permit here): 401 with the rejection alert and no
 * echoed value (the fixture with the alert also echoes an email, so this arm
 * is asserted structurally). */
CF_TEST(sessions_create_missing_parameters_is_401) {
    sess_env env;
    CF_REQUIRE(env_open(&env));
    cf_auth_rate_limit_test_reset();
    CF_REQUIRE(sessions_assets_setup());
    seed_golden_login_state(&env);

    cf_request req;
    cf_response resp;
    post_session(&req, "", "same-origin", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 401);
    cf_span body = cf_buf_span(resp.body);
    CF_CHECK(span_contains(body, "Too many requests or unauthorized."));
    CF_CHECK(span_contains(body, "<div class=\"panel shake\">"));
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* The fixed window: attempts 1..10 are evaluated (401), attempt 11 is the
 * 429 rejection page. */
CF_TEST(sessions_create_rate_limit_eleventh_attempt_is_429) {
    sess_env env;
    CF_REQUIRE(env_open(&env));
    cf_auth_rate_limit_test_reset();
    cf_test_clock_set_fixed_us(RATE_T0);
    CF_REQUIRE(sessions_assets_setup());
    seed_golden_login_state(&env);

    for (int attempt = 1; attempt <= 11; attempt++) {
        cf_request req;
        cf_response resp;
        post_session(&req,
                     "email_address=david%4037signals.com&password=wrong",
                     "same-origin", NULL);
        CF_REQUIRE(run_request(&env, &req, &resp));
        if (attempt <= 10) {
            CF_CHECK(resp.status == 401);
        } else {
            CF_CHECK(resp.status == 429);
            cf_span body = cf_buf_span(resp.body);
            CF_CHECK(span_contains(
                body, "Too many requests or unauthorized."));
        }
        cf_response_dispose(&resp);
    }
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions") == 0);
    cf_test_clock_clear();
    env_close(&env);
}

/* The window is fixed, not sliding: at exactly three minutes the counter
 * starts over. */
CF_TEST(sessions_create_rate_limit_window_resets_after_the_window) {
    sess_env env;
    CF_REQUIRE(env_open(&env));
    cf_auth_rate_limit_test_reset();
    cf_test_clock_set_fixed_us(RATE_T0);
    CF_REQUIRE(sessions_assets_setup());
    seed_golden_login_state(&env);

    for (int attempt = 1; attempt <= 11; attempt++) {
        cf_request req;
        cf_response resp;
        post_session(&req,
                     "email_address=david%4037signals.com&password=wrong",
                     "same-origin", NULL);
        CF_REQUIRE(run_request(&env, &req, &resp));
        CF_CHECK(attempt <= 10 ? resp.status == 401 : resp.status == 429);
        cf_response_dispose(&resp);
    }

    /* One window later the expired entry is reclaimed and the attempt is
     * evaluated again. */
    cf_test_clock_set_fixed_us(RATE_T0 + CF_AUTH_SIGN_IN_WINDOW_US);
    cf_request req;
    cf_response resp;
    post_session(&req, "email_address=david%4037signals.com&password=wrong",
                 "same-origin", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 401);
    cf_response_dispose(&resp);
    cf_test_clock_clear();
    env_close(&env);
}

/* Logout: redirect to root, session row gone, both cookies cleared, the
 * current user's push subscription removed, DISCONNECT_USER(reconnect=true)
 * delivered through the writer. */
CF_TEST(sessions_destroy_logs_out_and_redirects_root) {
    sess_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Active", "a@example.com", NULL, 0, 0);
    seed_user(env.scratch.db, 2, "Other", "b@example.com", NULL, 0, 0);
    char cookie_header[4096];
    make_session_cookie(env.config, env.scratch.db, cookie_header,
                        sizeof cookie_header, "sessions-destroy-token", 1);
    seed_push_subscription(env.scratch.db, 11, 1,
                           "https://push.example/ep1");
    seed_push_subscription(env.scratch.db, 12, 2,
                           "https://push.example/ep2");

    /* A prior protected hit without the token stores the return-to in the
     * encrypted session cookie, so the logout request carries both cookies —
     * the state in which Rails' reset_session deletes the session cookie. */
    cf_request req0;
    cf_response resp0;
    get_protected_probe(&req0);
    CF_REQUIRE(run_request(&env, &req0, &resp0));
    CF_CHECK(resp0.status == 302);
    char session_cookie[4096];
    CF_REQUIRE(extract_set_cookie(&resp0, &req0, "_campfire_session",
                                  session_cookie, sizeof session_cookie));
    cf_response_dispose(&resp0);
    size_t at = strlen(cookie_header);
    snprintf(cookie_header + at, sizeof cookie_header - at,
             "; _campfire_session=%s", session_cookie);

    cf_request req;
    cf_response resp;
    delete_session(&req, "https://push.example/ep1", cookie_header);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, "Location: " ORIGIN "/\r\n"));

    /* Cookies: both are deleted in the finished response. */
    CF_CHECK(head_contains(&resp, &req, "Set-Cookie: session_token=;"));
    CF_CHECK(head_contains(&resp, &req, "Set-Cookie: _campfire_session=;"));

    /* State: the session row is gone; the endpoint row of the current user is
     * gone and the other user's row is untouched. */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions "
                        "WHERE id=11") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions "
                        "WHERE id=12") == 1);

    /* Side effect: the mandatory DISCONNECT_USER(reconnect=true). */
    CF_REQUIRE(env.event_count == 1);
    CF_CHECK(env.events[0].kind == CF_EVENT_DISCONNECT_USER);
    CF_CHECK(env.events[0].user_id == 1);
    CF_CHECK(env.events[0].reconnect);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- Rack method override (V01's logout form shape) ----------------------- */

/* Value after "Location: " in the serialized response headers. */
static bool location_value(cf_response *resp, cf_request *req, char *out,
                           size_t cap) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    cf_span head = cf_buf_span(ser.headers);
    static const char needle[] = "Location: ";
    bool found = false;
    for (size_t i = 0; i + sizeof needle - 1 <= head.len; i++) {
        if (memcmp(head.ptr + i, needle, sizeof needle - 1) != 0) continue;
        size_t at = i + sizeof needle - 1, end = at;
        while (end < head.len && head.ptr[end] != '\r') end++;
        size_t n = end - at < cap - 1 ? end - at : cap - 1;
        memcpy(out, head.ptr + at, n);
        out[n] = '\0';
        found = true;
        break;
    }
    cf_buf_release(ser.headers);
    return found;
}

/* V01's logout form: POST /session + `_method=delete` must reach
 * sessions#destroy with the direct DELETE's response shape and side effects
 * (before the fix the POST row's create action answered the 401 page). */
CF_TEST(sessions_destroy_reached_via_post_method_override) {
    /* Direct DELETE /session?push_subscription_endpoint=... */
    sess_env direct_env;
    CF_REQUIRE(env_open(&direct_env));
    seed_user(direct_env.scratch.db, 1, "Active", "a@example.com", NULL, 0, 0);
    seed_push_subscription(direct_env.scratch.db, 11, 1,
                           "https://push.example/ep1");
    char direct_cookie[4096];
    make_session_cookie(direct_env.config, direct_env.scratch.db,
                        direct_cookie, sizeof direct_cookie,
                        "sessions-override-direct", 1);
    cf_request direct_req;
    cf_response direct_resp;
    delete_session(&direct_req, "https://push.example/ep1", direct_cookie);
    CF_REQUIRE(run_request(&direct_env, &direct_req, &direct_resp));
    CF_CHECK(direct_resp.status == 302);
    char direct_location[512];
    CF_REQUIRE(location_value(&direct_resp, &direct_req, direct_location,
                              sizeof direct_location));
    cf_span direct_body = {NULL, 0};
    if (direct_resp.body != NULL) {
        direct_body = cf_buf_span(direct_resp.body);
    }

    /* The form's shape: POST /session with the hidden `_method` field and
     * the same query string. */
    sess_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Active", "a@example.com", NULL, 0, 0);
    seed_push_subscription(env.scratch.db, 11, 1, "https://push.example/ep1");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "sessions-override-form", 1);
    cf_request req;
    static char target[2048];
    char encoded[1024];
    query_encode(encoded, sizeof encoded, "https://push.example/ep1");
    snprintf(target, sizeof target,
             "/session?push_subscription_endpoint=%s", encoded);
    cf_test_req_init(&req);
    req.method = CF_POST;
    req.original_method = CF_POST;
    req.path = SP("/session");
    req.target = SP(target);
    req.query = SP(target + strlen("/session?"));
    req.body = SP("_method=delete");
    CF_REQUIRE(cf_test_req_header(&req, SP("Content-Type"),
                                  SP("application/x-www-form-urlencoded")) ==
               CF_OK);
    CF_REQUIRE(cf_test_req_header(&req, SP("Sec-Fetch-Site"),
                                  SP("same-origin")) == CF_OK);
    CF_REQUIRE(cf_test_req_header(&req, SP("Cookie"), SP(cookie)) == CF_OK);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == direct_resp.status);
    char location[512];
    CF_REQUIRE(location_value(&resp, &req, location, sizeof location));
    CF_CHECK(strcmp(location, direct_location) == 0);
    cf_span body = {NULL, 0};
    if (resp.body != NULL) body = cf_buf_span(resp.body);
    CF_CHECK(body.len == direct_body.len);
    CF_CHECK(body.len == 0 || memcmp(body.ptr, direct_body.ptr, body.len) == 0);

    /* Same side effects: the session row, both cookies and this user's push
     * subscription go, and the mandatory disconnect is emitted. */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions WHERE id=11") ==
             0);
    CF_REQUIRE(env.event_count == 1);
    CF_CHECK(env.events[0].kind == CF_EVENT_DISCONNECT_USER);
    CF_CHECK(env.events[0].user_id == 1);
    CF_CHECK(env.events[0].reconnect);

    cf_response_dispose(&resp);
    cf_response_dispose(&direct_resp);
    env_close(&env);
    env_close(&direct_env);
}

/* Logout without a session: the chain redirects to sign-in and nothing is
 * removed or disconnected. */
CF_TEST(sessions_destroy_unauthenticated_redirects_to_sign_in) {
    sess_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Active", "a@example.com", NULL, 0, 0);
    seed_push_subscription(env.scratch.db, 11, 1,
                           "https://push.example/ep1");

    cf_request req;
    cf_response resp;
    delete_session(&req, "https://push.example/ep1", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions") == 1);
    CF_CHECK(env.event_count == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
