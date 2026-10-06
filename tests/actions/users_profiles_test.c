/* tests/actions/users_profiles_test.c — A-users-profiles acceptance:
 * `users/profiles#show` (route ID 60) and `users/profiles#update` (route
 * IDs 61/62), per docs/devel/implementation/contracts/controller-packets.md
 * "A-users-profiles" and 03-application.md's remaining-packets rule
 * (permit lists, callback order, status/redirect, scoping).
 *
 * Every case runs the real A00 dispatch path through the route double
 * (tests/app/support/route_double.c), which binds rows 60/61/62 to the
 * real actions.  cf.h and src/actions/actions.h are integrator-owned and
 * do not yet declare this packet's symbols, so the entry points are
 * declared here; the integrator's routes.c rebind needs the same
 * declarations (c_symbols cf_action_users_profiles_show /
 * cf_action_users_profiles_update).
 *
 * V02 render: the show gate is gone.  The page renders through the
 * cf_presenter_users_profile model and is compared token-for-token with the
 * golden/a profile fixtures the views suite uses (the fixture rows are
 * seeded here; the transfer token only reproduces with the parity
 * SECRET_KEY_BASE and the capture instant, frozen per case), plus a
 * Turbo-Frame structural case.  The presenter's load/mapping (transfer id
 * purpose, attached flag, ordered partition, display names, involvements,
 * UserSummary) is exercised directly against its view model.
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
#include "models/active_storage.h"
#include "models/membership.h"
#include "models/room.h"
#include "models/user.h"
#include "presenters/users_profiles.h"
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

cf_err cf_action_users_profiles_show(cf_ctx *ctx);
cf_err cf_action_users_profiles_update(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
/* parity/.env.reference (reference-tools/views/a/golden.sh); the golden
 * fixtures' signed tokens only reproduce with it. */
#define GOLDEN_SECRET                                                        \
    "5335c3b1ad35b4ad170c3413bd651ef3b6ed64e257261871a6de3f978cf3868ee"     \
    "417a927040935fb30b0f7debdedb34a2a403e9f34b16cf594c917c2ecd4a995"
#define GOLDEN_VAPID_PUBLIC_KEY                                              \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8" \
    "cTriz_qYBVicY02_VxTQ="
/* render.rb's CHROME_MAC (the profile goldens' per-UA cases use it). */
#define CHROME_MAC                                                            \
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "     \
    "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36"
/* The capture instants (transfer exp minus 4h) the golden tokens embed. */
#define GOLDEN_DAVID_NOW_US INT64_C(1790427622182000)
#define GOLDEN_KEVIN_NOW_US INT64_C(1790427622196000)

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

#define ALL_PETS 104393281
#define DIRECT_JASON 186869642
#define HQ 201306877
#define DIRECT_BENDER 340026324
#define ALL_TALK 486777696
#define DESIGNERS 654632876
#define DIRECT_KEVIN 699448325

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- fixture-faithful asset root (users_sidebars_test.c precedent) --------- */

/* Stage a static root whose manifest is the pinned fixture root and whose
 * importmap-tags.html is the facts' block, so the action renders the layout
 * bytes the golden was captured with. */
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
    char tmpl[] = "/tmp/campfire-profiles-assets-XXXXXX";
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
} profiles_env;

