/* src/models/active_storage.c — model functions of the pinned
 * tmp/rust-ref/crates/db/src/models/active_storage.rs (Blob, Attachment).
 *
 * Tables: active_storage_blobs, active_storage_attachments (schema.sql).
 * active_storage_variant_records has no struct or function in the reference
 * models (header comment); variant rows belong to the storage packets.
 *
 * Translation notes:
 *  - Reads take cf_db* and use the per-connection statement cache; mutations
 *    take cf_tx* and obtain the writer connection with cf_tx_db (D02).
 *  - Rust `tx.now()` is `Tx::now() -> Env::now()` (database.rs), i.e. the
 *    process/shared clock.  The frozen api.h has no tx clock accessor and D02's
 *    landed writer.c cf_tx carries only the connection (plus its event list),
 *    so the C model convention is the injectable process clock cf_now_us
 *    (core/clock.c), which tests time-travel with cf_test_clock_set_fixed_us;
 *    account/ban/message/search/session/push_subscription translate it the
 *    same way.
 *  - Datetime columns use the reference representation: UTC SQL text on the
 *    wire (cf_db_time_to_text/from_text), int64 UTC microseconds in memory.
 *  - `SELECT *` is kept from the reference and the column indexes below are
 *    the schema.sql declaration order the frozen DDL guarantees.
 *  - No function here emits events: the six reference functions only touch
 *    rows.  PURGE_BLOB events belong to message.rs / the storage packets.
 */
#include "models/active_storage.h"

#include "db/db_internal.h"

#include <stdlib.h>
#include <string.h>

/* --- fixed statement set (one per reference query) ------------------------ */

enum {
    CF_AS_STMT_BLOB_FIND = 0,
    CF_AS_STMT_BLOB_CREATE,
    CF_AS_STMT_ATTACHMENT_FIND_FOR,
    CF_AS_STMT_ATTACHMENT_CREATE,
    CF_AS_STMT_ATTACHMENT_DELETE,
    CF_AS_STMT_COUNT
};

static const cf_stmt_def cf_as_stmt_defs[CF_AS_STMT_COUNT] = {
    {"SELECT * FROM \"active_storage_blobs\" WHERE "
     "\"active_storage_blobs\".\"id\" = ? LIMIT 1"},
    {"INSERT INTO \"active_storage_blobs\" (\"byte_size\", \"checksum\", "
     "\"content_type\", \"created_at\", \"filename\", \"key\", \"metadata\", "
     "\"service_name\") VALUES (?, ?, ?, ?, ?, ?, ?, ?) RETURNING \"id\""},
    {"SELECT * FROM \"active_storage_attachments\" WHERE "
     "\"active_storage_attachments\".\"record_id\" = ? AND "
     "\"active_storage_attachments\".\"record_type\" = ? AND "
     "\"active_storage_attachments\".\"name\" = ? LIMIT 1"},
    {"INSERT INTO \"active_storage_attachments\" (\"blob_id\", "
     "\"created_at\", \"name\", \"record_id\", \"record_type\") "
     "VALUES (?, ?, ?, ?, ?) RETURNING \"id\""},
    {"DELETE FROM \"active_storage_attachments\" WHERE "
     "\"active_storage_attachments\".\"id\" = ?"},
};

static const cf_stmt_set cf_as_stmts = {cf_as_stmt_defs, CF_AS_STMT_COUNT};

/* SELECT * column positions, from contracts/schema.sql:
 *   active_storage_blobs:
 *     0 id, 1 byte_size, 2 checksum, 3 content_type, 4 created_at,
 *     5 filename, 6 key, 7 metadata, 8 service_name
 *   active_storage_attachments:
 *     0 id, 1 blob_id, 2 created_at, 3 name, 4 record_id, 5 record_type */
enum {
    CF_AS_BLOB_COL_ID = 0,
    CF_AS_BLOB_COL_BYTE_SIZE = 1,
    CF_AS_BLOB_COL_CHECKSUM = 2,
    CF_AS_BLOB_COL_CONTENT_TYPE = 3,
    CF_AS_BLOB_COL_CREATED_AT = 4,
    CF_AS_BLOB_COL_FILENAME = 5,
    CF_AS_BLOB_COL_KEY = 6,
    CF_AS_BLOB_COL_METADATA = 7,
    CF_AS_BLOB_COL_SERVICE_NAME = 8
};

enum {
    CF_AS_ATTACHMENT_COL_ID = 0,
    CF_AS_ATTACHMENT_COL_BLOB_ID = 1,
    CF_AS_ATTACHMENT_COL_CREATED_AT = 2,
    CF_AS_ATTACHMENT_COL_NAME = 3,
    CF_AS_ATTACHMENT_COL_RECORD_ID = 4,
    CF_AS_ATTACHMENT_COL_RECORD_TYPE = 5
};

