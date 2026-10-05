/* src/actions/messages.c — MessagesController (task A-messages; route IDs
 * 76/137 `index`, 77/138 `create`, 79/140 `edit`, 80/141 `show`, 81/82/142/143
 * `update`, 83/144 `destroy`; contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/messages.rs, the
 * pinned port of reference/app/controllers/messages_controller.rb:
 *
 *   before_action (Before::default(): authentication REQUIRED, deny_bots,
 *   forgery protection) on every action; RoomScoped#set_room (the current
 *   user's membership by params[:room_id], 404 otherwise) on every action;
 *   `layout false, only: :index`.
 *
 *   index:  find_paged_messages (params[:before] wins over params[:after];
 *           a present-but-uncastable value is RecordNotFound; else the last
 *           page; 40 per page from the model), 204 for an empty page,
 *           fresh_when([record cache keys], max updated_at,
 *           "messages/index"), respond_to HTML, present the items and render
 *           the bare template.
 *   create: set_room's NotFound renders messages/room_not_found in the
 *           application layout at 200; params.require(:message).permit(
 *           :body, :attachment, :client_message_id); the message (body
 *           canonicalization + attachment assignment + rich-text/FTS rows)
 *           in one writer transaction with the current membership rechecked;
 *           broadcast_create; deliver_webhooks_to_bots; respond_to
 *           TURBO_STREAM; the create stream rendered from the stored row.
 *   show:   set_message (@room.messages.find(params[:id])); HTML page.
 *   edit:   show + ensure_can_administer (403); the editor page.
 *   update: show + ensure_can_administer; params; update_message;
 *           broadcast_replace; HTML redirect, JSON is the missing
 *           messages/show template (500).
 *   destroy: show + ensure_can_administer; destroy_message (row + remove
 *           broadcast); respond_to TURBO_STREAM; the remove stream.
 *
 * The C context conventions follow src/auth.h: an action is
 * `cf_err cf_action_NAME(cf_ctx *)`, a helper that answers the request sets
 * ctx->response and returns CF_OK, and the caller stops with
 * cf_auth_halted.  Absolute URLs use PUBLIC_ORIGIN (03-application.md, "URL
 * helpers ... use PUBLIC_ORIGIN for absolute URLs"; D-C07).
 *
 * c_symbols for the integrator's route rebind (src/routes.c, currently the
 * development 501 rows): cf_action_messages_index, cf_action_messages_create,
 * cf_action_messages_edit, cf_action_messages_show, cf_action_messages_update,
 * cf_action_messages_destroy.
 *
 * Reported dependencies (docs/devel/evidence/A-messages.md):
 *  1. `c.app().broadcasts` is app.h's cf_app_cable accessor (landed by the
 *     app repair).
 *  2. `canonical_body` (ActionText::Content.new(body, canonicalize: true)
 *     .to_html) is R02's cf_richtext_canonical_body.
 *  3. The attachment `Create` arm (a real upload) needs S02's staged blob and
 *     process_attachment jobs; multipart never builds an upload param node
 *     today, and the arm fails loudly (CF_INTERNAL, the reference's 500 for
 *     an unbuildable attachment) instead of being faked.  The `Invalid` arm
 *     is the reference's own "Could not find or build blob" 500.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "cable/channels.h"
#include "config.h"
#include "context.h"
#include "models/membership.h"
#include "models/message.h"
#include "models/room.h"
#include "models/user.h"
#include "richtext.h"
#include "views.h"
#include "views/internal.h"

#include <inttypes.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ spans */

static cf_span msg_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* Owned non-NULL string copy; an empty span yields an empty string. */
static cf_err msg_str_dup(cf_span span, cf_str *out) {
    if (out == NULL) return CF_INVALID;
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

static void msg_str_dispose(cf_str *value) {
    if (value == NULL) return;
    free(value->ptr);
    memset(value, 0, sizeof *value);
}

/* ------------------------------------------------------ request helpers */

/* HeaderValue::to_str (http 1.5.0 value.rs): every byte must be HTAB or
 * visible ASCII; obs-text (>= 0x80, valid UTF-8 included), DEL and the other
 * controls make the header read as absent, never as raw bytes. */
static bool msg_header_readable(cf_span value) {
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (c != '\t' && (c < 0x20 || c > 0x7E)) return false;
    }
    return true;
}

/* The kit's `request.header(name)`: the first matching header (case-
 * insensitive), whose value must pass `HeaderValue::to_str`; an unreadable
 * first value reads as absent and a later same-named header is not
 * consulted (`HeaderMap::get` returns the first). */
static bool msg_header(const cf_request *request, const char *name,
                       cf_span *out) {
    if (request == NULL || name == NULL || out == NULL) return false;
    size_t name_len = strlen(name);
    for (size_t i = 0; i < request->header_count; i++) {
        cf_span hname = request->headers[i].name;
        if (hname.len != name_len) continue;
        bool match = true;
        for (size_t k = 0; k < name_len; k++) {
            unsigned char c = hname.ptr[k];
            if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
            if (c != (unsigned char)name[k]) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        if (!msg_header_readable(request->headers[i].value)) return false;
        *out = request->headers[i].value;
        return true;
    }
    return false;
}

/* `is_turbo_frame_request`: the first `Turbo-Frame` header, whose bytes must
 * pass http 1.5.0's `HeaderValue::to_str` (HTAB or visible ASCII only; any
 * other byte makes the header read as absent) and be nonempty after
 * `str::trim()` (only spaces and tabs).  Local copy of the helper the other
 * action packets carry until it has a shared home. */
static bool msg_turbo_frame_request(const cf_request *request) {
    cf_span value;
    if (!msg_header(request, "turbo-frame", &value)) return false;
    bool blank = true;
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = value.ptr[i];
        /* Not through `HeaderValue::to_str` -> the header reads as absent. */
        if (!((c >= 32 && c < 127) || c == '\t')) return false;
        if (c != ' ' && c != '\t') blank = false;
    }
    return !blank;
}

/* `ActiveSupport#blank?` (kit params.rs `Param::is_blank`): the Unicode
 * White_Space property for strings. */
