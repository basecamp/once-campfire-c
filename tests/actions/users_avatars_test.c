/* tests/actions/users_avatars_test.c — A-users-avatars acceptance:
 * `users/avatars#show` (route ID 53), per
 * docs/devel/implementation/contracts/controller-packets.md
 * "A-users-avatars" and the integrator's Phase 3 pull-forward ruling
 * (fallback branches only; route 54 `destroy` stays dev-501 and is not
 * implemented).
 *
 * Cases:
 *   - route binding: ID 53 resolves to the action;
 *   - no token -> `head :not_found` (404, empty body);
 *   - bad signature -> the same 404 head; a scratch falsification (evidence
 *     A-users-avatars.md) removed the verify and this case failed red;
 *   - unknown user (valid token) -> the generic 404 (reference public page);
 *   - bot -> the pinned default-bot-avatar.svg bytes, image/svg+xml, >100
 *     bytes, inline disposition;
 *   - user -> the initials SVG, byte-identical to the pinned Rust golden
 *     tests/fixtures/crates/views/tests/golden/a/avatar_david.svg;
 *   - ETag/If-None-Match -> 304 with the reference's headers (ETag and the
 *     normalized ETag-only Cache-Control); a non-matching validator -> 200;
 *   - Cache-Control present on the 200 (max-age=1800, public,
 *     stale-while-revalidate=604800); the pinned Rust `expires_in` emits no
 *     Expires header, asserted explicitly;
 *   - the ETag carries the template digest only when the request's formats
 *     find show.svg (Accept: image wildcard -> digest part; no Accept -> HTML, no
 *     digest part);
 *   - an attached avatar is the documented blocked arm (S02/S03): 500, no
 *     fallback bytes ever served;
 *   - unauthenticated -> the before-chain 302 to sign-in.
 *
 * The route double (tests/app/support/route_double.c) binds row 53 to the
 * real action, so every case runs the real A00 dispatch path.  cf.h and
 * src/actions/actions.h are integrator-owned and do not yet declare this
 * packet's symbol, so the entry point is declared here; the integrator's
 * routes.c rebind needs the same declaration (c_symbol
 * cf_action_users_avatars_show, route 53).
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
#include "routes.h"
#include "views.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

cf_err cf_action_users_avatars_show(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define HEX64_OTHER \
    "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="

/* The user facts of the pinned avatar goldens (facts.json cases
 * avatar_david / avatar_three_initials).  updated_at 2026-09-26 13:00:20
 * gives the cache version 20260926130020000000. */
#define DAVID_ID 127326141
#define DAVID_UPDATED "2026-09-26 13:00:20.000000"
/* sha256("users/127326141-20260926130020000000") truncated to 16 bytes,
 * weak: combine_etags with no template part (HTML format). */
#define DAVID_ETAG_HTML "W/\"ef4d06a7d85fda1a4f6dbdf4f82b3352\""
/* ... with the show.svg template digest part (the image wildcard Accept). */
#define DAVID_ETAG_SVG "W/\"b67cc631c7484d3b022700b7e3f38478\""
/* ... with turbo-rails' frame etagger part too (Turbo-Frame request). */
#define DAVID_ETAG_FRAME "W/\"b792f2955d59afbf15a95ef93e6c575f\""

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- scratch app + started writer ----------------------------------------- */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
} av_env;

