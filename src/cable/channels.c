/* src/cable/channels.c — task C02: the eight application channels and the
 * Action Cable command dispatch. Sources: tmp/rust-ref/crates/campfire/src/
 * channels.rs (registration), channels/{presence,read_rooms,room,
 * room_messages,typing_notifications,unread_rooms}.rs (per-channel behavior)
 * and crates/cable/src/{connection.rs,channel.rs,turbo.rs} (subscribe /
 * perform / unsubscribe semantics and bounds).
 *
 * Every subscription lives in the connection loop's array and carries the
 * client's raw identifier (echoed byte for byte in confirmations, rejections
 * and deliveries), its canonical class name and the streams it reads. On the
 * production reactor path the `subscribed` model validation runs on the app's
 * bounded request-worker pool (its result installed through the C03 gate and
 * its deferred presence effect run as a second worker phase); the synchronous
 * path (standalone sockets, tests) runs the identical plan-based validation
 * on the owner thread with the loop's own reader. The confirmation is written
 * only after those effects succeeded. Presence membership changes go through
 * cf_write and broadcast after the write.
 *
 * Unknown channels/actions, duplicate subscribe, unsubscribe of an unknown
 * identifier and malformed identifiers/commands are logged and ignored, as
 * the reference connection.rs does; a `subscribed` that raises leaves the
 * subscription registered and sends neither confirmation nor rejection. */
#include "cable/channels.h"

#include "app.h"
#include "auth.h"
#include "db/db_internal.h"

#include <yyjson.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- channel registry ------------------------------------------------------- */

typedef enum {
    CH_EMPTY = 0, /* ApplicationCable::Channel, HeartbeatChannel */
    CH_PRESENCE,
    CH_READ_ROOMS,
    CH_ROOM,
    CH_ROOM_MESSAGES,
    CH_TYPING,
    CH_UNREAD_ROOMS,
    CH_TURBO_STREAMS
} channel_kind;

typedef struct {
    channel_kind kind;
    bool has_room;
    cf_room room;
} channel_state;

/* The exact registered names (channels.rs `register`). */
static const struct {
    const char *name;
    channel_kind kind;
} CHANNEL_CLASSES[] = {
    {"ApplicationCable::Channel", CH_EMPTY},
    {"HeartbeatChannel", CH_EMPTY},
    {"PresenceChannel", CH_PRESENCE},
    {"ReadRoomsChannel", CH_READ_ROOMS},
    {"RoomChannel", CH_ROOM},
    {"RoomMessagesChannel", CH_ROOM_MESSAGES},
    {"TypingNotificationsChannel", CH_TYPING},
    {"UnreadRoomsChannel", CH_UNREAD_ROOMS},
    {"Turbo::StreamsChannel", CH_TURBO_STREAMS},
};

static const size_t CHANNEL_CLASSES_LEN =
    sizeof CHANNEL_CLASSES / sizeof CHANNEL_CLASSES[0];

/* `safe_constantize` resolves a leading "::" to the same class, whose
 * broadcastings are named after the class alone (server.rs `channel`). */
static bool class_for(const char *requested, channel_kind *kind,
                      const char **canonical) {
    if (requested == NULL) requested = "";
    if (requested[0] == ':' && requested[1] == ':') requested += 2;
    for (size_t i = 0; i < CHANNEL_CLASSES_LEN; i++) {
        if (strcmp(requested, CHANNEL_CLASSES[i].name) == 0) {
            *kind = CHANNEL_CLASSES[i].kind;
            *canonical = CHANNEL_CLASSES[i].name;
            return true;
        }
    }
    return false;
}

/* `ActiveSupport::Inflector.underscore` without acronym inflections, as
 * naming.rs channel_name: delete the "Channel" suffix, "::" -> ":", then
 * underscore. */
static cf_err channel_name(cf_span class_name, cf_builder *out) {
    cf_span body = class_name;
    static const char suffix[] = "Channel";
    if (body.len >= sizeof suffix - 1 &&
        memcmp(body.ptr + body.len - (sizeof suffix - 1), suffix,
               sizeof suffix - 1) == 0) {
        body.len -= sizeof suffix - 1;
    }
    cf_err rc = CF_OK;
    for (size_t i = 0; i < body.len && rc == CF_OK; i++) {
        unsigned char c = body.ptr[i];
        if (c == ':' && i + 1 < body.len && body.ptr[i + 1] == ':') {
            /* naming.rs: `gsub("::", ":")`, so Turbo::StreamsChannel is
             * "turbo:streams", not "turbo/streams". */
            rc = cf_builder_append(out, (cf_span){(const unsigned char *)":", 1});
            i++;
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            unsigned char prev = i > 0 ? body.ptr[i - 1] : 0;
            unsigned char next = i + 1 < body.len ? body.ptr[i + 1] : 0;
            bool lower_before = (prev >= 'a' && prev <= 'z') ||
                                (prev >= '0' && prev <= '9');
            bool acronym_end = ((prev >= 'A' && prev <= 'Z') ||
                                (prev >= '0' && prev <= '9')) &&
                               next >= 'a' && next <= 'z';
            if (i > 0 && (lower_before || acronym_end)) {
                rc = cf_builder_append(out,
                                       (cf_span){(const unsigned char *)"_", 1});
                if (rc != CF_OK) break;
            }
            unsigned char lower = (unsigned char)(c - 'A' + 'a');
            rc = cf_builder_append(out, (cf_span){&lower, 1});
            continue;
        }
        unsigned char out_c = c == '-' ? '_' : c;
        rc = cf_builder_append(out, (cf_span){&out_c, 1});
    }
    return rc;
}

/* `broadcasting_for(class_name, [broadcastable])`: "<channel name>:<part>". */
static cf_err broadcasting_for(cf_span class_name, cf_span part,
                               cf_str *out) {
    cf_builder b = {0};
    cf_err rc = channel_name(class_name, &b);
    if (rc == CF_OK) {
        rc = cf_builder_append(&b, (cf_span){(const unsigned char *)":", 1});
    }
    if (rc == CF_OK) rc = cf_builder_append(&b, part);
    if (rc != CF_OK) {
        cf_builder_dispose(&b);
        return rc;
    }
    char *text = malloc(b.len + 1);
    if (text == NULL) {
        cf_builder_dispose(&b);
        return CF_NOMEM;
    }
    memcpy(text, b.ptr, b.len);
    text[b.len] = '\0';
    *out = (cf_str){text, b.len};
    cf_builder_dispose(&b);
    return CF_OK;
}

/* Test-only access to the exact naming.rs mapping (including namespaced
 * classes, which no production channel streams from today). */
cf_err cf_cable_test_broadcasting_for(const char *class_name, cf_span part,
                                      cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (class_name == NULL) return CF_INVALID;
    return broadcasting_for(
        (cf_span){(const unsigned char *)class_name, strlen(class_name)}, part,
        out);
}

/* ---- identifiers -------------------------------------------------------------- */