static bool env_open(profiles_env *env) {
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
    cf_config_test_set_bcrypt_cost(env->config, 4);
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
    if (cf_test_routes_add("GET", "/users/:user_id/profile(.:format)", 60,
                           cf_action_users_profiles_show) != CF_OK ||
        cf_test_routes_add("GET", "/users/:user_id/profile", 60,
                           cf_action_users_profiles_show) != CF_OK ||
        cf_test_routes_add("PATCH", "/users/:user_id/profile(.:format)", 61,
                           cf_action_users_profiles_update) != CF_OK ||
        cf_test_routes_add("PATCH", "/users/:user_id/profile", 61,
                           cf_action_users_profiles_update) != CF_OK ||
        cf_test_routes_add("PUT", "/users/:user_id/profile(.:format)", 62,
                           cf_action_users_profiles_update) != CF_OK ||
        cf_test_routes_add("PUT", "/users/:user_id/profile", 62,
                           cf_action_users_profiles_update) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(profiles_env *env) {
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

static void seed_user(cf_db *db, int64_t id, const char *name,
                      const char *email, const char *updated_at) {
    char sql[768];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, updated_at) "
             "VALUES (%lld, NULL, NULL, '2026-01-02 03:04:05', %s, '%s', "
             "NULL, 0, 0, '%s')",
             (long long)id, email != NULL ? "?" : "NULL", name, updated_at);
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

static void seed_room(cf_db *db, int64_t id, const char *name,
                      const char *type_text, int64_t creator_id) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (%lld, '2026-01-02 03:04:05', %lld, '%s', "
             "'%s', '2026-01-02 03:04:05')",
             (long long)id, (long long)creator_id, name, type_text);
    exec_sql(db, sql);
}

static void seed_membership(cf_db *db, int64_t id, int64_t room_id,
                            int64_t user_id, const char *involvement) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO memberships (id, connected_at, connections, "
             "created_at, involvement, room_id, unread_at, updated_at, "
             "user_id) VALUES (%lld, NULL, 0, '2026-01-02 03:04:05', "
             "'%s', %lld, NULL, '2026-01-02 03:04:05', %lld)",
             (long long)id, involvement, (long long)room_id,
             (long long)user_id);
    exec_sql(db, sql);
}

static void seed_blob_attachment(cf_db *db, int64_t blob_id,
                                 int64_t attachment_id, int64_t user_id) {
    char sql[768];
    snprintf(sql, sizeof sql,
             "INSERT INTO active_storage_blobs (id, byte_size, checksum, "
             "content_type, created_at, filename, key, metadata, "
             "service_name) VALUES (%lld, 10, NULL, 'image/png', "
             "'2026-01-02 03:04:05', 'a.png', 'key-%lld', NULL, 'disk')",
             (long long)blob_id, (long long)blob_id);
    exec_sql(db, sql);
    snprintf(sql, sizeof sql,
             "INSERT INTO active_storage_attachments (id, blob_id, "
             "created_at, name, record_id, record_type) VALUES (%lld, %lld, "
             "'2026-01-02 03:04:05', 'avatar', %lld, 'User')",
             (long long)attachment_id, (long long)blob_id,
             (long long)user_id);
    exec_sql(db, sql);
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
                              int status) {
    char sql[768];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, created_at, email_address, name, "
             "role, status, updated_at) VALUES (%lld, %s, "
             "'2026-09-26 13:00:10.000000', %s, '%s', %d, %d, "
             "'2026-09-26 13:00:20.000000')",
             (long long)id, bio != NULL ? "?" : "NULL",
             email != NULL ? "?" : "NULL", name, role, status);
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
                              const char *type, int64_t creator_id,
                              const char *created_at) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (%lld, '%s', %lld, %s, '%s', "
             "'2026-09-26 13:00:20.300000')",
             (long long)id, created_at, (long long)creator_id,
             name != NULL ? "?" : "NULL", type);
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) == SQLITE_OK);
    if (name != NULL) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    }
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

/* The facts.json rows: David's and Kevin's memberships drive the two golden
 * pages; Designers is seeded as David's earliest room (`Room.original`), the
 * push page's last-room target.  The direct memberships are inserted so the
 * `ORDER BY LOWER(rooms.name)` ties (all direct rooms have a NULL name) list
 * them in the reference's order (users_sidebars_test.c note). */
