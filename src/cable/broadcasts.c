/* src/cable/broadcasts.c — task C02: the exact Turbo payload source of
 * channels/broadcasts.rs. Every stream name, DOM target and <turbo-stream>
 * attribute is the reference's: records stream from their GID param, rooms
 * use their STI param_key (rooms_open / rooms_closed / rooms_direct), a
 * message's key is its client_message_id, and only `replace`/`append` for the
 * presentation and boost partials carry maintain_scroll="true".
 *
 * The HTML inside each stream comes from the caller's `Partials` (Rust's
 * trait): production passes A02's presenters and renderers through
 * cf_broadcast_partials_views, so no markup is duplicated here. Direct-room
 * creates render per recipient (the membership's own unread state), room
 * list partials render once and are shared by every recipient. Broadcasts
 * happen after the caller's commit; a CF_BUSY return is the post-commit
 * delivery failure the caller records (never a rollback). */
#include "cable/channels.h"

#include "views.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- dom_id and param_key -------------------------------------------------------- */

/* `Room.model_name.param_key` for the room's STI class ("rooms_open"). */
static cf_err room_param_key(const cf_room *room, cf_builder *out) {
    const char *class_name = cf_room_type_class_name(room->room_type);
    if (class_name == NULL) return CF_INVALID;
    cf_err rc = CF_OK;
    for (const char *p = class_name; *p != '\0' && rc == CF_OK; p++) {
        if (p[0] == ':' && p[1] == ':') {
            rc = cf_builder_append(out,
                                   (cf_span){(const unsigned char *)"_", 1});
            p++;
            continue;
        }
        unsigned char c = (unsigned char)*p;
        unsigned char lower = c >= 'A' && c <= 'Z'
                                  ? (unsigned char)(c - 'A' + 'a')
                                  : c;
        rc = cf_builder_append(out, (cf_span){&lower, 1});
    }
    return rc;
}

static cf_err dom_id(cf_span param_key, cf_span key, const char *prefix,
                     cf_builder *out) {
    cf_err rc = CF_OK;
    if (prefix != NULL) {
        rc = cf_builder_append(
            out, (cf_span){(const unsigned char *)prefix, strlen(prefix)});
        if (rc == CF_OK) {
            rc = cf_builder_append(
                out, (cf_span){(const unsigned char *)"_", 1});
        }
    }
    if (rc == CF_OK) rc = cf_builder_append(out, param_key);
    if (rc == CF_OK) {
        rc = cf_builder_append(out, (cf_span){(const unsigned char *)"_", 1});
    }
    if (rc == CF_OK) rc = cf_builder_append(out, key);
    return rc;
}

static cf_err room_dom_id(const cf_room *room, const char *prefix,
                          cf_str *out) {
    memset(out, 0, sizeof *out);
    cf_builder key = {0};
    cf_err rc = room_param_key(room, &key);
    char id_text[32];
    int id_len = snprintf(id_text, sizeof id_text, "%lld", (long long)room->id);
    if (rc == CF_OK && (id_len < 0 || (size_t)id_len >= sizeof id_text)) {
        rc = CF_INTERNAL;
    }
    cf_builder result = {0};
    if (rc == CF_OK) {
        rc = dom_id((cf_span){key.ptr, key.len},
                    (cf_span){(const unsigned char *)id_text, (size_t)id_len},
                    prefix, &result);
    }
    cf_builder_dispose(&key);
    if (rc != CF_OK) {
        cf_builder_dispose(&result);
        return rc;
    }
    char *text = malloc(result.len + 1);
    if (text == NULL) {
        cf_builder_dispose(&result);
        return CF_NOMEM;
    }
    memcpy(text, result.ptr, result.len);
    text[result.len] = '\0';
    *out = (cf_str){text, result.len};
    cf_builder_dispose(&result);
    return CF_OK;
}

static cf_err message_dom_id(const cf_message *message, const char *prefix,
                             cf_builder *out) {
    static const char key[] = "message";
    return dom_id((cf_span){(const unsigned char *)key, sizeof key - 1},
                  (cf_span){(const unsigned char *)message->client_message_id.ptr,
                            message->client_message_id.len},
                  prefix, out);
}

