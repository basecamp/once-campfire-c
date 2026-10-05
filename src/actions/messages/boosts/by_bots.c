/* src/actions/messages/boosts/by_bots.c — Messages::Boosts::ByBotsController
 * (task A-messages-boosts-by_bots; route ID 84 `create`, route ID 85
 * `destroy`; contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/messages/boosts/
 * by_bots.rs, the pinned port of
 * reference/app/controllers/messages/boosts/by_bots_controller.rb (bots boost
 * with the raw request body as the content):
 *
 *   before_action (Before::default().allow_bot_access(): authentication
 *   REQUIRED, bots allowed, forgery protection on but bot-exempt in the
 *   chain).
 *
 *   set_message: the room among `Current.user.rooms` (Room::find_for_user
 *           by params[:room_id]), then its message (Message::find_by_id
 *           filtered to the room); `head :not_found` without one.
 *   create: ensure_content_present (the raw request body, blank through
 *           String#blank?); create_boost (boosted by Current.user, the bot);
 *           broadcast_create; `render :show, status: :created` (the
 *           boosts/by_bots JSON).
 *   destroy: set_boost, with `rescue ActiveRecord::RecordNotFound` mapping
 *           to `head :not_found`; destroy_boost; `head :no_content`.
 *
 * The halted heads are concerns::head (the status plus the bare text/html
 * content type); the missing-message halt and the rescued missing boost map
 * the same way.  The JSON body is the pinned boosts/by_bots/show Jbuilder
 * (tmp/rust-ref/crates/views/src/messages/json.rs BoostJson field order),
 * serialized with R01's cf_json_string; absolute URLs use PUBLIC_ORIGIN
 * (03-application.md; D-C07).
 *
 * c_symbols for the integrator's route rebind (src/routes.c rows 84/85,
 * currently the development 501):
 * cf_action_messages_boosts_by_bots_create,
 * cf_action_messages_boosts_by_bots_destroy.
 *
 * Integrator requests (no shared home exists yet; the static helpers below
 * are marked shims with the proposed declarations):
 *   B1. Shared bot-JSON serializers for MessageJson/BoostJson (views.h):
 *         cf_err cf_views_message_json(cf_ctx *, const cf_message *,
 *                                      cf_builder *out);
 *         cf_err cf_views_boost_json(cf_ctx *, const cf_boost *,
 *                                    const cf_message *, cf_builder *out);
 *   B2. Shared request-base absolute URL helper for JSON payloads (context.h
 *       or a urls header), so JSON links follow one rule with redirects.
 *
 * Write-transaction revalidation mirrors A-messages-boosts: the pre-write
 * room/message scope is only the fast path; the callback rechecks the
 * actor's membership, the actor's active status and the message scope
 * before the first mutation.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "cable/channels.h"
#include "config.h"
#include "context.h"
#include "models/boost.h"
#include "models/membership.h"
#include "models/message.h"
#include "models/room.h"
#include "models/user.h"
#include "richtext.h"
#include "views.h"
#include "views/internal.h" /* cf_views_integer_cast (ruby_compat::integer_cast) */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BB_CONTENT_JSON "application/json; charset=utf-8"
#define BB_HEAD_HTML "text/html"

static cf_span bb_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `c.param_str(key)`: only a string parameter reads. */
static bool bb_param_string(const cf_ctx *ctx, const char *name,
                            cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, bb_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) return false;
    return cf_param_string(param, out) == CF_OK;
}

static cf_err bb_current_user(cf_ctx *ctx, cf_user *out) {
    bool found = false;
    cf_err rc = cf_auth_current_user(ctx, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_INTERNAL;
    return CF_OK;
}

/* `concerns::head(status)`: the status plus the bare text/html content
 * type (204/304 carry no content type). */
static cf_err bb_head(cf_ctx *ctx, unsigned status) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    ctx->response->status = status;
    if (status == 204 || status == 304) return CF_OK;
    return cf_response_header(ctx->response, bb_span("Content-Type"),
                              bb_span(BB_HEAD_HTML));
}

/* `RawRequestBody#raw_request_body`: the whole body, as UTF-8 (lossy, like
 * String::from_utf8_lossy).  Invalid sequences become U+FFFD. */
