/* src/actions/active_storage/direct_uploads.c —
 * ActiveStorage::DirectUploadsController#create (task S02-C; route ID 177;
 * contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/active_storage.rs
 * (`direct_uploads_create`, `direct_upload_json`, `json_time`), over the
 * S02 helpers (src/storage/active_storage.{c,h}: the direct-upload
 * parameter check, the exact JSON body, the created_at rendering, the
 * upload token signing) and the D01 blob insert. Storage semantics stay
 * S02's; this file is the controller layer.
 *
 * Translated in order:
 *   verify_authenticity_token (CSRF only, like the other Active Storage
 *   controllers) -> `require_active_storage_authentication` (401 with no
 *   session, no redirect) -> `params.expect(blob: [...])` (a missing or
 *   non-object `blob` is ParameterMissing, i.e. the C 400) -> the filename
 *   / checksum presence checks and the `byte_size` integer cast (422), the
 *   16 MiB cap (413) -> the blob row insert inside the writer (the P12-01
 *   mutation transaction; a pure insert, so there is nothing to
 *   revalidate) -> the direct-upload token signing (purpose `blob_token`,
 *   5-minute expiry) and the exact `as_json(...).merge(direct_upload:)`
 *   body at 200 (`application/json; charset=utf-8`).
 *
 * Parameter texts (`filename`, `checksum`, `byte_size`, `content_type`)
 * accept strings and numbers (the JavaScript client sends `byte_size` as a
 * number), exactly like the reference's Str/Number match; bools, nulls,
 * arrays, objects and uploads read as absent. `filename` and `checksum`
 * must be non-empty; `content_type` keeps even an empty string when it is
 * a string or number (no presence filter, like the reference).
 *
 * Metadata: absent or non-object metadata stores `{}`. For a JSON request
 * body the object is re-emitted from the raw body with yyjson (verbatim
 * canonical JSON, matching the reference's parse-then-encode round trip
 * for ordinary values). Form-encoded nested metadata has no public
 * object-enumeration accessor (see the integrator request below) and stores
 * `{}`; direct uploads in Campfire arrive as JSON.
 *
 * c_symbol for the integrator's route rebind (src/routes.c row 177):
 *   cf_action_active_storage_direct_uploads_create.
 * The integrator also owns src/actions/actions.h and the Makefile
 * (this file's build entry) plus:
 *   - a `cf_param` object-enumeration accessor (or a param-to-JSON
 *     helper) so form-encoded `blob[metadata]` objects can round-trip
 *     instead of storing `{}`.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "http/params.h"
#include "models/active_storage.h"
#include "models/session.h"
#include "models/types.h"
#include "storage/active_storage.h"
#include "storage/storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#define DU_EXPIRE_IN_US INT64_C(300000000)
#define DU_SERVICE_NAME "local"
#define DU_JSON_CONTENT_TYPE "application/json; charset=utf-8"

static cf_span du_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static cf_span du_str_span(cf_str value) {
    return (cf_span){(const unsigned char *)value.ptr, value.len};
}

static cf_span du_secret(cf_ctx *ctx) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->secret_key_base == NULL) {
        return (cf_span){NULL, 0};
    }
    return (cf_span){(const unsigned char *)config->secret_key_base,
                     config->secret_key_base_len};
}

static cf_err du_head(cf_ctx *ctx, unsigned status) {
    ctx->response->status = status;
    if (status == 204 || status == 304) return CF_OK;
    const cf_format *format = cf_ctx_rendered_format(ctx);
    return cf_response_header(ctx->response, du_span("Content-Type"),
                              du_span(format->string));
}

/* `require_active_storage_authentication`: 401 without a session. */
static cf_err du_require_session(cf_ctx *ctx) {
    cf_span raw = {NULL, 0};
    if (cf_ctx_cookie_get(ctx, du_span("session_token"), &raw) != CF_OK) {
        return du_head(ctx, 401);
    }
    cf_str token = {0};
    bool verified = false;
    cf_err rc = cf_auth_signed_cookie_verify(
        du_secret(ctx), du_span("session_token"), raw,
        cf_now_us(ctx->app), &token, &verified);
    if (rc != CF_OK || !verified) {
        cf_str_dispose(&token);
        if (rc != CF_OK) return rc;
        return du_head(ctx, 401);
    }
    bool found = false;
    cf_session session = {0};
    rc = cf_session_find_by_token(ctx->reader, token, &found, &session);
    cf_str_dispose(&token);
    cf_session_dispose(&session);
    if (rc != CF_OK) return rc;
    if (!found) return du_head(ctx, 401);
    return CF_OK;
}

/* One `blob` field's text: strings as-is, numbers in their JSON source
 * spelling (the reference's Str/Number match); anything else is absent. */
