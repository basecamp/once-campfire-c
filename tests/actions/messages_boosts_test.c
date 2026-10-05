/* tests/actions/messages_boosts_test.c — A-messages-boosts acceptance:
 * `messages/boosts#index` (route ID 129), `#create` (130), `#new` (131) and
 * `#destroy` (136), per
 * docs/devel/implementation/contracts/controller-packets.md
 * "A-messages-boosts" and 03-application.md's family boundary ("Message
 * edit/update/delete and boosts scope the record through the accessible
 * room, then check creator/admin privileges. Do not authorize merely by
 * ID").
 *
 * Cases:
 *   - route binding: ids 129/130/131/136 resolve to the four actions;
 *   - index/new success: 200 text/html pages carrying the message and its
 *     boosts (the new page carries the boost form for the current user);
 *     the frame variant renders the frame layout; an unknown or
 *     unreachable message is 404; unauthenticated is the sign-in redirect;
 *     an unacceptable format is 406;
 *   - create success: 302 to <PUBLIC_ORIGIN>/messages/<id>/boosts, the
 *     boosts row (message, booster, content), and the exact boost_create
 *     broadcast frame on a subscribed room connection;
 *   - create failures: a missing `boost` param is 400; a missing content
 *     is the reference's NOT NULL 500; an unknown message is 404; a
 *     cross-site POST is 422; all with no row effects and no broadcast;
 *   - destroy success: 204, the row (and only that row) is gone, and the
 *     exact boost_remove frame; destroying another user's boost is 404;
 *     an unknown message or boost is 404; unauthenticated is the sign-in
 *     redirect.
 *
 * The route double (tests/app/support/route_double.c) binds the four rows
 * to the real actions, so every case runs the real A00 dispatch path
 * (params, cookies, before-actions, format negotiation, generic error
 * mapping).
 *
 * cf.h is integrator-owned and does not yet declare this packet's symbols,
 * so the four entry points are declared here; the integrator's routes.c
 * rebind needs the same declarations (c_symbols
 * cf_action_messages_boosts_index / _create / _new / _destroy).
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "cable/channels.h"
#include "config.h"
#include "context.h"
#include "core/testclock.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "http/http_internal.h"
#include "models/boost.h"
#include "models/message.h"
#include "models/room.h"
#include "models/user.h"
#include "richtext.h"
#include "views.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"
#include "../cable/cable_testutil.h"
#include "../views/support/facts.h"

#include <inttypes.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

cf_err cf_action_messages_boosts_index(cf_ctx *ctx);
cf_err cf_action_messages_boosts_create(cf_ctx *ctx);
cf_err cf_action_messages_boosts_new(cf_ctx *ctx);
cf_err cf_action_messages_boosts_destroy(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="

#define DAVID INT64_C(11)
#define KEVIN INT64_C(12)
#define ROOM INT64_C(100)
#define OTHER_ROOM INT64_C(101)
#define MESSAGE INT64_C(1000)

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- environment ------------------------------------------------------------ */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    cf_cable *cable;
    bool writer_started;
    int session_seq;
} boosts_env;

