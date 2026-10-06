/* tests/actions/users_push_subscriptions_test.c — A-users-push_subscriptions
 * acceptance: `users/push_subscriptions#index` (route ID 66),
 * `users/push_subscriptions#create` (route ID 67) and `#destroy` (route ID
 * 73), per docs/devel/implementation/contracts/controller-packets.md
 * "A-users-push_subscriptions" and 03-application.md's remaining-packets
 * rule (permit lists, callback order, status/redirect, ownership).
 *
 * Every case runs the real A00 dispatch path through the route double
 * (tests/app/support/route_double.c), which binds rows 66/67/73 to the
 * real actions.  cf.h and src/actions/actions.h are integrator-owned and
 * do not yet declare this packet's symbols, so the entry points are
 * declared here; the integrator's routes.c rebind needs the same
 * declarations (c_symbols cf_action_users_push_subscriptions_index/create/
 * destroy).
 *
 * Endpoint resolution is pinned without network: the strong
 * cf_users_push_resolve_host below replaces the action's weak seam and
 * resolves only fcm.googleapis.com (like the model tests' fake).  Push
 * delivery itself stays I02's; these cases cover the controller half
 * (scoping, find_by, touch, heads, redirects).
 *
 * V02 render: the index gate is gone.  The page renders through the
 * cf_presenter_users_push_index model and is compared token-for-token with
 * the golden/a push_subscriptions fixture (the parity SECRET_KEY_BASE and
 * the fixture rows are seeded here), plus a Turbo-Frame structural case and
 * a direct presenter case (scoping, UA mapping, last_room_visited).
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "core/testclock.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "http/http_internal.h"
#include "models/push_subscription.h"
#include "models/room.h"
#include "models/user.h"
#include "presenters/push_subscriptions.h"
#include "views.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"
#include "../views/support/golden.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "yyjson.h"

cf_err cf_action_users_push_subscriptions_index(cf_ctx *ctx);
cf_err cf_action_users_push_subscriptions_create(cf_ctx *ctx);
cf_err cf_action_users_push_subscriptions_destroy(cf_ctx *ctx);

/* Strong resolution pin for the action's weak seam: only the permitted
 * fcm host resolves, to a public IP (model-test convention). */
static int resolve_calls = 0;

cf_optional_str cf_users_push_resolve_host(void *arg, cf_str host) {
    (void)arg;
    cf_optional_str out = {false, {NULL, 0}};
    resolve_calls++;
    static const char fcm[] = "fcm.googleapis.com";
    static const char ip[] = "142.250.185.206";
    if (host.len != sizeof fcm - 1 ||
        memcmp(host.ptr, fcm, sizeof fcm - 1) != 0) {
        return out;
    }
    char *copy = malloc(sizeof ip);
    if (copy == NULL) return out;
    memcpy(copy, ip, sizeof ip);
    out.present = true;
    out.value.ptr = copy;
    out.value.len = sizeof ip - 1;
    return out;
}

#define ORIGIN "http://campfire.test"
/* parity/.env.reference (reference-tools/views/a/golden.sh); the golden
 * fixtures' signed tokens only reproduce with it. */
#define GOLDEN_SECRET                                                        \
    "5335c3b1ad35b4ad170c3413bd651ef3b6ed64e257261871a6de3f978cf3868ee"     \
    "417a927040935fb30b0f7debdedb34a2a403e9f34b16cf594c917c2ecd4a995"
#define GOLDEN_VAPID_PUBLIC_KEY                                              \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8" \
    "cTriz_qYBVicY02_VxTQ="
#define FCM_SEND "https://fcm.googleapis.com/fcm/send/x"
/* The capture's chrome_mac request UA (render.rb). */
#define CHROME_MAC                                                            \
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "     \
    "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36"
/* The fixture push_subscriptions row's stored agent (facts.json: Chrome
 * 113.0.0.0 on Macintosh). */
#define FIXTURE_PUSH_UA                                                       \
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "     \
    "(KHTML, like Gecko) Chrome/113.0.0.0 Safari/537.36"

/* tests/fixtures/crates/views/tests/golden/a/facts.json (fixture rows). */
#define ACCOUNT_ID 873240054
#define DAVID 127326141
#define DESIGNERS 654632876
#define FIXTURE_SUBSCRIPTION 56887440

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- fixture-faithful asset root (users_sidebars_test.c precedent) --------- */

static char g_assets_root[4096];

static const char *golden_dir(void) {
    const char *dir = getenv("CF_GOLDEN_DIR");
    return dir != NULL && dir[0] != '\0'
               ? dir
               : "tests/fixtures/crates/views/tests/golden";
}

