/* src/models/room.h — frozen model header (D01; declarations only).
 *
 * Source: tmp/rust-ref/crates/db/src/models/room.rs
 * Tables: rooms (schema.sql), joined to memberships/users for the scoped
 *         queries; room/message_pusher queries live in push_subscription.h.
 * Shared RoomType helpers (class names, default involvement) are declared in
 * types.h under the type-name convention.
 */
#ifndef CF_MODELS_ROOM_H
#define CF_MODELS_ROOM_H

#include "types.h"

/* rooms row; room_type is the decoded STI "type" column. */
typedef struct cf_room {
    int64_t id;
    cf_optional_str name;
    cf_room_type room_type;
    int64_t creator_id;
    int64_t created_at; /* UTC microseconds */
    int64_t updated_at; /* UTC microseconds */
} cf_room;

void cf_room_dispose(cf_room *room);

typedef struct cf_room_vector {
    cf_room *items;
    size_t len, cap;
} cf_room_vector;

void cf_room_vector_dispose(cf_room_vector *vector);

/* Rust: Room::find — CF_NOT_FOUND when absent. */
cf_err cf_room_find(cf_db *db, int64_t id, cf_room *out);
/* Rust: Room::find_by_id */
cf_err cf_room_find_by_id(cf_db *db, int64_t id, bool *found, cf_room *out);
/* Rust: Room::all */
cf_err cf_room_all(cf_db *db, cf_room_vector *out);
/* Rust: Room::of_type */
cf_err cf_room_of_type(cf_db *db, cf_room_type room_type, cf_room_vector *out);
/* Rust: Room::count_of_type */
cf_err cf_room_count_of_type(cf_db *db, cf_room_type room_type, int64_t *out);
/* Rust: Room::original — the oldest room. */
cf_err cf_room_original(cf_db *db, bool *found, cf_room *out);
/* Rust: Room::for_user */
cf_err cf_room_for_user(cf_db *db, int64_t user_id, cf_room_vector *out);
/* Rust: Room::find_for_user */
cf_err cf_room_find_for_user(cf_db *db, int64_t user_id, int64_t room_id, bool *found, cf_room *out);
/* Rust: Room::for_user_of_type */
cf_err cf_room_for_user_of_type(cf_db *db, int64_t user_id, cf_room_type room_type, cf_room_vector *out);
/* Rust: Room::for_user_without_directs */
cf_err cf_room_for_user_without_directs(cf_db *db, int64_t user_id, cf_room_vector *out);
/* Rust: Room::original_for_user */
cf_err cf_room_original_for_user(cf_db *db, int64_t user_id, bool *found, cf_room *out);
/* Rust: Room::last_for_user */
cf_err cf_room_last_for_user(cf_db *db, int64_t user_id, bool *found, cf_room *out);
/* Rust: Room::create — an open room grants itself to every active user
 * (in-transaction per spec 02 D02). */
cf_err cf_room_create(cf_tx *tx, cf_room_type room_type, cf_optional_str name, int64_t creator_id, cf_room *out);
/* Rust: Room::create_for */
cf_err cf_room_create_for(cf_tx *tx, cf_room_type room_type, cf_optional_str name, int64_t creator_id,
                          const int64_t *user_ids, size_t user_ids_len, cf_room *out);
/* Rust: Room::find_or_create_direct_for */
cf_err cf_room_find_or_create_direct_for(cf_tx *tx, const int64_t *user_ids, size_t user_ids_len, int64_t creator_id,
                                         cf_room *out);
/* Rust: Room::find_direct_for */
cf_err cf_room_find_direct_for(cf_db *db, const int64_t *user_ids, size_t user_ids_len, bool *found, cf_room *out);
/* Rust: Room::update — name: NULL leaves it unchanged, present=false writes
 * NULL; room_type: NULL leaves it unchanged. A direct room cannot change type
 * (CF_INVALID). Becoming open grants every active user. */
cf_err cf_room_update(cf_tx *tx, cf_room *room, const cf_optional_str *name, const cf_room_type *room_type);
/* Rust: Room::touch */
cf_err cf_room_touch(cf_tx *tx, int64_t room_id);
/* Rust: Room::destroy — memberships without callbacks, messages destroyed. */
cf_err cf_room_destroy(cf_tx *tx, const cf_room *room);
/* Rust: Room::memberships */
cf_err cf_room_memberships(cf_db *db, const cf_room *room, cf_membership_vector *out);
/* Rust: Room::grant_to */
cf_err cf_room_grant_to(cf_tx *tx, const cf_room *room, const int64_t *user_ids, size_t user_ids_len);
/* Rust: Room::revoke_from — each removed member reconnects after commit. */
cf_err cf_room_revoke_from(cf_tx *tx, const cf_room *room, const int64_t *user_ids, size_t user_ids_len);
/* Rust: Room::revise */
cf_err cf_room_revise(cf_tx *tx, const cf_room *room, const int64_t *granted, size_t granted_len, const int64_t *revoked,
                      size_t revoked_len);
/* Rust: Room::users */
cf_err cf_room_users(cf_db *db, const cf_room *room, cf_user_vector *out);
/* Rust: Room::user_ids */
cf_err cf_room_user_ids(cf_db *db, const cf_room *room, cf_int64_vector *out);
/* Rust: Room::active_bots */
cf_err cf_room_active_bots(cf_db *db, const cf_room *room, cf_user_vector *out);
/* Rust: receive — marks members unread, then the push event. */
cf_err cf_room_receive(cf_tx *tx, int64_t room_id, const cf_message *message);
/* Rust: Room::open */
bool cf_room_open(const cf_room *room);
/* Rust: Room::closed */
bool cf_room_closed(const cf_room *room);
/* Rust: Room::direct */
bool cf_room_direct(const cf_room *room);
/* Rust: Room::default_involvement */
cf_involvement cf_room_default_involvement(const cf_room *room);
/* Rust: Room::reload */
cf_err cf_room_reload(cf_db *db, cf_room *room);

/* Every model-functions.json symbol for this module is prototyped above. */

#endif /* CF_MODELS_ROOM_H */
