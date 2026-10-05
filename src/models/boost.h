/* src/models/boost.h — frozen model header (D01; declarations only).
 *
 * Source: tmp/rust-ref/crates/db/src/models/boost.rs
 * Tables: boosts (schema.sql).
 */
#ifndef CF_MODELS_BOOST_H
#define CF_MODELS_BOOST_H

#include "types.h"

/* boosts row. */
typedef struct cf_boost {
    int64_t id;
    int64_t message_id;
    int64_t booster_id;
    cf_str content;
    int64_t created_at; /* UTC microseconds */
    int64_t updated_at; /* UTC microseconds */
} cf_boost;

void cf_boost_dispose(cf_boost *boost);

typedef struct cf_boost_vector {
    cf_boost *items;
    size_t len, cap;
} cf_boost_vector;

void cf_boost_vector_dispose(cf_boost_vector *vector);

/* Rust: Boost::find — CF_NOT_FOUND when absent. */
cf_err cf_boost_find(cf_db *db, int64_t id, cf_boost *out);
/* Rust: Boost::for_message */
cf_err cf_boost_for_message(cf_db *db, int64_t message_id, cf_boost_vector *out);
/* Rust: Boost::for_message_ordered — created_at ASC. */
cf_err cf_boost_for_message_ordered(cf_db *db, int64_t message_id, cf_boost_vector *out);
/* Rust: Boost::find_by_message_and_booster */
cf_err cf_boost_find_by_message_and_booster(cf_db *db, int64_t message_id, int64_t id, int64_t booster_id, cf_boost *out);
/* Rust: Boost::create — touches the message (and so the room). */
cf_err cf_boost_create(cf_tx *tx, int64_t message_id, int64_t booster_id, cf_str content, cf_boost *out);
/* Rust: Boost::destroy — touches the message (and so the room). */
cf_err cf_boost_destroy(cf_tx *tx, const cf_boost *boost);
/* Rust: Boost::delete_row — delete alone, for a message being destroyed. */
cf_err cf_boost_delete_row(cf_tx *tx, const cf_boost *boost);

/* Every model-functions.json symbol for this module is prototyped above. */

#endif /* CF_MODELS_BOOST_H */
