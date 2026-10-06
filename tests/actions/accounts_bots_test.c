/* tests/actions/accounts_bots_test.c — A-accounts-bots acceptance:
 * `accounts/bots#index` (route ID 29), `#create` (30), `#new` (31), `#edit`
 * (32), `#update` (34, 35) and `#destroy` (36), per
 * docs/devel/implementation/contracts/controller-packets.md "A-accounts-bots"
 * and 03-application.md's family boundaries (administrator checks, permit
 * lists, callback order, status/redirect).
 *
 * Cases:
 *   - route binding: ids 29/30/31/32/34/35/36 each reach their action;
 *   - index: lists active bots ordered (name + "id-token"), excludes
 *     deactivated bots and members; 200 text/html;
 *   - new/edit: 200 forms; edit prefills name and webhook URL, 404 for a
 *     member id or a non-integer id;
 *   - create: user + webhook rows, 12-char token, redirect to
 *     <ORIGIN>/account/bots; blank name inserts (exact reference behavior:
 *     neither Rails nor the Rust model validates bot names); missing `user`
 *     is 400; missing name is the reference 500 (Error::internal);
 *   - update: rename + webhook replace/destroy in one transaction,
 *     redirect; unknown/non-integer id is 404 with no effects; a nonscalar
 *     field reads as unchanged; a non-empty avatar scalar is the reference
 *     Invalid arm (500, no partial write); avatar "" deletes the attachment
 *     row and emits the best-effort purge event;
 *   - destroy: deactivates (status + sessions removed), emits mandatory
 *     DISCONNECT_USER(reconnect=false) through the registered control
 *     handler, redirect; unknown id is 404;
 *   - auth: unauthenticated requests redirect to sign-in (302, no effects);
 *     non-admin is 403 before any lookup (unknown bot still 403) with no
 *     effects; missing Sec-Fetch-Site on mutations is 422; JSON-only Accept
 *     on index/new/edit is 406.
 *
 * The route double (tests/app/support/route_double.c) binds the rows above
 * to the real actions, so every case runs the real A00 dispatch path. The
 * writer is started with a capture control handler for the mandatory
 * DISCONNECT_USER barrier.
 *
 * V02 render: the static shims are gone.  The render cases assert the real
 * V-B views' facts and the page layout (status, content type, escaped facts
 * and the Link preload header); the golden cases compare the index/new/edit
 * pages with the golden/a bots_* fixtures the views suite uses (the fixture
 * rows are seeded; the parity SECRET_KEY_BASE reproduces the signed avatar
 * paths and the blob redirect URL), plus a Turbo-Frame structural case.
 *
 * cf.h and src/actions/actions.h are integrator-owned and do not yet declare
 * this packet's symbols, so the entry points are declared here; the
 * integrator's routes.c rebind needs the same declarations (c_symbols
 * cf_action_accounts_bots_index/create/new/edit/update/destroy).
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
#include "models/active_storage.h"
#include "models/user.h"
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

cf_err cf_action_accounts_bots_index(cf_ctx *ctx);
cf_err cf_action_accounts_bots_create(cf_ctx *ctx);
cf_err cf_action_accounts_bots_new(cf_ctx *ctx);
cf_err cf_action_accounts_bots_edit(cf_ctx *ctx);
cf_err cf_action_accounts_bots_update(cf_ctx *ctx);
cf_err cf_action_accounts_bots_destroy(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
/* parity/.env.reference (reference-tools/views/a/golden.sh); the golden
 * fixtures' signed avatar paths and blob redirect URL only reproduce with
 * it. */
#define GOLDEN_SECRET                                                        \
    "5335c3b1ad35b4ad170c3413bd651ef3b6ed64e257261871a6de3f978cf3868ee"     \
    "417a927040935fb30b0f7debdedb34a2a403e9f34b16cf594c917c2ecd4a995"
/* render.rb's CHROME_MAC (the goldens' per-UA cases use it). */
#define CHROME_MAC                                                            \
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "     \
    "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36"
/* tests/fixtures/crates/views/tests/golden/a/facts.json (fixture rows). */
#define FIXTURE_ACCOUNT 873240054
#define FIXTURE_DAVID 127326141
#define FIXTURE_BENDER 394959859
#define FIXTURE_ALL_TALK 486777696
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="
#define BOTS_REDIRECT "Location: " ORIGIN "/account/bots\r\n"
#define BOTS_SIGN_IN "Location: " ORIGIN "/session/new\r\n"

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- fixture-faithful asset root (users_profiles_test.c precedent) --------- */

