/* tests/actions/rooms_test.c — A-rooms acceptance: `rooms#show` (route IDs
 * 96, 101), `rooms#index` (97) and `rooms#destroy` (104)
 * (docs/devel/implementation/contracts/controller-packets.md "A-rooms";
 * 03-application.md "First complete slice" rooms#show row).
 *
 * Every case runs the real A00 dispatch path (params, cookies, before-actions,
 * format negotiation) through the route double
 * (tests/app/support/route_double.c), which binds rows 96/97/101/104 to the
 * three actions.
 *
 * Matrix from the card: success, unauthorized/forbidden/not-found, parameter
 * failure and state/side-effect assertions.  `rooms#destroy` additionally
 * performs the source's post-commit `broadcast_remove_to :rooms, target:
 * [@room, :list]` through the app->cable accessor (cf_app_set_cable /
 * cf_app_cable in src/app.h), and the broadcast case asserts the exact
 * delivered Turbo frame on a subscribed cable connection.  The render
 * comparisons use the A02 golden/b rooms fixtures through their own runner
 * (assert_dom), with one named mask: the three platform-dependent
 * `details.notifications-help` nodes of the bell are dropped from both sides
 * because these requests carry no User-Agent, so A01's parser stores no
 * platform and the action's layout has zeroed platform facts while the
 * fixtures are Chrome/macOS.  (Since the A01 completion pass a real UA would
 * fill those facts; the fixtures' own UA is not replayed here.)  No other mask
 * exists.
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
#include "models/message.h"
#include "models/room.h"
#include "models/user.h"
#include "richtext.h"
#include "views.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"
#include "../cable/cable_testutil.h"
#include "../views/support/golden_b.h"

#include <limits.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ORIGIN "http://campfire.test"

/* The golden/b reference environment (reference-tools/views/b/run.sh): the
 * fixtures were rendered with this SECRET_KEY_BASE, so the signed avatar and
 * stream names in them only reproduce with it. */
#define ROOMS_SECRET \
    "views-b-secret-key-base-0123456789abcdef0123456789abcdef"
/* golden/a facts.json root vapid_public_key (the same reference env). */
#define ROOMS_VAPID_PUBLIC_KEY                                               \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="

/* routes.json rows 96, 97, 101, 104.  cf.h is frozen and no packet-action
 * header exists yet, so the test declares them (the integrator's routes.c
 * rebind needs the same declarations; reported in the evidence). */
cf_err cf_action_rooms_show(cf_ctx *ctx);
cf_err cf_action_rooms_index(cf_ctx *ctx);
cf_err cf_action_rooms_destroy(cf_ctx *ctx);

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
} rooms_env;

/* The C config validator (F03) requires SECRET_KEY_BASE to be hexadecimal,
 * but the golden/b reference environment used a non-hex key
 * (reference-tools/views/b/run.sh); parsing with a hex placeholder and then
 * replacing the key material reproduces the fixtures' signatures exactly.
 * This is the same kind of test-side injection as
 * cf_config_test_set_bcrypt_cost; the production path is unchanged. */
static bool env_open(rooms_env *env) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) {
        fprintf(stderr, "  env_open: scratch open failed\n");
        return false;
    }
    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", ORIGIN},
        {"SECRET_KEY_BASE",
         "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
        {"VAPID_PUBLIC_KEY", ROOMS_VAPID_PUBLIC_KEY},
        {"DATABASE_PATH", env->scratch.path},
    };
    if (cf_config_parse(entries, 4, NULL, &env->config) != CF_OK) {
        fprintf(stderr, "  env_open: config parse failed\n");
        return false;
    }
    {
        char *secret = strdup(ROOMS_SECRET);
        if (secret == NULL) return false;
        free(env->config->secret_key_base);
        env->config->secret_key_base = secret;
        env->config->secret_key_base_len = strlen(secret);
    }
    cf_richtext_configure(
        (cf_span){(const unsigned char *)ROOMS_SECRET,
                  sizeof ROOMS_SECRET - 1});
    /* The action renders through the real asset module (the pinned manifest,
     * whose digests match the golden/b asset map exactly); an unconfigured
     * module would fail the layout with CF_INVALID. */
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
    /* destroy's Room#destroy goes through the writer (cf_write). */
    if (cf_writer_start(env->app, env->config) != CF_OK) {
        fprintf(stderr, "  env_open: writer start failed\n");
        cf_app_destroy(env->app);
        env->app = NULL;
        cf_db_scratch_close(&env->scratch);
        return false;
    }
    /* The app->cable seam the destroy broadcast needs, exactly as main.c sets
     * it after cf_cable_create; a cable without connections is complete. */
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
    bool ok = cf_test_routes_add("GET", "/rooms/:room_id/@/:message_id", 96,
                                 cf_action_rooms_show) == CF_OK &&
              cf_test_routes_add("GET", "/rooms", 97,
                                 cf_action_rooms_index) == CF_OK &&
              cf_test_routes_add("GET", "/rooms/:id", 101,
                                 cf_action_rooms_show) == CF_OK &&
              cf_test_routes_add("DELETE", "/rooms/:id", 104,
                                 cf_action_rooms_destroy) == CF_OK;
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

static void env_close(rooms_env *env) {
    /* The cable is destroyed first (no connection loops remain); the app
     * borrows the pointer but never touches it during destroy. */
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

static void seed_user_full(cf_db *db, int64_t id, const char *name,
                           const char *bio, int role,
                           const char *updated_at) {
    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, created_at, email_address, name, "
             "role, status, updated_at) VALUES (%lld, %s, "
             "'2026-09-26 13:00:29.000000', NULL, '%s', %d, 0, '%s')",
             (long long)id, bio != NULL ? bio : "NULL", name, role,
             updated_at);
    exec_sql(db, sql);
}

static void seed_user(cf_db *db, int64_t id, const char *name, int role) {
    seed_user_full(db, id, name, NULL, role, "2026-09-26 13:00:29.000000");
}

/* type_text is the STI class name ("Rooms::Open", ...). */
static void seed_room_typed(cf_db *db, int64_t id, const char *name,
                            const char *type_text, int64_t creator_id,
                            const char *created_at, const char *updated_at) {
    char name_sql[256];
    if (name != NULL) {
        snprintf(name_sql, sizeof name_sql, "'%s'", name);
    } else {
        snprintf(name_sql, sizeof name_sql, "NULL");
    }
    char sql[768];
    snprintf(sql, sizeof sql,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (%lld, '%s', %lld, %s, '%s', '%s')",
             (long long)id, created_at, (long long)creator_id, name_sql,
             type_text, updated_at);
    exec_sql(db, sql);
}

static void seed_room(cf_db *db, int64_t id, const char *name,
                      int64_t creator_id) {
    seed_room_typed(db, id, name, "Rooms::Open", creator_id,
                    "2026-01-01 00:00:00.000000",
                    "2026-01-01 00:00:00.000000");
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

static void seed_message(cf_db *db, int64_t id, int64_t room_id,
                         int64_t creator_id, const char *client_message_id,
                         const char *at) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO messages (id, client_message_id, created_at, "
             "creator_id, room_id, updated_at) VALUES (%lld, '%s', '%s', "
             "%lld, %lld, '%s')",
             (long long)id, client_message_id, at, (long long)creator_id,
             (long long)room_id, at);
    exec_sql(db, sql);
}

static void seed_body(cf_db *db, int64_t record_id, const char *body) {
    char sql[2048];
    snprintf(sql, sizeof sql,
             "INSERT INTO action_text_rich_texts (body, created_at, name, "
             "record_id, record_type, updated_at) VALUES ('%s', "
             "'2026-01-01 00:00:00.000000', 'body', %lld, 'Message', "
             "'2026-01-01 00:00:00.000000')",
             body, (long long)record_id);
    exec_sql(db, sql);
}

