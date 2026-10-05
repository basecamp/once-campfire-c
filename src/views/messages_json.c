/* src/views/messages_json.c — bot API JSON serializers (packet V-F).
 *
 * Reference: tmp/rust-ref/crates/views/src/messages/json.rs (the Jbuilder
 * field order: `messages/_message.json`, `messages/by_bots/{index,show}.json`,
 * `messages/boosts/_boost.json`, `messages/boosts/by_bots/show.json`) and
 * tmp/rust-ref/crates/campfire/src/controllers/presenters.rs
 * (render_message_json/render_boost_json/user_json/body_html) behind
 * tmp/rust-ref/crates/campfire/src/controllers/messages/by_bots.rs and
 * messages/boosts/by_bots.rs.
 *
 * Field order is byte-exact per the pinned Jbuilder templates:
 *   user:    id, name, role, avatar_url
 *   message: id, created_at, body{plain_text, html}, creator, room{id}, url
 *   boost:   id, content, created_at, booster, message{id, url}
 * Strings go through R01's cf_json_string (the JSON writer, valid UTF-8,
 * `<>&` as lowercase-hex \u escapes per ActiveSupport's
 * escape_html_entities).  Name fields are NOT passed through an HTML escaper
 * first: the pinned templates call `json.(user, :id, :name, :role)` with the
 * raw column, and Jbuilder never applies h() to JSON strings; the \u003c /
 * \u003e / \u0026 spellings in the output are the JSON writer's own work.
 * created_at uses messages/support.rs json_time (milliseconds, truncated).
 *
 * Rendering does no SQL of its own beyond the row lookups the presenter
 * performs (creator/booster, plain text, stored body); no mutation,
 * filesystem or network work.  On any error the caller's builder is left at
 * its entry length.
 *
 * URL-helper decision (B2, confirms the shim behavior): absolute URLs are
 * PUBLIC_ORIGIN + path (03-application.md A00 "URL helpers ... use
 * PUBLIC_ORIGIN for absolute URLs"; D-C07 proxy headers are untrusted and
 * the listener owns the scheme).  This intentionally differs from the Rust
 * reference's `c.url_for` (request Host base_url): the C port must not let a
 * forged Host feed bot-payload URLs, the same reason the reference keys its
 * Jbuilder fragment cache by base_url.  cf_views_absolute_url is the one
 * shared helper for JSON payloads; redirects keep their own identical
 * PUBLIC_ORIGIN rule in the action files.
 *
 * Proposed shared declarations for the integrator (views.h / context.h):
 *   cf_err cf_views_message_json(cf_ctx *, const cf_message *,
 *                                cf_builder *out);
 *   cf_err cf_views_boost_json(cf_ctx *, const cf_boost *,
 *                              const cf_message *, cf_builder *out);
 *   cf_err cf_views_absolute_url(cf_ctx *, cf_span path, cf_builder *out);
 */
#include "cf.h"