static bool copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (in == NULL) return false;
    FILE *out = fopen(to, "wb");
    if (out == NULL) {
        fclose(in);
        return false;
    }
    char buffer[8192];
    size_t n;
    bool ok = true;
    while ((n = fread(buffer, 1, sizeof buffer, in)) != 0) {
        if (fwrite(buffer, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    return ok;
}

static bool stage_assets_root(void) {
    char tmpl[] = "/tmp/campfire-push-assets-XXXXXX";
    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "  assets: mkdtemp failed\n");
        return false;
    }
    snprintf(g_assets_root, sizeof g_assets_root, "%s", tmpl);

    char public_dir[4200], assets_dir[4300];
    snprintf(public_dir, sizeof public_dir, "%s/public", g_assets_root);
    snprintf(assets_dir, sizeof assets_dir, "%s/assets", public_dir);
    if (mkdir(public_dir, 0700) != 0 || mkdir(assets_dir, 0700) != 0) {
        fprintf(stderr, "  assets: mkdir failed: %s\n", strerror(errno));
        return false;
    }
    char manifest_from[4200], manifest_to[4600];
    snprintf(manifest_from, sizeof manifest_from,
             "tests/fixtures/assets/public/assets/.manifest.json");
    snprintf(manifest_to, sizeof manifest_to, "%s/.manifest.json", assets_dir);
    if (!copy_file(manifest_from, manifest_to)) {
        fprintf(stderr, "  assets: manifest copy failed\n");
        return false;
    }

    char facts_path[4600];
    snprintf(facts_path, sizeof facts_path, "%s/a/facts.json", golden_dir());
    yyjson_doc *facts = yyjson_read_file(facts_path, 0, NULL, NULL);
    if (facts == NULL) {
        fprintf(stderr, "  assets: cannot read %s\n", facts_path);
        return false;
    }
    const char *importmap =
        yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(facts),
                                      "importmap_tags"));
    bool ok = importmap != NULL;
    if (ok) {
        char importmap_to[4600];
        snprintf(importmap_to, sizeof importmap_to, "%s/importmap-tags.html",
                 g_assets_root);
        FILE *file = fopen(importmap_to, "wb");
        ok = file != NULL &&
             fwrite(importmap, 1, strlen(importmap), file) == strlen(importmap);
        if (file != NULL && fclose(file) != 0) ok = false;
    }
    yyjson_doc_free(facts);
    if (!ok) fprintf(stderr, "  assets: importmap staging failed\n");
    return ok;
}

/* --- scratch app + started writer ------------------------------------------ */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
} push_env;

static bool env_open(push_env *env) {
    if (g_assets_root[0] == '\0' && !stage_assets_root()) return false;
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;
    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", ORIGIN},
        {"SECRET_KEY_BASE", GOLDEN_SECRET},
        {"VAPID_PUBLIC_KEY", GOLDEN_VAPID_PUBLIC_KEY},
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
    /* The staged root renders the layout with the fixtures' importmap. */
    if (cf_views_assets_configure(g_assets_root) != CF_OK) {
        fprintf(stderr, "  env_open: assets configure failed\n");
        return false;
    }
    if (cf_writer_start(env->app, env->config) != CF_OK) return false;
    env->writer_started = true;
    cf_test_routes_reset();
    resolve_calls = 0;
    if (cf_test_routes_add("GET", "/users/:user_id/push_subscriptions(.:format)",
                           66, cf_action_users_push_subscriptions_index) !=
            CF_OK ||
        cf_test_routes_add("GET", "/users/:user_id/push_subscriptions", 66,
                           cf_action_users_push_subscriptions_index) != CF_OK ||
        cf_test_routes_add("POST",
                           "/users/:user_id/push_subscriptions(.:format)", 67,
                           cf_action_users_push_subscriptions_create) !=
            CF_OK ||
        cf_test_routes_add("POST", "/users/:user_id/push_subscriptions", 67,
                           cf_action_users_push_subscriptions_create) !=
            CF_OK ||
        cf_test_routes_add("DELETE",
                           "/users/:user_id/push_subscriptions/:id(.:format)",
                           73,
                           cf_action_users_push_subscriptions_destroy) !=
            CF_OK ||
        cf_test_routes_add("DELETE",
                           "/users/:user_id/push_subscriptions/:id", 73,
                           cf_action_users_push_subscriptions_destroy) !=
            CF_OK) {
        return false;
    }
    return true;
}

