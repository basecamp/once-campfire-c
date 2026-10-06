/* tests/actions/rooms_involvements_test.c — A-rooms-involvements
 * acceptance: `rooms/involvements#show` (route ID 93) and
 * `rooms/involvements#update` (route IDs 94, 95)
 * (docs/devel/implementation/contracts/controller-packets.md
 * "A-rooms-involvements").
 *
 * Every case runs the real A00 dispatch path (params, cookies,
 * before-actions, format negotiation) through the route double
 * (tests/app/support/route_double.c), which binds rows 93/94/95 to the two
 * actions.
 *
 * Matrix from the card: the authenticated success paths (show rendering the
 * room kind and membership involvement; update storing blank-as-nil and
 * validated values, redirecting to the room involvement URL, and emitting
 * the involvement_change broadcast), unauthorized/forbidden/not-found
 * cases, parameter failures (unknown value, hash/array), mutation
 * state assertions, and route-ID binding including the PATCH/PUT update
 * alias.  The update-from-invisible prepend branch asserts the production
 * shared-room partial (V02 wired cf_view_rooms_shared_room_partial into
 * cf_broadcast_partials_views, so the branch no longer fails after commit).
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "cable/channels.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "http/http_internal.h"
#include "models/membership.h"
#include "models/room.h"
#include "models/user.h"
#include "views.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"
#include "../cable/cable_testutil.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ORIGIN "http://campfire.test"

/* routes.json rows 93, 94, 95.  cf.h is frozen and no packet-action header
 * exists yet, so the test declares them (the integrator's routes.c rebind
 * needs the same declarations). */
cf_err cf_action_rooms_involvements_show(cf_ctx *ctx);
cf_err cf_action_rooms_involvements_update(cf_ctx *ctx);

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- scratch app + database ------------------------------------------------ */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    cf_cable *cable;
    unsigned session_seq;
} inv_env;

static bool env_open(inv_env *env) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) {
        fprintf(stderr, "  env_open: scratch open failed\n");
        return false;
    }
    cf_config_entry entries[3] = {
        {"PUBLIC_ORIGIN", ORIGIN},
        {"SECRET_KEY_BASE",
         "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
        {"DATABASE_PATH", env->scratch.path},
    };
    if (cf_config_parse(entries, 3, NULL, &env->config) != CF_OK) {
        fprintf(stderr, "  env_open: config parse failed\n");
        return false;
    }
    if (cf_views_assets_configure("tests/fixtures/assets") != CF_OK) {
        fprintf(stderr, "  env_open: assets configure failed\n");
        cf_config_destroy(env->config);
        env->config = NULL;
        cf_db_scratch_close(&env->scratch);
        return false;
    }
    if (cf_app_create(env->config, &env->app) != CF_OK) {
        fprintf(stderr, "  env_open: app create failed\n");
        cf_config_destroy(env->config);
        env->config = NULL;
        return false;
    }
    /* The update writes through cf_write. */
    if (cf_writer_start(env->app, env->config) != CF_OK) {
        fprintf(stderr, "  env_open: writer start failed\n");
        cf_app_destroy(env->app);
        env->app = NULL;
        cf_db_scratch_close(&env->scratch);
        return false;
    }
    /* The involvement_change broadcast goes through the app->cable seam,
     * exactly as main.c sets it after cf_cable_create. */
    cf_cable_config cable_config;
    memset(&cable_config, 0, sizeof cable_config);
    cable_config.app = env->app;
    cable_config.database_path = env->scratch.path;
    if (cf_cable_create(&cable_config, &env->cable) != CF_OK) {
        fprintf(stderr, "  env_open: cable creation failed\n");
        cf_app_destroy(env->app);
        env->app = NULL;
        cf_db_scratch_close(&env->scratch);
        return false;
    }
    cf_app_set_cable(env->app, env->cable);
    cf_test_routes_reset();
    /* The double needs both the `(.:format)` rows and the bare rows for the
     * same ids (its literal-segment match takes a suffix row only when an
     * extension is present); the real table has the single optional-format
     * rows. */
    bool ok =
        cf_test_routes_add("GET", "/rooms/:room_id/involvement(.:format)",
                           93, cf_action_rooms_involvements_show) == CF_OK &&
        cf_test_routes_add("GET", "/rooms/:room_id/involvement", 93,
                           cf_action_rooms_involvements_show) == CF_OK &&
        cf_test_routes_add("PATCH", "/rooms/:room_id/involvement(.:format)",
                           94, cf_action_rooms_involvements_update) == CF_OK &&
        cf_test_routes_add("PATCH", "/rooms/:room_id/involvement", 94,
                           cf_action_rooms_involvements_update) == CF_OK &&
        cf_test_routes_add("PUT", "/rooms/:room_id/involvement(.:format)",
                           95, cf_action_rooms_involvements_update) == CF_OK &&
        cf_test_routes_add("PUT", "/rooms/:room_id/involvement", 95,
                           cf_action_rooms_involvements_update) == CF_OK;
    if (!ok) {
        fprintf(stderr, "  env_open: route registration failed\n");
        cf_cable_destroy(env->cable);
        env->cable = NULL;
        cf_app_destroy(env->app);
        env->app = NULL;
        cf_db_scratch_close(&env->scratch);
        return false;
    }
    return true;
}

