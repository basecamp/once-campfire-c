/* Test-only helpers for H03's route/asset tests (files under tests/routes/
 * and tests/assets/, owned by H03; never linked into the application
 * library). */
#ifndef CF_H03_TEST_UTIL_H
#define CF_H03_TEST_UTIL_H

#include "app.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "http/http_internal.h"
#include "routes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define CF_TEST_SPAN(lit) \
    ((cf_span){(const unsigned char *)(lit), sizeof(lit) - 1})

/* A real app (config + app state) so cf_ctx_create/cf_ctx_process work; the
 * database is not touched by these tests (reader is NULL). Returns NULL on
 * setup failure, which a test reports as a failed assertion. */
static inline cf_app *h03_make_app(void) {
    static int seq;
    char origin[64];
    snprintf(origin, sizeof origin, "http://127.0.0.1:%d", 41000 + seq++);
    cf_config_entry entries[2] = {
        {"PUBLIC_ORIGIN", origin},
        {"SECRET_KEY_BASE",
         "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
    };
    cf_config *config = NULL;
    if (cf_config_parse(entries, 2, NULL, &config) != CF_OK) return NULL;
    cf_app *app = NULL;
    if (cf_app_create(config, &app) != CF_OK) {
        cf_config_destroy(config);
        return NULL;
    }
    return app;
}

static inline void h03_req_init(cf_request *req) {
    memset(req, 0, sizeof *req);
    req->method = CF_GET;
    req->original_method = CF_GET;
    req->path = CF_TEST_SPAN("/");
    req->peer_ip = CF_TEST_SPAN("127.0.0.1");
}

static inline cf_err h03_req_header(cf_request *req, cf_span name,
                                    cf_span value) {
    if (req->header_count >= 100) return CF_LIMIT;
    req->headers[req->header_count].name = name;
    req->headers[req->header_count].value = value;
    req->header_count++;
    return CF_OK;
}

/* Run one request through the full A00 path (real H03 table and handlers). */
static inline cf_err h03_process(cf_app *app, cf_request *req,
                                 cf_response *resp) {
    return cf_ctx_process(app, NULL, req, resp);
}

/* Exact body bytes of a finished response (BUFFER or FILE), owned. */
static inline unsigned char *h03_response_bytes(const cf_response *resp,
                                                size_t *len) {
    *len = 0;
    if (resp->body_kind == CF_BODY_BUFFER && resp->body != NULL) {
        cf_span s = cf_buf_span(resp->body);
        unsigned char *copy = malloc(s.len == 0 ? 1 : s.len);
        if (copy == NULL) return NULL;
        memcpy(copy, s.ptr, s.len);
        *len = s.len;
        return copy;
    }
    if (resp->body_kind == CF_BODY_FILE && resp->file_fd >= 0) {
        unsigned char *copy =
            malloc(resp->file_length == 0 ? 1 : (size_t)resp->file_length);
        if (copy == NULL) return NULL;
        ssize_t n = pread(resp->file_fd, copy, (size_t)resp->file_length,
                          (off_t)resp->file_offset);
        if (n < 0 || (uint64_t)n != resp->file_length) {
            free(copy);
            return NULL;
        }
        *len = (size_t)n;
        return copy;
    }
    unsigned char *copy = malloc(1);
    return copy;
}

/* H01's serialized header block for the finished response, NUL-terminated
 * and owned; header assertions search this exact wire form. */
static inline char *h03_serialize_headers(const cf_response *resp,
                                          const cf_request *req) {
    cf_http_serialized ser;
    if (cf_http_response_serialize(resp, req, &ser) != CF_OK) return NULL;
    cf_span s = cf_buf_span(ser.headers);
    char *out = malloc(s.len + 1);
    if (out != NULL) {
        memcpy(out, s.ptr, s.len);
        out[s.len] = '\0';
    }
    cf_buf_release(ser.headers);
    return out;
}

/* `Name: value\r\n` appears in the serialized header block. */
static inline bool h03_header_is(const char *headers, const char *name,
                                 const char *value) {
    char line[512];
    int n = snprintf(line, sizeof line, "%s: %s\r\n", name, value);
    if (n < 0 || (size_t)n >= sizeof line) return false;
    return strstr(headers, line) != NULL;
}

/* Read a whole file, owned; NULL when it cannot be read (missing fixture is
 * a test failure, never a skip). */
static inline unsigned char *h03_read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    unsigned char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        return NULL;
    }
    *len = got;
    return buf;
}

#endif /* CF_H03_TEST_UTIL_H */
