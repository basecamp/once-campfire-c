/* tests/actions/accounts_test.c — A-accounts acceptance: `accounts#edit`
 * (route ID 44) and `accounts#update` (route IDs 46, 47), per
 * docs/devel/implementation/contracts/controller-packets.md "A-accounts"
 * and 03-application.md's family boundaries (administrator gating, no
 * writer-side password hashing).
 *
 * Cases run the real A00 dispatch path through the route double, which
 * binds rows 44/46/47 to this packet's actions:
 *   - route binding: ids 44/46/47 resolve to edit/update;
 *   - edit renders the account name, join code, restrict flag and the
 *     partitioned administrator/member lists (200, HTML), plus the
 *     next-page loader when the list exceeds one 500-page;
 *   - edit as JSON is 406; unauthenticated is 302 to sign-in; a missing
 *     account is 500;
 *   - update renames and flips the room-creation restriction (302 to
 *     /account/edit, settings JSON changed);
 *   - update is administrator-only (403 before any lookup), requires the
 *     account param (400), rejects unknown settings keys (500, the
 *     reference's raise), deletes the logo on an empty logo value
 *     (attachment gone, purge event), and rejects a non-empty logo string
 *     (500, the reference's "expected attachable" raise);
 *   - a logo upload arm is unreachable through dispatch today (H02 never
 *     builds an upload param node) and is reported, not faked.
 *
 * cf.h and src/actions/actions.h are integrator-owned and do not yet declare
 * this packet's symbols, so the entry points are declared here; the
 * integrator's routes.c rebind needs the same declarations (c_symbols
 * cf_action_accounts_edit / cf_action_accounts_update).
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

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <inttypes.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

cf_err cf_action_accounts_edit(cf_ctx *ctx);
cf_err cf_action_accounts_update(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- scratch app + started writer ------------------------------------------ */

#define ACCOUNTS_MAX_EVENTS 8

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
    cf_event purge_events[ACCOUNTS_MAX_EVENTS];
    size_t purge_event_count;
} accounts_env;

static cf_err accounts_capture_purge(void *ctx, const cf_event *event) {
    accounts_env *env = ctx;
    if (env->purge_event_count < ACCOUNTS_MAX_EVENTS) {
        env->purge_events[env->purge_event_count++] = *event;
    }
    return CF_OK;
}

