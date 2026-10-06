/* src/actions/users/avatars.c — Users::AvatarsController#show (task
 * A-users-avatars, route ID 53; contracts/routes.json).  `destroy` (route 54)
 * is implemented by users/avatars_destroy.c.
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/users/avatars.rs
 * (whole file), the port of reference/app/controllers/users/avatars_controller.rb:
 *
 *   def show
 *     user = User.from_avatar_token(params[:user_id])
 *     if user.avatar.variable?
 *       send_file user.avatar.variant(:square).processed
 *     elsif user.bot?
 *       send_file Rails.root.join("app/assets/images/default-bot-avatar.svg"), ...
 *     else
 *       render formats: :svg
 *     end
 *   end
 *
 * Translated in order:
 *   use_live_response -> before_actions(Before::default()) ->
 *   from_avatar_token -> fresh_when(cache_key_with_version + template digest)
 *   -> expires_in(30.minutes, public: true, stale_while_revalidate: 1.week)
 *   -> the avatar variant (blocked, see below) -> default bot avatar ->
 *   initials SVG.
 *
 * Token: `User.from_avatar_token(sid)` is `find_signed!(sid, purpose:
 * :avatar)`, i.e. cf_auth_signed_id_verify(secret, "User", token, "avatar")
 * (A01, src/auth.h).  A bad signature is `head :not_found` (status 404, the
 * rendered format's content type, empty body); a valid signature for a
 * missing user is `Error::NotFound` (the generic map renders the reference's
 * public 404 page).  The token check runs before any freshness work, so a
 * 404 never carries an ETag.
 *
 * Freshness: `cache_key_with_version("users", user.id, user.updated_at)` plus
 * the `ETagWithTemplateDigest` digest when `lookup_context.find_all("show",
 * ["users/avatars", ...])` finds `show.svg.erb` for the request's formats
 * (no negotiated format at all, the wildcard, or svg), plus turbo-rails' frame
 * etagger and `ETagWithFlash`, joined with "/" and digested as in
 * kit/src/ctx.rs combine_etags (SHA-256, first 16 bytes, weak).  The
 * reference sets the ETag with `fresh_when` BEFORE `expires_in`, so the 304
 * early return carries the ETag and only the normalized default
 * Cache-Control (`max-age=0, private, must-revalidate`); the 200 carries
 * `max-age=1800, public, stale-while-revalidate=604800`.  The pinned
 * `expires_in` writes no `Expires` header (Rails' expires_in sets
 * Cache-Control and Date; kit/src/ctx.rs:503-516; the Date header is added
 * by the C serializer for every response).
 *
 * Uploaded variable images are processed as the :square webp variant;
 * non-variable attachments preserve the source initials/bot fallback.
 * Processing and file reads run on request/media workers before response
 * ownership transfers the opened file descriptor to the HTTP loop.
 *
 * c_symbol for the integrator's route rebind (src/routes.c row 53):
 *   cf_action_users_avatars_show.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "models/active_storage.h"
#include "models/user.h"
#include "routes.h"      /* cf_static_root: the pinned asset tree */
#include "storage/active_storage.h"
#include "storage/storage.h"
#include "views.h"       /* cf_views_asset_path, cf_view_users_avatar_svg */

#include <fcntl.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* `ActionView::Digestor.digest(name: "users/avatars/show", ...)`: the SHA256
 * (truncated) of show.svg.erb's source plus "-" (it renders nothing else);
 * ETagWithTemplateDigest adds it whenever the template is found for the
 * request's formats (avatars.rs TEMPLATE_DIGEST). */
#define AV_TEMPLATE_DIGEST "d500db55e2a67222018ef0156839c3c9"

/* `expires_in 30.minutes, public: true, stale_while_revalidate: 1.week`. */
#define AV_MAX_AGE "max-age=1800, public, stale-while-revalidate=604800"

/* The normalized default kit apply_cache_headers applies to a response that
 * has an ETag and no cache-control of its own (the fresh_when 304 path). */
#define AV_NOT_MODIFIED_CACHE_CONTROL "max-age=0, private, must-revalidate"

