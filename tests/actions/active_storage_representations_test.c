/* tests/actions/active_storage_representations_test.c — S02-C
 * acceptance: `active_storage/representations#show` redirect (route IDs
 * 172, 174) and proxy (route ID 173).
 *
 * Cases:
 *   - route binding: IDs 172/173/174 resolve to the redirect/proxy actions;
 *   - redirect with a recorded variant: 302 to PUBLIC_ORIGIN + the variant
 *     image's disk path (the digest covers the format-defaulted variation),
 *     Cache-Control max-age=300, private; an explicit `format` in the
 *     variation overrides the default (different digest, different record);
 *   - proxy with a recorded variant: 200 with the variant bytes, the
 *     image's content type/disposition, the 100-year validators and no
 *     Accept-Ranges (this controller has no byte ranges — a Range header
 *     is ignored);
 *   - failure paths: tampered variation keys and wrong-purpose keys are
 *     the 404 head; a bad signed blob is the 404 head; a valid signed blob
 *     for a missing blob is the mapped 404;
 *   - missing source files report404; unrepresentable blobs and invalid
 *     operations fail explicitly;
 *   - real generated variants persist analyzed images and reuse them; video
 *     previews match the committed pinnedJPEG bytes;
 *   - preview path: a video blob with a recorded preview image and an
 *     empty variation redirects to the preview's disk URL;
 *   - public: no session cookie is sent on any case above.
 *
 * Variant digests in the fixtures are computed through the public S02
 * API (default + digest), the same functions the action uses.
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
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

cf_err cf_action_active_storage_representations_redirect_show(cf_ctx *ctx);
cf_err cf_action_active_storage_representations_proxy_show(cf_ctx *ctx);
/* Route 175 for the redirect Location round-trip. */
cf_err cf_action_active_storage_disk_show(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="
#define KEY_SRC "aaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define KEY_VAR "bbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define KEY_PREV "cccccccccccccccccccccccccccc"
#define CREATED_AT "'2026-09-26 13:00:20.000000'"

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
    char storage_root[256];
} rp_env;

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

static bool env_open(rp_env *env) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;
    snprintf(env->storage_root, sizeof env->storage_root,
             "/tmp/cf-s02c-repr-XXXXXX");
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
            "GET", "/rails/active_storage/representations/redirect/:signed_blob_id/:variation_key/*filename(.:format)",
            172,
            cf_action_active_storage_representations_redirect_show) != CF_OK ||
        cf_test_routes_add(
            "GET", "/rails/active_storage/representations/proxy/:signed_blob_id/:variation_key/*filename(.:format)",
            173,
            cf_action_active_storage_representations_proxy_show) != CF_OK ||
        cf_test_routes_add(
            "GET", "/rails/active_storage/representations/:signed_blob_id/:variation_key/*filename(.:format)",
            174,
            cf_action_active_storage_representations_redirect_show) != CF_OK ||
        cf_test_routes_add(
            "GET", "/rails/active_storage/disk/:encoded_key/*filename(.:format)",
            175, cf_action_active_storage_disk_show) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(rp_env *env) {
    if (env->writer_started) cf_writer_stop(env->app);
    if (env->app != NULL) cf_app_destroy(env->app);
    env->config = NULL;
    cf_db_scratch_close(&env->scratch);
    remove_tree(env->storage_root);
    memset(env, 0, sizeof *env);
}

static void seed_blob(cf_db *db, int64_t id, const char *key,
                       const char *filename, const char *content_type,
                       int64_t byte_size) {
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(
                   handle,
                   "INSERT INTO active_storage_blobs (id, byte_size, "
                   "checksum, content_type, created_at, filename, key, "
                   "metadata, service_name) VALUES (?1, ?2, 'cs', ?3, "
                   "'2026-09-26 13:00:20.000000', ?4, ?5, '{}', 'local')",
                   -1, &stmt, NULL) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_int64(stmt, 1, id) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_int64(stmt, 2, byte_size) == SQLITE_OK);
    if (content_type != NULL) {
        CF_REQUIRE(sqlite3_bind_text(stmt, 3, content_type, -1,
                                     SQLITE_TRANSIENT) == SQLITE_OK);
    } else {
        CF_REQUIRE(sqlite3_bind_null(stmt, 3) == SQLITE_OK);
    }
    CF_REQUIRE(sqlite3_bind_text(stmt, 4, filename, -1, SQLITE_TRANSIENT) ==
               SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_text(stmt, 5, key, -1, SQLITE_TRANSIENT) ==
               SQLITE_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
}

