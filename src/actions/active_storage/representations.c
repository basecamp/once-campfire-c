/* Active Storage representations: reuse recorded images, otherwise transform
 * variants or draw video previews off the writer through bounded subprocesses.
 * The fixed libvips adapter mirrors the pinned reference's page0/autorotation,
 * down-only resize and sharpen mask; ffmpeg uses the reference poster argv.
 * Generated images are staged and analyzed before a short writer transaction
 * records the variant/preview attachment. A competing winner is re-read and
 * the losing staged file is discarded. Failed work never publishes a row.
 * Source: crates/campfire/src/active_storage.rs and crates/storage/src/storage.rs.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "http/params.h"
#include "models/active_storage.h"
#include "models/types.h"
#include "storage/active_storage.h"
#include "storage/storage.h"
#include "storage/media.h"
#include <errno.h>
#include <fcntl.h>

#include <openssl/evp.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RP_EXPIRE_IN_US INT64_C(300000000)
#define RP_FOREVER_CACHE_CONTROL "max-age=3155695200, public, immutable"
#define RP_FOREVER_LAST_MODIFIED "Sat, 01 Jan 2011 00:00:00 GMT"
#define RP_SERVICE_NAME "local"

static cf_span rp_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static cf_span rp_str_span(cf_str value) {
    return (cf_span){(const unsigned char *)value.ptr, value.len};
}

static bool rp_header_readable(cf_span value) {
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (c != '\t' && (c < 0x20 || c > 0x7E)) return false;
    }
    return true;
}

static bool rp_header(const cf_request *request, const char *name,
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
        if (!rp_header_readable(request->headers[i].value)) return false;
        *out = request->headers[i].value;
        return true;
    }
    return false;
}

static bool rp_param_str(cf_ctx *ctx, const char *name, cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, rp_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) return false;
    return cf_param_string(param, out) == CF_OK;
}

static cf_span rp_secret(cf_ctx *ctx) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->secret_key_base == NULL) {
        return (cf_span){NULL, 0};
    }
    return (cf_span){(const unsigned char *)config->secret_key_base,
                     config->secret_key_base_len};
}

static cf_err rp_head(cf_ctx *ctx, unsigned status) {
    ctx->response->status = status;
    const cf_format *format = cf_ctx_rendered_format(ctx);
    return cf_response_header(ctx->response, rp_span("Content-Type"),
                              rp_span(format->string));
}

static cf_err rp_redirect_to(cf_ctx *ctx, cf_span location) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    ctx->response->status = 302;
    cf_err rc =
        cf_response_header(ctx->response, rp_span("Location"), location);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, rp_span("Content-Type"),
                                rp_span("text/html; charset=utf-8"));
    }
    return rc;
}

static cf_err rp_storage_open(cf_ctx *ctx, cf_storage **out) {
    *out = NULL;
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->storage_path == NULL) return CF_INTERNAL;
    return cf_storage_open(config->storage_path, out);
}

/* `set_blob` over `params[:signed_blob_id]` (see blobs.c). */
static cf_err rp_resolve_blob(cf_ctx *ctx, cf_blob *out, bool *halted) {
    *halted = false;
    cf_span token = {NULL, 0};
    (void)rp_param_str(ctx, "signed_blob_id", &token);
    int64_t id = 0;
    bool found = false;
    cf_err rc = cf_active_blob_verify(rp_secret(ctx), token,
                                      cf_now_us(ctx->app), &id, &found);
    if (rc != CF_OK) return rc;
    if (!found) {
        rc = rp_head(ctx, 404);
        *halted = rc == CF_OK;
        return rc;
    }
    rc = cf_blob_find(ctx->reader, id, out);
    if (rc != CF_OK) cf_blob_dispose(out);
    return rc;
}

/* ---- variant-record SELECT (S02 left this SQL for actions) ---------------- */

/* `blob.variant_records.find_by(variation_digest:)` (blob.rs
 * find_variant_record): the record id, or found=false. No model accessor
 * exists (D01 header); this static statement is the marked shim — see the
 * integrator request at the end of this file. */
enum { RP_STMT_VARIANT_FIND = 0, RP_STMT_COUNT };

static const cf_stmt_def rp_stmt_defs[RP_STMT_COUNT] = {
    {"SELECT id FROM active_storage_variant_records WHERE blob_id = ?1 AND "
     "variation_digest = ?2"},
};

static const cf_stmt_set rp_stmts = {rp_stmt_defs, RP_STMT_COUNT};

static cf_err rp_find_variant_record(cf_db *db, int64_t blob_id,
                                     cf_span digest, bool *found,
                                     int64_t *out_id) {
    *found = false;
    *out_id = 0;
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &rp_stmts, RP_STMT_VARIANT_FIND, &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 1, blob_id);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2, digest);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            *out_id = cf_stmt_column_i64(stmt, 0);
            *found = true;
        } else if (step != SQLITE_DONE) {
            rc = cf_db_err(step);
        }
    }
    if (stmt != NULL) cf_db_stmt_done(stmt);
    return rc;
}

