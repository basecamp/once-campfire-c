/* tests/actions/active_storage_disk_test.c — S02-C acceptance:
 * `active_storage/disk#show` (route ID 175) and `disk#update` (route ID
 * 176).
 *
 * Cases (show):
 *   - route binding: IDs 175/176 resolve to the show/update actions;
 *   - show 200: the key's content type and full disposition, Last-Modified,
 *     Cache-Control max-age=3600, public, exact bytes; HEAD answers the
 *     same headers with Content-Length and no body bytes;
 *   - show conditional GET: an exact If-Modified-Since match is a 304
 *     (Content-Type/Disposition ride along, Last-Modified does not);
 *   - show ranges: single (206 + Content-Range + slice), multi (206
 *     multipart/byteranges with the fixed AaB03x boundary), unsatisfiable
 *     (416 with the Byte-range-unsatisfiable body and Content-Range
 *     bytes-star/N), malformed (200 - the disk server ignores them, unlike
 *     the proxy's 416);
 *   - show failure paths: tampered/expired keys are 404 (with the disk
 *     Cache-Control); a verified key for a missing file is 404;
 *     OPTIONS answers Allow;
 *   - show is public: no session cookie is sent above.
 *
 * Cases (update):
 *   - no session is 401 (and no CSRF check runs: a forged request without
 *     a session is still 401);
 *   - a bad token is 404; a token/content-type or token/length mismatch is
 *     422; a checksum mismatch stages nothing and is 422;
 *   - success is 204 and the bytes land at the key (readable through the
 *     show); a second PUT to the same key is 422 (single-issue keys - the
 *     reference would overwrite, the S02 upload never does);
 *   - an expired token is 404.
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

cf_err cf_action_active_storage_disk_show(cf_ctx *ctx);
cf_err cf_action_active_storage_disk_update(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="
#define KEY1 "dddddddddddddddddddddddddddd"
#define KEY2 "eeeeeeeeeeeeeeeeeeeeeeeeeeee"

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
    char storage_root[256];
} dk_env;

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

static bool env_open(dk_env *env) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;
    snprintf(env->storage_root, sizeof env->storage_root,
             "/tmp/cf-s02c-disk-XXXXXX");
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
            "GET", "/rails/active_storage/disk/:encoded_key/*filename(.:format)",
            175, cf_action_active_storage_disk_show) != CF_OK ||
        cf_test_routes_add(
            "PUT", "/rails/active_storage/disk/:encoded_token(.:format)",
            176, cf_action_active_storage_disk_update) != CF_OK ||
        cf_test_routes_add("PUT", "/rails/active_storage/disk/:encoded_token",
                           176, cf_action_active_storage_disk_update) !=
            CF_OK) {
        return false;
    }
    return true;
}

static void env_close(dk_env *env) {
    if (env->writer_started) cf_writer_stop(env->app);
    if (env->app != NULL) cf_app_destroy(env->app);
    env->config = NULL;
    cf_db_scratch_close(&env->scratch);
    remove_tree(env->storage_root);
    memset(env, 0, sizeof *env);
}

static void stage_file(dk_env *env, const char *key,
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

static cf_span secret_of(dk_env *env) {
    return (cf_span){(const unsigned char *)env->config->secret_key_base,
                     env->config->secret_key_base_len};
}

/* A signed disk key (purpose blob_key), expiring or not. */
static void make_disk_key(dk_env *env, const char *key,
                          const char *content_type, bool expiring,
                          cf_str *out) {
    int64_t expires = expiring ? cf_now_us(NULL) + INT64_C(300000000)
                               : 0;
    CF_REQUIRE(cf_active_disk_key_sign(
                   secret_of(env), SP(key), SP("inline"), SP(content_type),
                   content_type != NULL, SP("local"), expiring, expires,
                   out) == CF_OK);
}

