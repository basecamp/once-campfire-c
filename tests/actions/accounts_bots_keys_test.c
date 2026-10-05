/* tests/actions/accounts_bots_keys_test.c — A-accounts-bots-keys acceptance:
 * `accounts/bots/keys#update` (route IDs 27 PATCH, 28 PUT), per
 * docs/devel/implementation/contracts/controller-packets.md
 * "A-accounts-bots-keys" and 03-application.md's family boundary ("Key
 * rotation invalidates prior bot keys").
 *
 * Cases:
 *   - route binding: ids 27/28 both reach the update action;
 *   - rotation: the token changes (still 12 chars), the prior "id-token"
 *     no longer authenticates via User.authenticate_bot while the new one
 *     does, the row keeps its name/role/status/webhook, and the response
 *     is the redirect to <ORIGIN>/account/bots;
 *   - failure paths: unknown/deactivated/non-bot/non-integer bot_id is 404
 *     with the token untouched; unauthenticated redirects to sign-in;
 *     non-admin is 403 before the lookup (unknown bot still 403); a missing
 *     CSRF header is 422; none of them rotates the key.
 *
 * The route double binds rows 27/28 to the real action, so every case runs
 * the real A00 dispatch path. Rotation needs no control handler (no
 * DISCONNECT_USER), but the writer still runs for cf_write.
 *
 * cf.h and src/actions/actions.h are integrator-owned and do not yet declare
 * this packet's symbol, so the entry point is declared here; the
 * integrator's routes.c rebind needs the same declaration (c_symbol
 * cf_action_accounts_bots_keys_update).
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
#include "models/user.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cf_err cf_action_accounts_bots_keys_update(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="
#define KEYS_REDIRECT "Location: " ORIGIN "/account/bots\r\n"
#define KEYS_SIGN_IN "Location: " ORIGIN "/session/new\r\n"

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- scratch app + started writer ------------------------------------------- */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
} keys_env;

static bool env_open(keys_env *env) {
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
    cf_test_routes_reset();
    /* routes.json ids 27 (PATCH) / 28 (PUT). The double's `(.:format)`
     * grammar needs an extension for a literal last segment, so the bare
     * form is registered as well. */
    if (cf_test_routes_add("PATCH", "/account/bots/:bot_id/key(.:format)",
                           27, cf_action_accounts_bots_keys_update) != CF_OK ||
        cf_test_routes_add("PATCH", "/account/bots/:bot_id/key", 27,
                           cf_action_accounts_bots_keys_update) != CF_OK ||
        cf_test_routes_add("PUT", "/account/bots/:bot_id/key(.:format)", 28,
                           cf_action_accounts_bots_keys_update) != CF_OK ||
        cf_test_routes_add("PUT", "/account/bots/:bot_id/key", 28,
                           cf_action_accounts_bots_keys_update) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(keys_env *env) {
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

/* --- seeding ----------------------------------------------------------------- */

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

/* --- request/response helpers -------------------------------------------------- */

static bool run_request(keys_env *env, cf_request *req, cf_response *resp) {
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

static void req_rotate(cf_request *req, cf_method method, const char *path,
                       const char *cookie, const char *fetch_site) {
    cf_test_req_init(req);
    req->method = method;
    req->original_method = method;
    req->path = SP(path);
    req->target = SP(path);
    req->body = SP("");
    CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                  SP("application/x-www-form-urlencoded")) ==
               CF_OK);
    if (fetch_site != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                      SP(fetch_site)) == CF_OK);
    }
    if (cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    }
}

/* Whether "id-token" authenticates as a bot right now. */
static bool bot_key_works(cf_db *db, const char *key) {
    bool found = false;
    cf_user bot = {0};
    cf_err rc = cf_user_authenticate_bot(
        db, (cf_str){(char *)key, strlen(key)}, &found, &bot);
    cf_user_dispose(&bot);
    CF_REQUIRE(rc == CF_OK);
    return found;
}

/* --- acceptance: route binding --------------------------------------------------- */

/* Routes 27 (PATCH) and 28 (PUT) both reach the update action. */
CF_TEST(accounts_bots_keys_route_ids_bind_the_update) {
    keys_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Helper", "oldtoken1234", 2, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_rotate(&req, CF_PATCH, "/account/bots/7/key", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, KEYS_REDIRECT));
    cf_response_dispose(&resp);

    req_rotate(&req, CF_PUT, "/account/bots/7/key", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, KEYS_REDIRECT));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- acceptance: rotation ---------------------------------------------------------- */