static bool du_field_text(const cf_param *blob, const char *name,
                          cf_span *out) {
    const cf_param *field = cf_param_field(blob, du_span(name));
    if (field == NULL) return false;
    cf_param_kind kind = cf_param_type(field);
    if (kind != CF_PARAM_STRING && kind != CF_PARAM_NUMBER) return false;
    return cf_param_to_s(field, out) == CF_OK;
}

/* The stored metadata JSON: the `blob.metadata` object re-emitted from the
 * raw JSON request body (verbatim canonical JSON), or `{}` when the body
 * is not JSON or the metadata is not an object there. */
static cf_err du_metadata_json(cf_ctx *ctx, const cf_param *blob,
                               cf_builder *out) {
    static const char empty[] = "{}";
    const cf_param *metadata = cf_param_field(blob, du_span("metadata"));
    if (metadata == NULL || cf_param_type(metadata) != CF_PARAM_OBJECT) {
        return cf_builder_append(out, du_span(empty));
    }
    yyjson_doc *doc = NULL;
    if (ctx->request->body.len != 0) {
        /* yyjson takes a mutable buffer; copy the borrowed body. */
        char *copy = malloc(ctx->request->body.len);
        if (copy != NULL) {
            memcpy(copy, ctx->request->body.ptr, ctx->request->body.len);
            doc = yyjson_read_opts(copy, ctx->request->body.len, 0, NULL,
                                   NULL);
            free(copy);
        }
    }
    if (doc == NULL) return cf_builder_append(out, du_span(empty));
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (yyjson_is_obj(root)) {
        yyjson_val *blob_val = yyjson_obj_get(root, "blob");
        if (blob_val != NULL && yyjson_is_obj(blob_val)) {
            yyjson_val *meta_val = yyjson_obj_get(blob_val, "metadata");
            if (meta_val != NULL && yyjson_is_obj(meta_val)) {
                size_t len = 0;
                char *written =
                    yyjson_val_write(meta_val, 0, &len);
                if (written != NULL) {
                    cf_err rc = cf_builder_append(
                        out, (cf_span){(unsigned char *)written, len});
                    free(written);
                    yyjson_doc_free(doc);
                    return rc;
                }
            }
        }
    }
    yyjson_doc_free(doc);
    return cf_builder_append(out, du_span(empty));
}

typedef struct {
    cf_span filename;
    cf_span content_type;
    bool has_content_type;
    cf_span metadata_json;
    cf_span key;
    int64_t byte_size;
    cf_span checksum;
    cf_blob created;
    bool has_created;
} du_write_arg;

/* The P12-01 mutation transaction: a pure blob-row insert (nothing read
 * before it to revalidate). Spans borrow the action's params/builders,
 * which stay alive until `cf_write` returns. */
static cf_err du_write_cb(cf_tx *tx, void *arg) {
    du_write_arg *write = arg;
    cf_blob input;
    memset(&input, 0, sizeof input);
    input.byte_size = write->byte_size;
    input.key = (cf_str){(char *)(uintptr_t)write->key.ptr, write->key.len};
    input.filename = (cf_str){(char *)(uintptr_t)write->filename.ptr,
                              write->filename.len};
    if (write->has_content_type) {
        input.content_type.present = true;
        input.content_type.value =
            (cf_str){(char *)(uintptr_t)write->content_type.ptr,
                     write->content_type.len};
    }
    input.metadata.present = true;
    input.metadata.value =
        (cf_str){(char *)(uintptr_t)write->metadata_json.ptr,
                 write->metadata_json.len};
    input.service_name =
        (cf_str){(char *)DU_SERVICE_NAME, strlen(DU_SERVICE_NAME)};
    input.checksum.present = true;
    input.checksum.value = (cf_str){(char *)(uintptr_t)write->checksum.ptr,
                                    write->checksum.len};
    memset(&write->created, 0, sizeof write->created);
    write->has_created = false;
    cf_err rc = cf_blob_create(tx, &input, &write->created);
    if (rc == CF_OK) write->has_created = true;
    return rc;
}