/* Stage a static root whose manifest is the pinned fixture root and whose
 * importmap-tags.html is the facts' block, so the action renders the layout
 * bytes the goldens were captured with. */
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
    char tmpl[] = "/tmp/campfire-bots-assets-XXXXXX";
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

/* --- scratch app + started writer + capture control handler ---------------- */

#define BOTS_MAX_EVENTS 16

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
    cf_event control_events[BOTS_MAX_EVENTS];
    size_t control_event_count;
} bots_env;

static cf_err bots_capture_control(void *ctx, const cf_event *event) {
    bots_env *env = ctx;
    if (env->control_event_count < BOTS_MAX_EVENTS) {
        env->control_events[env->control_event_count++] = *event;
    }
    return CF_OK;
}

static bool env_open(bots_env *env) {
    if (g_assets_root[0] == '\0' && !stage_assets_root()) return false;
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;
    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", ORIGIN},
        {"SECRET_KEY_BASE", GOLDEN_SECRET},
        {"VAPID_PUBLIC_KEY", VAPID_PUBLIC_KEY},
        {"DATABASE_PATH", env->scratch.path},
    };
    if (cf_config_parse(entries, 4, NULL, &env->config) != CF_OK) return false;
    if (cf_app_create(env->config, &env->app) != CF_OK) {
        cf_config_destroy(env->config);
        env->config = NULL;
        return false;
    }
    /* The staged root renders the layout with the fixtures' assets (the real
     * views always render images; an unconfigured root fails the render). */
    if (cf_views_assets_configure(g_assets_root) != CF_OK) {
        fprintf(stderr, "  env_open: assets configure failed\n");
        return false;
    }
    if (cf_writer_start(env->app, env->config) != CF_OK) return false;
    env->writer_started = true;
    if (cf_writer_set_control_handler(env->app, bots_capture_control, env) !=
        CF_OK) {
        return false;
    }
    cf_test_routes_reset();
    /* routes.json ids 29-32/34-36. The double's `(.:format)` grammar needs
     * an extension for a literal last segment, so the bare form is
     * registered as well (the same note as the sessions/bans tests). */
    if (cf_test_routes_add("GET", "/account/bots(.:format)", 29,
                           cf_action_accounts_bots_index) != CF_OK ||
        cf_test_routes_add("GET", "/account/bots", 29,
                           cf_action_accounts_bots_index) != CF_OK ||
        cf_test_routes_add("POST", "/account/bots(.:format)", 30,
                           cf_action_accounts_bots_create) != CF_OK ||
        cf_test_routes_add("POST", "/account/bots", 30,
                           cf_action_accounts_bots_create) != CF_OK ||
        cf_test_routes_add("GET", "/account/bots/new(.:format)", 31,
                           cf_action_accounts_bots_new) != CF_OK ||
        cf_test_routes_add("GET", "/account/bots/new", 31,
                           cf_action_accounts_bots_new) != CF_OK ||
        cf_test_routes_add("GET", "/account/bots/:id/edit(.:format)", 32,
                           cf_action_accounts_bots_edit) != CF_OK ||
        cf_test_routes_add("GET", "/account/bots/:id/edit", 32,
                           cf_action_accounts_bots_edit) != CF_OK ||
        cf_test_routes_add("PATCH", "/account/bots/:id(.:format)", 34,
                           cf_action_accounts_bots_update) != CF_OK ||
        cf_test_routes_add("PATCH", "/account/bots/:id", 34,
                           cf_action_accounts_bots_update) != CF_OK ||
        cf_test_routes_add("PUT", "/account/bots/:id(.:format)", 35,
                           cf_action_accounts_bots_update) != CF_OK ||
        cf_test_routes_add("PUT", "/account/bots/:id", 35,
                           cf_action_accounts_bots_update) != CF_OK ||
        cf_test_routes_add("DELETE", "/account/bots/:id(.:format)", 36,
                           cf_action_accounts_bots_destroy) != CF_OK ||
        cf_test_routes_add("DELETE", "/account/bots/:id", 36,
                           cf_action_accounts_bots_destroy) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(bots_env *env) {
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
                      const char *token, int role, int status) {
    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, updated_at) "
             "VALUES (%lld, NULL, %s%s%s, '2026-01-02 03:04:05', NULL, '%s', "
             "NULL, %d, %d, '2026-01-02 03:04:05')",
             (long long)id, token != NULL ? "'" : "",
             token != NULL ? token : "NULL", token != NULL ? "'" : "", name,
             role, status);
    exec_sql(db, sql);
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

static void seed_webhook(cf_db *db, int64_t user_id, const char *url) {
    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO webhooks (created_at, updated_at, url, user_id) "
             "VALUES ('2026-01-02 03:04:05', '2026-01-02 03:04:05', '%s', "
             "%lld)",
             url, (long long)user_id);
    exec_sql(db, sql);
}