static void env_close(push_env *env) {
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

static void seed_user(cf_db *db, int64_t id, const char *name) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, updated_at) "
             "VALUES (%lld, NULL, NULL, '2026-01-02 03:04:05', NULL, '%s', "
             "NULL, 0, 0, '2026-01-02 03:04:05')",
             (long long)id, name);
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

static void seed_subscription(cf_db *db, int64_t id, int64_t user_id,
                              const char *endpoint, const char *p256dh,
                              const char *auth, const char *updated_at) {
    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO push_subscriptions (id, auth_key, created_at, "
             "endpoint, p256dh_key, updated_at, user_agent, user_id) "
             "VALUES (%lld, %s, '2026-01-02 03:04:05', %s, %s, '%s', NULL, "
             "%lld)",
             (long long)id, auth != NULL ? "?" : "NULL",
             endpoint != NULL ? "?" : "NULL", p256dh != NULL ? "?" : "NULL",
             updated_at, (long long)user_id);
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) == SQLITE_OK);
    int bind = 1;
    /* Binds follow the placeholder order: auth, endpoint, p256dh. */
    if (auth != NULL) sqlite3_bind_text(stmt, bind++, auth, -1, SQLITE_TRANSIENT);
    if (endpoint != NULL) {
        sqlite3_bind_text(stmt, bind++, endpoint, -1, SQLITE_TRANSIENT);
    }
    if (p256dh != NULL) {
        sqlite3_bind_text(stmt, bind++, p256dh, -1, SQLITE_TRANSIENT);
    }
    int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (step != SQLITE_DONE) {
        fprintf(stderr, "  seed_subscription failed (%d): %s\n", step, sql);
    }
    CF_REQUIRE(step == SQLITE_DONE);
}

/* --- request/response helpers ---------------------------------------------- */

static bool run_request(push_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void push_request(cf_request *req, cf_method method, const char *target,
                         const char *body, const char *content_type,
                         const char *sec_fetch_site, const char *cookie_header,
                         const char *user_agent) {
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
    if (user_agent != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("User-Agent"),
                                      SP(user_agent)) == CF_OK);
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

static bool span_contains(cf_span span, const char *needle) {
    size_t len = strlen(needle);
    if (len == 0) return true;
    if (span.len < len) return false;
    for (size_t i = 0; i + len <= span.len; i++) {
        if (memcmp(span.ptr + i, needle, len) == 0) return true;
    }
    return false;
}

/* --- golden fixture rows (facts.json) -------------------------------------- */

/* The push golden's page (David) and last-room target: the account, David,
 * Designers as his earliest room, its membership, and the fixture
 * subscription (id 56887440, endpoint .../123, the stored Chrome-113 UA). */
static void seed_fixture_push(cf_db *db) {
    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
             "name, settings, singleton_guard, updated_at) VALUES "
             "(%d, '2026-09-26 13:00:20.000000', NULL, 'CRMu-l8Ge-KB9B', "
             "'37signals', NULL, 0, '2026-09-26 13:00:20.000000')",
             ACCOUNT_ID);
    exec_sql(db, sql);
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, created_at, email_address, name, "
             "role, status, updated_at) VALUES (%lld, NULL, "
             "'2026-09-26 13:00:10.000000', 'david@37signals.com', 'David', "
             "1, 0, '2026-09-26 13:00:20.000000')",
             (long long)DAVID);
    exec_sql(db, sql);
    snprintf(sql, sizeof sql,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (%lld, '2026-09-26 11:00:00.000000', %lld, "
             "'Designers', 'Rooms::Closed', '2026-09-26 13:00:20.300000')",
             (long long)DESIGNERS, (long long)DAVID);
    exec_sql(db, sql);
    snprintf(sql, sizeof sql,
             "INSERT INTO memberships (id, created_at, involvement, room_id, "
             "unread_at, updated_at, user_id) VALUES (900010, '2026-09-26 "
             "12:00:00.000000', 'mentions', %lld, NULL, "
             "'2026-09-26 12:00:00.000000', %lld)",
             (long long)DESIGNERS, (long long)DAVID);
    exec_sql(db, sql);
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_stmt *stmt = NULL;
    snprintf(sql, sizeof sql,
             "INSERT INTO push_subscriptions (id, auth_key, created_at, "
             "endpoint, p256dh_key, updated_at, user_agent, user_id) VALUES "
             "(%lld, 'auth-key', '2026-09-26 13:00:20.000000', "
             "'https://fcm.googleapis.com/fcm/send/123', 'p256dh-key', "
             "'2026-09-26 13:00:20.000000', ?, %lld)",
             (long long)FIXTURE_SUBSCRIPTION, (long long)DAVID);
    CF_REQUIRE(sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, FIXTURE_PUSH_UA, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    CF_REQUIRE(step == SQLITE_DONE);
}

/* --- acceptance ------------------------------------------------------------ */

#include "auth_write_race.h"

CF_TEST(users_push_subscriptions_route_ids_bind_the_actions) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/users/:user_id/push_subscriptions",
                                  66,
                                  cf_action_users_push_subscriptions_index) ==
               CF_OK);
    CF_REQUIRE(cf_test_routes_add("POST", "/users/:user_id/push_subscriptions",
                                  67,
                                  cf_action_users_push_subscriptions_create) ==
               CF_OK);
    CF_REQUIRE(cf_test_routes_add(
                   "DELETE", "/users/:user_id/push_subscriptions/:id", 73,
                   cf_action_users_push_subscriptions_destroy) == CF_OK);
    CF_CHECK(cf_route_action(66) == cf_action_users_push_subscriptions_index);
    CF_CHECK(cf_route_action(67) == cf_action_users_push_subscriptions_create);
    CF_CHECK(cf_route_action(73) ==
             cf_action_users_push_subscriptions_destroy);
}

