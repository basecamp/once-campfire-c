/* tests/actions/users_profiles_test.c — A-users-profiles acceptance:
 * `users/profiles#show` (route ID 60) and `users/profiles#update` (route
 * IDs 61/62), per docs/devel/implementation/contracts/controller-packets.md
 * "A-users-profiles" and 03-application.md's remaining-packets rule
 * (permit lists, callback order, status/redirect, scoping).
 *
 * Every case runs the real A00 dispatch path through the route double
 * (tests/app/support/route_double.c), which binds rows 60/61/62 to the
 * real actions.  cf.h and src/actions/actions.h are integrator-owned and
 * do not yet declare this packet's symbols, so the entry points (plus the
 * non-static loader) are declared here; the integrator's routes.c rebind
 * needs the same declarations (c_symbols cf_action_users_profiles_show /
 * cf_action_users_profiles_update).
 *
 * Render gate: ProfileShow has no C view yet (profiles.c R1), so show's
 * ordinary path answers the reference's 500 after loading.  The loader
 * itself (transfer_id signing, attached check, direct/shared partition)
 * is exercised directly, and dispatch covers the auth/format paths.
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "http/http_internal.h"
#include "models/active_storage.h"
#include "models/membership.h"
#include "models/room.h"
#include "models/user.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

cf_err cf_action_users_profiles_show(cf_ctx *ctx);
cf_err cf_action_users_profiles_update(cf_ctx *ctx);
cf_err cf_users_profiles_load(cf_db *db, const cf_config *config,
                              int64_t user_id, int64_t now_us,
                              cf_str *transfer_id_out,
                              bool *avatar_attached_out,
                              cf_membership_room_pair_vector *direct_out,
                              cf_membership_room_pair_vector *shared_out);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- scratch app + started writer ------------------------------------------ */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
} profiles_env;

static bool env_open(profiles_env *env) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;
    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", ORIGIN},
        {"SECRET_KEY_BASE", HEX64},
        {"VAPID_PUBLIC_KEY",
         "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"
         "cTriz_qYBVicY02_VxTQ="},
        {"DATABASE_PATH", env->scratch.path},
    };
    if (cf_config_parse(entries, 4, NULL, &env->config) != CF_OK) {
        return false;
    }
    cf_config_test_set_bcrypt_cost(env->config, 4);
    if (cf_app_create(env->config, &env->app) != CF_OK) {
        cf_config_destroy(env->config);
        env->config = NULL;
        return false;
    }
    if (cf_writer_start(env->app, env->config) != CF_OK) return false;
    env->writer_started = true;
    cf_test_routes_reset();
    if (cf_test_routes_add("GET", "/users/:user_id/profile(.:format)", 60,
                           cf_action_users_profiles_show) != CF_OK ||
        cf_test_routes_add("GET", "/users/:user_id/profile", 60,
                           cf_action_users_profiles_show) != CF_OK ||
        cf_test_routes_add("PATCH", "/users/:user_id/profile(.:format)", 61,
                           cf_action_users_profiles_update) != CF_OK ||
        cf_test_routes_add("PATCH", "/users/:user_id/profile", 61,
                           cf_action_users_profiles_update) != CF_OK ||
        cf_test_routes_add("PUT", "/users/:user_id/profile(.:format)", 62,
                           cf_action_users_profiles_update) != CF_OK ||
        cf_test_routes_add("PUT", "/users/:user_id/profile", 62,
                           cf_action_users_profiles_update) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(profiles_env *env) {
    if (env->writer_started) cf_writer_stop(env->app);
    if (env->app != NULL) cf_app_destroy(env->app); /* frees config too */
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

static void read_text(cf_db *db, const char *sql, char *out, size_t cap) {
    CF_REQUIRE(cf_db_test_text(cf_db_handle(db), sql, out, cap) != NULL);
}

/* --- seeding --------------------------------------------------------------- */

static void seed_user(cf_db *db, int64_t id, const char *name,
                      const char *email, const char *updated_at) {
    char sql[768];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, updated_at) "
             "VALUES (%lld, NULL, NULL, '2026-01-02 03:04:05', %s, '%s', "
             "NULL, 0, 0, '%s')",
             (long long)id, email != NULL ? "?" : "NULL", name, updated_at);
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
             "INSERT INTO sessions (created_at, ip_address, "
             "last_active_at, token, updated_at, user_agent, user_id) "
             "VALUES ('2040-01-01 00:00:00.000000', NULL, "
             "'2040-01-01 00:00:00.000000', '%s', "
             "'2040-01-01 00:00:00.000000', NULL, %lld)",
             token, (long long)user_id);
    exec_sql(db, sql);
}

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

