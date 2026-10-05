/* src/actions/users/push_subscriptions.c — Users::PushSubscriptionsController
 * (task A-users-push_subscriptions; route ID 66 `index`, route ID 67
 * `create`, route ID 73 `destroy`; contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/users/
 * push_subscriptions.rs, the pinned port of reference/app/controllers/users/
 * push_subscriptions_controller.rb:
 *
 *   index:   before_actions(Before::default()) -> respond_to([HTML]) ->
 *            require_current_user ->
 *            PushSubscription::for_user(Current.user) -> map
 *            presenters::accounts::push_subscription (UA parse) ->
 *            framed_page(OK, users::PushSubscriptionsIndex)
 *   create:  wrap_parameters(:push_subscription) ->
 *            before_actions(Before::default()) -> require_current_user ->
 *            push_subscription_params
 *            (require+permit :endpoint, :p256dh_key, :auth_key) ->
 *            find_by(user, params) (only the given keys are conditions,
 *            nil is IS NULL; empty finds the user's first subscription) ->
 *            existing: valid? -> touch + head 200, else head 422;
 *            new: PushSubscription::new(user, endpoint, p256dh, auth,
 *            user_agent) -> resolve_endpoint -> create (validates again;
 *            RecordInvalid -> head 422) -> head 200
 *   destroy: before_actions(Before::default()) -> require_current_user ->
 *            param id integer_cast (absent/uncastable: no-op) ->
 *            write(find + ownership check + destroy; RecordNotFound and
 *            wrong-owner are Ok(())) -> redirect_to user_push_subscriptions
 *
 * The C translation preserves the callback order, the permit list, the
 * find_by condition rule (implemented over D01's for_user in row order,
 * matching the reference's unordered SELECT id ... LIMIT 1), the touch
 * (UPDATE updated_at) on the valid-existing path, the head shape (status +
 * rendered format's bare content type, empty body), and the destroy
 * ownership gate (another user's id never mutates; the redirect is
 * unconditional).
 *
 * Endpoint resolution: the reference resolves the endpoint host through
 * the system resolver behind the private-network guard.  The C default
 * resolver below does getaddrinfo on the request worker (blocking I/O is
 * the worker's contract, like I01's exchange) and filters through I01's
 * landed guard classifier (cf_unfurl_blocked_ip, surfguard default
 * policy), v4 first like guard.rs.  Exotic numeric forms the guard parses
 * itself (0x7f.1, 2130706433) come back unresolvable here; the outcome is
 * the same (invalid), and the delta is documented, not hidden.  The weak
 * cf_users_push_resolve_host seam (handlers.c precedent) lets the action
 * test pin resolution without network; production uses the default until
 * the integrator blesses a shared net-guard helper.
 *
 * JSON wrap_parameters (format [:json], include None): nest the body
 * params under push_subscription unless present, dropping
 * authenticity_token/_method/utf8.  The contract offers no param
 * enumeration, so the C port reads the three subscription keys from the
 * merged top level when the key is absent on a nonempty-body JSON
 * request — exactly equivalent to wrap+permit for this filter set, since
 * permit keeps only those keys.  Narrow documented deltas: an empty-body
 * JSON request answers the source's require-400 exactly (no wrap could be
 * non-empty); a nonempty body carrying only excluded keys (or a
 * non-object JSON body, or subscribable keys smuggled in the query
 * string) proceeds to find_by/create where the source answers 400.
 *
 * Render gate (A02): PushSubscriptionsIndex (index.html +
 * _push_subscription, UA-parse mapping) has no C view yet, so index
 * answers the reference's 500 after loading (loud, never a stub page).
 * The row scoping is real and covered below; the list loader is
 * non-static so the action test exercises it directly.
 *
 * Integrator requests:
 *  R1. PushSubscriptionsIndex view model + renderer (src/views.h),
 *      including the UA-parse mapping (browser/version/platform strings).
 *  R2. A shared record-touch helper (see profiles.c R2); until it lands,
 *      the static push_subscriptions touch below is the local copy.
 *  R3. A shared private-network host resolver for actions (this file's
 *      default + seam is the proposal).
 *
 * c_symbols for the integrator's route rebind (src/routes.c rows 66,67,73):
 *   cf_action_users_push_subscriptions_index,
 *   cf_action_users_push_subscriptions_create,
 *   cf_action_users_push_subscriptions_destroy.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "http/params.h"
#include "models/push_subscription.h"
#include "models/user.h"
#include "views/internal.h" /* cf_views_integer_cast */

#include <arpa/inet.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static cf_span push_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `require_current_user`: the chain guarantees one (messages.c). */
static cf_err push_current_user(cf_ctx *ctx, cf_user *out) {
    bool found = false;
    cf_err rc = cf_auth_current_user(ctx, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_INTERNAL;
    return CF_OK;
}

/* `redirect_to` (302, absolute location, reference content type). */
static cf_err push_redirect_to(cf_ctx *ctx, cf_span location) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    ctx->response->status = 302;
    cf_err rc = cf_response_header(ctx->response, push_span("Location"),
                                   location);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response,
                                push_span("Content-Type"),
                                push_span("text/html; charset=utf-8"));
    }
    return rc;
}

