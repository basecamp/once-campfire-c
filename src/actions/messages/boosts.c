/* src/actions/messages/boosts.c — Messages::BoostsController (task
 * A-messages-boosts; route IDs 129 `index`, 130 `create`, 131 `new`, 136
 * `destroy`; contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/messages/boosts.rs,
 * the pinned port of reference/app/controllers/messages/boosts_controller.rb
 * (whose show/edit/update routes have no action or template):
 *
 *   before_action (Before::default(): authentication REQUIRED, deny_bots,
 *   forgery protection) on every action.
 *
 *   set_message: `Current.user.reachable_messages.find(params[:message_id])`
 *           (the string param through integer_cast, else NotFound).
 *   index:  respond_to HTML; present the message; page::content (the boosts
 *           index in the application layout, or the frame layout for a
 *           Turbo-Frame request).
 *   new:    index + the current user's UserView for the boost form.
 *   create: params.require(:boost).permit(:content); the boost (boosted by
 *           Current.user) in one writer transaction; a nil content violates
 *           the column's NOT NULL (the reference's 500); broadcast_create
 *           (messages/boosts/_boost appended to the message's boosts);
 *           redirect to message_boosts(message.id).
 *   destroy: set_boost (@message.boosts.find_by!(id:, booster:
 *           Current.user)); destroy_boost (@boost.destroy! then
 *           broadcast_remove); head :no_content.
 *
 * The C context conventions follow src/auth.h: an action is
 * `cf_err cf_action_NAME(cf_ctx *)`, a helper that answers the request sets
 * ctx->response and returns CF_OK, and the caller stops with
 * cf_auth_halted.  Absolute URLs use PUBLIC_ORIGIN (03-application.md, "URL
 * helpers ... use PUBLIC_ORIGIN for absolute URLs"; D-C07).
 *
 * c_symbols for the integrator's route rebind (src/routes.c rows 129-131,
 * 136, currently the development 501): cf_action_messages_boosts_index,
 * cf_action_messages_boosts_create, cf_action_messages_boosts_new,
 * cf_action_messages_boosts_destroy.
 *
 * Write-transaction revalidation (00-contracts.md "Mutation order" and
 * 02-data-auth.md D02) mirrors A-messages: the pre-write reachable lookup
 * is only the fast path; the writer callback rechecks the actor's
 * membership, the actor's active status and the message's room scope before
 * the first mutation.
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
#include "views.h"
#include "views/internal.h" /* cf_views_integer_cast (ruby_compat::integer_cast) */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOSTS_CONTENT_HTML "text/html; charset=utf-8"

static cf_span boosts_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `c.param_str(key)`: only a string parameter reads; anything else (a hash,
 * an array, a missing key) reads as absent. */
static bool boosts_param_string(const cf_ctx *ctx, const char *name,
                                cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, boosts_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) return false;
    return cf_param_string(param, out) == CF_OK;
}

/* `require_current_user`: the chain guarantees one. */
static cf_err boosts_current_user(cf_ctx *ctx, cf_user *out) {
    bool found = false;
    cf_err rc = cf_auth_current_user(ctx, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_INTERNAL;
    return CF_OK;
}

/* `c.is_turbo_frame_request()`: kit/src/ctx.rs `turbo_frame_request_id()`
 * over the first Turbo-Frame header through http 1.5.0's
 * `HeaderValue::to_str` (only HTAB or visible ASCII passes; any other byte
 * makes the header read as absent), nonempty after trimming spaces/tabs
 * (the same helper the other action packets carry until it has a shared
 * home). */
static unsigned char boosts_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

static bool boosts_turbo_frame_request(const cf_request *request) {
    if (request == NULL) return false;
    cf_span value = {NULL, 0};
    bool found = false;
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        static const char name[] = "turbo-frame";
        if (header->name.len != sizeof name - 1) continue;
        bool match = true;
        for (size_t k = 0; k < sizeof name - 1; k++) {
            if (boosts_lower(header->name.ptr[k]) !=
                (unsigned char)name[k]) {
                match = false;
                break;
            }
        }
        if (match) {
            value = header->value;
            found = true;
            break;
        }
    }
    if (!found) return false;
    bool blank = true;
    for (size_t i = 0; i < value.len; i++) {
        unsigned char b = value.ptr[i];
        if (!((b >= 32 && b < 127) || b == '\t')) return false;
        if (b != ' ' && b != '\t') blank = false;
    }
    return !blank;
}

