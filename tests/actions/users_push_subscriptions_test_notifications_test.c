/* tests/actions/users_push_subscriptions_test_notifications_test.c —
 * A-users-push_subscriptions-test_notifications acceptance:
 * `users/push_subscriptions/test_notifications#create` (route ID 65), per
 * docs/devel/implementation/contracts/controller-packets.md
 * "A-users-push_subscriptions-test_notifications" and 03-application.md's
 * remaining-packets rule (callback order, status/redirect, ownership).
 *
 * Every case runs the real A00 dispatch path through the route double
 * (tests/app/support/route_double.c), which binds row 65 to the real
 * action.  cf.h and src/actions/actions.h are integrator-owned and do not
 * yet declare this packet's symbol, so the entry point is declared here;
 * the integrator's routes.c rebind needs the same declaration (c_symbol
 * cf_action_users_push_subscriptions_test_notifications_create).
 *
 * Delivery is pinned without network: the strong resolver replaces the
 * action's weak DNS seam (fcm.googleapis.com only), and the strong
 * exchange replaces I01's not-yet-wired helper with a canned push-service
 * status.  The cases assert the controller half: ownership gating,
 * VAPID-off loud failure, inline error propagation (any non-OK delivery
 * is the reference's 500, with the subscription preserved — the inline
 * path never destroys), and the redirect on success.
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
#include "integrations/push.h"
#include "models/membership.h"
#include "models/push_subscription.h"
#include "models/user.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cf_err cf_action_users_push_subscriptions_test_notifications_create(
    cf_ctx *ctx);

/* Strong pins for the action's weak seams. */
static int resolve_calls = 0;

cf_optional_str cf_users_push_test_resolve_host(void *arg, cf_str host) {
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

static int exchange_calls = 0;
static unsigned exchange_status = 201;
static char exchange_endpoint[512];

cf_err cf_users_push_test_exchange(void *ctx,
                                   const cf_push_request *request,
                                   unsigned *out_status, char *reason_buf,
                                   size_t reason_cap,
                                   cf_push_transport_error *transport_err) {
    (void)ctx;
    exchange_calls++;
    if (request != NULL && request->endpoint_url != NULL) {
        snprintf(exchange_endpoint, sizeof exchange_endpoint, "%s",
                 request->endpoint_url);
    } else {
        exchange_endpoint[0] = '\0';
    }
    if (out_status != NULL) *out_status = exchange_status;
    if (reason_buf != NULL && reason_cap != 0) reason_buf[0] = '\0';
    if (transport_err != NULL) *transport_err = CF_PUSH_TRANSPORT_OK;
    return CF_OK;
}

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define FCM_SEND "https://fcm.googleapis.com/fcm/send/x"
/* RFC 8291 key pair (I02 vectors): encryptable at delivery. */
#define RFC_P256DH \
    "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4"
#define RFC_AUTH "BTBZMqHH6r4Tts7J_aSIgg"
#define VAPID_PUBLIC \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8" \
    "cTriz_qYBVicY02_VxTQ="
#define VAPID_PRIVATE "qfXLHghuG1rSHZUVo9SscNRI-0EIHRbIrfeGCqbAwak="
#define VAPID_SUBJECT "mailto:support@37signals.com"

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- scratch app + started writer ------------------------------------------ */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
} notify_env;