/* `url_for(user_push_subscriptions())`: PUBLIC_ORIGIN + the route path
 * (campfire_routes: "/users/me/push_subscriptions"). */
static cf_err push_redirect_index(cf_ctx *ctx) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    cf_err rc = cf_builder_append(&location,
                                  push_span(config->public_origin));
    if (rc == CF_OK) {
        rc = cf_builder_append(&location,
                               push_span("/users/me/push_subscriptions"));
    }
    if (rc == CF_OK) {
        rc = push_redirect_to(ctx, (cf_span){location.ptr, location.len});
    }
    cf_builder_dispose(&location);
    return rc;
}

/* `head status`: the status plus the rendered format's bare content type
 * (kit Ctx::head), empty body. */
static cf_err push_head(cf_ctx *ctx, unsigned status) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    ctx->response->status = status;
    const cf_format *format = cf_ctx_rendered_format(ctx);
    return cf_response_header(ctx->response, push_span("Content-Type"),
                              push_span(format->string));
}

/* ---- endpoint resolution ------------------------------------------------- */

static void push_optional_dispose(cf_optional_str *value) {
    if (value == NULL) return;
    free(value->value.ptr);
    value->present = false;
    value->value.ptr = NULL;
    value->value.len = 0;
}

static bool push_host_usable(cf_str host) {
    if (host.len == 0 || host.len > 255 || host.ptr == NULL) return false;
    for (size_t i = 0; i < host.len; i++) {
        unsigned char c = (unsigned char)host.ptr[i];
        if (c == '\0' || c == '%' || c > 127) return false;
    }
    return true;
}

/* Private-network guard, local copy of I01's surfguard default-policy
 * tables (src/integrations/unfurl.c kDisV4/kDisV6/..., themselves the
 * port of guard.rs).  This packet cannot link I01's translation unit
 * (not in the build yet), so the tables live here until the integrator
 * blesses a shared helper (R3); they are byte-identical by construction.
 * `blocked` means unusable for push: the resolver skips the address. */
typedef struct {
    uint32_t net;
    uint8_t prefix;
} push_v4range;
typedef struct {
    unsigned __int128 net;
    uint8_t prefix;
} push_v6range;

#define PUSH_V4(a, b, c, d, p) \
    {(uint32_t)(((a) << 24) | ((b) << 16) | ((c) << 8) | (d)), p}
#define PUSH_V6(s0, s1, s2, s3, s4, s5, s6, s7, p)                     \
    {(((unsigned __int128)(s0) << 112) | ((unsigned __int128)(s1) << 96) | \
      ((unsigned __int128)(s2) << 80) | ((unsigned __int128)(s3) << 64) |  \
      ((unsigned __int128)(s4) << 48) | ((unsigned __int128)(s5) << 32) |  \
      ((unsigned __int128)(s6) << 16) | (unsigned __int128)(s7)),       \
     p}

static const push_v4range push_dis_v4[] = {
    PUSH_V4(0, 0, 0, 0, 8), PUSH_V4(10, 0, 0, 0, 8),
    PUSH_V4(100, 64, 0, 0, 10), PUSH_V4(127, 0, 0, 0, 8),
    PUSH_V4(168, 63, 129, 16, 32), PUSH_V4(169, 254, 0, 0, 16),
    PUSH_V4(172, 16, 0, 0, 12), PUSH_V4(192, 0, 0, 0, 24),
    PUSH_V4(192, 0, 2, 0, 24), PUSH_V4(192, 88, 99, 0, 24),
    PUSH_V4(192, 168, 0, 0, 16), PUSH_V4(198, 18, 0, 0, 15),
    PUSH_V4(198, 51, 100, 0, 24), PUSH_V4(203, 0, 113, 0, 24),
    PUSH_V4(224, 0, 0, 0, 4), PUSH_V4(240, 0, 0, 0, 4),
};

static const push_v6range push_dis_v6[] = {
    PUSH_V6(0, 0, 0, 0, 0, 0, 0, 0, 128),
    PUSH_V6(0x100, 0, 0, 0, 0, 0, 0, 0, 64),
    PUSH_V6(0x100, 0, 0, 1, 0, 0, 0, 0, 64),
    PUSH_V6(0x2001, 0, 0, 0, 0, 0, 0, 0, 32),
    PUSH_V6(0x2001, 2, 0, 0, 0, 0, 0, 0, 48),
    PUSH_V6(0x2001, 0xdb8, 0, 0, 0, 0, 0, 0, 32),
    PUSH_V6(0x2002, 0, 0, 0, 0, 0, 0, 0, 16),
    PUSH_V6(0x3fff, 0, 0, 0, 0, 0, 0, 0, 20),
    PUSH_V6(0x5f00, 0, 0, 0, 0, 0, 0, 0, 16),
    PUSH_V6(0xfec0, 0, 0, 0, 0, 0, 0, 0, 10),
    PUSH_V6(0xff00, 0, 0, 0, 0, 0, 0, 0, 8),
};

