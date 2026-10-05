/* src/presenters/messages.c — message/boost view mapping
 * (tmp/rust-ref/crates/campfire/src/controllers/presenters.rs Presenter and
 * messages view structs).
 *
 * One read transaction per public presenter call; the renderers that follow
 * only read the owned models.  Rich text goes through R02 (cf_richtext_*);
 * attachment paths need S02's signed storage helpers and are reported
 * unresolved (the presenter refuses rather than inventing a path).
 */
#include "views.h"

#include "app.h"
#include "config.h"
#include "db/db_internal.h"
#include "models/active_storage.h"
#include "models/boost.h"
#include "models/message.h"
#include "models/room.h"
#include "models/sound.h"
#include "models/user.h"
#include "richtext.h"

#include <stdlib.h>
#include <string.h>

/* Unicode 16.0 Extended_Pictographic / Emoji_Presentation ranges, transcribed
 * from the pinned regex-syntax 0.8.11 tables the reference's `\p{...}`
 * classes compile from (crates/ruby? no: the regex crate's generated
 * unicode_tables/property_bool.rs). */
static const struct { uint32_t first, last; } EMOJI_EXTENDED_PICTOGRAPHIC[] = {
    {0xA9, 0xA9}, {0xAE, 0xAE}, {0x203C, 0x203C}, {0x2049, 0x2049},
    {0x2122, 0x2122}, {0x2139, 0x2139}, {0x2194, 0x2199}, {0x21A9, 0x21AA},
    {0x231A, 0x231B}, {0x2328, 0x2328}, {0x2388, 0x2388}, {0x23CF, 0x23CF},
    {0x23E9, 0x23F3}, {0x23F8, 0x23FA}, {0x24C2, 0x24C2}, {0x25AA, 0x25AB},
    {0x25B6, 0x25B6}, {0x25C0, 0x25C0}, {0x25FB, 0x25FE}, {0x2600, 0x2605},
    {0x2607, 0x2612}, {0x2614, 0x2685}, {0x2690, 0x2705}, {0x2708, 0x2712},
    {0x2714, 0x2714}, {0x2716, 0x2716}, {0x271D, 0x271D}, {0x2721, 0x2721},
    {0x2728, 0x2728}, {0x2733, 0x2734}, {0x2744, 0x2744}, {0x2747, 0x2747},
    {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757},
    {0x2763, 0x2767}, {0x2795, 0x2797}, {0x27A1, 0x27A1}, {0x27B0, 0x27B0},
    {0x27BF, 0x27BF}, {0x2934, 0x2935}, {0x2B05, 0x2B07}, {0x2B1B, 0x2B1C},
    {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x3030, 0x3030}, {0x303D, 0x303D},
    {0x3297, 0x3297}, {0x3299, 0x3299}, {0x1F000, 0x1F0FF}, {0x1F10D, 0x1F10F},
    {0x1F12F, 0x1F12F}, {0x1F16C, 0x1F171}, {0x1F17E, 0x1F17F}, {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A}, {0x1F1AD, 0x1F1E5}, {0x1F201, 0x1F20F}, {0x1F21A, 0x1F21A},
    {0x1F22F, 0x1F22F}, {0x1F232, 0x1F23A}, {0x1F23C, 0x1F23F}, {0x1F249, 0x1F3FA},
    {0x1F400, 0x1F53D}, {0x1F546, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F774, 0x1F77F},
    {0x1F7D5, 0x1F7FF}, {0x1F80C, 0x1F80F}, {0x1F848, 0x1F84F}, {0x1F85A, 0x1F85F},
    {0x1F888, 0x1F88F}, {0x1F8AE, 0x1F8FF}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945},
    {0x1F947, 0x1FAFF}, {0x1FC00, 0x1FFFD},
};
static const struct { uint32_t first, last; } EMOJI_PRESENTATION[] = {
    {0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3},
    {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F},
    {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE},
    {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA},
    {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD},
    {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C},
    {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797},
    {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50},
    {0x2B55, 0x2B55}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A}, {0x1F1E6, 0x1F1FF}, {0x1F201, 0x1F201}, {0x1F21A, 0x1F21A},
    {0x1F22F, 0x1F22F}, {0x1F232, 0x1F236}, {0x1F238, 0x1F23A}, {0x1F250, 0x1F251},
    {0x1F300, 0x1F320}, {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393},
    {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4},
    {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D},
    {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596},
    {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC},
    {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D7}, {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC},
    {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A},
    {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FA7C}, {0x1FA80, 0x1FA89},
    {0x1FA8F, 0x1FAC6}, {0x1FACE, 0x1FADC}, {0x1FADF, 0x1FAE9}, {0x1FAF0, 0x1FAF8},
};

