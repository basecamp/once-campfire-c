/* src/views/bots.c — accounts/bots views (packet V-B).
 *
 * Templates: tmp/rust-ref/crates/views/templates/accounts/bots/
 * (index.html, new.html, edit.html, _bot.html, _form.html), via
 * tmp/rust-ref/crates/views/src/accounts.rs (`BotsIndex`, `BotsNew`,
 * `BotsEdit`, `Bot`, `BotForm`) and the helpers they call
 * (helpers/{application,assets,forms,links,translations,users}.rs,
 * `campfire_routes`).
 *
 * The pages extend layouts/application with an empty head block, so a
 * Turbo-Frame request renders head + content in turbo-rails' frame layout.
 * Pure render: no SQL, mutation, filesystem or network work.  The authenticity
 * token inputs the reference renders are omitted (forgery protection is by
 * Sec-Fetch-Site); the golden comparison masks them.
 */
#include "views/internal.h"

#include "presenters/bots.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SHIM (R-BOTS-T9N): the `chat_bots`/`bot_name`/`webhook_url` translation
 * sets, verbatim from
 * tmp/rust-ref/crates/views/src/helpers/translations_table.rs.  The shared
 * src/views/translations.c table does not carry them yet (it only has the
 * foundation families' keys), and this packet may not touch shared files.
 * bots_translation_button below renders byte-identically to
 * cf_view_translation_button for these three keys; when the integrator lands
 * them in translations.c, delete this block and switch the three call sites
 * back to cf_view_translation_button. */
struct bots_translation {
    const char *language;
    const char *text;
};

struct bots_translation_set {
    const char *key;
    size_t count;
    const struct bots_translation *items;
};

static const struct bots_translation bots_chat_bots_items[] = {
    {"\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8", "Chat bots. With Chat bots, other sites and services can post updates directly to Campfire."},
    {"\xF0\x9F\x87\xAA\xF0\x9F\x87\xB8", "Bots de chat. Con los bots de chat, otros sitios y servicios pueden publicar actualizaciones directamente en Campfire."},
    {"\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7", "Bots de discussion. Avec les bots de discussion, d'autres sites et services peuvent publier des mises à jour directement sur Campfire."},
    {"\xF0\x9F\x87\xAE\xF0\x9F\x87\xB3", "चैट बॉट। चैट बॉट के साथ, अन्य साइटों और सेवाएं सीधे कैम्पफायर में अपडेट पोस्ट कर सकती हैं।"},
    {"\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA", "Chat-Bots. Mit Chat-Bots können andere Websites und Dienste Updates direkt in Campfire veröffentlichen."},
    {"\xF0\x9F\x87\xA7\xF0\x9F\x87\xB7", "Chat bots. Com Chat bots, outros sites e serviços podem postar atualizações diretamente no Campfire."},
    {"\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5", "チャットボット。チャットボットを使用すると、他のサイトやサービスがCampfireに直接更新情報を投稿できます。"},
};

static const struct bots_translation bots_bot_name_items[] = {
    {"\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8", "Name the bot"},
    {"\xF0\x9F\x87\xAA\xF0\x9F\x87\xB8", "Nombrar al bot"},
    {"\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7", "Nommer le bot"},
    {"\xF0\x9F\x87\xAE\xF0\x9F\x87\xB3", "बॉट का नाम दें"},
    {"\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA", "Benenne den Bot"},
    {"\xF0\x9F\x87\xA7\xF0\x9F\x87\xB7", "Dê um nome ao bot"},
    {"\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5", "ボットに名前を付ける"},
};

static const struct bots_translation bots_webhook_url_items[] = {
    {"\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8", "Webhook URL"},
    {"\xF0\x9F\x87\xAA\xF0\x9F\x87\xB8", "URL del Webhook"},
    {"\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7", "URL du webhook"},
    {"\xF0\x9F\x87\xAE\xF0\x9F\x87\xB3", "वेबहुक URL"},
    {"\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA", "Webhook-URL"},
    {"\xF0\x9F\x87\xA7\xF0\x9F\x87\xB7", "URL do Webhook"},
    {"\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5", "Webhook URL"},
};

static const struct bots_translation_set BOTS_SETS[] = {
    {"chat_bots", sizeof bots_chat_bots_items / sizeof bots_chat_bots_items[0],
     bots_chat_bots_items},
    {"bot_name", sizeof bots_bot_name_items / sizeof bots_bot_name_items[0],
     bots_bot_name_items},
    {"webhook_url",
     sizeof bots_webhook_url_items / sizeof bots_webhook_url_items[0],
     bots_webhook_url_items},
};

/* `translations_for` over BOTS_SETS (src/views/translations.c). */
static cf_err bots_translations_for(const char *key, cf_builder *out) {
    const struct bots_translation_set *set = NULL;
    for (size_t i = 0; i < sizeof BOTS_SETS / sizeof BOTS_SETS[0]; i++) {
        if (strcmp(BOTS_SETS[i].key, key) == 0) {
            set = &BOTS_SETS[i];
            break;
        }
    }
    if (set == NULL) return CF_NOT_FOUND;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "language-list"));
    CF_VIEW_TRY(cf_view_open_start(out, "dl", &attrs));
    for (size_t i = 0; i < set->count; i++) {
        cf_view_attrs dt;
        cf_view_attrs_init(&dt);
        CF_VIEW_TRY(cf_view_content_text(
            out, "dt", &dt,
            (cf_span){(const unsigned char *)set->items[i].language,
                      strlen(set->items[i].language)}));
        cf_view_attrs dd;
        cf_view_attrs_init(&dd);
        CF_VIEW_TRY(cf_view_attr_cstr(&dd, "class", "margin-none"));
        CF_VIEW_TRY(cf_view_content_text(
            out, "dd", &dd,
            (cf_span){(const unsigned char *)set->items[i].text,
                      strlen(set->items[i].text)}));
    }
    CF_VIEW_TRY(cf_view_close_tag(out, "dl"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* `translation_button` for the shimmed keys (cf_view_translation_button in
 * src/views/translations.c). */
static cf_err bots_translation_button(const cf_view_ctx *ctx, const char *key,
                                      cf_builder *out) {
    if (ctx == NULL || key == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder inner = {0}, summary = {0}, menu = {0}, list = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    CF_VIEW_TRY(cf_view_str(&summary,
                            "<summary class=\"btn\" tabindex=\"-1\">"));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "size", "20"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "class", "color-icon"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("globe.svg"), &img,
                                      &summary));
        cf_view_attrs sr;
        cf_view_attrs_init(&sr);
        CF_VIEW_TRY(cf_view_attr_cstr(&sr, "class", "for-screen-reader"));
        CF_VIEW_TRY(cf_view_content_text(&summary, "span", &sr,
                                         cf_span_of_lit("Translate")));
    }
    CF_VIEW_TRY(cf_view_str(&summary, "</summary>"));
    CF_VIEW_TRY(cf_view_raw(&inner, cf_view_span_of(&summary)));
    cf_builder_dispose(&summary);

    CF_VIEW_TRY(bots_translations_for(key, &list));
    {
        cf_view_attrs div;
        cf_view_attrs_init(&div);
        CF_VIEW_TRY(cf_view_attr_cstr(&div, "class",
                                      "language-list-menu shadow"));
        CF_VIEW_TRY(cf_view_attr_cstr(&div, "data-popup-target", "menu"));
        CF_VIEW_TRY(cf_view_content(&menu, "div", &div,
                                    cf_view_span_of(&list)));
    }
    CF_VIEW_TRY(cf_view_raw(&inner, cf_view_span_of(&menu)));
    cf_builder_dispose(&menu);
    cf_builder_dispose(&list);

    cf_view_attrs details;
    cf_view_attrs_init(&details);
    CF_VIEW_TRY(cf_view_attr_cstr(&details, "class", "position-relative"));
    CF_VIEW_TRY(cf_view_attr_cstr(&details, "data-controller", "popup"));
    CF_VIEW_TRY(cf_view_attr_cstr(
        &details, "data-action",
        "keydown.esc->popup#close toggle->popup#toggle "
        "click@document->popup#closeOnClickOutside"));
    CF_VIEW_TRY(cf_view_attr_cstr(&details, "data-popup-orientation-top-class",
                                  "popup-orientation-top"));
    CF_VIEW_TRY(cf_view_content(out, "details", &details,
                                cf_view_span_of(&inner)));
    cf_builder_dispose(&inner);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&inner);
    cf_builder_dispose(&summary);
    cf_builder_dispose(&menu);
    cf_builder_dispose(&list);
    return cf_view_fail(&guard, rc);
}

