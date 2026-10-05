/* tests/actions/active_storage_blobs_test.c — S02-C acceptance:
 * `active_storage/blobs#show` redirect (route IDs 169, 171) and proxy
 * (route ID 170).
 *
 * Cases:
 *   - route binding: IDs 169/170/171 resolve to the redirect/proxy actions;
 *   - redirect: 302 to PUBLIC_ORIGIN + the disk path, Content-Type
 *     text/html, Cache-Control max-age=300, private; the Location's disk
 *     key verifies and round-trips through the disk show (proving the
 *     redirect bakes the forced/inline disposition); ?disposition=attachment
 *     changes the Location;
 *   - redirect failure paths: tampered/wrong-purpose/expired signed ids
 *     are the 404 head; a valid id for a missing blob is the mapped 404;
 *   - proxy 200: serving content type, inline disposition for the blob
 *     filename, content-transfer-encoding binary, Accept-Ranges bytes, the
 *     100-year ETag/Cache-Control/Last-Modified, exact file bytes; the
 *     returned ETag round-trips to a 304 (no Content-Type, empty body) and
 *     a non-matching validator serves 200 again; forced-attachment types
 *     (text/html) serve as octet-stream attachments;
 *   - proxy ranges: single (206 + Content-Range + slice), multi
 *     (multipart/byteranges with both slices), unsatisfiable and malformed
 *     (416 head);
 *   - proxy missing file: 404 with Cache-Control no-cache; missing file
 *     with a Range is a 500 (the reference raises internally);
 *   - public: no session cookie is sent on any success case above.
 *
 * The route double binds rows 169-171 to the real actions, so every case
 * runs the real A00 dispatch path. The action symbols are declared here;
 * the integrator's routes.c rebind needs the same declarations
 * (c_symbols cf_action_active_storage_blobs_redirect_show,
 * cf_action_active_storage_blobs_proxy_show; routes 169/171 -> redirect,
 * 170 -> proxy).
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
#include "storage/active_storage.h"
#include "storage/storage.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <dirent.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

cf_err cf_action_active_storage_blobs_redirect_show(cf_ctx *ctx);
cf_err cf_action_active_storage_blobs_proxy_show(cf_ctx *ctx);
/* Route 175 for the redirect Location round-trip. */
cf_err cf_action_active_storage_disk_show(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="
#define KEY1 "aaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define KEY2 "bbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define KEY3 "cccccccccccccccccccccccccccc"

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- scratch app + storage ------------------------------------------------- */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
    char storage_root[256];
} bl_env;

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

