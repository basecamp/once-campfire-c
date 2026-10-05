/* src/actions/searches.c — A-searches: `searches#clear` (route ID 145),
 * `searches#index` (146) and `searches#create` (147)
 * (docs/devel/implementation/contracts/controller-packets.md "A-searches";
 * routes.json rows 145/146/147).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/searches.rs, the
 * pinned port of reference/app/controllers/searches_controller.rb:
 *
 *   before_action :set_messages, only: %i[index create clear]
 *
 *   index:   query_param; set_messages / user.searches.ordered /
 *            last_room_visited in one reader trip; present the message items;
 *            page::framed_page!(OK, views::Index).
 *   create:  query_param; set_messages; `Current.user.searches.record(query)`
 *            (a nil query violates the column's NOT NULL -> 500); redirect to
 *            search_path(query), or /searches with no query.
 *   clear:   query_param; set_messages; `user.searches.destroy_all`;
 *            redirect to /searches.
 *
 * `set_messages` runs `Current.user.reachable_messages.search(query).last
 * (100)` only for a query that survives `sanitize_query` + `is_present`; a
 * query FTS5 rejects fails create and clear too, so the search runs (and any
 * error propagates) before the mutation.
 *
 * Route registration: src/routes.c rows 145/146/147 bind
 * cf_action_searches_clear (145), cf_action_searches_index (146) and
 * cf_action_searches_create (147) to this file's actions.
 *
 * D02: both write callbacks revalidate the actor on the transaction's
 * connection before the first mutation (an active user row for a user-scoped
 * write), as A-rooms/A-messages do.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "models/message.h"
#include "models/search.h"
#include "models/user.h"
#include "presenters/searches.h"
#include "views.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small helpers -------------------------------------------------------- */

static cf_span searches_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static cf_span searches_str_span(const cf_str *value) {
    return (cf_span){(const unsigned char *)value->ptr, value->len};
}

/* `require_current_user`: the chain guarantees one; a missing row is the
 * reference's internal error. */