static void seed_room(cf_db *db, int64_t id, const char *name,
                      const char *type_text, int64_t creator_id) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (%lld, '2026-01-02 03:04:05', %lld, '%s', "
             "'%s', '2026-01-02 03:04:05')",
             (long long)id, (long long)creator_id, name, type_text);
    exec_sql(db, sql);
}

static void seed_membership(cf_db *db, int64_t id, int64_t room_id,
                            int64_t user_id) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO memberships (id, connected_at, connections, "
             "created_at, involvement, room_id, unread_at, updated_at, "
             "user_id) VALUES (%lld, NULL, 0, '2026-01-02 03:04:05', "
             "'mentions', %lld, NULL, '2026-01-02 03:04:05', %lld)",
             (long long)id, (long long)room_id, (long long)user_id);
    exec_sql(db, sql);
}

static void seed_blob_attachment(cf_db *db, int64_t blob_id,
                                 int64_t attachment_id, int64_t user_id) {
    char sql[768];
    snprintf(sql, sizeof sql,
             "INSERT INTO active_storage_blobs (id, byte_size, checksum, "
             "content_type, created_at, filename, key, metadata, "
             "service_name) VALUES (%lld, 10, NULL, 'image/png', "
             "'2026-01-02 03:04:05', 'a.png', 'key-%lld', NULL, 'disk')",
             (long long)blob_id, (long long)blob_id);
    exec_sql(db, sql);
    snprintf(sql, sizeof sql,
             "INSERT INTO active_storage_attachments (id, blob_id, "
             "created_at, name, record_id, record_type) VALUES (%lld, %lld, "
             "'2026-01-02 03:04:05', 'avatar', %lld, 'User')",
             (long long)attachment_id, (long long)blob_id,
             (long long)user_id);
    exec_sql(db, sql);
}

/* --- request/response helpers ---------------------------------------------- */

static bool run_request(profiles_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void profile_request(cf_request *req, cf_method method,
                            const char *target, const char *body,
                            const char *content_type,
                            const char *sec_fetch_site,
                            const char *cookie_header) {
    cf_test_req_init(req);
    req->method = method;
    req->original_method = method;
    req->path = SP(target);
    req->target = SP(target);
    req->body = SP(body != NULL ? body : "");
    if (content_type != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                      SP(content_type)) == CF_OK);
    }
    if (sec_fetch_site != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                      SP(sec_fetch_site)) == CF_OK);
    }
    if (cookie_header != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"),
                                      SP(cookie_header)) == CF_OK);
    }
}

static bool head_contains(cf_response *resp, cf_request *req,
                          const char *needle) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    size_t len = strlen(needle);
    bool found = false;
    cf_span headers = cf_buf_span(ser.headers);
    if (headers.len >= len) {
        for (size_t i = 0; i + len <= headers.len; i++) {
            if (memcmp(headers.ptr + i, needle, len) == 0) {
                found = true;
                break;
            }
        }
    }
    cf_buf_release(ser.headers);
    return found;
}

/* Run the action on a hand-built context and read the flash map (rooms
 * precedent: the session commit does not persist flash yet). */
static bool run_direct(profiles_env *env, cf_request *req, cf_response *resp,
                       cf_action_fn action, cf_ctx *ctx) {
    CF_REQUIRE(cf_ctx_create(ctx, env->app, env->scratch.db, req, resp) ==
               CF_OK);
    return (*action)(ctx) == CF_OK;
}

static bool flash_value(cf_ctx *ctx, const char *key, const char *expected) {
    cf_span value = {NULL, 0};
    if (cf_ctx_flash_get(ctx, SP(key), &value) != CF_OK) return false;
    size_t len = strlen(expected);
    return value.len == len && memcmp(value.ptr, expected, len) == 0;
}

/* --- acceptance ------------------------------------------------------------ */

CF_TEST(users_profiles_route_ids_bind_the_actions) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/users/:user_id/profile", 60,
                                  cf_action_users_profiles_show) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("PATCH", "/users/:user_id/profile", 61,
                                  cf_action_users_profiles_update) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("PUT", "/users/:user_id/profile", 62,
                                  cf_action_users_profiles_update) == CF_OK);
    CF_CHECK(cf_route_action(60) == cf_action_users_profiles_show);
    CF_CHECK(cf_route_action(61) == cf_action_users_profiles_update);
    CF_CHECK(cf_route_action(62) == cf_action_users_profiles_update);
}