static void seed_avatar(cf_db *db, int64_t bot_id) {
    exec_sql(db,
             "INSERT INTO active_storage_blobs (byte_size, checksum, "
             "content_type, created_at, filename, key, metadata, "
             "service_name) VALUES (8, NULL, 'image/png', "
             "'2026-01-02 03:04:05', 'a.png', 'bots-avatar-1', NULL, "
             "'local')");
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO active_storage_attachments (blob_id, created_at, "
             "name, record_id, record_type) VALUES (1, "
             "'2026-01-02 03:04:05', 'avatar', %lld, 'User')",
             (long long)bot_id);
    exec_sql(db, sql);
}

/* --- golden fixture rows (facts.json / the reference fixtures) ------------- */

static void seed_fixture_account(cf_db *db, const char *updated_at,
                                 const char *custom_styles) {
    char sql[768];
    snprintf(sql, sizeof sql,
             "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
             "name, settings, singleton_guard, updated_at) VALUES "
             "(%d, '2026-09-26 13:00:20.000000', %s, 'CRMu-l8Ge-KB9B', "
             "'37signals', NULL, 0, '%s')",
             FIXTURE_ACCOUNT, custom_styles != NULL ? "?" : "NULL",
             updated_at);
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) == SQLITE_OK);
    if (custom_styles != NULL) {
        sqlite3_bind_text(stmt, 1, custom_styles, -1, SQLITE_TRANSIENT);
    }
    int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    CF_REQUIRE(step == SQLITE_DONE);
}

static void seed_fixture_user(cf_db *db, int64_t id, const char *name,
                              const char *token, int role, int status) {
    char sql[768];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, updated_at) "
             "VALUES (%lld, NULL, %s%s%s, '2026-09-26 13:00:10.000000', "
             "NULL, '%s', NULL, %d, %d, '2026-09-26 13:00:20.000000')",
             (long long)id, token != NULL ? "'" : "",
             token != NULL ? token : "NULL", token != NULL ? "'" : "", name,
             role, status);
    exec_sql(db, sql);
}

static void seed_fixture_room(cf_db *db, int64_t id, const char *name,
                              const char *type, int64_t creator_id) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (%lld, '2026-09-26 12:00:00.000000', %lld, "
             "'%s', '%s', '2026-09-26 13:00:20.000000')",
             (long long)id, (long long)creator_id, name, type);
    exec_sql(db, sql);
}

static void seed_fixture_membership(cf_db *db, int64_t id, int64_t room_id,
                                    int64_t user_id, const char *involvement) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO memberships (id, created_at, involvement, room_id, "
             "unread_at, updated_at, user_id) VALUES (%lld, '2026-09-26 "
             "12:00:00.000000', '%s', %lld, NULL, "
             "'2026-09-26 12:00:00.000000', %lld)",
             (long long)id, involvement, (long long)room_id,
             (long long)user_id);
    exec_sql(db, sql);
}

/* The facts' Bender avatar: blob 2, bender.png (the golden's blob redirect
 * URL is signed for exactly these rows). */
static void seed_fixture_bender_avatar(cf_db *db) {
    exec_sql(db,
             "INSERT INTO active_storage_blobs (id, byte_size, checksum, "
             "content_type, created_at, filename, key, metadata, "
             "service_name) VALUES (2, 8, NULL, 'image/png', "
             "'2026-09-26 13:00:20.000000', 'bender.png', 'bender-avatar', "
             "NULL, 'local')");
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO active_storage_attachments (id, blob_id, created_at, "
             "name, record_id, record_type) VALUES (2, 2, "
             "'2026-09-26 13:00:20.000000', 'avatar', %d, 'User')",
             FIXTURE_BENDER);
    exec_sql(db, sql);
}

static void seed_fixture_account_logo(cf_db *db) {
    exec_sql(db,
             "INSERT INTO active_storage_blobs (id, byte_size, checksum, "
             "content_type, created_at, filename, key, metadata, "
             "service_name) VALUES (3, 8, NULL, 'image/png', "
             "'2026-09-26 13:00:29.000000', 'logo.png', 'account-logo', NULL, "
             "'local')");
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO active_storage_attachments (id, blob_id, created_at, "
             "name, record_id, record_type) VALUES (3, 3, "
             "'2026-09-26 13:00:29.000000', 'logo', %d, 'Account')",
             FIXTURE_ACCOUNT);
    exec_sql(db, sql);
}

/* The facts.json rows the bots goldens render: the account, David (viewer),
 * Bender Bot with his webhook and All Talk membership. */