static void seed_fixture(cf_db *db) {
    seed_fixture_account(db);
    seed_fixture_user(db, DAVID, "David", "david@37signals.com", NULL, 1, 0);
    seed_fixture_user(db, JASON, "Jason", "jason@37signals.com", NULL, 1, 0);
    seed_fixture_user(db, BENDER, "Bender Bot", NULL, NULL, 2, 0);
    seed_fixture_user(db, KEVIN, "Kevin", "kevin@37signals.com", "Programmer",
                      0, 0);
    seed_fixture_user(db, JZ, "JZ", "jz@37signals.com", "Designer", 0, 0);
    seed_fixture_user(db, EX_EMPLOYEE, "Ex Employee",
                      "ex-deactivated-e892fe70-ae77-4cee-9fc0-0b7c37f5b417"
                      "@37signals.com",
                      NULL, 0, 1);
    seed_fixture_user(db, SPAM_HAM, "Spam Ham", "spam@example.com", NULL, 0,
                      2);
    seed_fixture_user(db, ANNA, "Anna Bea Cole", "abc@example.com", NULL, 0,
                      0);

    seed_fixture_room(db, ALL_PETS, "All Pets", "Rooms::Open", DAVID,
                      "2026-09-26 12:00:00.000000");
    seed_fixture_room(db, DIRECT_JASON, NULL, "Rooms::Direct", JASON,
                      "2026-09-26 12:00:00.000000");
    seed_fixture_room(db, HQ, "HQ", "Rooms::Open", DAVID,
                      "2026-09-26 12:00:00.000000");
    seed_fixture_room(db, DIRECT_BENDER, NULL, "Rooms::Direct", BENDER,
                      "2026-09-26 12:00:00.000000");
    seed_fixture_room(db, ALL_TALK, "All Talk", "Rooms::Closed", JASON,
                      "2026-09-26 12:00:00.000000");
    seed_fixture_room(db, DESIGNERS, "Designers", "Rooms::Closed", DAVID,
                      "2026-09-26 11:00:00.000000");
    seed_fixture_room(db, DIRECT_KEVIN, NULL, "Rooms::Direct", DAVID,
                      "2026-09-26 12:00:00.000000");

    seed_fixture_membership(db, 900001, DIRECT_JASON, JASON, "everything");
    seed_fixture_membership(db, 900002, DIRECT_JASON, DAVID, "everything");
    seed_fixture_membership(db, 900003, DIRECT_KEVIN, DAVID, "everything");
    seed_fixture_membership(db, 900004, DIRECT_KEVIN, KEVIN, "everything");
    seed_fixture_membership(db, 900005, DIRECT_BENDER, BENDER, "everything");
    seed_fixture_membership(db, 900006, DIRECT_BENDER, KEVIN, "everything");
    seed_fixture_membership(db, 900010, DESIGNERS, DAVID, "mentions");
    seed_fixture_membership(db, 900011, DESIGNERS, KEVIN, "mentions");
    seed_fixture_membership(db, 900012, DESIGNERS, JZ, "everything");
    seed_fixture_membership(db, 900013, DESIGNERS, JASON, "everything");
    seed_fixture_membership(db, 900020, ALL_PETS, DAVID, "everything");
    seed_fixture_membership(db, 900021, ALL_PETS, JASON, "everything");
    seed_fixture_membership(db, 900022, ALL_PETS, SPAM_HAM, "mentions");
    seed_fixture_membership(db, 900023, ALL_PETS, ANNA, "mentions");
    seed_fixture_membership(db, 900030, HQ, DAVID, "everything");
    seed_fixture_membership(db, 900031, HQ, KEVIN, "everything");
    seed_fixture_membership(db, 900032, HQ, JASON, "everything");
    seed_fixture_membership(db, 900033, HQ, JZ, "everything");
    seed_fixture_membership(db, 900034, HQ, SPAM_HAM, "mentions");
    seed_fixture_membership(db, 900035, HQ, ANNA, "mentions");
    seed_fixture_membership(db, 900040, ALL_TALK, DAVID, "everything");
    seed_fixture_membership(db, 900041, ALL_TALK, JASON, "everything");
    seed_fixture_membership(db, 900042, ALL_TALK, BENDER, "mentions");
}

/* --- request/response helpers ---------------------------------------------- */

static bool run_request(profiles_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void profile_request(cf_request *req, cf_method method,
                            const char *target, const char *body,
                            const char *content_type,
                            const char *sec_fetch_site,
                            const char *cookie_header) {
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

/* Run the action on a hand-built context and read the flash map (rooms
 * precedent: the session commit does not persist flash yet). */
static bool run_direct(profiles_env *env, cf_request *req, cf_response *resp,
                       cf_action_fn action, cf_ctx *ctx) {
    CF_REQUIRE(cf_ctx_create(ctx, env->app, env->scratch.db, req, resp) ==
               CF_OK);
    return (*action)(ctx) == CF_OK;
}

static bool flash_value(cf_ctx *ctx, const char *key, const char *expected) {
    cf_span value = {NULL, 0};
    if (cf_ctx_flash_get(ctx, SP(key), &value) != CF_OK) return false;
    size_t len = strlen(expected);
    return value.len == len && memcmp(value.ptr, expected, len) == 0;
}

/* Build GET `/users/me/profile` as `user_id` with the capture's UA. */
static unsigned profile_fixture_seq = 0;

static void profile_fixture_get(profiles_env *env, cf_request *req,
                                int64_t user_id, const char *ua) {
    static char cookie[4096];
    char token[64];
    snprintf(token, sizeof token, "profile-token-%u",
             profile_fixture_seq++);
    make_session_cookie(env->config, env->scratch.db, cookie, sizeof cookie,
                        token, user_id);
    profile_request(req, CF_GET, "/users/me/profile", NULL, NULL, "same-origin",
                    cookie);
    if (ua != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("User-Agent"), SP(ua)) == CF_OK);
    }
}