static cf_err bb_raw_body(const cf_ctx *ctx, cf_str *out) {
    memset(out, 0, sizeof *out);
    cf_span body = ctx->request->body;
    if (body.len != 0 && body.ptr == NULL) return CF_INVALID;
    char *copy = malloc(body.len * 3 + 1);
    if (copy == NULL) return CF_NOMEM;
    size_t at = 0;
    size_t i = 0;
    while (i < body.len) {
        unsigned char b = body.ptr[i];
        size_t len = 0;
        if (b < 0x80) {
            len = 1;
        } else if (b >= 0xC2 && b <= 0xDF) {
            len = 2;
        } else if (b >= 0xE0 && b <= 0xEF) {
            len = 3;
        } else if (b >= 0xF0 && b <= 0xF4) {
            len = 4;
        }
        bool valid = len > 1 && i + len <= body.len;
        if (valid) {
            for (size_t k = 1; k < len; k++) {
                if ((body.ptr[i + k] & 0xC0) != 0x80) {
                    valid = false;
                    break;
                }
            }
        }
        if (len == 1) {
            copy[at++] = (char)b;
            i += 1;
            continue;
        }
        if (!valid) {
            /* U+FFFD, like String::from_utf8_lossy. */
            memcpy(copy + at, "\xEF\xBF\xBD", 3);
            at += 3;
            i += 1;
            continue;
        }
        memcpy(copy + at, body.ptr + i, len);
        at += len;
        i += len;
    }
    copy[at] = '\0';
    out->ptr = copy;
    out->len = at;
    return CF_OK;
}

static void bb_str_dispose(cf_str *value) {
    if (value == NULL) return;
    free(value->ptr);
    memset(value, 0, sizeof *value);
}

/* `String#blank?` over Unicode White_Space (by_bots.rs is_blank, through
 * char::is_whitespace): empty or only whitespace. */
