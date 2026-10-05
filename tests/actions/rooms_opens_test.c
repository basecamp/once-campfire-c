/* tests/actions/rooms_opens_test.c — A-rooms-opens acceptance:
 * `rooms/opens#index` (route ID 105), `rooms/opens#create` (106),
 * `rooms/opens#new` (107), `rooms/opens#edit` (108), `rooms/opens#show`
 * (109), `rooms/opens#update` (110, 111) and `rooms/opens#destroy` (112)
 * (docs/devel/implementation/contracts/controller-packets.md "A-rooms-opens";
 * 03-application.md open/closed/direct family boundaries).
 *
 * Every case runs the real A00 dispatch path (params, cookies,
 * before-actions, format negotiation) through the route double
 * (tests/app/support/route_double.c), which binds rows 105-112 to this
 * packet's actions.
 *
 * Matrix from the card: success, unauthorized/forbidden/not-found, parameter
 * failure and state/side-effect assertions.  `create` asserts the granted
 * memberships (the creator plus every other active user — `Rooms::Open`
 * grants itself to all active users), the exact prepend broadcast on the
 * `:rooms` stream, and the redirect; `update` asserts the rename, the
 * forced open type (a converted closed room grants every active user), the
 * exact replace broadcast, and the redirect; `destroy` asserts the
 * reference's internal error with no row changes (there is deliberately no
 * opens#destroy success path).  The new/edit renders use the packet's R1
 * placeholder forms (asserting action, name value, user list and the
 * conditional delete section), not the golden templates, which are the views
 * packet's integrator request.
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "cable/channels.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "http/http_internal.h"
#include "models/room.h"
#include "models/user.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"
#include "../cable/cable_testutil.h"

#include <inttypes.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* cf.h is frozen and no packet-action header exists yet, so the test
 * declares the entry points (the integrator's routes.c rebind needs the
 * same declarations). */
cf_err cf_action_rooms_opens_index(cf_ctx *ctx);
cf_err cf_action_rooms_opens_create(cf_ctx *ctx);
cf_err cf_action_rooms_opens_new(cf_ctx *ctx);
cf_err cf_action_rooms_opens_edit(cf_ctx *ctx);
cf_err cf_action_rooms_opens_show(cf_ctx *ctx);
cf_err cf_action_rooms_opens_update(cf_ctx *ctx);
cf_err cf_action_rooms_opens_destroy(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="
#define ROOMS_SECRET "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- scratch app + database ------------------------------------------------ */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    cf_cable *cable;
    int session_seq;
} opens_env;