/* INSERT INTO active_storage_variant_records + its `image` attachment. */
static void seed_variant(cf_db *db, int64_t blob_id, const char *digest,
                         int64_t image_id) {
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(
                   handle,
                   "INSERT INTO active_storage_variant_records (blob_id, "
                   "variation_digest) VALUES (?1, ?2)",
                   -1, &stmt, NULL) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_int64(stmt, 1, blob_id) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_text(stmt, 2, digest, -1, SQLITE_TRANSIENT) ==
               SQLITE_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    int64_t record_id = sqlite3_last_insert_rowid(handle);
    CF_REQUIRE(sqlite3_prepare_v2(
                   handle,
                   "INSERT INTO active_storage_attachments (blob_id, "
                   "created_at, name, record_id, record_type) VALUES (?1, "
                   "'2026-09-26 13:00:20.000000', 'image', ?2, "
                   "'ActiveStorage::VariantRecord')",
                   -1, &stmt, NULL) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_int64(stmt, 1, image_id) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_int64(stmt, 2, record_id) == SQLITE_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
}

static void seed_preview(cf_db *db, int64_t blob_id, int64_t image_id) {
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(
                   handle,
                   "INSERT INTO active_storage_attachments (blob_id, "
                   "created_at, name, record_id, record_type) VALUES (?1, "
                   "'2026-09-26 13:00:20.000000', 'preview_image', ?2, "
                   "'ActiveStorage::Blob')",
                   -1, &stmt, NULL) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_int64(stmt, 1, image_id) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_int64(stmt, 2, blob_id) == SQLITE_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
}

static void stage_file(rp_env *env, const char *key,
                       const unsigned char *bytes, size_t len) {
    cf_storage *storage = NULL;
    CF_REQUIRE(cf_storage_open(env->storage_root, &storage) == CF_OK);
    cf_storage_upload *upload = NULL;
    CF_REQUIRE(cf_storage_upload_begin(storage, &upload) == CF_OK);
    CF_REQUIRE(cf_storage_upload_write(upload, (cf_span){bytes, len}) ==
               CF_OK);
    CF_REQUIRE(cf_storage_upload_move(
                   upload, (cf_span){(const unsigned char *)key,
                                     strlen(key)}) == CF_OK);
    CF_REQUIRE(cf_storage_upload_commit(upload) == CF_OK);
    cf_storage_upload_dispose(upload);
    cf_storage_close(storage);
}

static cf_span secret_of(rp_env *env) {
    return (cf_span){(const unsigned char *)env->config->secret_key_base,
                     env->config->secret_key_base_len};
}

/* The digest of `entries` defaulted with format "png" (the web-image
 * default for the png/jpeg fixtures below). */
static void digest_with_png(rp_env *env, const cf_active_ventries *entries,
                            cf_str *out) {
    (void)env;
    cf_active_ventries defaults = {0};
    cf_active_vval *format = NULL;
    CF_REQUIRE(cf_active_vstr(SP("png"), &format) == CF_OK);
    CF_REQUIRE(cf_active_ventries_push(&defaults, SP("format"), format) ==
               CF_OK);
    cf_active_ventries defaulted = {0};
    CF_REQUIRE(cf_active_variation_default(&defaults, entries, &defaulted) ==
               CF_OK);
    cf_active_ventries_dispose(&defaults);
    CF_REQUIRE(cf_active_variation_digest(&defaulted, out) == CF_OK);
    cf_active_ventries_dispose(&defaulted);
}

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

static bool run_request(rp_env *env, cf_request *req, cf_response *resp) {
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
} rp_headers;

static void rp_request(cf_request *req, const char *path,
                       const rp_headers *headers) {
    static char target[8192];
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

/* --- acceptance ------------------------------------------------------------ */

CF_TEST(active_storage_representations_route_ids_bind_the_actions) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add(
                   "GET", "/rails/active_storage/representations/redirect/:signed_blob_id/:variation_key/*filename(.:format)",
                   172,
                   cf_action_active_storage_representations_redirect_show) ==
               CF_OK);
    CF_REQUIRE(cf_test_routes_add(
                   "GET", "/rails/active_storage/representations/proxy/:signed_blob_id/:variation_key/*filename(.:format)",
                   173,
                   cf_action_active_storage_representations_proxy_show) ==
               CF_OK);
    CF_REQUIRE(cf_test_routes_add(
                   "GET", "/rails/active_storage/representations/:signed_blob_id/:variation_key/*filename(.:format)",
                   174,
                   cf_action_active_storage_representations_redirect_show) ==
               CF_OK);
    CF_CHECK(cf_route_action(172) ==
             cf_action_active_storage_representations_redirect_show);
    CF_CHECK(cf_route_action(173) ==
             cf_action_active_storage_representations_proxy_show);
    CF_CHECK(cf_route_action(174) ==
             cf_action_active_storage_representations_redirect_show);
}