static bool bb_utf8_next(cf_span text, size_t *index, uint32_t *cp) {
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

static bool bb_cp_whitespace(uint32_t cp) {
    if (cp < 0x80) {
        return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\v' ||
               cp == '\f' || cp == '\r';
    }
    return cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

static bool bb_is_blank(cf_span text) {
    size_t i = 0;
    while (i < text.len) {
        uint32_t cp = 0;
        if (!bb_utf8_next(text, &i, &cp)) return false;
        if (!bb_cp_whitespace(cp)) return false;
    }
    return true;
}

/* The room among `Current.user.rooms`, then its message; `head :not_found`
 * without one. */
static cf_err bb_set_message(cf_ctx *ctx, int64_t user_id, cf_message *out) {
    cf_span room_text;
    int64_t room_id = 0;
    if (!bb_param_string(ctx, "room_id", &room_text) ||
        !cf_views_integer_cast(room_text, &room_id)) {
        goto not_found;
    }
    cf_span message_text;
    int64_t message_id = 0;
    if (!bb_param_string(ctx, "message_id", &message_text) ||
        !cf_views_integer_cast(message_text, &message_id)) {
        goto not_found;
    }
    {
        bool room_found = false;
        cf_room room = {0};
        cf_err rc =
            cf_room_find_for_user(ctx->reader, user_id, room_id, &room_found,
                                  &room);
        if (rc != CF_OK) {
            cf_room_dispose(&room);
            return rc;
        }
        if (!room_found) {
            cf_room_dispose(&room);
            goto not_found;
        }
        bool message_found = false;
        cf_message message = {0};
        rc = cf_message_find_by_id(ctx->reader, message_id, &message_found,
                                   &message);
        if (rc != CF_OK) {
            cf_room_dispose(&room);
            cf_message_dispose(&message);
            return rc;
        }
        bool scoped =
            message_found && message.room_id == room.id;
        cf_room_dispose(&room);
        if (!scoped) {
            cf_message_dispose(&message);
            goto not_found;
        }
        *out = message;
        return CF_OK;
    }
not_found:
    memset(out, 0, sizeof *out);
    return bb_head(ctx, 404);
}

/* `set_boost`, with `rescue ActiveRecord::RecordNotFound` mapping to
 * `head :not_found`. */
static cf_err bb_set_boost(cf_ctx *ctx, const cf_message *message,
                           int64_t user_id, cf_boost *out) {
    cf_span text;
    int64_t id = 0;
    if (!bb_param_string(ctx, "id", &text) ||
        !cf_views_integer_cast(text, &id)) {
        memset(out, 0, sizeof *out);
        return bb_head(ctx, 404);
    }
    cf_err rc = cf_boost_find_by_message_and_booster(ctx->reader, message->id,
                                                     id, user_id, out);
    if (rc == CF_NOT_FOUND) {
        memset(out, 0, sizeof *out);
        return bb_head(ctx, 404);
    }
    return rc;
}

/* ------------------------------------------------------- writer ------------------------------------------------------- */

static cf_err bb_revalidate(cf_tx *tx, int64_t room_id, int64_t actor_id,
                            int64_t message_id, cf_message *fresh) {
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
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
    cf_user_dispose(&actor);
    return cf_message_find_in_room(db, room_id, message_id, fresh);
}

typedef struct {
    int64_t room_id;
    int64_t actor_id;
    int64_t message_id;
    cf_str content; /* borrowed */
    cf_boost boost;
} bb_create_write;

static cf_err bb_create_write_cb(cf_tx *tx, void *arg) {
    bb_create_write *write = arg;
    cf_message fresh = {0};
    cf_err rc = bb_revalidate(tx, write->room_id, write->actor_id,
                              write->message_id, &fresh);
    cf_message_dispose(&fresh);
    if (rc != CF_OK) return rc;
    return cf_boost_create(tx, write->message_id, write->actor_id,
                           write->content, &write->boost);
}

typedef struct {
    int64_t room_id;
    int64_t actor_id;
    int64_t message_id;
    int64_t boost_id;
} bb_destroy_write;

static cf_err bb_destroy_write_cb(cf_tx *tx, void *arg) {
    bb_destroy_write *write = arg;
    cf_message fresh = {0};
    cf_err rc = bb_revalidate(tx, write->room_id, write->actor_id,
                              write->message_id, &fresh);
    cf_message_dispose(&fresh);
    if (rc != CF_OK) return rc;
    cf_boost boost = {0};
    rc = cf_boost_find_by_message_and_booster(cf_tx_db(tx), write->message_id,
                                              write->boost_id, write->actor_id,
                                              &boost);
    if (rc != CF_OK) {
        cf_boost_dispose(&boost);
        return rc;
    }
    rc = cf_boost_destroy(tx, &boost);
    cf_boost_dispose(&boost);
    return rc;
}

/* ------------------------------------------------------- broadcasts ------------------------------------------------------- */

static cf_err bb_cable(cf_ctx *ctx, cf_cable **out) {
    *out = NULL;
    if (ctx->app == NULL) return CF_INVALID;
    cf_cable *cable = cf_app_cable(ctx->app);
    if (cable == NULL) return CF_INTERNAL;
    *out = cable;
    return CF_OK;
}

static cf_err bb_broadcast_outcome(cf_err rc, const char *what) {
    if (rc == CF_BUSY) {
        cf_cable_log("dropped broadcast", what);
        return CF_OK;
    }
    return rc;
}

static cf_err bb_broadcast_create(cf_ctx *ctx, const cf_room *room,
                                  const cf_message *message,
                                  const cf_boost *boost) {
    cf_cable *cable = NULL;
    cf_err rc = bb_cable(ctx, &cable);
    if (rc != CF_OK) return rc;
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, NULL);
    cf_broadcast_views views = {ctx, &view_ctx};
    cf_broadcast_partials partials;
    cf_broadcast_partials_views(&partials, &views);
    rc = cf_broadcast_boost_create(cable, room, message, boost, &partials);
    return bb_broadcast_outcome(rc, "messages/boosts/by_bots#create");
}

static cf_err bb_broadcast_remove(cf_ctx *ctx, const cf_room *room,
                                  const cf_boost *boost) {
    cf_cable *cable = NULL;
    cf_err rc = bb_cable(ctx, &cable);
    if (rc != CF_OK) return rc;
    rc = cf_broadcast_boost_remove(cable, room, boost);
    return bb_broadcast_outcome(rc, "messages/boosts/by_bots#destroy");
}

/* ------------------------------------------------------- JSON (shim B1) ------------------------------------------------------- */

/* UTC civil date from days since 1970-01-01 (Howard Hinnant's algorithm;
 * the same conversion A-messages uses for its cache versions). */
static void bb_civil_from_days(int64_t days, int64_t *year, unsigned *month,
                               unsigned *day) {
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    *year = y + (m <= 2);
    *month = m;
    *day = d;
}

/* `messages/support.rs json_time`: `time.as_json`, Active Support default
 * precision (`2026-09-26T12:26:46.848Z`). */
static cf_err bb_json_time(int64_t time_us, cf_builder *out) {
    int64_t secs = time_us / 1000000;
    int64_t rem = time_us % 1000000;
    if (rem < 0) {
        secs -= 1;
        rem += 1000000;
    }
    int64_t days = secs / 86400;
    int64_t sod = secs % 86400;
    if (sod < 0) {
        days -= 1;
        sod += 86400;
    }
    int64_t year = 0;
    unsigned month = 0, day = 0;
    bb_civil_from_days(days, &year, &month, &day);
    char text[32];
    int n = snprintf(text, sizeof text,
                     "%04" PRId64 "-%02u-%02uT%02" PRId64 ":%02" PRId64
                     ":%02" PRId64 ".%03" PRId64 "Z",
                     year, month, day, sod / 3600, (sod % 3600) / 60,
                     sod % 60, rem / 1000);
    if (n < 0 || (size_t)n >= sizeof text) return CF_INTERNAL;
    return cf_builder_append(out, (cf_span){(const unsigned char *)text,
                                            (size_t)n});
}

static cf_err bb_json_i64(cf_builder *out, int64_t value) {
    char text[24];
    int n = snprintf(text, sizeof text, "%" PRId64, value);
    if (n < 0 || (size_t)n >= sizeof text) return CF_INTERNAL;
    return cf_builder_append(out, (cf_span){(const unsigned char *)text,
                                            (size_t)n});
}

/* Absolute URL for a JSON payload (shim B2): PUBLIC_ORIGIN + path. */
static cf_err bb_absolute_url(cf_ctx *ctx, cf_span path, cf_builder *out) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_err rc = cf_builder_append(out, bb_span(config->public_origin));
    if (rc == CF_OK) rc = cf_builder_append(out, path);
    return rc;
}

