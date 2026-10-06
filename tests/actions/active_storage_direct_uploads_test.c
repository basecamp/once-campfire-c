/* tests/actions/active_storage_direct_uploads_test.c — S02-C
 * acceptance: `active_storage/direct_uploads#create` (route ID 177).
 *
 * Cases:
 *   - route binding: ID 177 resolves to the create action;
 *   - auth: no session is 401; a forged (cross-site) request with a
 *     session is 422;
 *   - parameter matrix: a missing/non-object `blob` is 400; a missing or
 *     empty filename or checksum is 422; a missing or non-numeric
 *     `byte_size` is 422; a `byte_size` over 16 MiB is 413;
 *   - success: 200 `application/json; charset=utf-8` with the exact
 *     as_json shape (id, key, filename, content_type, metadata,
 *     service_name, byte_size, checksum, created_at, signed_id,
 *     direct_upload.url/headers); the signed_id verifies to the inserted
 *     row, the key is a 28-char storage key, the URL is PUBLIC_ORIGIN +
 *     the disk path, and the row exists in the database with matching
 *     fields;
 *   - metadata: a JSON `blob.metadata` object round-trips verbatim into
 *     the stored row and the response;
 *   - numbers: a numeric `byte_size` (as the JavaScript client sends) is
 *     accepted.
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
#include "http/params.h"
#include "models/active_storage.h"
#include "models/session.h"
#include "storage/active_storage.h"
#include "storage/storage.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <dirent.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <yyjson.h>

cf_err cf_action_active_storage_direct_uploads_create(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
    char storage_root[256];
} du_env;

static void remove_tree(const char *path) {
    DIR *dir = opendir(path);
    if (dir == NULL) {
        (void)unlink(path);
        return;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[512];
        snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
        remove_tree(child);
    }
    closedir(dir);
    (void)rmdir(path);
}

static bool env_open(du_env *env) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;
    snprintf(env->storage_root, sizeof env->storage_root,
             "/tmp/cf-s02c-du-XXXXXX");
    if (mkdtemp(env->storage_root) == NULL) return false;
    cf_config_entry entries[5] = {
        {"PUBLIC_ORIGIN", ORIGIN},
        {"SECRET_KEY_BASE", HEX64},
        {"VAPID_PUBLIC_KEY", VAPID_PUBLIC_KEY},
        {"DATABASE_PATH", env->scratch.path},
        {"STORAGE_PATH", env->storage_root},
    };
    if (cf_config_parse(entries, 5, NULL, &env->config) != CF_OK) return false;
    if (cf_app_create(env->config, &env->app) != CF_OK) {
        cf_config_destroy(env->config);
        env->config = NULL;
        return false;
    }
    if (cf_writer_start(env->app, env->config) != CF_OK) return false;
    env->writer_started = true;
    cf_test_routes_reset();
    if (cf_test_routes_add("POST",
                           "/rails/active_storage/direct_uploads(.:format)",
                           177,
                           cf_action_active_storage_direct_uploads_create) !=
            CF_OK ||
        cf_test_routes_add("POST", "/rails/active_storage/direct_uploads",
                           177,
                           cf_action_active_storage_direct_uploads_create) !=
            CF_OK) {
        return false;
    }
    return true;
}

static void env_close(du_env *env) {
    if (env->writer_started) cf_writer_stop(env->app);
    if (env->app != NULL) cf_app_destroy(env->app);
    env->config = NULL;
    cf_db_scratch_close(&env->scratch);
    remove_tree(env->storage_root);
    memset(env, 0, sizeof *env);
}

static cf_span secret_of(du_env *env) {
    return (cf_span){(const unsigned char *)env->config->secret_key_base,
                     env->config->secret_key_base_len};
}

static void seed_user(du_env *env, int64_t id) {
    sqlite3 *handle = cf_db_handle(env->scratch.db);
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, "
             "updated_at) VALUES (%lld, NULL, NULL, "
             "'2026-09-26 13:00:20.000000', 'member@example.com', "
             "'Member', NULL, 0, 0, '2026-09-26 13:00:20.000000')",
             (long long)id);
    CF_REQUIRE(sqlite3_exec(handle, sql, NULL, NULL, NULL) == SQLITE_OK);
}

static void make_session_cookie(du_env *env, char *out, size_t cap,
                                const char *token, int64_t user_id) {
    sqlite3 *handle = cf_db_handle(env->scratch.db);
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO sessions (created_at, ip_address, last_active_at, "
             "token, updated_at, user_agent, user_id) VALUES "
             "('2040-01-01 00:00:00.000000', NULL, "
             "'2040-01-01 00:00:00.000000', '%s', "
             "'2040-01-01 00:00:00.000000', NULL, %lld)",
             token, (long long)user_id);
    CF_REQUIRE(sqlite3_exec(handle, sql, NULL, NULL, NULL) == SQLITE_OK);
    cf_str wire = {0};
    CF_REQUIRE(cf_auth_signed_cookie_generate(
                   secret_of(env), SP("session_token"),
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

static bool run_request(du_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

typedef struct {
    const char *body;
    const char *cookie;
    const char *fetch_site; /* NULL omits the header */
} du_headers;