cf_err cf_cable_room_gid_param(const cf_room *room, cf_str *out) {
    if (room == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    const char *class_name = cf_room_type_class_name(room->room_type);
    if (class_name == NULL) return CF_INVALID;
    cf_builder uri = {0};
    char id_text[32];
    int id_len = snprintf(id_text, sizeof id_text, "%lld", (long long)room->id);
    if (id_len < 0 || (size_t)id_len >= sizeof id_text) {
        return CF_INTERNAL;
    }
    cf_err rc = cf_builder_append(
        &uri, (cf_span){(const unsigned char *)"gid://campfire/", 15});
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &uri, (cf_span){(const unsigned char *)class_name,
                            strlen(class_name)});
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(&uri, (cf_span){(const unsigned char *)"/", 1});
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &uri, (cf_span){(const unsigned char *)id_text, (size_t)id_len});
    }
    if (rc == CF_OK) {
        rc = cf_auth_global_id_param(
            (cf_span){uri.ptr, uri.len}, out);
    }
    cf_builder_dispose(&uri);
    return rc;
}

cf_err cf_cable_user_gid_param(int64_t user_id, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    char uri[64];
    int len = snprintf(uri, sizeof uri, "gid://campfire/User/%lld",
                       (long long)user_id);
    if (len < 0 || (size_t)len >= sizeof uri) return CF_INTERNAL;
    return cf_auth_global_id_param(
        (cf_span){(const unsigned char *)uri, (size_t)len}, out);
}

/* `Base64.urlsafe_decode64`: translate -_ to +/, pad a short unpadded string,
 * then a strict standard decode (rails_compat encoding.rs). */
static cf_err urlsafe_decode(cf_span encoded, cf_buf **out) {
    *out = NULL;
    size_t padded_len = encoded.len;
    bool has_pad = encoded.len != 0 && encoded.ptr[encoded.len - 1] == '=';
    if (!has_pad && encoded.len % 4 != 0) {
        padded_len += 4 - encoded.len % 4;
    }
    if (padded_len % 4 != 0 || padded_len > encoded.len + 3) {
        return CF_OK; /* not decodable; *out stays NULL */
    }
    cf_builder padded = {0};
    for (size_t i = 0; i < encoded.len; i++) {
        unsigned char c = encoded.ptr[i];
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        cf_err rc = cf_builder_append(&padded, (cf_span){&c, 1});
        if (rc != CF_OK) {
            cf_builder_dispose(&padded);
            return rc;
        }
    }
    while (padded.len < padded_len) {
        const unsigned char pad = '=';
        cf_err rc = cf_builder_append(&padded, (cf_span){&pad, 1});
        if (rc != CF_OK) {
            cf_builder_dispose(&padded);
            return rc;
        }
    }
    if (padded.len % 4 != 0) {
        cf_builder_dispose(&padded);
        return CF_OK;
    }
    /* Strict decode: a '=' is only legal as the final one or two characters,
     * and the unused trailing bits must be zero. */
    cf_builder decoded = {0};
    size_t i = 0;
    bool ok = true;
    while (i < padded.len) {
        int values[4];
        size_t pads = 0;
        for (size_t j = 0; j < 4; j++, i++) {
            unsigned char c = padded.ptr[i];
            if (c == '=') {
                if (j < 2) {
                    ok = false; /* partial padding */
                    break;
                }
                values[j] = -1;
                pads++;
                continue;
            }
            if (pads != 0) {
                ok = false; /* character after padding */
                break;
            }
            int v;
            if (c >= 'A' && c <= 'Z') v = c - 'A';
            else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
            else if (c >= '0' && c <= '9') v = c - '0' + 52;
            else if (c == '+') v = 62;
            else if (c == '/') v = 63;
            else {
                ok = false;
                break;
            }
            values[j] = v;
        }
        if (!ok || pads > 2) {
            ok = false;
            break;
        }
        unsigned char bytes[3];
        bytes[0] = (unsigned char)((values[0] << 2) | (values[1] >> 4));
        bytes[1] = (unsigned char)(((values[1] & 0xF) << 4) |
                                   ((values[2] >= 0 ? values[2] : 0) >> 2));
        bytes[2] = (unsigned char)(((values[2] >= 0 ? values[2] & 0x3 : 0) << 6) |
                                   (values[3] >= 0 ? values[3] : 0));
        size_t produced = pads == 0 ? 3 : (pads == 2 ? 1 : 2);
        if (pads == 2 && (values[1] & 0xF) != 0) ok = false;
        if (pads == 1 && (values[2] & 0x3) != 0) ok = false;
        if (!ok) break;
        cf_err rc = cf_builder_append(&decoded,
                                      (cf_span){bytes, produced});
        if (rc != CF_OK) {
            cf_builder_dispose(&padded);
            cf_builder_dispose(&decoded);
            return rc;
        }
    }
    cf_builder_dispose(&padded);
    if (!ok) {
        cf_builder_dispose(&decoded);
        return CF_OK;
    }
    /* Strict UTF-8 output: GlobalID.parse does String::from_utf8. */
    size_t pos = 0;
    while (pos < decoded.len) {
        unsigned char c = decoded.ptr[pos];
        size_t n = 1;
        if (c < 0x80) {
            n = 1;
        } else if ((c & 0xE0) == 0xC0) {
            n = 2;
        } else if ((c & 0xF0) == 0xE0) {
            n = 3;
        } else if ((c & 0xF8) == 0xF0) {
            n = 4;
        } else {
            ok = false;
            break;
        }
        if (pos + n > decoded.len) {
            ok = false;
            break;
        }
        for (size_t k = 1; k < n; k++) {
            if ((decoded.ptr[pos + k] & 0xC0) != 0x80) ok = false;
        }
        if (!ok) break;
        pos += n;
    }
    if (!ok) {
        cf_builder_dispose(&decoded);
        return CF_OK;
    }
    cf_buf *buf = NULL;
    cf_err rc = cf_buf_copy((cf_span){decoded.ptr, decoded.len}, &buf);
    cf_builder_dispose(&decoded);
    if (rc != CF_OK) return rc;
    *out = buf;
    return CF_OK;
}

/* `GlobalID::Locator.locate gid_param, only: Room`, with RecordNotFound as
 * nil and an unknown constant as an error (room_messages.rs). */
static const char *const KNOWN_MODELS[] = {
    "Account",  "Ban",     "Boost",   "Current",  "Membership",
    "Message",  "Push::Subscription", "Search", "Session", "User", "Webhook",
};

