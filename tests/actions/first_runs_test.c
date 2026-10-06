/* tests/actions/first_runs_test.c — A-first_runs acceptance: `first_runs#show`
 * (route ID 4) and `first_runs#create` (route ID 8), per
 * docs/devel/implementation/contracts/controller-packets.md "A-first_runs"
 * and 03-application.md's "First complete slice" row:
 *
 *   - show only before the account exists (prevent_repeats -> root redirect);
 *   - the nested user field allowlist (name/avatar/email_address/password),
 *     parameter failure 400 for a missing :user, the NOT NULL name failure;
 *   - create writes the Campfire account, its administrator, the open
 *     "All Talk" room and the administrator's membership, then starts a
 *     session and redirects to root;
 *   - two concurrent setup requests never duplicate the account,
 *     administrator or original room (DB-04), exercised deterministically
 *     through the writer;
 *   - the render is compared against A02's golden/a/first_run.html.
 *
 * The route double (tests/app/support/route_double.c) binds rows 4/8 to the
 * actions, so 16 of the 18 cases run the real A00 dispatch path (params,
 * cookies, before-actions, format negotiation, generic error mapping). The
 * concurrent DB-04 case creates its contexts (route match included) on the
 * test thread and runs only the actions on the request threads, because the
 * double's match counter is not synchronized.
 *
 * cf.h is frozen and no packet-action header exists yet, so the two symbols
 * are declared here; the integrator's routes.c rebind needs the same
 * declarations (reported in docs/devel/evidence/A-first_runs.md). */
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
#include "models/room.h"
#include "models/user.h"
#include "views.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"
#include "avatar_test.h"
#include "../views/support/golden.h"

#include <limits.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

cf_err cf_action_first_runs_show(cf_ctx *ctx);
cf_err cf_action_first_runs_create(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
/* golden/a facts.json root vapid_public_key */
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="
#define PASSWORD "secret123456"

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- scratch app + database ------------------------------------------------ */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
} fr_env;

static bool env_open(fr_env *env) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;
    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", ORIGIN},
        {"SECRET_KEY_BASE", HEX64},
        {"VAPID_PUBLIC_KEY", VAPID_PUBLIC_KEY},
        {"DATABASE_PATH", env->scratch.path},
    };
    if (cf_config_parse(entries, 4, NULL, &env->config) != CF_OK) return false;
    /* Tests inject the low bcrypt cost (01-foundation-http.md); the action
     * reads it from the config at call time. */
    cf_config_test_set_bcrypt_cost(env->config, 4);
    if (cf_app_create(env->config, &env->app) != CF_OK) {
        cf_config_destroy(env->config);
        env->config = NULL;
        return false;
    }
    /* The controller writes through D02; nothing may call cf_write before the
     * writer is started. */
    if (cf_writer_start(env->app, env->config) != CF_OK) return false;
    env->writer_started = true;
    cf_test_routes_reset();
    /* routes.json ids 4/8.  The double's `(.:format)` grammar requires an
     * extension for a literal last segment (unlike the real table), so the
     * bare form is registered as well; reported in the evidence. */
    if (cf_test_routes_add("GET", "/first_run(.:format)", 4,
                           cf_action_first_runs_show) != CF_OK) {
        return false;
    }
    if (cf_test_routes_add("GET", "/first_run", 4,
                           cf_action_first_runs_show) != CF_OK) {
        return false;
    }
    if (cf_test_routes_add("POST", "/first_run(.:format)", 8,
                           cf_action_first_runs_create) != CF_OK) {
        return false;
    }
    if (cf_test_routes_add("POST", "/first_run", 8,
                           cf_action_first_runs_create) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(fr_env *env) {
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

static int64_t count_rows(cf_db *db, const char *table) {
    char sql[128];
    snprintf(sql, sizeof sql, "SELECT count(*) FROM %s", table);
    bool ok = false;
    int64_t value = cf_db_test_i64(cf_db_handle(db), sql, &ok);
    CF_REQUIRE(ok);
    return value;
}

/* --- request helpers ------------------------------------------------------- */

static bool run_request(fr_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

/* POST /first_run; `body`/`extra` are borrowed by the caller. */
static void post_first_run(cf_request *req, const char *body,
                           const char *extra_header, const char *extra_value) {
    cf_test_req_init(req);
    req->method = CF_POST;
    req->original_method = CF_POST;
    req->path = SP("/first_run");
    req->target = SP("/first_run");
    req->body = SP(body);
    CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                  SP("application/x-www-form-urlencoded")) ==
               CF_OK);
    CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                  SP("same-origin")) == CF_OK);
    if (extra_header != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP(extra_header),
                                      SP(extra_value)) == CF_OK);
    }
}