static void seed_account(cf_db *db) {
    exec_sql(db,
             "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
             "name, settings, singleton_guard, updated_at) VALUES "
             "(873240054, '2026-09-26 13:03:57.000000', NULL, "
             "'CRMu-l8Ge-KB9B', '37signals', NULL, 0, "
             "'2026-09-26 13:03:57.000000')");
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

/* "session_token=<wire>" with the Rack form-escaping a base64 value needs
 * (same rule as tests/actions/welcome_test.c). */
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

static void append_cookie(char *out, size_t cap, const char *pair) {
    size_t at = strlen(out);
    snprintf(out + at, cap - at, "; %s", pair);
}

/* --- request helpers ------------------------------------------------------- */

static bool run_request(rooms_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    } else if (getenv("ROOMS_TEST_DEBUG") != NULL) {
        fprintf(stderr, "  [debug] %.*s -> status %u\n", (int)req->path.len,
                req->path.ptr, resp->status);
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

/* Authenticated GET: the session cookie for `user_id`; `cookie_extra` is
 * appended to the header when non-NULL.  The method/path are set by
 * `prepare`.  `header` is the caller's storage: the request keeps spans, not
 * copies, so it must outlive the request. */
static void authenticated_request(rooms_env *env, int64_t user_id,
                                  const char *cookie_extra, cf_request *req,
                                  char *header, size_t cap) {
    static unsigned counter;
    char token[64];
    snprintf(token, sizeof token, "rooms-token-%u", counter++);
    make_session_cookie(env->config, env->scratch.db, header, cap, token,
                        user_id);
    if (cookie_extra != NULL) append_cookie(header, cap, cookie_extra);
    CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(header)) == CF_OK);
}

static void prepare_get(cf_request *req, const char *path) {
    cf_test_req_init(req);
    req->path = SP(path);
    req->target = req->path;
}

static void prepare_delete(cf_request *req, const char *path) {
    cf_test_req_init(req);
    req->method = CF_DELETE;
    req->original_method = CF_DELETE;
    req->path = SP(path);
    req->target = req->path;
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
    snprintf(sql, sizeof sql, "SELECT count(*) FROM %s WHERE %s", table,
             where);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    int64_t count = sqlite3_step(stmt) == SQLITE_ROW
                        ? sqlite3_column_int64(stmt, 0)
                        : -1;
    sqlite3_finalize(stmt);
    return count;
}

/* --- route IDs ------------------------------------------------------------- */

CF_TEST(rooms_route_ids_bind_the_actions) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    CF_CHECK(cf_route_action(96) == cf_action_rooms_show);
    CF_CHECK(cf_route_action(101) == cf_action_rooms_show);
    CF_CHECK(cf_route_action(97) == cf_action_rooms_index);
    CF_CHECK(cf_route_action(104) == cf_action_rooms_destroy);
    env_close(&env);
}

/* --- rooms#show ------------------------------------------------------------ */

CF_TEST(rooms_show_unauthenticated_is_redirected_to_sign_in) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", 0);

    cf_request req;
    prepare_get(&req, "/rooms/42");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(response_head_contains(&resp, &req,
                                    "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Unknown room id, a room the user is not a member of, and unparsable or
 * missing ids all take set_room's failure arm: root redirect with the alert. */
CF_TEST(rooms_show_not_found_or_inaccessible_redirects_to_root) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", 0);
    seed_room(env.scratch.db, 42, "HQ", 1);
    seed_membership(env.scratch.db, 201, 42, 1);
    seed_room(env.scratch.db, 99, "Other", 2);
    seed_membership(env.scratch.db, 202, 99, 2);

    static const struct {
        const char *path;
        const char *cookie;
    } cases[] = {
        {"/rooms/12345", NULL}, /* no such room */
        {"/rooms/99", NULL},    /* exists, not a member */
        {"/rooms/abc", NULL},   /* integer_cast fails -> nil */
        {"/rooms/0x2a", NULL},  /* String#to_i stops at 'x' -> 0 */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cf_request req;
        prepare_get(&req, cases[i].path);
        char auth_header_433[4096];
    authenticated_request(&env, 1, cases[i].cookie, &req, auth_header_433, sizeof auth_header_433);
        cf_response resp;
        CF_REQUIRE(run_request(&env, &req, &resp));
        CF_CHECK(resp.status == 302);
        CF_CHECK(response_head_contains(&resp, &req,
                                        "Location: " ORIGIN "/\r\n"));
        cf_response_dispose(&resp);
    }
    env_close(&env);
}

/* The alert is set before the redirect (the reference's `redirect_to_with`).
 * A01's session commit does not persist the flash map yet, so the observable
 * is the context's flash map; see the evidence file. */
CF_TEST(rooms_show_failure_sets_the_room_alert) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", 0);
    seed_room(env.scratch.db, 42, "HQ", 1);
    seed_membership(env.scratch.db, 201, 42, 1);

    cf_request req;
    prepare_get(&req, "/rooms/12345");
    char auth_header_456[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header_456, sizeof auth_header_456);
    cf_response resp;
    cf_response_init(&resp);
    cf_ctx ctx;
    CF_REQUIRE(cf_ctx_create(&ctx, env.app, env.scratch.db, &req, &resp) ==
               CF_OK);
    CF_REQUIRE(cf_action_rooms_show(&ctx) == CF_OK);
    CF_CHECK(cf_auth_halted(&ctx));
    cf_span alert = {NULL, 0};
    CF_CHECK(cf_ctx_flash_get(&ctx, SP("alert"), &alert) == CF_OK);
    CF_CHECK(alert.len == strlen("Room not found or inaccessible") &&
             memcmp(alert.ptr, "Room not found or inaccessible",
                    alert.len) == 0);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Success: the room page, the messages, and the permanent last_room cookie
 * set because it changed. */
CF_TEST(rooms_show_renders_and_remembers_the_room) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room(env.scratch.db, 42, "HQ", 1);
    seed_membership(env.scratch.db, 201, 42, 1);
    seed_message(env.scratch.db, 100, 42, 1, "hq-1",
                 "2026-01-01 00:00:01.000000");
    seed_body(env.scratch.db, 100, "Pizza party in HQ");

    cf_request req;
    prepare_get(&req, "/rooms/42");
    char auth_header_487[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header_487, sizeof auth_header_487);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(response_head_contains(
        &resp, &req, "Content-Type: text/html; charset=utf-8\r\n"));
    CF_CHECK(response_body_contains(&resp, "HQ"));
    CF_CHECK(response_body_contains(&resp, "hq-1"));
    CF_CHECK(response_body_contains(&resp, "Pizza party in HQ"));
    CF_CHECK(response_body_contains(&resp, "turbo-cable-stream-source"));
    CF_CHECK(response_head_contains(
        &resp, &req, "last_room=42; path=/; expires="));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* The cookie is left alone when its value already equals the room id (the
 * source's guard); the session cookie is not refreshed either (the seeded
 * session's last_active_at is in the future). */
CF_TEST(rooms_show_unchanged_last_room_sets_no_cookie) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room(env.scratch.db, 42, "HQ", 1);
    seed_membership(env.scratch.db, 201, 42, 1);

    cf_request req;
    prepare_get(&req, "/rooms/42");
    char auth_header_515[4096];
    authenticated_request(&env, 1, "last_room=42", &req, auth_header_515, sizeof auth_header_515);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(!response_head_contains(&resp, &req, "Set-Cookie:"));
    cf_response_dispose(&resp);

    /* A different textual value ("042") is not equal, so it is rewritten. */
    cf_request req2;
    prepare_get(&req2, "/rooms/42");
    char auth_header_525[4096];
    authenticated_request(&env, 1, "last_room=042", &req2, auth_header_525, sizeof auth_header_525);
    cf_response resp2;
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 200);
    CF_CHECK(response_head_contains(&resp2, &req2,
                                    "Set-Cookie: last_room=42;"));
    cf_response_dispose(&resp2);
    env_close(&env);
}

/* Route 96 (`/rooms/:room_id/@:message_id`): a message of this room selects
 * the page around it; the last page otherwise. */
