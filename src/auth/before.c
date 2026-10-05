/* src/auth/before.c — ApplicationController's before-action chain in the
 * pinned concerns.rs order (block_banned_requests, authentication,
 * deny_bots, verify_authenticity_token, allow_browser) plus the CSRF check
 * (kit ctx.rs verify_authenticity_token, header-only: no form tokens).
 *
 * Halt convention: a helper that answers the request sets ctx->response and
 * returns CF_OK; callers stop with cf_auth_halted(ctx).  See src/auth.h.
 */
#include "internal.h"

#include "app.h"
#include "config.h"
#include "models/ban.h"
#include "models/room.h"
#include "platform.h"
#include "routes.h"
#include "user_agent.h"
#include "views.h"

#include <string.h>

/* src/routes.c owns the real endpoint lookup; the test route double linked by
 * the auth/actions/views buckets provides no endpoint table, so the reference
 * is weak: without src/routes.c "no endpoint" answers, and the Turbo-Frame
 * rule decides the layout.  A test may define its own strong cf_route_by_id. */
extern const cf_route *cf_route_by_id(uint32_t id) __attribute__((weak));

bool cf_auth_halted(const cf_ctx *ctx) {
    return ctx != NULL && ctx->response != NULL && ctx->response->status != 0;
}

static unsigned char auth_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

/* The kit's `request.header(name)` is
 * `headers.get(name).and_then(|v| v.to_str().ok())`: the first matching
 * header, whose value must pass http 1.5.0's `HeaderValue::to_str` (HTAB or
 * visible ASCII, 0x20..=0x7E). Anything else -- obs-text (which H01 admits),
 * DEL or another control -- makes the read answer the reference's `None`
 * (absent), never the raw bytes. A later header with the same name is not
 * consulted: `HeaderMap::get` returns the first value. */
bool auth_request_header(const cf_request *request, const char *name,
                         cf_span *out) {
    if (request == NULL || name == NULL || out == NULL) return false;
    size_t name_len = strlen(name);
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != name_len) continue;
        bool match = true;
        for (size_t k = 0; k < name_len; k++) {
            if (auth_lower(header->name.ptr[k]) !=
                (unsigned char)name[k]) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        if (!cf_ua_header_readable(header->value)) return false;
        *out = header->value;
        return true;
    }
    return false;
}

static bool auth_method_safe(const cf_request *request) {
    return request->method == CF_GET || request->method == CF_HEAD;
}

/* 1. set_version_headers: X-Version from the build-baked CF_APP_VERSION (the
 * Makefile's APP_VERSION), X-Rev only when a nonempty CF_GIT_REVISION define
 * exists.  Rails drops a header set to nil; the version is never nil. */
#ifndef CF_APP_VERSION
#define CF_APP_VERSION "0"
#endif
static cf_err auth_set_version_headers(cf_ctx *ctx) {
    cf_err rc = cf_response_header(ctx->response, auth_cstr_span("X-Version"),
                                   auth_cstr_span(CF_APP_VERSION));
#ifdef CF_GIT_REVISION
    if (rc == CF_OK && CF_GIT_REVISION[0] != '\0') {
        rc = cf_response_header(ctx->response, auth_cstr_span("X-Rev"),
                                auth_cstr_span(CF_GIT_REVISION));
    }
#endif
    return rc;
}

/* The kit's `request.user_agent()`: the first User-Agent value readable by
 * HeaderValue::to_str (HTAB or visible ASCII; H01 already rejected the other
 * control bytes, and obs-text is unreadable).  False is the reference's
 * `None` -- an absent header or one whose bytes cannot be read -- which the
 * platform helper parses as "".  The present? filter is applied only by
 * allow_browser's check, exactly like the Rust. */
static bool auth_user_agent(cf_ctx *ctx, cf_span *out) {
    cf_span value;
    if (!auth_request_header(ctx->request, "user-agent", &value)) return false;
    if (!cf_ua_header_readable(value)) return false;
    *out = value;
    return true;
}

/* `c.is_turbo_frame_request()`: the first `Turbo-Frame` header, whose bytes
 * must pass HeaderValue::to_str (HTAB or visible ASCII) and be nonempty after
 * `str::trim()`.  Same semantics as src/actions/sessions.c. */
