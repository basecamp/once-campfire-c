/* tests/actions/users_sidebars_test.c — A-users-sidebars acceptance:
 * `users/sidebars#show` (route ID 57), per
 * docs/devel/implementation/contracts/controller-packets.md
 * "A-users-sidebars" and 03-application.md's users/sidebars slice row.
 *
 * Cases:
 *   - route binding: id 57 resolves to cf_action_users_sidebars_show;
 *   - unauthenticated: the chain redirects to sign-in before the action body;
 *   - bot-key authentication: `deny_bots` (403, Before::default);
 *   - a non-HTML request: `respond_to(&[HTML])` is 406 before any read;
 *   - a successful page render of the golden/a sidebar_david fixture: the
 *     seeded rows are the fixture's rows (users/rooms/memberships), and the
 *     finished response body is compared token-for-token with the reference
 *     render (assert_parity's DOM comparison, no masks for this page);
 *   - the same for sidebar_kevin (a different current user: the direct list,
 *     shared list and placeholder set all change);
 *   - the Turbo-Frame variant: the frame layout around the same content, no
 *     `Link` preload header (sidebar_frame fixture);
 *   - the route's `user_id` segment is ignored exactly as the source ignores
 *     params[:user_id]: /users/999/sidebar and /users/abc/sidebar both render
 *     the signed-in user's sidebar;
 *   - can_create_rooms follows the account setting: with
 *     restrict_room_creation_to_administrators the member loses the new-room
 *     link while the administrator keeps it;
 *   - the disclosed S02 gap: a user with an attached avatar still renders the
 *     token-based `fresh_user_avatar_path` (no variant branch exists in the
 *     pinned source, and the presenter reads no blob).
 *
 * The route double (tests/app/support/route_double.c) binds row 57 to the
 * real action, so every case runs the real A00 dispatch path (params, cookies,
 * before-actions, format negotiation, generic error mapping).  cf.h and
 * src/actions/actions.h are integrator-owned and do not yet declare this
 * packet's symbol, so the entry point is declared here; the integrator's
 * routes.c rebind needs the same declaration (cf_action_users_sidebars_show).
 *
 * The golden/a fixtures were rendered with the parity SECRET_KEY_BASE from
 * parity/.env.reference, so the signed avatar tokens and stream names only
 * reproduce with it.
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "db/db_testutil.h"
#include "http/http_internal.h"
#include "views.h"

#include "../views/support/golden.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "yyjson.h"

cf_err cf_action_users_sidebars_show(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
/* parity/.env.reference (reference-tools/views/a/golden.sh). */
#define GOLDEN_SECRET                                                        \
    "5335c3b1ad35b4ad170c3413bd651ef3b6ed64e257261871a6de3f978cf3868ee"     \
    "417a927040935fb30b0f7debdedb34a2a403e9f34b16cf594c917c2ecd4a995"
#define GOLDEN_VAPID_PUBLIC_KEY                                              \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8" \
    "cTriz_qYBVicY02_VxTQ="

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

/* --- fixture-faithful asset root ------------------------------------------- */

/* The action renders through the configured asset module for the layout's
 * importmap/stylesheet tags, while the golden/a fixtures were cut with the
 * a-runner's asset build (facts.json): the pinned fixture root's importmap
 * differs in one digest.  Stage a static root whose manifest is the pinned
 * fixture root (its stylesheet digests do match the fixtures) and whose
 * importmap-tags.html is the facts' block, so the action renders the layout
 * bytes the fixture was captured with (the view tests' facts-tag override,
 * applied to the module the action path reads). */
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
    char tmpl[] = "/tmp/campfire-sidebar-assets-XXXXXX";
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

/* --- scratch app ----------------------------------------------------------- */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    unsigned session_seq;
} sidebar_env;

static bool env_open(sidebar_env *env) {
    if (g_assets_root[0] == '\0' && !stage_assets_root()) return false;
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;
    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", ORIGIN},
        {"SECRET_KEY_BASE", GOLDEN_SECRET},
        {"VAPID_PUBLIC_KEY", GOLDEN_VAPID_PUBLIC_KEY},
        {"DATABASE_PATH", env->scratch.path},
    };
    if (cf_config_parse(entries, 4, NULL, &env->config) != CF_OK) return false;
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
    cf_test_routes_reset();
    /* routes.json id 57.  The double's `(.:format)` grammar needs an
     * extension for a literal last segment, so the bare form is registered as
     * well (the same note as the sessions/bans tests). */
    return cf_test_routes_add("GET", "/users/:user_id/sidebar(.:format)", 57,
                              cf_action_users_sidebars_show) == CF_OK &&
           cf_test_routes_add("GET", "/users/:user_id/sidebar", 57,
                              cf_action_users_sidebars_show) == CF_OK;
}