/* ---- turbo_stream_action_tag ------------------------------------------------------ */

/* The application's rendered-output cap for the stream tag the broadcasts
 * build (views.h's CF_VIEWS_MAX_OUTPUT, 03-application.md A02). Cable already
 * depends on the public views.h contract, so the constant is shared rather
 * than copied. The tag is rendered output like a view's: an append that would
 * cross the cap fails with CF_LIMIT and writes nothing, and
 * cf_broadcast_action_tag restores the builder to its entry length, so an
 * oversized payload is dropped at construction instead of being copied
 * without bound. The cable drop counters count bus drops inside
 * cf_cable_publish; a pre-publish build failure never reaches the bus and is
 * surfaced to the caller as CF_LIMIT (the same post-commit failure policy as
 * CF_BUSY). */
#define CF_BROADCASTS_MAX_OUTPUT CF_VIEWS_MAX_OUTPUT

/* Exact bytes cf_html_attr appends for `value` (R01's attribute table in
 * views/escape.c: `&` 5, `<`/`>` 4, `"` 6, `'` 5, everything else 1),
 * clamped once past the cap like render.c's escaped_size so adversarial
 * lengths cannot overflow. */
static size_t broadcast_attr_size(cf_span value) {
    size_t total = 0;
    for (size_t i = 0; i < value.len; i++) {
        switch (value.ptr[i]) {
        case '&': total += 5; break;            /* &amp; */
        case '<': case '>': total += 4; break;  /* &lt; / &gt; */
        case '"': total += 6; break;            /* &quot; */
        case '\'': total += 5; break;           /* &#39; */
        default: total += 1; break;
        }
        if (total > CF_BROADCASTS_MAX_OUTPUT) return total;
    }
    return total;
}

/* Cap-checked appends (cf_view_raw / cf_view_html_attr semantics): on
 * CF_LIMIT nothing is written, and an empty append stays a no-op even at the
 * cap. */
static cf_err broadcast_raw(cf_builder *out, cf_span bytes) {
    if (bytes.len != 0 && bytes.ptr == NULL) return CF_INVALID;
    if (bytes.len != 0 &&
        (out->len >= CF_BROADCASTS_MAX_OUTPUT ||
         bytes.len > CF_BROADCASTS_MAX_OUTPUT - out->len)) {
        return CF_LIMIT;
    }
    return cf_builder_append(out, bytes);
}

static cf_err broadcast_attr(cf_builder *out, cf_span value) {
    if (value.len != 0 && value.ptr == NULL) return CF_INVALID;
    if (value.len != 0) {
        if (out->len > CF_BROADCASTS_MAX_OUTPUT) return CF_LIMIT;
        if (broadcast_attr_size(value) > CF_BROADCASTS_MAX_OUTPUT - out->len) {
            return CF_LIMIT;
        }
    }
    return cf_html_attr(out, value);
}