/* --- acceptance ------------------------------------------------------------ */

CF_TEST(users_profiles_route_ids_bind_the_actions) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/users/:user_id/profile", 60,
                                  cf_action_users_profiles_show) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("PATCH", "/users/:user_id/profile", 61,
                                  cf_action_users_profiles_update) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("PUT", "/users/:user_id/profile", 62,
                                  cf_action_users_profiles_update) == CF_OK);
    CF_CHECK(cf_route_action(60) == cf_action_users_profiles_show);
    CF_CHECK(cf_route_action(61) == cf_action_users_profiles_update);
    CF_CHECK(cf_route_action(62) == cf_action_users_profiles_update);
}

/* The V02 render: David's page matches the golden/a fixture token-for-token
 * (fixture rows seeded; parity SECRET_KEY_BASE; the capture instant frozen so
 * the transfer token, expiry included, reproduces byte-exactly). */
CF_TEST(users_profiles_show_matches_the_golden) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_fixture(env.scratch.db);
    cf_test_clock_set_fixed_us(GOLDEN_DAVID_NOW_US);

    cf_request req;
    cf_response resp;
    profile_fixture_get(&env, &req, DAVID, CHROME_MAC);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_REQUIRE(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    /* The page layout carries the stylesheet preload links. */
    CF_CHECK(head_contains(&resp, &req, "Link: <"));
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("profile_chrome_mac", (const char *)body.ptr, body.len);
    cf_response_dispose(&resp);
    cf_test_clock_clear();
    env_close(&env);
}

/* Kevin (member) viewing his own profile: his memberships, bio and email. */
CF_TEST(users_profiles_kevin_matches_the_golden) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_fixture(env.scratch.db);
    cf_test_clock_set_fixed_us(GOLDEN_KEVIN_NOW_US);

    cf_request req;
    cf_response resp;
    profile_fixture_get(&env, &req, KEVIN, CHROME_MAC);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_REQUIRE(resp.status == 200);
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("profile_kevin", (const char *)body.ptr, body.len);
    cf_response_dispose(&resp);
    cf_test_clock_clear();
    env_close(&env);
}

/* An attached avatar renders the delete control (the golden's with_avatar
 * case) while the avatar image stays the token-based path. */
CF_TEST(users_profiles_with_avatar_matches_the_golden) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_fixture(env.scratch.db);
    seed_blob_attachment(env.scratch.db, 50, 60, KEVIN);
    /* The capture attached the avatar (and the account logo) later in the
     * run, touching both rows: the golden's fresh paths carry 13:00:29.  The
     * same late section had already set the account's custom styles. */
    {
        char sql[256];
        snprintf(sql, sizeof sql,
                 "UPDATE users SET updated_at = '2026-09-26 13:00:29.000000' "
                 "WHERE id = %lld",
                 (long long)KEVIN);
        exec_sql(env.scratch.db, sql);
    }
    exec_sql(env.scratch.db,
             "UPDATE accounts SET updated_at = '2026-09-26 13:00:29.000000', "
             "custom_styles = 'body { --x: 1; } a > b { color: red }'");
    /* The same late section attached the account logo (body class). */
    {
        char sql[768];
        snprintf(sql, sizeof sql,
                 "INSERT INTO active_storage_blobs (id, byte_size, checksum, "
                 "content_type, created_at, filename, key, metadata, "
                 "service_name) VALUES (51, 10, NULL, 'image/png', "
                 "'2026-09-26 13:00:29.000000', 'logo.png', 'logo-key', NULL, "
                 "'disk')");
        exec_sql(env.scratch.db, sql);
        snprintf(sql, sizeof sql,
                 "INSERT INTO active_storage_attachments (id, blob_id, "
                 "created_at, name, record_id, record_type) VALUES (61, 51, "
                 "'2026-09-26 13:00:29.000000', 'logo', %d, 'Account')",
                 ACCOUNT_ID);
        exec_sql(env.scratch.db, sql);
    }
    cf_test_clock_set_fixed_us(GOLDEN_KEVIN_NOW_US);

    cf_request req;
    cf_response resp;
    profile_fixture_get(&env, &req, KEVIN, CHROME_MAC);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_REQUIRE(resp.status == 200);
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("profile_with_avatar", (const char *)body.ptr, body.len);
    cf_response_dispose(&resp);
    cf_test_clock_clear();
    env_close(&env);
}