static void env_close(sidebar_env *env) {
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

/* --- seeding (the golden/a sidebar fixtures' rows) ------------------------- */

static void seed_account(cf_db *db, const char *settings) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
             "name, settings, singleton_guard, updated_at) VALUES "
             "(%d, '2026-09-26 13:00:20.000000', NULL, 'CRMu-l8Ge-KB9B', "
             "'37signals', %s, 0, '2026-09-26 13:00:20.000000')",
             ACCOUNT_ID, settings != NULL ? settings : "NULL");
    exec_sql(db, sql);
}

static void seed_user(cf_db *db, int64_t id, const char *name, int role,
                      int status, const char *created_at,
                      const char *updated_at) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, created_at, email_address, name, "
             "role, status, updated_at) VALUES (%lld, NULL, '%s', NULL, '%s', "
             "%d, %d, '%s')",
             (long long)id, created_at, name, role, status, updated_at);
    exec_sql(db, sql);
}

static void seed_users(cf_db *db) {
    seed_user(db, DAVID, "David", 1, 0, "2026-09-26 13:00:10.000000",
              "2026-09-26 13:00:20.000000");
    seed_user(db, JASON, "Jason", 1, 0, "2026-09-26 13:00:11.000000",
              "2026-09-26 13:00:20.000000");
    seed_user(db, BENDER, "Bender Bot", 2, 0, "2026-09-26 13:00:12.000000",
              "2026-09-26 13:00:20.000000");
    seed_user(db, KEVIN, "Kevin", 0, 0, "2026-09-26 13:00:13.000000",
              "2026-09-26 13:00:20.000000");
    seed_user(db, JZ, "JZ", 0, 0, "2026-09-26 13:00:14.000000",
              "2026-09-26 13:00:20.000000");
    seed_user(db, EX_EMPLOYEE, "Ex Employee", 0, 1,
              "2026-09-26 13:00:15.000000", "2026-09-26 13:00:21.000000");
    seed_user(db, SPAM_HAM, "Spam Ham", 0, 2, "2026-09-26 13:00:16.000000",
              "2026-09-26 13:00:21.000000");
    seed_user(db, ANNA, "Anna Bea Cole", 0, 0, "2026-09-26 13:00:17.000000",
              "2026-09-26 13:00:22.000000");
}

static void seed_room(cf_db *db, int64_t id, const char *name,
                      const char *type, int64_t creator_id) {
    char name_sql[128];
    if (name != NULL) {
        snprintf(name_sql, sizeof name_sql, "'%s'", name);
    } else {
        snprintf(name_sql, sizeof name_sql, "NULL");
    }
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (%lld, '2026-09-26 12:00:00.000000', %lld, "
             "%s, '%s', '2026-09-26 13:00:20.300000')",
             (long long)id, (long long)creator_id, name_sql, type);
    exec_sql(db, sql);
}

