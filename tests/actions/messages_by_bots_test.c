/* tests/actions/messages_by_bots_test.c — A-messages-by_bots acceptance:
 * `messages/by_bots#index` (route ID 86), `#create` (87), `#update` (88,
 * 89) and `#destroy` (90), per
 * docs/devel/implementation/contracts/controller-packets.md
 * "A-messages-by_bots" and 03-application.md's bot-API boundary ("Bot API
 * endpoints authenticate active bot keys, preserve JSON serialization, and
 * choose webhook recipients according to direct-room/mention rules").
 *
 * Cases:
 *   - route binding: ids 86/87/88/89/90 resolve to the four actions (88
 *     and 89 share the update symbol);
 *   - index success: 200 with the by_bots index JSON (message shape,
 *     created_at, plain_text, creator, room, URL), the X-Total-Count
 *     header, and the next-page Link header when a further page exists;
 *     unknown room, wrong key and unacceptable format failures;
 *   - create success: 201 with the message Location, the stored message
 *     (bot creator, canonical body), the message_create broadcast, and
 *     webhook delivery to an active webhook bot in a direct room;
 *   - create failures: blank body without attachment is 422; unknown room
 *     is 404; all with no row effects;
 *   - update success: the JSON show for a JSON client, the room-message
 *     redirect for an HTML client, the stored body change, and the replace
 *     broadcast; a non-creator non-admin is 403; an unknown message is
 *     404;
 *   - destroy success: 204, the row is gone, and the remove broadcast; a
 *     non-creator non-admin is 403.
 *
 * The route double (tests/app/support/route_double.c) binds the five rows
 * to the real actions, so every case runs the real A00 dispatch path.
 *
 * cf.h is integrator-owned and does not yet declare this packet's symbols,
 * so the four entry points are declared here; the integrator's routes.c
 * rebind needs the same declarations (c_symbols
 * cf_action_messages_by_bots_index / _create / _update / _destroy).
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
#include "models/message.h"
#include "models/room.h"
#include "models/user.h"
#include "richtext.h"
#include "views.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"
#include "../cable/cable_testutil.h"
#include "../views/support/facts.h"

#include <dirent.h>
#include <inttypes.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

cf_err cf_action_messages_by_bots_index(cf_ctx *ctx);
cf_err cf_action_messages_by_bots_create(cf_ctx *ctx);
cf_err cf_action_messages_by_bots_update(cf_ctx *ctx);
cf_err cf_action_messages_by_bots_destroy(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="

#define DAVID INT64_C(11)
#define KEVIN INT64_C(12)
#define BOT INT64_C(7)
#define BOT2 INT64_C(8)
#define BOT_KEY "7-testtoken1"
#define ROOM INT64_C(100)
#define DIRECT INT64_C(200)

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
    cf_event events[64];
    size_t event_count;
    int session_seq;
} byb_env;

static cf_err env_capture(void *ctx, const cf_event *event) {
    byb_env *env = ctx;
    if (env->event_count < 64) env->events[env->event_count++] = *event;
    return CF_OK;
}

static const char *g_storage_override;

static bool env_open(byb_env *env) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;
    cf_config_entry entries[5] = {
        {"PUBLIC_ORIGIN", ORIGIN},
        {"SECRET_KEY_BASE", HEX64},
        {"VAPID_PUBLIC_KEY", VAPID_PUBLIC_KEY},
        {"DATABASE_PATH", env->scratch.path},
        {"STORAGE_PATH", "storage/files"},
    };
    size_t entry_count = 4;
    if (g_storage_override != NULL) {
        entries[4].name = "STORAGE_PATH";
        entries[4].value = g_storage_override;
        entry_count = 5;
    }
    if (cf_config_parse(entries, entry_count, NULL, &env->config) != CF_OK) return false;
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
    if (cf_writer_set_event_handler(env->app, CF_EVENT_DELIVER_WEBHOOK,
                                    env_capture, env) != CF_OK) {
        return false;
    }
    cf_richtext_configure(SP(HEX64));
    if (!cf_test_views_setup()) return false;

    cf_cable_config cable_config;
    memset(&cable_config, 0, sizeof cable_config);
    cable_config.app = env->app;
    cable_config.database_path = env->scratch.path;
    if (cf_cable_create(&cable_config, &env->cable) != CF_OK) return false;
    cf_app_set_cable(env->app, env->cable);

    cf_test_routes_reset();
    /* routes.json ids 86-90. */
    if (cf_test_routes_add("GET", "/rooms/:room_id/:bot_key/messages(.:format)",
                           86, cf_action_messages_by_bots_index) != CF_OK ||
        cf_test_routes_add("GET", "/rooms/:room_id/:bot_key/messages", 86,
                           cf_action_messages_by_bots_index) != CF_OK ||
        cf_test_routes_add("POST",
                           "/rooms/:room_id/:bot_key/messages(.:format)", 87,
                           cf_action_messages_by_bots_create) != CF_OK ||
        cf_test_routes_add("POST", "/rooms/:room_id/:bot_key/messages", 87,
                           cf_action_messages_by_bots_create) != CF_OK ||
        cf_test_routes_add("PATCH",
                           "/rooms/:room_id/:bot_key/messages/:id(.:format)",
                           88, cf_action_messages_by_bots_update) != CF_OK ||
        cf_test_routes_add("PATCH", "/rooms/:room_id/:bot_key/messages/:id",
                           88, cf_action_messages_by_bots_update) != CF_OK ||
        cf_test_routes_add("PUT",
                           "/rooms/:room_id/:bot_key/messages/:id(.:format)",
                           89, cf_action_messages_by_bots_update) != CF_OK ||
        cf_test_routes_add("PUT", "/rooms/:room_id/:bot_key/messages/:id",
                           89, cf_action_messages_by_bots_update) != CF_OK ||
        cf_test_routes_add("DELETE",
                           "/rooms/:room_id/:bot_key/messages/:id(.:format)",
                           90, cf_action_messages_by_bots_destroy) != CF_OK ||
        cf_test_routes_add("DELETE", "/rooms/:room_id/:bot_key/messages/:id",
                           90, cf_action_messages_by_bots_destroy) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(byb_env *env) {
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

static void seed_user(cf_db *db, int64_t id, const char *name, int role,
                      const char *token) {
    static const char sql[] =
        "INSERT INTO users (id, bio, bot_token, created_at, email_address, "
        "name, password_digest, role, status, updated_at) VALUES (?, NULL, "
        "?, '2026-09-26 12:00:00.000000', NULL, ?, NULL, ?, 0, "
        "'2026-09-26 12:00:00.000000')";
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    bind_text(stmt, 2, token);
    bind_text(stmt, 3, name);
    sqlite3_bind_int(stmt, 4, role);
    exec_stmt(db, sql, stmt);
}

static void seed_room(cf_db *db, int64_t id, const char *name,
                      const char *type) {
    static const char sql[] =
        "INSERT INTO rooms (id, created_at, creator_id, name, type, "
        "updated_at) VALUES (?, '2026-09-26 12:00:00.000000', ?, ?, ?, "
        "'2026-09-26 12:00:00.000000')";
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, DAVID);
    bind_text(stmt, 3, name);
    bind_text(stmt, 4, type);
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

/* Distinct timestamps per message: paging compares created_at. */
static void seed_message_at(cf_db *db, int64_t id, const char *client,
                            int64_t room_id, int64_t creator, const char *body,
                            const char *plain, const char *at) {
    static const char sql[] =
        "INSERT INTO messages (id, client_message_id, created_at, "
        "creator_id, room_id, updated_at) VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    bind_text(stmt, 2, client);
    bind_text(stmt, 3, at);
    sqlite3_bind_int64(stmt, 4, creator);
    sqlite3_bind_int64(stmt, 5, room_id);
    bind_text(stmt, 6, at);
    exec_stmt(db, sql, stmt);
    static const char rich[] =
        "INSERT INTO action_text_rich_texts (id, body, created_at, name, "
        "record_id, record_type, updated_at) VALUES (?, ?, ?, 'body', ?, "
        "'Message', ?)";
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), rich, -1, &stmt, NULL) ==
               SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    bind_text(stmt, 2, body);
    bind_text(stmt, 3, at);
    sqlite3_bind_int64(stmt, 4, id);
    bind_text(stmt, 5, at);
    exec_stmt(db, rich, stmt);
    static const char fts[] =
        "INSERT INTO message_search_index (rowid, body) VALUES (?, ?)";
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), fts, -1, &stmt, NULL) ==
               SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    bind_text(stmt, 2, plain);
    exec_stmt(db, fts, stmt);
}