CF_TEST(users_profiles_show_authenticated_gates_on_the_missing_view) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", "david@example.com",
              "2026-01-02 03:04:05");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    profile_request(&req, CF_GET, "/users/me/profile", NULL, NULL,
                    "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    /* R1: everything loads (see the loader cases), but ProfileShow has no
     * C view yet, so the ordinary path fails loudly instead of stubbing. */
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_profiles_show_unauthenticated_redirects_to_sign_in) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2026-01-02 03:04:05");

    cf_request req;
    cf_response resp;
    profile_request(&req, CF_GET, "/users/me/profile", NULL, NULL,
                    "same-origin", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_profiles_show_unacceptable_format_is_406) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2026-01-02 03:04:05");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    profile_request(&req, CF_GET, "/users/me/profile.json", NULL, NULL,
                    "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_profiles_loader_signs_partitions_and_reports_attached) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2026-01-02 03:04:05");
    seed_user(env.scratch.db, 2, "Kevin", NULL, "2026-01-02 03:04:05");
    /* LOWER(name) order: all-talk, Bravo, dm (direct), Zebra. */
    seed_room(env.scratch.db, 10, "Zebra", "Rooms::Open", 2);
    seed_room(env.scratch.db, 11, "all-talk", "Rooms::Open", 2);
    seed_room(env.scratch.db, 12, "dm", "Rooms::Direct", 2);
    seed_room(env.scratch.db, 13, "Bravo", "Rooms::Open", 2);
    seed_membership(env.scratch.db, 101, 10, 1);
    seed_membership(env.scratch.db, 102, 11, 1);
    seed_membership(env.scratch.db, 103, 12, 1);
    seed_membership(env.scratch.db, 104, 13, 1);
    seed_membership(env.scratch.db, 105, 10, 2); /* another user's row */

    cf_str transfer = {0};
    bool attached = true; /* must be overwritten */
    cf_membership_room_pair_vector direct = {0};
    cf_membership_room_pair_vector shared = {0};
    int64_t now = INT64_C(1767225600000000); /* 2026-01-01T00:00:00Z */
    CF_REQUIRE(cf_users_profiles_load(env.scratch.db, env.config, 1, now,
                                      &transfer, &attached, &direct,
                                      &shared) == CF_OK);
    CF_CHECK(!attached);

    /* transfer_id verifies as purpose "transfer" for this user. */
    cf_optional_i64 verified = {false, 0};
    bool found = false;
    CF_REQUIRE(cf_auth_signed_id_verify(
                   (cf_span){(const unsigned char *)env.config->secret_key_base,
                             env.config->secret_key_base_len},
                   SP("User"),
                   (cf_span){(const unsigned char *)transfer.ptr,
                             transfer.len},
                   SP("transfer"), true, now, &verified, &found) == CF_OK);
    CF_CHECK(found && verified.present && verified.value == 1);
    /* ...and for no other purpose. */
    cf_optional_i64 wrong = {false, 0};
    bool wrong_found = true;
    CF_REQUIRE(cf_auth_signed_id_verify(
                   (cf_span){(const unsigned char *)env.config->secret_key_base,
                             env.config->secret_key_base_len},
                   SP("User"),
                   (cf_span){(const unsigned char *)transfer.ptr,
                             transfer.len},
                   SP("avatar"), true, now, &wrong, &wrong_found) == CF_OK);
    CF_CHECK(!wrong_found);

    /* Partition keeps with_ordered_room order within each side. */
    CF_REQUIRE(shared.len == 3 && direct.len == 1);
    CF_CHECK(shared.items[0].room.id == 11);
    CF_CHECK(shared.items[1].room.id == 13);
    CF_CHECK(shared.items[2].room.id == 10);
    CF_CHECK(direct.items[0].room.id == 12);
    for (size_t i = 0; i < shared.len; i++) {
        CF_CHECK(shared.items[i].membership.user_id == 1);
        CF_CHECK(shared.items[i].room.room_type != CF_ROOM_DIRECT);
    }
    CF_CHECK(direct.items[0].room.room_type == CF_ROOM_DIRECT);

    cf_str_dispose(&transfer);
    cf_membership_room_pair_vector_dispose(&direct);
    cf_membership_room_pair_vector_dispose(&shared);

    /* With an avatar attached, the flag flips and nothing else changes. */
    seed_blob_attachment(env.scratch.db, 50, 60, 1);
    memset(&transfer, 0, sizeof transfer);
    memset(&direct, 0, sizeof direct);
    memset(&shared, 0, sizeof shared);
    CF_REQUIRE(cf_users_profiles_load(env.scratch.db, env.config, 1, now,
                                      &transfer, &attached, &direct,
                                      &shared) == CF_OK);
    CF_CHECK(attached);
    CF_CHECK(shared.len == 3 && direct.len == 1);
    cf_str_dispose(&transfer);
    cf_membership_room_pair_vector_dispose(&direct);
    cf_membership_room_pair_vector_dispose(&shared);
    env_close(&env);
}

