/* src/actions/messages/by_bots.c — Messages::ByBotsController (task
 * A-messages-by_bots; route ID 86 `index`, route ID 87 `create`, route IDs
 * 88/89 `update`, route ID 90 `destroy`; contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/messages/by_bots.rs,
 * the pinned port of reference/app/controllers/messages/by_bots_controller.rb
 * (the bot API under /rooms/:room_id/:bot_key/messages, JSON by route
 * default; bodies are the raw request body, or a top-level multipart
 * `attachment`):
 *
 *   before_action (Before::default().allow_bot_access(): authentication
 *   REQUIRED, bots allowed, forgery protection on but bot-exempt).
 *
 *   set_room: `Current.user.rooms.find_by(id: params[:room_id])`, else
 *           `head :not_found`.
 *   index:  find_paged_messages (the shared page-before/after/last page);
 *           X-Total-Count and the next-page Link headers; respond_to JSON;
 *           the by_bots index JSON.
 *   create: `head :unprocessable_content` when the attachment is blank and
 *           the raw request body is blank; message_params (the attachment
 *           when the key is present and non-null, else the raw body);
 *           create_message; broadcast_create; deliver_webhooks_to_bots;
 *           `head :created` with the message Location.
 *   update: set_message (@room.messages.find(params[:id]));
 *           ensure_can_administer (403); message_params; update_message;
 *           broadcast_replace; respond_to HTML/JSON (JSON renders the
 *           by_bots show, HTML redirects to the room message).
 *   destroy: set_message; ensure_can_administer; destroy_message;
 *           `head :no_content`.
 *
 * The halted heads are concerns::head (the status plus the bare text/html
 * content type).  The JSON bodies are the pinned by_bots Jbuilder shapes
 * (tmp/rust-ref/crates/views/src/messages/json.rs field order), serialized
 * with R01's cf_json_string; absolute URLs use PUBLIC_ORIGIN (03-application
 * .md; D-C07).  The message create/update/destroy write paths (rich-text
 * canonicalization, the attachment arms, webhook delivery) are the
 * A-messages translation; multipart never builds an upload param today, so
 * the Create arm fails loudly (the reference's 500 for an unbuildable
 * attachment) instead of being faked.
 *
 * c_symbols for the integrator's route rebind (src/routes.c rows 86-90,
 * currently the development 501): cf_action_messages_by_bots_index,
 * cf_action_messages_by_bots_create, cf_action_messages_by_bots_update
 * (routes 88 and 89 share the symbol), cf_action_messages_by_bots_destroy.
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
 * Write-transaction revalidation mirrors A-messages: the pre-write room and
 * privilege checks are only the fast path; each writer callback rechecks
 * the actor's membership, the actor's active status (and, for update and
 * destroy, the administer privilege against the fresh row) before the first
 * mutation.
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

#define BYB_CONTENT_JSON "application/json; charset=utf-8"
#define BYB_HEAD_HTML "text/html"

static cf_span byb_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `c.param_str(key)`: only a string parameter reads. */
static bool byb_param_string(const cf_ctx *ctx, const char *name,
                             cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, byb_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) return false;
    return cf_param_string(param, out) == CF_OK;
}

static cf_err byb_current_user(cf_ctx *ctx, cf_user *out) {
    bool found = false;
    cf_err rc = cf_auth_current_user(ctx, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_INTERNAL;
    return CF_OK;
}

/* `concerns::head(status)`: the status plus the bare text/html content
 * type (204/304 carry no content type). */
static cf_err byb_head(cf_ctx *ctx, unsigned status) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    ctx->response->status = status;
    if (status == 204 || status == 304) return CF_OK;
    return cf_response_header(ctx->response, byb_span("Content-Type"),
                              byb_span(BYB_HEAD_HTML));
}

/* `Param::is_blank` (kit params.rs): Null is blank, a bool is blank when
 * false, numbers and uploads never are, strings use String#blank?, empty
 * arrays/objects are blank. */