static bool env_open(opens_env *env) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) {
        fprintf(stderr, "  env_open: scratch open failed\n");
        return false;
    }
    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", ORIGIN},
        {"SECRET_KEY_BASE", HEX64},
        {"VAPID_PUBLIC_KEY", VAPID_PUBLIC_KEY},
        {"DATABASE_PATH", env->scratch.path},
    };
    if (cf_config_parse(entries, 4, NULL, &env->config) != CF_OK) {
        fprintf(stderr, "  env_open: config parse failed\n");
        return false;
    }
    /* The action renders through the real asset module (the pinned manifest,
     * whose digests match the golden/b asset map exactly); an unconfigured
     * module fails layout renders (the rooms_test.c pattern). */
    if (cf_views_assets_configure("tests/fixtures/assets") != CF_OK) {
        fprintf(stderr, "  env_open: assets configure failed\n");
        return false;
    }
    if (cf_app_create(env->config, &env->app) != CF_OK) {
        fprintf(stderr, "  env_open: app create failed\n");
        return false;
    }
    if (cf_writer_start(env->app, env->config) != CF_OK) {
        fprintf(stderr, "  env_open: writer start failed\n");
        cf_app_destroy(env->app);
        env->app = NULL;
        return false;
    }
    /* The app->cable seam the create/update broadcasts need, exactly as
     * main.c sets it after cf_cable_create. */
    cf_cable_config cable_config;
    memset(&cable_config, 0, sizeof cable_config);
    cable_config.app = env->app;
    cable_config.database_path = env->scratch.path;
    if (cf_cable_create(&cable_config, &env->cable) != CF_OK) {
        fprintf(stderr, "  env_open: cable creation failed\n");
        cf_app_destroy(env->app);
        env->app = NULL;
        return false;
    }
    cf_app_set_cable(env->app, env->cable);
    cf_test_routes_reset();
    bool ok = cf_test_routes_add("GET", "/rooms/opens", 105,
                                 cf_action_rooms_opens_index) == CF_OK &&
              cf_test_routes_add("POST", "/rooms/opens", 106,
                                 cf_action_rooms_opens_create) == CF_OK &&
              cf_test_routes_add("GET", "/rooms/opens/new", 107,
                                 cf_action_rooms_opens_new) == CF_OK &&
              cf_test_routes_add("GET", "/rooms/opens/:id/edit", 108,
                                 cf_action_rooms_opens_edit) == CF_OK &&
              cf_test_routes_add("GET", "/rooms/opens/:id", 109,
                                 cf_action_rooms_opens_show) == CF_OK &&
              cf_test_routes_add("PATCH", "/rooms/opens/:id", 110,
                                 cf_action_rooms_opens_update) == CF_OK &&
              cf_test_routes_add("PUT", "/rooms/opens/:id", 111,
                                 cf_action_rooms_opens_update) == CF_OK &&
              cf_test_routes_add("DELETE", "/rooms/opens/:id", 112,
                                 cf_action_rooms_opens_destroy) == CF_OK;
    if (!ok) {
        fprintf(stderr, "  env_open: route registration failed\n");
        cf_cable_destroy(env->cable);
        env->cable = NULL;
        cf_app_destroy(env->app);
        env->app = NULL;
        return false;
    }
    return true;
}

static void env_close(opens_env *env) {
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

static void seed_user(cf_db *db, int64_t id, const char *name, int role) {
    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, created_at, email_address, name, role, "
             "status, updated_at) VALUES (%lld, "
             "'2026-09-26 13:00:29.000000', NULL, '%s', %d, 0, "
             "'2026-09-26 13:00:29.000000')",
             (long long)id, name, role);
    exec_sql(db, sql);
}

/* type_text is the STI class name ("Rooms::Open", ...). */
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

static void seed_membership(cf_db *db, int64_t id, int64_t room_id,
                            int64_t user_id) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO memberships (id, created_at, room_id, updated_at, "
             "user_id) VALUES (%lld, '2026-01-01 00:00:00.000000', %lld, "
             "'2026-01-01 00:00:00.000000', %lld)",
             (long long)id, (long long)room_id, (long long)user_id);
    exec_sql(db, sql);
}

static void seed_account(cf_db *db, const char *settings_json_or_null) {
    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
             "name, settings, singleton_guard, updated_at) VALUES "
             "(873240054, '2026-09-26 13:03:57.000000', NULL, "
             "'CRMu-l8Ge-KB9B', '37signals', %s, 0, "
             "'2026-09-26 13:03:57.000000')",
             settings_json_or_null);
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

/* --- request helpers ------------------------------------------------------- */

static bool run_request(opens_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
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

/* Authenticated request with the session cookie for `user_id`; extra_cookie
 * is appended when non-NULL. */
static void authenticated_request(opens_env *env, int64_t user_id,
                                  const char *extra_cookie, cf_request *req,
                                  char *header, size_t cap) {
    static unsigned counter;
    char token[64];
    snprintf(token, sizeof token, "opens-token-%u", counter++);
    make_session_cookie(env->config, env->scratch.db, header, cap, token,
                        user_id);
    if (extra_cookie != NULL) {
        size_t at = strlen(header);
        snprintf(header + at, cap - at, "; %s", extra_cookie);
    }
    CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(header)) == CF_OK);
}

static void prepare_get(cf_request *req, const char *path) {
    cf_test_req_init(req);
    req->path = SP(path);
    req->target = req->path;
}

