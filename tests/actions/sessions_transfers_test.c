/* tests/actions/sessions_transfers_test.c — A-sessions-transfers acceptance:
 * `sessions/transfers#show` (route ID 9) and `#update` (route IDs 10/11),
 * per docs/devel/implementation/contracts/controller-packets.md
 * "A-sessions-transfers".
 *
 * Cases:
 *   - route binding: ids 9/10/11 resolve to the two actions;
 *   - show: 200, HTML, the auto-submitting PUT form posts back to the
 *     request path (TransferShow action), in the frame layout for a
 *     Turbo-Frame request;
 *   - update success (PUT and PATCH): the active user is signed in (a
 *     session row plus the session_token cookie) and the response is 302
 *     to the post-authenticating URL (root when nothing was stored);
 *   - update failures: unknown/tampered/expired transfer ids and an
 *     inactive user are all head-400 with no session row and no cookie;
 *   - transfer ids are bearer tokens with no one-use step in the pinned
 *     source: reusing one signs in again (pinned, not assumed);
 *   - unauthenticated access is allowed throughout (no redirect to
 *     sign-in); a cross-site PUT is 422.
 *
 * The route double binds rows 9/10/11 to the real actions, so every case
 * runs the real A00 dispatch path.  The writer is started (start_new_session
 * writes through it).
 *
 * cf.h and src/actions/actions.h are integrator-owned and do not yet declare
 * this packet's symbols, so the entry points are declared here; the
 * integrator's routes.c rebind needs the same declarations (c_symbols
 * cf_action_sessions_transfers_show / cf_action_sessions_transfers_update).
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

cf_err cf_action_sessions_transfers_show(cf_ctx *ctx);
cf_err cf_action_sessions_transfers_update(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="
#define TRANSFER_EXPIRY_US (INT64_C(4) * INT64_C(3600) * INT64_C(1000000))

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
} transfers_env;

/* Page-layout renders read the configured asset module (stylesheet /
 * importmap tags); the pinned fixture tree serves directly. */
static int g_assets_ready = 0;

static bool env_open(transfers_env *env) {
    memset(env, 0, sizeof *env);
    if (!g_assets_ready) {
        if (cf_views_assets_configure("tests/fixtures/assets") != CF_OK) {
            return false;
        }
        g_assets_ready = 1;
    }
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
    if (cf_writer_start(env->app, env->config) != CF_OK) return false;
    env->writer_started = true;
    cf_test_routes_reset();
    if (cf_test_routes_add("GET", "/session/transfers/:id(.:format)", 9,
                           cf_action_sessions_transfers_show) != CF_OK ||
        cf_test_routes_add("GET", "/session/transfers/:id", 9,
                           cf_action_sessions_transfers_show) != CF_OK ||
        cf_test_routes_add("PATCH", "/session/transfers/:id(.:format)", 10,
                           cf_action_sessions_transfers_update) != CF_OK ||
        cf_test_routes_add("PATCH", "/session/transfers/:id", 10,
                           cf_action_sessions_transfers_update) != CF_OK ||
        cf_test_routes_add("PUT", "/session/transfers/:id(.:format)", 11,
                           cf_action_sessions_transfers_update) != CF_OK ||
        cf_test_routes_add("PUT", "/session/transfers/:id", 11,
                           cf_action_sessions_transfers_update) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(transfers_env *env) {
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

static int64_t count_rows(cf_db *db, const char *sql) {
    bool ok = false;
    int64_t value = cf_db_test_i64(cf_db_handle(db), sql, &ok);
    CF_REQUIRE(ok);
    return value;
}

static void seed_user(cf_db *db, int64_t id, const char *name, int status) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, updated_at) "
             "VALUES (%lld, NULL, NULL, '2026-01-02 03:04:05', NULL, '%s', "
             "NULL, 0, %d, '2026-01-02 03:04:05')",
             (long long)id, name, status);
    exec_sql(db, sql);
}

static bool run_request(transfers_env *env, cf_request *req,
                        cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void get_transfer(cf_request *req, const char *id) {
    static char target[1024];
    cf_test_req_init(req);
    snprintf(target, sizeof target, "/session/transfers/%s", id);
    req->path = SP(target);
    req->target = SP(target);
}

static void put_transfer(cf_request *req, const char *id, const char *method,
                         const char *sec_fetch_site) {
    static char target[1024];
    cf_test_req_init(req);
    if (strcmp(method, "PATCH") == 0) {
        req->method = CF_PATCH;
        req->original_method = CF_PATCH;
    } else {
        req->method = CF_PUT;
        req->original_method = CF_PUT;
    }
    snprintf(target, sizeof target, "/session/transfers/%s", id);
    req->path = SP(target);
    req->target = SP(target);
    CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                  SP(sec_fetch_site)) == CF_OK);
}

/* A transfer id for user_id, expiring `expiry_offset_us` from now. */
static void make_transfer_id(transfers_env *env, int64_t user_id,
                              int64_t expiry_offset_us, char *out,
                              size_t cap) {
    cf_str signed_id = {0};
    CF_REQUIRE(cf_auth_signed_id_generate(
                   (cf_span){(const unsigned char *)env->config
                                 ->secret_key_base,
                             env->config->secret_key_base_len},
                   SP("User"), user_id, SP("transfer"), true, true,
                   cf_now_us(env->app) + expiry_offset_us,
                   &signed_id) == CF_OK);
    CF_REQUIRE(signed_id.len < cap);
    memcpy(out, signed_id.ptr, signed_id.len);
    out[signed_id.len] = '\0';
    cf_str_dispose(&signed_id);
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

static bool head_contains(cf_response *resp, cf_request *req,
                          const char *needle) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    bool found = buf_contains(ser.headers, needle);
    cf_buf_release(ser.headers);
    return found;
}

