/* src/models/rich_text_record.h — frozen model header (D01; declarations only).
 *
 * Source: tmp/rust-ref/crates/db/src/models/rich_text_record.rs
 * Tables: action_text_rich_texts (schema.sql); Campfire stores Message#body
 *         here (record_type "Message", name "body").
 */
#ifndef CF_MODELS_RICH_TEXT_RECORD_H
#define CF_MODELS_RICH_TEXT_RECORD_H

#include "types.h"

/* action_text_rich_texts row. */
typedef struct cf_rich_text_record {
    int64_t id;
    cf_str name;
    cf_optional_str body;
    cf_str record_type;
    int64_t record_id;
    int64_t created_at; /* UTC microseconds */
    int64_t updated_at; /* UTC microseconds */
} cf_rich_text_record;

void cf_rich_text_record_dispose(cf_rich_text_record *record);

typedef struct cf_rich_text_record_vector {
    cf_rich_text_record *items;
    size_t len, cap;
} cf_rich_text_record_vector;

void cf_rich_text_record_vector_dispose(cf_rich_text_record_vector *vector);

/* Rust: RichTextRecord::find_for */
cf_err cf_rich_text_record_find_for(cf_db *db, cf_str record_type, int64_t record_id, cf_str name, bool *found,
                                    cf_rich_text_record *out);
/* Rust: RichTextRecord::create */
cf_err cf_rich_text_record_create(cf_tx *tx, cf_str record_type, int64_t record_id, cf_str name, cf_str body,
                                  cf_rich_text_record *out);
/* Rust: RichTextRecord::update_body */
cf_err cf_rich_text_record_update_body(cf_tx *tx, cf_rich_text_record *record, cf_str body);
/* Rust: RichTextRecord::delete */
cf_err cf_rich_text_record_delete(cf_tx *tx, const cf_rich_text_record *record);

/* Every model-functions.json symbol for this module is prototyped above. */

#endif /* CF_MODELS_RICH_TEXT_RECORD_H */