static bool env_open(bl_env *env) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;
    snprintf(env->storage_root, sizeof env->storage_root,
             "/tmp/cf-s02c-blobs-XXXXXX");
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
    if (cf_test_routes_add(
            "GET", "/rails/active_storage/blobs/redirect/:signed_id/*filename(.:format)",
            169, cf_action_active_storage_blobs_redirect_show) != CF_OK ||
        cf_test_routes_add(
            "GET", "/rails/active_storage/blobs/proxy/:signed_id/*filename(.:format)",
            170, cf_action_active_storage_blobs_proxy_show) != CF_OK ||
        cf_test_routes_add(
            "GET", "/rails/active_storage/blobs/:signed_id/*filename(.:format)",
            171, cf_action_active_storage_blobs_redirect_show) != CF_OK ||
        cf_test_routes_add(
            "GET", "/rails/active_storage/disk/:encoded_key/*filename(.:format)",
            175, cf_action_active_storage_disk_show) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(bl_env *env) {
    if (env->writer_started) cf_writer_stop(env->app);
    if (env->app != NULL) cf_app_destroy(env->app);
    env->config = NULL;
    cf_db_scratch_close(&env->scratch);
    remove_tree(env->storage_root);
    memset(env, 0, sizeof *env);
}

static void seed_blob(cf_db *db, int64_t id, const char *key,
                       const char *filename, const char *content_type,
                       int64_t byte_size, const char *checksum) {
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(
                   handle,
                   "INSERT INTO active_storage_blobs (id, byte_size, "
                   "checksum, content_type, created_at, filename, key, "
                   "metadata, service_name) VALUES (?1, ?2, ?3, ?4, "
                   "'2026-09-26 13:00:20.000000', ?5, ?6, '{}', 'local')",
                   -1, &stmt, NULL) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_int64(stmt, 1, id) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_int64(stmt, 2, byte_size) == SQLITE_OK);
    if (checksum != NULL) {
        CF_REQUIRE(sqlite3_bind_text(stmt, 3, checksum, -1,
                                     SQLITE_TRANSIENT) == SQLITE_OK);
    } else {
        CF_REQUIRE(sqlite3_bind_null(stmt, 3) == SQLITE_OK);
    }
    if (content_type != NULL) {
        CF_REQUIRE(sqlite3_bind_text(stmt, 4, content_type, -1,
                                     SQLITE_TRANSIENT) == SQLITE_OK);
    } else {
        CF_REQUIRE(sqlite3_bind_null(stmt, 4) == SQLITE_OK);
    }
    CF_REQUIRE(sqlite3_bind_text(stmt, 5, filename, -1, SQLITE_TRANSIENT) ==
               SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_text(stmt, 6, key, -1, SQLITE_TRANSIENT) ==
               SQLITE_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
}

/* Stage `len` bytes at `key` through the S01 upload boundary. */
static void stage_file(bl_env *env, const char *key,
                       const unsigned char *bytes, size_t len) {
    cf_storage *storage = NULL;
    CF_REQUIRE(cf_storage_open(env->storage_root, &storage) == CF_OK);
    cf_storage_upload *upload = NULL;
    CF_REQUIRE(cf_storage_upload_begin(storage, &upload) == CF_OK);
    CF_REQUIRE(cf_storage_upload_write(
                   upload, (cf_span){bytes, len}) == CF_OK);
    CF_REQUIRE(cf_storage_upload_move(
                   upload, (cf_span){(const unsigned char *)key,
                                     strlen(key)}) == CF_OK);
    CF_REQUIRE(cf_storage_upload_commit(upload) == CF_OK);
    cf_storage_upload_dispose(upload);
    cf_storage_close(storage);
}

static cf_span secret_of(bl_env *env) {
    return (cf_span){(const unsigned char *)env->config->secret_key_base,
                     env->config->secret_key_base_len};
}

static void make_signed_id(bl_env *env, int64_t id, cf_str *out) {
    CF_REQUIRE(cf_active_blob_sign(secret_of(env), id, false, 0, out) ==
               CF_OK);
}

/* Percent-escape a token for one path segment (`/` and `%` only; `+` and
 * `=` stay literal in path segments). */
static void escape_token(const cf_str *token, char *out, size_t cap) {
    size_t at = 0;
    for (size_t i = 0; i < token->len && at + 4 < cap; i++) {
        unsigned char c = (unsigned char)token->ptr[i];
        if (c == '%') {
            at += (size_t)snprintf(out + at, cap - at, "%%25");
        } else if (c == '/') {
            at += (size_t)snprintf(out + at, cap - at, "%%2F");
        } else {
            out[at++] = (char)c;
        }
    }
    out[at] = '\0';
}

/* --- request/response helpers ---------------------------------------------- */

static bool run_request(bl_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

typedef struct {
    const char *query;
    const char *range;
    const char *if_none_match;
    const char *cookie;
} bl_headers;

static void bl_request(cf_request *req, const char *path,
                       const bl_headers *headers) {
    static char target[4096];
    cf_test_req_init(req);
    req->method = CF_GET;
    req->original_method = CF_GET;
    if (headers != NULL && headers->query != NULL &&
        headers->query[0] != '\0') {
        snprintf(target, sizeof target, "%s?%s", path, headers->query);
        req->path = SP(path);
        req->target = SP(target);
        req->query = SP(target + strlen(path) + 1);
    } else {
        req->path = SP(path);
        req->target = SP(path);
    }
    if (headers != NULL && headers->range != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Range"),
                                      SP(headers->range)) == CF_OK);
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
    char got[2048];
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
    char got[2048];
    if (response_header(resp, req, name, got, sizeof got)) {
        fprintf(stderr, "  header %s: present ('%s'), want absent\n", name,
                got);
        return false;
    }
    return true;
}

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
        ssize_t got =
            pread(resp->file_fd, copy, len, (off_t)resp->file_offset);
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

/* Substring search over an unterminated body buffer. */
static bool body_contains(const unsigned char *body, size_t len,
                          const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return true;
    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(body + i, needle, nlen) == 0) return true;
    }
    return false;
}

