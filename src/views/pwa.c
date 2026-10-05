/* src/views/pwa.c — the pwa notification-help partials the room bell embeds
 * (tmp/rust-ref/crates/views/templates/pwa/{_browser_settings,
 * _system_settings,_install_instructions}.html and pwa.rs).
 *
 * The rooms#show nav renders rooms/involvements/_bell, whose dialog includes
 * these three partials; their branches key off the platform facts (A01's UA
 * parser).  A zeroed platform renders the generic branches, exactly as the
 * reference's Platform::default does.
 */
#include "views/internal.h"

/* An `img` with the attribute sets the templates use: `aria_hidden()` first
 * or `alt` first, then size, then an optional class; image_tag appends
 * src/width/height. */
typedef struct {
    bool aria_hidden;
    const char *alt;   /* NULL for none */
    int64_t size;
    const char *class_attr; /* NULL for none */
} pwa_img;

static cf_err pwa_image(const cf_view_ctx *ctx, const char *logical,
                        const pwa_img *spec, cf_builder *out) {
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    cf_err rc = CF_OK;
    if (spec->aria_hidden) {
        rc = cf_view_attr_cstr(&attrs, "aria-hidden", "true");
    } else if (spec->alt != NULL) {
        rc = cf_view_attr_cstr(&attrs, "alt", spec->alt);
    }
    if (rc == CF_OK) rc = cf_view_attr_i64(&attrs, "size", spec->size);
    if (rc == CF_OK && spec->class_attr != NULL) {
        rc = cf_view_attr_cstr(&attrs, "class", spec->class_attr);
    }
    if (rc != CF_OK) return rc;
    return cf_view_image_tag(ctx, cf_span_of_lit(logical), &attrs, out);
}

static pwa_img hidden_img(int64_t size) {
    return (pwa_img){.aria_hidden = true, .size = size};
}

static pwa_img alt_img(const char *alt, int64_t size) {
    return (pwa_img){.alt = alt, .size = size};
}

