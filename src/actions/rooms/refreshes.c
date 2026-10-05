/* src/actions/rooms/refreshes.c — A-rooms-refreshes: `rooms/refreshes#show`
 * (route ID 91)
 * (docs/devel/implementation/contracts/controller-packets.md
 * "A-rooms-refreshes").
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/rooms/refreshes.rs,
 * the pinned port of
 * reference/app/controllers/rooms/refreshes_controller.rb:
 *
 *   before_actions(Before::default()) — authentication REQUIRED, deny_bots,
 *   forgery protection;
 *   concerns::set_room — RoomScoped: `Current.user.memberships.find_by!(
 *   room_id: params[:room_id])` (integer_cast; missing/unparsable/absent is
 *   RecordNotFound, i.e. 404), then the membership's room (a membership whose
 *   room row is gone is the reference's 500);
 *   set_last_updated_at — `Time.at(0, params[:since].to_i, :millisecond)`:
 *   missing/null => 0, a string through ruby_compat::to_i, a hash or an array
 *   (anything without `to_i`) => internal error; the millisecond count is
 *   saturating-mul'd to microseconds and clamped to the representable range;
 *   respond_to([TURBO_STREAM]);
 *   one read: Message::page_created_since(room, since) plus
 *   Message::page_updated_since(room, since, excluding the new ids), both
 *   presented, rendered through RefreshShow in page::bare (no layout, the
 *   turbo-stream content type).
 *
 * The response conventions are the landed actions' (A-rooms/A-messages):
 * helpers that answer the request set ctx->response and return CF_OK
 * (callers stop with cf_auth_halted, src/auth.h); the bare render carries no
 * `Link` preload header (page::bare, like messages#index).
 *
 * Route registration: src/routes.c row 91 binds
 * cf_action_rooms_refreshes_show to this file's action.
 *
 * Integrator request (views packet; this file is self-contained meanwhile):
 *  R1. cf_view_room_refresh_stream — the RefreshShow template as a shared
 *      renderer.  Proposed declaration (src/views.h):
 *        cf_err cf_view_room_refresh_stream(
 *            const cf_view_ctx *ctx, const cf_room *room,
 *            const cf_view_message_item *new_items, size_t new_count,
 *            const cf_view_message_item *updated_items, size_t updated_count,
 *            cf_builder *out);
 *      Until it lands, the static function of the same name below renders the
 *      exact template bytes (empty refresh is "\n"; the append block only
 *      when new messages exist; two-space-indented replace lines —
 *      verified byte for byte against the Askama 0.14 render of
 *      tmp/rust-ref/crates/views/templates/rooms/refreshes/
 *      show.turbo_stream.html).  Item bodies use the landed
 *      cf_view_message_item_partial (the View arm of cached_message_item;
 *      this phase has no fragment cache).
 */
#include "cf.h"

#include "auth.h"
#include "context.h"
#include "models/membership.h"
#include "models/message.h"
#include "models/room.h"
#include "models/user.h"
#include "views.h"
#include "views/internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cf_span refresh_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `require_current_user(c)` (concerns.rs): the authenticated user row.  The
 * chain guarantees a signed-in identity; a missing row is the reference's
 * internal error. */
static cf_err refresh_current_user(cf_ctx *ctx, cf_user *out) {
    bool found = false;
    cf_err rc = cf_auth_current_user(ctx, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_INTERNAL;
    return CF_OK;
}

/* `concerns::set_room` (RoomScoped, concerns.rs): the current user's
 * membership by the string `room_id` param through integer_cast (missing,
 * non-string or uncastable is RecordNotFound), then the membership's room (a
 * membership whose room row is gone is the reference's internal error). */
static cf_err refresh_set_room(cf_ctx *ctx, int64_t user_id,
                               cf_membership *membership_out, cf_room *room_out) {
    const cf_param *param = cf_ctx_param(ctx, refresh_span("room_id"));
    cf_span text = {NULL, 0};
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING ||
        cf_param_string(param, &text) != CF_OK) {
        return CF_NOT_FOUND;
    }
    int64_t room_id = 0;
    if (!cf_views_integer_cast(text, &room_id)) return CF_NOT_FOUND;

    bool found = false;
    cf_err rc = cf_membership_find_by_room_and_user(ctx->reader, room_id,
                                                    user_id, &found,
                                                    membership_out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_NOT_FOUND;

    bool room_found = false;
    rc = cf_room_find_by_id(ctx->reader, membership_out->room_id, &room_found,
                            room_out);
    if (rc != CF_OK) return rc;
    if (!room_found) return CF_INTERNAL;
    return CF_OK;
}

/* ---- ruby_compat::to_i (tmp/rust-ref/crates/ruby/src/integer.rs) ---------- */

/* Ruby's ISSPACE: what `String#to_i` skips (ruby/src/string.rs SPACE). */
static bool refresh_is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
           c == '\r';
}