/* --- acceptance ------------------------------------------------------------ */

CF_TEST(active_storage_blobs_route_ids_bind_the_actions) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add(
                   "GET", "/rails/active_storage/blobs/redirect/:signed_id/*filename(.:format)",
                   169,
                   cf_action_active_storage_blobs_redirect_show) == CF_OK);
    CF_REQUIRE(cf_test_routes_add(
                   "GET", "/rails/active_storage/blobs/proxy/:signed_id/*filename(.:format)",
                   170,
                   cf_action_active_storage_blobs_proxy_show) == CF_OK);
    CF_REQUIRE(cf_test_routes_add(
                   "GET", "/rails/active_storage/blobs/:signed_id/*filename(.:format)",
                   171,
                   cf_action_active_storage_blobs_redirect_show) == CF_OK);
    CF_CHECK(cf_route_action(169) ==
             cf_action_active_storage_blobs_redirect_show);
    CF_CHECK(cf_route_action(170) ==
             cf_action_active_storage_blobs_proxy_show);
    CF_CHECK(cf_route_action(171) ==
             cf_action_active_storage_blobs_redirect_show);
}

CF_TEST(active_storage_blobs_redirect_is_a_disk_url) {
    bl_env env;
    CF_REQUIRE(env_open(&env));
    static const unsigned char bytes[] = "png-bytes";
    seed_blob(env.scratch.db, 1, KEY1, "file.png", "image/png",
              sizeof bytes - 1, "checksum-a");
    stage_file(&env, KEY1, bytes, sizeof bytes - 1);
    cf_str token = {0};
    make_signed_id(&env, 1, &token);
    char esc[1024];
    escape_token(&token, esc, sizeof esc);

    char path[2048];
    snprintf(path, sizeof path, "/rails/active_storage/blobs/redirect/%s/file.png",
             esc);
    cf_request req;
    cf_response resp;
    bl_request(&req, path, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(header_equals(&resp, &req, "Content-Type",
                           "text/html; charset=utf-8"));
    CF_CHECK(header_equals(&resp, &req, "Cache-Control",
                           "max-age=300, private"));
    char location[2048];
    CF_REQUIRE(response_header(&resp, &req, "Location", location,
                               sizeof location));
    CF_CHECK(strncmp(location, ORIGIN "/rails/active_storage/disk/", strlen(ORIGIN) + 26) == 0);
    CF_CHECK(strstr(location, "/file.png") != NULL);
    cf_response_dispose(&resp);

    /* The Location round-trips through the disk show: the baked key
     * verifies and serves the bytes. */
    const char *disk_path = location + strlen(ORIGIN);
    cf_request req2;
    cf_response resp2;
    bl_request(&req2, disk_path, NULL);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 200);
    unsigned char *body = NULL;
    size_t body_len = 0;
    CF_REQUIRE(response_body(&resp2, &body, &body_len));
    CF_CHECK(body_len == sizeof bytes - 1 &&
             memcmp(body, bytes, body_len) == 0);
    free(body);
    cf_response_dispose(&resp2);

    cf_str_dispose(&token);
    env_close(&env);
}

CF_TEST(active_storage_blobs_redirect_disposition_param) {
    bl_env env;
    CF_REQUIRE(env_open(&env));
    static const unsigned char bytes[] = "png-bytes";
    seed_blob(env.scratch.db, 1, KEY1, "file.png", "image/png",
              sizeof bytes - 1, "checksum-a");
    stage_file(&env, KEY1, bytes, sizeof bytes - 1);
    cf_str token = {0};
    make_signed_id(&env, 1, &token);
    char esc[1024];
    escape_token(&token, esc, sizeof esc);

    char path[2048];
    snprintf(path, sizeof path, "/rails/active_storage/blobs/redirect/%s/file.png",
             esc);
    char plain[2048], forced[2048];
    for (int i = 0; i < 2; i++) {
        cf_request req;
        cf_response resp;
        bl_headers headers = {
            .query = i == 0 ? NULL : "disposition=attachment",
        };
        bl_request(&req, path, i == 0 ? NULL : &headers);
        CF_REQUIRE(run_request(&env, &req, &resp));
        CF_CHECK(resp.status == 302);
        CF_REQUIRE(response_header(&resp, &req, "Location",
                                   i == 0 ? plain : forced,
                                   sizeof plain));
        cf_response_dispose(&resp);
    }
    /* The disposition is baked into the signed disk key. */
    CF_CHECK(strcmp(plain, forced) != 0);
    cf_str_dispose(&token);
    env_close(&env);
}