cf_err cf_broadcast_action_tag(cf_builder *out, cf_span action,
                               bool has_target, cf_span target,
                               bool has_template, cf_span html,
                               bool maintain_scroll) {
    if (out == NULL) return CF_INVALID;
    size_t mark = out->len;
    cf_err rc = broadcast_raw(
        out, (cf_span){(const unsigned char *)"<turbo-stream", 13});
    if (rc == CF_OK && maintain_scroll) {
        rc = broadcast_raw(
            out, (cf_span){(const unsigned char *)" maintain_scroll=\"true\"",
                           23});
    }
    if (rc == CF_OK) {
        rc = broadcast_raw(out,
                           (cf_span){(const unsigned char *)" action=\"", 9});
    }
    if (rc == CF_OK) rc = broadcast_attr(out, action);
    if (rc == CF_OK) {
        rc = broadcast_raw(out, (cf_span){(const unsigned char *)"\"", 1});
    }
    if (rc == CF_OK && has_target) {
        rc = broadcast_raw(out,
                           (cf_span){(const unsigned char *)" target=\"", 9});
        if (rc == CF_OK) rc = broadcast_attr(out, target);
        if (rc == CF_OK) {
            rc = broadcast_raw(out, (cf_span){(const unsigned char *)"\"", 1});
        }
    }
    if (rc == CF_OK) {
        rc = broadcast_raw(out, (cf_span){(const unsigned char *)">", 1});
    }
    bool with_template = !(action.len == 6 &&
                           memcmp(action.ptr, "remove", 6) == 0) &&
                         !(action.len == 7 &&
                           memcmp(action.ptr, "refresh", 7) == 0);
    if (rc == CF_OK && with_template) {
        rc = broadcast_raw(
            out, (cf_span){(const unsigned char *)"<template>", 10});
        if (rc == CF_OK && has_template) rc = broadcast_raw(out, html);
        if (rc == CF_OK) {
            rc = broadcast_raw(
                out, (cf_span){(const unsigned char *)"</template>", 11});
        }
    }
    if (rc == CF_OK) {
        rc = broadcast_raw(
            out, (cf_span){(const unsigned char *)"</turbo-stream>", 15});
    }
    if (rc != CF_OK) out->len = mark;
    return rc;
}

/* ---- streamables and publish ------------------------------------------------------ */

static bool blank(cf_span part) {
    for (size_t i = 0; i < part.len; i++) {
        unsigned char c = part.ptr[i];
        if (!(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
              c == '\f')) {
            return false;
        }
    }
    return true;
}

/* Turbo::StreamsChannel.broadcast_*_to: blank streamables are dropped and
 * nothing is sent when none remain; the stream is the parts joined by ':',
 * the payload the Active Support JSON string of `html`. */
static cf_err turbo_publish(cf_cable *cable, const cf_span *streamables,
                            size_t count, cf_span action, cf_span target,
                            bool has_target, bool has_template, cf_span html,
                            bool maintain_scroll) {
    cf_builder stream = {0};
    cf_err rc = CF_OK;
    bool first = true;
    for (size_t i = 0; i < count && rc == CF_OK; i++) {
        if (blank(streamables[i])) continue;
        if (!first) {
            rc = cf_builder_append(
                &stream, (cf_span){(const unsigned char *)":", 1});
            if (rc != CF_OK) break;
        }
        rc = cf_builder_append(&stream, streamables[i]);
        first = false;
    }
    if (rc != CF_OK || first) {
        cf_builder_dispose(&stream);
        return rc;
    }
    cf_builder tag = {0};
    rc = cf_broadcast_action_tag(&tag, action, has_target, target, has_template,
                                 html, maintain_scroll);
    if (rc == CF_LIMIT) {
        /* An over-cap build is a dropped broadcast: logged sanitized (no
         * identifiers), never handed to cf_cable_publish, and returned to the
         * caller like the pubsub's CF_BUSY drops. */
        cf_cable_log("dropped broadcast", "output cap");
    }
    cf_builder payload = {0};
    if (rc == CF_OK) {
        rc = cf_json_string(&payload, (cf_span){tag.ptr, tag.len});
    }
    cf_builder_dispose(&tag);
    if (rc == CF_OK) {
        cf_buf *buf = NULL;
        rc = cf_buf_copy((cf_span){payload.ptr, payload.len}, &buf);
        if (rc == CF_OK) {
            rc = cf_cable_publish(cable, (cf_span){stream.ptr, stream.len}, buf);
            cf_buf_release(buf);
        }
    }
    cf_builder_dispose(&payload);
    cf_builder_dispose(&stream);
    return rc;
}

/* The two streamables of `turbo_stream_from @room, :messages`. */
static cf_err room_messages_streams(const cf_room *room, cf_str *gid,
                                    cf_span *parts) {
    static const char messages[] = "messages";
    cf_err rc = cf_cable_room_gid_param(room, gid);
    if (rc != CF_OK) return rc;
    parts[0] = (cf_span){(const unsigned char *)gid->ptr, gid->len};
    parts[1] = (cf_span){(const unsigned char *)messages, sizeof messages - 1};
    return CF_OK;
}

