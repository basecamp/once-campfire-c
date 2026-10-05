/* src/models/ban.h — frozen model header (D01; declarations only).
 *
 * Source: tmp/rust-ref/crates/db/src/models/ban.rs
 * Tables: bans (schema.sql).
 */
#ifndef CF_MODELS_BAN_H
#define CF_MODELS_BAN_H

#include "types.h"

/* bans row. */
typedef struct cf_ban {
    int64_t id;
    int64_t user_id;
    cf_str ip_address;
    int64_t created_at; /* UTC microseconds */
    int64_t updated_at; /* UTC microseconds */
} cf_ban;

void cf_ban_dispose(cf_ban *ban);

typedef struct cf_ban_vector {
    cf_ban *items;
    size_t len, cap;
} cf_ban_vector;

void cf_ban_vector_dispose(cf_ban_vector *vector);

/* Rust: Ban::banned */
cf_err cf_ban_banned(cf_db *db, cf_str ip_address, bool *out);
/* Rust: Ban::for_user */
cf_err cf_ban_for_user(cf_db *db, int64_t user_id, cf_ban_vector *out);
/* Rust: Ban::create — validates as Ban::validate first; CF_INVALID on a
 * private/internal or unparsable address. Use cf_ban_validate to obtain the
 * reference message. */
cf_err cf_ban_create(cf_tx *tx, int64_t user_id, cf_str ip_address, cf_ban *out);
/* Rust: Ban::validate — len == 0 in out means valid. */
cf_err cf_ban_validate(cf_str ip_address, cf_model_errors *out);

/* Every model-functions.json symbol for this module is prototyped above. */

#endif /* CF_MODELS_BAN_H */
