/* tests/actions/autocompletable_users_test.c — A-autocompletable-users
 * acceptance: `autocompletable/users#index` (route ID 75), per
 * docs/devel/implementation/contracts/controller-packets.md
 * "A-autocompletable-users".
 *
 * Cases:
 *   - route binding: id 75 resolves to the action;
 *   - JSON index: 200, application/json, the X-Total-Count header, active
 *     users ordered by LOWER(name) including bots (the pinned scope has no
 *     without_bots), banned users excluded; entries carry name (HTML
 *     escaped), value, absolute avatar_url and sgid;
 *   - filter=/query=: the mentions prompt (`filter`) and the autocomplete
 *     inputs (`query`) narrow to a LIKE substring; filter wins when both
 *     are present;
 *   - room_id: membership-scoped (members only, still active-only);
 *     an unreachable room and a non-castable id are 404;
 *   - pagination: per_page 20 (page 1 holds 20 of 26 with a next Link,
 *     page 2 holds the last 6 with no Link, X-Total-Count always the full
 *     count);
 *   - HTML: 200, text/html, the real prompt list with no layout Link
 *     header; an unacceptable format is 406;
 *   - unauthenticated GET is redirected to sign-in.
 *
 * The prompt-item template is wired in (src/views/users.c renders the real
 * mention items); the JSON shaping is the action's own faithful port
 * (verified field by field here).  The route double binds row 75 to the real action, so every
 * case runs the real A00 dispatch path (no writer use: read-only).
 *
 * cf.h and src/actions/actions.h are integrator-owned and do not yet declare
 * this packet's symbol, so the entry point is declared here; the
 * integrator's routes.c rebind needs the same declaration (c_symbol
 * cf_action_autocompletable_users_index).
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "cf.h"
#include "config.h"

#include "context.h"
#include "views.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "http/http_internal.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

cf_err cf_action_autocompletable_users_index(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* ---- scratch app ------------------------------------------------------------ */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
} auto_env;

static bool env_open(auto_env *env) {
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
    /* Real renders resolve digested assets through the pinned manifest. */
    if (cf_views_assets_configure("tests/fixtures/assets") != CF_OK) {
        fprintf(stderr, "  env_open: assets configure failed\n");
        return false;
    }
    if (cf_writer_start(env->app, env->config) != CF_OK) return false;
    env->writer_started = true;
    cf_test_routes_reset();
    if (cf_test_routes_add("GET", "/autocompletable/users(.:format)", 75,
                           cf_action_autocompletable_users_index) != CF_OK ||
        cf_test_routes_add("GET", "/autocompletable/users", 75,
                           cf_action_autocompletable_users_index) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(auto_env *env) {
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

static void seed_user(cf_db *db, int64_t id, const char *name, int role,
                      int status) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, updated_at) "
             "VALUES (%lld, NULL, NULL, '2026-01-02 03:04:05', NULL, '%s', "
             "NULL, %d, %d, '2026-01-02 03:04:05')",
             (long long)id, name, role, status);
    exec_sql(db, sql);
}

