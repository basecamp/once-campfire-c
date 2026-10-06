/* src/views/users_push.c — users/push_subscriptions/index.html +
 * _push_subscription.html (packet V-D; task A-users-push_subscriptions R1).
 *
 * Reference: tmp/rust-ref/crates/views/src/users.rs (PushSubscriptionsIndex,
 * PushSubscription), templates/users/push_subscriptions/{index,
 * _push_subscription}.html and helpers/{application,assets,forms,links,tag}.rs.
 *
 * The view models and renderer declarations live in src/views.h (landed by
 * V02 from this file's earlier proposal, verbatim); the dispose functions and
 * the pure UA row mapping (presenters::accounts::push_subscription over the
 * landed cf_ua_* parser) below are the definitions behind them.
 *
 * The row scoping (for_user, model order) and the last-room selection are the
 * V02 presenter cf_presenter_users_push_index
 * (src/presenters/push_subscriptions.c); rendering performs no SQL.  The
 * test-notification button posts to
 * user_push_subscription_test_notifications(id) (route 65, owned by the
 * test_notifications packet); the delete button posts to
 * user_push_subscription(id) with _method delete.  No token inputs (D-C02).
 */
#include "views/internal.h"

#include "auth/user_agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- view-model disposal (types declared in views.h) --------------------- */

static void push_str_clear(cf_str *value) {
    free(value->ptr);
    memset(value, 0, sizeof *value);
}

static cf_err push_str_dup(cf_span span, cf_str *out) {
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

void cf_view_push_subscription_vector_dispose(
    cf_view_push_subscription_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        push_str_clear(&vector->items[i].endpoint);
        push_str_clear(&vector->items[i].browser);
        push_str_clear(&vector->items[i].version);
        push_str_clear(&vector->items[i].platform);
    }
    free(vector->items);
    memset(vector, 0, sizeof *vector);
}

void cf_view_users_push_index_model_dispose(
    cf_view_users_push_index_model *model) {
    if (model == NULL) return;
    cf_view_push_subscription_vector_dispose(&model->subscriptions);
    memset(model, 0, sizeof *model);
}

/* ---- UA mapping (presenters::accounts::push_subscription) -------------------
 * agent = UserAgent.parse(user_agent or ""); browser = agent.browser(),
 * version = agent.version().to_string(), platform = agent.platform() or "".
 * Blank strings parse as "Mozilla/4.0 (compatible)" (user_agent.rs).
 */
static cf_err push_ua_parse(cf_span ua, cf_ua_agent *agent) {
    static const char fallback[] = "Mozilla/4.0 (compatible)";
    cf_span text = ua;
    bool blank = true;
    for (size_t i = 0; i < text.len; i++) {
        unsigned char c = text.ptr[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\v' && c != '\f' &&
            c != '\r') {
            blank = false;
            break;
        }
    }
    if (text.ptr == NULL || blank) {
        text.ptr = (const unsigned char *)fallback;
        text.len = sizeof fallback - 1;
    }
    return cf_ua_parse(text, agent);
}

cf_err cf_view_push_subscription_parse(cf_span user_agent,
                                       cf_str *browser_out,
                                       cf_str *version_out,
                                       cf_str *platform_out) {
    if (browser_out != NULL) memset(browser_out, 0, sizeof *browser_out);
    if (version_out != NULL) memset(version_out, 0, sizeof *version_out);
    if (platform_out != NULL) memset(platform_out, 0, sizeof *platform_out);
    if (browser_out == NULL || version_out == NULL || platform_out == NULL) {
        return CF_INVALID;
    }
    if (user_agent.len != 0 && user_agent.ptr == NULL) return CF_INVALID;
    cf_ua_agent agent;
    memset(&agent, 0, sizeof agent);
    cf_err rc = push_ua_parse(user_agent, &agent);
    if (rc != CF_OK) return rc;

    cf_span browser = {NULL, 0};
    if (cf_ua_browser(&agent, &browser) != CF_UA_SOME) {
        browser.ptr = NULL;
        browser.len = 0;
    }
    cf_span platform = {NULL, 0};
    if (cf_ua_platform(&agent, &platform) != CF_UA_SOME) {
        platform.ptr = NULL;
        platform.len = 0;
    }
    char scratch_buf[256];
    cf_ua_buf scratch = {scratch_buf, 0, sizeof scratch_buf, false};
    cf_ua_version_result version = cf_ua_version_of(&agent, &scratch);
    cf_span version_text = {NULL, 0};
    if (!version.raised && version.found) {
        version_text = version.version.text;
    }
    rc = push_str_dup(browser.ptr != NULL ? browser : (cf_span){NULL, 0},
                       browser_out);
    if (rc == CF_OK) {
        rc = push_str_dup(version_text, version_out);
    }
    if (rc == CF_OK) {
        rc = push_str_dup(platform.ptr != NULL ? platform
                                              : (cf_span){NULL, 0},
                           platform_out);
    }
    cf_ua_dispose(&agent);
    if (rc != CF_OK) {
        push_str_clear(browser_out);
        push_str_clear(version_out);
        push_str_clear(platform_out);
    }
    return rc;
}