static bool auth_turbo_frame_request(const cf_request *request) {
    if (request == NULL) return false;
    cf_span value = {NULL, 0};
    bool found = false;
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != 11) continue;
        static const char name[] = "turbo-frame";
        bool match = true;
        for (size_t k = 0; k < 11; k++) {
            if (auth_lower(header->name.ptr[k]) != (unsigned char)name[k]) {
                match = false;
                break;
            }
        }
        if (match) {
            value = header->value;
            found = true;
            break;
        }
    }
    if (!found) return false;
    bool blank = true;
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (!((c >= 32 && c < 127) || c == '\t')) return false;
        if (c != ' ' && c != '\t') blank = false;
    }
    return !blank;
}

/* render_incompatible_browser's own-layout rule, read from the Rust: the
 * matched route's endpoint starts with "messages#" or "messages/by_bots#",
 * and those controllers always use the application layout. */
static bool auth_route_owns_layout(const cf_ctx *ctx) {
    const cf_route *row =
        cf_route_by_id != NULL ? cf_route_by_id(ctx->route.id) : NULL;
    if (row == NULL || row->endpoint == NULL) return false;
    return strncmp(row->endpoint, "messages#", 9) == 0 ||
           strncmp(row->endpoint, "messages/by_bots#", 17) == 0;
}

/* `Layout#page` appends the stylesheet preload links (the `Link` header); the
 * frame layout carries none (same as src/actions/sessions.c). */
static cf_err auth_link_header(cf_ctx *ctx) {
    cf_builder links = {0};
    cf_err rc = cf_views_preload_links(&links);
    if (rc == CF_INVALID) return CF_OK;
    if (rc == CF_OK && links.len != 0) {
        rc = cf_response_header(ctx->response, auth_cstr_span("Link"),
                                (cf_span){links.ptr, links.len});
    }
    cf_builder_dispose(&links);
    return rc;
}

/* `render template: "sessions/incompatible_browser"` from the before-action:
 * 200 + text/html whatever the request format (no respond_to, never 406). */
static cf_err auth_render_incompatible_browser(cf_ctx *ctx) {
    cf_view_layout_model layout = {0};
    cf_err rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) return rc;
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);

    bool own_layout = auth_route_owns_layout(ctx);
    bool frame = !own_layout && auth_turbo_frame_request(ctx->request);

    cf_builder body = {0};
    rc = frame ? cf_view_session_incompatible_frame(&view_ctx, &body)
               : cf_view_session_incompatible(&view_ctx, &body);
    if (rc == CF_OK) {
        cf_buf *buf = NULL;
        rc = cf_builder_freeze(&body, &buf);
        if (rc != CF_OK) {
            cf_builder_dispose(&body);
        } else {
            ctx->response->status = 200;
            rc = cf_response_body(ctx->response, buf);
            cf_buf_release(buf);
            if (rc == CF_OK) {
                rc = cf_response_header(
                    ctx->response, auth_cstr_span("Content-Type"),
                    auth_cstr_span("text/html; charset=utf-8"));
            }
            if (rc == CF_OK && !frame) rc = auth_link_header(ctx);
        }
    } else {
        cf_builder_dispose(&body);
    }
    cf_view_layout_model_dispose(&layout);
    return rc;
}

/* 7. allow_browser: check a present, non-blank UA and render the
 * incompatible-browser page (200, HTML, any format) when the browser is below
 * Campfire's minimums.
 *
 * The layout's platform is the reference's `platform` helper: the
 * ApplicationPlatform allow_browser stored, else `ApplicationPlatform.new(
 * request.user_agent())` -- the header value when the kit can read it (blank
 * included: the gem then parses it, which for whitespace-only values is the
 * default UA), and "" when the header is absent or unreadable.  So every
 * request leaves step 7 with facts for the views; only the *check* is gated on
 * the kit's present? filter. */
static cf_err auth_allow_browser(cf_ctx *ctx) {
    cf_span user_agent = {NULL, 0};
    bool readable = auth_user_agent(ctx, &user_agent);
    cf_platform platform;
    cf_err rc = cf_platform_parse(readable ? user_agent : (cf_span){NULL, 0},
                                  &platform);
    if (rc != CF_OK) return rc;
    /* The context stores its own copy (cf_ctx is the frozen contract). */
    rc = cf_ctx_set_platform(ctx, &platform);
    if (rc != CF_OK) return rc;
    if (!readable || !cf_ua_present(user_agent)) return CF_OK;
    if (!cf_platform_blocked(&platform)) return CF_OK;
    return auth_render_incompatible_browser(ctx);
}