static bool byb_utf8_next(cf_span text, size_t *index, uint32_t *cp) {
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

static bool byb_cp_whitespace(uint32_t cp) {
    if (cp < 0x80) {
        return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\v' ||
               cp == '\f' || cp == '\r';
    }
    return cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

static bool byb_span_blank(cf_span text) {
    size_t i = 0;
    while (i < text.len) {
        uint32_t cp = 0;
        if (!byb_utf8_next(text, &i, &cp)) return false;
        if (!byb_cp_whitespace(cp)) return false;
    }
    return true;
}

static bool byb_param_blank(const cf_param *param) {
    if (param == NULL) return true;
    switch (cf_param_type(param)) {
    case CF_PARAM_NULL:
        return true;
    case CF_PARAM_BOOL: {
        bool present = false, value = false;
        if (cf_param_bool(param, &present, &value) != CF_OK) return true;
        return !value;
    }
    case CF_PARAM_NUMBER:
    case CF_PARAM_UPLOAD:
        return false;
    case CF_PARAM_STRING: {
        cf_span text;
        if (cf_param_string(param, &text) != CF_OK) return true;
        return byb_span_blank(text);
    }
    case CF_PARAM_ARRAY:
    case CF_PARAM_OBJECT:
        return cf_param_count(param) == 0;
    }
    return true;
}

static bool byb_param_present(const cf_param *param) {
    return !byb_param_blank(param);
}

/* `RawRequestBody#raw_request_body`: the whole body, as UTF-8 (lossy, like
 * String::from_utf8_lossy). */
static cf_err byb_raw_body(const cf_ctx *ctx, cf_str *out) {
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

static void byb_str_dispose(cf_str *value) {
    if (value == NULL) return;
    free(value->ptr);
    memset(value, 0, sizeof *value);
}

/* `set_room`: `Current.user.rooms.find_by(id: params[:room_id])`, else
 * `head :not_found`. */
static cf_err byb_set_room(cf_ctx *ctx, int64_t user_id, cf_room *out) {
    cf_span text;
    int64_t room_id = 0;
    if (!byb_param_string(ctx, "room_id", &text) ||
        !cf_views_integer_cast(text, &room_id)) {
        memset(out, 0, sizeof *out);
        return byb_head(ctx, 404);
    }
    bool found = false;
    cf_err rc =
        cf_room_find_for_user(ctx->reader, user_id, room_id, &found, out);
    if (rc != CF_OK) {
        cf_room_dispose(out);
        memset(out, 0, sizeof *out);
        return rc;
    }
    if (!found) {
        cf_room_dispose(out);
        memset(out, 0, sizeof *out);
        return byb_head(ctx, 404);
    }
    return CF_OK;
}

/* `@room.messages.find(params[:id])` (messages.rs set_message): a missing
 * or uncastable id, or a message of another room, is RecordNotFound. */
static cf_err byb_set_message(cf_ctx *ctx, const cf_room *room,
                              cf_message *out) {
    cf_span text;
    if (!byb_param_string(ctx, "id", &text)) return CF_NOT_FOUND;
    int64_t id = 0;
    if (!cf_views_integer_cast(text, &id)) return CF_NOT_FOUND;
    return cf_message_find_in_room(ctx->reader, room->id, id, out);
}

/* `head :forbidden unless Current.user.can_administer?(@message)`. */
static cf_err byb_ensure_can_administer(cf_ctx *ctx,
                                        const cf_message *message) {
    cf_user user = {0};
    cf_err rc = byb_current_user(ctx, &user);
    if (rc != CF_OK) return rc;
    bool allowed = cf_user_can_administer(
        &user, (cf_optional_i64){true, message->creator_id}, false);
    cf_user_dispose(&user);
    if (!allowed) return byb_head(ctx, 403);
    return CF_OK;
}

/* `present("before"/"after")`: whether the param is present and whether it
 * casts (`integer_cast`); present-but-uncastable is RecordNotFound. */
static void byb_integer_param(const cf_ctx *ctx, const char *name,
                              bool *given, bool *cast, int64_t *value) {
    *given = false;
    *cast = false;
    *value = 0;
    const cf_param *param = cf_ctx_param(ctx, byb_span(name));
    if (!byb_param_present(param)) return;
    *given = true;
    if (cf_param_type(param) != CF_PARAM_STRING) return;
    cf_span text;
    if (cf_param_string(param, &text) != CF_OK) return;
    *cast = cf_views_integer_cast(text, value);
}

/* find_paged_messages (messages.rs): `before` wins over `after`; each
 * names a message of this room (else RecordNotFound); neither is the last
 * page. */
static cf_err byb_find_paged(cf_ctx *ctx, const cf_room *room,
                             cf_message_vector *out) {
    bool has_before = false, before_ok = false, has_after = false,
         after_ok = false;
    int64_t before = 0, after = 0;
    byb_integer_param(ctx, "before", &has_before, &before_ok, &before);
    byb_integer_param(ctx, "after", &has_after, &after_ok, &after);
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

/* ------------------------------------------------------- params ------------------------------------------------------- */

typedef enum {
    BYB_ATTACHMENT_UNCHANGED = 0,
    BYB_ATTACHMENT_DELETE,
    BYB_ATTACHMENT_CREATE,
    BYB_ATTACHMENT_INVALID
} byb_attachment;

typedef struct {
    bool has_body;
    cf_str body; /* owned when has_body */
    byb_attachment attachment;
} byb_params;

static void byb_params_dispose(byb_params *params) {
    byb_str_dispose(&params->body);
    memset(params, 0, sizeof *params);
}

/* `attachments::Assignment::from_params` for the permitted `attachment`
 * (`params.permit(:attachment)`): absent (or dropped as an array/object by
 * strong parameters) is Unchanged, nil/"" is Delete, an uploaded file is
 * Create, anything else raises. */
static byb_attachment byb_attachment_assignment(cf_ctx *ctx) {
    const cf_param *value = cf_ctx_param(ctx, byb_span("attachment"));
    if (value == NULL) return BYB_ATTACHMENT_UNCHANGED;
    switch (cf_param_type(value)) {
    case CF_PARAM_NULL:
        return BYB_ATTACHMENT_DELETE;
    case CF_PARAM_STRING: {
        cf_span text;
        if (cf_param_string(value, &text) != CF_OK) {
            return BYB_ATTACHMENT_INVALID;
        }
        return text.len == 0 ? BYB_ATTACHMENT_DELETE
                             : BYB_ATTACHMENT_INVALID;
    }
    case CF_PARAM_UPLOAD:
        return BYB_ATTACHMENT_CREATE;
    case CF_PARAM_ARRAY:
    case CF_PARAM_OBJECT:
        /* Dropped by `permit` (not a permitted scalar): Unchanged. */
        return BYB_ATTACHMENT_UNCHANGED;
    default:
        return BYB_ATTACHMENT_INVALID;
    }
}

/* `head :unprocessable_content if params[:attachment].blank? &&
 * raw_request_body.blank?` */
static cf_err byb_ensure_body_or_attachment(cf_ctx *ctx) {
    const cf_param *attachment = cf_ctx_param(ctx, byb_span("attachment"));
    if (!byb_param_blank(attachment)) return CF_OK;
    cf_str body = {0};
    cf_err rc = byb_raw_body(ctx, &body);
    if (rc != CF_OK) return rc;
    bool blank = byb_span_blank((cf_span){(const unsigned char *)body.ptr,
                                          body.len});
    byb_str_dispose(&body);
    if (blank) return byb_head(ctx, 422);
    return CF_OK;
}

/* `params[:attachment] ? params.permit(:attachment) : { body:
 * raw_request_body }`: the attachment arm when the key is present and
 * non-null, else the raw body. */
static cf_err byb_message_params(cf_ctx *ctx, byb_params *out) {
    memset(out, 0, sizeof *out);
    const cf_param *attachment = cf_ctx_param(ctx, byb_span("attachment"));
    if (attachment != NULL &&
        cf_param_type(attachment) != CF_PARAM_NULL) {
        out->attachment = byb_attachment_assignment(ctx);
        /* `params.permit(:attachment)` reads the merged tree; the upload arm
         * below reports the S02 boundary. */
        return CF_OK;
    }
    cf_str body = {0};
    cf_err rc = byb_raw_body(ctx, &body);
    if (rc != CF_OK) return rc;
    out->body = body;
    out->has_body = true;
    out->attachment = BYB_ATTACHMENT_UNCHANGED;
    return CF_OK;
}

/* The reference's `invalid_attachment`: Error::internal("Could not find or
 * build blob: expected attachable"). */
static cf_err byb_invalid_attachment(void) {
    return CF_INTERNAL;
}

/* `canonical_body`: assigning a String to a rich text attribute stores
 * `ActionText::Content.new(body, canonicalize: true).to_html()`, computed on
 * a reader ahead of the write with the request Host, including the
 * reference's `unwrap_or_else(|_| body.to_string())` rescue (R02). */
static cf_err byb_canonical_body(cf_ctx *ctx, cf_span body, cf_str *out) {
    return cf_richtext_canonical_body(ctx, body, out);
}

/* ------------------------------------------------------- writer ------------------------------------------------------- */

static cf_err byb_revalidate(cf_tx *tx, int64_t room_id, int64_t actor_id,
                            bool need_message, int64_t message_id,
                            bool need_administer, cf_message *fresh) {
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

typedef struct {
    int64_t room_id;
    int64_t creator_id;
    bool has_body;
    cf_str body; /* borrowed; only when has_body */
    cf_message message;
} byb_create_write;

static cf_err byb_create_write_cb(cf_tx *tx, void *arg) {
    byb_create_write *write = arg;
    cf_err rc = byb_revalidate(tx, write->room_id, write->creator_id, false,
                               0, false, NULL);
    if (rc != CF_OK) return rc;
    cf_new_message attributes;
    memset(&attributes, 0, sizeof attributes);
    attributes.room_id = write->room_id;
    attributes.creator_id = write->creator_id;
    if (write->has_body) {
        attributes.body.present = true;
        attributes.body.value = write->body;
    }
    return cf_message_create(tx, &attributes, &write->message);
}

/* `@room.messages.create_with_attachment!` in one writer transaction. */
static cf_err byb_create_message(cf_ctx *ctx, const cf_user *user,
                                 const cf_room *room, byb_params *params,
                                 cf_message *out) {
    if (params->attachment == BYB_ATTACHMENT_INVALID) {
        return byb_invalid_attachment();
    }
    if (params->attachment == BYB_ATTACHMENT_CREATE) {
        return CF_INTERNAL; /* S02: stage/process_attachment unavailable */
    }

    cf_str body = {0};
    bool has_body = false;
    if (params->has_body) {
        cf_err rc = byb_canonical_body(
            ctx, (cf_span){(const unsigned char *)params->body.ptr,
                           params->body.len},
            &body);
        if (rc != CF_OK) return rc;
        has_body = true;
    }
    /* Otherwise the attachment-only create stores no body text. */

    byb_create_write write;
    memset(&write, 0, sizeof write);
    write.room_id = room->id;
    write.creator_id = user->id;
    write.has_body = has_body;
    write.body = body;
    cf_err rc = cf_write(ctx->app, byb_create_write_cb, &write);
    if (rc == CF_OK) {
        *out = write.message;
    } else {
        cf_message_dispose(&write.message);
    }
    cf_str_dispose(&body);
    return rc;
}

typedef struct {
    int64_t room_id;
    int64_t actor_id;
    int64_t message_id;
    bool has_body;
    cf_str body; /* borrowed */
    bool attachment_given;
    cf_optional_i64 blob_id;
    cf_message message; /* in/out; its owned fields alias the caller's row */
} byb_update_write;

static cf_err byb_update_write_cb(cf_tx *tx, void *arg) {
    byb_update_write *write = arg;
    cf_message fresh = {0};
    cf_err rc = byb_revalidate(tx, write->room_id, write->actor_id, true,
                               write->message_id, true, &fresh);
    cf_message_dispose(&fresh);
    if (rc != CF_OK) return rc;
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

static cf_err byb_update_message(cf_ctx *ctx, const cf_message *message,
                                 int64_t actor_id, byb_params *params,
                                 cf_message *out) {
    if (params->attachment == BYB_ATTACHMENT_INVALID) {
        return byb_invalid_attachment();
    }
    if (params->attachment == BYB_ATTACHMENT_CREATE) {
        return CF_INTERNAL; /* S02: stage/process_attachment unavailable */
    }

    cf_str body = {0};
    bool has_body = false;
    if (params->has_body) {
        cf_err rc = byb_canonical_body(
            ctx, (cf_span){(const unsigned char *)params->body.ptr,
                           params->body.len},
            &body);
        if (rc != CF_OK) return rc;
        has_body = true;
    }

    byb_update_write write;
    memset(&write, 0, sizeof write);
    write.room_id = message->room_id;
    write.actor_id = actor_id;
    write.message_id = message->id;
    write.has_body = has_body;
    write.body = body;
    write.attachment_given = params->attachment == BYB_ATTACHMENT_DELETE;
    write.message = *message; /* borrows the caller's owned fields */
    cf_err rc = cf_write(ctx->app, byb_update_write_cb, &write);
    cf_str_dispose(&body);
    if (rc != CF_OK) return rc;
    /* `c.app().read(|conn| Message::find(conn, id))`. */
    return cf_message_find(ctx->reader, message->id, out);
}

typedef struct {
    int64_t room_id;
    int64_t actor_id;
    cf_message message;
} byb_destroy_write;

static cf_err byb_destroy_write_cb(cf_tx *tx, void *arg) {
    byb_destroy_write *write = arg;
    cf_message fresh = {0};
    cf_err rc = byb_revalidate(tx, write->room_id, write->actor_id, true,
                               write->message.id, true, &fresh);
    cf_message_dispose(&fresh);
    if (rc != CF_OK) return rc;
    return cf_message_destroy(tx, &write->message);
}

/* ------------------------------------------------------- broadcasts & webhooks ------------------------------------------------------- */

static cf_err byb_cable(cf_ctx *ctx, cf_cable **out) {
    *out = NULL;
    if (ctx->app == NULL) return CF_INVALID;
    cf_cable *cable = cf_app_cable(ctx->app);
    if (cable == NULL) return CF_INTERNAL;
    *out = cable;
    return CF_OK;
}

static cf_err byb_broadcast_outcome(cf_err rc, const char *what) {
    if (rc == CF_BUSY) {
        cf_cable_log("dropped broadcast", what);
        return CF_OK;
    }
    return rc;
}

static cf_err byb_broadcast_create(cf_ctx *ctx, const cf_room *room,
                                   const cf_message *message) {
    cf_cable *cable = NULL;
    cf_err rc = byb_cable(ctx, &cable);
    if (rc != CF_OK) return rc;
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, NULL);
    cf_broadcast_views views = {ctx, &view_ctx};
    cf_broadcast_partials partials;
    cf_broadcast_partials_views(&partials, &views);
    rc = cf_broadcast_message_create(ctx->reader, cable, room, message,
                                     &partials);
    return byb_broadcast_outcome(rc, "messages/by_bots#create");
}

static cf_err byb_broadcast_replace(cf_ctx *ctx, const cf_room *room,
                                    const cf_message *message) {
    cf_cable *cable = NULL;
    cf_err rc = byb_cable(ctx, &cable);
    if (rc != CF_OK) return rc;
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, NULL);
    cf_broadcast_views views = {ctx, &view_ctx};
    cf_broadcast_partials partials;
    cf_broadcast_partials_views(&partials, &views);
    rc = cf_broadcast_message_replace(cable, room, message, &partials);
    return byb_broadcast_outcome(rc, "messages/by_bots#update");
}

static cf_err byb_broadcast_remove(cf_ctx *ctx, const cf_room *room,
                                   const cf_message *message) {
    cf_cable *cable = NULL;
    cf_err rc = byb_cable(ctx, &cable);
    if (rc != CF_OK) return rc;
    rc = cf_broadcast_message_remove(cable, room, message);
    return byb_broadcast_outcome(rc, "messages/by_bots#destroy");
}

typedef struct {
    const cf_user *const *bots;
    size_t count;
    int64_t message_id;
} byb_webhook_write;

static cf_err byb_webhook_write_cb(cf_tx *tx, void *arg) {
    byb_webhook_write *write = arg;
    for (size_t i = 0; i < write->count; i++) {
        cf_err rc = cf_user_deliver_webhook_later(tx, write->bots[i],
                                                  write->message_id);
        if (rc != CF_OK) return rc;
    }
    return CF_OK;
}

/* `deliver_webhooks_to_bots`: every active bot in a direct room, else every
 * mentioned active bot, except the message's creator. */
static cf_err byb_deliver_webhooks(cf_ctx *ctx, const cf_room *room,
                                   const cf_message *message) {
    cf_user_vector candidates = {0};
    cf_err rc;
    if (cf_room_direct(room)) {
        rc = cf_room_active_bots(ctx->reader, room, &candidates);
    } else {
        rc = cf_message_mentionees(ctx->reader, message,
                                   cf_tx_rich_text(NULL), &candidates);
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
        byb_webhook_write write = {bots, count, message->id};
        rc = cf_write(ctx->app, byb_webhook_write_cb, &write);
    }
    free(bots);
    cf_user_vector_dispose(&candidates);
    return rc;
}

/* ------------------------------------------------------- JSON (shim B1) ------------------------------------------------------- */

/* UTC civil date from days since 1970-01-01 (Howard Hinnant's algorithm). */
/* B1/B2 (integrator): local JSON serializers replaced by the shared
 * src/views/messages_json.c renderers (cf_views_message_json,
 * cf_views_absolute_url); exact shapes verified by tests/views/test_messages_json.c. */

/* `Ctx::render(status, JSON, body)`: `application/json; charset=utf-8`,
 * with `Vary: Accept` when negotiated from the Accept header. */
static cf_err byb_send_json(cf_ctx *ctx, unsigned status, cf_builder *body) {
    cf_buf *buf = NULL;
    cf_err rc = cf_builder_freeze(body, &buf);
    if (rc != CF_OK) {
        cf_builder_dispose(body);
        return rc;
    }
    ctx->response->status = status;
    rc = cf_response_header(ctx->response, byb_span("Content-Type"),
                            byb_span(BYB_CONTENT_JSON));
    if (rc == CF_OK && cf_ctx_vary_accept(ctx)) {
        rc = cf_response_header(ctx->response, byb_span("Vary"),
                                byb_span("Accept"));
    }
    if (rc == CF_OK) rc = cf_response_body(ctx->response, buf);
    cf_buf_release(buf);
    return rc;
}

/* `render :show` (messages/by_bots/show.json.jbuilder). */
static cf_err byb_render_show(cf_ctx *ctx, const cf_message *message) {
    cf_builder body = {0};
    cf_err rc = cf_views_message_json(ctx, message, &body);
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        return rc;
    }
    return byb_send_json(ctx, 200, &body);
}

/* `head :created` with the message Location: the absolute message URL.
 * The Location passes through compute_location unchanged (it is already
 * absolute); the head carries the rendered format's bare content type
 * (kit Ctx::head), which here is the negotiated JSON. */
static cf_err byb_created_with_location(cf_ctx *ctx,
                                        const cf_message *message) {
    char path[64];
    int n = snprintf(path, sizeof path, "/rooms/%" PRId64 "/messages/%" PRId64,
                     message->room_id, message->id);
    if (n < 0 || (size_t)n >= sizeof path) return CF_INTERNAL;
    cf_builder location = {0};
    cf_err rc = cf_views_absolute_url(ctx,
                                 (cf_span){(const unsigned char *)path,
                                           (size_t)n},
                                 &location);
    if (rc != CF_OK) {
        cf_builder_dispose(&location);
        return rc;
    }
    const cf_format *format = cf_ctx_rendered_format(ctx);
    ctx->response->status = 201;
    rc = cf_response_header(ctx->response, byb_span("Location"),
                            (cf_span){location.ptr, location.len});
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, byb_span("Content-Type"),
                                byb_span(format->string));
    }
    cf_builder_dispose(&location);
    return rc;
}

/* `redirect_to <room message URL>` (Rails default 302, text/html). */
static cf_err byb_redirect_message(cf_ctx *ctx, const cf_room *room,
                                   const cf_message *message) {
    char path[64];
    int n = snprintf(path, sizeof path, "/rooms/%" PRId64 "/messages/%" PRId64,
                     room->id, message->id);
    if (n < 0 || (size_t)n >= sizeof path) return CF_INTERNAL;
    cf_builder location = {0};
    cf_err rc = cf_views_absolute_url(ctx,
                                 (cf_span){(const unsigned char *)path,
                                           (size_t)n},
                                 &location);
    if (rc == CF_OK) {
        ctx->response->status = 302;
        rc = cf_response_header(ctx->response, byb_span("Location"),
                                (cf_span){location.ptr, location.len});
        if (rc == CF_OK) {
            rc = cf_response_header(ctx->response, byb_span("Content-Type"),
                                    byb_span("text/html; charset=utf-8"));
        }
    }
    cf_builder_dispose(&location);
    return rc;
}

/* `X-Total-Count`, and a `Link` to the next page when there is one. */
static cf_err byb_pagination_headers(cf_ctx *ctx, const cf_room *room,
                                     const cf_message *rows, size_t count) {
    int64_t total = 0;
    cf_err rc = cf_message_count_in_room(ctx->reader, room->id, &total);
    if (rc != CF_OK) return rc;
    char total_text[24];
    int n = snprintf(total_text, sizeof total_text, "%" PRId64, total);
    if (n < 0 || (size_t)n >= sizeof total_text) return CF_INTERNAL;
    rc = cf_response_header(
        ctx->response, byb_span("X-Total-Count"),
        (cf_span){(const unsigned char *)total_text, (size_t)n});
    if (rc != CF_OK) return rc;

    if (count == 0) return CF_OK;
    /* `params.get("after").is_some_and(Param::is_present)`. */
    bool after = byb_param_present(cf_ctx_param(ctx, byb_span("after")));
    bool exists = false;
    const char *key = NULL;
    int64_t id = 0;
    if (after) {
        rc = cf_message_exists_after(ctx->reader, room->id,
                                     &rows[count - 1], &exists);
        key = "after";
        id = rows[count - 1].id;
    } else {
        rc = cf_message_exists_before(ctx->reader, room->id, &rows[0],
                                      &exists);
        key = "before";
        id = rows[0].id;
    }
    if (rc != CF_OK) return rc;
    if (!exists) return CF_OK;

    /* `room_bot_messages(room_id, bot_key)?{key}={id}`; the bot key is the
     * raw param, defaulting to empty. */
    cf_span bot_key = byb_span("");
    const cf_param *param = cf_ctx_param(ctx, byb_span("bot_key"));
    if (param != NULL && cf_param_type(param) == CF_PARAM_STRING) {
        if (cf_param_string(param, &bot_key) != CF_OK) return CF_INTERNAL;
    }
    cf_builder link = {0};
    rc = cf_builder_append(&link, byb_span("<"));
    if (rc == CF_OK) rc = cf_views_absolute_url(ctx, byb_span(""), &link);
    if (rc == CF_OK) {
        char path[256];
        int m = snprintf(path, sizeof path,
                         "/rooms/%" PRId64 "/%.*s/messages?%s=%" PRId64,
                         room->id, (int)bot_key.len,
                         bot_key.len != 0 ? (const char *)bot_key.ptr : "",
                         key, id);
        if (m < 0 || (size_t)m >= sizeof path) rc = CF_INTERNAL;
        else {
            rc = cf_builder_append(&link, (cf_span){
                                                 (const unsigned char *)path,
                                                 (size_t)m});
        }
    }
    if (rc == CF_OK) rc = cf_builder_append(&link, byb_span(">; rel=\"next\""));
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, byb_span("Link"),
                                (cf_span){link.ptr, link.len});
    }
    cf_builder_dispose(&link);
    return rc;
}