static cf_err room_from(cf_db *db, cf_span gid_param, bool *found,
                        cf_room *out) {
    *found = false;
    cf_str app = {0}, model = {0}, id = {0};
    bool parsed = false;
    cf_err rc = cf_auth_global_id_parse(gid_param, &app, &model, &id, &parsed);
    if (rc != CF_OK) return rc;
    if (!parsed) {
        cf_buf *decoded = NULL;
        rc = urlsafe_decode(gid_param, &decoded);
        if (rc != CF_OK) return rc;
        if (decoded != NULL) {
            cf_span text = cf_buf_span(decoded);
            rc = cf_auth_global_id_parse(text, &app, &model, &id, &parsed);
            cf_buf_release(decoded);
            if (rc != CF_OK) return rc;
        }
    }
    if (!parsed) return CF_OK;
    bool type_ok = false;
    cf_room_type required_type = CF_ROOM_OPEN;
    bool require_type = false;
    if (strcmp(model.ptr, "Room") == 0) {
        type_ok = true;
    } else if (cf_room_type_from_class_name(
                   (cf_str){model.ptr, model.len}, &required_type)) {
        require_type = true;
        type_ok = true;
    } else {
        for (size_t i = 0; i < sizeof KNOWN_MODELS / sizeof KNOWN_MODELS[0];
             i++) {
            if (strcmp(model.ptr, KNOWN_MODELS[i]) == 0) {
                type_ok = false;
                goto done;
            }
        }
        /* `model_name.constantize` raises NameError: logged, no reply. */
        cf_cable_log("could not execute command", "uninitialized constant");
        rc = CF_INVALID;
        goto done;
    }
    if (type_ok) {
        /* `gid.id.parse::<i64>()`: strict decimal with an optional sign. */
        const char *p = id.ptr;
        size_t len = id.len;
        bool negative = false;
        if (len != 0 && (*p == '+' || *p == '-')) {
            negative = *p == '-';
            p++;
            len--;
        }
        bool digits = len != 0;
        int64_t value = 0;
        for (size_t i = 0; i < len && digits; i++) {
            if (p[i] < '0' || p[i] > '9') {
                digits = false;
                break;
            }
            if (value > (INT64_MAX - (p[i] - '0')) / 10) {
                digits = false; /* out of range, as Rust's parse fails */
                break;
            }
            value = value * 10 + (p[i] - '0');
        }
        if (digits && negative) {
            value = -value;
        }
        if (digits) {
            bool room_found = false;
            rc = cf_room_find_by_id(db, value, &room_found, out);
            if (rc != CF_OK) goto done;
            if (room_found) {
                if (require_type && out->room_type != required_type) {
                    cf_room_dispose(out);
                } else {
                    *found = true;
                }
            }
        }
    }
done:
    cf_str_dispose(&app);
    cf_str_dispose(&model);
    cf_str_dispose(&id);
    return rc;
}

/* `subscribable_room(conn, user, stream_name)`. */
static cf_err subscribable_room(cf_db *db, int64_t user_id, cf_span stream_name,
                                bool *found, cf_room *out) {
    *found = false;
    if (stream_name.ptr == NULL || stream_name.len == 0) return CF_OK;
    const unsigned char *colon = memchr(stream_name.ptr, ':', stream_name.len);
    if (colon == NULL) return CF_OK;
    cf_span gid_param = {stream_name.ptr, (size_t)(colon - stream_name.ptr)};
    cf_span suffix = {colon + 1, stream_name.len - gid_param.len - 1};
    if (!(suffix.len == 8 && memcmp(suffix.ptr, "messages", 8) == 0)) {
        return CF_OK;
    }
    bool room_found = false;
    cf_room room;
    cf_err rc = room_from(db, gid_param, &room_found, &room);
    if (rc != CF_OK) return rc;
    if (!room_found) return CF_OK;
    rc = cf_room_find_for_user(db, user_id, room.id, found, out);
    cf_room_dispose(&room);
    return rc;
}

/* `RoomMessagesChannel.guarded_stream?`. */
static bool guarded_stream(cf_span name) {
    if (name.ptr == NULL || name.len == 0) return false;
    const unsigned char *colon = memchr(name.ptr, ':', name.len);
    if (colon == NULL) return false;
    cf_span suffix = {colon + 1, name.len - (size_t)(colon - name.ptr) - 1};
    return suffix.len == 8 && memcmp(suffix.ptr, "messages", 8) == 0;
}

/* params[:signed_stream_name] verified with the stock Turbo verifier; a
 * missing/null name is simply unverified, any other non-string raises. The
 * secret is explicit so the worker path verifies with the cable's borrowed
 * secret without touching the loop. */
static cf_err verified_stream_name(cf_span secret, yyjson_val *params,
                                   cf_str *out, bool *found) {
    *found = false;
    cf_str empty = {0};
    *out = empty;
    yyjson_val *value = yyjson_obj_get(params, "signed_stream_name");
    if (value == NULL || yyjson_is_null(value)) return CF_OK;
    if (!yyjson_is_str(value)) return CF_INVALID;
    cf_span signed_name = {(const unsigned char *)yyjson_get_str(value),
                           yyjson_get_len(value)};
    return cf_auth_turbo_verified_stream_name(secret, signed_name, out, found);
}

/* ---- small JSON helpers ---------------------------------------------------------- */

static const char *json_field_str(yyjson_val *object, const char *name) {
    yyjson_val *value = yyjson_obj_get(object, name);
    return value != NULL && yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
}

/* Ruby's ISSPACE: what String#to_i skips. */
static bool ruby_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
           c == '\r';
}

/* Rust char::is_whitespace's White_Space code points (yyjson strings are
 * valid UTF-8). connection.rs uses `action.trim().is_empty()`, so only a
 * whitespace-only action at all becomes "receive". */
