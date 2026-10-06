/* src/models/active_storage.h — frozen model header (D01; declarations only).
 *
 * Source: tmp/rust-ref/crates/db/src/models/active_storage.rs
 * Tables: active_storage_blobs, active_storage_attachments (schema.sql).
 *         active_storage_variant_records has no struct/function in the
 *         reference models; variants belong to the storage packets (S02/S03).
 *
 * Naming: this module has two reference impl blocks (Blob, Attachment) whose
 * associated functions would collide under a single module prefix; D01 uses
 * the record name as the prefix (cf_blob_*, cf_attachment_*), as it does for
 * enum helpers with colliding type impls.
 */
#ifndef CF_MODELS_ACTIVE_STORAGE_H
#define CF_MODELS_ACTIVE_STORAGE_H

#include "types.h"

/* active_storage_blobs row. */
typedef struct cf_blob {
    int64_t id;
    cf_str key;
    cf_str filename;
    cf_optional_str content_type;
    cf_optional_str metadata; /* JSON column raw text */
    cf_str service_name;
    int64_t byte_size;
    cf_optional_str checksum;
    int64_t created_at; /* UTC microseconds */
} cf_blob;

void cf_blob_dispose(cf_blob *blob);

typedef struct cf_blob_vector {
    cf_blob *items;
    size_t len, cap;
} cf_blob_vector;

void cf_blob_vector_dispose(cf_blob_vector *vector);

/* active_storage_attachments row. */
typedef struct cf_attachment {
    int64_t id;
    cf_str name;
    cf_str record_type;
    int64_t record_id;
    int64_t blob_id;
    int64_t created_at; /* UTC microseconds */
} cf_attachment;

void cf_attachment_dispose(cf_attachment *attachment);

typedef struct cf_attachment_vector {
    cf_attachment *items;
    size_t len, cap;
} cf_attachment_vector;

void cf_attachment_vector_dispose(cf_attachment_vector *vector);

/* Rust: Blob::find — CF_NOT_FOUND when absent. */
cf_err cf_blob_find(cf_db *db, int64_t id, cf_blob *out);
/* Rust: Blob::create — blob.id/created_at are assigned, other fields copied. */
cf_err cf_blob_create(cf_tx *tx, const cf_blob *blob, cf_blob *out);
/* storage/src/blob.rs Blob::update_metadata (`update!(metadata:)`): writes the
 * JSON text for the row (the C API takes the id; callers do not rely on a
 * mutated record). */
cf_err cf_blob_update_metadata(cf_tx *tx, int64_t blob_id, cf_str metadata);
/* storage/src/blob.rs blob::attachment_records: the (record_type, record_id)
 * of every attachment of a blob, ordered by attachment id. Only those fields
 * are filled; dispose with cf_attachment_vector_dispose. */
cf_err cf_attachment_records_for_blob(cf_db *db, int64_t blob_id, cf_attachment_vector *out);
/* Rust: Attachment::find_for */
cf_err cf_attachment_find_for(cf_db *db, cf_str record_type, int64_t record_id, cf_str name, bool *found, cf_attachment *out);
/* Rust: Attachment::create */
cf_err cf_attachment_create(cf_tx *tx, cf_str record_type, int64_t record_id, cf_str name, int64_t blob_id,
                            cf_attachment *out);
/* Rust: Attachment::delete */
cf_err cf_attachment_delete(cf_tx *tx, const cf_attachment *attachment);
/* Rust: Attachment::blob */
cf_err cf_attachment_blob(cf_db *db, const cf_attachment *attachment, cf_blob *out);

/* Every model-functions.json symbol for this module is prototyped above. */

#endif /* CF_MODELS_ACTIVE_STORAGE_H */
