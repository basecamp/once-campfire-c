/* tests/actions/pwa_test.c — A-pwa acceptance: `pwa#manifest` (route ID
 * 149) and `pwa#service_worker` (route ID 150)
 * (docs/devel/implementation/contracts/controller-packets.md "A-pwa").
 *
 * Every case runs the real A00 dispatch path (params, cookies,
 * before-actions, format negotiation) through the route double
 * (tests/app/support/route_double.c), which binds rows 149/150 to the two
 * actions.  Both actions allow unauthenticated access and skip forgery
 * protection; the matrix covers the public success paths (with and without
 * an account row, signed-in and anonymous), the JSON escaping of the
 * account name, format negotiation ([JSON]/[JS], suffix and Accept forms,
 * 406 otherwise), and the verbatim service-worker bytes.
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "db/db_testutil.h"
#include "http/http_internal.h"
#include "models/account.h"
#include "views.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ORIGIN "http://campfire.test"

/* routes.json rows 149, 150.  cf.h is frozen and no packet-action header
 * exists yet, so the test declares them (the integrator's routes.c rebind
 * needs the same declarations). */
cf_err cf_action_pwa_manifest(cf_ctx *ctx);
cf_err cf_action_pwa_service_worker(cf_ctx *ctx);

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- scratch app + database ------------------------------------------------ */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
} pwa_env;

static bool env_open(pwa_env *env) {
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
    /* The manifest resolves its shortcut/screenshot image URLs through the
     * real asset module (the pinned manifest, whose digests match the
     * fixture map); an unconfigured module would fail the render. */
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
    bool ok =
        cf_test_routes_add("GET", "/webmanifest(.:format)", 149,
                           cf_action_pwa_manifest) == CF_OK &&
        cf_test_routes_add("GET", "/webmanifest", 149,
                           cf_action_pwa_manifest) == CF_OK &&
        cf_test_routes_add("GET", "/service-worker(.:format)", 150,
                           cf_action_pwa_service_worker) == CF_OK &&
        cf_test_routes_add("GET", "/service-worker", 150,
                           cf_action_pwa_service_worker) == CF_OK;
    if (!ok) {
        fprintf(stderr, "  env_open: route registration failed\n");
        cf_app_destroy(env->app);
        env->app = NULL;
        cf_db_scratch_close(&env->scratch);
        return false;
    }
    return true;
}

