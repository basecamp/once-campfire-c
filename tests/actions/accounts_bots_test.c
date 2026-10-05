/* tests/actions/accounts_bots_test.c — A-accounts-bots acceptance:
 * `accounts/bots#index` (route ID 29), `#create` (30), `#new` (31), `#edit`
 * (32), `#update` (34, 35) and `#destroy` (36), per
 * docs/devel/implementation/contracts/controller-packets.md "A-accounts-bots"
 * and 03-application.md's family boundaries (administrator checks, permit
 * lists, callback order, status/redirect).
 *
 * Cases:
 *   - route binding: ids 29/30/31/32/34/35/36 each reach their action;
 *   - index: lists active bots ordered (name + "id-token"), excludes
 *     deactivated bots and members; 200 text/html;
 *   - new/edit: 200 forms; edit prefills name and webhook URL, 404 for a
 *     member id or a non-integer id;
 *   - create: user + webhook rows, 12-char token, redirect to
 *     <ORIGIN>/account/bots; blank name inserts (exact reference behavior:
 *     neither Rails nor the Rust model validates bot names); missing `user`
 *     is 400; missing name is the reference 500 (Error::internal);
 *   - update: rename + webhook replace/destroy in one transaction,
 *     redirect; unknown/non-integer id is 404 with no effects; a nonscalar
 *     field reads as unchanged; a non-empty avatar scalar is the reference
 *     Invalid arm (500, no partial write); avatar "" deletes the attachment
 *     row and emits the best-effort purge event;
 *   - destroy: deactivates (status + sessions removed), emits mandatory
 *     DISCONNECT_USER(reconnect=false) through the registered control
 *     handler, redirect; unknown id is 404;
 *   - auth: unauthenticated requests redirect to sign-in (302, no effects);
 *     non-admin is 403 before any lookup (unknown bot still 403) with no
 *     effects; missing Sec-Fetch-Site on mutations is 422; JSON-only Accept
 *     on index/new/edit is 406.
 *
 * The route double (tests/app/support/route_double.c) binds the rows above
 * to the real actions, so every case runs the real A00 dispatch path. The
 * writer is started with a capture control handler for the mandatory
 * DISCONNECT_USER barrier.
 *
 * Renders use the packet's static shims (R-BOTS-VIEWS): assertions cover
 * status, content type and the escaped facts, not A02 goldens.
 *
 * cf.h and src/actions/actions.h are integrator-owned and do not yet declare
 * this packet's symbols, so the entry points are declared here; the
 * integrator's routes.c rebind needs the same declarations (c_symbols
 * cf_action_accounts_bots_index/create/new/edit/update/destroy).
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "http/http_internal.h"
#include "models/active_storage.h"
#include "models/user.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cf_err cf_action_accounts_bots_index(cf_ctx *ctx);
cf_err cf_action_accounts_bots_create(cf_ctx *ctx);
cf_err cf_action_accounts_bots_new(cf_ctx *ctx);
cf_err cf_action_accounts_bots_edit(cf_ctx *ctx);
cf_err cf_action_accounts_bots_update(cf_ctx *ctx);
cf_err cf_action_accounts_bots_destroy(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="
#define BOTS_REDIRECT "Location: " ORIGIN "/account/bots\r\n"
#define BOTS_SIGN_IN "Location: " ORIGIN "/session/new\r\n"

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- scratch app + started writer + capture control handler ---------------- */

#define BOTS_MAX_EVENTS 16

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
    cf_event control_events[BOTS_MAX_EVENTS];
    size_t control_event_count;
} bots_env;

static cf_err bots_capture_control(void *ctx, const cf_event *event) {
    bots_env *env = ctx;
    if (env->control_event_count < BOTS_MAX_EVENTS) {
        env->control_events[env->control_event_count++] = *event;
    }
    return CF_OK;
}