static void prepare_form(cf_request *req, cf_method method, const char *path,
                         const char *body, bool csrf_ok) {
    cf_test_req_init(req);
    req->method = method;
    req->original_method = method;
    req->path = SP(path);
    req->target = req->path;
    req->body = SP(body);
    CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                  SP("application/x-www-form-urlencoded")) ==
               CF_OK);
    /* The chain's CSRF input is Sec-Fetch-Site (the users_bans_test.c
     * pattern): same-origin passes, cross-site is the 422. */
    CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                  SP(csrf_ok ? "same-origin"
                                            : "cross-site")) == CF_OK);
}

static void prepare_delete(cf_request *req, const char *path) {
    cf_test_req_init(req);
    req->method = CF_DELETE;
    req->original_method = CF_DELETE;
    req->path = SP(path);
    req->target = req->path;
    CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                  SP("same-origin")) == CF_OK);
}

static bool room_row_exists(cf_db *db, int64_t id) {
    char sql[128];
    snprintf(sql, sizeof sql, "SELECT count(*) FROM rooms WHERE id = %lld",
             (long long)id);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    bool exists = sqlite3_step(stmt) == SQLITE_ROW &&
                  sqlite3_column_int64(stmt, 0) > 0;
    sqlite3_finalize(stmt);
    return exists;
}

static int64_t count_rows(cf_db *db, const char *table, const char *where) {
    char sql[256];
    snprintf(sql, sizeof sql, "SELECT count(*) FROM %s WHERE %s", table, where);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    int64_t count =
        sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    return count;
}

static bool member_of(cf_db *db, int64_t room_id, int64_t user_id) {
    char sql[256];
    snprintf(sql, sizeof sql,
             "SELECT count(*) FROM memberships WHERE room_id = %lld AND "
             "user_id = %lld",
             (long long)room_id, (long long)user_id);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    bool member = sqlite3_step(stmt) == SQLITE_ROW &&
                  sqlite3_column_int64(stmt, 0) > 0;
    sqlite3_finalize(stmt);
    return member;
}

static int64_t last_room_id(cf_db *db) {
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db),
                                  "SELECT max(id) FROM rooms", -1, &stmt,
                                  NULL) == SQLITE_OK);
    int64_t id =
        sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    return id;
}

static bool room_type_is(cf_db *db, int64_t id, const char *type_text) {
    char sql[256];
    snprintf(sql, sizeof sql, "SELECT type FROM rooms WHERE id = %lld",
             (long long)id);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    bool match = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *type = (const char *)sqlite3_column_text(stmt, 0);
        match = type != NULL && strcmp(type, type_text) == 0;
    }
    sqlite3_finalize(stmt);
    return match;
}

static bool room_name_is(cf_db *db, int64_t id, const char *expected) {
    char sql[256];
    snprintf(sql, sizeof sql, "SELECT name FROM rooms WHERE id = %lld",
             (long long)id);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    bool match = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (expected == NULL) {
            match = sqlite3_column_type(stmt, 0) == SQLITE_NULL;
        } else {
            const char *name = (const char *)sqlite3_column_text(stmt, 0);
            match = name != NULL && strcmp(name, expected) == 0;
        }
    }
    sqlite3_finalize(stmt);
    return match;
}

/* --- the rooms-list cable connection (rooms_test.c pattern) ---------------- */

typedef struct {
    ct_run run;
    cf_cable_request request;
    cf_header headers[1];
    char cookie[1024];
    char identifier[1024];
} opens_conn;

/* `Turbo::StreamsChannel` over the signed stream name of `["rooms"]`. */
static void opens_conn_identifier(opens_conn *c) {
    cf_span parts[1] = {{(const unsigned char *)"rooms", 5}};
    cf_str signed_name = {0};
    CF_REQUIRE(cf_auth_turbo_signed_stream_name(SP(ROOMS_SECRET), parts, 1,
                                                &signed_name) == CF_OK);
    snprintf(c->identifier, sizeof c->identifier,
             "{\"channel\":\"Turbo::StreamsChannel\","
             "\"signed_stream_name\":\"%.*s\"}",
             (int)signed_name.len, signed_name.ptr);
    cf_str_dispose(&signed_name);
}