CF_TEST(users_profiles_update_renames_with_check_notice) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PATCH, "/users/me/profile", "user[name]=Dave",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/users/me/profile\r\n"));
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    char name[64];
    read_text(env.scratch.db,
              "SELECT \"name\" FROM \"users\" WHERE \"id\" = 1", name,
              sizeof name);
    CF_CHECK(strcmp(name, "Dave") == 0);
    /* updated_at moved (the reference writes it when something changed). */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM \"users\" WHERE \"id\" = 1 "
                        "AND \"updated_at\" > '2020-01-01 00:00:00'") == 1);
    cf_response_dispose(&resp);

    /* The notice is "✓": no avatar key was given. */
    cf_request req2;
    cf_response resp2;
    cf_ctx ctx;
    profile_request(&req2, CF_PATCH, "/users/1/profile", "user[name]=Dave2",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    cf_response_init(&resp2);
    CF_REQUIRE(run_direct(&env, &req2, &resp2,
                          cf_action_users_profiles_update, &ctx));
    CF_CHECK(cf_auth_halted(&ctx));
    CF_CHECK(resp2.status == 302);
    CF_CHECK(flash_value(&ctx, "notice", "\342\234\223"));
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp2);
    env_close(&env);
}

CF_TEST(users_profiles_update_put_alias_and_email_bio) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", "old@example.com",
              "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    /* Route 62 (PUT) shares the update function. */
    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PUT, "/users/me/profile",
                    "user[email_address]=new%40example.com&user[bio]=Hello",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/users/me/profile\r\n"));
    char email[128];
    read_text(env.scratch.db,
              "SELECT \"email_address\" FROM \"users\" WHERE \"id\" = 1",
              email, sizeof email);
    CF_CHECK(strcmp(email, "new@example.com") == 0);
    char bio[128];
    read_text(env.scratch.db,
              "SELECT \"bio\" FROM \"users\" WHERE \"id\" = 1", bio,
              sizeof bio);
    CF_CHECK(strcmp(bio, "Hello") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_profiles_update_empty_avatar_deletes_with_long_notice) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    /* No attachment: Delete is a no-op that still redirects. */
    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PATCH, "/users/me/profile", "user[avatar]=",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM "
                        "active_storage_attachments") == 0);
    cf_response_dispose(&resp);

    /* The notice is the 30-minute text: the avatar key was non-nil. */
    cf_request req2;
    cf_response resp2;
    cf_ctx ctx;
    profile_request(&req2, CF_PATCH, "/users/1/profile", "user[avatar]=",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    cf_response_init(&resp2);
    CF_REQUIRE(run_direct(&env, &req2, &resp2,
                          cf_action_users_profiles_update, &ctx));
    CF_CHECK(resp2.status == 302);
    CF_CHECK(flash_value(&ctx, "notice",
                         "It may take up to 30 minutes to change "
                         "everywhere."));
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp2);
    env_close(&env);
}