static bool msg_utf8_next(cf_span text, size_t *index, uint32_t *cp) {
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

static bool msg_cp_whitespace(uint32_t cp) {
    if (cp < 0x80) {
        return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\v' ||
               cp == '\f' || cp == '\r';
    }
    return cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

static bool msg_span_blank(cf_span text) {
    size_t i = 0;
    while (i < text.len) {
        uint32_t cp = 0;
        if (!msg_utf8_next(text, &i, &cp)) return false; /* not blank */
        if (!msg_cp_whitespace(cp)) return false;
    }
    return true;
}

/* `Param::is_present` (kit params.rs). */
static bool msg_param_present(const cf_param *param) {
    if (param == NULL) return false;
    switch (cf_param_type(param)) {
    case CF_PARAM_NULL:
        return false;
    case CF_PARAM_BOOL: {
        bool present = false, value = false;
        if (cf_param_bool(param, &present, &value) != CF_OK) return false;
        return value;
    }
    case CF_PARAM_NUMBER:
    case CF_PARAM_UPLOAD:
        return true;
    case CF_PARAM_STRING: {
        cf_span text;
        if (cf_param_string(param, &text) != CF_OK) return false;
        return !msg_span_blank(text);
    }
    case CF_PARAM_ARRAY:
    case CF_PARAM_OBJECT:
        return cf_param_count(param) > 0;
    }
    return false;
}

/* `c.param_str(key) -> Param::as_str` (only strings). */
static bool msg_param_string(const cf_ctx *ctx, const char *name,
                             cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, msg_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) return false;
    return cf_param_string(param, out) == CF_OK;
}

/* The permitted scalar value for `key`; NULL when the key is absent or the
 * value is an array/object (strong parameters). */
static const cf_param *msg_permitted(const cf_param *object, const char *key) {
    if (object == NULL || cf_param_type(object) != CF_PARAM_OBJECT) return NULL;
    const cf_param *value = cf_param_field(object, msg_span(key));
    if (value == NULL) return NULL;
    cf_param_kind kind = cf_param_type(value);
    if (kind == CF_PARAM_ARRAY || kind == CF_PARAM_OBJECT) return NULL;
    return value;
}

/* `params.require(:message)`: present, or the false literal. */
static bool msg_param_required(const cf_param *param) {
    if (param == NULL) return false;
    if (cf_param_type(param) == CF_PARAM_BOOL) {
        bool present = false, value = false;
        if (cf_param_bool(param, &present, &value) != CF_OK) return false;
        return true; /* false is what require accepts */
    }
    return msg_param_present(param);
}

/* `present("before")`: whether the param is present and whether it casts
 * (`integer_cast`); present-but-uncastable is RecordNotFound. */
static void msg_integer_param(const cf_ctx *ctx, const char *name,
                              bool *given, bool *cast, int64_t *value) {
    *given = false;
    *cast = false;
    *value = 0;
    const cf_param *param = cf_ctx_param(ctx, msg_span(name));
    if (!msg_param_present(param)) return;
    *given = true;
    if (cf_param_type(param) != CF_PARAM_STRING) return;
    cf_span text;
    if (cf_param_string(param, &text) != CF_OK) return;
    *cast = cf_views_integer_cast(text, value);
}

/* --------------------------------------------------------- room scope */

/* `require_current_user`: the chain guarantees one. */
static cf_err msg_current_user(cf_ctx *ctx, cf_user *out) {
    bool found = false;
    cf_err rc = cf_auth_current_user(ctx, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_INTERNAL;
    return CF_OK;
}

/* `set_room`: `Current.user.memberships.find_by!(room_id: params[:room_id])`
 * then `membership.room` (`Room::find_by_id`; a missing room is a 500 in the
 * reference, unlike the 404 of a missing membership). */
static cf_err msg_set_room(cf_ctx *ctx, const cf_user *user, cf_room *out) {
    cf_span text;
    if (!msg_param_string(ctx, "room_id", &text)) return CF_NOT_FOUND;
    int64_t room_id = 0;
    if (!cf_views_integer_cast(text, &room_id)) return CF_NOT_FOUND;

    bool found = false;
    cf_membership membership = {0};
    cf_err rc = cf_membership_find_by_room_and_user(ctx->reader, room_id,
                                                    user->id, &found,
                                                    &membership);
    if (rc != CF_OK) return rc;
    int64_t member_room_id = membership.room_id;
    cf_membership_dispose(&membership);
    if (!found) return CF_NOT_FOUND;

    bool room_found = false;
    rc = cf_room_find_by_id(ctx->reader, member_room_id, &room_found, out);
    if (rc != CF_OK) return rc;
    if (!room_found) return CF_INTERNAL; /* "the membership's room is gone" */
    return CF_OK;
}

/* `@room.messages.find(params[:id])`. */
static cf_err msg_set_message(cf_ctx *ctx, const cf_room *room,
                              cf_message *out) {
    cf_span text;
    if (!msg_param_string(ctx, "id", &text)) return CF_NOT_FOUND;
    int64_t id = 0;
    if (!cf_views_integer_cast(text, &id)) return CF_NOT_FOUND;
    return cf_message_find_in_room(ctx->reader, room->id, id, out);
}

/* `head :forbidden unless Current.user.can_administer?(@message)`: a bare
 * 403 with text/html and no body (concerns::head). */
static cf_err msg_head(cf_ctx *ctx, unsigned status) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    ctx->response->status = status;
    return cf_response_header(ctx->response, msg_span("Content-Type"),
                              msg_span("text/html"));
}

static cf_err msg_ensure_can_administer(cf_ctx *ctx,
                                        const cf_message *message) {
    cf_user user = {0};
    cf_err rc = msg_current_user(ctx, &user);
    if (rc != CF_OK) return rc;
    bool allowed = cf_user_can_administer(
        &user, (cf_optional_i64){true, message->creator_id}, false);
    cf_user_dispose(&user);
    if (!allowed) return msg_head(ctx, 403);
    return CF_OK;
}

/* ------------------------------------------------------- paged messages */

/* find_paged_messages: `before` wins over `after`; each names a message of
 * this room (else RecordNotFound); neither is the last page. */
static cf_err msg_find_paged(cf_ctx *ctx, const cf_room *room,
                             cf_message_vector *out) {
    bool has_before = false, before_ok = false, has_after = false,
         after_ok = false;
    int64_t before = 0, after = 0;
    msg_integer_param(ctx, "before", &has_before, &before_ok, &before);
    msg_integer_param(ctx, "after", &has_after, &after_ok, &after);
    if (has_before) {
        if (!before_ok) return CF_NOT_FOUND;
        cf_message target = {0};
        cf_err rc =
            cf_message_find_in_room(ctx->reader, room->id, before, &target);
        if (rc != CF_OK) return rc;
        rc = cf_message_page_before(ctx->reader, room->id, &target, out);
        cf_message_dispose(&target);
        return rc;
    }
    if (has_after) {
        if (!after_ok) return CF_NOT_FOUND;
        cf_message target = {0};
        cf_err rc =
            cf_message_find_in_room(ctx->reader, room->id, after, &target);
        if (rc != CF_OK) return rc;
        rc = cf_message_page_after(ctx->reader, room->id, &target, out);
        cf_message_dispose(&target);
        return rc;
    }
    return cf_message_last_page(ctx->reader, room->id, out);
}

/* ---------------------------------------------------- conditional GET */

/* UTC civil date from days since 1970-01-01 (Howard Hinnant's algorithm). */
static void msg_civil_from_days(int64_t days, int64_t *year, unsigned *month,
                                unsigned *day) {
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    /* Hinnant: m = mp + (mp < 10 ? 3 : -9); January/February wrap the year. */
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    *year = y + (m <= 2);
    *month = m;
    *day = d;
}

#define MSG_US_PER_SEC INT64_C(1000000)

static void msg_floor_div(int64_t value, int64_t divisor, int64_t *quot,
                          int64_t *rem) {
    int64_t q = value / divisor;
    int64_t r = value % divisor;
    if (r < 0) {
        q -= 1;
        r += divisor;
    }
    if (quot != NULL) *quot = q;
    if (rem != NULL) *rem = r;
}

/* `Time#to_fs(:usec)` cache version: YYYYMMDDHHMMSS + 6-digit microseconds. */
static void msg_cache_version(int64_t time_us, char out[21]) {
    int64_t secs = 0, usec = 0;
    msg_floor_div(time_us, MSG_US_PER_SEC, &secs, &usec);
    int64_t days = 0, sod = 0;
    msg_floor_div(secs, 86400, &days, &sod);
    int64_t year = 0;
    unsigned month = 0, day = 0;
    msg_civil_from_days(days, &year, &month, &day);
    unsigned hour = (unsigned)(sod / 3600);
    unsigned minute = (unsigned)((sod % 3600) / 60);
    unsigned second = (unsigned)(sod % 60);
    snprintf(out, 21, "%04" PRId64 "%02u%02u%02u%02u%02u%06" PRId64, year,
             month, day, hour, minute, second, usec);
}

/* `Time#httpdate`: "Thu, 01 Jan 1970 00:00:00 GMT" (RFC 9110 IMF-fixdate). */
static void msg_httpdate(int64_t time_us, char out[32]) {
    int64_t secs = 0;
    msg_floor_div(time_us, MSG_US_PER_SEC, &secs, NULL);
    int64_t days = 0, sod = 0;
    msg_floor_div(secs, 86400, &days, &sod);
    int64_t year = 0;
    unsigned month = 0, day = 0;
    msg_civil_from_days(days, &year, &month, &day);
    static const char *const WEEKDAYS[7] = {"Thu", "Fri", "Sat", "Sun",
                                            "Mon", "Tue", "Wed"};
    static const char *const MONTHS[12] = {"Jan", "Feb", "Mar", "Apr",
                                           "May", "Jun", "Jul", "Aug",
                                           "Sep", "Oct", "Nov", "Dec"};
    int64_t weekday = days % 7;
    if (weekday < 0) weekday += 7;
    snprintf(out, 32,
             "%s, %02u %s %04" PRId64 " %02" PRId64 ":%02" PRId64
             ":%02" PRId64 " GMT",
             WEEKDAYS[weekday], day, MONTHS[month - 1], year, sod / 3600,
             (sod % 3600) / 60, sod % 60);
}

/* Parse the same IMF-fixdate form (weekday ignored), trimmed; false when
 * malformed. */
static bool msg_parse_httpdate(cf_span value, int64_t *out_us) {
    while (value.len != 0 &&
           (value.ptr[0] == ' ' || value.ptr[0] == '\t')) {
        value.ptr++;
        value.len--;
    }
    while (value.len != 0 && (value.ptr[value.len - 1] == ' ' ||
                              value.ptr[value.len - 1] == '\t')) {
        value.len--;
    }
    char text[64];
    if (value.len >= sizeof text) return false;
    memcpy(text, value.ptr, value.len);
    text[value.len] = '\0';
    static const char *const MONTHS[12] = {"Jan", "Feb", "Mar", "Apr",
                                           "May", "Jun", "Jul", "Aug",
                                           "Sep", "Oct", "Nov", "Dec"};
    char wday[4] = {0}, mon[4] = {0}, zone[4] = {0};
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    if (sscanf(text, "%3[A-Za-z], %2d %3[A-Za-z] %4d %2d:%2d:%2d %3[A-Za-z]",
               wday, &day, mon, &year, &hour, &minute, &second, zone) != 8) {
        return false;
    }
    (void)wday;
    if (strcmp(zone, "GMT") != 0) return false;
    int month = 0;
    for (int i = 0; i < 12; i++) {
        if (strcmp(mon, MONTHS[i]) == 0) {
            month = i + 1;
            break;
        }
    }
    if (month == 0 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
        second > 60 || year < 1970) {
        return false;
    }
    /* days_from_civil (the inverse of msg_civil_from_days) */
    int64_t y = year;
    unsigned m = (unsigned)month;
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    /* Hinnant: doy = (153*(m + (m > 2 ? -3 : 9)) + 2)/5 + d-1. */
    unsigned mp = m > 2 ? m - 3 : m + 9;
    unsigned doy = (153 * mp + 2) / 5 + (unsigned)day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + (int64_t)doe - 719468;
    *out_us =
        (days * 86400 + hour * 3600 + minute * 60 + second) * MSG_US_PER_SEC;
    return true;
}

static cf_err msg_sha256(const unsigned char *data, size_t len,
                         unsigned char out[32]) {
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (md == NULL) return CF_NOMEM;
    unsigned int out_len = 0;
    cf_err rc = CF_OK;
    if (EVP_DigestInit_ex(md, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(md, data, len) != 1 ||
        EVP_DigestFinal_ex(md, out, &out_len) != 1) {
        rc = CF_INTERNAL;
    } else if (out_len != 32) {
        rc = CF_INTERNAL;
    }
    EVP_MD_CTX_free(md);
    return rc;
}

/* The `{:?}` spelling of one flash value (`Some("...")`), appended raw. */
static cf_err msg_append_flash_part(cf_builder *out, cf_span key,
                                    cf_span value) {
    cf_err rc = cf_builder_append(out, key);
    if (rc == CF_OK) rc = cf_builder_append(out, msg_span("=Some(\""));
    for (size_t i = 0; rc == CF_OK && i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (c == '"') {
            rc = cf_builder_append(out, msg_span("\\\""));
        } else if (c == '\\') {
            rc = cf_builder_append(out, msg_span("\\\\"));
        } else if (c == '\n') {
            rc = cf_builder_append(out, msg_span("\\n"));
        } else if (c == '\r') {
            rc = cf_builder_append(out, msg_span("\\r"));
        } else if (c == '\t') {
            rc = cf_builder_append(out, msg_span("\\t"));
        } else if (c < 0x20 || c == 0x7F) {
            char esc[16];
            snprintf(esc, sizeof esc, "\\u{%x}", (unsigned)c);
            rc = cf_builder_append(out, msg_span(esc));
        } else {
            rc = cf_builder_append(out, (cf_span){&c, 1});
        }
    }
    if (rc == CF_OK) rc = cf_builder_append(out, msg_span("\")"));
    return rc;
}

/* fresh_when's combine_etags: [validator, "frame", template, flash parts]
 * joined by '/', SHA-256, first 16 bytes hex, weak.  Owned output. */
static cf_err msg_index_etag(cf_ctx *ctx, const cf_message *rows,
                             size_t count, cf_str *out) {
    memset(out, 0, sizeof *out);
    cf_builder parts = {0};
    cf_err rc = CF_OK;
    for (size_t i = 0; rc == CF_OK && i < count; i++) {
        if (i != 0) rc = cf_builder_append(&parts, msg_span("/"));
        char version[21];
        msg_cache_version(rows[i].updated_at, version);
        char key[64];
        int n = snprintf(key, sizeof key, "messages/%" PRId64 "-%s",
                         rows[i].id, version);
        if (n < 0 || (size_t)n >= sizeof key) {
            rc = CF_INTERNAL;
            break;
        }
        rc = cf_builder_append(
            &parts, (cf_span){(const unsigned char *)key, (size_t)n});
    }
    if (rc == CF_OK && msg_turbo_frame_request(ctx->request)) {
        rc = cf_builder_append(&parts, msg_span("/frame"));
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(&parts, msg_span("/messages/index"));
    }
    /* `flash.keys().map(|k| format!("{k}={:?}", flash.get(k))).join("&")` is
     * one etag part, in the map's insertion order (an overwrite keeps the
     * entry's position, a delete removes it).  cf_ctx_flash_at loads the
     * persisted flash on first use, like the reference's `self.flash()` inside
     * combine_etags. */
    for (size_t i = 0; rc == CF_OK; i++) {
        cf_span flash_key, flash_value;
        if (!cf_ctx_flash_at(ctx, i, &flash_key, &flash_value)) break;
        rc = cf_builder_append(&parts, i == 0 ? msg_span("/") : msg_span("&"));
        if (rc == CF_OK) {
            rc = msg_append_flash_part(&parts, flash_key, flash_value);
        }
    }
    unsigned char digest[32];
    if (rc == CF_OK) {
        rc = msg_sha256(parts.ptr != NULL ? parts.ptr
                                          : (const unsigned char *)"",
                        parts.len, digest);
    }
    cf_builder_dispose(&parts);
    if (rc != CF_OK) return rc;
    static const char HEX[] = "0123456789abcdef";
    char etag[40];
    size_t at = 0;
    etag[at++] = 'W';
    etag[at++] = '/';
    etag[at++] = '"';
    for (size_t i = 0; i < 16; i++) {
        etag[at++] = HEX[digest[i] >> 4];
        etag[at++] = HEX[digest[i] & 0x0F];
    }
    etag[at++] = '"';
    return msg_str_dup((cf_span){(const unsigned char *)etag, at}, out);
}

/* `request.fresh?` with strict freshness: an If-None-Match list naming the
 * ETag (or `*`), else an If-Modified-Since no earlier than Last-Modified. */
static bool msg_request_fresh(const cf_request *request, cf_span etag,
                              bool has_last_modified,
                              int64_t last_modified_us) {
    cf_span if_none_match;
    if (msg_header(request, "if-none-match", &if_none_match)) {
        size_t at = 0;
        while (at <= if_none_match.len) {
            size_t end = at;
            while (end < if_none_match.len && if_none_match.ptr[end] != ',') {
                end++;
            }
            size_t start = at, stop = end;
            while (start < stop && (if_none_match.ptr[start] == ' ' ||
                                    if_none_match.ptr[start] == '\t')) {
                start++;
            }
            while (stop > start && (if_none_match.ptr[stop - 1] == ' ' ||
                                    if_none_match.ptr[stop - 1] == '\t')) {
                stop--;
            }
            cf_span value = {if_none_match.ptr + start, stop - start};
            bool star = value.len == 1 && value.ptr[0] == '*';
            if (star || (value.len == etag.len &&
                         memcmp(value.ptr, etag.ptr, etag.len) == 0)) {
                return true;
            }
            if (end == if_none_match.len) break;
            at = end + 1;
        }
        return false;
    }
    cf_span since;
    if (msg_header(request, "if-modified-since", &since)) {
        int64_t since_us = 0;
        if (!msg_parse_httpdate(since, &since_us)) return false;
        return has_last_modified && since_us >= last_modified_us;
    }
    return false;
}

/* ---------------------------------------------------------- responses */

#define MSG_CONTENT_HTML "text/html; charset=utf-8"
#define MSG_CONTENT_TURBO "text/vnd.turbo-stream.html; charset=utf-8"

/* `Ctx::render`: content type, body, `Vary: Accept` when the format came
 * from the Accept header, the layout page's preload Link header.
 * Consumes the builder. */
static cf_err msg_send(cf_ctx *ctx, unsigned status, const char *content_type,
                       cf_builder *body, bool link_header) {
    cf_buf *buf = NULL;
    cf_err rc = cf_builder_freeze(body, &buf);
    if (rc != CF_OK) {
        cf_builder_dispose(body);
        return rc;
    }
    ctx->response->status = status;
    rc = cf_response_header(ctx->response, msg_span("Content-Type"),
                            msg_span(content_type));
    if (rc == CF_OK && cf_ctx_vary_accept(ctx)) {
        rc = cf_response_header(ctx->response, msg_span("Vary"),
                                msg_span("Accept"));
    }
    if (rc == CF_OK) rc = cf_response_body(ctx->response, buf);
    cf_buf_release(buf);
    if (rc != CF_OK) return rc;
    if (link_header) {
        cf_builder links = {0};
        cf_err link_rc = cf_views_preload_links(&links);
        if (link_rc == CF_INVALID) link_rc = CF_OK; /* unconfigured assets */
        if (link_rc == CF_OK && links.len != 0) {
            link_rc = cf_response_header(ctx->response, msg_span("Link"),
                                         (cf_span){links.ptr, links.len});
        }
        cf_builder_dispose(&links);
        if (link_rc != CF_OK) return link_rc;
    }
    return CF_OK;
}

/* `redirect_to <PUBLIC_ORIGIN><path>` (Rails default 302, text/html with
 * charset). */
static cf_err msg_redirect(cf_ctx *ctx, cf_span path) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    cf_err rc = cf_builder_append(&location, msg_span(config->public_origin));
    if (rc == CF_OK) rc = cf_builder_append(&location, path);
    if (rc == CF_OK) {
        ctx->response->status = 302;
        rc = cf_response_header(ctx->response, msg_span("Location"),
                                (cf_span){location.ptr, location.len});
        if (rc == CF_OK) {
            rc = cf_response_header(ctx->response, msg_span("Content-Type"),
                                    msg_span(MSG_CONTENT_HTML));
        }
    }
    cf_builder_dispose(&location);
    return rc;
}

/* `respond_to([HTML])`. */
static cf_err msg_respond_html(cf_ctx *ctx) {
    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    return cf_ctx_respond_to(ctx, offered, 1, &chosen);
}

/* `render action: :room_not_found` (inside the application layout). */
static cf_err msg_render_room_not_found(cf_ctx *ctx) {
    cf_err rc = msg_respond_html(ctx);
    if (rc != CF_OK) return rc;
    cf_view_layout_model layout = {0};
    rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) return rc;
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);
    cf_builder body = {0};
    rc = cf_view_message_room_not_found(&view_ctx, &body);
    if (rc == CF_OK) {
        rc = msg_send(ctx, 200, MSG_CONTENT_HTML, &body, true);
    } else {
        cf_builder_dispose(&body);
    }
    cf_view_layout_model_dispose(&layout);
    return rc;
}