/* --- owned-text helpers ---------------------------------------------------- */

static cf_span cf_str_span(cf_str text) {
    cf_span span = {(const unsigned char *)text.ptr, text.len};
    return span;
}

/* Owned NUL-terminated copy of borrowed bytes.  The callee keeps ownership;
 * callers release the result with cf_str_dispose (frozen types.h rule). */
static cf_err cf_as_copy_text(cf_str *out, cf_span bytes) {
    char *copy = malloc(bytes.len + 1);
    if (copy == NULL) return cf_db_failf(CF_NOMEM, "out of memory");
    if (bytes.len != 0) memcpy(copy, bytes.ptr, bytes.len);
    copy[bytes.len] = '\0';
    out->ptr = copy;
    out->len = bytes.len;
    return CF_OK;
}

static cf_err cf_as_copy_optional(cf_optional_str *out, sqlite3_stmt *stmt,
                                  int column) {
    out->present = false;
    out->value.ptr = NULL;
    out->value.len = 0;
    if (cf_stmt_column_is_null(stmt, column)) return CF_OK;
    out->present = true;
    return cf_as_copy_text(&out->value, cf_stmt_column_text(stmt, column));
}

static cf_err cf_as_clone_optional(cf_optional_str *out,
                                   cf_optional_str source) {
    out->present = false;
    out->value.ptr = NULL;
    out->value.len = 0;
    if (!source.present) return CF_OK;
    out->present = true;
    return cf_as_copy_text(&out->value, cf_str_span(source.value));
}

/* UTC microseconds from a datetime(6) text column. */
static cf_err cf_as_read_time(sqlite3_stmt *stmt, int column, int64_t *out_us) {
    *out_us = 0;
    if (cf_stmt_column_is_null(stmt, column)) {
        return cf_db_failf(CF_INVALID, "datetime column is NULL");
    }
    return cf_db_time_from_text(cf_stmt_column_text(stmt, column), out_us);
}

/* The reference writes `now` with Timestamp::to_db before binding it. */
static cf_err cf_as_bind_time(sqlite3_stmt *stmt, int index, int64_t us) {
    char text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(us, text);
    if (rc != CF_OK) return rc;
    cf_span span = {(const unsigned char *)text, strlen(text)};
    return cf_stmt_bind_text(stmt, index, span);
}

/* --- Blob::from_row ------------------------------------------------------- */

static cf_err cf_as_blob_from_row(sqlite3_stmt *stmt, cf_blob *out) {
    cf_blob blob;
    memset(&blob, 0, sizeof blob);
    blob.id = cf_stmt_column_i64(stmt, CF_AS_BLOB_COL_ID);
    blob.byte_size = cf_stmt_column_i64(stmt, CF_AS_BLOB_COL_BYTE_SIZE);
    cf_err rc = cf_as_copy_optional(&blob.checksum, stmt,
                                    CF_AS_BLOB_COL_CHECKSUM);
    if (rc == CF_OK) {
        rc = cf_as_copy_optional(&blob.content_type, stmt,
                                 CF_AS_BLOB_COL_CONTENT_TYPE);
    }
    if (rc == CF_OK) {
        rc = cf_as_read_time(stmt, CF_AS_BLOB_COL_CREATED_AT,
                             &blob.created_at);
    }
    if (rc == CF_OK) {
        rc = cf_as_copy_text(&blob.filename,
                             cf_stmt_column_text(stmt, CF_AS_BLOB_COL_FILENAME));
    }
    if (rc == CF_OK) {
        rc = cf_as_copy_text(&blob.key,
                             cf_stmt_column_text(stmt, CF_AS_BLOB_COL_KEY));
    }
    if (rc == CF_OK) {
        rc = cf_as_copy_optional(&blob.metadata, stmt,
                                 CF_AS_BLOB_COL_METADATA);
    }
    if (rc == CF_OK) {
        rc = cf_as_copy_text(
            &blob.service_name,
            cf_stmt_column_text(stmt, CF_AS_BLOB_COL_SERVICE_NAME));
    }
    if (rc != CF_OK) {
        cf_blob_dispose(&blob);
        return rc;
    }
    *out = blob;
    return CF_OK;
}

/* --- Attachment::from_row -------------------------------------------------- */