CF_TEST(active_storage_representations_redirect_serves_recorded_variant) {
    rp_env env;
    CF_REQUIRE(env_open(&env));
    seed_blob(env.scratch.db, 1, KEY_SRC, "photo.png", "image/png", 100);
    static const unsigned char variant_bytes[] = "variant-webp";
    seed_blob(env.scratch.db, 2, KEY_VAR, "photo.webp", "image/webp",
              sizeof variant_bytes - 1);
    stage_file(&env, KEY_VAR, variant_bytes, sizeof variant_bytes - 1);

    /* The empty variation, defaulted with format png. */
    cf_active_ventries empty = {0};
    cf_str digest = {0};
    digest_with_png(&env, &empty, &digest);
    seed_variant(env.scratch.db, 1, digest.ptr, 2);

    cf_str signed_id = {0};
    CF_REQUIRE(cf_active_blob_sign(secret_of(&env), 1, false, 0,
                                   &signed_id) == CF_OK);
    cf_str vkey = {0};
    CF_REQUIRE(cf_active_variation_sign(secret_of(&env), &empty, &vkey) ==
               CF_OK);
    cf_active_ventries_dispose(&empty);
    char esc_id[1024], esc_key[4096];
    escape_token(&signed_id, esc_id, sizeof esc_id);
    escape_token(&vkey, esc_key, sizeof esc_key);

    char path[8192];
    snprintf(path, sizeof path,
             "/rails/active_storage/representations/redirect/%s/%s/photo.png",
             esc_id, esc_key);
    cf_request req;
    cf_response resp;
    rp_request(&req, path, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(header_equals(&resp, &req, "Cache-Control",
                           "max-age=300, private"));
    char location[8192];
    CF_REQUIRE(response_header(&resp, &req, "Location", location,
                               sizeof location));
    /* The redirect targets the variant image (its key and filename). */
    CF_CHECK(strncmp(location, ORIGIN "/rails/active_storage/disk/",
                     strlen(ORIGIN) + 26) == 0);
    CF_CHECK(strstr(location, "/photo.webp") != NULL);
    cf_response_dispose(&resp);

    /* The Location serves the variant bytes through the disk show. */
    const char *disk_path = location + strlen(ORIGIN);
    cf_request req2;
    cf_response resp2;
    rp_request(&req2, disk_path, NULL);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 200);
    unsigned char *body = NULL;
    size_t body_len = 0;
    CF_REQUIRE(response_body(&resp2, &body, &body_len));
    CF_CHECK(body_len == sizeof variant_bytes - 1 &&
             memcmp(body, variant_bytes, body_len) == 0);
    free(body);
    cf_response_dispose(&resp2);

    cf_str_dispose(&digest);
    cf_str_dispose(&signed_id);
    cf_str_dispose(&vkey);
    env_close(&env);
}