static cf_err auth_halt(cf_ctx *ctx, unsigned status, const char *content_type) {
    ctx->response->status = status;
    if (content_type != NULL) {
        return cf_response_header(ctx->response, auth_cstr_span("Content-Type"),
                                  auth_cstr_span(content_type));
    }
    return CF_OK;
}

static cf_err auth_halt_redirect(cf_ctx *ctx, cf_span location) {
    ctx->response->status = 302;
    cf_err rc = cf_response_header(ctx->response, auth_cstr_span("Location"),
                                   location);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response,
                                auth_cstr_span("Content-Type"),
                                auth_cstr_span("text/html; charset=utf-8"));
    }
    return rc;
}

/* BlockBannedRequests#reject_banned_ip: 429 for a banned remote IP on
 * non-GET/HEAD methods. */
static cf_err auth_reject_banned_ip(cf_ctx *ctx) {
    if (auth_method_safe(ctx->request)) return CF_OK;
    cf_span ip = ctx->request->peer_ip;
    if (ip.len == 0 || ctx->reader == NULL) return CF_OK;
    bool banned = false;
    cf_err rc = cf_ban_banned(
        ctx->reader, (cf_str){.ptr = (char *)(uintptr_t)ip.ptr, .len = ip.len},
        &banned);
    if (rc != CF_OK) return rc;
    if (banned) {
        /* `head(429)`: an empty text/html response. */
        return auth_halt(ctx, 429, "text/html");
    }
    return CF_OK;
}

/* deny_bots: 403 for bot-key requests when the controller denies bots. */
static cf_err auth_deny_bots(cf_ctx *ctx) {
    if (ctx->identity.kind == CF_AUTH_BOT) {
        return auth_halt(ctx, 403, "text/html");
    }
    return CF_OK;
}

/* The pinned request.base_url for the Origin comparison: scheme from the
 * listener (D-C07: proxy headers ignored), host_with_port from the Host
 * header read through the kit's `request.header()` gate (an unreadable value
 * reads as absent), the port only when it is not the scheme default.  A
 * request without a readable Host header falls back the way the pin's
 * `host()` does: `header("host")` -> `uri.authority()` (always None here
 * because H01 admits origin-form targets only) -> "localhost"
 * (request.rs:160-166).  The absent Host therefore yields scheme +
 * "localhost" with the scheme-default port, not PUBLIC_ORIGIN. */
static cf_err auth_base_url(cf_ctx *ctx, cf_builder *out) {
    const char *scheme = ctx->request->tls ? "https://" : "http://";
    unsigned standard_port = ctx->request->tls ? 443 : 80;
    cf_span host;
    if (!auth_request_header(ctx->request, "host", &host) || host.len == 0) {
        cf_err rc = cf_builder_append(out, auth_cstr_span(scheme));
        if (rc == CF_OK) {
            rc = cf_builder_append(out, auth_cstr_span("localhost"));
        }
        return rc;
    }
    cf_err rc = cf_builder_append(out, auth_cstr_span(scheme));
    if (rc != CF_OK) return rc;
    /* host[:port]; a bracketed IPv6 literal may contain colons. */
    cf_span name = host;
    unsigned port = standard_port;
    if (host.ptr[host.len - 1] != ']') {
        const unsigned char *colon = NULL;
        for (size_t i = host.len; i > 0; i--) {
            if (host.ptr[i - 1] == ':') {
                colon = host.ptr + i - 1;
                break;
            }
        }
        if (colon != NULL) {
            cf_span port_text = {colon + 1, (size_t)(host.ptr + host.len - colon - 1)};
            bool digits = port_text.len != 0;
            unsigned value = 0;
            for (size_t i = 0; i < port_text.len && digits; i++) {
                unsigned char c = port_text.ptr[i];
                if (c < '0' || c > '9') {
                    digits = false;
                    break;
                }
                if (value <= 65535) value = value * 10 + (c - '0');
            }
            if (digits && value <= 65535) {
                name.len = (size_t)(colon - host.ptr);
                port = value;
            }
        }
    }
    rc = cf_builder_append(out, name);
    if (rc == CF_OK && port != standard_port) {
        char suffix[8];
        int n = snprintf(suffix, sizeof suffix, ":%u", port);
        if (n > 0) rc = cf_builder_append(out, auth_cstr_span(suffix));
    }
    return rc;
}

