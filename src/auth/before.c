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

#include <string.h>

bool cf_auth_halted(const cf_ctx *ctx) {
    return ctx != NULL && ctx->response != NULL && ctx->response->status != 0;
}

static unsigned char auth_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

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
        *out = header->value;
        return true;
    }
    return false;
}

static bool auth_method_safe(const cf_request *request) {
    return request->method == CF_GET || request->method == CF_HEAD;
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
 * listener (D-C07: proxy headers ignored), host_with_port from the (H01
 * validated) Host header, the port only when it is not the scheme default.
 * A request without a Host header falls back to PUBLIC_ORIGIN, which H01
 * admits only when it is consistent with the origin. */
static cf_err auth_base_url(cf_ctx *ctx, cf_builder *out) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL) return CF_INTERNAL;
    const char *scheme = ctx->request->tls ? "https://" : "http://";
    unsigned standard_port = ctx->request->tls ? 443 : 80;
    cf_span host;
    if (!auth_request_header(ctx->request, "host", &host) || host.len == 0) {
        return cf_builder_append(out, auth_cstr_span(config->public_origin));
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
    /* 1. set_version_headers: the pinned source writes X-Version/X-Rev from
     *    config.app_version/git_revision; F03's cf_config has neither field.
     *    Reported to the integrator; other tasks cannot add them either. */
    /* 2. Current.request = request: nothing to store in this context design. */

    /* 3. reject_banned_ip (unsafe methods only). */
    cf_err rc = auth_reject_banned_ip(ctx);
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

    /* 7. allow_browser: deferred to the integrator; it needs the platform_agent
     *    UA parser (campfire/src/concerns/user_agent.rs, no A01 fixture) and
     *    A02's sessions/incompatible_browser view for its block response.
     *    Reported in docs/devel/evidence/A01.md. */

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