static bool opens_conn_open(opens_env *env, int64_t user_id, opens_conn *c) {
    memset(c, 0, sizeof *c);
    char token[64];
    snprintf(token, sizeof token, "opens-cable-token-%d", env->session_seq++);
    make_session_cookie(env->config, env->scratch.db, c->cookie,
                        sizeof c->cookie, token, user_id);
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
static bool opens_conn_next_text(opens_conn *c, char *buf, size_t cap,
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

static bool opens_conn_subscribe(opens_conn *c) {
    opens_conn_identifier(c);
    cf_builder quoted = {0};
    if (cf_json_string(&quoted, SP(c->identifier)) != CF_OK) return false;
    char command[2048];
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
    if (!opens_conn_next_text(c, got, sizeof got, 5000)) {
        fprintf(stderr, "  no subscribe confirmation\n");
        return false;
    }
    return strstr(got, "\"type\":\"confirm_subscription\"") != NULL;
}

/* Read one frame and compare it with the exact delivery frame the broadcast
 * source produces for `payload` on this subscription. */
static bool opens_conn_expect_payload(opens_conn *c, cf_span payload,
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
    if (!opens_conn_next_text(c, got, sizeof got, timeout_ms)) {
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

static void opens_conn_close(opens_conn *c) {
    ct_run_finish(&c->run);
    ct_run_join(&c->run);
}

/* --- route IDs ------------------------------------------------------------- */

CF_TEST(opens_route_ids_bind_the_actions) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    CF_CHECK(cf_route_action(105) == cf_action_rooms_opens_index);
    CF_CHECK(cf_route_action(106) == cf_action_rooms_opens_create);
    CF_CHECK(cf_route_action(107) == cf_action_rooms_opens_new);
    CF_CHECK(cf_route_action(108) == cf_action_rooms_opens_edit);
    CF_CHECK(cf_route_action(109) == cf_action_rooms_opens_show);
    CF_CHECK(cf_route_action(110) == cf_action_rooms_opens_update);
    CF_CHECK(cf_route_action(111) == cf_action_rooms_opens_update);
    CF_CHECK(cf_route_action(112) == cf_action_rooms_opens_destroy);
    env_close(&env);
}

/* --- opens#index ----------------------------------------------------------- */

CF_TEST(opens_index_redirects_to_the_last_room) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room_typed(env.scratch.db, 42, "HQ", "Rooms::Open", 1);
    seed_room_typed(env.scratch.db, 43, "All Talk", "Rooms::Open", 1);
    seed_membership(env.scratch.db, 201, 42, 1);
    seed_membership(env.scratch.db, 202, 43, 1);

    cf_request req;
    prepare_get(&req, "/rooms/opens");
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    bool to_42 = head_contains(&resp, &req,
                               "Location: " ORIGIN "/rooms/42\r\n");
    bool to_43 = head_contains(&resp, &req,
                               "Location: " ORIGIN "/rooms/43\r\n");
    CF_CHECK(to_42 || to_43);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(opens_index_without_rooms_is_500) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);

    cf_request req;
    prepare_get(&req, "/rooms/opens");
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(opens_index_unauthenticated_is_redirected_to_sign_in) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);

    cf_request req;
    prepare_get(&req, "/rooms/opens");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- opens#new ------------------------------------------------------------- */

CF_TEST(opens_new_renders_the_form_with_default_name_and_users) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_user(env.scratch.db, 2, "JZ", 0);

    cf_request req;
    prepare_get(&req, "/rooms/opens/new");
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    CF_CHECK(body_contains(&resp, "<form action=\"/rooms/opens\""));
    CF_CHECK(body_contains(&resp, "value=\"New room\""));
    CF_CHECK(body_contains(&resp, "Kevin"));
    CF_CHECK(body_contains(&resp, "JZ"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(opens_new_unauthenticated_is_redirected_to_sign_in) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);

    cf_request req;
    prepare_get(&req, "/rooms/opens/new");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* ensure_permission_to_create_rooms: a restricted account forbids a plain
 * member but still serves an administrator. */
CF_TEST(opens_new_restricted_account_forbids_plain_members) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_user(env.scratch.db, 2, "Admin", 1);
    seed_account(env.scratch.db,
                 "'{\"restrict_room_creation_to_administrators\":true}'");

    cf_request req;
    prepare_get(&req, "/rooms/opens/new");
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);

    cf_request admin_req;
    prepare_get(&admin_req, "/rooms/opens/new");
    char admin_header[4096];
    authenticated_request(&env, 2, NULL, &admin_req, admin_header,
                          sizeof admin_header);
    cf_response admin_resp;
    CF_REQUIRE(run_request(&env, &admin_req, &admin_resp));
    CF_CHECK(admin_resp.status == 200);
    cf_response_dispose(&admin_resp);
    env_close(&env);
}

