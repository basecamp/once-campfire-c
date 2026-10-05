/* tests/actions/searches_test.c — A-searches acceptance: `searches#index`
 * (route ID 146), `#create` (147) and `#clear` (145), per
 * docs/devel/implementation/contracts/controller-packets.md "A-searches" and
 * routes.json rows 145/146/147.
 *
 * Every case runs the real A00 dispatch path (merged params, cookies,
 * before-actions, CSRF, format negotiation) through the H03 route double
 * (tests/app/support/route_double.c), which binds the three route-ID rows to
 * the real actions.  Cases: route binding; the unauthenticated redirect;
 * index over a seeded database (the benchmark preflight's
 * `GET /searches?q=coffee` shape, scoping to accessible rooms, blank and
 * non-string q, conditional HTML negotiation, the frame layout, recents and
 * the room exit); create's record side effect and redirect; create's
 * NOT-NULL 500 without q; clear's destroy-all for the current user only.
 *
 * The view-level golden comparison of `searches_index` /
 * `searches_index_empty` lives in tests/views/test_searches.c, beside the
 * Rust runner's own searches cases (crates/views/tests/searches_views.rs).
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "http/http_internal.h"
#include "models/search.h"
#include "models/user.h"
#include "richtext.h"
#include "views.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* cf.h is frozen and no packet-action header exists yet; the integrator's
 * routes.c rebind needs the same declarations. */
cf_err cf_action_searches_clear(cf_ctx *ctx);
cf_err cf_action_searches_index(cf_ctx *ctx);
cf_err cf_action_searches_create(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define SEARCH_SECRET \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define DAVID INT64_C(127326141)
#define KEVIN INT64_C(712064548)
#define COFFEE_ROOM INT64_C(654632876)
#define SECRET_ROOM INT64_C(654632877)
#define LOUNGE_ROOM INT64_C(654632878)
#define COFFEE_MESSAGE INT64_C(309456473)
#define SECRET_MESSAGE INT64_C(309456474)

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- scratch app + database ------------------------------------------------ */

typedef struct {
    cf_db_scratch scratch;
    cf_app *app;
    int session_seq;
} searches_env;

static bool env_open(searches_env *env) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) {
        fprintf(stderr, "  env_open: scratch open failed\n");
        return false;
    }
    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", ORIGIN},
        {"SECRET_KEY_BASE", SEARCH_SECRET},
        {"DATABASE_PATH", env->scratch.path},
        {"STORAGE_PATH", "storage/files"},
    };
    cf_config *config = NULL;
    if (cf_config_parse(entries, 4, NULL, &config) != CF_OK) {
        fprintf(stderr, "  env_open: config parse failed\n");
        cf_db_scratch_close(&env->scratch);
        return false;
    }
    if (cf_views_assets_configure("tests/fixtures/assets") != CF_OK) {
        fprintf(stderr, "  env_open: assets configure failed\n");
        cf_config_destroy(config);
        cf_db_scratch_close(&env->scratch);
        return false;
    }
    cf_richtext_configure(SP(SEARCH_SECRET));
    if (cf_app_create(config, &env->app) != CF_OK) {
        fprintf(stderr, "  env_open: app create failed\n");
        cf_config_destroy(config);
        cf_db_scratch_close(&env->scratch);
        return false;
    }
    if (cf_writer_start(env->app, cf_app_config(env->app)) != CF_OK) {
        fprintf(stderr, "  env_open: writer start failed\n");
        cf_app_destroy(env->app);
        env->app = NULL;
        cf_db_scratch_close(&env->scratch);
        return false;
    }
    cf_test_routes_reset();
    /* The double needs an extension for a literal last segment, so the bare
     * forms are registered alongside the (.:format) rows. */
    bool ok =
        cf_test_routes_add("GET", "/searches(.:format)", 146,
                           cf_action_searches_index) == CF_OK &&
        cf_test_routes_add("GET", "/searches", 146,
                           cf_action_searches_index) == CF_OK &&
        cf_test_routes_add("POST", "/searches(.:format)", 147,
                           cf_action_searches_create) == CF_OK &&
        cf_test_routes_add("POST", "/searches", 147,
                           cf_action_searches_create) == CF_OK &&
        cf_test_routes_add("DELETE", "/searches/clear(.:format)", 145,
                           cf_action_searches_clear) == CF_OK &&
        cf_test_routes_add("DELETE", "/searches/clear", 145,
                           cf_action_searches_clear) == CF_OK;
    if (!ok) {
        fprintf(stderr, "  env_open: route registration failed\n");
        cf_writer_stop(env->app);
        cf_app_destroy(env->app);
        env->app = NULL;
        cf_db_scratch_close(&env->scratch);
        return false;
    }
    return true;
}

