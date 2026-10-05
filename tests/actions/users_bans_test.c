/* tests/actions/users_bans_test.c — A-users-bans acceptance: `users/bans
 * #destroy` (route ID 55) and `users/bans#create` (route ID 56), per
 * docs/devel/implementation/contracts/controller-packets.md "A-users-bans"
 * and 03-application.md's "users/bans#create/destroy" slice row:
 * "Administrator only; exact ban/session/IP/content effects; disconnect
 * barrier; redirect to user".
 *
 * Cases:
 *   - route binding: ids 55/56 resolve to the two actions;
 *   - create success: the target's sessions become one bans row per distinct
 *     non-blank ip_address, every target session row is deleted, the target
 *     status becomes banned, and the writer applied the mandatory
 *     DISCONNECT_USER(reconnect=false) with CF_OK, then the best-effort
 *     REMOVE_BANNED_CONTENT event; the redirect is 302 to
 *     <PUBLIC_ORIGIN>/users/<id> with no body;
 *   - the J02 disclosure: without a REMOVE_BANNED_CONTENT consumer the event
 *     is dropped and counted (per-kind writer counter), the committed write
 *     unchanged;
 *   - a failing mandatory control handler makes the committed write report
 *     CF_INTERNAL (500, rows stay; never a pretend redirect);
 *   - unauthorized (302 to sign-in), forbidden (403, and before find_user),
 *     CSRF (422), parameter failure / not found (404: non-integer segment
 *     and absent user), all with no row effects and no events;
 *   - destroy is unban exactly as the source writes it: bans rows and the
 *     banned status only; sessions stay untouched and nothing is
 *     disconnected (not an assumed inverse of create).
 *
 * The route double (tests/app/support/route_double.c) binds rows 55/56 to
 * the real actions, so every case runs the real A00 dispatch path (params,
 * cookies, before-actions, format negotiation, generic error mapping).  The
 * writer is started, a capture control handler is registered (the mandatory
 * DISCONNECT path) and, where the case asserts delivery, a best-effort
 * handler for CF_EVENT_REMOVE_BANNED_CONTENT.
 *
 * cf.h and src/actions/actions.h are integrator-owned and do not yet declare
 * this packet's symbols, so the two entry points are declared here; the
 * integrator's routes.c rebind needs the same declarations (c_symbols
 * cf_action_users_bans_destroy / cf_action_users_bans_create).
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "http/http_internal.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

cf_err cf_action_users_bans_destroy(cf_ctx *ctx);
cf_err cf_action_users_bans_create(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="
/* Public IPs (Ban::validate rejects private/loopback/link-local). */
#define PUBLIC_IP_A "203.0.113.7"
#define PUBLIC_IP_B "8.8.8.8"
#define PUBLIC_IP_OTHER "198.51.100.9"

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- scratch app + started writer + capture handlers ----------------------- */

#define BANS_MAX_EVENTS 16

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
    bool fail_control; /* the mandatory control handler reports CF_INTERNAL */
    cf_event control_events[BANS_MAX_EVENTS];
    size_t control_event_count;
    cf_event best_effort_events[BANS_MAX_EVENTS];
    size_t best_effort_event_count;
} bans_env;

static cf_err bans_capture_control(void *ctx, const cf_event *event) {
    bans_env *env = ctx;
    if (env->fail_control) return CF_INTERNAL;
    if (env->control_event_count < BANS_MAX_EVENTS) {
        env->control_events[env->control_event_count++] = *event;
    }
    return CF_OK;
}

static cf_err bans_capture_best_effort(void *ctx, const cf_event *event) {
    bans_env *env = ctx;
    if (env->best_effort_event_count < BANS_MAX_EVENTS) {
        env->best_effort_events[env->best_effort_event_count++] = *event;
    }
    return CF_OK;
}

/* capture_remove registers the best-effort REMOVE_BANNED_CONTENT consumer
 * (J02's slot); fail_control makes the mandatory DISCONNECT handler fail. */