/* `[user_gid.to_param, "rooms"]`. */
static cf_err user_rooms_streams(int64_t user_id, cf_str *gid,
                                 cf_span *parts) {
    static const char rooms[] = "rooms";
    cf_err rc = cf_cable_user_gid_param(user_id, gid);
    if (rc != CF_OK) return rc;
    parts[0] = (cf_span){(const unsigned char *)gid->ptr, gid->len};
    parts[1] = (cf_span){(const unsigned char *)rooms, sizeof rooms - 1};
    return rc;
}

static const cf_span ROOMS_PARTS[] = {
    {(const unsigned char *)"rooms", 5},
};

static cf_err to_stream(const cf_span *streamables, size_t count,
                        cf_cable *cable, const char *action,
                        const cf_str *target, const cf_builder *html,
                        bool maintain_scroll) {
    cf_span action_span = {(const unsigned char *)action, strlen(action)};
    return turbo_publish(
        cable, streamables, count, action_span,
        target != NULL ? (cf_span){(const unsigned char *)target->ptr,
                                   target->len}
                       : (cf_span){NULL, 0},
        target != NULL, html != NULL,
        html != NULL ? (cf_span){html->ptr, html->len} : (cf_span){NULL, 0},
        maintain_scroll);
}

/* ---- Partials --------------------------------------------------------------------- */

static cf_err partial_missing(const char *name) {
    cf_cable_log("broadcast partial unavailable", name);
    return CF_NOT_FOUND;
}

static cf_err partial_call(cf_err (*fn)(void *, const cf_message *,
                                        cf_builder *),
                           void *user, const cf_message *message,
                           cf_builder *out, const char *name) {
    if (fn == NULL) return partial_missing(name);
    return fn(user, message, out);
}

/* ---- the broadcasts (channels/broadcasts.rs) --------------------------------------- */

cf_err cf_broadcast_read_room(cf_cable *cable, int64_t user_id,
                              int64_t room_id) {
    if (cable == NULL) return CF_INVALID;
    cf_builder stream = {0};
    char stream_text[64];
    int len = snprintf(stream_text, sizeof stream_text, "user_%lld_reads",
                       (long long)user_id);
    if (len < 0 || (size_t)len >= sizeof stream_text) return CF_INTERNAL;
    cf_err rc = cf_builder_append(
        &stream, (cf_span){(const unsigned char *)stream_text, (size_t)len});
    char payload_text[64];
    int payload_len = snprintf(payload_text, sizeof payload_text,
                               "{\"room_id\":%lld}", (long long)room_id);
    if (rc == CF_OK &&
        (payload_len < 0 || (size_t)payload_len >= sizeof payload_text)) {
        rc = CF_INTERNAL;
    }
    if (rc == CF_OK) {
        cf_buf *buf = NULL;
        rc = cf_buf_copy((cf_span){(const unsigned char *)payload_text,
                                   (size_t)payload_len},
                         &buf);
        if (rc == CF_OK) {
            rc = cf_cable_publish(cable,
                                  (cf_span){stream.ptr, stream.len}, buf);
            cf_buf_release(buf);
        }
    }
    cf_builder_dispose(&stream);
    return rc;
}