CF_TEST(accounts_bots_keys_rotation_invalidates_prior_key) {
    keys_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Helper", "oldtoken1234", 2, 0);
    seed_webhook(env.scratch.db, 7, "https://hooks.example/bot");
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);
    CF_REQUIRE(bot_key_works(env.scratch.db, "7-oldtoken1234"));

    cf_request req;
    cf_response resp;
    req_rotate(&req, CF_PATCH, "/account/bots/7/key", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, KEYS_REDIRECT));
    CF_CHECK(resp.body == NULL);
    cf_response_dispose(&resp);

    /* The prior key is dead; the row keeps everything but the token. */
    CF_CHECK(!bot_key_works(env.scratch.db, "7-oldtoken1234"));
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id = 7 AND "
                        "bot_token != 'oldtoken1234' AND LENGTH(bot_token) "
                        "= 12 AND name = 'Helper' AND role = 2 AND status = "
                        "0") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM webhooks WHERE user_id = 7 AND "
                        "url = 'https://hooks.example/bot'") == 1);
    env_close(&env);
}

CF_TEST(accounts_bots_keys_new_key_authenticates) {
    keys_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Helper", "oldtoken1234", 2, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_rotate(&req, CF_PUT, "/account/bots/7/key", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);

    /* Read the fresh token back and prove the new "id-token" authenticates. */
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(env.scratch.db),
                                  "SELECT bot_token FROM users WHERE id = 7",
                                  -1, &stmt, NULL) == SQLITE_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    const char *token =
        (const char *)sqlite3_column_text(stmt, 0);
    CF_REQUIRE(token != NULL && strlen(token) == 12 &&
               strcmp(token, "oldtoken1234") != 0);
    char fresh[64];
    snprintf(fresh, sizeof fresh, "7-%s", token);
    sqlite3_finalize(stmt);
    CF_CHECK(bot_key_works(env.scratch.db, fresh));
    env_close(&env);
}

/* --- acceptance: failure paths ------------------------------------------------------- */

CF_TEST(accounts_bots_keys_not_found_cases_keep_key) {
    keys_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 2, "Member", NULL, 0, 0);
    seed_user(env.scratch.db, 7, "Helper", "oldtoken1234", 2, 0);
    seed_user(env.scratch.db, 9, "Gone", "gonetoken123", 2, 1);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    /* Unknown id, deactivated bot, non-bot user, non-integer segment. */
    const char *paths[4] = {"/account/bots/999/key", "/account/bots/9/key",
                            "/account/bots/2/key", "/account/bots/abc/key"};
    for (size_t i = 0; i < 4; i++) {
        cf_request req;
        cf_response resp;
        req_rotate(&req, CF_PATCH, paths[i], admin_cookie, "same-origin");
        CF_REQUIRE(run_request(&env, &req, &resp));
        CF_CHECK(resp.status == 404);
        cf_response_dispose(&resp);
    }
    CF_CHECK(bot_key_works(env.scratch.db, "7-oldtoken1234"));
    env_close(&env);
}

CF_TEST(accounts_bots_keys_auth_cases_keep_key) {
    keys_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 2, "Member", NULL, 0, 0);
    seed_user(env.scratch.db, 7, "Helper", "oldtoken1234", 2, 0);
    char admin_cookie[1024];
    char member_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);
    make_session_cookie(env.config, env.scratch.db, member_cookie,
                        sizeof member_cookie, "tok-member", 2);

    cf_request req;
    cf_response resp;
    /* Unauthenticated: redirect, key kept. */
    req_rotate(&req, CF_PATCH, "/account/bots/7/key", NULL, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, KEYS_SIGN_IN));
    cf_response_dispose(&resp);
    /* Non-admin: 403 before the lookup — even an unknown bot is 403. */
    req_rotate(&req, CF_PATCH, "/account/bots/999/key", member_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    req_rotate(&req, CF_PATCH, "/account/bots/7/key", member_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    /* Forged cross-site request: 422, key kept. */
    req_rotate(&req, CF_PATCH, "/account/bots/7/key", admin_cookie,
               "cross-site");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    cf_response_dispose(&resp);

    CF_CHECK(bot_key_works(env.scratch.db, "7-oldtoken1234"));
    env_close(&env);
}

CF_TEST_MAIN()
