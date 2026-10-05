/* tests/actions/users_avatars_destroy_test.c — A-users-avatars (destroy
 * half only) acceptance: `users/avatars#destroy` (route ID 54), per
 * docs/devel/implementation/contracts/controller-packets.md
 * "A-users-avatars" and 03-application.md's remaining-packets rule
 * (callback order, status/redirect, scoping).  Route 53 `show` belongs to
 * another lane and is not covered here.
 *
 * Every case runs the real A00 dispatch path through the route double
 * (tests/app/support/route_double.c), which binds row 54 to the real
 * action.  cf.h and src/actions/actions.h are integrator-owned and do not
 * yet declare this packet's symbol, so the entry point is declared here;
 * the integrator's routes.c rebind needs the same declaration (c_symbol
 * cf_action_users_avatars_destroy).
 *
 * Destroy needs no S02/S03 completion (rows plus a best-effort purge
 * event for J02's kind), so every path is fully implemented: attachment
 * removal, the users touch, the PurgeBlob drop-count without a consumer,
 * the unconditional profile redirect, and the auth/CSRF/scoping failure
 * paths.
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
#include "models/user.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

cf_err cf_action_users_avatars_destroy(cf_ctx *ctx);

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
} avatars_env;

static bool env_open(avatars_env *env) {
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
    if (cf_app_create(env->config, &env->app) != CF_OK) {
        cf_config_destroy(env->config);
        env->config = NULL;
        return false;
    }
    if (cf_writer_start(env->app, env->config) != CF_OK) return false;
    env->writer_started = true;
    cf_test_routes_reset();
    if (cf_test_routes_add("DELETE", "/users/:user_id/avatar(.:format)", 54,
                           cf_action_users_avatars_destroy) != CF_OK ||
        cf_test_routes_add("DELETE", "/users/:user_id/avatar", 54,
                           cf_action_users_avatars_destroy) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(avatars_env *env) {
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

/* --- seeding --------------------------------------------------------------- */

static void seed_user(cf_db *db, int64_t id, const char *name,
                      const char *updated_at) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, updated_at) "
             "VALUES (%lld, NULL, NULL, '2026-01-02 03:04:05', NULL, '%s', "
             "NULL, 0, 0, '%s')",
             (long long)id, name, updated_at);
    exec_sql(db, sql);
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

static bool run_request(avatars_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void avatar_request(cf_request *req, const char *target,
                           const char *sec_fetch_site,
                           const char *cookie_header) {
    cf_test_req_init(req);
    req->method = CF_DELETE;
    req->original_method = CF_DELETE;
    req->path = SP(target);
    req->target = SP(target);
    req->body = SP("");
    CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                  SP(sec_fetch_site)) == CF_OK);
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

/* --- acceptance ------------------------------------------------------------ */

CF_TEST(users_avatars_destroy_route_id_binds_the_action) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("DELETE", "/users/:user_id/avatar", 54,
                                  cf_action_users_avatars_destroy) == CF_OK);
    CF_CHECK(cf_route_action(54) == cf_action_users_avatars_destroy);
}

CF_TEST(users_avatars_destroy_removes_touches_purges_and_redirects) {
    avatars_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", "2020-01-01 00:00:00");
    seed_user(env.scratch.db, 2, "Kevin", "2020-01-01 00:00:00");
    seed_blob_attachment(env.scratch.db, 50, 60, 1);
    seed_blob_attachment(env.scratch.db, 51, 61, 2);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    avatar_request(&req, "/users/me/avatar", "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/users/me/profile\r\n"));
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    /* Only the current user's attachment goes (self-scoped, AUTH-06). */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_attachments "
                        "WHERE record_id = 1") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_attachments "
                        "WHERE record_id = 2") == 1);
    /* The blob row stays: the purge job owns file/row removal. */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_blobs WHERE "
                        "id = 50") == 1);
    /* The user row was touched by the destroy. */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM \"users\" WHERE \"id\" = 1 "
                        "AND \"updated_at\" > '2020-01-01 00:00:00'") == 1);
    /* Best-effort PurgeBlob: no consumer registered, so dropped+counted
     * with the committed write unchanged. */
    cf_writer_stats stats;
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.best_effort_dropped[CF_EVENT_PURGE_BLOB] == 1);
    CF_CHECK(stats.committed == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_avatars_destroy_without_attachment_is_a_quiet_redirect) {
    avatars_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    avatar_request(&req, "/users/me/avatar", "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/users/me/profile\r\n"));
    /* Nothing to destroy: no touch, no event. */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM \"users\" WHERE \"id\" = 1 "
                        "AND \"updated_at\" = '2020-01-01 00:00:00'") == 1);
    cf_writer_stats stats;
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.best_effort_dropped[CF_EVENT_PURGE_BLOB] == 0);
    CF_CHECK(stats.committed == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_avatars_destroy_unauthenticated_redirects_to_sign_in) {
    avatars_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", "2020-01-01 00:00:00");
    seed_blob_attachment(env.scratch.db, 50, 60, 1);

    cf_request req;
    cf_response resp;
    avatar_request(&req, "/users/me/avatar", "same-origin", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_attachments") ==
              1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_avatars_destroy_cross_site_delete_is_422) {
    avatars_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", "2020-01-01 00:00:00");
    seed_blob_attachment(env.scratch.db, 50, 60, 1);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    avatar_request(&req, "/users/me/avatar", "cross-site", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_attachments") ==
              1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