CF_TEST(active_storage_representations_explicit_format_overrides_default) {
    rp_env env;
    CF_REQUIRE(env_open(&env));
    seed_blob(env.scratch.db, 1, KEY_SRC, "photo.png", "image/png", 100);
    static const unsigned char variant_bytes[] = "variant-webp";
    seed_blob(env.scratch.db, 2, KEY_VAR, "variant.webp", "image/webp",
              sizeof variant_bytes - 1);
    stage_file(&env, KEY_VAR, variant_bytes, sizeof variant_bytes - 1);

    /* {format: "webp"} overrides the "png" default; the digest covers the
     * override (a plain-default record would not match). */
    cf_active_ventries entries = {0};
    cf_active_vval *format = NULL;
    CF_REQUIRE(cf_active_vstr(SP("webp"), &format) == CF_OK);
    CF_REQUIRE(cf_active_ventries_push(&entries, SP("format"), format) ==
               CF_OK);
    cf_str digest = {0};
    digest_with_png(&env, &entries, &digest);
    seed_variant(env.scratch.db, 1, digest.ptr, 2);

    cf_str signed_id = {0};
    CF_REQUIRE(cf_active_blob_sign(secret_of(&env), 1, false, 0,
                                   &signed_id) == CF_OK);
    cf_str vkey = {0};
    CF_REQUIRE(cf_active_variation_sign(secret_of(&env), &entries, &vkey) ==
               CF_OK);
    cf_active_ventries_dispose(&entries);
    char esc_id[1024], esc_key[4096];
    escape_token(&signed_id, esc_id, sizeof esc_id);
    escape_token(&vkey, esc_key, sizeof esc_key);

    char path[8192];
    snprintf(path, sizeof path,
             "/rails/active_storage/representations/redirect/%s/%s/photo.png",
             esc_id, esc_key);
    cf_request req;
    cf_response resp;
    rp_request(&req, path, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    char location[8192];
    CF_REQUIRE(response_header(&resp, &req, "Location", location,
                               sizeof location));
    CF_CHECK(strstr(location, "/variant.webp") != NULL);
    cf_response_dispose(&resp);

    cf_str_dispose(&digest);
    cf_str_dispose(&signed_id);
    cf_str_dispose(&vkey);
    env_close(&env);
}

CF_TEST(active_storage_representations_proxy_streams_recorded_variant) {
    rp_env env;
    CF_REQUIRE(env_open(&env));
    seed_blob(env.scratch.db, 1, KEY_SRC, "photo.png", "image/png", 100);
    static const unsigned char variant_bytes[] = "variant-webp-bytes";
    seed_blob(env.scratch.db, 2, KEY_VAR, "photo.webp", "image/webp",
              sizeof variant_bytes - 1);
    stage_file(&env, KEY_VAR, variant_bytes, sizeof variant_bytes - 1);

    cf_active_ventries empty = {0};
    cf_str digest = {0};
    digest_with_png(&env, &empty, &digest);
    seed_variant(env.scratch.db, 1, digest.ptr, 2);

    cf_str signed_id = {0};
    CF_REQUIRE(cf_active_blob_sign(secret_of(&env), 1, false, 0,
                                   &signed_id) == CF_OK);
    cf_str vkey = {0};
    CF_REQUIRE(cf_active_variation_sign(secret_of(&env), &empty, &vkey) ==
               CF_OK);
    cf_active_ventries_dispose(&empty);
    char esc_id[1024], esc_key[4096];
    escape_token(&signed_id, esc_id, sizeof esc_id);
    escape_token(&vkey, esc_key, sizeof esc_key);

    char path[8192];
    snprintf(path, sizeof path,
             "/rails/active_storage/representations/proxy/%s/%s/photo.png",
             esc_id, esc_key);
    cf_request req;
    cf_response resp;
    rp_request(&req, path, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(header_equals(&resp, &req, "Content-Type", "image/webp"));
    CF_CHECK(header_equals(
        &resp, &req, "Content-Disposition",
        "inline; filename=\"photo.webp\"; filename*=UTF-8''photo.webp"));
    CF_CHECK(header_equals(&resp, &req, "content-transfer-encoding",
                           "binary"));
    CF_CHECK(header_absent(&resp, &req, "Accept-Ranges"));
    CF_CHECK(header_equals(&resp, &req, "Cache-Control",
                           "max-age=3155695200, public, immutable"));
    unsigned char *body = NULL;
    size_t body_len = 0;
    CF_REQUIRE(response_body(&resp, &body, &body_len));
    CF_CHECK(body_len == sizeof variant_bytes - 1 &&
             memcmp(body, variant_bytes, body_len) == 0);
    free(body);
    cf_response_dispose(&resp);

    /* A Range header is ignored on this controller (no byte ranges). */
    cf_request req2;
    cf_response resp2;
    rp_headers range = {.range = "bytes=0-1"};
    rp_request(&req2, path, &range);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 200);
    unsigned char *body2 = NULL;
    size_t body2_len = 0;
    CF_REQUIRE(response_body(&resp2, &body2, &body2_len));
    CF_CHECK(body2_len == sizeof variant_bytes - 1);
    free(body2);
    cf_response_dispose(&resp2);

    cf_str_dispose(&digest);
    cf_str_dispose(&signed_id);
    cf_str_dispose(&vkey);
    env_close(&env);
}

CF_TEST(active_storage_representations_failure_paths) {
    rp_env env;
    CF_REQUIRE(env_open(&env));
    seed_blob(env.scratch.db, 1, KEY_SRC, "photo.png", "image/png", 100);

    cf_active_ventries empty = {0};
    cf_str vkey = {0};
    CF_REQUIRE(cf_active_variation_sign(secret_of(&env), &empty, &vkey) ==
               CF_OK);
    cf_active_ventries_dispose(&empty);
    cf_str signed_id = {0};
    CF_REQUIRE(cf_active_blob_sign(secret_of(&env), 1, false, 0,
                                   &signed_id) == CF_OK);
    char esc_id[1024], esc_key[4096];
    escape_token(&signed_id, esc_id, sizeof esc_id);
    escape_token(&vkey, esc_key, sizeof esc_key);
    char path[8192];
    snprintf(path, sizeof path,
             "/rails/active_storage/representations/redirect/%s/%s/photo.png",
             esc_id, esc_key);

    /* No variant record and no source file: missing storage file -> 404. */
    cf_request req;
    cf_response resp;
    rp_request(&req, path, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404); /* missing source file */
    cf_response_dispose(&resp);

    /* Tampered variation key: 404 head. */
    char *tampered = malloc(vkey.len + 1);
    CF_REQUIRE(tampered != NULL);
    memcpy(tampered, vkey.ptr, vkey.len);
    tampered[vkey.len] = '\0';
    tampered[vkey.len - 1] = tampered[vkey.len - 1] == 'A' ? 'B' : 'A';
    char path2[8192];
    snprintf(path2, sizeof path2,
             "/rails/active_storage/representations/redirect/%s/%s/photo.png",
             esc_id, tampered);
    cf_request req2;
    cf_response resp2;
    rp_request(&req2, path2, NULL);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 404);
    /* The head content type follows the request format (.png path). */
    CF_CHECK(header_equals(&resp2, &req2, "Content-Type", "image/png"));
    cf_response_dispose(&resp2);
    free(tampered);

    /* Wrong purpose (a signed blob id as variation key): 404. */
    char esc_wrong[1024];
    escape_token(&signed_id, esc_wrong, sizeof esc_wrong);
    char path3[8192];
    snprintf(path3, sizeof path3,
             "/rails/active_storage/representations/redirect/%s/%s/photo.png",
             esc_id, esc_wrong);
    cf_request req3;
    cf_response resp3;
    rp_request(&req3, path3, NULL);
    CF_REQUIRE(run_request(&env, &req3, &resp3));
    CF_CHECK(resp3.status == 404);
    cf_response_dispose(&resp3);

    /* Bad signed blob: 404 head. */
    char path4[8192];
    snprintf(path4, sizeof path4,
             "/rails/active_storage/representations/redirect/%s/%s/photo.png",
             "bogus", esc_key);
    cf_request req4;
    cf_response resp4;
    rp_request(&req4, path4, NULL);
    CF_REQUIRE(run_request(&env, &req4, &resp4));
    CF_CHECK(resp4.status == 404);
    cf_response_dispose(&resp4);

    /* Valid signed blob for a missing blob: the mapped 404. */
    cf_str missing = {0};
    CF_REQUIRE(cf_active_blob_sign(secret_of(&env), 999, false, 0,
                                   &missing) == CF_OK);
    char esc_missing[1024];
    escape_token(&missing, esc_missing, sizeof esc_missing);
    char path5[8192];
    snprintf(path5, sizeof path5,
             "/rails/active_storage/representations/redirect/%s/%s/photo.png",
             esc_missing, esc_key);
    cf_request req5;
    cf_response resp5;
    rp_request(&req5, path5, NULL);
    CF_REQUIRE(run_request(&env, &req5, &resp5));
    CF_CHECK(resp5.status == 404);
    cf_response_dispose(&resp5);
    cf_str_dispose(&missing);

    cf_str_dispose(&signed_id);
    cf_str_dispose(&vkey);
    env_close(&env);
}