/* ------------------------------------------------------------ nav block */

/* `link_back_to(destination)`: the btn with the arrow image and the
 * screen-reader "Go Back". */
static cf_err bots_back_link(const cf_view_ctx *ctx, cf_span href,
                             cf_builder *out) {
    cf_err rc = CF_OK;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "btn"));
    CF_VIEW_TRY(cf_view_attr(&attrs, "href", href));
    CF_VIEW_TRY(cf_view_open_start(out, "a", &attrs));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
        CF_VIEW_TRY(
            cf_view_image_tag(ctx, cf_span_of_lit("arrow-left.svg"), &img, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out, "<span class=\"for-screen-reader\">Go Back</span></a>"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

static cf_err bots_nav(const cf_view_ctx *ctx, const char *destination,
                       cf_builder *out) {
    cf_err rc = CF_OK;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, "  <div class=\"flex-item-justify-start\">\n    "));
    CF_VIEW_TRY(bots_back_link(ctx, cf_span_of_lit(destination), out));
    CF_VIEW_TRY(cf_view_str(out, "\n  </div>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* ------------------------------------------------------- copy-to-clipboard */

/* `button_to_copy_to_clipboard(url) { content }` with the copy-paste image
 * and the screen-reader label. */
static cf_err bots_copy_button(const cf_view_ctx *ctx, cf_span curl_line,
                               const char *label, cf_builder *out) {
    cf_err rc = CF_OK;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "btn"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-controller",
                                  "copy-to-clipboard"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-action",
                                  "copy-to-clipboard#copy"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs,
                                  "data-copy-to-clipboard-success-class",
                                  "btn--success"));
    CF_VIEW_TRY(cf_view_attr(&attrs, "data-copy-to-clipboard-content-value",
                             curl_line));
    CF_VIEW_TRY(cf_view_open_start(out, "button", &attrs));
    CF_VIEW_TRY(cf_view_str(out, "\n            "));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("copy-paste.svg"),
                                      &img, out));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n            <span class=\"for-screen-reader\">"));
    CF_VIEW_TRY(cf_view_str(out, label));
    CF_VIEW_TRY(cf_view_str(out, "</span>\n"));
    CF_VIEW_TRY(cf_view_close_tag(out, "button"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* One curl row: the leading image, the readonly input and the copy button. */
static cf_err bots_curl_row(const cf_view_ctx *ctx, cf_span curl_line,
                            const char *icon, const char *aria_label,
                            const char *copy_label, cf_builder *out) {
    cf_err rc = CF_OK;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, "      <div class=\"flex align-center gap\">\n        "));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 24));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit(icon), &img, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n\n        <div class=\"flex-item-grow\">\n"
        "          <input type=\"text\" class=\"input full-width fill-white\" value=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, curl_line));
    CF_VIEW_TRY(cf_view_str(out, "\" aria-label=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_span_of_lit(aria_label)));
    /* Bare `readonly` (rooms.c's invite-url precedent): the flag helper
     * would render `readonly="readonly"`, which the DOM comparison reads
     * as a different attribute value. */
    CF_VIEW_TRY(cf_view_str(out, "\" readonly>\n        </div>\n\n        <div class=\"txt-small\">\n          "));
    CF_VIEW_TRY(bots_copy_button(ctx, curl_line, copy_label, out));
    CF_VIEW_TRY(cf_view_str(out, "        </div>\n      </div>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* ---------------------------------------------------------- _bot partial */

/* `accounts/bots/_bot.html`: the avatar, name, edit link and one fieldset
 * per non-direct room with the text/upload curl commands. */
static cf_err bots_bot_partial(const cf_view_ctx *ctx,
                               const cf_view_accounts_bot *bot,
                               cf_builder *out) {
    cf_err rc = CF_OK;
    cf_view_guard guard;
    cf_builder url = {0};
    char id_text[24];
    int n = snprintf(id_text, sizeof id_text, "%lld", (long long)bot->id);
    if (n < 0 || (size_t)n >= sizeof id_text) return CF_INTERNAL;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(
        out,
        "<li class=\"flex flex-column gap flush fill-shade border-radius "
        "pad-block pad-inline-double\">\n"
        "  <div class=\"flex align-center gap\">\n"
        "    <figure class=\"avatar flex-item--no-shrink\" "
        "style=\"--avatar-size: 2.65em;\">\n      "));
    /* `avatar_tag(bot.user, loading: :lazy)`. */
    {
        cf_builder path = {0};
        rc = cf_view_str(&path, "/users/");
        if (rc == CF_OK) {
            rc = cf_view_raw(&path, (cf_span){(const unsigned char *)id_text,
                                              (size_t)n});
        }
        if (rc == CF_OK) {
            cf_view_attrs link;
            cf_view_attrs_init(&link);
            CF_VIEW_TRY(cf_view_attr(&link, "title", cf_str_span(bot->title)));
            CF_VIEW_TRY(cf_view_attr_cstr(&link, "class", "btn avatar"));
            CF_VIEW_TRY(cf_view_attr_cstr(&link, "data-turbo-frame", "_top"));
            CF_VIEW_TRY(cf_view_attr(&link, "href", cf_view_span_of(&path)));
            CF_VIEW_TRY(cf_view_open_start(out, "a", &link));
        }
        if (rc == CF_OK) {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "loading", "lazy"));
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "size", "48"));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_str_span(bot->avatar_url),
                                          &img, out));
        }
        if (rc == CF_OK) rc = cf_view_close_tag(out, "a");
        cf_builder_dispose(&path);
        if (rc != CF_OK) goto fail;
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n    </figure>\n\n"
        "    <div class=\"min-width\">\n"
        "      <div class=\"overflow-ellipsis txt-large\"><strong>"));
    CF_VIEW_TRY(cf_view_text(out, cf_str_span(bot->name)));
    CF_VIEW_TRY(cf_view_str(out, "</strong></div>\n    </div>\n\n    "));
    /* `link_to edit_account_bot_path(bot), class:, style:`. */
    {
        char style[64];
        int style_n =
            snprintf(style, sizeof style, "view-transition-name: chat-bot-%s",
                     id_text);
        if (style_n < 0 || (size_t)style_n >= sizeof style) {
            rc = CF_INTERNAL;
            goto fail;
        }
        char href[64];
        n = snprintf(href, sizeof href, "/account/bots/%s/edit", id_text);
        if (n < 0 || (size_t)n >= sizeof href) {
            rc = CF_INTERNAL;
            goto fail;
        }
        cf_view_attrs link;
        cf_view_attrs_init(&link);
        CF_VIEW_TRY(cf_view_attr_cstr(&link, "class", "btn flex-item-justify-end"));
        CF_VIEW_TRY(cf_view_attr(
            &link, "style",
            (cf_span){(const unsigned char *)style, (size_t)style_n}));
        size_t href_len = strlen(href);
        CF_VIEW_TRY(cf_view_attr(
            &link, "href",
            (cf_span){(const unsigned char *)href, href_len}));
        CF_VIEW_TRY(cf_view_open_start(out, "a", &link));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n      "));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("pencil.svg"), &img,
                                      out));
    }
    CF_VIEW_TRY(cf_view_str(
        out, "\n      <span class=\"for-screen-reader\">Edit "));
    CF_VIEW_TRY(cf_view_text(out, cf_str_span(bot->name)));
    CF_VIEW_TRY(cf_view_str(out, "</span>\n"));
    CF_VIEW_TRY(cf_view_close_tag(out, "a"));
    CF_VIEW_TRY(cf_view_str(out, "  </div>\n\n"));
    for (size_t i = 0; i < bot->rooms.len; i++) {
        const cf_view_accounts_bot_room *room = &bot->rooms.items[i];
        CF_VIEW_TRY(cf_view_str(
            out,
            "    <fieldset class=\"gap max-width pad border border-radius\">\n"
            "      <legend class=\"min-width txt-align-start pad-inline\">\n"
            "        <strong class=\"overflow-ellipsis\">"));
        CF_VIEW_TRY(cf_view_text(out, cf_str_span(room->name)));
        CF_VIEW_TRY(cf_view_str(out, "</strong>\n      </legend>\n\n"));
        /* `room_bot_messages_url(room, bot.bot_key)`. */
        {
            char room_text[24];
            n = snprintf(room_text, sizeof room_text, "%lld",
                         (long long)room->id);
            if (n < 0 || (size_t)n >= sizeof room_text) {
                rc = CF_INTERNAL;
                goto fail;
            }
            cf_builder path = {0};
            rc = cf_view_str(&path, "/rooms/");
            if (rc == CF_OK) {
                rc = cf_view_raw(&path,
                                 (cf_span){(const unsigned char *)room_text,
                                           strlen(room_text)});
            }
            if (rc == CF_OK) rc = cf_view_str(&path, "/");
            if (rc == CF_OK) {
                rc = cf_view_raw(&path, cf_str_span(bot->bot_key));
            }
            if (rc == CF_OK) rc = cf_view_str(&path, "/messages");
            if (rc == CF_OK) rc = cf_view_ctx_url(ctx, cf_view_span_of(&path), &url);
            cf_builder_dispose(&path);
            if (rc != CF_OK) goto fail;
        }
        {
            cf_builder text_line = {0}, upload_line = {0};
            rc = cf_view_str(&text_line, "curl -d 'Hello!' ");
            if (rc == CF_OK) {
                rc = cf_view_raw(&text_line, cf_view_span_of(&url));
            }
            if (rc == CF_OK) {
                rc = cf_view_str(&upload_line,
                                 "curl -F \"attachment=@/path/to/file\" ");
            }
            if (rc == CF_OK) {
                rc = cf_view_raw(&upload_line, cf_view_span_of(&url));
            }
            if (rc == CF_OK) {
                rc = bots_curl_row(ctx, cf_view_span_of(&text_line),
                                   "messages-outlined.svg",
                                   "curl command for posting messages",
                                   "Copy message command", out);
            }
            if (rc == CF_OK) {
                rc = cf_view_str(out, "\n");
            }
            if (rc == CF_OK) {
                rc = bots_curl_row(ctx, cf_view_span_of(&upload_line),
                                   "attachment.svg",
                                   "curl command for posting attachments",
                                   "Copy attachment command", out);
            }
            cf_builder_dispose(&text_line);
            cf_builder_dispose(&upload_line);
            if (rc != CF_OK) goto fail;
        }
        CF_VIEW_TRY(cf_view_str(out, "    </fieldset>\n"));
        cf_builder_dispose(&url);
        url = (cf_builder){0};
    }
    cf_builder_dispose(&url);
    CF_VIEW_TRY(cf_view_str(out, "</li>\n"));
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&url);
    return cf_view_fail(&guard, rc);
}