/* A signed upload token (purpose blob_token), always expiring. */
static void make_disk_token(dk_env *env, const char *key,
                            const char *content_type, int64_t length,
                            const char *checksum, bool expiring,
                            cf_str *out) {
    int64_t expires = expiring ? cf_now_us(NULL) + INT64_C(300000000)
                               : cf_now_us(NULL) - INT64_C(1000000);
    CF_REQUIRE(cf_active_disk_token_sign(
                   secret_of(env), SP(key), SP(content_type),
                   content_type != NULL, length, SP(checksum), SP("local"),
                   expires, out) == CF_OK);
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

static void seed_user(dk_env *env, int64_t id) {
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

static void make_session_cookie(dk_env *env, char *out, size_t cap,
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

static bool run_request(dk_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

typedef struct {
    const char *query;
    const char *method;
    const char *body;
    const char *content_type;
    const char *content_length;
    const char *range;
    const char *if_modified_since;
    const char *cookie;
    const char *fetch_site;
} dk_headers;

static void dk_request(cf_request *req, const char *path,
                       const dk_headers *headers) {
    static char target[8192];
    cf_test_req_init(req);
    const char *method = headers != NULL && headers->method != NULL
                             ? headers->method
                             : "GET";
    if (strcmp(method, "PUT") == 0) {
        req->method = CF_PUT;
        req->original_method = CF_PUT;
    } else if (strcmp(method, "HEAD") == 0) {
        req->method = CF_HEAD;
        req->original_method = CF_HEAD;
    } else if (strcmp(method, "OPTIONS") == 0) {
        req->method = CF_OPTIONS;
        req->original_method = CF_OPTIONS;
    }
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
    if (headers != NULL && headers->body != NULL) {
        req->body = SP(headers->body);
    } else {
        req->body = SP("");
    }
    if (headers != NULL && headers->content_type != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                      SP(headers->content_type)) == CF_OK);
    }
    if (headers != NULL && headers->content_length != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Content-Length"),
                                      SP(headers->content_length)) == CF_OK);
    }
    if (headers != NULL && headers->range != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Range"),
                                      SP(headers->range)) == CF_OK);
    }
    if (headers != NULL && headers->if_modified_since != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("If-Modified-Since"),
                                      SP(headers->if_modified_since)) ==
                   CF_OK);
    }
    if (headers != NULL && headers->cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"),
                                      SP(headers->cookie)) == CF_OK);
    }
    if (headers != NULL && headers->fetch_site != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                      SP(headers->fetch_site)) == CF_OK);
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

CF_TEST(active_storage_disk_route_ids_bind_the_actions) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add(
                   "GET", "/rails/active_storage/disk/:encoded_key/*filename(.:format)",
                   175, cf_action_active_storage_disk_show) == CF_OK);
    CF_REQUIRE(cf_test_routes_add(
                   "PUT", "/rails/active_storage/disk/:encoded_token",
                   176, cf_action_active_storage_disk_update) == CF_OK);
    CF_CHECK(cf_route_action(175) == cf_action_active_storage_disk_show);
    CF_CHECK(cf_route_action(176) == cf_action_active_storage_disk_update);
}