cf_err cf_view_push_subscription_from_row(int64_t id, cf_span endpoint,
                                         cf_span user_agent,
                                         cf_view_push_subscription *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if ((endpoint.len != 0 && endpoint.ptr == NULL) ||
        (user_agent.len != 0 && user_agent.ptr == NULL)) {
        return CF_INVALID;
    }
    cf_err rc = push_str_dup(endpoint, &out->endpoint);
    if (rc == CF_OK) {
        rc = cf_view_push_subscription_parse(user_agent, &out->browser,
                                             &out->version, &out->platform);
    }
    if (rc == CF_OK) {
        out->id = id;
        return CF_OK;
    }
    push_str_clear(&out->endpoint);
    push_str_clear(&out->browser);
    push_str_clear(&out->version);
    push_str_clear(&out->platform);
    memset(out, 0, sizeof *out);
    return rc;
}

/* ---- partial --------------------------------------------------------------- */

static cf_err push_img(const cf_view_ctx *ctx, const char *src, cf_builder *out) {
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    cf_err rc = cf_view_attr_cstr(&attrs, "aria-hidden", "true");
    if (rc == CF_OK) rc = cf_view_attr_i64(&attrs, "size", 20);
    if (rc != CF_OK) return rc;
    return cf_view_image_tag(ctx, cf_span_of_lit(src), &attrs, out);
}

/* users/push_subscriptions/_push_subscription.html. */
cf_err cf_view_push_subscription_partial(
    const cf_view_ctx *ctx, const cf_view_push_subscription *subscription,
    cf_builder *out) {
    if (ctx == NULL || subscription == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    CF_VIEW_TRY(cf_view_str(out, "<li class=\"flex flex-column margin-none "
                                 "membership-item\">\n  <span class=\"overflow-"
                                 "ellipsis txt-primary txt-undecorated\">\n"
                                 "    <strong>"));
    CF_VIEW_TRY(cf_view_text(
        out, (cf_span){(const unsigned char *)subscription->browser.ptr,
                      subscription->browser.len}));
    CF_VIEW_TRY(cf_view_str(out, " "));
    CF_VIEW_TRY(cf_view_text(
        out, (cf_span){(const unsigned char *)subscription->version.ptr,
                      subscription->version.len}));
    CF_VIEW_TRY(cf_view_str(out, " on "));
    CF_VIEW_TRY(cf_view_text(
        out, (cf_span){(const unsigned char *)subscription->platform.ptr,
                      subscription->platform.len}));
    CF_VIEW_TRY(cf_view_str(out, "</strong><br>\n  </span>\n\n  <span "
                                 "class=\"flex align-start gap txt-small\">\n"
                                 "    <span>"));
    CF_VIEW_TRY(cf_view_text(
        out, (cf_span){(const unsigned char *)subscription->endpoint.ptr,
                      subscription->endpoint.len}));
    CF_VIEW_TRY(cf_view_str(out, "</span>\n\n    <span class=\"flex "
                                 "align-center gap\">\n      "));
    /* Test-notification button (route 65). */
    {
        char path[64];
        int n = snprintf(path, sizeof path,
                         "/users/me/push_subscriptions/%lld/test_notifications",
                         (long long)subscription->id);
        if (n < 0 || (size_t)n >= sizeof path) CF_VIEW_TRY(CF_INTERNAL);
        cf_view_attrs opts;
        cf_view_attrs_init(&opts);
        CF_VIEW_TRY(
            cf_view_attr_cstr(&opts, "class", "btn btn--reversed"));
        cf_builder body = {0};
        CF_VIEW_TRY(cf_view_str(&body, "\n        "));
        CF_VIEW_TRY(push_img(ctx, "notification-bell-everything.svg", &body));
        CF_VIEW_TRY(cf_view_str(
            &body, "\n        <span class=\"for-screen-reader\">Send test "
                   "notification</span>\n"));
        CF_VIEW_TRY(cf_view_button_to(
            out, (cf_span){(const unsigned char *)path, (size_t)n}, &opts,
            (cf_span){NULL, 0}, (cf_span){NULL, 0}, cf_view_span_of(&body)));
        cf_builder_dispose(&body);
    }
    CF_VIEW_TRY(cf_view_str(out, "\n      "));
    /* Delete button (method delete travels separately, like the
     * involvement partial above). */
    {
        char path[64];
        int n = snprintf(path, sizeof path, "/users/me/push_subscriptions/%lld",
                         (long long)subscription->id);
        if (n < 0 || (size_t)n >= sizeof path) CF_VIEW_TRY(CF_INTERNAL);
        cf_view_attrs button_opts;
        cf_view_attrs_init(&button_opts);
        CF_VIEW_TRY(cf_view_attr_cstr(&button_opts, "class",
                                      "btn btn--negative"));
        cf_builder body = {0};
        CF_VIEW_TRY(cf_view_str(&body, "\n        "));
        CF_VIEW_TRY(push_img(ctx, "minus.svg", &body));
        CF_VIEW_TRY(cf_view_str(
            &body, "\n        <span class=\"for-screen-reader\">Delete "
                   "subscription</span>\n"));
        CF_VIEW_TRY(cf_view_button_to(
            out, (cf_span){(const unsigned char *)path, (size_t)n},
            &button_opts, (cf_span){(const unsigned char *)"delete", 6},
            (cf_span){NULL, 0}, cf_view_span_of(&body)));
        cf_builder_dispose(&body);
    }
    CF_VIEW_TRY(cf_view_str(out, "    </span>\n  </span>\n</li>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* ---- page ------------------------------------------------------------------- */

static cf_err push_link_back_to(const cf_view_ctx *ctx, cf_span dest,
                                cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder body = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("arrow-left.svg"),
                                      &img, &body));
    }
    {
        cf_view_attrs sr;
        cf_view_attrs_init(&sr);
        CF_VIEW_TRY(cf_view_attr_cstr(&sr, "class", "for-screen-reader"));
        CF_VIEW_TRY(cf_view_content_text(&body, "span", &sr,
                                         cf_span_of_lit("Go Back")));
    }
    {
        cf_view_attrs link;
        cf_view_attrs_init(&link);
        CF_VIEW_TRY(cf_view_attr_cstr(&link, "class", "btn"));
        CF_VIEW_TRY(cf_view_attr(&link, "href", dest));
        CF_VIEW_TRY(cf_view_content(out, "a", &link, cf_view_span_of(&body)));
    }
    cf_builder_dispose(&body);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&body);
    return cf_view_fail(&guard, rc);
}