/* The V02 render: the index page matches the golden/a fixture token-for-token
 * (fixture rows seeded; parity SECRET_KEY_BASE; the fixture's stored UA and
 * David's original room drive the mapped strings and the back link). */
CF_TEST(users_push_subscriptions_index_matches_the_golden) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_fixture_push(env.scratch.db);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "fixture-david", DAVID);

    cf_request req;
    cf_response resp;
    push_request(&req, CF_GET, "/users/me/push_subscriptions", NULL, NULL,
                 "same-origin", cookie, CHROME_MAC);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_REQUIRE(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    /* The page layout carries the stylesheet preload links. */
    CF_CHECK(head_contains(&resp, &req, "Link: <"));
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("push_subscriptions", (const char *)body.ptr, body.len);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A Turbo-Frame request renders turbo-rails' frame layout (head + content, no
 * title/nav) and carries no Link preload header. */
CF_TEST(users_push_subscriptions_index_turbo_frame_renders_the_frame) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_fixture_push(env.scratch.db);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "fixture-david-frame", DAVID);

    cf_request req;
    cf_response resp;
    push_request(&req, CF_GET, "/users/me/push_subscriptions", NULL, NULL,
                 "same-origin", cookie, CHROME_MAC);
    CF_REQUIRE(cf_test_req_header(&req, SP("Turbo-Frame"),
                                  SP("push_subscriptions")) == CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_REQUIRE(resp.status == 200);
    CF_CHECK(!head_contains(&resp, &req, "Link: <"));
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    CF_CHECK(span_contains(body, "id=\"push_subscriptions\""));
    CF_CHECK(
        !span_contains(body, "Push notification subscriptions</title>"));
    CF_CHECK(!span_contains(body, "<nav id=\"nav\">"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_push_subscriptions_index_unauthenticated_redirects) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David");

    cf_request req;
    cf_response resp;
    push_request(&req, CF_GET, "/users/me/push_subscriptions", NULL, NULL,
                 "same-origin", NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_push_subscriptions_index_unacceptable_format_is_406) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    push_request(&req, CF_GET, "/users/me/push_subscriptions.json", NULL,
                 NULL, "same-origin", cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_push_subscriptions_presenter_scopes_and_maps) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David");
    seed_user(env.scratch.db, 2, "Kevin");
    seed_subscription(env.scratch.db, 20, 1, FCM_SEND, "p1", "a1",
                      "2026-01-02 03:04:05");
    seed_subscription(env.scratch.db, 21, 1, FCM_SEND, "p2", NULL,
                      "2026-01-02 03:04:05");
    seed_subscription(env.scratch.db, 22, 2, FCM_SEND, "p3", "a3",
                      "2026-01-02 03:04:05");
    /* The stored UA maps to browser/version/platform (row 20). */
    {
        sqlite3 *handle = cf_db_handle(env.scratch.db);
        sqlite3_stmt *stmt = NULL;
        CF_REQUIRE(sqlite3_prepare_v2(
                       handle,
                       "UPDATE push_subscriptions SET user_agent = ? WHERE "
                       "id = 20",
                       -1, &stmt, NULL) == SQLITE_OK);
        sqlite3_bind_text(stmt, 1, FIXTURE_PUSH_UA, -1, SQLITE_TRANSIENT);
        CF_REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }
    /* user 1's original room (earliest membership): room 30. */
    exec_sql(env.scratch.db,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (30, '2026-01-01 00:00:00', 1, 'Original', "
             "'Rooms::Open', '2026-01-01 00:00:00')");
    exec_sql(env.scratch.db,
             "INSERT INTO memberships (id, created_at, involvement, room_id, "
             "unread_at, updated_at, user_id) VALUES (300, '2026-01-01 "
             "00:00:00', 'everything', 30, NULL, '2026-01-01 00:00:00', 1)");
    exec_sql(env.scratch.db,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (31, '2026-01-02 00:00:00', 1, 'Visited', "
             "'Rooms::Open', '2026-01-02 00:00:00')");
    exec_sql(env.scratch.db,
             "INSERT INTO memberships (id, created_at, involvement, room_id, "
             "unread_at, updated_at, user_id) VALUES (301, '2026-01-02 "
             "00:00:00', 'everything', 31, NULL, '2026-01-02 00:00:00', 1)");

    cf_request req;
    cf_response resp;
    cf_response_init(&resp);
    push_request(&req, CF_GET, "/users/me/push_subscriptions", NULL, NULL,
                 NULL, NULL, NULL);
    cf_ctx ctx;
    CF_REQUIRE(cf_ctx_create(&ctx, env.app, env.scratch.db, &req, &resp) ==
               CF_OK);
    cf_user user = {0};
    bool found = false;
    CF_REQUIRE(cf_user_find_by_id(env.scratch.db, 1, &found, &user) == CF_OK);
    CF_REQUIRE(found);
    cf_view_users_push_index_model model = {0};
    CF_REQUIRE(cf_presenter_users_push_index(&ctx, &user, &model) == CF_OK);

    /* Exactly the caller's rows, in model order. */
    CF_REQUIRE(model.subscriptions.len == 2);
    CF_CHECK(model.subscriptions.items[0].id == 20);
    CF_CHECK(model.subscriptions.items[1].id == 21);
    const cf_view_push_subscription *row = &model.subscriptions.items[0];
    CF_CHECK(span_contains((cf_span){(const unsigned char *)row->endpoint.ptr,
                                     row->endpoint.len},
                           FCM_SEND));
    CF_CHECK(span_contains((cf_span){(const unsigned char *)row->browser.ptr,
                                     row->browser.len},
                           "Chrome"));
    CF_CHECK(span_contains((cf_span){(const unsigned char *)row->version.ptr,
                                     row->version.len},
                           "113.0.0.0"));
    CF_CHECK(span_contains((cf_span){(const unsigned char *)row->platform.ptr,
                                     row->platform.len},
                           "Macintosh"));
    /* No cookie: the original room. */
    CF_CHECK(model.has_last_room && model.last_room_id == 30);
    cf_view_users_push_index_model_dispose(&model);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);

    /* The last_room cookie wins when the user is a member of that room. */
    cf_request req2;
    cf_response resp2;
    cf_response_init(&resp2);
    push_request(&req2, CF_GET, "/users/me/push_subscriptions", NULL, NULL,
                 NULL, NULL, NULL);
    CF_REQUIRE(cf_test_req_header(&req2, SP("Cookie"), SP("last_room=31")) ==
               CF_OK);
    CF_REQUIRE(cf_ctx_create(&ctx, env.app, env.scratch.db, &req2, &resp2) ==
               CF_OK);
    memset(&model, 0, sizeof model);
    CF_REQUIRE(cf_presenter_users_push_index(&ctx, &user, &model) == CF_OK);
    CF_CHECK(model.has_last_room && model.last_room_id == 31);
    cf_view_users_push_index_model_dispose(&model);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp2);

    /* An unknown room in the cookie falls back to the original. */
    cf_request req3;
    cf_response resp3;
    cf_response_init(&resp3);
    push_request(&req3, CF_GET, "/users/me/push_subscriptions", NULL, NULL,
                 NULL, NULL, NULL);
    CF_REQUIRE(cf_test_req_header(&req3, SP("Cookie"), SP("last_room=999")) ==
               CF_OK);
    CF_REQUIRE(cf_ctx_create(&ctx, env.app, env.scratch.db, &req3, &resp3) ==
               CF_OK);
    memset(&model, 0, sizeof model);
    CF_REQUIRE(cf_presenter_users_push_index(&ctx, &user, &model) == CF_OK);
    CF_CHECK(model.has_last_room && model.last_room_id == 30);
    cf_view_users_push_index_model_dispose(&model);
    cf_user_dispose(&user);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp3);
    env_close(&env);
}