/* ---- default_variant_format shim (S03-owned Marcel) ----------------------- */

/* Exact pinned EXTENSIONS rows (tables.rs) for the image extensions the
 * shim arbitrates. Anything else reports no agreement. */
static const char *rp_marcel_for_extension(cf_span lower_ext) {
    static const struct {
        const char *ext;
        const char *type;
    } table[] = {
        {"avif", "image/avif"}, {"bmp", "image/bmp"},
        {"dib", "image/bmp"}, {"gif", "image/gif"},
        {"heic", "image/heic"}, {"heif", "image/heif"},
        {"ico", "image/vnd.microsoft.icon"}, {"jfif", "image/jpeg"},
        {"jfi", "image/jpeg"}, {"jif", "image/jpeg"},
        {"jpe", "image/jpeg"}, {"jpeg", "image/jpeg"},
        {"jpg", "image/jpeg"}, {"png", "image/png"},
        {"psb", "image/vnd.adobe.photoshop"},
        {"psd", "image/vnd.adobe.photoshop"}, {"tif", "image/tiff"},
        {"tiff", "image/tiff"}, {"webp", "image/webp"},
    };
    char lower[64];
    if (lower_ext.len >= sizeof lower) return NULL;
    for (size_t i = 0; i < lower_ext.len; i++) {
        unsigned char c = lower_ext.ptr[i];
        lower[i] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    lower[lower_ext.len] = '\0';
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strcmp(lower, table[i].ext) == 0) return table[i].type;
    }
    return NULL;
}

/* Exact pinned TYPE_EXTS firsts (tables.rs) for the four web-image types. */
static const char *rp_marcel_first_extension(cf_span content_type) {
    static const struct {
        const char *type;
        const char *first;
    } table[] = {
        {"image/gif", "gif"}, {"image/jpeg", "jpg"},
        {"image/png", "png"}, {"image/webp", "webp"},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        size_t n = strlen(table[i].type);
        if (content_type.len == n &&
            memcmp(content_type.ptr, table[i].type, n) == 0) {
            return table[i].first;
        }
    }
    return NULL;
}

/* Ruby `File.extname`: from the last dot of the basename (leading dots
 * stripped), "" when there is none. */
static cf_span rp_extname(cf_span filename) {
    size_t base = 0;
    for (size_t i = 0; i < filename.len; i++) {
        if (filename.ptr[i] == '/') base = i + 1;
    }
    size_t start = base;
    while (start < filename.len && filename.ptr[start] == '.') start++;
    size_t dot = filename.len;
    for (size_t i = start; i < filename.len; i++) {
        if (filename.ptr[i] == '.') dot = i;
    }
    if (dot == filename.len) return (cf_span){NULL, 0};
    return (cf_span){filename.ptr + dot, filename.len - dot};
}

/* `default_variant_format`: web images keep their format (the filename's
 * extension when Marcel agrees it names the content type, else the content
 * type's first registered extension, else "png"); everything else is "png".
 * The returned span borrows `filename` or a static literal. */
static cf_span rp_default_variant_format(cf_span content_type,
                                         cf_span filename) {
    static const char png[] = "png";
    if (!cf_active_content_type_web_image(content_type)) {
        return (cf_span){(const unsigned char *)png, 3};
    }
    cf_span ext = rp_extname(filename);
    if (ext.ptr != NULL && ext.len > 1) {
        cf_span bare = {ext.ptr + 1, ext.len - 1};
        const char *named = rp_marcel_for_extension(bare);
        if (named != NULL && strlen(named) == content_type.len &&
            memcmp(named, content_type.ptr, content_type.len) == 0) {
            return bare;
        }
    }
    const char *first = rp_marcel_first_extension(content_type);
    if (first != NULL) return rp_span(first);
    return (cf_span){(const unsigned char *)png, 3};
}

static bool rp_content_is_video(cf_span content_type) {
    static const char prefix[] = "video";
    return content_type.len >= sizeof prefix - 1 &&
           memcmp(content_type.ptr, prefix, sizeof prefix - 1) == 0;
}

/* ---- representation resolution -------------------------------------------- */