static bool env_open(bots_env *env) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;
    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", ORIGIN},
        {"SECRET_KEY_BASE", HEX64},
        {"VAPID_PUBLIC_KEY", VAPID_PUBLIC_KEY},
        {"DATABASE_PATH", env->scratch.path},
    };
    if (cf_config_parse(entries, 4, NULL, &env->config) != CF_OK) return false;
    if (cf_app_create(env->config, &env->app) != CF_OK) {
        cf_config_destroy(env->config);
        env->config = NULL;
        return false;
    }
    if (cf_writer_start(env->app, env->config) != CF_OK) return false;
    env->writer_started = true;
    if (cf_writer_set_control_handler(env->app, bots_capture_control, env) !=
        CF_OK) {
        return false;
    }
    cf_test_routes_reset();
    /* routes.json ids 29-32/34-36. The double's `(.:format)` grammar needs
     * an extension for a literal last segment, so the bare form is
     * registered as well (the same note as the sessions/bans tests). */
    if (cf_test_routes_add("GET", "/account/bots(.:format)", 29,
                           cf_action_accounts_bots_index) != CF_OK ||
        cf_test_routes_add("GET", "/account/bots", 29,
                           cf_action_accounts_bots_index) != CF_OK ||
        cf_test_routes_add("POST", "/account/bots(.:format)", 30,
                           cf_action_accounts_bots_create) != CF_OK ||
        cf_test_routes_add("POST", "/account/bots", 30,
                           cf_action_accounts_bots_create) != CF_OK ||
        cf_test_routes_add("GET", "/account/bots/new(.:format)", 31,
                           cf_action_accounts_bots_new) != CF_OK ||
        cf_test_routes_add("GET", "/account/bots/new", 31,
                           cf_action_accounts_bots_new) != CF_OK ||
        cf_test_routes_add("GET", "/account/bots/:id/edit(.:format)", 32,
                           cf_action_accounts_bots_edit) != CF_OK ||
        cf_test_routes_add("GET", "/account/bots/:id/edit", 32,
                           cf_action_accounts_bots_edit) != CF_OK ||
        cf_test_routes_add("PATCH", "/account/bots/:id(.:format)", 34,
                           cf_action_accounts_bots_update) != CF_OK ||
        cf_test_routes_add("PATCH", "/account/bots/:id", 34,
                           cf_action_accounts_bots_update) != CF_OK ||
        cf_test_routes_add("PUT", "/account/bots/:id(.:format)", 35,
                           cf_action_accounts_bots_update) != CF_OK ||
        cf_test_routes_add("PUT", "/account/bots/:id", 35,
                           cf_action_accounts_bots_update) != CF_OK ||
        cf_test_routes_add("DELETE", "/account/bots/:id(.:format)", 36,
                           cf_action_accounts_bots_destroy) != CF_OK ||
        cf_test_routes_add("DELETE", "/account/bots/:id", 36,
                           cf_action_accounts_bots_destroy) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(bots_env *env) {
    if (env->writer_started) cf_writer_stop(env->app);
    if (env->app != NULL) cf_app_destroy(env->app); /* frees config too */
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
                      const char *token, int role, int status) {
    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, updated_at) "
             "VALUES (%lld, NULL, %s%s%s, '2026-01-02 03:04:05', NULL, '%s', "
             "NULL, %d, %d, '2026-01-02 03:04:05')",
             (long long)id, token != NULL ? "'" : "",
             token != NULL ? token : "NULL", token != NULL ? "'" : "", name,
             role, status);
    exec_sql(db, sql);
}

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

static void seed_webhook(cf_db *db, int64_t user_id, const char *url) {
    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO webhooks (created_at, updated_at, url, user_id) "
             "VALUES ('2026-01-02 03:04:05', '2026-01-02 03:04:05', '%s', "
             "%lld)",
             url, (long long)user_id);
    exec_sql(db, sql);
}