/* A Turbo-Frame request renders turbo-rails' frame layout (head + content,
 * no title/nav) and carries no Link preload header. */
CF_TEST(users_profiles_turbo_frame_renders_the_frame_layout) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_fixture(env.scratch.db);
    cf_test_clock_set_fixed_us(GOLDEN_KEVIN_NOW_US);

    cf_request req;
    cf_response resp;
    profile_fixture_get(&env, &req, KEVIN, CHROME_MAC);
    CF_REQUIRE(cf_test_req_header(&req, SP("Turbo-Frame"),
                                  SP("profile")) == CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_REQUIRE(resp.status == 200);
    CF_CHECK(!head_contains(&resp, &req, "Link: <"));
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    CF_CHECK(span_contains(body, "id=\"session_transfer_url\""));
    CF_CHECK(span_contains(body, "name=\"user[name]\""));
    CF_CHECK(!span_contains(body, "<title>Kevin</title>"));
    CF_CHECK(!span_contains(body, "<nav id=\"nav\">"));
    cf_response_dispose(&resp);
    cf_test_clock_clear();
    env_close(&env);
}

CF_TEST(users_profiles_show_unauthenticated_redirects_to_sign_in) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2026-01-02 03:04:05");

    cf_request req;
    cf_response resp;
    profile_request(&req, CF_GET, "/users/me/profile", NULL, NULL,
                    "same-origin", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_profiles_show_unacceptable_format_is_406) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2026-01-02 03:04:05");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    profile_request(&req, CF_GET, "/users/me/profile.json", NULL, NULL,
                    "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_profiles_presenter_maps_the_profile_model) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2026-01-02 03:04:05");
    seed_user(env.scratch.db, 2, "Kevin", NULL, "2026-01-02 03:04:05");
    /* LOWER(name) order: all-talk, Bravo, dm (direct), Zebra. */
    seed_room(env.scratch.db, 10, "Zebra", "Rooms::Open", 2);
    seed_room(env.scratch.db, 11, "all-talk", "Rooms::Open", 2);
    seed_room(env.scratch.db, 12, "dm", "Rooms::Direct", 2);
    seed_room(env.scratch.db, 13, "Bravo", "Rooms::Open", 2);
    seed_membership(env.scratch.db, 101, 10, 1, "everything");
    seed_membership(env.scratch.db, 102, 11, 1, "mentions");
    seed_membership(env.scratch.db, 103, 12, 1, "nothing");
    seed_membership(env.scratch.db, 104, 13, 1, "invisible");
    seed_membership(env.scratch.db, 105, 10, 2, "everything"); /* other user */

    cf_request req;
    cf_response resp;
    cf_response_init(&resp);
    profile_request(&req, CF_GET, "/users/me/profile", NULL, NULL,
                    "same-origin", NULL);
    cf_ctx ctx;
    CF_REQUIRE(cf_ctx_create(&ctx, env.app, env.scratch.db, &req, &resp) ==
               CF_OK);
    cf_user user = {0};
    bool found = false;
    CF_REQUIRE(cf_user_find_by_id(env.scratch.db, 1, &found, &user) == CF_OK);
    CF_REQUIRE(found);
    cf_view_users_profile_model model = {0};
    CF_REQUIRE(cf_presenter_users_profile(&ctx, &user, &model) == CF_OK);

    /* transfer_id verifies as purpose "transfer" for this user... */
    int64_t now = cf_now_us(env.app);
    cf_optional_i64 verified = {false, 0};
    bool tf_found = false;
    CF_REQUIRE(cf_auth_signed_id_verify(
                   (cf_span){(const unsigned char *)env.config->secret_key_base,
                             env.config->secret_key_base_len},
                   SP("User"),
                   (cf_span){(const unsigned char *)model.transfer_id.ptr,
                             model.transfer_id.len},
                   SP("transfer"), true, now, &verified, &tf_found) == CF_OK);
    CF_CHECK(tf_found && verified.present && verified.value == 1);
    /* ...and for no other purpose. */
    cf_optional_i64 wrong = {false, 0};
    bool wrong_found = true;
    CF_REQUIRE(cf_auth_signed_id_verify(
                   (cf_span){(const unsigned char *)env.config->secret_key_base,
                             env.config->secret_key_base_len},
                   SP("User"),
                   (cf_span){(const unsigned char *)model.transfer_id.ptr,
                             model.transfer_id.len},
                   SP("avatar"), true, now, &wrong, &wrong_found) == CF_OK);
    CF_CHECK(!wrong_found);

    /* UserSummary: the row's fields plus the token-based avatar path. */
    CF_CHECK(!model.avatar_attached);
    CF_CHECK(model.user.id == 1);
    CF_CHECK(model.user.name.len == 5 &&
             memcmp(model.user.name.ptr, "David", 5) == 0);
    CF_CHECK(!model.user.has_bio && !model.user.has_email);
    CF_CHECK(model.user.role == CF_ROLE_MEMBER &&
             model.user.status == CF_STATUS_ACTIVE);
    CF_CHECK(span_contains(
        (cf_span){(const unsigned char *)model.user.avatar_path.ptr,
                  model.user.avatar_path.len},
        "/avatar?v=20260102030405"));

    /* Partition keeps with_ordered_room order within each side; param keys,
     * display names (a direct room with no other member is the viewer) and
     * involvements match profile_memberships. */
    CF_REQUIRE(model.shared_memberships.len == 3 &&
               model.direct_memberships.len == 1);
    const cf_view_profile_membership *s0 = &model.shared_memberships.items[0];
    const cf_view_profile_membership *s1 = &model.shared_memberships.items[1];
    const cf_view_profile_membership *s2 = &model.shared_memberships.items[2];
    const cf_view_profile_membership *d0 = &model.direct_memberships.items[0];
    CF_CHECK(s0->room_id == 11 && s1->room_id == 13 && s2->room_id == 10);
    CF_CHECK(d0->room_id == 12 && d0->direct);
    CF_CHECK(!s0->direct && !s1->direct && !s2->direct);
    CF_CHECK(span_contains((cf_span){(const unsigned char *)
                                         s0->room_param_key.ptr,
                                     s0->room_param_key.len},
                           "rooms_open"));
    CF_CHECK(span_contains((cf_span){(const unsigned char *)
                                         d0->room_param_key.ptr,
                                     d0->room_param_key.len},
                           "rooms_direct"));
    CF_CHECK(span_contains((cf_span){(const unsigned char *)
                                         s0->room_display_name.ptr,
                                     s0->room_display_name.len},
                           "all-talk"));
    CF_CHECK(span_contains((cf_span){(const unsigned char *)
                                         s1->room_display_name.ptr,
                                     s1->room_display_name.len},
                           "Bravo"));
    CF_CHECK(span_contains((cf_span){(const unsigned char *)
                                         s2->room_display_name.ptr,
                                     s2->room_display_name.len},
                           "Zebra"));
    CF_CHECK(span_contains((cf_span){(const unsigned char *)
                                         d0->room_display_name.ptr,
                                     d0->room_display_name.len},
                           "David"));
    CF_CHECK(span_contains((cf_span){(const unsigned char *)s0->involvement.ptr,
                                     s0->involvement.len},
                           "mentions"));
    CF_CHECK(span_contains((cf_span){(const unsigned char *)s1->involvement.ptr,
                                     s1->involvement.len},
                           "invisible"));
    CF_CHECK(span_contains((cf_span){(const unsigned char *)s2->involvement.ptr,
                                     s2->involvement.len},
                           "everything"));
    CF_CHECK(span_contains((cf_span){(const unsigned char *)d0->involvement.ptr,
                                     d0->involvement.len},
                           "nothing"));
    cf_view_users_profile_model_dispose(&model);

    /* With an avatar attached, the flag flips and nothing else changes. */
    seed_blob_attachment(env.scratch.db, 50, 60, 1);
    memset(&model, 0, sizeof model);
    CF_REQUIRE(cf_presenter_users_profile(&ctx, &user, &model) == CF_OK);
    CF_CHECK(model.avatar_attached);
    CF_CHECK(model.shared_memberships.len == 3 &&
             model.direct_memberships.len == 1);
    cf_view_users_profile_model_dispose(&model);

    cf_user_dispose(&user);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_profiles_update_renames_with_check_notice) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PATCH, "/users/me/profile", "user[name]=Dave",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/users/me/profile\r\n"));
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    char name[64];
    read_text(env.scratch.db,
              "SELECT \"name\" FROM \"users\" WHERE \"id\" = 1", name,
              sizeof name);
    CF_CHECK(strcmp(name, "Dave") == 0);
    /* updated_at moved (the reference writes it when something changed). */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM \"users\" WHERE \"id\" = 1 "
                        "AND \"updated_at\" > '2020-01-01 00:00:00'") == 1);
    cf_response_dispose(&resp);

    /* The notice is "✓": no avatar key was given. */
    cf_request req2;
    cf_response resp2;
    cf_ctx ctx;
    profile_request(&req2, CF_PATCH, "/users/1/profile", "user[name]=Dave2",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    cf_response_init(&resp2);
    CF_REQUIRE(run_direct(&env, &req2, &resp2,
                          cf_action_users_profiles_update, &ctx));
    CF_CHECK(cf_auth_halted(&ctx));
    CF_CHECK(resp2.status == 302);
    CF_CHECK(flash_value(&ctx, "notice", "\342\234\223"));
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp2);
    env_close(&env);
}