static void seed_state(auto_env *env) {
    cf_db *db = env->scratch.db;
    /* Current user (1, "mallory"), Alice (2), bob (3), banned (4, in the
     * room to prove the active filter applies inside the scope), Zoe (5,
     * outside the room), Botty (27, active bot: the scope keeps bots). */
    seed_user(db, 1, "mallory", 0, 0);
    seed_user(db, 2, "Alice", 0, 0);
    seed_user(db, 3, "bob", 0, 0);
    seed_user(db, 4, "Banned", 0, 2);
    seed_user(db, 5, "Zoe", 0, 0);
    seed_user(db, 27, "Botty", 2, 0);
    char name[32];
    for (int64_t id = 6; id <= 26; id++) {
        snprintf(name, sizeof name, "Bulk%02lld", (long long)id);
        seed_user(db, id, name, 0, 0);
    }
    exec_sql(db,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (1, '2026-01-02 03:04:05', 1, 'HQ', "
             "'Rooms::Open', '2026-01-02 03:04:05')");
    exec_sql(db,
             "INSERT INTO memberships (id, created_at, room_id, updated_at, "
             "user_id) VALUES (1, '2026-01-02 03:04:05', 1, "
             "'2026-01-02 03:04:05', 1)");
    exec_sql(db,
             "INSERT INTO memberships (id, created_at, room_id, updated_at, "
             "user_id) VALUES (2, '2026-01-02 03:04:05', 1, "
             "'2026-01-02 03:04:05', 2)");
    exec_sql(db,
             "INSERT INTO memberships (id, created_at, room_id, updated_at, "
             "user_id) VALUES (3, '2026-01-02 03:04:05', 1, "
             "'2026-01-02 03:04:05', 3)");
    exec_sql(db,
             "INSERT INTO memberships (id, created_at, room_id, updated_at, "
             "user_id) VALUES (4, '2026-01-02 03:04:05', 1, "
             "'2026-01-02 03:04:05', 4)");
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

static void make_session_cookie(auto_env *env, char *out, size_t cap,
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

static bool run_request(auto_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void get_index(cf_request *req, const char *target,
                      const char *accept, const char *cookie_header) {
    cf_test_req_init(req);
    req->path = SP("/autocompletable/users");
    req->target = SP(target);
    const char *mark = strchr(target, '?');
    if (mark != NULL) {
        req->query =
            (cf_span){(const unsigned char *)(mark + 1), strlen(mark + 1)};
    }
    if (accept != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Accept"), SP(accept)) ==
                   CF_OK);
    }
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

/* Offset of needle in buf, or SIZE_MAX. */
static size_t buf_offset(const cf_buf *buf, const char *needle) {
    size_t len = strlen(needle);
    cf_span span = cf_buf_span(buf);
    if (span.len < len) return SIZE_MAX;
    for (size_t i = 0; i + len <= span.len; i++) {
        if (memcmp(span.ptr + i, needle, len) == 0) return i;
    }
    return SIZE_MAX;
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

/* --- acceptance ------------------------------------------------------------ */

CF_TEST(autocomplete_route_id_binds_the_action) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/autocompletable/users", 75,
                                  cf_action_autocompletable_users_index) ==
               CF_OK);
    CF_CHECK(cf_route_action(75) == cf_action_autocompletable_users_index);
}

CF_TEST(autocomplete_json_lists_active_users_ordered) {
    auto_env env;
    CF_REQUIRE(env_open(&env));
    seed_state(&env);
    char cookie[1024];
    make_session_cookie(&env, cookie, sizeof cookie, "tok-1", 1);

    cf_request req;
    cf_response resp;
    get_index(&req, "/autocompletable/users?page=2", "application/json",
              cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: application/json; charset=utf-8"
                           "\r\n"));
    /* 26 active users in all (bots included, banned excluded). */
    CF_CHECK(head_contains(&resp, &req, "X-Total-Count: 26\r\n"));
    /* Page 2 holds the last 6: no next link. */
    CF_CHECK(!head_contains(&resp, &req, "rel=\"next\""));
    CF_CHECK(buf_contains(resp.body, "\"name\":\"mallory\""));
    CF_CHECK(buf_contains(resp.body, "\"name\":\"Zoe\""));
    CF_CHECK(!buf_contains(resp.body, "Banned"));
    /* Entry shape: value, absolute avatar_url, sgid. */
    CF_CHECK(buf_contains(resp.body, "\"value\":2"));
    CF_CHECK(buf_contains(resp.body,
                           "\"avatar_url\":\"" ORIGIN "/users/"));
    CF_CHECK(buf_contains(resp.body, "\"sgid\":\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(autocomplete_json_first_page_links_next_and_orders_names) {
    auto_env env;
    CF_REQUIRE(env_open(&env));
    seed_state(&env);
    char cookie[1024];
    make_session_cookie(&env, cookie, sizeof cookie, "tok-1", 1);

    cf_request req;
    cf_response resp;
    get_index(&req, "/autocompletable/users", "application/json", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req, "X-Total-Count: 26\r\n"));
    CF_CHECK(head_contains(&resp, &req, "rel=\"next\""));
    CF_CHECK(head_contains(&resp, &req, "page=2"));
    /* ORDER BY LOWER(name): Alice, bob, Botty, Bulk.., mallory, Zoe. */
    size_t alice = buf_offset(resp.body, "\"name\":\"Alice\"");
    size_t bob = buf_offset(resp.body, "\"name\":\"bob\"");
    size_t botty = buf_offset(resp.body, "\"name\":\"Botty\"");
    size_t mallory = buf_offset(resp.body, "\"name\":\"mallory\"");
    CF_CHECK(alice < bob && bob < botty && botty < mallory);
    CF_CHECK(!buf_contains(resp.body, "\"name\":\"Zoe\"")); /* page 2 */
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(autocomplete_filter_narrows_to_a_substring) {
    auto_env env;
    CF_REQUIRE(env_open(&env));
    seed_state(&env);
    char cookie[1024];
    make_session_cookie(&env, cookie, sizeof cookie, "tok-1", 1);

    cf_request req;
    cf_response resp;
    get_index(&req, "/autocompletable/users?filter=ali", "application/json",
              cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req, "X-Total-Count: 1\r\n"));
    CF_CHECK(buf_contains(resp.body, "\"name\":\"Alice\""));
    CF_CHECK(!buf_contains(resp.body, "\"name\":\"bob\""));
    cf_response_dispose(&resp);

    /* The autocomplete inputs filter with `query`; filter wins. */
    get_index(&req, "/autocompletable/users?query=zo", "application/json",
              cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(buf_contains(resp.body, "\"name\":\"Zoe\""));
    cf_response_dispose(&resp);

    get_index(&req, "/autocompletable/users?filter=ali&query=zo",
              "application/json", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req, "X-Total-Count: 1\r\n"));
    CF_CHECK(buf_contains(resp.body, "\"name\":\"Alice\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(autocomplete_room_scope_lists_members_only) {
    auto_env env;
    CF_REQUIRE(env_open(&env));
    seed_state(&env);
    char cookie[1024];
    make_session_cookie(&env, cookie, sizeof cookie, "tok-1", 1);

    cf_request req;
    cf_response resp;
    get_index(&req, "/autocompletable/users?room_id=1", "application/json",
              cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    /* Members 1/2/3 (the banned member 4 is filtered, Zoe is outside). */
    CF_CHECK(head_contains(&resp, &req, "X-Total-Count: 3\r\n"));
    CF_CHECK(buf_contains(resp.body, "\"name\":\"Alice\""));
    CF_CHECK(buf_contains(resp.body, "\"name\":\"bob\""));
    CF_CHECK(buf_contains(resp.body, "\"name\":\"mallory\""));
    CF_CHECK(!buf_contains(resp.body, "Zoe"));
    CF_CHECK(!buf_contains(resp.body, "Banned"));
    cf_response_dispose(&resp);

    /* The room filter composes with the query. */
    get_index(&req, "/autocompletable/users?room_id=1&filter=bo",
              "application/json", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req, "X-Total-Count: 1\r\n"));
    CF_CHECK(buf_contains(resp.body, "\"name\":\"bob\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(autocomplete_bad_room_is_404) {
    auto_env env;
    CF_REQUIRE(env_open(&env));
    seed_state(&env);
    char cookie[1024];
    make_session_cookie(&env, cookie, sizeof cookie, "tok-1", 1);

    /* Zoe (5) has no room 999 membership: rooms.find raises. */
    const char *targets[] = {
        "/autocompletable/users?room_id=999",
        "/autocompletable/users?room_id=abc",
    };
    for (size_t i = 0; i < sizeof targets / sizeof targets[0]; i++) {
        cf_request req;
        cf_response resp;
        get_index(&req, targets[i], "application/json", cookie);
        CF_REQUIRE(run_request(&env, &req, &resp));
        if (resp.status != 404) {
            fprintf(stderr, "  %s: status=%u\n", targets[i], resp.status);
        }
        CF_CHECK(resp.status == 404);
        cf_response_dispose(&resp);
    }
    env_close(&env);
}

CF_TEST(autocomplete_html_renders_the_prompt_list) {
    auto_env env;
    CF_REQUIRE(env_open(&env));
    seed_state(&env);
    char cookie[1024];
    make_session_cookie(&env, cookie, sizeof cookie, "tok-1", 1);

    cf_request req;
    cf_response resp;
    get_index(&req, "/autocompletable/users", NULL, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    /* layout: false — the real prompt items, and no preload Link header. */
    CF_CHECK(buf_contains(resp.body, "Alice"));
    CF_CHECK(buf_contains(resp.body, "lexxy-prompt-item"));
    CF_CHECK(!head_contains(&resp, &req, "Link: "));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(autocomplete_unacceptable_format_is_406) {
    auto_env env;
    CF_REQUIRE(env_open(&env));
    seed_state(&env);
    char cookie[1024];
    make_session_cookie(&env, cookie, sizeof cookie, "tok-1", 1);

    cf_request req;
    cf_response resp;
    get_index(&req, "/autocompletable/users", "application/xml", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(autocomplete_unauthenticated_redirects_to_sign_in) {
    auto_env env;
    CF_REQUIRE(env_open(&env));
    seed_state(&env);

    cf_request req;
    cf_response resp;
    get_index(&req, "/autocompletable/users", "application/json", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