static cf_err searches_current_user(cf_ctx *ctx, cf_user *out) {
    bool found = false;
    cf_err rc = cf_auth_current_user(ctx, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_INTERNAL;
    return CF_OK;
}

/* `params[:q]` (`query_param`): absent or JSON null is None; any other
 * non-string value makes `gsub` raise (`Error::internal`).  The returned text
 * is owned. */
static cf_err searches_query_param(cf_ctx *ctx, bool *has, cf_str *out) {
    *has = false;
    out->ptr = NULL;
    out->len = 0;
    const cf_param *param = cf_ctx_param(ctx, searches_span("q"));
    if (param == NULL || cf_param_type(param) == CF_PARAM_NULL) return CF_OK;
    if (cf_param_type(param) != CF_PARAM_STRING) return CF_INTERNAL;
    cf_span text = {NULL, 0};
    cf_err rc = cf_param_string(param, &text);
    if (rc != CF_OK) return rc;
    if (text.len == 0) {
        *has = true;
        return CF_OK;
    }
    char *copy = malloc(text.len + 1);
    if (copy == NULL) return CF_NOMEM;
    memcpy(copy, text.ptr, text.len);
    copy[text.len] = '\0';
    out->ptr = copy;
    out->len = text.len;
    *has = true;
    return CF_OK;
}

/* `set_messages`: the search for a sanitized query that `is_present`; the
 * rows are discarded (create/clear only need the search to run). */
static cf_err searches_set_messages(cf_ctx *ctx, const cf_user *user,
                                    bool has_query, cf_str query) {
    if (!has_query ||
        !cf_searches_query_present(
            (cf_span){(const unsigned char *)query.ptr, query.len})) {
        return CF_OK;
    }
    cf_message_vector messages = {0};
    cf_err rc =
        cf_message_search_reachable(ctx->reader, user->id, query, &messages);
    cf_message_vector_dispose(&messages);
    return rc;
}

/* `kit is_turbo_frame_request`: the first `Turbo-Frame` header
 * (case-insensitive), whose bytes must pass http's `HeaderValue::to_str`
 * (HTAB or visible ASCII only), and whose value must be nonblank after
 * trimming spaces/tabs. */
static unsigned char searches_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

static bool searches_turbo_frame_request(const cf_request *request) {
    if (request == NULL) return false;
    static const char target[] = "turbo-frame";
    const size_t target_len = sizeof target - 1;
    for (size_t i = 0; i < request->header_count; i++) {
        cf_span name = request->headers[i].name;
        if (name.len != target_len) continue;
        bool match = true;
        for (size_t k = 0; k < target_len; k++) {
            if (searches_lower(name.ptr[k]) !=
                (unsigned char)target[k]) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        cf_span value = request->headers[i].value;
        bool blank = true;
        for (size_t k = 0; k < value.len; k++) {
            unsigned char b = value.ptr[k];
            if (!((b >= 32 && b < 127) || b == '\t')) return false;
            if (b != ' ' && b != '\t') blank = false;
        }
        return !blank;
    }
    return false;
}

/* `redirect_to <PUBLIC_ORIGIN><path>` (Rails default 302, the reference
 * redirect's content type; the C port's url_for is PUBLIC_ORIGIN + the path,
 * 03-application.md "URL contract"). */
static cf_err searches_redirect(cf_ctx *ctx, cf_span path) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    cf_err rc =
        cf_builder_append(&location, searches_span(config->public_origin));
    if (rc == CF_OK) rc = cf_builder_append(&location, path);
    if (rc == CF_OK) {
        ctx->response->status = 302;
        rc = cf_response_header(ctx->response, searches_span("Location"),
                                (cf_span){location.ptr, location.len});
        if (rc == CF_OK) {
            rc = cf_response_header(ctx->response,
                                    searches_span("Content-Type"),
                                    searches_span("text/html; charset=utf-8"));
        }
    }
    cf_builder_dispose(&location);
    return rc;
}

/* The layout page's preload Link header (`Layout#page`; a frame carries
 * none).  K01c: the hit path re-emits the same deterministic links, which are
 * not part of the cached representation. */
static cf_err searches_link_header(cf_ctx *ctx) {
    cf_builder links = {0};
    cf_err rc = cf_views_preload_links(&links);
    if (rc == CF_INVALID) return CF_OK; /* unconfigured assets */
    if (rc == CF_OK && links.len != 0) {
        rc = cf_response_header(ctx->response, searches_span("Link"),
                                (cf_span){links.ptr, links.len});
    }
    cf_builder_dispose(&links);
    return rc;
}

/* `Ctx::render`'s response frame: HTML, the layout page's preload Link header
 * for a page (Layout#page; a frame carries none), `Vary: Accept` when the
 * format came from the Accept header.  Consumes the builder. */
static cf_err searches_send(cf_ctx *ctx, unsigned status, cf_builder *body,
                            bool link_header) {
    cf_buf *buf = NULL;
    cf_err rc = cf_builder_freeze(body, &buf);
    if (rc != CF_OK) {
        cf_builder_dispose(body);
        return rc;
    }
    ctx->response->status = status;
    rc = cf_response_header(ctx->response, searches_span("Content-Type"),
                            searches_span("text/html; charset=utf-8"));
    /* K01c: on the body-cache path the round owns one combined Vary
     * (Accept-Encoding plus Accept when negotiated); the action's own header
     * would be a duplicate. Off the cache path this is the pinned behavior. */
    if (rc == CF_OK && cf_ctx_vary_accept(ctx) &&
        !cf_cache_representation_active(ctx)) {
        rc = cf_response_header(ctx->response, searches_span("Vary"),
                                searches_span("Accept"));
    }
    if (rc == CF_OK) rc = cf_response_body(ctx->response, buf);
    cf_buf_release(buf);
    if (rc != CF_OK) return rc;
    if (link_header) return searches_link_header(ctx);
    return CF_OK;
}

/* `page::framed_page!(c, StatusCode::OK, views::Index)`: find_template(HTML)
 * (406 otherwise), Layout::load, then the application layout or turbo-rails'
 * frame layout for a Turbo-Frame request. */
static cf_err searches_render_index(
    cf_ctx *ctx, const cf_view_searches_index_model *model) {
    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    cf_err rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) return rc;

    cf_view_layout_model layout = {0};
    rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) return rc;
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);

    bool frame = searches_turbo_frame_request(ctx->request);
    cf_builder body = {0};
    rc = frame ? cf_view_searches_index_frame(&view_ctx, model, &body)
               : cf_view_searches_index(&view_ctx, model, &body);
    if (rc == CF_OK) {
        rc = searches_send(ctx, 200, &body, !frame);
    } else {
        cf_builder_dispose(&body);
    }
    cf_view_layout_model_dispose(&layout);
    return rc;
}

