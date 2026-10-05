/* tests/actions/accounts_join_codes_test.c — A-accounts-join_codes
 * acceptance: `accounts/join_codes#create` (route ID 37), per
 * docs/devel/implementation/contracts/controller-packets.md
 * "A-accounts-join_codes".
 *
 * Cases run the real A00 dispatch path through the route double, which
 * binds row 37 to this packet's action:
 *   - route binding: id 37 resolves to create;
 *   - create rotates the join code (new 4-4-4 value, 302 to /account/edit);
 *   - administrator-only (403 leaves the code), unauthenticated redirect,
 *     cross-site POST is 422, a missing account is 500.
 *
 * cf.h and src/actions/actions.h are integrator-owned and do not yet declare
 * this packet's symbol, so the entry point is declared here; the
 * integrator's routes.c rebind needs the same declaration (c_symbol
 * cf_action_accounts_join_codes_create).
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

#include <ctype.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

cf_err cf_action_accounts_join_codes_create(cf_ctx *ctx);

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
} join_codes_env;

static bool env_open(join_codes_env *env) {
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
    if (cf_test_routes_add("POST", "/account/join_code", 37,
                           cf_action_accounts_join_codes_create) != CF_OK ||
        cf_test_routes_add("POST", "/account/join_code(.:format)", 37,
                           cf_action_accounts_join_codes_create) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(join_codes_env *env) {
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

static void seed_account(cf_db *db) {
    exec_sql(db,
             "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
             "name, settings, singleton_guard, updated_at) VALUES (1, "
             "'2026-01-02 03:04:05', NULL, 'ABCD-EFGH-IJKL', 'Campfire', "
             "'{\"restrict_room_creation_to_administrators\":false}', 0, "
             "'2026-01-02 03:04:05')");
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

static bool run_request(join_codes_env *env, cf_request *req,
                        cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void prepare_post(cf_request *req, const char *cookie,
                         const char *sec_fetch_site) {
    cf_test_req_init(req);
    req->method = CF_POST;
    req->original_method = CF_POST;
    req->path = SP("/account/join_code");
    req->target = SP("/account/join_code");
    req->body = SP("");
    CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                  SP("application/x-www-form-urlencoded")) ==
               CF_OK);
    CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                  SP(sec_fetch_site)) == CF_OK);
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

static void read_join_code(cf_db *db, char *out, size_t cap) {
    CF_REQUIRE(cf_db_test_text(cf_db_handle(db),
                               "SELECT join_code FROM accounts WHERE id=1",
                               out, cap) != NULL);
}

/* `SecureRandom.alphanumeric(12)` grouped 4-4-4. */
static bool join_code_shape(const char *code) {
    if (strlen(code) != 14 || code[4] != '-' || code[9] != '-') return false;
    for (size_t i = 0; i < 14; i++) {
        if (i == 4 || i == 9) continue;
        if (!isalnum((unsigned char)code[i])) return false;
    }
    return true;
}

CF_TEST(accounts_join_codes_route_id_binds_the_action) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("POST", "/account/join_code", 37,
                                  cf_action_accounts_join_codes_create) ==
               CF_OK);
    CF_CHECK(cf_route_action(37) == cf_action_accounts_join_codes_create);
}

CF_TEST(accounts_join_codes_create_rotates_the_code) {
    join_codes_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_post(&req, cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/account/edit\r\n"));
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    CF_CHECK(resp.body == NULL);
    char code[64];
    read_join_code(env.scratch.db, code, sizeof code);
    CF_CHECK(join_code_shape(code));
    CF_CHECK(strcmp(code, "ABCD-EFGH-IJKL") != 0);
    cf_writer_stats stats = {0};
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.committed == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_join_codes_create_requires_an_administrator) {
    join_codes_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Member", "member@example.com", 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session", 1);

    cf_request req;
    cf_response resp;
    prepare_post(&req, cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    char code[64];
    read_join_code(env.scratch.db, code, sizeof code);
    CF_CHECK(strcmp(code, "ABCD-EFGH-IJKL") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_join_codes_create_unauthenticated_redirects_to_sign_in) {
    join_codes_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);

    cf_request req;
    cf_response resp;
    prepare_post(&req, NULL, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    char code[64];
    read_join_code(env.scratch.db, code, sizeof code);
    CF_CHECK(strcmp(code, "ABCD-EFGH-IJKL") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_join_codes_create_cross_site_post_is_422) {
    join_codes_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_post(&req, cookie, "cross-site");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    char code[64];
    read_join_code(env.scratch.db, code, sizeof code);
    CF_CHECK(strcmp(code, "ABCD-EFGH-IJKL") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_join_codes_create_without_account_is_500) {
    join_codes_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_post(&req, cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    CF_CHECK(count_sql(env.scratch.db, "SELECT count(*) FROM accounts") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
