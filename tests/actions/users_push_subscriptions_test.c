/* tests/actions/users_push_subscriptions_test.c — A-users-push_subscriptions
 * acceptance: `users/push_subscriptions#index` (route ID 66),
 * `users/push_subscriptions#create` (route ID 67) and
 * `users/push_subscriptions#destroy` (route ID 73), per
 * docs/devel/implementation/contracts/controller-packets.md
 * "A-users-push_subscriptions" and 03-application.md's remaining-packets
 * rule (permit lists, callback order, status/redirect, ownership).
 *
 * Every case runs the real A00 dispatch path through the route double
 * (tests/app/support/route_double.c), which binds rows 66/67/73 to the
 * real actions.  cf.h and src/actions/actions.h are integrator-owned and
 * do not yet declare this packet's symbols, so the entry points (plus the
 * non-static list loader) are declared here; the integrator's routes.c
 * rebind needs the same declarations (c_symbols
 * cf_action_users_push_subscriptions_index/create/destroy).
 *
 * Endpoint resolution is pinned without network: the strong
 * cf_users_push_resolve_host below replaces the action's weak seam and
 * resolves only fcm.googleapis.com (like the model tests' fake).  Push
 * delivery itself stays I02's; these cases cover the controller half
 * (scoping, find_by, touch, heads, redirects).
 *
 * Render gate: PushSubscriptionsIndex has no C view yet
 * (push_subscriptions.c R1), so index's ordinary path answers the
 * reference's 500 after loading.  The list loader is exercised directly,
 * and dispatch covers the auth/format paths.
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
#include "models/push_subscription.h"
#include "models/user.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

cf_err cf_action_users_push_subscriptions_index(cf_ctx *ctx);
cf_err cf_action_users_push_subscriptions_create(cf_ctx *ctx);
cf_err cf_action_users_push_subscriptions_destroy(cf_ctx *ctx);
cf_err cf_users_push_subscriptions_list(cf_db *db, int64_t user_id,
                                        cf_push_subscription_vector *out);

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
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define FCM_SEND "https://fcm.googleapis.com/fcm/send/x"

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- scratch app + started writer ------------------------------------------ */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
} push_env;

static bool env_open(push_env *env) {
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

/* --- acceptance ------------------------------------------------------------ */

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

CF_TEST(users_push_subscriptions_index_authenticated_gates_on_view) {
    push_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David");
    seed_subscription(env.scratch.db, 20, 1, FCM_SEND, "p256dh-key", "auth-key",
                      "2026-01-02 03:04:05");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    push_request(&req, CF_GET, "/users/me/push_subscriptions", NULL, NULL,
                 "same-origin", cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    /* R1: rows load (see the list case), but PushSubscriptionsIndex has no
     * C view yet, so the ordinary path fails loudly instead of stubbing. */
    CF_CHECK(resp.status == 500);
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

CF_TEST(users_push_subscriptions_list_scopes_to_the_user) {
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

    cf_push_subscription_vector out = {0};
    CF_REQUIRE(cf_users_push_subscriptions_list(env.scratch.db, 1, &out) ==
               CF_OK);
    CF_REQUIRE(out.len == 2);
    CF_CHECK(out.items[0].id == 20 && out.items[0].user_id == 1);
    CF_CHECK(out.items[1].id == 21 && out.items[1].user_id == 1);
    cf_push_subscription_vector_dispose(&out);

    memset(&out, 0, sizeof out);
    CF_REQUIRE(cf_users_push_subscriptions_list(env.scratch.db, 2, &out) ==
               CF_OK);
    CF_REQUIRE(out.len == 1);
    CF_CHECK(out.items[0].id == 22);
    cf_push_subscription_vector_dispose(&out);
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

CF_TEST_MAIN()
