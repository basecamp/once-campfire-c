/* src/views/model.c — disposal for the rooms/messages view models (A02
 * conventions: owned cf_str fields, owned vectors with named disposal).
 *
 * Kept beside the views so rendering tests can dispose the models they build
 * from fixtures without linking the database-backed presenters.
 */
#include "views.h"

#include <stdlib.h>
#include <string.h>

static void clear_str(cf_str *value) {
    free(value->ptr);
    memset(value, 0, sizeof *value);
}

void cf_view_user_dispose(cf_view_user *user) {
    if (user == NULL) return;
    clear_str(&user->name);
    clear_str(&user->title);
    clear_str(&user->avatar_url);
    memset(user, 0, sizeof *user);
}

void cf_view_room_dispose(cf_view_room *room) {
    if (room == NULL) return;
    if (room->has_name) clear_str(&room->name);
    clear_str(&room->display_name);
    memset(room, 0, sizeof *room);
}

void cf_view_boost_vector_dispose(cf_view_boost_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        clear_str(&vector->items[i].content);
        cf_view_user_dispose(&vector->items[i].booster);
    }
    free(vector->items);
    memset(vector, 0, sizeof *vector);
}

static void dispose_sound(cf_view_sound *sound) {
    clear_str(&sound->url);
    if (sound->has_image) clear_str(&sound->image.src);
    if (sound->has_text) clear_str(&sound->text);
    memset(sound, 0, sizeof *sound);
}

static void dispose_attachment(cf_view_attachment *attachment) {
    clear_str(&attachment->filename);
    clear_str(&attachment->blob_path);
    clear_str(&attachment->download_path);
    clear_str(&attachment->preview_url);
    memset(attachment, 0, sizeof *attachment);
}

void cf_view_message_dispose(cf_view_message *message) {
    if (message == NULL) return;
    clear_str(&message->client_message_id);
    clear_str(&message->room_name);
    cf_view_user_dispose(&message->creator);
    if (message->content_kind == CF_VIEW_CONTENT_TEXT) {
        clear_str(&message->text_html);
    } else if (message->content_kind == CF_VIEW_CONTENT_SOUND) {
        dispose_sound(&message->sound);
    } else if (message->content_kind == CF_VIEW_CONTENT_ATTACHMENT) {
        dispose_attachment(&message->attachment);
    }
    cf_view_boost_vector_dispose(&message->boosts);
    memset(message, 0, sizeof *message);
}

void cf_view_message_item_dispose(cf_view_message_item *item) {
    if (item == NULL) return;
    if (item->is_fragment) {
        clear_str(&item->fragment_html);
        clear_str(&item->fragment_client_message_id);
    } else {
        cf_view_message_dispose(&item->message);
    }
    memset(item, 0, sizeof *item);
}

void cf_view_message_item_vector_dispose(cf_view_message_item_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_view_message_item_dispose(&vector->items[i]);
    }
    free(vector->items);
    memset(vector, 0, sizeof *vector);
}

void cf_view_message_edit_model_dispose(cf_view_message_edit_model *model) {
    if (model == NULL) return;
    cf_view_message_dispose(&model->message);
    clear_str(&model->editable_body_html);
    memset(model, 0, sizeof *model);
}

void cf_view_room_show_model_dispose(cf_view_room_show_model *model) {
    if (model == NULL) return;
    cf_view_room_dispose(&model->room);
    cf_view_user_dispose(&model->user);
    cf_view_message_item_vector_dispose(&model->messages);
    clear_str(&model->join_code);
    clear_str(&model->messages_stream_name);
    memset(model, 0, sizeof *model);
}