cf_err cf_broadcast_unread_room(cf_db *db, cf_cable *cable,
                                const cf_room *room) {
    if (db == NULL || cable == NULL || room == NULL) return CF_INVALID;
    cf_membership_vector memberships = {0};
    cf_err rc = cf_membership_for_room(db, room->id, &memberships);
    cf_err result = rc;
    for (size_t i = 0; i < memberships.len; i++) {
        cf_builder stream = {0};
        char stream_text[64];
        int len = snprintf(stream_text, sizeof stream_text, "user_%lld_unreads",
                           (long long)memberships.items[i].user_id);
        if (len < 0 || (size_t)len >= sizeof stream_text) {
            result = CF_INTERNAL;
            break;
        }
        cf_err item_rc = cf_builder_append(
            &stream, (cf_span){(const unsigned char *)stream_text, (size_t)len});
        char payload_text[64];
        int payload_len = snprintf(payload_text, sizeof payload_text,
                                   "{\"roomId\":%lld}", (long long)room->id);
        if (item_rc == CF_OK &&
            (payload_len < 0 || (size_t)payload_len >= sizeof payload_text)) {
            item_rc = CF_INTERNAL;
        }
        if (item_rc == CF_OK) {
            cf_buf *buf = NULL;
            item_rc = cf_buf_copy((cf_span){(const unsigned char *)payload_text,
                                            (size_t)payload_len},
                                  &buf);
            if (item_rc == CF_OK) {
                item_rc = cf_cable_publish(
                    cable, (cf_span){stream.ptr, stream.len}, buf);
                cf_buf_release(buf);
            }
        }
        cf_builder_dispose(&stream);
        if (item_rc != CF_OK && result == CF_OK) result = item_rc;
    }
    cf_membership_vector_dispose(&memberships);
    return result;
}

cf_err cf_broadcast_message_create(cf_db *db, cf_cable *cable,
                                   const cf_room *room,
                                   const cf_message *message,
                                   const cf_broadcast_partials *partials) {
    if (cable == NULL || room == NULL || message == NULL || partials == NULL) {
        return CF_INVALID;
    }
    cf_builder html = {0};
    cf_err rc = partial_call(partials->message, partials->user, message, &html,
                             "messages/_message");
    cf_str gid = {0};
    cf_span parts[2];
    cf_str target = {0};
    if (rc == CF_OK) rc = room_messages_streams(room, &gid, parts);
    if (rc == CF_OK) rc = room_dom_id(room, "messages", &target);
    cf_err append = CF_OK;
    if (rc == CF_OK) {
        append = to_stream(parts, 2, cable, "append", &target, &html, false);
    }
    /* `message.broadcast_create` always tells the members' unread streams,
     * even when the room append hit a full loop queue. */
    cf_err unread = cf_broadcast_unread_room(db, cable, room);
    cf_str_dispose(&gid);
    cf_str_dispose(&target);
    cf_builder_dispose(&html);
    if (rc != CF_OK) return rc;
    return append != CF_OK ? append : unread;
}

cf_err cf_broadcast_message_remove(cf_cable *cable, const cf_room *room,
                                   const cf_message *message) {
    if (cable == NULL || room == NULL || message == NULL) return CF_INVALID;
    cf_str gid = {0};
    cf_span parts[2];
    cf_err rc = room_messages_streams(room, &gid, parts);
    cf_builder target = {0};
    if (rc == CF_OK) rc = message_dom_id(message, NULL, &target);
    if (rc == CF_OK) {
        rc = to_stream(parts, 2, cable, "remove",
                       &(cf_str){(char *)target.ptr, target.len}, NULL, false);
    }
    cf_str_dispose(&gid);
    cf_builder_dispose(&target);
    return rc;
}

cf_err cf_broadcast_message_replace(cf_cable *cable, const cf_room *room,
                                    const cf_message *message,
                                    const cf_broadcast_partials *partials) {
    if (cable == NULL || room == NULL || message == NULL || partials == NULL) {
        return CF_INVALID;
    }
    cf_builder html = {0};
    cf_err rc = partial_call(partials->message_presentation, partials->user,
                             message, &html, "messages/_presentation");
    cf_str gid = {0};
    cf_span parts[2];
    cf_builder target = {0};
    if (rc == CF_OK) rc = room_messages_streams(room, &gid, parts);
    if (rc == CF_OK) rc = message_dom_id(message, "presentation", &target);
    if (rc == CF_OK) {
        rc = to_stream(parts, 2, cable, "replace",
                       &(cf_str){(char *)target.ptr, target.len}, &html, true);
    }
    cf_str_dispose(&gid);
    cf_builder_dispose(&target);
    cf_builder_dispose(&html);
    return rc;
}