static void env_close(searches_env *env) {
    if (env->app != NULL) {
        cf_writer_stop(env->app);
        cf_app_destroy(env->app);
    }
    env->app = NULL;
    cf_db_scratch_close(&env->scratch);
    memset(env, 0, sizeof *env);
}

/* --- SQL helpers ----------------------------------------------------------- */

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

/* The first column of the first row as owned text ("" when no row). */
static bool one_text(cf_db *db, const char *sql, char *out, size_t cap) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) !=
        SQLITE_OK) {
        return false;
    }
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    if (found) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        snprintf(out, cap, "%s", text != NULL ? (const char *)text : "");
    } else {
        out[0] = '\0';
    }
    sqlite3_finalize(stmt);
    return found;
}

static void seed_account(cf_db *db) {
    exec_sql(db,
             "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
             "name, settings, singleton_guard, updated_at) VALUES "
             "(873240054, '2026-09-26 13:03:57.000000', NULL, "
             "'CRMu-l8Ge-KB9B', '37signals', NULL, 0, "
             "'2026-09-26 13:03:57.000000')");
}

static void seed_user(cf_db *db, int64_t id, const char *name, int role) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, created_at, email_address, name, "
             "role, status, updated_at) VALUES (%lld, NULL, "
             "'2026-09-26 13:03:57.000000', NULL, '%s', %d, 0, "
             "'2026-09-26 13:03:57.000000')",
             (long long)id, name, role);
    exec_sql(db, sql);
}

static void seed_room(cf_db *db, int64_t id, const char *name,
                      int64_t creator, const char *created_at) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (%lld, '%s', %lld, "
             "'%s', 'Rooms::Closed', '2026-09-26 13:03:59.000000')",
             (long long)id, created_at, (long long)creator, name);
    exec_sql(db, sql);
}

static void seed_membership(cf_db *db, int64_t id, int64_t room_id,
                            int64_t user_id) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO memberships (id, created_at, room_id, updated_at, "
             "user_id) VALUES (%lld, '2026-09-26 12:00:00.000000', %lld, "
             "'2026-09-26 12:00:00.000000', %lld)",
             (long long)id, (long long)room_id, (long long)user_id);
    exec_sql(db, sql);
}

static void seed_message(cf_db *db, int64_t id, int64_t room_id,
                         int64_t creator, const char *client,
                         const char *body) {
    char sql[2048];
    snprintf(sql, sizeof sql,
             "INSERT INTO messages (id, client_message_id, created_at, "
             "creator_id, room_id, updated_at) VALUES (%lld, '%s', "
             "'2026-09-26 13:01:07.148296', %lld, %lld, "
             "'2026-09-26 13:01:07.148296')",
             (long long)id, client, (long long)creator, (long long)room_id);
    exec_sql(db, sql);
    snprintf(sql, sizeof sql,
             "INSERT INTO action_text_rich_texts (id, body, created_at, "
             "name, record_id, record_type, updated_at) VALUES (%lld, '%s', "
             "'2026-09-26 13:01:07.148296', 'body', %lld, 'Message', "
             "'2026-09-26 13:01:07.148296')",
             (long long)id, body, (long long)id);
    exec_sql(db, sql);
    snprintf(sql, sizeof sql,
             "INSERT INTO message_search_index (rowid, body) VALUES (%lld, "
             "'%s')",
             (long long)id, body);
    exec_sql(db, sql);
}

