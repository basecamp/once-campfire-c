/* src/views/searches.c — A-searches views:
 * tmp/rust-ref/crates/views/templates/searches/index.html and _recents.html,
 * with SearchesHelper#search_path (crates/views/src/searches.rs).
 *
 * The templates extend layouts/application with an empty head block, so a
 * Turbo-Frame request renders head + content in turbo-rails' frame layout
 * (page::framed_page!).  The message items go through the landed messages
 * partial (messages::cached_message_item -> messages/_message), not a search
 * copy, so the search results are byte-identical to the room page's items.
 */
#include "views/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------- model disposal */

void cf_view_str_vector_dispose(cf_view_str_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_str_dispose(&vector->items[i]);
    }
    free(vector->items);
    memset(vector, 0, sizeof *vector);
}

void cf_view_searches_index_model_dispose(cf_view_searches_index_model *model) {
    if (model == NULL) return;
    cf_str_dispose(&model->query);
    cf_str_dispose(&model->q);
    cf_view_message_item_vector_dispose(&model->messages);
    cf_view_str_vector_dispose(&model->recent_searches);
    memset(model, 0, sizeof *model);
}

/* ------------------------------------------------------------- helpers */

/* `SearchesHelper#search_path`: "/searches?q=" + CGI.escape(query) —
 * alphanumerics and `_.-~` pass, a space is `+`, every other byte `%XX`
 * (uppercase), exactly ruby_compat::cgi_escape. */
cf_err cf_view_searches_search_path(cf_span query, cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    if (query.len != 0 && query.ptr == NULL) return CF_INVALID;
    static const char *const HEX = "0123456789ABCDEF";
    cf_err rc = cf_view_str(out, "/searches?q=");
    if (rc != CF_OK) return rc;
    for (size_t i = 0; i < query.len; i++) {
        unsigned char byte = query.ptr[i];
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '_' || byte == '.' ||
            byte == '-' || byte == '~') {
            rc = cf_view_raw(out, (cf_span){&byte, 1});
        } else if (byte == ' ') {
            rc = cf_view_str(out, "+");
        } else {
            char encoded[3] = {'%', HEX[byte >> 4], HEX[byte & 0x0F]};
            rc = cf_view_raw(out, (cf_span){(const unsigned char *)encoded, 3});
        }
        if (rc != CF_OK) return rc;
    }
    return CF_OK;
}

/* searches/_recents.html: the recent-search links (nav and sidebar) plus the
 * "Clear recent searches" button_to form when there are any. */
static cf_err searches_recents(const cf_view_ctx *ctx,
                               const cf_view_searches_index_model *model,
                               cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc = CF_OK;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    for (size_t i = 0; i < model->recent_searches.len; i++) {
        cf_builder path = {0};
        rc = cf_view_searches_search_path(
            cf_str_span(model->recent_searches.items[i]), &path);
        if (rc == CF_OK) {
            CF_VIEW_TRY(cf_view_str(
                out,
                "\n      <a class=\"align-center gap room btn txt-nowrap\" "
                "href=\""));
            CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&path)));
            CF_VIEW_TRY(cf_view_str(
                out,
                "\">\n        <span class=\"overflow-ellipsis\">"
                "\xE2\x80\x9C"));
            CF_VIEW_TRY(cf_view_text(
                out, cf_str_span(model->recent_searches.items[i])));
            CF_VIEW_TRY(cf_view_str(out, "\xE2\x80\x9D</span>\n</a>"));
        }
        cf_builder_dispose(&path);
        if (rc != CF_OK) goto fail;
    }

    if (model->recent_searches.len != 0) {
        cf_builder action = {0};
        rc = cf_view_ctx_url(ctx, cf_span_of_lit("/searches/clear"), &action);
        if (rc == CF_OK) {
            CF_VIEW_TRY(cf_view_str(
                out,
                "\n      <form class=\"button_to\" method=\"post\" "
                "action=\""));
            CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&action)));
            CF_VIEW_TRY(cf_view_str(
                out,
                "\"><input type=\"hidden\" name=\"_method\" "
                "value=\"delete\" /><button class=\"btn searches__btn\" "
                "data-turbo-confirm=\"Are you sure you want to clear your "
                "recent searches?\" type=\"submit\">\n        "));
            cf_view_attrs attrs;
            cf_view_attrs_init(&attrs);
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("broom.svg"),
                                          &attrs, out));
            CF_VIEW_TRY(cf_view_str(
                out,
                "\n        <span class=\"for-screen-reader\">Clear recent "
                "searches</span>\n</button></form>"));
        }
        cf_builder_dispose(&action);
        if (rc != CF_OK) goto fail;
    }
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* The nav block: the query chip when a query was submitted, then the recents
 * bar. */