/* Resolve an already recorded image without doing media work. */
static cf_err rp_resolve_image(cf_ctx *ctx, const cf_blob *blob,
                               const cf_active_ventries *variation,
                               cf_blob *out, bool *found) {
    *found = false;
    cf_span raw_ct = {NULL, 0};
    if (blob->content_type.present) {
        raw_ct = rp_str_span(blob->content_type.value);
    }
    if (cf_active_content_type_variable(raw_ct)) {
        /* `variation_for`: default `format`, then digest and look up the
         * recorded variant's image. */
        cf_active_ventries defaulted = {0};
        cf_active_ventries defaults = {0};
        cf_active_vval *format = NULL;
        cf_span fmt = rp_default_variant_format(
            raw_ct, rp_str_span(blob->filename));
        cf_err rc = cf_active_vstr(fmt, &format);
        if (rc == CF_OK) {
            rc = cf_active_ventries_push(&defaults, rp_span("format"),
                                         format);
            if (rc != CF_OK) cf_active_vval_dispose(format);
        }
        if (rc == CF_OK) {
            rc = cf_active_variation_default(&defaults, variation,
                                             &defaulted);
        }
        cf_active_ventries_dispose(&defaults);
        cf_str digest = {0};
        if (rc == CF_OK) rc = cf_active_variation_digest(&defaulted, &digest);
        cf_active_ventries_dispose(&defaulted);
        bool have_record = false;
        int64_t record_id = 0;
        if (rc == CF_OK) {
            rc = rp_find_variant_record(
                ctx->reader, blob->id,
                (cf_span){(unsigned char *)digest.ptr, digest.len},
                &have_record, &record_id);
        }
        cf_str_dispose(&digest);
        if (rc != CF_OK) return rc;
        if (!have_record) return CF_OK;
        bool have_attachment = false;
        cf_attachment attachment = {0};
        cf_str record_type =
            {(char *)"ActiveStorage::VariantRecord", 28};
        cf_str name = {(char *)"image", 5};
        rc = cf_attachment_find_for(ctx->reader, record_type, record_id,
                                    name, &have_attachment, &attachment);
        int64_t image_id = 0;
        if (rc == CF_OK && have_attachment) image_id = attachment.blob_id;
        cf_attachment_dispose(&attachment);
        if (rc != CF_OK) return rc;
        if (!have_attachment) return CF_OK;
        rc = cf_blob_find(ctx->reader, image_id, out);
        if (rc == CF_NOT_FOUND) return CF_OK;
        if (rc != CF_OK) {
            cf_blob_dispose(out);
            return rc;
        }
        *found = true;
        return CF_OK;
    }
    /* `previewable?` is `video && ffmpeg_exists`; the ffmpeg gate is
     * S03-owned (no transform runs on the serve path below, so serving a
     * recorded preview needs no media work). */
    if (rp_content_is_video(raw_ct)) {
        if (!cf_active_ventries_empty(variation)) return CF_OK;
        bool have_attachment = false;
        cf_attachment attachment = {0};
        cf_str record_type = {(char *)"ActiveStorage::Blob", 19};
        cf_str name = {(char *)"preview_image", 13};
        cf_err rc = cf_attachment_find_for(ctx->reader, record_type,
                                           blob->id, name, &have_attachment,
                                           &attachment);
        int64_t image_id = 0;
        if (rc == CF_OK && have_attachment) image_id = attachment.blob_id;
        cf_attachment_dispose(&attachment);
        if (rc != CF_OK) return rc;
        if (!have_attachment) return CF_OK;
        rc = cf_blob_find(ctx->reader, image_id, out);
        if (rc == CF_NOT_FOUND) return CF_OK;
        if (rc != CF_OK) {
            cf_blob_dispose(out);
            return rc;
        }
        *found = true;
        return CF_OK;
    }
    return CF_OK;
}

/* Expensive processing runs on the request worker before the short writer
 * transaction. Published files remain owned by staged until the commit. */
static bool rp_blank(const cf_active_vval *v) {
    if (v->kind == CF_ACTIVE_V_NIL) return true;
    if (v->kind == CF_ACTIVE_V_BOOL) return !v->boolean;
    if (v->kind == CF_ACTIVE_V_ARR) return v->count == 0;
    if (v->kind == CF_ACTIVE_V_HASH) return v->hcount == 0;
    if (v->kind == CF_ACTIVE_V_STR) {
        for (size_t i = 0; i < v->len; i++)
            if (v->bytes[i] != ' ' && v->bytes[i] != '\t' &&
                v->bytes[i] != '\r' && v->bytes[i] != '\n') return false;
        return true;
    }
    return false;
}