static bool env_open(av_env *env) {
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
    if (cf_views_assets_configure("tests/fixtures/assets") != CF_OK) {
        return false;
    }
    cf_test_routes_reset();
    /* routes.json id 53.  The double's `(.:format)` grammar needs an
     * extension for a literal last segment, so the bare form is registered
     * as well; "/users/avatar" reaches the action with no user_id param at
     * all (the `param_str(...).unwrap_or_default()` arm). */
    if (cf_test_routes_add("GET", "/users/:user_id/avatar(.:format)", 53,
                           cf_action_users_avatars_show) != CF_OK ||
        cf_test_routes_add("GET", "/users/:user_id/avatar", 53,
                           cf_action_users_avatars_show) != CF_OK ||
        cf_test_routes_add("GET", "/users/avatar", 53,
                           cf_action_users_avatars_show) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(av_env *env) {
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

/* --- seeding --------------------------------------------------------------- */

static void seed_user(cf_db *db, int64_t id, const char *name,
                      const char *email, int role, const char *updated_at) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, updated_at) "
             "VALUES (%lld, NULL, NULL, '2026-09-26 13:00:20.000000', %s, "
             "'%s', NULL, %d, 0, '%s')",
             (long long)id, email != NULL ? "?" : "NULL", name, role,
             updated_at);
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
 * needs the writer. */
static void seed_session(cf_db *db, const char *token, int64_t user_id) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO sessions (created_at, ip_address, last_active_at, "
             "token, updated_at, user_agent, user_id) "
             "VALUES ('2040-01-01 00:00:00.000000', NULL, "
             "'2040-01-01 00:00:00.000000', '%s', "
             "'2040-01-01 00:00:00.000000', NULL, %lld)",
             token, (long long)user_id);
    exec_sql(db, sql);
}

/* An avatar attachment for `user_id` (the blocked arm). */
static void seed_avatar_attachment(cf_db *db, int64_t user_id) {
    exec_sql(db,
             "INSERT INTO active_storage_blobs (id, byte_size, checksum, "
             "content_type, created_at, filename, key, metadata, service_name)"
             " VALUES (1, 1234, NULL, 'image/png', "
             "'2026-09-26 13:00:20.000000', 'avatar.png', 'key1', NULL, "
             "'local')");
    char sql[256];
    snprintf(sql, sizeof sql,
             "INSERT INTO active_storage_attachments (id, blob_id, "
             "created_at, name, record_id, record_type) VALUES (1, 1, "
             "'2026-09-26 13:00:20.000000', 'avatar', %lld, 'User')",
             (long long)user_id);
    exec_sql(db, sql);
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

/* A valid `find_signed!(purpose: :avatar)` token for `user_id` under
 * `secret_key_base`. */
static void make_token(cf_span secret_key_base, int64_t user_id, cf_str *out) {
    CF_REQUIRE(cf_auth_signed_id_generate(secret_key_base, SP("User"), user_id,
                                          SP("avatar"), true, false, 0,
                                          out) == CF_OK);
}

/* --- request/response helpers ---------------------------------------------- */

static bool run_request(av_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

typedef struct {
    const char *accept;
    const char *if_none_match;
    const char *cookie;
    const char *turbo_frame;
} av_headers;

static void av_request(cf_request *req, const char *target,
                       const av_headers *headers) {
    cf_test_req_init(req);
    req->method = CF_GET;
    req->original_method = CF_GET;
    req->path = SP(target);
    req->target = SP(target);
    req->body = SP("");
    if (headers != NULL && headers->accept != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Accept"),
                                      SP(headers->accept)) == CF_OK);
    }
    if (headers != NULL && headers->turbo_frame != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Turbo-Frame"),
                                      SP(headers->turbo_frame)) == CF_OK);
    }
    if (headers != NULL && headers->if_none_match != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("If-None-Match"),
                                      SP(headers->if_none_match)) == CF_OK);
    }
    if (headers != NULL && headers->cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"),
                                      SP(headers->cookie)) == CF_OK);
    }
}

/* The value of response header `name` (case-insensitive), copied out of the
 * serialized wire form (which is released before returning); false when
 * absent or longer than cap. */
