/* src/actions/pwa.c — A-pwa: `pwa#manifest` (route ID 149) and
 * `pwa#service_worker` (route ID 150)
 * (docs/devel/implementation/contracts/controller-packets.md "A-pwa").
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/pwa.rs, the pinned
 * port of reference/app/controllers/pwa_controller.rb:
 *
 *   allow_unauthenticated_access, skip_forgery_protection
 *
 *   manifest:       respond_to([JSON]); Account::first (None when no account
 *                   row exists yet); the pwa/manifest.json.erb template with
 *                   Current.account&.name (default "Campfire"),
 *                   fresh_account_logo_path(size: :small) and
 *                   fresh_account_logo_path, request.base_url and image_url
 *                   (base_url + asset_path); render_as JSON_UTF8.
 *   service_worker: respond_to([JS]); pwa/service_worker.js served verbatim
 *                   as text/javascript; charset=utf-8.
 *
 * The response conventions are the landed actions' (A-welcome/A-first_runs/
 * A-sessions/A-rooms/A-messages): a helper that answers the request sets
 * ctx->response and returns CF_OK (callers stop with cf_auth_halted,
 * src/auth.h); render_as carries `Vary: Accept` exactly when the format came
 * from the Accept header (cf_ctx_vary_accept, kit ctx.rs set_vary_header);
 * absolute URLs are PUBLIC_ORIGIN + the route path (the C port's url_for;
 * src/views/ctx.c builds the render base_url the same way).
 *
 * Route registration: src/routes.c rows 149/150 bind cf_action_pwa_manifest
 * and cf_action_pwa_service_worker to this file's actions.
 *
 * Integrator requests (views packet; this file is self-contained meanwhile):
 *  R1. cf_view_pwa_manifest — the Manifest template as a shared renderer.
 *      Proposed declaration (src/views.h):
 *        cf_err cf_view_pwa_manifest(const cf_view_ctx *ctx,
 *                                    const cf_account *account, // NULL when none
 *                                    cf_builder *out);
 *      Until it lands, the static pwa_manifest_body below renders the exact
 *      template bytes (verified field order/whitespace against
 *      tmp/rust-ref/crates/views/templates/pwa/manifest.json), including
 *      the template's `self.json()` (serde_json, raw `&`) rather than the
 *      port-wide cf_json_string (json-gem + ActiveSupport `&`-escaping):
 *      the static pwa_json_string below is serde_json's string escaping,
 *      byte-checked against serde_json 1.x.
 *  R2. cf_pwa_service_worker_js — the verbatim service_worker.js bytes.
 *      Proposed declaration: `extern const char cf_pwa_service_worker_js[];`
 *      plus `extern const size_t cf_pwa_service_worker_js_len;`
 *      Until it lands, the static copy below carries the pinned bytes
 *      (sha256 26694d89aeba03969b5ff8ef07d36c85be3d7d3952da4b656d44809596e44fa0).
 *  R3. (optional) a shared fresh_account_logo_path helper with the size
 *      argument (presenters/accounts.rs fresh_account_logo_path; routes
 *      fresh_account_logo: "?size=small" first, then "?v=").  The static
 *      pwa_logo_path below is that exact construction; src/presenters/
 *      layout.c already carries the no-size arm as account_logo_url.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "models/account.h"
#include "views.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static cf_span pwa_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `Manifest::json`: `serde_json::to_string(value)` (pwa.rs) — the plain
 * JSON string escaping, deliberately *without* the HTML-entity escaping
 * the old ERB template applied (which emitted `&amp;` into the logo URLs
 * and could invalidate the JSON).  This differs from the port-wide
 * cf_json_string (R01: json-gem + ActiveSupport escaping) exactly where
 * the pinned handler differs: `&`, `<` and `>` stay raw; only `"`, `\`
 * and the C0 controls are escaped (`\b \t \n \f \r` named, the rest
 * lowercase `\u00xx`); UTF-8 passes through raw.  Byte-checked against
 * serde_json 1.x.  Input must be valid UTF-8 (the reference operates on
 * `&str`); otherwise CF_INVALID, like cf_json_string. */