static cf_err rp_operation(const cf_active_ventries *variation,
                           char format[16], int32_t *width, int32_t *height) {
    strcpy(format, "png");
    *width = *height = 0;
    for (size_t i = 0; i < variation->len; i++) {
        const cf_active_ventry *e = &variation->items[i];
        const cf_active_vval *v = e->value;
        if (strcmp(e->key, "format") == 0) {
            if ((v->kind != CF_ACTIVE_V_STR && v->kind != CF_ACTIVE_V_SYM) ||
                v->len >= 16 || !cf_media_format_valid((cf_span){(unsigned char *)v->bytes, v->len})) return CF_INVALID;
            for (size_t k = 0; k < v->len; k++)
                format[k] = v->bytes[k] >= 'A' && v->bytes[k] <= 'Z' ? v->bytes[k] + ('a' - 'A') : v->bytes[k];
            format[v->len] = 0;
        } else {
            if (strcmp(e->key, "combine_options") == 0) return CF_INVALID;
            if (rp_blank(v)) continue;
            if (strcmp(e->key, "resize_to_limit") != 0 ||
                v->kind != CF_ACTIVE_V_ARR || v->count != 2) return CF_INVALID;
            int32_t dims[2] = {0, 0};
            for (size_t k = 0; k < 2; k++) {
                const cf_active_vval *d = v->items[k];
                if (d->kind == CF_ACTIVE_V_NIL) continue;
                if (d->kind != CF_ACTIVE_V_INT || d->integer <= 0 || d->integer > INT32_MAX) return CF_INVALID;
                dims[k] = (int32_t)d->integer;
            }
            if (dims[0] == 0 && dims[1] == 0) return CF_INVALID;
            *width = dims[0]; *height = dims[1];
        }
    }
    return CF_OK;
}

static cf_err rp_copy_to_temp(cf_storage *storage, cf_span key,
                              char path[512], int *fd_out) {
    int src = -1;
    uint64_t size = 0;
    cf_err rc = cf_storage_open_read(storage, key, &src, &size);
    if (rc == CF_OK && size > CF_STORAGE_IMPORT_MAX_BYTES) rc = CF_LIMIT;
    if (rc == CF_OK) rc = cf_media_temp_create("/tmp", ".input", path, 512, fd_out);
    unsigned char buffer[65536];
    uint64_t offset = 0;
    while (rc == CF_OK && offset < size) {
        size_t take = size - offset < sizeof buffer ? (size_t)(size - offset) : sizeof buffer;
        ssize_t got = pread(src, buffer, take, (off_t)offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { rc = CF_IO; break; }
        size_t done = 0;
        while (done < (size_t)got) {
            ssize_t wrote = write(*fd_out, buffer + done, (size_t)got - done);
            if (wrote < 0 && errno == EINTR) continue;
            if (wrote <= 0) { rc = CF_IO; break; }
            done += (size_t)wrote;
        }
        offset += (uint64_t)got;
    }
    if (src >= 0) close(src);
    return rc;
}

typedef struct {
    const cf_blob *source;
    const cf_active_staged *staged;
    cf_span digest;
    bool preview, inserted;
    cf_blob image;
} rp_record_arg;

static cf_err rp_record(cf_tx *tx, void *opaque) {
    rp_record_arg *arg = opaque;
    cf_db *db = cf_tx_db(tx);
    cf_blob source = {0};
    cf_err rc = cf_blob_find(db, arg->source->id, &source);
    cf_blob_dispose(&source);
    if (rc != CF_OK) return rc;
    int64_t record = arg->source->id;
    bool existing = false;
    cf_attachment attachment = {0};
    if (arg->preview) {
        rc = cf_attachment_find_for(db, (cf_str){"ActiveStorage::Blob", 19},
                                   record, (cf_str){"preview_image", 13}, &existing, &attachment);
    } else {
        rc = rp_find_variant_record(db, record, arg->digest, &existing, &record);
        if (rc == CF_OK && existing) {
            bool found = false;
            rc = cf_attachment_find_for(db, (cf_str){"ActiveStorage::VariantRecord", 28}, record,
                                       (cf_str){"image", 5}, &found, &attachment);
            if (rc == CF_OK && !found) rc = CF_NOT_FOUND;
        }
    }
    if (rc == CF_OK && existing) rc = cf_blob_find(db, attachment.blob_id, &arg->image);
    cf_attachment_dispose(&attachment);
    if (rc != CF_OK || existing) return rc;
    if (!arg->preview) {
        sqlite3_stmt *stmt = NULL;
        int step = sqlite3_prepare_v2(cf_db_handle(db),
            "INSERT INTO active_storage_variant_records(blob_id,variation_digest) VALUES (?1,?2)", -1, &stmt, NULL);
        rc = cf_db_err(step);
        if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 1, arg->source->id);
        if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2, arg->digest);
        if (rc == CF_OK) {
            int result = sqlite3_step(stmt);
            rc = result == SQLITE_DONE ? CF_OK : cf_db_err(result);
        }
        sqlite3_finalize(stmt);
        if (rc != CF_OK) return rc;
        record = sqlite3_last_insert_rowid(cf_db_handle(db));
    }
    const cf_active_staged *st = arg->staged;
    cf_blob blob = {.key=st->key, .filename=st->filename, .content_type=st->content_type,
        .metadata={true, st->metadata}, .service_name=st->service_name, .byte_size=st->byte_size,
        .checksum={true, st->checksum}};
    rc = cf_blob_create(tx, &blob, &arg->image);
    if (rc == CF_OK) {
        rc = cf_attachment_create(tx, arg->preview ? (cf_str){"ActiveStorage::Blob", 19} : (cf_str){"ActiveStorage::VariantRecord", 28},
            record, arg->preview ? (cf_str){"preview_image", 13} : (cf_str){"image", 5}, arg->image.id, &attachment);
        cf_attachment_dispose(&attachment);
    }
    if (rc == CF_OK) arg->inserted = true;
    return rc;
}