CF_TEST(active_storage_disk_show_full_and_head) {
    dk_env env;
    CF_REQUIRE(env_open(&env));
    static const unsigned char bytes[] = "0123456789abcdef";
    stage_file(&env, KEY1, bytes, sizeof bytes - 1);
    cf_str key = {0};
    make_disk_key(&env, KEY1, "image/png", true, &key);
    char esc[4096];
    escape_token(&key, esc, sizeof esc);

    char path[8192];
    snprintf(path, sizeof path, "/rails/active_storage/disk/%s/file.png",
             esc);
    cf_request req;
    cf_response resp;
    dk_request(&req, path, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(header_equals(&resp, &req, "Content-Type", "image/png"));
    CF_CHECK(header_equals(&resp, &req, "Content-Disposition", "inline"));
    CF_CHECK(header_equals(&resp, &req, "Cache-Control",
                           "max-age=3600, public"));
    char last_modified[128];
    CF_REQUIRE(response_header(&resp, &req, "Last-Modified", last_modified,
                               sizeof last_modified));
    unsigned char *body = NULL;
    size_t body_len = 0;
    CF_REQUIRE(response_body(&resp, &body, &body_len));
    CF_CHECK(body_len == sizeof bytes - 1 &&
             memcmp(body, bytes, body_len) == 0);
    free(body);
    cf_response_dispose(&resp);

    /* Conditional GET on the exact mtime: 304 with the stamped headers
     * but no Last-Modified and no body. */
    cf_request req2;
    cf_response resp2;
    dk_headers ims = {.if_modified_since = last_modified};
    dk_request(&req2, path, &ims);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 304);
    CF_CHECK(header_equals(&resp2, &req2, "Content-Type", "image/png"));
    CF_CHECK(header_equals(&resp2, &req2, "Cache-Control",
                           "max-age=3600, public"));
    CF_CHECK(header_absent(&resp2, &req2, "Last-Modified"));
    CF_CHECK(resp2.body_kind == CF_BODY_NONE);
    cf_response_dispose(&resp2);

    /* HEAD: the same headers with Content-Length and no body bytes. */
    cf_request req3;
    cf_response resp3;
    dk_headers head = {.method = "HEAD"};
    dk_request(&req3, path, &head);
    CF_REQUIRE(run_request(&env, &req3, &resp3));
    CF_CHECK(resp3.status == 200);
    CF_CHECK(header_equals(&resp3, &req3, "Content-Length", "16"));
    CF_CHECK(header_equals(&resp3, &req3, "Content-Type", "image/png"));
    cf_response_dispose(&resp3);

    /* OPTIONS is unreachable through the router (row 175 is GET-only;
     * the double has no OPTIONS row), so only the planner covers it -
     * see the S02 unit tests. */

    cf_str_dispose(&key);
    env_close(&env);
}

CF_TEST(active_storage_disk_show_ranges) {
    dk_env env;
    CF_REQUIRE(env_open(&env));
    static const unsigned char bytes[] = "0123456789abcdef";
    stage_file(&env, KEY1, bytes, sizeof bytes - 1);
    cf_str key = {0};
    make_disk_key(&env, KEY1, "image/png", true, &key);
    char esc[4096];
    escape_token(&key, esc, sizeof esc);

    char path[8192];
    snprintf(path, sizeof path, "/rails/active_storage/disk/%s/file.png",
             esc);

    /* Single range. */
    cf_request req;
    cf_response resp;
    dk_headers single = {.range = "bytes=2-5"};
    dk_request(&req, path, &single);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 206);
    CF_CHECK(header_equals(&resp, &req, "Content-Range", "bytes 2-5/16"));
    unsigned char *body = NULL;
    size_t body_len = 0;
    CF_REQUIRE(response_body(&resp, &body, &body_len));
    CF_CHECK(body_len == 4 && memcmp(body, "2345", 4) == 0);
    free(body);
    cf_response_dispose(&resp);

    /* Multiple ranges: the fixed AaB03x multipart framing. */
    cf_request req2;
    cf_response resp2;
    dk_headers multi = {.range = "bytes=0-1, 14-15"};
    dk_request(&req2, path, &multi);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 206);
    CF_CHECK(header_equals(&resp2, &req2, "Content-Type",
                           "multipart/byteranges; boundary=AaB03x"));
    unsigned char *body2 = NULL;
    size_t body2_len = 0;
    CF_REQUIRE(response_body(&resp2, &body2, &body2_len));
    CF_CHECK(body2_len > 0);
    CF_CHECK(body_contains(body2, body2_len, "--AaB03x"));
    CF_CHECK(body_contains(body2, body2_len, "content-range: bytes 0-1/16"));
    CF_CHECK(
        body_contains(body2, body2_len, "content-range: bytes 14-15/16"));
    free(body2);
    cf_response_dispose(&resp2);

    /* Unsatisfiable: 416 with the file-server message body. */
    cf_request req3;
    cf_response resp3;
    dk_headers unsat = {.range = "bytes=100-200"};
    dk_request(&req3, path, &unsat);
    CF_REQUIRE(run_request(&env, &req3, &resp3));
    CF_CHECK(resp3.status == 416);
    CF_CHECK(header_equals(&resp3, &req3, "Content-Range", "bytes */16"));
    unsigned char *body3 = NULL;
    size_t body3_len = 0;
    CF_REQUIRE(response_body(&resp3, &body3, &body3_len));
    CF_CHECK(body3_len == strlen("Byte range unsatisfiable\n") &&
             memcmp(body3, "Byte range unsatisfiable\n", body3_len) == 0);
    free(body3);
    cf_response_dispose(&resp3);

    /* Malformed: ignored (200), unlike the proxy's 416. */
    cf_request req4;
    cf_response resp4;
    dk_headers malformed = {.range = "nonsense"};
    dk_request(&req4, path, &malformed);
    CF_REQUIRE(run_request(&env, &req4, &resp4));
    CF_CHECK(resp4.status == 200);
    cf_response_dispose(&resp4);

    cf_str_dispose(&key);
    env_close(&env);
}

