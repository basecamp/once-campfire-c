/* src/actions/active_storage/blobs.c — ActiveStorage::Blobs controllers
 * (task S02-C; route IDs 169 `redirect#show`, 170 `proxy#show`, 171 the
 * legacy `redirect#show` path; contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/active_storage.rs
 * (`blobs_redirect`, `blobs_proxy`), over the S02 helpers in
 * src/storage/active_storage.{c,h} and the S01 disk service
 * (src/storage/storage.h). Storage semantics (signing, ranges, serve
 * planning, dispositions) stay S02's; this file is the controller layer.
 *
 * Translated in order:
 *   verify_authenticity_token (CSRF only: no session, no bots, no browser
 *   check — these controllers inherit from ActiveStorage::BaseController,
 *   not ApplicationController) -> set_blob (`Blob.find_signed!`: a bad
 *   signature is `head :not_found`; a valid one for a missing blob is
 *   `RecordNotFound`, which the C dispatch maps to the reference 404 page)
 *   -> redirect: `expires_in(5.minutes)` + `redirect_to blob.url`
 *   (allow_other_host, absolute URL under PUBLIC_ORIGIN per D-C07);
 *   proxy: byte-range path for a present non-blank Range, else
 *   `http_cache_forever` (100-year public immutable Cache-Control, weak
 *   ETag over the full path, fixed Last-Modified) with a 304 when fresh,
 *   else the whole file with Accept-Ranges.
 *
 * Header accounting (controller-explicit only, matching the C-port
 * convention of unfurl_links/avatars: Rack middleware effects such as the
 * body-digest ETag and the default `no-cache` are not ported):
 *   redirect 302: Location, Content-Type text/html, Cache-Control
 *     `max-age=300, private` (ExpiresIn::default is not public).
 *   proxy 200/206: Content-Type (for_serving; the multipart type for a
 *     multi-range 206), Content-Disposition (forced-or-param, blob
 *     filename), content-transfer-encoding binary, Accept-Ranges bytes,
 *     ETag, Last-Modified (Sat, 01 Jan 2011 00:00:00 GMT) and
 *     Cache-Control `max-age=3155695200, public, immutable`.
 *   proxy 304: ETag, Last-Modified and the same Cache-Control; no
 *     Content-Type, no body.
 *   proxy 416 (bad/unsatisfiable Range): `head :range_not_satisfiable`
 *     (rendered format's content type, empty body). The range verdict comes
 *     first: a bad Range answers 416 even when the file is missing, exactly
 *     like `send_blob_byte_range_data` (ranges are cut from
 *     `blob.byte_size`, then the file check, whose failure there raises
 *     internally, i.e. a 500 here).
 *   proxy 404 (file missing, no Range): `expires_now` (Cache-Control
 *     `no-cache`) + `head :not_found`.
 *
 * c_symbols for the integrator's route rebind (src/routes.c rows 169-171):
 *   cf_action_active_storage_blobs_redirect_show,
 *   cf_action_active_storage_blobs_proxy_show.
 * The integrator also owns src/actions/actions.h and the Makefile
 * (this file's build entry).
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "http/params.h"
#include "models/active_storage.h"
#include "models/types.h"
#include "storage/active_storage.h"
#include "storage/storage.h"

#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* `ActiveStorage.service_urls_expire_in` (5 minutes, in microseconds). */
#define BL_EXPIRE_IN_US INT64_C(300000000)
/* `http_cache_forever`: `expires_in 100.years`. */
#define BL_FOREVER_CACHE_CONTROL "max-age=3155695200, public, immutable"
/* `http_cache_forever`'s fixed Last-Modified (2011-01-01T00:00:00Z). */
#define BL_FOREVER_LAST_MODIFIED "Sat, 01 Jan 2011 00:00:00 GMT"
/* The disk service backing `blob.url` (app.rs `DiskService::new(..., "local")`). */
#define BL_SERVICE_NAME "local"

static cf_span bl_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static cf_span bl_str_span(cf_str value) {
    return (cf_span){(const unsigned char *)value.ptr, value.len};
}

/* The kit's `request.header(name)`: first case-insensitive match whose value
 * passes `HeaderValue::to_str` (HTAB or visible ASCII only). */
static bool bl_header_readable(cf_span value) {
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (c != '\t' && (c < 0x20 || c > 0x7E)) return false;
    }
    return true;
}

static bool bl_header(const cf_request *request, const char *name,
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
        if (!bl_header_readable(request->headers[i].value)) return false;
        *out = request->headers[i].value;
        return true;
    }
    return false;
}