#include "app.h"
#include "config.h"
#include "context.h"
#include "models/boost.h"
#include "models/message.h"
#include "models/user.h"
#include "richtext.h"
#include "views.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cf_span mj_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* UTC civil date from days since 1970-01-01 (Howard Hinnant's algorithm). */
static void mj_civil_from_days(int64_t days, int64_t *year, unsigned *month,
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

/* `messages/support.rs json_time`: Active Support default precision
 * (`2026-09-26T12:26:46.848Z`): whole seconds print `.000`, sub-second
 * digits beyond milliseconds are truncated, never rounded. */
static cf_err mj_json_time(int64_t time_us, cf_builder *out) {
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
    mj_civil_from_days(days, &year, &month, &day);
    char text[32];
    int n = snprintf(text, sizeof text,
                     "%04" PRId64 "-%02u-%02uT%02" PRId64 ":%02" PRId64
                     ":%02" PRId64 ".%03" PRId64 "Z",
                     year, month, day, sod / 3600, (sod % 3600) / 60, sod % 60,
                     rem / 1000);
    if (n < 0 || (size_t)n >= sizeof text) return CF_INTERNAL;
    return cf_builder_append(out, (cf_span){(const unsigned char *)text,
                                           (size_t)n});
}

static cf_err mj_json_i64(cf_builder *out, int64_t value) {
    char text[24];
    int n = snprintf(text, sizeof text, "%" PRId64, value);
    if (n < 0 || (size_t)n >= sizeof text) return CF_INTERNAL;
    return cf_builder_append(out, (cf_span){(const unsigned char *)text,
                                           (size_t)n});
}

/* Absolute URL for a JSON payload (B2): PUBLIC_ORIGIN + path. */
cf_err cf_views_absolute_url(cf_ctx *ctx, cf_span path, cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    if (path.len != 0 && path.ptr == NULL) return CF_INVALID;
    if (ctx->app == NULL) return CF_INVALID;
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    size_t mark = out->len;
    cf_err rc = cf_builder_append(out, mj_span(config->public_origin));
    if (rc == CF_OK) rc = cf_builder_append(out, path);
    if (rc != CF_OK) out->len = mark;
    return rc;
}

/* `users/_user.json.jbuilder`: `json.(user, :id, :name, :role)` and the
 * absolute avatar_url.  The name is the raw column through cf_json_string;
 * no h() pass (see the file header). */
static cf_err mj_user_json(cf_ctx *ctx, const cf_user *user, cf_builder *out) {
    cf_view_user view = {0};
    cf_err rc = cf_presenter_user_view(ctx, user, &view);
    if (rc != CF_OK) {
        cf_view_user_dispose(&view);
        return rc;
    }
    rc = cf_builder_append(out, mj_span("{\"id\":"));
    if (rc == CF_OK) rc = mj_json_i64(out, user->id);
    if (rc == CF_OK) rc = cf_builder_append(out, mj_span(",\"name\":"));
    if (rc == CF_OK) {
        rc = cf_json_string(out, (cf_span){(const unsigned char *)view.name.ptr,
                                           view.name.len});
    }
    if (rc == CF_OK) rc = cf_builder_append(out, mj_span(",\"role\":\""));
    if (rc == CF_OK) {
        const char *role = cf_role_name(user->role);
        if (role == NULL) rc = CF_INTERNAL;
        else rc = cf_builder_append(out, mj_span(role));
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(out, mj_span("\",\"avatar_url\":"));
    }
    if (rc == CF_OK) {
        cf_builder url = {0};
        rc = cf_views_absolute_url(
            ctx, (cf_span){(const unsigned char *)view.avatar_url.ptr,
                           view.avatar_url.len},
            &url);
        if (rc == CF_OK) {
            /* cf_json_string supplies the quotes (unlike the shim's raw
             * append, which is byte-identical for ordinary origins but
             * trusts the token alphabet); output validity never depends
             * on signer output. */
            rc = cf_json_string(out, (cf_span){url.ptr, url.len});
        }
        cf_builder_dispose(&url);
    }
    if (rc == CF_OK) rc = cf_builder_append(out, mj_span("}"));
    cf_view_user_dispose(&view);
    return rc;
}

/* The stored body rendered with its layout (`message.body.to_s`), with the
 * reference's `unwrap_or_default` rescue (cf_richtext_body_html already
 * clears to empty on render failure). */
static cf_err mj_body_html(cf_ctx *ctx, const cf_message *message,
                           cf_str *out) {
    memset(out, 0, sizeof *out);
    cf_str stored = {0};
    bool found = false;
    cf_err rc = cf_message_body_html(ctx->reader, message, &found, &stored);
    if (rc != CF_OK) return rc;
    if (!found || stored.ptr == NULL) {
        cf_str_dispose(&stored);
        return CF_OK;
    }
    cf_safe_html safe = {0};
    rc = cf_richtext_body_html(
        ctx, (cf_span){(const unsigned char *)stored.ptr, stored.len}, &safe);
    cf_str_dispose(&stored);
    if (rc != CF_OK) {
        cf_safe_html_dispose(&safe);
        return rc;
    }
    if (safe.bytes != NULL) {
        cf_span html = cf_buf_span(safe.bytes);
        char *copy = malloc(html.len + 1);
        if (copy == NULL) {
            cf_safe_html_dispose(&safe);
            return CF_NOMEM;
        }
        if (html.len != 0) memcpy(copy, html.ptr, html.len);
        copy[html.len] = '\0';
        out->ptr = copy;
        out->len = html.len;
    }
    cf_safe_html_dispose(&safe);
    return CF_OK;
}

/* Serialize into a scratch builder, then commit atomically so the caller's
 * builder is unchanged on failure. */
static cf_err mj_commit(cf_builder *scratch, cf_builder *out) {
    cf_err rc = cf_builder_append(out, (cf_span){scratch->ptr, scratch->len});
    cf_builder_dispose(scratch);
    return rc;
}

/* `messages/_message.json.jbuilder`. */
cf_err cf_views_message_json(cf_ctx *ctx, const cf_message *message,
                             cf_builder *out) {
    if (ctx == NULL || message == NULL || out == NULL) return CF_INVALID;
    size_t mark = out->len;
    cf_builder body = {0};
    cf_user creator = {0};
    cf_err rc = cf_user_find(ctx->reader, message->creator_id, &creator);
    cf_str plain = {0};
    cf_str html = {0};
    if (rc == CF_OK) {
        rc = cf_message_plain_text_body(ctx->reader, message,
                                        cf_tx_rich_text(NULL), &plain);
    }
    if (rc == CF_OK) rc = mj_body_html(ctx, message, &html);
    if (rc == CF_OK) rc = cf_builder_append(&body, mj_span("{\"id\":"));
    if (rc == CF_OK) rc = mj_json_i64(&body, message->id);
    if (rc == CF_OK) {
        rc = cf_builder_append(&body, mj_span(",\"created_at\":\""));
    }
    if (rc == CF_OK) rc = mj_json_time(message->created_at, &body);
    if (rc == CF_OK) {
        rc = cf_builder_append(&body, mj_span("\",\"body\":{\"plain_text\":"));
    }
    if (rc == CF_OK) {
        rc = cf_json_string(&body, (cf_span){(const unsigned char *)plain.ptr,
                                             plain.len});
    }
    if (rc == CF_OK) rc = cf_builder_append(&body, mj_span(",\"html\":"));
    if (rc == CF_OK) {
        rc = cf_json_string(&body, (cf_span){(const unsigned char *)html.ptr,
                                            html.len});
    }
    if (rc == CF_OK) rc = cf_builder_append(&body, mj_span("},\"creator\":"));
    if (rc == CF_OK) rc = mj_user_json(ctx, &creator, &body);
    if (rc == CF_OK) rc = cf_builder_append(&body, mj_span(",\"room\":{\"id\":"));
    if (rc == CF_OK) rc = mj_json_i64(&body, message->room_id);
    if (rc == CF_OK) rc = cf_builder_append(&body, mj_span("},\"url\":"));
    if (rc == CF_OK) {
        char path[64];
        int n = snprintf(path, sizeof path,
                         "/rooms/%" PRId64 "/messages/%" PRId64,
                         message->room_id, message->id);
        if (n < 0 || (size_t)n >= sizeof path) rc = CF_INTERNAL;
        else {
            cf_builder url = {0};
            rc = cf_views_absolute_url(ctx,
                                       (cf_span){(const unsigned char *)path,
                                                 (size_t)n},
                                       &url);
            if (rc == CF_OK) {
                rc = cf_json_string(&body, (cf_span){url.ptr, url.len});
            }
            cf_builder_dispose(&url);
        }
    }
    if (rc == CF_OK) rc = cf_builder_append(&body, mj_span("}"));
    cf_user_dispose(&creator);
    cf_str_dispose(&plain);
    cf_str_dispose(&html);
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        out->len = mark;
        return rc;
    }
    return mj_commit(&body, out);
}