CF_TEST(active_storage_disk_show_failure_paths) {
    dk_env env;
    CF_REQUIRE(env_open(&env));
    static const unsigned char bytes[] = "0123456789abcdef";
    stage_file(&env, KEY1, bytes, sizeof bytes - 1);
    cf_str key = {0};
    make_disk_key(&env, KEY1, "image/png", true, &key);
    char esc[4096];
    escape_token(&key, esc, sizeof esc);

    /* Tampered key: flip the last byte. */
    char *tampered = malloc(key.len + 1);
    CF_REQUIRE(tampered != NULL);
    memcpy(tampered, key.ptr, key.len);
    tampered[key.len] = '\0';
    tampered[key.len - 1] = tampered[key.len - 1] == 'A' ? 'B' : 'A';
    char path[8192];
    snprintf(path, sizeof path, "/rails/active_storage/disk/%s/file.png",
             tampered);
    cf_request req;
    cf_response resp;
    dk_request(&req, path, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    CF_CHECK(header_equals(&resp, &req, "Cache-Control",
                           "max-age=3600, public"));
    cf_response_dispose(&resp);
    free(tampered);

    /* Expired key. */
    cf_str expired = {0};
    CF_REQUIRE(cf_active_disk_key_sign(
                   secret_of(&env), SP(KEY1), SP("inline"), SP("image/png"),
                   true, SP("local"), true, cf_now_us(NULL) - INT64_C(60000000),
                   &expired) == CF_OK);
    escape_token(&expired, esc, sizeof esc);
    snprintf(path, sizeof path, "/rails/active_storage/disk/%s/file.png",
             esc);
    cf_request req2;
    cf_response resp2;
    dk_request(&req2, path, NULL);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 404);
    cf_response_dispose(&resp2);
    cf_str_dispose(&expired);

    /* Verified key for a missing file: 404 with the disk Cache-Control. */
    cf_str missing = {0};
    make_disk_key(&env, KEY2, "image/png", true, &missing);
    escape_token(&missing, esc, sizeof esc);
    snprintf(path, sizeof path, "/rails/active_storage/disk/%s/file.png",
             esc);
    cf_request req3;
    cf_response resp3;
    dk_request(&req3, path, NULL);
    CF_REQUIRE(run_request(&env, &req3, &resp3));
    CF_CHECK(resp3.status == 404);
    CF_CHECK(header_equals(&resp3, &req3, "Cache-Control",
                           "max-age=3600, public"));
    cf_response_dispose(&resp3);
    cf_str_dispose(&missing);

    cf_str_dispose(&key);
    env_close(&env);
}