static bool in_ranges(const void *table, size_t count, uint32_t cp) {
    const struct {
        uint32_t first, last;
    } *ranges = table;
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < ranges[mid].first) {
            hi = mid;
        } else if (cp > ranges[mid].last) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

/* Strict UTF-8 decode; false on invalid bytes. */
static bool utf8_next(cf_span text, size_t *index, uint32_t *cp) {
    size_t i = *index;
    if (i >= text.len) return false;
    unsigned char b = text.ptr[i];
    size_t len;
    uint32_t value;
    if (b < 0x80) {
        len = 1;
        value = b;
    } else if ((b & 0xE0) == 0xC0) {
        len = 2;
        value = b & 0x1F;
    } else if ((b & 0xF0) == 0xE0) {
        len = 3;
        value = b & 0x0F;
    } else if ((b & 0xF8) == 0xF0) {
        len = 4;
        value = b & 0x07;
    } else {
        return false;
    }
    if (i + len > text.len) return false;
    for (size_t k = 1; k < len; k++) {
        unsigned char c = text.ptr[i + k];
        if ((c & 0xC0) != 0x80) return false;
        value = (value << 6) | (c & 0x3F);
    }
    *index = i + len;
    *cp = value;
    return true;
}

/* `String#all_emoji?` (reference/lib/rails_ext/string.rb):
 * `\A(\p{Emoji_Presentation}|\p{Extended_Pictographic}|\x{FE0F})+\z`. */
static bool all_emoji(cf_span text) {
    if (text.len == 0) return false;
    size_t i = 0;
    while (i < text.len) {
        uint32_t cp = 0;
        if (!utf8_next(text, &i, &cp)) return false;
        if (cp == 0xFE0F) continue;
        if (in_ranges(EMOJI_EXTENDED_PICTOGRAPHIC,
                      sizeof EMOJI_EXTENDED_PICTOGRAPHIC /
                          sizeof EMOJI_EXTENDED_PICTOGRAPHIC[0],
                      cp) ||
            in_ranges(EMOJI_PRESENTATION,
                      sizeof EMOJI_PRESENTATION / sizeof EMOJI_PRESENTATION[0],
                      cp)) {
            continue;
        }
        return false;
    }
    return true;
}

/* ------------------------------------------------------------- mapping */

static void clear_str(cf_str *value) {
    free(value->ptr);
    memset(value, 0, sizeof *value);
}