cf_err cf_broadcast_boost_create(cf_cable *cable, const cf_room *room,
                                 const cf_message *message,
                                 const cf_boost *boost,
                                 const cf_broadcast_partials *partials) {
    if (cable == NULL || room == NULL || message == NULL || boost == NULL ||
        partials == NULL) {
        return CF_INVALID;
    }
    cf_builder html = {0};
    cf_err rc = partials->boost != NULL
                    ? partials->boost(partials->user, boost, &html)
                    : partial_missing("messages/boosts/_boost");
    cf_str gid = {0};
    cf_span parts[2];
    cf_builder target = {0};
    if (rc == CF_OK) rc = room_messages_streams(room, &gid, parts);
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &target, (cf_span){(const unsigned char *)"boosts_message_", 15});
        if (rc == CF_OK) {
            rc = cf_builder_append(
                &target,
                (cf_span){(const unsigned char *)message->client_message_id.ptr,
                          message->client_message_id.len});
        }
    }
    if (rc == CF_OK) {
        rc = to_stream(parts, 2, cable, "append",
                       &(cf_str){(char *)target.ptr, target.len}, &html, true);
    }
    cf_str_dispose(&gid);
    cf_builder_dispose(&target);
    cf_builder_dispose(&html);
    return rc;
}

cf_err cf_broadcast_boost_remove(cf_cable *cable, const cf_room *room,
                                 const cf_boost *boost) {
    if (cable == NULL || room == NULL || boost == NULL) return CF_INVALID;
    cf_str gid = {0};
    cf_span parts[2];
    cf_err rc = room_messages_streams(room, &gid, parts);
    cf_builder target = {0};
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &target, (cf_span){(const unsigned char *)"boost_", 6});
    }
    char id_text[32];
    int id_len = snprintf(id_text, sizeof id_text, "%lld", (long long)boost->id);
    if (rc == CF_OK && (id_len < 0 || (size_t)id_len >= sizeof id_text)) {
        rc = CF_INTERNAL;
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &target, (cf_span){(const unsigned char *)id_text, (size_t)id_len});
    }
    if (rc == CF_OK) {
        rc = to_stream(parts, 2, cable, "remove",
                       &(cf_str){(char *)target.ptr, target.len}, NULL, false);
    }
    cf_str_dispose(&gid);
    cf_builder_dispose(&target);
    return rc;
}

cf_err cf_broadcast_room_remove(cf_cable *cable, const cf_room *room) {
    if (cable == NULL || room == NULL) return CF_INVALID;
    cf_str target = {0};
    cf_err rc = room_dom_id(room, "list", &target);
    if (rc == CF_OK) {
        rc = to_stream(ROOMS_PARTS, 1, cable, "remove", &target, NULL, false);
    }
    cf_str_dispose(&target);
    return rc;
}

cf_err cf_broadcast_open_room_create(cf_cable *cable, const cf_room *room,
                                     const cf_broadcast_partials *partials) {
    if (cable == NULL || room == NULL || partials == NULL) return CF_INVALID;
    cf_builder html = {0};
    cf_err rc = partials->shared_room != NULL
                    ? partials->shared_room(partials->user, room, &html)
                    : partial_missing("users/sidebars/rooms/_shared");
    static const char target[] = "shared_rooms";
    if (rc == CF_OK) {
        rc = to_stream(ROOMS_PARTS, 1, cable, "prepend",
                       &(cf_str){(char *)target, sizeof target - 1}, &html,
                       false);
    }
    cf_builder_dispose(&html);
    return rc;
}

cf_err cf_broadcast_open_room_update(cf_cable *cable, const cf_room *room,
                                     const cf_broadcast_partials *partials) {
    if (cable == NULL || room == NULL || partials == NULL) return CF_INVALID;
    cf_builder html = {0};
    cf_err rc = partials->shared_room != NULL
                    ? partials->shared_room(partials->user, room, &html)
                    : partial_missing("users/sidebars/rooms/_shared");
    cf_str target = {0};
    if (rc == CF_OK) rc = room_dom_id(room, "list", &target);
    if (rc == CF_OK) {
        rc = to_stream(ROOMS_PARTS, 1, cable, "replace", &target, &html, false);
    }
    cf_str_dispose(&target);
    cf_builder_dispose(&html);
    return rc;
}

