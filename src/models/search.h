/* src/models/search.h — frozen model header (D01; declarations only).
 *
 * Source: tmp/rust-ref/crates/db/src/models/search.rs
 * Tables: searches (schema.sql).
 */
#ifndef CF_MODELS_SEARCH_H
#define CF_MODELS_SEARCH_H

#include "types.h"

/* Search::RECENT_SEARCHES: how many recent searches record() keeps. */
#define CF_SEARCH_RECENT_SEARCHES 10

/* searches row. */
typedef struct cf_search {
    int64_t id;
    int64_t user_id;
    cf_str query;
    int64_t created_at; /* UTC microseconds */
    int64_t updated_at; /* UTC microseconds */
} cf_search;

void cf_search_dispose(cf_search *search);

typedef struct cf_search_vector {
    cf_search *items;
    size_t len, cap;
} cf_search_vector;

void cf_search_vector_dispose(cf_search_vector *vector);

/* Rust: Search::ordered_for_user — updated_at DESC. */
cf_err cf_search_ordered_for_user(cf_db *db, int64_t user_id, cf_search_vector *out);
/* Rust: Search::count */
cf_err cf_search_count(cf_db *db, int64_t *out);
/* Rust: Search::count_for_user */
cf_err cf_search_count_for_user(cf_db *db, int64_t user_id, int64_t *out);
/* Rust: Search::record — find_or_create_by(query).touch; creating trims the
 * user's searches to RECENT_SEARCHES. */
cf_err cf_search_record(cf_tx *tx, int64_t user_id, cf_str query, cf_search *out);
/* Rust: Search::destroy_all_for_user */
cf_err cf_search_destroy_all_for_user(cf_tx *tx, int64_t user_id);

/* Every model-functions.json symbol for this module is prototyped above. */

#endif /* CF_MODELS_SEARCH_H */
