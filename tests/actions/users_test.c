/* tests/actions/users_test.c — A-users acceptance: `users#new` (route ID
 * 50), `users#create` (route ID 51) and `users#show` (route ID 74), per
 * docs/devel/implementation/contracts/controller-packets.md "A-users".
 *
 * Cases:
 *   - route binding: ids 50/51/74 resolve to the three actions;
 *   - new success: 200, HTML, the stubbed join page carries the account's
 *     join code (and the help contact when an administrator exists);
 *   - new failures: a wrong join code is an empty 404 (no account lookup
 *     effects); a missing account row is a 500; a signed-in GET is
 *     redirected to root (require_unauthenticated_access);
 *   - create success: 302 to root, a member/active user row with the name
 *     and email, a session row plus the session_token cookie; open-room
 *     memberships are granted by the model layer;
 *   - create duplicate email: 302 to /session/new?email_address=<escaped>,
 *     no second row;
 *   - create failures: a missing :user is 400; a missing name is a 500; a
 *     wrong join code is 404 with no row; an empty password stores NULL;
 *   - show success: 200, HTML, the stubbed page carries the user name and a
 *     transfer id that verifies (purpose "transfer", ~4h expiry) for that
 *     user; the frame layout answers a Turbo-Frame request;
 *   - show failures: an unknown id and a non-castable id are 404; an
 *     unauthenticated GET is redirected to sign-in.
 *
 * The users/new and users/show templates are wired in (src/views/users.c);
 * bodies assert real render facts (join code, help-contact name, user name,
 * admin-only mail, avatar path); the transfer id itself is produced by the
 * real A01 signed-id layer and verified here.
 * The route double binds rows 50/51/74 to the real actions, so every case
 * runs the real A00 dispatch path, and the writer is started for the create
 * and sign-in writes.
 *
 * cf.h and src/actions/actions.h are integrator-owned and do not yet declare
 * this packet's symbols, so the entry points are declared here; the
 * integrator's routes.c rebind needs the same declarations (c_symbols
 * cf_action_users_new / cf_action_users_create / cf_action_users_show).
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
#include "models/types.h"
#include "views.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

cf_err cf_action_users_new(cf_ctx *ctx);
cf_err cf_action_users_create(cf_ctx *ctx);
cf_err cf_action_users_show(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="
#define JOIN_CODE "CRMu-l8Ge-KB9B"
#define PASSWORD "secret123456"

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* ---- scratch app ------------------------------------------------------------ */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
} users_env;