CF_TEST(rooms_show_message_id_selects_the_page_around) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room(env.scratch.db, 42, "HQ", 1);
    seed_membership(env.scratch.db, 201, 42, 1);
    char at[32];
    for (int i = 1; i <= 45; i++) {
        /* Nonzero microseconds, like Active Record writes: a whole-second
         * time is stored without a fraction (`quoted_date`), which would make
         * the model's bound text compare inexactly. */
        snprintf(at, sizeof at, "2026-01-01 00:00:%02d.500000", i);
        char client[16];
        snprintf(client, sizeof client, "m%02d", i);
        seed_message(env.scratch.db, 100 + i, 42, 1, client, at);
        seed_body(env.scratch.db, 100 + i, client);
    }

    /* The last page: the newest 40, so m01 is absent and m45 present. */
    cf_request last_req;
    prepare_get(&last_req, "/rooms/42");
    char auth_header_554[4096];
    authenticated_request(&env, 1, NULL, &last_req, auth_header_554, sizeof auth_header_554);
    cf_response last_resp;
    CF_REQUIRE(run_request(&env, &last_req, &last_resp));
    CF_CHECK(last_resp.status == 200);
    CF_CHECK(response_body_contains(&last_resp, "message_m45"));
    CF_CHECK(!response_body_contains(&last_resp, "message_m01"));
    cf_response_dispose(&last_resp);

    /* Around m03: up to 40 before (m01..m02), the message, up to 40 after
     * (m04..m43), so m44/m45 are absent. */
    cf_request around_req;
    prepare_get(&around_req, "/rooms/42/@/103");
    char auth_header_566[4096];
    authenticated_request(&env, 1, NULL, &around_req, auth_header_566, sizeof auth_header_566);
    cf_response around_resp;
    CF_REQUIRE(run_request(&env, &around_req, &around_resp));
    CF_CHECK(around_resp.status == 200);
    CF_CHECK(response_body_contains(&around_resp, "message_m01"));
    CF_CHECK(response_body_contains(&around_resp, "message_m03"));
    CF_CHECK(response_body_contains(&around_resp, "message_m43"));
    CF_CHECK(!response_body_contains(&around_resp, "message_m44"));
    CF_CHECK(!response_body_contains(&around_resp, "message_m45"));
    cf_response_dispose(&around_resp);

    /* A message id of another room, an unknown id and an unparsable id all
     * fall back to the last page. */
    seed_room(env.scratch.db, 77, "Other", 1);
    seed_membership(env.scratch.db, 202, 77, 1);
    seed_message(env.scratch.db, 200, 77, 1, "other-1",
                 "2026-01-01 00:01:00.000000");
    static const char *fallbacks[] = {"/rooms/42/@/200", "/rooms/42/@/999999",
                                      "/rooms/42/@/abc"};
    for (size_t i = 0; i < sizeof fallbacks / sizeof fallbacks[0]; i++) {
        cf_request req;
        prepare_get(&req, fallbacks[i]);
        char auth_header_588[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header_588, sizeof auth_header_588);
        cf_response resp;
        CF_REQUIRE(run_request(&env, &req, &resp));
        CF_CHECK(resp.status == 200);
        CF_CHECK(response_body_contains(&resp, "message_m45"));
        CF_CHECK(!response_body_contains(&resp, "message_m01"));
        cf_response_dispose(&resp);
    }
    env_close(&env);
}

/* A Turbo-Frame request renders turbo-rails' frame layout: the message area
 * and no page chrome. */