/* --------------------------------------------------------- _form partial */

/* `accounts/bots/_form.html` fields, rendered into the form body (the file
 * field marks the FormWith multipart, like first_run.c). */
static cf_err bots_form_fields(const cf_view_ctx *ctx, cf_view_form *form,
                               const cf_view_accounts_bot_form *bot,
                               bool *multipart_out) {
    cf_builder *body = form->out;
    cf_err rc = CF_OK;
    if (body == NULL || bot == NULL || multipart_out == NULL) return CF_INVALID;
    *multipart_out = false;
    CF_VIEW_TRY(cf_view_str(
        body,
        "<h1 class=\"for-screen-reader\">Chat Bot Setup</h1>\n"
        "<label class=\"align-center center avatar__form gap\" "
        "data-controller=\"upload-preview\">\n"
        "  <div class=\"btn input--file\">\n    "));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("camera.svg"), &img,
                                      body));
    }
    CF_VIEW_TRY(cf_view_str(body, "\n    "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "input"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "accept", "image/*"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-upload-preview-target",
                                      "input"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-action",
                                      "upload-preview#previewImage"));
        CF_VIEW_TRY(cf_view_form_file_field(form, "avatar", &attrs,
                                            multipart_out));
    }
    CF_VIEW_TRY(cf_view_str(
        body,
        "\n    <span class=\"for-screen-reader\">Upload bot avatar</span>\n"
        "  </div>\n\n"
        "  <div class=\"avatar input--file txt-xx-large\" "
        "style=\"--avatar-size: var(--btn-size);\">\n    "));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "alt", "Bot avatar"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "data-upload-preview-target",
                                      "image"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "size", "48"));
        cf_span source = bot->has_avatar_url
                             ? cf_str_span(bot->avatar_url)
                             : cf_span_of_lit("default-bot-avatar.svg");
        CF_VIEW_TRY(cf_view_image_tag(ctx, source, &img, body));
    }
    CF_VIEW_TRY(cf_view_str(
        body,
        "\n  </div>\n</label>\n\n"
        "<div class=\"flex align-center gap\">\n  "));
    CF_VIEW_TRY(bots_translation_button(ctx, "bot_name", body));
    CF_VIEW_TRY(cf_view_str(
        body,
        "\n  <label class=\"flex align-center gap flex-item-grow txt-large "
        "input input--actor\">\n    "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "input"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "autocomplete", "name"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "placeholder", "Name the bot"));
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "autofocus", true));
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "required", true));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-1p-ignore", "true"));
        CF_VIEW_TRY(cf_view_form_field(
            form, "text", "name",
            bot->has_name ? cf_str_span(bot->name) : (cf_span){NULL, 0},
            bot->has_name, &attrs));
    }
    CF_VIEW_TRY(cf_view_str(body, "\n    "));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 24));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("bot.svg"), &img,
                                      body));
    }
    CF_VIEW_TRY(cf_view_str(
        body,
        "\n  </label>\n</div>\n\n"
        "<div class=\"flex align-center gap\">\n  "));
    CF_VIEW_TRY(bots_translation_button(ctx, "webhook_url", body));
    CF_VIEW_TRY(cf_view_str(
        body,
        "\n  <label class=\"flex align-center gap flex-item-grow txt-large "
        "input input--actor\">\n    "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "input"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "placeholder", "Webhook URL"));
        CF_VIEW_TRY(cf_view_form_field(
            form, "url", "webhook_url",
            bot->has_webhook_url ? cf_str_span(bot->webhook_url)
                                 : (cf_span){NULL, 0},
            bot->has_webhook_url, &attrs));
    }
    CF_VIEW_TRY(cf_view_str(body, "\n    "));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 24));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("web.svg"), &img,
                                      body));
    }
    CF_VIEW_TRY(cf_view_str(body, "\n  </label>\n</div>\n\n"));
    /* `profile_form_submit_button`: no `name="button"` (cf_view_button
     * would add it; the fixture has class + type only). */
    {
        cf_builder content = {0};
        rc = cf_view_str(&content, "");
        if (rc == CF_OK) {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("check.svg"),
                                          &img, &content));
        }
        if (rc == CF_OK) {
            rc = cf_view_str(
                &content,
                "<span class=\"for-screen-reader\">Save changes</span>");
        }
        if (rc == CF_OK) {
            cf_view_attrs button;
            cf_view_attrs_init(&button);
            CF_VIEW_TRY(cf_view_attr_cstr(&button, "class",
                                          "btn btn--reversed center txt-large"));
            CF_VIEW_TRY(cf_view_attr_cstr(&button, "type", "submit"));
            CF_VIEW_TRY(cf_view_open_start(body, "button", &button));
        }
        if (rc == CF_OK) rc = cf_view_raw(body, cf_view_span_of(&content));
        if (rc == CF_OK) rc = cf_view_close_tag(body, "button");
        cf_builder_dispose(&content);
        if (rc != CF_OK) return rc;
    }
    CF_VIEW_TRY(cf_view_str(body, "\n"));
    return CF_OK;