static void seed_search(cf_db *db, int64_t id, int64_t user_id,
                        const char *query, const char *updated_at) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO searches (id, created_at, query, updated_at, "
             "user_id) VALUES (%lld, '%s', '%s', '%s', %lld)",
             (long long)id, updated_at, query, updated_at,
             (long long)user_id);
    exec_sql(db, sql);
}

/* Account, David (administrator) and Kevin; David is in Coffee Club (with
 * both messages), Kevin is not a member of Coffee Club but is in Secret.  Two
 * searchable messages both match "coffee", one per room, so index's room
 * scoping is observable. */
static void seed_world(searches_env *env) {
    cf_db *db = env->scratch.db;
    seed_account(db);
    seed_user(db, DAVID, "David", 1);
    seed_user(db, KEVIN, "Kevin", 0);
    seed_room(db, COFFEE_ROOM, "Coffee Club", KEVIN,
              "2026-09-26 12:00:00.000000");
    seed_room(db, SECRET_ROOM, "Secret", KEVIN,
              "2026-09-26 12:00:00.000000");
    /* A later room for the `last_room` cookie (David's original room stays
     * Coffee Club: it is older). */
    seed_room(db, LOUNGE_ROOM, "Lounge", KEVIN,
              "2026-09-26 12:30:00.000000");
    seed_membership(db, 1, COFFEE_ROOM, DAVID);
    seed_membership(db, 2, COFFEE_ROOM, KEVIN);
    seed_membership(db, 3, SECRET_ROOM, KEVIN);
    seed_membership(db, 4, LOUNGE_ROOM, DAVID);
    seed_message(db, COFFEE_MESSAGE, COFFEE_ROOM, KEVIN, "c1",
                 "Coffee time");
    seed_message(db, SECRET_MESSAGE, SECRET_ROOM, KEVIN, "c2",
                 "Coffee secrets");
}

/* --- sessions and requests ------------------------------------------------- */

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

static void make_session_cookie(searches_env *env, int64_t user_id, char *out,
                                size_t cap) {
    char token[64];
    snprintf(token, sizeof token, "session-%lld-%d", (long long)user_id,
             env->session_seq++);
    seed_session(env->scratch.db, token, user_id);
    const cf_config *config = cf_app_config(env->app);
    cf_str wire = {0};
    CF_REQUIRE(cf_auth_signed_cookie_generate(
                   SP(config->secret_key_base), SP("session_token"), SP(token),
                   false, 0, &wire) == CF_OK);
    size_t at = (size_t)snprintf(out, cap, "session_token=");
    for (size_t i = 0; i < wire.len && at + 4 < cap; i++) {
        unsigned char c = (unsigned char)wire.ptr[i];
        if (c == '+' || c == '/' || c == '=') {
            at += (size_t)snprintf(out + at, cap - at, "%%%02X", c);
        } else {
            out[at++] = (char)c;
        }
    }
    out[at] = '\0';
    cf_str_dispose(&wire);
}

static void append_cookie(char *out, size_t cap, const char *pair) {
    size_t at = strlen(out);
    snprintf(out + at, cap - at, "; %s", pair);
}

static bool run_request(searches_env *env, cf_request *req,
                        cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void req_get(cf_request *req, const char *path, const char *query,
                    const char *cookie, const char *accept,
                    const char *turbo_frame) {
    static char target[1024];
    cf_test_req_init(req);
    req->path = SP(path);
    if (query != NULL && query[0] != '\0') {
        snprintf(target, sizeof target, "%s?%s", path, query);
        req->target = SP(target);
        req->query = SP(target + strlen(path) + 1);
    } else {
        req->target = SP(path);
    }
    if (cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    }
    if (accept != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Accept"), SP(accept)) == CF_OK);
    }
    if (turbo_frame != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Turbo-Frame"),
                                      SP(turbo_frame)) == CF_OK);
    }
}