static void seed_message(cf_db *db, int64_t id, const char *client,
                         int64_t room_id, int64_t creator, const char *body,
                         const char *plain) {
    seed_message_at(db, id, client, room_id, creator, body, plain,
                    "2026-09-26 13:01:07.148296");
}

static void seed_base(byb_env *env) {
    seed_account(env->scratch.db);
    seed_user(env->scratch.db, DAVID, "David", 1, NULL);
    seed_user(env->scratch.db, KEVIN, "Kevin", 0, NULL);
    seed_user(env->scratch.db, BOT, "Helper", 2, "testtoken1");
    seed_room(env->scratch.db, ROOM, "Designers", "Rooms::Closed");
    seed_membership(env->scratch.db, 901, ROOM, DAVID);
    seed_membership(env->scratch.db, 902, ROOM, KEVIN);
    seed_membership(env->scratch.db, 903, ROOM, BOT);
    seed_message(env->scratch.db, 1000, "m1", ROOM, DAVID,
                 "<div>bot index</div>", "bot index");
}

/* A direct room with a second webhook bot, for the delivery case. */
static void seed_direct(byb_env *env) {
    seed_user(env->scratch.db, BOT2, "Hook", 2, "testtoken2");
    seed_room(env->scratch.db, DIRECT, NULL, "Rooms::Direct");
    seed_membership(env->scratch.db, 904, DIRECT, BOT);
    seed_membership(env->scratch.db, 905, DIRECT, BOT2);
    static const char sql[] =
        "INSERT INTO webhooks (id, created_at, updated_at, url, user_id) "
        "VALUES (1, '2026-09-26 12:00:00.000000', "
        "'2026-09-26 12:00:00.000000', 'https://example.com/hook', 8)";
    exec_sql(env->scratch.db, sql);
}