static cf_span av_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* A cf_str view of a literal (the model layer's record_type/name take an
 * owned-length string, not a span). */
static cf_str av_cstr(const char *text) {
    return (cf_str){(char *)text, strlen(text)};
}

/* Borrowed span of an owned cf_str. */
static cf_span av_str_span(cf_str value) {
    return (cf_span){(const unsigned char *)value.ptr, value.len};
}

/* ------------------------------------------------------ request helpers */

/* HeaderValue::to_str (http 1.5.0): every byte must be HTAB or visible
 * ASCII; obs-text, DEL and the other controls make the header read as
 * absent. */
static bool av_header_readable(cf_span value) {
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (c != '\t' && (c < 0x20 || c > 0x7E)) return false;
    }
    return true;
}

/* The kit's `request.header(name)`: first case-insensitive match whose value
 * passes HeaderValue::to_str. */
static bool av_header(const cf_request *request, const char *name,
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
        if (!av_header_readable(request->headers[i].value)) return false;
        *out = request->headers[i].value;
        return true;
    }
    return false;
}

/* `is_turbo_frame_request`: a readable, non-blank Turbo-Frame header. */
static bool av_turbo_frame_request(const cf_request *request) {
    cf_span value;
    if (!av_header(request, "turbo-frame", &value)) return false;
    bool blank = true;
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (!((c >= 32 && c < 127) || c == '\t')) return false;
        if (c != ' ' && c != '\t') blank = false;
    }
    return !blank;
}

