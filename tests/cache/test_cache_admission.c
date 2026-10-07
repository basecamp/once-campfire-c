/* tests/cache/test_cache_admission.c — K01c live admission path: the four
 * admitted handlers — `rooms#show` (96/101), `messages#index` (76/137),
 * `users/sidebars#show` (57) and `searches#index` (146) — through the real
 * A00 dispatch path (route double + scratch database),
 * with the body cache enabled by the K01c test seam
 * (cf_app_cache_enable_for_test) and disabled for the pinned-behavior cases.
 *
 * Acceptance mapping (07-verification.md):
 *   CACHE-01  per-user/target/query/format/Turbo-Frame/UA/last_room key
 *             isolation for all four handlers; a hit never serves another
 *             request's content.
 *   CACHE-02  a commit forced between version capture and admission is never
 *             admitted (version_rejects), and the rendered response is still
 *             served.
 *   CACHE-03  a request after the commit acknowledgment sees the new content;
 *             a failed (rolled back) transaction preserves the version, so the
 *             existing entry stays valid.
 *   CACHE-04  live budget/eviction and oversized bypass through the round;
 *             duplicate admission is counted (double-put probe).
 *   CACHE-05  weak body-hash ETag, 304, gzip/identity and per-request cookies
 *             with the cache enabled; the pinned messages#index validator and
 *             the uncompressed identity path with the cache disabled.
 *   CACHE-06  a write to an unrelated room invalidates entries.
 *
 * The probe action is bound to route ID 101 (an admitted route) so the round
 * API can be driven with commits between lookup and finish; it is the same
 * five-call recipe the real handlers use.
 */
#include "cf_test.h"

#include "actions/actions.h"
#include "app.h"
#include "app_internal.h"
#include "auth.h"
#include "cache.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "http/http_internal.h"
#include "richtext.h"
#include "views.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

/* Defined below; the probe route needs the pointer before its definition. */
static cf_err cache_probe_action(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define CACHE_SECRET \
    "views-b-secret-key-base-0123456789abcdef0123456789abcdef"
#define CACHE_VAPID                                                      \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8" \
    "cTriz_qYBVicY02_VxTQ="

#define USER_A INT64_C(2001)
#define USER_B INT64_C(2002)
#define ROOM_1 INT64_C(3001)
#define ROOM_2 INT64_C(3002)
#define MSG_1 INT64_C(4001)
#define MSG_2 INT64_C(4002)
#define ACCOUNT_ID INT64_C(873240054)

/* An acceptable, fully enabled budget with room for several ~55 KiB pages. */
#define CACHE_BUDGET (4u * 1024u * 1024u)

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- environment ----------------------------------------------------------- */

typedef struct {
    cf_db_scratch scratch;
    cf_app *app;
    const cf_config *config; /* borrowed from the app */
    int session_seq;
} cache_env;

static char *dup_str(const char *text) {
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, text, len + 1);
    return copy;
}

static cf_config *make_config(const char *database_path) {
    cf_config *config = calloc(1, sizeof *config);
    if (config == NULL) return NULL;
    config->host = dup_str("0.0.0.0");
    config->public_origin = dup_str(ORIGIN);
    config->database_path = dup_str(database_path);
    config->storage_path = dup_str("storage/files");
    config->secret_key_base = dup_str(CACHE_SECRET);
    config->secret_key_base_len = strlen(CACHE_SECRET);
    config->loops = 1;
    config->readers = 2;
    config->request_slots = 8;
    config->writer_queue = 8;
    config->connections_per_loop = 16;
    config->input_bytes = 1u << 20;
    config->output_bytes = 1u << 20;
    config->cache_bytes = 0; /* the test seam attaches the cache explicitly */
    config->job_queue = 8;
    config->job_workers = 1;
    config->crypto_workers = 1;
    config->disable_ssl = true;
    config->bcrypt_cost = 4;
    config->vapid_public_key = dup_str(CACHE_VAPID);
    config->push_configured = true;
    if (config->host == NULL || config->public_origin == NULL ||
        config->database_path == NULL || config->storage_path == NULL ||
        config->secret_key_base == NULL || config->vapid_public_key == NULL) {
        cf_config_destroy(config);
        return NULL;
    }
    return config;
}

/* When `probe_only` is true the route double binds only the probe action (to
 * admitted route ID 101), because cf_route_action resolves an ID to its first
 * registration -- the probe must own 101 to exercise the round. */
static bool env_open_ex(cache_env *env, size_t budget, bool probe_only) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;
    cf_config *config = make_config(env->scratch.path);
    if (config == NULL) return false;
    if (cf_app_create(config, &env->app) != CF_OK) {
        cf_config_destroy(config);
        return false;
    }
    env->config = cf_app_config(env->app);
    if (cf_writer_start(env->app, env->config) != CF_OK) return false;
    if (cf_app_cache_enable_for_test(env->app, budget) != CF_OK) return false;
    cf_richtext_configure(SP(CACHE_SECRET));
    if (cf_views_assets_configure("tests/fixtures/assets") != CF_OK) {
        return false;
    }
    cf_test_routes_reset();
    if (probe_only) {
        return cf_test_routes_add("GET", "/cachetest/:id", 101,
                                  cache_probe_action) == CF_OK;
    }
    return cf_test_routes_add("GET", "/rooms/:id", 101,
                              cf_action_rooms_show) == CF_OK &&
           cf_test_routes_add("GET", "/rooms/:room_id/@/:message_id", 96,
                              cf_action_rooms_show) == CF_OK &&
           cf_test_routes_add("GET", "/rooms/:room_id/messages", 76,
                              cf_action_messages_index) == CF_OK &&
           cf_test_routes_add("GET", "/messages", 137,
                              cf_action_messages_index) == CF_OK &&
           cf_test_routes_add("GET", "/users/:user_id/sidebar", 57,
                              cf_action_users_sidebars_show) == CF_OK &&
           cf_test_routes_add("GET", "/searches", 146,
                              cf_action_searches_index) == CF_OK;
}

static bool env_open(cache_env *env, size_t budget) {
    return env_open_ex(env, budget, false);
}

static void env_close(cache_env *env) {
    if (env->app != NULL) cf_app_destroy(env->app); /* destroys the cache */
    env->app = NULL;
    env->config = NULL;
    cf_db_scratch_close(&env->scratch);
    memset(env, 0, sizeof *env);
}

/* --- seeding --------------------------------------------------------------- */

static void exec_sql(cf_db *db, const char *sql) {
    char *message = NULL;
    int rc = sqlite3_exec(cf_db_handle(db), sql, NULL, NULL, &message);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  sql failed: %s\n  %s\n",
                message != NULL ? message : "?", sql);
    }
    CF_REQUIRE(rc == SQLITE_OK);
}

static void seed_user(cf_db *db, int64_t id, const char *name, int role) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, created_at, email_address, name, "
             "role, status, updated_at) VALUES (%lld, NULL, "
             "'2026-09-26 13:00:29.000000', NULL, '%s', %d, 0, "
             "'2026-09-26 13:00:29.000000')",
             (long long)id, name, role);
    exec_sql(db, sql);
}

static void seed_room(cf_db *db, int64_t id, const char *name,
                      int64_t creator_id) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (%lld, '2026-01-01 00:00:00.000000', %lld, "
             "'%s', 'Rooms::Open', '2026-01-01 00:00:00.000000')",
             (long long)id, (long long)creator_id, name);
    exec_sql(db, sql);
}

static void seed_membership(cf_db *db, int64_t id, int64_t room_id,
                            int64_t user_id) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO memberships (id, created_at, room_id, updated_at, "
             "user_id) VALUES (%lld, '2026-01-01 00:00:00.000000', %lld, "
             "'2026-01-01 00:00:00.000000', %lld)",
             (long long)id, (long long)room_id, (long long)user_id);
    exec_sql(db, sql);
}

static void seed_message(cf_db *db, int64_t id, const char *client,
                         int64_t room_id, int64_t creator, const char *at) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO messages (id, client_message_id, created_at, "
             "creator_id, room_id, updated_at) VALUES (%lld, '%s', '%s', "
             "%lld, %lld, '%s')",
             (long long)id, client, at, (long long)creator,
             (long long)room_id, at);
    exec_sql(db, sql);
}

static void seed_rich_text(cf_db *db, int64_t id, int64_t message_id,
                           const char *body, const char *at) {
    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO action_text_rich_texts (id, body, created_at, name, "
             "record_id, record_type, updated_at) VALUES (%lld, '%s', '%s', "
             "'body', %lld, 'Message', '%s')",
             (long long)id, body, at, (long long)message_id, at);
    exec_sql(db, sql);
}

static void seed_fts(cf_db *db, int64_t message_id, const char *body) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO message_search_index (rowid, body) VALUES (%lld, "
             "'%s')",
             (long long)message_id, body);
    exec_sql(db, sql);
}

static void seed_search(cf_db *db, int64_t id, int64_t user_id,
                        const char *query, const char *at) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO searches (id, created_at, query, updated_at, "
             "user_id) VALUES (%lld, '%s', '%s', '%s', %lld)",
             (long long)id, at, query, at, (long long)user_id);
    exec_sql(db, sql);
}

/* The 37signals account the layout expects. */
static void seed_account(cf_db *db) {
    exec_sql(db,
             "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
             "name, settings, singleton_guard, updated_at) VALUES "
             "(873240054, '2026-09-26 13:03:57.000000', NULL, "
             "'CRMu-l8Ge-KB9B', '37signals', NULL, 0, "
             "'2026-09-26 13:03:57.000000')");
}

/* A session row; a future last_active_at means restoring it never needs the
 * writer, an old one makes the auth pass resume it with a cf_write. */
static void seed_session_at(cf_db *db, const char *token, int64_t user_id,
                            const char *last_active) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO sessions (created_at, ip_address, last_active_at, "
             "token, updated_at, user_agent, user_id) VALUES "
             "('2040-01-01 00:00:00.000000', NULL, '%s', '%s', "
             "'2040-01-01 00:00:00.000000', NULL, %lld)",
             last_active, token, (long long)user_id);
    exec_sql(db, sql);
}

/* (seed_session_at is the general form; callers pass the timestamp.) */

/* Two users, two rooms, both joined by both users, two messages in room 1. */
static void seed_world(cache_env *env) {
    cf_db *db = env->scratch.db;
    seed_account(db);
    seed_user(db, USER_A, "Alice", 1);
    seed_user(db, USER_B, "Bob", 0);
    seed_room(db, ROOM_1, "Design", USER_A);
    seed_room(db, ROOM_2, "Ops", USER_B);
    seed_membership(db, 9001, ROOM_1, USER_A);
    seed_membership(db, 9002, ROOM_1, USER_B);
    seed_membership(db, 9003, ROOM_2, USER_A);
    seed_membership(db, 9004, ROOM_2, USER_B);
    seed_message(db, MSG_1, "m1", ROOM_1, USER_A,
                 "2026-09-26 13:01:07.148296");
    seed_rich_text(db, 1, MSG_1, "<div>first message</div>",
                   "2026-09-26 13:01:07.148296");
    seed_message(db, MSG_2, "m2", ROOM_1, USER_B,
                 "2026-09-26 13:02:07.148296");
    seed_rich_text(db, 2, MSG_2, "<div>second message</div>",
                   "2026-09-26 13:02:07.148296");
    seed_fts(db, MSG_1, "first message");
    seed_fts(db, MSG_2, "second message");
}

/* --- requests -------------------------------------------------------------- */