static bool pwa_utf8_valid(cf_span text) {
    size_t i = 0;
    while (i < text.len) {
        unsigned char b = text.ptr[i];
        size_t len;
        if (b < 0x80) {
            len = 1;
        } else if ((b & 0xE0) == 0xC0) {
            len = 2;
        } else if ((b & 0xF0) == 0xE0) {
            len = 3;
        } else if ((b & 0xF8) == 0xF0) {
            len = 4;
        } else {
            return false;
        }
        if (i + len > text.len) return false;
        for (size_t k = 1; k < len; k++) {
            if ((text.ptr[i + k] & 0xC0) != 0x80) return false;
        }
        i += len;
    }
    return true;
}

static cf_err pwa_json_string(cf_builder *out, cf_span value) {
    if (value.len != 0 && value.ptr == NULL) return CF_INVALID;
    if (!pwa_utf8_valid(value)) return CF_INVALID;
    static const char hex[] = "0123456789abcdef";
    cf_err rc = cf_builder_append(out, pwa_span("\""));
    for (size_t i = 0; rc == CF_OK && i < value.len; i++) {
        unsigned char c = value.ptr[i];
        switch (c) {
        case '"':
            rc = cf_builder_append(out, pwa_span("\\\""));
            break;
        case '\\':
            rc = cf_builder_append(out, pwa_span("\\\\"));
            break;
        case '\b':
            rc = cf_builder_append(out, pwa_span("\\b"));
            break;
        case '\t':
            rc = cf_builder_append(out, pwa_span("\\t"));
            break;
        case '\n':
            rc = cf_builder_append(out, pwa_span("\\n"));
            break;
        case '\f':
            rc = cf_builder_append(out, pwa_span("\\f"));
            break;
        case '\r':
            rc = cf_builder_append(out, pwa_span("\\r"));
            break;
        default:
            if (c < 0x20) {
                char esc[8] = {'\\', 'u', '0', '0', hex[c >> 4],
                               hex[c & 0x0F], '\0', '\0'};
                rc = cf_builder_append(
                    out, (cf_span){(const unsigned char *)esc, 6});
            } else {
                rc = cf_builder_append(out, (cf_span){&c, 1});
            }
            break;
        }
    }
    if (rc == CF_OK) rc = cf_builder_append(out, pwa_span("\""));
    return rc;
}

/* `Ctx::render_as` (kit ctx.rs render_as -> set_vary_header): status, content
 * type, body, plus `Vary: Accept` exactly when the format came from the
 * Accept header.  Consumes the builder. */
static cf_err pwa_send(cf_ctx *ctx, unsigned status, const char *content_type,
                       cf_builder *body) {
    cf_buf *buf = NULL;
    cf_err rc = cf_builder_freeze(body, &buf);
    if (rc != CF_OK) {
        cf_builder_dispose(body);
        return rc;
    }
    ctx->response->status = status;
    rc = cf_response_header(ctx->response, pwa_span("Content-Type"),
                            pwa_span(content_type));
    if (rc == CF_OK && cf_ctx_vary_accept(ctx)) {
        rc = cf_response_header(ctx->response, pwa_span("Vary"),
                                pwa_span("Accept"));
    }
    if (rc == CF_OK) rc = cf_response_body(ctx->response, buf);
    cf_buf_release(buf);
    return rc;
}

/* `Time#to_fs(:number)`: `%Y%m%d%H%M%S` in UTC (presenters.rs to_fs_number;
 * local copy of the helper src/presenters/layout.c and src/presenters/
 * rooms.c carry until it has a shared home). */
static void pwa_to_fs_number(int64_t us, char out[16]) {
    time_t seconds = (time_t)(us / 1000000);
    struct tm tm;
    if (gmtime_r(&seconds, &tm) == NULL) {
        memcpy(out, "00000000000000", 15);
    } else {
        strftime(out, 16, "%Y%m%d%H%M%S", &tm);
    }
    out[15] = '\0';
}