/* `c.param_str(name)`: a value only for a string param. */
static bool bl_param_str(cf_ctx *ctx, const char *name, cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, bl_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) return false;
    return cf_param_string(param, out) == CF_OK;
}

/* The application's secret, borrowed from the loaded config. */
static cf_span bl_secret(cf_ctx *ctx) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->secret_key_base == NULL) {
        return (cf_span){NULL, 0};
    }
    return (cf_span){(const unsigned char *)config->secret_key_base,
                     config->secret_key_base_len};
}

/* `head :status` (kit Ctx::head): the status, the rendered format's content
 * type and an empty body. */
static cf_err bl_head(cf_ctx *ctx, unsigned status) {
    ctx->response->status = status;
    const cf_format *format = cf_ctx_rendered_format(ctx);
    return cf_response_header(ctx->response, bl_span("Content-Type"),
                              bl_span(format->string));
}

/* `redirect_to location` (Rails default 302, absolute location, the
 * reference redirect's content type). */
static cf_err bl_redirect_to(cf_ctx *ctx, cf_span location) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    ctx->response->status = 302;
    cf_err rc =
        cf_response_header(ctx->response, bl_span("Location"), location);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, bl_span("Content-Type"),
                                bl_span("text/html; charset=utf-8"));
    }
    return rc;
}

/* `set_blob`: `Blob.find_signed!(params[:signed_id])`. A bad signature
 * answers `head :not_found` (halted); a valid one for a missing blob is
 * CF_NOT_FOUND (the generic map renders the reference 404 page). */
static cf_err bl_resolve_blob(cf_ctx *ctx, cf_blob *out, bool *halted) {
    *halted = false;
    cf_span token = {NULL, 0};
    (void)bl_param_str(ctx, "signed_id", &token);
    int64_t id = 0;
    bool found = false;
    cf_err rc = cf_active_blob_verify(bl_secret(ctx), token,
                                      cf_now_us(ctx->app), &id, &found);
    if (rc != CF_OK) return rc;
    if (!found) {
        rc = bl_head(ctx, 404);
        *halted = rc == CF_OK;
        return rc;
    }
    rc = cf_blob_find(ctx->reader, id, out);
    if (rc != CF_OK) cf_blob_dispose(out);
    return rc;
}

/* `request.fullpath`: path plus the query string when one is present. */
static cf_err bl_fullpath(cf_ctx *ctx, cf_builder *out) {
    cf_err rc = cf_builder_append(out, ctx->request->path);
    if (rc == CF_OK && ctx->request->query.len != 0) {
        rc = cf_builder_append(out, bl_span("?"));
    }
    if (rc == CF_OK && ctx->request->query.len != 0) {
        rc = cf_builder_append(out, ctx->request->query);
    }
    return rc;
}

/* `is_turbo_frame_request` (the frame etagger part of `combine_etags`). */
static bool bl_turbo_frame_request(const cf_request *request) {
    cf_span value = {NULL, 0};
    if (!bl_header(request, "turbo-frame", &value)) return false;
    bool blank = true;
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (c != ' ' && c != '\t') blank = false;
    }
    return !blank;
}

/* One flash value in `{:?}` spelling (`Some("...")`), appended raw. */
static cf_err bl_append_flash_part(cf_builder *out, cf_span key,
                                   cf_span value) {
    cf_err rc = cf_builder_append(out, key);
    if (rc == CF_OK) rc = cf_builder_append(out, bl_span("=Some(\""));
    for (size_t i = 0; rc == CF_OK && i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (c == '"') {
            rc = cf_builder_append(out, bl_span("\\\""));
        } else if (c == '\\') {
            rc = cf_builder_append(out, bl_span("\\\\"));
        } else if (c == '\n') {
            rc = cf_builder_append(out, bl_span("\\n"));
        } else if (c == '\r') {
            rc = cf_builder_append(out, bl_span("\\r"));
        } else if (c == '\t') {
            rc = cf_builder_append(out, bl_span("\\t"));
        } else if (c < 0x20 || c == 0x7F) {
            char esc[16];
            snprintf(esc, sizeof esc, "\\u{%x}", (unsigned)c);
            rc = cf_builder_append(out, bl_span(esc));
        } else {
            rc = cf_builder_append(out, (cf_span){&c, 1});
        }
    }
    if (rc == CF_OK) rc = cf_builder_append(out, bl_span("\")"));
    return rc;
}