#define REFRESH_I128_MAX \
    ((__int128)(((unsigned __int128)1 << 127) - 1))

/* Saturating `number * 10 + digit` over the nonnegative i128 accumulator. */
static __int128 refresh_sat_mul10_add(__int128 number, unsigned digit) {
    if (number > (REFRESH_I128_MAX - (__int128)digit) / 10) {
        return REFRESH_I128_MAX;
    }
    return number * 10 + (__int128)digit;
}

/* `String#to_i`: optional leading whitespace and sign, an optional `0d`,
 * then digits (one `_` allowed between two), stopping at the first other
 * byte; saturating at the i64 bounds where Ruby goes on to a Bignum. */
static int64_t refresh_to_i(cf_span text) {
    size_t at = 0;
    while (at < text.len && refresh_is_space(text.ptr[at])) at++;
    bool negative = false;
    if (at < text.len && (text.ptr[at] == '-' || text.ptr[at] == '+')) {
        negative = text.ptr[at] == '-';
        at++;
    }
    if (at + 2 <= text.len && text.ptr[at] == '0' &&
        (text.ptr[at + 1] == 'd' || text.ptr[at + 1] == 'D')) {
        at += 2;
    }
    __int128 number = 0;
    bool previous_digit = false;
    while (at < text.len) {
        unsigned char c = text.ptr[at];
        if (c >= '0' && c <= '9') {
            number = refresh_sat_mul10_add(number, (unsigned)(c - '0'));
            previous_digit = true;
            at++;
        } else if (c == '_' && previous_digit) {
            previous_digit = false;
            at++;
        } else {
            break;
        }
    }
    if (negative) number = -number;
    if (number > (__int128)INT64_MAX) return INT64_MAX;
    if (number < (__int128)INT64_MIN) return INT64_MIN;
    return (int64_t)number;
}

/* jiff::Timestamp's representable range in microseconds
 * (MIN = -9999-01-01T00:00:00Z, MAX = 9999-12-31T23:59:59.999999999Z).
 * Every stored message time lies inside it, so clamping a crafted `since`
 * to these ends is behaviorally identical to the reference's MIN/MAX.
 *
 * One port boundary applies at the low end: the model binds query times
 * through cf_db_time_to_text, which rejects years below 0 (CF_INVALID),
 * while jiff MIN is the year -9999.  A `since` below the bindable floor is
 * therefore clamped to the floor (0000-01-01T00:00:00Z) instead: every row
 * the model can store is at year 0 or later, so the query result is
 * identical to clamping at jiff MIN (reported to the integrator as a
 * model-range request). */
#define REFRESH_JIFF_MIN_US INT64_C(-377705203200000000)
#define REFRESH_JIFF_MAX_US INT64_C(253402300799999999)
#define REFRESH_MODEL_MIN_US INT64_C(-62167219200000000)

/* `Time.at(0, params[:since].to_i, :millisecond)`: missing/null => 0, a
 * string through to_i, anything without `to_i` (hash/array — and, through
 * as_str, number/bool/upload) => internal error.  The millisecond count is
 * saturating-mul'd to microseconds, then clamped to the representable
 * range (a crafted `since` takes the nearest end of it). */
static cf_err refresh_last_updated_at(cf_ctx *ctx, int64_t *out) {
    const cf_param *param = cf_ctx_param(ctx, refresh_span("since"));
    int64_t since_ms = 0;
    if (param != NULL) {
        if (cf_param_type(param) == CF_PARAM_STRING) {
            cf_span text = {NULL, 0};
            cf_err rc = cf_param_string(param, &text);
            if (rc != CF_OK) return rc;
            since_ms = refresh_to_i(text);
        } else if (cf_param_type(param) == CF_PARAM_NULL) {
            since_ms = 0;
        } else {
            return CF_INTERNAL;
        }
    }
    int64_t since_us;
    if (since_ms > INT64_MAX / 1000 || since_ms < INT64_MIN / 1000) {
        since_us = since_ms < 0 ? INT64_MIN : INT64_MAX;
    } else {
        since_us = since_ms * 1000;
    }
    if (since_us < REFRESH_MODEL_MIN_US) since_us = REFRESH_MODEL_MIN_US;
    if (since_us > REFRESH_JIFF_MAX_US) since_us = REFRESH_JIFF_MAX_US;
    *out = since_us;
    return CF_OK;
}

/* ---- the RefreshShow render (INTEGRATOR SHIM R1; see the header) ---------- */