static cf_err cf_as_attachment_from_row(sqlite3_stmt *stmt,
                                        cf_attachment *out) {
    cf_attachment attachment;
    memset(&attachment, 0, sizeof attachment);
    attachment.id = cf_stmt_column_i64(stmt, CF_AS_ATTACHMENT_COL_ID);
    attachment.blob_id = cf_stmt_column_i64(stmt, CF_AS_ATTACHMENT_COL_BLOB_ID);
    cf_err rc = cf_as_read_time(stmt, CF_AS_ATTACHMENT_COL_CREATED_AT,
                                &attachment.created_at);
    if (rc == CF_OK) {
        rc = cf_as_copy_text(
            &attachment.name,
            cf_stmt_column_text(stmt, CF_AS_ATTACHMENT_COL_NAME));
    }
    if (rc == CF_OK) {
        attachment.record_id =
            cf_stmt_column_i64(stmt, CF_AS_ATTACHMENT_COL_RECORD_ID);
        rc = cf_as_copy_text(
            &attachment.record_type,
            cf_stmt_column_text(stmt, CF_AS_ATTACHMENT_COL_RECORD_TYPE));
    }
    if (rc != CF_OK) {
        cf_attachment_dispose(&attachment);
        return rc;
    }
    *out = attachment;
    return CF_OK;
}

/* --- statement execution --------------------------------------------------- */

static cf_err cf_as_step_error(cf_db *db, int rc, const char *what) {
    return cf_db_failf(cf_db_err(rc), "%s: %s", what,
                       sqlite3_errmsg(cf_db_handle(db)));
}

/* --- Blob::find ------------------------------------------------------------ */

cf_err cf_blob_find(cf_db *db, int64_t id, cf_blob *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output record");
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_as_stmts, CF_AS_STMT_BLOB_FIND, &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_i64(stmt, 1, id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            rc = cf_as_blob_from_row(stmt, out);
        } else if (step == SQLITE_DONE) {
            /* Error::RecordNotFound display:
             * "Couldn't find ActiveStorage::Blob" (active_storage.rs:42). */
            rc = cf_db_failf(CF_NOT_FOUND,
                             "Couldn't find ActiveStorage::Blob");
        } else {
            rc = cf_as_step_error(db, step, "blob find failed");
        }
    }
    cf_db_stmt_done(stmt);
    return rc;
}

/* --- Blob::create ---------------------------------------------------------- */

cf_err cf_blob_create(cf_tx *tx, const cf_blob *blob, cf_blob *out) {
    if (tx == NULL || blob == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "invalid blob create argument");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INTERNAL, "no transaction database");

    int64_t now = cf_now_us(NULL);
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_as_stmts, CF_AS_STMT_BLOB_CREATE, &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_i64(stmt, 1, blob->byte_size);
    if (rc == CF_OK) {
        rc = cf_stmt_bind_opt_text(stmt, 2, blob->checksum.present,
                                   cf_str_span(blob->checksum.value));
    }
    if (rc == CF_OK) {
        rc = cf_stmt_bind_opt_text(stmt, 3, blob->content_type.present,
                                   cf_str_span(blob->content_type.value));
    }
    if (rc == CF_OK) rc = cf_as_bind_time(stmt, 4, now);
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 5, cf_str_span(blob->filename));
    }
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 6, cf_str_span(blob->key));
    if (rc == CF_OK) {
        rc = cf_stmt_bind_opt_text(stmt, 7, blob->metadata.present,
                                   cf_str_span(blob->metadata.value));
    }
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 8, cf_str_span(blob->service_name));
    }
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    int64_t id = 0;
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        id = cf_stmt_column_i64(stmt, 0);
        rc = CF_OK;
    } else if (step == SQLITE_DONE) {
        rc = cf_db_failf(CF_DB, "blob insert returned no id");
    } else {
        rc = cf_as_step_error(db, step, "blob insert failed");
    }
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) return rc;

    /* Result is the inserted row: id/created_at assigned, other fields copied
     * from the borrowed input (Rust `Self { id, created_at: now, ..blob }`). */
    cf_blob created;
    memset(&created, 0, sizeof created);
    created.id = id;
    created.created_at = now;
    created.byte_size = blob->byte_size;
    rc = cf_as_copy_text(&created.key, cf_str_span(blob->key));
    if (rc == CF_OK) {
        rc = cf_as_copy_text(&created.filename, cf_str_span(blob->filename));
    }
    if (rc == CF_OK) {
        rc = cf_as_clone_optional(&created.content_type, blob->content_type);
    }
    if (rc == CF_OK) {
        rc = cf_as_clone_optional(&created.metadata, blob->metadata);
    }
    if (rc == CF_OK) {
        rc = cf_as_copy_text(&created.service_name,
                             cf_str_span(blob->service_name));
    }
    if (rc == CF_OK) {
        rc = cf_as_clone_optional(&created.checksum, blob->checksum);
    }
    if (rc != CF_OK) {
        cf_blob_dispose(&created);
        return rc;
    }
    *out = created;
    return CF_OK;
}

/* --- Attachment::find_for --------------------------------------------------- */