static bool env_open(bans_env *env, bool capture_remove, bool fail_control) {
    memset(env, 0, sizeof *env);
    env->fail_control = fail_control;
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
    if (cf_writer_set_control_handler(env->app, bans_capture_control, env) !=
        CF_OK) {
        return false;
    }
    if (capture_remove &&
        cf_writer_set_event_handler(env->app, CF_EVENT_REMOVE_BANNED_CONTENT,
                                    bans_capture_best_effort, env) != CF_OK) {
        return false;
    }
    cf_test_routes_reset();
    /* routes.json ids 55 (DELETE) / 56 (POST).  The double's `(.:format)`
     * grammar needs an extension for a literal last segment, so the bare form
     * is registered as well (reported in the evidence). */
    if (cf_test_routes_add("DELETE", "/users/:user_id/ban(.:format)", 55,
                           cf_action_users_bans_destroy) != CF_OK ||
        cf_test_routes_add("DELETE", "/users/:user_id/ban", 55,
                           cf_action_users_bans_destroy) != CF_OK ||
        cf_test_routes_add("POST", "/users/:user_id/ban(.:format)", 56,
                           cf_action_users_bans_create) != CF_OK ||
        cf_test_routes_add("POST", "/users/:user_id/ban", 56,
                           cf_action_users_bans_create) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(bans_env *env) {
    if (env->writer_started) cf_writer_stop(env->app);
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

static int64_t count_rows(cf_db *db, const char *sql) {
    bool ok = false;
    int64_t value = cf_db_test_i64(cf_db_handle(db), sql, &ok);
    CF_REQUIRE(ok);
    return value;
}

/* --- seeding --------------------------------------------------------------- */

static void seed_user(cf_db *db, int64_t id, const char *name,
                      const char *email, int role, int status) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, updated_at) "
             "VALUES (%lld, NULL, NULL, '2026-01-02 03:04:05', %s, '%s', "
             "NULL, %d, %d, '2026-01-02 03:04:05')",
             (long long)id, email != NULL ? "?" : "NULL", name, role, status);
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
    if (step != SQLITE_DONE) {
        fprintf(stderr, "  seed_user failed (%d): %s\n", step, sql);
    }
    CF_REQUIRE(step == SQLITE_DONE);
}

/* A session row whose last_active_at is in the future, so restoring it never
 * needs the writer (cf_session_needs_resume is false). */
static void seed_session(cf_db *db, const char *token, int64_t user_id,
                         const char *ip_address) {
    char sql[512];
    if (ip_address == NULL) {
        snprintf(sql, sizeof sql,
                 "INSERT INTO sessions (created_at, ip_address, "
                 "last_active_at, token, updated_at, user_agent, user_id) "
                 "VALUES ('2040-01-01 00:00:00.000000', NULL, "
                 "'2040-01-01 00:00:00.000000', '%s', "
                 "'2040-01-01 00:00:00.000000', NULL, %lld)",
                 token, (long long)user_id);
        exec_sql(db, sql);
        return;
    }
    snprintf(sql, sizeof sql,
             "INSERT INTO sessions (created_at, ip_address, last_active_at, "
             "token, updated_at, user_agent, user_id) VALUES "
             "('2040-01-01 00:00:00.000000', ?, "
             "'2040-01-01 00:00:00.000000', '%s', "
             "'2040-01-01 00:00:00.000000', NULL, %lld)",
             token, (long long)user_id);
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, ip_address, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (step != SQLITE_DONE) {
        fprintf(stderr, "  seed_session failed (%d): %s\n", step, sql);
    }
    CF_REQUIRE(step == SQLITE_DONE);
}

static void seed_ban(cf_db *db, int64_t id, int64_t user_id,
                     const char *ip_address) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO bans (id, created_at, ip_address, updated_at, "
             "user_id) VALUES (%lld, '2026-01-02 03:04:05', '%s', "
             "'2026-01-02 03:04:05', %lld)",
             (long long)id, ip_address, (long long)user_id);
    exec_sql(db, sql);
}