/* `users/_user.json.jbuilder`: `json.(user, :id, :name, :role)` and the
 * absolute avatar_url. */
static cf_err bb_user_json(cf_ctx *ctx, const cf_user *user, cf_builder *out) {
    cf_view_user view = {0};
    cf_err rc = cf_presenter_user_view(ctx, user, &view);
    if (rc != CF_OK) {
        cf_view_user_dispose(&view);
        return rc;
    }
    rc = cf_builder_append(out, bb_span("{\"id\":"));
    if (rc == CF_OK) rc = bb_json_i64(out, user->id);
    if (rc == CF_OK) rc = cf_builder_append(out, bb_span(",\"name\":"));
    if (rc == CF_OK) {
        rc = cf_json_string(out, (cf_span){(const unsigned char *)view.name.ptr,
                                           view.name.len});
    }
    if (rc == CF_OK) rc = cf_builder_append(out, bb_span(",\"role\":\""));
    if (rc == CF_OK) rc = cf_builder_append(out, bb_span(cf_role_name(user->role)));
    if (rc == CF_OK) rc = cf_builder_append(out, bb_span("\",\"avatar_url\":\""));
    if (rc == CF_OK) {
        rc = bb_absolute_url(ctx,
                              (cf_span){(const unsigned char *)view.avatar_url.ptr,
                                        view.avatar_url.len},
                              out);
    }
    if (rc == CF_OK) rc = cf_builder_append(out, bb_span("\"}"));
    cf_view_user_dispose(&view);
    return rc;
}

/* `messages/boosts/_boost.json.jbuilder` through boosts_by_bots_show. */
static cf_err bb_boost_json(cf_ctx *ctx, const cf_boost *boost,
                            const cf_message *message, cf_builder *out) {
    cf_user booster = {0};
    cf_err rc = cf_user_find(ctx->reader, boost->booster_id, &booster);
    if (rc != CF_OK) {
        cf_user_dispose(&booster);
        return rc;
    }
    rc = cf_builder_append(out, bb_span("{\"id\":"));
    if (rc == CF_OK) rc = bb_json_i64(out, boost->id);
    if (rc == CF_OK) rc = cf_builder_append(out, bb_span(",\"content\":"));
    if (rc == CF_OK) {
        rc = cf_json_string(out, (cf_span){(const unsigned char *)boost->content.ptr,
                                           boost->content.len});
    }
    if (rc == CF_OK) rc = cf_builder_append(out, bb_span(",\"created_at\":\""));
    if (rc == CF_OK) rc = bb_json_time(boost->created_at, out);
    if (rc == CF_OK) rc = cf_builder_append(out, bb_span("\",\"booster\":"));
    if (rc == CF_OK) rc = bb_user_json(ctx, &booster, out);
    cf_user_dispose(&booster);
    if (rc != CF_OK) return rc;
    rc = cf_builder_append(out, bb_span(",\"message\":{\"id\":"));
    if (rc == CF_OK) rc = bb_json_i64(out, boost->message_id);
    if (rc == CF_OK) rc = cf_builder_append(out, bb_span(",\"url\":\""));
    if (rc == CF_OK) {
        char path[64];
        int n = snprintf(path, sizeof path, "/rooms/%" PRId64 "/messages/%" PRId64,
                         message->room_id, message->id);
        if (n < 0 || (size_t)n >= sizeof path) return CF_INTERNAL;
        rc = bb_absolute_url(ctx, (cf_span){(const unsigned char *)path,
                                            (size_t)n},
                             out);
    }
    if (rc == CF_OK) rc = cf_builder_append(out, bb_span("\"}}"));
    return rc;
}