/* ------------------------------------------------------- actions ------------------------------------------------------- */

cf_err cf_action_messages_by_bots_index(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, false, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = byb_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    int64_t user_id = user.id;
    cf_user_dispose(&user);

    cf_room room = {0};
    rc = byb_set_room(ctx, user_id, &room);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_room_dispose(&room);
        return rc;
    }
    cf_message_vector messages = {0};
    rc = byb_find_paged(ctx, &room, &messages);
    if (rc != CF_OK) {
        cf_message_vector_dispose(&messages);
        cf_room_dispose(&room);
        return rc;
    }
    rc = byb_pagination_headers(ctx, &room, messages.items, messages.len);
    if (rc != CF_OK) {
        cf_message_vector_dispose(&messages);
        cf_room_dispose(&room);
        return rc;
    }
    const cf_format *offered[1] = {&cf_format_json};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) {
        cf_message_vector_dispose(&messages);
        cf_room_dispose(&room);
        return rc;
    }
    cf_builder body = {0};
    rc = cf_builder_append(&body, byb_span("["));
    for (size_t i = 0; rc == CF_OK && i < messages.len; i++) {
        if (i != 0) rc = cf_builder_append(&body, byb_span(","));
        if (rc == CF_OK) {
            rc = cf_views_message_json(ctx, &messages.items[i], &body);
        }
    }
    cf_message_vector_dispose(&messages);
    cf_room_dispose(&room);
    if (rc == CF_OK) rc = cf_builder_append(&body, byb_span("]"));
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        return rc;
    }
    return byb_send_json(ctx, 200, &body);
}