CF_TEST(users_profiles_update_avatar_delete_destroys_touches_purges) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2020-01-01 00:00:00");
    seed_user(env.scratch.db, 2, "Kevin", NULL, "2020-01-01 00:00:00");
    seed_blob_attachment(env.scratch.db, 50, 60, 1);
    seed_blob_attachment(env.scratch.db, 51, 61, 2);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PATCH, "/users/me/profile",
                    "user[avatar]=&user[name]=Dave",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    /* Only the current user's attachment goes; the blob row stays (the
     * purge job owns file/row removal). */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_attachments "
                        "WHERE record_id = 1") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_attachments "
                        "WHERE record_id = 2") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_blobs "
                        "WHERE id = 50") == 1);
    /* The user row was touched by the destroy. */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM \"users\" WHERE \"id\" = 1 "
                        "AND \"updated_at\" > '2020-01-01 00:00:00'") == 1);
    /* Best-effort PurgeBlob: no consumer registered, so dropped+counted
     * with the committed write unchanged (bans precedent). */
    cf_writer_stats stats;
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.best_effort_dropped[CF_EVENT_PURGE_BLOB] == 1);
    CF_CHECK(stats.committed == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_profiles_update_avatar_string_is_internal) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    /* A plain string is Assignment::Invalid ("Could not find or build
     * blob"), the reference's 500 — and the user row is untouched. */
    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PATCH, "/users/me/profile",
                    "user[avatar]=not-a-blob&user[name]=Dave",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    char name[64];
    read_text(env.scratch.db,
              "SELECT \"name\" FROM \"users\" WHERE \"id\" = 1", name,
              sizeof name);
    CF_CHECK(strcmp(name, "David") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_profiles_update_password_sets_digest_blank_keeps) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2020-01-01 00:00:00");
    /* Bcrypt the seed digest through the same service (cost 4). */
    cf_str before = {0};
    CF_REQUIRE(cf_auth_password_digest((cf_str){(char *)"old-pw", 6}, 4,
                                       &before) == CF_OK);
    {
        sqlite3 *handle = cf_db_handle(env.scratch.db);
        sqlite3_stmt *stmt = NULL;
        CF_REQUIRE(sqlite3_prepare_v2(handle,
                                      "UPDATE users SET password_digest = "
                                      "? WHERE id = 1",
                                      -1, &stmt, NULL) == SQLITE_OK);
        sqlite3_bind_text(stmt, 1, before.ptr, -1, SQLITE_TRANSIENT);
        CF_REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PATCH, "/users/me/profile",
                    "user[password]=brand-new-password",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    char after[256];
    read_text(env.scratch.db,
              "SELECT \"password_digest\" FROM \"users\" WHERE \"id\" = 1",
              after, sizeof after);
    CF_CHECK(strcmp(after, before.ptr) != 0);
    bool ok = false;
    cf_user candidate = {0};
    bool found = false;
    CF_REQUIRE(cf_user_find_by_id(env.scratch.db, 1, &found, &candidate) ==
               CF_OK);
    CF_REQUIRE(found);
    CF_REQUIRE(cf_user_authenticate(&candidate,
                                    (cf_str){(char *)"brand-new-password",
                                             18},
                                    &ok) == CF_OK);
    CF_CHECK(ok);
    cf_user_dispose(&candidate);
    cf_response_dispose(&resp);

    /* A blank password leaves the digest alone (`password=` ignores it). */
    cf_request req2;
    cf_response resp2;
    profile_request(&req2, CF_PATCH, "/users/me/profile", "user[password]=",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 302);
    char kept[256];
    read_text(env.scratch.db,
              "SELECT \"password_digest\" FROM \"users\" WHERE \"id\" = 1",
              kept, sizeof kept);
    CF_CHECK(strcmp(kept, after) == 0);
    cf_response_dispose(&resp2);
    cf_str_dispose(&before);
    env_close(&env);
}

CF_TEST(users_profiles_update_json_false_is_a_noop_redirect) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    /* `require` accepts the false literal; permit of it is empty, so the
     * update changes nothing but still redirects with "✓". */
    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PATCH, "/users/me/profile", "{\"user\":false}",
                    "application/json", "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/users/me/profile\r\n"));
    char name[64];
    read_text(env.scratch.db,
              "SELECT \"name\" FROM \"users\" WHERE \"id\" = 1", name,
              sizeof name);
    CF_CHECK(strcmp(name, "David") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_profiles_update_missing_user_param_is_400) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PATCH, "/users/me/profile", "name=Dave",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 400);
    char name[64];
    read_text(env.scratch.db,
              "SELECT \"name\" FROM \"users\" WHERE \"id\" = 1", name,
              sizeof name);
    CF_CHECK(strcmp(name, "David") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_profiles_update_unauthenticated_redirects_to_sign_in) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2020-01-01 00:00:00");

    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PATCH, "/users/me/profile", "user[name]=Dave",
                    "application/x-www-form-urlencoded", "same-origin",
                    NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    char name[64];
    read_text(env.scratch.db,
              "SELECT \"name\" FROM \"users\" WHERE \"id\" = 1", name,
              sizeof name);
    CF_CHECK(strcmp(name, "David") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_profiles_update_cross_site_post_is_422) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PATCH, "/users/me/profile", "user[name]=Dave",
                    "application/x-www-form-urlencoded", "cross-site",
                    cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    char name[64];
    read_text(env.scratch.db,
              "SELECT \"name\" FROM \"users\" WHERE \"id\" = 1", name,
              sizeof name);
    CF_CHECK(strcmp(name, "David") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