static bool rust_white_space(uint32_t cp) {
    if (cp < 0x80) return ruby_space((unsigned char)cp);
    return cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

static bool action_blank(cf_span s) {
    size_t i = 0;
    while (i < s.len) {
        unsigned char c = s.ptr[i];
        if (c < 0x80) {
            if (!rust_white_space(c)) return false;
            i++;
            continue;
        }
        uint32_t cp;
        size_t need;
        if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            need = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            need = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            need = 3;
        } else {
            return false;
        }
        if (s.len - i - 1 < need) return false;
        for (size_t k = 0; k < need; k++) {
            unsigned char cc = s.ptr[i + 1 + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (uint32_t)(cc & 0x3F);
        }
        if (!rust_white_space(cp)) return false;
        i += need + 1;
    }
    return true;
}

/* ActiveModel::Type::Integer#serialize (ruby/src/integer.rs): nil unless it
 * starts like a number, then String#to_i, saturating; out of i64 is nil. */
static bool integer_cast(cf_span text, int64_t *out) {
    size_t i = 0;
    while (i < text.len && ruby_space(text.ptr[i])) i++;
    if (i < text.len && (text.ptr[i] == '+' || text.ptr[i] == '-')) i++;
    if (i >= text.len || text.ptr[i] < '0' || text.ptr[i] > '9') return false;
    /* String#to_i with 128-bit saturation, then the i64 conversion. */
    __int128 number = 0;
    bool negative = false;
    size_t j = 0;
    while (j < text.len && ruby_space(text.ptr[j])) j++;
    if (j < text.len && (text.ptr[j] == '+' || text.ptr[j] == '-')) {
        negative = text.ptr[j] == '-';
        j++;
    }
    if (j + 1 < text.len && text.ptr[j] == '0' &&
        (text.ptr[j + 1] == 'd' || text.ptr[j + 1] == 'D')) {
        j += 2;
    }
    bool previous_digit = false;
    for (; j < text.len; j++) {
        unsigned char c = text.ptr[j];
        if (c >= '0' && c <= '9') {
            number = number * 10 + (c - '0');
            if (number > ((__int128)1 << 100)) number = (__int128)1 << 100;
            previous_digit = true;
        } else if (c == '_' && previous_digit) {
            previous_digit = false;
        } else {
            break;
        }
    }
    if (negative) number = -number;
    if (number < (__int128)INT64_MIN || number > (__int128)INT64_MAX) {
        return false;
    }
    *out = (int64_t)number;
    return true;
}

/* How Active Record casts a value for an integer `id` condition (room.rs). */
static bool cast_id(yyjson_val *value, int64_t *out) {
    if (value == NULL) return false;
    if (yyjson_is_bool(value)) {
        *out = yyjson_get_bool(value) ? 1 : 0;
        return true;
    }
    if (yyjson_is_str(value)) {
        cf_span text = {(const unsigned char *)yyjson_get_str(value),
                        yyjson_get_len(value)};
        return integer_cast(text, out);
    }
    if (yyjson_is_int(value)) {
        *out = yyjson_get_sint(value);
        return true;
    }
    if (yyjson_is_uint(value)) {
        uint64_t u = yyjson_get_uint(value);
        if (u <= (uint64_t)INT64_MAX) {
            *out = (int64_t)u;
            return true;
        }
        double d = (double)u;
        if (isfinite(d) && fabs(d) < 9.2e18) {
            *out = (int64_t)trunc(d);
            return true;
        }
        return false;
    }
    if (yyjson_is_real(value)) {
        double d = yyjson_get_real(value);
        if (isfinite(d) && fabs(d) < 9.2e18) {
            *out = (int64_t)trunc(d);
            return true;
        }
    }
    return false;
}

/* ---- per-subscription effects ------------------------------------------------------ */

/* `RoomChannel#subscribed`'s stream_for: "<channel name>:<room gid param>". */
static cf_err broadcasting_for_room(const char *class_name, const cf_room *room,
                                    cf_str *out) {
    cf_str gid = {0};
    cf_err rc = cf_cable_room_gid_param(room, &gid);
    if (rc != CF_OK) return rc;
    cf_str stream = {0};
    rc = broadcasting_for(
        (cf_span){(const unsigned char *)class_name, strlen(class_name)},
        (cf_span){(const unsigned char *)gid.ptr, gid.len}, &stream);
    cf_str_dispose(&gid);
    if (rc != CF_OK) return rc;
    *out = stream;
    return CF_OK;
}

/* `current_user.rooms.find_by(id: params[:room_id])` for RoomChannel,
 * PresenceChannel and TypingNotificationsChannel. The reader is explicit so
 * the production worker path can use its own connection. */
static cf_err find_room(cf_db *reader, int64_t user_id, yyjson_val *params,
                        bool *found, cf_room *out) {
    *found = false;
    int64_t room_id = 0;
    if (!cast_id(yyjson_obj_get(params, "room_id"), &room_id)) {
        return CF_OK;
    }
    return cf_room_find_for_user(reader, user_id, room_id, found, out);
}

typedef enum {
    MEMBERSHIP_PRESENT = 0,
    MEMBERSHIP_DISCONNECTED,
    MEMBERSHIP_REFRESH
} membership_change_kind;

typedef struct {
    cf_app *app;
    int64_t room_id, user_id;
    membership_change_kind change;
    bool found;
} membership_args;

/* Membership::Connectable: present/disconnected/refresh_connection inside
 * BEGIN IMMEDIATE. A missing membership leaves found=false (the reference
 * raises NoMethodError after the write). */
static cf_err membership_write(cf_tx *tx, void *arg) {
    membership_args *args = arg;
    cf_db *db = cf_tx_db(tx);
    cf_membership membership;
    bool found = false;
    cf_err rc = cf_membership_find_by_room_and_user(
        db, args->room_id, args->user_id, &found, &membership);
    if (rc != CF_OK) return rc;
    if (!found) return CF_OK;
    switch (args->change) {
    case MEMBERSHIP_PRESENT:
        rc = cf_membership_present(tx, &membership);
        break;
    case MEMBERSHIP_DISCONNECTED:
        rc = cf_membership_disconnected(tx, &membership);
        break;
    case MEMBERSHIP_REFRESH:
        rc = cf_membership_refresh_connection(tx, &membership);
        break;
    default:
        rc = CF_INVALID;
    }
    cf_membership_dispose(&membership);
    if (rc == CF_OK) args->found = true;
    return rc;
}

static cf_err membership_change_ids(cf_app *app, int64_t user_id,
                                    int64_t room_id, membership_change_kind change,
                                    bool *found) {
    membership_args args = {
        .app = app,
        .room_id = room_id,
        .user_id = user_id,
        .change = change,
        .found = false,
    };
    cf_err rc = cf_write(app, membership_write, &args);
    *found = args.found;
    return rc;
}

static cf_err membership_change(cf_cable_loop *loop, int64_t room_id,
                                membership_change_kind change, bool *found) {
    return membership_change_ids(cf_cable_loop_app(loop),
                                 cf_cable_loop_user_id(loop), room_id, change,
                                 found);
}

static cf_err publish_text(cf_cable *cable, cf_span stream, cf_span payload) {
    cf_buf *buf = NULL;
    cf_err rc = cf_buf_copy(payload, &buf);
    if (rc != CF_OK) return rc;
    rc = cf_cable_publish(cable, stream, buf);
    cf_buf_release(buf);
    return rc;
}

static cf_err make_id_payload(const char *prefix, const char *suffix,
                              int64_t id, cf_builder *out) {
    cf_err rc = cf_builder_append(
        out, (cf_span){(const unsigned char *)prefix, strlen(prefix)});
    char number[32];
    int len = snprintf(number, sizeof number, "%lld", (long long)id);
    if (rc == CF_OK && len > 0 && (size_t)len < sizeof number) {
        rc = cf_builder_append(
            out, (cf_span){(const unsigned char *)number, (size_t)len});
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(
            out, (cf_span){(const unsigned char *)suffix, strlen(suffix)});
    }
    return rc;
}

/* `ActionCable.server.broadcast "user_#{id}_reads", { room_id: }` (the
 * PresenceChannel#present read notification). */
static cf_err broadcast_read_room(cf_cable *cable, int64_t user_id,
                                  int64_t room_id) {
    cf_builder stream = {0};
    cf_err rc = make_id_payload("user_", "_reads", user_id, &stream);
    cf_builder payload = {0};
    if (rc == CF_OK) {
        rc = make_id_payload("{\"room_id\":", "}", room_id, &payload);
    }
    if (rc == CF_OK) {
        rc = publish_text(cable,
                          (cf_span){stream.ptr, stream.len},
                          (cf_span){payload.ptr, payload.len});
    }
    cf_builder_dispose(&stream);
    cf_builder_dispose(&payload);
    return rc;
}

/* PresenceChannel#present / #absent / #refresh_connection. */
static cf_err presence_change(cf_cable_loop *loop, cf_cable_subscription *sub,
                              membership_change_kind change) {
    channel_state *state = sub->channel;
    if (change == MEMBERSHIP_PRESENT && !state->has_room) {
        return CF_INVALID; /* @room is nil: NoMethodError */
    }
    if (change != MEMBERSHIP_PRESENT && !state->has_room) {
        return CF_INVALID;
    }
    int64_t room_id = state->room.id;
    bool found = false;
    cf_err rc = membership_change(loop, room_id, change, &found);
    if (rc != CF_OK) return rc;
    if (!found) {
        cf_cable_log("could not execute command", "nil membership");
        return CF_INVALID;
    }
    if (change == MEMBERSHIP_PRESENT) {
        return broadcast_read_room(cf_cable_loop_cable(loop),
                                   cf_cable_loop_user_id(loop), room_id);
    }
    return CF_OK;
}

/* TypingNotificationsChannel#broadcast: {"action":..,"user":{id,name}} to the
 * class's room broadcasting (Active Support JSON escaping). */
static cf_err typing_broadcast(cf_cable_loop *loop, cf_cable_subscription *sub,
                               const char *action) {
    channel_state *state = sub->channel;
    if (!state->has_room) {
        cf_cable_log("could not execute command", "nil room");
        return CF_INVALID;
    }
    cf_str gid = {0};
    cf_err rc = cf_cable_room_gid_param(&state->room, &gid);
    if (rc != CF_OK) return rc;
    cf_str stream = {0};
    rc = broadcasting_for(
        (cf_span){(const unsigned char *)sub->class_name,
                  strlen(sub->class_name)},
        (cf_span){(const unsigned char *)gid.ptr, gid.len}, &stream);
    cf_str_dispose(&gid);
    if (rc != CF_OK) return rc;

    cf_builder payload = {0};
    rc = cf_builder_append(
        &payload, (cf_span){(const unsigned char *)"{\"action\":", 10});
    if (rc == CF_OK) {
        rc = cf_json_string(
            &payload, (cf_span){(const unsigned char *)action,
                                strlen(action)});
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &payload,
            (cf_span){(const unsigned char *)",\"user\":{\"id\":", 14});
    }
    if (rc == CF_OK) {
        rc = make_id_payload("", ",\"name\":", cf_cable_loop_user_id(loop),
                             &payload);
    }
    if (rc == CF_OK) {
        const char *name = cf_cable_loop_user_name(loop);
        rc = cf_json_string(&payload,
                            (cf_span){(const unsigned char *)name,
                                      name != NULL ? strlen(name) : 0});
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(&payload,
                               (cf_span){(const unsigned char *)"}}", 2});
    }
    if (rc == CF_OK) {
        rc = publish_text(cf_cable_loop_cable(loop),
                          (cf_span){(const unsigned char *)stream.ptr,
                                    stream.len},
                          (cf_span){payload.ptr, payload.len});
    }
    cf_builder_dispose(&payload);
    cf_str_dispose(&stream);
    return rc;
}