/* `http_cache_forever`'s ETag: `W/"<sha256hex[0..16]>"` over the full path
 * plus turbo-rails' frame etagger and the flash parts, exactly like
 * `combine_etags` with the fullpath validator (weak). Owned output. */
static cf_err bl_forever_etag(cf_ctx *ctx, cf_str *out) {
    memset(out, 0, sizeof *out);
    cf_builder parts = {0};
    cf_err rc = bl_fullpath(ctx, &parts);
    if (rc == CF_OK && bl_turbo_frame_request(ctx->request)) {
        rc = cf_builder_append(&parts, bl_span("/frame"));
    }
    for (size_t i = 0; rc == CF_OK; i++) {
        cf_span key, value;
        if (!cf_ctx_flash_at(ctx, i, &key, &value)) break;
        rc = cf_builder_append(&parts, i == 0 ? bl_span("/") : bl_span("&"));
        if (rc == CF_OK) rc = bl_append_flash_part(&parts, key, value);
    }
    unsigned char digest[32];
    unsigned int digest_len = 0;
    EVP_MD_CTX *md = NULL;
    if (rc == CF_OK) {
        md = EVP_MD_CTX_new();
        if (md == NULL) rc = CF_NOMEM;
    }
    if (rc == CF_OK) {
        if (EVP_DigestInit_ex(md, EVP_sha256(), NULL) != 1 ||
            EVP_DigestUpdate(md,
                             parts.ptr != NULL
                                 ? parts.ptr
                                 : (const unsigned char *)"",
                             parts.len) != 1 ||
            EVP_DigestFinal_ex(md, digest, &digest_len) != 1 ||
            digest_len != 32) {
            rc = CF_INTERNAL;
        }
    }
    EVP_MD_CTX_free(md);
    cf_builder_dispose(&parts);
    if (rc != CF_OK) return rc;
    static const char HEX[] = "0123456789abcdef";
    char etag[40];
    size_t at = 0;
    etag[at++] = 'W';
    etag[at++] = '/';
    etag[at++] = '"';
    for (size_t i = 0; i < 16; i++) {
        etag[at++] = HEX[digest[i] >> 4];
        etag[at++] = HEX[digest[i] & 0x0F];
    }
    etag[at++] = '"';
    char *copy = malloc(at + 1);
    if (copy == NULL) return CF_NOMEM;
    memcpy(copy, etag, at);
    copy[at] = '\0';
    out->ptr = copy;
    out->len = at;
    return CF_OK;
}

/* `request.fresh?` for a proxy response (ETag only: the response carries no
 * Last-Modified validator on the request side, and an If-Modified-Since
 * alone is never fresh). */
static bool bl_request_fresh(const cf_request *request, cf_span etag) {
    cf_span inm = {NULL, 0};
    if (!bl_header(request, "if-none-match", &inm)) return false;
    size_t at = 0;
    while (at <= inm.len) {
        size_t end = at;
        while (end < inm.len && inm.ptr[end] != ',') end++;
        size_t start = at, stop = end;
        while (start < stop &&
               (inm.ptr[start] == ' ' || inm.ptr[start] == '\t')) {
            start++;
        }
        while (stop > start &&
               (inm.ptr[stop - 1] == ' ' || inm.ptr[stop - 1] == '\t')) {
            stop--;
        }
        cf_span value = {inm.ptr + start, stop - start};
        if ((value.len == 1 && value.ptr[0] == '*') ||
            (value.len == etag.len &&
             memcmp(value.ptr, etag.ptr, etag.len) == 0)) {
            return true;
        }
        if (end == inm.len) break;
        at = end + 1;
    }
    return false;
}

/* The storage service for this request, rooted at the configured
 * STORAGE_PATH (never request data). The caller closes the handle. */
static cf_err bl_storage_open(cf_ctx *ctx, cf_storage **out) {
    *out = NULL;
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->storage_path == NULL) return CF_INTERNAL;
    return cf_storage_open(config->storage_path, out);
}

/* `http_cache_forever`'s 304: ETag, Last-Modified and the 100-year
 * Cache-Control, no Content-Type, no body. */
static cf_err bl_answer_not_modified(cf_ctx *ctx, cf_span etag) {
    ctx->response->status = 304;
    cf_err rc = cf_response_header(ctx->response, bl_span("ETag"), etag);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, bl_span("Last-Modified"),
                                bl_span(BL_FOREVER_LAST_MODIFIED));
    }
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, bl_span("Cache-Control"),
                                bl_span(BL_FOREVER_CACHE_CONTROL));
    }
    return rc;
}