CF_TEST(rooms_show_turbo_frame_request_renders_the_frame_layout) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room(env.scratch.db, 42, "HQ", 1);
    seed_membership(env.scratch.db, 201, 42, 1);

    cf_request req;
    prepare_get(&req, "/rooms/42");
    char auth_header_610[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header_610, sizeof auth_header_610);
    CF_REQUIRE(cf_test_req_header(&req, SP("Turbo-Frame"),
                                  SP("room_messages")) == CF_OK);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(response_body_contains(&resp, "id=\"message-area\""));
    CF_CHECK(response_body_contains(&resp, "turbo-cable-stream-source"));
    CF_CHECK(!response_body_contains(&resp, "<title>"));
    CF_CHECK(!response_body_contains(&resp, "<nav id=\"nav\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* The pinned `is_turbo_frame_request` is `header("turbo-frame")
 * .is_some_and(|id| !id.trim().is_empty())` (kit/src/ctx.rs:294); the header
 * lookup goes through http 1.5.0's `HeaderValue::to_str`, which accepts a
 * byte only when it is HTAB or visible ASCII (`b >= 32 && b < 127 ||
 * b == b'\t'`).  Any control byte, DEL or byte >= 0x80 therefore makes the
 * header read as absent (page layout), even when it is Unicode whitespace or
 * surrounds a real id; otherwise only spaces and tabs are blank.  The first
 * seven cases are the divergent vectors the previous Unicode-White_Space
 * implementation got wrong (it rendered the frame layout where the pin
 * renders the page). */
CF_TEST(rooms_show_turbo_frame_header_matches_the_pinned_predicate) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room(env.scratch.db, 42, "HQ", 1);
    seed_membership(env.scratch.db, 201, 42, 1);

    static const struct {
        cf_span value;
        bool frame;
        const char *what;
    } cases[] = {
        /* Divergences from the pinned predicate: all page-rendering now. */
        {{(const unsigned char *)"\xE2\x80\x8B", 3}, false, "U+200B only"},
        {{(const unsigned char *)"\xC3\xA9", 2}, false, "U+00E9 only"},
        {{(const unsigned char *)"\xC2\xA0room_messages\xE2\x80\x83", 18},
         false, "U+00A0/U+2003 around an id"},
        {{(const unsigned char *)"room_messages\0", 14}, false,
         "embedded NUL"},
        {{(const unsigned char *)"\0", 1}, false, "NUL only"},
        {{(const unsigned char *)"\x7F", 1}, false, "DEL only"},
        {{(const unsigned char *)"room_messages\r\n", 15}, false,
         "id + CRLF"},
        /* Boundary keeps. */
        {{(const unsigned char *)"room_messages", 13}, true, "ascii id"},
        {{(const unsigned char *)" room_messages ", 15}, true,
         "spaces around an id"},
        {{(const unsigned char *)"", 0}, false, "empty"},
        {{(const unsigned char *)" ", 1}, false, "space only"},
        {{(const unsigned char *)"\t", 1}, false, "tab only"},
        {{(const unsigned char *)"\t\t", 2}, false, "two tabs"},
        {{(const unsigned char *)" \t\r\n", 4}, false, "ascii blanks"},
        {{(const unsigned char *)"\x01", 1}, false, "0x01 only"},
        {{(const unsigned char *)"\x1F", 1}, false, "0x1F only"},
        {{(const unsigned char *)"\x0B", 1}, false, "vertical tab only"},
        {{(const unsigned char *)"\x0C", 1}, false, "form feed only"},
        {{(const unsigned char *)"~", 1}, true, "0x7E only"},
        {{(const unsigned char *)"room~messages", 13}, true,
         "0x7E inside an id"},
        {{(const unsigned char *)"\xFF", 1}, false, "invalid UTF-8"},
        {{(const unsigned char *)"\xC2\xA0", 2}, false, "U+00A0 only"},
        {{(const unsigned char *)"\xE2\x80\xA8", 3}, false, "U+2028 only"},
        {{(const unsigned char *)"\xE3\x80\x80", 3}, false, "U+3000 only"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cf_request req;
        prepare_get(&req, "/rooms/42");
        char auth_header[4096];
        authenticated_request(&env, 1, NULL, &req, auth_header,
                              sizeof auth_header);
        CF_REQUIRE(cf_test_req_header(&req, SP("Turbo-Frame"),
                                      cases[i].value) == CF_OK);
        cf_response resp;
        CF_REQUIRE(run_request(&env, &req, &resp));
        CF_CHECK(resp.status == 200);
        /* The page layout carries the document head (`<title>`); the frame
         * layout is the content area only. */
        bool frame = !response_body_contains(&resp, "<title>");
        if (frame != cases[i].frame) {
            fprintf(stderr, "  [%s] expected frame=%d\n", cases[i].what,
                    (int)cases[i].frame);
        }
        CF_CHECK(frame == cases[i].frame);
        cf_response_dispose(&resp);
    }

    /* The name match is case-insensitive and the first matching header's
     * value wins: a blank first value is not rescued by a later nonblank
     * one. */
    {
        cf_request req;
        prepare_get(&req, "/rooms/42");
        char auth_header[4096];
        authenticated_request(&env, 1, NULL, &req, auth_header,
                              sizeof auth_header);
        CF_REQUIRE(cf_test_req_header(&req, SP("TURBO-FRAME"),
                                      SP("room_messages")) == CF_OK);
        CF_REQUIRE(cf_test_req_header(
                       &req, SP("turbo-frame"),
                       (cf_span){(const unsigned char *)"\xC3\xA9", 2}) ==
                   CF_OK);
        cf_response resp;
        CF_REQUIRE(run_request(&env, &req, &resp));
        CF_CHECK(resp.status == 200);
        /* The first matching value ("room_messages") decides: frame layout,
         * not the later non-ASCII value's page layout. */
        CF_CHECK(!response_body_contains(&resp, "<title>"));
        cf_response_dispose(&resp);
    }
    env_close(&env);
}

/* A non-HTML client: ActionController::UnknownFormat, 406. */
CF_TEST(rooms_show_json_client_is_406) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room(env.scratch.db, 42, "HQ", 1);
    seed_membership(env.scratch.db, 201, 42, 1);

    cf_request req;
    prepare_get(&req, "/rooms/42");
    char auth_header_634[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header_634, sizeof auth_header_634);
    CF_REQUIRE(cf_test_req_header(&req, SP("Accept"),
                                  SP("application/json")) == CF_OK);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* deny_bots (Before::default): an authenticated bot is forbidden. */
CF_TEST(rooms_show_bot_authentication_is_forbidden) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    exec_sql(env.scratch.db,
             "INSERT INTO users (id, created_at, email_address, name, role, "
             "status, updated_at, bot_token) VALUES (7, "
             "'2026-09-26 13:00:29.000000', NULL, 'Bender Bot', 2, 0, "
             "'2026-09-26 13:00:29.000000', 'roomsbotkey')");

    cf_request req;
    prepare_get(&req, "/rooms/42?bot_key=7-roomsbotkey");
    req.query = SP("bot_key=7-roomsbotkey");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- rooms#index ----------------------------------------------------------- */

CF_TEST(rooms_index_redirects_to_the_last_room) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);
    seed_room(env.scratch.db, 42, "HQ", 1);
    seed_room(env.scratch.db, 43, "All Talk", 1);
    seed_membership(env.scratch.db, 201, 42, 1);
    seed_membership(env.scratch.db, 202, 43, 1);

    cf_request req;
    prepare_get(&req, "/rooms");
    char auth_header_677[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header_677, sizeof auth_header_677);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    /* Member of both rooms: the redirect leaves for whichever room
     * `Room::last_for_user` selects; it must be one of them and an absolute
     * URL. */
    bool to_42 = response_head_contains(&resp, &req,
                                        "Location: " ORIGIN "/rooms/42\r\n");
    bool to_43 = response_head_contains(&resp, &req,
                                        "Location: " ORIGIN "/rooms/43\r\n");
    CF_CHECK(to_42 || to_43);
    CF_CHECK(response_head_contains(
        &resp, &req, "Content-Type: text/html; charset=utf-8\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A user who is in no room: `room_url(nil)` raises (500). */
CF_TEST(rooms_index_without_rooms_is_500) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);

    cf_request req;
    prepare_get(&req, "/rooms");
    char auth_header_703[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header_703, sizeof auth_header_703);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(rooms_index_unauthenticated_is_redirected_to_sign_in) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Kevin", 0);

    cf_request req;
    prepare_get(&req, "/rooms");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(response_head_contains(&resp, &req,
                                    "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- rooms#destroy --------------------------------------------------------- */

static void destroy_fixture(rooms_env *env, int64_t creator_id,
                            int64_t member_role) {
    seed_user_full(env->scratch.db, 1, "Kevin", NULL, member_role,
                   "2026-09-26 13:00:29.000000");
    seed_user(env->scratch.db, 2, "JZ", 0);
    seed_room(env->scratch.db, 42, "HQ", creator_id);
    seed_membership(env->scratch.db, 201, 42, 1);
    seed_membership(env->scratch.db, 202, 42, 2);
    seed_message(env->scratch.db, 100, 42, 1, "hq-1",
                 "2026-01-01 00:00:01.000000");
    seed_body(env->scratch.db, 100, "Pizza party in HQ");
    seed_message(env->scratch.db, 101, 42, 2, "hq-2",
                 "2026-01-01 00:00:02.000000");
    seed_body(env->scratch.db, 101, "Second message");
}

/* --- the rooms-list cable connection (C02 test hooks) ---------------------- */

typedef struct {
    ct_run run;
    cf_cable_request request;
    cf_header headers[1];
    char cookie[1024];
    char identifier[1024];
} rooms_conn;

/* `Turbo::StreamsChannel` over the signed stream name of `["rooms"]` — the
 * stream Room#destroy's `broadcast_remove_to :rooms` publishes to
 * (channels/broadcasts.rs ROOMS), as the reference's own channel test uses. */
static void rooms_conn_identifier(rooms_conn *c) {
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

static bool rooms_conn_open(rooms_env *env, int64_t user_id, rooms_conn *c) {
    memset(c, 0, sizeof *c);
    char token[64];
    snprintf(token, sizeof token, "rooms-cable-token-%d", env->session_seq++);
    make_session_cookie(env->config, env->scratch.db, c->cookie,
                        sizeof c->cookie, token, user_id);
    c->headers[0].name = SP("Cookie");
    c->headers[0].value = SP(c->cookie);
    c->request.method = CF_GET;
    c->request.headers = c->headers;
    c->request.header_count = 1;
    cf_cable_hooks hooks;
    cf_cable_server_hooks(env->cable, &hooks);
    if (!ct_run_start_with_request(&c->run, &hooks, NULL, false, &c->request)) {
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
static bool rooms_conn_next_text(rooms_conn *c, char *buf, size_t cap,
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

static bool rooms_conn_subscribe(rooms_conn *c) {
    rooms_conn_identifier(c);
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
    if (!rooms_conn_next_text(c, got, sizeof got, 5000)) {
        fprintf(stderr, "  no subscribe confirmation\n");
        return false;
    }
    return strstr(got, "\"type\":\"confirm_subscription\"") != NULL;
}

/* Read one frame and compare it with the exact delivery frame the broadcast
 * source produces for `payload` on this subscription. */
static bool rooms_conn_expect_payload(rooms_conn *c, cf_span payload,
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
    if (!rooms_conn_next_text(c, got, sizeof got, timeout_ms)) {
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

static void rooms_conn_close(rooms_conn *c) {
    ct_run_finish(&c->run);
    ct_run_join(&c->run);
}

/* destroy's post-commit named effect: `c.app().broadcasts.room_remove(&room)`
 * (rooms.rs destroy_room) removes `[room, :list]` from everyone's `:rooms`
 * Turbo stream.  The subscription is the stock Turbo::StreamsChannel over the
 * signed `["rooms"]` stream; the delivered frame is compared byte for byte
 * with `<turbo-stream action="remove" target="list_rooms_open_42">`. */
CF_TEST(rooms_destroy_broadcasts_the_room_removal) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    destroy_fixture(&env, 1, 0);

    rooms_conn conn;
    CF_REQUIRE(rooms_conn_open(&env, 1, &conn));
    CF_REQUIRE(rooms_conn_subscribe(&conn));

    cf_request req;
    prepare_delete(&req, "/rooms/42");
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(response_head_contains(&resp, &req,
                                    "Location: " ORIGIN "/\r\n"));
    CF_CHECK(!room_row_exists(env.scratch.db, 42));
    cf_response_dispose(&resp);

    CF_CHECK(rooms_conn_expect_payload(
        &conn,
        SP("<turbo-stream action=\"remove\" "
           "target=\"list_rooms_open_42\"></turbo-stream>"),
        5000));
    rooms_conn_close(&conn);
    env_close(&env);
}

CF_TEST(rooms_destroy_by_creator_removes_the_room_and_redirects) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    destroy_fixture(&env, 1, 0);

    cf_request req;
    prepare_delete(&req, "/rooms/42");
    char auth_header_751[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header_751, sizeof auth_header_751);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(response_head_contains(&resp, &req,
                                    "Location: " ORIGIN "/\r\n"));
    CF_CHECK(!room_row_exists(env.scratch.db, 42));
    CF_CHECK(count_rows(env.scratch.db, "memberships", "room_id = 42") == 0);
    CF_CHECK(count_rows(env.scratch.db, "messages", "room_id = 42") == 0);
    CF_CHECK(count_rows(env.scratch.db, "action_text_rich_texts",
                        "record_id IN (100, 101)") == 0);
    /* The revalidated destroy commits exactly one transaction. */
    cf_writer_stats stats = {0};
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.committed == 1);
    CF_CHECK(stats.rolled_back == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(rooms_destroy_by_administrator_removes_the_room) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    destroy_fixture(&env, 2, 1 /* role: administrator */);

    cf_request req;
    prepare_delete(&req, "/rooms/42");
    char auth_header_773[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header_773, sizeof auth_header_773);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(response_head_contains(&resp, &req,
                                    "Location: " ORIGIN "/\r\n"));
    CF_CHECK(!room_row_exists(env.scratch.db, 42));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A plain member who is neither administrator nor creator: 403 head, nothing
 * deleted. */
CF_TEST(rooms_destroy_by_plain_member_is_forbidden) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    destroy_fixture(&env, 2, 0);

    cf_request req;
    prepare_delete(&req, "/rooms/42");
    char auth_header_793[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header_793, sizeof auth_header_793);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    CF_CHECK(response_head_contains(&resp, &req,
                                    "Content-Type: text/html\r\n"));
    CF_CHECK(resp.body == NULL || cf_buf_span(resp.body).len == 0);
    CF_CHECK(room_row_exists(env.scratch.db, 42));
    CF_CHECK(count_rows(env.scratch.db, "messages", "room_id = 42") == 2);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A room the user cannot reach takes set_room's arm before the permission
 * check: redirect, not 403, and nothing deleted. */
CF_TEST(rooms_destroy_inaccessible_room_redirects_to_root) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    destroy_fixture(&env, 2, 0);
    seed_room(env.scratch.db, 99, "Other", 2);
    seed_membership(env.scratch.db, 203, 99, 2);

    static const char *paths[] = {"/rooms/99", "/rooms/abc", "/rooms/12345"};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        cf_request req;
        prepare_delete(&req, paths[i]);
        char auth_header_817[4096];
        authenticated_request(&env, 1, NULL, &req, auth_header_817,
                              sizeof auth_header_817);
        cf_response resp;
        CF_REQUIRE(run_request(&env, &req, &resp));
        CF_CHECK(resp.status == 302);
        CF_CHECK(response_head_contains(&resp, &req,
                                        "Location: " ORIGIN "/\r\n"));
        CF_CHECK(room_row_exists(env.scratch.db, 99));
        cf_response_dispose(&resp);
    }
    env_close(&env);
}

CF_TEST(rooms_destroy_unauthenticated_is_redirected_to_sign_in) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    destroy_fixture(&env, 1, 0);

    cf_request req;
    prepare_delete(&req, "/rooms/42");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(response_head_contains(&resp, &req,
                                    "Location: " ORIGIN "/session/new\r\n"));
    CF_CHECK(room_row_exists(env.scratch.db, 42));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- P12-01: controlled stale-authorization races -------------------------- */

/* A second connection holds BEGIN IMMEDIATE with the authorization change
 * (ban, role demotion, membership removal or ownership change) still
 * uncommitted.  The action's pre-write checks run on the reader against the
 * old snapshot; its write callback is then admitted to the writer queue but
 * cannot start (BEGIN IMMEDIATE waits on this connection's lock).  The
 * committer commits the change the moment the writer admission count
 * advances, so the order is fixed by the lock, not by a sleep: every
 * pre-write check necessarily saw the old state, and the in-transaction
 * revalidation necessarily sees the new one. */
typedef struct {
    cf_app *app;
    sqlite3 *lock;
    uint64_t admitted_before;
    _Atomic bool request_done;
    bool commit_ok;
} rooms_stale_race;

static int64_t rooms_race_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *rooms_race_commit(void *arg) {
    rooms_stale_race *race = arg;
    int64_t deadline = rooms_race_ms() + 10000;
    for (;;) {
        cf_writer_stats stats = {0};
        if (cf_writer_stats_get(race->app, &stats) != CF_OK) break;
        if (stats.admitted > race->admitted_before) break;
        if (atomic_load_explicit(&race->request_done,
                                 memory_order_relaxed)) {
            break; /* the request failed before queueing a write */
        }
        if (rooms_race_ms() >= deadline) break;
        struct timespec pause = {0, 500000}; /* 0.5 ms */
        nanosleep(&pause, NULL);
    }
    char *message = NULL;
    int rc = sqlite3_exec(race->lock, "COMMIT", NULL, NULL, &message);
    race->commit_ok = rc == SQLITE_OK;
    if (!race->commit_ok) {
        fprintf(stderr, "  race commit failed (%d): %s\n", rc,
                message != NULL ? message : "?");
    }
    sqlite3_free(message);
    return NULL;
}

/* Open the second connection, take the write lock and apply `change_sql`
 * without committing.  The baseline admission count is captured before the
 * request, so the committer recognizes the action's callback. */
static bool rooms_race_begin(rooms_env *env, rooms_stale_race *race,
                             const char *change_sql) {
    memset(race, 0, sizeof *race);
    race->app = env->app;
    if (sqlite3_open(env->scratch.path, &race->lock) != SQLITE_OK) {
        fprintf(stderr, "  race: second connection failed\n");
        return false;
    }
    sqlite3_busy_timeout(race->lock, 1000);
    char *message = NULL;
    if (sqlite3_exec(race->lock, "BEGIN IMMEDIATE", NULL, NULL, &message) !=
        SQLITE_OK) {
        fprintf(stderr, "  race: BEGIN IMMEDIATE failed: %s\n",
                message != NULL ? message : "?");
        sqlite3_free(message);
        sqlite3_close(race->lock);
        race->lock = NULL;
        return false;
    }
    if (sqlite3_exec(race->lock, change_sql, NULL, NULL, &message) !=
        SQLITE_OK) {
        fprintf(stderr, "  race: change failed: %s\n",
                message != NULL ? message : "?");
        sqlite3_free(message);
        sqlite3_exec(race->lock, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(race->lock);
        race->lock = NULL;
        return false;
    }
    cf_writer_stats stats = {0};
    if (cf_writer_stats_get(env->app, &stats) != CF_OK) return false;
    race->admitted_before = stats.admitted;
    return true;
}

static void rooms_race_end(rooms_stale_race *race) {
    if (race->lock != NULL) sqlite3_close(race->lock);
    race->lock = NULL;
}

/* Run `req` while the held change commits as soon as its write is queued;
 * always joins the committer before returning (also on a request failure, so
 * no thread outlives the case). */
static bool rooms_race_request(rooms_env *env, rooms_stale_race *race,
                               cf_request *req, cf_response *resp) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, rooms_race_commit, race) != 0) {
        return false;
    }
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    atomic_store_explicit(&race->request_done, true, memory_order_relaxed);
    bool joined = pthread_join(thread, NULL) == 0;
    if (rc != CF_OK) {
        fprintf(stderr, "  race request failed: rc=%d\n", rc);
        return false;
    }
    return joined && race->commit_ok;
}

/* The review's first reproduction: the actor's ban commits while the destroy
 * waits for the writer.  The pre-write checks saw an active administrator;
 * the in-transaction revalidation must reject the destroy with no row
 * changes and no broadcast.  Before the repair this returned 302 and deleted
 * the room. */
CF_TEST(rooms_destroy_rejects_an_actor_banned_before_the_write) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    destroy_fixture(&env, 1, 1); /* actor 1: administrator and creator */

    rooms_conn conn;
    CF_REQUIRE(rooms_conn_open(&env, 1, &conn));
    CF_REQUIRE(rooms_conn_subscribe(&conn));

    cf_request req;
    prepare_delete(&req, "/rooms/42");
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);

    rooms_stale_race race;
    CF_REQUIRE(rooms_race_begin(&env, &race,
                                "UPDATE users SET status = 2 WHERE id = 1"));

    cf_response resp;
    CF_CHECK(rooms_race_request(&env, &race, &req, &resp));
    rooms_race_end(&race);

    if (resp.status != 403) {
        fprintf(stderr,
                "  stale destroy accepted: status=%u room=%d memberships=%lld "
                "messages=%lld\n",
                resp.status, (int)room_row_exists(env.scratch.db, 42),
                (long long)count_rows(env.scratch.db, "memberships",
                                      "room_id = 42"),
                (long long)count_rows(env.scratch.db, "messages",
                                      "room_id = 42"));
    }
    CF_CHECK(resp.status == 403);
    CF_CHECK(room_row_exists(env.scratch.db, 42));
    CF_CHECK(count_rows(env.scratch.db, "memberships", "room_id = 42") == 2);
    CF_CHECK(count_rows(env.scratch.db, "messages", "room_id = 42") == 2);
    CF_CHECK(count_rows(env.scratch.db, "users", "id = 1 AND status = 2") == 1);
    cf_writer_stats stats = {0};
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.committed == 0);
    CF_CHECK(stats.rolled_back == 1);
    char frame[4096];
    CF_CHECK(!rooms_conn_next_text(&conn, frame, sizeof frame, 300));

    cf_response_dispose(&resp);
    rooms_conn_close(&conn);
    env_close(&env);
}

/* Ownership removed in the race window: the actor stays an active plain
 * member, but the room's creator moves to another user, revoking the
 * creator/administrator authorization the pre-write check used. */
CF_TEST(rooms_destroy_rejects_ownership_changed_before_the_write) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    destroy_fixture(&env, 1, 0); /* actor 1: creator, plain member */

    cf_request req;
    prepare_delete(&req, "/rooms/42");
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);

    rooms_stale_race race;
    CF_REQUIRE(rooms_race_begin(&env, &race,
                                "UPDATE rooms SET creator_id = 2 WHERE id = 42"));

    cf_response resp;
    CF_CHECK(rooms_race_request(&env, &race, &req, &resp));
    rooms_race_end(&race);

    if (resp.status != 403) {
        fprintf(stderr, "  stale destroy accepted: status=%u room=%d\n",
                resp.status, (int)room_row_exists(env.scratch.db, 42));
    }
    CF_CHECK(resp.status == 403);
    CF_CHECK(room_row_exists(env.scratch.db, 42));
    CF_CHECK(count_rows(env.scratch.db, "rooms",
                        "id = 42 AND creator_id = 2") == 1);
    CF_CHECK(count_rows(env.scratch.db, "messages", "room_id = 42") == 2);
    cf_writer_stats stats = {0};
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.committed == 0);
    CF_CHECK(stats.rolled_back == 1);

    cf_response_dispose(&resp);
    env_close(&env);
}

/* Membership removed in the race window: set_room(Scope::All) is
 * membership-scoped, so the committed removal revokes the reach the
 * pre-write check used, even for an administrator. */
CF_TEST(rooms_destroy_rejects_removed_membership_before_the_write) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    destroy_fixture(&env, 2, 1); /* actor 1: administrator, member */

    cf_request req;
    prepare_delete(&req, "/rooms/42");
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);

    rooms_stale_race race;
    CF_REQUIRE(rooms_race_begin(
        &env, &race,
        "DELETE FROM memberships WHERE room_id = 42 AND user_id = 1"));

    cf_response resp;
    CF_CHECK(rooms_race_request(&env, &race, &req, &resp));
    rooms_race_end(&race);

    if (resp.status != 403) {
        fprintf(stderr, "  stale destroy accepted: status=%u room=%d\n",
                resp.status, (int)room_row_exists(env.scratch.db, 42));
    }
    CF_CHECK(resp.status == 403);
    CF_CHECK(room_row_exists(env.scratch.db, 42));
    CF_CHECK(count_rows(env.scratch.db, "memberships", "room_id = 42") == 1);
    CF_CHECK(count_rows(env.scratch.db, "messages", "room_id = 42") == 2);
    cf_writer_stats stats = {0};
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.committed == 0);
    CF_CHECK(stats.rolled_back == 1);

    cf_response_dispose(&resp);
    env_close(&env);
}

/* Role demotion in the race window: the actor is an administrator only by
 * role (not the creator), so demotion to member removes the authorization. */
CF_TEST(rooms_destroy_rejects_an_admin_demoted_before_the_write) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    destroy_fixture(&env, 2, 1); /* actor 1: administrator (not creator) */

    cf_request req;
    prepare_delete(&req, "/rooms/42");
    char auth_header[4096];
    authenticated_request(&env, 1, NULL, &req, auth_header,
                          sizeof auth_header);

    rooms_stale_race race;
    CF_REQUIRE(rooms_race_begin(&env, &race,
                                "UPDATE users SET role = 0 WHERE id = 1"));

    cf_response resp;
    CF_CHECK(rooms_race_request(&env, &race, &req, &resp));
    rooms_race_end(&race);

    if (resp.status != 403) {
        fprintf(stderr, "  stale destroy accepted: status=%u room=%d\n",
                resp.status, (int)room_row_exists(env.scratch.db, 42));
    }
    CF_CHECK(resp.status == 403);
    CF_CHECK(room_row_exists(env.scratch.db, 42));
    CF_CHECK(count_rows(env.scratch.db, "users", "id = 1 AND role = 0") == 1);
    CF_CHECK(count_rows(env.scratch.db, "messages", "room_id = 42") == 2);
    cf_writer_stats stats = {0};
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.committed == 0);
    CF_CHECK(stats.rolled_back == 1);

    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- golden/b render comparisons ------------------------------------------- */

/* The fixtures were served to a Chrome/macOS browser, so their bell carries
 * the browser-settings, system-settings (and, for Chrome, no) install
 * `details.notifications-help` nodes.  The action renders the same bell with
 * zeroed platform facts (these requests carry no User-Agent, so the stored
 * platform is NULL), which changes exactly those nodes.  The mask below drops
 * every `details.notifications-help` element from BOTH sides by name; no
 * other difference is masked. */

/* Remove every `<details class="notifications-help"...>...</details>` element
 * from `html` (the element's matching first `</details>` closes it; the
 * partials never nest another details element). */
static cf_err strip_notifications_help(cf_span html, cf_builder *out) {
    static const char open_marker[] = "<details class=\"notifications-help";
    static const char close_marker[] = "</details>";
    const size_t open_len = sizeof open_marker - 1;
    const size_t close_len = sizeof close_marker - 1;
    size_t at = 0;
    for (;;) {
        if (html.len - at < open_len) {
            return cf_builder_append(out,
                                     (cf_span){html.ptr + at, html.len - at});
        }
        const unsigned char *found =
            memmem(html.ptr + at, html.len - at, open_marker, open_len);
        if (found == NULL) {
            return cf_builder_append(out,
                                     (cf_span){html.ptr + at, html.len - at});
        }
        size_t open_at = (size_t)(found - html.ptr);
        cf_err rc = cf_builder_append(out,
                                      (cf_span){html.ptr + at, open_at - at});
        if (rc != CF_OK) return rc;
        const unsigned char *close = memmem(html.ptr + open_at,
                                            html.len - open_at, close_marker,
                                            close_len);
        if (close == NULL) return CF_INVALID; /* malformed fixture render */
        at = (size_t)(close - html.ptr) + close_len;
    }
}

/* The same removal in the fixture's token stream: drop the tokens of every
 * element whose start token is `<details class="notifications-help...` up to
 * its `</details>`, then merge the whitespace-only text tokens that become
 * adjacent (the tokenizer merges adjacent text runs in the actual render). */
static yyjson_doc *mask_expected_notifications_help(yyjson_doc *doc) {
    yyjson_mut_doc *mut = yyjson_doc_mut_copy(doc, NULL);
    if (mut == NULL) return NULL;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(mut);
    yyjson_mut_val *expected = yyjson_mut_obj_get(root, "expected");
    if (expected == NULL || !yyjson_mut_is_arr(expected)) {
        yyjson_mut_doc_free(mut);
        return NULL;
    }
    static const char open_marker[] = "<details class=\"notifications-help";
    const size_t open_len = sizeof open_marker - 1;
    size_t i = 0;
    while (i < yyjson_mut_arr_size(expected)) {
        yyjson_mut_val *token = yyjson_mut_arr_get(expected, i);
        const char *text = yyjson_mut_get_str(token);
        if (text == NULL || strncmp(text, open_marker, open_len) != 0) {
            i++;
            continue;
        }
        size_t j = i + 1;
        while (j < yyjson_mut_arr_size(expected)) {
            const char *close =
                yyjson_mut_get_str(yyjson_mut_arr_get(expected, j));
            if (close != NULL && strcmp(close, "</details>") == 0) break;
            j++;
        }
        if (j >= yyjson_mut_arr_size(expected)) {
            yyjson_mut_doc_free(mut);
            return NULL;
        }
        for (size_t k = i; k <= j; k++) {
            yyjson_mut_arr_remove(expected, i);
        }
        /* Merge the whitespace-only runs that are now adjacent. */
        while (i > 0 && i < yyjson_mut_arr_size(expected)) {
            const char *before =
                yyjson_mut_get_str(yyjson_mut_arr_get(expected, i - 1));
            const char *after =
                yyjson_mut_get_str(yyjson_mut_arr_get(expected, i));
            if (before == NULL || after == NULL || strcmp(before, "# ") != 0 ||
                strcmp(after, "# ") != 0) {
                break;
            }
            yyjson_mut_arr_remove(expected, i);
        }
    }
    yyjson_doc *masked = yyjson_mut_doc_imut_copy(mut, NULL);
    yyjson_mut_doc_free(mut);
    return masked;
}

/* --- fixture seeding: the same rows the reference rendered ----------------- */

static void builder_exec(cf_db *db, cf_builder *sql) {
    static const unsigned char nul = 0;
    CF_REQUIRE(cf_builder_append(sql, (cf_span){&nul, 1}) == CF_OK);
    exec_sql(db, (const char *)sql->ptr);
    cf_builder_dispose(sql);
}

/* One SQL-quoted string value. */
static void sql_quote(cf_builder *sql, const char *text) {
    CF_REQUIRE(cf_builder_append(sql, SP("'")) == CF_OK);
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '\'') {
            CF_REQUIRE(cf_builder_append(sql, SP("''")) == CF_OK);
        } else {
            CF_REQUIRE(cf_builder_append(
                           sql, (cf_span){(const unsigned char *)p, 1}) ==
                       CF_OK);
        }
    }
    CF_REQUIRE(cf_builder_append(sql, SP("'")) == CF_OK);
}