static bool env_open(notify_env *env, bool with_vapid) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;
    if (with_vapid) {
        cf_config_entry entries[6] = {
            {"PUBLIC_ORIGIN", ORIGIN},
            {"SECRET_KEY_BASE", HEX64},
            {"VAPID_PUBLIC_KEY", VAPID_PUBLIC},
            {"VAPID_PRIVATE_KEY", VAPID_PRIVATE},
            {"VAPID_SUBJECT", VAPID_SUBJECT},
            {"DATABASE_PATH", env->scratch.path},
        };
        if (cf_config_parse(entries, 6, NULL, &env->config) != CF_OK) {
            return false;
        }
    } else {
        cf_config_entry entries[3] = {
            {"PUBLIC_ORIGIN", ORIGIN},
            {"SECRET_KEY_BASE", HEX64},
            {"DATABASE_PATH", env->scratch.path},
        };
        if (cf_config_parse(entries, 3, NULL, &env->config) != CF_OK) {
            return false;
        }
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
    exchange_calls = 0;
    exchange_status = 201;
    exchange_endpoint[0] = '\0';
    if (cf_test_routes_add(
            "POST",
            "/users/:user_id/push_subscriptions/:push_subscription_id/"
            "test_notifications(.:format)",
            65,
            cf_action_users_push_subscriptions_test_notifications_create) !=
            CF_OK ||
        cf_test_routes_add(
            "POST",
            "/users/:user_id/push_subscriptions/:push_subscription_id/"
            "test_notifications",
            65,
            cf_action_users_push_subscriptions_test_notifications_create) !=
            CF_OK) {
        return false;
    }
    return true;
}

static void env_close(notify_env *env) {
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
                              const char *auth) {
    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO push_subscriptions (id, auth_key, created_at, "
             "endpoint, p256dh_key, updated_at, user_agent, user_id) "
             "VALUES (%lld, ?, '2026-01-02 03:04:05', ?, ?, "
             "'2026-01-02 03:04:05', NULL, %lld)",
             (long long)id, (long long)user_id);
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, auth, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, endpoint, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, p256dh, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    CF_REQUIRE(step == SQLITE_DONE);
}

static void seed_room_membership(cf_db *db, int64_t room_id,
                                 int64_t membership_id, int64_t user_id,
                                 bool unread) {
    char sql[768];
    snprintf(sql, sizeof sql,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (%lld, '2026-01-02 03:04:05', %lld, 'HQ', "
             "'Rooms::Open', '2026-01-02 03:04:05')",
             (long long)room_id, (long long)user_id);
    exec_sql(db, sql);
    snprintf(sql, sizeof sql,
             "INSERT INTO memberships (id, connected_at, connections, "
             "created_at, involvement, room_id, unread_at, updated_at, "
             "user_id) VALUES (%lld, NULL, 0, '2026-01-02 03:04:05', "
             "'mentions', %lld, %s, '2026-01-02 03:04:05', %lld)",
             (long long)membership_id, (long long)room_id,
             unread ? "'2026-01-03 03:04:05'" : "NULL", (long long)user_id);
    exec_sql(db, sql);
}

/* --- request/response helpers ---------------------------------------------- */

static bool run_request(notify_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void notify_request(cf_request *req, const char *target,
                           const char *sec_fetch_site,
                           const char *cookie_header) {
    cf_test_req_init(req);
    req->method = CF_POST;
    req->original_method = CF_POST;
    req->path = SP(target);
    req->target = SP(target);
    req->body = SP("");
    CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                  SP("application/x-www-form-urlencoded")) ==
               CF_OK);
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

CF_TEST(users_push_test_notifications_route_id_binds_the_action) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add(
                   "POST",
                   "/users/:user_id/push_subscriptions/:push_subscription_id/"
                   "test_notifications",
                   65,
                   cf_action_users_push_subscriptions_test_notifications_create) ==
               CF_OK);
    CF_CHECK(cf_route_action(65) ==
             cf_action_users_push_subscriptions_test_notifications_create);
}