/* ---- subscribed handlers --------------------------------------------------------- */

/* ---- subscribed validation (shared by the owner and worker paths) ---------- */

static cf_err plan_add_stream(cf_cable_sub_plan *plan, cf_span name) {
    char *copy = malloc(name.len + 1);
    if (copy == NULL) return CF_NOMEM;
    memcpy(copy, name.ptr, name.len);
    copy[name.len] = '\0';
    if (plan->stream_count % 8 == 0) {
        size_t cap = plan->stream_count + 8;
        char **grown = realloc(plan->streams, cap * sizeof *grown);
        if (grown == NULL) {
            free(copy);
            return CF_NOMEM;
        }
        plan->streams = grown;
    }
    plan->streams[plan->stream_count++] = copy;
    return CF_OK;
}

void cf_cable_sub_plan_dispose(cf_cable_sub_plan *plan) {
    if (plan == NULL) return;
    if (plan->have_room) cf_room_dispose(&plan->room);
    for (size_t i = 0; i < plan->stream_count; i++) free(plan->streams[i]);
    free(plan->streams);
    memset(plan, 0, sizeof *plan);
}

/* The `subscribed` validation of every channel kind, with all model reads on
 * `reader` and every result owned by the plan (no loop/subscription access).
 * `secret` is used only by Turbo's signed-stream verifier. */
cf_err cf_cable_subscribe_validate(const char *class_name, cf_db *reader,
                                   int64_t user_id, cf_span identifier,
                                   cf_span secret, cf_cable_sub_plan *plan) {
    memset(plan, 0, sizeof *plan);
    if (class_name == NULL) return CF_INVALID;
    channel_kind kind;
    const char *canonical = NULL;
    if (!class_for(class_name, &kind, &canonical)) return CF_INVALID;
    switch (kind) {
    case CH_EMPTY:
        return CF_OK;
    case CH_READ_ROOMS: {
        cf_builder stream = {0};
        cf_err rc = make_id_payload("user_", "_reads", user_id, &stream);
        if (rc == CF_OK) {
            rc = plan_add_stream(plan, (cf_span){stream.ptr, stream.len});
        }
        cf_builder_dispose(&stream);
        return rc;
    }
    case CH_UNREAD_ROOMS: {
        cf_builder stream = {0};
        cf_err rc = make_id_payload("user_", "_unreads", user_id, &stream);
        if (rc == CF_OK) {
            rc = plan_add_stream(plan, (cf_span){stream.ptr, stream.len});
        }
        cf_builder_dispose(&stream);
        return rc;
    }
    case CH_ROOM:
    case CH_PRESENCE:
    case CH_TYPING: {
        yyjson_doc *doc =
            yyjson_read((const char *)identifier.ptr, identifier.len, 0);
        if (doc == NULL || !yyjson_is_obj(yyjson_doc_get_root(doc))) {
            yyjson_doc_free(doc);
            return CF_INVALID;
        }
        yyjson_val *params = yyjson_doc_get_root(doc);
        bool found = false;
        cf_room room;
        memset(&room, 0, sizeof room);
        cf_err rc = find_room(reader, user_id, params, &found, &room);
        yyjson_doc_free(doc);
        if (rc != CF_OK) return rc;
        if (!found) {
            plan->rejected = true;
            return CF_OK;
        }
        plan->room = room;
        plan->have_room = true;
        plan->room_id = room.id;
        cf_str stream = {0};
        rc = broadcasting_for_room(canonical, &room, &stream);
        if (rc == CF_OK) {
            rc = plan_add_stream(
                plan, (cf_span){(const unsigned char *)stream.ptr, stream.len});
            cf_str_dispose(&stream);
        }
        if (rc != CF_OK) return rc;
        if (kind == CH_PRESENCE) plan->presence_present = true;
        return CF_OK;
    }
    case CH_ROOM_MESSAGES: {
        yyjson_doc *doc =
            yyjson_read((const char *)identifier.ptr, identifier.len, 0);
        if (doc == NULL || !yyjson_is_obj(yyjson_doc_get_root(doc))) {
            yyjson_doc_free(doc);
            return CF_INVALID;
        }
        yyjson_val *params = yyjson_doc_get_root(doc);
        cf_str verified = {0};
        bool found = false;
        cf_err rc = verified_stream_name(secret, params, &verified, &found);
        if (rc == CF_OK && found && verified.len == 0) found = false;
        if (rc == CF_OK && found) {
            bool room_found = false;
            cf_room room;
            memset(&room, 0, sizeof room);
            rc = subscribable_room(
                reader, user_id,
                (cf_span){(const unsigned char *)verified.ptr, verified.len},
                &room_found, &room);
            if (rc == CF_OK) {
                if (room_found) {
                    cf_room_dispose(&room);
                    rc = plan_add_stream(
                        plan, (cf_span){(const unsigned char *)verified.ptr,
                                        verified.len});
                } else {
                    plan->rejected = true;
                }
            }
        } else if (rc == CF_OK) {
            plan->rejected = true;
        }
        cf_str_dispose(&verified);
        yyjson_doc_free(doc);
        return rc;
    }
    case CH_TURBO_STREAMS: {
        yyjson_doc *doc =
            yyjson_read((const char *)identifier.ptr, identifier.len, 0);
        if (doc == NULL || !yyjson_is_obj(yyjson_doc_get_root(doc))) {
            yyjson_doc_free(doc);
            return CF_INVALID;
        }
        yyjson_val *params = yyjson_doc_get_root(doc);
        cf_str verified = {0};
        bool found = false;
        cf_err rc = verified_stream_name(secret, params, &verified, &found);
        if (rc == CF_OK) {
            cf_span name = found
                               ? (cf_span){(const unsigned char *)verified.ptr,
                                           verified.len}
                               : (cf_span){NULL, 0};
            if (guarded_stream(name)) {
                plan->rejected = true;
            } else if (found && verified.len != 0) {
                rc = plan_add_stream(plan, name);
            } else {
                plan->rejected = true;
            }
        }
        cf_str_dispose(&verified);
        yyjson_doc_free(doc);
        return rc;
    }
    }
    return CF_INVALID;
}