static void seed_avatar(cf_db *db, int64_t bot_id) {
    exec_sql(db,
             "INSERT INTO active_storage_blobs (byte_size, checksum, "
             "content_type, created_at, filename, key, metadata, "
             "service_name) VALUES (8, NULL, 'image/png', "
             "'2026-01-02 03:04:05', 'a.png', 'bots-avatar-1', NULL, "
             "'local')");
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO active_storage_attachments (blob_id, created_at, "
             "name, record_id, record_type) VALUES (1, "
             "'2026-01-02 03:04:05', 'avatar', %lld, 'User')",
             (long long)bot_id);
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

/* --- request/response helpers ---------------------------------------------- */

static bool run_request(bots_env *env, cf_request *req, cf_response *resp) {
    cf_response_init(resp);
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static bool span_contains(cf_span haystack, const char *needle) {
    size_t len = strlen(needle);
    if (len == 0) return true;
    if (haystack.len < len || haystack.ptr == NULL) return false;
    for (size_t i = 0; i + len <= haystack.len; i++) {
        if (memcmp(haystack.ptr + i, needle, len) == 0) return true;
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

static bool body_contains(const cf_response *resp, const char *needle) {
    return buf_contains(resp->body, needle);
}

static void req_get(cf_request *req, const char *path, const char *cookie,
                    const char *accept) {
    cf_test_req_init(req);
    req->method = CF_GET;
    req->original_method = CF_GET;
    req->path = SP(path);
    req->target = SP(path);
    if (accept != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Accept"), SP(accept)) == CF_OK);
    }
    if (cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    }
}

static void req_mut(cf_request *req, cf_method method, const char *path,
                    const char *body, const char *cookie,
                    const char *fetch_site) {
    cf_test_req_init(req);
    req->method = method;
    req->original_method = method;
    req->path = SP(path);
    req->target = SP(path);
    req->body = SP(body);
    CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                  SP("application/x-www-form-urlencoded")) ==
               CF_OK);
    /* The reference verifies by Sec-Fetch-Site, not tokens: "same-origin"
     * passes; "cross-site" is the forged request (422). A NULL fetch_site
     * omits the header, which plain-HTTP requests allow. */
    if (fetch_site != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                      SP(fetch_site)) == CF_OK);
    }
    if (cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    }
}

/* --- acceptance: route binding ---------------------------------------------- */

/* Every assigned route ID reaches its action through the real dispatch path:
 * 29 index (200 list), 30 create (302), 31 new (200 form), 32 edit (200
 * form), 34/35 update (302), 36 destroy (302). */
CF_TEST(accounts_bots_route_ids_bind_their_actions) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Route", "routetoken12", 2, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;

    req_get(&req, "/account/bots", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "Chat bots"));
    CF_CHECK(body_contains(&resp, "Route"));
    cf_response_dispose(&resp);

    req_mut(&req, CF_POST, "/account/bots", "user[name]=Posted", admin_cookie,
            "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_REDIRECT));
    cf_response_dispose(&resp);

    req_get(&req, "/account/bots/new", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "New chat bot"));
    cf_response_dispose(&resp);

    req_get(&req, "/account/bots/7/edit", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "Edit bot"));
    cf_response_dispose(&resp);

    req_mut(&req, CF_PATCH, "/account/bots/7", "user[name]=Patched",
            admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_REDIRECT));
    cf_response_dispose(&resp);

    req_mut(&req, CF_PUT, "/account/bots/7", "user[name]=Putted",
            admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_REDIRECT));
    cf_response_dispose(&resp);

    req_mut(&req, CF_DELETE, "/account/bots/7", "", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_REDIRECT));
    cf_response_dispose(&resp);

    env_close(&env);
}

/* --- acceptance: index -------------------------------------------------------- */