/* "session_token=<wire>" with the Rack form-escaping a base64 value needs. */
static void make_session_cookie(const cf_config *config, cf_db *db, char *out,
                                size_t cap, const char *token,
                                int64_t user_id) {
    seed_session(db, token, user_id, NULL);
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

/* --- request/response helpers ---------------------------------------------- */

static bool run_request(bans_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

/* POST/DELETE /users/<segment>/ban with the chain's CSRF input
 * (Sec-Fetch-Site; the reference verifies by it, not tokens). */
static void bans_request(cf_request *req, cf_method method, const char *target,
                         const char *sec_fetch_site,
                         const char *cookie_header) {
    cf_test_req_init(req);
    req->method = method;
    req->original_method = method;
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

static bool span_contains(cf_span span, const char *needle) {
    size_t len = strlen(needle);
    if (len == 0) return true;
    if (span.len < len) return false;
    for (size_t i = 0; i + len <= span.len; i++) {
        if (memcmp(span.ptr + i, needle, len) == 0) return true;
    }
    return false;
}

static bool head_contains(cf_response *resp, cf_request *req,
                          const char *needle) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    bool found = span_contains(cf_buf_span(ser.headers), needle);
    cf_buf_release(ser.headers);
    return found;
}

/* --- acceptance ------------------------------------------------------------ */

CF_TEST(users_bans_route_ids_bind_the_actions) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("DELETE", "/users/:user_id/ban", 55,
                                  cf_action_users_bans_destroy) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("POST", "/users/:user_id/ban", 56,
                                  cf_action_users_bans_create) == CF_OK);
    CF_CHECK(cf_route_action(55) == cf_action_users_bans_destroy);
    CF_CHECK(cf_route_action(56) == cf_action_users_bans_create);
}

/* Success: exact ban/session/IP/content effects, DISCONNECT(false) applied
 * with CF_OK, then REMOVE_BANNED_CONTENT; 302 to the user page. */
CF_TEST(users_bans_create_bans_ips_sessions_and_disconnects) {
    bans_env env;
    CF_REQUIRE(env_open(&env, true, false));
    seed_user(env.scratch.db, 1, "Admin", "admin@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Target", "target@example.com", 0, 0);
    seed_user(env.scratch.db, 3, "Other", "other@example.com", 0, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session-token", 1);

    /* The target's sessions, in row order: a duplicated public IP, a blank
     * one, a NULL one, another public IP, plus a decoy for another user. */
    seed_session(env.scratch.db, "t1", 2, PUBLIC_IP_A);
    seed_session(env.scratch.db, "t2", 2, PUBLIC_IP_A);
    seed_session(env.scratch.db, "t3", 2, NULL);
    seed_session(env.scratch.db, "t4", 2, "");
    seed_session(env.scratch.db, "t5", 2, PUBLIC_IP_B);
    seed_session(env.scratch.db, "o1", 3, PUBLIC_IP_OTHER);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions WHERE user_id=2") == 5);

    cf_request req;
    cf_response resp;
    bans_request(&req, CF_POST, "/users/2/ban", "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, "Location: " ORIGIN "/users/2\r\n"));
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    CF_CHECK(resp.body == NULL);

    /* Bans: one row per distinct non-blank session IP, scoped to the target. */
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM bans") == 2);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM bans WHERE user_id=2 AND "
                        "ip_address='" PUBLIC_IP_A "'") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM bans WHERE user_id=2 AND "
                        "ip_address='" PUBLIC_IP_B "'") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM bans WHERE ip_address=''") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM bans WHERE user_id=3") == 0);

    /* Sessions: all of the target's rows are gone; others stay. */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions WHERE user_id=2") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions WHERE user_id=1") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions WHERE user_id=3") == 1);

    /* Status transitions: only the target becomes banned. */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id=1 AND status=0") ==
              1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id=2 AND status=2") ==
              1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id=3 AND status=0") ==
              1);

    /* Side effects: the mandatory DISCONNECT(false) ran (CF_OK), then the
     * best-effort REMOVE_BANNED_CONTENT. */
    CF_REQUIRE(env.control_event_count == 1);
    CF_CHECK(env.control_events[0].kind == CF_EVENT_DISCONNECT_USER);
    CF_CHECK(env.control_events[0].user_id == 2);
    CF_CHECK(!env.control_events[0].reconnect);
    CF_REQUIRE(env.best_effort_event_count == 1);
    CF_CHECK(env.best_effort_events[0].kind ==
             CF_EVENT_REMOVE_BANNED_CONTENT);
    CF_CHECK(env.best_effort_events[0].user_id == 2);

    cf_writer_stats stats;
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.committed == 1);
    CF_CHECK(stats.rolled_back == 0);
    CF_CHECK(stats.mandatory_failures == 0);

    cf_response_dispose(&resp);
    env_close(&env);
}