/* `fresh_account_logo_path(account, size)` (presenters/accounts.rs over
 * routes fresh_account_logo): "/account/logo", then "?" with "size=small"
 * first when sized, then "v=<to_fs_number>" when the account exists. */
static cf_err pwa_logo_path(const cf_account *account, bool small,
                            cf_builder *out) {
    cf_err rc = cf_builder_append(out, pwa_span("/account/logo"));
    if (rc != CF_OK) return rc;
    bool first = true;
    if (small) {
        rc = cf_builder_append(out, pwa_span("?size=small"));
        if (rc != CF_OK) return rc;
        first = false;
    }
    if (account != NULL) {
        char number[16];
        pwa_to_fs_number(account->updated_at, number);
        rc = cf_builder_append(
            out, pwa_span(first ? "?v=" : "&v="));
        if (rc != CF_OK) return rc;
        rc = cf_builder_append(
            out, (cf_span){(const unsigned char *)number, 14});
    }
    return rc;
}

/* `image_url(source)`: base_url + asset_path(source) (pwa.rs Manifest).
 * A logical path missing from the manifest is the reference's
 * MissingAssetError: CF_INTERNAL here (dispatch must not turn the
 * renderer's CF_NOT_FOUND into a 404 for a broken build). */
static cf_err pwa_image_url(cf_ctx *ctx, const char *logical, cf_builder *out) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_err rc = cf_builder_append(out, pwa_span(config->public_origin));
    if (rc != CF_OK) return rc;
    cf_builder resolved = {0};
    rc = cf_views_asset_path(pwa_span(logical), &resolved);
    if (rc == CF_NOT_FOUND) {
        cf_builder_dispose(&resolved);
        return CF_INTERNAL;
    }
    if (rc != CF_OK) {
        cf_builder_dispose(&resolved);
        return rc;
    }
    rc = cf_builder_append(out, (cf_span){resolved.ptr, resolved.len});
    cf_builder_dispose(&resolved);
    return rc;
}

/* One `"key": <json-string>` line at the template's indentation. */
static cf_err pwa_json_field(cf_builder *out, const char *indent,
                             const char *key, cf_span value) {
    cf_err rc = cf_builder_append(out, pwa_span(indent));
    if (rc == CF_OK) rc = cf_builder_append(out, pwa_span("\""));
    if (rc == CF_OK) rc = cf_builder_append(out, pwa_span(key));
    if (rc == CF_OK) rc = cf_builder_append(out, pwa_span("\": "));
    if (rc == CF_OK) rc = pwa_json_string(out, value);
    return rc;
}

/* INTEGRATOR SHIM R1 (see the header): the Manifest template
 * (tmp/rust-ref/crates/views/templates/pwa/manifest.json) rendered field by
 * field in template order with the template's own whitespace.  `account` is
 * NULL when Account::first finds no row (name defaults to "Campfire", logo
 * paths carry no `v=`).  Askama strips exactly one trailing template newline
 * (verified against askama 0.14), so the body ends `}\n`. */