static void seed_bots_fixture(cf_db *db) {
    seed_fixture_account(db, "2026-09-26 13:00:20.000000", NULL);
    seed_fixture_user(db, FIXTURE_DAVID, "David", NULL, 1, 0);
    seed_fixture_user(db, FIXTURE_BENDER, "Bender Bot", "e0LbMoZhDhOs", 2, 0);
    seed_fixture_room(db, FIXTURE_ALL_TALK, "All Talk", "Rooms::Closed",
                      FIXTURE_BENDER);
    seed_fixture_membership(db, 900001, FIXTURE_ALL_TALK, FIXTURE_BENDER,
                            "mentions");
    seed_webhook(db, FIXTURE_BENDER, "https://example.com/webhook?a=1&b=2");
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

static bool run_request(bots_env *env, cf_request *req, cf_response *resp) {
    cf_response_init(resp);
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static bool span_contains(cf_span haystack, const char *needle) {
    size_t len = strlen(needle);
    if (len == 0) return true;
    if (haystack.len < len || haystack.ptr == NULL) return false;
    for (size_t i = 0; i + len <= haystack.len; i++) {
        if (memcmp(haystack.ptr + i, needle, len) == 0) return true;
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

static bool body_contains(const cf_response *resp, const char *needle) {
    return buf_contains(resp->body, needle);
}

/* The golden fixture request: the caller-owned session cookie plus
 * render.rb's CHROME_MAC. */
static void fixture_get(cf_request *req, const char *path, const char *cookie) {
    cf_test_req_init(req);
    req->method = CF_GET;
    req->original_method = CF_GET;
    req->path = SP(path);
    req->target = SP(path);
    CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    CF_REQUIRE(cf_test_req_header(req, SP("User-Agent"), SP(CHROME_MAC)) ==
               CF_OK);
}

/* The fixture viewer's session, built into the caller's buffer. */
static void fixture_cookie(bots_env *env, char *out, size_t cap,
                           const char *token, int64_t viewer_id) {
    make_session_cookie(env->config, env->scratch.db, out, cap, token,
                        viewer_id);
}

static void req_get(cf_request *req, const char *path, const char *cookie,
                    const char *accept) {
    cf_test_req_init(req);
    req->method = CF_GET;
    req->original_method = CF_GET;
    req->path = SP(path);
    req->target = SP(path);
    if (accept != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Accept"), SP(accept)) == CF_OK);
    }
    if (cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    }
}

static void req_mut(cf_request *req, cf_method method, const char *path,
                    const char *body, const char *cookie,
                    const char *fetch_site) {
    cf_test_req_init(req);
    req->method = method;
    req->original_method = method;
    req->path = SP(path);
    req->target = SP(path);
    req->body = SP(body);
    CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                  SP("application/x-www-form-urlencoded")) ==
               CF_OK);
    /* The reference verifies by Sec-Fetch-Site, not tokens: "same-origin"
     * passes; "cross-site" is the forged request (422). A NULL fetch_site
     * omits the header, which plain-HTTP requests allow. */
    if (fetch_site != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                      SP(fetch_site)) == CF_OK);
    }
    if (cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    }
}

/* --- acceptance: route binding ---------------------------------------------- */

/* Every assigned route ID reaches its action through the real dispatch path:
 * 29 index (200 list), 30 create (302), 31 new (200 form), 32 edit (200
 * form), 34/35 update (302), 36 destroy (302). */
CF_TEST(accounts_bots_route_ids_bind_their_actions) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Route", "routetoken12", 2, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;

    req_get(&req, "/account/bots", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "Chat bots"));
    CF_CHECK(body_contains(&resp, "Route"));
    cf_response_dispose(&resp);

    req_mut(&req, CF_POST, "/account/bots", "user[name]=Posted", admin_cookie,
            "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_REDIRECT));
    cf_response_dispose(&resp);

    req_get(&req, "/account/bots/new", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "New chat bot"));
    cf_response_dispose(&resp);

    req_get(&req, "/account/bots/7/edit", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "Edit bot"));
    cf_response_dispose(&resp);

    req_mut(&req, CF_PATCH, "/account/bots/7", "user[name]=Patched",
            admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_REDIRECT));
    cf_response_dispose(&resp);

    req_mut(&req, CF_PUT, "/account/bots/7", "user[name]=Putted",
            admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_REDIRECT));
    cf_response_dispose(&resp);

    req_mut(&req, CF_DELETE, "/account/bots/7", "", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_REDIRECT));
    cf_response_dispose(&resp);

    env_close(&env);
}

/* --- acceptance: index -------------------------------------------------------- */

