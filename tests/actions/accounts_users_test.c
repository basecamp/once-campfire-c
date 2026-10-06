/* tests/actions/accounts_users_test.c — A-accounts-users acceptance:
 * `accounts/users#index` (route ID 19), `accounts/users#update` (route IDs
 * 24, 25) and `accounts/users#destroy` (route ID 26), per
 * docs/devel/implementation/contracts/controller-packets.md
 * "A-accounts-users".
 *
 * Cases run the real A00 dispatch path through the route double, which
 * binds rows 19/24/25/26 to this packet's actions:
 *   - route binding: ids 19/24/25/26 resolve to index/update/destroy;
 *   - index serves the ordered non-bot turbo stream (200,
 *     text/vnd.turbo-stream.html) with page slices and the next-page
 *     container, 406 for HTML, 302 when unauthenticated;
 *   - update promotes to administrator and demotes anything else to
 *     member (302 to /account/edit), requires the user param (400),
 *     administrator-only (403), 404 for unknown/non-numeric/inactive ids;
 *   - destroy deactivates (status, scrambled email, sessions removed,
 *     mandatory disconnect applied; 302), administrator-only,
 *     unauthenticated redirect, 404 for a missing user.
 *
 * cf.h and src/actions/actions.h are integrator-owned and do not yet declare
 * this packet's symbols, so the entry points are declared here; the
 * integrator's routes.c rebind needs the same declarations (c_symbols
 * cf_action_accounts_users_index / cf_action_accounts_users_update /
 * cf_action_accounts_users_destroy).
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "views.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "http/http_internal.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <inttypes.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

cf_err cf_action_accounts_users_index(cf_ctx *ctx);
cf_err cf_action_accounts_users_update(cf_ctx *ctx);
cf_err cf_action_accounts_users_destroy(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- scratch app + started writer + control capture ------------------------ */

#define USERS_MAX_EVENTS 8

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
    cf_event control_events[USERS_MAX_EVENTS];
    size_t control_event_count;
} accounts_users_env;

static cf_err accounts_users_capture_control(void *ctx,
                                             const cf_event *event) {
    accounts_users_env *env = ctx;
    if (env->control_event_count < USERS_MAX_EVENTS) {
        env->control_events[env->control_event_count++] = *event;
    }
    return CF_OK;
}

static bool env_open(accounts_users_env *env) {
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
    if (cf_views_assets_configure("tests/fixtures/assets") != CF_OK) return false;
    if (cf_writer_start(env->app, env->config) != CF_OK) return false;
    env->writer_started = true;
    if (cf_writer_set_control_handler(env->app,
                                      accounts_users_capture_control,
                                      env) != CF_OK) {
        return false;
    }
    cf_test_routes_reset();
    if (cf_test_routes_add("GET", "/account/users", 19,
                           cf_action_accounts_users_index) != CF_OK ||
        cf_test_routes_add("GET", "/account/users(.:format)", 19,
                           cf_action_accounts_users_index) != CF_OK ||
        cf_test_routes_add("PATCH", "/account/users/:id", 24,
                           cf_action_accounts_users_update) != CF_OK ||
        cf_test_routes_add("PUT", "/account/users/:id", 25,
                           cf_action_accounts_users_update) != CF_OK ||
        cf_test_routes_add("DELETE", "/account/users/:id", 26,
                           cf_action_accounts_users_destroy) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(accounts_users_env *env) {
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

static void seed_user(cf_db *db, int64_t id, const char *name,
                      const char *email, int role, int status) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, updated_at) "
             "VALUES (%lld, NULL, NULL, '2026-01-02 03:04:05', %s, '%s', "
             "NULL, %d, %d, '2026-01-02 03:04:05')",
             (long long)id, email != NULL ? "?" : "NULL", name, role, status);
    if (email == NULL) {
        exec_sql(db, sql);
        return;
    }
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

static bool run_request(accounts_users_env *env, cf_request *req,
                        cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void prepare_get(cf_request *req, const char *path, const char *query,
                        const char *cookie, const char *accept) {
    cf_test_req_init(req);
    req->path = SP(path);
    req->target = SP(path);
    if (query != NULL) req->query = SP(query);
    if (cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    }
    if (accept != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Accept"), SP(accept)) == CF_OK);
    }
}

static void prepare_form(cf_request *req, cf_method method, const char *path,
                         const char *body, const char *cookie) {
    cf_test_req_init(req);
    req->method = method;
    req->original_method = method;
    req->path = SP(path);
    req->target = SP(path);
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

#define TURBO_ACCEPT "text/vnd.turbo-stream.html"

/* --- acceptance ------------------------------------------------------------ */

CF_TEST(accounts_users_route_ids_bind_the_actions) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/account/users", 19,
                                  cf_action_accounts_users_index) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("PATCH", "/account/users/:id", 24,
                                  cf_action_accounts_users_update) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("PUT", "/account/users/:id", 25,
                                  cf_action_accounts_users_update) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("DELETE", "/account/users/:id", 26,
                                  cf_action_accounts_users_destroy) == CF_OK);
    CF_CHECK(cf_route_action(19) == cf_action_accounts_users_index);
    CF_CHECK(cf_route_action(24) == cf_action_accounts_users_update);
    CF_CHECK(cf_route_action(25) == cf_action_accounts_users_update);
    CF_CHECK(cf_route_action(26) == cf_action_accounts_users_destroy);
}

