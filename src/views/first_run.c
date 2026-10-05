/* src/views/first_run.c — first_runs/show.html.erb: account setup, shown
 * until the first user exists (tmp/rust-ref/crates/views/templates/
 * first_runs/show.html, first_runs.rs).
 */
#include "views/internal.h"

static cf_err first_run_content(const cf_view_ctx *ctx, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    /* `form_with ... do |form|`: the fields render first (into `body`) so a
     * file_field can turn the form multipart, then the tag wraps the body.
     * Both builders live for the whole render so every exit path frees
     * them (the button content is built in its own builder first). */
    cf_builder body = {0};
    cf_builder button = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_form form;
    cf_view_form_init(&form, &body);
    form.action = cf_span_of_lit("/first_run");
    form.param_key = "user";
    form.class_attr = "center max-width";

    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n  <section class=\"nametag u-relative\">\n"
        "    <div class=\"flex justify-center align-center pad-block\">\n      "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "nametag__lanyard"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("lanyard.svg"), &attrs, &body));
    }
    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n    </div>\n\n"
        "    <div class=\"nametag__inner flex flex-column gap\">\n"
        "      <fieldset class=\"flex flex-column center-block\">\n"
        "        <legend class=\"txt-large txt-align-center\"><strong>Set up "
        "Campfire</strong></legend>\n\n"
        "        <label class=\"align-center center avatar__form gap\" "
        "data-controller=\"upload-preview\">\n"
        "          <div class=\"btn input--file\">\n            "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("camera.svg"), &attrs, &body));
    }
    CF_VIEW_TRY(cf_view_str(&body, "\n            "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "input"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "accept", "image/*"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-upload-preview-target",
                                      "input"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-action",
                                      "upload-preview#previewImage"));
        bool multipart = false;
        CF_VIEW_TRY(cf_view_form_file_field(&form, "avatar", &attrs,
                                            &multipart));
        if (!multipart) {
            rc = CF_INTERNAL;
            goto fail;
        }
    }
    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n            <span class=\"for-screen-reader\">Add your "
        "avatar</span>\n"
        "          </div>\n\n"
        "          <div class=\"btn avatar input--file txt-xx-large\">\n"
        "            "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-upload-preview-target",
                                      "image"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "alt", "Add your avatar"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("default-avatar.svg"), &attrs, &body));
    }
    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n            <span class=\"for-screen-reader\">Avatar</span>\n"
        "          </div>\n"
        "        </label>\n"
        "      </fieldset>\n\n"
        "      <div class=\"flex align-center gap\">\n        "));
    CF_VIEW_TRY(cf_view_translation_button(ctx, "user_name", &body));
    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n        <label class=\"flex align-center gap flex-item-grow "
        "txt-large input input--actor\">\n          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "input"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "autocomplete", "name"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "placeholder", "Name"));
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "autofocus", true));
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "required", true));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-1p-ignore", "true"));
        CF_VIEW_TRY(cf_view_form_field(&form, "text", "name",
                                       (cf_span){NULL, 0}, false, &attrs));
    }
    CF_VIEW_TRY(cf_view_str(&body, "\n          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 24));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("person.svg"), &attrs, &body));
    }
    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n        </label>\n      </div>\n\n"
        "      <div class=\"flex align-center gap\">\n        "));
    CF_VIEW_TRY(cf_view_translation_button(ctx, "email_address", &body));
    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n        <label class=\"flex align-center gap flex-item-grow "
        "txt-large input input--actor\">\n          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "input"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "autocomplete", "username"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "placeholder", "Email address"));
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "required", true));
        CF_VIEW_TRY(cf_view_form_field(&form, "email", "email_address",
                                       (cf_span){NULL, 0}, false, &attrs));
    }
    CF_VIEW_TRY(cf_view_str(&body, "\n          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 24));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("email.svg"), &attrs, &body));
    }
    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n        </label>\n      </div>\n\n"
        "      <div class=\"flex align-center gap\">\n        "));
    CF_VIEW_TRY(cf_view_translation_button(ctx, "password", &body));
    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n        <label class=\"flex align-center gap flex-item-grow "
        "txt-large input input--actor\">\n          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "input"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "autocomplete", "new-password"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "placeholder", "Password"));
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "required", true));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "maxlength", 72));
        CF_VIEW_TRY(cf_view_form_password_field(&form, "password", &attrs));
    }
    CF_VIEW_TRY(cf_view_str(&body, "\n          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 24));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("password.svg"), &attrs, &body));
    }
    CF_VIEW_TRY(cf_view_str(&body,
                            "\n        </label>\n      </div>\n\n      "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        cf_view_attrs inner;
        cf_view_attrs_init(&inner);
        CF_VIEW_TRY(cf_view_str(&button, "\n        "));
        CF_VIEW_TRY(cf_view_attr_cstr(&inner, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("arrow-right.svg"),
                                      &inner, &button));
        CF_VIEW_TRY(cf_view_str(
            &button,
            "\n        <span class=\"for-screen-reader\">Save</span>\n"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class",
                                      "btn btn--reversed center txt-large"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "type", "submit"));
        CF_VIEW_TRY(cf_view_button(&body, &attrs, cf_view_span_of(&button)));
    }
    CF_VIEW_TRY(cf_view_str(&body, "    </div>\n  </section>\n"));

    /* The multipart flag is known only now: render the form tag, the body,
     * then the closing tag (form_with's block behavior). */
    form.out = out;
    form.multipart = true;
    CF_VIEW_TRY(cf_view_form_open(&form));
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&body)));
    CF_VIEW_TRY(cf_view_str(out, "</form>"));
    cf_builder_dispose(&body);
    cf_builder_dispose(&button);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&body);
    cf_builder_dispose(&button);
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_first_run(const cf_view_ctx *ctx, cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    cf_view_guard guard;
    cf_err rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_builder content = {0};
    rc = first_run_content(ctx, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_page(ctx, cf_span_of_lit("Set up Campfire"), true,
                                 cf_span_of_lit("signup"), true,
                                 (cf_span){NULL, 0}, cf_view_span_of(&content),
                                 (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                 (cf_span){NULL, 0}, out);
    }
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_first_run_frame(const cf_view_ctx *ctx, cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    cf_view_guard guard;
    cf_err rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_builder content = {0};
    rc = first_run_content(ctx, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, (cf_span){NULL, 0},
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}