static cf_err searches_nav(const cf_view_ctx *ctx,
                           const cf_view_searches_index_model *model,
                           cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc = CF_OK;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    if (model->has_query) {
        char count[24];
        int n = snprintf(count, sizeof count, "%zu", model->messages.len);
        if (n < 0 || (size_t)n >= sizeof count) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_str(
            out,
            "\n  <div class=\"searches__query flex align-center gap "
            "pad-block-start-half\">\n"
            "      <div class=\"btn btn--reversed btn--faux align-center gap "
            "txt-nowrap\">\n"
            "        <span class=\"overflow-ellipsis\">\xE2\x80\x9C"));
        CF_VIEW_TRY(cf_view_text(out, cf_str_span(model->query)));
        CF_VIEW_TRY(cf_view_str(
            out,
            "\xE2\x80\x9D</span>\n"
            "        <span class=\"flex-item-no-shrink\">"));
        CF_VIEW_TRY(cf_view_raw(out, (cf_span){
            (const unsigned char *)count, (size_t)n}));
        CF_VIEW_TRY(cf_view_str(out, "</span>\n</div>    </div>"));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n\n  <div class=\"searches__recents align-center gap "
        "pad-block-half overflow-y overflow-hide-scrollbar\">"));
    CF_VIEW_TRY(searches_recents(ctx, model, out));
    CF_VIEW_TRY(cf_view_str(out, "\n  </div>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* The sidebar block: the same recents partial in its own scroll box. */
static cf_err searches_sidebar(const cf_view_ctx *ctx,
                               const cf_view_searches_index_model *model,
                               cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc = CF_OK;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n  <div class=\"rooms position-relative flex flex-column gap "
        "overflow-y overflow-hide-scrollbar\">"));
    CF_VIEW_TRY(searches_recents(ctx, model, out));
    CF_VIEW_TRY(cf_view_str(out, "\n  </div>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* The content block: the empty-state figure and the search-results message
 * area. */
static cf_err searches_content(const cf_view_ctx *ctx,
                               const cf_view_searches_index_model *model,
                               cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc = CF_OK;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    CF_VIEW_TRY(cf_view_str(
        out,
        "<div id=\"message-area\" class=\"message-area\">\n"
        "  <div class=\"message-area--empty min-width center\">\n"
        "    <figure class=\"center pad\">\n"
        "      "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class",
                                      "colorize--black translucent"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("search.svg"),
                                      &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n    </figure>\n"
        "  </div>\n\n"
        "  <div id=\"search-results\" class=\"messages searches__results\" "
        "data-controller=\"search-results\" "
        "data-search-results-target=\"messages\" "
        "data-search-results-me-class=\"message--me\" "
        "data-search-results-threaded-class=\"message--threaded\" "
        "data-search-results-mentioned-class=\"message--mentioned\" "
        "data-search-results-formatted-class=\"message--formatted\">"));
    for (size_t i = 0; i < model->messages.len; i++) {
        CF_VIEW_TRY(cf_view_str(out, "\n    "));
        CF_VIEW_TRY(cf_view_message_item_partial(
            ctx, &model->messages.items[i], out));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n  </div></div>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* The footer block: the exit link back to the last room and the search form. */
static cf_err searches_footer(const cf_view_ctx *ctx,
                              const cf_view_searches_index_model *model,
                              cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc = CF_OK;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    CF_VIEW_TRY(cf_view_str(out,
                            "\n  <div class=\"composer flex align-end gap\">\n"
                            "    "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(
            &attrs, "class", "btn flex-item-no-shrink margin-block-end"));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &attrs, "style",
            "view-transition-name: input-switcher; --btn-border-radius: "
            "0.5em"));
        char href[64];
        int n = snprintf(href, sizeof href, "/rooms/%lld",
                         (long long)model->return_to_room_id);
        if (n < 0 || (size_t)n >= sizeof href) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_attr(
            &attrs, "href", (cf_span){(const unsigned char *)href,
                                      (size_t)n}));
        CF_VIEW_TRY(cf_view_open_start(out, "a", &attrs));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n      "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("arrow-left.svg"),
                                      &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n      <span class=\"for-screen-reader\">Exit search "
        "</span>\n</a>\n"
        "    "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class",
                                      "margin-block flex-item-grow contain "
                                      "flex align-center gap"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-controller", "form"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-action",
                                      "keydown.esc->form#cancel"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "action", "/searches"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "accept-charset", "UTF-8"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "method", "post"));
        CF_VIEW_TRY(cf_view_open_start(out, "form", &attrs));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n      <div class=\"composer__input flex align-center "
        "flex-item-grow gap full-width input input--actor min-width\">\n"
        "        "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &attrs, "class", "composer__input-hint colorize--black"));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &attrs, "style", "view-transition-name: input-btn;"));
        /* image_tag's `size` renders width/height after src, the template's
         * order (`src=... width="20" height="20"`). */
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "size", "20x20"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("search.svg"),
                                      &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n\n        "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_opt(&attrs, "value", model->has_q,
                                     cf_str_span(model->q)));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &attrs, "class", "searches__input input flex-item-grow"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "role", "searchbox"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-label", "search"));
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "autofocus", true));
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "required", true));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "type", "text"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "name", "q"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "id", "q"));
        CF_VIEW_TRY(cf_view_builder_tag(out, "input", &attrs));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n\n        <a data-form-target=\"cancel\" role=\"button\" "
        "class=\"searches__reset\" href=\"/searches\">\n"
        "          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "size", "14x14"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("remove.svg"),
                                      &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n          <span class=\"for-screen-reader\">Clear search "
        "field</span>\n</a>\n"
        "        "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "name", "button"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "type", "submit"));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &attrs, "class",
            "btn btn--reversed flex-item-no-shrink txt-small"));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &attrs, "style", "--btn-border-radius: 0.5em"));
        CF_VIEW_TRY(cf_view_open_start(out, "button", &attrs));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("arrow-up.svg"),
                                      &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n          <span class=\"for-screen-reader\">Search</span>\n"
        "</button>      </div>\n</form>  </div>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* ------------------------------------------------------------- renderers */