static cf_err rp_process(cf_ctx *ctx, const cf_blob *blob,
                         const cf_active_ventries *variation, bool preview,
                         cf_blob *out) {
    cf_active_ventries defaults = {0}, defaulted = {0};
    cf_active_vval *v = NULL;
    cf_str digest = {0};
    cf_span ct = blob->content_type.present ? rp_str_span(blob->content_type.value) : (cf_span){0};
    cf_err rc = CF_OK;
    char format[16] = "jpg";
    int32_t width = 0, height = 0;
    if (!preview) {
        rc = cf_active_vstr(rp_default_variant_format(ct, rp_str_span(blob->filename)), &v);
        if (rc == CF_OK) { rc = cf_active_ventries_push(&defaults, rp_span("format"), v); if (rc != CF_OK) cf_active_vval_dispose(v); }
        if (rc == CF_OK) rc = cf_active_variation_default(&defaults, variation, &defaulted);
        if (rc == CF_OK) {
            rc = rp_operation(&defaulted, format, &width, &height);
            if (rc == CF_INVALID) rc = CF_INTERNAL;
        }
        if (rc == CF_OK) rc = cf_active_variation_digest(&defaulted, &digest);
    }
    cf_active_ventries_dispose(&defaults);
    cf_active_ventries_dispose(&defaulted);
    if (rc != CF_OK) { cf_str_dispose(&digest); return rc; }
    rc = cf_media_slots_acquire();
    if (rc != CF_OK) { cf_str_dispose(&digest); return rc; }
    cf_storage *storage = NULL;
    cf_active_staged staged = {0};
    char input[512] = {0}, output[512] = {0};
    int input_fd = -1, output_fd = -1;
    cf_proc_result result = {0};
    rc = rp_storage_open(ctx, &storage);
    if (rc == CF_OK) rc = rp_copy_to_temp(storage, rp_str_span(blob->key), input, &input_fd);
    char suffix[20];
    snprintf(suffix, sizeof suffix, ".%s", format);
    if (rc == CF_OK) rc = cf_media_temp_create("/tmp", suffix, output, sizeof output, &output_fd);
    cf_proc_opts opts;
    cf_media_argv argv;
    char width_text[16], height_text[16];
    snprintf(width_text, sizeof width_text, "%d", width);
    snprintf(height_text, sizeof height_text, "%d", height);
    char *args[] = {"transform", input, output, format, width_text, height_text, NULL};
    if (rc == CF_OK && preview) {
        rc = cf_media_poster_argv(&argv, input);
        if (rc == CF_OK) rc = cf_media_proc_opts(&opts, CF_PROC_FFMPEG, &argv, CF_MEDIA_OUTPUT_MAX_BYTES, CF_MEDIA_FFMPEG_TIMEOUT_MS);
    } else if (rc == CF_OK) {
        opts = (cf_proc_opts){.exe=CF_PROC_VIPS_ADAPTER, .args=args, .stdin_fd=-1, .stdout_fd=-1,
            .stdout_limit=CF_MEDIA_METADATA_MAX_BYTES, .stderr_fd=-1, .timeout_ms=CF_MEDIA_VIPS_TIMEOUT_MS};
    }
    if (rc == CF_OK) {
        rc = cf_proc_run(&opts, &result);
        if (rc == CF_NOT_FOUND) rc = CF_INTERNAL; /* missing required tool */
    }
    if (rc == CF_OK) rc = cf_media_check_result(&result);
    if (rc == CF_OK && preview) {
        cf_span bytes = cf_buf_span(result.out);
        if (bytes.len == 0) rc = CF_IO;
        size_t done = 0;
        while (rc == CF_OK && done < bytes.len) {
            ssize_t n = write(output_fd, bytes.ptr + done, bytes.len - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { rc = CF_IO; break; }
            done += (size_t)n;
        }
    }
    cf_proc_result_dispose(&result);
    cf_media_slots_release();
    cf_builder filename = {0}, metadata = {0};
    size_t base = blob->filename.len;
    for (size_t i = blob->filename.len; i > 0; i--) {
        if (blob->filename.ptr[i-1] == '.') { base = i-1; break; }
        if (blob->filename.ptr[i-1] == '/' || blob->filename.ptr[i-1] == '\\') break;
    }
    if (rc == CF_OK) rc = cf_builder_append(&filename, (cf_span){(unsigned char *)blob->filename.ptr, base});
    if (rc == CF_OK) rc = cf_builder_append(&filename, rp_span(suffix));
    const char *output_ct = preview ? "image/jpeg" : cf_media_content_type_for_format(rp_span(format));
    if (rc == CF_OK) rc = cf_active_stage_upload(storage, output_fd, (cf_span){filename.ptr,filename.len}, rp_span(output_ct), true, &staged);
    if (rc == CF_OK) rc = cf_media_analyze_path(output, rp_str_span(staged.content_type.value), rp_str_span(staged.metadata), &metadata);
    if (rc == CF_OK) {
        cf_str_dispose(&staged.metadata);
        staged.metadata.ptr = malloc(metadata.len + 1);
        if (staged.metadata.ptr == NULL) rc = CF_NOMEM;
        else { memcpy(staged.metadata.ptr, metadata.ptr, metadata.len); staged.metadata.ptr[metadata.len] = 0; staged.metadata.len = metadata.len; }
    }
    rp_record_arg record = {.source=blob, .staged=&staged, .digest=rp_str_span(digest), .preview=preview};
    if (rc == CF_OK) rc = cf_write(ctx->app, rp_record, &record);
    if (rc == CF_OK && record.inserted) rc = cf_active_staged_commit(&staged);
    if (rc == CF_OK) { *out = record.image; memset(&record.image,0,sizeof record.image); }
    cf_blob_dispose(&record.image);
    cf_active_staged_dispose(&staged);
    if (input_fd >= 0) close(input_fd);
    if (output_fd >= 0) close(output_fd);
    if (input[0]) (void)cf_media_temp_cleanup(input);
    if (output[0]) (void)cf_media_temp_cleanup(output);
    if (storage != NULL) cf_storage_close(storage);
    cf_builder_dispose(&filename); cf_builder_dispose(&metadata); cf_str_dispose(&digest);
    return rc;
}

cf_err cf_active_processed_representation(cf_ctx *ctx, const cf_blob *blob,
                           const cf_active_ventries *variation, cf_blob *out) {
    bool found = false;
    cf_err rc = rp_resolve_image(ctx, blob, variation, out, &found);
    if (rc != CF_OK || found) return rc;
    cf_span ct = blob->content_type.present ? rp_str_span(blob->content_type.value) : (cf_span){0};
    if (cf_active_content_type_variable(ct)) return rp_process(ctx, blob, variation, false, out);
    if (!rp_content_is_video(ct)) return CF_INTERNAL;
    cf_active_ventries empty = {0};
    cf_blob preview = {0};
    rc = rp_resolve_image(ctx, blob, &empty, &preview, &found);
    if (rc == CF_OK && !found) rc = rp_process(ctx, blob, &empty, true, &preview);
    if (rc == CF_OK && cf_active_ventries_empty(variation)) {
        *out = preview; memset(&preview, 0, sizeof preview);
    } else if (rc == CF_OK) rc = cf_active_processed_representation(ctx, &preview, variation, out);
    cf_blob_dispose(&preview);
    return rc;
}

/* ---- shared serve machinery (over the resolved image blob) ---------------- */

static cf_err rp_fullpath(cf_ctx *ctx, cf_builder *out) {
    cf_err rc = cf_builder_append(out, ctx->request->path);
    if (rc == CF_OK && ctx->request->query.len != 0) {
        rc = cf_builder_append(out, rp_span("?"));
    }
    if (rc == CF_OK && ctx->request->query.len != 0) {
        rc = cf_builder_append(out, ctx->request->query);
    }
    return rc;
}

static bool rp_turbo_frame_request(const cf_request *request) {
    cf_span value = {NULL, 0};
    if (!rp_header(request, "turbo-frame", &value)) return false;
    bool blank = true;
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (c != ' ' && c != '\t') blank = false;
    }
    return !blank;
}