/* `messages/boosts/_boost.json.jbuilder` through boosts_by_bots_show. */
cf_err cf_views_boost_json(cf_ctx *ctx, const cf_boost *boost,
                           const cf_message *message, cf_builder *out) {
    if (ctx == NULL || boost == NULL || message == NULL || out == NULL) {
        return CF_INVALID;
    }
    size_t mark = out->len;
    cf_builder body = {0};
    cf_user booster = {0};
    cf_err rc = cf_user_find(ctx->reader, boost->booster_id, &booster);
    if (rc == CF_OK) rc = cf_builder_append(&body, mj_span("{\"id\":"));
    if (rc == CF_OK) rc = mj_json_i64(&body, boost->id);
    if (rc == CF_OK) rc = cf_builder_append(&body, mj_span(",\"content\":"));
    if (rc == CF_OK) {
        rc = cf_json_string(&body, (cf_span){(const unsigned char *)boost->content.ptr,
                                            boost->content.len});
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(&body, mj_span(",\"created_at\":\""));
    }
    if (rc == CF_OK) rc = mj_json_time(boost->created_at, &body);
    if (rc == CF_OK) rc = cf_builder_append(&body, mj_span("\",\"booster\":"));
    if (rc == CF_OK) rc = mj_user_json(ctx, &booster, &body);
    if (rc == CF_OK) rc = cf_builder_append(&body, mj_span(",\"message\":{\"id\":"));
    if (rc == CF_OK) rc = mj_json_i64(&body, boost->message_id);
    if (rc == CF_OK) rc = cf_builder_append(&body, mj_span(",\"url\":"));
    if (rc == CF_OK) {
        char path[64];
        int n = snprintf(path, sizeof path,
                         "/rooms/%" PRId64 "/messages/%" PRId64,
                         message->room_id, message->id);
        if (n < 0 || (size_t)n >= sizeof path) rc = CF_INTERNAL;
        else {
            cf_builder url = {0};
            rc = cf_views_absolute_url(ctx,
                                       (cf_span){(const unsigned char *)path,
                                                 (size_t)n},
                                       &url);
            if (rc == CF_OK) {
                rc = cf_json_string(&body, (cf_span){url.ptr, url.len});
            }
            cf_builder_dispose(&url);
        }
    }
    if (rc == CF_OK) rc = cf_builder_append(&body, mj_span("}}"));
    cf_user_dispose(&booster);
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        out->len = mark;
        return rc;
    }
    return mj_commit(&body, out);
}