CF_TEST(accounts_bots_index_lists_active_bots_ordered) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Zulu", "zulutoken123", 2, 0);
    seed_user(env.scratch.db, 8, "alpha", "alphatoken12", 2, 0);
    seed_user(env.scratch.db, 9, "Gone", "gonetoken123", 2, 1);
    seed_user(env.scratch.db, 10, "Member", NULL, 0, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    /* ORDER BY LOWER(name): alpha before Zulu; the "id-token" key renders. */
    CF_CHECK(body_contains(&resp, "alpha"));
    CF_CHECK(body_contains(&resp, "Zulu"));
    CF_CHECK(body_contains(&resp, "8-alphatoken12"));
    CF_CHECK(body_contains(&resp, "7-zulutoken123"));
    CF_CHECK(!body_contains(&resp, "Gone"));
    CF_CHECK(!body_contains(&resp, "Member"));
    cf_span body = cf_buf_span(resp.body);
    /* Ordered: alpha's row precedes Zulu's row. */
    const char *text = (const char *)body.ptr;
    size_t alpha_at = body.len, zulu_at = body.len;
    for (size_t i = 0; i + 5 <= body.len; i++) {
        if (memcmp(text + i, "alpha", 5) == 0 && alpha_at == body.len) {
            alpha_at = i;
        }
        if (memcmp(text + i, "Zulu", 4) == 0 && zulu_at == body.len) {
            zulu_at = i;
        }
    }
    CF_CHECK(alpha_at < zulu_at);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_bots_index_escapes_names) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);
    /* A name with markup must render escaped (the shim uses cf_html_text). */
    exec_sql(env.scratch.db,
             "INSERT INTO users (bio, bot_token, created_at, email_address, "
             "name, password_digest, role, status, updated_at) VALUES (NULL, "
             "'esctoken1234', '2026-01-02 03:04:05', NULL, '<b>Bold</b>', "
             "NULL, 2, 0, '2026-01-02 03:04:05')");

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(!body_contains(&resp, "<b>Bold</b>"));
    CF_CHECK(body_contains(&resp, "&lt;b&gt;Bold&lt;/b&gt;"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_bots_index_unauthenticated_redirects_to_sign_in) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots", NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_SIGN_IN));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_bots_index_forbidden_for_non_admin) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 2, "Member", NULL, 0, 0);
    char member_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, member_cookie,
                        sizeof member_cookie, "tok-member", 2);

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots", member_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_bots_index_rejects_json_format) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots", admin_cookie, "application/json");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- acceptance: new ---------------------------------------------------------- */

CF_TEST(accounts_bots_new_renders_form) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots/new", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "user[name]"));
    CF_CHECK(body_contains(&resp, "user[webhook_url]"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_bots_new_unauthenticated_redirects_to_sign_in) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots/new", NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_SIGN_IN));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- acceptance: create ------------------------------------------------------- */

CF_TEST(accounts_bots_create_bot_with_webhook) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_POST, "/account/bots",
            "user[name]=Helper&user[webhook_url]=https://hooks.example/bot",
            admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_REDIRECT));
    CF_CHECK(resp.body == NULL);
    cf_response_dispose(&resp);

    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE role = 2 AND "
                        "status = 0 AND name = 'Helper'") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT LENGTH(bot_token) FROM users WHERE name = "
                        "'Helper'") == 12);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM webhooks WHERE url = "
                        "'https://hooks.example/bot'") == 1);
    env_close(&env);
}

CF_TEST(accounts_bots_create_bot_without_webhook) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_POST, "/account/bots", "user[name]=Silent",
            admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_REDIRECT));
    cf_response_dispose(&resp);

    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE role = 2 AND name "
                        "= 'Silent'") == 1);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM webhooks") ==
             0);
    env_close(&env);
}

/* Exact reference behavior: neither the Rails model nor the Rust/C models
 * validate bot names, so a blank name inserts and redirects. */
CF_TEST(accounts_bots_create_blank_name_inserts) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_POST, "/account/bots", "user[name]=", admin_cookie,
            "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE role = 2 AND name "
                        "= ''") == 1);
    env_close(&env);
}

/* `params.require(:user)`: a missing `user` root is ParameterMissing (400)
 * with no row effects. */
CF_TEST(accounts_bots_create_missing_user_param_is_400) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_POST, "/account/bots", "name=Nope", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 400);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id != 1") == 0);
    env_close(&env);
}

/* `name.ok_or(Error::internal)`: a missing name key is the reference 500,
 * never a silent blank insert, and writes nothing. */
CF_TEST(accounts_bots_create_missing_name_is_500) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_POST, "/account/bots",
            "user[webhook_url]=https://hooks.example/x", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id != 1") == 0);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM webhooks") ==
             0);
    env_close(&env);
}

CF_TEST(accounts_bots_create_forbidden_for_non_admin) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 2, "Member", NULL, 0, 0);
    char member_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, member_cookie,
                        sizeof member_cookie, "tok-member", 2);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_POST, "/account/bots", "user[name]=Nope", member_cookie,
            "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id NOT IN (1,2)") ==
             0);
    env_close(&env);
}

CF_TEST(accounts_bots_create_requires_csrf_and_auth) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    /* Forged cross-site POST: the chain's CSRF check answers 422, writing
     * nothing. */
    req_mut(&req, CF_POST, "/account/bots", "user[name]=Nope", admin_cookie,
            "cross-site");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id != 1") == 0);

    /* No session at all: the chain redirects to sign-in, writing nothing. */
    req_mut(&req, CF_POST, "/account/bots", "user[name]=Nope", NULL, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_SIGN_IN));
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id != 1") == 0);
    env_close(&env);
}