/* link_back_to_last_room_visited: room path when the ctx carries one. */
static cf_err push_nav(const cf_view_ctx *ctx, int64_t last_room_id,
                       bool has_last_room, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder dest = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, "  <div class=\"flex-item-justify-start\">\n"
                                 "    "));
    if (has_last_room) {
        char tmp[48];
        int n = snprintf(tmp, sizeof tmp, "/rooms/%lld",
                         (long long)last_room_id);
        if (n < 0 || (size_t)n >= sizeof tmp) CF_VIEW_TRY(CF_INTERNAL);
        CF_VIEW_TRY(cf_view_str(&dest, tmp));
    } else {
        CF_VIEW_TRY(cf_view_str(&dest, "/"));
    }
    CF_VIEW_TRY(push_link_back_to(ctx, cf_view_span_of(&dest), out));
    CF_VIEW_TRY(cf_view_str(out, "\n  </div>\n"));
    cf_builder_dispose(&dest);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&dest);
    return cf_view_fail(&guard, rc);
}

static cf_err push_content(const cf_view_ctx *ctx,
                           const cf_view_users_push_index_model *model,
                           cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(
        out, "<section class=\"panel panel--wide flex flex-column gap\">\n"
             "  <h1 class=\"txt-align-center txt-large margin-none\">Push "
             "Notification Subscriptions</h1>\n"
             "  <div class=\"pad-inline fill-shade border-radius\" "
             "id=\"push_subscriptions\">\n"
             "    <menu class=\"pad flex flex-column gap\">\n"));
    for (size_t i = 0; i < model->subscriptions.len; i++) {
        CF_VIEW_TRY(cf_view_str(out, "      "));
        CF_VIEW_TRY(cf_view_push_subscription_partial(
            ctx, &model->subscriptions.items[i], out));
    }
    CF_VIEW_TRY(cf_view_str(out, "    </menu>\n  </div>\n</section>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_users_push_index(const cf_view_ctx *ctx,
                                const cf_view_users_push_index_model *model,
                                cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder head = {0}, content = {0}, nav = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = push_content(ctx, model, &content);
    if (rc != CF_OK) goto done;
    rc = push_nav(ctx, model->last_room_id, model->has_last_room, &nav);
    if (rc != CF_OK) goto done;
    rc = cf_view_layout_page(ctx, cf_span_of_lit("Push notification "
                                                 "subscriptions"),
                             true, (cf_span){NULL, 0}, false,
                             cf_view_span_of(&head), cf_view_span_of(&content),
                             cf_view_span_of(&nav), (cf_span){NULL, 0},
                             (cf_span){NULL, 0}, out);
done:
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    cf_builder_dispose(&nav);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

/* Frame variant (Turbo-Frame requests render head + content only). */
cf_err cf_view_users_push_index_frame(
    const cf_view_ctx *ctx, const cf_view_users_push_index_model *model,
    cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder head = {0}, content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = push_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, cf_view_span_of(&head),
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}