/* `Room.model_name.param_key` for the room's STI class
 * (views messages.rs RoomKind::param_key: "rooms_open" etc.), through the
 * landed class-name helper the way src/cable/broadcasts.c derives it. */
static cf_err refresh_room_param_key(cf_room_type kind, cf_builder *out) {
    const char *class_name = cf_room_type_class_name(kind);
    if (class_name == NULL) return CF_INVALID;
    cf_err rc = CF_OK;
    for (const char *p = class_name; *p != '\0' && rc == CF_OK; p++) {
        if (p[0] == ':' && p[1] == ':') {
            rc = cf_builder_append(
                out, (cf_span){(const unsigned char *)"_", 1});
            p++;
            continue;
        }
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        rc = cf_builder_append(out, (cf_span){&c, 1});
    }
    return rc;
}

/* `dom_id(room, "messages")`: "messages_<param_key>_<id>". */
static cf_err refresh_messages_target(const cf_room *room, cf_builder *out) {
    static const char prefix[] = "messages_";
    cf_err rc = cf_builder_append(
        out, (cf_span){(const unsigned char *)prefix, sizeof prefix - 1});
    if (rc == CF_OK) rc = refresh_room_param_key(room->room_type, out);
    if (rc == CF_OK) {
        rc = cf_builder_append(out, (cf_span){(const unsigned char *)"_", 1});
    }
    if (rc == CF_OK) {
        char id[32];
        int n = snprintf(id, sizeof id, "%lld", (long long)room->id);
        if (n < 0 || (size_t)n >= sizeof id) return CF_INTERNAL;
        rc = cf_builder_append(
            out, (cf_span){(const unsigned char *)id, (size_t)n});
    }
    return rc;
}

/* `item.dom_id("")`: "message_<client_message_id>" (views messages.rs
 * MessageItem::dom_id; this phase has no fragment cache, so the View arm's
 * client_message_id; the Fragment arm's equivalent when one is ever
 * produced). */
static cf_err refresh_item_dom_id(const cf_view_message_item *item,
                                  cf_builder *out) {
    static const char prefix[] = "message_";
    cf_err rc = cf_builder_append(
        out, (cf_span){(const unsigned char *)prefix, sizeof prefix - 1});
    if (rc != CF_OK) return rc;
    if (item->is_fragment) {
        return cf_builder_append(
            out, (cf_span){(const unsigned char *)item->fragment_client_message_id.ptr,
                           item->fragment_client_message_id.len});
    }
    return cf_builder_append(
        out, (cf_span){(const unsigned char *)item->message.client_message_id.ptr,
                       item->message.client_message_id.len});
}

/* The template escapes `{{ }}` targets for HTML (Askama default); the room
 * target is `[a-z0-9_]` by construction, the message target carries the
 * client message id. */
static cf_err refresh_target_attr(cf_builder *out, cf_builder *raw) {
    return cf_html_attr(out, (cf_span){raw->ptr, raw->len});
}

/* The RefreshShow turbo stream (see the header): the append block only when
 * new messages exist, the blank line between the blocks (an empty refresh
 * is "\n"), then one two-space-indented replace line per updated message. */
static cf_err cf_view_room_refresh_stream(
    const cf_view_ctx *view_ctx, const cf_room *room,
    const cf_view_message_item *new_items, size_t new_count,
    const cf_view_message_item *updated_items, size_t updated_count,
    cf_builder *out) {
    cf_err rc = CF_OK;
    if (new_count != 0) {
        cf_builder target = {0};
        rc = refresh_messages_target(room, &target);
        if (rc == CF_OK) {
            rc = cf_builder_append(out, refresh_span("<turbo-stream action=\"append\" target=\""));
        }
        if (rc == CF_OK) rc = refresh_target_attr(out, &target);
        cf_builder_dispose(&target);
        if (rc == CF_OK) {
            rc = cf_builder_append(out, refresh_span("\"><template>\n  "));
        }
        for (size_t i = 0; rc == CF_OK && i < new_count; i++) {
            rc = cf_view_message_item_partial(view_ctx, &new_items[i], out);
        }
        if (rc == CF_OK) {
            rc = cf_builder_append(out, refresh_span("\n</template></turbo-stream>\n"));
        }
    } else {
        rc = cf_builder_append(out, refresh_span("\n"));
    }
    for (size_t i = 0; rc == CF_OK && i < updated_count; i++) {
        cf_builder target = {0};
        rc = refresh_item_dom_id(&updated_items[i], &target);
        if (rc == CF_OK) {
            rc = cf_builder_append(out, refresh_span("  <turbo-stream action=\"replace\" target=\""));
        }
        if (rc == CF_OK) rc = refresh_target_attr(out, &target);
        cf_builder_dispose(&target);
        if (rc == CF_OK) {
            rc = cf_builder_append(out, refresh_span("\"><template>"));
        }
        if (rc == CF_OK) {
            rc = cf_view_message_item_partial(view_ctx, &updated_items[i], out);
        }
        if (rc == CF_OK) {
            rc = cf_builder_append(out, refresh_span("</template></turbo-stream>\n"));
        }
    }
    return rc;
}

