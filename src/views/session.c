/* src/views/session.c — sessions/new, sessions/incompatible_browser and
 * sessions/transfers/show (tmp/rust-ref/crates/views/templates/sessions/,
 * sessions.rs).
 */
#include "views/internal.h"

/* The head block sessions/new.html defines: turbo_page_requires_reload_tag. */
static cf_err session_new_head(cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_view_attrs attrs;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "name", "turbo-visit-control"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "content", "reload"));
    CF_VIEW_TRY(cf_view_builder_tag(out, "meta", &attrs));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

static cf_err session_new_content(const cf_view_ctx *ctx,
                                  const cf_view_session_new_model *model,
                                  cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    /* Local builders live for the whole render so the fail path can free
     * them (A02: a failed render leaks nothing). */
    cf_builder action = {0}, button = {0}, link = {0}, url = {0};
    cf_builder title = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    CF_VIEW_TRY(cf_view_str(out, "<section class=\"txt-align-center\">\n"
                                 "  <div class=\"panel "));
    if (ctx->has_flash_alert) CF_VIEW_TRY(cf_view_str(out, "shake"));
    CF_VIEW_TRY(cf_view_str(out, "\">\n    "));
    CF_VIEW_TRY(cf_view_account_logo(ctx, "center margin-block-end txt-xx-large",
                                     true, out));
    CF_VIEW_TRY(cf_view_str(out, "\n\n    "));

    /* `form_with(ctx.url(session))`, no model. */
    cf_view_form form;
    cf_view_form_init(&form, out);
    CF_VIEW_TRY(cf_view_raw(&action, ctx->base_url));
    CF_VIEW_TRY(cf_view_str(&action, "/session"));
    form.action = cf_view_span_of(&action);
    form.class_attr = "flex flex-column gap";
    CF_VIEW_TRY(cf_view_form_open(&form));

    CF_VIEW_TRY(cf_view_str(
        out,
        "\n      <fieldset class=\"flex flex-column gap center-block upad\">\n"
        "        <legend class=\"txt-large txt-align-center\"><strong>"));
    CF_VIEW_TRY(cf_view_text(out, cf_str_span(ctx->account.name)));
    CF_VIEW_TRY(cf_view_str(out, "</strong></legend>\n\n"
                                 "        <div class=\"flex align-center gap\">\n"
                                 "          "));
    CF_VIEW_TRY(cf_view_translation_button(ctx, "email_address", out));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n          <label class=\"flex align-center gap input input--actor "
        "txt-large\">\n            "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "required", true));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "input"));
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "autofocus", true));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "autocomplete", "username"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "placeholder",
                                      "Enter your email address"));
        CF_VIEW_TRY(cf_view_form_field(
            &form, "email", "email_address",
            model->has_email_address ? cf_str_span(model->email_address)
                                     : (cf_span){NULL, 0},
            model->has_email_address, &attrs));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n            "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 24));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("email.svg"), &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n          </label>\n        </div>\n\n"
        "        <div class=\"flex align-center gap\">\n          "));
    CF_VIEW_TRY(cf_view_translation_button(ctx, "password", out));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n          <label class=\"flex align-center gap input input--actor "
        "txt-large\">\n            "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "required", true));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "input"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "autocomplete",
                                      "current-password"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "placeholder",
                                      "Enter your password"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "maxlength", 72));
        CF_VIEW_TRY(cf_view_form_password_field(&form, "password", &attrs));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n            "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 24));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("password.svg"), &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n          </label>\n        </div>\n\n"
                                 "        "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        cf_view_attrs inner;
        cf_view_attrs_init(&inner);
        CF_VIEW_TRY(cf_view_str(&button, "\n          "));
        CF_VIEW_TRY(cf_view_attr_cstr(&inner, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("arrow-right.svg"),
                                      &inner, &button));
        CF_VIEW_TRY(
            cf_view_str(&button, "\n          <span class=\"for-screen-reader\">"
                                 "Go</span>\n"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class",
                                      "btn btn--reversed center txt-large"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "type", "submit"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "name", "log_in"));
        CF_VIEW_TRY(cf_view_button(out, &attrs, cf_view_span_of(&button)));
    }
    CF_VIEW_TRY(cf_view_str(out, "      </fieldset>\n</form>  </div>\n\n  "));
    if (model->has_help_contact && model->help_contact != NULL) {
        const cf_view_help_contact *owner = model->help_contact;
        CF_VIEW_TRY(cf_view_str(
            out, "<div class=\"txt-align-center margin-block-double "
                 "full-width\">\n    "));
        {
            CF_VIEW_TRY(cf_view_str(&url, "mailto:\""));
            CF_VIEW_TRY(cf_view_raw(&url, cf_str_span(owner->name)));
            CF_VIEW_TRY(cf_view_str(&url, "\" <"));
            CF_VIEW_TRY(cf_view_raw(&url, cf_str_span(owner->email_address)));
            CF_VIEW_TRY(cf_view_str(&url, ">"));
            CF_VIEW_TRY(cf_view_str(&link, "\n      "));
            cf_view_attrs inner;
            cf_view_attrs_init(&inner);
            CF_VIEW_TRY(cf_view_attr_cstr(&inner, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("lifebuoy.svg"),
                                          &inner, &link));
            CF_VIEW_TRY(cf_view_str(&link, "\n      <span>"));
            CF_VIEW_TRY(cf_view_text(&link, cf_str_span(owner->email_address)));
            CF_VIEW_TRY(cf_view_str(&link, "</span>\n"));

            cf_view_attrs attrs;
            cf_view_attrs_init(&attrs);
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "btn center"));
            CF_VIEW_TRY(cf_view_str(&title, "Email "));
            CF_VIEW_TRY(cf_view_raw(&title, cf_str_span(owner->name)));
            CF_VIEW_TRY(cf_view_attr(&attrs, "title",
                                     cf_view_span_of(&title)));
            CF_VIEW_TRY(cf_view_attr(&attrs, "href", cf_view_span_of(&url)));
            CF_VIEW_TRY(cf_view_content(out, "a", &attrs,
                                        cf_view_span_of(&link)));
        }
        CF_VIEW_TRY(cf_view_str(
            out,
            "\n    <div class=\"txt-align-center center margin-block "
            "txt-subtle\">Campfire&trade; version "));
        CF_VIEW_TRY(cf_view_version_badge(ctx, out));
        CF_VIEW_TRY(cf_view_str(out, "</div>\n  </div>\n"));
    }
    CF_VIEW_TRY(cf_view_str(out, "</section>\n"));
    cf_builder_dispose(&action);
    cf_builder_dispose(&button);
    cf_builder_dispose(&link);
    cf_builder_dispose(&url);
    cf_builder_dispose(&title);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&action);
    cf_builder_dispose(&button);
    cf_builder_dispose(&link);
    cf_builder_dispose(&url);
    cf_builder_dispose(&title);
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_session_new(const cf_view_ctx *ctx,
                           const cf_view_session_new_model *model,
                           cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_view_guard guard;
    cf_err rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_builder head = {0}, content = {0};
    rc = session_new_head(&head);
    if (rc == CF_OK) rc = session_new_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_page(ctx, cf_span_of_lit("Sign in"), true,
                                 (cf_span){NULL, 0}, false,
                                 cf_view_span_of(&head),
                                 cf_view_span_of(&content), (cf_span){NULL, 0},
                                 (cf_span){NULL, 0}, (cf_span){NULL, 0}, out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_session_new_frame(const cf_view_ctx *ctx,
                                 const cf_view_session_new_model *model,
                                 cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_view_guard guard;
    cf_err rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_builder head = {0}, content = {0};
    rc = session_new_head(&head);
    if (rc == CF_OK) rc = session_new_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, cf_view_span_of(&head),
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

/* ------------------------------------------------- incompatible browser */

static cf_err incompatible_content(const cf_view_ctx *ctx, cf_builder *out) {
    static const struct {
        const char *browser;
        const char *version;
    } ALLOWED[] = {
        {"safari", "17.2"}, {"chrome", "120"}, {"firefox", "121"},
        {"opera", "104"},
    };
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    CF_VIEW_TRY(cf_view_str(
        out,
        "<div class=\"panel center\">\n  <header>\n"
        "    <h1 class=\"txt-x-large txt-tight-lines txt-align-center "
        "margin-none-block-start margin-block-end\">\n"
        "      Upgrade to a supported web browser\n    </h1>\n"
        "    <div class=\"flex align-start gap\">\n      "));
    CF_VIEW_TRY(
        cf_view_translation_button(ctx, "incompatible_browser_messsage", out));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n      <p class=\"margin-none-block-start\">Campfire requires a "
        "modern web browser. Please use one of the browsers listed below and "
        "make sure auto-updates are enabled.</p>\n"
        "    </div>\n  </header>\n\n"
        "  <div class=\"browser-list flex align-center flex-wrap gap "
        "justify-center margin-block\">\n"));
    for (size_t i = 0; i < sizeof ALLOWED / sizeof ALLOWED[0]; i++) {
        CF_VIEW_TRY(cf_view_str(out, "      <div class=\"browser flex "
                                      "flex-column\">\n        "));
        {
            cf_builder logical = {0};
            cf_view_attrs attrs;
            cf_view_attrs_init(&attrs);
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "center"));
            CF_VIEW_TRY(cf_view_str(&logical, "browsers/"));
            CF_VIEW_TRY(cf_view_str(&logical, ALLOWED[i].browser));
            CF_VIEW_TRY(cf_view_str(&logical, ".svg"));
            rc = cf_view_image_tag(ctx, cf_view_span_of(&logical), &attrs, out);
            cf_builder_dispose(&logical);
            if (rc != CF_OK) goto fail;
        }
        CF_VIEW_TRY(cf_view_str(
            out,
            "\n        <div class=\"flex flex-column align-center "
            "margin-block-start-half\">\n          <strong>"));
        {
            /* capitalize(browser) */
            char name[16];
            size_t n = strlen(ALLOWED[i].browser);
            for (size_t k = 0; k < n && k < sizeof name - 1; k++) {
                char c = ALLOWED[i].browser[k];
                if (k == 0 && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                name[k] = c;
            }
            name[n < sizeof name ? n : sizeof name - 1] = '\0';
            CF_VIEW_TRY(cf_view_text_cstr(out, name));
        }
        CF_VIEW_TRY(cf_view_str(out, "</strong>\n          <span> "));
        CF_VIEW_TRY(cf_view_text_cstr(out, ALLOWED[i].version));
        CF_VIEW_TRY(cf_view_str(out, "+</span>\n        </div>\n      </div>\n"));
    }
    CF_VIEW_TRY(cf_view_str(out, "  </div>\n</div>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

static cf_err incompatible_title(const cf_view_ctx *ctx, cf_builder *out) {
    if (ctx==NULL||out==NULL) return CF_INVALID;
    return cf_view_str(out, ctx->platform.apple_messages ? "Campfire"
                                                         : "Unsupported browser");
}

cf_err cf_view_session_incompatible(const cf_view_ctx *ctx, cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    cf_view_guard guard;
    cf_err rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_builder title = {0}, content = {0};
    rc = incompatible_title(ctx, &title);
    if (rc == CF_OK) rc = incompatible_content(ctx, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_page(ctx, cf_view_span_of(&title), true,
                                 (cf_span){NULL, 0}, false,
                                 (cf_span){NULL, 0}, cf_view_span_of(&content),
                                 (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                 (cf_span){NULL, 0}, out);
    }
    cf_builder_dispose(&title);
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_session_incompatible_frame(const cf_view_ctx *ctx,
                                          cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    cf_view_guard guard;
    cf_err rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_builder content = {0};
    rc = incompatible_content(ctx, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, (cf_span){NULL, 0},
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

/* -------------------------------------------------------- transfer show */

static cf_err transfer_content(const cf_view_ctx *ctx,
                               const cf_view_session_transfer_model *model,
                               cf_builder *out) {
    (void)ctx;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_form form;
    cf_view_form_init(&form, out);
    form.action = model->action;
    form.method = "put";
    cf_view_attrs_init(&form.data);
    form.has_data = true;
    CF_VIEW_TRY(cf_view_attr_cstr(&form.data, "data-controller", "auto-submit"));
    CF_VIEW_TRY(cf_view_form_open(&form));
    CF_VIEW_TRY(cf_view_str(out, "\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_session_transfer(const cf_view_ctx *ctx,
                                const cf_view_session_transfer_model *model,
                                cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_view_guard guard;
    cf_err rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_builder content = {0};
    rc = transfer_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_page(ctx, (cf_span){NULL, 0}, false,
                                 (cf_span){NULL, 0}, false, (cf_span){NULL, 0},
                                 cf_view_span_of(&content), (cf_span){NULL, 0},
                                 (cf_span){NULL, 0}, (cf_span){NULL, 0}, out);
    }
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_session_transfer_frame(const cf_view_ctx *ctx,
                                      const cf_view_session_transfer_model *model,
                                      cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_view_guard guard;
    cf_err rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_builder content = {0};
    rc = transfer_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, (cf_span){NULL, 0},
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}
