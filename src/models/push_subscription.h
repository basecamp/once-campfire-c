/* src/models/push_subscription.h — frozen model header (D01; declarations only).
 *
 * Source: tmp/rust-ref/crates/db/src/models/push_subscription.rs
 *         (the subscriber queries of room/message_pusher.rs)
 * Tables: push_subscriptions (schema.sql); the delivery itself belongs to I02.
 */
#ifndef CF_MODELS_PUSH_SUBSCRIPTION_H
#define CF_MODELS_PUSH_SUBSCRIPTION_H

#include "types.h"

/* push_subscriptions row. */
typedef struct cf_push_subscription {
    int64_t id;
    int64_t user_id;
    cf_optional_str endpoint;
    cf_optional_str p256dh_key;
    cf_optional_str auth_key;
    cf_optional_str user_agent;
    int64_t created_at; /* UTC microseconds */
    int64_t updated_at; /* UTC microseconds */
} cf_push_subscription;

void cf_push_subscription_dispose(cf_push_subscription *subscription);

typedef struct cf_push_subscription_vector {
    cf_push_subscription *items;
    size_t len, cap;
} cf_push_subscription_vector;

void cf_push_subscription_vector_dispose(cf_push_subscription_vector *vector);

/* What Room::MessagePusher#build_payload sends. */
typedef struct cf_push_payload {
    cf_str title;
    cf_str body;
    cf_str path;
} cf_push_payload;

void cf_push_payload_dispose(cf_push_payload *payload);

/* C shape of `&dyn Fn(&str) -> Option<String>` from
 * RestrictedHTTP::PrivateNetworkGuard.resolve: returns owned address text, or
 * present=false for a private or unresolvable host. The model disposes the
 * returned value. A missing resolver (NULL) means "cannot resolve".
 * Proposed D01 boundary; see docs/devel/evidence/D01-headers.md. */
typedef cf_optional_str (*cf_push_resolve_fn)(void *arg, cf_str host);

/* PushSubscription::MAX_PAYLOAD_TITLE_BYTES / _BODY_BYTES. */
#define CF_PUSH_SUBSCRIPTION_MAX_PAYLOAD_TITLE_BYTES 256
#define CF_PUSH_SUBSCRIPTION_MAX_PAYLOAD_BODY_BYTES 3072

/* Rust: PushSubscription::new — unsaved record for validation; the borrowed
 * text is copied into the returned record. */
cf_err cf_push_subscription_new(int64_t user_id, cf_optional_str endpoint, cf_optional_str p256dh_key,
                                cf_optional_str auth_key, cf_optional_str user_agent, cf_push_subscription *out);
/* Rust: PushSubscription::find — CF_NOT_FOUND when absent. */
cf_err cf_push_subscription_find(cf_db *db, int64_t id, cf_push_subscription *out);
/* Rust: PushSubscription::count */
cf_err cf_push_subscription_count(cf_db *db, int64_t *out);
/* Rust: PushSubscription::for_user */
cf_err cf_push_subscription_for_user(cf_db *db, int64_t user_id, cf_push_subscription_vector *out);
/* Rust: PushSubscription::find_for_user_by_keys */
cf_err cf_push_subscription_find_for_user_by_keys(cf_db *db, int64_t user_id, cf_str endpoint, cf_str p256dh_key,
                                                  cf_str auth_key, bool *found, cf_push_subscription *out);
/* Rust: PushSubscription::create — validates first (validate semantics);
 * CF_INVALID on failure (cf_push_subscription_validate gives the message). */
cf_err cf_push_subscription_create(cf_tx *tx, const cf_push_subscription *subscription, cf_push_resolve_fn resolve,
                                   void *resolve_arg, cf_push_subscription *out);
/* Rust: PushSubscription::destroy */
cf_err cf_push_subscription_destroy(cf_tx *tx, const cf_push_subscription *subscription);
/* Rust: PushSubscription::destroy_by_endpoint */
cf_err cf_push_subscription_destroy_by_endpoint(cf_tx *tx, int64_t user_id, cf_str endpoint);
/* Rust: PushSubscription::badge — user.memberships.unread.count. */
cf_err cf_push_subscription_badge(cf_db *db, const cf_push_subscription *subscription, int64_t *out);
/* Rust: PushSubscription::validate — len == 0 in out means valid. resolve may
 * be NULL when no resolution can be attempted (treated as unresolvable). */
cf_err cf_push_subscription_validate(const cf_push_subscription *subscription, cf_push_resolve_fn resolve,
                                     void *resolve_arg, cf_model_errors *out);
/* Rust: PushSubscription::resolved_endpoint_ip — true and out set (owned,
 * present) when the endpoint host resolves to a permitted public address. */
bool cf_push_subscription_resolved_endpoint_ip(const cf_push_subscription *subscription, cf_push_resolve_fn resolve,
                                               void *resolve_arg, cf_optional_str *out);
/* Rust: PushSubscription::payload_for */
cf_err cf_push_subscription_payload_for(cf_db *db, const cf_richtext *rich_text, const cf_room *room,
                                        const cf_message *message, cf_push_payload *out);
/* Rust: PushSubscription::for_users_involved_in_everything */
cf_err cf_push_subscription_for_users_involved_in_everything(cf_db *db, int64_t room_id, int64_t creator_id,
                                                             int64_t now_us, cf_push_subscription_vector *out);
/* Rust: PushSubscription::for_mentioned_users */
cf_err cf_push_subscription_for_mentioned_users(cf_db *db, int64_t room_id, int64_t creator_id,
                                                const int64_t *mentionee_ids, size_t mentionee_ids_len, int64_t now_us,
                                                cf_push_subscription_vector *out);
/* Rust: PushSubscription::pushes_for — payload plus (everything, mentions). */
cf_err cf_push_subscription_pushes_for(cf_db *db, const cf_richtext *rich_text, const cf_message *message,
                                       int64_t now_us, cf_push_payload *out_payload,
                                       cf_push_subscription_vector *out_everything,
                                       cf_push_subscription_vector *out_mentions);

/* Every model-functions.json symbol for this module is prototyped above. */

#endif /* CF_MODELS_PUSH_SUBSCRIPTION_H */