static bool env_open(boosts_env *env) {
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
    env->config = NULL; /* owned by the app */
    if (cf_writer_start(env->app, cf_app_config(env->app)) != CF_OK) {
        return false;
    }
    env->writer_started = true;
    cf_richtext_configure(SP(HEX64));
    if (!cf_test_views_setup()) return false;

    cf_cable_config cable_config;
    memset(&cable_config, 0, sizeof cable_config);
    cable_config.app = env->app;
    cable_config.database_path = env->scratch.path;
    if (cf_cable_create(&cable_config, &env->cable) != CF_OK) return false;
    cf_app_set_cable(env->app, env->cable);

    cf_test_routes_reset();
    /* routes.json ids 129/130/131/136.  The double's `(.:format)` grammar
     * needs an extension for a literal last segment, so the bare forms are
     * registered as well. */
    if (cf_test_routes_add("GET", "/messages/:message_id/boosts(.:format)",
                           129, cf_action_messages_boosts_index) != CF_OK ||
        cf_test_routes_add("GET", "/messages/:message_id/boosts", 129,
                           cf_action_messages_boosts_index) != CF_OK ||
        cf_test_routes_add("POST", "/messages/:message_id/boosts(.:format)",
                           130, cf_action_messages_boosts_create) != CF_OK ||
        cf_test_routes_add("POST", "/messages/:message_id/boosts", 130,
                           cf_action_messages_boosts_create) != CF_OK ||
        cf_test_routes_add("GET", "/messages/:message_id/boosts/new(.:format)",
                           131, cf_action_messages_boosts_new) != CF_OK ||
        cf_test_routes_add("GET", "/messages/:message_id/boosts/new", 131,
                           cf_action_messages_boosts_new) != CF_OK ||
        cf_test_routes_add("DELETE",
                           "/messages/:message_id/boosts/:id(.:format)", 136,
                           cf_action_messages_boosts_destroy) != CF_OK ||
        cf_test_routes_add("DELETE", "/messages/:message_id/boosts/:id",
                           136, cf_action_messages_boosts_destroy) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(boosts_env *env) {
    if (env->app != NULL) cf_app_set_cable(env->app, NULL);
    if (env->cable != NULL) cf_cable_destroy(env->cable);
    env->cable = NULL;
    if (env->writer_started) cf_writer_stop(env->app);
    if (env->app != NULL) cf_app_destroy(env->app);
    env->app = NULL;
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

static void bind_text(sqlite3_stmt *stmt, int index, const char *text) {
    if (text == NULL) {
        CF_REQUIRE(sqlite3_bind_null(stmt, index) == SQLITE_OK);
    } else {
        CF_REQUIRE(sqlite3_bind_text(stmt, index, text, -1,
                                      SQLITE_TRANSIENT) == SQLITE_OK);
    }
}

static void exec_stmt(cf_db *db, const char *sql, sqlite3_stmt *stmt) {
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "  stmt failed (%d): %s\n  %s\n", rc,
                sqlite3_errmsg(cf_db_handle(db)), sql);
    }
    sqlite3_finalize(stmt);
    CF_REQUIRE(rc == SQLITE_DONE);
}

/* --- seeding --------------------------------------------------------------- */

static void seed_account(cf_db *db) {
    static const char sql[] =
        "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
        "name, settings, singleton_guard, updated_at) VALUES "
        "(7, '2026-09-26 12:00:00.000000', NULL, 'CRMu-l8Ge-KB9B', "
        "'37signals', NULL, 0, '2026-09-26 12:00:00.000000')";
    exec_sql(db, sql);
}

static void seed_user(cf_db *db, int64_t id, const char *name, int role) {
    static const char sql[] =
        "INSERT INTO users (id, bio, bot_token, created_at, email_address, "
        "name, password_digest, role, status, updated_at) VALUES (?, NULL, "
        "NULL, '2026-09-26 12:00:00.000000', NULL, ?, NULL, ?, 0, "
        "'2026-09-26 12:00:00.000000')";
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    bind_text(stmt, 2, name);
    sqlite3_bind_int(stmt, 3, role);
    exec_stmt(db, sql, stmt);
}

static void seed_room(cf_db *db, int64_t id, const char *name) {
    static const char sql[] =
        "INSERT INTO rooms (id, created_at, creator_id, name, type, "
        "updated_at) VALUES (?, '2026-09-26 12:00:00.000000', ?, ?, "
        "'Rooms::Closed', '2026-09-26 12:00:00.000000')";
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, KEVIN);
    bind_text(stmt, 3, name);
    exec_stmt(db, sql, stmt);
}

static void seed_membership(cf_db *db, int64_t id, int64_t room_id,
                            int64_t user_id) {
    static const char sql[] =
        "INSERT INTO memberships (id, created_at, room_id, updated_at, "
        "user_id) VALUES (?, '2026-09-26 12:00:00.000000', ?, "
        "'2026-09-26 12:00:00.000000', ?)";
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, room_id);
    sqlite3_bind_int64(stmt, 3, user_id);
    exec_stmt(db, sql, stmt);
}