CF_TEST(accounts_bots_index_lists_active_bots_ordered) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Zulu", "zulutoken123", 2, 0);
    seed_user(env.scratch.db, 8, "alpha", "alphatoken12", 2, 0);
    seed_user(env.scratch.db, 9, "Gone", "gonetoken123", 2, 1);
    seed_user(env.scratch.db, 10, "Member", NULL, 0, 0);
    /* alpha's and Zulu's non-direct room renders the curl URL with their
     * "id-token" keys. */
    seed_fixture_room(env.scratch.db, 100, "All Talk", "Rooms::Open", 1);
    seed_fixture_membership(env.scratch.db, 1000, 100, 8, "everything");
    seed_fixture_membership(env.scratch.db, 1001, 100, 7, "everything");
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    /* ORDER BY LOWER(name): alpha before Zulu; the "id-token" key renders. */
    CF_CHECK(body_contains(&resp, "alpha"));
    CF_CHECK(body_contains(&resp, "Zulu"));
    CF_CHECK(body_contains(&resp, "8-alphatoken12"));
    CF_CHECK(body_contains(&resp, "7-zulutoken123"));
    CF_CHECK(!body_contains(&resp, "Gone"));
    CF_CHECK(!body_contains(&resp, "Member"));
    cf_span body = cf_buf_span(resp.body);
    /* Ordered: alpha's row precedes Zulu's row. */
    const char *text = (const char *)body.ptr;
    size_t alpha_at = body.len, zulu_at = body.len;
    for (size_t i = 0; i + 5 <= body.len; i++) {
        if (memcmp(text + i, "alpha", 5) == 0 && alpha_at == body.len) {
            alpha_at = i;
        }
        if (memcmp(text + i, "Zulu", 4) == 0 && zulu_at == body.len) {
            zulu_at = i;
        }
    }
    CF_CHECK(alpha_at < zulu_at);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_bots_index_escapes_names) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);
    /* A name with markup must render escaped (the _bot partial's text). */
    exec_sql(env.scratch.db,
             "INSERT INTO users (bio, bot_token, created_at, email_address, "
             "name, password_digest, role, status, updated_at) VALUES (NULL, "
             "'esctoken1234', '2026-01-02 03:04:05', NULL, '<b>Bold</b>', "
             "NULL, 2, 0, '2026-01-02 03:04:05')");

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(!body_contains(&resp, "<b>Bold</b>"));
    CF_CHECK(body_contains(&resp, "&lt;b&gt;Bold&lt;/b&gt;"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_bots_index_unauthenticated_redirects_to_sign_in) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots", NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_SIGN_IN));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_bots_index_forbidden_for_non_admin) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 2, "Member", NULL, 0, 0);
    char member_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, member_cookie,
                        sizeof member_cookie, "tok-member", 2);

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots", member_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_bots_index_rejects_json_format) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots", admin_cookie, "application/json");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- acceptance: new ---------------------------------------------------------- */

CF_TEST(accounts_bots_new_renders_form) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots/new", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "user[name]"));
    CF_CHECK(body_contains(&resp, "user[webhook_url]"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_bots_new_unauthenticated_redirects_to_sign_in) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots/new", NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_SIGN_IN));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- acceptance: create ------------------------------------------------------- */

CF_TEST(accounts_bots_create_bot_with_webhook) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_POST, "/account/bots",
            "user[name]=Helper&user[webhook_url]=https://hooks.example/bot",
            admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_REDIRECT));
    CF_CHECK(resp.body == NULL);
    cf_response_dispose(&resp);

    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE role = 2 AND "
                        "status = 0 AND name = 'Helper'") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT LENGTH(bot_token) FROM users WHERE name = "
                        "'Helper'") == 12);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM webhooks WHERE url = "
                        "'https://hooks.example/bot'") == 1);
    env_close(&env);
}

CF_TEST(accounts_bots_create_bot_without_webhook) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_POST, "/account/bots", "user[name]=Silent",
            admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_REDIRECT));
    cf_response_dispose(&resp);

    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE role = 2 AND name "
                        "= 'Silent'") == 1);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM webhooks") ==
             0);
    env_close(&env);
}

/* Exact reference behavior: neither the Rails model nor the Rust/C models
 * validate bot names, so a blank name inserts and redirects. */
CF_TEST(accounts_bots_create_blank_name_inserts) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_POST, "/account/bots", "user[name]=", admin_cookie,
            "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE role = 2 AND name "
                        "= ''") == 1);
    env_close(&env);
}

/* `params.require(:user)`: a missing `user` root is ParameterMissing (400)
 * with no row effects. */