/* "2026-09-26T13:03:59.891935000Z" -> "2026-09-26 13:03:59.891935"; a
 * whole-second time is written without a fraction, like Active Record. */
static void iso_to_sql(const char *iso, char out[40]) {
    if (iso == NULL || strlen(iso) < 19) {
        snprintf(out, 40, "1970-01-01 00:00:00.000000");
        return;
    }
    size_t len = 0;
    memcpy(out, iso, 10);
    out[10] = ' ';
    memcpy(out + 11, iso + 11, 8);
    len = 19;
    if (iso[19] == '.') {
        out[len++] = '.';
        size_t k = 0;
        while (k < 6 && iso[20 + k] >= '0' && iso[20 + k] <= '9') {
            out[len++] = iso[20 + k];
            k++;
        }
        while (k < 6) {
            out[len++] = '0';
            k++;
        }
    }
    out[len] = '\0';
}

/* The `?v=YYYYMMDDHHMMSS` suffix of `fresh_user_avatar_path` -> the user's
 * updated_at the avatar URL version came from. */
static void avatar_v_to_sql(const char *avatar_url, char out[40]) {
    snprintf(out, 40, "2026-09-26 13:03:57.000000");
    if (avatar_url == NULL) return;
    const char *v = strstr(avatar_url, "?v=");
    if (v == NULL || strlen(v + 3) < 14) return;
    v += 3;
    snprintf(out, 40, "%.4s-%.2s-%.2s %.2s:%.2s:%.2s.000000", v, v + 4,
             v + 6, v + 8, v + 10, v + 12);
}