CF_TEST(opens_new_json_client_is_406) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);

    cf_request req;
    prepare_get(&req, "/rooms/opens/new");
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    CF_REQUIRE(cf_test_req_header(&req, SP("Accept"),
                                  SP("application/json")) == CF_OK);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- opens#create ---------------------------------------------------------- */

/* Success: the open room row, memberships for the creator AND every other
 * active user (`Rooms::Open` grants itself to all active users), the exact
 * prepend broadcast on `:rooms`, and the redirect to the room. */
CF_TEST(opens_create_grants_every_active_user_and_broadcasts) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_user(env.scratch.db, 2, "JZ", 0);

    opens_conn conn;
    CF_REQUIRE(opens_conn_open(&env, 1, &conn));
    CF_REQUIRE(opens_conn_subscribe(&conn));

    cf_request req;
    prepare_form(&req, CF_POST, "/rooms/opens", "room[name]=HQ", true);
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    int64_t id = last_room_id(env.scratch.db);
    CF_CHECK(id > 0);
    char location[128];
    snprintf(location, sizeof location, "Location: " ORIGIN "/rooms/%" PRId64
                                        "\r\n",
             id);
    CF_CHECK(head_contains(&resp, &req, location));
    cf_response_dispose(&resp);

    CF_CHECK(room_type_is(env.scratch.db, id, "Rooms::Open"));
    CF_CHECK(room_name_is(env.scratch.db, id, "HQ"));
    CF_CHECK(member_of(env.scratch.db, id, 1));
    CF_CHECK(member_of(env.scratch.db, id, 2));

    char payload[256];
    snprintf(payload, sizeof payload,
             "<turbo-stream action=\"prepend\" target=\"shared_rooms\">"
             "<template><div class=\"shared-room\" data-room-id=\"%" PRId64
             "\">HQ</div></template></turbo-stream>",
             id);
    CF_CHECK(opens_conn_expect_payload(&conn, SP(payload), 5000));
    opens_conn_close(&conn);
    env_close(&env);
}

/* No `room` hash: ParameterMissing (400) with no row effects. */
CF_TEST(opens_create_without_room_param_is_400) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);

    cf_request req;
    prepare_form(&req, CF_POST, "/rooms/opens", "user_ids[]=1", true);
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 400);
    cf_response_dispose(&resp);
    CF_CHECK(last_room_id(env.scratch.db) <= 0);
    env_close(&env);
}

/* A restricted account forbids a plain member's create with no row effects. */
CF_TEST(opens_create_restricted_account_forbids_plain_members) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_account(env.scratch.db,
                 "'{\"restrict_room_creation_to_administrators\":true}'");

    cf_request req;
    prepare_form(&req, CF_POST, "/rooms/opens", "room[name]=HQ", true);
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    CF_CHECK(last_room_id(env.scratch.db) <= 0);
    env_close(&env);
}