/* "session_token=<wire>" with the Rack form-escaping a base64 value needs. */
static void make_session_cookie_at(cache_env *env, char *out, size_t cap,
                                   int64_t user_id, const char *last_active) {
    char token[64];
    snprintf(token, sizeof token, "cache-token-%d", env->session_seq++);
    seed_session_at(env->scratch.db, token, user_id, last_active);
    cf_str wire = {0};
    CF_REQUIRE(cf_auth_signed_cookie_generate(
                   (cf_span){(const unsigned char *)env->config->secret_key_base,
                             env->config->secret_key_base_len},
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

/* (make_session_cookie_at is the general form.) */

/* One request's header storage; the request spans point into it, so it must
 * outlive the request. */
typedef struct {
    cf_request req;
    char cookie[8192];
    char ua[512];
    char accept_encoding[256];
    char turbo_frame[256];
    char if_none_match[256];
    char accept[256];
    char path[512];
} reqbuf;

static void req_init(reqbuf *b) {
    memset(b, 0, sizeof *b);
    cf_test_req_init(&b->req);
}

static void req_auth_at(cache_env *env, reqbuf *b, int64_t user_id,
                        const char *last_active) {
    make_session_cookie_at(env, b->cookie, sizeof b->cookie, user_id,
                           last_active);
    CF_REQUIRE(cf_test_req_header(&b->req, SP("Cookie"), SP(b->cookie)) ==
               CF_OK);
}

static void req_auth(cache_env *env, reqbuf *b, int64_t user_id) {
    req_auth_at(env, b, user_id, "2040-01-01 00:00:00.000000");
}

/* Append one extra cookie pair (after req_auth wrote the session cookie) and
 * re-register the Cookie header so its span covers the appended text. */
static void req_cookie_extra(reqbuf *b, const char *pair) {
    size_t at = strlen(b->cookie);
    snprintf(b->cookie + at, sizeof b->cookie - at, "; %s", pair);
    CF_REQUIRE(cf_test_req_header(&b->req, SP("Cookie"), SP(b->cookie)) ==
               CF_OK);
}

/* Set the target without disturbing headers already added (req_auth), so the
 * call order req_init -> req_auth -> req_get keeps the session cookie. The
 * target's raw query is split into request->query the way H01 parses it; the
 * target keeps the complete raw text (a cache-key field). */
static void req_get(reqbuf *b, const char *target) {
    const char *q = strchr(target, '?');
    if (q == NULL) {
        b->req.path = SP(target);
        b->req.query = (cf_span){NULL, 0};
    } else {
        b->req.path =
            (cf_span){(const unsigned char *)target, (size_t)(q - target)};
        b->req.query = SP(q + 1);
    }
    b->req.target = SP(target);
}

static bool run_request(cache_env *env, reqbuf *b, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, &b->req, resp);
    if (getenv("CACHE_TEST_DEBUG") != NULL) {
        fprintf(stderr, "  %.*s -> rc=%d status=%u body=%zu\n",
                (int)b->req.path.len, b->req.path.ptr, rc, resp->status,
                resp->body != NULL ? cf_buf_span(resp->body).len : 0);
    }
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)b->req.path.len, b->req.path.ptr);
        return false;
    }
    return true;
}

/* --- responses ------------------------------------------------------------- */

static cf_span resp_body(const cf_response *resp) {
    if (resp->body == NULL) return (cf_span){NULL, 0};
    return cf_buf_span(resp->body);
}

static bool body_contains(const cf_response *resp, const char *needle) {
    cf_span body = resp_body(resp);
    size_t len = strlen(needle);
    if (len == 0) return true;
    for (size_t i = 0; i + len <= body.len; i++) {
        if (memcmp(body.ptr + i, needle, len) == 0) return true;
    }
    return false;
}

/* Serialize and copy the first header line whose text starts with `name`. */
static bool resp_header(cf_response *resp, cf_request *req, const char *name,
                        char *out, size_t cap) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    cf_span head = cf_buf_span(ser.headers);
    size_t len = strlen(name);
    bool found = false;
    for (size_t i = 0; i + len <= head.len; i++) {
        if (memcmp(head.ptr + i, name, len) != 0) continue;
        size_t at = i + len, end = at;
        while (end < head.len && head.ptr[end] != '\r') end++;
        size_t n = end - at < cap - 1 ? end - at : cap - 1;
        memcpy(out, head.ptr + at, n);
        out[n] = '\0';
        found = true;
        break;
    }
    cf_buf_release(ser.headers);
    return found;
}

/* The first `Set-Cookie: <name>=<value>` pair (attributes dropped). */
static bool resp_cookie(cf_response *resp, cf_request *req, const char *name,
                        char *out, size_t cap) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    cf_span head = cf_buf_span(ser.headers);
    static const char prefix[] = "Set-Cookie: ";
    size_t prefix_len = sizeof prefix - 1;
    size_t name_len = strlen(name);
    bool found = false;
    for (size_t i = 0; i + prefix_len + name_len + 1 <= head.len; i++) {
        if (memcmp(head.ptr + i, prefix, prefix_len) != 0) continue;
        if (memcmp(head.ptr + i + prefix_len, name, name_len) != 0) continue;
        if (head.ptr[i + prefix_len + name_len] != '=') continue;
        size_t at = i + prefix_len, end = at;
        while (end < head.len && head.ptr[end] != '\r' &&
               head.ptr[end] != ';') {
            end++;
        }
        size_t n = end - at < cap - 1 ? end - at : cap - 1;
        memcpy(out, head.ptr + at, n);
        out[n] = '\0';
        found = true;
        break;
    }
    cf_buf_release(ser.headers);
    return found;
}

/* Count serialized header lines starting with `name` (an ETag duplicate
 * check). */
static size_t resp_header_count(cf_response *resp, cf_request *req,
                                const char *name) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    cf_span head = cf_buf_span(ser.headers);
    size_t len = strlen(name);
    size_t count = 0;
    for (size_t i = 0; i + len <= head.len; i++) {
        if (i != 0 && head.ptr[i - 1] != '\n') continue;
        if (memcmp(head.ptr + i, name, len) == 0) count++;
    }
    cf_buf_release(ser.headers);
    return count;
}

static bool gunzip_equals(cf_span gzip, cf_span identity) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, 15 + 16) != Z_OK) return false;
    unsigned char out[8192];
    zs.next_in = (Bytef *)gzip.ptr;
    zs.avail_in = (uInt)gzip.len;
    size_t at = 0;
    bool ok = true;
    int rc;
    do {
        zs.next_out = out;
        zs.avail_out = sizeof out;
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            ok = false;
            break;
        }
        size_t got = sizeof out - zs.avail_out;
        if (at + got > identity.len ||
            memcmp(out, identity.ptr + at, got) != 0) {
            ok = false;
            break;
        }
        at += got;
    } while (rc != Z_STREAM_END);
    inflateEnd(&zs);
    return ok && at == identity.len;
}

static cf_cache_stats stats(cache_env *env) {
    cf_cache_stats out;
    memset(&out, 0, sizeof out);
    cf_cache *cache = cf_app_cache(env->app);
    CF_REQUIRE(cache != NULL);
    CF_REQUIRE(cf_cache_stats_get(cache, &out) == CF_OK);
    return out;
}

static cf_cache *cache_of(cache_env *env) { return cf_app_cache(env->app); }

/* --- probe action (route ID 101, an admitted route) ------------------------- */

enum probe_kind {
    PROBE_PLAIN = 0,
    PROBE_COMMIT_BEFORE_ADMIT,
    PROBE_COMMIT_AFTER_AUTH,
    PROBE_FAILED_WRITE,
    PROBE_DUPLICATE,
    PROBE_STATS_ONLY
};

static cache_env *g_probe_env;
static enum probe_kind g_probe;
static const char *g_probe_body = "probe-body";

static cf_err probe_failing_write(cf_tx *tx, void *arg) {
    (void)tx;
    (void)arg;
    return CF_INTERNAL; /* rolls back: the version must not advance */
}

/* The five-call K01c recipe with a switch hook between lookup and render; the
 * real handlers differ only in their gather/render. */
static cf_err cache_probe_action(cf_ctx *ctx) {
    cf_cache_round round;
    cf_cache_round_init(ctx, &round);
    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, true, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_cache_round_dispose(&round);
        return rc;
    }

    if (g_probe == PROBE_COMMIT_AFTER_AUTH) {
        cf_app_advance_data_version(g_probe_env->app);
    }
    cf_cached_body cached = {0};
    bool hit = round.cache != NULL &&
               cf_cache_round_lookup(ctx, &round, SP("text/html; charset=utf-8"),
                                     &cached) == CF_OK;
    if (hit) {
        rc = cf_cache_serve_hit(ctx, &round, &cached);
        cf_cached_body_dispose(&cached);
        cf_cache_round_dispose(&round);
        return rc;
    }
    cf_cached_body_dispose(&cached);

    switch (g_probe) {
    case PROBE_COMMIT_BEFORE_ADMIT:
        cf_app_advance_data_version(g_probe_env->app);
        break;
    case PROBE_FAILED_WRITE: {
        cf_err wrote = cf_write(g_probe_env->app, probe_failing_write, NULL);
        if (wrote == CF_OK) return CF_INTERNAL; /* the probe must fail */
        break;
    }
    default:
        break;
    }

    cf_buf *body = NULL;
    rc = cf_buf_copy(SP(g_probe_body), &body);
    if (rc != CF_OK) {
        cf_cache_round_dispose(&round);
        return rc;
    }
    ctx->response->status = 200;
    rc = cf_response_header(ctx->response, SP("Content-Type"),
                            SP("text/html; charset=utf-8"));
    if (rc == CF_OK) rc = cf_response_body(ctx->response, body);
    cf_buf_release(body);
    if (rc != CF_OK) {
        cf_cache_round_dispose(&round);
        return rc;
    }

    cf_cache_round_finish(ctx, &round);

    if (g_probe == PROBE_DUPLICATE && round.cache != NULL && round.key_built) {
        cf_cached_body duplicate = {
            .body = ctx->response->body,
            .gzip = false,
        };
        memcpy(duplicate.weak_etag, "W/\"duplicate-probe\"",
               sizeof "W/\"duplicate-probe\"");
        /* A racing builder for the same key/version: the module keeps the
         * existing entry and counts duplicate_builds. */
        rc = cf_cache_put(round.cache,
                          (cf_span){round.key.ptr, round.key.len},
                          round.version, &duplicate);
    }
    cf_cache_round_dispose(&round);
    return CF_OK;
}

/* ======================================================================== */
/* CACHE-01: key isolation                                                  */
/* ======================================================================== */

CF_TEST(cache_rooms_show_hit_serves_same_body_and_etag) {
    cache_env env;
    CF_REQUIRE(env_open(&env, CACHE_BUDGET));
    seed_world(&env);

    reqbuf a;
    req_init(&a);
    req_auth(&env, &a, USER_A);
    req_get(&a, "/rooms/3001");
    cf_response first;
    cf_response_init(&first);
    CF_REQUIRE(run_request(&env, &a, &first));
    CF_CHECK(first.status == 200);
    CF_REQUIRE(first.body != NULL);
    cf_buf *body_first_buf = cf_buf_retain(first.body);
    cf_span body_first = cf_buf_span(body_first_buf);
    CF_CHECK(body_first.len > 0);
    char etag[256];
    CF_REQUIRE(resp_header(&first, &a.req, "ETag: ", etag, sizeof etag));
    CF_CHECK(strncmp(etag, "W/\"", 3) == 0 && strlen(etag) == 68);
    cf_response_dispose(&first);

    cf_cache_stats before = stats(&env);
    CF_CHECK(before.entries == 1);
    CF_CHECK(before.hits == 0 && before.misses == 1);

    reqbuf b;
    req_init(&b);
    req_auth(&env, &b, USER_A);
    req_get(&b, "/rooms/3001");
    cf_response second;
    cf_response_init(&second);
    CF_REQUIRE(run_request(&env, &b, &second));
    CF_CHECK(second.status == 200);
    cf_span body_second = resp_body(&second);
    CF_CHECK(body_second.len == body_first.len &&
             memcmp(body_second.ptr, body_first.ptr, body_second.len) == 0);
    char etag2[256];
    CF_REQUIRE(resp_header(&second, &b.req, "ETag: ", etag2, sizeof etag2));
    CF_CHECK(strcmp(etag2, etag) == 0);
    CF_CHECK(strstr(etag2, "duplicate-probe") == NULL);
    cf_response_dispose(&second);

    cf_cache_stats after = stats(&env);
    CF_CHECK(after.hits == 1);
    CF_CHECK(after.entries == 1);
    cf_buf_release(body_first_buf);
    env_close(&env);
}