/* ---- writer callbacks ----------------------------------------------------- */

/* D02 revalidation: the actor row must still exist and be active before a
 * user-scoped search row is written. */
static cf_err searches_revalidate_user(cf_tx *tx, int64_t user_id) {
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    bool found = false;
    cf_user actor = {0};
    cf_err rc = cf_user_find_by_id(db, user_id, &found, &actor);
    bool active = rc == CF_OK && found && cf_user_is_active(&actor);
    cf_user_dispose(&actor);
    if (rc != CF_OK) return rc;
    if (!active) return CF_FORBIDDEN;
    return CF_OK;
}

typedef struct {
    int64_t user_id;
    cf_str query; /* borrowed from the action until cf_write returns */
} searches_record_write_arg;

static cf_err searches_record_write(cf_tx *tx, void *arg) {
    const searches_record_write_arg *write = arg;
    cf_err rc = searches_revalidate_user(tx, write->user_id);
    if (rc != CF_OK) return rc;
    cf_search recorded = {0};
    rc = cf_search_record(tx, write->user_id, write->query, &recorded);
    cf_search_dispose(&recorded);
    return rc;
}

typedef struct {
    int64_t user_id;
} searches_clear_write_arg;

static cf_err searches_clear_write(cf_tx *tx, void *arg) {
    const searches_clear_write_arg *write = arg;
    cf_err rc = searches_revalidate_user(tx, write->user_id);
    if (rc != CF_OK) return rc;
    return cf_search_destroy_all_for_user(tx, write->user_id);
}

/* ---- actions -------------------------------------------------------------- */

/* `index`: `before_actions`; query_param; the presenter's one-trip read
 * (search results, recent searches, last room); present; framed_page. */
cf_err cf_action_searches_index(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    /* K01c: capture the version before authentication (06 step 1); the round
     * serves a hit after authorization, before the one-trip presenter read. */
    cf_cache_round round;
    cf_cache_round_init(ctx, &round);
    /* The Turbo-Frame header selects the frame layout and is a key field. */
    bool frame = searches_turbo_frame_request(ctx->request);

    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, true, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_cache_round_dispose(&round);
        return rc;
    }

    bool has_q = false;
    cf_str q = {0};
    rc = searches_query_param(ctx, &has_q, &q);
    if (rc != CF_OK) {
        cf_str_dispose(&q);
        cf_cache_round_dispose(&round);
        return rc;
    }

    cf_user user = {0};
    rc = searches_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        cf_str_dispose(&q);
        cf_cache_round_dispose(&round);
        return rc;
    }

    /* Cache lookup after authorization and before the gather; the raw query
     * is carried by the key's raw-target field. */
    cf_cached_body cached = {0};
    bool hit = round.cache != NULL &&
               cf_cache_round_lookup(
                   ctx, &round,
                   searches_span("text/html; charset=utf-8"), &cached) == CF_OK;
    if (hit) {
        cf_user_dispose(&user);
        cf_str_dispose(&q);
        rc = cf_cache_serve_hit(ctx, &round, &cached);
        cf_cached_body_dispose(&cached);
        if (rc == CF_OK && !frame) rc = searches_link_header(ctx);
        cf_cache_round_dispose(&round);
        return rc;
    }
    cf_cached_body_dispose(&cached);

    {
        cf_view_searches_index_model model = {0};
        rc = cf_presenter_searches_index(
            ctx, &user, has_q,
            has_q ? searches_str_span(&q) : (cf_span){NULL, 0}, &model);
        if (rc == CF_OK) rc = searches_render_index(ctx, &model);
        cf_view_searches_index_model_dispose(&model);
    }
    cf_cache_round_finish(ctx, &round);
    cf_cache_round_dispose(&round);
    cf_user_dispose(&user);
    cf_str_dispose(&q);
    return rc;
}