CF_TEST(opens_create_unauthenticated_is_redirected_to_sign_in) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);

    cf_request req;
    prepare_form(&req, CF_POST, "/rooms/opens", "room[name]=HQ", true);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    CF_CHECK(last_room_id(env.scratch.db) <= 0);
    env_close(&env);
}

/* Forgery protection: an unsafe POST without a same-origin Fetch-Site is
 * 422 with no row effects. */
CF_TEST(opens_create_without_csrf_header_is_422) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);

    cf_request req;
    prepare_form(&req, CF_POST, "/rooms/opens", "room[name]=HQ", false);
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    cf_response_dispose(&resp);
    CF_CHECK(last_room_id(env.scratch.db) <= 0);
    env_close(&env);
}

/* --- opens#show ------------------------------------------------------------ */

CF_TEST(opens_show_redirects_to_the_room_and_remembers_it) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room_typed(env.scratch.db, 42, "HQ", "Rooms::Open", 1);
    seed_membership(env.scratch.db, 201, 42, 1);

    cf_request req;
    prepare_get(&req, "/rooms/opens/42");
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/rooms/42\r\n"));
    CF_CHECK(head_contains(&resp, &req, "last_room=42; path=/; expires="));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Scope WithoutDirects: a direct room, an unknown id, an unparsable id and
 * a room the user is not a member of all take set_room's arm (root
 * redirect, nothing remembered). */
CF_TEST(opens_show_out_of_scope_redirects_to_root) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_user(env.scratch.db, 2, "JZ", 0);
    seed_room_typed(env.scratch.db, 42, "HQ", "Rooms::Open", 1);
    seed_membership(env.scratch.db, 201, 42, 1);
    seed_room_typed(env.scratch.db, 43, NULL, "Rooms::Direct", 1);
    seed_membership(env.scratch.db, 202, 43, 1);
    seed_membership(env.scratch.db, 203, 43, 2);
    seed_room_typed(env.scratch.db, 44, "Other", "Rooms::Open", 2);
    seed_membership(env.scratch.db, 204, 44, 2);

    static const char *paths[] = {
        "/rooms/opens/43",     /* direct: out of scope */
        "/rooms/opens/44",     /* exists, not a member */
        "/rooms/opens/12345",  /* no such room */
        "/rooms/opens/abc",    /* integer_cast fails */
    };
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        cf_request req;
        prepare_get(&req, paths[i]);
        char auth_header[4096];
        authenticated_request(&env, 1, NULL, &req, auth_header,
                              sizeof auth_header);
        cf_response resp;
        CF_REQUIRE(run_request(&env, &req, &resp));
        CF_CHECK(resp.status == 302);
        CF_CHECK(head_contains(&resp, &req, "Location: " ORIGIN "/\r\n"));
        cf_response_dispose(&resp);
    }
    env_close(&env);
}

CF_TEST(opens_show_unauthenticated_is_redirected_to_sign_in) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room_typed(env.scratch.db, 42, "HQ", "Rooms::Open", 1);
    seed_membership(env.scratch.db, 201, 42, 1);

    cf_request req;
    prepare_get(&req, "/rooms/opens/42");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- opens#edit ------------------------------------------------------------ */