CF_TEST(accounts_bots_create_missing_user_param_is_400) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_POST, "/account/bots", "name=Nope", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 400);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id != 1") == 0);
    env_close(&env);
}

/* `name.ok_or(Error::internal)`: a missing name key is the reference 500,
 * never a silent blank insert, and writes nothing. */
CF_TEST(accounts_bots_create_missing_name_is_500) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_POST, "/account/bots",
            "user[webhook_url]=https://hooks.example/x", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id != 1") == 0);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM webhooks") ==
             0);
    env_close(&env);
}

CF_TEST(accounts_bots_create_forbidden_for_non_admin) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 2, "Member", NULL, 0, 0);
    char member_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, member_cookie,
                        sizeof member_cookie, "tok-member", 2);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_POST, "/account/bots", "user[name]=Nope", member_cookie,
            "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id NOT IN (1,2)") ==
             0);
    env_close(&env);
}

CF_TEST(accounts_bots_create_requires_csrf_and_auth) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    /* Forged cross-site POST: the chain's CSRF check answers 422, writing
     * nothing. */
    req_mut(&req, CF_POST, "/account/bots", "user[name]=Nope", admin_cookie,
            "cross-site");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id != 1") == 0);

    /* No session at all: the chain redirects to sign-in, writing nothing. */
    req_mut(&req, CF_POST, "/account/bots", "user[name]=Nope", NULL, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_SIGN_IN));
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id != 1") == 0);
    env_close(&env);
}

/* --- acceptance: edit --------------------------------------------------------- */

CF_TEST(accounts_bots_edit_shows_prefilled_form) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Helper", "helpertoken1", 2, 0);
    seed_webhook(env.scratch.db, 7, "https://hooks.example/bot");
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots/7/edit", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "value=\"Helper\""));
    CF_CHECK(body_contains(&resp, "value=\"https://hooks.example/bot\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* `set_bot` scopes to active bots: a member id and a non-integer id are
 * NotFound (404), even for an administrator. */
CF_TEST(accounts_bots_edit_not_found_cases) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 2, "Member", NULL, 0, 0);
    seed_user(env.scratch.db, 9, "Gone", "gonetoken123", 2, 1);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots/2/edit", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    req_get(&req, "/account/bots/9/edit", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    req_get(&req, "/account/bots/abc/edit", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    req_get(&req, "/account/bots/999/edit", admin_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Admin gating precedes the lookup: a non-admin gets 403 even for an
 * unknown bot. */
CF_TEST(accounts_bots_edit_forbidden_before_lookup) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 2, "Member", NULL, 0, 0);
    char member_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, member_cookie,
                        sizeof member_cookie, "tok-member", 2);

    cf_request req;
    cf_response resp;
    req_get(&req, "/account/bots/999/edit", member_cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- acceptance: update ------------------------------------------------------- */

CF_TEST(accounts_bots_update_renames_and_replaces_webhook) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Old", "oldtoken1234", 2, 0);
    seed_webhook(env.scratch.db, 7, "https://old.example/hook");
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_PATCH, "/account/bots/7",
            "user[name]=New&user[webhook_url]=https://new.example/hook",
            admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_REDIRECT));
    cf_response_dispose(&resp);

    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id = 7 AND name = "
                        "'New' AND role = 2 AND status = 0") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM webhooks WHERE user_id = 7 AND "
                        "url = 'https://new.example/hook'") == 1);
    /* The token is untouched by an ordinary update. */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id = 7 AND "
                        "bot_token = 'oldtoken1234'") == 1);
    env_close(&env);
}

/* A blank webhook_url destroys the row (`filter(!blank)` in update_bot). */
CF_TEST(accounts_bots_update_blank_webhook_destroys_it) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Helper", "helpertoken1", 2, 0);
    seed_webhook(env.scratch.db, 7, "https://old.example/hook");
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_PUT, "/account/bots/7",
            "user[name]=Helper&user[webhook_url]=", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM webhooks WHERE user_id = 7") ==
             0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id = 7 AND name = "
                        "'Helper'") == 1);
    env_close(&env);
}

/* Unknown, deactivated, non-bot and non-integer ids are 404 with no effects
 * (PUT exercises the 35 alias here). */
CF_TEST(accounts_bots_update_not_found_cases) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 2, "Member", NULL, 0, 0);
    seed_user(env.scratch.db, 7, "Helper", "helpertoken1", 2, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    const char *paths[3] = {"/account/bots/999", "/account/bots/2",
                            "/account/bots/abc"};
    for (size_t i = 0; i < 3; i++) {
        cf_request req;
        cf_response resp;
        req_mut(&req, CF_PUT, paths[i], "user[name]=Nope", admin_cookie,
                "same-origin");
        CF_REQUIRE(run_request(&env, &req, &resp));
        CF_CHECK(resp.status == 404);
        cf_response_dispose(&resp);
    }
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE name = 'Nope'") ==
             0);
    env_close(&env);
}

