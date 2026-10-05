/* tests/actions/rooms_refreshes_test.c — A-rooms-refreshes acceptance:
 * `rooms/refreshes#show` (route ID 91)
 * (docs/devel/implementation/contracts/controller-packets.md
 * "A-rooms-refreshes").
 *
 * Every case runs the real A00 dispatch path (params, cookies,
 * before-actions, format negotiation) through the route double
 * (tests/app/support/route_double.c), which binds row 91 to the action.
 *
 * Matrix from the card: the authenticated success paths (empty refresh,
 * new-messages append, updated-messages replace, new-wins-over-updated),
 * the `since` parameter's ruby_compat::to_i semantics (missing => 0,
 * strings parsed, hash/array => 500, saturating ms=>us with MIN/MAX clamp),
 * unauthorized/forbidden/not-found cases, and the TURBO_STREAM format
 * negotiation (bare render, no layout `Link` header).
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "db/db_testutil.h"
#include "http/http_internal.h"
#include "models/message.h"
#include "models/room.h"
#include "models/user.h"
#include "views.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ORIGIN "http://campfire.test"
#define TURBO_ACCEPT "text/vnd.turbo-stream.html"

/* Message instants with nonzero microseconds (the model binds query times
 * as text, where a whole-second value compares inexactly; the rooms packet
 * seeds the same way).  T1_MS/T2_MS are their exact epoch milliseconds. */
#define T1_MS "1767225601500"
#define T2_MS "1767225602500"
#define T1_AT "2026-01-01 00:00:01.500000"
#define T2_AT "2026-01-01 00:00:02.500000"

/* routes.json row 91.  cf.h is frozen and no packet-action header exists
 * yet, so the test declares it (the integrator's routes.c rebind needs the
 * same declaration). */
cf_err cf_action_rooms_refreshes_show(cf_ctx *ctx);

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- scratch app + database ------------------------------------------------ */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    unsigned session_seq;
} refresh_env;

static bool env_open(refresh_env *env) {
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
    /* The stream renders message partials through the real asset module. */
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
    cf_test_routes_reset();
    /* The double needs both the `(.:format)` row and the bare row for the
     * same id (its literal-segment match takes the suffix row only when an
     * extension is present); the real table has the single optional-format
     * row. */
    if (cf_test_routes_add("GET", "/rooms/:room_id/refresh(.:format)", 91,
                           cf_action_rooms_refreshes_show) != CF_OK ||
        cf_test_routes_add("GET", "/rooms/:room_id/refresh", 91,
                           cf_action_rooms_refreshes_show) != CF_OK) {
        fprintf(stderr, "  env_open: route registration failed\n");
        cf_app_destroy(env->app);
        env->app = NULL;
        cf_db_scratch_close(&env->scratch);
        return false;
    }
    return true;
}