/* The creator's form carries the room name and the delete section. */
CF_TEST(opens_edit_renders_name_and_delete_for_creator) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room_typed(env.scratch.db, 42, "HQ", "Rooms::Open", 1);
    seed_membership(env.scratch.db, 201, 42, 1);

    cf_request req;
    prepare_get(&req, "/rooms/opens/42/edit");
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "<form action=\"/rooms/opens/42\""));
    CF_CHECK(body_contains(&resp, "value=\"HQ\""));
    CF_CHECK(body_contains(&resp, "name=\"_method\""));
    CF_CHECK(body_contains(&resp, "value=\"delete\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A plain member may open the form (no administer gate on edit) but sees no
 * delete section. */
CF_TEST(opens_edit_hides_delete_for_plain_members) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_user(env.scratch.db, 2, "JZ", 0);
    seed_room_typed(env.scratch.db, 42, "HQ", "Rooms::Open", 1);
    seed_membership(env.scratch.db, 201, 42, 1);
    seed_membership(env.scratch.db, 202, 42, 2);

    cf_request req;
    prepare_get(&req, "/rooms/opens/42/edit");
    char auth_header[4096];
    authenticated_request(&env, 2, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "<form action=\"/rooms/opens/42\""));
    CF_CHECK(body_contains(&resp, "value=\"HQ\""));
    CF_CHECK(!body_contains(&resp, "value=\"delete\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* force_room_type: a closed room edits as open (the form posts to opens). */
CF_TEST(opens_edit_serves_a_closed_room_as_open) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room_typed(env.scratch.db, 42, "HQ", "Rooms::Closed", 1);
    seed_membership(env.scratch.db, 201, 42, 1);

    cf_request req;
    prepare_get(&req, "/rooms/opens/42/edit");
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "<form action=\"/rooms/opens/42\""));
    CF_CHECK(room_type_is(env.scratch.db, 42, "Rooms::Closed"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(opens_edit_direct_room_redirects_to_root) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room_typed(env.scratch.db, 43, NULL, "Rooms::Direct", 1);
    seed_membership(env.scratch.db, 202, 43, 1);

    cf_request req;
    prepare_get(&req, "/rooms/opens/43/edit");
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, "Location: " ORIGIN "/\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- opens#update ---------------------------------------------------------- */

/* Success over PATCH: the rename commits, the exact replace broadcast goes
 * out on `:rooms`, and the response redirects to the room. */
CF_TEST(opens_update_renames_and_broadcasts_over_patch) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room_typed(env.scratch.db, 42, "HQ", "Rooms::Open", 1);
    seed_membership(env.scratch.db, 201, 42, 1);

    opens_conn conn;
    CF_REQUIRE(opens_conn_open(&env, 1, &conn));
    CF_REQUIRE(opens_conn_subscribe(&conn));

    cf_request req;
    prepare_form(&req, CF_PATCH, "/rooms/opens/42", "room[name]=Lobby", true);
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/rooms/42\r\n"));
    cf_response_dispose(&resp);

    CF_CHECK(room_name_is(env.scratch.db, 42, "Lobby"));
    CF_CHECK(room_type_is(env.scratch.db, 42, "Rooms::Open"));

    CF_CHECK(opens_conn_expect_payload(
        &conn,
        SP("<turbo-stream action=\"replace\" target=\"list_rooms_open_42\">"
           "<template><div class=\"shared-room\" "
           "data-room-id=\"42\">Lobby</div></template></turbo-stream>"),
        5000));
    opens_conn_close(&conn);
    env_close(&env);
}

/* PUT (route 111) shares the update action. */
CF_TEST(opens_update_renames_over_put) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room_typed(env.scratch.db, 42, "HQ", "Rooms::Open", 1);
    seed_membership(env.scratch.db, 201, 42, 1);

    cf_request req;
    prepare_form(&req, CF_PUT, "/rooms/opens/42", "room[name]=Lobby", true);
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/rooms/42\r\n"));
    cf_response_dispose(&resp);
    CF_CHECK(room_name_is(env.scratch.db, 42, "Lobby"));
    env_close(&env);
}

/* Converting a closed room to open grants every active user (the model's
 * becoming-open grant), not just the current members. */
CF_TEST(opens_update_converts_closed_and_grants_everyone) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_user(env.scratch.db, 2, "JZ", 0);
    seed_user(env.scratch.db, 3, "Sam", 0);
    seed_room_typed(env.scratch.db, 42, "HQ", "Rooms::Closed", 1);
    seed_membership(env.scratch.db, 201, 42, 1);
    CF_CHECK(!member_of(env.scratch.db, 42, 2));
    CF_CHECK(!member_of(env.scratch.db, 42, 3));

    cf_request req;
    prepare_form(&req, CF_PATCH, "/rooms/opens/42", "room[name]=HQ", true);
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    CF_CHECK(room_type_is(env.scratch.db, 42, "Rooms::Open"));
    CF_CHECK(member_of(env.scratch.db, 42, 1));
    CF_CHECK(member_of(env.scratch.db, 42, 2));
    CF_CHECK(member_of(env.scratch.db, 42, 3));
    env_close(&env);
}