/* `C.respond_to([HTML])`. */
static cf_err boosts_respond_html(cf_ctx *ctx) {
    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    return cf_ctx_respond_to(ctx, offered, 1, &chosen);
}

/* `Ctx::render`: content type, body, `Vary: Accept` when the format came
 * from the Accept header, the layout page's preload Link header for full
 * pages only (page::content's layout.page; the frame layout carries none,
 * like A-rooms' rooms_page_response).  Consumes the builder. */
static cf_err boosts_send(cf_ctx *ctx, unsigned status, cf_builder *body,
                          bool link_header) {
    cf_buf *buf = NULL;
    cf_err rc = cf_builder_freeze(body, &buf);
    if (rc != CF_OK) {
        cf_builder_dispose(body);
        return rc;
    }
    ctx->response->status = status;
    rc = cf_response_header(ctx->response, boosts_span("Content-Type"),
                            boosts_span(BOOSTS_CONTENT_HTML));
    if (rc == CF_OK && cf_ctx_vary_accept(ctx)) {
        rc = cf_response_header(ctx->response, boosts_span("Vary"),
                                boosts_span("Accept"));
    }
    if (rc == CF_OK) rc = cf_response_body(ctx->response, buf);
    cf_buf_release(buf);
    if (rc != CF_OK) return rc;
    if (link_header) {
        cf_builder links = {0};
        cf_err link_rc = cf_views_preload_links(&links);
        if (link_rc == CF_INVALID) link_rc = CF_OK; /* unconfigured assets */
        if (link_rc == CF_OK && links.len != 0) {
            link_rc = cf_response_header(ctx->response, boosts_span("Link"),
                                          (cf_span){links.ptr, links.len});
        }
        cf_builder_dispose(&links);
        if (link_rc != CF_OK) return link_rc;
    }
    return CF_OK;
}

/* `redirect_to <PUBLIC_ORIGIN><path>` (Rails default 302). */
static cf_err boosts_redirect(cf_ctx *ctx, cf_span path) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    cf_err rc = cf_builder_append(&location, boosts_span(config->public_origin));
    if (rc == CF_OK) rc = cf_builder_append(&location, path);
    if (rc == CF_OK) {
        ctx->response->status = 302;
        rc = cf_response_header(ctx->response, boosts_span("Location"),
                                (cf_span){location.ptr, location.len});
        if (rc == CF_OK) {
            rc = cf_response_header(ctx->response,
                                    boosts_span("Content-Type"),
                                    boosts_span(BOOSTS_CONTENT_HTML));
        }
    }
    cf_builder_dispose(&location);
    return rc;
}

/* `set_message`: `Current.user.reachable_messages.find(params[:message_id])`
 * (boosts.rs set_message). */
static cf_err boosts_set_message(cf_ctx *ctx, int64_t user_id,
                                 cf_message *out) {
    cf_span text;
    if (!boosts_param_string(ctx, "message_id", &text)) return CF_NOT_FOUND;
    int64_t id = 0;
    if (!cf_views_integer_cast(text, &id)) return CF_NOT_FOUND;
    return cf_message_find_reachable(ctx->reader, user_id, id, out);
}

/* `set_boost`: `@message.boosts.find_by!(id: params[:id], booster:
 * Current.user)` (boosts.rs set_boost). */
static cf_err boosts_set_boost(cf_ctx *ctx, const cf_message *message,
                               int64_t user_id, cf_boost *out) {
    cf_span text;
    if (!boosts_param_string(ctx, "id", &text)) return CF_NOT_FOUND;
    int64_t id = 0;
    if (!cf_views_integer_cast(text, &id)) return CF_NOT_FOUND;
    return cf_boost_find_by_message_and_booster(ctx->reader, message->id, id,
                                                user_id, out);
}

/* The permitted scalar value for `key`; NULL when the key is absent or the
 * value is an array/object (strong parameters). */