/* --- acceptance: edit --------------------------------------------------------- */

CF_TEST(accounts_bots_edit_shows_prefilled_form) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Helper", "helpertoken1", 2, 0);
    seed_webhook(env.scratch.db, 7, "https://hooks.example/bot");
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots/7/edit", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "value=\"Helper\""));
    CF_CHECK(body_contains(&resp, "value=\"https://hooks.example/bot\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* `set_bot` scopes to active bots: a member id and a non-integer id are
 * NotFound (404), even for an administrator. */
CF_TEST(accounts_bots_edit_not_found_cases) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 2, "Member", NULL, 0, 0);
    seed_user(env.scratch.db, 9, "Gone", "gonetoken123", 2, 1);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots/2/edit", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    req_get(&req, "/account/bots/9/edit", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    req_get(&req, "/account/bots/abc/edit", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    req_get(&req, "/account/bots/999/edit", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Admin gating precedes the lookup: a non-admin gets 403 even for an
 * unknown bot. */
CF_TEST(accounts_bots_edit_forbidden_before_lookup) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 2, "Member", NULL, 0, 0);
    char member_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, member_cookie,
                        sizeof member_cookie, "tok-member", 2);

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots/999/edit", member_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- acceptance: update ------------------------------------------------------- */

CF_TEST(accounts_bots_update_renames_and_replaces_webhook) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Old", "oldtoken1234", 2, 0);
    seed_webhook(env.scratch.db, 7, "https://old.example/hook");
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_PATCH, "/account/bots/7",
            "user[name]=New&user[webhook_url]=https://new.example/hook",
            admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_REDIRECT));
    cf_response_dispose(&resp);

    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id = 7 AND name = "
                        "'New' AND role = 2 AND status = 0") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM webhooks WHERE user_id = 7 AND "
                        "url = 'https://new.example/hook'") == 1);
    /* The token is untouched by an ordinary update. */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id = 7 AND "
                        "bot_token = 'oldtoken1234'") == 1);
    env_close(&env);
}

/* A blank webhook_url destroys the row (`filter(!blank)` in update_bot). */
CF_TEST(accounts_bots_update_blank_webhook_destroys_it) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Helper", "helpertoken1", 2, 0);
    seed_webhook(env.scratch.db, 7, "https://old.example/hook");
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_PUT, "/account/bots/7",
            "user[name]=Helper&user[webhook_url]=", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM webhooks WHERE user_id = 7") ==
             0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id = 7 AND name = "
                        "'Helper'") == 1);
    env_close(&env);
}

/* Unknown, deactivated, non-bot and non-integer ids are 404 with no effects
 * (PUT exercises the 35 alias here). */
CF_TEST(accounts_bots_update_not_found_cases) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 2, "Member", NULL, 0, 0);
    seed_user(env.scratch.db, 7, "Helper", "helpertoken1", 2, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    const char *paths[3] = {"/account/bots/999", "/account/bots/2",
                            "/account/bots/abc"};
    for (size_t i = 0; i < 3; i++) {
        cf_request req;
        cf_response resp;
        req_mut(&req, CF_PUT, paths[i], "user[name]=Nope", admin_cookie,
                "same-origin");
        CF_REQUIRE(run_request(&env, &req, &resp));
        CF_CHECK(resp.status == 404);
        cf_response_dispose(&resp);
    }
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE name = 'Nope'") ==
             0);
    env_close(&env);
}

/* A nonscalar field is the reference None (unchanged), not an error. */
CF_TEST(accounts_bots_update_nonscalar_field_is_unchanged) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Helper", "helpertoken1", 2, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_PATCH, "/account/bots/7",
            "user[name][x]=1&user[webhook_url]=https://hooks.example/kept",
            admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id = 7 AND name = "
                        "'Helper'") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM webhooks WHERE user_id = 7 AND "
                        "url = 'https://hooks.example/kept'") == 1);
    env_close(&env);
}