/* GET /first_run. */
static void get_first_run(cf_request *req) {
    cf_test_req_init(req);
    req->path = SP("/first_run");
    req->target = SP("/first_run");
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

/* --- assets for the exact golden render ------------------------------------ */

static int g_assets_ready = 0;

static bool write_whole_file(const char *path, const char *bytes, size_t len) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    size_t wrote = len == 0 ? 0 : fwrite(bytes, 1, len, file);
    bool ok = wrote == len && fclose(file) == 0;
    return ok;
}

/* The loadable importmap-tags.html differs from golden/a facts.json in digests
 * (A02-foundation.md): the golden runner injected the facts' tags, the C
 * action reads the configured asset module.  Configure the module with a
 * scratch root whose importmap-tags.html is the golden page's own block and
 * whose public/ tree is the pinned manifest, so the action renders with
 * exactly the golden's tags. */
static bool fr_assets_setup(void) {
    if (g_assets_ready) return true;
    char dir[] = "/tmp/cf_first_runs_assets_XXXXXX";
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

    golden = cf_golden_read("a", "first_run", "html", NULL);
    if (golden == NULL) goto out;
    {
        static const char head[] = "<script type=\"importmap\"";
        static const char tail[] = "import \"application\"</script>";
        char *start = strstr(golden, head);
        char *end = start != NULL ? strstr(start, tail) : NULL;
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

/* --- route wiring ---------------------------------------------------------- */

/* Routes.json rows 4/8 name these exact C symbols; the row binding in the
 * double proves the ids select them for GET/POST /first_run. */
CF_TEST(first_runs_route_ids_bind_the_actions) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/first_run(.:format)", 4,
                                  cf_action_first_runs_show) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("POST", "/first_run(.:format)", 8,
                                  cf_action_first_runs_create) == CF_OK);
    CF_CHECK(cf_route_action(4) == cf_action_first_runs_show);
    CF_CHECK(cf_route_action(8) == cf_action_first_runs_create);
}

/* --- show ------------------------------------------------------------------ */