static cf_err rp_append_flash_part(cf_builder *out, cf_span key,
                                   cf_span value) {
    cf_err rc = cf_builder_append(out, key);
    if (rc == CF_OK) rc = cf_builder_append(out, rp_span("=Some(\""));
    for (size_t i = 0; rc == CF_OK && i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (c == '"') {
            rc = cf_builder_append(out, rp_span("\\\""));
        } else if (c == '\\') {
            rc = cf_builder_append(out, rp_span("\\\\"));
        } else if (c == '\n') {
            rc = cf_builder_append(out, rp_span("\\n"));
        } else if (c == '\r') {
            rc = cf_builder_append(out, rp_span("\\r"));
        } else if (c == '\t') {
            rc = cf_builder_append(out, rp_span("\\t"));
        } else if (c < 0x20 || c == 0x7F) {
            char esc[16];
            snprintf(esc, sizeof esc, "\\u{%x}", (unsigned)c);
            rc = cf_builder_append(out, rp_span(esc));
        } else {
            rc = cf_builder_append(out, (cf_span){&c, 1});
        }
    }
    if (rc == CF_OK) rc = cf_builder_append(out, rp_span("\")"));
    return rc;
}

static cf_err rp_forever_etag(cf_ctx *ctx, cf_str *out) {
    memset(out, 0, sizeof *out);
    cf_builder parts = {0};
    cf_err rc = rp_fullpath(ctx, &parts);
    if (rc == CF_OK && rp_turbo_frame_request(ctx->request)) {
        rc = cf_builder_append(&parts, rp_span("/frame"));
    }
    for (size_t i = 0; rc == CF_OK; i++) {
        cf_span key, value;
        if (!cf_ctx_flash_at(ctx, i, &key, &value)) break;
        rc = cf_builder_append(&parts, i == 0 ? rp_span("/") : rp_span("&"));
        if (rc == CF_OK) rc = rp_append_flash_part(&parts, key, value);
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

static bool rp_request_fresh(const cf_request *request, cf_span etag) {
    cf_span inm = {NULL, 0};
    if (!rp_header(request, "if-none-match", &inm)) return false;
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

/* The 302 to the image's disk URL (shared by routes 172 and 174). */
static cf_err rp_answer_redirect(cf_ctx *ctx, const cf_blob *image,
                                 cf_span disp_param, bool has_disp) {
    cf_span raw_ct = {NULL, 0};
    if (image->content_type.present) {
        raw_ct = rp_str_span(image->content_type.value);
    }
    cf_span serving = cf_active_content_type_for_serving(raw_ct);
    const char *forced = cf_active_forced_disposition(raw_ct);
    cf_span kind;
    if (forced != NULL) {
        kind = rp_span(forced);
    } else if (has_disp) {
        kind = disp_param;
    } else {
        kind = rp_span("inline");
    }
    cf_builder sanitized = {0};
    cf_err rc =
        cf_active_filename_sanitize(rp_str_span(image->filename), &sanitized);
    cf_builder path = {0};
    if (rc == CF_OK) {
        rc = cf_active_service_url_path(
            rp_secret(ctx), rp_str_span(image->key),
            cf_now_us(ctx->app) + RP_EXPIRE_IN_US,
            (cf_span){sanitized.ptr, sanitized.len}, serving,
            image->content_type.present, kind, rp_span(RP_SERVICE_NAME),
            &path);
    }
    cf_builder_dispose(&sanitized);
    const cf_config *config = cf_app_config(ctx->app);
    if (rc == CF_OK && (config == NULL || config->public_origin == NULL)) {
        rc = CF_INTERNAL;
    }
    cf_builder location = {0};
    if (rc == CF_OK) {
        rc = cf_builder_append(&location, rp_span(config->public_origin));
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(&location, (cf_span){path.ptr, path.len});
    }
    cf_builder_dispose(&path);
    if (rc == CF_OK) {
        rc = rp_redirect_to(ctx, (cf_span){location.ptr, location.len});
    }
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, rp_span("Cache-Control"),
                                rp_span("max-age=300, private"));
    }
    cf_builder_dispose(&location);
    return rc;
}

/* The image stream (route 173): `http_cache_forever` (304 when fresh),
 * else `send_blob_stream` over the image (no byte ranges on this
 * controller). */
static cf_err rp_answer_proxy(cf_ctx *ctx, const cf_blob *image,
                              cf_span disp_param, bool has_disp) {
    cf_span raw_ct = {NULL, 0};
    if (image->content_type.present) {
        raw_ct = rp_str_span(image->content_type.value);
    }
    cf_builder sanitized = {0};
    cf_err rc =
        cf_active_filename_sanitize(rp_str_span(image->filename), &sanitized);
    if (rc != CF_OK) {
        cf_builder_dispose(&sanitized);
        return rc;
    }
    cf_str etag = {0};
    rc = rp_forever_etag(ctx, &etag);
    if (rc != CF_OK) {
        cf_str_dispose(&etag);
        cf_builder_dispose(&sanitized);
        return rc;
    }
    if (rp_request_fresh(ctx->request, rp_str_span(etag))) {
        ctx->response->status = 304;
        rc = cf_response_header(ctx->response, rp_span("ETag"),
                                rp_str_span(etag));
        if (rc == CF_OK) {
            rc = cf_response_header(ctx->response, rp_span("Last-Modified"),
                                    rp_span(RP_FOREVER_LAST_MODIFIED));
        }
        if (rc == CF_OK) {
            rc = cf_response_header(ctx->response, rp_span("Cache-Control"),
                                    rp_span(RP_FOREVER_CACHE_CONTROL));
        }
        cf_str_dispose(&etag);
        cf_builder_dispose(&sanitized);
        return rc;
    }

    cf_storage *storage = NULL;
    rc = rp_storage_open(ctx, &storage);
    int fd = -1;
    uint64_t file_size = 0;
    if (rc == CF_OK) {
        rc = cf_storage_open_read(storage, rp_str_span(image->key), &fd,
                                  &file_size);
        if (rc == CF_NOT_FOUND) {
            if (storage != NULL) cf_storage_close(storage);
            storage = NULL;
            ctx->response->status = 404;
            rc = cf_response_header(ctx->response, rp_span("Cache-Control"),
                                    rp_span("no-cache"));
            if (rc == CF_OK) rc = rp_head(ctx, 404);
            cf_str_dispose(&etag);
            cf_builder_dispose(&sanitized);
            return rc;
        }
    }
    if (rc != CF_OK) {
        if (fd >= 0) close(fd);
        if (storage != NULL) cf_storage_close(storage);
        cf_str_dispose(&etag);
        cf_builder_dispose(&sanitized);
        return rc;
    }

    cf_active_serve plan = {0};
    rc = cf_active_proxy_serve(true, false, (cf_span){NULL, 0}, file_size,
                               raw_ct, (cf_span){sanitized.ptr,
                                                 sanitized.len},
                               disp_param, has_disp, (cf_span){NULL, 0},
                               false, &plan);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, rp_span("Content-Type"),
                                rp_str_span(plan.content_type));
    }
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, rp_span("Content-Disposition"),
                                rp_str_span(plan.content_disposition));
    }
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response,
                                rp_span("content-transfer-encoding"),
                                rp_span("binary"));
    }
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, rp_span("ETag"),
                                rp_str_span(etag));
    }
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, rp_span("Last-Modified"),
                                rp_span(RP_FOREVER_LAST_MODIFIED));
    }
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, rp_span("Cache-Control"),
                                rp_span(RP_FOREVER_CACHE_CONTROL));
    }
    if (rc == CF_OK) {
        /* No Accept-Ranges on this controller (the reference sets it only
         * on the blobs proxy). */
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
    return rc;
}