static void make_session_cookie(byb_env *env, int64_t user_id, char *out,
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

static bool run_request(byb_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void req_bot(cf_request *req, cf_method method, const char *path,
                    const char *query, const char *content_type,
                    const char *body, const char *accept) {
    static char target[1024];
    cf_test_req_init(req);
    req->method = method;
    req->original_method = method;
    req->path = SP(path);
    if (query != NULL && query[0] != '\0') {
        snprintf(target, sizeof target, "%s?%s", path, query);
        req->target = SP(target);
        req->query = SP(target + strlen(path) + 1);
    } else {
        req->target = SP(path);
    }
    req->body = SP(body);
    if (content_type != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                      SP(content_type)) == CF_OK);
    }
    if (accept != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Accept"), SP(accept)) ==
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

/* --- acceptance: routes ---------------------------------------------------- */

CF_TEST(messages_by_bots_route_ids_bind_the_actions) {
    byb_env env;
    CF_REQUIRE(env_open(&env));
    CF_CHECK(cf_route_action(86) == cf_action_messages_by_bots_index);
    CF_CHECK(cf_route_action(87) == cf_action_messages_by_bots_create);
    CF_CHECK(cf_route_action(88) == cf_action_messages_by_bots_update);
    CF_CHECK(cf_route_action(89) == cf_action_messages_by_bots_update);
    CF_CHECK(cf_route_action(90) == cf_action_messages_by_bots_destroy);
    env_close(&env);
}

/* --- acceptance: index ----------------------------------------------------- */

CF_TEST(messages_by_bots_index_lists_json_with_pagination_headers) {
    byb_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_message_at(env.scratch.db, 1001, "m2", ROOM, KEVIN,
                    "<div>second</div>", "second",
                    "2026-09-26 13:01:08.148296");

    cf_request req;
    cf_response resp;
    req_bot(&req, CF_GET, "/rooms/100/" BOT_KEY "/messages", NULL, NULL, "",
            "application/json");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: application/json; charset=utf-8\r\n"));
    CF_CHECK(head_contains(&resp, &req, "X-Total-Count: 2\r\n"));
    /* The message shape: id, created_at, body, creator, room, URL. */
    CF_CHECK(body_contains(&resp, "\"id\":1000"));
    CF_CHECK(body_contains(&resp, "\"created_at\":\"2026-09-26T13:01:07.148Z\""));
    CF_CHECK(body_contains(&resp, "\"plain_text\":\"bot index\""));
    CF_CHECK(body_contains(&resp, "\"creator\":{\"id\":11"));
    CF_CHECK(body_contains(&resp, "\"role\":\"administrator\""));
    CF_CHECK(body_contains(&resp, "\"room\":{\"id\":100}"));
    CF_CHECK(body_contains(&resp, "\"url\":\"" ORIGIN
                                    "/rooms/100/messages/1000\""));
    char link[512];
    /* One page only: no next link. */
    CF_CHECK(!header_value(&resp, &req, "Link: ", link, sizeof link));
    cf_response_dispose(&resp);

    /* ?after=1000 pages forward and links back only when more exist. */
    req_bot(&req, CF_GET, "/rooms/100/" BOT_KEY "/messages", "after=1000",
            NULL, "", "application/json");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "\"id\":1001"));
    CF_CHECK(head_contains(&resp, &req, "X-Total-Count: 2\r\n"));
    cf_response_dispose(&resp);

    /* A session member reaches the same route (the bot_key segment only
     * feeds bot authentication). */
    char cookie[2048];
    make_session_cookie(&env, DAVID, cookie, sizeof cookie);
    cf_test_req_init(&req);
    req.method = CF_GET;
    req.original_method = CF_GET;
    req.path = SP("/rooms/100/ignored/messages");
    req.target = SP("/rooms/100/ignored/messages");
    CF_REQUIRE(cf_test_req_header(&req, SP("Accept"),
                                  SP("application/json")) == CF_OK);
    CF_REQUIRE(cf_test_req_header(&req, SP("Cookie"), SP(cookie)) == CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "\"id\":1000"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(messages_by_bots_index_links_to_the_next_page) {
    byb_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    /* Two pages: the last page links before its first message. */
    char client[16];
    for (int i = 0; i < 42; i++) {
        char at[32];
        snprintf(client, sizeof client, "p%d", i);
        snprintf(at, sizeof at, "2026-09-26 13:02:%02d.148296", i);
        seed_message_at(env.scratch.db, 2000 + i, client, ROOM, DAVID,
                        "<div>page</div>", "page", at);
    }
    cf_request req;
    cf_response resp;
    req_bot(&req, CF_GET, "/rooms/100/" BOT_KEY "/messages", NULL, NULL, "",
            "application/json");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req, "X-Total-Count: 43\r\n"));
    char link[512];
    CF_REQUIRE(header_value(&resp, &req, "Link: ", link, sizeof link));
    CF_CHECK(span_contains(SP(link), "?before="));
    CF_CHECK(span_contains(SP(link), "/rooms/100/" BOT_KEY "/messages"));
    CF_CHECK(span_contains(SP(link), "rel=\"next\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(messages_by_bots_index_failures) {
    byb_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    cf_request req;
    cf_response resp;
    /* Unknown room. */
    req_bot(&req, CF_GET, "/rooms/999/" BOT_KEY "/messages", NULL, NULL, "",
            "application/json");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    /* Wrong key: the sign-in redirect. */
    req_bot(&req, CF_GET, "/rooms/100/7-wrong/messages", NULL, NULL, "",
            "application/json");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    /* Unacceptable format. */
    req_bot(&req, CF_GET, "/rooms/100/" BOT_KEY "/messages", NULL, NULL, "",
            "application/xml");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- acceptance: create ---------------------------------------------------- */

CF_TEST(messages_by_bots_create_stores_broadcasts_and_delivers) {
    byb_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_direct(&env);

    cf_request req;
    cf_response resp;
    /* The bot posts to the direct room: the other active bot has a webhook,
     * so delivery is enqueued. */
    req_bot(&req, CF_POST, "/rooms/200/" BOT_KEY "/messages", NULL,
            "text/plain", "Ping hook", "application/json");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 201);
    char location[512];
    CF_REQUIRE(
        header_value(&resp, &req, "Location: ", location, sizeof location));
    CF_CHECK(span_contains(SP(location), ORIGIN "/rooms/200/messages/"));
    cf_response_dispose(&resp);

    int64_t id = count_rows(
        env.scratch.db,
        "SELECT id FROM messages WHERE room_id=200 AND creator_id=7");
    CF_REQUIRE(id > 0);
    /* Webhook delivery to the other bot (not the creator). */
    bool delivered = false;
    for (size_t i = 0; i < env.event_count; i++) {
        if (env.events[i].kind == CF_EVENT_DELIVER_WEBHOOK &&
            env.events[i].user_id == BOT2 && env.events[i].message_id == id) {
            delivered = true;
        }
    }
    CF_CHECK(delivered);
    env_close(&env);
}

CF_TEST(messages_by_bots_create_attachment_only_stores_no_body) {
    byb_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    cf_request req;
    cf_response resp;
    /* The attachment key present with an empty value is the Delete arm:
     * no body text is stored. */
    cf_test_req_init(&req);
    req.method = CF_POST;
    req.original_method = CF_POST;
    req.path = SP("/rooms/100/" BOT_KEY "/messages");
    req.target = SP("/rooms/100/" BOT_KEY "/messages");
    req.body = SP("attachment=");
    CF_REQUIRE(cf_test_req_header(&req, SP("Content-Type"),
                                  SP("application/x-www-form-urlencoded")) ==
               CF_OK);
    CF_REQUIRE(cf_test_req_header(&req, SP("Accept"),
                                  SP("application/json")) == CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 201);
    char location[512];
    CF_REQUIRE(
        header_value(&resp, &req, "Location: ", location, sizeof location));
    CF_CHECK(span_contains(SP(location), ORIGIN "/rooms/100/messages/"));
    cf_response_dispose(&resp);
    /* The Delete arm stores the message with no body text. */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM messages WHERE room_id=100") ==
             2);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM action_text_rich_texts WHERE "
                        "record_type='Message' AND record_id NOT IN (1000)") ==
             0);
    env_close(&env);
}

