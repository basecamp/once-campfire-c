/* Shared User avatar upload boundary. File I/O runs before cf_write; only
 * attachment/blob rows, record touches and events run on the writer. */
#ifndef CF_ACTIONS_AVATAR_UPLOAD_H
#define CF_ACTIONS_AVATAR_UPLOAD_H
#include "app.h"
#include "config.h"
#include "context.h"
#include "http/params.h"
#include "models/active_storage.h"
#include "models/touch.h"
#include "storage/active_storage.h"
#include "storage/storage.h"
#include <stdio.h>

static inline cf_err cf_avatar_stage(cf_ctx *ctx, const cf_param *param,
                                    cf_storage **storage,
                                    cf_active_staged *staged) {
    *storage = NULL;
    cf_upload upload = {0};
    cf_err rc = cf_param_upload(param, &upload);
    if (rc != CF_OK) return rc;
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL) return CF_INTERNAL;
    rc = cf_storage_open(config->storage_path, storage);
    if (rc == CF_OK)
        rc = cf_active_stage_upload(*storage, upload.fd, upload.filename,
                                    upload.content_type,
                                    upload.has_content_type, staged);
    return rc;
}

static inline cf_err cf_avatar_attach(cf_tx *tx, cf_user *user,
                                     const cf_active_staged *staged,
                                     int64_t *blob_id) {
    if (staged == NULL) return CF_INTERNAL;
    cf_str record = {(char *)"User", 4}, name = {(char *)"avatar", 6};
    bool found = false;
    cf_attachment old = {0};
    cf_err rc = cf_attachment_find_for(cf_tx_db(tx), record, user->id,
                                       name, &found, &old);
    if (rc == CF_OK && found) rc = cf_attachment_delete(tx, &old);
    if (rc == CF_OK && found)
        rc = cf_tx_event(tx, (cf_event){.kind = CF_EVENT_PURGE_BLOB,
                                        .blob_id = old.blob_id});
    cf_attachment_dispose(&old);
    cf_blob input = {0}, blob = {0};
    input.key = staged->key;
    input.filename = staged->filename;
    input.content_type = staged->content_type;
    input.metadata = (cf_optional_str){true, staged->metadata};
    input.service_name = staged->service_name;
    input.byte_size = staged->byte_size;
    input.checksum = (cf_optional_str){true, staged->checksum};
    if (rc == CF_OK) rc = cf_blob_create(tx, &input, &blob);
    cf_attachment attachment = {0};
    if (rc == CF_OK)
        rc = cf_attachment_create(tx, record, user->id, name, blob.id,
                                   &attachment);
    if (rc == CF_OK) rc = cf_touch_user(tx, user);
    if (rc == CF_OK) *blob_id = blob.id;
    cf_attachment_dispose(&attachment);
    cf_blob_dispose(&blob);
    return rc;
}
/* Analysis is best effort after commit, like the reference jobs queue. A
 * refusal is recorded rather than changing an already committed response. */
static inline void cf_avatar_analyze_later(cf_ctx *ctx, int64_t blob_id) {
    if (cf_app_enqueue_media(ctx->app, blob_id,
        (cf_span){(const unsigned char *)"analyze", 7}, (cf_span){NULL, 0}) != CF_OK)
        fprintf(stderr, "campfire: dropped avatar analysis job\n");
}
#endif