/* Verify both signed parameters before processing or serving the image. */
static cf_err rp_resolve_image_request(cf_ctx *ctx, cf_blob *image_out,
                                       cf_span *disp_param_out,
                                       bool *has_disp_out) {
    cf_blob blob = {0};
    bool halted = false;
    cf_err rc = rp_resolve_blob(ctx, &blob, &halted);
    if (rc != CF_OK || halted) {
        cf_blob_dispose(&blob);
        return rc;
    }
    cf_span key = {NULL, 0};
    (void)rp_param_str(ctx, "variation_key", &key);
    cf_active_ventries variation = {0};
    bool decoded = false;
    rc = cf_active_variation_verify(rp_secret(ctx), key,
                                    cf_now_us(ctx->app), &variation, &decoded);
    if (rc != CF_OK) {
        /* A verified key with a non-object shape raises internally. */
        cf_active_ventries_dispose(&variation);
        cf_blob_dispose(&blob);
        return rc;
    }
    if (!decoded) {
        cf_active_ventries_dispose(&variation);
        cf_blob_dispose(&blob);
        return rp_head(ctx, 404);
    }
    memset(image_out, 0, sizeof *image_out);
    rc = cf_active_processed_representation(ctx, &blob, &variation, image_out);
    cf_active_ventries_dispose(&variation);
    cf_blob_dispose(&blob);
    if (rc != CF_OK) {
        cf_blob_dispose(image_out);
        memset(image_out, 0, sizeof *image_out);
        return rc;
    }
    *has_disp_out = rp_param_str(ctx, "disposition", disp_param_out);
    return CF_OK;
}

/* ---- actions -------------------------------------------------------------- */

/* `Representations::RedirectController#show` (routes 172, 174). */
cf_err cf_action_active_storage_representations_redirect_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc = cf_check_csrf(ctx, false);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_blob image = {0};
    cf_span disp_param = {NULL, 0};
    bool has_disp = false;
    rc = rp_resolve_image_request(ctx, &image, &disp_param, &has_disp);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_blob_dispose(&image);
        return rc;
    }
    rc = rp_answer_redirect(ctx, &image, disp_param, has_disp);
    cf_blob_dispose(&image);
    return rc;
}

/* `Representations::ProxyController#show` (route 173). */
cf_err cf_action_active_storage_representations_proxy_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc = cf_check_csrf(ctx, false);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_blob image = {0};
    cf_span disp_param = {NULL, 0};
    bool has_disp = false;
    rc = rp_resolve_image_request(ctx, &image, &disp_param, &has_disp);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_blob_dispose(&image);
        return rc;
    }
    rc = rp_answer_proxy(ctx, &image, disp_param, has_disp);
    cf_blob_dispose(&image);
    return rc;
}
