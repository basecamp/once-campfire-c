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
 * V02 render: the Edit gate is gone.  The page renders through
 * cf_presenter_accounts_edit and is compared with the golden/a
 * account_edit_member/account_edit_admin fixtures the views suite uses (the
 * fixture rows are seeded here; the parity SECRET_KEY_BASE and the capture
 * instants in the rows reproduce the signed avatar paths), plus a Turbo-Frame
 * structural case and a direct presenter case (partition order, title, avatar
 * path, last-room link, next page).
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
#include "models/account.h"
#include "models/active_storage.h"
#include "models/user.h"
#include "presenters/accounts.h"
#include "views.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"
#include "avatar_test.h"
#include "auth_write_race.h"
#include "../views/support/golden.h"

#include <errno.h>
#include <inttypes.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "yyjson.h"

cf_err cf_action_accounts_edit(cf_ctx *ctx);
cf_err cf_action_accounts_update(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
/* parity/.env.reference (reference-tools/views/a/golden.sh); the golden
 * fixtures' signed avatar paths only reproduce with it. */
#define GOLDEN_SECRET                                                        \
    "5335c3b1ad35b4ad170c3413bd651ef3b6ed64e257261871a6de3f978cf3868ee"     \
    "417a927040935fb30b0f7debdedb34a2a403e9f34b16cf594c917c2ecd4a995"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="
/* render.rb's CHROME_MAC (the goldens' per-UA cases use it). */
#define CHROME_MAC                                                            \
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "     \
    "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36"

/* tests/fixtures/crates/views/tests/golden/a/facts.json (fixture rows). */
#define ACCOUNT_ID 873240054
#define DAVID 127326141
#define JASON 149087659
#define BENDER 394959859
#define KEVIN 712064548
#define JZ 773523953
#define EX_EMPLOYEE 773523954
#define SPAM_HAM 773523955
#define ANNA 773523956
#define DESIGNERS 654632876
#define DIRECT_KEVIN 699448325

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
    char tmpl[] = "/tmp/campfire-accounts-assets-XXXXXX";
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

/* --- golden fixture rows (facts.json / the reference fixtures) ------------- */

static void seed_fixture_account(cf_db *db) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
             "name, settings, singleton_guard, updated_at) VALUES "
             "(%d, '2026-09-26 13:00:20.000000', NULL, 'CRMu-l8Ge-KB9B', "
             "'37signals', NULL, 0, '2026-09-26 13:00:20.000000')",
             ACCOUNT_ID);
    exec_sql(db, sql);
}

static void seed_fixture_user(cf_db *db, int64_t id, const char *name,
                              const char *email, const char *bio, int role,
                              int status, const char *updated_at) {
    char sql[768];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, created_at, email_address, name, "
             "role, status, updated_at) VALUES (%lld, %s, "
             "'2026-09-26 13:00:10.000000', %s, '%s', %d, %d, '%s')",
             (long long)id, bio != NULL ? "?" : "NULL",
             email != NULL ? "?" : "NULL", name, role, status, updated_at);
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) == SQLITE_OK);
    int bind = 1;
    if (bio != NULL) sqlite3_bind_text(stmt, bind++, bio, -1, SQLITE_TRANSIENT);
    if (email != NULL) {
        sqlite3_bind_text(stmt, bind++, email, -1, SQLITE_TRANSIENT);
    }
    int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    CF_REQUIRE(step == SQLITE_DONE);
}

static void seed_fixture_room(cf_db *db, int64_t id, const char *name,
                              const char *type, int64_t creator_id) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (%lld, '2026-09-26 12:00:00.000000', %lld, "
             "%s, '%s', '2026-09-26 13:00:20.000000')",
             (long long)id, (long long)creator_id,
             name != NULL ? "?" : "NULL", type);
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) == SQLITE_OK);
    if (name != NULL) sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    CF_REQUIRE(step == SQLITE_DONE);
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

/* The facts.json rows the Edit goldens render: the account, every user (the
 * roster lists all non-bot active users, plus banned users for an
 * administrator) and the viewing user's original room. */