CF_TEST(users_push_subscriptions_create_inserts_and_heads_ok) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    push_request(&req, CF_POST, "/users/me/push_subscriptions",
                 "push_subscription[endpoint]=" FCM_SEND
                 "&push_subscription[p256dh_key]=p256dh-key"
                 "&push_subscription[auth_key]=auth-key",
                 "application/x-www-form-urlencoded", "same-origin", cookie,
                 "Mozilla/5.0 (Test)");
    resolve_calls = 0;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req, "Content-Type: text/html\r\n"));
    CF_CHECK(resp.body == NULL);
    /* One resolution: create validates once with the live resolver. */
    CF_CHECK(resolve_calls == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions") == 1);
    char endpoint[256];
    read_text(env.scratch.db,
              "SELECT \"endpoint\" FROM \"push_subscriptions\"", endpoint,
              sizeof endpoint);
    CF_CHECK(strcmp(endpoint, FCM_SEND) == 0);
    char agent[64];
    read_text(env.scratch.db,
              "SELECT \"user_agent\" FROM \"push_subscriptions\"", agent,
              sizeof agent);
    CF_CHECK(strcmp(agent, "Mozilla/5.0 (Test)") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions WHERE "
                        "\"user_id\" = 1") == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_push_subscriptions_create_existing_valid_touches) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David");
    seed_subscription(env.scratch.db, 20, 1, FCM_SEND, "p256dh-key",
                      "auth-key", "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    push_request(&req, CF_POST, "/users/me/push_subscriptions",
                 "push_subscription[endpoint]=" FCM_SEND
                 "&push_subscription[p256dh_key]=p256dh-key"
                 "&push_subscription[auth_key]=auth-key",
                 "application/x-www-form-urlencoded", "same-origin", cookie,
                 NULL);
    resolve_calls = 0;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(resp.body == NULL);
    /* Existing endpoints revalidate (one resolution), then touch. */
    CF_CHECK(resolve_calls == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions WHERE "
                        "\"id\" = 20 AND \"updated_at\" > "
                        "'2020-01-01 00:00:00'") == 1);
    /* Absent user agent stores NULL (request carried none). */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions WHERE "
                        "\"id\" = 20 AND \"user_agent\" IS NULL") == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_push_subscriptions_create_existing_invalid_is_422) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David");
    /* Same keys as the request, but the stored endpoint no longer
     * validates (wrong port): no resolution is even attempted. */
    seed_subscription(env.scratch.db, 20, 1,
                      "https://fcm.googleapis.com:8443/fcm/send/x",
                      "p256dh-key", "auth-key", "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    push_request(&req, CF_POST, "/users/me/push_subscriptions",
                 "push_subscription[endpoint]="
                 "https://fcm.googleapis.com:8443/fcm/send/x"
                 "&push_subscription[p256dh_key]=p256dh-key"
                 "&push_subscription[auth_key]=auth-key",
                 "application/x-www-form-urlencoded", "same-origin", cookie,
                 NULL);
    resolve_calls = 0;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    CF_CHECK(head_contains(&resp, &req, "Content-Type: text/html\r\n"));
    CF_CHECK(resp.body == NULL);
    CF_CHECK(resolve_calls == 0);
    /* No touch on the invalid path. */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions WHERE "
                        "\"id\" = 20 AND \"updated_at\" = "
                        "'2020-01-01 00:00:00'") == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_push_subscriptions_create_new_invalid_is_422) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    /* A non-permitted host never validates (and never resolves). */
    cf_request req;
    cf_response resp;
    push_request(&req, CF_POST, "/users/me/push_subscriptions",
                 "push_subscription[endpoint]="
                 "https://attacker.example.com/collect",
                 "application/x-www-form-urlencoded", "same-origin", cookie,
                 NULL);
    resolve_calls = 0;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    CF_CHECK(resp.body == NULL);
    CF_CHECK(resolve_calls == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_push_subscriptions_create_empty_conditions_find_first) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David");
    seed_subscription(env.scratch.db, 20, 1, FCM_SEND, "p1", "a1",
                      "2020-01-01 00:00:00");
    seed_subscription(env.scratch.db, 21, 1, FCM_SEND, "p2", "a2",
                      "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    /* `other` is not permitted, so no key is a condition: the user's first
     * subscription matches and is touched. */
    cf_request req;
    cf_response resp;
    push_request(&req, CF_POST, "/users/me/push_subscriptions",
                 "push_subscription[other]=1",
                 "application/x-www-form-urlencoded", "same-origin", cookie,
                 NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions") == 2);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions WHERE "
                        "\"id\" = 20 AND \"updated_at\" > "
                        "'2020-01-01 00:00:00'") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions WHERE "
                        "\"id\" = 21 AND \"updated_at\" = "
                        "'2020-01-01 00:00:00'") == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_push_subscriptions_create_json_bodies_wrap) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    /* Flat JSON keys wrap under push_subscription (wrap_parameters). */
    cf_request req;
    cf_response resp;
    push_request(&req, CF_POST, "/users/me/push_subscriptions",
                 "{\"endpoint\":\"" FCM_SEND
                 "\",\"p256dh_key\":\"p256dh-key\",\"auth_key\":\"auth-key\"}",
                 "application/json", "same-origin", cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions") == 1);
    cf_response_dispose(&resp);

    /* Nested JSON needs no wrap. */
    cf_request req2;
    cf_response resp2;
    push_request(&req2, CF_POST, "/users/me/push_subscriptions",
                 "{\"push_subscription\":{\"endpoint\":\"" FCM_SEND
                 "\",\"p256dh_key\":\"other\",\"auth_key\":\"auth-key\"}}",
                 "application/json", "same-origin", cookie, NULL);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 200);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions") == 2);
    cf_response_dispose(&resp2);
    env_close(&env);
}