cf_err cf_broadcast_closed_room_create(cf_db *db, cf_cable *cable,
                                       const cf_room *room,
                                       const cf_broadcast_partials *partials) {
    if (db == NULL || cable == NULL || room == NULL || partials == NULL) {
        return CF_INVALID;
    }
    cf_builder html = {0};
    cf_err rc = partials->shared_room != NULL
                    ? partials->shared_room(partials->user, room, &html)
                    : partial_missing("users/sidebars/rooms/_shared");
    static const char target[] = "shared_rooms";
    cf_int64_vector user_ids = {0};
    if (rc == CF_OK) rc = cf_room_user_ids(db, room, &user_ids);
    cf_err result = rc;
    for (size_t i = 0; i < user_ids.len; i++) {
        cf_str gid = {0};
        cf_span parts[2];
        cf_err item_rc = user_rooms_streams(user_ids.items[i], &gid, parts);
        if (item_rc == CF_OK) {
            item_rc = to_stream(parts, 2, cable, "prepend",
                                &(cf_str){(char *)target, sizeof target - 1},
                                &html, false);
        }
        cf_str_dispose(&gid);
        if (item_rc != CF_OK && result == CF_OK) result = item_rc;
    }
    cf_int64_vector_dispose(&user_ids);
    cf_builder_dispose(&html);
    return result;
}

cf_err cf_broadcast_closed_room_update(cf_db *db, cf_cable *cable,
                                       const cf_room *room,
                                       const cf_broadcast_partials *partials) {
    if (db == NULL || cable == NULL || room == NULL || partials == NULL) {
        return CF_INVALID;
    }
    cf_builder html = {0};
    cf_err rc = partials->shared_room != NULL
                    ? partials->shared_room(partials->user, room, &html)
                    : partial_missing("users/sidebars/rooms/_shared");
    cf_str target = {0};
    cf_int64_vector user_ids = {0};
    if (rc == CF_OK) rc = room_dom_id(room, "list", &target);
    if (rc == CF_OK) rc = cf_room_user_ids(db, room, &user_ids);
    cf_err result = rc;
    for (size_t i = 0; i < user_ids.len; i++) {
        cf_str gid = {0};
        cf_span parts[2];
        cf_err item_rc = user_rooms_streams(user_ids.items[i], &gid, parts);
        if (item_rc == CF_OK) {
            item_rc = to_stream(parts, 2, cable, "replace", &target, &html,
                                false);
        }
        cf_str_dispose(&gid);
        if (item_rc != CF_OK && result == CF_OK) result = item_rc;
    }
    cf_int64_vector_dispose(&user_ids);
    cf_str_dispose(&target);
    cf_builder_dispose(&html);
    return result;
}

cf_err cf_broadcast_direct_room_create(cf_db *db, cf_cable *cable,
                                       const cf_room *room,
                                       const cf_broadcast_partials *partials) {
    if (db == NULL || cable == NULL || room == NULL || partials == NULL) {
        return CF_INVALID;
    }
    static const char target[] = "direct_rooms";
    cf_membership_vector memberships = {0};
    cf_err rc = cf_membership_for_room(db, room->id, &memberships);
    cf_err result = rc;
    for (size_t i = 0; i < memberships.len; i++) {
        cf_builder html = {0};
        cf_err item_rc = partials->direct_room != NULL
                             ? partials->direct_room(partials->user,
                                                     &memberships.items[i], &html)
                             : partial_missing("users/sidebars/rooms/_direct");
        cf_str gid = {0};
        cf_span parts[2];
        if (item_rc == CF_OK) {
            item_rc = user_rooms_streams(memberships.items[i].user_id, &gid,
                                         parts);
        }
        if (item_rc == CF_OK) {
            item_rc = to_stream(parts, 2, cable, "prepend",
                                &(cf_str){(char *)target, sizeof target - 1},
                                &html, false);
        }
        cf_str_dispose(&gid);
        cf_builder_dispose(&html);
        if (item_rc != CF_OK && result == CF_OK) result = item_rc;
    }
    cf_membership_vector_dispose(&memberships);
    return result;
}