/* A plain member who is neither administrator nor creator: 403 head,
 * nothing changed. */
CF_TEST(opens_update_by_plain_member_is_forbidden) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_user(env.scratch.db, 2, "JZ", 0);
    seed_room_typed(env.scratch.db, 42, "HQ", "Rooms::Open", 1);
    seed_membership(env.scratch.db, 201, 42, 1);
    seed_membership(env.scratch.db, 202, 42, 2);

    cf_request req;
    prepare_form(&req, CF_PATCH, "/rooms/opens/42", "room[name]=Lobby", true);
    char auth_header[4096];
    authenticated_request(&env, 2, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    CF_CHECK(head_contains(&resp, &req, "Content-Type: text/html\r\n"));
    cf_response_dispose(&resp);
    CF_CHECK(room_name_is(env.scratch.db, 42, "HQ"));
    env_close(&env);
}

/* No `room` hash: ParameterMissing (400) with no row effects. */
CF_TEST(opens_update_without_room_param_is_400) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room_typed(env.scratch.db, 42, "HQ", "Rooms::Open", 1);
    seed_membership(env.scratch.db, 201, 42, 1);

    cf_request req;
    prepare_form(&req, CF_PATCH, "/rooms/opens/42", "name=Lobby", true);
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 400);
    cf_response_dispose(&resp);
    CF_CHECK(room_name_is(env.scratch.db, 42, "HQ"));
    env_close(&env);
}

CF_TEST(opens_update_unauthenticated_is_redirected_to_sign_in) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room_typed(env.scratch.db, 42, "HQ", "Rooms::Open", 1);
    seed_membership(env.scratch.db, 201, 42, 1);

    cf_request req;
    prepare_form(&req, CF_PATCH, "/rooms/opens/42", "room[name]=Lobby", true);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    CF_CHECK(room_name_is(env.scratch.db, 42, "HQ"));
    env_close(&env);
}

/* --- opens#destroy --------------------------------------------------------- */

/* destroy_without_room: `nil.destroy` raises — the reference's internal
 * error — with no row changes.  There is no success path to test. */
CF_TEST(opens_destroy_is_500_and_changes_nothing) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room_typed(env.scratch.db, 42, "HQ", "Rooms::Open", 1);
    seed_membership(env.scratch.db, 201, 42, 1);

    cf_request req;
    prepare_delete(&req, "/rooms/opens/42");
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    CF_CHECK(room_row_exists(env.scratch.db, 42));
    CF_CHECK(count_rows(env.scratch.db, "memberships", "room_id = 42") == 1);
    env_close(&env);
}

CF_TEST(opens_destroy_unauthenticated_is_redirected_to_sign_in) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room_typed(env.scratch.db, 42, "HQ", "Rooms::Open", 1);
    seed_membership(env.scratch.db, 201, 42, 1);

    cf_request req;
    prepare_delete(&req, "/rooms/opens/42");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    CF_CHECK(room_row_exists(env.scratch.db, 42));
    env_close(&env);
}

/* --- AUTH-06: cross-scope and bot access ----------------------------------- */

/* deny_bots (Before::default): an authenticated bot is forbidden. */
CF_TEST(opens_new_bot_authentication_is_forbidden) {
    opens_env env;
    CF_REQUIRE(env_open(&env));
    exec_sql(env.scratch.db,
             "INSERT INTO users (id, created_at, email_address, name, role, "
             "status, updated_at, bot_token) VALUES (7, "
             "'2026-09-26 13:00:29.000000', NULL, 'Bender Bot', 2, 0, "
             "'2026-09-26 13:00:29.000000', 'opensbotkey')");

    cf_request req;
    prepare_get(&req, "/rooms/opens/new?bot_key=7-opensbotkey");
    req.query = SP("bot_key=7-opensbotkey");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