CF_TEST(first_runs_show_renders_the_setup_page_before_the_account_exists) {
    fr_env env;
    CF_REQUIRE(env_open(&env));
    CF_REQUIRE(fr_assets_setup());

    cf_request req;
    get_first_run(&req);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(cf_test_routes_matches() == 1); /* row 4 selected for GET */
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    /* The application layout (not the frame layout) adds the stylesheet
     * preload header. */
    CF_CHECK(head_contains(&resp, &req, "Link: <"));
    CF_CHECK(head_contains(&resp, &req, "rel=preload; as=style; nopush"));
    cf_span body = cf_buf_span(resp.body);
    CF_CHECK(span_contains(body, "<title>Set up Campfire</title>"));
    CF_CHECK(span_contains(body, "<body class=\"signup\""));
    CF_CHECK(span_contains(body, "action=\"/first_run\""));
    CF_CHECK(span_contains(body, "name=\"user[avatar]\""));
    CF_CHECK(span_contains(body, "Set up Campfire</strong>"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* The finished response body matches A02's golden/a/first_run.html exactly
 * (the shared comparator masks only Rails' CSRF tags). */
CF_TEST(first_runs_show_matches_the_a02_golden) {
    fr_env env;
    CF_REQUIRE(env_open(&env));
    CF_REQUIRE(fr_assets_setup());

    cf_request req;
    get_first_run(&req);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    cf_span body = cf_buf_span(resp.body);
    cf_golden_expect("first_run", (const char *)body.ptr, body.len);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A Turbo-Frame request gets turbo-rails' frame layout: the head/content
 * body of the same renderer, without the application layout's preload
 * header. */
CF_TEST(first_runs_show_frame_renders_the_frame_layout) {
    fr_env env;
    CF_REQUIRE(env_open(&env));
    CF_REQUIRE(fr_assets_setup());

    cf_request req;
    get_first_run(&req);
    CF_REQUIRE(cf_test_req_header(&req, SP("Turbo-Frame"),
                                  SP("setup_frame")) == CF_OK);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(!head_contains(&resp, &req, "Link: <"));

    /* Control render through the A02 renderer for the same context. */
    cf_request control_req;
    get_first_run(&control_req);
    cf_response control_resp;
    cf_response_init(&control_resp);
    cf_ctx control;
    CF_REQUIRE(cf_ctx_create(&control, env.app, env.scratch.db, &control_req,
                             &control_resp) == CF_OK);
    cf_view_layout_model layout = {0};
    CF_REQUIRE(cf_presenter_layout_load(&control, NULL, &layout) == CF_OK);
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, &control, &layout);
    cf_builder expected = {0};
    CF_REQUIRE(cf_view_first_run_frame(&view_ctx, &expected) == CF_OK);
    cf_span body = cf_buf_span(resp.body);
    CF_CHECK(body.len == expected.len &&
             memcmp(body.ptr, expected.ptr, body.len) == 0);
    CF_CHECK(span_contains(body, "<form class=\"center max-width\""));
    CF_CHECK(!span_contains(body, "<!DOCTYPE html>"));
    cf_builder_dispose(&expected);
    cf_view_layout_model_dispose(&layout);
    cf_ctx_destroy(&control);
    cf_response_dispose(&control_resp);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A non-ASCII `Turbo-Frame` value fails http 1.5.0's `HeaderValue::to_str`,
 * so the pin reads the header as absent: the page render (with its preload
 * header) is used. */
CF_TEST(first_runs_show_non_ascii_turbo_frame_is_page) {
    fr_env env;
    CF_REQUIRE(env_open(&env));
    CF_REQUIRE(fr_assets_setup());

    cf_request req;
    get_first_run(&req);
    CF_REQUIRE(cf_test_req_header(
                   &req, SP("Turbo-Frame"),
                   (cf_span){(const unsigned char *)"\xC3\xA9", 2}) == CF_OK);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    cf_span body = cf_buf_span(resp.body);
    CF_CHECK(span_contains(body, "<!DOCTYPE html>"));
    CF_CHECK(head_contains(&resp, &req, "Link: <"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* prevent_repeats: once the account exists, show redirects to root. */
CF_TEST(first_runs_show_redirects_once_the_account_exists) {
    fr_env env;
    CF_REQUIRE(env_open(&env));
    exec_sql(env.scratch.db,
             "INSERT INTO accounts (id, created_at, join_code, name, "
             "singleton_guard, updated_at) VALUES (1, "
             "'2026-09-26 13:00:20.000000', 'CRMu-l8Ge-KB9B', 'Campfire', 0, "
             "'2026-09-26 13:00:20.000000')");

    cf_request req;
    get_first_run(&req);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, "Location: " ORIGIN "/\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* The redirect precedes format negotiation. */
CF_TEST(first_runs_show_redirect_precedes_format_negotiation) {
    fr_env env;
    CF_REQUIRE(env_open(&env));
    exec_sql(env.scratch.db,
             "INSERT INTO accounts (id, created_at, join_code, name, "
             "singleton_guard, updated_at) VALUES (1, "
             "'2026-09-26 13:00:20.000000', 'CRMu-l8Ge-KB9B', 'Campfire', 0, "
             "'2026-09-26 13:00:20.000000')");

    cf_request req;
    get_first_run(&req);
    CF_REQUIRE(cf_test_req_header(&req, SP("Accept"),
                                  SP("application/json")) == CF_OK);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* respond_to(&[HTML]) with no HTML in the Accept header: UnknownFormat, 406. */
CF_TEST(first_runs_show_unacceptable_format_is_406) {
    fr_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    get_first_run(&req);
    CF_REQUIRE(cf_test_req_header(&req, SP("Accept"),
                                  SP("application/json")) == CF_OK);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- create: parameter failures -------------------------------------------- */

/* params.require(:user): ParameterMissing -> 400, and no write happens. */
CF_TEST(first_runs_create_missing_user_is_400) {
    fr_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    post_first_run(&req, "name=Alice", NULL, NULL);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 400);
    CF_CHECK(count_rows(env.scratch.db, "accounts") == 0);
    CF_CHECK(count_rows(env.scratch.db, "users") == 0);
    CF_CHECK(count_rows(env.scratch.db, "rooms") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* users.name is NOT NULL: a missing permitted name is the reference's raised
 * NotNullViolation, an internal error, with nothing written. */
CF_TEST(first_runs_create_missing_name_is_500) {
    fr_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    post_first_run(&req,
                   "user[email_address]=alice@example.com&user[password]="
                   PASSWORD,
                   NULL, NULL);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    CF_CHECK(count_rows(env.scratch.db, "accounts") == 0);
    CF_CHECK(count_rows(env.scratch.db, "users") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* The always-on forgery protection: a cross-origin POST is 422 before the
 * action body runs. */
CF_TEST(first_runs_create_cross_origin_post_is_422) {
    fr_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    post_first_run(&req,
                   "user[name]=Alice&user[email_address]=alice@example.com&"
                   "user[password]=" PASSWORD,
                   "Origin", "http://evil.test");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    CF_CHECK(count_rows(env.scratch.db, "accounts") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- create: the success path ---------------------------------------------- */

/* Asserts every row the reference creates for one successful setup. */
static void check_setup_rows(fr_env *env, const char *name,
                             const char *email) {
    CF_CHECK(count_rows(env->scratch.db, "accounts") == 1);
    CF_CHECK(count_rows(env->scratch.db, "users") == 1);
    CF_CHECK(count_rows(env->scratch.db, "rooms") == 1);
    CF_CHECK(count_rows(env->scratch.db, "memberships") == 1);
    CF_CHECK(count_rows(env->scratch.db, "sessions") == 1);

    cf_account account = {0};
    bool found = false;
    CF_REQUIRE(cf_account_first(env->scratch.db, &found, &account) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(strcmp(account.name.ptr, "Campfire") == 0);
    CF_CHECK(account.join_code.len == 14);
    int64_t account_id = account.id;
    cf_account_dispose(&account);

    cf_user user = {0};
    found = false;
    CF_REQUIRE(cf_user_find_active_by_email_address(
                   env->scratch.db, (cf_str){(char *)email, strlen(email)},
                   &found, &user) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(strcmp(user.name.ptr, name) == 0);
    CF_CHECK(user.role == CF_ROLE_ADMINISTRATOR);
    CF_CHECK(user.status == CF_STATUS_ACTIVE);
    CF_CHECK(user.password_digest.present);
    CF_CHECK(cf_password_verify(
        (cf_str){(char *)PASSWORD, sizeof PASSWORD - 1},
        user.password_digest.value));
    int64_t user_id = user.id;
    cf_user_dispose(&user);

    cf_room room = {0};
    found = false;
    CF_REQUIRE(cf_room_original_for_user(env->scratch.db, user_id, &found,
                                         &room) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(room.room_type == CF_ROOM_OPEN);
    CF_CHECK(room.name.present && strcmp(room.name.value.ptr, "All Talk") == 0);
    int64_t room_id = room.id;
    cf_room_dispose(&room);

    /* The administrator's single membership is the granted open-room one. */
    char sql[256];
    snprintf(sql, sizeof sql,
             "SELECT count(*) FROM memberships WHERE room_id=%lld AND "
             "user_id=%lld",
             (long long)room_id, (long long)user_id);
    bool ok = false;
    CF_CHECK(cf_db_test_i64(cf_db_handle(env->scratch.db), sql, &ok) == 1);
    CF_CHECK(ok);

    /* The account is the singleton (id 1 on a fresh database). */
    CF_CHECK(account_id == 1);
}

CF_TEST(first_runs_create_creates_the_campfire_account_and_redirects) {
    fr_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    /* avatar= is the permitted "delete" value (a no-op for a new user); the
     * unknown nested keys must be dropped by the allowlist. */
    post_first_run(&req,
                   "user[name]=Alice&user[email_address]=alice@example.com&"
                   "user[password]=" PASSWORD "&user[avatar]=&"
                   "user[role]=0&user[status]=1&user[bot_token]=nope",
                   NULL, NULL);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(cf_test_routes_matches() == 1); /* row 8 selected for POST */
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, "Location: " ORIGIN "/\r\n"));
    /* start_new_session_for sets the signed session_token cookie. */
    CF_CHECK(head_contains(&resp, &req, "Set-Cookie: session_token="));
    check_setup_rows(&env, "Alice", "alice@example.com");
    cf_response_dispose(&resp);
    env_close(&env);
}

/* avatar absent: Assignment::Unchanged, the same successful setup. */
CF_TEST(first_runs_create_without_avatar_succeeds) {
    fr_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    post_first_run(&req,
                   "user[name]=Bob&user[email_address]=bob@example.com&"
                   "user[password]=" PASSWORD,
                   NULL, NULL);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    check_setup_rows(&env, "Bob", "bob@example.com");
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A non-empty string avatar is Assignment::Invalid in the reference: the
 * write raises and the whole transaction rolls back. */
CF_TEST(first_runs_create_with_an_unusable_avatar_leaves_no_rows) {
    fr_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    post_first_run(&req,
                   "user[name]=Alice&user[email_address]=alice@example.com&"
                   "user[password]=" PASSWORD "&user[avatar]=not-a-file",
                   NULL, NULL);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    CF_CHECK(count_rows(env.scratch.db, "accounts") == 0);
    CF_CHECK(count_rows(env.scratch.db, "users") == 0);
    CF_CHECK(count_rows(env.scratch.db, "rooms") == 0);
    CF_CHECK(count_rows(env.scratch.db, "memberships") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* The redirect does not negotiate a format: a JSON client still gets 302. */
CF_TEST(first_runs_create_non_html_client_still_redirects) {
    fr_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    post_first_run(&req,
                   "user[name]=Alice&user[email_address]=alice@example.com&"
                   "user[password]=" PASSWORD,
                   "Accept", "application/json");
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    check_setup_rows(&env, "Alice", "alice@example.com");
    cf_response_dispose(&resp);
    env_close(&env);
}

/* prevent_repeats runs before the parameters are read and before any write:
 * a request with no :user at all still redirects once the account exists. */
CF_TEST(first_runs_create_redirects_once_the_account_exists) {
    fr_env env;
    CF_REQUIRE(env_open(&env));
    exec_sql(env.scratch.db,
             "INSERT INTO accounts (id, created_at, join_code, name, "
             "singleton_guard, updated_at) VALUES (1, "
             "'2026-09-26 13:00:20.000000', 'CRMu-l8Ge-KB9B', 'Campfire', 0, "
             "'2026-09-26 13:00:20.000000')");

    cf_request req;
    post_first_run(&req, "name=Alice", NULL, NULL);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req, "Location: " ORIGIN "/\r\n"));
    CF_CHECK(count_rows(env.scratch.db, "users") == 0);
    CF_CHECK(count_rows(env.scratch.db, "rooms") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A whitespace-only :user is blank to params.require: 400, no write. */
CF_TEST(first_runs_create_blank_user_is_400) {
    fr_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    post_first_run(&req, "user=%20", NULL, NULL);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 400);
    CF_CHECK(count_rows(env.scratch.db, "accounts") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* name present but empty is Some("") in the reference (to_s), not a
 * NotNullViolation; email_address/password default to "" and the empty
 * password still gets a digest. */
CF_TEST(first_runs_create_empty_name_and_missing_optional_fields) {
    fr_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    post_first_run(&req, "user[name]=", NULL, NULL);
    cf_response resp;
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(count_rows(env.scratch.db, "accounts") == 1);
    CF_CHECK(count_rows(env.scratch.db, "users") == 1);

    cf_user user = {0};
    CF_REQUIRE(cf_user_find(env.scratch.db, 1, &user) == CF_OK);
    CF_CHECK(user.name.len == 0 && user.name.ptr != NULL &&
             user.name.ptr[0] == '\0');
    CF_CHECK(user.email_address.present);
    CF_CHECK(user.email_address.value.len == 0);
    CF_CHECK(user.password_digest.present);
    CF_CHECK(cf_password_verify((cf_str){(char *)"", 0},
                                user.password_digest.value));
    cf_user_dispose(&user);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- DB-04: the concurrent setup race -------------------------------------- */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool entered;
    bool release;
} fr_blocker;

static cf_err fr_blocker_cb(cf_tx *tx, void *arg) {
    (void)tx;
    fr_blocker *blocker = arg;
    pthread_mutex_lock(&blocker->mu);
    blocker->entered = true;
    pthread_cond_broadcast(&blocker->cv);
    while (!blocker->release) pthread_cond_wait(&blocker->cv, &blocker->mu);
    pthread_mutex_unlock(&blocker->mu);
    return CF_OK;
}

struct fr_blocker_job {
    fr_env *env;
    fr_blocker *blocker;
    cf_err rc;
};

static void *fr_blocker_thread(void *arg) {
    struct fr_blocker_job *job = arg;
    job->rc = cf_write(job->env->app, fr_blocker_cb, job->blocker);
    return NULL;
}

/* One racing request: the context is created (params parsed, route matched)
 * on the main thread; the thread opens its own reader connection and runs the
 * action, so no two threads touch the route double's match counter or share
 * a reader connection. */
typedef struct {
    fr_env *env;
    char body[256];
    cf_request req;
    cf_response resp;
    cf_ctx ctx;
    cf_err rc;
    unsigned status;
    bool has_session_cookie;
    bool location_root;
} fr_race_request;

static void *fr_race_thread(void *arg) {
    fr_race_request *job = arg;
    /* A reader connection belongs to the thread that opened it. */
    cf_db *reader = NULL;
    if (cf_db_open(job->env->scratch.path, true, &reader) != CF_OK) {
        job->rc = CF_INTERNAL;
        return NULL;
    }
    job->ctx.reader = reader;
    job->rc = cf_action_first_runs_create(&job->ctx);
    if (job->rc == CF_OK) {
        cf_err finish = cf_finish_cookies(&job->ctx);
        if (finish != CF_OK) job->rc = finish;
    }
    if (job->rc == CF_OK) {
        job->status = job->resp.status;
        job->has_session_cookie =
            head_contains(&job->resp, &job->req,
                          "Set-Cookie: session_token=");
        job->location_root =
            head_contains(&job->resp, &job->req, "Location: " ORIGIN "/\r\n");
    }
    cf_db_close(reader);
    return NULL;
}

/* Two concurrent setup requests pass prevent_repeats (the writer is held
 * inside a blocking callback) and then race their write: exactly one wins the
 * account insert; the loser's constraint failure redirects to root.  The
 * outcome state is deterministic: one account, one administrator, one
 * original room, one membership and one session — the winner's. */
CF_TEST(first_runs_concurrent_setup_yields_one_account_and_room) {
    fr_env env;
    CF_REQUIRE(env_open(&env));

    fr_blocker blocker;
    memset(&blocker, 0, sizeof blocker);
    CF_REQUIRE(pthread_mutex_init(&blocker.mu, NULL) == 0);
    CF_REQUIRE(pthread_cond_init(&blocker.cv, NULL) == 0);

    /* Hold the writer: both requests reach their write before either
     * commits. */
    pthread_t blocker_thread;
    struct fr_blocker_job blocker_job = {&env, &blocker, CF_OK};
    CF_REQUIRE(pthread_create(&blocker_thread, NULL, fr_blocker_thread,
                              &blocker_job) == 0);

    /* Wait until the writer is inside the blocker (bounded). */
    bool blocker_seen = false;
    for (int i = 0; i < 10000 && !blocker_seen; i++) {
        cf_writer_stats stats = {0};
        CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
        blocker_seen = stats.callback_running;
        if (!blocker_seen) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
    }
    CF_REQUIRE(blocker_seen);

    /* Two requests with their contexts created serially on this thread. */
    fr_race_request a = {.env = &env};
    fr_race_request b = {.env = &env};
    snprintf(a.body, sizeof a.body,
             "user[name]=Alice&user[email_address]=alice@example.com&"
             "user[password]=" PASSWORD);
    snprintf(b.body, sizeof b.body,
             "user[name]=Bob&user[email_address]=bob@example.com&"
             "user[password]=" PASSWORD);
    post_first_run(&a.req, a.body, NULL, NULL);
    post_first_run(&b.req, b.body, NULL, NULL);
    cf_response_init(&a.resp);
    cf_response_init(&b.resp);
    CF_REQUIRE(cf_ctx_create(&a.ctx, env.app, NULL, &a.req, &a.resp) == CF_OK);
    CF_REQUIRE(cf_ctx_create(&b.ctx, env.app, NULL, &b.req, &b.resp) == CF_OK);
    CF_CHECK(a.ctx.route.id == 8 && b.ctx.route.id == 8);
    pthread_t ta, tb;
    CF_REQUIRE(pthread_create(&ta, NULL, fr_race_thread, &a) == 0);
    CF_REQUIRE(pthread_create(&tb, NULL, fr_race_thread, &b) == 0);

    /* Both have passed prevent_repeats and enqueued their write behind the
     * blocker (bounded). */
    bool both_queued = false;
    for (int i = 0; i < 10000 && !both_queued; i++) {
        cf_writer_stats stats = {0};
        CF_REQUIRE(cf_writer_stats_get(env.app, &stats) == CF_OK);
        both_queued = stats.pending >= 2;
        if (!both_queued) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
    }
    CF_REQUIRE(both_queued);

    pthread_mutex_lock(&blocker.mu);
    blocker.release = true;
    pthread_cond_broadcast(&blocker.cv);
    pthread_mutex_unlock(&blocker.mu);

    CF_REQUIRE(pthread_join(ta, NULL) == 0);
    CF_REQUIRE(pthread_join(tb, NULL) == 0);
    CF_REQUIRE(pthread_join(blocker_thread, NULL) == 0);
    CF_CHECK(blocker_job.rc == CF_OK);

    /* Both requests answered; one succeeded and one lost the race. */
    CF_CHECK(a.rc == CF_OK && b.rc == CF_OK);
    CF_CHECK(a.status == 302 && b.status == 302);
    CF_CHECK(a.location_root && b.location_root);
    CF_CHECK(a.has_session_cookie != b.has_session_cookie);
    CF_CHECK(a.has_session_cookie || b.has_session_cookie);

    /* DB-04: one account, one administrator, one original room. */
    CF_CHECK(count_rows(env.scratch.db, "accounts") == 1);
    CF_CHECK(count_rows(env.scratch.db, "users") == 1);
    CF_CHECK(count_rows(env.scratch.db, "rooms") == 1);
    CF_CHECK(count_rows(env.scratch.db, "memberships") == 1);
    CF_CHECK(count_rows(env.scratch.db, "sessions") == 1);

    const char *winner_name = a.has_session_cookie ? "Alice" : "Bob";
    cf_user user = {0};
    CF_REQUIRE(cf_user_find(env.scratch.db, 1, &user) == CF_OK);
    CF_CHECK(strcmp(user.name.ptr, winner_name) == 0);
    CF_CHECK(user.role == CF_ROLE_ADMINISTRATOR);
    cf_user_dispose(&user);

    cf_ctx_destroy(&a.ctx);
    cf_ctx_destroy(&b.ctx);
    cf_response_dispose(&a.resp);
    cf_response_dispose(&b.resp);
    pthread_mutex_destroy(&blocker.mu);
    pthread_cond_destroy(&blocker.cv);
    env_close(&env);
}

CF_TEST(first_runs_multipart_avatar_is_committed_with_the_administrator) {
    fr_env env;
    CF_REQUIRE(env_open(&env));
    char root[] = "/tmp/cf-avatar-first-XXXXXX";
    avatar_test_root(env.config, root);
    const char *body = "--avatar\r\nContent-Disposition: form-data; name=\"user[name]\"\r\n\r\nAda\r\n--avatar\r\nContent-Disposition: form-data; name=\"user[email_address]\"\r\n\r\nada@example.com\r\n--avatar\r\nContent-Disposition: form-data; name=\"user[password]\"\r\n\r\nsecret\r\n--avatar\r\n" AVATAR_PART;
    cf_request req; cf_response resp;
    post_first_run(&req, body, NULL, NULL);
    req.headers[0].value = SP("multipart/form-data; boundary=avatar");
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    avatar_test_persisted(env.scratch.db, root);
    CF_CHECK(count_rows(env.scratch.db,"accounts") == 1);
    CF_CHECK(count_rows(env.scratch.db,"users") == 1);
    cf_response_dispose(&resp);
    env_close(&env);
    avatar_test_remove_tree(root);
}

CF_TEST_MAIN()