CF_TEST(active_storage_blobs_redirect_failure_paths) {
    bl_env env;
    CF_REQUIRE(env_open(&env));
    seed_blob(env.scratch.db, 1, KEY1, "file.png", "image/png", 9,
              "checksum-a");

    /* Tampered token: flip the last byte. */
    cf_str good = {0};
    make_signed_id(&env, 1, &good);
    char *tampered = malloc(good.len + 1);
    CF_REQUIRE(tampered != NULL);
    memcpy(tampered, good.ptr, good.len);
    tampered[good.len] = '\0';
    tampered[good.len - 1] = tampered[good.len - 1] == 'A' ? 'B' : 'A';
    char path[2048];
    snprintf(path, sizeof path, "/rails/active_storage/blobs/redirect/%s/file.png",
             tampered);
    cf_request req;
    cf_response resp;
    bl_request(&req, path, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    /* The head content type follows the request format (.png path). */
    CF_CHECK(header_equals(&resp, &req, "Content-Type", "image/png"));
    CF_CHECK(resp.body_kind == CF_BODY_NONE);
    cf_response_dispose(&resp);
    free(tampered);

    /* Wrong purpose: a disk key (purpose blob_key) as a signed id. */
    cf_str wrong = {0};
    CF_REQUIRE(cf_active_disk_key_sign(
                   secret_of(&env), SP(KEY1), SP("inline"), SP("image/png"),
                   true, SP("local"), false, 0, &wrong) == CF_OK);
    char esc[2048];
    cf_str wrong_wrap = wrong;
    escape_token(&wrong_wrap, esc, sizeof esc);
    snprintf(path, sizeof path, "/rails/active_storage/blobs/redirect/%s/file.png",
             esc);
    cf_request req2;
    cf_response resp2;
    bl_request(&req2, path, NULL);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 404);
    cf_response_dispose(&resp2);
    cf_str_dispose(&wrong);

    /* Expired signed id. */
    cf_str expired = {0};
    CF_REQUIRE(cf_active_blob_sign(secret_of(&env), 1, true,
                                   cf_now_us(NULL) - INT64_C(3600000000),
                                   &expired) == CF_OK);
    escape_token(&expired, esc, sizeof esc);
    snprintf(path, sizeof path, "/rails/active_storage/blobs/redirect/%s/file.png",
             esc);
    cf_request req3;
    cf_response resp3;
    bl_request(&req3, path, NULL);
    CF_REQUIRE(run_request(&env, &req3, &resp3));
    CF_CHECK(resp3.status == 404);
    cf_response_dispose(&resp3);
    cf_str_dispose(&expired);

    /* Valid signature for a missing blob: the mapped 404 (header-less
     * fresh response in the double bucket, like the avatars arm). */
    cf_str missing = {0};
    make_signed_id(&env, 999, &missing);
    escape_token(&missing, esc, sizeof esc);
    snprintf(path, sizeof path, "/rails/active_storage/blobs/redirect/%s/file.png",
             esc);
    cf_request req4;
    cf_response resp4;
    bl_request(&req4, path, NULL);
    CF_REQUIRE(run_request(&env, &req4, &resp4));
    CF_CHECK(resp4.status == 404);
    CF_CHECK(resp4.body_kind == CF_BODY_NONE);
    cf_response_dispose(&resp4);
    cf_str_dispose(&missing);

    cf_str_dispose(&good);
    env_close(&env);
}