/* `page::content_in_application_layout(OK, views::Show)`: Layout::load, the
 * application layout (MessagesController's own layout overrides turbo-rails'
 * frame layout) and the preload Link header. */
static cf_err msg_render_show(cf_ctx *ctx, const cf_view_message *message) {
    cf_view_layout_model layout = {0};
    cf_err rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) return rc;
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);
    cf_builder body = {0};
    rc = cf_view_message_show(&view_ctx, message, &body);
    if (rc == CF_OK) {
        rc = msg_send(ctx, 200, MSG_CONTENT_HTML, &body, true);
    } else {
        cf_builder_dispose(&body);
    }
    cf_view_layout_model_dispose(&layout);
    return rc;
}

/* `page::content_in_application_layout(OK, views::Edit)`. */
static cf_err msg_render_edit(cf_ctx *ctx,
                              const cf_view_message_edit_model *model) {
    cf_view_layout_model layout = {0};
    cf_err rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) return rc;
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);
    cf_builder body = {0};
    rc = cf_view_message_edit(&view_ctx, model, &body);
    if (rc == CF_OK) {
        rc = msg_send(ctx, 200, MSG_CONTENT_HTML, &body, true);
    } else {
        cf_builder_dispose(&body);
    }
    cf_view_layout_model_dispose(&layout);
    return rc;
}