static bool env_open(accounts_env *env) {
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
    if (cf_writer_start(env->app, env->config) != CF_OK) return false;
    env->writer_started = true;
    if (cf_writer_set_event_handler(env->app, CF_EVENT_PURGE_BLOB,
                                    accounts_capture_purge, env) != CF_OK) {
        return false;
    }
    cf_test_routes_reset();
    if (cf_test_routes_add("GET", "/account/edit", 44,
                           cf_action_accounts_edit) != CF_OK ||
        cf_test_routes_add("GET", "/account/edit(.:format)", 44,
                           cf_action_accounts_edit) != CF_OK ||
        cf_test_routes_add("PATCH", "/account", 46,
                           cf_action_accounts_update) != CF_OK ||
        cf_test_routes_add("PATCH", "/account(.:format)", 46,
                           cf_action_accounts_update) != CF_OK ||
        cf_test_routes_add("PUT", "/account", 47,
                           cf_action_accounts_update) != CF_OK ||
        cf_test_routes_add("PUT", "/account(.:format)", 47,
                           cf_action_accounts_update) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(accounts_env *env) {
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

static int64_t count_sql(cf_db *db, const char *sql) {
    bool ok = false;
    int64_t value = cf_db_test_i64(cf_db_handle(db), sql, &ok);
    CF_REQUIRE(ok);
    return value;
}

static void seed_account(cf_db *db) {
    exec_sql(db,
             "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
             "name, settings, singleton_guard, updated_at) VALUES (1, "
             "'2026-01-02 03:04:05', NULL, 'ABCD-EFGH-IJKL', 'Campfire', "
             "'{\"restrict_room_creation_to_administrators\":false}', 0, "
             "'2026-01-02 03:04:05')");
}

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
    CF_REQUIRE(step == SQLITE_DONE);
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

static void seed_blob_attachment(cf_db *db) {
    exec_sql(db,
             "INSERT INTO active_storage_blobs (id, byte_size, checksum, "
             "content_type, created_at, filename, key, metadata, "
             "service_name) VALUES (1, 4, NULL, 'image/png', "
             "'2026-01-02 03:04:05', 'logo.png', "
             "'aaaaaaaaaaaaaaaaaaaaaaaaaaaa', NULL, 'local')");
    exec_sql(db,
             "INSERT INTO active_storage_attachments (id, blob_id, created_at, "
             "name, record_id, record_type) VALUES (1, 1, "
             "'2026-01-02 03:04:05', 'logo', 1, 'Account')");
}

/* "session_token=<wire>" with the Rack form-escaping a base64 value needs. */
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

/* --- request/response helpers ---------------------------------------------- */

static bool run_request(accounts_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void prepare_get(cf_request *req, const char *path,
                        const char *cookie) {
    cf_test_req_init(req);
    req->path = SP(path);
    req->target = SP(path);
    if (cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    }
}

static void prepare_form(cf_request *req, cf_method method, const char *path,
                         const char *body, const char *cookie, bool csrf_ok) {
    cf_test_req_init(req);
    req->method = method;
    req->original_method = method;
    req->path = SP(path);
    req->target = SP(path);
    req->body = SP(body);
    CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                  SP("application/x-www-form-urlencoded")) ==
               CF_OK);
    if (csrf_ok) {
        CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                      SP("same-origin")) == CF_OK);
    }
    if (cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
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

static bool body_contains(const cf_response *resp, const char *needle) {
    return resp->body != NULL && span_contains(cf_buf_span(resp->body), needle);
}

static bool head_contains(cf_response *resp, cf_request *req,
                          const char *needle) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    bool found = ser.headers != NULL &&
                 span_contains(cf_buf_span(ser.headers), needle);
    cf_buf_release(ser.headers);
    return found;
}

/* --- acceptance ------------------------------------------------------------ */

CF_TEST(accounts_route_ids_bind_the_actions) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/account/edit", 44,
                                  cf_action_accounts_edit) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("PATCH", "/account", 46,
                                  cf_action_accounts_update) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("PUT", "/account", 47,
                                  cf_action_accounts_update) == CF_OK);
    CF_CHECK(cf_route_action(44) == cf_action_accounts_edit);
    CF_CHECK(cf_route_action(46) == cf_action_accounts_update);
    CF_CHECK(cf_route_action(47) == cf_action_accounts_update);
}