CF_TEST(active_storage_disk_update_auth_and_token_checks) {
    dk_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(&env, 1);
    char cookie[4096];
    make_session_cookie(&env, cookie, sizeof cookie, "session-token-1", 1);

    cf_str token = {0};
    make_disk_token(&env, KEY1, "image/png", 9, "checksum-a", true, &token);
    char esc[4096];
    escape_token(&token, esc, sizeof esc);
    char path[8192];
    snprintf(path, sizeof path, "/rails/active_storage/disk/%s", esc);

    /* No session: 401, even for a forged request (no CSRF runs here). */
    for (int i = 0; i < 2; i++) {
        cf_request req;
        cf_response resp;
        dk_headers headers = {
            .method = "PUT",
            .body = "123456789",
            .content_type = "image/png",
            .content_length = "9",
            .fetch_site = i == 0 ? NULL : "cross-site",
        };
        dk_request(&req, path, &headers);
        CF_REQUIRE(run_request(&env, &req, &resp));
        CF_CHECK(resp.status == 401);
        CF_CHECK(header_equals(&resp, &req, "Content-Type", "text/html"));
        cf_response_dispose(&resp);
    }

    /* Bad token with a session: 404. */
    cf_request req2;
    cf_response resp2;
    dk_headers bad_token = {
        .method = "PUT",
        .body = "123456789",
        .content_type = "image/png",
        .content_length = "9",
        .cookie = cookie,
        .fetch_site = "same-origin",
    };
    char bad_path[8192];
    snprintf(bad_path, sizeof bad_path, "/rails/active_storage/disk/bogus");
    dk_request(&req2, bad_path, &bad_token);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 404);
    cf_response_dispose(&resp2);

    /* Content-Type mismatch: 422. */
    cf_request req3;
    cf_response resp3;
    dk_headers wrong_ct = {
        .method = "PUT",
        .body = "123456789",
        .content_type = "text/plain",
        .content_length = "9",
        .cookie = cookie,
        .fetch_site = "same-origin",
    };
    dk_request(&req3, path, &wrong_ct);
    CF_REQUIRE(run_request(&env, &req3, &resp3));
    CF_CHECK(resp3.status == 422);
    CF_CHECK(header_equals(&resp3, &req3, "Content-Type", "text/html"));
    cf_response_dispose(&resp3);

    /* Content-Length mismatch: 422. */
    cf_request req4;
    cf_response resp4;
    dk_headers wrong_len = {
        .method = "PUT",
        .body = "123456789",
        .content_type = "image/png",
        .content_length = "10",
        .cookie = cookie,
        .fetch_site = "same-origin",
    };
    dk_request(&req4, path, &wrong_len);
    CF_REQUIRE(run_request(&env, &req4, &resp4));
    CF_CHECK(resp4.status == 422);
    cf_response_dispose(&resp4);

    /* Expired token: 404. */
    cf_str expired = {0};
    make_disk_token(&env, KEY1, "image/png", 9, "checksum-a", false,
                    &expired);
    escape_token(&expired, esc, sizeof esc);
    char expired_path[8192];
    snprintf(expired_path, sizeof expired_path,
             "/rails/active_storage/disk/%s", esc);
    cf_request req5;
    cf_response resp5;
    dk_headers headers5 = {
        .method = "PUT",
        .body = "123456789",
        .content_type = "image/png",
        .content_length = "9",
        .cookie = cookie,
        .fetch_site = "same-origin",
    };
    dk_request(&req5, expired_path, &headers5);
    CF_REQUIRE(run_request(&env, &req5, &resp5));
    CF_CHECK(resp5.status == 404);
    cf_response_dispose(&resp5);
    cf_str_dispose(&expired);

    cf_str_dispose(&token);
    env_close(&env);
}