static bool env_open(users_env *env) {
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
    /* Real renders resolve digested assets through the pinned manifest. */
    if (cf_views_assets_configure("tests/fixtures/assets") != CF_OK) {
        fprintf(stderr, "  env_open: assets configure failed\n");
        return false;
    }
    if (cf_writer_start(env->app, env->config) != CF_OK) return false;
    env->writer_started = true;
    cf_test_routes_reset();
    if (cf_test_routes_add("GET", "/join/:join_code(.:format)", 50,
                           cf_action_users_new) != CF_OK ||
        cf_test_routes_add("GET", "/join/:join_code", 50,
                           cf_action_users_new) != CF_OK ||
        cf_test_routes_add("POST", "/join/:join_code(.:format)", 51,
                           cf_action_users_create) != CF_OK ||
        cf_test_routes_add("POST", "/join/:join_code", 51,
                           cf_action_users_create) != CF_OK ||
        cf_test_routes_add("GET", "/users/:id(.:format)", 74,
                           cf_action_users_show) != CF_OK ||
        cf_test_routes_add("GET", "/users/:id", 74,
                           cf_action_users_show) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(users_env *env) {
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

static int64_t count_rows(cf_db *db, const char *sql) {
    bool ok = false;
    int64_t value = cf_db_test_i64(cf_db_handle(db), sql, &ok);
    CF_REQUIRE(ok);
    return value;
}

static void seed_account(cf_db *db) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
             "name, settings, singleton_guard, updated_at) VALUES "
             "(1, '2026-01-02 03:04:05', NULL, '%s', 'HQ', NULL, 0, "
             "'2026-01-02 03:04:05')",
             JOIN_CODE);
    exec_sql(db, sql);
}

static void seed_user(cf_db *db, int64_t id, const char *name,
                      const char *email, const char *digest, int role,
                      int status) {
    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, updated_at) "
             "VALUES (%lld, NULL, NULL, '2026-01-02 03:04:05', %s, '%s', %s, "
             "%d, %d, '2026-01-02 03:04:05')",
             (long long)id, email != NULL ? "?" : "NULL", name,
             digest != NULL ? "?" : "NULL", role, status);
    if (email == NULL && digest == NULL) {
        exec_sql(db, sql);
        return;
    }
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) == SQLITE_OK);
    int bind = 1;
    if (email != NULL) sqlite3_bind_text(stmt, bind++, email, -1, SQLITE_TRANSIENT);
    if (digest != NULL) {
        sqlite3_bind_text(stmt, bind++, digest, -1, SQLITE_TRANSIENT);
    }
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
static void make_session_cookie(users_env *env, char *out, size_t cap,
                                const char *token, int64_t user_id) {
    seed_session(env->scratch.db, token, user_id);
    cf_str wire = {0};
    CF_REQUIRE(cf_auth_signed_cookie_generate(
                   (cf_span){(const unsigned char *)env->config
                                 ->secret_key_base,
                             env->config->secret_key_base_len},
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

static bool run_request(users_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void get_join(cf_request *req, const char *code,
                     const char *cookie_header) {
    static char target[512];
    cf_test_req_init(req);
    snprintf(target, sizeof target, "/join/%s", code);
    req->path = SP(target);
    req->target = SP(target);
    if (cookie_header != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie_header)) ==
                   CF_OK);
    }
}

static void post_join(cf_request *req, const char *code, const char *body,
                      const char *cookie_header) {
    static char target[512];
    cf_test_req_init(req);
    req->method = CF_POST;
    req->original_method = CF_POST;
    snprintf(target, sizeof target, "/join/%s", code);
    req->path = SP(target);
    req->target = SP(target);
    req->body = SP(body);
    CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                  SP("application/x-www-form-urlencoded")) ==
               CF_OK);
    CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                  SP("same-origin")) == CF_OK);
    if (cookie_header != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie_header)) ==
                   CF_OK);
    }
}

static void get_user(cf_request *req, const char *id,
                     const char *cookie_header) {
    static char target[512];
    cf_test_req_init(req);
    snprintf(target, sizeof target, "/users/%s", id);
    req->path = SP(target);
    req->target = SP(target);
    if (cookie_header != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie_header)) ==
                   CF_OK);
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

/* The transfer token, copied out of the rendered page body: the transfer
 * partial links `/session/transfers/<signed id>`. */
static bool extract_transfer(const cf_buf *body, char *out, size_t cap) {
    static const char marker[] = "/session/transfers/";
    cf_span span = cf_buf_span(body);
    size_t mlen = sizeof marker - 1;
    for (size_t i = 0; i + mlen <= span.len; i++) {
        if (memcmp(span.ptr + i, marker, mlen) != 0) continue;
        size_t at = i + mlen;
        size_t end = at;
        while (end < span.len && span.ptr[end] != '"' &&
               span.ptr[end] != '\'' && span.ptr[end] != ' ' &&
               span.ptr[end] != '\r' && span.ptr[end] != '\n' &&
               span.ptr[end] != '<') {
            end++;
        }
        size_t copy = end - at;
        if (copy == 0 || copy >= cap) continue;
        memcpy(out, span.ptr + at, copy);
        out[copy] = '\0';
        return true;
    }
    return false;
}

/* --- acceptance ------------------------------------------------------------ */

CF_TEST(users_route_ids_bind_the_actions) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/join/:join_code", 50,
                                  cf_action_users_new) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("POST", "/join/:join_code", 51,
                                  cf_action_users_create) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("GET", "/users/:id", 74,
                                  cf_action_users_show) == CF_OK);
    CF_CHECK(cf_route_action(50) == cf_action_users_new);
    CF_CHECK(cf_route_action(51) == cf_action_users_create);
    CF_CHECK(cf_route_action(74) == cf_action_users_show);
}