/* ------------------------------------------------------------- params */

typedef enum {
    MSG_ATTACHMENT_UNCHANGED = 0,
    MSG_ATTACHMENT_DELETE,
    MSG_ATTACHMENT_CREATE,
    MSG_ATTACHMENT_INVALID
} msg_attachment;

typedef struct {
    bool has_body;
    cf_str body; /* owned when has_body */
    bool has_client_message_id;
    cf_str client_message_id; /* owned when present */
    msg_attachment attachment;
} msg_params;

static void msg_params_dispose(msg_params *params) {
    msg_str_dispose(&params->body);
    msg_str_dispose(&params->client_message_id);
    memset(params, 0, sizeof *params);
}

/* `attachments::Assignment::from_params` for the permitted `attachment`:
 * absent (or dropped as an array/object) is Unchanged, nil/"" is Delete, an
 * uploaded file is Create, anything else raises. */
static msg_attachment msg_attachment_assignment(const cf_param *permitted) {
    const cf_param *value = msg_permitted(permitted, "attachment");
    if (value == NULL) return MSG_ATTACHMENT_UNCHANGED;
    switch (cf_param_type(value)) {
    case CF_PARAM_NULL:
        return MSG_ATTACHMENT_DELETE;
    case CF_PARAM_STRING: {
        cf_span text;
        if (cf_param_string(value, &text) != CF_OK) {
            return MSG_ATTACHMENT_INVALID;
        }
        return text.len == 0 ? MSG_ATTACHMENT_DELETE
                             : MSG_ATTACHMENT_INVALID;
    }
    case CF_PARAM_UPLOAD:
        return MSG_ATTACHMENT_CREATE;
    default:
        return MSG_ATTACHMENT_INVALID;
    }
}