/* `User#title` is name and bio joined by " – "; recover the bio. */
static char *bio_from_title(const char *name, const char *title) {
    if (name == NULL || title == NULL) return NULL;
    size_t name_len = strlen(name);
    static const char sep[] = " \xE2\x80\x93 "; /* " – " */
    if (strncmp(title, name, name_len) != 0 ||
        strncmp(title + name_len, sep, sizeof sep - 1) != 0) {
        return NULL;
    }
    return strdup(title + name_len + sizeof sep - 1);
}

/* `messages/_message`'s text presentation: the stored body wrapped as
 * `<div class="lexxy-content">\n  <body>\n</div>\n`. */
static char *body_from_html(const char *html) {
    static const char prefix[] = "<div class=\"lexxy-content\">\n  ";
    static const char suffix[] = "\n</div>\n";
    if (html == NULL) return NULL;
    size_t len = strlen(html);
    size_t plen = sizeof prefix - 1;
    size_t slen = sizeof suffix - 1;
    if (len < plen + slen || strncmp(html, prefix, plen) != 0 ||
        strcmp(html + len - slen, suffix) != 0) {
        return NULL;
    }
    size_t blen = len - plen - slen;
    char *body = malloc(blen + 1);
    if (body == NULL) return NULL;
    memcpy(body, html + plen, blen);
    body[blen] = '\0';
    return body;
}