static cf_err pwa_manifest_body(cf_ctx *ctx, const cf_account *account,
                                cf_builder *out) {
    cf_err rc = cf_builder_append(out, pwa_span("{\n"));
    /* "name" */
    if (rc == CF_OK) {
        cf_span name = pwa_span("Campfire");
        if (account != NULL) {
            name = (cf_span){(const unsigned char *)account->name.ptr,
                             account->name.len};
        }
        rc = pwa_json_field(out, "  ", "name", name);
    }
    if (rc == CF_OK) rc = cf_builder_append(out, pwa_span(",\n  \"icons\": [\n"));
    /* The three icon entries share the two logo paths. */
    cf_builder small = {0}, full = {0};
    if (rc == CF_OK) rc = pwa_logo_path(account, true, &small);
    if (rc == CF_OK) rc = pwa_logo_path(account, false, &full);
    if (rc == CF_OK) {
        rc = cf_builder_append(out, pwa_span("    {\n"));
    }
    if (rc == CF_OK) {
        rc = pwa_json_field(out, "      ", "src",
                            (cf_span){small.ptr, small.len});
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(
            out, pwa_span(",\n      \"type\": \"image/png\",\n      \"sizes\": "
                          "\"192x192\"\n    },\n    {\n"));
    }
    if (rc == CF_OK) {
        rc = pwa_json_field(out, "      ", "src",
                            (cf_span){full.ptr, full.len});
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(
            out, pwa_span(",\n      \"type\": \"image/png\",\n      \"sizes\": "
                          "\"512x512\"\n    },\n    {\n"));
    }
    if (rc == CF_OK) {
        rc = pwa_json_field(out, "      ", "src",
                            (cf_span){full.ptr, full.len});
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(
            out, pwa_span(",\n      \"type\": \"image/png\",\n      \"sizes\": "
                          "\"512x512\",\n      \"purpose\": \"maskable\"\n    "
                          "}\n  ],\n  \"start_url\": \"/\",\n  \"display\": "
                          "\"standalone\",\n  \"scope\": \"/\",\n  "
                          "\"description\": \"A chat app from the makers of "
                          "Basecamp and HEY.\",\n  \"categories\": [\"social\", "
                          "\"business\", \"productivity\"],\n  \"theme_color\": "
                          "\"#ffffff\",\n  \"background_color\": "
                          "\"#ffffff\",\n  \"shortcuts\": [\n    {\n"));
    }
    cf_builder_dispose(&small);
    cf_builder_dispose(&full);
    if (rc != CF_OK) return rc;
    /* Shortcuts (image_url over the asset map). */
    static const struct {
        const char *name, *description, *url, *icon;
    } shortcuts[2] = {
        {"New chat room", "Open Campfire and start a new chat room",
         "rooms/opens/new", "add.svg"},
        {"My profile", "Open Campfire and view your profile",
         "/users/me/profile", "person.svg"},
    };
    for (size_t i = 0; rc == CF_OK && i < 2; i++) {
        cf_builder icon = {0};
        if (i != 0) rc = cf_builder_append(out, pwa_span(",\n    {\n"));
        if (rc == CF_OK) rc = pwa_json_field(out, "      ", "name",
                                             pwa_span(shortcuts[i].name));
        if (rc == CF_OK) rc = cf_builder_append(out, pwa_span(",\n"));
        if (rc == CF_OK) {
            rc = pwa_json_field(out, "      ", "description",
                                pwa_span(shortcuts[i].description));
        }
        if (rc == CF_OK) rc = cf_builder_append(out, pwa_span(",\n"));
        if (rc == CF_OK) {
            rc = pwa_json_field(out, "      ", "url",
                                pwa_span(shortcuts[i].url));
        }
        if (rc == CF_OK) rc = cf_builder_append(out, pwa_span(",\n"));
        if (rc == CF_OK) rc = pwa_image_url(ctx, shortcuts[i].icon, &icon);
        if (rc == CF_OK) {
            rc = cf_builder_append(out, pwa_span("      \"icons\": [{ \"src\": "));
        }
        if (rc == CF_OK) {
            rc = pwa_json_string(out, (cf_span){icon.ptr, icon.len});
        }
        cf_builder_dispose(&icon);
        if (rc == CF_OK) {
            rc = cf_builder_append(out, pwa_span(", \"sizes\": \"any\" }]\n    }"));
        }
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(out, pwa_span("\n  ],\n  \"screenshots\": [\n"));
    }
    /* Screenshots (image_url over the asset map). */
    static const struct {
        const char *file, *label;
    } screenshots[3] = {
        {"screenshots/android-chat.png",
         "Campfire is an installable, self-hosted group chat system."},
        {"screenshots/android-sidebar.png",
         "Easily invite people. Make rooms. @mentions, DMs, and mobile support."},
        {"screenshots/android-dark-mode.png",
         "Full support for dark mode, customizable to your brand."},
    };
    for (size_t i = 0; rc == CF_OK && i < 3; i++) {
        cf_builder src = {0};
        if (i != 0) rc = cf_builder_append(out, pwa_span(",\n    {\n"));
        else rc = cf_builder_append(out, pwa_span("    {\n"));
        if (rc == CF_OK) rc = pwa_image_url(ctx, screenshots[i].file, &src);
        if (rc == CF_OK) {
            rc = pwa_json_field(out, "      ", "src",
                                (cf_span){src.ptr, src.len});
        }
        cf_builder_dispose(&src);
        if (rc == CF_OK) {
            rc = cf_builder_append(out, pwa_span(",\n      \"sizes\": "
                                                 "\"1080x2400\",\n      "
                                                 "\"form_factor\": "
                                                 "\"narrow\",\n"));
        }
        if (rc == CF_OK) {
            rc = pwa_json_field(out, "      ", "label",
                                pwa_span(screenshots[i].label));
        }
        if (rc == CF_OK) rc = cf_builder_append(out, pwa_span("\n    }"));
    }
    if (rc == CF_OK) rc = cf_builder_append(out, pwa_span("\n  ]\n}\n"));
    return rc;
}