/* `params.require(:message).permit(:body, :attachment, :client_message_id)`
 * and the source's three reads; CF_INVALID is ParameterMissing (dispatch
 * answers 400). */
static cf_err msg_message_params(cf_ctx *ctx, msg_params *out) {
    memset(out, 0, sizeof *out);
    const cf_param *message = cf_ctx_param(ctx, msg_span("message"));
    if (!msg_param_required(message)) return CF_INVALID;
    const cf_param *permitted = message;
    if (cf_param_type(message) != CF_PARAM_OBJECT) permitted = NULL;

    const cf_param *body = msg_permitted(permitted, "body");
    if (body != NULL && cf_param_type(body) == CF_PARAM_STRING) {
        cf_span text;
        cf_err rc = cf_param_string(body, &text);
        if (rc != CF_OK) return rc;
        rc = msg_str_dup(text, &out->body);
        if (rc != CF_OK) return rc;
        out->has_body = true;
    }
    const cf_param *client = msg_permitted(permitted, "client_message_id");
    if (client != NULL && cf_param_type(client) == CF_PARAM_STRING) {
        cf_span text;
        cf_err rc = cf_param_string(client, &text);
        if (rc == CF_OK) rc = msg_str_dup(text, &out->client_message_id);
        if (rc != CF_OK) {
            msg_params_dispose(out);
            return rc;
        }
        out->has_client_message_id = true;
    }
    out->attachment = msg_attachment_assignment(permitted);
    return CF_OK;
}

/* `canonical_body`: assigning a String to a rich text attribute stores
 * `ActionText::Content.new(body, canonicalize: true).to_html()`, computed on
 * a reader ahead of the write with the request Host (controllers/messages.rs
 * canonical_body). R02's cf_richtext_canonical_body is that exact load and
 * serialization, including the reference's
 * `unwrap_or_else(|_| body.to_string())` rescue. */
static cf_err msg_canonical_body(cf_ctx *ctx, cf_span body, cf_str *out) {
    return cf_richtext_canonical_body(ctx, body, out);
}

/* The reference's `invalid_attachment`: Error::internal("Could not find or
 * build blob: expected attachable"). */
static cf_err msg_invalid_attachment(void) {
    return CF_INTERNAL;
}

/* ------------------------------------------------ broadcasts (C02) */

/* `c.app().broadcasts`: app.h's cable accessor (landed by the app repair,
 * set once in main.c after cf_cable_create). A NULL cable is an internal
 * error, never a silently dropped broadcast. */
static cf_err msg_cable(cf_ctx *ctx, cf_cable **out) {
    *out = NULL;
    if (ctx->app == NULL) return CF_INVALID;
    cf_cable *cable = cf_app_cable(ctx->app);
    if (cable == NULL) return CF_INTERNAL;
    *out = cable;
    return CF_OK;
}

/* C02's post-commit delivery policy: a full loop queue (or a stopping cable)
 * drops the broadcast and is counted, exactly as the reference's pubsub
 * does; it must not become a 503 for a committed mutation (00-contracts.md).
 * Other failures are render/allocation failures and propagate. */
static cf_err msg_broadcast_outcome(cf_err rc, const char *what) {
    if (rc == CF_BUSY) {
        cf_cable_log("dropped broadcast", what);
        return CF_OK;
    }
    return rc;
}

/* `@message.broadcast_create`: the message partial appended to the room and
 * the unread pings, through C02's production partials (A02 presenters and
 * renderers). */
static cf_err msg_broadcast_create(cf_ctx *ctx, const cf_room *room,
                                   const cf_message *message) {
    cf_cable *cable = NULL;
    cf_err rc = msg_cable(ctx, &cable);
    if (rc != CF_OK) return rc;
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, NULL);
    cf_broadcast_views views = {ctx, &view_ctx};
    cf_broadcast_partials partials;
    cf_broadcast_partials_views(&partials, &views);
    rc = cf_broadcast_message_create(ctx->reader, cable, room, message,
                                     &partials);
    return msg_broadcast_outcome(rc, "messages#create");
}