static bool response_header(cf_response *resp, cf_request *req,
                            const char *name, char *out, size_t cap) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    cf_span headers = cf_buf_span(ser.headers);
    bool found = false;
    size_t at = 0;
    bool first = true;
    while (at < headers.len) {
        size_t end = at;
        while (end + 1 < headers.len &&
               !(headers.ptr[end] == '\r' && headers.ptr[end + 1] == '\n')) {
            end++;
        }
        cf_span line = {headers.ptr + at, end - at};
        if (!first && line.len > strlen(name) &&
            line.ptr[strlen(name)] == ':') {
            bool match = true;
            for (size_t i = 0; i < strlen(name); i++) {
                unsigned char c = line.ptr[i];
                if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
                unsigned char want = (unsigned char)name[i];
                if (want >= 'A' && want <= 'Z') {
                    want = (unsigned char)(want - 'A' + 'a');
                }
                if (c != want) {
                    match = false;
                    break;
                }
            }
            if (match) {
                size_t start = strlen(name) + 1;
                while (start < line.len && line.ptr[start] == ' ') start++;
                size_t value_len = line.len - start;
                if (value_len < cap) {
                    memcpy(out, line.ptr + start, value_len);
                    out[value_len] = '\0';
                    found = true;
                }
                break;
            }
        }
        first = false;
        if (end + 1 >= headers.len) break;
        at = end + 2;
    }
    cf_buf_release(ser.headers);
    return found;
}

static bool header_equals(cf_response *resp, cf_request *req,
                          const char *name, const char *value) {
    char got[256];
    if (!response_header(resp, req, name, got, sizeof got)) {
        fprintf(stderr, "  header %s: absent, want '%s'\n", name, value);
        return false;
    }
    if (strcmp(got, value) != 0) {
        fprintf(stderr, "  header %s: got '%s', want '%s'\n", name, got,
                value);
        return false;
    }
    return true;
}

static bool header_absent(cf_response *resp, cf_request *req,
                          const char *name) {
    char got[256];
    if (response_header(resp, req, name, got, sizeof got)) {
        fprintf(stderr, "  header %s: present ('%s'), want absent\n", name,
                got);
        return false;
    }
    return true;
}

/* Byte copy of the response body, whatever its kind (BUFFER or FILE). */
static bool response_body(cf_response *resp, unsigned char **out,
                          size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (resp->body_kind == CF_BODY_NONE) return true;
    if (resp->body_kind == CF_BODY_BUFFER) {
        cf_span body = cf_buf_span(resp->body);
        unsigned char *copy = malloc(body.len != 0 ? body.len : 1);
        if (copy == NULL) return false;
        if (body.len != 0) memcpy(copy, body.ptr, body.len);
        *out = copy;
        *out_len = body.len;
        return true;
    }
    if (resp->body_kind == CF_BODY_FILE) {
        if (resp->file_length > (uint64_t)SIZE_MAX) return false;
        size_t len = (size_t)resp->file_length;
        unsigned char *copy = malloc(len != 0 ? len : 1);
        if (copy == NULL) return false;
        ssize_t got = pread(resp->file_fd, copy, len, (off_t)resp->file_offset);
        if (got < 0 || (size_t)got != len) {
            free(copy);
            return false;
        }
        *out = copy;
        *out_len = len;
        return true;
    }
    return false;
}

/* Read a fixture file (the pinned bot asset) into an owned buffer. */
static bool read_file_bytes(const char *path, unsigned char **out,
                            size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return false;
    }
    rewind(file);
    unsigned char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(file);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(buf);
        return false;
    }
    *out = buf;
    *out_len = got;
    return true;
}

/* --- acceptance ------------------------------------------------------------ */

CF_TEST(users_avatars_route_id_binds_the_action) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/users/:user_id/avatar", 53,
                                  cf_action_users_avatars_show) == CF_OK);
    CF_CHECK(cf_route_action(53) == cf_action_users_avatars_show);
}

/* No user_id parameter at all: `param_str("user_id").unwrap_or_default()` is
 * the empty token, so the signature check fails and `head :not_found`
 * answers (404, no body, the rendered format's content type). */
CF_TEST(users_avatars_missing_token_is_not_found) {
    av_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Member", "member@example.com", 0,
              DAVID_UPDATED);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session-token", 1);

    cf_request req;
    cf_response resp;
    av_headers headers = {.cookie = cookie};
    av_request(&req, "/users/avatar", &headers);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    CF_CHECK(resp.body_kind == CF_BODY_NONE && resp.body == NULL);
    CF_CHECK(header_equals(&resp, &req, "Content-Type", "text/html"));
    CF_CHECK(header_absent(&resp, &req, "ETag"));
    cf_response_dispose(&resp);
    env_close(&env);
}