/* J02 disclosure: REMOVE_BANNED_CONTENT has no consumer until Phase 4; the
 * writer drops it and counts it per kind, and the committed write is
 * unchanged (302, rows applied). */
CF_TEST(users_bans_create_drops_remove_banned_content_and_counts_it) {
    bans_env env;
    CF_REQUIRE(env_open(&env, false, false));
    seed_user(env.scratch.db, 1, "Admin", "admin@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Target", "target@example.com", 0, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session-token", 1);
    seed_session(env.scratch.db, "t1", 2, PUBLIC_IP_A);

    cf_request req;
    cf_response resp;
    bans_request(&req, CF_POST, "/users/2/ban", "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM bans") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id=2 AND status=2") ==
              1);
    CF_CHECK(env.best_effort_event_count == 0);
    CF_REQUIRE(env.control_event_count == 1); /* the mandatory path still ran */

    cf_writer_stats stats;
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.best_effort_dropped[CF_EVENT_REMOVE_BANNED_CONTENT] == 1);
    CF_CHECK(stats.committed == 1);
    CF_CHECK(stats.mandatory_failures == 0);

    cf_response_dispose(&resp);
    env_close(&env);
}

/* A failing mandatory DISCONNECT handler: the write reports CF_INTERNAL after
 * the commit (D02/C03), the action propagates it, and the rows stay. */
CF_TEST(users_bans_create_disconnect_failure_is_internal_after_commit) {
    bans_env env;
    CF_REQUIRE(env_open(&env, false, true));
    seed_user(env.scratch.db, 1, "Admin", "admin@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Target", "target@example.com", 0, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session-token", 1);
    seed_session(env.scratch.db, "t1", 2, PUBLIC_IP_A);

    cf_request req;
    cf_response resp;
    bans_request(&req, CF_POST, "/users/2/ban", "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    CF_CHECK(!head_contains(&resp, &req, "Location: "));

    /* Never a pretend rollback: the committed effects are all there. */
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM bans") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id=2 AND status=2") ==
              1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions WHERE user_id=2") == 0);
    cf_writer_stats stats;
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.committed == 1);
    CF_CHECK(stats.mandatory_failures == 1);

    cf_response_dispose(&resp);
    env_close(&env);
}

/* Forbidden: an authenticated non-administrator is 403 (head :forbidden) and
 * nothing changes. */