/* A nonscalar field is the reference None (unchanged), not an error. */
CF_TEST(accounts_bots_update_nonscalar_field_is_unchanged) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Helper", "helpertoken1", 2, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_PATCH, "/account/bots/7",
            "user[name][x]=1&user[webhook_url]=https://hooks.example/kept",
            admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id = 7 AND name = "
                        "'Helper'") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM webhooks WHERE user_id = 7 AND "
                        "url = 'https://hooks.example/kept'") == 1);
    env_close(&env);
}

/* The Invalid avatar arm: the reference 500 with no partial write. */
CF_TEST(accounts_bots_update_invalid_avatar_is_500) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Helper", "helpertoken1", 2, 0);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_PATCH, "/account/bots/7",
            "user[name]=Changed&user[avatar]=not-a-file", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id = 7 AND name = "
                        "'Helper'") == 1);
    env_close(&env);
}

/* The Delete avatar arm: an existing User/avatar attachment row is removed
 * (the blob row itself is purged asynchronously, so it stays). */
CF_TEST(accounts_bots_update_avatar_delete_removes_attachment) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Helper", "helpertoken1", 2, 0);
    seed_avatar(env.scratch.db, 7);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);
    CF_REQUIRE(count_rows(env.scratch.db,
                          "SELECT count(*) FROM active_storage_attachments "
                          "WHERE record_type = 'User' AND record_id = 7 AND "
                          "name = 'avatar'") == 1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_PATCH, "/account/bots/7",
            "user[name]=Helper&user[avatar]=", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_attachments "
                        "WHERE record_type = 'User' AND record_id = 7 AND "
                        "name = 'avatar'") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_blobs WHERE id "
                        "= 1") == 1);
    env_close(&env);
}

CF_TEST(accounts_bots_update_auth_cases) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 2, "Member", NULL, 0, 0);
    seed_user(env.scratch.db, 7, "Helper", "helpertoken1", 2, 0);
    char admin_cookie[1024];
    char member_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);
    make_session_cookie(env.config, env.scratch.db, member_cookie,
                        sizeof member_cookie, "tok-member", 2);

    cf_request req;
    cf_response resp;
    /* Unauthenticated: redirect, no change. */
    req_mut(&req, CF_PATCH, "/account/bots/7", "user[name]=Nope", NULL,
            "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_SIGN_IN));
    cf_response_dispose(&resp);
    /* Non-admin: 403 even though the bot exists, no change. */
    req_mut(&req, CF_PATCH, "/account/bots/7", "user[name]=Nope",
            member_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    /* Forged cross-site request: 422, no change. */
    req_mut(&req, CF_PATCH, "/account/bots/7", "user[name]=Nope",
            admin_cookie, "cross-site");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    cf_response_dispose(&resp);

    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id = 7 AND name = "
                        "'Helper'") == 1);
    env_close(&env);
}

/* --- acceptance: destroy ------------------------------------------------------ */

CF_TEST(accounts_bots_destroy_deactivates) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 7, "Helper", "helpertoken1", 2, 0);
    seed_session(env.scratch.db, "tok-bot", 7);
    char admin_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);
    CF_REQUIRE(count_rows(env.scratch.db,
                          "SELECT count(*) FROM sessions WHERE user_id = 7") ==
               1);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_DELETE, "/account/bots/7", "", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_REDIRECT));
    cf_response_dispose(&resp);

    /* Deactivated, sessions removed, row kept (email was NULL). */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id = 7 AND status "
                        "= 1 AND email_address IS NULL") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM sessions WHERE user_id = 7") ==
             0);
    /* The mandatory barrier ran: exactly one DISCONNECT_USER, no reconnect. */
    CF_REQUIRE(env.control_event_count == 1);
    CF_CHECK(env.control_events[0].kind == CF_EVENT_DISCONNECT_USER);
    CF_CHECK(env.control_events[0].user_id == 7);
    CF_CHECK(!env.control_events[0].reconnect);
    env_close(&env);
}