static void seed_golden_user(cf_db *db, yyjson_val *user, int role) {
    if (!yyjson_is_obj(user)) return;
    int64_t id = yyjson_get_sint(yyjson_obj_get(user, "id"));
    const char *name = yyjson_get_str(yyjson_obj_get(user, "name"));
    const char *title = yyjson_get_str(yyjson_obj_get(user, "title"));
    const char *avatar = yyjson_get_str(yyjson_obj_get(user, "avatar_url"));
    if (name == NULL) return;
    char updated[40];
    avatar_v_to_sql(avatar, updated);
    char *bio = bio_from_title(name, title);

    cf_builder sql = {0};
    CF_REQUIRE(cf_builder_append(
                   &sql,
                   SP("INSERT OR IGNORE INTO users (id, bio, created_at, "
                      "email_address, name, role, status, updated_at) "
                      "VALUES (")) == CF_OK);
    {
        char number[32];
        snprintf(number, sizeof number, "%lld, ", (long long)id);
        CF_REQUIRE(cf_builder_append(
                       &sql, (cf_span){(const unsigned char *)number,
                                       strlen(number)}) == CF_OK);
    }
    if (bio != NULL) {
        sql_quote(&sql, bio);
        CF_REQUIRE(cf_builder_append(&sql, SP(", ")) == CF_OK);
    } else {
        CF_REQUIRE(cf_builder_append(&sql, SP("NULL, ")) == CF_OK);
    }
    CF_REQUIRE(cf_builder_append(
                   &sql, SP("'2026-09-26 13:00:29.000000', NULL, ")) == CF_OK);
    sql_quote(&sql, name);
    {
        char tail[128];
        snprintf(tail, sizeof tail, ", %d, 0, ", role);
        CF_REQUIRE(cf_builder_append(
                       &sql, (cf_span){(const unsigned char *)tail,
                                       strlen(tail)}) == CF_OK);
    }
    sql_quote(&sql, updated);
    CF_REQUIRE(cf_builder_append(&sql, SP(")")) == CF_OK);
    free(bio);
    builder_exec(db, &sql);
}

/* Seed the fixture input's rows: the account, the room (and, unless it is the
 * original room, an older room so the invitation condition is false), the
 * current user, every message creator, memberships and the message bodies.
 * `older_room` seeds the "All Pets" room the reference database's oldest room
 * is; the fixtures that are not that room need it. */