CF_TEST(active_storage_blobs_proxy_full_and_fresh) {
    bl_env env;
    CF_REQUIRE(env_open(&env));
    static const unsigned char bytes[] = "0123456789abcdef";
    seed_blob(env.scratch.db, 1, KEY1, "file.png", "image/png",
              sizeof bytes - 1, "checksum-a");
    stage_file(&env, KEY1, bytes, sizeof bytes - 1);
    cf_str token = {0};
    make_signed_id(&env, 1, &token);
    char esc[1024];
    escape_token(&token, esc, sizeof esc);

    char path[2048];
    snprintf(path, sizeof path, "/rails/active_storage/blobs/proxy/%s/file.png",
             esc);
    cf_request req;
    cf_response resp;
    bl_request(&req, path, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(header_equals(&resp, &req, "Content-Type", "image/png"));
    CF_CHECK(header_equals(
        &resp, &req, "Content-Disposition",
        "inline; filename=\"file.png\"; filename*=UTF-8''file.png"));
    CF_CHECK(header_equals(&resp, &req, "content-transfer-encoding",
                           "binary"));
    CF_CHECK(header_equals(&resp, &req, "Accept-Ranges", "bytes"));
    CF_CHECK(header_equals(&resp, &req, "Cache-Control",
                           "max-age=3155695200, public, immutable"));
    CF_CHECK(header_equals(&resp, &req, "Last-Modified",
                           "Sat, 01 Jan 2011 00:00:00 GMT"));
    char etag[128];
    CF_REQUIRE(response_header(&resp, &req, "ETag", etag, sizeof etag));
    CF_CHECK(strncmp(etag, "W/\"", 3) == 0 && strlen(etag) == 36);
    unsigned char *body = NULL;
    size_t body_len = 0;
    CF_REQUIRE(response_body(&resp, &body, &body_len));
    CF_CHECK(body_len == sizeof bytes - 1 &&
             memcmp(body, bytes, body_len) == 0);
    free(body);
    cf_response_dispose(&resp);

    /* The ETag round-trips to a 304 with no body and no Content-Type. */
    cf_request req2;
    cf_response resp2;
    bl_headers fresh = {.if_none_match = etag};
    bl_request(&req2, path, &fresh);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 304);
    CF_CHECK(resp2.body_kind == CF_BODY_NONE);
    CF_CHECK(header_equals(&resp2, &req2, "ETag", etag));
    CF_CHECK(header_absent(&resp2, &req2, "Content-Type"));
    cf_response_dispose(&resp2);

    /* A non-matching validator serves the 200 again. */
    cf_request req3;
    cf_response resp3;
    bl_headers stale = {.if_none_match = "W/\"00000000000000000000000000000000\""};
    bl_request(&req3, path, &stale);
    CF_REQUIRE(run_request(&env, &req3, &resp3));
    CF_CHECK(resp3.status == 200);
    CF_CHECK(resp3.body_kind == CF_BODY_FILE);
    cf_response_dispose(&resp3);

    cf_str_dispose(&token);
    env_close(&env);
}