static bool has_set_cookie(cf_response *resp, cf_request *req,
                           const char *name) {
    char needle[128];
    snprintf(needle, sizeof needle, "Set-Cookie: %s=", name);
    return head_contains(resp, req, needle);
}

/* --- acceptance ------------------------------------------------------------ */

CF_TEST(transfers_route_ids_bind_the_actions) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/session/transfers/:id", 9,
                                  cf_action_sessions_transfers_show) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("PATCH", "/session/transfers/:id", 10,
                                  cf_action_sessions_transfers_update) ==
               CF_OK);
    CF_REQUIRE(cf_test_routes_add("PUT", "/session/transfers/:id", 11,
                                  cf_action_sessions_transfers_update) ==
               CF_OK);
    CF_CHECK(cf_route_action(9) == cf_action_sessions_transfers_show);
    CF_CHECK(cf_route_action(10) == cf_action_sessions_transfers_update);
    CF_CHECK(cf_route_action(11) == cf_action_sessions_transfers_update);
}

CF_TEST(transfers_show_renders_the_auto_submitting_form) {
    transfers_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    cf_response resp;
    get_transfer(&req, "whatever");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    /* TransferShow: the PUT form posts back to the request's own path. */
    CF_CHECK(buf_contains(resp.body, "data-controller=\"auto-submit\""));
    CF_CHECK(buf_contains(resp.body, "/session/transfers/whatever"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(transfers_show_turbo_frame_renders_the_frame_layout) {
    transfers_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    cf_response resp;
    get_transfer(&req, "whatever");
    CF_REQUIRE(cf_test_req_header(&req, SP("Turbo-Frame"), SP("main")) ==
               CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(buf_contains(resp.body, "data-controller=\"auto-submit\""));
    CF_CHECK(!head_contains(&resp, &req, "Link: "));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(transfers_show_unacceptable_format_is_406) {
    transfers_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    cf_response resp;
    get_transfer(&req, "whatever");
    CF_REQUIRE(cf_test_req_header(&req, SP("Accept"), SP("application/xml")) ==
               CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(transfers_update_signs_in_and_redirects_root) {
    transfers_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 7, "Mover", 0);
    char id[512];
    make_transfer_id(&env, 7, TRANSFER_EXPIRY_US, id, sizeof id);

    cf_request req;
    cf_response resp;
    put_transfer(&req, id, "PUT", "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, "Location: " ORIGIN "/\r\n"));
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM sessions "
                                        "WHERE user_id=7") == 1);
    CF_CHECK(has_set_cookie(&resp, &req, "session_token"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(transfers_update_patch_signs_in_too) {
    transfers_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 8, "Patcher", 0);
    char id[512];
    make_transfer_id(&env, 8, TRANSFER_EXPIRY_US, id, sizeof id);

    cf_request req;
    cf_response resp;
    put_transfer(&req, id, "PATCH", "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, "Location: " ORIGIN "/\r\n"));
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM sessions "
                                        "WHERE user_id=8") == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(transfers_update_rejects_bad_ids_with_400) {
    transfers_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 7, "Mover", 0);

    /* Unknown user, tampered token, expired token, non-string id. */
    char unknown[512], expired[512];
    make_transfer_id(&env, 4242, TRANSFER_EXPIRY_US, unknown, sizeof unknown);
    make_transfer_id(&env, 7, -INT64_C(3600000000), expired, sizeof expired);
    char tampered[512];
    make_transfer_id(&env, 7, TRANSFER_EXPIRY_US, tampered, sizeof tampered);
    tampered[strlen(tampered) - 1] =
        tampered[strlen(tampered) - 1] == 'A' ? 'B' : 'A';

    const char *ids[] = {unknown, expired, tampered, "!!!"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        cf_request req;
        cf_response resp;
        put_transfer(&req, ids[i], "PUT", "same-origin");
        CF_REQUIRE(run_request(&env, &req, &resp));
        if (resp.status != 400) {
            fprintf(stderr, "  id %s: status=%u\n", ids[i], resp.status);
        }
        CF_CHECK(resp.status == 400);
        cf_response_dispose(&resp);
    }
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM sessions") ==
             0);
    env_close(&env);
}

CF_TEST(transfers_update_inactive_user_is_400) {
    transfers_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 9, "Banned", 2);
    char id[512];
    make_transfer_id(&env, 9, TRANSFER_EXPIRY_US, id, sizeof id);

    cf_request req;
    cf_response resp;
    put_transfer(&req, id, "PUT", "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 400);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM sessions") ==
             0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(transfers_update_token_is_reusable) {
    transfers_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 7, "Mover", 0);
    char id[512];
    make_transfer_id(&env, 7, TRANSFER_EXPIRY_US, id, sizeof id);

    for (int round = 0; round < 2; round++) {
        cf_request req;
        cf_response resp;
        put_transfer(&req, id, "PUT", "same-origin");
        CF_REQUIRE(run_request(&env, &req, &resp));
        CF_CHECK(resp.status == 302);
        cf_response_dispose(&resp);
    }
    /* No one-use invalidation in the pinned source: both PUTs signed in. */
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM sessions "
                                        "WHERE user_id=7") == 2);
    env_close(&env);
}

CF_TEST(transfers_update_cross_site_put_is_422) {
    transfers_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 7, "Mover", 0);
    char id[512];
    make_transfer_id(&env, 7, TRANSFER_EXPIRY_US, id, sizeof id);

    cf_request req;
    cf_response resp;
    put_transfer(&req, id, "PUT", "cross-site");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM sessions") ==
             0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