/* `broadcast_replace_to @room, :messages, target: [@message, :presentation]`. */
static cf_err msg_broadcast_replace(cf_ctx *ctx, const cf_room *room,
                                    const cf_message *message) {
    cf_cable *cable = NULL;
    cf_err rc = msg_cable(ctx, &cable);
    if (rc != CF_OK) return rc;
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, NULL);
    cf_broadcast_views views = {ctx, &view_ctx};
    cf_broadcast_partials partials;
    cf_broadcast_partials_views(&partials, &views);
    rc = cf_broadcast_message_replace(cable, room, message, &partials);
    return msg_broadcast_outcome(rc, "messages#update");
}

/* `@message.broadcast_remove`. */
static cf_err msg_broadcast_remove(cf_ctx *ctx, const cf_room *room,
                                   const cf_message *message) {
    cf_cable *cable = NULL;
    cf_err rc = msg_cable(ctx, &cable);
    if (rc != CF_OK) return rc;
    rc = cf_broadcast_message_remove(cable, room, message);
    return msg_broadcast_outcome(rc, "messages#destroy");
}

/* --------------------------------------------- writer revalidation */

/* D02: "Revalidate relevant user status and ownership in the write
 * transaction even if request authentication happened earlier."  The
 * membership lookup is RoomScoped's scope, the status check the
 * authenticated identity, and `fresh` is the message row as it exists now. */
static cf_err msg_revalidate(cf_tx *tx, int64_t room_id, int64_t actor_id,
                             bool need_message, int64_t message_id,
                             bool need_administer, cf_message *fresh) {
    cf_db *db = cf_tx_db(tx);
    bool member = false;
    cf_membership membership = {0};
    cf_err rc = cf_membership_find_by_room_and_user(db, room_id, actor_id,
                                                    &member, &membership);
    cf_membership_dispose(&membership);
    if (rc != CF_OK) return rc;
    if (!member) return CF_NOT_FOUND;

    bool found = false;
    cf_user actor = {0};
    rc = cf_user_find_by_id(db, actor_id, &found, &actor);
    if (rc != CF_OK) {
        cf_user_dispose(&actor);
        return rc;
    }
    if (!found || !cf_user_is_active(&actor)) {
        cf_user_dispose(&actor);
        return CF_FORBIDDEN;
    }
    if (need_message) {
        rc = cf_message_find_in_room(db, room_id, message_id, fresh);
        if (rc != CF_OK) {
            cf_user_dispose(&actor);
            return rc;
        }
        if (need_administer &&
            !cf_user_can_administer(
                &actor, (cf_optional_i64){true, fresh->creator_id}, false)) {
            cf_user_dispose(&actor);
            return CF_FORBIDDEN;
        }
    }
    cf_user_dispose(&actor);
    return CF_OK;
}

/* `@room.messages.create_with_attachment!(attributes)` inside one writer
 * transaction (the rich-text body and the FTS/room effects are the model's,
 * spec 02 D02/D-C09). */
struct msg_create_write {
    int64_t room_id;
    int64_t creator_id;
    cf_new_message attributes;
    cf_message message;
};

/* Non-static so the action test can exercise the D02 in-transaction
 * revalidation directly (the callback itself is not a controller entry
 * point). */
cf_err msg_create_write_cb(cf_tx *tx, void *arg) {
    struct msg_create_write *write = arg;
    cf_err rc = msg_revalidate(tx, write->room_id, write->creator_id, false,
                               0, false, NULL);
    if (rc != CF_OK) return rc;
    return cf_message_create(tx, &write->attributes, &write->message);
}

/* The `Create` upload arm is S02's (stage + process_attachment jobs);
 * multipart never reaches it today, and it fails loudly instead of faking an
 * attachment. */
static cf_err msg_create_message(cf_ctx *ctx, const cf_user *user,
                                 const cf_room *room, msg_params *params,
                                 cf_message *out) {
    if (params->attachment == MSG_ATTACHMENT_INVALID) {
        return msg_invalid_attachment();
    }
    if (params->attachment == MSG_ATTACHMENT_CREATE) {
        return CF_INTERNAL; /* S02: stage/process_attachment unavailable */
    }

    cf_str body = {0};
    bool has_body = false;
    if (params->has_body) {
        cf_err rc = msg_canonical_body(
            ctx, (cf_span){(const unsigned char *)params->body.ptr,
                           params->body.len},
            &body);
        if (rc != CF_OK) return rc;
        has_body = true;
    }

    struct msg_create_write write;
    memset(&write, 0, sizeof write);
    write.room_id = room->id;
    write.creator_id = user->id;
    write.attributes.room_id = room->id;
    write.attributes.creator_id = user->id;
    if (params->has_client_message_id) {
        write.attributes.client_message_id.present = true;
        write.attributes.client_message_id.value = params->client_message_id;
    }
    if (has_body) {
        write.attributes.body.present = true;
        write.attributes.body.value = body;
    }
    cf_err rc = cf_write(ctx->app, msg_create_write_cb, &write);
    if (rc == CF_OK) {
        *out = write.message;
    } else {
        cf_message_dispose(&write.message);
    }
    msg_str_dispose(&body);
    return rc;
}

/* `@message.update!(message_params)` inside the writer. */
struct msg_update_write {
    int64_t room_id;
    int64_t actor_id;
    int64_t message_id;
    bool has_body;
    cf_str body; /* borrowed */
    bool attachment_given;
    cf_optional_i64 blob_id;
    cf_message message; /* in/out; its owned fields alias the caller's row */
};

static cf_err msg_update_write_cb(cf_tx *tx, void *arg) {
    struct msg_update_write *write = arg;
    cf_message fresh = {0};
    cf_err rc = msg_revalidate(tx, write->room_id, write->actor_id, true,
                               write->message_id, true, &fresh);
    cf_message_dispose(&fresh);
    if (rc != CF_OK) return rc;
    /* The write mutates the caller's row instance (the reference updates the
     * loaded record); the fresh read is only the revalidation. */
    if (write->has_body) {
        rc = cf_message_update_body(tx, &write->message, write->body);
        if (rc != CF_OK) return rc;
    }
    if (write->attachment_given) {
        rc = cf_message_replace_attachment(tx, &write->message,
                                           write->blob_id);
    }
    return rc;
}

static cf_err msg_update_message(cf_ctx *ctx, const cf_message *message,
                                 int64_t actor_id, msg_params *params,
                                 cf_message *out) {
    if (params->attachment == MSG_ATTACHMENT_INVALID) {
        return msg_invalid_attachment();
    }
    if (params->attachment == MSG_ATTACHMENT_CREATE) {
        return CF_INTERNAL; /* S02: stage/process_attachment unavailable */
    }

    cf_str body = {0};
    bool has_body = false;
    if (params->has_body) {
        cf_err rc = msg_canonical_body(
            ctx, (cf_span){(const unsigned char *)params->body.ptr,
                           params->body.len},
            &body);
        if (rc != CF_OK) return rc;
        has_body = true;
    }

    struct msg_update_write write;
    memset(&write, 0, sizeof write);
    write.room_id = message->room_id;
    write.actor_id = actor_id;
    write.message_id = message->id;
    write.has_body = has_body;
    write.body = body;
    write.attachment_given = params->attachment == MSG_ATTACHMENT_DELETE;
    write.message = *message; /* borrows the caller's owned fields */
    cf_err rc = cf_write(ctx->app, msg_update_write_cb, &write);
    msg_str_dispose(&body);
    if (rc != CF_OK) return rc;
    /* `c.app().read(|conn| Message::find(conn, id))`. */
    return cf_message_find(ctx->reader, message->id, out);
}

