/* src/actions/active_storage/disk.c — ActiveStorage::DiskController
 * (task S02-C; route IDs 175 `disk#show`, 176 `disk#update`;
 * contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/active_storage.rs
 * (`disk_show`, `disk_serve`, `disk_update`, `acceptable_content`,
 * `require_active_storage_authentication`), over the S02 helpers
 * (src/storage/active_storage.{c,h}: disk key/token verify, the
 * acceptable-check, the checksum-guarded upload, the file-server serve
 * planner) and the S01 disk service. Storage semantics stay S02's; this
 * file is the controller layer.
 *
 * Translated in order:
 *   show: no authentication at all. `decode_verified_key` (a bad key is
 *     `head :not_found`), then the file-server plan (OPTIONS, conditional
 *     GET on an exact If-Modified-Since match, HEAD, single and multipart
 *     ranges with the fixed `AaB03x` boundary, 416s) plus the initializer's
 *     `Cache-Control: max-age=3600, public` on every response, including
 *     OPTIONS and 404 (the reference adds it after `disk_serve`).
 *   update: `require_active_storage_authentication` first (401 with no
 *     session and no CSRF — these are API-style token PUTs, so the CSRF
 *     chain never runs), then `decode_verified_token` (bad: 404), then the
 *     content check (declared content type, compared case-insensitively
 *     after stripping parameters, and exact content length must both equal
 *     the token's; else 422), then the checksum-guarded upload: 204 on
 *     success, 422 on an integrity mismatch.
 *
 * Divergences (documented, not silent):
 *   - an existing key answers 422, not an overwrite: the S02 upload helper
 *     never overwrites (CF_BUSY) while the reference `File::create`s. Disk
 *     keys are freshly generated per direct upload, so this arm is a
 *     retried-PUT shape only.
 *   - a blank Content-Type reads as absent (the kit's media type is None
 *     for an empty base); a present-but-garbage value never matches, like
 *     the reference's parsed comparison.
 *
 * Multipart 206 bodies are buffered (headings plus file slices); single
 * ranges and whole files stream with `cf_response_file`. Bodies are always
 * attached for 200/206/416 so the serializer's Content-Length is exact;
 * HEAD suppression is the serializer's own job (verified safe: it closes
 * the file fd through `cf_response_dispose`).
 *
 * c_symbols for the integrator's route rebind (src/routes.c rows 175, 176):
 *   cf_action_active_storage_disk_show, cf_action_active_storage_disk_update.
 * The integrator also owns src/actions/actions.h and the Makefile
 * (this file's build entry).
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "http/params.h"
#include "models/session.h"
#include "models/types.h"
#include "storage/active_storage.h"
#include "storage/storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DK_DISK_CACHE_CONTROL "max-age=3600, public"

static cf_span dk_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static cf_span dk_str_span(cf_str value) {
    return (cf_span){(const unsigned char *)value.ptr, value.len};
}

static bool dk_header_readable(cf_span value) {
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (c != '\t' && (c < 0x20 || c > 0x7E)) return false;
    }
    return true;
}

static bool dk_header(const cf_request *request, const char *name,
                      cf_span *out) {
    if (request == NULL || name == NULL || out == NULL) return false;
    size_t name_len = strlen(name);
    for (size_t i = 0; i < request->header_count; i++) {
        cf_span hname = request->headers[i].name;
        if (hname.len != name_len) continue;
        bool match = true;
        for (size_t k = 0; k < name_len; k++) {
            unsigned char c = hname.ptr[k];
            if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
            if (c != (unsigned char)name[k]) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        if (!dk_header_readable(request->headers[i].value)) return false;
        *out = request->headers[i].value;
        return true;
    }
    return false;
}

static bool dk_param_str(cf_ctx *ctx, const char *name, cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, dk_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) return false;
    return cf_param_string(param, out) == CF_OK;
}

static cf_span dk_secret(cf_ctx *ctx) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->secret_key_base == NULL) {
        return (cf_span){NULL, 0};
    }
    return (cf_span){(const unsigned char *)config->secret_key_base,
                     config->secret_key_base_len};
}

/* `head :status`: the status, the rendered format's content type (none for
 * 204/304) and an empty body. */
static cf_err dk_head(cf_ctx *ctx, unsigned status) {
    ctx->response->status = status;
    if (status == 204 || status == 304) return CF_OK;
    const cf_format *format = cf_ctx_rendered_format(ctx);
    return cf_response_header(ctx->response, dk_span("Content-Type"),
                              dk_span(format->string));
}

