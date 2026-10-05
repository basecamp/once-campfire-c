/* tests/actions/welcome_test.c — A-welcome acceptance: `welcome#show`
 * (route ID 1), docs/devel/implementation/contracts/controller-packets.md
 * "A-welcome" and 03-application.md's "welcome#show" slice row.
 *
 * The route double (tests/app/support/route_double.c) binds row 1 to
 * cf_action_welcome_show, so every case runs the real A00 dispatch path
 * (params, cookies, before-actions, format negotiation).
 *
 * State selection matrix from the card: unauthenticated -> sign-in redirect
 * (with no users as well; the setup leg is sessions#new's ensure_user_exists),
 * authenticated -> last-room selection (cookie room when a member, else the
 * original room; never a hardcoded id), authenticated without rooms -> the
 * rendered welcome page.  The render case compares the finished response body
 * against A02's golden/a/welcome.html fixture exactly. */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "http/http_internal.h"
#include "models/room.h"
#include "models/user.h"
#include "views.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"
#include "../views/support/golden.h"

#include <limits.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ORIGIN "http://campfire.test"

/* routes.json row 1's exact C symbol.  cf.h is frozen and no packet-action
 * header exists yet, so the test declares it (the integrator's routes.c
 * rebind needs the same declaration; reported in the evidence). */
cf_err cf_action_welcome_show(cf_ctx *ctx);

#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
/* golden/a facts.json root vapid_public_key */
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="

/* golden/a facts.json "welcome" case: user id/name and the account's
 * updated_at that produces the fixture's /account/logo?v=20260926130029. */
#define GOLDEN_USER_ID INT64_C(773523957)
#define GOLDEN_USER_NAME "New Bie"

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- scratch app + database ------------------------------------------------ */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
} welcome_env;

static bool env_open(welcome_env *env) {
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
    cf_test_routes_reset();
    if (cf_test_routes_add("GET", "/", 1, cf_action_welcome_show) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(welcome_env *env) {
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

static void seed_user(cf_db *db, int64_t id, const char *name, int role,
                      int status) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, created_at, email_address, name, role, "
             "status, updated_at) VALUES (%lld, "
             "'2026-09-26 13:00:29.000000', NULL, '%s', %d, %d, "
             "'2026-09-26 13:00:29.000000')",
             (long long)id, name, role, status);
    exec_sql(db, sql);
}

static void seed_bot(cf_db *db, int64_t id, const char *name,
                     const char *token) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, created_at, email_address, name, role, "
             "status, updated_at, bot_token) VALUES (%lld, "
             "'2026-09-26 13:00:29.000000', NULL, '%s', 2, 0, "
             "'2026-09-26 13:00:29.000000', '%s')",
             (long long)id, name, token);
    exec_sql(db, sql);
}

static void seed_golden_account(cf_db *db) {
    exec_sql(db,
             "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
             "name, settings, singleton_guard, updated_at) VALUES "
             "(873240054, '2026-09-26 13:00:29.000000', NULL, "
             "'CRMu-l8Ge-KB9B', '37signals', NULL, 0, "
             "'2026-09-26 13:00:29.000000')");
}

static void seed_room(cf_db *db, int64_t id, const char *name,
                      const char *created_at, int64_t creator_id) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (%lld, '%s', %lld, '%s', 'Rooms::Open', "
             "'%s')",
             (long long)id, created_at, (long long)creator_id, name,
             created_at);
    exec_sql(db, sql);
}

static void seed_membership(cf_db *db, int64_t id, int64_t room_id,
                            int64_t user_id) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO memberships (id, created_at, room_id, updated_at, "
             "user_id) VALUES (%lld, '2026-09-26 13:00:29.000000', %lld, "
             "'2026-09-26 13:00:29.000000', %lld)",
             (long long)id, (long long)room_id, (long long)user_id);
    exec_sql(db, sql);
}

/* A session row whose last_active_at is in the future, so restoring it never
 * needs the writer (cf_session_needs_resume is false). */
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

/* "session_token=<wire>" with the Rack form-escaping a base64 value needs
 * (same rule as tests/auth/test_session.c). */
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

static void append_cookie(char *out, size_t cap, const char *pair) {
    size_t at = strlen(out);
    snprintf(out + at, cap - at, "; %s", pair);
}

/* --- asset configuration for the exact golden render ----------------------- */