/* A syntactically valid token signed with another key: never an avatar. */
CF_TEST(users_avatars_bad_signature_is_not_found) {
    av_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Member", "member@example.com", 0,
              DAVID_UPDATED);
    seed_user(env.scratch.db, DAVID_ID, "David", "david@37signals.com", 1,
              DAVID_UPDATED);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session-token", 1);
    cf_str token = {0};
    make_token(SP(HEX64_OTHER), DAVID_ID, &token);

    char target[512];
    snprintf(target, sizeof target, "/users/%.*s/avatar", (int)token.len,
             token.ptr);
    cf_request req;
    cf_response resp;
    av_headers headers = {.cookie = cookie};
    av_request(&req, target, &headers);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    CF_CHECK(resp.body_kind == CF_BODY_NONE);
    cf_response_dispose(&resp);
    cf_str_dispose(&token);
    env_close(&env);
}

/* A valid token for a user that does not exist: `Error::NotFound`, the
 * reference's public 404 page (a body, unlike the head 404 above). */
CF_TEST(users_avatars_unknown_user_is_not_found) {
    av_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Member", "member@example.com", 0,
              DAVID_UPDATED);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session-token", 1);
    cf_str token = {0};
    make_token((cf_span){(const unsigned char *)env.config->secret_key_base,
                         env.config->secret_key_base_len},
               999, &token);

    char target[512];
    snprintf(target, sizeof target, "/users/%.*s/avatar", (int)token.len,
             token.ptr);
    cf_request req;
    cf_response resp;
    av_headers headers = {.cookie = cookie};
    av_request(&req, target, &headers);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    /* Which 404 arm ran: the bad-token `head :not_found` above carries the
     * rendered format's content type; the mapped `Error::NotFound` path
     * answers a fresh, header-less response.  In the real table that path
     * renders public/404.html (H03's reference handler); the action bucket
     * links the route double, whose stand-in pins status/ownership only. */
    CF_CHECK(header_absent(&resp, &req, "Content-Type"));
    CF_CHECK(resp.body_kind == CF_BODY_NONE);
    cf_response_dispose(&resp);
    cf_str_dispose(&token);
    env_close(&env);
}

/* A bot user without an avatar: the pinned default-bot-avatar.svg bytes
 * (send_file, image/svg+xml, inline) — not initials. */
CF_TEST(users_avatars_bot_serves_default_avatar) {
    av_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Member", "member@example.com", 0,
              DAVID_UPDATED);
    seed_user(env.scratch.db, 2, "Bender Bot", NULL, 2, DAVID_UPDATED);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session-token", 1);
    cf_str token = {0};
    make_token((cf_span){(const unsigned char *)env.config->secret_key_base,
                         env.config->secret_key_base_len},
               2, &token);

    char target[512];
    snprintf(target, sizeof target, "/users/%.*s/avatar", (int)token.len,
             token.ptr);
    cf_request req;
    cf_response resp;
    av_headers headers = {.accept = "image/*", .cookie = cookie};
    av_request(&req, target, &headers);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(header_equals(&resp, &req, "Content-Type", "image/svg+xml"));
    CF_CHECK(header_equals(
        &resp, &req, "Content-Disposition",
        "inline; filename=\"default-bot-avatar.svg\"; "
        "filename*=UTF-8''default-bot-avatar.svg"));
    CF_CHECK(header_equals(&resp, &req, "content-transfer-encoding",
                           "binary"));
    CF_CHECK(header_absent(&resp, &req, "Vary"));

    /* The body is exactly the pinned asset file (resolved through the loaded
     * manifest, as the action resolves it). */
    cf_builder url = {0};
    CF_REQUIRE(cf_views_asset_path(SP("default-bot-avatar.svg"), &url) ==
               CF_OK);
    char path[512];
    int n = snprintf(path, sizeof path, "%s/public%.*s", cf_static_root(),
                     (int)url.len, url.ptr);
    CF_REQUIRE(n > 0 && (size_t)n < sizeof path);
    cf_builder_dispose(&url);
    unsigned char *want = NULL;
    size_t want_len = 0;
    CF_REQUIRE(read_file_bytes(path, &want, &want_len));
    CF_CHECK(want_len > 100);
    unsigned char *body = NULL;
    size_t body_len = 0;
    CF_REQUIRE(response_body(&resp, &body, &body_len));
    CF_CHECK(body_len == want_len && memcmp(body, want, want_len) == 0);
    free(body);
    free(want);

    cf_response_dispose(&resp);
    cf_str_dispose(&token);
    env_close(&env);
}