/* Validator headers shared by the proxy 200/206 bodies. */
static cf_err bl_answer_validators(cf_ctx *ctx, cf_span etag) {
    cf_err rc = cf_response_header(ctx->response, bl_span("ETag"), etag);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, bl_span("Last-Modified"),
                                bl_span(BL_FOREVER_LAST_MODIFIED));
    }
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, bl_span("Cache-Control"),
                                bl_span(BL_FOREVER_CACHE_CONTROL));
    }
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, bl_span("Accept-Ranges"),
                                bl_span("bytes"));
    }
    return rc;
}

/* Read exactly `len` bytes at `offset` into the builder. */
static cf_err bl_pread_all(int fd, uint64_t offset, uint64_t len,
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

/* ---- actions -------------------------------------------------------------- */

/* `Blobs::RedirectController#show` (routes 169, 171). */
cf_err cf_action_active_storage_blobs_redirect_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc = cf_check_csrf(ctx, false);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_blob blob = {0};
    bool halted = false;
    rc = bl_resolve_blob(ctx, &blob, &halted);
    if (rc != CF_OK || halted) {
        cf_blob_dispose(&blob);
        return rc;
    }

    cf_span disp_param = {NULL, 0};
    bool has_disp = bl_param_str(ctx, "disposition", &disp_param);
    cf_span raw_ct = {NULL, 0};
    if (blob.content_type.present) {
        raw_ct = bl_str_span(blob.content_type.value);
    }
    cf_span serving = cf_active_content_type_for_serving(raw_ct);
    const char *forced = cf_active_forced_disposition(raw_ct);
    cf_span kind;
    if (forced != NULL) {
        kind = bl_span(forced);
    } else if (has_disp) {
        kind = disp_param;
    } else {
        kind = bl_span("inline");
    }
    cf_builder sanitized = {0};
    rc = cf_active_filename_sanitize(bl_str_span(blob.filename), &sanitized);
    cf_builder path = {0};
    if (rc == CF_OK) {
        rc = cf_active_service_url_path(
            bl_secret(ctx), bl_str_span(blob.key),
            cf_now_us(ctx->app) + BL_EXPIRE_IN_US,
            (cf_span){sanitized.ptr, sanitized.len}, serving,
            blob.content_type.present, kind, bl_span(BL_SERVICE_NAME), &path);
    }
    cf_builder_dispose(&sanitized);
    const cf_config *config = cf_app_config(ctx->app);
    if (rc == CF_OK && (config == NULL || config->public_origin == NULL)) {
        rc = CF_INTERNAL;
    }
    cf_builder location = {0};
    if (rc == CF_OK) {
        rc = cf_builder_append(&location, bl_span(config->public_origin));
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(&location, (cf_span){path.ptr, path.len});
    }
    cf_builder_dispose(&path);
    if (rc == CF_OK) {
        rc = bl_redirect_to(ctx, (cf_span){location.ptr, location.len});
    }
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, bl_span("Cache-Control"),
                                bl_span("max-age=300, private"));
    }
    cf_builder_dispose(&location);
    cf_blob_dispose(&blob);
    return rc;
}