static int g_assets_ready = 0;

static bool head_contains(const cf_buf *buf, const char *needle) {
    cf_span span = cf_buf_span(buf);
    size_t len = strlen(needle);
    if (len == 0) return true;
    if (span.len < len) return false;
    for (size_t i = 0; i + len <= span.len; i++) {
        if (memcmp(span.ptr + i, needle, len) == 0) return true;
    }
    return false;
}

static bool response_head_contains(cf_response *resp, cf_request *req,
                                   const char *needle) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    bool found = head_contains(ser.headers, needle);
    cf_buf_release(ser.headers);
    return found;
}

static bool write_whole_file(const char *path, const char *bytes, size_t len) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    size_t wrote = len == 0 ? 0 : fwrite(bytes, 1, len, file);
    bool ok = wrote == len && fclose(file) == 0;
    return ok;
}

/* The asset build (tests/fixtures/assets) and the golden were rendered at
 * different reference revisions: the loadable importmap-tags.html differs
 * from golden/a facts.json in three digests (A02-foundation.md reports this).
 * The Rust golden runner injects the facts' importmap tags into its render
 * context; the C action reads the configured asset module, so configure it
 * with a scratch root whose importmap-tags.html is the golden's own block and
 * whose public/ tree is the pinned manifest.  The action's render then uses
 * exactly the golden's tags. */
static bool welcome_assets_setup(void) {
    if (g_assets_ready) return true;
    char dir[] = "/tmp/cf_welcome_assets_XXXXXX";
    if (mkdtemp(dir) == NULL) return false;

    char importmap_path[PATH_MAX];
    char link_path[PATH_MAX];
    int n = snprintf(importmap_path, sizeof importmap_path,
                     "%s/importmap-tags.html", dir);
    int m = snprintf(link_path, sizeof link_path, "%s/public", dir);
    if (n < 0 || (size_t)n >= sizeof importmap_path || m < 0 ||
        (size_t)m >= sizeof link_path) {
        rmdir(dir);
        return false;
    }
    bool ok = false;
    char *golden = NULL;
    size_t golden_len = 0;

    golden = cf_golden_read("a", "welcome", "html", &golden_len);
    if (golden == NULL) goto out;
    {
        static const char head[] = "<script type=\"importmap\"";
        static const char tail[] = "import \"application\"</script>";
        char *start = memmem(golden, golden_len, head, sizeof head - 1);
        char *end = start != NULL
                        ? memmem(start, golden_len - (size_t)(start - golden),
                                 tail, sizeof tail - 1)
                        : NULL;
        if (start == NULL || end == NULL) goto out;
        size_t block_len = (size_t)(end - start) + (sizeof tail - 1);
        if (!write_whole_file(importmap_path, start, block_len)) goto out;
    }
    {
        char resolved[PATH_MAX];
        if (realpath("tests/fixtures/assets/public", resolved) == NULL) {
            goto out;
        }
        if (symlink(resolved, link_path) != 0) goto out;
    }
    ok = cf_views_assets_configure(dir) == CF_OK;

out:
    free(golden);
    remove(importmap_path);
    remove(link_path);
    rmdir(dir);
    if (ok) g_assets_ready = 1;
    return ok;
}

/* --- helpers for the route-level cases ------------------------------------- */

static bool run_request(welcome_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

/* --- acceptance ------------------------------------------------------------ */

/* Route ID 1 is bound to this action, and the double's ordered match selects
 * it for GET /. */
CF_TEST(welcome_route_id_1_binds_the_action) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/", 1, cf_action_welcome_show) ==
               CF_OK);
    CF_CHECK(cf_route_action(1) == cf_action_welcome_show);
}