cf_err cf_action_messages_by_bots_create(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, false, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = byb_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    cf_room room = {0};
    rc = byb_set_room(ctx, user.id, &room);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_user_dispose(&user);
        cf_room_dispose(&room);
        return rc;
    }
    rc = byb_ensure_body_or_attachment(ctx);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_user_dispose(&user);
        cf_room_dispose(&room);
        return rc;
    }
    byb_params params;
    memset(&params, 0, sizeof params);
    rc = byb_message_params(ctx, &params);
    if (rc != CF_OK) {
        byb_params_dispose(&params);
        cf_user_dispose(&user);
        cf_room_dispose(&room);
        return rc;
    }
    cf_message message = {0};
    rc = byb_create_message(ctx, &user, &room, &params, &message);
    int64_t creator_id = user.id;
    byb_params_dispose(&params);
    cf_user_dispose(&user);
    if (rc != CF_OK) {
        cf_message_dispose(&message);
        cf_room_dispose(&room);
        return rc;
    }
    (void)creator_id;
    rc = byb_broadcast_create(ctx, &room, &message);
    if (rc == CF_OK) rc = byb_deliver_webhooks(ctx, &room, &message);
    if (rc != CF_OK) {
        cf_message_dispose(&message);
        cf_room_dispose(&room);
        return rc;
    }
    rc = byb_created_with_location(ctx, &message);
    cf_message_dispose(&message);
    cf_room_dispose(&room);
    return rc;
}