/* A non-bot user: the initials SVG, byte-identical to the pinned Rust
 * golden; the HTML format also pins the template-digest condition (no
 * digest part) and the 200 caching headers. */
CF_TEST(users_avatars_user_renders_initials_golden) {
    av_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Member", "member@example.com", 0,
              DAVID_UPDATED);
    seed_user(env.scratch.db, DAVID_ID, "David", "david@37signals.com", 1,
              DAVID_UPDATED);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session-token", 1);
    cf_str token = {0};
    make_token((cf_span){(const unsigned char *)env.config->secret_key_base,
                         env.config->secret_key_base_len},
               DAVID_ID, &token);

    char target[512];
    snprintf(target, sizeof target, "/users/%.*s/avatar", (int)token.len,
             token.ptr);
    cf_request req;
    cf_response resp;
    av_headers headers = {.cookie = cookie};
    av_request(&req, target, &headers);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(header_equals(&resp, &req, "Content-Type",
                           "image/svg+xml; charset=utf-8"));
    CF_CHECK(header_equals(&resp, &req, "Cache-Control",
                           "max-age=1800, public, stale-while-revalidate=604800"));
    CF_CHECK(header_equals(&resp, &req, "ETag", DAVID_ETAG_HTML));
    /* The pinned Rust expires_in writes only Cache-Control (+Date); no
     * Expires header exists to port (kit/src/ctx.rs:503-516). */
    CF_CHECK(header_absent(&resp, &req, "Expires"));

    FILE *golden = fopen(
        "tests/fixtures/crates/views/tests/golden/a/avatar_david.svg", "rb");
    CF_REQUIRE(golden != NULL);
    char golden_buf[4096];
    size_t golden_len = fread(golden_buf, 1, sizeof golden_buf, golden);
    fclose(golden);
    CF_REQUIRE(resp.body_kind == CF_BODY_BUFFER);
    cf_span body = cf_buf_span(resp.body);
    CF_REQUIRE(golden_len > 0);
    CF_CHECK(body.len == golden_len &&
             memcmp(body.ptr, golden_buf, golden_len) == 0);

    cf_response_dispose(&resp);
    cf_str_dispose(&token);
    env_close(&env);
}

/* ETag/If-None-Match: an image wildcard Accept adds the template digest to the ETag
 * (the format lookup finds show.svg); a matching validator is a 304 with the
 * ETag and the normalized ETag-only Cache-Control, no body and no
 * content type; a non-matching validator serves the 200 again. */