static cf_err av_sha256(const unsigned char *data, size_t len,
                        unsigned char out[32]) {
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (md == NULL) return CF_NOMEM;
    unsigned int out_len = 0;
    cf_err rc = CF_OK;
    if (EVP_DigestInit_ex(md, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(md, data, len) != 1 ||
        EVP_DigestFinal_ex(md, out, &out_len) != 1) {
        rc = CF_INTERNAL;
    } else if (out_len != 32) {
        rc = CF_INTERNAL;
    }
    EVP_MD_CTX_free(md);
    return rc;
}

/* `Time#to_fs(:usec)` cache version: YYYYMMDDHHMMSS + 6-digit microseconds
 * (crates/views/src/fragment_cache.rs push_cache_version). */
static void av_floor_div(int64_t value, int64_t divisor, int64_t *quot,
                         int64_t *rem) {
    int64_t q = value / divisor;
    int64_t r = value % divisor;
    if (r < 0) {
        q -= 1;
        r += divisor;
    }
    if (quot != NULL) *quot = q;
    if (rem != NULL) *rem = r;
}

/* UTC civil date from days since 1970-01-01 (Howard Hinnant's algorithm). */
static void av_civil_from_days(int64_t days, int64_t *year, unsigned *month,
                               unsigned *day) {
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    *year = y + (m <= 2);
    *month = m;
    *day = d;
}

static void av_cache_version(int64_t time_us, char out[21]) {
    int64_t secs = 0, usec = 0;
    av_floor_div(time_us, INT64_C(1000000), &secs, &usec);
    int64_t days = 0, sod = 0;
    av_floor_div(secs, 86400, &days, &sod);
    int64_t year = 0;
    unsigned month = 0, day = 0;
    av_civil_from_days(days, &year, &month, &day);
    unsigned hour = (unsigned)(sod / 3600);
    unsigned minute = (unsigned)((sod % 3600) / 60);
    unsigned second = (unsigned)(sod % 60);
    /* Format into a wider local so GCC's -Wformat-truncation sees no
     * possible truncation, then keep the same 20-character prefix a
     * 21-byte snprintf would. */
    char text[64];
    int n = snprintf(text, sizeof text, "%04" PRId64 "%02u%02u%02u%02u%02u%06" PRId64,
                     year, month, day, hour, minute, second, usec);
    if (n < 0) n = 0;
    if (n > 20) n = 20;
    memcpy(out, text, (size_t)n);
    out[n] = '\0';
}

/* `template_found`: `lookup_context.find_all("show", ["users/avatars", ...])
 * finds show.svg.erb` for the request's formats — the wildcard or svg, or no
 * registered format at all (an image wildcard Accept parses to none).
 * InvalidMimeType is false, not an error. */
static bool av_template_found(cf_ctx *ctx) {
    const cf_format **formats = NULL;
    size_t count = 0;
    if (cf_ctx_formats(ctx, &formats, &count) != CF_OK) return false;
    if (count == 0) return true;
    for (size_t i = 0; i < count; i++) {
        if (formats[i] == &cf_format_all ||
            strcmp(formats[i]->symbol, "svg") == 0) {
            return true;
        }
    }
    return false;
}

/* The `{:?}` spelling of one flash value (`Some("...")`), appended raw. */
static cf_err av_append_flash_part(cf_builder *out, cf_span key,
                                   cf_span value) {
    cf_err rc = cf_builder_append(out, key);
    if (rc == CF_OK) rc = cf_builder_append(out, av_span("=Some(\""));
    for (size_t i = 0; rc == CF_OK && i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (c == '"') {
            rc = cf_builder_append(out, av_span("\\\""));
        } else if (c == '\\') {
            rc = cf_builder_append(out, av_span("\\\\"));
        } else if (c == '\n') {
            rc = cf_builder_append(out, av_span("\\n"));
        } else if (c == '\r') {
            rc = cf_builder_append(out, av_span("\\r"));
        } else if (c == '\t') {
            rc = cf_builder_append(out, av_span("\\t"));
        } else if (c < 0x20 || c == 0x7F) {
            char esc[16];
            snprintf(esc, sizeof esc, "\\u{%x}", (unsigned)c);
            rc = cf_builder_append(out, av_span(esc));
        } else {
            rc = cf_builder_append(out, (cf_span){&c, 1});
        }
    }
    if (rc == CF_OK) rc = cf_builder_append(out, av_span("\")"));
    return rc;
}

static cf_err av_str_dup(cf_span span, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (span.len != 0 && span.ptr == NULL) return CF_INVALID;
    char *copy = malloc(span.len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (span.len != 0) memcpy(copy, span.ptr, span.len);
    copy[span.len] = '\0';
    out->ptr = copy;
    out->len = span.len;
    return CF_OK;
}

static void av_str_dispose(cf_str *value) {
    if (value == NULL) return;
    free(value->ptr);
    memset(value, 0, sizeof *value);
}

/* `fresh_when`'s combine_etags over
 * [cache_key_with_version, "frame", template digest, flash] joined by "/",
 * SHA-256, first 16 bytes hex, weak.  Owned output. */
static cf_err av_etag(cf_ctx *ctx, const cf_user *user, cf_str *out) {
    memset(out, 0, sizeof *out);
    char version[21];
    av_cache_version(user->updated_at, version);
    char key[96];
    int n = snprintf(key, sizeof key, "users/%" PRId64 "-%s", user->id,
                     version);
    if (n < 0 || (size_t)n >= sizeof key) return CF_INTERNAL;

    cf_builder parts = {0};
    cf_err rc = cf_builder_append(
        &parts, (cf_span){(const unsigned char *)key, (size_t)n});
    if (rc == CF_OK && av_turbo_frame_request(ctx->request)) {
        rc = cf_builder_append(&parts, av_span("/frame"));
    }
    if (rc == CF_OK && av_template_found(ctx)) {
        rc = cf_builder_append(&parts, av_span("/" AV_TEMPLATE_DIGEST));
    }
    /* `flash.keys().map(|k| format!("{k}={:?}", flash.get(k))).join("&")` is
     * one etag part; cf_ctx_flash_at loads the persisted flash like the
     * reference's combine_etags. */
    for (size_t i = 0; rc == CF_OK; i++) {
        cf_span flash_key, flash_value;
        if (!cf_ctx_flash_at(ctx, i, &flash_key, &flash_value)) break;
        rc = cf_builder_append(&parts, i == 0 ? av_span("/") : av_span("&"));
        if (rc == CF_OK) {
            rc = av_append_flash_part(&parts, flash_key, flash_value);
        }
    }
    unsigned char digest[32];
    if (rc == CF_OK) {
        rc = av_sha256(parts.ptr != NULL ? parts.ptr
                                         : (const unsigned char *)"",
                       parts.len, digest);
    }
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
    return av_str_dup((cf_span){(const unsigned char *)etag, at}, out);
}

/* `request.fresh?` with the reference's validators: the ETag, and no
 * Last-Modified (so an If-Modified-Since alone is never fresh).  An
 * If-None-Match list naming the ETag (or `*`) wins. */
static bool av_request_fresh(const cf_request *request, cf_span etag) {
    cf_span if_none_match;
    if (!av_header(request, "if-none-match", &if_none_match)) return false;
    size_t at = 0;
    while (at <= if_none_match.len) {
        size_t end = at;
        while (end < if_none_match.len && if_none_match.ptr[end] != ',') {
            end++;
        }
        size_t start = at, stop = end;
        while (start < stop &&
               (if_none_match.ptr[start] == ' ' || if_none_match.ptr[start] == '\t')) {
            start++;
        }
        while (stop > start &&
               (if_none_match.ptr[stop - 1] == ' ' ||
                if_none_match.ptr[stop - 1] == '\t')) {
            stop--;
        }
        cf_span value = {if_none_match.ptr + start, stop - start};
        bool star = value.len == 1 && value.ptr[0] == '*';
        if (star ||
            (value.len == etag.len &&
             memcmp(value.ptr, etag.ptr, etag.len) == 0)) {
            return true;
        }
        if (end == if_none_match.len) break;
        at = end + 1;
    }
    return false;
}

/* `head :status` (kit Ctx::head): the status, the rendered format's content
 * type and an empty body. */
static cf_err av_head(cf_ctx *ctx, unsigned status) {
    ctx->response->status = status;
    const cf_format *format = cf_ctx_rendered_format(ctx);
    return cf_response_header(ctx->response, av_span("Content-Type"),
                              av_span(format->string));
}

/* `User.from_avatar_token(params[:user_id])`.  A bad/missing token answers
 * `head :not_found` through the response (halted); a valid token for a
 * missing user is CF_NOT_FOUND (the generic mapping answers the reference's
 * public 404 page). */
static cf_err av_resolve_user(cf_ctx *ctx, cf_user *out, bool *halted) {
    *halted = false;
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->secret_key_base == NULL) return CF_INTERNAL;

    /* `param_str("user_id").unwrap_or_default()`: absent or non-string reads
     * as the empty token. */
    const cf_param *param = cf_ctx_param(ctx, av_span("user_id"));
    cf_span token = {NULL, 0};
    if (param != NULL && cf_param_type(param) == CF_PARAM_STRING &&
        cf_param_string(param, &token) != CF_OK) {
        token = (cf_span){NULL, 0};
    }

    cf_optional_i64 id = {false, 0};
    bool found = false;
    cf_err rc = cf_auth_signed_id_verify(
        (cf_span){(const unsigned char *)config->secret_key_base,
                  config->secret_key_base_len},
        av_span("User"), token, av_span("avatar"), true, cf_now_us(ctx->app),
        &id, &found);
    if (rc != CF_OK) return rc;
    if (!found) {
        rc = av_head(ctx, 404);
        *halted = rc == CF_OK;
        return rc;
    }

    rc = cf_user_find_by_id(ctx->reader, id.value, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_NOT_FOUND;
    return CF_OK;
}

/* Process only variable images. Non-variable attachments retain the source's
 * initials/bot fallback, rather than turning every attached file into a 500. */
static cf_err av_send_uploaded(cf_ctx *ctx, int64_t user_id, bool *sent) {
    *sent = false;
    bool found = false;
    cf_attachment attachment = {0};
    cf_blob blob = {0}, variant = {0};
    cf_err rc = cf_attachment_find_for(ctx->reader, av_cstr("User"), user_id,
                                       av_cstr("avatar"), &found, &attachment);
    if (rc == CF_OK && found) rc = cf_attachment_blob(ctx->reader, &attachment, &blob);
    cf_attachment_dispose(&attachment);
    if (rc != CF_OK || !found) goto done;
    if (!blob.content_type.present ||
        !cf_active_content_type_variable(av_str_span(blob.content_type.value))) goto done;
    cf_active_ventries variation = {0};
    cf_active_vval *format = NULL, *dimensions = NULL, *width = NULL, *height = NULL;
    rc = cf_active_vsym(av_span("webp"), &format);
    if (rc == CF_OK) rc = cf_active_ventries_push(&variation, av_span("format"), format);
    if (rc == CF_OK) format = NULL;
    if (rc == CF_OK) rc = cf_active_varr(&dimensions);
    if (rc == CF_OK) rc = cf_active_vint(512, &width);
    if (rc == CF_OK) rc = cf_active_varr_push(dimensions, width);
    if (rc == CF_OK) width = NULL;
    if (rc == CF_OK) rc = cf_active_vint(512, &height);
    if (rc == CF_OK) rc = cf_active_varr_push(dimensions, height);
    if (rc == CF_OK) height = NULL;
    if (rc == CF_OK) rc = cf_active_ventries_push(&variation, av_span("resize_to_limit"), dimensions);
    if (rc == CF_OK) dimensions = NULL;
    if (rc == CF_OK) rc = cf_active_processed_representation(ctx, &blob, &variation, &variant);
    cf_active_vval_dispose(format); cf_active_vval_dispose(dimensions);
    cf_active_vval_dispose(width); cf_active_vval_dispose(height);
    cf_active_ventries_dispose(&variation);
    cf_storage *storage = NULL; int fd = -1; uint64_t size = 0;
    const cf_config *config = cf_app_config(ctx->app);
    if (rc == CF_OK) rc = cf_storage_open(config->storage_path, &storage);
    if (rc == CF_OK) rc = cf_storage_open_read(storage, av_str_span(variant.key), &fd, &size);
    cf_storage_close(storage);
    if (rc == CF_OK) rc = cf_response_header(ctx->response, av_span("Content-Type"), av_span("image/webp"));
    if (rc == CF_OK) rc = cf_response_header(ctx->response, av_span("Content-Disposition"), av_span("inline"));
    if (rc == CF_OK) rc = cf_response_header(ctx->response, av_span("content-transfer-encoding"), av_span("binary"));
    if (rc == CF_OK) {
        rc = cf_response_file(ctx->response, fd, 0, size);
        if (rc == CF_OK) { fd = -1; *sent = true; ctx->response->status = 200; }
    }
    if (fd >= 0) close(fd);
    if (rc == CF_NOT_FOUND) rc = CF_INTERNAL;
done:
    cf_blob_dispose(&variant); cf_blob_dispose(&blob);
    return rc;
}

/* `send_file <assets>/default-bot-avatar.svg, content_type: "image/svg+xml",
 * disposition: :inline`: the pinned asset tree is resolved through the
 * loaded manifest (cf_views_asset_path -> "/assets/<digested>"), then served
 * from <static root>/public.  The response filename is the logical asset
 * name, as the reference's temp-file path carries it. */
static cf_err av_send_default_bot(cf_ctx *ctx) {
    cf_builder url = {0};
    cf_err rc = cf_views_asset_path(av_span("default-bot-avatar.svg"), &url);
    if (rc != CF_OK) {
        cf_builder_dispose(&url);
        return CF_INTERNAL; /* a missing asset is send_file's internal error */
    }
    static const char PUBLIC[] = "/public";
    const char *root = cf_static_root();
    size_t root_len = strlen(root);
    size_t total = root_len + (sizeof PUBLIC - 1) + url.len + 1;
    char *path = malloc(total);
    if (path == NULL) {
        cf_builder_dispose(&url);
        return CF_NOMEM;
    }
    memcpy(path, root, root_len);
    memcpy(path + root_len, PUBLIC, sizeof PUBLIC - 1);
    memcpy(path + root_len + (sizeof PUBLIC - 1), url.ptr, url.len);
    path[total - 1] = '\0';
    cf_builder_dispose(&url);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    free(path);
    if (fd < 0) return CF_INTERNAL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(fd);
        return CF_INTERNAL;
    }

    rc = cf_response_header(ctx->response, av_span("Content-Type"),
                            av_span("image/svg+xml"));
    if (rc == CF_OK) {
        rc = cf_response_header(
            ctx->response, av_span("Content-Disposition"),
            av_span("inline; filename=\"default-bot-avatar.svg\"; "
                    "filename*=UTF-8''default-bot-avatar.svg"));
    }
    if (rc == CF_OK) {
        /* The kit response's lower-case header name, as the Rust writes it
         * (kit/src/response.rs send). */
        rc = cf_response_header(ctx->response,
                                av_span("content-transfer-encoding"),
                                av_span("binary"));
    }
    if (rc != CF_OK) {
        close(fd);
        return rc;
    }
    ctx->response->status = 200;
    rc = cf_response_file(ctx->response, fd, 0, (uint64_t)st.st_size);
    if (rc != CF_OK) close(fd);
    return rc;
}

/* `render formats: :svg` (`users/avatars/show.svg.erb`): the initials SVG,
 * with `Vary: Accept` like any render whose format came from Accept. */
static cf_err av_send_initials(cf_ctx *ctx, const cf_user *user) {
    cf_str initials = {0};
    cf_err rc = cf_user_initials(user, &initials);
    if (rc != CF_OK) {
        av_str_dispose(&initials);
        return rc;
    }
    cf_builder svg = {0};
    rc = cf_view_users_avatar_svg(user->id, av_str_span(initials), &svg);
    av_str_dispose(&initials);
    if (rc != CF_OK) {
        cf_builder_dispose(&svg);
        return rc;
    }
    cf_buf *body = NULL;
    rc = cf_builder_freeze(&svg, &body);
    if (rc != CF_OK) {
        cf_builder_dispose(&svg);
        return rc;
    }
    ctx->response->status = 200;
    rc = cf_response_header(ctx->response, av_span("Content-Type"),
                            av_span("image/svg+xml; charset=utf-8"));
    if (rc == CF_OK && cf_ctx_vary_accept(ctx)) {
        rc = cf_response_header(ctx->response, av_span("Vary"),
                                av_span("Accept"));
    }
    if (rc == CF_OK) rc = cf_response_body(ctx->response, body);
    cf_buf_release(body);
    return rc;
}

cf_err cf_action_users_avatars_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    /* `before_actions(Before::default())`: authentication required, bots
     * denied, forgery protection on (a GET never reaches the CSRF step). */
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    bool halted = false;
    rc = av_resolve_user(ctx, &user, &halted);
    if (rc != CF_OK || halted) {
        cf_user_dispose(&user);
        return rc;
    }

    cf_str etag = {0};
    rc = av_etag(ctx, &user, &etag);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, av_span("ETag"),
                                av_str_span(etag));
    }
    if (rc == CF_OK && av_request_fresh(ctx->request, av_str_span(etag))) {
        /* `fresh_when`'s early `head :not_modified`: ETag already set,
         * no expires_in, so only the normalized ETag-response
         * Cache-Control applies. */
        ctx->response->status = 304;
        rc = cf_response_header(ctx->response, av_span("Cache-Control"),
                                av_span(AV_NOT_MODIFIED_CACHE_CONTROL));
        av_str_dispose(&etag);
        cf_user_dispose(&user);
        return rc;
    }
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, av_span("Cache-Control"),
                                av_span(AV_MAX_AGE));
    }
    bool sent = false;
    if (rc == CF_OK) rc = av_send_uploaded(ctx, user.id, &sent);
    if (rc == CF_OK && !sent) {
        if (cf_user_is_bot(&user)) {
            rc = av_send_default_bot(ctx);
        } else {
            rc = av_send_initials(ctx, &user);
        }
    }
    av_str_dispose(&etag);
    cf_user_dispose(&user);
    return rc;
}