/* `@message.destroy` inside the writer. */
struct msg_destroy_write {
    int64_t room_id;
    int64_t actor_id;
    cf_message message;
};

static cf_err msg_destroy_write_cb(cf_tx *tx, void *arg) {
    struct msg_destroy_write *write = arg;
    cf_message fresh = {0};
    cf_err rc = msg_revalidate(tx, write->room_id, write->actor_id, true,
                               write->message.id, true, &fresh);
    cf_message_dispose(&fresh);
    if (rc != CF_OK) return rc;
    return cf_message_destroy(tx, &write->message);
}

/* `destroy_message`: the row first, then `message_remove` (post-commit). */
static cf_err msg_destroy_message(cf_ctx *ctx, const cf_room *room,
                                  const cf_message *message,
                                  int64_t actor_id) {
    struct msg_destroy_write write;
    memset(&write, 0, sizeof write);
    write.room_id = room->id;
    write.actor_id = actor_id;
    write.message = *message; /* borrows the caller's owned fields */
    cf_err rc = cf_write(ctx->app, msg_destroy_write_cb, &write);
    if (rc != CF_OK) return rc;
    return msg_broadcast_remove(ctx, room, message);
}

/* ----------------------------------------------- webhook delivery */

struct msg_webhook_write {
    const cf_user *const *bots;
    size_t count;
    int64_t message_id;
};

static cf_err msg_webhook_write_cb(cf_tx *tx, void *arg) {
    struct msg_webhook_write *write = arg;
    for (size_t i = 0; i < write->count; i++) {
        cf_err rc = cf_user_deliver_webhook_later(tx, write->bots[i],
                                                  write->message_id);
        if (rc != CF_OK) return rc;
    }
    return CF_OK;
}

/* `deliver_webhooks_to_bots`: every active bot in a direct room, else every
 * mentioned active bot, except the message's creator. */
static cf_err msg_deliver_webhooks(cf_ctx *ctx, const cf_room *room,
                                   const cf_message *message) {
    cf_user_vector candidates = {0};
    cf_err rc;
    if (cf_room_direct(room)) {
        rc = cf_room_active_bots(ctx->reader, room, &candidates);
    } else {
        rc = cf_message_mentionees(ctx->reader, message, cf_tx_rich_text(NULL),
                                   &candidates);
    }
    if (rc != CF_OK) {
        cf_user_vector_dispose(&candidates);
        return rc;
    }
    const cf_user **bots = NULL;
    size_t count = 0;
    if (candidates.len != 0) {
        bots = calloc(candidates.len, sizeof *bots);
        if (bots == NULL) {
            cf_user_vector_dispose(&candidates);
            return CF_NOMEM;
        }
    }
    for (size_t i = 0; i < candidates.len; i++) {
        const cf_user *user = &candidates.items[i];
        if (user->role == CF_ROLE_BOT && user->status == CF_STATUS_ACTIVE &&
            user->id != message->creator_id) {
            bots[count++] = user;
        }
    }
    if (count != 0) {
        struct msg_webhook_write write = {bots, count, message->id};
        rc = cf_write(ctx->app, msg_webhook_write_cb, &write);
    }
    free(bots);
    cf_user_vector_dispose(&candidates);
    return rc;
}

/* ------------------------------------------------------- index action */

cf_err cf_action_messages_index(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, true, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = msg_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    cf_room room = {0};
    rc = msg_set_room(ctx, &user, &room);
    cf_user_dispose(&user);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }

    cf_message_vector messages = {0};
    rc = msg_find_paged(ctx, &room, &messages);
    if (rc != CF_OK) {
        cf_message_vector_dispose(&messages);
        cf_room_dispose(&room);
        return rc;
    }
    if (messages.len == 0) {
        cf_message_vector_dispose(&messages);
        cf_room_dispose(&room);
        ctx->response->status = 204; /* `head :no_content` */
        return CF_OK;
    }

    /* fresh_when: the records' cache keys, their latest updated_at and the
     * template digest. */
    cf_str etag = {0};
    int64_t last_modified_us = messages.items[0].updated_at;
    for (size_t i = 1; i < messages.len; i++) {
        if (messages.items[i].updated_at > last_modified_us) {
            last_modified_us = messages.items[i].updated_at;
        }
    }
    rc = msg_index_etag(ctx, messages.items, messages.len, &etag);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, msg_span("ETag"),
                                (cf_span){(const unsigned char *)etag.ptr,
                                          etag.len});
    }
    char httpdate[32];
    if (rc == CF_OK) {
        msg_httpdate(last_modified_us, httpdate);
        rc = cf_response_header(
            ctx->response, msg_span("Last-Modified"),
            (cf_span){(const unsigned char *)httpdate, strlen(httpdate)});
    }
    bool fresh = rc == CF_OK &&
                 msg_request_fresh(ctx->request,
                                   (cf_span){
                                       (const unsigned char *)etag.ptr,
                                       etag.len},
                                   true, last_modified_us);
    msg_str_dispose(&etag);
    if (rc != CF_OK || fresh) {
        cf_message_vector_dispose(&messages);
        cf_room_dispose(&room);
        if (rc == CF_OK) ctx->response->status = 304; /* `head :not_modified */
        return rc;
    }

    rc = msg_respond_html(ctx);
    if (rc != CF_OK) {
        cf_message_vector_dispose(&messages);
        cf_room_dispose(&room);
        return rc;
    }

    /* `present(|presenter| presenter.messages(&messages))` then
     * `page::bare(OK, HTML, views::Index)`: the bare template, no layout
     * Link header. */
    cf_view_message_item_vector items = {0};
    rc = cf_presenter_messages(ctx, messages.items, messages.len, &items);
    cf_message_vector_dispose(&messages);
    if (rc == CF_OK) {
        cf_view_layout_model layout = {0};
        rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
        if (rc == CF_OK) {
            cf_view_ctx view_ctx;
            cf_view_ctx_init(&view_ctx, ctx, &layout);
            cf_builder body = {0};
            rc = cf_view_message_index(&view_ctx, items.items, items.len,
                                       &body);
            if (rc == CF_OK) {
                rc = msg_send(ctx, 200, MSG_CONTENT_HTML, &body, false);
            } else {
                cf_builder_dispose(&body);
            }
            cf_view_layout_model_dispose(&layout);
        }
    }
    cf_view_message_item_vector_dispose(&items);
    cf_room_dispose(&room);
    return rc;
}

/* ------------------------------------------------------ create action */