/* Recursive scratch-tree removal (S01 dirs/files are 0700/0600). */
static void remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d != NULL) {
            struct dirent *entry;
            while ((entry = readdir(d)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 ||
                    strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                char child[4096];
                snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
                remove_tree(child);
            }
            closedir(d);
        }
        (void)rmdir(path);
    } else {
        (void)unlink(path);
    }
}

/* The bot API's multipart attachment arm shares messages.c's staging:
 * POST attachment=<file> stores the blob + attachment (the reference's
 * create_with_attachment! path) and answers 201. */
CF_TEST(messages_by_bots_create_with_attachment_stages) {
    char root[64];
    snprintf(root, sizeof root, "/tmp/cf-byb-attach-XXXXXX");
    CF_REQUIRE(mkdtemp(root) != NULL);
    g_storage_override = root;
    byb_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    static const char payload[] = "bot payload";
    char body[1024];
    size_t len = 0;
    len += (size_t)snprintf(body + len, sizeof body - len,
                            "--B\r\nContent-Disposition: form-data; "
                            "name=\"attachment\"; filename=\"bot.txt\"\r\n"
                            "Content-Type: text/plain\r\n\r\n");
    memcpy(body + len, payload, sizeof payload - 1);
    len += sizeof payload - 1;
    len += (size_t)snprintf(body + len, sizeof body - len,
                            "\r\n--B--\r\n");

    cf_request req;
    cf_response resp;
    cf_test_req_init(&req);
    req.method = CF_POST;
    req.original_method = CF_POST;
    req.path = SP("/rooms/100/" BOT_KEY "/messages");
    req.target = req.path;
    req.body = (cf_span){(const unsigned char *)body, len};
    CF_REQUIRE(cf_test_req_header(&req, SP("Content-Type"),
                                  SP("multipart/form-data; boundary=B")) ==
               CF_OK);
    CF_REQUIRE(cf_test_req_header(&req, SP("Accept"), SP("application/json")) ==
               CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 201);
    cf_response_dispose(&resp);

    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_blobs") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_attachments "
                        "WHERE name='attachment' AND record_type='Message'") ==
             1);
    char text[256];
    CF_CHECK(cf_db_test_text(cf_db_handle(env.scratch.db),
                             "SELECT content_type FROM active_storage_blobs",
                             text, sizeof text) != NULL &&
             strcmp(text, "text/plain") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT byte_size FROM active_storage_blobs") == 11);

    env_close(&env);
    remove_tree(root);
    g_storage_override = NULL;
}