cf_err cf_view_searches_index(const cf_view_ctx *ctx,
                              const cf_view_searches_index_model *model,
                              cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder head = {0}, content = {0}, nav = {0}, footer = {0},
               sidebar = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    /* `{%- block head %}{% endblock %}` — empty. */
    if (rc == CF_OK) rc = searches_nav(ctx, model, &nav);
    if (rc == CF_OK) rc = searches_sidebar(ctx, model, &sidebar);
    if (rc == CF_OK) rc = searches_content(ctx, model, &content);
    if (rc == CF_OK) rc = searches_footer(ctx, model, &footer);
    if (rc == CF_OK) {
        rc = cf_view_layout_page(
            ctx, cf_span_of_lit("Search"), true,
            cf_span_of_lit("sidebar searches"), true,
            cf_view_span_of(&head), cf_view_span_of(&content),
            cf_view_span_of(&nav), cf_view_span_of(&footer),
            cf_view_span_of(&sidebar), out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    cf_builder_dispose(&nav);
    cf_builder_dispose(&footer);
    cf_builder_dispose(&sidebar);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_searches_index_frame(const cf_view_ctx *ctx,
                                    const cf_view_searches_index_model *model,
                                    cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder head = {0}, content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    /* blocks = ["head", "content"]: the frame layout drops nav/sidebar/footer
     * (page::framed_page!'s turbo-rails choice). */
    rc = searches_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, cf_view_span_of(&head),
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}