/* IANA special-purpose globals that are NOT disallowed (unfurl.c kIanaV6
 * verbatim: a global address outside every table below is disallowed by
 * the fallthrough, exactly like disallowed_v6). */
static const push_v6range push_global_v6[] = {
    PUSH_V6(0x2001, 3, 0, 0, 0, 0, 0, 0, 32),
    PUSH_V6(0x2001, 4, 0x112, 0, 0, 0, 0, 0, 48),
};

static const push_v6range push_iana_v6[] = {
    PUSH_V6(0x2001, 0, 0, 0, 0, 0, 0, 0, 23),
    PUSH_V6(0x2001, 0x200, 0, 0, 0, 0, 0, 0, 23),
    PUSH_V6(0x2001, 0x400, 0, 0, 0, 0, 0, 0, 23),
    PUSH_V6(0x2001, 0x600, 0, 0, 0, 0, 0, 0, 23),
    PUSH_V6(0x2001, 0x800, 0, 0, 0, 0, 0, 0, 22),
    PUSH_V6(0x2001, 0xc00, 0, 0, 0, 0, 0, 0, 23),
    PUSH_V6(0x2001, 0xe00, 0, 0, 0, 0, 0, 0, 23),
    PUSH_V6(0x2001, 0x1200, 0, 0, 0, 0, 0, 0, 23),
    PUSH_V6(0x2001, 0x1400, 0, 0, 0, 0, 0, 0, 22),
    PUSH_V6(0x2001, 0x1800, 0, 0, 0, 0, 0, 0, 23),
    PUSH_V6(0x2001, 0x1a00, 0, 0, 0, 0, 0, 0, 23),
    PUSH_V6(0x2001, 0x1c00, 0, 0, 0, 0, 0, 0, 22),
    PUSH_V6(0x2001, 0x2000, 0, 0, 0, 0, 0, 0, 19),
    PUSH_V6(0x2001, 0x4000, 0, 0, 0, 0, 0, 0, 23),
    PUSH_V6(0x2001, 0x4200, 0, 0, 0, 0, 0, 0, 23),
    PUSH_V6(0x2001, 0x4400, 0, 0, 0, 0, 0, 0, 23),
    PUSH_V6(0x2001, 0x4600, 0, 0, 0, 0, 0, 0, 23),
    PUSH_V6(0x2001, 0x4800, 0, 0, 0, 0, 0, 0, 23),
    PUSH_V6(0x2001, 0x4a00, 0, 0, 0, 0, 0, 0, 23),
    PUSH_V6(0x2001, 0x4c00, 0, 0, 0, 0, 0, 0, 23),
    PUSH_V6(0x2001, 0x5000, 0, 0, 0, 0, 0, 0, 20),
    PUSH_V6(0x2001, 0x8000, 0, 0, 0, 0, 0, 0, 19),
    PUSH_V6(0x2001, 0xa000, 0, 0, 0, 0, 0, 0, 20),
    PUSH_V6(0x2001, 0xb000, 0, 0, 0, 0, 0, 0, 20),
    PUSH_V6(0x2002, 0, 0, 0, 0, 0, 0, 0, 16),
    PUSH_V6(0x2003, 0, 0, 0, 0, 0, 0, 0, 18),
    PUSH_V6(0x2400, 0, 0, 0, 0, 0, 0, 0, 12),
    PUSH_V6(0x2410, 0, 0, 0, 0, 0, 0, 0, 12),
    PUSH_V6(0x2600, 0, 0, 0, 0, 0, 0, 0, 12),
    PUSH_V6(0x2610, 0, 0, 0, 0, 0, 0, 0, 23),
    PUSH_V6(0x2620, 0, 0, 0, 0, 0, 0, 0, 23),
    PUSH_V6(0x2630, 0, 0, 0, 0, 0, 0, 0, 12),
    PUSH_V6(0x2800, 0, 0, 0, 0, 0, 0, 0, 12),
    PUSH_V6(0x2a00, 0, 0, 0, 0, 0, 0, 0, 12),
    PUSH_V6(0x2a10, 0, 0, 0, 0, 0, 0, 0, 12),
    PUSH_V6(0x2c00, 0, 0, 0, 0, 0, 0, 0, 12),
};

static bool push_in_v4(uint32_t ip, push_v4range r) {
    return r.prefix == 0 || (ip ^ r.net) >> (32 - r.prefix) == 0;
}

static bool push_in_v6(unsigned __int128 ip, push_v6range r) {
    return r.prefix == 0 || (ip ^ r.net) >> (128 - r.prefix) == 0;
}

