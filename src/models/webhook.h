/* src/models/webhook.h — frozen model header (D01; declarations only).
 *
 * Source: tmp/rust-ref/crates/db/src/models/webhook.rs
 * Tables: webhooks (schema.sql). Delivery (HTTP, replies) belongs to I01.
 */
#ifndef CF_MODELS_WEBHOOK_H
#define CF_MODELS_WEBHOOK_H

#include "types.h"

/* Webhook::ENDPOINT_TIMEOUT */
#define CF_WEBHOOK_ENDPOINT_TIMEOUT_SECONDS 7

/* webhooks row. */
typedef struct cf_webhook {
    int64_t id;
    int64_t user_id;
    cf_optional_str url;
    int64_t created_at; /* UTC microseconds */
    int64_t updated_at; /* UTC microseconds */
} cf_webhook;

void cf_webhook_dispose(cf_webhook *webhook);

typedef struct cf_webhook_vector {
    cf_webhook *items;
    size_t len, cap;
} cf_webhook_vector;

void cf_webhook_vector_dispose(cf_webhook_vector *vector);

/* Rust: Webhook::find_by_user */
cf_err cf_webhook_find_by_user(cf_db *db, int64_t user_id, bool *found, cf_webhook *out);
/* Rust: Webhook::create */
cf_err cf_webhook_create(cf_tx *tx, int64_t user_id, cf_optional_str url, cf_webhook *out);
/* Rust: Webhook::update_url — a no-op when unchanged. */
cf_err cf_webhook_update_url(cf_tx *tx, cf_webhook *webhook, cf_str url);
/* Rust: Webhook::destroy */
cf_err cf_webhook_destroy(cf_tx *tx, const cf_webhook *webhook);
/* Rust: Webhook::payload — the JSON body deliver posts; the two path
 * arguments are borrowed route-helper results. */
cf_err cf_webhook_payload(cf_db *db, const cf_webhook *webhook, const cf_richtext *rich_text,
                          const cf_message *message, cf_str room_bot_messages_path, cf_str message_path, cf_str *out);

/* Every model-functions.json symbol for this module is prototyped above. */

#endif /* CF_MODELS_WEBHOOK_H */