CF_TEST(users_push_subscriptions_create_json_false_finds_first) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David");
    seed_subscription(env.scratch.db, 20, 1, FCM_SEND, "p256dh-key",
                      "auth-key", "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    /* `require` accepts the false literal (wrapped non-empty); with no
     * permitted keys the first subscription matches and is touched. */
    cf_request req;
    cf_response resp;
    push_request(&req, CF_POST, "/users/me/push_subscriptions", "false",
                 "application/json", "same-origin", cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions WHERE "
                        "\"id\" = 20 AND \"updated_at\" > "
                        "'2020-01-01 00:00:00'") == 1);
    cf_response_dispose(&resp);

    /* The nested false literal takes the non-wrapped path to the same
     * empty-conditions match. */
    cf_request req2;
    cf_response resp2;
    push_request(&req2, CF_POST, "/users/me/push_subscriptions",
                 "{\"push_subscription\":false}", "application/json",
                 "same-origin", cookie, NULL);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 200);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions") == 1);
    cf_response_dispose(&resp2);
    env_close(&env);
}

CF_TEST(users_push_subscriptions_create_missing_param_is_400) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    push_request(&req, CF_POST, "/users/me/push_subscriptions", "other=1",
                 "application/x-www-form-urlencoded", "same-origin", cookie,
                 NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 400);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions") == 0);
    cf_response_dispose(&resp);

    /* An empty JSON body cannot wrap anything either (require fails). */
    cf_request req2;
    cf_response resp2;
    push_request(&req2, CF_POST, "/users/me/push_subscriptions", "",
                 "application/json", "same-origin", cookie, NULL);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 400);
    cf_response_dispose(&resp2);
    env_close(&env);
}