cf_err cf_action_messages_by_bots_update(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, false, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = byb_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    int64_t user_id = user.id;
    cf_user_dispose(&user);

    cf_room room = {0};
    rc = byb_set_room(ctx, user_id, &room);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_room_dispose(&room);
        return rc;
    }
    cf_message message = {0};
    rc = byb_set_message(ctx, &room, &message);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        cf_message_dispose(&message);
        return rc;
    }
    rc = byb_ensure_can_administer(ctx, &message);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_room_dispose(&room);
        cf_message_dispose(&message);
        return rc;
    }
    byb_params params;
    memset(&params, 0, sizeof params);
    rc = byb_message_params(ctx, &params);
    if (rc != CF_OK) {
        byb_params_dispose(&params);
        cf_room_dispose(&room);
        cf_message_dispose(&message);
        return rc;
    }
    cf_message updated = {0};
    rc = byb_update_message(ctx, &message, user_id, &params, &updated);
    byb_params_dispose(&params);
    cf_message_dispose(&message);
    if (rc != CF_OK) {
        cf_message_dispose(&updated);
        cf_room_dispose(&room);
        return rc;
    }
    rc = byb_broadcast_replace(ctx, &room, &updated);
    if (rc != CF_OK) {
        cf_message_dispose(&updated);
        cf_room_dispose(&room);
        return rc;
    }
    /* respond_to html: redirect; json: `render :show`. */
    const cf_format *offered[2] = {&cf_format_html, &cf_format_json};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 2, &chosen);
    if (rc != CF_OK) {
        cf_message_dispose(&updated);
        cf_room_dispose(&room);
        return rc;
    }
    if (chosen == &cf_format_json) {
        rc = byb_render_show(ctx, &updated);
        cf_message_dispose(&updated);
        cf_room_dispose(&room);
        return rc;
    }
    rc = byb_redirect_message(ctx, &room, &updated);
    cf_message_dispose(&updated);
    cf_room_dispose(&room);
    return rc;
}

cf_err cf_action_messages_by_bots_destroy(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, false, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = byb_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    int64_t user_id = user.id;
    cf_user_dispose(&user);

    cf_room room = {0};
    rc = byb_set_room(ctx, user_id, &room);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_room_dispose(&room);
        return rc;
    }
    cf_message message = {0};
    rc = byb_set_message(ctx, &room, &message);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        cf_message_dispose(&message);
        return rc;
    }
    rc = byb_ensure_can_administer(ctx, &message);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_room_dispose(&room);
        cf_message_dispose(&message);
        return rc;
    }
    byb_destroy_write write;
    memset(&write, 0, sizeof write);
    write.room_id = room.id;
    write.actor_id = user_id;
    write.message = message; /* borrows the caller's owned fields */
    rc = cf_write(ctx->app, byb_destroy_write_cb, &write);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        cf_message_dispose(&message);
        return rc;
    }
    rc = byb_broadcast_remove(ctx, &room, &message);
    cf_room_dispose(&room);
    cf_message_dispose(&message);
    if (rc != CF_OK) return rc;

    ctx->response->status = 204;
    return CF_OK;
}