CF_TEST(users_push_test_notifications_deliver_and_redirect) {
    notify_env env;
    CF_REQUIRE(env_open(&env, true));
    seed_user(env.scratch.db, 1, "David");
    seed_subscription(env.scratch.db, 20, 1, FCM_SEND, RFC_P256DH, RFC_AUTH);
    seed_room_membership(env.scratch.db, 10, 100, 1, true);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    exchange_status = 201;
    cf_request req;
    cf_response resp;
    notify_request(&req, "/users/me/push_subscriptions/20/test_notifications",
                   "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN
                           "/users/me/push_subscriptions\r\n"));
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    /* The exchange ran once, against this subscription's endpoint. */
    CF_CHECK(exchange_calls == 1);
    CF_CHECK(strcmp(exchange_endpoint, FCM_SEND) == 0);
    /* The subscription survives its own test (no inline destroy). */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions WHERE "
                        "\"id\" = 20") == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_push_test_notifications_failed_delivery_is_500) {
    notify_env env;
    CF_REQUIRE(env_open(&env, true));
    seed_user(env.scratch.db, 1, "David");
    seed_subscription(env.scratch.db, 20, 1, FCM_SEND, RFC_P256DH, RFC_AUTH);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    /* A gone subscription (410) is still the reference's 500 here, and
     * the row is preserved: the pool (not this action) owns invalidation. */
    exchange_status = 410;
    cf_request req;
    cf_response resp;
    notify_request(&req, "/users/me/push_subscriptions/20/test_notifications",
                   "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    CF_CHECK(!head_contains(&resp, &req, "Location: "));
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM push_subscriptions WHERE "
                        "\"id\" = 20") == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_push_test_notifications_vapid_off_is_500) {
    notify_env env;
    CF_REQUIRE(env_open(&env, false)); /* no VAPID keys configured */
    seed_user(env.scratch.db, 1, "David");
    seed_subscription(env.scratch.db, 20, 1, FCM_SEND, RFC_P256DH, RFC_AUTH);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    notify_request(&req, "/users/me/push_subscriptions/20/test_notifications",
                   "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    /* "Web Push is off": nothing resolved, nothing delivered. */
    CF_CHECK(resolve_calls == 0);
    CF_CHECK(exchange_calls == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_push_test_notifications_unknown_or_foreign_is_404) {
    notify_env env;
    CF_REQUIRE(env_open(&env, true));
    seed_user(env.scratch.db, 1, "David");
    seed_user(env.scratch.db, 2, "Kevin");
    seed_subscription(env.scratch.db, 20, 2, FCM_SEND, RFC_P256DH, RFC_AUTH);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    /* Another user's subscription is RecordNotFound (AUTH-06): no delivery. */
    cf_request req;
    cf_response resp;
    notify_request(&req, "/users/me/push_subscriptions/20/test_notifications",
                   "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    CF_CHECK(exchange_calls == 0);
    cf_response_dispose(&resp);

    /* So is a missing row. */
    cf_request req2;
    cf_response resp2;
    notify_request(&req2,
                   "/users/me/push_subscriptions/999/test_notifications",
                   "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 404);
    CF_CHECK(exchange_calls == 0);
    cf_response_dispose(&resp2);

    /* And an uncastable id segment. */
    cf_request req3;
    cf_response resp3;
    notify_request(&req3,
                   "/users/me/push_subscriptions/abc/test_notifications",
                   "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req3, &resp3));
    CF_CHECK(resp3.status == 404);
    CF_CHECK(exchange_calls == 0);
    cf_response_dispose(&resp3);
    env_close(&env);
}

CF_TEST(users_push_test_notifications_unauthenticated_redirects) {
    notify_env env;
    CF_REQUIRE(env_open(&env, true));
    seed_user(env.scratch.db, 1, "David");
    seed_subscription(env.scratch.db, 20, 1, FCM_SEND, RFC_P256DH, RFC_AUTH);

    cf_request req;
    cf_response resp;
    notify_request(&req, "/users/me/push_subscriptions/20/test_notifications",
                   "same-origin", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    CF_CHECK(exchange_calls == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_push_test_notifications_cross_site_post_is_422) {
    notify_env env;
    CF_REQUIRE(env_open(&env, true));
    seed_user(env.scratch.db, 1, "David");
    seed_subscription(env.scratch.db, 20, 1, FCM_SEND, RFC_P256DH, RFC_AUTH);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    notify_request(&req, "/users/me/push_subscriptions/20/test_notifications",
                   "cross-site", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    CF_CHECK(exchange_calls == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