cf_err cf_action_messages_create(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, true, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = msg_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    cf_room room = {0};
    rc = msg_set_room(ctx, &user, &room);
    if (rc == CF_NOT_FOUND) {
        /* A room that's gone (or left) renders room_not_found, not a 404. */
        cf_user_dispose(&user);
        cf_room_dispose(&room);
        return msg_render_room_not_found(ctx);
    }
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        cf_room_dispose(&room);
        return rc;
    }

    msg_params params;
    rc = msg_message_params(ctx, &params);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        cf_room_dispose(&room);
        return rc;
    }

    cf_message message = {0};
    rc = msg_create_message(ctx, &user, &room, &params, &message);
    msg_params_dispose(&params);
    cf_user_dispose(&user);
    if (rc != CF_OK) {
        cf_message_dispose(&message);
        cf_room_dispose(&room);
        return rc;
    }

    rc = msg_broadcast_create(ctx, &room, &message);
    if (rc == CF_OK) rc = msg_deliver_webhooks(ctx, &room, &message);
    if (rc != CF_OK) {
        cf_message_dispose(&message);
        cf_room_dispose(&room);
        return rc;
    }

    /* `c.respond_to(&[format::TURBO_STREAM])` — a client that cannot take a
     * turbo stream is UnknownFormat (406) *after* the message committed. */
    const cf_format *offered[1] = {&cf_format_turbo_stream};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) {
        cf_message_dispose(&message);
        cf_room_dispose(&room);
        return rc;
    }

    /* The request-less rendering (`render_detached_at`): no request, no
     * Current.user, the renderer's base URL. */
    cf_view_message_item item = {0};
    rc = cf_presenter_message_item(ctx, &message, &item);
    if (rc == CF_OK) {
        cf_view_ctx view_ctx;
        cf_view_ctx_init(&view_ctx, ctx, NULL);
        cf_builder body = {0};
        rc = cf_view_message_create_stream(&view_ctx, &item, room.room_type,
                                           &body);
        if (rc == CF_OK) {
            rc = msg_send(ctx, 200, MSG_CONTENT_TURBO, &body, false);
        } else {
            cf_builder_dispose(&body);
        }
    }
    cf_view_message_item_dispose(&item);
    cf_message_dispose(&message);
    cf_room_dispose(&room);
    return rc;
}

/* -------------------------------------------------------- show action */

cf_err cf_action_messages_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, true, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = msg_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    cf_room room = {0};
    rc = msg_set_room(ctx, &user, &room);
    cf_user_dispose(&user);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    cf_message message = {0};
    rc = msg_set_message(ctx, &room, &message);
    cf_room_dispose(&room);
    if (rc != CF_OK) {
        cf_message_dispose(&message);
        return rc;
    }

    rc = msg_respond_html(ctx);
    cf_view_message view = {0};
    if (rc == CF_OK) rc = cf_presenter_message(ctx, &message, &view);
    if (rc == CF_OK) rc = msg_render_show(ctx, &view);
    cf_view_message_dispose(&view);
    cf_message_dispose(&message);
    return rc;
}

/* -------------------------------------------------------- edit action */

cf_err cf_action_messages_edit(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, true, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = msg_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    cf_room room = {0};
    rc = msg_set_room(ctx, &user, &room);
    cf_user_dispose(&user);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    cf_message message = {0};
    rc = msg_set_message(ctx, &room, &message);
    cf_room_dispose(&room);
    if (rc != CF_OK) {
        cf_message_dispose(&message);
        return rc;
    }
    rc = msg_ensure_can_administer(ctx, &message);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_message_dispose(&message);
        return rc;
    }

    rc = msg_respond_html(ctx);
    cf_view_message_edit_model model = {0};
    if (rc == CF_OK) rc = cf_presenter_message_edit(ctx, &message, &model);
    if (rc == CF_OK) rc = msg_render_edit(ctx, &model);
    cf_view_message_edit_model_dispose(&model);
    cf_message_dispose(&message);
    return rc;
}

/* ------------------------------------------------------ update action */

cf_err cf_action_messages_update(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, true, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = msg_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    int64_t actor_id = user.id;
    cf_room room = {0};
    rc = msg_set_room(ctx, &user, &room);
    cf_user_dispose(&user);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    cf_message message = {0};
    rc = msg_set_message(ctx, &room, &message);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        cf_message_dispose(&message);
        return rc;
    }
    rc = msg_ensure_can_administer(ctx, &message);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_room_dispose(&room);
        cf_message_dispose(&message);
        return rc;
    }

    msg_params params;
    rc = msg_message_params(ctx, &params);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        cf_message_dispose(&message);
        return rc;
    }

    cf_message updated = {0};
    rc = msg_update_message(ctx, &message, actor_id, &params, &updated);
    msg_params_dispose(&params);
    cf_message_dispose(&message);
    if (rc != CF_OK) {
        cf_message_dispose(&updated);
        cf_room_dispose(&room);
        return rc;
    }

    rc = msg_broadcast_replace(ctx, &room, &updated);
    if (rc != CF_OK) {
        cf_message_dispose(&updated);
        cf_room_dispose(&room);
        return rc;
    }

    /* respond_to([HTML, JSON]): JSON is `render :show`, which has no JSON
     * template here (Error::internal("Missing template messages/show")). */
    const cf_format *offered[2] = {&cf_format_html, &cf_format_json};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 2, &chosen);
    if (rc == CF_OK && chosen == &cf_format_json) rc = CF_INTERNAL;
    if (rc == CF_OK) {
        char path[96];
        int n = snprintf(path, sizeof path, "/rooms/%" PRId64 "/messages/%" PRId64,
                         room.id, updated.id);
        if (n < 0 || (size_t)n >= sizeof path) {
            rc = CF_INTERNAL;
        } else {
            rc = msg_redirect(
                ctx, (cf_span){(const unsigned char *)path, (size_t)n});
        }
    }
    cf_message_dispose(&updated);
    cf_room_dispose(&room);
    return rc;
}

/* ----------------------------------------------------- destroy action */

cf_err cf_action_messages_destroy(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, true, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = msg_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    int64_t actor_id = user.id;
    cf_room room = {0};
    rc = msg_set_room(ctx, &user, &room);
    cf_user_dispose(&user);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    cf_message message = {0};
    rc = msg_set_message(ctx, &room, &message);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        cf_message_dispose(&message);
        return rc;
    }
    rc = msg_ensure_can_administer(ctx, &message);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_room_dispose(&room);
        cf_message_dispose(&message);
        return rc;
    }

    /* The row and the remove broadcast happen before respond_to: a client
     * that cannot take a turbo stream still destroys the message (the
     * source's order). */
    rc = msg_destroy_message(ctx, &room, &message, actor_id);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        cf_message_dispose(&message);
        return rc;
    }

    const cf_format *offered[1] = {&cf_format_turbo_stream};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    cf_view_message view = {0};
    if (rc == CF_OK) rc = cf_presenter_message(ctx, &message, &view);
    if (rc == CF_OK) {
        cf_builder body = {0};
        rc = cf_view_message_destroy_stream(&view, &body);
        if (rc == CF_OK) {
            rc = msg_send(ctx, 200, MSG_CONTENT_TURBO, &body, false);
        } else {
            cf_builder_dispose(&body);
        }
    }
    cf_view_message_dispose(&view);
    cf_room_dispose(&room);
    cf_message_dispose(&message);
    return rc;
}