/* The Invalid avatar arm: the reference 500 with no partial write. */
CF_TEST(accounts_bots_update_invalid_avatar_is_500) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Helper", "helpertoken1", 2, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_PATCH, "/account/bots/7",
            "user[name]=Changed&user[avatar]=not-a-file", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id = 7 AND name = "
                        "'Helper'") == 1);
    env_close(&env);
}

/* The Delete avatar arm: an existing User/avatar attachment row is removed
 * (the blob row itself is purged asynchronously, so it stays). */
CF_TEST(accounts_bots_update_avatar_delete_removes_attachment) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Helper", "helpertoken1", 2, 0);
    seed_avatar(env.scratch.db, 7);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);
    CF_REQUIRE(count_rows(env.scratch.db,
                          "SELECT count(*) FROM active_storage_attachments "
                          "WHERE record_type = 'User' AND record_id = 7 AND "
                          "name = 'avatar'") == 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_PATCH, "/account/bots/7",
            "user[name]=Helper&user[avatar]=", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_attachments "
                        "WHERE record_type = 'User' AND record_id = 7 AND "
                        "name = 'avatar'") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_blobs WHERE id "
                        "= 1") == 1);
    env_close(&env);
}

CF_TEST(accounts_bots_update_auth_cases) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 2, "Member", NULL, 0, 0);
    seed_user(env.scratch.db, 7, "Helper", "helpertoken1", 2, 0);
    char admin_cookie[1024];
    char member_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);
    make_session_cookie(env.config, env.scratch.db, member_cookie,
                        sizeof member_cookie, "tok-member", 2);

    cf_request req;
    cf_response resp;
    /* Unauthenticated: redirect, no change. */
    req_mut(&req, CF_PATCH, "/account/bots/7", "user[name]=Nope", NULL,
            "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_SIGN_IN));
    cf_response_dispose(&resp);
    /* Non-admin: 403 even though the bot exists, no change. */
    req_mut(&req, CF_PATCH, "/account/bots/7", "user[name]=Nope",
            member_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    /* Forged cross-site request: 422, no change. */
    req_mut(&req, CF_PATCH, "/account/bots/7", "user[name]=Nope",
            admin_cookie, "cross-site");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    cf_response_dispose(&resp);

    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id = 7 AND name = "
                        "'Helper'") == 1);
    env_close(&env);
}

/* --- acceptance: destroy ------------------------------------------------------ */

CF_TEST(accounts_bots_destroy_deactivates) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Helper", "helpertoken1", 2, 0);
    seed_session(env.scratch.db, "tok-bot", 7);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);
    CF_REQUIRE(count_rows(env.scratch.db,
                          "SELECT count(*) FROM sessions WHERE user_id = 7") ==
               1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_DELETE, "/account/bots/7", "", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_REDIRECT));
    cf_response_dispose(&resp);

    /* Deactivated, sessions removed, row kept (email was NULL). */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id = 7 AND status "
                        "= 1 AND email_address IS NULL") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions WHERE user_id = 7") ==
             0);
    /* The mandatory barrier ran: exactly one DISCONNECT_USER, no reconnect. */
    CF_REQUIRE(env.control_event_count == 1);
    CF_CHECK(env.control_events[0].kind == CF_EVENT_DISCONNECT_USER);
    CF_CHECK(env.control_events[0].user_id == 7);
    CF_CHECK(!env.control_events[0].reconnect);
    env_close(&env);
}

CF_TEST(accounts_bots_destroy_not_found_and_auth) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 2, "Member", NULL, 0, 0);
    char admin_cookie[1024];
    char member_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);
    make_session_cookie(env.config, env.scratch.db, member_cookie,
                        sizeof member_cookie, "tok-member", 2);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_DELETE, "/account/bots/999", "", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    req_mut(&req, CF_DELETE, "/account/bots/abc", "", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    req_mut(&req, CF_DELETE, "/account/bots/2", "", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id = 2 AND status "
                        "= 0") == 1);

    req_mut(&req, CF_DELETE, "/account/bots/999", "", member_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);

    req_mut(&req, CF_DELETE, "/account/bots/999", "", NULL, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_SIGN_IN));
    cf_response_dispose(&resp);

    CF_CHECK(env.control_event_count == 0);
    env_close(&env);
}

CF_TEST_MAIN()
