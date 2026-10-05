/* src/models/session.h — frozen model header (D01; declarations only).
 *
 * Source: tmp/rust-ref/crates/db/src/models/session.rs
 * Tables: sessions (schema.sql).
 */
#ifndef CF_MODELS_SESSION_H
#define CF_MODELS_SESSION_H

#include "types.h"

/* Session::ACTIVITY_REFRESH_RATE (1 h) in UTC microseconds. */
#define CF_SESSION_ACTIVITY_REFRESH_RATE_US 3600000000LL

/* sessions row. */
typedef struct cf_session {
    int64_t id;
    int64_t user_id;
    cf_str token; /* 24-character base58 has_secure_token */
    cf_optional_str ip_address;
    cf_optional_str user_agent;
    int64_t last_active_at; /* UTC microseconds */
    int64_t created_at;     /* UTC microseconds */
    int64_t updated_at;     /* UTC microseconds */
} cf_session;

void cf_session_dispose(cf_session *session);

typedef struct cf_session_vector {
    cf_session *items;
    size_t len, cap;
} cf_session_vector;

void cf_session_vector_dispose(cf_session_vector *vector);

/* Rust: Session::find — CF_NOT_FOUND when absent. */
cf_err cf_session_find(cf_db *db, int64_t id, cf_session *out);
/* Rust: Session::find_by_token */
cf_err cf_session_find_by_token(cf_db *db, cf_str token, bool *found, cf_session *out);
/* Rust: Session::for_user */
cf_err cf_session_for_user(cf_db *db, int64_t user_id, cf_session_vector *out);
/* Rust: Session::count_for_user */
cf_err cf_session_count_for_user(cf_db *db, int64_t user_id, int64_t *out);
/* Rust: Session::start */
cf_err cf_session_start(cf_tx *tx, int64_t user_id, cf_optional_str user_agent, cf_optional_str ip_address,
                        cf_session *out);
/* Rust: Session::needs_resume — activity older than one hour. */
bool cf_session_needs_resume(const cf_session *session, int64_t now_us);
/* Rust: Session::resume — refresh at most once an hour. */
cf_err cf_session_resume(cf_tx *tx, cf_session *session, cf_optional_str user_agent, cf_optional_str ip_address);
/* Rust: Session::destroy */
cf_err cf_session_destroy(cf_tx *tx, const cf_session *session);

/* Every model-functions.json symbol for this module is prototyped above. */

#endif /* CF_MODELS_SESSION_H */