CF_TEST(active_storage_representations_unrepresentable_is_500) {
    rp_env env;
    CF_REQUIRE(env_open(&env));
    seed_blob(env.scratch.db, 1, KEY_SRC, "notes.txt", "text/plain", 100);

    cf_active_ventries empty = {0};
    cf_str vkey = {0};
    CF_REQUIRE(cf_active_variation_sign(secret_of(&env), &empty, &vkey) ==
               CF_OK);
    cf_active_ventries_dispose(&empty);
    cf_str signed_id = {0};
    CF_REQUIRE(cf_active_blob_sign(secret_of(&env), 1, false, 0,
                                   &signed_id) == CF_OK);
    char esc_id[1024], esc_key[4096];
    escape_token(&signed_id, esc_id, sizeof esc_id);
    escape_token(&vkey, esc_key, sizeof esc_key);
    char path[8192];
    snprintf(path, sizeof path,
             "/rails/active_storage/representations/redirect/%s/%s/notes.txt",
             esc_id, esc_key);
    cf_request req;
    cf_response resp;
    rp_request(&req, path, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);

    cf_str_dispose(&signed_id);
    cf_str_dispose(&vkey);
    env_close(&env);
}

CF_TEST(active_storage_representations_preview_image_paths) {
    rp_env env;
    CF_REQUIRE(env_open(&env));
    seed_blob(env.scratch.db, 1, KEY_SRC, "clip.mp4", "video/mp4", 100);
    static const unsigned char preview_bytes[] = "preview-png";
    seed_blob(env.scratch.db, 2, KEY_PREV, "clip.png", "image/png",
              sizeof preview_bytes - 1);
    stage_file(&env, KEY_PREV, preview_bytes, sizeof preview_bytes - 1);
    seed_preview(env.scratch.db, 1, 2);

    cf_active_ventries empty = {0};
    cf_str vkey = {0};
    CF_REQUIRE(cf_active_variation_sign(secret_of(&env), &empty, &vkey) ==
               CF_OK);
    cf_active_ventries_dispose(&empty);
    cf_str signed_id = {0};
    CF_REQUIRE(cf_active_blob_sign(secret_of(&env), 1, false, 0,
                                   &signed_id) == CF_OK);
    char esc_id[1024], esc_key[4096];
    escape_token(&signed_id, esc_id, sizeof esc_id);
    escape_token(&vkey, esc_key, sizeof esc_key);

    /* Empty variation: the recorded preview serves. */
    char path[8192];
    snprintf(path, sizeof path,
             "/rails/active_storage/representations/redirect/%s/%s/clip.mp4",
             esc_id, esc_key);
    cf_request req;
    cf_response resp;
    rp_request(&req, path, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    char location[8192];
    CF_REQUIRE(response_header(&resp, &req, "Location", location,
                               sizeof location));
    CF_CHECK(strstr(location, "/clip.png") != NULL);
    cf_response_dispose(&resp);

    /* Unsupported width operation remains an internal processing error. */
    cf_active_ventries entries = {0};
    cf_active_vval *width = NULL;
    CF_REQUIRE(cf_active_vint(100, &width) == CF_OK);
    CF_REQUIRE(cf_active_ventries_push(&entries, SP("width"), width) ==
               CF_OK);
    cf_str vkey2 = {0};
    CF_REQUIRE(cf_active_variation_sign(secret_of(&env), &entries, &vkey2) ==
               CF_OK);
    cf_active_ventries_dispose(&entries);
    char esc_key2[4096];
    escape_token(&vkey2, esc_key2, sizeof esc_key2);
    char path2[8192];
    snprintf(path2, sizeof path2,
             "/rails/active_storage/representations/redirect/%s/%s/clip.mp4",
             esc_id, esc_key2);
    cf_request req2;
    cf_response resp2;
    rp_request(&req2, path2, NULL);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 500);
    cf_response_dispose(&resp2);
    cf_str_dispose(&vkey2);

    cf_str_dispose(&signed_id);
    cf_str_dispose(&vkey);
    env_close(&env);
}