/* `Blobs::ProxyController#show` (route 170). */
cf_err cf_action_active_storage_blobs_proxy_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc = cf_check_csrf(ctx, false);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_blob blob = {0};
    bool halted = false;
    rc = bl_resolve_blob(ctx, &blob, &halted);
    if (rc != CF_OK || halted) {
        cf_blob_dispose(&blob);
        return rc;
    }

    cf_span disp_param = {NULL, 0};
    bool has_disp = bl_param_str(ctx, "disposition", &disp_param);
    cf_span raw_ct = {NULL, 0};
    if (blob.content_type.present) {
        raw_ct = bl_str_span(blob.content_type.value);
    }
    cf_builder sanitized = {0};
    rc = cf_active_filename_sanitize(bl_str_span(blob.filename), &sanitized);
    if (rc != CF_OK) {
        cf_builder_dispose(&sanitized);
        cf_blob_dispose(&blob);
        return rc;
    }
    cf_span san = {sanitized.ptr, sanitized.len};

    cf_span range = {NULL, 0};
    bool has_range = bl_header(ctx->request, "range", &range);
    bool blank = true;
    if (has_range && range.ptr != NULL) {
        for (size_t i = 0; i < range.len; i++) {
            if (range.ptr[i] != ' ' && range.ptr[i] != '\t') {
                blank = false;
                break;
            }
        }
    }
    /* Ranges are cut from `blob.byte_size`, before any file check. */
    uint64_t size = (uint64_t)(blob.byte_size < 0 ? 0 : blob.byte_size);

    if (has_range && !blank) {
        /* `send_blob_byte_range_data`: the range verdict first, then the
         * file (whose absence raises internally from here). */
        unsigned char raw[16];
        rc = cf_random_bytes(raw, sizeof raw);
        static const char HEX[] = "0123456789abcdef";
        char boundary[33];
        if (rc == CF_OK) {
            for (size_t i = 0; i < sizeof raw; i++) {
                boundary[2 * i] = HEX[raw[i] >> 4];
                boundary[2 * i + 1] = HEX[raw[i] & 0x0F];
            }
            boundary[32] = '\0';
        }
        cf_active_serve plan = {0};
        if (rc == CF_OK) {
            rc = cf_active_proxy_serve(
                true, true, range, size, raw_ct, san, disp_param, has_disp,
                (cf_span){(unsigned char *)boundary, 32}, true, &plan);
        }
        if (rc == CF_OK && plan.status == 416) {
            cf_active_serve_dispose(&plan);
            cf_builder_dispose(&sanitized);
            cf_blob_dispose(&blob);
            return bl_head(ctx, 416);
        }
        cf_storage *storage = NULL;
        int fd = -1;
        uint64_t file_size = 0;
        if (rc == CF_OK) {
            rc = bl_storage_open(ctx, &storage);
        }
        if (rc == CF_OK) {
            rc = cf_storage_open_read(storage, bl_str_span(blob.key), &fd,
                                      &file_size);
            if (rc == CF_NOT_FOUND) rc = CF_INTERNAL;
        }
        if (rc != CF_OK) {
            if (fd >= 0) close(fd);
            if (storage != NULL) cf_storage_close(storage);
            cf_active_serve_dispose(&plan);
            cf_builder_dispose(&sanitized);
            cf_blob_dispose(&blob);
            return rc;
        }
        if (plan.has_single) {
            rc = cf_response_header(ctx->response, bl_span("Content-Type"),
                                    bl_str_span(plan.content_type));
            if (rc == CF_OK) {
                rc = cf_response_header(
                    ctx->response, bl_span("Content-Disposition"),
                    bl_str_span(plan.content_disposition));
            }
            if (rc == CF_OK) {
                rc = cf_response_header(ctx->response,
                                        bl_span("content-transfer-encoding"),
                                        bl_span("binary"));
            }
            if (rc == CF_OK) {
                cf_str etag = {0};
                rc = bl_forever_etag(ctx, &etag);
                if (rc == CF_OK) {
                    rc = bl_answer_validators(ctx, bl_str_span(etag));
                }
                cf_str_dispose(&etag);
            }
            if (rc == CF_OK) {
                rc = cf_response_header(ctx->response,
                                        bl_span("Content-Range"),
                                        bl_str_span(plan.content_range));
            }
            if (rc == CF_OK) ctx->response->status = 206;
            if (rc == CF_OK) {
                uint64_t len = plan.single.end - plan.single.start + 1;
                rc = cf_response_file(ctx->response, fd, plan.single.start,
                                      len);
                if (rc != CF_OK) close(fd);
            } else {
                close(fd);
            }
        } else {
            /* Multipart: the S02 plan already carries the multipart
             * Content-Type and length for this boundary. */
            cf_builder body = {0};
            if (rc == CF_OK && plan.status != 206) rc = CF_INTERNAL;
            if (rc == CF_OK) {
                cf_span bspan = {(unsigned char *)boundary, 32};
                cf_span serving =
                    cf_active_content_type_for_serving(raw_ct);
                for (size_t i = 0; i < plan.n_ranges && rc == CF_OK; i++) {
                    rc = cf_active_proxy_part_heading(
                        bspan, serving, plan.ranges[i].start,
                        plan.ranges[i].end, size, &body);
                    if (rc == CF_OK) {
                        rc = bl_pread_all(fd, plan.ranges[i].start,
                                          plan.ranges[i].end -
                                              plan.ranges[i].start + 1,
                                          &body);
                    }
                }
                if (rc == CF_OK) {
                    rc = cf_active_proxy_part_trailer(bspan, &body);
                }
            }
            if (rc == CF_OK && body.len != plan.content_length) {
                rc = CF_INTERNAL;
            }
            if (rc == CF_OK) {
                rc = cf_response_header(ctx->response, bl_span("Content-Type"),
                                        bl_str_span(plan.content_type));
            }
            if (rc == CF_OK) {
                rc = cf_response_header(
                    ctx->response, bl_span("Content-Disposition"),
                    bl_str_span(plan.content_disposition));
            }
            if (rc == CF_OK) {
                rc = cf_response_header(ctx->response,
                                        bl_span("content-transfer-encoding"),
                                        bl_span("binary"));
            }
            if (rc == CF_OK) {
                cf_str etag = {0};
                rc = bl_forever_etag(ctx, &etag);
                if (rc == CF_OK) {
                    rc = bl_answer_validators(ctx, bl_str_span(etag));
                }
                cf_str_dispose(&etag);
            }
            if (rc == CF_OK) ctx->response->status = 206;
            if (rc == CF_OK) {
                cf_buf *buf = NULL;
                rc = cf_builder_freeze(&body, &buf);
                if (rc == CF_OK) rc = cf_response_body(ctx->response, buf);
                cf_buf_release(buf);
                if (rc != CF_OK) cf_builder_dispose(&body);
            } else {
                cf_builder_dispose(&body);
            }
            close(fd);
        }
        if (storage != NULL) cf_storage_close(storage);
        cf_active_serve_dispose(&plan);
        cf_builder_dispose(&sanitized);
        cf_blob_dispose(&blob);
        return rc;
    }

    /* No Range: `http_cache_forever` (304 when fresh), else the stream. */
    cf_str etag = {0};
    rc = bl_forever_etag(ctx, &etag);
    if (rc != CF_OK) {
        cf_str_dispose(&etag);
        cf_builder_dispose(&sanitized);
        cf_blob_dispose(&blob);
        return rc;
    }
    if (bl_request_fresh(ctx->request, bl_str_span(etag))) {
        rc = bl_answer_not_modified(ctx, bl_str_span(etag));
        cf_str_dispose(&etag);
        cf_builder_dispose(&sanitized);
        cf_blob_dispose(&blob);
        return rc;
    }

    cf_storage *storage = NULL;
    rc = bl_storage_open(ctx, &storage);
    int fd = -1;
    uint64_t file_size = 0;
    if (rc == CF_OK) {
        rc = cf_storage_open_read(storage, bl_str_span(blob.key), &fd,
                                  &file_size);
        if (rc == CF_NOT_FOUND) {
            /* `rescue FileNotFoundError`: expires_now, head :not_found. */
            if (storage != NULL) cf_storage_close(storage);
            storage = NULL;
            ctx->response->status = 404;
            rc = cf_response_header(ctx->response, bl_span("Cache-Control"),
                                    bl_span("no-cache"));
            if (rc == CF_OK) rc = bl_head(ctx, 404);
            cf_str_dispose(&etag);
            cf_builder_dispose(&sanitized);
            cf_blob_dispose(&blob);
            return rc;
        }
    }
    if (rc != CF_OK) {
        if (fd >= 0) close(fd);
        if (storage != NULL) cf_storage_close(storage);
        cf_str_dispose(&etag);
        cf_builder_dispose(&sanitized);
        cf_blob_dispose(&blob);
        return rc;
    }

    /* `send_blob_stream`: the whole file, inline unless forced. */
    cf_active_serve plan = {0};
    rc = cf_active_proxy_serve(true, false, (cf_span){NULL, 0}, file_size,
                               raw_ct, san, disp_param, has_disp,
                               (cf_span){NULL, 0}, false, &plan);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, bl_span("Content-Type"),
                                bl_str_span(plan.content_type));
    }
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, bl_span("Content-Disposition"),
                                bl_str_span(plan.content_disposition));
    }
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response,
                                bl_span("content-transfer-encoding"),
                                bl_span("binary"));
    }
    if (rc == CF_OK) rc = bl_answer_validators(ctx, bl_str_span(etag));
    if (rc == CF_OK) {
        ctx->response->status = 200;
        rc = cf_response_file(ctx->response, fd, 0, file_size);
        if (rc != CF_OK) close(fd);
    } else {
        close(fd);
    }
    if (storage != NULL) cf_storage_close(storage);
    cf_active_serve_dispose(&plan);
    cf_str_dispose(&etag);
    cf_builder_dispose(&sanitized);
    cf_blob_dispose(&blob);
    return rc;
}