/* `Ctx::render(CREATED, JSON, body)`: `application/json; charset=utf-8`. */
static cf_err bb_send_json(cf_ctx *ctx, unsigned status, cf_builder *body) {
    cf_buf *buf = NULL;
    cf_err rc = cf_builder_freeze(body, &buf);
    if (rc != CF_OK) {
        cf_builder_dispose(body);
        return rc;
    }
    ctx->response->status = status;
    rc = cf_response_header(ctx->response, bb_span("Content-Type"),
                            bb_span(BB_CONTENT_JSON));
    if (rc == CF_OK && cf_ctx_vary_accept(ctx)) {
        rc = cf_response_header(ctx->response, bb_span("Vary"),
                                bb_span("Accept"));
    }
    if (rc == CF_OK) rc = cf_response_body(ctx->response, buf);
    cf_buf_release(buf);
    return rc;
}

/* ------------------------------------------------------- actions ------------------------------------------------------- */

cf_err cf_action_messages_boosts_by_bots_create(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    /* `Before::default().allow_bot_access()`: authentication REQUIRED,
     * bots allowed, forgery protection on (bot-exempt in the chain). */
    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, false, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = bb_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    int64_t user_id = user.id;
    cf_user_dispose(&user);

    cf_message message = {0};
    rc = bb_set_message(ctx, user_id, &message);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_message_dispose(&message);
        return rc;
    }

    /* ensure_content_present over the raw request body. */
    cf_str content = {0};
    rc = bb_raw_body(ctx, &content);
    if (rc != CF_OK) {
        cf_message_dispose(&message);
        return rc;
    }
    if (bb_is_blank((cf_span){(const unsigned char *)content.ptr,
                              content.len})) {
        bb_str_dispose(&content);
        cf_message_dispose(&message);
        return bb_head(ctx, 422);
    }

    bb_create_write write;
    memset(&write, 0, sizeof write);
    write.room_id = message.room_id;
    write.actor_id = user_id;
    write.message_id = message.id;
    write.content = content;
    rc = cf_write(ctx->app, bb_create_write_cb, &write);
    bb_str_dispose(&content);
    if (rc != CF_OK) {
        cf_boost_dispose(&write.boost);
        cf_message_dispose(&message);
        return rc;
    }

    cf_room room = {0};
    rc = cf_room_find(ctx->reader, message.room_id, &room);
    if (rc == CF_OK) rc = bb_broadcast_create(ctx, &room, &message,
                                              &write.boost);
    cf_room_dispose(&room);
    if (rc != CF_OK) {
        cf_boost_dispose(&write.boost);
        cf_message_dispose(&message);
        return rc;
    }

    /* `render :show, status: :created` (the JSON boosts/show). */
    const cf_format *offered[1] = {&cf_format_json};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) {
        cf_boost_dispose(&write.boost);
        cf_message_dispose(&message);
        return rc;
    }
    cf_builder body = {0};
    rc = bb_boost_json(ctx, &write.boost, &message, &body);
    cf_boost_dispose(&write.boost);
    cf_message_dispose(&message);
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        return rc;
    }
    return bb_send_json(ctx, 201, &body);
}

cf_err cf_action_messages_boosts_by_bots_destroy(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, false, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = bb_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    int64_t user_id = user.id;
    cf_user_dispose(&user);

    cf_message message = {0};
    rc = bb_set_message(ctx, user_id, &message);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_message_dispose(&message);
        return rc;
    }
    cf_boost boost = {0};
    rc = bb_set_boost(ctx, &message, user_id, &boost);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_boost_dispose(&boost);
        cf_message_dispose(&message);
        return rc;
    }

    bb_destroy_write write = {.room_id = message.room_id,
                              .actor_id = user_id,
                              .message_id = message.id,
                              .boost_id = boost.id};
    rc = cf_write(ctx->app, bb_destroy_write_cb, &write);
    if (rc != CF_OK) {
        cf_boost_dispose(&boost);
        cf_message_dispose(&message);
        return rc;
    }

    cf_room room = {0};
    rc = cf_room_find(ctx->reader, message.room_id, &room);
    if (rc == CF_OK) rc = bb_broadcast_remove(ctx, &room, &boost);
    cf_room_dispose(&room);
    cf_boost_dispose(&boost);
    cf_message_dispose(&message);
    if (rc != CF_OK) return rc;

    ctx->response->status = 204;
    return CF_OK;
}