static void seed_membership(cf_db *db, int64_t id, int64_t room_id,
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

/* The fixture rows.  The direct memberships are inserted so that the
 * `ORDER BY LOWER(rooms.name)` scan (all direct rooms have a NULL name) lists
 * them in the reference's order: David's [DIRECT_JASON, DIRECT_KEVIN] and
 * Kevin's [DIRECT_KEVIN, DIRECT_BENDER] before the stable sort and reverse. */
static void seed_fixture(cf_db *db) {
    seed_account(db, NULL);
    seed_users(db);
    seed_room(db, ALL_PETS, "All Pets", "Rooms::Open", DAVID);
    seed_room(db, DIRECT_JASON, NULL, "Rooms::Direct", JASON);
    seed_room(db, HQ, "HQ", "Rooms::Open", DAVID);
    seed_room(db, DIRECT_BENDER, NULL, "Rooms::Direct", BENDER);
    seed_room(db, ALL_TALK, "All Talk", "Rooms::Closed", JASON);
    seed_room(db, DESIGNERS, "Designers", "Rooms::Closed", DAVID);
    seed_room(db, DIRECT_KEVIN, NULL, "Rooms::Direct", DAVID);

    seed_membership(db, 900001, DIRECT_JASON, JASON, "everything");
    seed_membership(db, 900002, DIRECT_JASON, DAVID, "everything");
    seed_membership(db, 900003, DIRECT_KEVIN, DAVID, "everything");
    seed_membership(db, 900004, DIRECT_KEVIN, KEVIN, "everything");
    seed_membership(db, 900005, DIRECT_BENDER, BENDER, "everything");
    seed_membership(db, 900006, DIRECT_BENDER, KEVIN, "everything");
    /* Designers. */
    seed_membership(db, 900010, DESIGNERS, DAVID, "mentions");
    seed_membership(db, 900011, DESIGNERS, KEVIN, "mentions");
    seed_membership(db, 900012, DESIGNERS, JZ, "everything");
    seed_membership(db, 900013, DESIGNERS, JASON, "everything");
    /* All Pets. */
    seed_membership(db, 900020, ALL_PETS, DAVID, "everything");
    seed_membership(db, 900021, ALL_PETS, JASON, "everything");
    seed_membership(db, 900022, ALL_PETS, SPAM_HAM, "mentions");
    seed_membership(db, 900023, ALL_PETS, ANNA, "mentions");
    /* HQ. */
    seed_membership(db, 900030, HQ, DAVID, "everything");
    seed_membership(db, 900031, HQ, KEVIN, "everything");
    seed_membership(db, 900032, HQ, JASON, "everything");
    seed_membership(db, 900033, HQ, JZ, "everything");
    seed_membership(db, 900034, HQ, SPAM_HAM, "mentions");
    seed_membership(db, 900035, HQ, ANNA, "mentions");
    /* All Talk. */
    seed_membership(db, 900040, ALL_TALK, DAVID, "everything");
    seed_membership(db, 900041, ALL_TALK, JASON, "everything");
    seed_membership(db, 900042, ALL_TALK, BENDER, "mentions");
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

static bool run_request(sidebar_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
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

static bool body_contains(cf_response *resp, const char *needle) {
    return resp->body != NULL && span_contains(cf_buf_span(resp->body), needle);
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

/* GET `path` with `user_id`'s session cookie; 0 means unauthenticated. */
static void sidebar_get(sidebar_env *env, cf_request *req, cf_response *resp,
                        const char *path, int64_t user_id) {
    cf_test_req_init(req);
    req->path = SP(path);
    req->target = req->path;
    if (user_id != 0) {
        static char cookie[4096];
        char token[64];
        snprintf(token, sizeof token, "sidebar-token-%u", env->session_seq++);
        make_session_cookie(env->config, env->scratch.db, cookie, sizeof cookie,
                            token, user_id);
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    }
    CF_REQUIRE(run_request(env, req, resp));
}

/* --- acceptance ------------------------------------------------------------ */

CF_TEST(users_sidebars_route_ids_bind_the_action) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/users/:user_id/sidebar(.:format)", 57,
                                  cf_action_users_sidebars_show) == CF_OK);
    CF_CHECK(cf_route_action(57) == cf_action_users_sidebars_show);
}

CF_TEST(users_sidebars_unauthenticated_redirects_to_sign_in) {
    sidebar_env env;
    CF_REQUIRE(env_open(&env));
    seed_fixture(env.scratch.db);
    cf_request req;
    cf_response resp;
    sidebar_get(&env, &req, &resp, "/users/me/sidebar", 0);
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, "Location: " ORIGIN "/session/new\r\n"));
    CF_CHECK(resp.body == NULL);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Before::default's deny_bots: a bot-key request is 403 before the action. */
