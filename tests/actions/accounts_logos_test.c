/* tests/actions/accounts_logos_test.c — A-accounts-logos acceptance:
 * `accounts/logos#show` (route ID 38) and `accounts/logos#destroy` (route
 * ID 39), per docs/devel/implementation/contracts/controller-packets.md
 * "A-accounts-logos".
 *
 * Cases run the real A00 dispatch path through the route double, which
 * binds rows 38/39 to this packet's actions:
 *   - route binding: ids 38/39 resolve to show/destroy;
 *   - show serves the pinned stock icon with no logo attached (200,
 *     image/png inline, Cache-Control, weak ETag), the small variant for
 *     size=small, 304 on a matching If-None-Match, and 200 with no ETag
 *     when no account exists;
 *   - a variable logo uses the production processor; missing source is404;
 *     nonvariable attachments use the stock fallback;
 *   - destroy removes the attachment (302, purge event) and is a no-op
 *     redirect without one; administrator-only; unauthenticated redirect;
 *     missing account is 500.
 *
 * cf.h and src/actions/actions.h are integrator-owned and do not yet declare
 * this packet's symbols, so the entry points are declared here; the
 * integrator's routes.c rebind needs the same declarations (c_symbols
 * cf_action_accounts_logos_show / cf_action_accounts_logos_destroy).
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
#include "views.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

cf_err cf_action_accounts_logos_show(cf_ctx *ctx);
cf_err cf_action_accounts_logos_destroy(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- scratch app + started writer + purge capture -------------------------- */

#define LOGOS_MAX_EVENTS 8

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
    bool assets_ready;
    cf_event purge_events[LOGOS_MAX_EVENTS];
    size_t purge_event_count;
} logos_env;

static cf_err logos_capture_purge(void *ctx, const cf_event *event) {
    logos_env *env = ctx;
    if (env->purge_event_count < LOGOS_MAX_EVENTS) {
        env->purge_events[env->purge_event_count++] = *event;
    }
    return CF_OK;
}

static bool env_open(logos_env *env, bool need_assets) {
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
                                    logos_capture_purge, env) != CF_OK) {
        return false;
    }
    if (need_assets) {
        if (cf_views_assets_configure(NULL) != CF_OK) {
            fprintf(stderr, "  logos env: asset manifest unavailable\n");
            return false;
        }
        env->assets_ready = true;
    }
    cf_test_routes_reset();
    if (cf_test_routes_add("GET", "/account/logo", 38,
                           cf_action_accounts_logos_show) != CF_OK ||
        cf_test_routes_add("GET", "/account/logo(.:format)", 38,
                           cf_action_accounts_logos_show) != CF_OK ||
        cf_test_routes_add("DELETE", "/account/logo", 39,
                           cf_action_accounts_logos_destroy) != CF_OK ||
        cf_test_routes_add("DELETE", "/account/logo(.:format)", 39,
                           cf_action_accounts_logos_destroy) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(logos_env *env) {
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
                      const char *email, int role) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, updated_at) "
             "VALUES (%lld, NULL, NULL, '2026-01-02 03:04:05', '%s', '%s', "
             "NULL, %d, 0, '2026-01-02 03:04:05')",
             (long long)id, email, name, role);
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

static bool run_request(logos_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void prepare_show(cf_request *req, const char *query,
                         const char *if_none_match) {
    cf_test_req_init(req);
    req->path = SP("/account/logo");
    req->target = SP("/account/logo");
    if (query != NULL) req->query = SP(query);
    if (if_none_match != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("If-None-Match"),
                                      SP(if_none_match)) == CF_OK);
    }
}

static void prepare_destroy(cf_request *req, const char *cookie) {
    cf_test_req_init(req);
    req->method = CF_DELETE;
    req->original_method = CF_DELETE;
    req->path = SP("/account/logo");
    req->target = SP("/account/logo");
    req->body = SP("");
    CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                  SP("same-origin")) == CF_OK);
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