static bool push_v6_ula(unsigned __int128 ip) {
    return push_in_v6(ip, (push_v6range){((unsigned __int128)0xfc00 << 112),
                                         7});
}

static bool push_v6_linklocal(unsigned __int128 ip) {
    return push_in_v6(ip, (push_v6range){((unsigned __int128)0xfe80 << 112),
                                         10});
}

static bool push_v6_ietf_proto(unsigned __int128 ip) {
    return push_in_v6(ip, (push_v6range){((unsigned __int128)0x2001 << 112),
                                         23});
}

/* Translation prefixes (unfurl.c kV4Mapped/kV4Trans/kV4Compat/kNat64*). */
static bool push_v6_translated(unsigned __int128 ip, bool *mapped) {
    static const push_v6range mapped_r[] = {
        PUSH_V6(0, 0, 0, 0, 0, 0xffff, 0, 0, 96),
        PUSH_V6(0, 0, 0, 0, 0, 0, 0, 0, 96),
        PUSH_V6(0x64, 0xff9b, 1, 0, 0, 0, 0, 0, 48),
    };
    for (size_t i = 0; i < sizeof mapped_r / sizeof mapped_r[0]; i++) {
        if (push_in_v6(ip, mapped_r[i])) {
            *mapped = true;
            return true;
        }
    }
    static const push_v6range v4_r[] = {
        PUSH_V6(0x64, 0xff9b, 0, 0, 0, 0, 0, 0, 96),
        PUSH_V6(0, 0, 0, 0, 0xffff, 0, 0, 0, 96),
    };
    for (size_t i = 0; i < sizeof v4_r / sizeof v4_r[0]; i++) {
        if (push_in_v6(ip, v4_r[i])) {
            *mapped = false;
            return true;
        }
    }
    return false;
}

static bool push_disallowed_v6(unsigned __int128 ip) {
    for (size_t i = 0;
         i < sizeof push_global_v6 / sizeof push_global_v6[0]; i++) {
        if (push_in_v6(ip, push_global_v6[i])) return false;
    }
    if (ip == 1 || push_v6_ula(ip) || push_v6_linklocal(ip) ||
        push_v6_ietf_proto(ip)) {
        return true;
    }
    for (size_t i = 0; i < sizeof push_dis_v6 / sizeof push_dis_v6[0]; i++) {
        if (push_in_v6(ip, push_dis_v6[i])) return true;
    }
    for (size_t i = 0;
         i < sizeof push_iana_v6 / sizeof push_iana_v6[0]; i++) {
        if (push_in_v6(ip, push_iana_v6[i])) return false;
    }
    return true;
}

static bool push_blocked_ip(const char *literal) {
    struct in_addr a4;
    struct in6_addr a6;
    if (inet_pton(AF_INET, literal, &a4) == 1) {
        uint32_t ip;
        memcpy(&ip, &a4, 4);
        ip = ntohl(ip);
        for (size_t i = 0;
             i < sizeof push_dis_v4 / sizeof push_dis_v4[0]; i++) {
            if (push_in_v4(ip, push_dis_v4[i])) return true;
        }
        return false;
    }
    if (inet_pton(AF_INET6, literal, &a6) == 1) {
        unsigned __int128 ip = 0;
        for (int i = 0; i < 16; i++) {
            ip = (ip << 8) | a6.s6_addr[i];
        }
        bool mapped = false;
        if (push_v6_translated(ip, &mapped)) {
            if (mapped) return true;
            ip &= (unsigned __int128)0xffffffffu;
            uint32_t v4 = (uint32_t)ip;
            for (size_t i = 0;
                 i < sizeof push_dis_v4 / sizeof push_dis_v4[0]; i++) {
                if (push_in_v4(v4, push_dis_v4[i])) return true;
            }
            return false;
        }
        return push_disallowed_v6(ip);
    }
    return true; /* unparsable => blocked */
}

/* First public address for `host`, v4 preferred (guard.rs order), as the
 * model's resolve callback: owned text, or absent for a private or
 * unresolvable host. */
static cf_optional_str push_default_resolve(void *arg, cf_str host) {
    (void)arg;
    cf_optional_str out = {false, {NULL, 0}};
    if (!push_host_usable(host)) return out;
    char name[256];
    memcpy(name, host.ptr, host.len);
    name[host.len] = '\0';

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *list = NULL;
    if (getaddrinfo(name, NULL, &hints, &list) != 0) return out;

    /* Two passes (v4, then v6), at most 256 answers like the guard. */
    for (int pass = 0; pass < 2; pass++) {
        size_t seen = 0;
        for (struct addrinfo *ai = list; ai != NULL && seen < 256;
             ai = ai->ai_next, seen++) {
            if ((pass == 0) != (ai->ai_family == AF_INET)) continue;
            if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
                continue;
            }
            char literal[INET6_ADDRSTRLEN];
            if (getnameinfo(ai->ai_addr, ai->ai_addrlen, literal,
                            sizeof literal, NULL, 0,
                            NI_NUMERICHOST) != 0) {
                continue;
            }
            if (push_blocked_ip(literal)) continue;
            size_t len = strlen(literal);
            char *copy = malloc(len + 1);
            if (copy == NULL) break;
            memcpy(copy, literal, len + 1);
            out.present = true;
            out.value.ptr = copy;
            out.value.len = len;
            break;
        }
        if (out.present) break;
    }
    freeaddrinfo(list);
    return out;
}