CF_TEST(active_storage_representations_missing_video_source_is_404) {
    rp_env env;
    CF_REQUIRE(env_open(&env));
    seed_blob(env.scratch.db, 1, KEY_SRC, "clip.mp4", "video/mp4", 100);

    cf_active_ventries empty = {0};
    cf_str vkey = {0};
    CF_REQUIRE(cf_active_variation_sign(secret_of(&env), &empty, &vkey) ==
               CF_OK);
    cf_active_ventries_dispose(&empty);
    cf_str signed_id = {0};
    CF_REQUIRE(cf_active_blob_sign(secret_of(&env), 1, false, 0,
                                   &signed_id) == CF_OK);
    char esc_id[1024], esc_key[4096];
    escape_token(&signed_id, esc_id, sizeof esc_id);
    escape_token(&vkey, esc_key, sizeof esc_key);
    char path[8192];
    snprintf(path, sizeof path,
             "/rails/active_storage/representations/proxy/%s/%s/clip.mp4",
             esc_id, esc_key);
    cf_request req;
    cf_response resp;
    rp_request(&req, path, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    /* A blob without its disk file cannot be processed. */
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    cf_str_dispose(&signed_id);
    cf_str_dispose(&vkey);
    env_close(&env);
}

CF_TEST(active_storage_representations_generates_and_reuses_image_variant) {
    rp_env env;
    CF_REQUIRE(env_open(&env));
    FILE *fixture = fopen("tests/fixtures/vectors/storage/black_hole-logo-small.png", "rb");
    CF_REQUIRE(fixture != NULL);
    CF_REQUIRE(fseek(fixture, 0, SEEK_END) == 0);
    long size = ftell(fixture);
    CF_REQUIRE(size > 0);
    rewind(fixture);
    unsigned char *bytes = malloc((size_t)size);
    CF_REQUIRE(bytes != NULL);
    CF_REQUIRE(fread(bytes, 1, (size_t)size, fixture) == (size_t)size);
    fclose(fixture);
    stage_file(&env, KEY_SRC, bytes, (size_t)size);
    free(bytes);
    seed_blob(env.scratch.db, 1, KEY_SRC, "black-hole.png", "image/png", size);
    cf_active_ventries variation = {0};
    cf_active_vval *resize = NULL, *dim = NULL;
    CF_REQUIRE(cf_active_varr(&resize) == CF_OK);
    CF_REQUIRE(cf_active_vint(16, &dim) == CF_OK);
    CF_REQUIRE(cf_active_varr_push(resize, dim) == CF_OK);
    CF_REQUIRE(cf_active_vint(16, &dim) == CF_OK);
    CF_REQUIRE(cf_active_varr_push(resize, dim) == CF_OK);
    CF_REQUIRE(cf_active_ventries_push(&variation, SP("resize_to_limit"), resize) == CF_OK);
    cf_str key = {0}, signed_id = {0};
    CF_REQUIRE(cf_active_variation_sign(secret_of(&env), &variation, &key) == CF_OK);
    CF_REQUIRE(cf_active_blob_sign(secret_of(&env), 1, false, 0, &signed_id) == CF_OK);
    char esc_id[1024], esc_key[4096], path[8192];
    escape_token(&signed_id, esc_id, sizeof esc_id);
    escape_token(&key, esc_key, sizeof esc_key);
    snprintf(path, sizeof path, "/rails/active_storage/representations/proxy/%s/%s/black-hole.png", esc_id, esc_key);
    for (int attempt = 0; attempt < 2; attempt++) {
        cf_request req;
        cf_response resp;
        rp_request(&req, path, NULL);
        CF_REQUIRE(run_request(&env, &req, &resp));
        CF_CHECK(resp.status == 200);
        unsigned char *body = NULL;
        size_t len = 0;
        CF_REQUIRE(response_body(&resp, &body, &len));
        CF_CHECK(len > 24 && memcmp(body, "\x89PNG\r\n\x1a\n", 8) == 0);
        if (len > 24) {
            unsigned width = (unsigned)body[16] << 24 | (unsigned)body[17] << 16 | (unsigned)body[18] << 8 | body[19];
            unsigned height = (unsigned)body[20] << 24 | (unsigned)body[21] << 16 | (unsigned)body[22] << 8 | body[23];
            CF_CHECK(width > 0 && width <= 16 && height > 0 && height <= 16);
        }
        free(body);
        cf_response_dispose(&resp);
        if (attempt == 0) {
            /* The second request must reuse the recorded variant even when
             * its source file is no longer available. */
            cf_storage *storage = NULL;
            CF_REQUIRE(cf_storage_open(env.storage_root, &storage) == CF_OK);
            CF_REQUIRE(cf_storage_delete(storage, SP(KEY_SRC)) == CF_OK);
            cf_storage_close(storage);
        }
    }
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(env.scratch.db), "SELECT count(*) FROM active_storage_variant_records WHERE blob_id=1", -1, &stmt, NULL) == SQLITE_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CF_CHECK(sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(env.scratch.db),
        "SELECT json_extract(metadata,'$.analyzed'), json_extract(metadata,'$.width'), json_extract(metadata,'$.height') FROM active_storage_blobs WHERE id != 1", -1, &stmt, NULL) == SQLITE_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CF_CHECK(sqlite3_column_int(stmt, 0) == 1);
    CF_CHECK(sqlite3_column_int(stmt, 1) > 0 && sqlite3_column_int(stmt, 1) <= 16);
    CF_CHECK(sqlite3_column_int(stmt, 2) > 0 && sqlite3_column_int(stmt, 2) <= 16);
    CF_CHECK(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    cf_active_ventries_dispose(&variation);
    cf_str_dispose(&key);
    cf_str_dispose(&signed_id);
    env_close(&env);
}

CF_TEST(active_storage_representations_draws_pinned_video_preview) {
    rp_env env;
    CF_REQUIRE(env_open(&env));
    FILE *input = fopen("tests/fixtures/media/alpha-centuri.mov", "rb");
    CF_REQUIRE(input != NULL);
    CF_REQUIRE(fseek(input, 0, SEEK_END) == 0);
    long size = ftell(input);
    CF_REQUIRE(size > 0);
    rewind(input);
    unsigned char *bytes = malloc((size_t)size);
    CF_REQUIRE(bytes != NULL);
    CF_REQUIRE(fread(bytes, 1, (size_t)size, input) == (size_t)size);
    fclose(input);
    stage_file(&env, KEY_SRC, bytes, (size_t)size);
    free(bytes);
    seed_blob(env.scratch.db, 1, KEY_SRC, "alpha-centuri.mov", "video/quicktime", size);
    cf_active_ventries variation = {0};
    cf_str key = {0}, id = {0};
    CF_REQUIRE(cf_active_variation_sign(secret_of(&env), &variation, &key) == CF_OK);
    CF_REQUIRE(cf_active_blob_sign(secret_of(&env), 1, false, 0, &id) == CF_OK);
    char esc_id[1024], esc_key[4096], path[8192];
    escape_token(&id, esc_id, sizeof esc_id);
    escape_token(&key, esc_key, sizeof esc_key);
    snprintf(path, sizeof path, "/rails/active_storage/representations/proxy/%s/%s/alpha-centuri.mov", esc_id, esc_key);
    cf_request req;
    cf_response resp;
    rp_request(&req, path, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    unsigned char *body = NULL;
    size_t len = 0;
    CF_REQUIRE(response_body(&resp, &body, &len));
    FILE *expected = fopen("tests/fixtures/vectors/storage/alpha-centuri-preview_image.jpg", "rb");
    CF_REQUIRE(expected != NULL);
    CF_REQUIRE(fseek(expected, 0, SEEK_END) == 0);
    long expected_size = ftell(expected);
    rewind(expected);
    CF_CHECK(expected_size > 0 && len == (size_t)expected_size);
    unsigned char *golden = malloc((size_t)expected_size);
    CF_REQUIRE(golden != NULL);
    CF_REQUIRE(fread(golden, 1, (size_t)expected_size, expected) == (size_t)expected_size);
    fclose(expected);
    if (len == (size_t)expected_size) CF_CHECK(memcmp(body, golden, len) == 0);
    free(golden); free(body);
    cf_response_dispose(&resp);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(env.scratch.db),
        "SELECT filename,content_type,json_extract(metadata,'$.analyzed') FROM active_storage_blobs WHERE id != 1", -1, &stmt, NULL) == SQLITE_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CF_CHECK(strcmp((const char *)sqlite3_column_text(stmt, 0), "alpha-centuri.jpg") == 0);
    CF_CHECK(strcmp((const char *)sqlite3_column_text(stmt, 1), "image/jpeg") == 0);
    CF_CHECK(sqlite3_column_int(stmt, 2) == 1);
    sqlite3_finalize(stmt);
    cf_str_dispose(&key); cf_str_dispose(&id);
    env_close(&env);
}

CF_TEST_MAIN()
