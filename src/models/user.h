/* src/models/user.h — frozen model header (D01; declarations only).
 *
 * Source: tmp/rust-ref/crates/db/src/models/user.rs
 * Tables: users (schema.sql), plus dependent cleanup across memberships,
 *         sessions, push_subscriptions, searches, bans, webhooks and messages.
 * Shared Role/Status helpers are declared in types.h under the type-name
 * convention.
 */
#ifndef CF_MODELS_USER_H
#define CF_MODELS_USER_H

#include "types.h"

/* User::MENTION_CONTENT_TYPE */
#define CF_USER_MENTION_CONTENT_TYPE "application/vnd.campfire.mention"

/* users row. */
typedef struct cf_user {
    int64_t id;
    cf_str name;
    cf_optional_str email_address;
    cf_optional_str password_digest; /* already hashed (A01 password service) */
    cf_role role;
    cf_status status;
    cf_optional_str bio;
    cf_optional_str bot_token;
    int64_t created_at; /* UTC microseconds */
    int64_t updated_at; /* UTC microseconds */
} cf_user;

void cf_user_dispose(cf_user *user);

typedef struct cf_user_vector {
    cf_user *items;
    size_t len, cap;
} cf_user_vector;

void cf_user_vector_dispose(cf_user_vector *vector);

/* Attributes for User::create (NewUser). Borrowed for the call; a
 * zero-initialized value is role member with all optionals absent. */
typedef struct cf_new_user {
    cf_str name;
    cf_optional_str email_address;
    cf_optional_str password_digest; /* already hashed; absent = no password */
    cf_role role;                    /* CF_ROLE_MEMBER in a zero-initialized value */
    cf_optional_str bio;
    cf_optional_str bot_token;
} cf_new_user;

/* Attributes for User::update (UserChanges). NULL means "leave unchanged";
 * an optional-text pointer with present=false means "write NULL". */
typedef struct cf_user_changes {
    const cf_str *name;
    const cf_optional_str *email_address; /* present=false writes NULL */
    const cf_str *password_digest;        /* already hashed */
    const cf_role *role;
    const cf_status *status;
    const cf_optional_str *bio;       /* present=false writes NULL */
    const cf_optional_str *bot_token; /* present=false writes NULL */
} cf_user_changes;

/* Rust: User::find — CF_NOT_FOUND when absent. */
cf_err cf_user_find(cf_db *db, int64_t id, cf_user *out);
/* Rust: User::find_by_id */
cf_err cf_user_find_by_id(cf_db *db, int64_t id, bool *found, cf_user *out);
/* Rust: User::find_active — CF_NOT_FOUND unless status = active. */
cf_err cf_user_find_active(cf_db *db, int64_t id, cf_user *out);
/* Rust: User::find_by_email_address */
cf_err cf_user_find_by_email_address(cf_db *db, cf_str email_address, bool *found, cf_user *out);
/* Rust: User::all */
cf_err cf_user_all(cf_db *db, cf_user_vector *out);
/* Rust: User::count */
cf_err cf_user_count(cf_db *db, int64_t *out);
/* Rust: User::where_ids */
cf_err cf_user_where_ids(cf_db *db, const int64_t *ids, size_t ids_len, cf_user_vector *out);
/* Rust: User::active_ordered */
cf_err cf_user_active_ordered(cf_db *db, cf_user_vector *out);
/* Rust: User::active */
cf_err cf_user_active(cf_db *db, cf_user_vector *out);
/* Rust: User::active_filtered_by_ordered */
cf_err cf_user_active_filtered_by_ordered(cf_db *db, cf_str query, cf_user_vector *out);
/* Rust: User::active_ordered_without_bots */
cf_err cf_user_active_ordered_without_bots(cf_db *db, cf_user_vector *out);
/* Rust: User::active_bots_ordered */
cf_err cf_user_active_bots_ordered(cf_db *db, cf_user_vector *out);
/* Rust: User::find_active_bot — CF_NOT_FOUND when absent. */
cf_err cf_user_find_active_bot(cf_db *db, int64_t id, cf_user *out);
/* Rust: User::find_active_by_email_address */
cf_err cf_user_find_active_by_email_address(cf_db *db, cf_str email_address, bool *found, cf_user *out);
/* Rust: User::authenticated — candidate may be NULL (account missing): a
 * nonempty password still runs the dummy verification. Requires the A01
 * password verifier; proposed boundary in the D01 evidence. */