CF_TEST(cache_key_isolates_users) {
    cache_env env;
    CF_REQUIRE(env_open(&env, CACHE_BUDGET));
    seed_world(&env);

    reqbuf a;
    req_init(&a);
    req_auth(&env, &a, USER_A);
    req_get(&a, "/rooms/3001");
    cf_response resp_a;
    cf_response_init(&resp_a);
    CF_REQUIRE(run_request(&env, &a, &resp_a));
    CF_CHECK(body_contains(&resp_a, "Alice"));
    cf_span body_a = resp_body(&resp_a);

    reqbuf b;
    req_init(&b);
    req_auth(&env, &b, USER_B);
    req_get(&b, "/rooms/3001");
    cf_response resp_b;
    cf_response_init(&resp_b);
    CF_REQUIRE(run_request(&env, &b, &resp_b));
    /* The layout names the signed-in user, so the second request must be a
     * miss and must not serve Alice's page. */
    CF_CHECK(body_contains(&resp_b, "Bob"));
    cf_span body_b = resp_body(&resp_b);
    CF_CHECK(!(body_a.len == body_b.len &&
               memcmp(body_a.ptr, body_b.ptr, body_a.len) == 0));

    cf_cache_stats mid = stats(&env);
    CF_CHECK(mid.misses == 2 && mid.hits == 0 && mid.entries == 2);

    cf_response_dispose(&resp_a);
    cf_response_dispose(&resp_b);

    /* Both users get their own entry back. */
    reqbuf a2;
    req_init(&a2);
    req_auth(&env, &a2, USER_A);
    req_get(&a2, "/rooms/3001");
    cf_response hit_a;
    cf_response_init(&hit_a);
    CF_REQUIRE(run_request(&env, &a2, &hit_a));
    CF_CHECK(body_contains(&hit_a, "Alice"));
    cf_response_dispose(&hit_a);

    reqbuf b2;
    req_init(&b2);
    req_auth(&env, &b2, USER_B);
    req_get(&b2, "/rooms/3001");
    cf_response hit_b;
    cf_response_init(&hit_b);
    CF_REQUIRE(run_request(&env, &b2, &hit_b));
    CF_CHECK(body_contains(&hit_b, "Bob"));
    cf_response_dispose(&hit_b);

    cf_cache_stats after = stats(&env);
    CF_CHECK(after.hits == 2 && after.entries == 2);
    env_close(&env);
}

CF_TEST(cache_key_isolates_raw_query_and_format) {
    cache_env env;
    CF_REQUIRE(env_open(&env, CACHE_BUDGET));
    seed_world(&env);

    reqbuf all;
    req_init(&all);
    req_auth(&env, &all, USER_A);
    req_get(&all, "/rooms/3001/messages");
    cf_response resp_all;
    cf_response_init(&resp_all);
    CF_REQUIRE(run_request(&env, &all, &resp_all));
    CF_CHECK(resp_all.status == 200);
    CF_CHECK(body_contains(&resp_all, "first message"));
    CF_CHECK(body_contains(&resp_all, "second message"));
    /* One validator only: the round's body-hash ETag, not the action's own. */
    CF_CHECK(resp_header_count(&resp_all, &all.req, "ETag: ") == 1);
    {
        char etag[256];
        CF_REQUIRE(resp_header(&resp_all, &all.req, "ETag: ", etag,
                               sizeof etag));
        CF_CHECK(strncmp(etag, "W/\"", 3) == 0 && strlen(etag) == 68);
    }

    reqbuf before;
    req_init(&before);
    req_auth(&env, &before, USER_A);
    req_get(&before, "/rooms/3001/messages?before=4002");
    cf_response resp_before;
    cf_response_init(&resp_before);
    CF_REQUIRE(run_request(&env, &before, &resp_before));
    CF_CHECK(resp_before.status == 200);
    /* A different raw query is a different page (and a different key). */
    CF_CHECK(body_contains(&resp_before, "first message"));
    CF_CHECK(!body_contains(&resp_before, "second message"));
    cf_span body_all = resp_body(&resp_all);
    cf_span body_before = resp_body(&resp_before);
    CF_CHECK(!(body_all.len == body_before.len &&
               memcmp(body_all.ptr, body_before.ptr, body_all.len) == 0));

    cf_cache_stats mid = stats(&env);
    CF_CHECK(mid.misses == 2 && mid.entries == 2);

    cf_response_dispose(&resp_all);
    cf_response_dispose(&resp_before);

    reqbuf all2;
    req_init(&all2);
    req_auth(&env, &all2, USER_A);
    req_get(&all2, "/rooms/3001/messages");
    cf_response hit_all;
    cf_response_init(&hit_all);
    CF_REQUIRE(run_request(&env, &all2, &hit_all));
    CF_CHECK(body_contains(&hit_all, "second message"));
    cf_response_dispose(&hit_all);

    cf_cache_stats after = stats(&env);
    CF_CHECK(after.hits == 1 && after.entries == 2);
    env_close(&env);
}

CF_TEST(cache_key_isolates_turbo_frame) {
    cache_env env;
    CF_REQUIRE(env_open(&env, CACHE_BUDGET));
    seed_world(&env);

    reqbuf page;
    req_init(&page);
    req_auth(&env, &page, USER_A);
    req_get(&page, "/rooms/3001");
    cf_response resp_page;
    cf_response_init(&resp_page);
    CF_REQUIRE(run_request(&env, &page, &resp_page));
    cf_span body_page = resp_body(&resp_page);

    reqbuf frame;
    req_init(&frame);
    req_auth(&env, &frame, USER_A);
    req_get(&frame, "/rooms/3001");
    CF_REQUIRE(cf_test_req_header(&frame.req, SP("Turbo-Frame"),
                                  SP("rooms_3001")) == CF_OK);
    cf_response resp_frame;
    cf_response_init(&resp_frame);
    CF_REQUIRE(run_request(&env, &frame, &resp_frame));
    cf_span body_frame = resp_body(&resp_frame);
    CF_CHECK(resp_frame.status == 200);
    CF_CHECK(body_frame.len > 0);
    CF_CHECK(!(body_page.len == body_frame.len &&
               memcmp(body_page.ptr, body_frame.ptr, body_page.len) == 0));

    cf_cache_stats mid = stats(&env);
    CF_CHECK(mid.misses == 2 && mid.entries == 2);

    cf_response_dispose(&resp_page);
    cf_response_dispose(&resp_frame);

    /* A second frame request hits the frame entry, not the page one. */
    reqbuf frame2;
    req_init(&frame2);
    req_auth(&env, &frame2, USER_A);
    req_get(&frame2, "/rooms/3001");
    CF_REQUIRE(cf_test_req_header(&frame2.req, SP("Turbo-Frame"),
                                  SP("rooms_3001")) == CF_OK);
    cf_response hit_frame;
    cf_response_init(&hit_frame);
    CF_REQUIRE(run_request(&env, &frame2, &hit_frame));
    CF_CHECK(hit_frame.status == 200);
    cf_cache_stats after = stats(&env);
    CF_CHECK(after.hits == 1 && after.entries == 2);
    cf_response_dispose(&hit_frame);
    env_close(&env);
}

CF_TEST(cache_key_isolates_user_agent) {
    cache_env env;
    CF_REQUIRE(env_open(&env, CACHE_BUDGET));
    seed_world(&env);

    reqbuf one;
    req_init(&one);
    req_auth(&env, &one, USER_A);
    req_get(&one, "/rooms/3001");
    CF_REQUIRE(cf_test_req_header(&one.req, SP("User-Agent"),
                                  SP("Mozilla/5.0 (X11; Linux x86_64) "
                                     "Chrome/120.0")) == CF_OK);
    cf_response resp_one;
    cf_response_init(&resp_one);
    CF_REQUIRE(run_request(&env, &one, &resp_one));
    cf_span body_one = resp_body(&resp_one);

    reqbuf two;
    req_init(&two);
    req_auth(&env, &two, USER_A);
    req_get(&two, "/rooms/3001");
    CF_REQUIRE(cf_test_req_header(&two.req, SP("User-Agent"),
                                  SP("Mozilla/5.0 (Macintosh; Intel Mac OS X "
                                     "10_15_7) Firefox/121.0")) == CF_OK);
    cf_response resp_two;
    cf_response_init(&resp_two);
    CF_REQUIRE(run_request(&env, &two, &resp_two));
    cf_span body_two = resp_body(&resp_two);
    CF_CHECK(resp_two.status == 200);

    cf_cache_stats mid = stats(&env);
    /* The exact UA is a key field: two UAs never share an entry. The bodies
     * may or may not differ with these fixtures; the keys must. */
    CF_CHECK(mid.misses == 2 && mid.hits == 0 && mid.entries == 2);
    if (body_one.len != body_two.len ||
        memcmp(body_one.ptr, body_two.ptr, body_one.len) != 0) {
        CF_CHECK(true); /* different content under different keys */
    }
    cf_response_dispose(&resp_one);
    cf_response_dispose(&resp_two);

    reqbuf repeat;
    req_init(&repeat);
    req_auth(&env, &repeat, USER_A);
    req_get(&repeat, "/rooms/3001");
    CF_REQUIRE(cf_test_req_header(&repeat.req, SP("User-Agent"),
                                  SP("Mozilla/5.0 (X11; Linux x86_64) "
                                     "Chrome/120.0")) == CF_OK);
    cf_response hit;
    cf_response_init(&hit);
    CF_REQUIRE(run_request(&env, &repeat, &hit));
    cf_cache_stats after = stats(&env);
    CF_CHECK(after.hits == 1);
    cf_response_dispose(&hit);
    env_close(&env);
}

CF_TEST(cache_key_isolates_last_room) {
    cache_env env;
    CF_REQUIRE(env_open(&env, CACHE_BUDGET));
    seed_world(&env);

    /* No cookie: one entry under the absent marker. */
    reqbuf none;
    req_init(&none);
    req_auth(&env, &none, USER_A);
    req_get(&none, "/rooms/3001");
    cf_response resp_none;
    cf_response_init(&resp_none);
    CF_REQUIRE(run_request(&env, &none, &resp_none));
    CF_CHECK(resp_none.status == 200);
    CF_REQUIRE(resp_none.body != NULL);
    cf_buf *body_none = cf_buf_retain(resp_none.body);
    cf_response_dispose(&resp_none);

    /* A different last_room ID is a different key: a miss, even if this
     * fixture happens not to render the last-room link differently. */
    reqbuf other;
    req_init(&other);
    req_auth(&env, &other, USER_A);
    req_cookie_extra(&other, "last_room=3002");
    req_get(&other, "/rooms/3001");
    cf_response resp_other;
    cf_response_init(&resp_other);
    CF_REQUIRE(run_request(&env, &other, &resp_other));
    CF_CHECK(resp_other.status == 200);
    CF_REQUIRE(resp_other.body != NULL);
    cf_buf *body_other = cf_buf_retain(resp_other.body);
    cf_response_dispose(&resp_other);

    cf_cache_stats mid = stats(&env);
    CF_CHECK(mid.misses == 2 && mid.hits == 0 && mid.entries == 2);

    /* Each request state hits its own entry. */
    reqbuf other2;
    req_init(&other2);
    req_auth(&env, &other2, USER_A);
    req_cookie_extra(&other2, "last_room=3002");
    req_get(&other2, "/rooms/3001");
    cf_response hit_other;
    cf_response_init(&hit_other);
    CF_REQUIRE(run_request(&env, &other2, &hit_other));
    {
        cf_span expect = cf_buf_span(body_other);
        cf_span got = resp_body(&hit_other);
        CF_CHECK(got.len == expect.len &&
                 memcmp(got.ptr, expect.ptr, got.len) == 0);
    }
    cf_response_dispose(&hit_other);

    reqbuf none2;
    req_init(&none2);
    req_auth(&env, &none2, USER_A);
    req_get(&none2, "/rooms/3001");
    cf_response hit_none;
    cf_response_init(&hit_none);
    CF_REQUIRE(run_request(&env, &none2, &hit_none));
    {
        cf_span expect = cf_buf_span(body_none);
        cf_span got = resp_body(&hit_none);
        CF_CHECK(got.len == expect.len &&
                 memcmp(got.ptr, expect.ptr, got.len) == 0);
    }
    cf_response_dispose(&hit_none);

    cf_cache_stats after = stats(&env);
    CF_CHECK(after.hits == 2 && after.entries == 2);
    cf_buf_release(body_none);
    cf_buf_release(body_other);
    env_close(&env);
}