CF_TEST(active_storage_blobs_proxy_forced_attachment) {
    bl_env env;
    CF_REQUIRE(env_open(&env));
    static const unsigned char bytes[] = "<html>";
    seed_blob(env.scratch.db, 1, KEY1, "page.html", "text/html",
              sizeof bytes - 1, "checksum-a");
    stage_file(&env, KEY1, bytes, sizeof bytes - 1);
    cf_str token = {0};
    make_signed_id(&env, 1, &token);
    char esc[1024];
    escape_token(&token, esc, sizeof esc);

    char path[2048];
    snprintf(path, sizeof path, "/rails/active_storage/blobs/proxy/%s/page.html",
             esc);
    cf_request req;
    cf_response resp;
    bl_request(&req, path, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    /* Binary-forced: served as octet-stream, always an attachment. */
    CF_CHECK(header_equals(&resp, &req, "Content-Type",
                           "application/octet-stream"));
    CF_CHECK(header_equals(
        &resp, &req, "Content-Disposition",
        "attachment; filename=\"page.html\"; filename*=UTF-8''page.html"));
    cf_response_dispose(&resp);
    cf_str_dispose(&token);
    env_close(&env);
}

CF_TEST(active_storage_blobs_proxy_ranges) {
    bl_env env;
    CF_REQUIRE(env_open(&env));
    static const unsigned char bytes[] = "0123456789abcdef";
    seed_blob(env.scratch.db, 1, KEY1, "file.png", "image/png",
              sizeof bytes - 1, "checksum-a");
    stage_file(&env, KEY1, bytes, sizeof bytes - 1);
    cf_str token = {0};
    make_signed_id(&env, 1, &token);
    char esc[1024];
    escape_token(&token, esc, sizeof esc);

    char path[2048];
    snprintf(path, sizeof path, "/rails/active_storage/blobs/proxy/%s/file.png",
             esc);

    /* Single range. */
    cf_request req;
    cf_response resp;
    bl_headers single = {.range = "bytes=2-5"};
    bl_request(&req, path, &single);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 206);
    CF_CHECK(header_equals(&resp, &req, "Content-Type", "image/png"));
    CF_CHECK(header_equals(&resp, &req, "Content-Range",
                           "bytes 2-5/16"));
    CF_CHECK(header_equals(&resp, &req, "Accept-Ranges", "bytes"));
    unsigned char *body = NULL;
    size_t body_len = 0;
    CF_REQUIRE(response_body(&resp, &body, &body_len));
    CF_CHECK(body_len == 4 && memcmp(body, "2345", 4) == 0);
    free(body);
    cf_response_dispose(&resp);

    /* Multiple ranges: one multipart body carrying both slices. */
    cf_request req2;
    cf_response resp2;
    bl_headers multi = {.range = "bytes=0-1, 14-15"};
    bl_request(&req2, path, &multi);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 206);
    char ctype[256];
    CF_REQUIRE(
        response_header(&resp2, &req2, "Content-Type", ctype, sizeof ctype));
    CF_CHECK(strncmp(ctype, "multipart/byteranges; boundary=", 30) == 0);
    CF_CHECK(header_absent(&resp2, &req2, "Content-Range"));
    unsigned char *body2 = NULL;
    size_t body2_len = 0;
    CF_REQUIRE(response_body(&resp2, &body2, &body2_len));
    CF_CHECK(body2_len > 0);
    /* Both slices ride in the multipart framing. */
    bool has_first = false, has_last = false;
    for (size_t i = 0; i + 1 < body2_len; i++) {
        if (body2[i] == '0' && body2[i + 1] == '1') has_first = true;
        if (body2[i] == 'e' && body2[i + 1] == 'f') has_last = true;
    }
    CF_CHECK(has_first && has_last);
    CF_CHECK(body_contains(body2, body2_len, "Content-Range: bytes 0-1/16"));
    CF_CHECK(
        body_contains(body2, body2_len, "Content-Range: bytes 14-15/16"));
    free(body2);
    cf_response_dispose(&resp2);

    /* Unsatisfiable and malformed ranges are 416 heads. */
    const char *bad[] = {"bytes=100-200", "bytes=5-3", "nonsense"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        cf_request reqn;
        cf_response respn;
        bl_headers headers = {.range = bad[i]};
        bl_request(&reqn, path, &headers);
        CF_REQUIRE(run_request(&env, &reqn, &respn));
        CF_CHECK(respn.status == 416);
        CF_CHECK(header_equals(&respn, &reqn, "Content-Type", "image/png"));
        CF_CHECK(respn.body_kind == CF_BODY_NONE);
        cf_response_dispose(&respn);
    }

    cf_str_dispose(&token);
    env_close(&env);
}

CF_TEST(active_storage_blobs_proxy_missing_file) {
    bl_env env;
    CF_REQUIRE(env_open(&env));
    /* Row exists, bytes never staged. */
    seed_blob(env.scratch.db, 1, KEY1, "file.png", "image/png", 9,
              "checksum-a");
    cf_str token = {0};
    make_signed_id(&env, 1, &token);
    char esc[1024];
    escape_token(&token, esc, sizeof esc);

    char path[2048];
    snprintf(path, sizeof path, "/rails/active_storage/blobs/proxy/%s/file.png",
             esc);
    cf_request req;
    cf_response resp;
    bl_request(&req, path, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    CF_CHECK(header_equals(&resp, &req, "Cache-Control", "no-cache"));
    CF_CHECK(header_equals(&resp, &req, "Content-Type", "image/png"));
    cf_response_dispose(&resp);

    /* Missing file with a Range is the reference's internal raise. */
    cf_request req2;
    cf_response resp2;
    bl_headers range = {.range = "bytes=0-1"};
    bl_request(&req2, path, &range);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 500);
    cf_response_dispose(&resp2);

    cf_str_dispose(&token);
    env_close(&env);
}

CF_TEST_MAIN()