/* Test seam (handlers.c weak-hook precedent): a strong definition in the
 * test binary replaces production DNS.  NULL in production until linked
 * otherwise — the default above runs then. */
extern cf_optional_str cf_users_push_resolve_host(void *arg, cf_str host)
    __attribute__((weak));

static cf_optional_str push_resolve(void *arg, cf_str host) {
    if (cf_users_push_resolve_host != NULL) {
        return cf_users_push_resolve_host(arg, host);
    }
    return push_default_resolve(arg, host);
}

/* ---- shared param helpers ------------------------------------------------- */

/* `c.param_str(key)`: strings only (kit Param::as_str). */
static bool push_param_string(const cf_ctx *ctx, const char *name,
                              cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, push_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) {
        return false;
    }
    return cf_param_string(param, out) == CF_OK;
}

/* `wrap_parameters(:push_subscription, None)` relevance: a JSON request
 * (format.rs's JSON synonyms, params.c media_is_json's set). */
static bool push_is_json_request(const cf_request *request) {
    if (request == NULL) return false;
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != 12) continue;
        static const char name[] = "content-type";
        bool match = true;
        for (size_t k = 0; k < 12; k++) {
            unsigned char c = header->name.ptr[k];
            if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
            if (c != (unsigned char)name[k]) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        cf_span ct = header->value;
        bool readable = true;
        for (size_t k = 0; k < ct.len; k++) {
            unsigned char c = ct.ptr[k];
            if (c != '\t' && (c < 0x20 || c > 0x7E)) {
                readable = false;
                break;
            }
        }
        if (!readable || ct.len == 0) return false;
        size_t end = 0;
        while (end < ct.len && ct.ptr[end] != ';' && ct.ptr[end] != ',') {
            end++;
        }
        size_t start = 0;
        while (start < end &&
               (ct.ptr[start] == ' ' || ct.ptr[start] == '\t')) {
            start++;
        }
        while (end > start &&
               (ct.ptr[end - 1] == ' ' || ct.ptr[end - 1] == '\t')) {
            end--;
        }
        if (end == start) return false;
        cf_span media = {ct.ptr + start, end - start};
        const char *candidates[] = {
            "application/json",
            "text/x-json",
            "application/jsonrequest",
            "application/problem+json",
        };
        for (size_t k = 0;
             k < sizeof candidates / sizeof candidates[0]; k++) {
            size_t n = strlen(candidates[k]);
            if (media.len != n) continue;
            bool same = true;
            for (size_t j = 0; j < n; j++) {
                unsigned char c = media.ptr[j];
                if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
                if (c != (unsigned char)candidates[k][j]) {
                    same = false;
                    break;
                }
            }
            if (same) return true;
        }
        return false;
    }
    return false;
}

/* `Param::is_present` (messages.c convention). */
static bool push_param_blank(cf_span text) {
    for (size_t i = 0; i < text.len; i++) {
        unsigned char c = text.ptr[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\v' && c != '\f' &&
            c != '\r') {
            return false;
        }
    }
    return true;
}

static bool push_param_present(const cf_param *param) {
    if (param == NULL) return false;
    switch (cf_param_type(param)) {
    case CF_PARAM_NULL:
        return false;
    case CF_PARAM_BOOL: {
        bool present = false, value = false;
        if (cf_param_bool(param, &present, &value) != CF_OK) return false;
        return value;
    }
    case CF_PARAM_NUMBER:
    case CF_PARAM_UPLOAD:
        return true;
    case CF_PARAM_STRING: {
        cf_span text = {NULL, 0};
        if (cf_param_string(param, &text) != CF_OK) return false;
        return !push_param_blank(text);
    }
    case CF_PARAM_ARRAY:
    case CF_PARAM_OBJECT:
        return cf_param_count(param) > 0;
    }
    return false;
}

/* ---- index --------------------------------------------------------------- */

/* Non-static so the action test exercises the scoping directly: exactly
 * the caller's subscriptions, in model order. */
cf_err cf_users_push_subscriptions_list(cf_db *db, int64_t user_id,
                                        cf_push_subscription_vector *out) {
    return cf_push_subscription_for_user(db, user_id, out);
}