cf_err cf_check_csrf(cf_ctx *ctx, bool bot_exempt) {
    if (ctx == NULL || ctx->app == NULL || ctx->response == NULL) {
        return CF_INVALID;
    }
    if (bot_exempt) return CF_OK;
    if (auth_method_safe(ctx->request)) return CF_OK;

    /* valid_request_origin? (forgery_protection_origin_check is on). */
    cf_span origin;
    if (auth_request_header(ctx->request, "origin", &origin)) {
        static const char null_origin[] = "null";
        cf_builder expected = {0};
        cf_err rc = auth_base_url(ctx, &expected);
        if (rc != CF_OK) {
            cf_builder_dispose(&expected);
            return rc;
        }
        cf_span base = {expected.ptr, expected.len};
        bool matches = origin.len == base.len &&
                       memcmp(origin.ptr, base.ptr, base.len) == 0;
        cf_builder_dispose(&expected);
        if (auth_span_equal(origin, auth_cstr_span(null_origin)) || !matches) {
            return auth_halt(ctx, 422, "text/html; charset=utf-8");
        }
    }

    cf_span fetch_site;
    if (auth_request_header(ctx->request, "sec-fetch-site", &fetch_site)) {
        if (auth_span_equal(fetch_site, auth_cstr_span("same-origin")) ||
            auth_span_equal(fetch_site, auth_cstr_span("same-site"))) {
            return CF_OK;
        }
        return auth_halt(ctx, 422, "text/html; charset=utf-8");
    }
    const cf_config *config = cf_app_config(ctx->app);
    if (config != NULL && !ctx->request->tls && config->disable_ssl) {
        /* Plain HTTP with SSL disabled: SameSite=Lax plus the Origin check is
         * the protection (browsers don't send Sec-Fetch-Site there). */
        return CF_OK;
    }
    return auth_halt(ctx, 422, "text/html; charset=utf-8");
}

cf_err cf_authorize_room(cf_db *db, int64_t user_id, int64_t room_id) {
    if (db == NULL) return CF_INVALID;
    cf_room room;
    bool found = false;
    cf_err rc = cf_room_find_for_user(db, user_id, room_id, &found, &room);
    if (rc != CF_OK) return rc;
    if (!found) return CF_NOT_FOUND;
    cf_room_dispose(&room);
    return CF_OK;
}

cf_err cf_before_actions(cf_ctx *ctx, cf_before policy) {
    if (ctx == NULL || ctx->app == NULL || ctx->response == NULL) {
        return CF_INVALID;
    }
    /* 1. set_version_headers (build-time defines; see above). */
    cf_err rc = auth_set_version_headers(ctx);
    if (rc != CF_OK) return rc;
    /* 2. Current.request = request: nothing to store in this context design. */

    /* 3. reject_banned_ip (unsafe methods only). */
    rc = auth_reject_banned_ip(ctx);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    /* 4. require_authentication / allow_unauthenticated_access /
     *    require_unauthenticated_access. */
    if (policy.authentication == CF_AUTH_REQUIRED) {
        rc = cf_authenticate(ctx);
        if (rc != CF_OK || cf_auth_halted(ctx)) return rc;
    }

    /* 5. deny_bots. */
    if (policy.deny_bots) {
        rc = auth_deny_bots(ctx);
        if (rc != CF_OK || cf_auth_halted(ctx)) return rc;
    }

    /* 6. verify_authenticity_token (skipped only for authenticated bots). */
    if (policy.forgery_protection && ctx->identity.kind != CF_AUTH_BOT) {
        rc = cf_check_csrf(ctx, false);
        if (rc != CF_OK || cf_auth_halted(ctx)) return rc;
    }

    /* 7. allow_browser (concerns.rs): parse a present UA, keep the platform
     *    on the context for the layout, block an outdated browser. */
    rc = auth_allow_browser(ctx);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    /* 8. require_unauthenticated_access: restore the session, then redirect a
     *    signed-in user to the root. */
    if (policy.authentication == CF_AUTH_REQUIRE_UNAUTHENTICATED) {
        bool restored = false, signed_in = false;
        rc = auth_restore_session_only(ctx, &restored, &signed_in);
        if (rc != CF_OK || cf_auth_halted(ctx)) return rc;
        if (signed_in) {
            const cf_config *config = cf_app_config(ctx->app);
            if (config == NULL) return CF_INTERNAL;
            cf_builder root = {0};
            rc = cf_builder_append(&root, auth_cstr_span(config->public_origin));
            if (rc == CF_OK) rc = cf_builder_append(&root, auth_cstr_span("/"));
            if (rc == CF_OK) {
                rc = auth_halt_redirect(ctx, (cf_span){root.ptr, root.len});
            }
            cf_builder_dispose(&root);
        }
    }
    return rc;
}