static void env_close(inv_env *env) {
    if (env->cable != NULL) cf_cable_destroy(env->cable);
    env->cable = NULL;
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

static void seed_user(cf_db *db, int64_t id, const char *name) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, created_at, email_address, name, role, "
             "status, updated_at) VALUES (%lld, "
             "'2026-09-26 13:00:29.000000', NULL, '%s', 0, 0, "
             "'2026-09-26 13:00:29.000000')",
             (long long)id, name);
    exec_sql(db, sql);
}

static void seed_room_typed(cf_db *db, int64_t id, const char *name,
                            const char *type_text, int64_t creator_id) {
    char name_sql[256];
    if (name != NULL) {
        snprintf(name_sql, sizeof name_sql, "'%s'", name);
    } else {
        snprintf(name_sql, sizeof name_sql, "NULL");
    }
    char sql[768];
    snprintf(sql, sizeof sql,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (%lld, '2026-01-01 00:00:00.000000', %lld, "
             "%s, '%s', '2026-01-01 00:00:00.000000')",
             (long long)id, (long long)creator_id, name_sql, type_text);
    exec_sql(db, sql);
}

/* involvement_sql is the SQL literal: 'mentions', 'everything', 'nothing',
 * 'invisible', or NULL (no quotes). */
static void seed_membership_involved(cf_db *db, int64_t id, int64_t room_id,
                                     int64_t user_id,
                                     const char *involvement_sql) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO memberships (id, created_at, involvement, room_id, "
             "updated_at, user_id) VALUES (%lld, "
             "'2026-01-01 00:00:00.000000', %s, %lld, "
             "'2026-01-01 00:00:00.000000', %lld)",
             (long long)id, involvement_sql, (long long)room_id,
             (long long)user_id);
    exec_sql(db, sql);
}

static void seed_membership(cf_db *db, int64_t id, int64_t room_id,
                            int64_t user_id) {
    seed_membership_involved(db, id, room_id, user_id, "'mentions'");
}

/* The stored involvement text, or NULL when the column is NULL.  The
 * caller frees the result (NULL stays NULL). */
static char *read_involvement(cf_db *db, int64_t room_id, int64_t user_id) {
    char sql[256];
    snprintf(sql, sizeof sql,
             "SELECT involvement FROM memberships WHERE room_id = %lld AND "
             "user_id = %lld",
             (long long)room_id, (long long)user_id);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    char *value = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW &&
        sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        value = malloc(strlen((const char *)text) + 1);
        CF_REQUIRE(value != NULL);
        strcpy(value, (const char *)text);
    }
    sqlite3_finalize(stmt);
    return value;
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