static void du_request(cf_request *req, const du_headers *headers) {
    static char body_store[65536];
    cf_test_req_init(req);
    req->method = CF_POST;
    req->original_method = CF_POST;
    req->path = SP("/rails/active_storage/direct_uploads");
    req->target = SP("/rails/active_storage/direct_uploads");
    if (headers != NULL && headers->body != NULL) {
        snprintf(body_store, sizeof body_store, "%s", headers->body);
        req->body = SP(body_store);
    } else {
        req->body = SP("");
    }
    CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                  SP("application/json")) == CF_OK);
    if (headers != NULL && headers->fetch_site != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                      SP(headers->fetch_site)) == CF_OK);
    }
    if (headers != NULL && headers->cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"),
                                      SP(headers->cookie)) == CF_OK);
    }
}

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
    char got[8192];
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

/* --- acceptance ------------------------------------------------------------ */

#include "auth_write_race.h"

CF_TEST(active_storage_direct_uploads_route_id_binds_the_action) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("POST",
                                  "/rails/active_storage/direct_uploads",
                                  177,
                                  cf_action_active_storage_direct_uploads_create) ==
               CF_OK);
    CF_CHECK(cf_route_action(177) ==
             cf_action_active_storage_direct_uploads_create);
}

CF_TEST(active_storage_direct_uploads_auth_checks) {
    du_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(&env, 1);
    char cookie[4096];
    make_session_cookie(&env, cookie, sizeof cookie, "session-token-1", 1);
    static const char body[] =
        "{\"blob\":{\"filename\":\"a.png\",\"byte_size\":\"3\","
        "\"checksum\":\"cs\",\"content_type\":\"image/png\"}}";

    /* No session: 401. */
    cf_request req;
    cf_response resp;
    du_headers no_session = {.body = body, .fetch_site = "same-origin"};
    du_request(&req, &no_session);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 401);
    cf_response_dispose(&resp);

    /* Forged cross-site request with a session: 422. */
    cf_request req2;
    cf_response resp2;
    du_headers forged = {
        .body = body, .cookie = cookie, .fetch_site = "cross-site"};
    du_request(&req2, &forged);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 422);
    cf_response_dispose(&resp2);

    env_close(&env);
}

CF_TEST(active_storage_direct_uploads_parameter_matrix) {
    du_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(&env, 1);
    char cookie[4096];
    make_session_cookie(&env, cookie, sizeof cookie, "session-token-1", 1);

    static const struct {
        const char *body;
        unsigned want;
    } cases[] = {
        /* Missing/non-object blob: 400. */
        {"{}", 400},
        {"{\"blob\":42}", 400},
        /* Missing/empty filename or checksum: 422. */
        {"{\"blob\":{\"byte_size\":\"3\",\"checksum\":\"cs\"}}", 422},
        {"{\"blob\":{\"filename\":\"\",\"byte_size\":\"3\",\"checksum\":"
         "\"cs\"}}",
         422},
        {"{\"blob\":{\"filename\":\"a.png\",\"byte_size\":\"3\"}}", 422},
        {"{\"blob\":{\"filename\":\"a.png\",\"byte_size\":\"3\","
         "\"checksum\":\"\"}}",
         422},
        /* Missing/non-numeric byte_size: 422. */
        {"{\"blob\":{\"filename\":\"a.png\",\"checksum\":\"cs\"}}", 422},
        {"{\"blob\":{\"filename\":\"a.png\",\"byte_size\":\"abc\","
         "\"checksum\":\"cs\"}}",
         422},
        /* Over 16 MiB: 413. */
        {"{\"blob\":{\"filename\":\"a.png\",\"byte_size\":\"16777217\","
         "\"checksum\":\"cs\"}}",
         413},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cf_request req;
        cf_response resp;
        du_headers headers = {.body = cases[i].body,
                              .cookie = cookie,
                              .fetch_site = "same-origin"};
        du_request(&req, &headers);
        CF_REQUIRE(run_request(&env, &req, &resp));
        if (resp.status != cases[i].want) {
            fprintf(stderr, "  case %zu: status %u, want %u (%s)\n", i,
                    resp.status, cases[i].want, cases[i].body);
        }
        CF_CHECK(resp.status == cases[i].want);
        cf_response_dispose(&resp);
    }

    env_close(&env);
}