static cf_err dk_storage_open(cf_ctx *ctx, cf_storage **out) {
    *out = NULL;
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->storage_path == NULL) return CF_INTERNAL;
    return cf_storage_open(config->storage_path, out);
}

/* `require_active_storage_authentication`: 401 without a session (no
 * CSRF, no redirect — the halted 401 head). */
static cf_err dk_require_session(cf_ctx *ctx) {
    cf_span raw = {NULL, 0};
    if (cf_ctx_cookie_get(ctx, dk_span("session_token"), &raw) != CF_OK) {
        return dk_head(ctx, 401);
    }
    cf_str token = {0};
    bool verified = false;
    cf_err rc = cf_auth_signed_cookie_verify(
        dk_secret(ctx), dk_span("session_token"), raw,
        cf_now_us(ctx->app), &token, &verified);
    if (rc != CF_OK || !verified) {
        cf_str_dispose(&token);
        if (rc != CF_OK) return rc;
        return dk_head(ctx, 401);
    }
    bool found = false;
    cf_session session = {0};
    rc = cf_session_find_by_token(ctx->reader, token, &found, &session);
    cf_str_dispose(&token);
    cf_session_dispose(&session);
    if (rc != CF_OK) return rc;
    if (!found) return dk_head(ctx, 401);
    return CF_OK;
}

