/* tests/actions/unfurl_links_test.c — A-unfurl_links acceptance:
 * `unfurl_links#create` (route ID 148), per
 * docs/devel/implementation/contracts/controller-packets.md
 * "A-unfurl_links": the composer asks for a pasted URL's OpenGraph
 * metadata through I01's cf_unfurl, with the no-unfurl/timeout arms
 * preserved (204, never an error page).
 *
 * Cases (all local, no network):
 *   - route binding: id 148 resolves to the action;
 *   - a missing `url` is the controller's 400, as is a blank one;
 *   - a hash or an array `url` passes `require` but unfurls nothing
 *     (204);
 *   - a private address unfurls nothing (204) through I01's guard;
 *   - unauthenticated is the sign-in redirect, cross-site is 422.
 *
 * The route double (tests/app/support/route_double.c) binds the row to the
 * real action, so every case runs the real A00 dispatch path.
 *
 * cf.h is integrator-owned and does not yet declare this packet's symbol,
 * so the entry point is declared here; the integrator's routes.c rebind
 * needs the same declaration (c_symbol cf_action_unfurl_links_create).
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "core/testclock.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "http/http_internal.h"
#include "models/user.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

cf_err cf_action_unfurl_links_create(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- environment ------------------------------------------------------------ */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
    int session_seq;
} unfurl_env;

static bool env_open(unfurl_env *env) {
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

    cf_test_routes_reset();
    /* routes.json id 148. */
    if (cf_test_routes_add("POST", "/unfurl_link(.:format)", 148,
                           cf_action_unfurl_links_create) != CF_OK ||
        cf_test_routes_add("POST", "/unfurl_link", 148,
                           cf_action_unfurl_links_create) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(unfurl_env *env) {
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

static void seed_user(cf_db *db) {
    static const char sql[] =
        "INSERT INTO users (id, bio, bot_token, created_at, email_address, "
        "name, password_digest, role, status, updated_at) VALUES (11, NULL, "
        "NULL, '2026-09-26 12:00:00.000000', NULL, 'David', NULL, 1, 0, "
        "'2026-09-26 12:00:00.000000')";
    exec_sql(db, sql);
}

static void make_session_cookie(unfurl_env *env, int64_t user_id, char *out,
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

static bool run_request(unfurl_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void req_form(cf_request *req, const char *body, const char *cookie,
                     const char *sec_fetch_site) {
    cf_test_req_init(req);
    req->method = CF_POST;
    req->original_method = CF_POST;
    req->path = SP("/unfurl_link");
    req->target = SP("/unfurl_link");
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

/* --- acceptance ------------------------------------------------------------ */

CF_TEST(unfurl_links_route_id_binds_the_action) {
    unfurl_env env;
    CF_REQUIRE(env_open(&env));
    CF_CHECK(cf_route_action(148) == cf_action_unfurl_links_create);
    env_close(&env);
}

CF_TEST(unfurl_links_create_param_gates) {
    unfurl_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db);
    char cookie[2048];
    make_session_cookie(&env, 11, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    /* A missing `url` is the controller's 400. */
    req_form(&req, "other=1", cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 400);
    cf_response_dispose(&resp);
    /* A blank `url` is the controller's 400. */
    req_form(&req, "url=", cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 400);
    cf_response_dispose(&resp);
    /* A hash passes `require` but unfurls nothing (204). */
    req_form(&req, "url[a]=http://example.com/", cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 204);
    cf_response_dispose(&resp);
    /* An array passes `require` but unfurls nothing (204). */
    req_form(&req, "url[]=http://example.com/", cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 204);
    cf_response_dispose(&resp);
    /* A private address unfurls nothing (204) through I01's guard. */
    req_form(&req, "url=http://127.0.0.1/secret", cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 204);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(unfurl_links_create_auth_gates) {
    unfurl_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db);
    char cookie[2048];
    make_session_cookie(&env, 11, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    /* Unauthenticated. */
    req_form(&req, "url=http://127.0.0.1/secret", NULL, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    /* Cross-site. */
    req_form(&req, "url=http://127.0.0.1/secret", cookie, "cross-site");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