static void env_close(pwa_env *env) {
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

static void seed_account(cf_db *db, const char *name, const char *updated_at) {
    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
             "name, settings, singleton_guard, updated_at) VALUES "
             "(873240054, '2026-09-26 13:03:57.000000', NULL, "
             "'CRMu-l8Ge-KB9B', '%s', NULL, 0, '%s')",
             name, updated_at);
    exec_sql(db, sql);
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

/* A session row whose last_active_at is in the future, so restoring it never
 * needs the writer. */
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

static bool run_request(pwa_env *env, cf_request *req, cf_response *resp) {
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

static void prepare_get(cf_request *req, const char *path) {
    cf_test_req_init(req);
    req->path = SP(path);
    req->target = req->path;
}

/* --- route IDs ------------------------------------------------------------- */

CF_TEST(pwa_route_ids_bind_the_actions) {
    pwa_env env;
    CF_REQUIRE(env_open(&env));
    CF_CHECK(cf_route_action(149) == cf_action_pwa_manifest);
    CF_CHECK(cf_route_action(150) == cf_action_pwa_service_worker);
    env_close(&env);
}

/* --- manifest -------------------------------------------------------------- */

/* Anonymous: allow_unauthenticated_access answers 200, never a sign-in
 * redirect.  The account name, the versioned logo paths and the absolute
 * asset image URLs come from the row and the asset map. */
CF_TEST(pwa_manifest_anonymous_renders_the_account) {
    pwa_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db, "37signals", "2026-09-26 13:03:57.000000");

    cf_request req;
    prepare_get(&req, "/webmanifest.json");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(response_head_contains(
        &resp, &req, "Content-Type: application/json; charset=utf-8\r\n"));
    CF_CHECK(response_body_contains(&resp, "\"name\": \"37signals\""));
    /* fresh_account_logo_path(size: :small) and (size: nil). */
    CF_CHECK(response_body_contains(
        &resp, "\"src\": \"/account/logo?size=small&v=20260926130357\""));
    CF_CHECK(response_body_contains(&resp,
                                    "\"src\": \"/account/logo?v=2026092613"
                                    "0357\""));
    /* image_url: PUBLIC_ORIGIN + the digested asset path. */
    CF_CHECK(response_body_contains(
        &resp, "\"src\": \"" ORIGIN "/assets/add-"));
    CF_CHECK(response_body_contains(
        &resp, "\"src\": \"" ORIGIN "/assets/person-"));
    CF_CHECK(response_body_contains(
        &resp, "\"src\": \"" ORIGIN "/assets/screenshots/android-chat-"));
    CF_CHECK(response_body_contains(&resp, "\"start_url\": \"/\""));
    CF_CHECK(response_body_contains(&resp, "\"display\": \"standalone\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Signed in: require_unauthenticated is not set, so the session still gets
 * the manifest (and the response is identical). */
CF_TEST(pwa_manifest_signed_in_renders_the_account) {
    pwa_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db, "37signals", "2026-09-26 13:03:57.000000");
    seed_user(env.scratch.db, 1, "David");

    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "pwa-token-1", 1);
    cf_request req;
    prepare_get(&req, "/webmanifest.json");
    CF_REQUIRE(cf_test_req_header(&req, SP("Cookie"), SP(cookie)) == CF_OK);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(response_body_contains(&resp, "\"name\": \"37signals\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* No account row yet: the name defaults to "Campfire" and the logo paths
 * carry no `v=` (fresh_account_logo with v: nil). */
CF_TEST(pwa_manifest_without_account_uses_defaults) {
    pwa_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    prepare_get(&req, "/webmanifest.json");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(response_body_contains(&resp, "\"name\": \"Campfire\""));
    CF_CHECK(response_body_contains(&resp,
                                    "\"src\": \"/account/logo?size=small\""));
    CF_CHECK(response_body_contains(&resp, "\"src\": \"/account/logo\""));
    CF_CHECK(!response_body_contains(&resp, "?v="));
    CF_CHECK(!response_body_contains(&resp, "&v="));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* The ERB template HTML-escaped its values (breaking the JSON); the port
 * emits JSON strings, so a quote/backslash name stays valid JSON. */
CF_TEST(pwa_manifest_escapes_the_account_name_as_json) {
    pwa_env env;
    CF_REQUIRE(env_open(&env));
    /* SQL escaping doubles the quote; the stored name is Q"R\S. */
    seed_account(env.scratch.db, "Q\"R\\S", "2026-09-26 13:03:57.000000");

    cf_request req;
    prepare_get(&req, "/webmanifest.json");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(response_body_contains(&resp, "\"name\": \"Q\\\"R\\\\S\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Format negotiation: the `.json` suffix and an explicit JSON Accept both
 * answer; an HTML-only client gets the reference's UnknownFormat 406, and a
 * format coming from the Accept header sets `Vary: Accept` (render_as). */
CF_TEST(pwa_manifest_format_negotiation) {
    pwa_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db, "37signals", "2026-09-26 13:03:57.000000");

    cf_request accept_req;
    prepare_get(&accept_req, "/webmanifest");
    CF_REQUIRE(cf_test_req_header(&accept_req, SP("Accept"),
                                  SP("application/json")) == CF_OK);
    cf_response accept_resp;
    CF_REQUIRE(run_request(&env, &accept_req, &accept_resp));
    CF_CHECK(accept_resp.status == 200);
    CF_CHECK(response_head_contains(&accept_resp, &accept_req,
                                    "Vary: Accept\r\n"));
    cf_response_dispose(&accept_resp);

    cf_request html_req;
    prepare_get(&html_req, "/webmanifest");
    CF_REQUIRE(cf_test_req_header(&html_req, SP("Accept"),
                                  SP("text/html")) == CF_OK);
    cf_response html_resp;
    CF_REQUIRE(run_request(&env, &html_req, &html_resp));
    CF_CHECK(html_resp.status == 406);
    cf_response_dispose(&html_resp);
    env_close(&env);
}

/* --- service worker -------------------------------------------------------- */

/* The pinned service_worker.js bytes, served verbatim as JavaScript to an
 * anonymous client. */
static const char pwa_expected_service_worker[] =
    "self.addEventListener(\"push\", async (event) => {\n"
    "  const data = await event.data.json()\n"
    "  event.waitUntil(Promise.all([ showNotification(data), "
    "updateBadgeCount(data.options) ]))\n"
    "})\n"
    "\n"
    "async function showNotification({ title, options }) {\n"
    "  return self.registration.showNotification(title, options)\n"
    "}\n"
    "\n"
    "async function updateBadgeCount({ data: { badge } }) {\n"
    "  return self.navigator.setAppBadge?.(badge || 0)\n"
    "}\n"
    "\n"
    "self.addEventListener(\"notificationclick\", (event) => {\n"
    "  event.notification.close()\n"
    "\n"
    "  const url = new URL(event.notification.data.path, "
    "self.location.origin).href\n"
    "  event.waitUntil(openURL(url))\n"
    "})\n"
    "\n"
    "async function openURL(url) {\n"
    "  const clients = await self.clients.matchAll({ type: \"window\" })\n"
    "  const focused = clients.find((client) => client.focused)\n"
    "\n"
    "  if (focused) {\n"
    "    await focused.navigate(url)\n"
    "  } else {\n"
    "    await self.clients.openWindow(url)\n"
    "  }\n"
    "}\n";

CF_TEST(pwa_service_worker_anonymous_serves_the_verbatim_script) {
    pwa_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    prepare_get(&req, "/service-worker.js");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(response_head_contains(
        &resp, &req, "Content-Type: text/javascript; charset=utf-8\r\n"));
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    CF_CHECK(body.len == sizeof pwa_expected_service_worker - 1 &&
             memcmp(body.ptr, pwa_expected_service_worker, body.len) == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A script fetch (Accept: text/javascript, like the worker registration)
 * answers; an HTML-only client gets 406. */
CF_TEST(pwa_service_worker_format_negotiation) {
    pwa_env env;
    CF_REQUIRE(env_open(&env));

    cf_request js_req;
    prepare_get(&js_req, "/service-worker");
    CF_REQUIRE(cf_test_req_header(&js_req, SP("Accept"),
                                  SP("text/javascript")) == CF_OK);
    cf_response js_resp;
    CF_REQUIRE(run_request(&env, &js_req, &js_resp));
    CF_CHECK(js_resp.status == 200);
    CF_CHECK(response_body_contains(
        &js_resp, "self.addEventListener(\"push\""));
    cf_response_dispose(&js_resp);

    cf_request html_req;
    prepare_get(&html_req, "/service-worker");
    CF_REQUIRE(cf_test_req_header(&html_req, SP("Accept"),
                                  SP("text/html")) == CF_OK);
    cf_response html_resp;
    CF_REQUIRE(run_request(&env, &html_req, &html_resp));
    CF_CHECK(html_resp.status == 406);
    cf_response_dispose(&html_resp);
    env_close(&env);
}

CF_TEST_MAIN()