/* `page::bare(OK, TURBO_STREAM, RefreshShow)`: the stream with the
 * turbo-stream content type and no layout `Link` header. */
static cf_err refresh_send(cf_ctx *ctx, cf_builder *body) {
    cf_buf *buf = NULL;
    cf_err rc = cf_builder_freeze(body, &buf);
    if (rc != CF_OK) {
        cf_builder_dispose(body);
        return rc;
    }
    ctx->response->status = 200;
    rc = cf_response_header(ctx->response, refresh_span("Content-Type"),
                            refresh_span("text/vnd.turbo-stream.html; charset=utf-8"));
    if (rc == CF_OK && cf_ctx_vary_accept(ctx)) {
        rc = cf_response_header(ctx->response, refresh_span("Vary"),
                                refresh_span("Accept"));
    }
    if (rc == CF_OK) rc = cf_response_body(ctx->response, buf);
    cf_buf_release(buf);
    return rc;
}

/* `show`: before_actions; set_room; set_last_updated_at;
 * respond_to([TURBO_STREAM]); one read of the new and updated pages with
 * their presenters; the bare RefreshShow render. */
cf_err cf_action_rooms_refreshes_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = refresh_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    cf_membership membership = {0};
    cf_room room = {0};
    rc = refresh_set_room(ctx, user.id, &membership, &room);
    cf_user_dispose(&user);
    if (rc != CF_OK) {
        cf_membership_dispose(&membership);
        cf_room_dispose(&room);
        return rc;
    }
    cf_membership_dispose(&membership);

    int64_t last_updated_at = 0;
    rc = refresh_last_updated_at(ctx, &last_updated_at);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }

    const cf_format *offered[1] = {&cf_format_turbo_stream};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }

    cf_message_vector new_messages = {0};
    rc = cf_message_page_created_since(ctx->reader, room.id, last_updated_at,
                                       &new_messages);
    if (rc != CF_OK) {
        cf_message_vector_dispose(&new_messages);
        cf_room_dispose(&room);
        return rc;
    }
    int64_t *excluding = NULL;
    if (new_messages.len != 0) {
        excluding = malloc(new_messages.len * sizeof *excluding);
        if (excluding == NULL) {
            cf_message_vector_dispose(&new_messages);
            cf_room_dispose(&room);
            return CF_NOMEM;
        }
        for (size_t i = 0; i < new_messages.len; i++) {
            excluding[i] = new_messages.items[i].id;
        }
    }
    cf_message_vector updated_messages = {0};
    rc = cf_message_page_updated_since(ctx->reader, room.id, last_updated_at,
                                       excluding, new_messages.len,
                                       &updated_messages);
    free(excluding);
    if (rc != CF_OK) {
        cf_message_vector_dispose(&updated_messages);
        cf_message_vector_dispose(&new_messages);
        cf_room_dispose(&room);
        return rc;
    }

    cf_view_message_item_vector new_items = {0};
    rc = cf_presenter_messages(ctx, new_messages.items, new_messages.len,
                               &new_items);
    cf_message_vector_dispose(&new_messages);
    if (rc != CF_OK) {
        cf_view_message_item_vector_dispose(&new_items);
        cf_message_vector_dispose(&updated_messages);
        cf_room_dispose(&room);
        return rc;
    }
    cf_view_message_item_vector updated_items = {0};
    rc = cf_presenter_messages(ctx, updated_messages.items,
                               updated_messages.len, &updated_items);
    cf_message_vector_dispose(&updated_messages);
    if (rc != CF_OK) {
        cf_view_message_item_vector_dispose(&updated_items);
        cf_view_message_item_vector_dispose(&new_items);
        cf_room_dispose(&room);
        return rc;
    }

    cf_view_layout_model layout = {0};
    rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) {
        cf_view_message_item_vector_dispose(&updated_items);
        cf_view_message_item_vector_dispose(&new_items);
        cf_room_dispose(&room);
        return rc;
    }
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);

    cf_builder body = {0};
    rc = cf_view_room_refresh_stream(&view_ctx, &room, new_items.items,
                                     new_items.len, updated_items.items,
                                     updated_items.len, &body);
    cf_view_layout_model_dispose(&layout);
    cf_view_message_item_vector_dispose(&updated_items);
    cf_view_message_item_vector_dispose(&new_items);
    cf_room_dispose(&room);
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        return rc;
    }
    return refresh_send(ctx, &body);
}