cf_err cf_action_users_push_subscriptions_index(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    /* `before_actions(Before::default())`. */
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    /* `respond_to([HTML])` before the user lookup (source order). */
    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) return rc;

    cf_user user = {0};
    rc = push_current_user(ctx, &user);
    int64_t user_id = user.id;
    cf_user_dispose(&user);
    if (rc != CF_OK) return rc;

    cf_push_subscription_vector subscriptions = {0};
    rc = cf_users_push_subscriptions_list(ctx->reader, user_id,
                                          &subscriptions);
    /* UA-parse mapping + framed_page(PushSubscriptionsIndex) need A02. */
    cf_push_subscription_vector_dispose(&subscriptions);
    if (rc == CF_OK) rc = CF_INTERNAL; /* R1 render gate: loud, no stub */
    return rc;
}

/* ---- create --------------------------------------------------------------- */

/* One permitted scalar condition of find_by: `given` when the key survived
 * permit (arrays/objects dropped), with its to_s text when the value has
 * one (kit to_s: strings/numbers/bools as text, nil as "", uploads as
 * none — the none case is the reference's IS NULL). */
typedef struct {
    bool given;
    bool has_text;
    cf_span text; /* borrowed when has_text */
} push_condition;

static void push_condition_from(const cf_param *value,
                                push_condition *out) {
    out->given = true;
    out->has_text = false;
    out->text = (cf_span){NULL, 0};
    if (value == NULL) {
        out->given = false;
        return;
    }
    cf_span text = {NULL, 0};
    if (cf_param_to_s(value, &text) == CF_OK) {
        out->has_text = true;
        out->text = text;
    }
}

/* Read one condition from a permitted map (arrays/objects dropped, like
 * permit). */
static void push_condition_get(const cf_param *permitted, const char *key,
                               push_condition *out) {
    out->given = false;
    out->has_text = false;
    out->text = (cf_span){NULL, 0};
    if (permitted == NULL ||
        cf_param_type(permitted) != CF_PARAM_OBJECT) {
        return;
    }
    const cf_param *value = cf_param_field(permitted, push_span(key));
    if (value == NULL) return;
    cf_param_kind kind = cf_param_type(value);
    if (kind == CF_PARAM_ARRAY || kind == CF_PARAM_OBJECT) return;
    push_condition_from(value, out);
}

/* Read one condition from the merged top level (the wrap fallback: same
 * array/object drop as permit). */
static void push_condition_top(cf_ctx *ctx, const char *key,
                               push_condition *out) {
    out->given = false;
    out->has_text = false;
    out->text = (cf_span){NULL, 0};
    const cf_param *value = cf_ctx_param(ctx, push_span(key));
    if (value == NULL) return;
    cf_param_kind kind = cf_param_type(value);
    if (kind == CF_PARAM_ARRAY || kind == CF_PARAM_OBJECT) return;
    push_condition_from(value, out);
}

static bool push_optional_matches(const cf_optional_str *column,
                                  const push_condition *condition) {
    if (!condition->has_text) return !column->present; /* IS NULL */
    if (!column->present) return false;                /* NULL = '' is not */
    if (column->value.len != condition->text.len) return false;
    return condition->text.len == 0 ||
           memcmp(column->value.ptr, condition->text.ptr,
                  condition->text.len) == 0;
}

/* `Current.user.push_subscriptions.find_by(...)`: only the given keys are
 * conditions; the first row in model order wins. */
static cf_err push_find_by_conditions(cf_db *db, int64_t user_id,
                                      const push_condition *endpoint,
                                      const push_condition *p256dh,
                                      const push_condition *auth, bool *found,
                                      cf_push_subscription *out) {
    cf_push_subscription_vector subscriptions = {0};
    cf_err rc = cf_push_subscription_for_user(db, user_id, &subscriptions);
    *found = false;
    if (rc == CF_OK) {
        for (size_t i = 0; i < subscriptions.len; i++) {
            cf_push_subscription *candidate = &subscriptions.items[i];
            if (endpoint->given &&
                !push_optional_matches(&candidate->endpoint, endpoint)) {
                continue;
            }
            if (p256dh->given &&
                !push_optional_matches(&candidate->p256dh_key, p256dh)) {
                continue;
            }
            if (auth->given &&
                !push_optional_matches(&candidate->auth_key, auth)) {
                continue;
            }
            /* Move the winner out (zero the slot against double free). */
            *out = candidate[0];
            memset(candidate, 0, sizeof *candidate);
            *found = true;
            break;
        }
    }
    cf_push_subscription_vector_dispose(&subscriptions);
    if (rc == CF_OK && !*found) *out = (cf_push_subscription){0};
    return rc;
}

/* `params.require(:push_subscription)`: present, or the false literal. */
static bool push_param_required(const cf_param *param) {
    if (param == NULL) return false;
    if (cf_param_type(param) == CF_PARAM_BOOL) {
        bool present = false, value = false;
        if (cf_param_bool(param, &present, &value) != CF_OK) return false;
        return true; /* false is what require accepts */
    }
    return push_param_present(param);
}