static cf_err copy_span(cf_span span, cf_str *out) {
    memset(out, 0, sizeof *out);
    if (span.len != 0 && span.ptr == NULL) return CF_INVALID;
    char *copy = malloc(span.len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (span.len != 0) memcpy(copy, span.ptr, span.len);
    copy[span.len] = '\0';
    out->ptr = copy;
    out->len = span.len;
    return CF_OK;
}

static cf_err copy_empty(cf_str *out) {
    out->ptr = malloc(1);
    if (out->ptr == NULL) return CF_NOMEM;
    out->ptr[0] = '\0';
    out->len = 0;
    return CF_OK;
}

/* `asset_path(logical)` through the configured asset module. */
static cf_err asset_path(cf_span logical, cf_str *out) {
    cf_builder builder = {0};
    cf_err rc = cf_views_asset_path(logical, &builder);
    if (rc == CF_OK) {
        rc = copy_span((cf_span){builder.ptr, builder.len}, out);
    }
    cf_builder_dispose(&builder);
    return rc;
}

/* `room_display_name(room, for_user: nil)`. */
static cf_err room_name_for_nobody(cf_db *db, int64_t room_id, cf_str *out) {
    memset(out, 0, sizeof *out);
    cf_room room = {0};
    cf_err rc = cf_room_find(db, room_id, &room);
    if (rc != CF_OK) return rc;
    rc = cf_view_room_display_name(
        (cf_span){(const unsigned char *)(room.name.present
                                              ? room.name.value.ptr
                                              : NULL),
                  room.name.present ? room.name.value.len : 0},
        cf_room_direct(&room), NULL, 0, (cf_span){NULL, 0}, out);
    cf_room_dispose(&room);
    return rc;
}

static cf_err map_boost(cf_ctx *ctx, const cf_boost *row, cf_view_boost *out) {
    memset(out, 0, sizeof *out);
    out->id = row->id;
    out->updated_at_us = row->updated_at;
    out->message_id = row->message_id;
    out->all_emoji = all_emoji((cf_span){(const unsigned char *)row->content.ptr,
                                         row->content.len});
    cf_err rc = copy_span((cf_span){(const unsigned char *)row->content.ptr,
                                    row->content.len},
                          &out->content);
    if (rc != CF_OK) goto fail;
    {
        cf_user booster = {0};
        rc = cf_user_find(ctx->reader, row->booster_id, &booster);
        if (rc != CF_OK) goto fail;
        rc = cf_presenter_user_view(ctx, &booster, &out->booster);
        cf_user_dispose(&booster);
        if (rc != CF_OK) goto fail;
    }
    return CF_OK;
fail:
    clear_str(&out->content);
    cf_view_user_dispose(&out->booster);
    return rc;
}

static cf_err map_boosts(cf_ctx *ctx, int64_t message_id,
                         cf_view_boost_vector *out) {
    cf_boost_vector rows = {0};
    cf_err rc = cf_boost_for_message_ordered(ctx->reader, message_id, &rows);
    if (rc != CF_OK) return rc;
    if (rows.len != 0) {
        out->items = calloc(rows.len, sizeof *out->items);
        if (out->items == NULL) {
            cf_boost_vector_dispose(&rows);
            return CF_NOMEM;
        }
        out->cap = rows.len;
    }
    for (size_t i = 0; i < rows.len; i++) {
        rc = map_boost(ctx, &rows.items[i], &out->items[i]);
        if (rc != CF_OK) {
            out->len = i;
            cf_boost_vector_dispose(&rows);
            cf_view_boost_vector_dispose(out);
            return rc;
        }
        out->len = i + 1;
    }
    cf_boost_vector_dispose(&rows);
    return CF_OK;
}

/* The message's content arm: attachment (S02-blocked), sound, presentation
 * HTML or the unrenderable path. */
static cf_err map_content(cf_ctx *ctx, const cf_message *row,
                          cf_view_message *out, cf_span plain) {
    cf_str body = {0};
    bool found = false;
    cf_err rc = cf_message_body_html(ctx->reader, row, &found, &body);
    if (rc != CF_OK) return rc;
    if (!found) body = (cf_str){0};

    /* `message.attachment`: the AttachmentView needs S02's signed blob and
     * representation paths, which do not exist yet (05-storage S02 owns
     * src/storage/active_storage.c and the Active Storage routes).  Refuse
     * loudly instead of inventing a path. */
    {
        cf_attachment attachment = {0};
        cf_blob blob = {0};
        bool attached = false;
        rc = cf_message_attachment(ctx->reader, row, &attached, &attachment,
                                   &blob);
        cf_attachment_dispose(&attachment);
        cf_blob_dispose(&blob);
        if (rc != CF_OK) {
            cf_str_dispose(&body);
            return rc;
        }
        if (attached) {
            cf_str_dispose(&body);
            return CF_INVALID; /* S02 signed paths unavailable (reported). */
        }
    }

    const cf_sound *sound = NULL;
    if (cf_message_sound_in((cf_str){(char *)plain.ptr, plain.len}, &sound) &&
        sound != NULL) {
        out->content_kind = CF_VIEW_CONTENT_SOUND;
        cf_str sound_path = {0};
        rc = cf_sound_asset_path(sound, &sound_path);
        if (rc == CF_OK) {
            rc = asset_path((cf_span){
                                (const unsigned char *)sound_path.ptr,
                                sound_path.len},
                            &out->sound.url);
        }
        cf_str_dispose(&sound_path);
        if (rc == CF_OK && sound->image != NULL) {
            cf_str image_path = {0};
            rc = cf_sound_image_asset_path(sound->image, &image_path);
            if (rc == CF_OK) {
                rc = asset_path((cf_span){
                                    (const unsigned char *)image_path.ptr,
                                    image_path.len},
                                &out->sound.image.src);
            }
            cf_str_dispose(&image_path);
            if (rc == CF_OK) {
                out->sound.has_image = true;
                out->sound.image.width = sound->image->width;
                out->sound.image.height = sound->image->height;
            }
        }
        if (rc == CF_OK && sound->image == NULL && sound->text != NULL) {
            out->sound.has_text = true;
            rc = copy_span((cf_span){(const unsigned char *)sound->text,
                                     strlen(sound->text)},
                           &out->sound.text);
        }
        cf_str_dispose(&body);
        return rc;
    }

    cf_safe_html safe = {0};
    rc = cf_richtext_render(
        ctx, (cf_span){(const unsigned char *)body.ptr, body.len}, &safe);
    cf_str_dispose(&body);
    if (rc == CF_INVALID) {
        out->content_kind = CF_VIEW_CONTENT_UNRENDERABLE;
        return CF_OK;
    }
    if (rc != CF_OK) return rc;
    out->content_kind = CF_VIEW_CONTENT_TEXT;
    cf_span html = safe.bytes != NULL ? cf_buf_span(safe.bytes)
                                      : (cf_span){NULL, 0};
    rc = copy_span(html, &out->text_html);
    if (rc == CF_OK && out->text_html.ptr == NULL) {
        rc = copy_empty(&out->text_html);
    }
    cf_safe_html_dispose(&safe);
    return rc;
}

static cf_err map_message(cf_ctx *ctx, const cf_message *row,
                          cf_view_message *out) {
    memset(out, 0, sizeof *out);
    cf_err rc = CF_OK;
    cf_user creator = {0};
    rc = cf_user_find(ctx->reader, row->creator_id, &creator);
    if (rc == CF_NOT_FOUND) {
        /* `message_tag`'s rescue: a message whose creator is gone renders
         * messages/_unrenderable with the reference's empty creator view. */
        out->id = row->id;
        out->room_id = row->room_id;
        out->created_at_us = row->created_at;
        out->updated_at_us = row->updated_at;
        out->creator.id = row->creator_id;
        rc = copy_span((cf_span){
                           (const unsigned char *)row->client_message_id.ptr,
                           row->client_message_id.len},
                       &out->client_message_id);
        if (rc != CF_OK) return rc;
        rc = copy_empty(&out->creator.name);
        if (rc == CF_OK) rc = copy_empty(&out->creator.title);
        if (rc == CF_OK) rc = copy_empty(&out->creator.avatar_url);
        if (rc != CF_OK) {
            cf_view_message_dispose(out);
            return rc;
        }
        rc = room_name_for_nobody(ctx->reader, row->room_id, &out->room_name);
        if (rc != CF_OK) {
            cf_view_message_dispose(out);
            return rc;
        }
        out->content_kind = CF_VIEW_CONTENT_UNRENDERABLE;
        return CF_OK;
    }
    if (rc != CF_OK) return rc;

    out->id = row->id;
    out->room_id = row->room_id;
    out->created_at_us = row->created_at;
    out->updated_at_us = row->updated_at;
    rc = copy_span((cf_span){(const unsigned char *)row->client_message_id.ptr,
                             row->client_message_id.len},
                   &out->client_message_id);
    if (rc != CF_OK) goto fail;
    rc = room_name_for_nobody(ctx->reader, row->room_id, &out->room_name);
    if (rc != CF_OK) goto fail;
    rc = cf_presenter_user_view(ctx, &creator, &out->creator);
    if (rc != CF_OK) goto fail;

    {
        cf_str plain = {0};
        bool found_body = false;
        cf_str body = {0};
        rc = cf_message_body_html(ctx->reader, row, &found_body, &body);
        if (rc != CF_OK) goto fail;
        (void)found_body;
        const cf_richtext *rich_text = cf_tx_rich_text(NULL);
        rc = cf_richtext_to_plain_text(
            ctx->reader, rich_text,
            (cf_span){(const unsigned char *)body.ptr, body.len}, &plain);
        cf_str_dispose(&body);
        if (rc != CF_OK) goto fail;
        out->all_emoji =
            all_emoji((cf_span){(const unsigned char *)plain.ptr, plain.len});
        cf_str_dispose(&plain);
    }

    {
        cf_str body = {0};
        bool found_body = false;
        cf_str plain_text = {0};
        cf_richtext_plain_outcome outcome = CF_RICHTEXT_PLAIN_TEXT;
        rc = cf_message_body_html(ctx->reader, row, &found_body, &body);
        if (rc != CF_OK) goto fail;
        const cf_richtext *rich_text = cf_tx_rich_text(NULL);
        rc = cf_richtext_to_plain_text_outcome(
            ctx->reader, rich_text,
            (cf_span){(const unsigned char *)body.ptr, body.len},
            &plain_text, &outcome);
        cf_str_dispose(&body);
        if (rc != CF_OK) goto fail; /* CF_INTERNAL: the page fails (500). */
        if (outcome == CF_RICHTEXT_PLAIN_UNRENDERABLE) {
            out->content_kind = CF_VIEW_CONTENT_UNRENDERABLE;
        } else {
            rc = map_content(ctx, row, out, (cf_span){
                (const unsigned char *)plain_text.ptr, plain_text.len});
            if (rc != CF_OK) { cf_str_dispose(&plain_text); goto fail; }
        }
        cf_str_dispose(&plain_text);
    }
    if (out->content_kind == CF_VIEW_CONTENT_UNRENDERABLE) {
        out->all_emoji = false;
    }
    rc = map_boosts(ctx, row->id, &out->boosts);
    if (rc != CF_OK) goto fail;
    cf_user_dispose(&creator);
    return CF_OK;
fail:
    cf_user_dispose(&creator);
    cf_view_message_dispose(out);
    return rc;
}

/* ---------------------------------------------------------- public API */

cf_err cf_presenter_message(cf_ctx *ctx, const cf_message *row,
                            cf_view_message *out) {
    if (ctx == NULL || row == NULL || out == NULL) return CF_INVALID;
    cf_err rc = cf_read_begin(ctx->reader);
    if (rc != CF_OK) return rc;
    rc = map_message(ctx, row, out);
    if (rc != CF_OK) {
        cf_err end_rc = cf_read_end(ctx->reader);
        (void)end_rc;
        return rc;
    }
    return cf_read_end(ctx->reader);
}

cf_err cf_presenter_message_item(cf_ctx *ctx, const cf_message *row,
                                 cf_view_message_item *out) {
    if (ctx == NULL || row == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    return cf_presenter_message(ctx, row, &out->message);
}

cf_err cf_presenter_messages_in_transaction(
    cf_ctx *ctx, const cf_message *rows, size_t count,
    cf_view_message_item_vector *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    if (count != 0 && rows == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    cf_err rc = CF_OK;
    if (count != 0) {
        out->items = calloc(count, sizeof *out->items);
        if (out->items == NULL) {
            cf_err end_rc = cf_read_end(ctx->reader);
            (void)end_rc;
            return CF_NOMEM;
        }
        out->cap = count;
    }
    for (size_t i = 0; i < count; i++) {
        rc = map_message(ctx, &rows[i], &out->items[i].message);
        if (rc != CF_OK) {
            out->len = i;
            cf_view_message_item_vector_dispose(out);
            return rc;
        }
        out->len = i + 1;
    }
    return CF_OK;
}

cf_err cf_presenter_messages(cf_ctx *ctx, const cf_message *rows, size_t count,
                             cf_view_message_item_vector *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    cf_err rc = cf_read_begin(ctx->reader);
    if (rc != CF_OK) return rc;
    rc = cf_presenter_messages_in_transaction(ctx, rows, count, out);
    if (rc != CF_OK) {
        cf_err end_rc = cf_read_end(ctx->reader);
        (void)end_rc;
        return rc;
    }
    return cf_read_end(ctx->reader);
}

cf_err cf_presenter_message_edit(cf_ctx *ctx, const cf_message *row,
                                 cf_view_message_edit_model *out) {
    if (ctx == NULL || row == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    cf_err rc = cf_read_begin(ctx->reader);
    if (rc != CF_OK) return rc;
    rc = map_message(ctx, row, &out->message);
    if (rc == CF_OK) {
        cf_str body = {0};
        bool found = false;
        rc = cf_message_body_html(ctx->reader, row, &found, &body);
        if (rc == CF_OK) {
            bool editable_found = false;
            rc = cf_richtext_editable(
                ctx,
                (cf_span){(const unsigned char *)body.ptr, body.len},
                &editable_found, &out->editable_body_html);
            if (rc == CF_OK && !editable_found) {
                rc = copy_empty(&out->editable_body_html);
            }
        }
        cf_str_dispose(&body);
    }
    if (rc != CF_OK) {
        cf_view_message_edit_model_dispose(out);
        cf_err end_rc = cf_read_end(ctx->reader);
        (void)end_rc;
        return rc;
    }
    return cf_read_end(ctx->reader);
}

cf_err cf_presenter_boost(cf_ctx *ctx, const cf_boost *row,
                          cf_view_boost *out) {
    if (ctx == NULL || row == NULL || out == NULL) return CF_INVALID;
    cf_err rc = cf_read_begin(ctx->reader);
    if (rc != CF_OK) return rc;
    rc = map_boost(ctx, row, out);
    if (rc != CF_OK) {
        cf_err end_rc = cf_read_end(ctx->reader);
        (void)end_rc;
        return rc;
    }
    return cf_read_end(ctx->reader);
}