CF_TEST(active_storage_disk_update_upload_lifecycle) {
    dk_env env;
    CF_REQUIRE(env_open(&env));
    seed_user(&env, 1);
    char cookie[4096];
    make_session_cookie(&env, cookie, sizeof cookie, "session-token-1", 1);

    /* The token's checksum is base64(md5("upload-bytes")); computed here
     * with the S01 upload boundary's own checksum over the staged body. */
    static const unsigned char payload[] = "upload-bytes";
    cf_storage *probe = NULL;
    CF_REQUIRE(cf_storage_open(env.storage_root, &probe) == CF_OK);
    cf_storage_upload *staging = NULL;
    CF_REQUIRE(cf_storage_upload_begin(probe, &staging) == CF_OK);
    CF_REQUIRE(cf_storage_upload_write(
                   staging,
                   (cf_span){payload, sizeof payload - 1}) == CF_OK);
    cf_builder checksum_builder = {0};
    CF_REQUIRE(cf_storage_upload_checksum(staging, &checksum_builder) ==
               CF_OK);
    char checksum[64];
    CF_REQUIRE(checksum_builder.len < sizeof checksum);
    memcpy(checksum, checksum_builder.ptr, checksum_builder.len);
    checksum[checksum_builder.len] = '\0';
    cf_builder_dispose(&checksum_builder);
    cf_storage_upload_dispose(staging);
    cf_storage_close(probe);

    cf_str token = {0};
    make_disk_token(&env, KEY1, "image/png",
                    (int64_t)(sizeof payload - 1), checksum, true, &token);
    char esc[4096];
    escape_token(&token, esc, sizeof esc);
    char path[8192];
    snprintf(path, sizeof path, "/rails/active_storage/disk/%s", esc);

    /* Checksum mismatch: 422, nothing staged at the key. */
    cf_request req;
    cf_response resp;
    dk_headers mismatch = {
        .method = "PUT",
        .body = "tampered!",
        .content_type = "image/png",
        .content_length = "12",
        .cookie = cookie,
        .fetch_site = "same-origin",
    };
    /* A 12-byte body needs a 12-byte token for the content check to pass
     * so the checksum arm is what fires. */
    cf_str token12 = {0};
    make_disk_token(&env, KEY1, "image/png", 12, checksum, true, &token12);
    escape_token(&token12, esc, sizeof esc);
    snprintf(path, sizeof path, "/rails/active_storage/disk/%s", esc);
    dk_request(&req, path, &mismatch);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    cf_response_dispose(&resp);
    cf_str_dispose(&token12);
    int fd = -1;
    uint64_t size = 0;
    cf_storage *check = NULL;
    CF_REQUIRE(cf_storage_open(env.storage_root, &check) == CF_OK);
    CF_CHECK(cf_storage_open_read(check, SP(KEY1), &fd, &size) ==
             CF_NOT_FOUND);
    cf_storage_close(check);

    /* Success: 204, bytes at the key, served back through the show. */
    escape_token(&token, esc, sizeof esc);
    snprintf(path, sizeof path, "/rails/active_storage/disk/%s", esc);
    cf_request req2;
    cf_response resp2;
    char body_text[64];
    snprintf(body_text, sizeof body_text, "%s", (const char *)payload);
    dk_headers put = {
        .method = "PUT",
        .body = body_text,
        .content_type = "image/png",
        .content_length = "12",
        .cookie = cookie,
        .fetch_site = "same-origin",
    };
    dk_request(&req2, path, &put);
    CF_REQUIRE(run_request(&env, &req2, &resp2));
    CF_CHECK(resp2.status == 204);
    CF_CHECK(resp2.body_kind == CF_BODY_NONE);
    cf_response_dispose(&resp2);

    CF_REQUIRE(cf_storage_open(env.storage_root, &check) == CF_OK);
    CF_CHECK(cf_storage_open_read(check, SP(KEY1), &fd, &size) == CF_OK);
    CF_CHECK(size == sizeof payload - 1);
    if (fd >= 0) close(fd);
    cf_storage_close(check);

    /* A second PUT to the same key is 422 (single-issue keys). */
    cf_request req3;
    cf_response resp3;
    dk_request(&req3, path, &put);
    CF_REQUIRE(run_request(&env, &req3, &resp3));
    CF_CHECK(resp3.status == 422);
    cf_response_dispose(&resp3);

    cf_str_dispose(&token);
    env_close(&env);
}

CF_TEST_MAIN()