CF_TEST(accounts_edit_renders_account_and_partitioned_users) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    seed_user(env.scratch.db, 2, "Bob Member", "bob@example.com", 0, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session", 2);

    cf_request req;
    cf_response resp;
    prepare_get(&req, "/account/edit", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    CF_CHECK(body_contains(&resp, "Campfire"));
    CF_CHECK(body_contains(&resp, "ABCD-EFGH-IJKL"));
    CF_CHECK(body_contains(&resp, "data-restrict=\"false\""));
    CF_CHECK(body_contains(&resp, "data-group=\"administrators\""));
    CF_CHECK(body_contains(&resp, "Ada Admin"));
    CF_CHECK(body_contains(&resp, "data-group=\"members\""));
    CF_CHECK(body_contains(&resp, "Bob Member"));
    /* Two users fit on one 500-page: no next-page loader. */
    CF_CHECK(!body_contains(&resp, "next_page_container\" src="));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_edit_paginates_with_a_next_page_loader) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    char sql[256];
    for (int i = 0; i < 505; i++) {
        snprintf(sql, sizeof sql,
                 "INSERT INTO users (bio, bot_token, created_at, "
                 "email_address, name, password_digest, role, status, "
                 "updated_at) VALUES (NULL, NULL, '2026-01-02 03:04:05', NULL, "
                 "'Member %d', NULL, 0, 0, '2026-01-02 03:04:05')",
                 i);
        exec_sql(env.scratch.db, sql);
    }
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_get(&req, "/account/edit", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "/account/users.turbo_stream?page=2"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_edit_unauthenticated_redirects_to_sign_in) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);

    cf_request req;
    cf_response resp;
    prepare_get(&req, "/account/edit", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_edit_json_is_406) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_get(&req, "/account/edit", cookie);
    CF_REQUIRE(cf_test_req_header(&req, SP("Accept"),
                                  SP("application/json")) == CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_edit_without_account_is_500) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_get(&req, "/account/edit", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_update_renames_and_restricts_room_creation) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "/account",
                 "account[name]=New+Name&account[settings]["
                 "restrict_room_creation_to_administrators]=1",
                 cookie, true);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/account/edit\r\n"));
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM accounts WHERE name='New Name'") ==
             1);
    char settings[256];
    CF_REQUIRE(cf_db_test_text(cf_db_handle(env.scratch.db),
                               "SELECT settings FROM accounts WHERE id=1",
                               settings, sizeof settings) != NULL);
    CF_CHECK(span_contains(
        (cf_span){(const unsigned char *)settings, strlen(settings)},
        "restrict_room_creation_to_administrators\":true"));
    cf_writer_stats stats = {0};
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.committed == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_update_over_put_redirects_to_edit_account) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PUT, "/account", "account[name]=Put+Name", cookie,
                 true);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/account/edit\r\n"));
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM accounts WHERE name='Put Name'") ==
             1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_update_requires_an_administrator) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Member", "member@example.com", 0, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "/account", "account[name]=Hijacked", cookie,
                 true);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM accounts WHERE name='Campfire'") ==
             1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_update_unauthenticated_redirects_to_sign_in) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "/account", "account[name]=Hijacked", NULL,
                 true);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM accounts WHERE name='Campfire'") ==
             1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_update_without_account_param_is_400) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "/account", "name=Nope", cookie, true);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 400);
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM accounts WHERE name='Campfire'") ==
             1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_update_unknown_settings_key_is_500) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "/account",
                 "account[settings][bogus_key]=1", cookie, true);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM accounts WHERE name='Campfire'") ==
             1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_update_empty_logo_destroys_the_attachment) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    seed_blob_attachment(env.scratch.db);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "/account", "account[logo]=", cookie, true);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM active_storage_attachments WHERE "
                       "record_type='Account' AND name='logo'") == 0);
    CF_REQUIRE(env.purge_event_count == 1);
    CF_CHECK(env.purge_events[0].kind == CF_EVENT_PURGE_BLOB);
    CF_CHECK(env.purge_events[0].blob_id == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_update_string_logo_is_500) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "/account", "account[logo]=not-a-blob",
                 cookie, true);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_update_without_account_is_500) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_form(&req, CF_PATCH, "/account", "account[name]=Ghost", cookie,
                 true);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_update_cross_site_post_is_422) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    cf_test_req_init(&req);
    req.method = CF_PATCH;
    req.original_method = CF_PATCH;
    req.path = SP("/account");
    req.target = SP("/account");
    req.body = SP("account[name]=Hijacked");
    CF_REQUIRE(cf_test_req_header(&req, SP("Content-Type"),
                                  SP("application/x-www-form-urlencoded")) ==
               CF_OK);
    CF_REQUIRE(cf_test_req_header(&req, SP("Sec-Fetch-Site"),
                                  SP("cross-site")) == CF_OK);
    CF_REQUIRE(cf_test_req_header(&req, SP("Cookie"), SP(cookie)) == CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM accounts WHERE name='Campfire'") ==
             1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