static void seed_message(cf_db *db) {
    static const char sql[] =
        "INSERT INTO messages (id, client_message_id, created_at, "
        "creator_id, room_id, updated_at) VALUES (1000, 'm1', "
        "'2026-09-26 13:01:07.148296', 12, 100, "
        "'2026-09-26 13:01:07.148296')";
    exec_sql(db, sql);
    static const char rich[] =
        "INSERT INTO action_text_rich_texts (id, body, created_at, name, "
        "record_id, record_type, updated_at) VALUES (1, "
        "'<div>hello boosts</div>', '2026-09-26 13:01:07.148296', 'body', "
        "1000, 'Message', '2026-09-26 13:01:07.148296')";
    exec_sql(db, rich);
    static const char fts[] =
        "INSERT INTO message_search_index (rowid, body) VALUES (1000, "
        "'hello boosts')";
    exec_sql(db, fts);
}

static void seed_boost(cf_db *db, int64_t id, int64_t message_id,
                       int64_t booster_id, const char *content) {
    static const char sql[] =
        "INSERT INTO boosts (id, booster_id, content, created_at, "
        "message_id, updated_at) VALUES (?, ?, ?, "
        "'2026-09-26 13:02:00.000000', ?, '2026-09-26 13:02:00.000000')";
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, booster_id);
    bind_text(stmt, 3, content);
    sqlite3_bind_int64(stmt, 4, message_id);
    exec_stmt(db, sql, stmt);
}

static void seed_base(boosts_env *env) {
    seed_account(env->scratch.db);
    seed_user(env->scratch.db, DAVID, "David", 1);
    seed_user(env->scratch.db, KEVIN, "Kevin", 0);
    seed_room(env->scratch.db, ROOM, "Designers");
    seed_membership(env->scratch.db, 901, ROOM, DAVID);
    seed_membership(env->scratch.db, 902, ROOM, KEVIN);
    seed_message(env->scratch.db);
    /* Exists, but neither user is a member. */
    seed_room(env->scratch.db, OTHER_ROOM, "Private");
}

/* A session row whose last_active_at is in the future, so restoring it never
 * needs the writer. */
static void make_session_cookie(boosts_env *env, int64_t user_id, char *out,
                                size_t cap) {
    char token[64];
    snprintf(token, sizeof token, "session-%lld-%d", (long long)user_id,
             env->session_seq++);
    char sql[256];
    snprintf(sql, sizeof sql,
             "INSERT INTO sessions (created_at,last_active_at,token,"
             "updated_at,user_id) VALUES "
             "('2040-01-01 00:00:00.000000','2040-01-01 00:00:00.000000',"
             "'%s','2040-01-01 00:00:00.000000',%lld)",
             token, (long long)user_id);
    exec_sql(env->scratch.db, sql);
    const cf_config *config = cf_app_config(env->app);
    cf_str signed_value = {0};
    CF_REQUIRE(cf_auth_signed_cookie_generate(
                   (cf_span){(const unsigned char *)config->secret_key_base,
                             config->secret_key_base_len},
                   SP("session_token"), SP(token), false, 0,
                   &signed_value) == CF_OK);
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, cap - pos, "session_token=");
    for (size_t i = 0; i < signed_value.len && pos + 4 < cap; i++) {
        unsigned char c = (unsigned char)signed_value.ptr[i];
        if (c == '+') {
            pos += (size_t)snprintf(out + pos, cap - pos, "%%2B");
        } else if (c == '/') {
            pos += (size_t)snprintf(out + pos, cap - pos, "%%2F");
        } else if (c == '=') {
            pos += (size_t)snprintf(out + pos, cap - pos, "%%3D");
        } else {
            out[pos++] = (char)c;
        }
    }
    out[pos < cap ? pos : cap - 1] = '\0';
    cf_str_dispose(&signed_value);
}

/* --- requests -------------------------------------------------------------- */