/* Copy the first ETag header value into out (without the name/CRLF). */
static bool head_etag(cf_response *resp, cf_request *req, char *out,
                      size_t cap) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    bool ok = false;
    if (ser.headers != NULL) {
        cf_span headers = cf_buf_span(ser.headers);
        const char *name = "ETag: ";
        for (size_t i = 0; i + 6 <= headers.len; i++) {
            if (memcmp(headers.ptr + i, name, 6) == 0) {
                size_t start = i + 6;
                size_t end = start;
                while (end + 1 < headers.len &&
                       !(headers.ptr[end] == '\r' &&
                         headers.ptr[end + 1] == '\n')) {
                    end++;
                }
                size_t len = end - start;
                if (len > 0 && len + 1 <= cap) {
                    memcpy(out, headers.ptr + start, len);
                    out[len] = '\0';
                    ok = true;
                }
                break;
            }
        }
    }
    cf_buf_release(ser.headers);
    return ok;
}

static bool file_is_png(const cf_response *resp) {
    static const unsigned char magic[8] = {0x89, 'P', 'N', 'G',
                                           '\r', '\n', 0x1a, '\n'};
    unsigned char head[8];
    if (resp->body_kind != CF_BODY_FILE || resp->file_fd < 0) return false;
    ssize_t n =
        pread(resp->file_fd, head, sizeof head, (off_t)resp->file_offset);
    return n == (ssize_t)sizeof head && memcmp(head, magic, 8) == 0;
}

/* --- acceptance ------------------------------------------------------------ */

CF_TEST(accounts_logos_route_ids_bind_the_actions) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/account/logo", 38,
                                  cf_action_accounts_logos_show) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("DELETE", "/account/logo", 39,
                                  cf_action_accounts_logos_destroy) == CF_OK);
    CF_CHECK(cf_route_action(38) == cf_action_accounts_logos_show);
    CF_CHECK(cf_route_action(39) == cf_action_accounts_logos_destroy);
}

CF_TEST(accounts_logos_show_serves_the_stock_icon) {
    logos_env env;
    CF_REQUIRE(env_open(&env, true));
    seed_account(env.scratch.db);

    cf_request req;
    cf_response resp;
    prepare_show(&req, NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: image/png\r\n"));
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Disposition: inline\r\n"));
    CF_CHECK(head_contains(&resp, &req, "Cache-Control: public, "
                                        "max-age=300, "
                                        "stale-while-revalidate=604800\r\n"));
    CF_CHECK(head_contains(&resp, &req, "ETag: W/\"accounts/1-"));
    CF_CHECK(file_is_png(&resp));
    CF_CHECK(resp.body_kind == CF_BODY_FILE && resp.file_length > 1000);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_logos_show_small_serves_the_small_icon) {
    logos_env env;
    CF_REQUIRE(env_open(&env, true));
    seed_account(env.scratch.db);

    cf_request large_req;
    cf_response large_resp;
    prepare_show(&large_req, NULL, NULL);
    CF_REQUIRE(run_request(&env, &large_req, &large_resp));
    CF_REQUIRE(large_resp.status == 200);

    cf_request req;
    cf_response resp;
    prepare_show(&req, "size=small", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req, "Content-Type: image/png\r\n"));
    CF_CHECK(file_is_png(&resp));
    CF_CHECK(large_resp.body_kind == CF_BODY_FILE);
    CF_CHECK(resp.body_kind == CF_BODY_FILE);
    CF_CHECK(resp.file_length != large_resp.file_length);
    cf_response_dispose(&large_resp);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_logos_show_matching_etag_is_304) {
    logos_env env;
    CF_REQUIRE(env_open(&env, true));
    seed_account(env.scratch.db);

    cf_request first;
    cf_response first_resp;
    prepare_show(&first, NULL, NULL);
    CF_REQUIRE(run_request(&env, &first, &first_resp));
    CF_REQUIRE(first_resp.status == 200);
    char etag[128];
    CF_REQUIRE(head_etag(&first_resp, &first, etag, sizeof etag));
    cf_response_dispose(&first_resp);

    cf_request req;
    cf_response resp;
    prepare_show(&req, NULL, etag);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 304);
    CF_CHECK(resp.body_kind == CF_BODY_NONE);
    CF_CHECK(head_contains(&resp, &req, "ETag: "));
    CF_CHECK(!head_contains(&resp, &req, "Cache-Control: "));
    cf_response_dispose(&resp);

    /* A non-matching validator serves the icon again. */
    cf_request stale;
    cf_response stale_resp;
    prepare_show(&stale, NULL, "W/\"accounts/1-0\"");
    CF_REQUIRE(run_request(&env, &stale, &stale_resp));
    CF_CHECK(stale_resp.status == 200);
    CF_CHECK(file_is_png(&stale_resp));
    cf_response_dispose(&stale_resp);
    env_close(&env);
}