/* `request.header("content-length").and_then(|l| l.trim().parse())`. */
static bool dk_content_length(cf_span header, int64_t *out) {
    size_t start = 0, end = header.len;
    while (start < end &&
           (header.ptr[start] == ' ' || header.ptr[start] == '\t')) {
        start++;
    }
    while (end > start &&
           (header.ptr[end - 1] == ' ' || header.ptr[end - 1] == '\t')) {
        end--;
    }
    if (start == end) return false;
    bool negative = false;
    if (header.ptr[start] == '+' || header.ptr[start] == '-') {
        negative = header.ptr[start] == '-';
        start++;
        if (start == end) return false;
    }
    uint64_t magnitude = 0;
    for (size_t i = start; i < end; i++) {
        unsigned char c = header.ptr[i];
        if (c < '0' || c > '9') return false;
        unsigned digit = (unsigned)(c - '0');
        uint64_t limit = negative ? UINT64_C(9223372036854775808)
                                  : UINT64_C(9223372036854775807);
        if (magnitude > (limit - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    }
    if (!negative) {
        *out = (int64_t)magnitude;
    } else if (magnitude == UINT64_C(9223372036854775808)) {
        *out = INT64_MIN;
    } else {
        *out = -(int64_t)magnitude;
    }
    return true;
}

static cf_err dk_pread_all(int fd, uint64_t offset, uint64_t len,
                           cf_builder *out) {
    uint64_t at = 0;
    while (at < len) {
        unsigned char chunk[32768];
        size_t want = (size_t)(len - at);
        if (want > sizeof chunk) want = sizeof chunk;
        ssize_t got = pread(fd, chunk, want, (off_t)(offset + at));
        if (got <= 0) return CF_IO;
        cf_err rc = cf_builder_append(out, (cf_span){chunk, (size_t)got});
        if (rc != CF_OK) return rc;
        at += (uint64_t)got;
    }
    return CF_OK;
}

static const char *dk_method_name(cf_method method) {
    switch (method) {
    case CF_HEAD: return "HEAD";
    case CF_OPTIONS: return "OPTIONS";
    default: return "GET";
    }
}

/* Emit one non-empty plan field as a response header. */
static cf_err dk_emit(cf_ctx *ctx, const char *name, cf_str field) {
    if (field.ptr == NULL || field.len == 0) return CF_OK;
    return cf_response_header(ctx->response, dk_span(name),
                              dk_str_span(field));
}

/* ---- actions -------------------------------------------------------------- */

/* `DiskController#show` (route 175), plus the initializer's cache header. */
cf_err cf_action_active_storage_disk_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_span encoded = {NULL, 0};
    (void)dk_param_str(ctx, "encoded_key", &encoded);
    cf_active_disk_key disk_key = {0};
    bool verified = false;
    cf_err rc = cf_active_disk_key_verify(dk_secret(ctx), encoded,
                                          cf_now_us(ctx->app), &disk_key,
                                          &verified);
    if (rc != CF_OK) {
        cf_active_disk_key_dispose(&disk_key);
        return rc;
    }
    if (!verified) {
        cf_active_disk_key_dispose(&disk_key);
        ctx->response->status = 404;
        rc = cf_response_header(ctx->response, dk_span("Cache-Control"),
                                dk_span(DK_DISK_CACHE_CONTROL));
        if (rc == CF_OK) rc = dk_head(ctx, 404);
        return rc;
    }

    cf_storage *storage = NULL;
    rc = dk_storage_open(ctx, &storage);
    int fd = -1;
    uint64_t file_size = 0;
    if (rc == CF_OK) {
        rc = cf_storage_open_read(storage, dk_str_span(disk_key.key), &fd,
                                  &file_size);
        if (rc == CF_NOT_FOUND) {
            /* The file-server's NotFound arm: `head :not_found`, still
             * under the initializer's Cache-Control. */
            if (storage != NULL) cf_storage_close(storage);
            storage = NULL;
            cf_active_disk_key_dispose(&disk_key);
            ctx->response->status = 404;
            rc = cf_response_header(ctx->response, dk_span("Cache-Control"),
                                    dk_span(DK_DISK_CACHE_CONTROL));
            if (rc == CF_OK) rc = dk_head(ctx, 404);
            return rc;
        }
    }
    if (rc != CF_OK) {
        if (fd >= 0) close(fd);
        if (storage != NULL) cf_storage_close(storage);
        cf_active_disk_key_dispose(&disk_key);
        return rc;
    }

    /* mtime for the conditional GET (second precision, like Time#httpdate). */
    struct stat st;
    char mtime[30];
    if (fstat(fd, &st) != 0) {
        close(fd);
        if (storage != NULL) cf_storage_close(storage);
        cf_active_disk_key_dispose(&disk_key);
        return CF_IO;
    }
    rc = cf_active_httpdate((int64_t)st.st_mtime, mtime);
    if (rc != CF_OK) {
        close(fd);
        if (storage != NULL) cf_storage_close(storage);
        cf_active_disk_key_dispose(&disk_key);
        return rc;
    }

    cf_span range = {NULL, 0};
    bool has_range = dk_header(ctx->request, "range", &range);
    cf_span ims = {NULL, 0};
    bool has_ims = dk_header(ctx->request, "if-modified-since", &ims);
    cf_span ct_opt = {NULL, 0};
    if (disk_key.content_type.present) {
        ct_opt = dk_str_span(disk_key.content_type.value);
    }
    cf_active_serve plan = {0};
    rc = cf_active_disk_serve(
        dk_span(dk_method_name(ctx->request->method)), has_range, range,
        has_ims, ims, true, file_size,
        (cf_span){(unsigned char *)mtime, strlen(mtime)}, ct_opt,
        dk_str_span(disk_key.disposition), &plan);
    if (rc != CF_OK) {
        close(fd);
        if (storage != NULL) cf_storage_close(storage);
        cf_active_disk_key_dispose(&disk_key);
        return rc;
    }

    ctx->response->status = plan.status;
    rc = cf_response_header(ctx->response, dk_span("Cache-Control"),
                            dk_span(DK_DISK_CACHE_CONTROL));
    if (rc == CF_OK && plan.status != 404) {
        /* OPTIONS carries only Allow (+ Cache-Control); every other status
         * rides the file-server's stamped headers. */
        if (plan.allow.len != 0) {
            rc = dk_emit(ctx, "Allow", plan.allow);
        } else {
            rc = dk_emit(ctx, "Content-Type", plan.content_type);
        }
    }
    if (rc == CF_OK && plan.allow.len == 0 && plan.status != 404) {
        rc = dk_emit(ctx, "Content-Disposition", plan.content_disposition);
    }
    if (rc == CF_OK && plan.last_modified.len != 0) {
        rc = dk_emit(ctx, "Last-Modified", plan.last_modified);
    }
    if (rc == CF_OK && plan.content_range.len != 0) {
        rc = dk_emit(ctx, "Content-Range", plan.content_range);
    }
    if (rc != CF_OK) {
        close(fd);
        if (storage != NULL) cf_storage_close(storage);
        cf_active_serve_dispose(&plan);
        cf_active_disk_key_dispose(&disk_key);
        return rc;
    }

    if (plan.status == 200 || plan.status == 206) {
        if (plan.has_single) {
            uint64_t len = plan.single.end - plan.single.start + 1;
            rc = cf_response_file(ctx->response, fd, plan.single.start,
                                  len);
            if (rc != CF_OK) close(fd);
            fd = -1;
        } else if (plan.n_ranges != 0) {
            cf_builder body = {0};
            for (size_t i = 0; i < plan.n_ranges && rc == CF_OK; i++) {
                rc = cf_active_disk_part_heading(
                    plan.ranges[i].start, plan.ranges[i].end, file_size,
                    &body);
                if (rc == CF_OK) {
                    rc = dk_pread_all(fd, plan.ranges[i].start,
                                      plan.ranges[i].end -
                                          plan.ranges[i].start + 1,
                                      &body);
                }
            }
            if (rc == CF_OK) rc = cf_active_disk_part_trailer(&body);
            if (rc == CF_OK && body.len != plan.content_length) {
                rc = CF_INTERNAL;
            }
            if (rc == CF_OK) {
                cf_buf *buf = NULL;
                rc = cf_builder_freeze(&body, &buf);
                if (rc == CF_OK) rc = cf_response_body(ctx->response, buf);
                cf_buf_release(buf);
                if (rc != CF_OK) cf_builder_dispose(&body);
            } else {
                cf_builder_dispose(&body);
            }
        } else {
            rc = cf_response_file(ctx->response, fd, 0, file_size);
            if (rc != CF_OK) close(fd);
            fd = -1;
        }
    } else if (plan.status == 416) {
        cf_buf *buf = NULL;
        rc = cf_buf_copy(dk_span(CF_ACTIVE_UNSATISFIABLE_MESSAGE), &buf);
        if (rc == CF_OK) rc = cf_response_body(ctx->response, buf);
        cf_buf_release(buf);
    }
    if (fd >= 0) close(fd);
    if (storage != NULL) cf_storage_close(storage);
    cf_active_serve_dispose(&plan);
    cf_active_disk_key_dispose(&disk_key);
    return rc;
}

/* `DiskController#update` (route 176). */
cf_err cf_action_active_storage_disk_update(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_err rc = dk_require_session(ctx);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_span encoded = {NULL, 0};
    (void)dk_param_str(ctx, "encoded_token", &encoded);
    cf_active_disk_token token = {0};
    bool verified = false;
    rc = cf_active_disk_token_verify(dk_secret(ctx), encoded,
                                     cf_now_us(ctx->app), &token, &verified);
    if (rc != CF_OK) {
        cf_active_disk_token_dispose(&token);
        return rc;
    }
    if (!verified) {
        cf_active_disk_token_dispose(&token);
        return dk_head(ctx, 404);
    }

    cf_span req_ct = {NULL, 0};
    bool has_ct = dk_header(ctx->request, "content-type", &req_ct);
    if (has_ct) {
        /* The kit's media type is None for an empty base: a blank header
         * reads as absent. */
        bool blank = true;
        for (size_t i = 0; i < req_ct.len; i++) {
            if (req_ct.ptr[i] != ' ' && req_ct.ptr[i] != '\t') {
                blank = false;
                break;
            }
        }
        if (blank) {
            has_ct = false;
            req_ct = (cf_span){NULL, 0};
        }
    }
    cf_span cl = {NULL, 0};
    bool has_cl_header = dk_header(ctx->request, "content-length", &cl);
    int64_t req_length = 0;
    bool has_length = has_cl_header && dk_content_length(cl, &req_length);
    if (!cf_active_disk_token_acceptable(&token, req_ct, req_length,
                                         has_length)) {
        cf_active_disk_token_dispose(&token);
        return dk_head(ctx, 422);
    }

    cf_storage *storage = NULL;
    rc = dk_storage_open(ctx, &storage);
    if (rc == CF_OK) {
        rc = cf_active_disk_upload(storage, dk_str_span(token.key),
                                   ctx->request->body,
                                   dk_str_span(token.checksum));
        if (storage != NULL) cf_storage_close(storage);
        storage = NULL;
    }
    cf_active_disk_token_dispose(&token);
    if (rc == CF_OK) {
        ctx->response->status = 204;
        return CF_OK;
    }
    if (rc == CF_INVALID || rc == CF_BUSY) {
        /* Integrity mismatch, or the key already exists (retried PUT; the
         * reference would overwrite, the S02 upload never does). */
        return dk_head(ctx, 422);
    }
    return rc;
}