/* INTEGRATOR SHIM R2 (see the header): `pwa::SERVICE_WORKER_JS`
 * (include_str! of the template, served verbatim — including its trailing
 * newline, since include_str! is not an Askama render). */
static const char pwa_service_worker_js[] =
    "self.addEventListener(\"push\", async (event) => {\n"
    "  const data = await event.data.json()\n"
    "  event.waitUntil(Promise.all([ showNotification(data), "
    "updateBadgeCount(data.options) ]))\n"
    "})\n"
    "\n"
    "async function showNotification({ title, options }) {\n"
    "  return self.registration.showNotification(title, options)\n"
    "}\n"
    "\n"
    "async function updateBadgeCount({ data: { badge } }) {\n"
    "  return self.navigator.setAppBadge?.(badge || 0)\n"
    "}\n"
    "\n"
    "self.addEventListener(\"notificationclick\", (event) => {\n"
    "  event.notification.close()\n"
    "\n"
    "  const url = new URL(event.notification.data.path, "
    "self.location.origin).href\n"
    "  event.waitUntil(openURL(url))\n"
    "})\n"
    "\n"
    "async function openURL(url) {\n"
    "  const clients = await self.clients.matchAll({ type: \"window\" })\n"
    "  const focused = clients.find((client) => client.focused)\n"
    "\n"
    "  if (focused) {\n"
    "    await focused.navigate(url)\n"
    "  } else {\n"
    "    await self.clients.openWindow(url)\n"
    "  }\n"
    "}\n";

/* `manifest`: `before_actions` with `allow_unauthenticated_access` +
 * `skip_forgery_protection` (deny_bots stays on); `respond_to([JSON])`;
 * `Account::first`; the Manifest render as `application/json`. */
cf_err cf_action_pwa_manifest(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_SKIPPED, true, false};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    const cf_format *offered[1] = {&cf_format_json};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) return rc;

    bool found = false;
    cf_account account = {0};
    rc = cf_account_first(ctx->reader, &found, &account);
    if (rc != CF_OK) {
        cf_account_dispose(&account);
        return rc;
    }

    cf_builder body = {0};
    rc = pwa_manifest_body(ctx, found ? &account : NULL, &body);
    cf_account_dispose(&account);
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        return rc;
    }
    return pwa_send(ctx, 200, "application/json; charset=utf-8", &body);
}

/* `service_worker`: `before_actions` as above; `respond_to([JS])`; the
 * verbatim JS as `text/javascript`. */
cf_err cf_action_pwa_service_worker(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_SKIPPED, true, false};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    const cf_format *offered[1] = {&cf_format_js};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) return rc;

    cf_builder body = {0};
    rc = cf_builder_append(
        &body, (cf_span){(const unsigned char *)pwa_service_worker_js,
                         sizeof pwa_service_worker_js - 1});
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        return rc;
    }
    return pwa_send(ctx, 200, "text/javascript; charset=utf-8", &body);
}