static void make_session_cookie(inv_env *env, char *out, size_t cap,
                                int64_t user_id) {
    char token[64];
    snprintf(token, sizeof token, "inv-token-%u", env->session_seq++);
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

/* --- request helpers ------------------------------------------------------- */

static bool run_request(inv_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static bool head_contains(const cf_buf *buf, const char *needle) {
    cf_span span = cf_buf_span(buf);
    size_t len = strlen(needle);
    if (len == 0) return true;
    if (span.len < len) return false;
    for (size_t i = 0; i + len <= span.len; i++) {
        if (memcmp(span.ptr + i, needle, len) == 0) return true;
    }
    return false;
}

static bool response_body_contains(cf_response *resp, const char *needle) {
    return resp->body != NULL && head_contains(resp->body, needle);
}

static bool response_head_contains(cf_response *resp, cf_request *req,
                                   const char *needle) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    bool found = head_contains(ser.headers, needle);
    cf_buf_release(ser.headers);
    return found;
}

/* Authenticated form request (PATCH/PUT share this; the route double binds
 * 94/95 to the same action).  The cookie lives in a static ring so its span
 * outlives the request. */
static void prepare_update(inv_env *env, cf_request *req, cf_method method,
                           int64_t user_id, const char *path,
                           const char *body) {
    static char ring[8][4096];
    static unsigned at;
    char *slot = ring[at++ % 8];
    make_session_cookie(env, slot, sizeof ring[0], user_id);
    cf_test_req_init(req);
    req->method = method;
    req->original_method = method;
    req->path = SP(path);
    req->target = req->path;
    req->body = SP(body);
    CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(slot)) == CF_OK);
    CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                  SP("application/x-www-form-urlencoded")) ==
               CF_OK);
    CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                  SP("same-origin")) == CF_OK);
}

static void prepare_show(inv_env *env, cf_request *req, int64_t user_id,
                         const char *path, const char *accept,
                         const char *header_name, cf_span header_value) {
    static char ring[8][4096];
    static unsigned slot_at;
    char *slot = ring[slot_at++ % 8];
    make_session_cookie(env, slot, sizeof ring[0], user_id);
    cf_test_req_init(req);
    req->path = SP(path);
    req->target = req->path;
    CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(slot)) == CF_OK);
    if (accept != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Accept"), SP(accept)) ==
                   CF_OK);
    }
    if (header_name != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP(header_name), header_value) ==
                   CF_OK);
    }
}

static void seed_base(inv_env *env) {
    seed_user(env->scratch.db, 1, "Kevin");
    seed_room_typed(env->scratch.db, 42, "HQ", "Rooms::Open", 1);
    seed_membership(env->scratch.db, 201, 42, 1);
}

/* --- route IDs ------------------------------------------------------------- */

CF_TEST(involvements_route_ids_bind_the_actions) {
    inv_env env;
    CF_REQUIRE(env_open(&env));
    CF_CHECK(cf_route_action(93) == cf_action_rooms_involvements_show);
    CF_CHECK(cf_route_action(94) == cf_action_rooms_involvements_update);
    CF_CHECK(cf_route_action(95) == cf_action_rooms_involvements_update);
    env_close(&env);
}

/* --- show ------------------------------------------------------------------ */