CF_TEST(messages_by_bots_create_blank_body_is_unprocessable) {    byb_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    cf_request req;
    cf_response resp;
    req_bot(&req, CF_POST, "/rooms/100/" BOT_KEY "/messages", NULL,
            "text/plain", "  ", "application/json");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM messages WHERE room_id=100") ==
             1);
    env_close(&env);
}

/* --- acceptance: update ---------------------------------------------------- */

CF_TEST(messages_by_bots_update_json_and_html_clients) {
    byb_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    seed_message(env.scratch.db, 1001, "m2", ROOM, BOT,
                 "<div>mine</div>", "mine");

    cf_request req;
    cf_response resp;
    /* The creator bot updates with a JSON client: the show JSON. */
    req_bot(&req, CF_PATCH, "/rooms/100/" BOT_KEY "/messages/1001", NULL,
            "text/plain", "Edited", "application/json");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "\"plain_text\":\"Edited\""));
    CF_CHECK(body_contains(&resp, "\"id\":1001"));
    cf_response_dispose(&resp);

    /* An HTML client is redirected to the room message. */
    req_bot(&req, CF_PUT, "/rooms/100/" BOT_KEY "/messages/1001", NULL,
            "text/plain", "Edited again", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/rooms/100/messages/1001\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(messages_by_bots_update_forbidden_and_not_found) {
    byb_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    cf_request req;
    cf_response resp;
    /* Kevin's bot-key-less message is not administrable by the bot. */
    req_bot(&req, CF_PATCH, "/rooms/100/" BOT_KEY "/messages/1000", NULL,
            "text/plain", "Hijack", "application/json");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    /* Unknown message. */
    req_bot(&req, CF_PATCH, "/rooms/100/" BOT_KEY "/messages/9999", NULL,
            "text/plain", "Hijack", "application/json");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- acceptance: destroy --------------------------------------------------- */

CF_TEST(messages_by_bots_destroy_removes_the_row) {
    byb_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_message(env.scratch.db, 1001, "m2", ROOM, BOT, "<div>bye</div>",
                 "bye");

    cf_request req;
    cf_response resp;
    /* A 403 first: David's message is not the bot's to destroy. */
    req_bot(&req, CF_DELETE, "/rooms/100/" BOT_KEY "/messages/1000", NULL,
            NULL, "", "application/json");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    /* The bot's own message goes away with 204. */
    req_bot(&req, CF_DELETE, "/rooms/100/" BOT_KEY "/messages/1001", NULL,
            NULL, "", "application/json");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 204);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM messages WHERE id=1001") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM messages WHERE id=1000") == 1);
    env_close(&env);
}

CF_TEST_MAIN()