static void req_post(cf_request *req, const char *path, const char *body,
                     const char *cookie) {
    cf_test_req_init(req);
    req->method = CF_POST;
    req->original_method = CF_POST;
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

static void req_delete(cf_request *req, const char *path, const char *query,
                       const char *cookie) {
    static char target[1024];
    cf_test_req_init(req);
    req->method = CF_DELETE;
    req->original_method = CF_DELETE;
    req->path = SP(path);
    if (query != NULL && query[0] != '\0') {
        snprintf(target, sizeof target, "%s?%s", path, query);
        req->target = SP(target);
        req->query = SP(target + strlen(path) + 1);
    } else {
        req->target = SP(path);
    }
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
    return resp->body != NULL &&
           span_contains(cf_buf_span(resp->body), needle);
}

/* The raw value after `"<name>: "` in the serialized response headers. */
static bool header_value(cf_response *resp, cf_request *req, const char *name,
                         char *out, size_t cap) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    cf_span head = cf_buf_span(ser.headers);
    size_t len = strlen(name);
    bool found = false;
    for (size_t i = 0; i + len <= head.len; i++) {
        if (memcmp(head.ptr + i, name, len) != 0) continue;
        size_t at = i + len, end = at;
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

/* --- route IDs ------------------------------------------------------------- */

CF_TEST(searches_route_ids_bind_the_actions) {
    searches_env env;
    CF_REQUIRE(env_open(&env));
    CF_CHECK(cf_route_action(145) == cf_action_searches_clear);
    CF_CHECK(cf_route_action(146) == cf_action_searches_index);
    CF_CHECK(cf_route_action(147) == cf_action_searches_create);
    env_close(&env);
}

/* --- authorization --------------------------------------------------------- */

CF_TEST(searches_unauthenticated_requests_redirect_to_sign_in) {
    searches_env env;
    CF_REQUIRE(env_open(&env));
    seed_world(&env);
    cf_request req;
    cf_response resp;

    req_get(&req, "/searches", NULL, NULL, NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    char location[256];
    CF_CHECK(header_value(&resp, &req, "Location: ", location,
                          sizeof location));
    CF_CHECK(strcmp(location, ORIGIN "/session/new") == 0);
    cf_response_dispose(&resp);

    req_get(&req, "/searches", "q=coffee", NULL, NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);

    req_post(&req, "/searches", "q=coffee", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);

    req_delete(&req, "/searches/clear", NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);

    CF_CHECK(count_rows(env.scratch.db, "SELECT COUNT(*) FROM searches") == 0);
    env_close(&env);
}

/* --- index ----------------------------------------------------------------- */

CF_TEST(searches_index_renders_query_results_recents_and_the_exit) {
    searches_env env;
    CF_REQUIRE(env_open(&env));
    seed_world(&env);
    seed_search(env.scratch.db, 1, DAVID, "pizza & \"pie\"",
                "2026-09-26 12:00:00.000000");
    seed_search(env.scratch.db, 2, DAVID, "post", "2026-09-26 13:00:00.000000");
    char cookie[1024];
    make_session_cookie(&env, DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_get(&req, "/searches", "q=coffee", cookie, NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    /* The benchmark preflight's shape: a populated `data-message-id`. */
    CF_CHECK(body_contains(&resp, "data-message-id=\"309456473\""));
    CF_CHECK(body_contains(&resp, "Coffee time"));
    CF_CHECK(body_contains(&resp, "id=\"search-results\""));
    /* The query chip (sanitized query and result count). */
    CF_CHECK(body_contains(&resp, "searches__query"));
    CF_CHECK(body_contains(&resp, "flex-item-no-shrink\">1<"));
    /* The recents, newest first, in nav and sidebar (twice each). */
    CF_CHECK(body_contains(&resp, "href=\"/searches?q=post\""));
    CF_CHECK(body_contains(&resp, "href=\"/searches?q=pizza+%26+%22pie%22\""));
    /* The exit button goes to `last_room_visited` (the original room when no
     * cookie names one). */
    CF_CHECK(body_contains(&resp, "href=\"/rooms/654632876\""));
    CF_CHECK(body_contains(&resp, "aria-label=\"search\""));
    /* Layout#page appends the stylesheet preload links (a frame carries
     * none). */
    char link[512];
    CF_CHECK(header_value(&resp, &req, "Link: ", link, sizeof link));
    cf_response_dispose(&resp);

    /* A last_room cookie naming a room the user is in wins; one naming a
     * room they cannot reach falls back to the original room. */
    char lounge_cookie[1200];
    make_session_cookie(&env, DAVID, lounge_cookie, sizeof lounge_cookie);
    append_cookie(lounge_cookie, sizeof lounge_cookie, "last_room=654632878");
    req_get(&req, "/searches", NULL, lounge_cookie, NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "href=\"/rooms/654632878\""));
    cf_response_dispose(&resp);

    char secret_cookie[1200];
    make_session_cookie(&env, DAVID, secret_cookie, sizeof secret_cookie);
    append_cookie(secret_cookie, sizeof secret_cookie, "last_room=654632877");
    req_get(&req, "/searches", NULL, secret_cookie, NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "href=\"/rooms/654632876\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A query that reaches only one of the two rooms: the inaccessible message
 * neither renders nor is counted. */
CF_TEST(searches_index_scopes_results_to_accessible_rooms) {
    searches_env env;
    CF_REQUIRE(env_open(&env));
    seed_world(&env);
    char cookie[1024];
    make_session_cookie(&env, DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_get(&req, "/searches", "q=coffee", cookie, NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "data-message-id=\"309456473\""));
    CF_CHECK(!body_contains(&resp, "data-message-id=\"309456474\""));
    CF_CHECK(!body_contains(&resp, "Coffee secrets"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* No query, a blank query and a non-string query. */
CF_TEST(searches_index_without_a_query_does_not_search) {
    searches_env env;
    CF_REQUIRE(env_open(&env));
    seed_world(&env);
    char cookie[1024];
    make_session_cookie(&env, DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_get(&req, "/searches", NULL, cookie, NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "<title>Search</title>"));
    CF_CHECK(!body_contains(&resp, "data-message-id="));
    CF_CHECK(!body_contains(&resp, "searches__query"));
    cf_response_dispose(&resp);

    /* `q` is all whitespace: sanitized to spaces, `is_present` false, so no
     * search — but it is still the submitted field value. */
    req_get(&req, "/searches", "q=+%20", cookie, NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(!body_contains(&resp, "data-message-id="));
    CF_CHECK(body_contains(&resp, "searches__query"));
    CF_CHECK(body_contains(&resp, "value=\"  \""));
    cf_response_dispose(&resp);

    /* `q[]=1`: a non-string param makes `gsub` raise (500). */
    req_get(&req, "/searches", "q%5B%5D=1", cookie, NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(searches_index_negotiates_html_and_the_frame_layout) {
    searches_env env;
    CF_REQUIRE(env_open(&env));
    seed_world(&env);
    char cookie[1024];
    make_session_cookie(&env, DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_get(&req, "/searches", "q=coffee", cookie, "application/json", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);

    req_get(&req, "/searches", "q=coffee", cookie, NULL, "searches");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "id=\"search-results\""));
    CF_CHECK(body_contains(&resp, "data-message-id=\"309456473\""));
    CF_CHECK(!body_contains(&resp, "<nav id=\"nav\""));
    CF_CHECK(!body_contains(&resp, "<aside id=\"sidebar\""));
    char link[512];
    CF_CHECK(!header_value(&resp, &req, "Link: ", link, sizeof link));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- create ---------------------------------------------------------------- */

CF_TEST(searches_create_records_the_sanitized_query_and_redirects) {
    searches_env env;
    CF_REQUIRE(env_open(&env));
    seed_world(&env);
    char cookie[1024];
    make_session_cookie(&env, DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_post(&req, "/searches", "q=hello%2C+world", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    char location[256];
    CF_CHECK(header_value(&resp, &req, "Location: ", location,
                          sizeof location));
    CF_CHECK(strcmp(location,
                    ORIGIN "/searches?q=hello++world") == 0);
    cf_response_dispose(&resp);

    char query[128];
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT query FROM searches WHERE user_id=127326141",
                      query, sizeof query));
    CF_CHECK(strcmp(query, "hello  world") == 0);

    /* Recording the same query again is find + touch: one row. */
    req_post(&req, "/searches", "q=hello%2C+world", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT COUNT(*) FROM searches WHERE user_id=127326141") ==
             1);

    /* A blank q is still recorded (`query` is Some("  ")) even though
     * set_messages finds nothing to search for. */
    req_post(&req, "/searches", "q=+%20", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(header_value(&resp, &req, "Location: ", location,
                          sizeof location));
    CF_CHECK(strcmp(location, ORIGIN "/searches?q=++") == 0);
    cf_response_dispose(&resp);
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT query FROM searches WHERE user_id=127326141 "
                      "AND query='  '",
                      query, sizeof query));
    CF_CHECK(strcmp(query, "  ") == 0);
    env_close(&env);
}

CF_TEST(searches_create_without_q_is_500_and_writes_nothing) {
    searches_env env;
    CF_REQUIRE(env_open(&env));
    seed_world(&env);
    char cookie[1024];
    make_session_cookie(&env, DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_post(&req, "/searches", "", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db, "SELECT COUNT(*) FROM searches") == 0);

    /* A non-string q makes `query_param` fail before anything is written. */
    req_post(&req, "/searches", "q%5B%5D=1", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db, "SELECT COUNT(*) FROM searches") == 0);
    env_close(&env);
}

/* --- clear ----------------------------------------------------------------- */

CF_TEST(searches_clear_destroys_only_the_current_users_recents) {
    searches_env env;
    CF_REQUIRE(env_open(&env));
    seed_world(&env);
    seed_search(env.scratch.db, 1, DAVID, "post",
                "2026-09-26 12:00:00.000000");
    seed_search(env.scratch.db, 2, KEVIN, "coffee",
                "2026-09-26 12:00:00.000000");
    char cookie[1024];
    make_session_cookie(&env, DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_delete(&req, "/searches/clear", NULL, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    char location[256];
    CF_CHECK(header_value(&resp, &req, "Location: ", location,
                          sizeof location));
    CF_CHECK(strcmp(location, ORIGIN "/searches") == 0);
    cf_response_dispose(&resp);

    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT COUNT(*) FROM searches WHERE user_id=127326141") ==
             0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT COUNT(*) FROM searches WHERE user_id=712064548") ==
             1);

    /* clear reads q too (the same before-action as index/create): a query
     * runs set_messages first, and a non-string q is the gsub 500 with
     * nothing destroyed. */
    seed_search(env.scratch.db, 3, DAVID, "coffee",
                "2026-09-26 12:00:00.000000");
    req_delete(&req, "/searches/clear", "q=coffee", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT COUNT(*) FROM searches WHERE user_id=127326141") ==
             0);

    seed_search(env.scratch.db, 4, DAVID, "coffee",
                "2026-09-26 12:00:00.000000");
    req_delete(&req, "/searches/clear", "q%5B%5D=1", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT COUNT(*) FROM searches WHERE user_id=127326141") ==
             1);
    env_close(&env);
}

CF_TEST_MAIN()