CF_TEST(users_bans_create_requires_an_administrator) {
    bans_env env;
    CF_REQUIRE(env_open(&env, true, false));
    seed_user(env.scratch.db, 1, "Member", "member@example.com", 0, 0);
    seed_user(env.scratch.db, 2, "Target", "target@example.com", 0, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session-token", 1);
    seed_session(env.scratch.db, "t1", 2, PUBLIC_IP_A);

    cf_request req;
    cf_response resp;
    bans_request(&req, CF_POST, "/users/2/ban", "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    CF_CHECK(head_contains(&resp, &req, "Content-Type: text/html\r\n"));
    CF_CHECK(resp.body == NULL);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM bans") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions WHERE user_id=2") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id=2 AND status=0") ==
              1);
    CF_CHECK(env.control_event_count == 0);
    CF_CHECK(env.best_effort_event_count == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* `head :forbidden` carries the rendered format's content type (kit
 * Ctx::head -> response.content_type(format.string)), not a hardcoded one. */
CF_TEST(users_bans_forbidden_head_uses_the_rendered_format) {
    bans_env env;
    CF_REQUIRE(env_open(&env, false, false));
    seed_user(env.scratch.db, 1, "Member", "member@example.com", 0, 0);
    seed_user(env.scratch.db, 2, "Target", "target@example.com", 0, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session-token", 1);

    cf_request req;
    cf_response resp;
    bans_request(&req, CF_POST, "/users/2/ban.json", "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    CF_CHECK(head_contains(&resp, &req, "Content-Type: application/json\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Callback order: ensure_can_administer runs before find_user, so a non-admin
 * gets 403 even for a user that does not exist. */
CF_TEST(users_bans_ensure_can_administer_precedes_find_user) {
    bans_env env;
    CF_REQUIRE(env_open(&env, false, false));
    seed_user(env.scratch.db, 1, "Member", "member@example.com", 0, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session-token", 1);

    cf_request req;
    cf_response resp;
    bans_request(&req, CF_POST, "/users/999/ban", "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403); /* not 404 */
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Unauthorized: the chain redirects to sign-in before the action body. */
CF_TEST(users_bans_create_unauthenticated_redirects_to_sign_in) {
    bans_env env;
    CF_REQUIRE(env_open(&env, true, false));
    seed_user(env.scratch.db, 2, "Target", "target@example.com", 0, 0);
    seed_session(env.scratch.db, "t1", 2, PUBLIC_IP_A);

    cf_request req;
    cf_response resp;
    bans_request(&req, CF_POST, "/users/2/ban", "same-origin", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM bans") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions WHERE user_id=2") == 1);
    CF_CHECK(env.control_event_count == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* CSRF: a cross-site POST is the chain's 422 before the action body. */
CF_TEST(users_bans_create_cross_site_post_is_422) {
    bans_env env;
    CF_REQUIRE(env_open(&env, true, false));
    seed_user(env.scratch.db, 1, "Admin", "admin@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Target", "target@example.com", 0, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session-token", 1);
    seed_session(env.scratch.db, "t1", 2, PUBLIC_IP_A);

    cf_request req;
    cf_response resp;
    bans_request(&req, CF_POST, "/users/2/ban", "cross-site", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM bans") == 0);
    CF_CHECK(env.control_event_count == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Not found: an absent user id is the reference's User.find_by_id -> nil
 * (NotFound), after the administrator check. */
CF_TEST(users_bans_create_missing_user_is_not_found) {
    bans_env env;
    CF_REQUIRE(env_open(&env, true, false));
    seed_user(env.scratch.db, 1, "Admin", "admin@example.com", 1, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session-token", 1);

    cf_request req;
    cf_response resp;
    bans_request(&req, CF_POST, "/users/999/ban", "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM bans") == 0);
    CF_CHECK(env.control_event_count == 0);
    CF_CHECK(env.best_effort_event_count == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Parameter failure: a path segment that does not integer_cast is
 * `param_str("user_id") -> None` (the is-numeric gate of
 * ruby_compat::integer_cast), never a lookup. */
CF_TEST(users_bans_create_non_numeric_user_id_is_not_found) {
    bans_env env;
    CF_REQUIRE(env_open(&env, true, false));
    seed_user(env.scratch.db, 1, "Admin", "admin@example.com", 1, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session-token", 1);

    cf_request req;
    cf_response resp;
    bans_request(&req, CF_POST, "/users/abc/ban", "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    CF_CHECK(env.control_event_count == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* The shared integer_cast is String#to_i-like: a segment with trailing
 * non-digits ("2abc") still casts to the leading integer, as the source's
 * `integer_cast` does, and the lookup uses it. */
CF_TEST(users_bans_create_user_id_uses_integer_cast_semantics) {
    bans_env env;
    CF_REQUIRE(env_open(&env, true, false));
    seed_user(env.scratch.db, 1, "Admin", "admin@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Target", "target@example.com", 0, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session-token", 1);
    seed_session(env.scratch.db, "t1", 2, PUBLIC_IP_A);

    cf_request req;
    cf_response resp;
    bans_request(&req, CF_POST, "/users/2abc/ban", "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, "Location: " ORIGIN "/users/2\r\n"));
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM bans WHERE user_id=2") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id=2 AND status=2") ==
              1);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Destroy is `@user.unban`: the bans rows go, the status returns to active,
 * the redirect points at the user -- and, following the source, sessions are
 * untouched and nothing is disconnected (not an assumed inverse). */
CF_TEST(users_bans_destroy_unbans_and_redirects_to_user) {
    bans_env env;
    CF_REQUIRE(env_open(&env, true, false));
    seed_user(env.scratch.db, 1, "Admin", "admin@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Target", "target@example.com", 0, 2);
    seed_user(env.scratch.db, 3, "Other", "other@example.com", 0, 2);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session-token", 1);
    seed_ban(env.scratch.db, 11, 2, PUBLIC_IP_A);
    seed_ban(env.scratch.db, 12, 2, PUBLIC_IP_B);
    seed_ban(env.scratch.db, 13, 3, PUBLIC_IP_OTHER);
    /* A session the target started after the ban stays: unban does not touch
     * sessions. */
    seed_session(env.scratch.db, "t1", 2, PUBLIC_IP_A);

    cf_request req;
    cf_response resp;
    bans_request(&req, CF_DELETE, "/users/2/ban", "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, "Location: " ORIGIN "/users/2\r\n"));
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    CF_CHECK(resp.body == NULL);

    /* Bans: only the target's rows are deleted. */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM bans WHERE user_id=2") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM bans WHERE user_id=3") == 1);

    /* Status: the target is active again; the other banned user is not. */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id=2 AND status=0") ==
              1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id=3 AND status=2") ==
              1);

    /* The source's unban emits nothing: no disconnect, no content job. */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions WHERE user_id=2") == 1);
    CF_CHECK(env.control_event_count == 0);
    CF_CHECK(env.best_effort_event_count == 0);

    cf_writer_stats stats;
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.committed == 1);
    CF_CHECK(stats.rolled_back == 0);
    CF_CHECK(stats.mandatory_failures == 0);

    cf_response_dispose(&resp);
    env_close(&env);
}

/* Destroy is administrator-only; a member's DELETE changes nothing. */
CF_TEST(users_bans_destroy_requires_an_administrator) {
    bans_env env;
    CF_REQUIRE(env_open(&env, false, false));
    seed_user(env.scratch.db, 1, "Member", "member@example.com", 0, 0);
    seed_user(env.scratch.db, 2, "Target", "target@example.com", 0, 2);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session-token", 1);
    seed_ban(env.scratch.db, 11, 2, PUBLIC_IP_A);

    cf_request req;
    cf_response resp;
    bans_request(&req, CF_DELETE, "/users/2/ban", "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM bans WHERE user_id=2") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id=2 AND status=2") ==
              1);
    CF_CHECK(env.control_event_count == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Unauthenticated DELETE: the chain redirects to sign-in; the ban stays. */
CF_TEST(users_bans_destroy_unauthenticated_redirects_to_sign_in) {
    bans_env env;
    CF_REQUIRE(env_open(&env, false, false));
    seed_user(env.scratch.db, 2, "Target", "target@example.com", 0, 2);
    seed_ban(env.scratch.db, 11, 2, PUBLIC_IP_A);

    cf_request req;
    cf_response resp;
    bans_request(&req, CF_DELETE, "/users/2/ban", "same-origin", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM bans WHERE user_id=2") == 1);
    CF_CHECK(env.control_event_count == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Destroy of an absent user is NotFound, like create. */
CF_TEST(users_bans_destroy_missing_user_is_not_found) {
    bans_env env;
    CF_REQUIRE(env_open(&env, false, false));
    seed_user(env.scratch.db, 1, "Admin", "admin@example.com", 1, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session-token", 1);

    cf_request req;
    cf_response resp;
    bans_request(&req, CF_DELETE, "/users/999/ban", "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM bans") == 0);
    CF_CHECK(env.control_event_count == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- P12-01: controlled stale-authorization races -------------------------- */

/* A second connection holds BEGIN IMMEDIATE with the actor/target change
 * (ban, role demotion or target removal) still uncommitted.  The action's
 * pre-write checks run on the reader against the old snapshot; its write
 * callback is then admitted to the writer queue but cannot start
 * (BEGIN IMMEDIATE waits on this connection's lock).  The committer commits
 * the change the moment the writer admission count advances, so the order is
 * fixed by the lock, not by a sleep: every pre-write check necessarily saw
 * the old state, and the in-transaction revalidation necessarily sees the
 * new one. */
typedef struct {
    cf_app *app;
    sqlite3 *lock;
    uint64_t admitted_before;
    _Atomic bool request_done;
    bool commit_ok;
} bans_stale_race;

static int64_t bans_race_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *bans_race_commit(void *arg) {
    bans_stale_race *race = arg;
    int64_t deadline = bans_race_ms() + 10000;
    for (;;) {
        cf_writer_stats stats = {0};
        if (cf_writer_stats_get(race->app, &stats) != CF_OK) break;
        if (stats.admitted > race->admitted_before) break;
        if (atomic_load_explicit(&race->request_done,
                                 memory_order_relaxed)) {
            break; /* the request failed before queueing a write */
        }
        if (bans_race_ms() >= deadline) break;
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
static bool bans_race_begin(bans_env *env, bans_stale_race *race,
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

static void bans_race_end(bans_stale_race *race) {
    if (race->lock != NULL) sqlite3_close(race->lock);
    race->lock = NULL;
}

/* Run `req` while the held change commits as soon as its write is queued;
 * always joins the committer before returning (also on a request failure, so
 * no thread outlives the case). */
static bool bans_race_request(bans_env *env, bans_stale_race *race,
                              cf_request *req, cf_response *resp) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, bans_race_commit, race) != 0) {
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

/* The review's second reproduction: the actor is demoted to member while the
 * unban waits for the writer.  The pre-write administrator check saw role 1;
 * the in-transaction revalidation must reject the unban with the target's
 * ban rows and status untouched and no events.  Before the repair this
 * returned 302 and reactivated the banned user. */
CF_TEST(users_bans_destroy_rejects_an_actor_demoted_before_the_write) {
    bans_env env;
    CF_REQUIRE(env_open(&env, true, false));
    seed_user(env.scratch.db, 1, "Admin", "admin@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Target", "target@example.com", 0, 2);
    seed_ban(env.scratch.db, 11, 2, PUBLIC_IP_A);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session-token", 1);

    cf_request req;
    bans_request(&req, CF_DELETE, "/users/2/ban", "same-origin", cookie);

    bans_stale_race race;
    CF_REQUIRE(bans_race_begin(&env, &race,
                               "UPDATE users SET role = 0 WHERE id = 1"));

    cf_response resp;
    CF_CHECK(bans_race_request(&env, &race, &req, &resp));
    bans_race_end(&race);

    if (resp.status != 403) {
        fprintf(stderr, "  stale unban accepted: status=%u bans=%lld\n",
                resp.status,
                (long long)count_rows(
                    env.scratch.db,
                    "SELECT count(*) FROM bans WHERE user_id=2"));
    }
    CF_CHECK(resp.status == 403);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM bans WHERE user_id=2") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id=2 AND status=2") ==
              1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id=1 AND role=0") ==
              1);
    CF_CHECK(env.control_event_count == 0);
    CF_CHECK(env.best_effort_event_count == 0);
    cf_writer_stats stats = {0};
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.committed == 0);
    CF_CHECK(stats.rolled_back == 1);

    cf_response_dispose(&resp);
    env_close(&env);
}

/* A banned actor in the race window: the pre-write role check passed while
 * the ban was uncommitted; the in-transaction status revalidation must deny
 * (AUTH-05) with no changes and no events. */
CF_TEST(users_bans_destroy_rejects_an_actor_banned_before_the_write) {
    bans_env env;
    CF_REQUIRE(env_open(&env, true, false));
    seed_user(env.scratch.db, 1, "Admin", "admin@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Target", "target@example.com", 0, 2);
    seed_ban(env.scratch.db, 11, 2, PUBLIC_IP_A);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session-token", 1);

    cf_request req;
    bans_request(&req, CF_DELETE, "/users/2/ban", "same-origin", cookie);

    bans_stale_race race;
    CF_REQUIRE(bans_race_begin(&env, &race,
                               "UPDATE users SET status = 2 WHERE id = 1"));

    cf_response resp;
    CF_CHECK(bans_race_request(&env, &race, &req, &resp));
    bans_race_end(&race);

    if (resp.status != 403) {
        fprintf(stderr, "  stale unban accepted: status=%u bans=%lld\n",
                resp.status,
                (long long)count_rows(
                    env.scratch.db,
                    "SELECT count(*) FROM bans WHERE user_id=2"));
    }
    CF_CHECK(resp.status == 403);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM bans WHERE user_id=2") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id=2 AND status=2") ==
              1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id=1 AND status=2") ==
              1);
    CF_CHECK(env.control_event_count == 0);
    CF_CHECK(env.best_effort_event_count == 0);
    cf_writer_stats stats = {0};
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.committed == 0);
    CF_CHECK(stats.rolled_back == 1);

    cf_response_dispose(&resp);
    env_close(&env);
}

/* Create's stale role: the demoted actor's ban of the target must be
 * rejected before any bans row, status change, session deletion or event. */
CF_TEST(users_bans_create_rejects_an_actor_demoted_before_the_write) {
    bans_env env;
    CF_REQUIRE(env_open(&env, true, false));
    seed_user(env.scratch.db, 1, "Admin", "admin@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Target", "target@example.com", 0, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session-token", 1);
    seed_session(env.scratch.db, "t1", 2, PUBLIC_IP_A);

    cf_request req;
    bans_request(&req, CF_POST, "/users/2/ban", "same-origin", cookie);

    bans_stale_race race;
    CF_REQUIRE(bans_race_begin(&env, &race,
                               "UPDATE users SET role = 0 WHERE id = 1"));

    cf_response resp;
    CF_CHECK(bans_race_request(&env, &race, &req, &resp));
    bans_race_end(&race);

    if (resp.status != 403) {
        fprintf(stderr, "  stale ban accepted: status=%u bans=%lld\n",
                resp.status,
                (long long)count_rows(env.scratch.db,
                                      "SELECT count(*) FROM bans"));
    }
    CF_CHECK(resp.status == 403);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM bans") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id=2 AND status=0") ==
              1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions WHERE user_id=2") == 1);
    CF_CHECK(env.control_event_count == 0);
    CF_CHECK(env.best_effort_event_count == 0);
    cf_writer_stats stats = {0};
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.committed == 0);
    CF_CHECK(stats.rolled_back == 1);

    cf_response_dispose(&resp);
    env_close(&env);
}

/* Create's stale target: the target user is deleted while the ban waits for
 * the writer, so the callback's fresh target lookup is absent (404) and
 * nothing is created or disconnected. */
CF_TEST(users_bans_create_rejects_a_removed_target_before_the_write) {
    bans_env env;
    CF_REQUIRE(env_open(&env, true, false));
    seed_user(env.scratch.db, 1, "Admin", "admin@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Target", "target@example.com", 0, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session-token", 1);
    seed_session(env.scratch.db, "t1", 2, PUBLIC_IP_A);

    cf_request req;
    bans_request(&req, CF_POST, "/users/2/ban", "same-origin", cookie);

    bans_stale_race race;
    CF_REQUIRE(bans_race_begin(&env, &race, "DELETE FROM users WHERE id = 2"));

    cf_response resp;
    CF_CHECK(bans_race_request(&env, &race, &req, &resp));
    bans_race_end(&race);

    if (resp.status != 404) {
        fprintf(stderr, "  stale ban on removed target: status=%u bans=%lld\n",
                resp.status,
                (long long)count_rows(env.scratch.db,
                                      "SELECT count(*) FROM bans"));
    }
    CF_CHECK(resp.status == 404);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM bans") == 0);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM users WHERE id=2") ==
              0);
    CF_CHECK(env.control_event_count == 0);
    CF_CHECK(env.best_effort_event_count == 0);
    cf_writer_stats stats = {0};
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.committed == 0);
    CF_CHECK(stats.rolled_back == 1);

    cf_response_dispose(&resp);
    env_close(&env);
}

/* Destroy parameter failure: the same integer_cast gate. */
CF_TEST(users_bans_destroy_non_numeric_user_id_is_not_found) {
    bans_env env;
    CF_REQUIRE(env_open(&env, false, false));
    seed_user(env.scratch.db, 1, "Admin", "admin@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Target", "target@example.com", 0, 2);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session-token", 1);
    seed_ban(env.scratch.db, 11, 2, PUBLIC_IP_A);

    cf_request req;
    cf_response resp;
    bans_request(&req, CF_DELETE, "/users/abc/ban", "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM bans") == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