/* `create`: `before_actions`; query_param; set_messages; require_current_user;
 * record the sanitized query (nil is the reference's NOT NULL 500); redirect
 * to search_path(query) or /searches. */
cf_err cf_action_searches_create(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, true, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    bool has_q = false;
    cf_str q = {0};
    rc = searches_query_param(ctx, &has_q, &q);
    if (rc != CF_OK) {
        cf_str_dispose(&q);
        return rc;
    }

    cf_user user = {0};
    rc = searches_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_str_dispose(&q);
        cf_user_dispose(&user);
        return rc;
    }

    /* `query(q)`: the sanitized query set_messages searches for, record
     * records and the redirect points at. */
    bool has_query = false;
    cf_str query = {0};
    if (has_q) {
        rc = cf_searches_sanitize_query(searches_str_span(&q), &query);
        has_query = true;
    }
    if (rc == CF_OK) {
        rc = searches_set_messages(ctx, &user, has_query, query);
    }
    /* `record(query)`: a nil query violates searches.query's NOT NULL. */
    if (rc == CF_OK && !has_query) rc = CF_INTERNAL;

    if (rc == CF_OK) {
        searches_record_write_arg write = {.user_id = user.id,
                                           .query = query};
        rc = cf_write(ctx->app, searches_record_write, &write);
    }

    if (rc == CF_OK) {
        cf_builder path = {0};
        if (has_query) {
            rc = cf_view_searches_search_path(searches_str_span(&query), &path);
        } else {
            rc = cf_builder_append(&path, searches_span("/searches"));
        }
        if (rc == CF_OK) {
            rc = searches_redirect(ctx, (cf_span){path.ptr, path.len});
        }
        cf_builder_dispose(&path);
    }

    cf_str_dispose(&query);
    cf_str_dispose(&q);
    cf_user_dispose(&user);
    return rc;
}

/* `clear`: `before_actions`; query_param; set_messages; require_current_user;
 * `user.searches.destroy_all`; redirect to /searches. */
cf_err cf_action_searches_clear(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, true, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    bool has_q = false;
    cf_str q = {0};
    rc = searches_query_param(ctx, &has_q, &q);
    if (rc != CF_OK) {
        cf_str_dispose(&q);
        return rc;
    }

    cf_user user = {0};
    rc = searches_current_user(ctx, &user);
    if (rc == CF_OK) {
        cf_str query = {0};
        if (has_q) {
            rc = cf_searches_sanitize_query(searches_str_span(&q), &query);
        }
        if (rc == CF_OK) {
            rc = searches_set_messages(ctx, &user, has_q, query);
        }
        cf_str_dispose(&query);
    }
    if (rc == CF_OK) {
        searches_clear_write_arg write = {.user_id = user.id};
        rc = cf_write(ctx->app, searches_clear_write, &write);
    }
    if (rc == CF_OK) {
        rc = searches_redirect(ctx, searches_span("/searches"));
    }
    cf_user_dispose(&user);
    cf_str_dispose(&q);
    return rc;
}