enum { PUSH_STMT_TOUCH_SUBSCRIPTION };

static const cf_stmt_def push_stmt_defs[] = {
    [PUSH_STMT_TOUCH_SUBSCRIPTION] = {
        "UPDATE \"push_subscriptions\" SET \"updated_at\" = ? "
        "WHERE \"push_subscriptions\".\"id\" = ?"},
};

static const cf_stmt_set push_stmt_set = {
    push_stmt_defs,
    sizeof push_stmt_defs / sizeof push_stmt_defs[0]};

/* `presenters::accounts::touch` on the subscription (R2 until shared). */
static cf_err push_touch_subscription(cf_db *db, int64_t id) {
    if (db == NULL) return CF_INVALID;
    char timebuf[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(cf_now_us(NULL), timebuf);
    sqlite3_stmt *stmt = NULL;
    if (rc == CF_OK) {
        rc = cf_db_stmt(db, &push_stmt_set, PUSH_STMT_TOUCH_SUBSCRIPTION,
                        &stmt);
    }
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(
            stmt, 1,
            (cf_span){(const unsigned char *)timebuf, strlen(timebuf)});
    }
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 2, id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step != SQLITE_DONE) rc = cf_db_err(step);
    }
    cf_db_stmt_done(stmt);
    return rc;
}

typedef struct {
    int64_t id;
} push_touch_arg;

static cf_err push_touch_cb(cf_tx *tx, void *arg) {
    push_touch_arg *touch = arg;
    return push_touch_subscription(cf_tx_db(tx), touch->id);
}

/* `request.user_agent()`: the first readable User-Agent value, else absent
 * (an empty header value stays present, like the reference). */
static cf_err push_user_agent(const cf_request *request,
                              cf_optional_str *out) {
    memset(out, 0, sizeof *out);
    if (request == NULL) return CF_INVALID;
    static const char name[] = "user-agent";
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != sizeof name - 1) continue;
        bool match = true;
        for (size_t k = 0; k < sizeof name - 1; k++) {
            unsigned char c = header->name.ptr[k];
            if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
            if (c != (unsigned char)name[k]) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        bool readable = true;
        for (size_t k = 0; k < header->value.len; k++) {
            unsigned char c = header->value.ptr[k];
            if (c != '\t' && (c < 0x20 || c > 0x7E)) {
                readable = false;
                break;
            }
        }
        if (!readable) return CF_OK; /* absent; later headers not consulted */
        if (header->value.len == 0) {
            out->present = true;
            return CF_OK;
        }
        char *copy = malloc(header->value.len + 1);
        if (copy == NULL) return CF_NOMEM;
        memcpy(copy, header->value.ptr, header->value.len);
        copy[header->value.len] = '\0';
        out->present = true;
        out->value.ptr = copy;
        out->value.len = header->value.len;
        return CF_OK;
    }
    return CF_OK;
}

typedef struct {
    cf_push_subscription input; /* owned; created pre-write */
} push_create_arg;

static cf_err push_create_cb(cf_tx *tx, void *arg) {
    push_create_arg *create = arg;
    cf_push_subscription created = {0};
    cf_err rc = cf_push_subscription_create(tx, &create->input, push_resolve,
                                            NULL, &created);
    cf_push_subscription_dispose(&created);
    return rc;
}

/* Build the new-record optionals from three conditions: a key the
 * permit kept is present with its to_s text (nil reads as ""); anything
 * else is absent — exactly `params.get(key).and_then(|p| p.to_s())`. */
static void push_new_optional(const push_condition *condition,
                              cf_optional_str *out) {
    out->present = false;
    out->value.ptr = NULL;
    out->value.len = 0;
    if (!condition->given || !condition->has_text) return;
    out->present = true;
    out->value.ptr = (char *)condition->text.ptr;
    out->value.len = condition->text.len;
}