CF_TEST(active_storage_direct_uploads_create_success) {
    du_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(&env, 1);
    char cookie[4096];
    make_session_cookie(&env, cookie, sizeof cookie, "session-token-1", 1);
    static const char body[] =
        "{\"blob\":{\"filename\":\"avatar.png\",\"byte_size\":\"1024\","
        "\"checksum\":\"YmFzZTY0LW1kNS8=\",\"content_type\":\"image/png\"}}";

    cf_request req;
    cf_response resp;
    du_headers headers = {
        .body = body, .cookie = cookie, .fetch_site = "same-origin"};
    du_request(&req, &headers);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(header_equals(&resp, &req, "Content-Type",
                           "application/json; charset=utf-8"));
    CF_REQUIRE(resp.body_kind == CF_BODY_BUFFER);
    cf_span text = cf_buf_span(resp.body);

    yyjson_doc *doc = yyjson_read((char *)text.ptr, text.len, 0);
    CF_REQUIRE(doc != NULL);
    yyjson_val *root = yyjson_doc_get_root(doc);
    CF_REQUIRE(yyjson_is_obj(root));
    yyjson_val *filename = yyjson_obj_get(root, "filename");
    CF_REQUIRE(yyjson_is_str(filename));
    CF_CHECK(strcmp(yyjson_get_str(filename), "avatar.png") == 0);
    yyjson_val *byte_size = yyjson_obj_get(root, "byte_size");
    CF_REQUIRE(yyjson_is_int(byte_size));
    CF_CHECK(yyjson_get_sint(byte_size) == 1024);
    yyjson_val *content_type = yyjson_obj_get(root, "content_type");
    CF_REQUIRE(yyjson_is_str(content_type));
    CF_CHECK(strcmp(yyjson_get_str(content_type), "image/png") == 0);
    yyjson_val *service = yyjson_obj_get(root, "service_name");
    CF_REQUIRE(yyjson_is_str(service));
    CF_CHECK(strcmp(yyjson_get_str(service), "local") == 0);
    yyjson_val *key = yyjson_obj_get(root, "key");
    CF_REQUIRE(yyjson_is_str(key));
    CF_CHECK(yyjson_get_len(key) == CF_STORAGE_KEY_LENGTH);
    CF_CHECK(cf_storage_key_valid(
                 (cf_span){(const unsigned char *)yyjson_get_str(key),
                           yyjson_get_len(key)}));
    yyjson_val *checksum = yyjson_obj_get(root, "checksum");
    CF_REQUIRE(yyjson_is_str(checksum));
    CF_CHECK(strcmp(yyjson_get_str(checksum), "YmFzZTY0LW1kNS8=") == 0);
    yyjson_val *created = yyjson_obj_get(root, "created_at");
    CF_REQUIRE(yyjson_is_str(created));

    /* signed_id verifies to the inserted row id. */
    yyjson_val *signed_id = yyjson_obj_get(root, "signed_id");
    CF_REQUIRE(yyjson_is_str(signed_id));
    int64_t row_id = 0;
    bool found = false;
    CF_REQUIRE(cf_active_blob_verify(
                   secret_of(&env),
                   (cf_span){(const unsigned char *)yyjson_get_str(signed_id),
                             yyjson_get_len(signed_id)},
                   cf_now_us(NULL), &row_id, &found) == CF_OK);
    CF_CHECK(found);
    yyjson_val *id = yyjson_obj_get(root, "id");
    CF_REQUIRE(yyjson_is_int(id));
    CF_CHECK(yyjson_get_sint(id) == row_id);

    /* direct_upload URL + headers. */
    yyjson_val *direct = yyjson_obj_get(root, "direct_upload");
    CF_REQUIRE(yyjson_is_obj(direct));
    yyjson_val *url = yyjson_obj_get(direct, "url");
    CF_REQUIRE(yyjson_is_str(url));
    CF_CHECK(strncmp(yyjson_get_str(url), ORIGIN,
                     strlen(ORIGIN)) == 0);
    CF_CHECK(strstr(yyjson_get_str(url), "/rails/active_storage/disk/") !=
             NULL);
    yyjson_val *du_headers = yyjson_obj_get(direct, "headers");
    CF_REQUIRE(yyjson_is_obj(du_headers));
    yyjson_val *upper_ct = yyjson_obj_get(du_headers, "Content-Type");
    CF_REQUIRE(yyjson_is_str(upper_ct));
    CF_CHECK(strcmp(yyjson_get_str(upper_ct), "image/png") == 0);

    /* The row exists with matching fields. */
    cf_blob row;
    memset(&row, 0, sizeof row);
    CF_REQUIRE(cf_blob_find(env.scratch.db, row_id, &row) == CF_OK);
    CF_CHECK(row.byte_size == 1024);
    CF_CHECK(row.filename.len == strlen("avatar.png") &&
             memcmp(row.filename.ptr, "avatar.png", row.filename.len) == 0);
    CF_CHECK(row.content_type.present);
    CF_CHECK(row.checksum.present);
    CF_CHECK(row.service_name.len == strlen("local"));
    cf_blob_dispose(&row);

    yyjson_doc_free(doc);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(active_storage_direct_uploads_metadata_round_trip) {
    du_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(&env, 1);
    char cookie[4096];
    make_session_cookie(&env, cookie, sizeof cookie, "session-token-1", 1);
    static const char body[] =
        "{\"blob\":{\"filename\":\"a.png\",\"byte_size\":7,"
        "\"checksum\":\"cs\",\"metadata\":{\"identified\":true,\"width\":"
        "640}}}";

    cf_request req;
    cf_response resp;
    du_headers headers = {
        .body = body, .cookie = cookie, .fetch_site = "same-origin"};
    du_request(&req, &headers);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_REQUIRE(resp.body_kind == CF_BODY_BUFFER);
    cf_span text = cf_buf_span(resp.body);
    yyjson_doc *doc = yyjson_read((char *)text.ptr, text.len, 0);
    CF_REQUIRE(doc != NULL);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *metadata = yyjson_obj_get(root, "metadata");
    CF_REQUIRE(yyjson_is_obj(metadata));
    yyjson_val *width = yyjson_obj_get(metadata, "width");
    CF_REQUIRE(yyjson_is_int(width));
    CF_CHECK(yyjson_get_sint(width) == 640);
    yyjson_val *identified = yyjson_obj_get(metadata, "identified");
    CF_REQUIRE(yyjson_is_bool(identified));
    CF_CHECK(yyjson_get_bool(identified));
    yyjson_val *id = yyjson_obj_get(root, "id");
    CF_REQUIRE(yyjson_is_int(id));

    /* Stored verbatim in the row. */
    cf_blob row;
    memset(&row, 0, sizeof row);
    CF_REQUIRE(cf_blob_find(env.scratch.db, yyjson_get_sint(id), &row) ==
               CF_OK);
    CF_REQUIRE(row.metadata.present);
    yyjson_doc *stored =
        yyjson_read(row.metadata.value.ptr, row.metadata.value.len, 0);
    CF_REQUIRE(stored != NULL);
    yyjson_val *stored_root = yyjson_doc_get_root(stored);
    CF_REQUIRE(yyjson_is_obj(stored_root));
    yyjson_val *stored_width = yyjson_obj_get(stored_root, "width");
    CF_REQUIRE(yyjson_is_int(stored_width));
    CF_CHECK(yyjson_get_sint(stored_width) == 640);
    yyjson_doc_free(stored);
    cf_blob_dispose(&row);

    yyjson_doc_free(doc);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(active_storage_direct_uploads_rejects_queued_ban) {
    const char *changes[] = {"UPDATE users SET status=2 WHERE id=1",
                             "UPDATE users SET status=1 WHERE id=1"};
    for (size_t i = 0; i < 2; i++) {
        du_env env;
        CF_REQUIRE(env_open(&env));
        seed_user(&env, 1);
        char cookie[4096];
        make_session_cookie(&env, cookie, sizeof cookie, "session-token-1", 1);
        cf_request req;
        cf_response resp;
        du_headers headers = {.body =
                                  "{\"blob\":{\"filename\":\"race.txt\",\"byte_"
                                  "size\":1,\"checksum\":\"checksum\"}}",
                              .cookie = cookie,
                              .fetch_site = "same-origin"};
        du_request(&req, &headers);
        auth_write_race race;
        CF_REQUIRE(
            auth_race_begin(env.app, env.scratch.path, &race, changes[i]));
        CF_REQUIRE(
            auth_race_request(env.app, env.scratch.db, &race, &req, &resp));
        auth_race_end(&race);
        CF_CHECK(resp.status == 403);
        sqlite3_stmt *stmt = NULL;
        CF_REQUIRE(
            sqlite3_prepare_v2(cf_db_handle(env.scratch.db),
                               "SELECT count(*) FROM active_storage_blobs", -1,
                               &stmt, NULL) == SQLITE_OK);
        CF_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        CF_CHECK(sqlite3_column_int(stmt, 0) == 0);
        sqlite3_finalize(stmt);
        cf_response_dispose(&resp);
        env_close(&env);
    }
}
CF_TEST_MAIN()