CF_TEST(welcome_unauthenticated_is_redirected_to_sign_in) {
    welcome_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", 1, 0);

    cf_request req;
    cf_test_req_init(&req);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(cf_test_routes_matches() == 1);
    CF_CHECK(resp.status == 302);
    CF_CHECK(response_head_contains(&resp, &req,
                                    "Location: " ORIGIN "/session/new\r\n"));
    /* The chain stored the return-to URL in the encrypted session cookie. */
    CF_CHECK(response_head_contains(&resp, &req, "_campfire_session="));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* No users at all: the same sign-in redirect; the first_run setup leg is
 * sessions#new's ensure_user_exists (SessionsController). */
CF_TEST(welcome_without_users_redirects_to_sign_in) {
    welcome_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    cf_test_req_init(&req);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(response_head_contains(&resp, &req,
                                    "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

static void seed_two_rooms(welcome_env *env, int64_t user_id) {
    /* Room 41 is newer than room 42, so neither insertion order nor id order
     * can produce the original-room answer by accident. */
    seed_room(env->scratch.db, 42, "All Talk", "2026-01-01 00:00:00.000000",
              user_id);
    seed_room(env->scratch.db, 41, "HQ", "2026-02-01 00:00:00.000000",
              user_id);
    seed_room(env->scratch.db, 99, "Other", "2026-03-01 00:00:00.000000", 2);
    seed_membership(env->scratch.db, 201, 42, user_id);
    seed_membership(env->scratch.db, 202, 41, user_id);
}

static void authenticated_request(welcome_env *env, const char *cookie_extra,
                                  cf_request *req, cf_response *resp) {
    static unsigned counter;
    char token[64];
    char header[4096];
    snprintf(token, sizeof token, "welcome-room-token-%u", counter++);
    make_session_cookie(env->config, env->scratch.db, header, sizeof header,
                        token, 1);
    if (cookie_extra != NULL) append_cookie(header, sizeof header,
                                            cookie_extra);
    cf_test_req_init(req);
    CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(header)) == CF_OK);
    CF_REQUIRE(run_request(env, req, resp));
}

CF_TEST(welcome_authenticated_redirects_to_the_original_room) {
    welcome_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", 1, 0);
    seed_two_rooms(&env, 1);

    cf_request req;
    cf_response resp;
    authenticated_request(&env, NULL, &req, &resp);
    CF_CHECK(resp.status == 302);
    CF_CHECK(response_head_contains(&resp, &req,
                                    "Location: " ORIGIN "/rooms/42\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(welcome_last_room_cookie_selects_that_room) {
    welcome_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", 1, 0);
    seed_two_rooms(&env, 1);

    cf_request req;
    cf_response resp;
    authenticated_request(&env, "last_room=41", &req, &resp);
    CF_CHECK(resp.status == 302);
    CF_CHECK(response_head_contains(&resp, &req,
                                    "Location: " ORIGIN "/rooms/41\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(welcome_last_room_cookie_outside_membership_falls_back) {
    welcome_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", 1, 0);
    seed_two_rooms(&env, 1);

    /* Room 99 exists but user 1 is not a member: the original room wins. */
    cf_request req;
    cf_response resp;
    authenticated_request(&env, "last_room=99", &req, &resp);
    CF_CHECK(resp.status == 302);
    CF_CHECK(response_head_contains(&resp, &req,
                                    "Location: " ORIGIN "/rooms/42\r\n"));
    cf_response_dispose(&resp);

    /* An unparsable cookie is the same as absent (Rails' integer cast). */
    cf_request req2;
    cf_response resp2;
    authenticated_request(&env, "last_room=abc", &req2, &resp2);
    CF_CHECK(resp2.status == 302);
    CF_CHECK(response_head_contains(&resp2, &req2,
                                    "Location: " ORIGIN "/rooms/42\r\n"));
    cf_response_dispose(&resp2);
    env_close(&env);
}

/* The pinned ruby_compat::integer_cast (crates/ruby/src/integer.rs:17-53),
 * used by concerns.rs `last_room_cookie`: trim Ruby ISSPACE (space, tab, LF,
 * VT, FF, CR), one optional sign, then must start with a digit; the value is
 * `String#to_i` (optional `0d`/`0D` prefix, one `_` allowed between digits,
 * stop at the first other byte) and is nil out of i64 range.  Verifier
 * scenario (A-welcome-verify.md discrepancy): member room 2, original room 9;
 * `0_2`, `0d2` and VT+`2` must all reach /rooms/2. */
CF_TEST(welcome_last_room_cookie_matches_the_pinned_integer_cast) {
    welcome_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", 1, 0);
    seed_room(env.scratch.db, 9, "All Talk", "2026-01-01 00:00:00.000000", 1);
    seed_room(env.scratch.db, 2, "HQ", "2026-02-01 00:00:00.000000", 1);
    seed_room(env.scratch.db, 5, "Lab", "2026-03-01 00:00:00.000000", 1);
    seed_membership(env.scratch.db, 201, 9, 1);
    seed_membership(env.scratch.db, 202, 2, 1);
    seed_membership(env.scratch.db, 203, 5, 1);

    struct cast_case {
        const char *cookie;
        const char *path;
    };
    static const struct cast_case cases[] = {
        {"last_room=2", "/rooms/2"},
        {"last_room=+2", "/rooms/2"},
        {"last_room=0002", "/rooms/2"},
        {"last_room=2abc", "/rooms/2"},
        {"last_room=0_2", "/rooms/2"},      /* '_' between two digits */
        {"last_room=0d2", "/rooms/2"},      /* String#to_i 0d prefix */
        {"last_room=0D2", "/rooms/2"},
        {"last_room=\x0b" "2", "/rooms/2"}, /* VT is Ruby ISSPACE */
        {"last_room=\x0c" "2", "/rooms/2"}, /* FF is Ruby ISSPACE */
        {"last_room=0_5", "/rooms/5"},
        {"last_room=0d5", "/rooms/5"},
        {"last_room=0d0_5", "/rooms/5"}, /* 0d prefix, then 0_5 -> 5 */
        {"last_room=5__6", "/rooms/5"},  /* a second '_' ends the number */
        {"last_room=2_5", "/rooms/9"},   /* parses 25 -> not a member */
        {"last_room=0x5", "/rooms/9"},   /* parses 0 -> not a member */
        {"last_room=abc", "/rooms/9"},
        {"last_room=", "/rooms/9"},
        {"last_room=9223372036854775808", "/rooms/9"},  /* i64 overflow -> nil */
        {"last_room=99999999999999999999", "/rooms/9"},
        {"last_room=-9223372036854775809", "/rooms/9"},
        {"last_room=\xc2\xa0" "2", "/rooms/9"}, /* NBSP is not ISSPACE */
        {"last_room=--2", "/rooms/9"},
        {"last_room=_2", "/rooms/9"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cf_request req;
        cf_response resp;
        authenticated_request(&env, cases[i].cookie, &req, &resp);
        CF_CHECK(resp.status == 302);
        char expected[64];
        snprintf(expected, sizeof expected, "Location: " ORIGIN "%s\r\n",
                 cases[i].path);
        if (!response_head_contains(&resp, &req, expected)) {
            fprintf(stderr, "  cast case %s: expected %s\n", cases[i].cookie,
                    expected);
        }
        CF_CHECK(response_head_contains(&resp, &req, expected));
        cf_response_dispose(&resp);
    }
    env_close(&env);
}

/* The redirect decision precedes format negotiation: a JSON client with rooms
 * still gets the redirect, not a 406. */
CF_TEST(welcome_redirect_precedes_format_negotiation) {
    welcome_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", 1, 0);
    seed_two_rooms(&env, 1);

    char header[4096];
    make_session_cookie(env.config, env.scratch.db, header, sizeof header,
                        "welcome-json-token", 1);
    cf_request req;
    cf_test_req_init(&req);
    CF_REQUIRE(cf_test_req_header(&req, SP("Cookie"), SP(header)) == CF_OK);
    CF_REQUIRE(cf_test_req_header(&req, SP("Accept"),
                                  SP("application/json")) == CF_OK);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(response_head_contains(&resp, &req,
                                    "Location: " ORIGIN "/rooms/42\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Authenticated without any room: the rendered page, compared byte-for-byte
 * against A02's golden/a/welcome.html (only Rails' CSRF tags are masked by
 * the shared golden comparator). */
CF_TEST(welcome_authenticated_without_rooms_matches_the_golden) {
    welcome_env env;
    CF_REQUIRE(env_open(&env));
    CF_REQUIRE(welcome_assets_setup());
    seed_golden_account(env.scratch.db);
    seed_user(env.scratch.db, GOLDEN_USER_ID, GOLDEN_USER_NAME, 0, 0);

    char header[4096];
    make_session_cookie(env.config, env.scratch.db, header, sizeof header,
                        "welcome-golden-token", GOLDEN_USER_ID);
    cf_request req;
    cf_test_req_init(&req);
    CF_REQUIRE(cf_test_req_header(&req, SP("Cookie"), SP(header)) == CF_OK);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(response_head_contains(
        &resp, &req, "Content-Type: text/html; charset=utf-8\r\n"));
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("welcome", (const char *)body.ptr, body.len);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A Turbo-Frame request gets turbo-rails' frame layout (head/content only),
 * byte-identical to the A02 frame renderer for the same context. */
CF_TEST(welcome_turbo_frame_request_renders_the_frame_layout) {
    welcome_env env;
    CF_REQUIRE(env_open(&env));
    CF_REQUIRE(welcome_assets_setup());
    seed_golden_account(env.scratch.db);
    seed_user(env.scratch.db, GOLDEN_USER_ID, GOLDEN_USER_NAME, 0, 0);

    char header[4096];
    make_session_cookie(env.config, env.scratch.db, header, sizeof header,
                        "welcome-frame-token", GOLDEN_USER_ID);
    cf_request req;
    cf_test_req_init(&req);
    CF_REQUIRE(cf_test_req_header(&req, SP("Cookie"), SP(header)) == CF_OK);
    CF_REQUIRE(cf_test_req_header(&req, SP("Turbo-Frame"),
                                  SP("user_sidebar")) == CF_OK);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);

    /* Control render through the A02 renderer for the same request context. */
    cf_response control_resp;
    cf_response_init(&control_resp);
    cf_request control_req;
    cf_test_req_init(&control_req);
    cf_ctx control;
    CF_REQUIRE(cf_ctx_create(&control, env.app, env.scratch.db, &control_req,
                             &control_resp) == CF_OK);
    control.identity.kind = CF_AUTH_SESSION;
    control.identity.user_id = GOLDEN_USER_ID;
    cf_view_layout_model layout = {0};
    CF_REQUIRE(cf_presenter_layout_load(&control, NULL, &layout) == CF_OK);
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, &control, &layout);
    cf_view_welcome_model model = {0};
    model.current_user_name = SP(GOLDEN_USER_NAME);
    cf_builder expected = {0};
    CF_REQUIRE(cf_view_welcome_frame(&view_ctx, &model, &expected) == CF_OK);
    cf_span body = cf_buf_span(resp.body);
    CF_CHECK(body.len == expected.len &&
             memcmp(body.ptr, expected.ptr, body.len) == 0);
    CF_CHECK(head_contains(resp.body, "id=\"message-area\""));
    CF_CHECK(!head_contains(resp.body, "user_sidebar"));
    CF_CHECK(!head_contains(resp.body, "<title>"));
    cf_builder_dispose(&expected);
    cf_view_layout_model_dispose(&layout);
    cf_ctx_destroy(&control);
    cf_response_dispose(&control_resp);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A non-HTML client without rooms: ActionController::UnknownFormat, 406. */
CF_TEST(welcome_unacceptable_format_is_406) {
    welcome_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, GOLDEN_USER_ID, GOLDEN_USER_NAME, 0, 0);

    char header[4096];
    make_session_cookie(env.config, env.scratch.db, header, sizeof header,
                        "welcome-406-token", GOLDEN_USER_ID);
    cf_request req;
    cf_test_req_init(&req);
    CF_REQUIRE(cf_test_req_header(&req, SP("Cookie"), SP(header)) == CF_OK);
    CF_REQUIRE(cf_test_req_header(&req, SP("Accept"),
                                  SP("application/json")) == CF_OK);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* deny_bots (Before::default): an authenticated bot is forbidden. */
CF_TEST(welcome_bot_authentication_is_forbidden) {
    welcome_env env;
    CF_REQUIRE(env_open(&env));
    /* Bot keys are "<id>-<token>"; the token part ends at the next '-'. */
    seed_bot(env.scratch.db, 7, "Bender Bot", "welcomebotkey");

    cf_request req;
    cf_test_req_init(&req);
    req.query = SP("bot_key=7-welcomebotkey");
    req.target = SP("/?bot_key=7-welcomebotkey");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    CF_CHECK(response_head_contains(&resp, &req,
                                    "Content-Type: text/html\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* Malformed parameter: `params[:bot_key]` of the wrong type is the chain's
 * internal error (500), not a crash or a rendered page. */
CF_TEST(welcome_malformed_bot_key_parameter_is_500) {
    welcome_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "David", 1, 0);

    cf_request req;
    cf_test_req_init(&req);
    req.query = SP("bot_key[]=x");
    req.target = SP("/?bot_key[]=x");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