cf_err cf_user_authenticated(const cf_user *candidate, cf_str password, bool *out);
/* Rust: User::authenticate_bot */
cf_err cf_user_authenticate_bot(cf_db *db, cf_str bot_key, bool *found, cf_user *out);
/* Rust: User::create */
cf_err cf_user_create(cf_tx *tx, const cf_new_user *attributes, cf_user *out);
/* Rust: User::create_bot */
cf_err cf_user_create_bot(cf_tx *tx, cf_str name, cf_optional_str webhook_url, cf_user *out);
/* Rust: User::update */
cf_err cf_user_update(cf_tx *tx, cf_user *user, const cf_user_changes *changes);
/* Rust: User::update_bot */
cf_err cf_user_update_bot(cf_tx *tx, cf_user *user, const cf_user_changes *changes, cf_optional_str webhook_url);
/* Rust: User::reset_bot_key */
cf_err cf_user_reset_bot_key(cf_tx *tx, cf_user *user);
/* Rust: User::deactivate */
cf_err cf_user_deactivate(cf_tx *tx, cf_user *user);
/* Rust: User::ban */
cf_err cf_user_ban(cf_tx *tx, cf_user *user);
/* Rust: User::unban */
cf_err cf_user_unban(cf_tx *tx, cf_user *user);
/* Rust: User::remove_banned_content — destroys and returns the user's
 * messages for broadcast_remove. */
cf_err cf_user_remove_banned_content(cf_tx *tx, const cf_user *user, cf_message_vector *out);
/* Rust: User::reset_remote_connections — disconnect with reconnect=true. */
cf_err cf_user_reset_remote_connections(cf_tx *tx, const cf_user *user);
/* Rust: User::deliver_webhook_later — only bots with a webhook. */
cf_err cf_user_deliver_webhook_later(cf_tx *tx, const cf_user *user, int64_t message_id);
/* Rust: User::memberships */
cf_err cf_user_memberships(cf_db *db, const cf_user *user, cf_membership_vector *out);
/* Rust: User::sessions */
cf_err cf_user_sessions(cf_db *db, const cf_user *user, cf_session_vector *out);
/* Rust: User::webhook */
cf_err cf_user_webhook(cf_db *db, const cf_user *user, bool *found, cf_webhook *out);
/* Rust: User::webhook_url */
cf_err cf_user_webhook_url(cf_db *db, const cf_user *user, bool *found, cf_str *out);
/* Rust: User::initials */
cf_err cf_user_initials(const cf_user *user, cf_str *out);
/* Rust: User::title */
cf_err cf_user_title(const cf_user *user, cf_str *out);
/* Rust: User::bot_key */
cf_err cf_user_bot_key(const cf_user *user, cf_str *out);
/* Rust: User::can_administer */
bool cf_user_can_administer(const cf_user *user, cf_optional_i64 record_creator_id, bool record_is_new);
/* Rust: User::authenticate */
cf_err cf_user_authenticate(const cf_user *user, cf_str password, bool *out);
/* Rust: User::is_member */
bool cf_user_is_member(const cf_user *user);
/* Rust: User::is_administrator */
bool cf_user_is_administrator(const cf_user *user);
/* Rust: User::is_bot */
bool cf_user_is_bot(const cf_user *user);
/* Rust: User::is_active */
bool cf_user_is_active(const cf_user *user);
/* Rust: User::is_deactivated */
bool cf_user_is_deactivated(const cf_user *user);
/* Rust: User::is_banned */
bool cf_user_is_banned(const cf_user *user);
/* Rust: User::attachable_plain_text_representation — "@name". */
cf_err cf_user_attachable_plain_text_representation(const cf_user *user, cf_str *out);
/* Rust: User::reload */
cf_err cf_user_reload(cf_db *db, cf_user *user);
/* Rust: generate_bot_token — SecureRandom.alphanumeric(12). */
cf_err cf_user_generate_bot_token(cf_str *out);

/* Deferred reference symbols (contracts/model-functions.json, user.rs).
 * Each is not required by released packets; reasons in parentheses.
 * deferred:User::find_by_sql — not required by released packets (generic rusqlite::Params has no C shape; every C read is fixed SQL)
 * deferred:PasswordDigest::create — not required by released packets (A01 owns the password service in src/auth)
 * deferred:PasswordDigest::hash — not required by released packets (A01 owns the password service in src/auth)
 * deferred:PasswordDigest::into_string — not required by released packets (A01 owns the password service in src/auth)
 * deferred:password_digest — not required by released packets (A01 owns the password service in src/auth)
 */

#endif /* CF_MODELS_USER_H */