CF_TEST(involvements_show_unauthenticated_is_redirected_to_sign_in) {
    inv_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    cf_request req;
    cf_test_req_init(&req);
    req.path = SP("/rooms/42/involvement");
    req.target = req.path;
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(response_head_contains(&resp, &req,
                                    "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(involvements_show_not_found_or_inaccessible_is_404) {
    inv_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_user(env.scratch.db, 2, "JZ");
    seed_room_typed(env.scratch.db, 99, "Other", "Rooms::Open", 2);
    seed_membership(env.scratch.db, 202, 99, 2);

    static const char *paths[] = {
        "/rooms/12345/involvement",
        "/rooms/99/involvement",
        "/rooms/abc/involvement",
    };
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        cf_request req;
        prepare_show(&env, &req, 1, paths[i], NULL, NULL,
                     (cf_span){NULL, 0});
        cf_response resp;
        CF_REQUIRE(run_request(&env, &req, &resp));
        CF_CHECK(resp.status == 404);
        cf_response_dispose(&resp);
    }
    env_close(&env);
}

CF_TEST(involvements_show_gone_room_is_500) {
    inv_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    exec_sql(env.scratch.db, "DELETE FROM rooms WHERE id = 42");

    cf_request req;
    prepare_show(&env, &req, 1, "/rooms/42/involvement", NULL, NULL,
                 (cf_span){NULL, 0});
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Success: the involvement frame for the room kind, carrying the
 * membership's involvement value, inside the application layout (with its
 * preload `Link` header). */
CF_TEST(involvements_show_renders_the_room_kind_and_involvement) {
    inv_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    cf_request req;
    prepare_show(&env, &req, 1, "/rooms/42/involvement", NULL, NULL,
                 (cf_span){NULL, 0});
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(response_head_contains(
        &resp, &req, "Content-Type: text/html; charset=utf-8\r\n"));
    CF_CHECK(response_body_contains(&resp, "id=\"involvement_rooms_open_42\""));
    CF_CHECK(response_body_contains(
        &resp, "data-turbo-frame-url-param=\"/rooms/42/involvement\""));
    CF_CHECK(response_body_contains(&resp, "mentions"));
    CF_CHECK(response_body_contains(&resp, "<title>"));
    CF_CHECK(response_head_contains(&resp, &req, "Link:"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A NULL involvement column renders as the empty value (the source's
 * unwrap_or_default), and closed/direct rooms render their own kind. */
CF_TEST(involvements_show_renders_nil_and_other_kinds) {
    inv_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin");
    seed_room_typed(env.scratch.db, 42, "HQ", "Rooms::Closed", 1);
    seed_membership_involved(env.scratch.db, 201, 42, 1, "NULL");
    seed_room_typed(env.scratch.db, 43, NULL, "Rooms::Direct", 1);
    seed_membership_involved(env.scratch.db, 202, 43, 1, "'everything'");

    cf_request closed_req;
    prepare_show(&env, &closed_req, 1, "/rooms/42/involvement", NULL, NULL,
                 (cf_span){NULL, 0});
    cf_response closed_resp;
    CF_REQUIRE(run_request(&env, &closed_req, &closed_resp));
    CF_CHECK(closed_resp.status == 200);
    CF_CHECK(response_body_contains(&closed_resp,
                                    "id=\"involvement_rooms_closed_42\""));
    CF_CHECK(!response_body_contains(&closed_resp, "mentions"));
    cf_response_dispose(&closed_resp);

    cf_request direct_req;
    prepare_show(&env, &direct_req, 1, "/rooms/43/involvement", NULL, NULL,
                 (cf_span){NULL, 0});
    cf_response direct_resp;
    CF_REQUIRE(run_request(&env, &direct_req, &direct_resp));
    CF_CHECK(direct_resp.status == 200);
    CF_CHECK(response_body_contains(&direct_resp,
                                    "id=\"involvement_rooms_direct_43\""));
    CF_CHECK(response_body_contains(&direct_resp, "everything"));
    cf_response_dispose(&direct_resp);
    env_close(&env);
}

/* A Turbo-Frame request renders the frame layout: content without the page
 * chrome and without the `Link` header. */
CF_TEST(involvements_show_turbo_frame_request_renders_the_frame_layout) {
    inv_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    cf_request req;
    prepare_show(&env, &req, 1, "/rooms/42/involvement", NULL, "Turbo-Frame",
                 SP("involvement"));
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(response_body_contains(&resp, "id=\"involvement_rooms_open_42\""));
    CF_CHECK(!response_body_contains(&resp, "<title>"));
    CF_CHECK(!response_head_contains(&resp, &req, "Link:"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* page::content negotiates HTML: a JSON-only client gets 406. */
CF_TEST(involvements_show_json_client_is_406) {
    inv_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    cf_request req;
    prepare_show(&env, &req, 1, "/rooms/42/involvement", "application/json",
                 NULL, (cf_span){NULL, 0});
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* deny_bots (Before::default): an authenticated bot is forbidden. */
CF_TEST(involvements_show_bot_authentication_is_forbidden) {
    inv_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    exec_sql(env.scratch.db,
             "INSERT INTO users (id, created_at, email_address, name, role, "
             "status, updated_at, bot_token) VALUES (7, "
             "'2026-09-26 13:00:29.000000', NULL, 'Bender Bot', 2, 0, "
             "'2026-09-26 13:00:29.000000', 'invbotkey')");

    cf_request req;
    cf_test_req_init(&req);
    req.path = SP("/rooms/42/involvement");
    req.target = req.path;
    req.query = SP("bot_key=7-invbotkey");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- update ---------------------------------------------------------------- */

CF_TEST(involvements_update_unauthenticated_is_redirected_to_sign_in) {
    inv_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    cf_request req;
    cf_test_req_init(&req);
    req.method = CF_PATCH;
    req.original_method = CF_PATCH;
    req.path = SP("/rooms/42/involvement");
    req.target = req.path;
    req.body = SP("involvement=everything");
    CF_REQUIRE(cf_test_req_header(&req, SP("Content-Type"),
                                  SP("application/x-www-form-urlencoded")) ==
               CF_OK);
    CF_REQUIRE(cf_test_req_header(&req, SP("Sec-Fetch-Site"),
                                  SP("same-origin")) == CF_OK);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(response_head_contains(&resp, &req,
                                    "Location: " ORIGIN "/session/new\r\n"));
    char *stored = read_involvement(env.scratch.db, 42, 1);
    CF_CHECK(stored != NULL && strcmp(stored, "mentions") == 0);
    free(stored);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A room the user cannot reach: 404 with no state change (AUTH-06: another
 * scope is neither revealed nor mutated). */
CF_TEST(involvements_update_inaccessible_room_is_404_without_change) {
    inv_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_user(env.scratch.db, 2, "JZ");
    seed_room_typed(env.scratch.db, 99, "Other", "Rooms::Open", 2);
    seed_membership(env.scratch.db, 202, 99, 2);

    cf_request req;
    prepare_update(&env, &req, CF_PATCH, 1, "/rooms/99/involvement",
                   "involvement=everything");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    char *stored = read_involvement(env.scratch.db, 99, 2);
    CF_CHECK(stored != NULL && strcmp(stored, "mentions") == 0);
    free(stored);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* The ordinary path: the value is stored, and the response redirects to
 * the room involvement URL.  mentions -> everything emits no broadcast
 * (involvement_change only acts on the invisible transitions). */
CF_TEST(involvements_update_stores_the_value_and_redirects) {
    inv_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    cf_cable_stats before = {0};
    cf_cable_stats_get(env.cable, &before);

    cf_request req;
    prepare_update(&env, &req, CF_PATCH, 1, "/rooms/42/involvement",
                   "involvement=everything");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(response_head_contains(
        &resp, &req, "Location: " ORIGIN "/rooms/42/involvement\r\n"));
    CF_CHECK(response_head_contains(
        &resp, &req, "Content-Type: text/html; charset=utf-8\r\n"));
    char *stored = read_involvement(env.scratch.db, 42, 1);
    CF_CHECK(stored != NULL && strcmp(stored, "everything") == 0);
    free(stored);
    cf_cable_stats after = {0};
    cf_cable_stats_get(env.cable, &after);
    CF_CHECK(after.published == before.published);
    CF_CHECK(after.delivered == before.delivered);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Route 95 (PUT) shares the update action. */
CF_TEST(involvements_update_put_alias_stores_the_value) {
    inv_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    cf_request req;
    prepare_update(&env, &req, CF_PUT, 1, "/rooms/42/involvement",
                   "involvement=nothing");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(response_head_contains(
        &resp, &req, "Location: " ORIGIN "/rooms/42/involvement\r\n"));
    char *stored = read_involvement(env.scratch.db, 42, 1);
    CF_CHECK(stored != NULL && strcmp(stored, "nothing") == 0);
    free(stored);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Blank values (missing, "", whitespace) are stored as nil. */
CF_TEST(involvements_update_blank_is_stored_as_nil) {
    static const char *bodies[] = {
        "involvement=", "involvement=++", "other=1",
    };
    for (size_t i = 0; i < sizeof bodies / sizeof bodies[0]; i++) {
        inv_env env;
        CF_REQUIRE(env_open(&env));
        seed_base(&env);

        cf_request req;
        prepare_update(&env, &req, CF_PATCH, 1, "/rooms/42/involvement",
                       bodies[i]);
        cf_response resp;
        CF_REQUIRE(run_request(&env, &req, &resp));
        if (resp.status != 302) {
            fprintf(stderr, "  [%s] status %u\n", bodies[i], resp.status);
        }
        CF_CHECK(resp.status == 302);
        char *stored = read_involvement(env.scratch.db, 42, 1);
        if (stored != NULL) {
            fprintf(stderr, "  [%s] stored %s\n", bodies[i], stored);
        }
        CF_CHECK(stored == NULL);
        free(stored);
        cf_response_dispose(&resp);
        env_close(&env);
    }
}

/* Anything that is not a value raises: 500 with no state change. */
CF_TEST(involvements_update_invalid_value_is_500_without_change) {
    inv_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    static const struct {
        const char *body;
        const char *content_type;
        const char *what;
    } cases[] = {
        {"involvement=admin", "application/x-www-form-urlencoded",
         "unknown name"},
        {"involvement[]=everything", "application/x-www-form-urlencoded",
         "array"},
        {"{\"involvement\": {\"a\": 1}}", "application/json", "hash"},
        {"{\"involvement\": 3}", "application/json", "number"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cf_request req;
        static char ring[4][4096];
        char *slot = ring[i % 4];
        make_session_cookie(&env, slot, sizeof ring[0], 1);
        cf_test_req_init(&req);
        req.method = CF_PATCH;
        req.original_method = CF_PATCH;
        req.path = SP("/rooms/42/involvement");
        req.target = req.path;
        req.body = SP(cases[i].body);
        CF_REQUIRE(cf_test_req_header(&req, SP("Cookie"), SP(slot)) == CF_OK);
        CF_REQUIRE(cf_test_req_header(&req, SP("Content-Type"),
                                      SP(cases[i].content_type)) == CF_OK);
        CF_REQUIRE(cf_test_req_header(&req, SP("Sec-Fetch-Site"),
                                      SP("same-origin")) == CF_OK);
        cf_response resp;
        CF_REQUIRE(run_request(&env, &req, &resp));
        if (resp.status != 500) {
            fprintf(stderr, "  [%s] status %u\n", cases[i].what, resp.status);
        }
        CF_CHECK(resp.status == 500);
        char *stored = read_involvement(env.scratch.db, 42, 1);
        CF_CHECK(stored != NULL && strcmp(stored, "mentions") == 0);
        free(stored);
        cf_response_dispose(&resp);
    }
    env_close(&env);
}

/* A nil previous involvement raises `nil.inquiry` after the update has
 * saved: the write commits (500 with the new value stored). */
CF_TEST(involvements_update_nil_previous_saves_then_raises) {
    inv_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin");
    seed_room_typed(env.scratch.db, 42, "HQ", "Rooms::Open", 1);
    seed_membership_involved(env.scratch.db, 201, 42, 1, "NULL");

    cf_request req;
    prepare_update(&env, &req, CF_PATCH, 1, "/rooms/42/involvement",
                   "involvement=mentions");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    char *stored = read_involvement(env.scratch.db, 42, 1);
    CF_CHECK(stored != NULL && strcmp(stored, "mentions") == 0);
    free(stored);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- the involvement_change broadcast -------------------------------------- */

typedef struct {
    ct_run run;
    cf_cable_request request;
    cf_header headers[1];
    char cookie[1024];
    char identifier[2048];
} inv_conn;

/* `Turbo::StreamsChannel` over the signed stream name of
 * `[user_gid.to_param, "rooms"]` — the stream involvement_change publishes
 * to (channels/broadcasts.rs user_rooms). */
static void inv_conn_identifier(inv_env *env, inv_conn *c, int64_t user_id) {
    cf_str gid = {0};
    CF_REQUIRE(cf_cable_user_gid_param(user_id, &gid) == CF_OK);
    cf_span parts[2] = {{(const unsigned char *)gid.ptr, gid.len},
                        SP("rooms")};
    cf_str signed_name = {0};
    CF_REQUIRE(cf_auth_turbo_signed_stream_name(
                   (cf_span){(const unsigned char *)env->config
                                 ->secret_key_base,
                             env->config->secret_key_base_len},
                   parts, 2, &signed_name) == CF_OK);
    snprintf(c->identifier, sizeof c->identifier,
             "{\"channel\":\"Turbo::StreamsChannel\","
             "\"signed_stream_name\":\"%.*s\"}",
             (int)signed_name.len, signed_name.ptr);
    cf_str_dispose(&signed_name);
    cf_str_dispose(&gid);
}

static bool inv_conn_open(inv_env *env, int64_t user_id, inv_conn *c) {
    memset(c, 0, sizeof *c);
    make_session_cookie(env, c->cookie, sizeof c->cookie, user_id);
    c->headers[0].name = SP("Cookie");
    c->headers[0].value = SP(c->cookie);
    c->request.method = CF_GET;
    c->request.headers = c->headers;
    c->request.header_count = 1;
    cf_cable_hooks hooks;
    cf_cable_server_hooks(env->cable, &hooks);
    if (!ct_run_start_with_request(&c->run, &hooks, NULL, false,
                                   &c->request)) {
        return false;
    }
    unsigned opcode = 0;
    bool rsv1 = false;
    unsigned char payload[256];
    ssize_t n = ct_read_server_frame(c->run.peer_fd, &opcode, &rsv1, payload,
                                     sizeof payload, 3000);
    return n == 18 && memcmp(payload, "{\"type\":\"welcome\"}", 18) == 0;
}

/* The next non-ping text frame on the socket; false on timeout/close. */
static bool inv_conn_next_text(inv_conn *c, char *buf, size_t cap,
                               int timeout_ms) {
    int64_t deadline = ct_now_ms() + timeout_ms;
    for (;;) {
        int left = (int)(deadline - ct_now_ms());
        if (left <= 0) return false;
        unsigned opcode = 0;
        bool rsv1 = false;
        ssize_t n = ct_read_server_frame(c->run.peer_fd, &opcode, &rsv1,
                                         (unsigned char *)buf, cap - 1, left);
        if (n < 0) return false;
        buf[n] = '\0';
        if (opcode != 0x1) continue;
        if (strncmp(buf, "{\"type\":\"ping\"", 14) == 0) continue;
        return true;
    }
}

static bool inv_conn_subscribe(inv_env *env, inv_conn *c, int64_t user_id) {
    inv_conn_identifier(env, c, user_id);
    cf_builder quoted = {0};
    if (cf_json_string(&quoted, SP(c->identifier)) != CF_OK) return false;
    char command[4096];
    snprintf(command, sizeof command,
             "{\"command\":\"subscribe\",\"identifier\":%.*s}",
             (int)quoted.len, quoted.ptr);
    cf_builder_dispose(&quoted);
    if (!ct_send_client_frame(c->run.peer_fd, 0x1, true, false,
                              (const unsigned char *)command,
                              strlen(command))) {
        return false;
    }
    char got[262144];
    if (!inv_conn_next_text(c, got, sizeof got, 5000)) {
        fprintf(stderr, "  no subscribe confirmation\n");
        return false;
    }
    return strstr(got, "\"type\":\"confirm_subscription\"") != NULL;
}

/* Read one frame and compare it with the exact delivery frame the broadcast
 * source produces for `payload` on this subscription. */
static bool inv_conn_expect_payload(inv_conn *c, cf_span payload,
                                    int timeout_ms) {
    cf_builder ident_json = {0};
    cf_builder payload_json = {0};
    cf_err rc = cf_json_string(&ident_json, SP(c->identifier));
    if (rc == CF_OK) rc = cf_json_string(&payload_json, payload);
    cf_str expected = {0};
    if (rc == CF_OK) {
        rc = cf_cable_message_frame((cf_span){ident_json.ptr, ident_json.len},
                                    (cf_span){payload_json.ptr,
                                              payload_json.len},
                                    &expected);
    }
    cf_builder_dispose(&ident_json);
    cf_builder_dispose(&payload_json);
    CF_REQUIRE(rc == CF_OK);

    char got[262144];
    if (!inv_conn_next_text(c, got, sizeof got, timeout_ms)) {
        fprintf(stderr, "  no broadcast frame\n");
        cf_str_dispose(&expected);
        return false;
    }
    bool ok = strlen(got) == expected.len &&
              memcmp(got, expected.ptr, expected.len) == 0;
    if (!ok) {
        fprintf(stderr, "  broadcast mismatch\n  got:      %s\n  expected: %s\n",
                got, expected.ptr);
    }
    cf_str_dispose(&expected);
    return ok;
}

static void inv_conn_close(inv_conn *c) {
    ct_run_finish(&c->run);
    ct_run_join(&c->run);
}

/* mentions -> invisible removes the room from the member's rooms stream
 * with the exact Turbo remove frame, alongside the redirect and the stored
 * value. */
CF_TEST(involvements_update_to_invisible_broadcasts_the_room_removal) {
    inv_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    inv_conn conn;
    CF_REQUIRE(inv_conn_open(&env, 1, &conn));
    CF_REQUIRE(inv_conn_subscribe(&env, &conn, 1));

    cf_request req;
    prepare_update(&env, &req, CF_PATCH, 1, "/rooms/42/involvement",
                   "involvement=invisible");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(response_head_contains(
        &resp, &req, "Location: " ORIGIN "/rooms/42/involvement\r\n"));
    char *stored = read_involvement(env.scratch.db, 42, 1);
    CF_CHECK(stored != NULL && strcmp(stored, "invisible") == 0);
    free(stored);
    cf_response_dispose(&resp);

    CF_CHECK(inv_conn_expect_payload(
        &conn,
        SP("<turbo-stream action=\"remove\" "
           "target=\"list_rooms_open_42\"></turbo-stream>"),
        5000));
    inv_conn_close(&conn);
    env_close(&env);
}

/* invisible -> mentions: `broadcast_visibility_changes`' prepend branch
 * renders the production users/sidebars/rooms/_shared anchor
 * (`render_shared_room`) on the member's own rooms stream.  Before V02 wired
 * the partial this update failed after committing (CF_NOT_FOUND). */
CF_TEST(involvements_update_from_invisible_prepends_the_shared_room) {
    inv_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    inv_conn conn;
    CF_REQUIRE(inv_conn_open(&env, 1, &conn));
    CF_REQUIRE(inv_conn_subscribe(&env, &conn, 1));

    /* mentions -> invisible: the previous value the branch later reads. */
    cf_request first;
    prepare_update(&env, &first, CF_PATCH, 1, "/rooms/42/involvement",
                   "involvement=invisible");
    cf_response first_resp;
    CF_REQUIRE(run_request(&env, &first, &first_resp));
    CF_CHECK(first_resp.status == 302);
    cf_response_dispose(&first_resp);
    CF_CHECK(inv_conn_expect_payload(
        &conn,
        SP("<turbo-stream action=\"remove\" "
           "target=\"list_rooms_open_42\"></turbo-stream>"),
        5000));

    /* invisible -> mentions: prepend the shared-room anchor. */
    cf_request second;
    prepare_update(&env, &second, CF_PATCH, 1, "/rooms/42/involvement",
                   "involvement=mentions");
    cf_response second_resp;
    CF_REQUIRE(run_request(&env, &second, &second_resp));
    CF_CHECK(second_resp.status == 302);
    char *stored = read_involvement(env.scratch.db, 42, 1);
    CF_CHECK(stored != NULL && strcmp(stored, "mentions") == 0);
    free(stored);
    cf_response_dispose(&second_resp);

    CF_CHECK(inv_conn_expect_payload(
        &conn,
        SP("<turbo-stream action=\"prepend\" target=\"shared_rooms\">"
           "<template><a id=\"list_rooms_open_42\" "
           "data-rooms-list-target=\"room\" data-room-id=\"42\" "
           "data-badge-dot-target=\"unread\" data-sorted-list-target=\"item\" "
           "data-sorted-list-name=\"HQ\" style=\"--column-gap: 0.5em\" "
           "class=\"align-center gap room btn txt-nowrap\" "
           "href=\"/rooms/42\">\n  <span "
           "class=\"overflow-ellipsis\">HQ</span>\n</a>"
           "</template></turbo-stream>"),
        5000));
    inv_conn_close(&conn);
    env_close(&env);
}

/* Direct rooms skip the broadcast (broadcasts.rs involvement_change returns
 * early), but the value is still stored and the redirect still answers. */
CF_TEST(involvements_update_direct_room_skips_the_broadcast) {
    inv_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin");
    seed_room_typed(env.scratch.db, 43, NULL, "Rooms::Direct", 1);
    seed_membership(env.scratch.db, 201, 43, 1);
    cf_cable_stats before = {0};
    cf_cable_stats_get(env.cable, &before);

    cf_request req;
    prepare_update(&env, &req, CF_PATCH, 1, "/rooms/43/involvement",
                   "involvement=invisible");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    char *stored = read_involvement(env.scratch.db, 43, 1);
    CF_CHECK(stored != NULL && strcmp(stored, "invisible") == 0);
    free(stored);
    cf_cable_stats after = {0};
    cf_cable_stats_get(env.cable, &after);
    CF_CHECK(after.published == before.published);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
