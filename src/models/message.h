/* src/models/message.h — frozen model header (D01; declarations only).
 *
 * Source: tmp/rust-ref/crates/db/src/models/message.rs
 * Tables: messages (schema.sql); writes action_text_rich_texts through
 *         rich_text_record.h, active_storage_attachments/blobs through
 *         active_storage.h, boosts, and the message_search_index FTS table.
 */
#ifndef CF_MODELS_MESSAGE_H
#define CF_MODELS_MESSAGE_H

#include "types.h"

/* Message::Pagination::PAGE_SIZE */
#define CF_MESSAGE_PAGE_SIZE 40

/* messages row. */
typedef struct cf_message {
    int64_t id;
    int64_t room_id;
    int64_t creator_id;
    cf_str client_message_id;
    int64_t created_at; /* UTC microseconds */
    int64_t updated_at; /* UTC microseconds */
} cf_message;

void cf_message_dispose(cf_message *message);

typedef struct cf_message_vector {
    cf_message *items;
    size_t len, cap;
} cf_message_vector;

void cf_message_vector_dispose(cf_message_vector *vector);

/* Attributes for Message::create (NewMessage). The borrowed text/ids are
 * copied by the call; a zero-initialized value has absent optionals. */
typedef struct cf_new_message {
    int64_t room_id;
    int64_t creator_id;
    cf_optional_str client_message_id;  /* absent -> random UUID */
    cf_optional_str body;               /* stored Action Text body */
    cf_optional_i64 attachment_blob_id; /* already-saved blob to attach */
} cf_new_message;

/* Message::content_type names, in reference declaration order. */
typedef enum {
    CF_CONTENT_TYPE_ATTACHMENT = 0,
    CF_CONTENT_TYPE_SOUND = 1,
    CF_CONTENT_TYPE_TEXT = 2
} cf_content_type;

/* Rust: ContentType::name */
const char *cf_content_type_name(cf_content_type content_type);

/* Rust: Message::find — CF_NOT_FOUND when absent. */
cf_err cf_message_find(cf_db *db, int64_t id, cf_message *out);
/* Rust: Message::find_by_id */
cf_err cf_message_find_by_id(cf_db *db, int64_t id, bool *found, cf_message *out);
/* Rust: Message::last */
cf_err cf_message_last(cf_db *db, bool *found, cf_message *out);
/* Rust: Message::count */
cf_err cf_message_count(cf_db *db, int64_t *out);
/* Rust: Message::by_creator */
cf_err cf_message_by_creator(cf_db *db, int64_t creator_id, cf_message_vector *out);
/* Rust: Message::for_room */
cf_err cf_message_for_room(cf_db *db, int64_t room_id, cf_message_vector *out);
/* Rust: Message::find_in_room — CF_NOT_FOUND when absent. */
cf_err cf_message_find_in_room(cf_db *db, int64_t room_id, int64_t id, cf_message *out);
/* Rust: Message::count_in_room */
cf_err cf_message_count_in_room(cf_db *db, int64_t room_id, int64_t *out);
/* Rust: Message::find_reachable — CF_NOT_FOUND when absent. */
cf_err cf_message_find_reachable(cf_db *db, int64_t user_id, int64_t id, cf_message *out);
/* Rust: Message::last_page — newest PAGE_SIZE, oldest first. */
cf_err cf_message_last_page(cf_db *db, int64_t room_id, cf_message_vector *out);
/* Rust: Message::first_page */
cf_err cf_message_first_page(cf_db *db, int64_t room_id, cf_message_vector *out);
/* Rust: Message::page_before */
cf_err cf_message_page_before(cf_db *db, int64_t room_id, const cf_message *message, cf_message_vector *out);
/* Rust: Message::page_after */
cf_err cf_message_page_after(cf_db *db, int64_t room_id, const cf_message *message, cf_message_vector *out);
/* Rust: Message::page_around */
cf_err cf_message_page_around(cf_db *db, int64_t room_id, const cf_message *message, cf_message_vector *out);
/* Rust: Message::page_created_since */
cf_err cf_message_page_created_since(cf_db *db, int64_t room_id, int64_t time_us, cf_message_vector *out);
/* Rust: Message::page_updated_since */
cf_err cf_message_page_updated_since(cf_db *db, int64_t room_id, int64_t time_us, const int64_t *excluding,
                                     size_t excluding_len, cf_message_vector *out);