/* The deferred model effect of a PresenceChannel subscription's subscribed
 * callback (Membership::Connectable present + read broadcast). Uses cf_write
 * and cf_cable_publish only, so a request worker may run it. */
cf_err cf_cable_subscribe_presence_effect(cf_app *app, cf_cable *cable,
                                          int64_t user_id, int64_t room_id) {
    bool found = false;
    cf_err rc = membership_change_ids(app, user_id, room_id,
                                      MEMBERSHIP_PRESENT, &found);
    if (rc != CF_OK) return rc;
    if (!found) {
        cf_cable_log("could not execute command", "nil membership");
        return CF_INVALID;
    }
    return broadcast_read_room(cable, user_id, room_id);
}

cf_err cf_cable_subscribe_apply(cf_cable_subscription *sub,
                                cf_cable_sub_plan *plan) {
    if (sub == NULL || plan == NULL) return CF_INVALID;
    channel_state *state = sub->channel;
    if (plan->rejected) sub->rejected = true;
    if (plan->have_room && state != NULL) {
        if (state->has_room) cf_room_dispose(&state->room);
        state->room = plan->room;
        state->has_room = true;
        plan->room = (cf_room){0};
        plan->have_room = false;
    }
    for (size_t i = 0; i < plan->stream_count; i++) {
        cf_span name = {(const unsigned char *)plan->streams[i],
                        strlen(plan->streams[i])};
        cf_err rc = cf_cable_subscription_stream(sub, name);
        if (rc != CF_OK) return rc;
    }
    return CF_OK;
}

/* One `subscribed` callback on the synchronous path: the same validation and
 * application the worker path uses, with the loop's own reader, plus the
 * inline presence effect unless the caller defers it until after the C03
 * install gate. */
static cf_err channel_subscribed(cf_cable_loop *loop,
                                 cf_cable_subscription *sub,
                                 bool defer_presence) {
    cf_cable_sub_plan plan;
    cf_err rc = cf_cable_subscribe_validate(
        sub->class_name, cf_cable_loop_reader(loop), cf_cable_loop_user_id(loop),
        (cf_span){(const unsigned char *)sub->identifier, sub->identifier_len},
        cf_cable_secret(cf_cable_loop_cable(loop)), &plan);
    if (rc == CF_OK) rc = cf_cable_subscribe_apply(sub, &plan);
    if (rc == CF_OK && plan.presence_present && !plan.rejected &&
        !defer_presence) {
        rc = cf_cable_subscribe_presence_effect(
            cf_cable_loop_app(loop), cf_cable_loop_cable(loop),
            cf_cable_loop_user_id(loop), plan.room_id);
    }
    cf_cable_sub_plan_dispose(&plan);
    return rc;
}

static cf_err channel_perform(cf_cable_loop *loop,
                              cf_cable_subscription *sub, const char *action,
                              yyjson_val *payload, bool *handled) {
    (void)payload;
    channel_state *state = sub->channel;
    *handled = false;
    switch (state->kind) {
    case CH_EMPTY:
        return CF_OK;
    case CH_READ_ROOMS:
    case CH_UNREAD_ROOMS:
        if (strcmp(action, "subscribed") == 0) {
            *handled = true;
            cf_err rc = channel_subscribed(loop, sub, false);
            return rc;
        }
        return CF_OK;
    case CH_ROOM:
        if (strcmp(action, "subscribed") == 0 && !sub->rejected) {
            *handled = true;
            return channel_subscribed(loop, sub, false);
        }
        return CF_OK;
    case CH_TYPING:
        if (sub->rejected) return CF_OK;
        if (strcmp(action, "start") == 0 || strcmp(action, "stop") == 0) {
            *handled = true;
            return typing_broadcast(loop, sub, action);
        }
        if (strcmp(action, "subscribed") == 0) {
            *handled = true;
            return channel_subscribed(loop, sub, false);
        }
        return CF_OK;
    case CH_PRESENCE:
        if (sub->rejected) return CF_OK;
        if (strcmp(action, "present") == 0) {
            *handled = true;
            return presence_change(loop, sub, MEMBERSHIP_PRESENT);
        }
        if (strcmp(action, "absent") == 0) {
            *handled = true;
            return presence_change(loop, sub, MEMBERSHIP_DISCONNECTED);
        }
        if (strcmp(action, "refresh") == 0) {
            *handled = true;
            return presence_change(loop, sub, MEMBERSHIP_REFRESH);
        }
        if (strcmp(action, "subscribed") == 0) {
            *handled = true;
            return channel_subscribed(loop, sub, false);
        }
        return CF_OK;
    case CH_ROOM_MESSAGES:
        if (sub->rejected) return CF_OK;
        if (strcmp(action, "subscribed") == 0) {
            *handled = true;
            return channel_subscribed(loop, sub, false);
        }
        if (strcmp(action, "verified_stream_name_from_params") == 0) {
            *handled = true;
            yyjson_doc *doc =
                yyjson_read(sub->identifier, sub->identifier_len, 0);
            if (doc == NULL || !yyjson_is_obj(yyjson_doc_get_root(doc))) {
                yyjson_doc_free(doc);
                return CF_INVALID;
            }
            cf_str verified = {0};
            bool found = false;
            cf_err rc = verified_stream_name(
                cf_cable_secret(cf_cable_loop_cable(loop)),
                yyjson_doc_get_root(doc), &verified, &found);
            cf_str_dispose(&verified);
            yyjson_doc_free(doc);
            return rc;
        }
        return CF_OK;
    case CH_TURBO_STREAMS:
        return CF_OK;
    }
    return CF_OK;
}

void cf_cable_subscription_unsubscribed(cf_cable_loop *loop,
                                        cf_cable_subscription *sub) {
    if (loop == NULL || sub == NULL || sub->channel == NULL) return;
    if (sub->rejected) return;
    channel_state *state = sub->channel;
    if (state->kind == CH_PRESENCE) {
        cf_err rc = presence_change(loop, sub, MEMBERSHIP_DISCONNECTED);
        if (rc != CF_OK) {
            cf_cable_log("could not execute command", "unsubscribe");
        }
    }
}

void cf_cable_channel_dispose(void *channel) {
    channel_state *state = channel;
    if (state == NULL) return;
    if (state->has_room) cf_room_dispose(&state->room);
    free(state);
}

/* ---- command dispatch --------------------------------------------------------------- */

static size_t subscription_index(cf_cable_loop *loop,
                                 cf_cable_subscription *sub) {
    for (size_t i = 0; i < cf_cable_loop_subscription_count(loop); i++) {
        if (cf_cable_loop_subscription_at(loop, i) == sub) return i;
    }
    return (size_t)-1;
}

/* Owner thread: parse a subscribe command and install its subscription shell
 * and channel state, with no model reads. CF_OK with *out == NULL means the
 * command is ignored with no reply (duplicate identifier, bounds, unknown
 * class, malformed command), exactly as the reference. */