fail:
    return rc;
}

/* The whole `form_with` around already-rendered fields (a file_field marks
 * the form multipart, as in first_run.c). */
static cf_err bots_form_with(const cf_view_ctx *ctx, cf_span action,
                             const char *method,
                             const cf_view_accounts_bot_form *bot,
                             cf_builder *out) {
    (void)ctx;
    cf_err rc = CF_OK;
    cf_view_guard guard;
    cf_builder body = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_form form;
    cf_view_form_init(&form, &body);
    form.action = action;
    form.param_key = "user";
    form.class_attr = "flex flex-column gap";
    form.method = method;
    bool multipart = false;
    rc = bots_form_fields(ctx, &form, bot, &multipart);
    if (rc != CF_OK) goto fail;
    if (!multipart) {
        rc = CF_INTERNAL;
        goto fail;
    }
    /* The multipart flag is known only now: the tag, the body, the close.
     * The templates put whitespace between the open tag and the fields. */
    form.out = out;
    form.multipart = true;
    CF_VIEW_TRY(cf_view_form_open(&form));
    CF_VIEW_TRY(cf_view_str(out, "\n    "));
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&body)));
    CF_VIEW_TRY(cf_view_str(out, "</form>"));
    cf_builder_dispose(&body);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&body);
    return cf_view_fail(&guard, rc);
}