CF_TEST(users_sidebars_bot_authentication_is_forbidden) {
    sidebar_env env;
    CF_REQUIRE(env_open(&env));
    seed_fixture(env.scratch.db);
    exec_sql(env.scratch.db,
             "UPDATE users SET bot_token = 'sidebarBotKey' WHERE id = "
             "394959859");
    cf_request req;
    cf_test_req_init(&req);
    req.path = SP("/users/me/sidebar");
    req.target = req.path;
    req.query = SP("bot_key=394959859-sidebarBotKey");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* `c.respond_to(&[&format::HTML])` runs before the read: a JSON client gets
 * 406. */
CF_TEST(users_sidebars_json_client_is_406) {
    sidebar_env env;
    CF_REQUIRE(env_open(&env));
    seed_fixture(env.scratch.db);
    cf_request req;
    cf_response resp;
    sidebar_get(&env, &req, &resp, "/users/me/sidebar.json", DAVID);
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_sidebars_david_matches_the_golden) {
    sidebar_env env;
    CF_REQUIRE(env_open(&env));
    seed_fixture(env.scratch.db);
    cf_request req;
    cf_response resp;
    sidebar_get(&env, &req, &resp, "/users/me/sidebar", DAVID);
    CF_REQUIRE(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    /* The page layout carries the stylesheet preload links. */
    CF_CHECK(head_contains(&resp, &req, "Link: <"));
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("sidebar_david", (const char *)body.ptr, body.len);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(users_sidebars_kevin_matches_the_golden) {
    sidebar_env env;
    CF_REQUIRE(env_open(&env));
    seed_fixture(env.scratch.db);
    cf_request req;
    cf_response resp;
    sidebar_get(&env, &req, &resp, "/users/me/sidebar", KEVIN);
    CF_REQUIRE(resp.status == 200);
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("sidebar_kevin", (const char *)body.ptr, body.len);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A Turbo-Frame request renders the frame layout (headless page), and the
 * frame layout carries no Link preload header. */
CF_TEST(users_sidebars_turbo_frame_matches_the_golden) {
    sidebar_env env;
    CF_REQUIRE(env_open(&env));
    seed_fixture(env.scratch.db);
    cf_request req;
    cf_response resp;
    cf_test_req_init(&req);
    req.path = SP("/users/me/sidebar");
    req.target = req.path;
    static char cookie[4096];
    {
        char token[64];
        snprintf(token, sizeof token, "sidebar-token-frame");
        make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                            token, DAVID);
    }
    CF_REQUIRE(cf_test_req_header(&req, SP("Cookie"), SP(cookie)) == CF_OK);
    CF_REQUIRE(cf_test_req_header(&req, SP("Turbo-Frame"),
                                  SP("user_sidebar")) == CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_REQUIRE(resp.status == 200);
    CF_CHECK(!head_contains(&resp, &req, "Link: <"));
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("sidebar_frame", (const char *)body.ptr, body.len);
    CF_CHECK(!span_contains(body, "<title>Campfire</title>"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* The source never reads params[:user_id]: any segment renders the signed-in
 * user's sidebar, numeric or not. */
CF_TEST(users_sidebars_ignores_the_user_id_segment) {
    sidebar_env env;
    CF_REQUIRE(env_open(&env));
    seed_fixture(env.scratch.db);

    const char *paths[3] = {"/users/999/sidebar", "/users/abc/sidebar",
                            "/users/me/sidebar.html"};
    for (size_t i = 0; i < 3; i++) {
        cf_request req;
        cf_response resp;
        sidebar_get(&env, &req, &resp, paths[i], DAVID);
        CF_CHECK(resp.status == 200);
        CF_REQUIRE(resp.body != NULL);
        cf_span body = cf_buf_span(resp.body);
        cf_golden_expect("sidebar_david", (const char *)body.ptr, body.len);
        cf_response_dispose(&resp);
    }
    env_close(&env);
}

/* `can_create_rooms = administrator? || !settings.restrict...`: with the
 * setting on, a member loses the new-room link and an administrator keeps
 * it. */
CF_TEST(users_sidebars_can_create_rooms_follows_the_account_setting) {
    sidebar_env env;
    CF_REQUIRE(env_open(&env));
    seed_fixture(env.scratch.db);
    exec_sql(env.scratch.db,
             "UPDATE accounts SET settings = "
             "'{\"restrict_room_creation_to_administrators\":true}'");

    cf_request req;
    cf_response resp;
    sidebar_get(&env, &req, &resp, "/users/me/sidebar", KEVIN);
    CF_REQUIRE(resp.status == 200);
    CF_CHECK(!body_contains(&resp, "rooms__new-btn"));
    cf_response_dispose(&resp);

    sidebar_get(&env, &req, &resp, "/users/me/sidebar", DAVID);
    CF_REQUIRE(resp.status == 200);
    CF_CHECK(body_contains(&resp, "href=\"/rooms/opens/new\""));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* The disclosed S02 gap: an attached avatar is still rendered through
 * `fresh_user_avatar_path` (the pinned source has no variant branch here),
 * and no Active Storage path appears in the sidebar. */
CF_TEST(users_sidebars_avatars_stay_token_paths_with_an_attachment) {
    sidebar_env env;
    CF_REQUIRE(env_open(&env));
    seed_fixture(env.scratch.db);
    exec_sql(env.scratch.db,
             "INSERT INTO active_storage_blobs (id, byte_size, checksum, "
             "content_type, created_at, filename, key, metadata, service_name) "
             "VALUES (1, 1234, NULL, 'image/png', "
             "'2026-09-26 13:00:20.000000', 'avatar.png', 'key1', NULL, "
             "'local')");
    char sql[256];
    snprintf(sql, sizeof sql,
             "INSERT INTO active_storage_attachments (id, blob_id, created_at, "
             "name, record_id, record_type) VALUES (1, 1, "
             "'2026-09-26 13:00:20.000000', 'avatar', %lld, 'User')",
             (long long)ANNA);
    exec_sql(env.scratch.db, sql);

    cf_request req;
    cf_response resp;
    sidebar_get(&env, &req, &resp, "/users/me/sidebar", DAVID);
    CF_REQUIRE(resp.status == 200);
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    /* The fixture render is unchanged: the token path for the user with the
     * attachment (Anna, updated_at v=20260926130022). */
    cf_golden_expect("sidebar_david", (const char *)body.ptr, body.len);
    CF_CHECK(!span_contains(body, "/rails/active_storage"));
    CF_CHECK(!span_contains(body, "representations"));
    CF_CHECK(!span_contains(body, "avatar.png"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
