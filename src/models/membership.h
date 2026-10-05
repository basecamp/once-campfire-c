/* src/models/membership.h — frozen model header (D01; declarations only).
 *
 * Source: tmp/rust-ref/crates/db/src/models/membership.rs
 * Tables: memberships (schema.sql), joined to rooms for the ordered-room
 *         queries.
 */
#ifndef CF_MODELS_MEMBERSHIP_H
#define CF_MODELS_MEMBERSHIP_H

#include "types.h"
#include "room.h" /* cf_membership_room_pair embeds cf_room by value */

/* Membership::Connectable::CONNECTION_TTL (60 s) in UTC microseconds. */
#define CF_MEMBERSHIP_CONNECTION_TTL_US 60000000LL

/* memberships row. */
typedef struct cf_membership {
    int64_t id;
    int64_t room_id;
    int64_t user_id;
    cf_optional_involvement involvement; /* column nullable, default "mentions" */
    cf_optional_i64 unread_at;           /* UTC microseconds or NULL */
    cf_optional_i64 connected_at;        /* UTC microseconds or NULL */
    int64_t connections;
    int64_t created_at; /* UTC microseconds */
    int64_t updated_at; /* UTC microseconds */
} cf_membership;

void cf_membership_dispose(cf_membership *membership);

typedef struct cf_membership_vector {
    cf_membership *items;
    size_t len, cap;
} cf_membership_vector;

void cf_membership_vector_dispose(cf_membership_vector *vector);

/* A membership and its room, as returned by the ordered-room queries. Named
 * *_pair so the type does not collide with the translated Membership::room
 * function. */
typedef struct cf_membership_room_pair {
    cf_membership membership;
    cf_room room;
} cf_membership_room_pair;

void cf_membership_room_pair_dispose(cf_membership_room_pair *pair);

typedef struct cf_membership_room_pair_vector {
    cf_membership_room_pair *items;
    size_t len, cap;
} cf_membership_room_pair_vector;

void cf_membership_room_pair_vector_dispose(cf_membership_room_pair_vector *vector);

/* Rust: Membership::find — CF_NOT_FOUND when absent. */
cf_err cf_membership_find(cf_db *db, int64_t id, cf_membership *out);
/* Rust: Membership::count */
cf_err cf_membership_count(cf_db *db, int64_t *out);
/* Rust: Membership::for_user */
cf_err cf_membership_for_user(cf_db *db, int64_t user_id, cf_membership_vector *out);
/* Rust: Membership::for_room */
cf_err cf_membership_for_room(cf_db *db, int64_t room_id, cf_membership_vector *out);
/* Rust: Membership::find_by_room_and_user */
cf_err cf_membership_find_by_room_and_user(cf_db *db, int64_t room_id, int64_t user_id, bool *found, cf_membership *out);
/* Rust: Membership::visible_with_ordered_room — LOWER(rooms.name) ASC. */
cf_err cf_membership_visible_with_ordered_room(cf_db *db, int64_t user_id, cf_membership_room_pair_vector *out);
/* Rust: Membership::with_ordered_room — invisible included. */
cf_err cf_membership_with_ordered_room(cf_db *db, int64_t user_id, cf_membership_room_pair_vector *out);
/* Rust: Membership::count_without_direct_rooms */
cf_err cf_membership_count_without_direct_rooms(cf_db *db, int64_t user_id, int64_t *out);
/* Rust: Membership::unread_count — the push badge. */
cf_err cf_membership_unread_count(cf_db *db, int64_t user_id, int64_t *out);
/* Rust: Membership::connected_exists */
cf_err cf_membership_connected_exists(cf_db *db, int64_t id, int64_t now_us, bool *out);
/* Rust: Membership::disconnected_exists */
cf_err cf_membership_disconnected_exists(cf_db *db, int64_t id, int64_t now_us, bool *out);
/* Rust: Membership::connection_cutoff — now - CONNECTION_TTL. */
int64_t cf_membership_connection_cutoff(int64_t now_us);
/* Rust: Membership::room */
cf_err cf_membership_room(cf_db *db, const cf_membership *membership, cf_room *out);
/* Rust: Membership::user */
cf_err cf_membership_user(cf_db *db, const cf_membership *membership, cf_user *out);
/* Rust: Membership::involved_in */
bool cf_membership_involved_in(const cf_membership *membership, cf_involvement involvement);
/* Rust: Membership::update_involvement — present=false sets the column NULL. */
cf_err cf_membership_update_involvement(cf_tx *tx, cf_membership *membership, cf_optional_involvement involvement);
/* Rust: Membership::read — clears unread_at. */
cf_err cf_membership_read(cf_tx *tx, cf_membership *membership);
/* Rust: Membership::unread */
bool cf_membership_unread(const cf_membership *membership);
/* Rust: Membership::destroy — the user's sockets reconnect after commit
 * (CF_EVENT_DISCONNECT_USER with reconnect=true). */
cf_err cf_membership_destroy(cf_tx *tx, const cf_membership *membership);
/* Rust: Membership::disconnect_all — returns the number of rows reset. */
cf_err cf_membership_disconnect_all(cf_tx *tx, size_t *out);
/* Rust: Membership::connect — no updated_at write. */
cf_err cf_membership_connect(cf_tx *tx, int64_t id, int64_t connections);
/* Rust: Membership::is_connected */
bool cf_membership_is_connected(const cf_membership *membership, int64_t now_us);
/* Rust: Membership::present */
cf_err cf_membership_present(cf_tx *tx, cf_membership *membership);
/* Rust: Membership::connected */
cf_err cf_membership_connected(cf_tx *tx, cf_membership *membership);
/* Rust: Membership::disconnected */
cf_err cf_membership_disconnected(cf_tx *tx, cf_membership *membership);
/* Rust: Membership::refresh_connection */
cf_err cf_membership_refresh_connection(cf_tx *tx, cf_membership *membership);
/* Rust: Membership::reload */
cf_err cf_membership_reload(cf_db *db, cf_membership *membership);

/* Every model-functions.json symbol for this module is prototyped above. */

#endif /* CF_MODELS_MEMBERSHIP_H */