cf_err cf_broadcast_involvement_change(
    cf_cable *cable, const cf_room *room, const cf_membership *membership,
    bool has_previous, cf_involvement previous,
    const cf_broadcast_partials *partials) {
    if (cable == NULL || room == NULL || membership == NULL ||
        partials == NULL) {
        return CF_INVALID;
    }
    if (cf_room_direct(room)) return CF_OK;
    cf_str gid = {0};
    cf_span parts[2];
    cf_err rc = user_rooms_streams(membership->user_id, &gid, parts);
    if (rc != CF_OK) {
        cf_str_dispose(&gid);
        return rc;
    }
    bool invisible = cf_membership_involved_in(membership,
                                               CF_INVOLVEMENT_INVISIBLE);
    if (invisible) {
        cf_str target = {0};
        rc = room_dom_id(room, "list", &target);
        if (rc == CF_OK) {
            rc = to_stream(parts, 2, cable, "remove", &target, NULL, false);
        }
        cf_str_dispose(&target);
    } else if (!has_previous) {
        /* nil.inquiry: NoMethodError after the update has saved. */
        cf_cable_log("could not execute command", "nil inquiry");
        rc = CF_INVALID;
    } else if (previous == CF_INVOLVEMENT_INVISIBLE) {
        cf_builder html = {0};
        rc = partials->shared_room != NULL
                 ? partials->shared_room(partials->user, room, &html)
                 : partial_missing("users/sidebars/rooms/_shared");
        static const char target[] = "shared_rooms";
        if (rc == CF_OK) {
            rc = to_stream(parts, 2, cable, "prepend",
                           &(cf_str){(char *)target, sizeof target - 1},
                           &html, false);
        }
        cf_builder_dispose(&html);
    }
    cf_str_dispose(&gid);
    return rc;
}

/* ---- production partials ------------------------------------------------------------ */

static cf_err views_message(void *user, const cf_message *message,
                            cf_builder *out) {
    cf_broadcast_views *views = user;
    cf_view_message view;
    cf_err rc = cf_presenter_message(views->ctx, message, &view);
    if (rc == CF_OK) {
        rc = cf_view_message_partial(views->view, &view, out);
    }
    cf_view_message_dispose(&view);
    return rc;
}

static cf_err views_message_presentation(void *user, const cf_message *message,
                                         cf_builder *out) {
    cf_broadcast_views *views = user;
    cf_view_message view;
    cf_err rc = cf_presenter_message(views->ctx, message, &view);
    if (rc == CF_OK) {
        rc = cf_view_message_presentation(views->view, &view, out);
    }
    cf_view_message_dispose(&view);
    return rc;
}

static cf_err views_boost(void *user, const cf_boost *boost,
                          cf_builder *out) {
    cf_broadcast_views *views = user;
    cf_view_boost view;
    cf_err rc = cf_presenter_boost(views->ctx, boost, &view);
    if (rc == CF_OK) {
        rc = cf_view_boost_partial(views->view, &view, out);
    }
    cf_str_dispose(&view.content);
    cf_view_user_dispose(&view.booster);
    return rc;
}

void cf_broadcast_partials_views(cf_broadcast_partials *out,
                                 cf_broadcast_views *views) {
    if (out == NULL) return;
    memset(out, 0, sizeof *out);
    if (views == NULL) return;
    out->message = views_message;
    out->message_presentation = views_message_presentation;
    out->boost = views_boost;
    /* The sidebar room partials are the A02 renderers behind the reference's
     * `Partials` trait (rooms.rs render_shared_room / directs.rs
     * broadcast_create_room); they take the cf_broadcast_views pair as their
     * `user`.  shared_room maps the room row once (unread=false), while
     * direct_room is called once per membership by
     * cf_broadcast_direct_room_create, so each recipient's own unread state
     * is rendered. */
    out->shared_room = cf_view_rooms_shared_room_partial;
    out->direct_room = cf_view_rooms_direct_room_partial;
    out->user = views;
}