cf_err cf_cable_subscribe_begin(cf_cable_loop *loop, cf_span text,
                                cf_cable_subscription **out) {
    *out = NULL;
    if (loop == NULL) return CF_INVALID;
    yyjson_doc *doc = yyjson_read((const char *)text.ptr, text.len, 0);
    if (doc == NULL) {
        cf_cable_log("could not execute command", "malformed json");
        return CF_OK;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        cf_cable_log("could not execute command", "malformed json");
        return CF_OK;
    }
    yyjson_val *identifier_value = yyjson_obj_get(root, "identifier");
    if (!yyjson_is_str(identifier_value)) {
        cf_cable_log("could not execute command", "missing identifier");
        yyjson_doc_free(doc);
        return CF_OK;
    }
    cf_span identifier = {
        (const unsigned char *)yyjson_get_str(identifier_value),
        yyjson_get_len(identifier_value)};
    yyjson_doc *idoc =
        yyjson_read((const char *)identifier.ptr, identifier.len, 0);
    if (idoc == NULL || !yyjson_is_obj(yyjson_doc_get_root(idoc))) {
        cf_cable_log("could not execute command", "invalid identifier");
        yyjson_doc_free(idoc);
        yyjson_doc_free(doc);
        return CF_OK;
    }
    /* A repeated identifier (byte for byte) is ignored without a reply. */
    if (cf_cable_loop_find_subscription(loop, identifier) != NULL) {
        yyjson_doc_free(idoc);
        yyjson_doc_free(doc);
        return CF_OK;
    }
    if (cf_cable_loop_subscription_count(loop) >= CF_CABLE_MAX_SUBSCRIPTIONS ||
        identifier.len > CF_CABLE_MAX_IDENTIFIER_BYTES) {
        cf_cable_log("could not execute command", "subscription limit");
        yyjson_doc_free(idoc);
        yyjson_doc_free(doc);
        return CF_OK;
    }
    yyjson_val *params = yyjson_doc_get_root(idoc);
    const char *requested = json_field_str(params, "channel");
    channel_kind kind;
    const char *canonical = NULL;
    if (!class_for(requested, &kind, &canonical)) {
        cf_cable_log("subscription class not found",
                     requested != NULL ? requested : "");
        yyjson_doc_free(idoc);
        yyjson_doc_free(doc);
        return CF_OK;
    }
    cf_cable_subscription *sub = cf_cable_loop_add_subscription(
        loop, identifier, canonical);
    if (sub == NULL) {
        cf_cable_log("could not execute command", "subscription limit");
        yyjson_doc_free(idoc);
        yyjson_doc_free(doc);
        return CF_OK;
    }
    channel_state *state = calloc(1, sizeof *state);
    if (state == NULL) {
        cf_cable_loop_remove_subscription(loop, subscription_index(loop, sub));
        yyjson_doc_free(idoc);
        yyjson_doc_free(doc);
        return CF_NOMEM;
    }
    state->kind = kind;
    sub->channel = state;
    yyjson_doc_free(idoc);
    yyjson_doc_free(doc);
    *out = sub;
    return CF_OK;
}

/* Owner thread: the reference reply once the model effects succeeded (or the
 * C03 gate refused). A refused or rejected subscription is answered with
 * reject_subscription and removed; a failed callback gets no reply and stays
 * registered, exactly like connection.rs. */
void cf_cable_subscribe_reply(cf_cable_loop *loop,
                              cf_cable_subscription *sub, bool refused,
                              cf_err rc) {
    if (loop == NULL || sub == NULL) return;
    cf_span identifier = {(const unsigned char *)sub->identifier,
                          sub->identifier_len};
    if (refused) {
        (void)cf_cable_loop_send_reject(loop, identifier);
        cf_cable_loop_remove_subscription(loop, subscription_index(loop, sub));
        return;
    }
    if (rc != CF_OK) {
        cf_cable_log("could not execute command", sub->class_name);
        return;
    }
    if (sub->rejected) {
        (void)cf_cable_loop_send_reject(loop, identifier);
        cf_cable_loop_remove_subscription(loop, subscription_index(loop, sub));
    } else {
        (void)cf_cable_loop_send_confirm(loop, identifier);
    }
}

/* The synchronous subscribe dispatch: begin + validate + C03 gate (+ one
 * bounded stale-version resubmission) + apply + deferred presence + reply,
 * all on the owner thread with the loop's own reader. The production wiring
 * runs the same pieces across the worker seam (pubsub.c). */
static cf_err command_subscribe(cf_cable_loop *loop, cf_span text) {
    cf_cable_subscription *sub = NULL;
    cf_err rc = cf_cable_subscribe_begin(loop, text, &sub);
    if (rc != CF_OK || sub == NULL) return rc;

    cf_app *app = cf_cable_loop_app(loop);
    cf_cable_revocation *slot = cf_cable_loop_revocation(loop);
    cf_span identifier = {(const unsigned char *)sub->identifier,
                          sub->identifier_len};
    cf_span secret = cf_cable_secret(cf_cable_loop_cable(loop));
    cf_cable_auth_ticket ticket = cf_cable_auth_capture(app);
    cf_cable_sub_plan plan;
    rc = cf_cable_subscribe_validate(sub->class_name, cf_cable_loop_reader(loop),
                                     cf_cable_loop_user_id(loop), identifier,
                                     secret, &plan);
    bool refused = false;
    if (rc == CF_OK && !plan.rejected && slot != NULL) {
        bool resubmit = false;
        cf_err gate = cf_cable_auth_install(slot, ticket, &resubmit);
        if (gate == CF_BUSY && resubmit &&
            cf_cable_loop_pending(loop) < CF_CABLE_LOOP_MAX_COMMANDS) {
            /* One bounded resubmission: fresh version, fresh reads; nothing
             * was applied yet, so the attempt state is already clean. */
            cf_cable_sub_plan_dispose(&plan);
            ticket = cf_cable_auth_capture(app);
            rc = cf_cable_subscribe_validate(sub->class_name,
                                             cf_cable_loop_reader(loop),
                                             cf_cable_loop_user_id(loop),
                                             identifier, secret, &plan);
            if (rc == CF_OK && !plan.rejected) {
                gate = cf_cable_auth_install(slot, ticket, &resubmit);
            }
        }
        if (rc == CF_OK && !plan.rejected && gate != CF_OK) refused = true;
    }
    if (rc == CF_OK && !refused) rc = cf_cable_subscribe_apply(sub, &plan);
    if (rc == CF_OK && !refused && !plan.rejected && plan.presence_present) {
        /* The deferred presence effect runs once the result is installed (or
         * immediately when the loop has no gate, i.e. a test loop). A raise
         * keeps the reference behavior: no reply, the subscription stays
         * registered. */
        rc = cf_cable_subscribe_presence_effect(
            cf_cable_loop_app(loop), cf_cable_loop_cable(loop),
            cf_cable_loop_user_id(loop), plan.room_id);
    }
    cf_cable_sub_plan_dispose(&plan);
    cf_cable_subscribe_reply(loop, sub, refused, rc);
    return CF_OK;
}

/* ---- message commands that need model work --------------------------------- */

cf_cable_subscription *cf_cable_message_subscription(cf_cable_loop *loop,
                                                     cf_span text) {
    if (loop == NULL) return NULL;
    yyjson_doc *doc = yyjson_read((const char *)text.ptr, text.len, 0);
    if (doc == NULL) return NULL;
    yyjson_val *root = yyjson_doc_get_root(doc);
    cf_cable_subscription *sub = NULL;
    if (yyjson_is_obj(root)) {
        yyjson_val *idv = yyjson_obj_get(root, "identifier");
        if (yyjson_is_str(idv)) {
            cf_span identifier = {(const unsigned char *)yyjson_get_str(idv),
                                  yyjson_get_len(idv)};
            sub = cf_cable_loop_find_subscription(loop, identifier);
        }
    }
    yyjson_doc_free(doc);
    return sub;
}