CF_TEST(users_profiles_update_put_alias_and_email_bio) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", "old@example.com",
              "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    /* Route 62 (PUT) shares the update function. */
    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PUT, "/users/me/profile",
                    "user[email_address]=new%40example.com&user[bio]=Hello",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/users/me/profile\r\n"));
    char email[128];
    read_text(env.scratch.db,
              "SELECT \"email_address\" FROM \"users\" WHERE \"id\" = 1",
              email, sizeof email);
    CF_CHECK(strcmp(email, "new@example.com") == 0);
    char bio[128];
    read_text(env.scratch.db,
              "SELECT \"bio\" FROM \"users\" WHERE \"id\" = 1", bio,
              sizeof bio);
    CF_CHECK(strcmp(bio, "Hello") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_profiles_update_empty_avatar_deletes_with_long_notice) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    /* No attachment: Delete is a no-op that still redirects. */
    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PATCH, "/users/me/profile", "user[avatar]=",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM "
                        "active_storage_attachments") == 0);
    cf_response_dispose(&resp);

    /* The notice is the 30-minute text: the avatar key was non-nil. */
    cf_request req2;
    cf_response resp2;
    cf_ctx ctx;
    profile_request(&req2, CF_PATCH, "/users/1/profile", "user[avatar]=",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    cf_response_init(&resp2);
    CF_REQUIRE(run_direct(&env, &req2, &resp2,
                          cf_action_users_profiles_update, &ctx));
    CF_CHECK(resp2.status == 302);
    CF_CHECK(flash_value(&ctx, "notice",
                         "It may take up to 30 minutes to change "
                         "everywhere."));
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp2);
    env_close(&env);
}