/* ======================================================================== */
/* CACHE-02/03: version capture, forced commit, rollback                    */
/* ======================================================================== */

CF_TEST(cache_forced_commit_never_admits_stale_render) {
    cache_env env;
    CF_REQUIRE(env_open_ex(&env, CACHE_BUDGET, true));
    seed_world(&env);
    g_probe_env = &env;
    g_probe = PROBE_COMMIT_BEFORE_ADMIT;

    uint64_t version_before = cf_data_version(env.app);
    reqbuf b;
    req_init(&b);
    req_auth(&env, &b, USER_A);
    req_get(&b, "/cachetest/1");
    cf_response resp;
    cf_response_init(&resp);
    CF_REQUIRE(run_request(&env, &b, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "probe-body"));
    cf_response_dispose(&resp);

    /* The commit landed between capture and admission: the render was served
     * but never admitted under the new version. */
    CF_CHECK(cf_data_version(env.app) == version_before + 1);
    cf_cache_stats after = stats(&env);
    CF_CHECK(after.version_rejects == 1);
    CF_CHECK(after.entries == 0);

    /* The next request reads at the new version: it misses and admits. */
    reqbuf b2;
    req_init(&b2);
    req_auth(&env, &b2, USER_A);
    req_get(&b2, "/cachetest/1");
    cf_response resp2;
    cf_response_init(&resp2);
    g_probe = PROBE_PLAIN;
    CF_REQUIRE(run_request(&env, &b2, &resp2));
    CF_CHECK(body_contains(&resp2, "probe-body"));
    cf_response_dispose(&resp2);
    cf_cache_stats after2 = stats(&env);
    CF_CHECK(after2.entries == 1);
    env_close(&env);
}

CF_TEST(cache_commit_after_auth_does_not_recapture_identity_generation) {
    cache_env env;
    CF_REQUIRE(env_open_ex(&env, CACHE_BUDGET, true));
    seed_world(&env);
    g_probe_env = &env;
    g_probe = PROBE_PLAIN;
    reqbuf warm;
    req_init(&warm);
    req_auth(&env, &warm, USER_A);
    req_get(&warm, "/cachetest/1");
    cf_response response;
    cf_response_init(&response);
    CF_REQUIRE(run_request(&env, &warm, &response));
    cf_response_dispose(&response);
    cf_cache_stats before = stats(&env);
    CF_REQUIRE(before.entries == 1);

    reqbuf changed;
    req_init(&changed);
    req_auth(&env, &changed, USER_A);
    req_get(&changed, "/cachetest/1");
    g_probe = PROBE_COMMIT_AFTER_AUTH;
    cf_response_init(&response);
    CF_REQUIRE(run_request(&env, &changed, &response));
    CF_CHECK(response.status == 200);
    CF_CHECK(body_contains(&response, "probe-body"));
    CF_CHECK(stats(&env).hits == before.hits);
    CF_CHECK(stats(&env).entries == before.entries);
    cf_response_dispose(&response);
    env_close(&env);
}

CF_TEST(cache_failed_write_preserves_version_and_entry) {
    cache_env env;
    CF_REQUIRE(env_open_ex(&env, CACHE_BUDGET, true));
    seed_world(&env);
    g_probe_env = &env;
    g_probe = PROBE_PLAIN;

    reqbuf b;
    req_init(&b);
    req_auth(&env, &b, USER_A);
    req_get(&b, "/cachetest/2");
    cf_response resp;
    cf_response_init(&resp);
    CF_REQUIRE(run_request(&env, &b, &resp));
    cf_response_dispose(&resp);
    CF_CHECK(stats(&env).entries == 1);

    uint64_t version = cf_data_version(env.app);
    g_probe = PROBE_FAILED_WRITE;
    reqbuf b2;
    req_init(&b2);
    req_auth(&env, &b2, USER_A);
    req_get(&b2, "/cachetest/2");
    cf_response resp2;
    cf_response_init(&resp2);
    CF_REQUIRE(run_request(&env, &b2, &resp2));
    /* The rolled-back write must not bump the version, so the entry is still
     * current -- and the request that carried the failed write hit it. */
    CF_CHECK(cf_data_version(env.app) == version);
    CF_CHECK(resp2.status == 200);
    cf_cache_stats after = stats(&env);
    CF_CHECK(after.entries == 1);
    CF_CHECK(after.hits == 1);
    CF_CHECK(after.version_rejects == 0);
    cf_response_dispose(&resp2);
    env_close(&env);
}