CF_TEST(accounts_users_index_serves_the_ordered_turbo_stream) {
    accounts_users_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Bob Member", "bob@example.com", 0, 0);
    seed_user(env.scratch.db, 3, "Zed Bot", NULL, 2, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_get(&req, "/account/users", NULL, cookie, TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/vnd.turbo-stream.html; "
                           "charset=utf-8\r\n"));
    /* Ordered, without bots: Ada before Bob, no Zed. */
    CF_CHECK(body_contains(&resp, "My settings"));
    CF_CHECK(body_contains(&resp, "Ada Admin"));
    CF_CHECK(body_contains(&resp, "action=\"/account/users/2\""));
    CF_CHECK(body_contains(&resp, "name=\"user[role]\""));
    CF_CHECK(body_contains(&resp, "value=\"delete\""));
    CF_CHECK(body_contains(&resp, "Delete Bob Member"));
    CF_CHECK(body_contains(&resp, "Bob Member"));
    CF_CHECK(!body_contains(&resp, "Zed Bot"));
    CF_CHECK(!body_contains(&resp, "next_page_container\" src="));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_users_index_second_page_is_empty_without_a_loader) {
    accounts_users_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_get(&req, "/account/users", "page=2", cookie, TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(!body_contains(&resp, "data-user-id="));
    CF_CHECK(!body_contains(&resp, "src=\"/account/users.turbo_stream"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_users_index_html_is_406) {
    accounts_users_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_get(&req, "/account/users", NULL, cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_users_index_unauthenticated_redirects_to_sign_in) {
    accounts_users_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);

    cf_request req;
    cf_response resp;
    prepare_get(&req, "/account/users", NULL, NULL, TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_users_update_promotes_to_administrator) {
    accounts_users_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Bob Member", "bob@example.com", 0, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "/account/users/2",
                 "user[role]=administrator", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/account/edit\r\n"));
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM users WHERE id=2 AND role=1") ==
             1);
    cf_writer_stats stats = {0};
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.committed == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_users_update_demotes_anything_else_to_member) {
    accounts_users_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Bob Admin", "bob@example.com", 1, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PUT, "/account/users/2", "user[role]=owner", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM users WHERE id=2 AND role=0") ==
             1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_users_update_without_user_param_is_400) {
    accounts_users_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Bob Member", "bob@example.com", 0, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "/account/users/2", "role=administrator",
                 cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 400);
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM users WHERE id=2 AND role=0") ==
             1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_users_update_requires_an_administrator) {
    accounts_users_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Member", "member@example.com", 0, 0);
    seed_user(env.scratch.db, 2, "Bob Member", "bob@example.com", 0, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "/account/users/2",
                 "user[role]=administrator", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM users WHERE id=2 AND role=0") ==
             1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_users_update_missing_user_is_not_found) {
    accounts_users_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "/account/users/999",
                 "user[role]=administrator", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_users_update_non_numeric_id_is_not_found) {
    accounts_users_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "/account/users/abc",
                 "user[role]=administrator", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_users_update_inactive_user_is_not_found) {
    accounts_users_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Gone", "gone@example.com", 0, 1);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "/account/users/2",
                 "user[role]=administrator", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM users WHERE id=2 AND role=0") ==
             1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_users_destroy_deactivates_and_disconnects) {
    accounts_users_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Bob Member", "bob@example.com", 0, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);
    seed_session(env.scratch.db, "target-session", 2);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_DELETE, "/account/users/2", "", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/account/edit\r\n"));
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM users WHERE id=2 AND status=1") ==
             1);
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM sessions WHERE user_id=2") == 0);
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM users WHERE id=1 AND status=0") ==
             1);
    char email[256];
    CF_REQUIRE(cf_db_test_text(cf_db_handle(env.scratch.db),
                               "SELECT email_address FROM users WHERE id=2",
                               email, sizeof email) != NULL);
    CF_CHECK(span_contains(
        (cf_span){(const unsigned char *)email, strlen(email)},
        "-deactivated-"));
    CF_REQUIRE(env.control_event_count == 1);
    CF_CHECK(env.control_events[0].kind == CF_EVENT_DISCONNECT_USER);
    CF_CHECK(env.control_events[0].user_id == 2);
    CF_CHECK(!env.control_events[0].reconnect);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_users_destroy_requires_an_administrator) {
    accounts_users_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Member", "member@example.com", 0, 0);
    seed_user(env.scratch.db, 2, "Bob Member", "bob@example.com", 0, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_DELETE, "/account/users/2", "", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM users WHERE id=2 AND status=0") ==
             1);
    CF_CHECK(env.control_event_count == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_users_destroy_unauthenticated_redirects_to_sign_in) {
    accounts_users_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 2, "Bob Member", "bob@example.com", 0, 0);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_DELETE, "/account/users/2", "", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM users WHERE id=2 AND status=0") ==
             1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_users_destroy_missing_user_is_not_found) {
    accounts_users_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_DELETE, "/account/users/999", "", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    CF_CHECK(env.control_event_count == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_users_update_rejects_a_demoted_actor) {
    accounts_users_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Bob Member", "bob@example.com", 0, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);
    exec_sql(env.scratch.db, "UPDATE users SET role=0 WHERE id=1");

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "/account/users/2",
                 "user[role]=administrator", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM users WHERE id=2 AND role=0") ==
             1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