CF_TEST(users_profiles_update_avatar_delete_destroys_touches_purges) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2020-01-01 00:00:00");
    seed_user(env.scratch.db, 2, "Kevin", NULL, "2020-01-01 00:00:00");
    seed_blob_attachment(env.scratch.db, 50, 60, 1);
    seed_blob_attachment(env.scratch.db, 51, 61, 2);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PATCH, "/users/me/profile",
                    "user[avatar]=&user[name]=Dave",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    /* Only the current user's attachment goes; the blob row stays (the
     * purge job owns file/row removal). */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_attachments "
                        "WHERE record_id = 1") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_attachments "
                        "WHERE record_id = 2") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_blobs "
                        "WHERE id = 50") == 1);
    /* The user row was touched by the destroy. */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM \"users\" WHERE \"id\" = 1 "
                        "AND \"updated_at\" > '2020-01-01 00:00:00'") == 1);
    /* Best-effort PurgeBlob: no consumer registered, so dropped+counted
     * with the committed write unchanged (bans precedent). */
    cf_writer_stats stats;
    CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
    CF_CHECK(stats.best_effort_dropped[CF_EVENT_PURGE_BLOB] == 1);
    CF_CHECK(stats.committed == 1);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_profiles_update_avatar_string_is_internal) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    /* A plain string is Assignment::Invalid ("Could not find or build
     * blob"), the reference's 500 — and the user row is untouched. */
    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PATCH, "/users/me/profile",
                    "user[avatar]=not-a-blob&user[name]=Dave",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    char name[64];
    read_text(env.scratch.db,
              "SELECT \"name\" FROM \"users\" WHERE \"id\" = 1", name,
              sizeof name);
    CF_CHECK(strcmp(name, "David") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_profiles_update_password_sets_digest_blank_keeps) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2020-01-01 00:00:00");
    /* Bcrypt the seed digest through the same service (cost 4). */
    cf_str before = {0};
    CF_REQUIRE(cf_auth_password_digest((cf_str){(char *)"old-pw", 6}, 4,
                                       &before) == CF_OK);
    {
        sqlite3 *handle = cf_db_handle(env.scratch.db);
        sqlite3_stmt *stmt = NULL;
        CF_REQUIRE(sqlite3_prepare_v2(handle,
                                      "UPDATE users SET password_digest = "
                                      "? WHERE id = 1",
                                      -1, &stmt, NULL) == SQLITE_OK);
        sqlite3_bind_text(stmt, 1, before.ptr, -1, SQLITE_TRANSIENT);
        CF_REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PATCH, "/users/me/profile",
                    "user[password]=brand-new-password",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    char after[256];
    read_text(env.scratch.db,
              "SELECT \"password_digest\" FROM \"users\" WHERE \"id\" = 1",
              after, sizeof after);
    CF_CHECK(strcmp(after, before.ptr) != 0);
    bool ok = false;
    cf_user candidate = {0};
    bool found = false;
    CF_REQUIRE(cf_user_find_by_id(env.scratch.db, 1, &found, &candidate) ==
               CF_OK);
    CF_REQUIRE(found);
    CF_REQUIRE(cf_user_authenticate(&candidate,
                                    (cf_str){(char *)"brand-new-password",
                                             18},
                                    &ok) == CF_OK);
    CF_CHECK(ok);
    cf_user_dispose(&candidate);
    cf_response_dispose(&resp);

    /* A blank password leaves the digest alone (`password=` ignores it). */
    cf_request req2;
    cf_response resp2;
    profile_request(&req2, CF_PATCH, "/users/me/profile", "user[password]=",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 302);
    char kept[256];
    read_text(env.scratch.db,
              "SELECT \"password_digest\" FROM \"users\" WHERE \"id\" = 1",
              kept, sizeof kept);
    CF_CHECK(strcmp(kept, after) == 0);
    cf_response_dispose(&resp2);
    cf_str_dispose(&before);
    env_close(&env);
}