static void seed_edit_fixture(cf_db *db) {
    seed_fixture_account(db);
    seed_fixture_user(db, DAVID, "David", "david@37signals.com", NULL, 1, 0,
                      "2026-09-26 13:00:20.000000");
    seed_fixture_user(db, JASON, "Jason", "jason@37signals.com", NULL, 1, 0,
                      "2026-09-26 13:00:20.000000");
    seed_fixture_user(db, BENDER, "Bender Bot", NULL, NULL, 2, 0,
                      "2026-09-26 13:00:20.000000");
    seed_fixture_user(db, KEVIN, "Kevin", "kevin@37signals.com", "Programmer",
                      0, 0, "2026-09-26 13:00:20.000000");
    seed_fixture_user(db, JZ, "JZ", "jz@37signals.com", "Designer", 0, 0,
                      "2026-09-26 13:00:20.000000");
    seed_fixture_user(db, EX_EMPLOYEE, "Ex Employee",
                      "ex-deactivated-e892fe70-ae77-4cee-9fc0-0b7c37f5b417"
                      "@37signals.com",
                      NULL, 0, 1, "2026-09-26 13:00:20.000000");
    seed_fixture_user(db, SPAM_HAM, "Spam Ham", "spam@example.com", NULL, 0,
                      2, "2026-09-26 13:00:21.000000");
    seed_fixture_user(db, ANNA, "Anna Bea Cole", "abc@example.com", NULL, 0,
                      0, "2026-09-26 13:00:22.000000");
}

/* The viewer's original room: Kevin (member) -> direct-699448325, David
 * (administrator) -> Designers. */