cf_cable_command_kind cf_cable_command_kind_of(cf_span text) {
    yyjson_doc *doc = yyjson_read((const char *)text.ptr, text.len, 0);
    if (doc == NULL) return CF_CABLE_CMD_OTHER;
    yyjson_val *root = yyjson_doc_get_root(doc);
    cf_cable_command_kind kind = CF_CABLE_CMD_OTHER;
    if (yyjson_is_obj(root)) {
        const char *command = json_field_str(root, "command");
        if (command != NULL && strcmp(command, "subscribe") == 0) {
            kind = CF_CABLE_CMD_SUBSCRIBE;
        } else if (command != NULL && strcmp(command, "message") == 0) {
            yyjson_val *data = yyjson_obj_get(root, "data");
            if (yyjson_is_str(data)) {
                yyjson_doc *ddoc = yyjson_read(
                    yyjson_get_str(data), yyjson_get_len(data), 0);
                if (ddoc != NULL) {
                    yyjson_val *droot = yyjson_doc_get_root(ddoc);
                    if (yyjson_is_obj(droot)) {
                        yyjson_val *action = yyjson_obj_get(droot, "action");
                        if (yyjson_is_str(action) &&
                            strcmp(yyjson_get_str(action), "subscribed") == 0) {
                            kind = CF_CABLE_CMD_MESSAGE_SUBSCRIBED;
                        }
                    }
                    yyjson_doc_free(ddoc);
                }
            }
        }
    }
    yyjson_doc_free(doc);
    return kind;
}

/* Owner thread, no reads: channel_perform's gating for a message command's
 * "subscribed" action (channel_perform leaves *handled false for a channel
 * that ignores the action and for an already-rejected subscription). */
bool cf_cable_message_subscribed_handled(const cf_cable_subscription *sub) {
    if (sub == NULL || sub->channel == NULL) return false;
    channel_state *state = sub->channel;
    switch (state->kind) {
    case CH_EMPTY:
    case CH_TURBO_STREAMS:
        return false;
    case CH_READ_ROOMS:
    case CH_UNREAD_ROOMS:
        return true;
    case CH_ROOM:
    case CH_TYPING:
    case CH_PRESENCE:
    case CH_ROOM_MESSAGES:
        return !sub->rejected;
    }
    return false;
}

static cf_err command_unsubscribe(cf_cable_loop *loop, yyjson_val *root) {
    yyjson_val *identifier_value = yyjson_obj_get(root, "identifier");
    if (!yyjson_is_str(identifier_value)) {
        cf_cable_log("unable to find subscription", "missing identifier");
        return CF_OK;
    }
    cf_span identifier = {
        (const unsigned char *)yyjson_get_str(identifier_value),
        yyjson_get_len(identifier_value)};
    cf_cable_subscription *sub =
        cf_cable_loop_find_subscription(loop, identifier);
    if (sub == NULL) {
        cf_cable_log("unable to find subscription", "identifier");
        return CF_OK;
    }
    size_t index = subscription_index(loop, sub);
    cf_cable_subscription_unsubscribed(loop, sub);
    cf_cable_loop_remove_subscription(loop, index);
    return CF_OK;
}

static cf_err command_message(cf_cable_loop *loop, yyjson_val *root) {
    yyjson_val *identifier_value = yyjson_obj_get(root, "identifier");
    if (!yyjson_is_str(identifier_value)) {
        cf_cable_log("unable to find subscription", "missing identifier");
        return CF_OK;
    }
    cf_span identifier = {
        (const unsigned char *)yyjson_get_str(identifier_value),
        yyjson_get_len(identifier_value)};
    cf_cable_subscription *sub =
        cf_cable_loop_find_subscription(loop, identifier);
    if (sub == NULL) {
        cf_cable_log("unable to find subscription", "identifier");
        return CF_OK;
    }
    yyjson_val *data_value = yyjson_obj_get(root, "data");
    if (!yyjson_is_str(data_value)) {
        cf_cable_log("could not execute command", "invalid data");
        return CF_OK;
    }
    cf_span data = {(const unsigned char *)yyjson_get_str(data_value),
                    yyjson_get_len(data_value)};
    yyjson_doc *doc = yyjson_read((const char *)data.ptr, data.len, 0);
    if (doc == NULL || !yyjson_is_obj(yyjson_doc_get_root(doc))) {
        cf_cable_log("could not execute command", "invalid data");
        yyjson_doc_free(doc);
        return CF_OK;
    }
    yyjson_val *payload = yyjson_doc_get_root(doc);
    yyjson_val *action_value = yyjson_obj_get(payload, "action");
    const char *action = "receive";
    char *action_owned = NULL;
    if (action_value != NULL && !yyjson_is_null(action_value)) {
        if (!yyjson_is_str(action_value)) {
            cf_cable_log("could not execute command", "invalid action");
            yyjson_doc_free(doc);
            return CF_OK;
        }
        cf_span action_span = {
            (const unsigned char *)yyjson_get_str(action_value),
            yyjson_get_len(action_value)};
        /* connection.rs perform_action: only an all-whitespace action becomes
         * "receive"; every other string is dispatched verbatim (no trimming),
         * exactly like Rails. */
        if (action_blank(action_span)) {
            action = "receive";
        } else {
            action_owned = malloc(action_span.len + 1);
            if (action_owned == NULL) {
                yyjson_doc_free(doc);
                return CF_NOMEM;
            }
            memcpy(action_owned, action_span.ptr, action_span.len);
            action_owned[action_span.len] = '\0';
            action = action_owned;
        }
    }
    bool handled = false;
    cf_err rc = channel_perform(loop, sub, action, payload, &handled);
    if (rc != CF_OK) {
        cf_cable_log("could not execute command", "perform");
        free(action_owned);
        yyjson_doc_free(doc);
        return CF_OK;
    }
    if (!handled) {
        cf_cable_log("unable to process", action);
    }
    free(action_owned);
    yyjson_doc_free(doc);
    return CF_OK;
}

cf_err cf_cable_channels_dispatch(cf_cable_loop *loop, cf_span text) {
    if (loop == NULL) return CF_INVALID;
    yyjson_doc *doc = yyjson_read((const char *)text.ptr, text.len, 0);
    if (doc == NULL) {
        cf_cable_log("could not execute command", "malformed json");
        return CF_OK;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        cf_cable_log("could not execute command", "malformed json");
        return CF_OK;
    }
    const char *command = json_field_str(root, "command");
    cf_err rc = CF_OK;
    if (command == NULL) {
        cf_cable_log("received unrecognized command", "missing command");
    } else if (strcmp(command, "subscribe") == 0) {
        rc = command_subscribe(loop, text);
    } else if (strcmp(command, "unsubscribe") == 0) {
        rc = command_unsubscribe(loop, root);
    } else if (strcmp(command, "message") == 0) {
        rc = command_message(loop, root);
    } else {
        cf_cable_log("received unrecognized command", command);
    }
    yyjson_doc_free(doc);
    return rc;
}