CF_TEST(accounts_bots_destroy_not_found_and_auth) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Admin", NULL, 1, 0);
    seed_user(env.scratch.db, 2, "Member", NULL, 0, 0);
    char admin_cookie[1024];
    char member_cookie[1024];
    make_session_cookie(env.config, env.scratch.db, admin_cookie,
                        sizeof admin_cookie, "tok-admin", 1);
    make_session_cookie(env.config, env.scratch.db, member_cookie,
                        sizeof member_cookie, "tok-member", 2);

    cf_request req;
    cf_response resp;
    req_mut(&req, CF_DELETE, "/account/bots/999", "", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    req_mut(&req, CF_DELETE, "/account/bots/abc", "", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    req_mut(&req, CF_DELETE, "/account/bots/2", "", admin_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM users WHERE id = 2 AND status "
                        "= 0") == 1);

    req_mut(&req, CF_DELETE, "/account/bots/999", "", member_cookie, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);

    req_mut(&req, CF_DELETE, "/account/bots/999", "", NULL, "same-origin");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, BOTS_SIGN_IN));
    cf_response_dispose(&resp);

    CF_CHECK(env.control_event_count == 0);
    env_close(&env);
}

/* --- V02 golden renders ----------------------------------------------------- */

/* The index page matches the golden/a bots_index fixture token-for-token:
 * the fixture rows seeded, the parity SECRET_KEY_BASE for the signed avatar
 * path, the blob-less default bot avatar. */
CF_TEST(accounts_bots_index_matches_the_golden) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_bots_fixture(env.scratch.db);

    char cookie[4096];
    fixture_cookie(&env, cookie, sizeof cookie, "david-session",
                   FIXTURE_DAVID);
    cf_request req;
    cf_response resp;
    fixture_get(&req, "/account/bots", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_REQUIRE(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    /* The page layout carries the stylesheet preload links. */
    CF_CHECK(head_contains(&resp, &req, "Link: <"));
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("bots_index", (const char *)body.ptr, body.len);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_bots_new_matches_the_golden) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_bots_fixture(env.scratch.db);

    char cookie[4096];
    fixture_cookie(&env, cookie, sizeof cookie, "david-session",
                   FIXTURE_DAVID);
    cf_request req;
    cf_response resp;
    fixture_get(&req, "/account/bots/new", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_REQUIRE(resp.status == 200);
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("bots_new", (const char *)body.ptr, body.len);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(accounts_bots_edit_matches_the_golden) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_bots_fixture(env.scratch.db);

    char cookie[4096];
    fixture_cookie(&env, cookie, sizeof cookie, "david-session",
                   FIXTURE_DAVID);
    cf_request req;
    cf_response resp;
    fixture_get(&req, "/account/bots/394959859/edit", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_REQUIRE(resp.status == 200);
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("bots_edit", (const char *)body.ptr, body.len);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* The avatar variant: an attached avatar renders the blob redirect URL and
 * the account-has-logo/custom-styles layout facts (the same late capture
 * section as profile_with_avatar). */
CF_TEST(accounts_bots_edit_with_avatar_matches_the_golden) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_bots_fixture(env.scratch.db);
    seed_fixture_bender_avatar(env.scratch.db);
    seed_fixture_account_logo(env.scratch.db);
    exec_sql(env.scratch.db,
             "UPDATE accounts SET updated_at = '2026-09-26 13:00:29.000000', "
             "custom_styles = 'body { --x: 1; } a > b { color: red }'");

    char cookie[4096];
    fixture_cookie(&env, cookie, sizeof cookie, "david-session",
                   FIXTURE_DAVID);
    cf_request req;
    cf_response resp;
    fixture_get(&req, "/account/bots/394959859/edit", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_REQUIRE(resp.status == 200);
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("bots_edit_with_avatar", (const char *)body.ptr,
                     body.len);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A Turbo-Frame request renders turbo-rails' frame layout (head + content,
 * no title/nav) and carries no Link preload header. */
CF_TEST(accounts_bots_index_turbo_frame_renders_the_frame_layout) {
    bots_env env;
    CF_REQUIRE(env_open(&env));
    seed_bots_fixture(env.scratch.db);

    char cookie[4096];
    fixture_cookie(&env, cookie, sizeof cookie, "david-session",
                   FIXTURE_DAVID);
    cf_request req;
    cf_response resp;
    fixture_get(&req, "/account/bots", cookie);
    CF_REQUIRE(cf_test_req_header(&req, SP("Turbo-Frame"), SP("account_bots")) ==
               CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_REQUIRE(resp.status == 200);
    CF_CHECK(!head_contains(&resp, &req, "Link: <"));
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    CF_CHECK(span_contains(body, "Chat bots"));
    CF_CHECK(span_contains(body, "Bender Bot"));
    CF_CHECK(span_contains(body, "394959859-e0LbMoZhDhOs"));
    CF_CHECK(!span_contains(body, "<!DOCTYPE html>"));
    CF_CHECK(!span_contains(body, "<title>"));
    CF_CHECK(!span_contains(body, "<nav id=\"nav\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