CF_TEST(users_push_subscriptions_create_unauthenticated_redirects) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David");

    cf_request req;
    cf_response resp;
    push_request(&req, CF_POST, "/users/me/push_subscriptions",
                 "push_subscription[endpoint]=" FCM_SEND,
                 "application/x-www-form-urlencoded", "same-origin", NULL,
                 NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_push_subscriptions_create_cross_site_post_is_422) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    push_request(&req, CF_POST, "/users/me/push_subscriptions",
                 "push_subscription[endpoint]=" FCM_SEND,
                 "application/x-www-form-urlencoded", "cross-site", cookie,
                 NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_push_subscriptions_create_head_uses_rendered_format) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    push_request(&req, CF_POST, "/users/me/push_subscriptions.json",
                 "push_subscription[endpoint]=" FCM_SEND,
                 "application/x-www-form-urlencoded", "same-origin", cookie,
                 NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: application/json\r\n"));
    CF_CHECK(resp.body == NULL);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_push_subscriptions_destroy_removes_own_and_redirects) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David");
    seed_subscription(env.scratch.db, 20, 1, FCM_SEND, "p1", "a1",
                      "2026-01-02 03:04:05");
    seed_subscription(env.scratch.db, 21, 1, FCM_SEND, "p2", "a2",
                      "2026-01-02 03:04:05");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    push_request(&req, CF_DELETE, "/users/me/push_subscriptions/20", NULL,
                 NULL, "same-origin", cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN
                           "/users/me/push_subscriptions\r\n"));
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions WHERE "
                        "\"id\" = 20") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions WHERE "
                        "\"id\" = 21") == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_push_subscriptions_destroy_never_touches_another_scope) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David");
    seed_user(env.scratch.db, 2, "Kevin");
    seed_subscription(env.scratch.db, 20, 2, FCM_SEND, "p1", "a1",
                      "2026-01-02 03:04:05");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    /* Another user's id: no-op, still the index redirect (AUTH-06). */
    cf_request req;
    cf_response resp;
    push_request(&req, CF_DELETE, "/users/me/push_subscriptions/20", NULL,
                 NULL, "same-origin", cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN
                           "/users/me/push_subscriptions\r\n"));
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions WHERE "
                        "\"id\" = 20") == 1);
    cf_response_dispose(&resp);

    /* A missing id and an uncastable id are the same no-op redirect. */
    cf_request req2;
    cf_response resp2;
    push_request(&req2, CF_DELETE, "/users/me/push_subscriptions/999", NULL,
                 NULL, "same-origin", cookie, NULL);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 302);
    cf_response_dispose(&resp2);

    cf_request req3;
    cf_response resp3;
    push_request(&req3, CF_DELETE, "/users/me/push_subscriptions/abc", NULL,
                 NULL, "same-origin", cookie, NULL);
    CF_REQUIRE(run_request(&env, &req3, &resp3));
    CF_CHECK(resp3.status == 302);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions") == 1);
    cf_response_dispose(&resp3);

    /* Leading-integer semantics (ruby_compat::integer_cast): "21x" casts
     * to 21, so an owned row still destroys. */
    seed_subscription(env.scratch.db, 21, 1, FCM_SEND, "p2", "a2",
                      "2026-01-02 03:04:05");
    cf_request req4;
    cf_response resp4;
    push_request(&req4, CF_DELETE, "/users/me/push_subscriptions/21x", NULL,
                 NULL, "same-origin", cookie, NULL);
    CF_REQUIRE(run_request(&env, &req4, &resp4));
    CF_CHECK(resp4.status == 302);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions WHERE "
                        "\"id\" = 21") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions WHERE "
                        "\"id\" = 20") == 1);
    cf_response_dispose(&resp4);
    env_close(&env);
}