CF_TEST(users_avatars_etag_if_none_match_is_304) {
    av_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Member", "member@example.com", 0,
              DAVID_UPDATED);
    seed_user(env.scratch.db, DAVID_ID, "David", "david@37signals.com", 1,
              DAVID_UPDATED);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session-token", 1);
    cf_str token = {0};
    make_token((cf_span){(const unsigned char *)env.config->secret_key_base,
                         env.config->secret_key_base_len},
               DAVID_ID, &token);

    char target[512];
    snprintf(target, sizeof target, "/users/%.*s/avatar", (int)token.len,
             token.ptr);

    cf_request req;
    cf_response resp;
    av_headers headers = {.accept = "image/*", .cookie = cookie};
    av_request(&req, target, &headers);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(header_equals(&resp, &req, "ETag", DAVID_ETAG_SVG));
    CF_CHECK(header_equals(&resp, &req, "Cache-Control",
                           "max-age=1800, public, stale-while-revalidate=604800"));
    /* A render whose format came from Accept carries Vary: Accept (the
     * send_file arms do not). */
    CF_CHECK(header_equals(&resp, &req, "Vary", "Accept"));
    cf_response_dispose(&resp);

    cf_request req2;
    cf_response resp2;
    av_headers headers2 = {.accept = "image/*",
                           .if_none_match = DAVID_ETAG_SVG,
                           .cookie = cookie};
    av_request(&req2, target, &headers2);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 304);
    CF_CHECK(resp2.body_kind == CF_BODY_NONE);
    CF_CHECK(header_equals(&resp2, &req2, "ETag", DAVID_ETAG_SVG));
    CF_CHECK(header_equals(&resp2, &req2, "Cache-Control",
                           "max-age=0, private, must-revalidate"));
    CF_CHECK(header_absent(&resp2, &req2, "Content-Type"));
    cf_response_dispose(&resp2);

    cf_request req3;
    cf_response resp3;
    av_headers headers3 = {.accept = "image/*",
                           .if_none_match = "W/\"00000000000000000000000000000000\"",
                           .cookie = cookie};
    av_request(&req3, target, &headers3);
    CF_REQUIRE(run_request(&env, &req3, &resp3));
    CF_CHECK(resp3.status == 200);
    CF_CHECK(resp3.body_kind == CF_BODY_BUFFER);
    cf_response_dispose(&resp3);

    /* turbo-rails' frame etagger joins the same combine (only for a
     * Turbo-Frame request). */
    cf_request req4;
    cf_response resp4;
    av_headers headers4 = {.accept = "image/*",
                           .turbo_frame = "avatar_frame",
                           .cookie = cookie};
    av_request(&req4, target, &headers4);
    CF_REQUIRE(run_request(&env, &req4, &resp4));
    CF_CHECK(resp4.status == 200);
    CF_CHECK(header_equals(&resp4, &req4, "ETag", DAVID_ETAG_FRAME));
    cf_response_dispose(&resp4);

    cf_str_dispose(&token);
    env_close(&env);
}

/* The blocked attachment arm: a user WITH an avatar attachment never falls
 * through to the initials/bot bytes (S02/S03 owns the webp variant). */
CF_TEST(users_avatars_attachment_arm_is_blocked) {
    av_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, 1, "Member", "member@example.com", 0,
              DAVID_UPDATED);
    seed_user(env.scratch.db, DAVID_ID, "David", "david@37signals.com", 1,
              DAVID_UPDATED);
    seed_avatar_attachment(env.scratch.db, DAVID_ID);
    char cookie[4096];
    make_session_cookie(env.config, env.scratch.db, cookie, sizeof cookie,
                        "member-session-token", 1);
    cf_str token = {0};
    make_token((cf_span){(const unsigned char *)env.config->secret_key_base,
                         env.config->secret_key_base_len},
               DAVID_ID, &token);

    char target[512];
    snprintf(target, sizeof target, "/users/%.*s/avatar", (int)token.len,
             token.ptr);
    cf_request req;
    cf_response resp;
    av_headers headers = {.cookie = cookie};
    av_request(&req, target, &headers);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    CF_CHECK(resp.body_kind == CF_BODY_NONE);
    cf_response_dispose(&resp);
    cf_str_dispose(&token);
    env_close(&env);
}

/* Unauthenticated: the before chain redirects to sign-in before the token is
 * even read. */
CF_TEST(users_avatars_unauthenticated_redirects_to_sign_in) {
    av_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(env.scratch.db, DAVID_ID, "David", "david@37signals.com", 1,
              DAVID_UPDATED);
    cf_str token = {0};
    make_token((cf_span){(const unsigned char *)env.config->secret_key_base,
                         env.config->secret_key_base_len},
               DAVID_ID, &token);

    char target[512];
    snprintf(target, sizeof target, "/users/%.*s/avatar", (int)token.len,
             token.ptr);
    cf_request req;
    cf_response resp;
    av_request(&req, target, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(header_equals(&resp, &req, "Location",
                           ORIGIN "/session/new"));
    CF_CHECK(resp.body_kind == CF_BODY_NONE);
    cf_response_dispose(&resp);
    cf_str_dispose(&token);
    env_close(&env);
}

CF_TEST_MAIN()