CF_TEST(users_new_renders_the_join_page_with_the_code) {
    users_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada", "ada@example.com", NULL, 1, 0);

    cf_request req;
    cf_response resp;
    get_join(&req, JOIN_CODE, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    CF_CHECK(buf_contains(resp.body, JOIN_CODE));
    CF_CHECK(buf_contains(resp.body, "Ada"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_new_wrong_code_is_404_with_no_body) {
    users_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);

    cf_request req;
    cf_response resp;
    get_join(&req, "nope", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_new_without_account_is_500) {
    users_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    cf_response resp;
    get_join(&req, JOIN_CODE, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    /* Current.account.join_code on nil raises: the internal error. */
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_new_signed_in_redirects_to_root) {
    users_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada", "ada@example.com", NULL, 0, 0);
    char cookie[1024];
    make_session_cookie(&env, cookie, sizeof cookie, "tok-1", 1);

    cf_request req;
    cf_response resp;
    get_join(&req, JOIN_CODE, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, "Location: " ORIGIN "/\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_new_unacceptable_format_is_406) {
    users_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);

    cf_request req;
    cf_response resp;
    get_join(&req, JOIN_CODE, NULL);
    CF_REQUIRE(cf_test_req_header(&req, SP("Accept"), SP("application/xml")) ==
               CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_create_joins_and_signs_in_at_root) {
    users_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);

    cf_request req;
    cf_response resp;
    post_join(&req, JOIN_CODE,
              "user[name]=Jo&user[email_address]=jo%40example.com&"
              "user[password]=" PASSWORD,
              NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, "Location: " ORIGIN "/\r\n"));
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM users") == 1);
    /* The model default: member, active, with the email and a digest. */
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM users WHERE "
                                        "name='Jo' AND "
                                        "email_address='jo@example.com' AND "
                                        "role=0 AND status=0 AND "
                                        "password_digest IS NOT NULL") == 1);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM sessions") ==
             1);
    CF_CHECK(head_contains(&resp, &req, "Set-Cookie: session_token="));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_create_empty_password_stores_null) {
    users_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);

    cf_request req;
    cf_response resp;
    post_join(&req, JOIN_CODE,
              "user[name]=No&user[email_address]=no%40example.com&"
              "user[password]=",
              NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM users WHERE "
                                        "name='No' AND "
                                        "password_digest IS NULL") == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_create_duplicate_email_redirects_to_sign_in) {
    users_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Taken", "dupe@example.com", NULL, 0, 0);

    cf_request req;
    cf_response resp;
    post_join(&req, JOIN_CODE,
              "user[name]=Other&user[email_address]=dupe%40example.com&"
              "user[password]=" PASSWORD,
              NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN
                           "/session/new?email_address=dupe%40example.com"
                           "\r\n"));
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM users") == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_create_missing_user_is_400) {
    users_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);

    cf_request req;
    cf_response resp;
    post_join(&req, JOIN_CODE, "name=Jo", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    /* params.require(:user): ParameterMissing. */
    CF_CHECK(resp.status == 400);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM users") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_create_missing_name_is_500) {
    users_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);

    cf_request req;
    cf_response resp;
    post_join(&req, JOIN_CODE,
              "user[email_address]=x%40example.com&user[password]=" PASSWORD,
              NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    /* users.name NOT NULL: the reference's raised violation. */
    CF_CHECK(resp.status == 500);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM users") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_create_wrong_code_is_404_with_no_row) {
    users_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);

    cf_request req;
    cf_response resp;
    post_join(&req, "wrong", "user[name]=Jo", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM users") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_create_signed_in_redirects_to_root_with_no_row) {
    users_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada", "ada@example.com", NULL, 0, 0);
    char cookie[1024];
    make_session_cookie(&env, cookie, sizeof cookie, "tok-1", 1);

    cf_request req;
    cf_response resp;
    post_join(&req, JOIN_CODE, "user[name]=Jo", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, "Location: " ORIGIN "/\r\n"));
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM users") == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_show_renders_the_user_with_a_live_transfer_id) {
    users_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 5, "Zed", "zed@example.com", NULL, 1, 0);
    char cookie[1024];
    make_session_cookie(&env, cookie, sizeof cookie, "tok-5", 5);

    cf_request req;
    cf_response resp;
    get_user(&req, "5", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    CF_CHECK(buf_contains(resp.body, "Zed"));
    CF_CHECK(buf_contains(resp.body, "zed@example.com"));
    /* fresh_user_avatar_path: "/users/<avatar token>/avatar?v=<number>". */
    CF_CHECK(buf_contains(resp.body, "/users/"));
    CF_CHECK(buf_contains(resp.body, "/avatar?v=20260102030405"));

    /* The echoed transfer id verifies for this user (purpose transfer). */
    char token[1024];
    CF_REQUIRE(extract_transfer(resp.body, token, sizeof token));
    cf_optional_i64 id = {false, 0};
    bool found = false;
    CF_REQUIRE(cf_auth_signed_id_verify(
                   (cf_span){(const unsigned char *)env.config
                                 ->secret_key_base,
                             env.config->secret_key_base_len},
                   SP("User"),
                   (cf_span){(const unsigned char *)token, strlen(token)},
                   SP("transfer"), true, cf_now_us(env.app), &id,
                   &found) == CF_OK);
    CF_CHECK(found && id.present && id.value == 5);
    /* ... but not ten hours from now (the 4-hour expiry is wired). */
    CF_REQUIRE(cf_auth_signed_id_verify(
                   (cf_span){(const unsigned char *)env.config
                                 ->secret_key_base,
                             env.config->secret_key_base_len},
                   SP("User"),
                   (cf_span){(const unsigned char *)token, strlen(token)},
                   SP("transfer"), true,
                   cf_now_us(env.app) +
                       INT64_C(10) * INT64_C(3600) * INT64_C(1000000),
                   &id, &found) == CF_OK);
    CF_CHECK(!found);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_show_turbo_frame_renders_the_frame_layout) {
    users_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 5, "Zed", "zed@example.com", NULL, 1, 0);
    char cookie[1024];
    make_session_cookie(&env, cookie, sizeof cookie, "tok-5", 5);

    cf_request req;
    cf_response resp;
    get_user(&req, "5", cookie);
    CF_REQUIRE(cf_test_req_header(&req, SP("Turbo-Frame"), SP("main")) ==
               CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(buf_contains(resp.body, "Zed"));
    CF_CHECK(!head_contains(&resp, &req, "Link: "));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_show_unacceptable_format_is_406) {
    users_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 5, "Zed", "zed@example.com", NULL, 0, 0);
    char cookie[1024];
    make_session_cookie(&env, cookie, sizeof cookie, "tok-5", 5);

    cf_request req;
    cf_response resp;
    get_user(&req, "5", cookie);
    CF_REQUIRE(cf_test_req_header(&req, SP("Accept"), SP("application/xml")) ==
               CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_show_unknown_or_bad_id_is_404) {
    users_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 5, "Zed", "zed@example.com", NULL, 0, 0);
    char cookie[1024];
    make_session_cookie(&env, cookie, sizeof cookie, "tok-5", 5);

    const char *ids[] = {"4242", "abc"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        cf_request req;
        cf_response resp;
        get_user(&req, ids[i], cookie);
        CF_REQUIRE(run_request(&env, &req, &resp));
        if (resp.status != 404) {
            fprintf(stderr, "  id %s: status=%u\n", ids[i], resp.status);
        }
        CF_CHECK(resp.status == 404);
        cf_response_dispose(&resp);
    }
    env_close(&env);
}

CF_TEST(users_show_unauthenticated_redirects_to_sign_in) {
    users_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 5, "Zed", "zed@example.com", NULL, 0, 0);

    cf_request req;
    cf_response resp;
    get_user(&req, "5", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