cf_err cf_action_users_push_subscriptions_create(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    /* `c.wrap_parameters("push_subscription", None)` first (source order).
     * With the key absent on a nonempty-body JSON request the three
     * subscription keys read from the top level (equivalent, see above);
     * an empty-body JSON request and every non-JSON request without the
     * key fail require here. */
    const cf_param *nested =
        cf_ctx_param(ctx, push_span("push_subscription"));
    bool wrapped = false;
    const cf_param *permitted = NULL; /* the require+permit map, or empty */
    if (nested == NULL) {
        if (push_is_json_request(ctx->request) && ctx->request->body.len > 0) {
            wrapped = true;
        } else {
            return CF_INVALID; /* ParameterMissing -> 400 */
        }
    } else if (cf_param_type(nested) == CF_PARAM_OBJECT) {
        if (!push_param_required(nested)) return CF_INVALID; /* {} -> 400 */
        permitted = nested;
    } else {
        if (!push_param_required(nested)) return CF_INVALID;
        permitted = NULL; /* require passed a scalar; permit is empty */
    }

    /* `before_actions(Before::default())` runs after wrap (source order). */
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = push_current_user(ctx, &user);
    int64_t user_id = user.id;
    cf_user_dispose(&user);
    if (rc != CF_OK) return rc;

    /* `push_subscription_params`: the three conditions. */
    push_condition endpoint, p256dh, auth;
    if (wrapped) {
        push_condition_top(ctx, "endpoint", &endpoint);
        push_condition_top(ctx, "p256dh_key", &p256dh);
        push_condition_top(ctx, "auth_key", &auth);
    } else {
        push_condition_get(permitted, "endpoint", &endpoint);
        push_condition_get(permitted, "p256dh_key", &p256dh);
        push_condition_get(permitted, "auth_key", &auth);
    }

    /* find_by over the read connection (the source reads, then writes). */
    bool found = false;
    cf_push_subscription existing = {0};
    rc = push_find_by_conditions(ctx->reader, user_id, &endpoint, &p256dh,
                                 &auth, &found, &existing);
    if (rc != CF_OK) {
        cf_push_subscription_dispose(&existing);
        return rc;
    }

    if (found) {
        /* Existing endpoints must pass current validations. */
        cf_model_errors errors;
        memset(&errors, 0, sizeof errors);
        rc = cf_push_subscription_validate(&existing, push_resolve, NULL,
                                           &errors);
        bool valid = rc == CF_OK && errors.len == 0;
        cf_model_errors_dispose(&errors);
        int64_t existing_id = existing.id;
        cf_push_subscription_dispose(&existing);
        if (rc != CF_OK) return rc;
        if (!valid) return push_head(ctx, 422);
        /* `touch` in the writer (the reference touches on tx.conn()). */
        push_touch_arg touch_arg = {existing_id};
        rc = cf_write(ctx->app, push_touch_cb, &touch_arg);
        if (rc != CF_OK) return rc;
        return push_head(ctx, 200);
    }

    /* `PushSubscription::new` + user agent + create (validates again). */
    cf_optional_str endpoint_opt = {false, {NULL, 0}};
    cf_optional_str p256dh_opt = {false, {NULL, 0}};
    cf_optional_str auth_opt = {false, {NULL, 0}};
    cf_optional_str agent_opt = {false, {NULL, 0}};
    rc = push_user_agent(ctx->request, &agent_opt);
    push_create_arg create_arg;
    memset(&create_arg, 0, sizeof create_arg);
    if (rc == CF_OK) {
        push_new_optional(&endpoint, &endpoint_opt);
        push_new_optional(&p256dh, &p256dh_opt);
        push_new_optional(&auth, &auth_opt);
        rc = cf_push_subscription_new(user_id, endpoint_opt, p256dh_opt,
                                      auth_opt, agent_opt, &create_arg.input);
    }
    push_optional_dispose(&agent_opt);
    if (rc == CF_OK) rc = cf_write(ctx->app, push_create_cb, &create_arg);
    cf_push_subscription_dispose(&create_arg.input);
    if (rc == CF_INVALID) return push_head(ctx, 422); /* RecordInvalid */
    if (rc != CF_OK) return rc;
    return push_head(ctx, 200);
}

/* ---- destroy -------------------------------------------------------------- */

typedef struct {
    int64_t user_id;
    int64_t id;
} push_destroy_arg;

/* `PushSubscription::find` + ownership, in the writer (source order):
 * RecordNotFound and wrong-owner are Ok(()). */
static cf_err push_destroy_cb(cf_tx *tx, void *arg) {
    push_destroy_arg *destroy = arg;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    cf_push_subscription subscription = {0};
    cf_err rc = cf_push_subscription_find(db, destroy->id, &subscription);
    if (rc == CF_NOT_FOUND) return CF_OK;
    if (rc != CF_OK) {
        cf_push_subscription_dispose(&subscription);
        return rc;
    }
    if (subscription.user_id != destroy->user_id) {
        cf_push_subscription_dispose(&subscription);
        return CF_OK;
    }
    rc = cf_push_subscription_destroy(tx, &subscription);
    cf_push_subscription_dispose(&subscription);
    return rc;
}

cf_err cf_action_users_push_subscriptions_destroy(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    /* `before_actions(Before::default())`. */
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = push_current_user(ctx, &user);
    int64_t user_id = user.id;
    cf_user_dispose(&user);
    if (rc != CF_OK) return rc;

    /* `c.param_str("id").and_then(integer_cast)`: only a castable string
     * destroys; anything else skips to the redirect. */
    cf_span text = {NULL, 0};
    int64_t id = 0;
    if (push_param_string(ctx, "id", &text) &&
        cf_views_integer_cast(text, &id)) {
        push_destroy_arg arg = {user_id, id};
        rc = cf_write(ctx->app, push_destroy_cb, &arg);
        if (rc != CF_OK) return rc;
    }
    return push_redirect_index(ctx);
}