static const cf_param *boosts_permitted(const cf_param *object,
                                       const char *key) {
    if (object == NULL || cf_param_type(object) != CF_PARAM_OBJECT) {
        return NULL;
    }
    const cf_param *value = cf_param_field(object, boosts_span(key));
    if (value == NULL) return NULL;
    cf_param_kind kind = cf_param_type(value);
    if (kind == CF_PARAM_ARRAY || kind == CF_PARAM_OBJECT) return NULL;
    return value;
}

static cf_span boosts_empty_span(void) {
    return (cf_span){(const unsigned char *)"", 0};
}

/* `String#blank?` over Unicode White_Space (kit params.rs
 * `Param::is_blank` for strings): empty or only whitespace. */
static bool boosts_utf8_next(cf_span text, size_t *index, uint32_t *cp) {
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

static bool boosts_cp_whitespace(uint32_t cp) {
    if (cp < 0x80) {
        return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\v' ||
               cp == '\f' || cp == '\r';
    }
    return cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

static bool boosts_span_blank(cf_span text) {
    size_t i = 0;
    while (i < text.len) {
        uint32_t cp = 0;
        if (!boosts_utf8_next(text, &i, &cp)) return false;
        if (!boosts_cp_whitespace(cp)) return false;
    }
    return true;
}

/* `params.require(:boost).permit(:content).get("content").as_str()`: the
 * required object (a missing key is ParameterMissing, answered 400 by
 * dispatch; like A-messages' require, the false literal passes), then the
 * permitted scalar, which may be absent (a nil content is the reference's
 * NOT NULL 500, not a 400). */
static cf_err boosts_boost_content(cf_ctx *ctx, bool *present,
                                   cf_span *content) {
    *present = false;
    *content = boosts_empty_span();
    const cf_param *boost = cf_ctx_param(ctx, boosts_span("boost"));
    bool required = false;
    if (boost != NULL) {
        switch (cf_param_type(boost)) {
        case CF_PARAM_BOOL:
            required = true; /* the false literal is what require takes */
            break;
        case CF_PARAM_NULL:
            required = false;
            break;
        case CF_PARAM_STRING: {
            cf_span text;
            if (cf_param_string(boost, &text) == CF_OK &&
                !boosts_span_blank(text)) {
                required = true;
            }
            break;
        }
        case CF_PARAM_NUMBER:
        case CF_PARAM_UPLOAD:
            required = true;
            break;
        case CF_PARAM_ARRAY:
        case CF_PARAM_OBJECT:
            required = cf_param_count(boost) > 0;
            break;
        }
    }
    if (!required) return CF_INVALID;
    const cf_param *value = boosts_permitted(boost, "content");
    if (value == NULL || cf_param_type(value) != CF_PARAM_STRING) {
        return CF_OK; /* absent: the NOT NULL arm below */
    }
    if (cf_param_string(value, content) != CF_OK) return CF_INTERNAL;
    *present = true;
    return CF_OK;
}

/* ------------------------------------------------------- writer ------------------------------------------------------- */

/* D02 revalidation for the boost writes: the actor's membership in the
 * message's room (the reachable scope), the actor's active status, and the
 * message still in that room.  `fresh` receives the message row as it
 * exists now. */
static cf_err boosts_revalidate(cf_tx *tx, int64_t room_id, int64_t actor_id,
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
} boosts_create_write;

static cf_err boosts_create_write_cb(cf_tx *tx, void *arg) {
    boosts_create_write *write = arg;
    cf_message fresh = {0};
    cf_err rc = boosts_revalidate(tx, write->room_id, write->actor_id,
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
} boosts_destroy_write;

static cf_err boosts_destroy_write_cb(cf_tx *tx, void *arg) {
    boosts_destroy_write *write = arg;
    cf_message fresh = {0};
    cf_err rc = boosts_revalidate(tx, write->room_id, write->actor_id,
                                  write->message_id, &fresh);
    cf_message_dispose(&fresh);
    if (rc != CF_OK) return rc;
    /* The boost still this actor's row of this message. */
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

/* `c.app().broadcasts`: app.h's cable accessor (landed by the app repair,
 * set once in main.c after cf_cable_create). A NULL cable is an internal
 * error, never a silently dropped broadcast. */
static cf_err boosts_cable(cf_ctx *ctx, cf_cable **out) {
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
 * Other failures propagate. */
static cf_err boosts_broadcast_outcome(cf_err rc, const char *what) {
    if (rc == CF_BUSY) {
        cf_cable_log("dropped broadcast", what);
        return CF_OK;
    }
    return rc;
}

static cf_err boosts_broadcast_create(cf_ctx *ctx, const cf_room *room,
                                      const cf_message *message,
                                      const cf_boost *boost) {
    cf_cable *cable = NULL;
    cf_err rc = boosts_cable(ctx, &cable);
    if (rc != CF_OK) return rc;
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, NULL);
    cf_broadcast_views views = {ctx, &view_ctx};
    cf_broadcast_partials partials;
    cf_broadcast_partials_views(&partials, &views);
    rc = cf_broadcast_boost_create(cable, room, message, boost, &partials);
    return boosts_broadcast_outcome(rc, "messages/boosts#create");
}

static cf_err boosts_broadcast_remove(cf_ctx *ctx, const cf_room *room,
                                      const cf_boost *boost) {
    cf_cable *cable = NULL;
    cf_err rc = boosts_cable(ctx, &cable);
    if (rc != CF_OK) return rc;
    rc = cf_broadcast_boost_remove(cable, room, boost);
    return boosts_broadcast_outcome(rc, "messages/boosts#destroy");
}

/* `Room::find` for the broadcast room, read after the mutation like the
 * reference's read blocks. */
static cf_err boosts_broadcast_room(cf_ctx *ctx, int64_t room_id,
                                    cf_room *out) {
    return cf_room_find(ctx->reader, room_id, out);
}

/* ------------------------------------------------------- page ------------------------------------------------------- */

/* `present(|presenter| presenter.message(&message))` then page::content:
 * the boosts index/new page, or its frame for a Turbo-Frame request. */
static cf_err boosts_render_page(cf_ctx *ctx, const cf_message *message,
                                 const cf_view_user *user_or_null) {
    cf_view_message view = {0};
    cf_err rc = cf_presenter_message(ctx, message, &view);
    if (rc != CF_OK) {
        cf_view_message_dispose(&view);
        return rc;
    }
    cf_view_layout_model layout = {0};
    rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) {
        cf_view_message_dispose(&view);
        return rc;
    }
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);
    bool frame = boosts_turbo_frame_request(ctx->request);
    cf_builder body = {0};
    if (user_or_null == NULL) {
        rc = frame ? cf_view_boosts_index_frame(&view_ctx, &view, &body)
                   : cf_view_boosts_index(&view_ctx, &view, &body);
    } else {
        rc = frame ? cf_view_new_boost_frame(&view_ctx, &view, user_or_null,
                                             &body)
                   : cf_view_new_boost(&view_ctx, &view, user_or_null, &body);
    }
    if (rc == CF_OK) rc = boosts_send(ctx, 200, &body, !frame);
    else cf_builder_dispose(&body);
    cf_view_layout_model_dispose(&layout);
    cf_view_message_dispose(&view);
    return rc;
}

/* ------------------------------------------------------- actions ------------------------------------------------------- */

cf_err cf_action_messages_boosts_index(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, true, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = boosts_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    int64_t user_id = user.id;
    cf_user_dispose(&user);

    cf_message message = {0};
    rc = boosts_set_message(ctx, user_id, &message);
    if (rc != CF_OK) {
        cf_message_dispose(&message);
        return rc;
    }
    rc = boosts_respond_html(ctx);
    if (rc != CF_OK) {
        cf_message_dispose(&message);
        return rc;
    }
    rc = boosts_render_page(ctx, &message, NULL);
    cf_message_dispose(&message);
    return rc;
}

cf_err cf_action_messages_boosts_new(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, true, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = boosts_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    cf_view_user viewer = {0};
    rc = cf_presenter_user_view(ctx, &user, &viewer);
    int64_t user_id = user.id;
    cf_user_dispose(&user);
    if (rc != CF_OK) {
        cf_view_user_dispose(&viewer);
        return rc;
    }

    cf_message message = {0};
    rc = boosts_set_message(ctx, user_id, &message);
    if (rc != CF_OK) {
        cf_view_user_dispose(&viewer);
        cf_message_dispose(&message);
        return rc;
    }
    rc = boosts_respond_html(ctx);
    if (rc != CF_OK) {
        cf_view_user_dispose(&viewer);
        cf_message_dispose(&message);
        return rc;
    }
    rc = boosts_render_page(ctx, &message, &viewer);
    cf_view_user_dispose(&viewer);
    cf_message_dispose(&message);
    return rc;
}

cf_err cf_action_messages_boosts_create(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, true, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = boosts_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    int64_t user_id = user.id;
    cf_user_dispose(&user);

    cf_message message = {0};
    rc = boosts_set_message(ctx, user_id, &message);
    if (rc != CF_OK) {
        cf_message_dispose(&message);
        return rc;
    }

    /* params.require(:boost).permit(:content). */
    bool has_content = false;
    cf_span content = boosts_empty_span();
    rc = boosts_boost_content(ctx, &has_content, &content);
    if (rc != CF_OK) {
        cf_message_dispose(&message);
        return rc;
    }
    if (!has_content) {
        /* A nil content violates the column's NOT NULL
         * (ActiveRecord::NotNullViolation, a 500). */
        cf_message_dispose(&message);
        return CF_INTERNAL;
    }

    boosts_create_write write;
    memset(&write, 0, sizeof write);
    write.room_id = message.room_id;
    write.actor_id = user_id;
    write.message_id = message.id;
    write.content = (cf_str){(char *)(uintptr_t)content.ptr, content.len};
    rc = cf_write(ctx->app, boosts_create_write_cb, &write);
    if (rc != CF_OK) {
        cf_boost_dispose(&write.boost);
        cf_message_dispose(&message);
        return rc;
    }

    cf_room room = {0};
    rc = boosts_broadcast_room(ctx, message.room_id, &room);
    if (rc == CF_OK) rc = boosts_broadcast_create(ctx, &room, &message,
                                                  &write.boost);
    cf_room_dispose(&room);
    cf_boost_dispose(&write.boost);
    if (rc != CF_OK) {
        cf_message_dispose(&message);
        return rc;
    }

    char path[48];
    int n = snprintf(path, sizeof path, "/messages/%" PRId64 "/boosts",
                     message.id);
    cf_message_dispose(&message);
    if (n < 0 || (size_t)n >= sizeof path) return CF_INTERNAL;
    return boosts_redirect(ctx, (cf_span){(const unsigned char *)path,
                                          (size_t)n});
}

cf_err cf_action_messages_boosts_destroy(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, true, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = boosts_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    int64_t user_id = user.id;
    cf_user_dispose(&user);

    cf_message message = {0};
    rc = boosts_set_message(ctx, user_id, &message);
    if (rc != CF_OK) {
        cf_message_dispose(&message);
        return rc;
    }
    cf_boost boost = {0};
    rc = boosts_set_boost(ctx, &message, user_id, &boost);
    if (rc != CF_OK) {
        cf_boost_dispose(&boost);
        cf_message_dispose(&message);
        return rc;
    }

    boosts_destroy_write write = {.room_id = message.room_id,
                                  .actor_id = user_id,
                                  .message_id = message.id,
                                  .boost_id = boost.id};
    rc = cf_write(ctx->app, boosts_destroy_write_cb, &write);
    if (rc != CF_OK) {
        cf_boost_dispose(&boost);
        cf_message_dispose(&message);
        return rc;
    }

    /* `@boost.destroy!` then `broadcast_remove`; no destroy template:
     * `head :no_content`. */
    cf_room room = {0};
    rc = boosts_broadcast_room(ctx, message.room_id, &room);
    if (rc == CF_OK) rc = boosts_broadcast_remove(ctx, &room, &boost);
    cf_room_dispose(&room);
    cf_boost_dispose(&boost);
    cf_message_dispose(&message);
    if (rc != CF_OK) return rc;

    ctx->response->status = 204;
    return CF_OK;
}