/* `DirectUploadsController#create` (route 177). */
cf_err cf_action_active_storage_direct_uploads_create(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc = cf_check_csrf(ctx, false);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;
    rc = du_require_session(ctx);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    /* `params.expect(blob: [...])`: missing/non-object is ParameterMissing
     * (the C 400). */
    const cf_param *blob = cf_ctx_param(ctx, du_span("blob"));
    if (blob == NULL || cf_param_type(blob) != CF_PARAM_OBJECT) {
        return CF_INVALID;
    }
    cf_span filename = {NULL, 0};
    cf_span checksum = {NULL, 0};
    cf_span byte_size_text = {NULL, 0};
    bool has_filename = du_field_text(blob, "filename", &filename) &&
                        filename.len != 0;
    bool has_checksum = du_field_text(blob, "checksum", &checksum) &&
                        checksum.len != 0;
    bool has_byte_size =
        du_field_text(blob, "byte_size", &byte_size_text);
    int64_t byte_size = 0;
    cf_active_upload_decision decision = cf_active_direct_upload_check(
        byte_size_text, has_byte_size, has_filename, has_checksum,
        &byte_size);
    if (decision == CF_ACTIVE_UPLOAD_UNPROCESSABLE) {
        cf_ctx_set_error_status(ctx, 422);
        return CF_INVALID;
    }
    if (decision == CF_ACTIVE_UPLOAD_TOO_LARGE) {
        cf_ctx_set_error_status(ctx, 413);
        return CF_INVALID;
    }
    cf_span content_type = {NULL, 0};
    bool has_content_type = du_field_text(blob, "content_type",
                                          &content_type);

    cf_builder key_builder = {0};
    rc = cf_storage_key_generate(&key_builder);
    if (rc != CF_OK) {
        cf_builder_dispose(&key_builder);
        return rc;
    }
    cf_builder metadata_builder = {0};
    rc = du_metadata_json(ctx, blob, &metadata_builder);
    if (rc != CF_OK) {
        cf_builder_dispose(&metadata_builder);
        cf_builder_dispose(&key_builder);
        return rc;
    }

    du_write_arg write;
    memset(&write, 0, sizeof write);
    write.filename = filename;
    write.content_type = content_type;
    write.has_content_type = has_content_type;
    write.metadata_json =
        (cf_span){metadata_builder.ptr, metadata_builder.len};
    write.key = (cf_span){key_builder.ptr, key_builder.len};
    write.byte_size = byte_size;
    write.checksum = checksum;
    rc = cf_write(ctx->app, du_write_cb, &write);
    if (rc != CF_OK || !write.has_created) {
        if (write.has_created) cf_blob_dispose(&write.created);
        cf_builder_dispose(&metadata_builder);
        cf_builder_dispose(&key_builder);
        return rc;
    }

    /* `url_for_direct_upload` on this request's host (PUBLIC_ORIGIN per
     * D-C07), expiring in `service_urls_expire_in`. */
    int64_t expires = cf_now_us(ctx->app) + DU_EXPIRE_IN_US;
    cf_str token = {0};
    rc = cf_active_disk_token_sign(
        du_secret(ctx), (cf_span){key_builder.ptr, key_builder.len},
        content_type, has_content_type, byte_size, checksum,
        du_span(DU_SERVICE_NAME), expires, &token);
    cf_builder url = {0};
    const cf_config *config = cf_app_config(ctx->app);
    if (rc == CF_OK && (config == NULL || config->public_origin == NULL)) {
        rc = CF_INTERNAL;
    }
    if (rc == CF_OK) rc = cf_builder_append(&url, du_span(config->public_origin));
    if (rc == CF_OK) {
        rc = cf_builder_append(&url, du_span("/rails/active_storage/disk/"));
    }
    if (rc == CF_OK) {
        rc = cf_active_escape_segment(du_str_span(token), &url);
    }
    cf_str_dispose(&token);
    cf_str signed_id = {0};
    if (rc == CF_OK) {
        rc = cf_active_blob_sign(du_secret(ctx), write.created.id, false, 0,
                                 &signed_id);
    }
    char created_text[CF_DB_TIME_TEXT_CAP];
    cf_builder created_iso = {0};
    if (rc == CF_OK) rc = cf_db_time_to_text(write.created.created_at,
                                             created_text);
    if (rc == CF_OK) {
        rc = cf_active_json_time(
            (cf_span){(unsigned char *)created_text, strlen(created_text)},
            &created_iso);
    }
    cf_builder body = {0};
    if (rc == CF_OK) {
        cf_span ct_span = {NULL, 0};
        if (has_content_type) ct_span = content_type;
        rc = cf_active_direct_upload_json(
            write.created.id, (cf_span){key_builder.ptr, key_builder.len},
            filename, content_type, has_content_type,
            (cf_span){metadata_builder.ptr, metadata_builder.len},
            du_span(DU_SERVICE_NAME), byte_size, checksum, true,
            (cf_span){created_iso.ptr, created_iso.len},
            du_str_span(signed_id), (cf_span){url.ptr, url.len}, ct_span,
            has_content_type, &body);
    }
    cf_builder_dispose(&created_iso);
    cf_str_dispose(&signed_id);
    cf_builder_dispose(&url);
    cf_blob_dispose(&write.created);
    cf_builder_dispose(&metadata_builder);
    cf_builder_dispose(&key_builder);
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        return rc;
    }
    cf_buf *buf = NULL;
    rc = cf_builder_freeze(&body, &buf);
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        return rc;
    }
    ctx->response->status = 200;
    rc = cf_response_header(ctx->response, du_span("Content-Type"),
                            du_span(DU_JSON_CONTENT_TYPE));
    if (rc == CF_OK) rc = cf_response_body(ctx->response, buf);
    cf_buf_release(buf);
    return rc;
}