/* `<summary class="btn">` with the icon, heading text and disclosure. */
static cf_err settings_summary(const cf_view_ctx *ctx, const char *icon,
                               cf_span text, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, "    <summary class=\"btn\">\n      "));
    {
        pwa_img spec = hidden_img(20);
        CF_VIEW_TRY(pwa_image(ctx, icon, &spec, out));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n      <strong>"));
    CF_VIEW_TRY(cf_view_text(out, text));
    CF_VIEW_TRY(cf_view_str(out, "</strong>\n      "));
    {
        pwa_img spec = {.aria_hidden = true, .size = 10,
                        .class_attr = "disclosure"};
        CF_VIEW_TRY(pwa_image(ctx, "disclosure.svg", &spec, out));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n    </summary>\n\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* `Check your <name> settings`. */
static cf_err settings_summary_for(const cf_view_ctx *ctx, const char *icon,
                                   cf_span name, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder text = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = cf_view_str(&text, "Check your ");
    if (rc == CF_OK) rc = cf_view_text(&text, name);
    if (rc == CF_OK) rc = cf_view_str(&text, " settings");
    if (rc == CF_OK) {
        rc = settings_summary(ctx, icon, cf_view_span_of(&text), out);
    }
    cf_builder_dispose(&text);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

/* `... <em>{{ image }}</em> ...` inside an li: `before`, the image and
 * `after` joined with no extra whitespace between (the templates' inline
 * form). */
static cf_err li_image(const cf_view_ctx *ctx, const char *indent,
                       const char *before, const char *logical,
                       const pwa_img *spec, const char *after,
                       cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, indent));
    CF_VIEW_TRY(cf_view_str(out, "<li>"));
    CF_VIEW_TRY(cf_view_str(out, before));
    CF_VIEW_TRY(cf_view_str(out, "<em>"));
    CF_VIEW_TRY(pwa_image(ctx, logical, spec, out));
    CF_VIEW_TRY(cf_view_str(out, "</em>"));
    CF_VIEW_TRY(cf_view_str(out, after));
    CF_VIEW_TRY(cf_view_str(out, "</li>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* The apple menu glyph: `<em aria-label="the Apple menu"></em>`. */
static cf_err apple_menu(const char *before, const char *after,
                         cf_builder *out) {
    cf_err rc = cf_view_str(out, before);
    if (rc != CF_OK) return rc;
    rc = cf_view_str(out, "<em aria-label=\"the Apple menu\">");
    if (rc != CF_OK) return rc;
    rc = cf_view_str(out, "\xEF\xA3\xBF");
    if (rc != CF_OK) return rc;
    rc = cf_view_str(out, "</em>");
    if (rc != CF_OK) return rc;
    return cf_view_str(out, after);
}

/* The `<ol>` items shared by the desktop browser branches' system section
 * (indent 12). */
static cf_err browser_system_items(const cf_view_ctx *ctx, cf_span browser,
                                   cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    if (ctx->platform.windows) {
        CF_VIEW_TRY(cf_view_str(
            out,
            "            <li>Click <em>Start</em>, then "
            "<em>Settings</em>.</li>\n"
            "            <li>Go to <em>System &gt; Notification</em>.</li>\n"
            "            <li>Click "));
        {
            pwa_img spec = alt_img("the switch", 22);
            CF_VIEW_TRY(pwa_image(ctx, "external/switch.svg", &spec, out));
        }
        CF_VIEW_TRY(cf_view_str(out, " <em>ON</em> for "));
        CF_VIEW_TRY(cf_view_text(out, browser));
        CF_VIEW_TRY(cf_view_str(out, ".</li>\n"));
    } else {
        CF_VIEW_TRY(apple_menu(
            "            <li>Click ", " in the top left.</li>\n", out));
        CF_VIEW_TRY(cf_view_str(
            out, "            <li>Click <em>System Settings\xE2\x80\xA6"
                 "</em>.</li>\n"
                 "            <li>Click <em>Notifications</em>.</li>\n"
                 "            <li>Click <em>"));
        CF_VIEW_TRY(cf_view_text(out, browser));
        CF_VIEW_TRY(cf_view_str(out, "</em>.</li>\n            <li>Click <em>"));
        {
            pwa_img spec = alt_img("the switch", 22);
            CF_VIEW_TRY(pwa_image(ctx, "external/switch.svg", &spec, out));
        }
        CF_VIEW_TRY(cf_view_str(
            out, "</em> to <em>Allow notifications</em>.</li>\n"));
    }
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_pwa_browser_settings(const cf_view_ctx *ctx, cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    if ((ctx->platform.safari || ctx->platform.chrome) && ctx->platform.ios) {
        return CF_OK; /* The whole partial is skipped on iOS Safari/Chrome. */
    }
    cf_err rc;
    cf_view_guard guard;
    cf_builder browser = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = cf_view_capitalize(ctx->platform.browser, &browser);
    if (rc != CF_OK) goto fail;
    cf_span name = cf_view_span_of(&browser);

    CF_VIEW_TRY(cf_view_str(
        out,
        "  <details class=\"notifications-help\" "
        "data-notifications-target=\"details\">\n"));
    CF_VIEW_TRY(settings_summary_for(ctx, "external/web.svg", name, out));
    if (ctx->platform.firefox && ctx->platform.android) {
        CF_VIEW_TRY(cf_view_str(out, "        <ol>\n"));
        {
            pwa_img spec = alt_img("the View site information button", 20);
            CF_VIEW_TRY(li_image(ctx, "          ", "Tap ", "lock.svg", &spec,
                                 " in the address bar.", out));
        }
        CF_VIEW_TRY(cf_view_str(
            out,
            "          <li>Tap <em>Notification</em> to change to "
            "<em>Allowed</em>.</li>\n"
            "        </ol>\n"));
    } else if (ctx->platform.edge && ctx->platform.desktop) {
        CF_VIEW_TRY(cf_view_str(
            out,
            "        <h2 class=\"txt-normal txt-medium "
            "margin-block-start\">Turn on notifications for this "
            "website.</h2>\n        <ol>\n"));
        {
            pwa_img spec = alt_img("the View site information button", 20);
            CF_VIEW_TRY(li_image(ctx, "          ", "Click the ", "lock.svg",
                                 &spec, " left of the address bar.", out));
        }
        CF_VIEW_TRY(cf_view_str(
            out,
            "          <li>Under <em>Permissions for this site &gt; "
            "Notifications</em>, choose <em>Allow</em>.</li>\n"
            "        </ol>\n"
            "        <h2 class=\"txt-normal txt-medium "
            "margin-block-start\">Turn on notifications for "));
        CF_VIEW_TRY(cf_view_text(out, name));
        CF_VIEW_TRY(cf_view_str(out, ".</h2>\n        <ol>\n"));
        CF_VIEW_TRY(browser_system_items(ctx, name, out));
        CF_VIEW_TRY(cf_view_str(out, "        </ol>\n"));
    } else if (ctx->platform.firefox && ctx->platform.desktop) {
        CF_VIEW_TRY(cf_view_str(
            out,
            "        <h2 class=\"txt-normal txt-medium "
            "margin-block-start\">Turn on notifications for this "
            "website.</h2>\n        <ol>\n          <li>Click <em>"));
        CF_VIEW_TRY(cf_view_text(out, name));
        CF_VIEW_TRY(cf_view_str(
            out,
            "</em> in the top left.</li>\n"
            "          <li>Click <em>Settings\xE2\x80\xA6</em>.</li>\n"
            "          <li>Click <em>Privacy & Security</em> in the "
            "sidebar.</li>\n"
            "          <li>Scroll down to <em>Permissions</em>.</li>\n"
            "          <li>Click <em>Settings</em> next to "
            "<em>Notifications</em>.</li>\n"
            "          <li>Select <em>Allow</em> next to <em>"));
        CF_VIEW_TRY(cf_view_ctx_url(ctx, cf_span_of_lit("/"), out));
        CF_VIEW_TRY(cf_view_str(
            out,
            "</em>.</li>\n"
            "        </ol>\n\n"
            "        <h2 class=\"txt-normal txt-medium "
            "margin-block-start\">Turn on notifications for "));
        CF_VIEW_TRY(cf_view_text(out, name));
        CF_VIEW_TRY(cf_view_str(out, ".</h2>\n        <ol>\n"));
        CF_VIEW_TRY(browser_system_items(ctx, name, out));
        CF_VIEW_TRY(cf_view_str(out, "        </ol>\n"));
    } else if (ctx->platform.chrome && ctx->platform.desktop) {
        CF_VIEW_TRY(cf_view_str(
            out,
            "        <h2 class=\"txt-normal txt-medium "
            "margin-block-start\">Turn on notifications for this "
            "website.</h2>\n        <ol>\n"));
        {
            pwa_img spec = alt_img("View site information", 20);
            CF_VIEW_TRY(li_image(ctx, "          ", "Click the ",
                                 "external/sliders.svg", &spec,
                                 " icon in the address bar.", out));
        }
        CF_VIEW_TRY(cf_view_str(
            out,
            "          <li>Click <em>Site Settings</em>.</li>\n"
            "          <li>Ensure notifications are <em>Allowed</em>.</li>\n"
            "        </ol>\n\n"
            "        <h2 class=\"txt-normal txt-medium "
            "margin-block-start\">Turn on notifications for "));
        CF_VIEW_TRY(cf_view_text(out, name));
        CF_VIEW_TRY(cf_view_str(out, ".</h2>\n        <ol>\n"));
        CF_VIEW_TRY(browser_system_items(ctx, name, out));
        CF_VIEW_TRY(cf_view_str(out, "        </ol>\n"));
    } else if (ctx->platform.chrome && ctx->platform.android) {
        CF_VIEW_TRY(cf_view_str(out, "        <ol>\n"));
        {
            pwa_img spec = alt_img("More options", 16);
            CF_VIEW_TRY(li_image(ctx, "          ", "Tap the ",
                                 "menu-dots-vertical.svg", &spec,
                                 " menu button.", out));
        }
        CF_VIEW_TRY(cf_view_str(
            out,
            "          <li>Tap <em>Settings</em>.</li>\n"
            "          <li>Tap <em>Notifications</em>.</li>\n"));
        CF_VIEW_TRY(cf_view_str(out, "          <li>Tap "));
        {
            pwa_img spec = alt_img("the switch", 22);
            CF_VIEW_TRY(pwa_image(ctx, "external/switch.svg", &spec, out));
        }
        CF_VIEW_TRY(cf_view_str(out, " to <em>Allow "));
        CF_VIEW_TRY(cf_view_text(out, name));
        CF_VIEW_TRY(cf_view_str(
            out,
            " notifications</em>.</li>\n"
            "          <li>Tap "));
        {
            pwa_img spec = alt_img("the switch", 22);
            CF_VIEW_TRY(pwa_image(ctx, "external/switch.svg", &spec, out));
        }
        CF_VIEW_TRY(cf_view_str(
            out,
            " next to <em>Web apps</em>.</li>\n"
            "          <li>Tap "));
        {
            pwa_img spec = alt_img("the notification bell", 16);
            CF_VIEW_TRY(pwa_image(ctx, "notification-bell-alert.svg", &spec,
                                  out));
        }
        CF_VIEW_TRY(cf_view_str(
            out,
            " and select <em>Allow</em>.</li>\n"
            "        </ol>\n"));
    } else if (ctx->platform.safari && ctx->platform.desktop) {
        CF_VIEW_TRY(cf_view_str(out, "        <ol>\n          <li>Click <em>"));
        CF_VIEW_TRY(cf_view_text(out, name));
        CF_VIEW_TRY(cf_view_str(
            out,
            "</em> in the top left.</li>\n"
            "          <li>Click <em>Settings\xE2\x80\xA6</em>.</li>\n"
            "          <li>Click the <em>Websites</em> tab.</li>\n"
            "          <li>Click <em>Notifications</em> in the sidebar.</li>\n"
            "          <li>Click <em>"));
        CF_VIEW_TRY(cf_view_ctx_url(ctx, cf_span_of_lit("/"), out));
        CF_VIEW_TRY(cf_view_str(
            out,
            "</em> in the list.</li>\n"
            "          <li>Select <em>Allow</em>.</li>\n"
            "        </ol>\n"));
    } else {
        CF_VIEW_TRY(cf_view_str(
            out,
            "        <p>Ensure notifications are enabled for <em>"));
        CF_VIEW_TRY(cf_view_ctx_url(ctx, cf_span_of_lit("/"), out));
        CF_VIEW_TRY(cf_view_str(
            out,
            "</em> in your web browser settings.</p>\n"));
    }
    CF_VIEW_TRY(cf_view_str(out, "  </details>\n"));
    cf_builder_dispose(&browser);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&browser);
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_pwa_system_settings(const cf_view_ctx *ctx, cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(
        out,
        "<details class=\"notifications-help hide-in-browser\" "
        "data-notifications-target=\"details\">\n"));
    CF_VIEW_TRY(settings_summary_for(ctx, "external/gear.svg",
                                     ctx->platform.operating_system, out));
    if (ctx->platform.firefox && ctx->platform.android) {
        CF_VIEW_TRY(cf_view_str(out, "      <ol>\n"));
        {
            pwa_img spec = alt_img("More options", 16);
            CF_VIEW_TRY(li_image(ctx, "        ", "Tap the ",
                                 "menu-dots-vertical.svg", &spec,
                                 " menu button.", out));
        }
        CF_VIEW_TRY(cf_view_str(out, "        <li>Tap <em>Settings</em>.</li>\n"
                                    "        <li>Tap <em>Notifications</em>."
                                    "</li>\n"));
        CF_VIEW_TRY(cf_view_str(out, "        <li>Tap "));
        {
            pwa_img spec = alt_img("the toggle button", 22);
            CF_VIEW_TRY(pwa_image(ctx, "external/switch.svg", &spec, out));
        }
        CF_VIEW_TRY(cf_view_str(out, " to <em>Allow "));
        CF_VIEW_TRY(cf_view_capitalize(ctx->platform.browser, out));
        CF_VIEW_TRY(cf_view_str(out, " notifications</em>.</li>\n"
                                    "      </ol>\n"));
    } else if (ctx->platform.edge && ctx->platform.desktop) {
        CF_VIEW_TRY(cf_view_str(
            out,
            "      <ol>\n"
            "        <li>Click <em>Start</em>, then <em>Settings</em>.</li>\n"
            "        <li>Go to <em>System &gt; Notification</em>.</li>\n"
            "        <li>Click "));
        {
            pwa_img spec = alt_img("the toggle button", 22);
            CF_VIEW_TRY(pwa_image(ctx, "external/switch.svg", &spec, out));
        }
        CF_VIEW_TRY(cf_view_str(
            out, " <em>ON</em> for Campfire.</li>\n      </ol>\n"));
    } else if ((ctx->platform.firefox || ctx->platform.chrome) &&
               ctx->platform.desktop) {
        CF_VIEW_TRY(cf_view_str(out, "      <ol>\n"));
        if (ctx->platform.windows) {
            CF_VIEW_TRY(cf_view_str(
                out,
                "          <li>Click <em>Start</em>, then "
                "<em>Settings</em>.</li>\n"
                "          <li>Go to <em>System &gt; Notification</em>."
                "</li>\n"
                "          <li>Click "));
            {
                pwa_img spec = alt_img("the toggle button", 22);
                CF_VIEW_TRY(pwa_image(ctx, "external/switch.svg", &spec, out));
            }
            CF_VIEW_TRY(cf_view_str(
                out, " <em>ON</em> for Campfire.</li>\n"));
        } else {
            CF_VIEW_TRY(apple_menu(
                "          <li>Click ", " in the top left.</li>\n", out));
            CF_VIEW_TRY(cf_view_str(
                out,
                "          <li>Click <em>System Settings\xE2\x80\xA6</em>."
                "</li>\n"
                "          <li>Click <em>Notifications</em>.</li>\n"
                "          <li>Click <em>Campfire</em>.</li>\n"
                "          <li>Click <em>"));
            {
                pwa_img spec = alt_img("the allow notifications switch", 22);
                CF_VIEW_TRY(pwa_image(ctx, "external/switch.svg", &spec, out));
            }
            CF_VIEW_TRY(cf_view_str(
                out,
                "</em> to <em>Allow notifications</em>.</li>\n"));
        }
        CF_VIEW_TRY(cf_view_str(out, "      </ol>\n"));
    } else if (ctx->platform.safari && ctx->platform.desktop) {
        CF_VIEW_TRY(cf_view_str(
            out,
            "      <ol>\n"
            "        <li>Click <em aria-label=\"the Apple menu\">\xEF\xA3\xBF"
            "</em> in the top left.</li>\n"
            "        <li>Click <em>System Settings\xE2\x80\xA6</em>.</li>\n"
            "        <li>Click <em>Notifications</em>.</li>\n"
            "        <li>Click <em>Campfire</em>.</li>\n"
            "        <li>Click <em>"));
        {
            pwa_img spec = alt_img("the allow notifications switch", 22);
            CF_VIEW_TRY(pwa_image(ctx, "external/switch.svg", &spec, out));
        }
        CF_VIEW_TRY(cf_view_str(
            out, "</em> to <em>Allow notifications</em>.</li>\n      </ol>\n"));
    } else if ((ctx->platform.safari || ctx->platform.chrome) &&
               ctx->platform.ios) {
        CF_VIEW_TRY(cf_view_str(
            out,
            "      <ol>\n"
            "        <li>Open the <em>"));
        {
            pwa_img spec = hidden_img(20);
            CF_VIEW_TRY(pwa_image(ctx, "external/gear.svg", &spec, out));
        }
        CF_VIEW_TRY(cf_view_str(
            out,
            "</em> Settings app.</li>\n"
            "        <li>Scroll to and tap <em>Campfire</em>.</li>\n"
            "        <li>Tap <em>Notifications</em>.</li>\n"
            "        <li>Tap "));
        {
            pwa_img spec = alt_img("the allow notifications switch button", 22);
            CF_VIEW_TRY(pwa_image(ctx, "external/switch.svg", &spec, out));
        }
        CF_VIEW_TRY(cf_view_str(
            out,
            " to <em>Allow Notifications</em>.</li>\n"
            "      </ol>\n"));
    } else if (ctx->platform.chrome && ctx->platform.android) {
        CF_VIEW_TRY(cf_view_str(
            out,
            "      <ol>\n"
            "        <li>Open the <em>"));
        {
            pwa_img spec = hidden_img(20);
            CF_VIEW_TRY(pwa_image(ctx, "external/gear.svg", &spec, out));
        }
        CF_VIEW_TRY(cf_view_str(
            out,
            "</em> Settings app.</li>\n"
            "        <li>Tap <em>Notifications</em>.</li>\n"
            "        <li>Tap <em>App notifications</em>.</li>\n"
            "        <li>Scroll to <em>Campfire</em>.</li>\n"
            "        <li>Tap "));
        {
            pwa_img spec = alt_img("the switch", 22);
            CF_VIEW_TRY(pwa_image(ctx, "external/switch.svg", &spec, out));
        }
        CF_VIEW_TRY(cf_view_str(
            out,
            " to <em>Allow Notifications</em>.</li>\n"
            "      </ol>\n"));
    } else {
        CF_VIEW_TRY(cf_view_str(
            out,
            "      <p>Ensure notifications are allowed for "));
        CF_VIEW_TRY(cf_view_capitalize(ctx->platform.browser, out));
        CF_VIEW_TRY(cf_view_str(
            out, " in your system settings.</p>\n"));
    }
    CF_VIEW_TRY(cf_view_str(out, "</details>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_pwa_install_instructions(const cf_view_ctx *ctx,
                                        cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    if (ctx->platform.chrome ||
        (ctx->platform.firefox && !ctx->platform.android)) {
        return CF_OK; /* Skipped where installing is not the flow. */
    }
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(
        out,
        "  <details class=\"notifications-help pwa__instructions "
        "hide-in-pwa\" data-controller=\"pwa-install\" "
        "data-pwa-install-prompting-class=\"pwa--can-install\" "
        "data-notifications-target=\"details\">\n"));
    CF_VIEW_TRY(settings_summary(
        ctx, "external/install.svg",
        cf_span_of_lit("Install Campfire as a web app."), out));
    if (ctx->platform.edge) {
        CF_VIEW_TRY(cf_view_str(out, "        <ol>\n"));
        {
            pwa_img spec =
                alt_img("the app available - install Campfire chat button", 16);
            CF_VIEW_TRY(li_image(ctx, "          ", "Click ", "install-edge.svg",
                                 &spec, "in the address bar.", out));
        }
        CF_VIEW_TRY(cf_view_str(
            out, "          <li>Click <em>Install</em>.</li>\n"
                 "        </ol>\n"));
    } else if (ctx->platform.chrome && ctx->platform.android) {
        CF_VIEW_TRY(cf_view_str(out, "        <ol>\n"));
        {
            pwa_img spec = alt_img("More options", 16);
            CF_VIEW_TRY(li_image(ctx, "          ", "Tap the ",
                                 "menu-dots-vertical.svg", &spec,
                                 " menu button.", out));
        }
        CF_VIEW_TRY(cf_view_str(
            out,
            "          <li>Tap <em>Install app</em> in the menu.</li>\n"
            "        </ol>\n"));
    } else if (ctx->platform.firefox && ctx->platform.android) {
        CF_VIEW_TRY(cf_view_str(out, "        <ol>\n"));
        {
            pwa_img spec = alt_img("More options", 16);
            CF_VIEW_TRY(li_image(ctx, "          ", "Tap the ",
                                 "menu-dots-vertical.svg", &spec,
                                 " menu button.", out));
        }
        CF_VIEW_TRY(cf_view_str(
            out,
            "          <li>Tap <em>Install</em> in the menu.</li>\n"
            "        </ol>\n"));
    } else if (ctx->platform.safari && ctx->platform.desktop) {
        CF_VIEW_TRY(cf_view_str(
            out,
            "        <ol>\n"
            "          <li>Click <em>File</em> in the top left.</li>\n"
            "          <li>Click <em>Add to Dock\xE2\x80\xA6</em>.</li>\n"
            "        </ol>\n"));
    } else if ((ctx->platform.safari || ctx->platform.chrome) &&
               ctx->platform.ios) {
        CF_VIEW_TRY(cf_view_str(
            out,
            "        <p>To receive push notifications in "));
        CF_VIEW_TRY(cf_view_capitalize(ctx->platform.browser, out));
        CF_VIEW_TRY(cf_view_str(out, " for "));
        CF_VIEW_TRY(cf_view_text(out, ctx->platform.operating_system));
        CF_VIEW_TRY(cf_view_str(
            out,
            ", you must install Campfire as a web app.</p>\n"
            "        <ol>\n"
            "          <li>Tap <em>"));
        {
            pwa_img spec = alt_img("the share button", 20);
            CF_VIEW_TRY(pwa_image(ctx, "external/share.svg", &spec, out));
        }
        CF_VIEW_TRY(cf_view_str(
            out,
            "</em></li>\n"
            "          <li>Tap <em>Add to Home Screen</em>.</li>\n"
            "        </ol>\n"));
    } else {
        CF_VIEW_TRY(cf_view_str(
            out,
            "        <p>Some platforms require you to install Campfire as a "
            "web app to receive push notifications.</p>\n"));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "    <div class=\"margin-block-start txt-align-center "
        "pwa__installer\">\n"
        "      <hr class=\"separator margin-block\">\n"
        "      <button class=\"btn btn--reversed center\" "
        "data-action=\"pwa-install#promptInstall\">\n        "));
    {
        /* attrs().aria_hidden() with no size: src only after. */
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("external/install.svg"),
                                      &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n        Install now\n"
        "      </button>\n"
        "    </div>\n"
        "  </details>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}
