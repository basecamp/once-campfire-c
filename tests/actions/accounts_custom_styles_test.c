/* tests/actions/accounts_custom_styles_test.c — A-accounts-custom_styles
 * acceptance: `accounts/custom_styles#edit` (route ID 40) and
 * `accounts/custom_styles#update` (route IDs 41, 42), per
 * docs/devel/implementation/contracts/controller-packets.md
 * "A-accounts-custom_styles".
 *
 * Cases run the real A00 dispatch path through the route double, which
 * binds rows 40/41/42 to this packet's actions:
 *   - route binding: ids 40/41/42 resolve to edit/update;
 *   - edit renders the current styles (200, HTML), administrator-only,
 *     unauthenticated redirect, missing account is 500;
 *   - update stores the styles (302 to /account/custom_styles/edit),
 *     writes "" for an empty value, leaves the column alone when the key
 *     is absent, administrator-only, unauthenticated redirect, missing
 *     account param is 400.
 *
 * cf.h and src/actions/actions.h are integrator-owned and do not yet declare
 * this packet's symbols, so the entry points are declared here; the
 * integrator's routes.c rebind needs the same declarations (c_symbols
 * cf_action_accounts_custom_styles_edit /
 * cf_action_accounts_custom_styles_update).
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "http/http_internal.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

cf_err cf_action_accounts_custom_styles_edit(cf_ctx *ctx);
cf_err cf_action_accounts_custom_styles_update(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
} custom_styles_env;

static bool env_open(custom_styles_env *env) {
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
    if (cf_test_routes_add("GET", "/account/custom_styles/edit", 40,
                           cf_action_accounts_custom_styles_edit) != CF_OK ||
        cf_test_routes_add("GET", "/account/custom_styles/edit(.:format)", 40,
                           cf_action_accounts_custom_styles_edit) != CF_OK ||
        cf_test_routes_add("PATCH", "/account/custom_styles", 41,
                           cf_action_accounts_custom_styles_update) != CF_OK ||
        cf_test_routes_add("PUT", "/account/custom_styles", 42,
                           cf_action_accounts_custom_styles_update) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(custom_styles_env *env) {
    if (env->writer_started) cf_writer_stop(env->app);
    if (env->app != NULL) cf_app_destroy(env->app);
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

static int64_t count_sql(cf_db *db, const char *sql) {
    bool ok = false;
    int64_t value = cf_db_test_i64(cf_db_handle(db), sql, &ok);
    CF_REQUIRE(ok);
    return value;
}

static void seed_account(cf_db *db, const char *styles_or_null) {
    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
             "name, settings, singleton_guard, updated_at) VALUES (1, "
             "'2026-01-02 03:04:05', %s, 'ABCD-EFGH-IJKL', 'Campfire', "
             "'{\"restrict_room_creation_to_administrators\":false}', 0, "
             "'2026-01-02 03:04:05')",
             styles_or_null);
    exec_sql(db, sql);
}

static void seed_user(cf_db *db, int64_t id, const char *name,
                      const char *email, int role) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, updated_at) "
             "VALUES (%lld, NULL, NULL, '2026-01-02 03:04:05', '%s', '%s', "
             "NULL, %d, 0, '2026-01-02 03:04:05')",
             (long long)id, email, name, role);
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, email, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    CF_REQUIRE(step == SQLITE_DONE);
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

static bool run_request(custom_styles_env *env, cf_request *req,
                        cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void prepare_get(cf_request *req, const char *cookie) {
    cf_test_req_init(req);
    req->path = SP("/account/custom_styles/edit");
    req->target = SP("/account/custom_styles/edit");
    if (cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    }
}

static void prepare_form(cf_request *req, cf_method method, const char *body,
                         const char *cookie) {
    cf_test_req_init(req);
    req->method = method;
    req->original_method = method;
    req->path = SP("/account/custom_styles");
    req->target = SP("/account/custom_styles");
    req->body = SP(body);
    CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                  SP("application/x-www-form-urlencoded")) ==
               CF_OK);
    CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                  SP("same-origin")) == CF_OK);
    if (cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    }
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

static bool body_contains(const cf_response *resp, const char *needle) {
    return resp->body != NULL && span_contains(cf_buf_span(resp->body), needle);
}

static bool head_contains(cf_response *resp, cf_request *req,
                          const char *needle) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    bool found = ser.headers != NULL &&
                 span_contains(cf_buf_span(ser.headers), needle);
    cf_buf_release(ser.headers);
    return found;
}

static const char *read_styles(cf_db *db, char *out, size_t cap) {
    const char *value = cf_db_test_text(
        cf_db_handle(db), "SELECT custom_styles FROM accounts WHERE id=1", out,
        cap);
    CF_REQUIRE(value != NULL);
    return value;
}

CF_TEST(accounts_custom_styles_route_ids_bind_the_actions) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/account/custom_styles/edit", 40,
                                  cf_action_accounts_custom_styles_edit) ==
               CF_OK);
    CF_REQUIRE(cf_test_routes_add("PATCH", "/account/custom_styles", 41,
                                  cf_action_accounts_custom_styles_update) ==
               CF_OK);
    CF_REQUIRE(cf_test_routes_add("PUT", "/account/custom_styles", 42,
                                  cf_action_accounts_custom_styles_update) ==
               CF_OK);
    CF_CHECK(cf_route_action(40) == cf_action_accounts_custom_styles_edit);
    CF_CHECK(cf_route_action(41) == cf_action_accounts_custom_styles_update);
    CF_CHECK(cf_route_action(42) == cf_action_accounts_custom_styles_update);
}

CF_TEST(accounts_custom_styles_edit_renders_current_styles) {
    custom_styles_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db, "'.panel{color:red}'");
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_get(&req, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    CF_CHECK(body_contains(&resp, ".panel{color:red}"));
    CF_CHECK(body_contains(&resp, "name=\"account[custom_styles]\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_custom_styles_edit_requires_an_administrator) {
    custom_styles_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db, "NULL");
    seed_user(env.scratch.db, 1, "Member", "member@example.com", 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session", 1);

    cf_request req;
    cf_response resp;
    prepare_get(&req, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_custom_styles_edit_unauthenticated_redirects_to_sign_in) {
    custom_styles_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db, "NULL");

    cf_request req;
    cf_response resp;
    prepare_get(&req, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_custom_styles_edit_without_account_is_500) {
    custom_styles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_get(&req, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_custom_styles_update_stores_styles) {
    custom_styles_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db, "NULL");
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "account[custom_styles]=.panel%7Bcolor%3Ared"
                                "%7D",
                 cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN
                           "/account/custom_styles/edit\r\n"));
    char styles[256];
    CF_CHECK(strcmp(read_styles(env.scratch.db, styles, sizeof styles),
                    ".panel{color:red}") == 0);
    cf_writer_stats stats = {0};
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.committed == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_custom_styles_update_over_put_redirects) {
    custom_styles_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db, "NULL");
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PUT, "account[custom_styles]=x", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN
                           "/account/custom_styles/edit\r\n"));
    char styles[256];
    CF_CHECK(strcmp(read_styles(env.scratch.db, styles, sizeof styles), "x") ==
             0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_custom_styles_update_empty_value_writes_empty) {
    custom_styles_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db, "'.old{}'");
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "account[custom_styles]=", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    char styles[256];
    CF_CHECK(strcmp(read_styles(env.scratch.db, styles, sizeof styles), "") ==
             0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_custom_styles_update_absent_key_leaves_column) {
    custom_styles_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db, "'.keep{}'");
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "account[name]=Ignored", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    char styles[256];
    CF_CHECK(strcmp(read_styles(env.scratch.db, styles, sizeof styles),
                    ".keep{}") == 0);
    /* The unpermitted name key is ignored too. */
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM accounts WHERE name='Campfire'") ==
             1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_custom_styles_update_requires_an_administrator) {
    custom_styles_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db, "NULL");
    seed_user(env.scratch.db, 1, "Member", "member@example.com", 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "account[custom_styles]=x", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM accounts WHERE custom_styles IS "
                       "NULL") == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_custom_styles_update_unauthenticated_redirects) {
    custom_styles_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db, "NULL");

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "account[custom_styles]=x", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_custom_styles_update_without_account_param_is_400) {
    custom_styles_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db, "NULL");
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "custom_styles=x", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 400);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