cf_err cf_attachment_find_for(cf_db *db, cf_str record_type, int64_t record_id,
                              cf_str name, bool *found, cf_attachment *out) {
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "no output argument");
    }
    *found = false;
    sqlite3_stmt *stmt = NULL;
    cf_err rc =
        cf_db_stmt(db, &cf_as_stmts, CF_AS_STMT_ATTACHMENT_FIND_FOR, &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_i64(stmt, 1, record_id);
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 2, cf_str_span(record_type));
    }
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 3, cf_str_span(name));
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            rc = cf_as_attachment_from_row(stmt, out);
            if (rc == CF_OK) *found = true;
        } else if (step == SQLITE_DONE) {
            rc = CF_OK; /* Option::None: found stays false, out stays empty */
        } else {
            rc = cf_as_step_error(db, step, "attachment find_for failed");
        }
    }
    cf_db_stmt_done(stmt);
    return rc;
}

/* --- Attachment::create ----------------------------------------------------- */

cf_err cf_attachment_create(cf_tx *tx, cf_str record_type, int64_t record_id,
                            cf_str name, int64_t blob_id, cf_attachment *out) {
    if (tx == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "invalid attachment create argument");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INTERNAL, "no transaction database");

    int64_t now = cf_now_us(NULL);
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_as_stmts, CF_AS_STMT_ATTACHMENT_CREATE, &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_i64(stmt, 1, blob_id);
    if (rc == CF_OK) rc = cf_as_bind_time(stmt, 2, now);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 3, cf_str_span(name));
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 4, record_id);
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 5, cf_str_span(record_type));
    }
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    int64_t id = 0;
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        id = cf_stmt_column_i64(stmt, 0);
        rc = CF_OK;
    } else if (step == SQLITE_DONE) {
        rc = cf_db_failf(CF_DB, "attachment insert returned no id");
    } else {
        rc = cf_as_step_error(db, step, "attachment insert failed");
    }
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) return rc;

    cf_attachment created;
    memset(&created, 0, sizeof created);
    created.id = id;
    created.record_id = record_id;
    created.blob_id = blob_id;
    created.created_at = now;
    rc = cf_as_copy_text(&created.name, cf_str_span(name));
    if (rc == CF_OK) {
        rc = cf_as_copy_text(&created.record_type, cf_str_span(record_type));
    }
    if (rc != CF_OK) {
        cf_attachment_dispose(&created);
        return rc;
    }
    *out = created;
    return CF_OK;
}

/* --- Attachment::delete ----------------------------------------------------- */

cf_err cf_attachment_delete(cf_tx *tx, const cf_attachment *attachment) {
    if (tx == NULL || attachment == NULL) {
        return cf_db_failf(CF_INVALID, "invalid attachment delete argument");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INTERNAL, "no transaction database");

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_as_stmts, CF_AS_STMT_ATTACHMENT_DELETE, &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_i64(stmt, 1, attachment->id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) {
            rc = CF_OK; /* execute: 0 affected rows is still success */
        } else if (step == SQLITE_ROW) {
            rc = cf_db_failf(CF_INTERNAL, "attachment delete returned a row");
        } else {
            rc = cf_as_step_error(db, step, "attachment delete failed");
        }
    }
    cf_db_stmt_done(stmt);
    return rc;
}

/* --- Attachment::blob ------------------------------------------------------- */

cf_err cf_attachment_blob(cf_db *db, const cf_attachment *attachment,
                          cf_blob *out) {
    if (attachment == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "invalid attachment blob argument");
    }
    return cf_blob_find(db, attachment->blob_id, out);
}

/* --- disposal --------------------------------------------------------------- */

void cf_blob_dispose(cf_blob *blob) {
    if (blob == NULL) return;
    cf_str_dispose(&blob->key);
    cf_str_dispose(&blob->filename);
    cf_optional_str_dispose(&blob->content_type);
    cf_optional_str_dispose(&blob->metadata);
    cf_str_dispose(&blob->service_name);
    cf_optional_str_dispose(&blob->checksum);
    blob->id = 0;
    blob->byte_size = 0;
    blob->created_at = 0;
}

void cf_blob_vector_dispose(cf_blob_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_blob_dispose(&vector->items[i]);
    }
    free(vector->items);
    vector->items = NULL;
    vector->len = 0;
    vector->cap = 0;
}

void cf_attachment_dispose(cf_attachment *attachment) {
    if (attachment == NULL) return;
    cf_str_dispose(&attachment->name);
    cf_str_dispose(&attachment->record_type);
    attachment->id = 0;
    attachment->record_id = 0;
    attachment->blob_id = 0;
    attachment->created_at = 0;
}

void cf_attachment_vector_dispose(cf_attachment_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_attachment_dispose(&vector->items[i]);
    }
    free(vector->items);
    vector->items = NULL;
    vector->len = 0;
    vector->cap = 0;
}