static void env_close(refresh_env *env) {
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
    char sql[768];
    snprintf(sql, sizeof sql,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (%lld, '2026-01-01 00:00:00.000000', %lld, "
             "'%s', '%s', '2026-01-01 00:00:00.000000')",
             (long long)id, (long long)creator_id, name, type_text);
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

static void seed_message(cf_db *db, int64_t id, int64_t room_id,
                         int64_t creator_id, const char *client_id,
                         const char *at) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO messages (id, client_message_id, created_at, "
             "creator_id, room_id, updated_at) VALUES (%lld, '%s', '%s', "
             "%lld, %lld, '%s')",
             (long long)id, client_id, at, (long long)creator_id,
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

static void make_session_cookie(refresh_env *env, char *out, size_t cap,
                                int64_t user_id) {
    char token[64];
    snprintf(token, sizeof token, "refresh-token-%u", env->session_seq++);
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

static bool run_request(refresh_env *env, cf_request *req, cf_response *resp) {
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

static bool response_body_equals(cf_response *resp, const char *text) {
    if (resp->body == NULL) return text[0] == '\0';
    cf_span span = cf_buf_span(resp->body);
    size_t len = strlen(text);
    return span.len == len && memcmp(span.ptr, text, len) == 0;
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

/* Authenticated GET /rooms/<room>/refresh with an optional query string,
 * Accept header and extra raw body (for the JSON-`since` cases). */
static void prepare_refresh(refresh_env *env, cf_request *req, int64_t user_id,
                            const char *path, const char *query,
                            const char *accept, const char *content_type,
                            const char *body) {
    cf_test_req_init(req);
    req->path = SP(path);
    req->target = req->path;
    /* The header spans must outlive the request: keep the cookie in a
     * static ring. */
    static char ring[8][4096];
    static unsigned at;
    char *slot = ring[at++ % 8];
    make_session_cookie(env, slot, sizeof ring[0], user_id);
    CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(slot)) == CF_OK);
    if (query != NULL) req->query = SP(query);
    if (accept != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Accept"), SP(accept)) == CF_OK);
    }
    if (content_type != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                      SP(content_type)) == CF_OK);
    }
    if (body != NULL) req->body = SP(body);
}

static void seed_base(refresh_env *env) {
    seed_user(env->scratch.db, 1, "Kevin");
    seed_room_typed(env->scratch.db, 42, "HQ", "Rooms::Open", 1);
    seed_membership(env->scratch.db, 201, 42, 1);
}

/* --- route IDs ------------------------------------------------------------- */

CF_TEST(refreshes_route_id_binds_the_action) {
    refresh_env env;
    CF_REQUIRE(env_open(&env));
    CF_CHECK(cf_route_action(91) == cf_action_rooms_refreshes_show);
    env_close(&env);
}

/* --- authentication and scope ---------------------------------------------- */

CF_TEST(refreshes_unauthenticated_is_redirected_to_sign_in) {
    refresh_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    cf_request req;
    cf_test_req_init(&req);
    req.path = SP("/rooms/42/refresh");
    req.target = req.path;
    req.query = SP("since=" T1_MS);
    CF_REQUIRE(cf_test_req_header(&req, SP("Accept"), SP(TURBO_ACCEPT)) ==
               CF_OK);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(response_head_contains(&resp, &req,
                                    "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Unknown room, a room the user is not a member of, and an unparsable id
 * all take set_room's RecordNotFound arm: 404. */
CF_TEST(refreshes_not_found_or_inaccessible_is_404) {
    refresh_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_user(env.scratch.db, 2, "JZ");
    seed_room_typed(env.scratch.db, 99, "Other", "Rooms::Open", 2);
    seed_membership(env.scratch.db, 202, 99, 2);

    static const char *paths[] = {
        "/rooms/12345/refresh",
        "/rooms/99/refresh",
        "/rooms/abc/refresh",
    };
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        cf_request req;
        prepare_refresh(&env, &req, 1, paths[i], NULL, TURBO_ACCEPT, NULL,
                        NULL);
        cf_response resp;
        CF_REQUIRE(run_request(&env, &req, &resp));
        CF_CHECK(resp.status == 404);
        cf_response_dispose(&resp);
    }
    env_close(&env);
}

/* A membership whose room row is gone: the reference's 500 (a missing row
 * is not a missing membership). */
CF_TEST(refreshes_gone_room_is_500) {
    refresh_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    exec_sql(env.scratch.db, "DELETE FROM rooms WHERE id = 42");

    cf_request req;
    prepare_refresh(&env, &req, 1, "/rooms/42/refresh", NULL, TURBO_ACCEPT,
                    NULL, NULL);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* deny_bots (Before::default): an authenticated bot is forbidden. */
CF_TEST(refreshes_bot_authentication_is_forbidden) {
    refresh_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    exec_sql(env.scratch.db,
             "INSERT INTO users (id, created_at, email_address, name, role, "
             "status, updated_at, bot_token) VALUES (7, "
             "'2026-09-26 13:00:29.000000', NULL, 'Bender Bot', 2, 0, "
             "'2026-09-26 13:00:29.000000', 'refreshbotkey')");

    cf_request req;
    cf_test_req_init(&req);
    req.path = SP("/rooms/42/refresh");
    req.target = req.path;
    req.query = SP("bot_key=7-refreshbotkey");
    CF_REQUIRE(cf_test_req_header(&req, SP("Accept"), SP(TURBO_ACCEPT)) ==
               CF_OK);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- the empty and nonempty refresh ---------------------------------------- */

/* No messages since the epoch: the template's empty render is exactly "\n",
 * with the turbo-stream content type and no layout `Link` header. */
CF_TEST(refreshes_empty_refresh_is_a_bare_newline_stream) {
    refresh_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    cf_request req;
    prepare_refresh(&env, &req, 1, "/rooms/42/refresh", "since=" T1_MS,
                    TURBO_ACCEPT, NULL, NULL);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(response_head_contains(
        &resp, &req,
        "Content-Type: text/vnd.turbo-stream.html; charset=utf-8\r\n"));
    CF_CHECK(response_body_equals(&resp, "\n"));
    CF_CHECK(!response_head_contains(&resp, &req, "Link:"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Messages created after `since` stream as one append block addressed at
 * the room's messages target. */
CF_TEST(refreshes_new_messages_append) {
    refresh_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_message(env.scratch.db, 100, 42, 1, "r1", T1_AT);
    seed_body(env.scratch.db, 100, "first");
    seed_message(env.scratch.db, 101, 42, 1, "r2", T2_AT);
    seed_body(env.scratch.db, 101, "second");

    cf_request req;
    prepare_refresh(&env, &req, 1, "/rooms/42/refresh", "since=" T1_MS,
                    TURBO_ACCEPT, NULL, NULL);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(response_body_contains(
        &resp, "<turbo-stream action=\"append\" "
               "target=\"messages_rooms_open_42\"><template>"));
    CF_CHECK(response_body_contains(&resp, "message_r2"));
    /* r1 was created exactly at `since`: page_created_since is strict. */
    CF_CHECK(!response_body_contains(&resp, "message_r1"));
    CF_CHECK(!response_body_contains(&resp, "action=\"replace\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A message created before `since` but updated after it streams as a
 * replace addressed at the message's dom id. */
CF_TEST(refreshes_updated_messages_replace) {
    refresh_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_message(env.scratch.db, 100, 42, 1, "r1", T1_AT);
    seed_body(env.scratch.db, 100, "first");
    exec_sql(env.scratch.db,
             "UPDATE messages SET updated_at = '" T2_AT "' WHERE id = 100");

    cf_request req;
    prepare_refresh(&env, &req, 1, "/rooms/42/refresh",
                    "since=1767225602000", TURBO_ACCEPT, NULL, NULL);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(!response_body_contains(&resp, "action=\"append\""));
    CF_CHECK(response_body_contains(
        &resp, "  <turbo-stream action=\"replace\" "
               "target=\"message_r1\"><template>"));
    CF_CHECK(response_body_contains(&resp, "message_r1"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A message both created and updated after `since` is new, not updated:
 * the new ids are excluded from the updated page. */
CF_TEST(refreshes_new_ids_are_excluded_from_the_updated_page) {
    refresh_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_message(env.scratch.db, 100, 42, 1, "r1", T2_AT);
    seed_body(env.scratch.db, 100, "first");
    /* Touch it again so updated_at is later still, but created_at is new. */
    exec_sql(env.scratch.db,
             "UPDATE messages SET updated_at = '2026-01-01 00:00:03.500000' "
             "WHERE id = 100");

    cf_request req;
    prepare_refresh(&env, &req, 1, "/rooms/42/refresh", "since=" T1_MS,
                    TURBO_ACCEPT, NULL, NULL);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(response_body_contains(&resp, "action=\"append\""));
    CF_CHECK(!response_body_contains(&resp, "action=\"replace\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A missing `since` is 0: every message is new. */
CF_TEST(refreshes_missing_since_loads_everything_new) {
    refresh_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_message(env.scratch.db, 100, 42, 1, "r1", T1_AT);
    seed_body(env.scratch.db, 100, "first");

    cf_request req;
    prepare_refresh(&env, &req, 1, "/rooms/42/refresh", NULL, TURBO_ACCEPT,
                    NULL, NULL);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(response_body_contains(&resp, "action=\"append\""));
    CF_CHECK(response_body_contains(&resp, "message_r1"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- `since` parsing (ruby_compat::to_i) ----------------------------------- */

CF_TEST(refreshes_since_strings_follow_ruby_to_i) {
    refresh_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_message(env.scratch.db, 100, 42, 1, "r1", T1_AT);
    seed_body(env.scratch.db, 100, "first");

    /* `since` value, then whether r1 (created at T1) is new. */
    static const struct {
        const char *query;
        bool is_new;
        const char *what;
    } cases[] = {
        {"since=abc", true, "non-numeric is 0"},
        {"since=", true, "empty is 0"},
        {"since=+%2B12abc", true, "sign and trailing text stop the parse"},
        {"since=" T1_MS, false, "exact T1 is not after itself"},
        {"since=1767225601499", true, "one ms before T1 is new"},
        {"since=-5", true, "negative is before everything"},
        /* Past the i64 ms range: saturating to_i, then the MAX clamp —
         * nothing is newer. */
        {"since=99999999999999999999999", false, "huge clamps to MAX"},
        {"since=-99999999999999999999999", true, "huge negative clamps to MIN"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cf_request req;
        prepare_refresh(&env, &req, 1, "/rooms/42/refresh", cases[i].query,
                        TURBO_ACCEPT, NULL, NULL);
        cf_response resp;
        CF_REQUIRE(run_request(&env, &req, &resp));
        if (resp.status != 200) {
            fprintf(stderr, "  [%s] status %u\n", cases[i].what, resp.status);
        }
        CF_CHECK(resp.status == 200);
        bool append = response_body_contains(&resp, "action=\"append\"");
        if (append != cases[i].is_new) {
            fprintf(stderr, "  [%s] append=%d\n", cases[i].what,
                    (int)append);
        }
        CF_CHECK(append == cases[i].is_new);
        cf_response_dispose(&resp);
    }
    env_close(&env);
}

/* A hash or an array has no `to_i`: internal error.  The array arrives as a
 * form-encoded `since[]`, the hash as a JSON body. */
CF_TEST(refreshes_since_without_to_i_is_500) {
    refresh_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    cf_request array_req;
    prepare_refresh(&env, &array_req, 1, "/rooms/42/refresh", "since[]=1",
                    TURBO_ACCEPT, NULL, NULL);
    cf_response array_resp;
    CF_REQUIRE(run_request(&env, &array_req, &array_resp));
    CF_CHECK(array_resp.status == 500);
    cf_response_dispose(&array_resp);

    cf_request hash_req;
    prepare_refresh(&env, &hash_req, 1, "/rooms/42/refresh", NULL,
                    TURBO_ACCEPT, "application/json", "{\"since\": {}}");
    cf_response hash_resp;
    CF_REQUIRE(run_request(&env, &hash_req, &hash_resp));
    CF_CHECK(hash_resp.status == 500);
    cf_response_dispose(&hash_resp);

    /* A JSON null is the reference's `param.is_null() => 0`: everything new. */
    seed_message(env.scratch.db, 100, 42, 1, "r1", T1_AT);
    seed_body(env.scratch.db, 100, "first");
    cf_request null_req;
    prepare_refresh(&env, &null_req, 1, "/rooms/42/refresh", NULL,
                    TURBO_ACCEPT, "application/json",
                    "{\"since\": null}");
    cf_response null_resp;
    CF_REQUIRE(run_request(&env, &null_req, &null_resp));
    CF_CHECK(null_resp.status == 200);
    CF_CHECK(response_body_contains(&null_resp, "message_r1"));
    cf_response_dispose(&null_resp);
    env_close(&env);
}

/* --- format negotiation ---------------------------------------------------- */

/* The bare render answers the turbo-stream format (Accept or suffix); an
 * HTML-only client gets the reference's UnknownFormat 406, and the default
 * HTML format negotiates the same way. */
CF_TEST(refreshes_format_negotiation) {
    refresh_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);

    cf_request suffix_req;
    prepare_refresh(&env, &suffix_req, 1, "/rooms/42/refresh.turbo_stream",
                    NULL, NULL, NULL, NULL);
    cf_response suffix_resp;
    CF_REQUIRE(run_request(&env, &suffix_req, &suffix_resp));
    CF_CHECK(suffix_resp.status == 200);
    CF_CHECK(response_body_equals(&suffix_resp, "\n"));
    cf_response_dispose(&suffix_resp);

    cf_request html_req;
    prepare_refresh(&env, &html_req, 1, "/rooms/42/refresh", NULL,
                    "text/html", NULL, NULL);
    cf_response html_resp;
    CF_REQUIRE(run_request(&env, &html_req, &html_resp));
    CF_CHECK(html_resp.status == 406);
    cf_response_dispose(&html_resp);

    /* No Accept header and no suffix: the default HTML format is not
     * offered either. */
    cf_request default_req;
    prepare_refresh(&env, &default_req, 1, "/rooms/42/refresh", NULL, NULL,
                    NULL, NULL);
    cf_response default_resp;
    CF_REQUIRE(run_request(&env, &default_req, &default_resp));
    CF_CHECK(default_resp.status == 406);
    cf_response_dispose(&default_resp);
    env_close(&env);
}

CF_TEST_MAIN()