CF_TEST(users_profiles_update_json_false_is_a_noop_redirect) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    /* `require` accepts the false literal; permit of it is empty, so the
     * update changes nothing but still redirects with "✓". */
    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PATCH, "/users/me/profile", "{\"user\":false}",
                    "application/json", "same-origin", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/users/me/profile\r\n"));
    char name[64];
    read_text(env.scratch.db,
              "SELECT \"name\" FROM \"users\" WHERE \"id\" = 1", name,
              sizeof name);
    CF_CHECK(strcmp(name, "David") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_profiles_update_missing_user_param_is_400) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PATCH, "/users/me/profile", "name=Dave",
                    "application/x-www-form-urlencoded", "same-origin",
                    cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 400);
    char name[64];
    read_text(env.scratch.db,
              "SELECT \"name\" FROM \"users\" WHERE \"id\" = 1", name,
              sizeof name);
    CF_CHECK(strcmp(name, "David") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_profiles_update_unauthenticated_redirects_to_sign_in) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2020-01-01 00:00:00");

    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PATCH, "/users/me/profile", "user[name]=Dave",
                    "application/x-www-form-urlencoded", "same-origin",
                    NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    char name[64];
    read_text(env.scratch.db,
              "SELECT \"name\" FROM \"users\" WHERE \"id\" = 1", name,
              sizeof name);
    CF_CHECK(strcmp(name, "David") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_profiles_update_cross_site_post_is_422) {
    profiles_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", NULL, "2020-01-01 00:00:00");
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "david-session", 1);

    cf_request req;
    cf_response resp;
    profile_request(&req, CF_PATCH, "/users/me/profile", "user[name]=Dave",
                    "application/x-www-form-urlencoded", "cross-site",
                    cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    char name[64];
    read_text(env.scratch.db,
              "SELECT \"name\" FROM \"users\" WHERE \"id\" = 1", name,
              sizeof name);
    CF_CHECK(strcmp(name, "David") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