static void seed_golden_fixture(rooms_env *env, yyjson_doc *doc,
                                int64_t *out_room_id, int64_t *out_user_id,
                                bool older_room) {
    cf_db *db = env->scratch.db;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *input = yyjson_obj_get(root, "input");
    yyjson_val *context = yyjson_obj_get(root, "context");
    yyjson_val *current = yyjson_obj_get(context, "current_user");
    yyjson_val *room = yyjson_obj_get(input, "room");
    yyjson_val *user = yyjson_obj_get(input, "user");
    int64_t user_id = yyjson_get_sint(yyjson_obj_get(current, "id"));
    bool administrator =
        yyjson_get_bool(yyjson_obj_get(current, "administrator"));
    int64_t room_id = yyjson_get_sint(yyjson_obj_get(room, "id"));
    const char *kind = yyjson_get_str(yyjson_obj_get(room, "kind"));
    const char *room_name = yyjson_get_str(yyjson_obj_get(room, "name"));
    const char *display_name =
        yyjson_get_str(yyjson_obj_get(room, "display_name"));
    char updated[40];
    iso_to_sql(yyjson_get_str(yyjson_obj_get(input, "updated_at")), updated);

    seed_account(db);
    seed_golden_user(db, user, administrator ? 1 : 0);

    const char *type_text =
        kind != NULL && strcmp(kind, "direct") == 0
            ? "Rooms::Direct"
            : (kind != NULL && strcmp(kind, "closed") == 0 ? "Rooms::Closed"
                                                           : "Rooms::Open");
    {
        cf_builder sql = {0};
        char head[160];
        snprintf(head, sizeof head,
                 "INSERT OR IGNORE INTO rooms (id, created_at, creator_id, "
                 "name, type, updated_at) VALUES (%lld, '%s', %lld, ",
                 (long long)room_id, updated, (long long)user_id);
        CF_REQUIRE(cf_builder_append(
                       &sql, (cf_span){(const unsigned char *)head,
                                       strlen(head)}) == CF_OK);
        if (room_name != NULL) {
            sql_quote(&sql, room_name);
        } else {
            CF_REQUIRE(cf_builder_append(&sql, SP("NULL")) == CF_OK);
        }
        CF_REQUIRE(cf_builder_append(&sql, SP(", ")) == CF_OK);
        sql_quote(&sql, type_text);
        char tail[64];
        snprintf(tail, sizeof tail, ", '%s')", updated);
        CF_REQUIRE(cf_builder_append(
                       &sql, (cf_span){(const unsigned char *)tail,
                                       strlen(tail)}) == CF_OK);
        builder_exec(db, &sql);
    }
    if (older_room) {
        exec_sql(db,
                 "INSERT OR IGNORE INTO rooms (id, created_at, creator_id, "
                 "name, type, updated_at) VALUES (1, "
                 "'2020-01-01 00:00:00.000000', 1, 'All Pets', 'Rooms::Open', "
                 "'2020-01-01 00:00:00.000000')");
    }
    {
        char sql[400];
        snprintf(sql, sizeof sql,
                 "INSERT OR IGNORE INTO memberships (id, created_at, room_id, "
                 "updated_at, user_id) VALUES (%lld, '%s', %lld, '%s', %lld)",
                 (long long)(room_id * 100 + 1), updated, (long long)room_id,
                 updated, (long long)user_id);
        exec_sql(db, sql);
    }

    /* The direct room's other member(s) make up its display_name. */
    if (strcmp(type_text, "Rooms::Direct") == 0) {
        char *copy = display_name != NULL ? strdup(display_name) : NULL;
        if (copy != NULL) {
            int64_t other_id = 149087659;
            char *save = NULL;
            for (char *name = strtok_r(copy, ",", &save); name != NULL;
                 name = strtok_r(NULL, ",", &save)) {
                while (*name == ' ') name++;
                size_t len = strlen(name);
                while (len != 0 && (name[len - 1] == ' ')) name[--len] = '\0';
                if (strncmp(name, "and ", 4) == 0) name += 4;
                if (*name == '\0') continue;
                cf_builder sql = {0};
                char head[160];
                snprintf(head, sizeof head,
                         "INSERT OR IGNORE INTO users (id, bio, created_at, "
                         "email_address, name, role, status, updated_at) "
                         "VALUES (%lld, NULL, '%s', NULL, ",
                         (long long)other_id, updated);
                CF_REQUIRE(cf_builder_append(
                               &sql, (cf_span){(const unsigned char *)head,
                                               strlen(head)}) == CF_OK);
                sql_quote(&sql, name);
                CF_REQUIRE(cf_builder_append(
                               &sql, SP(", 0, 0, ")) == CF_OK);
                sql_quote(&sql, updated);
                CF_REQUIRE(cf_builder_append(&sql, SP(")")) == CF_OK);
                builder_exec(db, &sql);
                char member_sql[400];
                snprintf(member_sql, sizeof member_sql,
                         "INSERT OR IGNORE INTO memberships (id, created_at, "
                         "room_id, updated_at, user_id) VALUES (%lld, '%s', "
                         "%lld, '%s', %lld)",
                         (long long)(room_id * 100 + 2), updated,
                         (long long)room_id, updated, (long long)other_id);
                exec_sql(db, member_sql);
                other_id++;
            }
            free(copy);
        }
    }

    yyjson_val *messages = yyjson_obj_get(input, "messages");
    size_t count = yyjson_arr_size(messages);
    for (size_t i = 0; i < count; i++) {
        yyjson_val *message = yyjson_arr_get(messages, i);
        yyjson_val *creator = yyjson_obj_get(message, "creator");
        int64_t creator_id = yyjson_get_sint(yyjson_obj_get(creator, "id"));
        int64_t message_id = yyjson_get_sint(yyjson_obj_get(message, "id"));
        const char *client =
            yyjson_get_str(yyjson_obj_get(message, "client_message_id"));
        char created[40], changed[40];
        iso_to_sql(yyjson_get_str(yyjson_obj_get(message, "created_at")),
                   created);
        iso_to_sql(yyjson_get_str(yyjson_obj_get(message, "updated_at")),
                   changed);
        seed_golden_user(db, creator, creator_id == user_id && administrator
                                         ? 1
                                         : 0);
        {
            cf_builder sql = {0};
            char head[128];
            snprintf(head, sizeof head,
                     "INSERT OR IGNORE INTO messages (id, client_message_id, "
                     "created_at, creator_id, room_id, updated_at) VALUES "
                     "(%lld, ",
                     (long long)message_id);
            CF_REQUIRE(cf_builder_append(
                           &sql, (cf_span){(const unsigned char *)head,
                                           strlen(head)}) == CF_OK);
            sql_quote(&sql, client);
            char tail[256];
            snprintf(tail, sizeof tail, ", '%s', %lld, %lld, '%s')", created,
                     (long long)creator_id, (long long)room_id, changed);
            CF_REQUIRE(cf_builder_append(
                           &sql, (cf_span){(const unsigned char *)tail,
                                           strlen(tail)}) == CF_OK);
            builder_exec(db, &sql);
        }
        yyjson_val *content = yyjson_obj_get(message, "content");
        const char *content_type =
            content != NULL ? yyjson_get_str(yyjson_obj_get(content, "type"))
                            : NULL;
        if (content_type != NULL && strcmp(content_type, "text") == 0) {
            char *body = body_from_html(
                yyjson_get_str(yyjson_obj_get(content, "html")));
            if (body != NULL) {
                cf_builder sql = {0};
                char head[160];
                snprintf(head, sizeof head,
                         "INSERT OR IGNORE INTO action_text_rich_texts "
                         "(body, created_at, name, record_id, record_type, "
                         "updated_at) VALUES (");
                CF_REQUIRE(cf_builder_append(
                               &sql, (cf_span){(const unsigned char *)head,
                                               strlen(head)}) == CF_OK);
                sql_quote(&sql, body);
                char tail[160];
                snprintf(tail, sizeof tail,
                         ", '%s', 'body', %lld, 'Message', '%s')", changed,
                         (long long)message_id, changed);
                CF_REQUIRE(cf_builder_append(
                               &sql, (cf_span){(const unsigned char *)tail,
                                               strlen(tail)}) == CF_OK);
                builder_exec(db, &sql);
                free(body);
            }
        }
    }
    *out_room_id = room_id;
    *out_user_id = user_id;
}

/* Dispatch GET /rooms/<id> for the fixture's current user and compare the
 * page with the fixture's token stream (assert_dom regions) after dropping
 * the platform-dependent `notifications-help` nodes from both sides. */
static void golden_rooms_show(const char *name, bool older_room) {
    rooms_env env;
    CF_REQUIRE(env_open(&env));
    yyjson_doc *doc = cf_golden_b_load(name);
    CF_REQUIRE(doc != NULL);
    int64_t room_id = 0, user_id = 0;
    seed_golden_fixture(&env, doc, &room_id, &user_id, older_room);

    char path[64];
    snprintf(path, sizeof path, "/rooms/%lld", (long long)room_id);
    cf_request req;
    prepare_get(&req, path);
    char auth_header[4096];
    authenticated_request(&env, user_id, NULL, &req, auth_header,
                          sizeof auth_header);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    if (resp.status == 200 && resp.body != NULL) {
        cf_builder stripped = {0};
        cf_span body = cf_buf_span(resp.body);
        CF_REQUIRE(strip_notifications_help(body, &stripped) == CF_OK);
        yyjson_doc *masked = mask_expected_notifications_help(doc);
        CF_REQUIRE(masked != NULL);
        cf_golden_b_expect(masked, name, 1, stripped.ptr, stripped.len);
        yyjson_doc_free(masked);
        cf_builder_dispose(&stripped);
    }
    cf_response_dispose(&resp);
    yyjson_doc_free(doc);
    env_close(&env);
}

CF_TEST(rooms_show_member_matches_the_golden) {
    golden_rooms_show("rooms_show_member", true);
}

CF_TEST(rooms_show_original_matches_the_golden_with_the_invitation) {
    golden_rooms_show("rooms_show_original", false);
}

CF_TEST(rooms_show_direct_matches_the_golden) {
    golden_rooms_show("rooms_show_direct", true);
}

CF_TEST_MAIN()