static bool run_request(boosts_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void req_get(cf_request *req, const char *path, const char *cookie) {
    cf_test_req_init(req);
    req->path = SP(path);
    req->target = SP(path);
    if (cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    }
}

static void req_form(cf_request *req, cf_method method, const char *path,
                     const char *body, const char *cookie,
                     const char *sec_fetch_site) {
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
                                  SP(sec_fetch_site)) == CF_OK);
    if (cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    }
}

static void req_delete(cf_request *req, const char *path,
                     const char *cookie) {
    cf_test_req_init(req);
    req->method = CF_DELETE;
    req->original_method = CF_DELETE;
    req->path = SP(path);
    req->target = SP(path);
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

/* --- broadcast capture ----------------------------------------------------- */

typedef struct {
    ct_run run;
    cf_cable_request request;
    cf_header headers[1];
    char cookie[2048];
    char identifier[1024];
} boosts_conn;

static void conn_identifier(boosts_env *env, int64_t room_id, char *out,
                            size_t cap) {
    cf_room room = {0};
    bool found = false;
    CF_REQUIRE(cf_room_find_by_id(env->scratch.db, room_id, &found, &room) ==
               CF_OK);
    CF_REQUIRE(found);
    cf_str gid = {0};
    CF_REQUIRE(cf_cable_room_gid_param(&room, &gid) == CF_OK);
    cf_span parts[2] = {{(const unsigned char *)gid.ptr, gid.len},
                        {(const unsigned char *)"messages", 8}};
    const cf_config *config = cf_app_config(env->app);
    cf_str signed_name = {0};
    CF_REQUIRE(cf_auth_turbo_signed_stream_name(
                   (cf_span){(const unsigned char *)config->secret_key_base,
                             config->secret_key_base_len},
                   parts, 2, &signed_name) == CF_OK);
    snprintf(out, cap,
             "{\"channel\":\"RoomMessagesChannel\","
             "\"signed_stream_name\":\"%.*s\"}",
             (int)signed_name.len, signed_name.ptr);
    cf_str_dispose(&signed_name);
    cf_str_dispose(&gid);
    cf_room_dispose(&room);
}

static bool conn_open(boosts_env *env, int64_t user_id, boosts_conn *c) {
    memset(c, 0, sizeof *c);
    make_session_cookie(env, user_id, c->cookie, sizeof c->cookie);
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

static bool conn_next_text(boosts_conn *c, char *buf, size_t cap,
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

static bool conn_subscribe(boosts_env *env, boosts_conn *c, int64_t room_id) {
    conn_identifier(env, room_id, c->identifier, sizeof c->identifier);
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
    if (!conn_next_text(c, got, sizeof got, 5000)) {
        fprintf(stderr, "  no subscribe confirmation\n");
        return false;
    }
    return strstr(got, "\"type\":\"confirm_subscription\"") != NULL;
}

static bool conn_expect_payload(boosts_conn *c, cf_span payload,
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
    if (!conn_next_text(c, got, sizeof got, timeout_ms)) {
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

static void conn_close(boosts_conn *c) {
    ct_run_finish(&c->run);
    ct_run_join(&c->run);
}

/* The exact boost partial the create broadcast carries, rendered through
 * A02's presenter and renderer. */
/* views.h exposes only the vector disposal for boosts; a single boost
 * view owns the same fields (content plus the booster user view). */
static void test_boost_dispose(cf_view_boost *view) {
    cf_str_dispose(&view->content);
    cf_view_user_dispose(&view->booster);
    memset(view, 0, sizeof *view);
}

static cf_err render_boost_partial(boosts_env *env, int64_t boost_id,
                                   cf_builder *out) {
    cf_ctx ctx = {0};
    ctx.app = env->app;
    ctx.reader = env->scratch.db;
    cf_view_ctx view_ctx = {0};
    cf_test_views_assets_ctx(&view_ctx);
    cf_boost boost = {0};
    cf_err rc = cf_boost_find(env->scratch.db, boost_id, &boost);
    cf_view_boost view = {0};
    if (rc == CF_OK) rc = cf_presenter_boost(&ctx, &boost, &view);
    if (rc == CF_OK) rc = cf_view_boost_partial(&view_ctx, &view, out);
    test_boost_dispose(&view);
    cf_boost_dispose(&boost);
    return rc;
}

/* --- acceptance: routes ---------------------------------------------------- */

CF_TEST(messages_boosts_route_ids_bind_the_actions) {
    boosts_env env;
    CF_REQUIRE(env_open(&env));
    CF_CHECK(cf_route_action(129) == cf_action_messages_boosts_index);
    CF_CHECK(cf_route_action(130) == cf_action_messages_boosts_create);
    CF_CHECK(cf_route_action(131) == cf_action_messages_boosts_new);
    CF_CHECK(cf_route_action(136) == cf_action_messages_boosts_destroy);
    env_close(&env);
}

/* --- acceptance: index/new ------------------------------------------------- */

CF_TEST(messages_boosts_index_renders_the_message_and_its_boosts) {
    boosts_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_boost(env.scratch.db, 500, MESSAGE, DAVID, "Nice!");
    char cookie[2048];
    make_session_cookie(&env, KEVIN, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_get(&req, "/messages/1000/boosts", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    CF_CHECK(body_contains(&resp, "Nice!"));
    CF_CHECK(body_contains(&resp, "boosts_message_m1"));
    cf_response_dispose(&resp);

    /* A Turbo-Frame request renders the frame layout. */
    req_get(&req, "/messages/1000/boosts", cookie);
    CF_REQUIRE(cf_test_req_header(&req, SP("Turbo-Frame"), SP("boosts")) ==
               CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "Nice!"));
    CF_CHECK(body_contains(&resp, "<turbo-frame"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(messages_boosts_index_scope_and_format_failures) {
    boosts_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    char cookie[2048];
    make_session_cookie(&env, KEVIN, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    /* Unknown message. */
    req_get(&req, "/messages/9999/boosts", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    /* Reachable-message scope, not any room's: the other room's message
     * would 404 here the same way (no membership, no reach). */
    req_get(&req, "/messages/abc/boosts", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    /* Unauthenticated. */
    req_get(&req, "/messages/1000/boosts", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    /* Unacceptable format. */
    req_get(&req, "/messages/1000/boosts", cookie);
    CF_REQUIRE(cf_test_req_header(&req, SP("Accept"),
                                  SP("application/xml")) == CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(messages_boosts_new_renders_the_boost_form) {
    boosts_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    char cookie[2048];
    make_session_cookie(&env, DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_get(&req, "/messages/1000/boosts/new", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    CF_CHECK(body_contains(&resp, "boost__form"));
    CF_CHECK(body_contains(&resp, "action=\"/messages/1000/boosts\""));
    cf_response_dispose(&resp);

    /* Unknown message. */
    req_get(&req, "/messages/9999/boosts/new", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- acceptance: create ---------------------------------------------------- */

CF_TEST(messages_boosts_create_redirects_and_broadcasts) {
    boosts_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    char cookie[2048];
    make_session_cookie(&env, DAVID, cookie, sizeof cookie);

    boosts_conn conn;
    CF_REQUIRE(conn_open(&env, KEVIN, &conn));
    CF_REQUIRE(conn_subscribe(&env, &conn, ROOM));

    cf_request req;
    cf_response resp;
    req_form(&req, CF_POST, "/messages/1000/boosts", "boost[content]=Great",
             cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/messages/1000/boosts\r\n"));
    cf_response_dispose(&resp);

    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM boosts WHERE message_id=1000 "
                        "AND booster_id=11") == 1);
    int64_t boost_id = count_rows(
        env.scratch.db, "SELECT id FROM boosts WHERE message_id=1000");
    CF_REQUIRE(boost_id > 0);

    /* The exact broadcast frame: the boost partial appended to
     * boosts_message_m1. */
    cf_builder html = {0};
    CF_REQUIRE(render_boost_partial(&env, boost_id, &html) == CF_OK);
    char target[128];
    snprintf(target, sizeof target, "boosts_message_m1");
    cf_builder payload = {0};
    CF_REQUIRE(cf_broadcast_action_tag(
                   &payload, SP("append"), true, SP(target), true,
                   (cf_span){html.ptr, html.len}, true) == CF_OK);
    CF_REQUIRE(conn_expect_payload(&conn, (cf_span){payload.ptr, payload.len},
                                   5000));
    cf_builder_dispose(&payload);
    cf_builder_dispose(&html);
    conn_close(&conn);
    env_close(&env);
}

CF_TEST(messages_boosts_create_failures_leave_no_rows) {
    boosts_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    char cookie[2048];
    make_session_cookie(&env, DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    /* A missing `boost` param is ParameterMissing (400). */
    req_form(&req, CF_POST, "/messages/1000/boosts", "other=1", cookie,
             "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 400);
    cf_response_dispose(&resp);
    /* A missing content violates NOT NULL (the reference's 500). */
    req_form(&req, CF_POST, "/messages/1000/boosts", "boost[other]=1", cookie,
             "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    /* Unknown message (404, through the reachable scope). */
    req_form(&req, CF_POST, "/messages/9999/boosts", "boost[content]=x",
             cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    /* A user without reach to the message gets the same 404. */
    exec_sql(env.scratch.db,
             "DELETE FROM memberships WHERE user_id=11 AND room_id=100");
    req_form(&req, CF_POST, "/messages/1000/boosts", "boost[content]=x",
             cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    exec_sql(env.scratch.db,
             "INSERT INTO memberships (id, created_at, room_id, updated_at, "
             "user_id) VALUES (903, '2026-09-26 12:00:00.000000', 100, "
             "'2026-09-26 12:00:00.000000', 11)");
    /* Cross-site POST is 422. */
    req_form(&req, CF_POST, "/messages/1000/boosts", "boost[content]=x",
             cookie, "cross-site");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    cf_response_dispose(&resp);
    /* Unauthenticated. */
    req_form(&req, CF_POST, "/messages/1000/boosts", "boost[content]=x", NULL,
             "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);

    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM boosts") == 0);
    env_close(&env);
}

/* --- acceptance: destroy --------------------------------------------------- */

CF_TEST(messages_boosts_destroy_removes_the_row_and_broadcasts) {
    boosts_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_boost(env.scratch.db, 500, MESSAGE, DAVID, "Nice!");
    char cookie[2048];
    make_session_cookie(&env, DAVID, cookie, sizeof cookie);

    boosts_conn conn;
    CF_REQUIRE(conn_open(&env, KEVIN, &conn));
    CF_REQUIRE(conn_subscribe(&env, &conn, ROOM));

    cf_request req;
    cf_response resp;
    req_delete(&req, "/messages/1000/boosts/500", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 204);
    cf_response_dispose(&resp);

    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM boosts") == 0);

    /* The exact remove frame for boost_500. */
    cf_builder payload = {0};
    CF_REQUIRE(cf_broadcast_action_tag(&payload, SP("remove"), true,
                                       SP("boost_500"), false, SP(""),
                                       false) == CF_OK);
    CF_REQUIRE(conn_expect_payload(&conn, (cf_span){payload.ptr, payload.len},
                                   5000));
    cf_builder_dispose(&payload);
    conn_close(&conn);
    env_close(&env);
}

CF_TEST(messages_boosts_destroy_scope_failures) {
    boosts_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    /* Kevin's boost: David (admin, but not the booster) cannot destroy it
     * through this controller (find_by! scoped to the booster). */
    seed_boost(env.scratch.db, 500, MESSAGE, KEVIN, "Mine");
    char cookie[2048];
    make_session_cookie(&env, DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_delete(&req, "/messages/1000/boosts/500", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    /* Unknown boost. */
    req_delete(&req, "/messages/1000/boosts/9999", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    /* Unknown message. */
    req_delete(&req, "/messages/9999/boosts/500", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    /* Unauthenticated. */
    req_delete(&req, "/messages/1000/boosts/500", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);

    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM boosts") == 1);
    env_close(&env);
}

CF_TEST_MAIN()