CF_TEST(accounts_logos_show_without_account_has_no_etag) {
    logos_env env;
    CF_REQUIRE(env_open(&env, true));

    cf_request req;
    cf_response resp;
    prepare_show(&req, NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(file_is_png(&resp));
    CF_CHECK(!head_contains(&resp, &req, "ETag: "));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_logos_show_missing_logo_source_is_404) {
    logos_env env;
    CF_REQUIRE(env_open(&env, true));
    seed_account(env.scratch.db);
    seed_blob_attachment(env.scratch.db);

    cf_request req;
    cf_response resp;
    prepare_show(&req, NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    /* Missing stored source is explicit; attachment remains intact. */
    CF_CHECK(resp.status == 404);
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM active_storage_attachments WHERE "
                       "record_type='Account' AND name='logo'") == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_logos_nonvariable_attachment_uses_stock_icon) {
    logos_env env;
    CF_REQUIRE(env_open(&env, true));
    seed_account(env.scratch.db);
    seed_blob_attachment(env.scratch.db);
    exec_sql(env.scratch.db, "UPDATE active_storage_blobs SET content_type='text/plain'");
    cf_request req;
    cf_response resp;
    prepare_show(&req, NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_logos_destroy_removes_the_attachment) {
    logos_env env;
    CF_REQUIRE(env_open(&env, false));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1);
    seed_blob_attachment(env.scratch.db);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_destroy(&req, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/account/edit\r\n"));
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM active_storage_attachments WHERE "
                       "record_type='Account' AND name='logo'") == 0);
    CF_REQUIRE(env.purge_event_count == 1);
    CF_CHECK(env.purge_events[0].kind == CF_EVENT_PURGE_BLOB);
    CF_CHECK(env.purge_events[0].blob_id == 1);
    cf_writer_stats stats = {0};
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.committed == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_logos_destroy_without_attachment_redirects) {
    logos_env env;
    CF_REQUIRE(env_open(&env, false));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_destroy(&req, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/account/edit\r\n"));
    CF_CHECK(env.purge_event_count == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_logos_destroy_requires_an_administrator) {
    logos_env env;
    CF_REQUIRE(env_open(&env, false));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Member", "member@example.com", 0);
    seed_blob_attachment(env.scratch.db);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session", 1);

    cf_request req;
    cf_response resp;
    prepare_destroy(&req, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM active_storage_attachments WHERE "
                       "record_type='Account' AND name='logo'") == 1);
    CF_CHECK(env.purge_event_count == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_logos_destroy_unauthenticated_redirects_to_sign_in) {
    logos_env env;
    CF_REQUIRE(env_open(&env, false));
    seed_account(env.scratch.db);
    seed_blob_attachment(env.scratch.db);

    cf_request req;
    cf_response resp;
    prepare_destroy(&req, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    CF_CHECK(count_sql(env.scratch.db,
                       "SELECT count(*) FROM active_storage_attachments WHERE "
                       "record_type='Account' AND name='logo'") == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_logos_destroy_without_account_is_500) {
    logos_env env;
    CF_REQUIRE(env_open(&env, false));
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "admin-session", 1);

    cf_request req;
    cf_response resp;
    prepare_destroy(&req, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