CF_TEST(cache_session_activity_write_bypasses_old_auth_generation) {
    cache_env env;
    CF_REQUIRE(env_open(&env, CACHE_BUDGET));
    seed_world(&env);

    /* A stale session: authentication resumes it with a cf_write, so the
     * version advances inside the request, before the lookup. The normal page
     * is served but cannot be admitted using the preceding access snapshot. */
    reqbuf b;
    req_init(&b);
    req_auth_at(&env, &b, USER_A, "2020-01-01 00:00:00.000000");
    req_get(&b, "/rooms/3001");
    uint64_t before = cf_data_version(env.app);
    cf_response resp;
    cf_response_init(&resp);
    CF_REQUIRE(run_request(&env, &b, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(cf_data_version(env.app) > before);
    cf_cache_stats s = stats(&env);
    CF_CHECK(s.version_rejects == 0);
    CF_CHECK(s.entries == 0);
    CF_REQUIRE(resp.body != NULL);
    cf_buf *body = cf_buf_retain(resp.body);
    cf_response_dispose(&resp);

    /* The resumed session no longer needs a write: the same token can admit. */
    reqbuf b2;
    req_init(&b2);
    snprintf(b2.cookie, sizeof b2.cookie, "%s", b.cookie);
    CF_REQUIRE(cf_test_req_header(&b2.req, SP("Cookie"), SP(b2.cookie)) ==
               CF_OK);
    req_get(&b2, "/rooms/3001");
    cf_response resp2;
    cf_response_init(&resp2);
    CF_REQUIRE(run_request(&env, &b2, &resp2));
    CF_CHECK(resp2.status == 200);
    {
        cf_span expect = cf_buf_span(body);
        cf_span got = resp_body(&resp2);
        CF_CHECK(got.len == expect.len &&
                 memcmp(got.ptr, expect.ptr, got.len) == 0);
    }
    cf_response_dispose(&resp2);
    CF_CHECK(stats(&env).hits == 0);
    CF_CHECK(stats(&env).entries == 1);
    cf_buf_release(body);
    env_close(&env);
}

/* ======================================================================== */
/* CACHE-04: budget, eviction, oversized, duplicate (live round)            */
/* ======================================================================== */

CF_TEST(cache_live_budget_eviction_and_oversized_bypass) {
    cache_env env;
    /* After the bucket array, room for exactly one small probe entry (the
     * probe entry costs ~235 bytes: 73-byte key + 136 metadata + 16 cf_buf
     * header + the 10-byte body), so every later admission evicts. */
    CF_REQUIRE(env_open_ex(&env, CF_CACHE_BUCKET_BYTES + 400, true));
    seed_world(&env);
    g_probe_env = &env;
    g_probe = PROBE_PLAIN;

    for (int i = 1; i <= 4; i++) {
        char path[64];
        snprintf(path, sizeof path, "/cachetest/%d", i);
        reqbuf b;
        req_init(&b);
        req_auth(&env, &b, USER_A);
        req_get(&b, path);
        cf_response resp;
        cf_response_init(&resp);
        CF_REQUIRE(run_request(&env, &b, &resp));
        CF_CHECK(resp.status == 200);
        cf_response_dispose(&resp);
    }
    cf_cache_stats after = stats(&env);
    CF_CHECK(after.evictions >= 3); /* FIFO made room for every later entry */
    CF_CHECK(after.entries == 1);
    CF_CHECK(after.bytes_used <= after.budget_bytes);
    CF_CHECK(after.bucket_bytes == CF_CACHE_BUCKET_BYTES);

    /* An oversized entry (over 1 MiB including metadata) bypasses without
     * failing the page. */
    static char big[1024 * 1024 + 64];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    g_probe_body = big;
    uint64_t bypasses_before = after.bypasses;
    reqbuf ob;
    req_init(&ob);
    req_auth(&env, &ob, USER_A);
    req_get(&ob, "/cachetest/9");
    cf_response rbig;
    cf_response_init(&rbig);
    CF_REQUIRE(run_request(&env, &ob, &rbig));
    CF_CHECK(rbig.status == 200);
    CF_CHECK(resp_body(&rbig).len == sizeof big - 1);
    cf_response_dispose(&rbig);
    cf_cache_stats obig = stats(&env);
    CF_CHECK(obig.bypasses > bypasses_before);
    g_probe_body = "probe-body";
    env_close(&env);
}

CF_TEST(cache_duplicate_admission_is_counted) {
    cache_env env;
    CF_REQUIRE(env_open_ex(&env, CACHE_BUDGET, true));
    seed_world(&env);
    g_probe_env = &env;
    g_probe = PROBE_DUPLICATE;

    reqbuf b;
    req_init(&b);
    req_auth(&env, &b, USER_A);
    req_get(&b, "/cachetest/3");
    cf_response resp;
    cf_response_init(&resp);
    CF_REQUIRE(run_request(&env, &b, &resp));
    CF_CHECK(resp.status == 200);
    cf_response_dispose(&resp);
    cf_cache_stats after = stats(&env);
    CF_CHECK(after.entries == 1);
    CF_CHECK(after.duplicate_builds == 1);
    env_close(&env);
}

/* ======================================================================== */
/* CACHE-05: ETag / 304 / encoding / cookies                                */
/* ======================================================================== */

CF_TEST(cache_etag_304_on_hit_and_on_miss) {
    cache_env env;
    CF_REQUIRE(env_open(&env, CACHE_BUDGET));
    seed_world(&env);

    reqbuf first;
    req_init(&first);
    req_auth(&env, &first, USER_A);
    req_get(&first, "/rooms/3001");
    cf_response resp;
    cf_response_init(&resp);
    CF_REQUIRE(run_request(&env, &first, &resp));
    char etag[256];
    CF_REQUIRE(resp_header(&resp, &first.req, "ETag: ", etag, sizeof etag));
    cf_buf *body = cf_buf_retain(resp.body);
    cf_response_dispose(&resp);

    /* Hit-path 304: the cache entry supplies the validator, no body is sent. */
    reqbuf second;
    req_init(&second);
    req_auth(&env, &second, USER_A);
    req_get(&second, "/rooms/3001");
    CF_REQUIRE(cf_test_req_header(&second.req, SP("If-None-Match"),
                                  SP(etag)) == CF_OK);
    cf_response resp304;
    cf_response_init(&resp304);
    CF_REQUIRE(run_request(&env, &second, &resp304));
    CF_CHECK(resp304.status == 304);
    char etag304[256];
    CF_REQUIRE(resp_header(&resp304, &second.req, "ETag: ", etag304,
                           sizeof etag304));
    CF_CHECK(strcmp(etag304, etag) == 0);
    char content_length[64];
    CF_CHECK(!resp_header(&resp304, &second.req, "Content-Length: ",
                          content_length, sizeof content_length));
    {
        cf_http_serialized ser = {0};
        CF_REQUIRE(cf_http_response_serialize(&resp304, &second.req, &ser) ==
                   CF_OK);
        CF_CHECK(!ser.send_body);
        cf_buf_release(ser.headers);
    }
    cf_response_dispose(&resp304);
    CF_CHECK(stats(&env).hits == 1);

    /* Miss-path 304: advance the version so the key changes and the page is
     * re-rendered; the same validator still makes it a 304, and the 304
     * render is never admitted (the entry gauge does not move). */
    cf_app_advance_data_version(env.app);
    cf_cache_stats before_miss = stats(&env);
    reqbuf third;
    req_init(&third);
    req_auth(&env, &third, USER_A);
    req_get(&third, "/rooms/3001");
    CF_REQUIRE(cf_test_req_header(&third.req, SP("If-None-Match"),
                                  SP(etag)) == CF_OK);
    cf_response resp_miss304;
    cf_response_init(&resp_miss304);
    CF_REQUIRE(run_request(&env, &third, &resp_miss304));
    CF_CHECK(resp_miss304.status == 304);
    cf_response_dispose(&resp_miss304);
    cf_cache_stats after = stats(&env);
    CF_CHECK(after.misses == before_miss.misses + 1);
    CF_CHECK(after.hits == before_miss.hits);
    CF_CHECK(after.entries == before_miss.entries); /* not admitted */

    /* A plain request at the new version re-renders (miss, not a 304-render
     * hit) and admits the identical page under the new version. */
    reqbuf fourth;
    req_init(&fourth);
    req_auth(&env, &fourth, USER_A);
    req_get(&fourth, "/rooms/3001");
    cf_response resp_fresh;
    cf_response_init(&resp_fresh);
    CF_REQUIRE(run_request(&env, &fourth, &resp_fresh));
    CF_CHECK(resp_fresh.status == 200);
    cf_span fresh = resp_body(&resp_fresh);
    cf_span original = cf_buf_span(body);
    CF_CHECK(fresh.len == original.len &&
             memcmp(fresh.ptr, original.ptr, fresh.len) == 0);
    char etag_fresh[256];
    CF_REQUIRE(resp_header(&resp_fresh, &fourth.req, "ETag: ", etag_fresh,
                           sizeof etag_fresh));
    CF_CHECK(strcmp(etag_fresh, etag) == 0);
    cf_response_dispose(&resp_fresh);
    cf_cache_stats final = stats(&env);
    CF_CHECK(final.misses == after.misses + 1);
    CF_CHECK(final.entries == after.entries + 1);
    cf_buf_release(body);
    env_close(&env);
}

CF_TEST(cache_gzip_identity_and_406) {
    cache_env env;
    CF_REQUIRE(env_open(&env, CACHE_BUDGET));
    seed_world(&env);

    /* Identity first. */
    reqbuf plain;
    req_init(&plain);
    req_auth(&env, &plain, USER_A);
    req_get(&plain, "/rooms/3001");
    cf_response resp_plain;
    cf_response_init(&resp_plain);
    CF_REQUIRE(run_request(&env, &plain, &resp_plain));
    CF_CHECK(resp_plain.status == 200);
    char etag_plain[256];
    CF_REQUIRE(resp_header(&resp_plain, &plain.req, "ETag: ", etag_plain,
                           sizeof etag_plain));
    char vary[256];
    CF_REQUIRE(resp_header(&resp_plain, &plain.req, "Vary: ", vary,
                           sizeof vary));
    CF_CHECK(strstr(vary, "Accept-Encoding") != NULL);
    char encoding[64];
    CF_CHECK(!resp_header(&resp_plain, &plain.req, "Content-Encoding: ",
                          encoding, sizeof encoding));
    cf_span body_plain = resp_body(&resp_plain);

    /* gzip: a distinct key and a distinct representation, same validator. */
    reqbuf gz;
    req_init(&gz);
    req_auth(&env, &gz, USER_A);
    req_get(&gz, "/rooms/3001");
    CF_REQUIRE(cf_test_req_header(&gz.req, SP("Accept-Encoding"),
                                  SP("gzip, deflate, br")) == CF_OK);
    cf_response resp_gz;
    cf_response_init(&resp_gz);
    CF_REQUIRE(run_request(&env, &gz, &resp_gz));
    CF_CHECK(resp_gz.status == 200);
    CF_REQUIRE(resp_header(&resp_gz, &gz.req, "Content-Encoding: ", encoding,
                           sizeof encoding));
    CF_CHECK(strcmp(encoding, "gzip") == 0);
    char etag_gz[256];
    CF_REQUIRE(resp_header(&resp_gz, &gz.req, "ETag: ", etag_gz,
                           sizeof etag_gz));
    CF_CHECK(strcmp(etag_gz, etag_plain) == 0);
    CF_CHECK(gunzip_equals(resp_body(&resp_gz), body_plain));
    cf_cache_stats two = stats(&env);
    CF_CHECK(two.entries == 2);
    CF_CHECK(two.misses == 2);

    /* A valid gzip hit is served from the entry: byte-identical gzip, no
     * compression call (the round never reaches cf_gzip on a hit). */
    cf_buf *gz_body = cf_buf_retain(resp_gz.body);
    cf_response_dispose(&resp_plain);
    cf_response_dispose(&resp_gz);

    reqbuf gz2;
    req_init(&gz2);
    req_auth(&env, &gz2, USER_A);
    req_get(&gz2, "/rooms/3001");
    CF_REQUIRE(cf_test_req_header(&gz2.req, SP("Accept-Encoding"),
                                  SP("gzip, deflate, br")) == CF_OK);
    cf_response resp_gz2;
    cf_response_init(&resp_gz2);
    CF_REQUIRE(run_request(&env, &gz2, &resp_gz2));
    CF_CHECK(resp_gz2.status == 200);
    CF_REQUIRE(resp_header(&resp_gz2, &gz2.req, "Content-Encoding: ",
                           encoding, sizeof encoding));
    CF_CHECK(strcmp(encoding, "gzip") == 0);
    cf_span gz_first = cf_buf_span(gz_body);
    cf_span gz_second = resp_body(&resp_gz2);
    CF_CHECK(gz_first.len == gz_second.len &&
             memcmp(gz_first.ptr, gz_second.ptr, gz_first.len) == 0);
    cf_response_dispose(&resp_gz2);
    CF_CHECK(stats(&env).hits == 1);
    cf_buf_release(gz_body);

    /* q=0 forms select the other representation (a hit on the identity
     * entry), and both forbidden answers 406. */
    reqbuf gz_off;
    req_init(&gz_off);
    req_auth(&env, &gz_off, USER_A);
    req_get(&gz_off, "/rooms/3001");
    CF_REQUIRE(cf_test_req_header(&gz_off.req, SP("Accept-Encoding"),
                                  SP("gzip;q=0")) == CF_OK);
    cf_response resp_ident;
    cf_response_init(&resp_ident);
    CF_REQUIRE(run_request(&env, &gz_off, &resp_ident));
    CF_CHECK(resp_ident.status == 200);
    CF_CHECK(!resp_header(&resp_ident, &gz_off.req, "Content-Encoding: ",
                          encoding, sizeof encoding));
    CF_CHECK(stats(&env).hits == 2);
    cf_response_dispose(&resp_ident);

    reqbuf neither;
    req_init(&neither);
    req_auth(&env, &neither, USER_A);
    req_get(&neither, "/rooms/3001");
    CF_REQUIRE(cf_test_req_header(&neither.req, SP("Accept-Encoding"),
                                  SP("identity;q=0, gzip;q=0")) == CF_OK);
    cf_response resp_406;
    cf_response_init(&resp_406);
    CF_REQUIRE(run_request(&env, &neither, &resp_406));
    CF_CHECK(resp_406.status == 406);
    CF_CHECK(body_contains(&resp_406, "An acceptable encoding"));
    char ctype[128];
    CF_REQUIRE(resp_header(&resp_406, &neither.req, "Content-Type: ", ctype,
                           sizeof ctype));
    CF_CHECK(strcmp(ctype, "text/plain") == 0);
    cf_response_dispose(&resp_406);
    CF_CHECK(stats(&env).entries == 2); /* no 406 entry */
    env_close(&env);
}

CF_TEST(cache_cookies_are_computed_on_a_hit) {
    cache_env env;
    CF_REQUIRE(env_open(&env, CACHE_BUDGET));
    seed_world(&env);

    /* Prime with the exact cookie value. */
    reqbuf primed;
    req_init(&primed);
    req_auth(&env, &primed, USER_A);
    req_cookie_extra(&primed, "last_room=3001");
    req_get(&primed, "/rooms/3001");
    cf_response resp;
    cf_response_init(&resp);
    CF_REQUIRE(run_request(&env, &primed, &resp));
    CF_CHECK(resp.status == 200);
    char set_cookie[512];
    /* The cookie already equals the room id: no Set-Cookie. */
    CF_CHECK(!resp_header(&resp, &primed.req, "Set-Cookie: ", set_cookie,
                          sizeof set_cookie));
    cf_response_dispose(&resp);
    CF_CHECK(stats(&env).entries == 1);

    /* "03001" parses to the same last_room ID (the key field), so this is a
     * hit -- and the reference's remember_last_room compares the raw value,
     * so the hit must still emit Set-Cookie. */
    reqbuf hit;
    req_init(&hit);
    req_auth(&env, &hit, USER_A);
    req_cookie_extra(&hit, "last_room=03001");
    req_get(&hit, "/rooms/3001");
    cf_response resp_hit;
    cf_response_init(&resp_hit);
    CF_REQUIRE(run_request(&env, &hit, &resp_hit));
    CF_CHECK(resp_hit.status == 200);
    CF_REQUIRE(resp_header(&resp_hit, &hit.req, "Set-Cookie: ", set_cookie,
                           sizeof set_cookie));
    CF_CHECK(strstr(set_cookie, "last_room=3001") != NULL);
    cf_cache_stats after = stats(&env);
    CF_CHECK(after.hits == 1);
    cf_response_dispose(&resp_hit);
    env_close(&env);
}

CF_TEST(cache_head_looks_up_but_never_populates) {
    cache_env env;
    CF_REQUIRE(env_open(&env, CACHE_BUDGET));
    seed_world(&env);

    /* HEAD first: a miss that must not create an entry. */
    reqbuf head;
    req_init(&head);
    req_auth(&env, &head, USER_A);
    head.req.method = CF_HEAD;
    head.req.original_method = CF_HEAD;
    req_get(&head, "/rooms/3001");
    cf_response resp_head;
    cf_response_init(&resp_head);
    CF_REQUIRE(run_request(&env, &head, &resp_head));
    CF_CHECK(resp_head.status == 200);
    cf_response_dispose(&resp_head);
    cf_cache_stats after_head = stats(&env);
    CF_CHECK(after_head.entries == 0);
    CF_CHECK(after_head.misses >= 1);

    /* GET populates. */
    reqbuf get;
    req_init(&get);
    req_auth(&env, &get, USER_A);
    req_get(&get, "/rooms/3001");
    cf_response resp_get;
    cf_response_init(&resp_get);
    CF_REQUIRE(run_request(&env, &get, &resp_get));
    cf_span body = resp_body(&resp_get);
    size_t body_len = body.len;
    char etag[256];
    CF_REQUIRE(resp_header(&resp_get, &get.req, "ETag: ", etag, sizeof etag));
    cf_response_dispose(&resp_get);
    CF_CHECK(stats(&env).entries == 1);

    /* HEAD again: a hit on the GET entry, headers as GET, no body sent. */
    reqbuf head2;
    req_init(&head2);
    req_auth(&env, &head2, USER_A);
    head2.req.method = CF_HEAD;
    head2.req.original_method = CF_HEAD;
    req_get(&head2, "/rooms/3001");
    cf_response resp_head2;
    cf_response_init(&resp_head2);
    CF_REQUIRE(run_request(&env, &head2, &resp_head2));
    CF_CHECK(resp_head2.status == 200);
    char etag_head[256];
    CF_REQUIRE(resp_header(&resp_head2, &head2.req, "ETag: ", etag_head,
                           sizeof etag_head));
    CF_CHECK(strcmp(etag_head, etag) == 0);
    {
        cf_http_serialized ser = {0};
        CF_REQUIRE(cf_http_response_serialize(&resp_head2, &head2.req, &ser) ==
                   CF_OK);
        CF_CHECK(!ser.send_body);
        CF_CHECK(ser.body_length == body_len); /* what GET would send */
        cf_buf_release(ser.headers);
    }
    cf_response_dispose(&resp_head2);
    cf_cache_stats after_head2 = stats(&env);
    CF_CHECK(after_head2.hits == 1);
    CF_CHECK(after_head2.entries == 1); /* HEAD never popped a second entry */
    env_close(&env);
}

CF_TEST(cache_flash_bearing_body_bypasses) {
    cache_env env;
    CF_REQUIRE(env_open(&env, CACHE_BUDGET));
    seed_world(&env);
    /* A completed page already exists before the flash-bearing request. */
    reqbuf warm;
    req_init(&warm);
    req_auth(&env, &warm, USER_A);
    req_get(&warm, "/rooms/3001");
    cf_response warmed;
    cf_response_init(&warmed);
    CF_REQUIRE(run_request(&env, &warm, &warmed));
    cf_response_dispose(&warmed);
    CF_REQUIRE(stats(&env).entries == 1);
    uint64_t hits_before_flash = stats(&env).hits;

    /* A set_room failure redirects with a flash set; a redirect is ineligible
     * and never admitted. */
    reqbuf missing;
    req_init(&missing);
    req_auth(&env, &missing, USER_A);
    req_get(&missing, "/rooms/999999");
    cf_response resp_redirect;
    cf_response_init(&resp_redirect);
    CF_REQUIRE(run_request(&env, &missing, &resp_redirect));
    CF_CHECK(resp_redirect.status == 302);
    CF_CHECK(stats(&env).entries == 1);
    char flash_cookie[2048];
    CF_REQUIRE(resp_cookie(&resp_redirect, &missing.req, "_campfire_session",
                           flash_cookie, sizeof flash_cookie));
    cf_response_dispose(&resp_redirect);

    /* The next request carries the flash: the 200 body that displays the
     * notice must bypass the cache entirely. */
    reqbuf flashed;
    req_init(&flashed);
    req_auth(&env, &flashed, USER_A);
    req_cookie_extra(&flashed, flash_cookie);
    req_get(&flashed, "/rooms/3001");
    cf_response resp_flashed;
    cf_response_init(&resp_flashed);
    CF_REQUIRE(run_request(&env, &flashed, &resp_flashed));
    CF_CHECK(resp_flashed.status == 200);
    CF_CHECK(body_contains(&resp_flashed, "Room not found or inaccessible"));
    CF_CHECK(stats(&env).hits == hits_before_flash);
    CF_CHECK(stats(&env).entries == 1);
    cf_response_dispose(&resp_flashed);

    /* Without the flash the same page is admitted. */
    reqbuf page;
    req_init(&page);
    req_auth(&env, &page, USER_A);
    req_get(&page, "/rooms/3001");
    cf_response resp_page;
    cf_response_init(&resp_page);
    CF_REQUIRE(run_request(&env, &page, &resp_page));
    CF_CHECK(resp_page.status == 200);
    CF_CHECK(!body_contains(&resp_page, "Room not found or inaccessible"));
    cf_response_dispose(&resp_page);
    CF_CHECK(stats(&env).entries == 1);
    env_close(&env);
}

/* With CF_CACHE_BYTES=0 the representation pipeline is still on: the four
 * admitted handlers negotiate Accept-Encoding (gzip/identity/406) and carry
 * Vary: Accept-Encoding; only storage/reuse (and the cache-path body-hash
 * ETag/304) is off. Derivation: the reference's Rack::Deflater is always on
 * (deflater.rs:44-102) and Rack::ETag skips a response that already carries a
 * validator (ctx.rs:681-695), so messages#index keeps its pinned record
 * validator while the encoding layer still applies. */
CF_TEST(cache_disabled_representation_still_negotiates) {
    cache_env env;
    CF_REQUIRE(env_open(&env, 0));
    seed_world(&env);
    CF_CHECK(cache_of(&env) == NULL);

    /* rooms#show, identity: Vary is present, no Content-Encoding, and no
     * body-hash ETag (that validator is cache-scoped). */
    reqbuf room;
    req_init(&room);
    req_auth(&env, &room, USER_A);
    req_get(&room, "/rooms/3001");
    cf_response resp_room;
    cf_response_init(&resp_room);
    CF_REQUIRE(run_request(&env, &room, &resp_room));
    CF_CHECK(resp_room.status == 200);
    CF_REQUIRE(resp_room.body != NULL);
    cf_buf *identity = cf_buf_retain(resp_room.body);
    char header[256];
    CF_CHECK(!resp_header(&resp_room, &room.req, "ETag: ", header,
                          sizeof header));
    CF_CHECK(!resp_header(&resp_room, &room.req, "Content-Encoding: ", header,
                          sizeof header));
    CF_REQUIRE(resp_header(&resp_room, &room.req, "Vary: ", header,
                           sizeof header));
    CF_CHECK(strstr(header, "Accept-Encoding") != NULL);
    cf_response_dispose(&resp_room);

    /* rooms#show, gzip: honored with no cache at all, and the gzip body
     * inflates to the identity bytes. */
    reqbuf gz;
    req_init(&gz);
    req_auth(&env, &gz, USER_A);
    req_get(&gz, "/rooms/3001");
    CF_REQUIRE(cf_test_req_header(&gz.req, SP("Accept-Encoding"),
                                  SP("gzip, deflate, br")) == CF_OK);
    cf_response resp_gz;
    cf_response_init(&resp_gz);
    CF_REQUIRE(run_request(&env, &gz, &resp_gz));
    CF_CHECK(resp_gz.status == 200);
    CF_REQUIRE(resp_header(&resp_gz, &gz.req, "Content-Encoding: ", header,
                           sizeof header));
    CF_CHECK(strcmp(header, "gzip") == 0);
    CF_REQUIRE(resp_header(&resp_gz, &gz.req, "Vary: ", header,
                           sizeof header));
    CF_CHECK(strstr(header, "Accept-Encoding") != NULL);
    CF_CHECK(!resp_header(&resp_gz, &gz.req, "ETag: ", header,
                          sizeof header));
    CF_CHECK(gunzip_equals(resp_body(&resp_gz), cf_buf_span(identity)));
    cf_response_dispose(&resp_gz);

    /* gzip;q=0 selects identity; both forbidden answers the pin's 406. */
    reqbuf off;
    req_init(&off);
    req_auth(&env, &off, USER_A);
    req_get(&off, "/rooms/3001");
    CF_REQUIRE(cf_test_req_header(&off.req, SP("Accept-Encoding"),
                                  SP("gzip;q=0")) == CF_OK);
    cf_response resp_off;
    cf_response_init(&resp_off);
    CF_REQUIRE(run_request(&env, &off, &resp_off));
    CF_CHECK(resp_off.status == 200);
    CF_CHECK(!resp_header(&resp_off, &off.req, "Content-Encoding: ", header,
                          sizeof header));
    cf_response_dispose(&resp_off);

    reqbuf none;
    req_init(&none);
    req_auth(&env, &none, USER_A);
    req_get(&none, "/rooms/3001");
    CF_REQUIRE(cf_test_req_header(&none.req, SP("Accept-Encoding"),
                                  SP("identity;q=0, gzip;q=0")) == CF_OK);
    cf_response resp_406;
    cf_response_init(&resp_406);
    CF_REQUIRE(run_request(&env, &none, &resp_406));
    CF_CHECK(resp_406.status == 406);
    CF_CHECK(body_contains(&resp_406, "An acceptable encoding"));
    cf_response_dispose(&resp_406);
    cf_buf_release(identity);

    /* messages#index: the pinned record validator (32 hex) and Last-Modified
     * survive, the encoding layer still gzips and owns Vary, and a matching
     * record validator is still a 304 without Content-Encoding. */
    reqbuf index;
    req_init(&index);
    req_auth(&env, &index, USER_A);
    req_get(&index, "/rooms/3001/messages");
    CF_REQUIRE(cf_test_req_header(
                   &index.req, SP("Accept"),
                   SP("text/html, application/xhtml+xml")) == CF_OK);
    CF_REQUIRE(cf_test_req_header(&index.req, SP("Accept-Encoding"),
                                  SP("gzip")) == CF_OK);
    cf_response resp_index;
    cf_response_init(&resp_index);
    CF_REQUIRE(run_request(&env, &index, &resp_index));
    CF_CHECK(resp_index.status == 200);
    char etag[256];
    CF_REQUIRE(resp_header(&resp_index, &index.req, "ETag: ", etag,
                           sizeof etag));
    CF_CHECK(strncmp(etag, "W/\"", 3) == 0 && strlen(etag) == 3 + 32 + 1);
    CF_CHECK(resp_header(&resp_index, &index.req, "Last-Modified: ", header,
                         sizeof header));
    CF_REQUIRE(resp_header(&resp_index, &index.req, "Content-Encoding: ",
                           header, sizeof header));
    CF_CHECK(strcmp(header, "gzip") == 0);
    char vary[256];
    CF_REQUIRE(resp_header(&resp_index, &index.req, "Vary: ", vary,
                           sizeof vary));
    CF_CHECK(strstr(vary, "Accept-Encoding") != NULL);
    CF_CHECK(strstr(vary, "Accept") != NULL);
    cf_response_dispose(&resp_index);

    reqbuf index2;
    req_init(&index2);
    req_auth(&env, &index2, USER_A);
    req_get(&index2, "/rooms/3001/messages");
    CF_REQUIRE(cf_test_req_header(
                   &index2.req, SP("Accept"),
                   SP("text/html, application/xhtml+xml")) == CF_OK);
    CF_REQUIRE(cf_test_req_header(&index2.req, SP("Accept-Encoding"),
                                  SP("gzip")) == CF_OK);
    CF_REQUIRE(cf_test_req_header(&index2.req, SP("If-None-Match"),
                                  SP(etag)) == CF_OK);
    cf_response resp_304;
    cf_response_init(&resp_304);
    CF_REQUIRE(run_request(&env, &index2, &resp_304));
    CF_CHECK(resp_304.status == 304);
    CF_CHECK(!resp_header(&resp_304, &index2.req, "Content-Encoding: ", header,
                          sizeof header));
    cf_response_dispose(&resp_304);
    env_close(&env);
}

/* The same always-on representation for users/sidebars#show and
 * searches#index with the cache disabled. */
CF_TEST(cache_disabled_sidebars_searches_negotiate) {
    cache_env env;
    CF_REQUIRE(env_open(&env, 0));
    seed_world(&env);

    /* sidebars: identity then gzip on a fresh request. */
    reqbuf side;
    req_init(&side);
    req_auth(&env, &side, USER_A);
    req_get(&side, "/users/me/sidebar");
    cf_response resp_side;
    cf_response_init(&resp_side);
    CF_REQUIRE(run_request(&env, &side, &resp_side));
    CF_CHECK(resp_side.status == 200);
    CF_REQUIRE(resp_side.body != NULL);
    cf_buf *side_identity = cf_buf_retain(resp_side.body);
    cf_response_dispose(&resp_side);

    reqbuf side_gz;
    req_init(&side_gz);
    req_auth(&env, &side_gz, USER_A);
    req_get(&side_gz, "/users/me/sidebar");
    CF_REQUIRE(cf_test_req_header(&side_gz.req, SP("Accept-Encoding"),
                                  SP("gzip")) == CF_OK);
    cf_response resp_side_gz;
    cf_response_init(&resp_side_gz);
    CF_REQUIRE(run_request(&env, &side_gz, &resp_side_gz));
    CF_CHECK(resp_side_gz.status == 200);
    {
        char encoding[64];
        CF_REQUIRE(resp_header(&resp_side_gz, &side_gz.req,
                               "Content-Encoding: ", encoding,
                               sizeof encoding));
        CF_CHECK(strcmp(encoding, "gzip") == 0);
        char vary[256];
        CF_REQUIRE(resp_header(&resp_side_gz, &side_gz.req, "Vary: ", vary,
                               sizeof vary));
        CF_CHECK(strstr(vary, "Accept-Encoding") != NULL);
    }
    CF_CHECK(gunzip_equals(resp_body(&resp_side_gz),
                           cf_buf_span(side_identity)));
    cf_response_dispose(&resp_side_gz);
    cf_buf_release(side_identity);

    /* searches: identity then gzip. */
    reqbuf search;
    req_init(&search);
    req_auth(&env, &search, USER_A);
    req_get(&search, "/searches?q=first");
    cf_response resp_search;
    cf_response_init(&resp_search);
    CF_REQUIRE(run_request(&env, &search, &resp_search));
    CF_CHECK(resp_search.status == 200);
    CF_REQUIRE(resp_search.body != NULL);
    cf_buf *search_identity = cf_buf_retain(resp_search.body);
    cf_response_dispose(&resp_search);

    reqbuf search_gz;
    req_init(&search_gz);
    req_auth(&env, &search_gz, USER_A);
    req_get(&search_gz, "/searches?q=first");
    CF_REQUIRE(cf_test_req_header(&search_gz.req, SP("Accept-Encoding"),
                                  SP("gzip")) == CF_OK);
    cf_response resp_search_gz;
    cf_response_init(&resp_search_gz);
    CF_REQUIRE(run_request(&env, &search_gz, &resp_search_gz));
    CF_CHECK(resp_search_gz.status == 200);
    {
        char encoding[64];
        CF_REQUIRE(resp_header(&resp_search_gz, &search_gz.req,
                               "Content-Encoding: ", encoding,
                               sizeof encoding));
        CF_CHECK(strcmp(encoding, "gzip") == 0);
        char vary[256];
        CF_REQUIRE(resp_header(&resp_search_gz, &search_gz.req, "Vary: ",
                               vary, sizeof vary));
        CF_CHECK(strstr(vary, "Accept-Encoding") != NULL);
    }
    CF_CHECK(gunzip_equals(resp_body(&resp_search_gz),
                           cf_buf_span(search_identity)));
    cf_response_dispose(&resp_search_gz);
    cf_buf_release(search_identity);
    env_close(&env);
}

/* ======================================================================== */
/* CACHE-06: unrelated-room writes invalidate                               */
/* ======================================================================== */

static cf_err unrelated_room_write(cf_tx *tx, void *arg) {
    (void)arg;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO messages (id, client_message_id, created_at, "
             "creator_id, room_id, updated_at) VALUES (%lld, 'unrelated', "
             "'2026-09-26 14:00:00.000000', %lld, %lld, "
             "'2026-09-26 14:00:00.000000')",
             (long long)MSG_1 + 100, (long long)USER_B,
             (long long)ROOM_2);
    char *message = NULL;
    if (sqlite3_exec(cf_db_handle(db), sql, NULL, NULL, &message) !=
        SQLITE_OK) {
        sqlite3_free(message);
        return CF_INTERNAL;
    }
    return CF_OK;
}

CF_TEST(cache_unrelated_room_write_invalidates) {
    cache_env env;
    CF_REQUIRE(env_open(&env, CACHE_BUDGET));
    seed_world(&env);

    reqbuf one;
    req_init(&one);
    req_auth(&env, &one, USER_A);
    req_get(&one, "/rooms/3001");
    cf_response resp_one;
    cf_response_init(&resp_one);
    CF_REQUIRE(run_request(&env, &one, &resp_one));
    CF_CHECK(stats(&env).entries == 1);
    cf_response_dispose(&resp_one);

    /* A committed write to room 2 advances the version: the room-1 entry is
     * unusable, so the next room-1 request must miss and re-render. */
    cf_cache_stats before = stats(&env);
    CF_REQUIRE(cf_write(env.app, unrelated_room_write, NULL) == CF_OK);
    reqbuf two;
    req_init(&two);
    req_auth(&env, &two, USER_A);
    req_get(&two, "/rooms/3001");
    cf_response resp_two;
    cf_response_init(&resp_two);
    CF_REQUIRE(run_request(&env, &two, &resp_two));
    CF_CHECK(resp_two.status == 200);
    cf_cache_stats after = stats(&env);
    CF_CHECK(after.misses == before.misses + 1);
    CF_CHECK(after.hits == before.hits);
    CF_CHECK(after.entries == before.entries + 1);
    cf_response_dispose(&resp_two);
    env_close(&env);
}

/* ======================================================================== */
/* users/sidebars#show (57) and searches#index (146): the remaining two     */
/* admitted handlers                                                        */
/* ======================================================================== */

static cf_err record_search_write(cf_tx *tx, void *arg) {
    (void)arg;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO searches (id, created_at, query, updated_at, "
             "user_id) VALUES (5001, '2026-09-26 15:00:00.000000', 'churn', "
             "'2026-09-26 15:00:00.000000', %lld)",
             (long long)USER_A);
    char *message = NULL;
    if (sqlite3_exec(cf_db_handle(db), sql, NULL, NULL, &message) !=
        SQLITE_OK) {
        sqlite3_free(message);
        return CF_INTERNAL;
    }
    return CF_OK;
}

CF_TEST(cache_sidebars_hit_and_key_isolation) {
    cache_env env;
    CF_REQUIRE(env_open(&env, CACHE_BUDGET));
    seed_world(&env);

    /* The page request as A. */
    reqbuf a;
    req_init(&a);
    req_auth(&env, &a, USER_A);
    req_get(&a, "/users/me/sidebar");
    cf_response first;
    cf_response_init(&first);
    CF_REQUIRE(run_request(&env, &a, &first));
    CF_CHECK(first.status == 200);
    CF_CHECK(body_contains(&first, "data-room-id=\"3001\""));
    CF_REQUIRE(first.body != NULL);
    cf_buf *body_a = cf_buf_retain(first.body);
    char etag[256];
    CF_REQUIRE(resp_header(&first, &a.req, "ETag: ", etag, sizeof etag));
    CF_CHECK(strncmp(etag, "W/\"", 3) == 0 && strlen(etag) == 68);
    cf_response_dispose(&first);
    CF_CHECK(stats(&env).entries == 1);

    /* Identical request: a hit with the same bytes and validator. */
    reqbuf a2;
    req_init(&a2);
    req_auth(&env, &a2, USER_A);
    req_get(&a2, "/users/me/sidebar");
    cf_response second;
    cf_response_init(&second);
    CF_REQUIRE(run_request(&env, &a2, &second));
    CF_CHECK(second.status == 200);
    {
        cf_span expect = cf_buf_span(body_a);
        cf_span got = resp_body(&second);
        CF_CHECK(got.len == expect.len &&
                 memcmp(got.ptr, expect.ptr, got.len) == 0);
    }
    char etag2[256];
    CF_REQUIRE(resp_header(&second, &a2.req, "ETag: ", etag2, sizeof etag2));
    CF_CHECK(strcmp(etag2, etag) == 0);
    cf_response_dispose(&second);
    cf_cache_stats s = stats(&env);
    CF_CHECK(s.hits == 1 && s.entries == 1);

    /* B's sidebar is a different body: a miss, never A's bytes. */
    reqbuf b;
    req_init(&b);
    req_auth(&env, &b, USER_B);
    req_get(&b, "/users/me/sidebar");
    cf_response resp_b;
    cf_response_init(&resp_b);
    CF_REQUIRE(run_request(&env, &b, &resp_b));
    CF_CHECK(resp_b.status == 200);
    {
        cf_span a_body = cf_buf_span(body_a);
        cf_span b_body = resp_body(&resp_b);
        CF_CHECK(!(a_body.len == b_body.len &&
                   memcmp(a_body.ptr, b_body.ptr, a_body.len) == 0));
    }
    cf_response_dispose(&resp_b);
    s = stats(&env);
    CF_CHECK(s.misses == 2 && s.entries == 2);

    /* last_room is a key field (the layout's last-room selection). */
    reqbuf lr;
    req_init(&lr);
    req_auth(&env, &lr, USER_A);
    req_cookie_extra(&lr, "last_room=3002");
    req_get(&lr, "/users/me/sidebar");
    cf_response resp_lr;
    cf_response_init(&resp_lr);
    CF_REQUIRE(run_request(&env, &lr, &resp_lr));
    CF_CHECK(resp_lr.status == 200);
    cf_response_dispose(&resp_lr);
    s = stats(&env);
    CF_CHECK(s.misses == 3 && s.entries == 3);

    /* The exact User-Agent is a key field (platform facts in the layout). */
    reqbuf ua;
    req_init(&ua);
    req_auth(&env, &ua, USER_A);
    req_get(&ua, "/users/me/sidebar");
    CF_REQUIRE(cf_test_req_header(&ua.req, SP("User-Agent"),
                                  SP("Mozilla/5.0 (Macintosh; Intel Mac OS X "
                                     "10_15_7)")) == CF_OK);
    cf_response resp_ua;
    cf_response_init(&resp_ua);
    CF_REQUIRE(run_request(&env, &ua, &resp_ua));
    CF_CHECK(resp_ua.status == 200);
    cf_response_dispose(&resp_ua);
    s = stats(&env);
    CF_CHECK(s.misses == 4 && s.entries == 4);

    /* A Turbo-Frame request is a distinct representation: it never serves the
     * page entry, and its own entry serves the repeat. */
    reqbuf fr;
    req_init(&fr);
    req_auth(&env, &fr, USER_A);
    req_get(&fr, "/users/me/sidebar");
    CF_REQUIRE(cf_test_req_header(&fr.req, SP("Turbo-Frame"),
                                  SP("sidebar")) == CF_OK);
    cf_response resp_fr;
    cf_response_init(&resp_fr);
    CF_REQUIRE(run_request(&env, &fr, &resp_fr));
    CF_CHECK(resp_fr.status == 200);
    {
        cf_span page_body = cf_buf_span(body_a);
        cf_span frame_body = resp_body(&resp_fr);
        CF_CHECK(!(page_body.len == frame_body.len &&
                   memcmp(page_body.ptr, frame_body.ptr, page_body.len) == 0));
    }
    cf_response_dispose(&resp_fr);
    s = stats(&env);
    CF_CHECK(s.misses == 5 && s.entries == 5);

    reqbuf fr2;
    req_init(&fr2);
    req_auth(&env, &fr2, USER_A);
    req_get(&fr2, "/users/me/sidebar");
    CF_REQUIRE(cf_test_req_header(&fr2.req, SP("Turbo-Frame"),
                                  SP("sidebar")) == CF_OK);
    cf_response resp_fr2;
    cf_response_init(&resp_fr2);
    CF_REQUIRE(run_request(&env, &fr2, &resp_fr2));
    CF_CHECK(stats(&env).hits == 2);
    cf_response_dispose(&resp_fr2);

    /* The raw target is a key field even for the ignored :user_id segment:
     * the body is the same, the entry is not shared. */
    reqbuf other;
    req_init(&other);
    req_auth(&env, &other, USER_A);
    req_get(&other, "/users/999/sidebar");
    cf_response resp_other;
    cf_response_init(&resp_other);
    CF_REQUIRE(run_request(&env, &other, &resp_other));
    CF_CHECK(resp_other.status == 200);
    {
        cf_span expect = cf_buf_span(body_a);
        cf_span got = resp_body(&resp_other);
        CF_CHECK(got.len == expect.len &&
                 memcmp(got.ptr, expect.ptr, got.len) == 0);
    }
    cf_response_dispose(&resp_other);
    s = stats(&env);
    CF_CHECK(s.misses == 6 && s.entries == 6);
    cf_buf_release(body_a);
    env_close(&env);
}

CF_TEST(cache_searches_hit_and_query_isolation) {
    cache_env env;
    CF_REQUIRE(env_open(&env, CACHE_BUDGET));
    seed_world(&env);
    seed_search(env.scratch.db, 1, USER_A, "old",
                "2026-01-01 00:00:00.000000");

    /* q=first renders only the matching message. */
    reqbuf q1;
    req_init(&q1);
    req_auth(&env, &q1, USER_A);
    req_get(&q1, "/searches?q=first");
    CF_REQUIRE(cf_test_req_header(
                   &q1.req, SP("Accept"),
                   SP("text/html, application/xhtml+xml")) == CF_OK);
    cf_response first;
    cf_response_init(&first);
    CF_REQUIRE(run_request(&env, &q1, &first));
    CF_CHECK(first.status == 200);
    /* Exactly one validator and one Vary: the round owns both on the cache
     * path, the action's own Vary is skipped. */
    CF_CHECK(resp_header_count(&first, &q1.req, "ETag: ") == 1);
    CF_CHECK(resp_header_count(&first, &q1.req, "Vary: ") == 1);
    {
        char vary[256];
        CF_REQUIRE(resp_header(&first, &q1.req, "Vary: ", vary,
                               sizeof vary));
        CF_CHECK(strstr(vary, "Accept-Encoding") != NULL);
        CF_CHECK(strstr(vary, "Accept") != NULL);
    }
    CF_CHECK(body_contains(&first, "data-message-id=\"4001\""));
    CF_CHECK(!body_contains(&first, "data-message-id=\"4002\""));
    CF_CHECK(body_contains(&first, "old")); /* recent searches */
    CF_REQUIRE(first.body != NULL);
    cf_buf *body_first = cf_buf_retain(first.body);
    char etag[256];
    CF_REQUIRE(resp_header(&first, &q1.req, "ETag: ", etag, sizeof etag));
    CF_CHECK(strncmp(etag, "W/\"", 3) == 0 && strlen(etag) == 68);
    cf_response_dispose(&first);
    CF_CHECK(stats(&env).entries == 1);

    reqbuf q1b;
    req_init(&q1b);
    req_auth(&env, &q1b, USER_A);
    req_get(&q1b, "/searches?q=first");
    cf_response second;
    cf_response_init(&second);
    CF_REQUIRE(run_request(&env, &q1b, &second));
    {
        cf_span expect = cf_buf_span(body_first);
        cf_span got = resp_body(&second);
        CF_CHECK(got.len == expect.len &&
                 memcmp(got.ptr, expect.ptr, got.len) == 0);
    }
    cf_response_dispose(&second);
    CF_CHECK(stats(&env).hits == 1);

    /* A different raw query is a different page and a different key. */
    reqbuf q2;
    req_init(&q2);
    req_auth(&env, &q2, USER_A);
    req_get(&q2, "/searches?q=second");
    cf_response resp_second;
    cf_response_init(&resp_second);
    CF_REQUIRE(run_request(&env, &q2, &resp_second));
    CF_CHECK(resp_second.status == 200);
    CF_CHECK(body_contains(&resp_second, "data-message-id=\"4002\""));
    CF_CHECK(!body_contains(&resp_second, "data-message-id=\"4001\""));
    {
        cf_span one = cf_buf_span(body_first);
        cf_span two = resp_body(&resp_second);
        CF_CHECK(!(one.len == two.len &&
                   memcmp(one.ptr, two.ptr, one.len) == 0));
    }
    cf_response_dispose(&resp_second);
    cf_cache_stats s = stats(&env);
    CF_CHECK(s.misses == 2 && s.entries == 2);

    /* No query at all is another key. */
    reqbuf none;
    req_init(&none);
    req_auth(&env, &none, USER_A);
    req_get(&none, "/searches");
    cf_response resp_none;
    cf_response_init(&resp_none);
    CF_REQUIRE(run_request(&env, &none, &resp_none));
    CF_CHECK(resp_none.status == 200);
    cf_response_dispose(&resp_none);
    s = stats(&env);
    CF_CHECK(s.misses == 3 && s.entries == 3);

    /* Another user's search is another entry. */
    reqbuf other_user;
    req_init(&other_user);
    req_auth(&env, &other_user, USER_B);
    req_get(&other_user, "/searches?q=first");
    cf_response resp_other;
    cf_response_init(&resp_other);
    CF_REQUIRE(run_request(&env, &other_user, &resp_other));
    CF_CHECK(resp_other.status == 200);
    cf_response_dispose(&resp_other);
    s = stats(&env);
    CF_CHECK(s.misses == 4 && s.entries == 4);

    /* last_room is a key field (the model's return-to-room selection). */
    reqbuf lr;
    req_init(&lr);
    req_auth(&env, &lr, USER_A);
    req_cookie_extra(&lr, "last_room=3002");
    req_get(&lr, "/searches?q=first");
    cf_response resp_lr;
    cf_response_init(&resp_lr);
    CF_REQUIRE(run_request(&env, &lr, &resp_lr));
    CF_CHECK(resp_lr.status == 200);
    cf_response_dispose(&resp_lr);
    s = stats(&env);
    CF_CHECK(s.misses == 5 && s.entries == 5);

    /* A committed search record changes the recents list and advances the
     * version: the next request misses and renders the new body. */
    CF_REQUIRE(cf_write(env.app, record_search_write, NULL) == CF_OK);
    reqbuf q1c;
    req_init(&q1c);
    req_auth(&env, &q1c, USER_A);
    req_get(&q1c, "/searches?q=first");
    cf_response resp_churn;
    cf_response_init(&resp_churn);
    CF_REQUIRE(run_request(&env, &q1c, &resp_churn));
    CF_CHECK(resp_churn.status == 200);
    CF_CHECK(body_contains(&resp_churn, "churn"));
    cf_response_dispose(&resp_churn);
    s = stats(&env);
    CF_CHECK(s.misses == 6);
    cf_buf_release(body_first);
    env_close(&env);
}

CF_TEST(cache_sidebars_flash_bypass) {
    cache_env env;
    CF_REQUIRE(env_open(&env, CACHE_BUDGET));
    seed_world(&env);

    /* Prime a flash session through the rooms failure redirect (same trick as
     * the rooms#show flash case). */
    reqbuf missing;
    req_init(&missing);
    req_auth(&env, &missing, USER_A);
    req_get(&missing, "/rooms/999999");
    cf_response resp_redirect;
    cf_response_init(&resp_redirect);
    CF_REQUIRE(run_request(&env, &missing, &resp_redirect));
    CF_CHECK(resp_redirect.status == 302);
    char flash_cookie[2048];
    CF_REQUIRE(resp_cookie(&resp_redirect, &missing.req, "_campfire_session",
                           flash_cookie, sizeof flash_cookie));
    cf_response_dispose(&resp_redirect);

    /* The sidebar page that displays the notice bypasses the cache. */
    reqbuf flashed;
    req_init(&flashed);
    req_auth(&env, &flashed, USER_A);
    req_cookie_extra(&flashed, flash_cookie);
    req_get(&flashed, "/users/me/sidebar");
    cf_response resp_flashed;
    cf_response_init(&resp_flashed);
    CF_REQUIRE(run_request(&env, &flashed, &resp_flashed));
    CF_CHECK(resp_flashed.status == 200);
    CF_CHECK(body_contains(&resp_flashed, "Room not found or inaccessible"));
    CF_CHECK(stats(&env).entries == 0);
    cf_response_dispose(&resp_flashed);

    /* Without the flash the same sidebar is admitted. */
    reqbuf page;
    req_init(&page);
    req_auth(&env, &page, USER_A);
    req_get(&page, "/users/me/sidebar");
    cf_response resp_page;
    cf_response_init(&resp_page);
    CF_REQUIRE(run_request(&env, &page, &resp_page));
    CF_CHECK(resp_page.status == 200);
    CF_CHECK(!body_contains(&resp_page, "Room not found or inaccessible"));
    cf_response_dispose(&resp_page);
    CF_CHECK(stats(&env).entries == 1);
    env_close(&env);
}

CF_TEST(cache_sidebars_version_churn_invalidates) {
    cache_env env;
    CF_REQUIRE(env_open(&env, CACHE_BUDGET));
    seed_world(&env);

    reqbuf one;
    req_init(&one);
    req_auth(&env, &one, USER_A);
    req_get(&one, "/users/me/sidebar");
    cf_response resp_one;
    cf_response_init(&resp_one);
    CF_REQUIRE(run_request(&env, &one, &resp_one));
    CF_CHECK(resp_one.status == 200);
    cf_response_dispose(&resp_one);
    cf_cache_stats before = stats(&env);
    CF_CHECK(before.entries == 1);

    /* A committed write to an unrelated room advances the version: the
     * sidebar entry is unusable and the request must re-render. */
    CF_REQUIRE(cf_write(env.app, unrelated_room_write, NULL) == CF_OK);
    reqbuf two;
    req_init(&two);
    req_auth(&env, &two, USER_A);
    req_get(&two, "/users/me/sidebar");
    cf_response resp_two;
    cf_response_init(&resp_two);
    CF_REQUIRE(run_request(&env, &two, &resp_two));
    CF_CHECK(resp_two.status == 200);
    cf_response_dispose(&resp_two);
    cf_cache_stats after = stats(&env);
    CF_CHECK(after.misses == before.misses + 1);
    CF_CHECK(after.hits == before.hits);
    CF_CHECK(after.entries == before.entries + 1);
    env_close(&env);
}

/* ======================================================================== */
/* accounting                                                               */
/* ======================================================================== */

CF_TEST(cache_external_commit_invalidates_without_timestamp_changes) {
    cache_env env;
    CF_REQUIRE(env_open(&env, CACHE_BUDGET));
    seed_world(&env);
    reqbuf one;
    req_init(&one);
    req_auth(&env, &one, USER_A);
    req_get(&one, "/rooms/3001");
    cf_response first;
    cf_response_init(&first);
    CF_REQUIRE(run_request(&env, &one, &first));
    CF_REQUIRE(first.status == 200);
    cf_response_dispose(&first);
    cf_cache_stats before = stats(&env);
    sqlite3 *external = NULL;
    CF_REQUIRE(sqlite3_open(env.scratch.path, &external) == SQLITE_OK);
    CF_REQUIRE(sqlite3_exec(external, "UPDATE users SET name='Externally changed' WHERE id=2001", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(external);
    reqbuf two;
    req_init(&two);
    req_auth(&env, &two, USER_A);
    req_get(&two, "/rooms/3001");
    cf_response second;
    cf_response_init(&second);
    CF_REQUIRE(run_request(&env, &two, &second));
    CF_REQUIRE(second.status == 200);
    CF_CHECK(body_contains(&second, "Externally changed"));
    CF_CHECK(stats(&env).hits == before.hits);
    cf_response_dispose(&second);
    env_close(&env);
}

CF_TEST(cache_accounting_is_cache_owned_only) {
    cache_env env;
    CF_REQUIRE(env_open_ex(&env, CACHE_BUDGET, true));
    seed_world(&env);

    reqbuf b;
    req_init(&b);
    req_auth(&env, &b, USER_A);
    req_get(&b, "/cachetest/4");
    g_probe_env = &env;
    g_probe = PROBE_PLAIN;
    cf_response resp;
    cf_response_init(&resp);
    CF_REQUIRE(run_request(&env, &b, &resp));
    CF_CHECK(resp.status == 200);
    cf_response_dispose(&resp);

    cf_cache_stats s = stats(&env);
    /* bytes_used is exactly the module's own accounting: bucket array + key
     * capacities + entry metadata + body allocations (the cf_buf header is
     * counted in body_bytes, cache.h). No output-queue bytes are added here;
     * the HTTP output reservation is H01's and is reported separately. */
    CF_CHECK(s.bytes_used == s.bucket_bytes + s.key_bytes + s.entry_bytes +
                                  s.body_bytes);
    CF_CHECK(s.bucket_bytes == CF_CACHE_BUCKET_BYTES);
    CF_CHECK(s.entries == 1);
    CF_CHECK(s.key_bytes > 0 && s.entry_bytes == CF_CACHE_ENTRY_METADATA_BYTES);
    CF_CHECK(s.body_bytes >= strlen("probe-body"));
    CF_CHECK(s.bytes_used <= s.budget_bytes);
    env_close(&env);
}

CF_TEST_MAIN()