/* --------------------------------------------------------------- content */

/* `accounts/bots/index.html.erb` content. */
static cf_err bots_index_content(
    const cf_view_ctx *ctx, const cf_view_accounts_bots_index_model *model,
    cf_builder *out) {
    cf_err rc = CF_OK;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(
        out,
        "<section class=\"panel panel--wide txt-align-center flex flex-column "
        "position-relative\" style=\"view-transition-name: chat-bots\">\n"
        "  <div class=\"flex align-center gap\">\n"
        "    <div class=\"panel__button\">\n      "));
    CF_VIEW_TRY(bots_translation_button(ctx, "chat_bots", out));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n    </div>\n"
        "    <div class=\"pad-inline-double center\">\n"
        "      <h1 class=\"margin-none\">Chat bots</h1>\n"
        "      <p class=\"margin-none-block-start\">With Chat bots, other sites "
        "and services can post updates directly to Campfire.</p>\n\n      "));
    {
        cf_builder content = {0};
        rc = cf_view_str(&content, "\n        ");
        if (rc == CF_OK) {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("bot.svg"), &img,
                                          &content));
        }
        if (rc == CF_OK) rc = cf_view_str(&content, "\n        ");
        if (rc == CF_OK) {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("add.svg"), &img,
                                          &content));
        }
        if (rc == CF_OK) rc = cf_view_str(&content, "\n");
        if (rc == CF_OK) {
            cf_view_attrs link;
            cf_view_attrs_init(&link);
            CF_VIEW_TRY(cf_view_attr_cstr(&link, "class",
                                          "btn btn--reversed txt-large"));
            CF_VIEW_TRY(cf_view_attr_cstr(&link, "aria-label", "Add a chat bot"));
            CF_VIEW_TRY(cf_view_attr(&link, "href",
                                     cf_span_of_lit("/account/bots/new")));
            CF_VIEW_TRY(cf_view_content(out, "a", &link,
                                        cf_view_span_of(&content)));
        }
        cf_builder_dispose(&content);
        if (rc != CF_OK) goto fail;
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "    </div>\n"
        "  </div>\n\n"
        "  <div class=\"pad-inline pad-block-start \">\n"
        "    <menu class=\"flex flex-column gap margin-none pad\">\n      "));
    for (size_t i = 0; i < model->bots.len; i++) {
        CF_VIEW_TRY(bots_bot_partial(ctx, &model->bots.items[i], out));
    }
    CF_VIEW_TRY(cf_view_str(out, "    </menu>\n  </div>\n</section>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* `accounts/bots/new.html.erb` content. */
static cf_err bots_new_content(const cf_view_ctx *ctx,
                               const cf_view_accounts_bots_new_model *model,
                               cf_builder *out) {
    cf_err rc = CF_OK;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, "<section class=\"panel\">\n  "));
    CF_VIEW_TRY(bots_form_with(ctx, cf_span_of_lit("/account/bots"), "post",
                               &model->form, out));
    CF_VIEW_TRY(cf_view_str(out, "</section>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* `button_to ... method: :delete` / `:put` rows on the edit page. */
static cf_err bots_edit_buttons(const cf_view_ctx *ctx, int64_t bot_id,
                                cf_builder *out) {
    cf_err rc = CF_OK;
    cf_view_guard guard;
    char id_text[24];
    int n = snprintf(id_text, sizeof id_text, "%lld", (long long)bot_id);
    if (n < 0 || (size_t)n >= sizeof id_text) return CF_INTERNAL;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(
        out,
        "  <hr class=\"separator full-width margin-block-double\">\n\n"
        "  <div class=\"flex align-center gap justify-space-between\">\n    "));
    /* Delete this chat bot. */
    {
        char action[64];
        n = snprintf(action, sizeof action, "/account/bots/%s", id_text);
        if (n < 0 || (size_t)n >= sizeof action) {
            rc = CF_INTERNAL;
            goto fail;
        }
        cf_builder content = {0};
        rc = cf_view_str(&content, "\n      ");
        if (rc == CF_OK) {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("trash.svg"),
                                          &img, &content));
        }
        if (rc == CF_OK) rc = cf_view_str(&content, "\n      ");
        if (rc == CF_OK) {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("bot.svg"), &img,
                                          &content));
        }
        if (rc == CF_OK) rc = cf_view_str(&content, "\n");
        if (rc == CF_OK) {
            cf_view_attrs button;
            cf_view_attrs_init(&button);
            CF_VIEW_TRY(cf_view_attr_cstr(&button, "class",
                                          "btn txt--small btn--negative"));
            CF_VIEW_TRY(cf_view_attr_cstr(&button, "aria-label",
                                          "Delete this chat bot"));
            CF_VIEW_TRY(cf_view_attr_cstr(
                &button, "data-turbo-confirm",
                "Are you sure you want to permanently remove this bot from "
                "the account? This can\xe2\x80\x99t be undone."));
            CF_VIEW_TRY(cf_view_button_to(
                out,
                (cf_span){(const unsigned char *)action, strlen(action)},
                &button, cf_span_of_lit("delete"), (cf_span){NULL, 0},
                cf_view_span_of(&content)));
        }
        cf_builder_dispose(&content);
        if (rc != CF_OK) goto fail;
    }
    CF_VIEW_TRY(cf_view_str(out, "\n    "));
    /* Generate a new key. */
    {
        char action[72];
        n = snprintf(action, sizeof action, "/account/bots/%s/key", id_text);
        if (n < 0 || (size_t)n >= sizeof action) {
            rc = CF_INTERNAL;
            goto fail;
        }
        cf_builder content = {0};
        rc = cf_view_str(&content, "\n      ");
        if (rc == CF_OK) {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("refresh.svg"),
                                          &img, &content));
        }
        if (rc == CF_OK) rc = cf_view_str(&content, "\n      ");
        if (rc == CF_OK) {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("key.svg"), &img,
                                          &content));
        }
        if (rc == CF_OK) rc = cf_view_str(&content, "\n");
        if (rc == CF_OK) {
            cf_view_attrs button;
            cf_view_attrs_init(&button);
            CF_VIEW_TRY(cf_view_attr_cstr(
                &button, "class", "btn full-width txt--small btn--negative"));
            CF_VIEW_TRY(cf_view_attr_cstr(&button, "aria-label",
                                          "Generate a new key"));
            CF_VIEW_TRY(cf_view_attr_cstr(
                &button, "data-turbo-confirm",
                "Are you sure you want to change the bot key? All usage of "
                "this bot must be updated."));
            CF_VIEW_TRY(cf_view_button_to(
                out,
                (cf_span){(const unsigned char *)action, strlen(action)},
                &button, cf_span_of_lit("put"), (cf_span){NULL, 0},
                cf_view_span_of(&content)));
        }
        cf_builder_dispose(&content);
        if (rc != CF_OK) goto fail;
    }
    CF_VIEW_TRY(cf_view_str(out, "  </div>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* `accounts/bots/edit.html.erb` content. */
static cf_err bots_edit_content(const cf_view_ctx *ctx,
                                const cf_view_accounts_bots_edit_model *model,
                                cf_builder *out) {
    cf_err rc = CF_OK;
    cf_view_guard guard;
    char id_text[24];
    int n = snprintf(id_text, sizeof id_text, "%lld",
                     (long long)model->bot_id);
    if (n < 0 || (size_t)n >= sizeof id_text) return CF_INTERNAL;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, "<section class=\"panel\" style=\"view-transition-name: chat-bot-"));
    CF_VIEW_TRY(cf_view_raw(out, (cf_span){(const unsigned char *)id_text,
                                           (size_t)n}));
    CF_VIEW_TRY(cf_view_str(out, "\">\n  "));
    {
        char action[64];
        int m = snprintf(action, sizeof action, "/account/bots/%s", id_text);
        if (m < 0 || (size_t)m >= sizeof action) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(bots_form_with(
            ctx, (cf_span){(const unsigned char *)action, strlen(action)},
            "patch", &model->form, out));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n"));
    CF_VIEW_TRY(bots_edit_buttons(ctx, model->bot_id, out));
    CF_VIEW_TRY(cf_view_str(out, "</section>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* -------------------------------------------------------------- renderers */

cf_err cf_view_accounts_bots_index(
    const cf_view_ctx *ctx, const cf_view_accounts_bots_index_model *model,
    cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder head = {0}, content = {0}, nav = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    /* `{%- block head %}{% endblock %}` — empty. */
    if (rc == CF_OK) rc = bots_nav(ctx, "/account/edit", &nav);
    if (rc == CF_OK) rc = bots_index_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_page(
            ctx, cf_span_of_lit("Chat bots"), true, (cf_span){NULL, 0}, false,
            cf_view_span_of(&head), cf_view_span_of(&content),
            cf_view_span_of(&nav), (cf_span){NULL, 0}, (cf_span){NULL, 0}, out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    cf_builder_dispose(&nav);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_accounts_bots_index_frame(
    const cf_view_ctx *ctx, const cf_view_accounts_bots_index_model *model,
    cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder head = {0}, content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = bots_index_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, cf_view_span_of(&head),
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_accounts_bots_new(
    const cf_view_ctx *ctx, const cf_view_accounts_bots_new_model *model,
    cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder head = {0}, content = {0}, nav = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    if (rc == CF_OK) rc = bots_nav(ctx, "/account/bots", &nav);
    if (rc == CF_OK) rc = bots_new_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_page(
            ctx, cf_span_of_lit("New chat bot"), true, (cf_span){NULL, 0},
            false, cf_view_span_of(&head), cf_view_span_of(&content),
            cf_view_span_of(&nav), (cf_span){NULL, 0}, (cf_span){NULL, 0}, out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    cf_builder_dispose(&nav);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_accounts_bots_new_frame(
    const cf_view_ctx *ctx, const cf_view_accounts_bots_new_model *model,
    cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder head = {0}, content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = bots_new_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, cf_view_span_of(&head),
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_accounts_bots_edit(
    const cf_view_ctx *ctx, const cf_view_accounts_bots_edit_model *model,
    cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder head = {0}, content = {0}, nav = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    if (rc == CF_OK) rc = bots_nav(ctx, "/account/bots", &nav);
    if (rc == CF_OK) rc = bots_edit_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_page(
            ctx, cf_span_of_lit("Edit bot"), true, (cf_span){NULL, 0}, false,
            cf_view_span_of(&head), cf_view_span_of(&content),
            cf_view_span_of(&nav), (cf_span){NULL, 0}, (cf_span){NULL, 0}, out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    cf_builder_dispose(&nav);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_accounts_bots_edit_frame(
    const cf_view_ctx *ctx, const cf_view_accounts_bots_edit_model *model,
    cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder head = {0}, content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = bots_edit_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, cf_view_span_of(&head),
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}