CF_TEST(users_push_subscriptions_destroy_unauthenticated_redirects) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David");
    seed_subscription(env.scratch.db, 20, 1, FCM_SEND, "p1", "a1",
                      "2026-01-02 03:04:05");

    cf_request req;
    cf_response resp;
    push_request(&req, CF_DELETE, "/users/me/push_subscriptions/20", NULL,
                 NULL, "same-origin", NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions") == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_push_subscriptions_rejects_queued_ban) {
    for (int status = 1; status <= 2; status++) {
        for (int arm = 0; arm < 4; arm++) {
            push_env env;
            CF_REQUIRE(env_open(&env));
            seed_user(env.scratch.db, 1, "David");
            seed_user(env.scratch.db, 2, "Other");
            if (arm > 0)
                seed_subscription(env.scratch.db, 20, 1, FCM_SEND, "p1", "a1",
                                  "2020-01-01 00:00:00");
            char cookie[4096];
            make_session_cookie(env.config, env.scratch.db, cookie,
                                sizeof cookie, "david-session", 1);
            cf_request req;
            cf_response resp;
            push_request(&req, arm == 2 ? CF_DELETE : CF_POST,
                         arm == 2 ? "/users/me/push_subscriptions/20"
                                  : "/users/me/push_subscriptions",
                         arm == 2 ? NULL
                                  : "push_subscription[endpoint]=" FCM_SEND
                                    "&push_subscription[p256dh_key]=p1&push_"
                                    "subscription[auth_key]=a1",
                         "application/x-www-form-urlencoded", "same-origin",
                         cookie, NULL);
            auth_write_race race;
            CF_REQUIRE(auth_race_begin(
                env.app, env.scratch.path, &race,
                arm == 3
                    ? "UPDATE push_subscriptions SET user_id=2 WHERE id=20"
                    : status == 1 ? "UPDATE users SET status=1 WHERE id=1"
                                  : "UPDATE users SET status=2 WHERE id=1"));
            CF_REQUIRE(
                auth_race_request(env.app, env.scratch.db, &race, &req, &resp));
            auth_race_end(&race);
            CF_CHECK(resp.status == 403);
            CF_CHECK(count_rows(env.scratch.db,
                                "SELECT count(*) FROM push_subscriptions") ==
                     (arm == 0 ? 0 : 1));
            if (arm > 0)
                CF_CHECK(
                    count_rows(env.scratch.db,
                               "SELECT count(*) FROM push_subscriptions WHERE "
                               "updated_at='2020-01-01 00:00:00'") == 1);
            cf_response_dispose(&resp);
            env_close(&env);
        }
    }
}
CF_TEST_MAIN()