/* Rust: Message::exists_before */
cf_err cf_message_exists_before(cf_db *db, int64_t room_id, const cf_message *message, bool *out);
/* Rust: Message::exists_after */
cf_err cf_message_exists_after(cf_db *db, int64_t room_id, const cf_message *message, bool *out);
/* Rust: Message::paged — more than one page exists. */
cf_err cf_message_paged(cf_db *db, int64_t room_id, bool *out);
/* Rust: Message::search_in_room */
cf_err cf_message_search_in_room(cf_db *db, int64_t room_id, cf_str query, cf_message_vector *out);
/* Rust: Message::search_reachable — newest 100 matches, oldest first. */
cf_err cf_message_search_reachable(cf_db *db, int64_t user_id, cf_str query, cf_message_vector *out);
/* Rust: Message::create — message, body, attachment, FTS and room.receive
 * effects inside the caller transaction (spec 02 D02). */
cf_err cf_message_create(cf_tx *tx, const cf_new_message *attributes, cf_message *out);
/* Rust: Message::update_body */
cf_err cf_message_update_body(cf_tx *tx, cf_message *message, cf_str body);
/* Rust: Message::touch — message, then its room, then the FTS reindex. */
cf_err cf_message_touch(cf_tx *tx, cf_message *message);
/* Rust: Message::replace_attachment — present=false removes the attachment. */
cf_err cf_message_replace_attachment(cf_tx *tx, cf_message *message, cf_optional_i64 blob_id);
/* Rust: Message::destroy */
cf_err cf_message_destroy(cf_tx *tx, const cf_message *message);
/* Rust: Message::room */
cf_err cf_message_room(cf_db *db, const cf_message *message, cf_room *out);
/* Rust: Message::creator */
cf_err cf_message_creator(cf_db *db, const cf_message *message, cf_user *out);
/* Rust: Message::boosts — ordered by created_at ASC. */
cf_err cf_message_boosts(cf_db *db, const cf_message *message, cf_boost_vector *out);
/* Rust: Message::body */
cf_err cf_message_body(cf_db *db, const cf_message *message, bool *found, cf_rich_text_record *out);
/* Rust: Message::body_html */
cf_err cf_message_body_html(cf_db *db, const cf_message *message, bool *found, cf_str *out);
/* Rust: Message::attachment */
cf_err cf_message_attachment(cf_db *db, const cf_message *message, bool *found, cf_attachment *out_attachment,
                             cf_blob *out_blob);
/* Rust: Message::plain_text_body */
cf_err cf_message_plain_text_body(cf_db *db, const cf_message *message, const cf_richtext *rich_text, cf_str *out);
/* Rust: Message::content_type */
cf_err cf_message_content_type(cf_db *db, const cf_message *message, const cf_richtext *rich_text,
                               cf_content_type *out);
/* Rust: Message::sound — a body of exactly "/play <name>"; out borrows the
 * static builtin table (never disposed). */
cf_err cf_message_sound(cf_db *db, const cf_message *message, const cf_richtext *rich_text, bool *found,
                        const cf_sound **out);
/* Rust: Message::mentionees */
cf_err cf_message_mentionees(cf_db *db, const cf_message *message, const cf_richtext *rich_text, cf_user_vector *out);
/* Rust: Message::reload */
cf_err cf_message_reload(cf_db *db, cf_message *message);
/* Rust: mentionees_in_room — room members among user_ids, in users.id order. */
cf_err cf_message_mentionees_in_room(cf_db *db, int64_t room_id, const int64_t *user_ids, size_t user_ids_len,
                                     cf_user_vector *out);
/* Rust: sound_in — out borrows the static builtin table; false leaves it NULL. */
bool cf_message_sound_in(cf_str plain_text, const cf_sound **out);

/* Every model-functions.json symbol for this module is prototyped above. */

#endif /* CF_MODELS_MESSAGE_H */