static void seed_edit_viewer_room(cf_db *db, int64_t viewer_id) {
    if (viewer_id == KEVIN) {
        seed_fixture_room(db, DIRECT_KEVIN, NULL, "Rooms::Direct", KEVIN);
        seed_fixture_membership(db, 900001, DIRECT_KEVIN, KEVIN, "everything");
        return;
    }
    seed_fixture_room(db, DESIGNERS, "Designers", "Rooms::Closed", DAVID);
    seed_fixture_membership(db, 900010, DESIGNERS, DAVID, "mentions");
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

/* The golden fixture request: the caller-owned session cookie plus
 * render.rb's CHROME_MAC. */
static void fixture_get(cf_request *req, const char *path, const char *cookie) {
    cf_test_req_init(req);
    req->path = SP(path);
    req->target = SP(path);
    CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    CF_REQUIRE(cf_test_req_header(req, SP("User-Agent"), SP(CHROME_MAC)) ==
               CF_OK);
}

/* The fixture viewer's session, built into the caller's buffer. */
static void fixture_cookie(accounts_env *env, char *out, size_t cap,
                           const char *token, int64_t viewer_id) {
    make_session_cookie(env->config, env->scratch.db, out, cap, token,
                        viewer_id);
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

/* The real accounts::Edit render: the member branch shows the account name
 * and invite, the roster partitions administrators first (LOWER(name) order
 * within each side). */
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
    CF_CHECK(body_contains(&resp, "/join/ABCD-EFGH-IJKL"));
    CF_CHECK(body_contains(&resp, "id=\"account_users\""));
    CF_CHECK(body_contains(&resp, "Ada Admin"));
    CF_CHECK(body_contains(&resp, "Bob Member"));
    /* Administrators render first, then members (search the roster only:
     * the viewer's name appears earlier in the head meta tags). */
    {
        cf_span whole = cf_buf_span(resp.body);
        cf_span body = whole;
        const char *frame_at = "id=\"account_users\"";
        for (size_t i = 0; i + 18 <= whole.len; i++) {
            if (memcmp(whole.ptr + i, frame_at, 18) == 0) {
                body = (cf_span){whole.ptr + i, whole.len - i};
                break;
            }
        }
        size_t ada = (size_t)-1, bob = (size_t)-1;
        for (size_t i = 0; i + 10 <= body.len; i++) {
            if (ada == (size_t)-1 && memcmp(body.ptr + i, "Ada Admin", 9) == 0) {
                ada = i;
            }
            if (bob == (size_t)-1 && memcmp(body.ptr + i, "Bob Member", 10) == 0) {
                bob = i;
            }
        }
        CF_CHECK(ada != (size_t)-1 && bob != (size_t)-1 && ada < bob);
    }
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
    CF_CHECK(body_contains(&resp, "id=\"next_page_container\""));
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

/* --- V02 golden renders ----------------------------------------------------- */

/* The member page (Kevin) matches the golden/a account_edit_member fixture
 * token-for-token: the fixture rows seeded, the parity SECRET_KEY_BASE for
 * the signed avatar paths, the viewer's original room for the back link. */
CF_TEST(accounts_edit_member_matches_the_golden) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_edit_fixture(env.scratch.db);
    seed_edit_viewer_room(env.scratch.db, KEVIN);

    char cookie[4096];
    fixture_cookie(&env, cookie, sizeof cookie, "kevin-session", KEVIN);
    cf_request req;
    cf_response resp;
    fixture_get(&req, "/account/edit", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_REQUIRE(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    /* The page layout carries the stylesheet preload links. */
    CF_CHECK(head_contains(&resp, &req, "Link: <"));
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("account_edit_member", (const char *)body.ptr, body.len);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* The administrator page (David) matches account_edit_admin: the logo and
 * settings forms render, and the roster includes the banned user. */
CF_TEST(accounts_edit_admin_matches_the_golden) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_edit_fixture(env.scratch.db);
    seed_edit_viewer_room(env.scratch.db, DAVID);

    char cookie[4096];
    fixture_cookie(&env, cookie, sizeof cookie, "david-session", DAVID);
    cf_request req;
    cf_response resp;
    fixture_get(&req, "/account/edit", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_REQUIRE(resp.status == 200);
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("account_edit_admin", (const char *)body.ptr, body.len);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A Turbo-Frame request renders turbo-rails' frame layout (head + content,
 * no title/nav) and carries no Link preload header. */
CF_TEST(accounts_edit_turbo_frame_renders_the_frame_layout) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_edit_fixture(env.scratch.db);
    seed_edit_viewer_room(env.scratch.db, KEVIN);

    char cookie[4096];
    fixture_cookie(&env, cookie, sizeof cookie, "kevin-session", KEVIN);
    cf_request req;
    cf_response resp;
    fixture_get(&req, "/account/edit", cookie);
    CF_REQUIRE(cf_test_req_header(&req, SP("Turbo-Frame"), SP("account")) ==
               CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_REQUIRE(resp.status == 200);
    CF_CHECK(!head_contains(&resp, &req, "Link: <"));
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    CF_CHECK(span_contains(body, "<html>"));
    CF_CHECK(span_contains(body, "id=\"account_users\""));
    CF_CHECK(span_contains(body, "Kevin"));
    CF_CHECK(!span_contains(body, "<!DOCTYPE html>"));
    CF_CHECK(!span_contains(body, "<title>"));
    CF_CHECK(!span_contains(body, "<nav id=\"nav\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* The presenter's mapping (direct, no dispatch): the member/administrator
 * account_users filter, LOWER(name) partition order, User#title and the
 * signed avatar path, and the viewer's original-room back link. */
CF_TEST(accounts_edit_presenter_maps_the_edit_model) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_fixture_account(env.scratch.db);
    seed_fixture_user(env.scratch.db, DAVID, "David", "david@37signals.com",
                      NULL, 1, 0, "2026-09-26 13:00:20.000000");
    seed_fixture_user(env.scratch.db, KEVIN, "Kevin", "kevin@37signals.com",
                      "Programmer", 0, 0, "2026-09-26 13:00:20.000000");
    seed_fixture_user(env.scratch.db, EX_EMPLOYEE, "Ex Employee", NULL, NULL,
                      0, 1, "2026-09-26 13:00:20.000000");
    seed_fixture_user(env.scratch.db, SPAM_HAM, "Spam Ham", "spam@example.com",
                      NULL, 0, 2, "2026-09-26 13:00:21.000000");
    seed_fixture_user(env.scratch.db, BENDER, "Bender Bot", NULL, NULL, 2, 0,
                      "2026-09-26 13:00:20.000000");
    seed_edit_viewer_room(env.scratch.db, KEVIN);

    cf_request req;
    cf_response resp;
    cf_response_init(&resp);
    prepare_get(&req, "/account/edit", NULL);
    cf_ctx ctx;
    CF_REQUIRE(cf_ctx_create(&ctx, env.app, env.scratch.db, &req, &resp) ==
               CF_OK);

    cf_account account = {0};
    bool found = false;
    CF_REQUIRE(cf_account_first(env.scratch.db, &found, &account) == CF_OK);
    CF_REQUIRE(found);
    bool user_found = false;
    cf_user kevin = {0};
    CF_REQUIRE(cf_user_find_by_id(env.scratch.db, KEVIN, &user_found, &kevin) ==
               CF_OK);
    CF_REQUIRE(user_found);

    /* Member arm: active non-bots only (deactivated and banned drop), the
     * administrators-first partition preserves LOWER(name) order. */
    cf_view_accounts_edit_model model = {0};
    CF_REQUIRE(cf_presenter_accounts_edit(&ctx, &account, &kevin, false,
                                          &model) == CF_OK);
    CF_CHECK(model.account_id == ACCOUNT_ID);
    CF_CHECK(span_contains((cf_span){
                  (const unsigned char *)model.join_code.ptr,
                  model.join_code.len},
              "CRMu-l8Ge-KB9B"));
    CF_CHECK(!model.restrict_room_creation_to_administrators);
    CF_CHECK(model.administrators.len == 1 && model.members.len == 1);
    CF_CHECK(model.administrators.items[0].id == DAVID);
    CF_CHECK(model.members.items[0].id == KEVIN);
    /* User#title: name – bio (U+2013). */
    CF_CHECK(span_contains((cf_span){
                  (const unsigned char *)model.members.items[0].title.ptr,
                  model.members.items[0].title.len},
              "Kevin \xE2\x80\x93 Programmer"));
    CF_CHECK(span_contains((cf_span){
                  (const unsigned char *)model.members.items[0].avatar_path.ptr,
                  model.members.items[0].avatar_path.len},
              "/avatar?v=20260926130020"));
    CF_CHECK(model.has_last_room_visited &&
             model.last_room_visited_id == DIRECT_KEVIN);
    CF_CHECK(!model.has_next_page);
    cf_view_accounts_edit_model_dispose(&model);

    /* Administrator arm: banned rows join the roster, bots never do; LOWER
     * order puts Kevin before Spam Ham. */
    memset(&model, 0, sizeof model);
    CF_REQUIRE(cf_presenter_accounts_edit(&ctx, &account, &kevin, true,
                                          &model) == CF_OK);
    CF_CHECK(model.administrators.len == 1 && model.members.len == 2);
    CF_CHECK(model.members.items[0].id == KEVIN);
    CF_CHECK(model.members.items[1].id == SPAM_HAM);
    cf_view_accounts_edit_model_dispose(&model);

    cf_user_dispose(&kevin);
    cf_account_dispose(&account);
    cf_ctx_destroy(&ctx);
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

static const char *accounts_logo_upload_body =
    "--logo\r\nContent-Disposition: form-data; name=\"account[name]\"\r\n\r\nUploaded Company\r\n--logo\r\n"
    "Content-Disposition: form-data; name=\"account[logo]\"; filename=\"logo.svg\"\r\n"
    "Content-Type: image/svg+xml\r\n\r\n" AVATAR_BYTES "\r\n--logo--\r\n";

static void accounts_logo_request(cf_request *req, const char *cookie) {
    prepare_form(req, CF_PATCH, "/account", accounts_logo_upload_body, cookie, true);
    req->headers[0].value = SP("multipart/form-data; boundary=logo");
}

CF_TEST(accounts_multipart_logo_creates_replaces_and_purges) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db, 1, "Ada Admin", "ada@example.com", 1, 0);
    seed_blob_attachment(env.scratch.db);
    char cookie[4096];
    make_session_cookie(env.config,env.scratch.db,cookie,sizeof cookie,"logo-session",1);
    char root[] = "/tmp/cf-account-logo-XXXXXX";
    avatar_test_root(env.config,root);
    int64_t previous = 1;
    for (size_t i=0;i<2;i++) {
        cf_request req; cf_response resp;
        accounts_logo_request(&req,cookie);
        CF_REQUIRE(run_request(&env,&req,&resp));
        CF_CHECK(resp.status==302);
        CF_CHECK(count_sql(env.scratch.db,"SELECT count(*) FROM accounts WHERE name='Uploaded Company'")==1);
        CF_CHECK(count_sql(env.scratch.db,"SELECT count(*) FROM active_storage_attachments WHERE record_type='Account' AND name='logo'")==1);
        bool found=false; cf_attachment attachment={0}; cf_blob blob={0};
        CF_REQUIRE(cf_attachment_find_for(env.scratch.db,(cf_str){"Account",7},1,(cf_str){"logo",4},&found,&attachment)==CF_OK);
        CF_REQUIRE(found);
        CF_REQUIRE(cf_attachment_blob(env.scratch.db,&attachment,&blob)==CF_OK);
        CF_CHECK(blob.filename.len==8 && memcmp(blob.filename.ptr,"logo.svg",8)==0);
        CF_CHECK(blob.byte_size==(int64_t)strlen(AVATAR_BYTES));
        cf_storage *storage=NULL; int fd=-1; uint64_t size=0;
        CF_REQUIRE(cf_storage_open(root,&storage)==CF_OK);
        CF_REQUIRE(cf_storage_open_read(storage,(cf_span){(unsigned char *)blob.key.ptr,blob.key.len},&fd,&size)==CF_OK);
        char bytes[64]={0}; CF_CHECK(read(fd,bytes,sizeof bytes)==(ssize_t)strlen(AVATAR_BYTES));
        CF_CHECK(strcmp(bytes,AVATAR_BYTES)==0);
        close(fd); cf_storage_close(storage);
        CF_REQUIRE(env.purge_event_count==i+1);
        CF_CHECK(env.purge_events[i].kind==CF_EVENT_PURGE_BLOB);
        CF_CHECK(env.purge_events[i].blob_id==previous);
        previous=blob.id;
        cf_blob_dispose(&blob); cf_attachment_dispose(&attachment); cf_response_dispose(&resp);
    }
    env_close(&env);
    avatar_test_remove_tree(root);
}

CF_TEST(accounts_multipart_logo_rolls_back_files_and_rows_on_write_failure) {
    accounts_env env;
    CF_REQUIRE(env_open(&env));
    seed_account(env.scratch.db);
    seed_user(env.scratch.db,1,"Ada Admin","ada@example.com",1,0);
    seed_blob_attachment(env.scratch.db);
    exec_sql(env.scratch.db,"CREATE TRIGGER fail_logo BEFORE INSERT ON active_storage_attachments BEGIN SELECT RAISE(ABORT,'test attachment failure'); END");
    char cookie[4096];
    make_session_cookie(env.config,env.scratch.db,cookie,sizeof cookie,"logo-failure-session",1);
    char root[]="/tmp/cf-account-logo-failure-XXXXXX";
    avatar_test_root(env.config,root);
    cf_request req; cf_response resp;
    accounts_logo_request(&req,cookie);
    CF_REQUIRE(run_request(&env,&req,&resp));
    CF_CHECK(resp.status==400);
    CF_CHECK(count_sql(env.scratch.db,"SELECT count(*) FROM accounts WHERE name='Campfire'")==1);
    CF_CHECK(count_sql(env.scratch.db,"SELECT count(*) FROM active_storage_blobs")==1);
    CF_CHECK(count_sql(env.scratch.db,"SELECT count(*) FROM active_storage_attachments WHERE record_type='Account' AND name='logo' AND blob_id=1")==1);
    CF_CHECK(avatar_test_file_count(root)==0);
    CF_CHECK(env.purge_event_count==0);
    cf_response_dispose(&resp);
    env_close(&env);
    avatar_test_remove_tree(root);
}

CF_TEST(accounts_multipart_logo_rejects_queued_admin_loss_and_cleans_files) {
    const char *changes[]={"UPDATE users SET status=2 WHERE id=1","UPDATE users SET role=0 WHERE id=1"};
    for(size_t i=0;i<2;i++) {
        accounts_env env;
        CF_REQUIRE(env_open(&env));
        seed_account(env.scratch.db);
        seed_user(env.scratch.db,1,"Ada Admin","ada@example.com",1,0);
        char cookie[4096];
        make_session_cookie(env.config,env.scratch.db,cookie,sizeof cookie,"logo-race-session",1);
        char root[]="/tmp/cf-account-logo-race-XXXXXX";
        avatar_test_root(env.config,root);
        cf_request req; cf_response resp;
        accounts_logo_request(&req,cookie);
        auth_write_race race;
        CF_REQUIRE(auth_race_begin(env.app,env.scratch.path,&race,changes[i]));
        CF_REQUIRE(auth_race_request(env.app,env.scratch.db,&race,&req,&resp));
        auth_race_end(&race);
        CF_CHECK(resp.status==403);
        CF_CHECK(count_sql(env.scratch.db,"SELECT count(*) FROM accounts WHERE name='Campfire'")==1);
        CF_CHECK(count_sql(env.scratch.db,"SELECT count(*) FROM active_storage_blobs")==0);
        CF_CHECK(count_sql(env.scratch.db,"SELECT count(*) FROM active_storage_attachments")==0);
        CF_CHECK(avatar_test_file_count(root)==0);
        CF_CHECK(env.purge_event_count==0);
        cf_response_dispose(&resp);
        env_close(&env);
        avatar_test_remove_tree(root);
    }
}

CF_TEST_MAIN()
