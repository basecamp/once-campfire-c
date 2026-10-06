/* src/actions/users/push_subscriptions/test_notifications.c —
 * Users::PushSubscriptions::TestNotificationsController (task
 * A-users-push_subscriptions-test_notifications; route ID 65 `create`;
 * contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/users/
 * push_subscriptions/test_notifications.rs, the pinned port of
 * reference/app/controllers/users/push_subscriptions/
 * test_notifications_controller.rb:
 *
 *   create: before_actions(Before::default()) -> require_current_user ->
 *           push_subscription_id integer_cast (missing/uncastable is
 *           NotFound) -> read(Current.user.push_subscriptions.find(id):
 *           RecordNotFound, incl. wrong-owner, plus unread badge) ->
 *           location user_push_subscriptions ->
 *           web_push (missing VAPID keys: internal "Web Push is off") ->
 *           deliver_test_notification (title "Campfire Test", random UUID
 *           body, path = location, badge) inline, errors internal ->
 *           redirect_to location
 *
 * The C translation preserves the callback order, the ownership gate
 * (another user's id is RecordNotFound, never delivered to), the badge
 * (D01 unread count), the payload (I02 title macro, UUID body, icon/path
 * from the location), the inline delivery with error propagation (any
 * non-OK outcome is the reference's 500, including gone/invalid-key —
 * the inline path never destroys the subscription; the pool does that
 * elsewhere), and the unconditional redirect on success (delivered or
 * skipped alike).
 *
 * Delivery runs on the request worker through I02's cf_push_deliver with
 * the model's guard semantics; the HTTP exchange itself is I01's
 * (cf_push_exchange_fn). Production uses cf_push_http_exchange; the weak
 * cf_users_push_test_exchange seam is an optional deterministic test
 * override. Endpoint resolution
 * reuses the push_subscriptions packet's resolver shape (own static
 * default + weak seam; see R2 there).
 *
 * Integrator requests:
 *  R1. I01's push exchange helper with cf_push_exchange_fn shape
 *      (10s/10s/30s budgets, pinned-IP TLS POST), plus its wiring context.
 *  R2. Shared host resolver (push_subscriptions.c R3 covers this file too).
 *
 * c_symbols for the integrator's route rebind (src/routes.c row 65):
 *   cf_action_users_push_subscriptions_test_notifications_create.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "integrations/push.h"
#include "integrations/push_http.h"
#include "models/membership.h"
#include "models/push_subscription.h"
#include "models/user.h"
#include "views/internal.h" /* cf_views_integer_cast */

#include <arpa/inet.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static cf_span test_notifications_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `require_current_user`: the chain guarantees one (messages.c). */
static cf_err test_notifications_current_user(cf_ctx *ctx, cf_user *out) {
    bool found = false;
    cf_err rc = cf_auth_current_user(ctx, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_INTERNAL;
    return CF_OK;
}

/* `redirect_to` (302, absolute location, reference content type). */
static cf_err test_notifications_redirect_to(cf_ctx *ctx, cf_span location) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    ctx->response->status = 302;
    cf_err rc = cf_response_header(ctx->response,
                                   test_notifications_span("Location"),
                                   location);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response,
                                test_notifications_span("Content-Type"),
                                test_notifications_span(
                                    "text/html; charset=utf-8"));
    }
    return rc;
}

/* `url_for(user_push_subscriptions())`: PUBLIC_ORIGIN + the route path. */
static cf_err test_notifications_location(cf_ctx *ctx, cf_builder *holder,
                                          cf_span *location) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_err rc = cf_builder_append(holder,
                                  test_notifications_span(
                                      config->public_origin));
    if (rc == CF_OK) {
        rc = cf_builder_append(holder,
                               test_notifications_span(
                                   "/users/me/push_subscriptions"));
    }
    if (rc != CF_OK) return rc;
    *location = (cf_span){holder->ptr, holder->len};
    return CF_OK;
}

/* `c.param_str(key)`: strings only (kit Param::as_str). */
static bool test_notifications_param_string(const cf_ctx *ctx,
                                            const char *name,
                                            cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, test_notifications_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) {
        return false;
    }
    return cf_param_string(param, out) == CF_OK;
}

/* ---- endpoint resolution (push_subscriptions.c R3 shape) ------------------ */

/* Private-network guard, local copy of I01's surfguard default-policy
 * tables (src/integrations/unfurl.c kDisV4/kDisV6/..., themselves the
 * port of guard.rs).  This packet cannot link I01's translation unit
 * (not in the build yet), so the tables live here until the integrator
 * blesses a shared helper (R2); they are byte-identical by construction.
 * `blocked` means unusable for push: the resolver skips the address. */
typedef struct {
    uint32_t net;
    uint8_t prefix;
} notify_v4range;
typedef struct {
    unsigned __int128 net;
    uint8_t prefix;
} notify_v6range;

#define NOTIFY_V4(a, b, c, d, p) \
    {(uint32_t)(((a) << 24) | ((b) << 16) | ((c) << 8) | (d)), p}
#define NOTIFY_V6(s0, s1, s2, s3, s4, s5, s6, s7, p)                     \
    {(((unsigned __int128)(s0) << 112) | ((unsigned __int128)(s1) << 96) | \
      ((unsigned __int128)(s2) << 80) | ((unsigned __int128)(s3) << 64) |  \
      ((unsigned __int128)(s4) << 48) | ((unsigned __int128)(s5) << 32) |  \
      ((unsigned __int128)(s6) << 16) | (unsigned __int128)(s7)),       \
     p}

static const notify_v4range notify_dis_v4[] = {
    NOTIFY_V4(0, 0, 0, 0, 8), NOTIFY_V4(10, 0, 0, 0, 8),
    NOTIFY_V4(100, 64, 0, 0, 10), NOTIFY_V4(127, 0, 0, 0, 8),
    NOTIFY_V4(168, 63, 129, 16, 32), NOTIFY_V4(169, 254, 0, 0, 16),
    NOTIFY_V4(172, 16, 0, 0, 12), NOTIFY_V4(192, 0, 0, 0, 24),
    NOTIFY_V4(192, 0, 2, 0, 24), NOTIFY_V4(192, 88, 99, 0, 24),
    NOTIFY_V4(192, 168, 0, 0, 16), NOTIFY_V4(198, 18, 0, 0, 15),
    NOTIFY_V4(198, 51, 100, 0, 24), NOTIFY_V4(203, 0, 113, 0, 24),
    NOTIFY_V4(224, 0, 0, 0, 4), NOTIFY_V4(240, 0, 0, 0, 4),
};

static const notify_v6range notify_dis_v6[] = {
    NOTIFY_V6(0, 0, 0, 0, 0, 0, 0, 0, 128),
    NOTIFY_V6(0x100, 0, 0, 0, 0, 0, 0, 0, 64),
    NOTIFY_V6(0x100, 0, 0, 1, 0, 0, 0, 0, 64),
    NOTIFY_V6(0x2001, 0, 0, 0, 0, 0, 0, 0, 32),
    NOTIFY_V6(0x2001, 2, 0, 0, 0, 0, 0, 0, 48),
    NOTIFY_V6(0x2001, 0xdb8, 0, 0, 0, 0, 0, 0, 32),
    NOTIFY_V6(0x2002, 0, 0, 0, 0, 0, 0, 0, 16),
    NOTIFY_V6(0x3fff, 0, 0, 0, 0, 0, 0, 0, 20),
    NOTIFY_V6(0x5f00, 0, 0, 0, 0, 0, 0, 0, 16),
    NOTIFY_V6(0xfec0, 0, 0, 0, 0, 0, 0, 0, 10),
    NOTIFY_V6(0xff00, 0, 0, 0, 0, 0, 0, 0, 8),
};

/* IANA special-purpose globals that are NOT disallowed (unfurl.c kIanaV6
 * verbatim: a global address outside every table below is disallowed by
 * the fallthrough, exactly like disallowed_v6). */
static const notify_v6range notify_global_v6[] = {
    NOTIFY_V6(0x2001, 3, 0, 0, 0, 0, 0, 0, 32),
    NOTIFY_V6(0x2001, 4, 0x112, 0, 0, 0, 0, 0, 48),
};

static const notify_v6range notify_iana_v6[] = {
    NOTIFY_V6(0x2001, 0, 0, 0, 0, 0, 0, 0, 23),
    NOTIFY_V6(0x2001, 0x200, 0, 0, 0, 0, 0, 0, 23),
    NOTIFY_V6(0x2001, 0x400, 0, 0, 0, 0, 0, 0, 23),
    NOTIFY_V6(0x2001, 0x600, 0, 0, 0, 0, 0, 0, 23),
    NOTIFY_V6(0x2001, 0x800, 0, 0, 0, 0, 0, 0, 22),
    NOTIFY_V6(0x2001, 0xc00, 0, 0, 0, 0, 0, 0, 23),
    NOTIFY_V6(0x2001, 0xe00, 0, 0, 0, 0, 0, 0, 23),
    NOTIFY_V6(0x2001, 0x1200, 0, 0, 0, 0, 0, 0, 23),
    NOTIFY_V6(0x2001, 0x1400, 0, 0, 0, 0, 0, 0, 22),
    NOTIFY_V6(0x2001, 0x1800, 0, 0, 0, 0, 0, 0, 23),
    NOTIFY_V6(0x2001, 0x1a00, 0, 0, 0, 0, 0, 0, 23),
    NOTIFY_V6(0x2001, 0x1c00, 0, 0, 0, 0, 0, 0, 22),
    NOTIFY_V6(0x2001, 0x2000, 0, 0, 0, 0, 0, 0, 19),
    NOTIFY_V6(0x2001, 0x4000, 0, 0, 0, 0, 0, 0, 23),
    NOTIFY_V6(0x2001, 0x4200, 0, 0, 0, 0, 0, 0, 23),
    NOTIFY_V6(0x2001, 0x4400, 0, 0, 0, 0, 0, 0, 23),
    NOTIFY_V6(0x2001, 0x4600, 0, 0, 0, 0, 0, 0, 23),
    NOTIFY_V6(0x2001, 0x4800, 0, 0, 0, 0, 0, 0, 23),
    NOTIFY_V6(0x2001, 0x4a00, 0, 0, 0, 0, 0, 0, 23),
    NOTIFY_V6(0x2001, 0x4c00, 0, 0, 0, 0, 0, 0, 23),
    NOTIFY_V6(0x2001, 0x5000, 0, 0, 0, 0, 0, 0, 20),
    NOTIFY_V6(0x2001, 0x8000, 0, 0, 0, 0, 0, 0, 19),
    NOTIFY_V6(0x2001, 0xa000, 0, 0, 0, 0, 0, 0, 20),
    NOTIFY_V6(0x2001, 0xb000, 0, 0, 0, 0, 0, 0, 20),
    NOTIFY_V6(0x2002, 0, 0, 0, 0, 0, 0, 0, 16),
    NOTIFY_V6(0x2003, 0, 0, 0, 0, 0, 0, 0, 18),
    NOTIFY_V6(0x2400, 0, 0, 0, 0, 0, 0, 0, 12),
    NOTIFY_V6(0x2410, 0, 0, 0, 0, 0, 0, 0, 12),
    NOTIFY_V6(0x2600, 0, 0, 0, 0, 0, 0, 0, 12),
    NOTIFY_V6(0x2610, 0, 0, 0, 0, 0, 0, 0, 23),
    NOTIFY_V6(0x2620, 0, 0, 0, 0, 0, 0, 0, 23),
    NOTIFY_V6(0x2630, 0, 0, 0, 0, 0, 0, 0, 12),
    NOTIFY_V6(0x2800, 0, 0, 0, 0, 0, 0, 0, 12),
    NOTIFY_V6(0x2a00, 0, 0, 0, 0, 0, 0, 0, 12),
    NOTIFY_V6(0x2a10, 0, 0, 0, 0, 0, 0, 0, 12),
    NOTIFY_V6(0x2c00, 0, 0, 0, 0, 0, 0, 0, 12),
};

static bool notify_in_v4(uint32_t ip, notify_v4range r) {
    return r.prefix == 0 || (ip ^ r.net) >> (32 - r.prefix) == 0;
}

static bool notify_in_v6(unsigned __int128 ip, notify_v6range r) {
    return r.prefix == 0 || (ip ^ r.net) >> (128 - r.prefix) == 0;
}

static bool notify_v6_ula(unsigned __int128 ip) {
    return notify_in_v6(ip, (notify_v6range){((unsigned __int128)0xfc00 << 112),
                                         7});
}

static bool notify_v6_linklocal(unsigned __int128 ip) {
    return notify_in_v6(ip, (notify_v6range){((unsigned __int128)0xfe80 << 112),
                                         10});
}

static bool notify_v6_ietf_proto(unsigned __int128 ip) {
    return notify_in_v6(ip, (notify_v6range){((unsigned __int128)0x2001 << 112),
                                         23});
}

/* Translation prefixes (unfurl.c kV4Mapped/kV4Trans/kV4Compat/kNat64*). */
static bool notify_v6_translated(unsigned __int128 ip, bool *mapped) {
    static const notify_v6range mapped_r[] = {
        NOTIFY_V6(0, 0, 0, 0, 0, 0xffff, 0, 0, 96),
        NOTIFY_V6(0, 0, 0, 0, 0, 0, 0, 0, 96),
        NOTIFY_V6(0x64, 0xff9b, 1, 0, 0, 0, 0, 0, 48),
    };
    for (size_t i = 0; i < sizeof mapped_r / sizeof mapped_r[0]; i++) {
        if (notify_in_v6(ip, mapped_r[i])) {
            *mapped = true;
            return true;
        }
    }
    static const notify_v6range v4_r[] = {
        NOTIFY_V6(0x64, 0xff9b, 0, 0, 0, 0, 0, 0, 96),
        NOTIFY_V6(0, 0, 0, 0, 0xffff, 0, 0, 0, 96),
    };
    for (size_t i = 0; i < sizeof v4_r / sizeof v4_r[0]; i++) {
        if (notify_in_v6(ip, v4_r[i])) {
            *mapped = false;
            return true;
        }
    }
    return false;
}

static bool notify_disallowed_v6(unsigned __int128 ip) {
    for (size_t i = 0;
         i < sizeof notify_global_v6 / sizeof notify_global_v6[0]; i++) {
        if (notify_in_v6(ip, notify_global_v6[i])) return false;
    }
    if (ip == 1 || notify_v6_ula(ip) || notify_v6_linklocal(ip) ||
        notify_v6_ietf_proto(ip)) {
        return true;
    }
    for (size_t i = 0; i < sizeof notify_dis_v6 / sizeof notify_dis_v6[0]; i++) {
        if (notify_in_v6(ip, notify_dis_v6[i])) return true;
    }
    for (size_t i = 0;
         i < sizeof notify_iana_v6 / sizeof notify_iana_v6[0]; i++) {
        if (notify_in_v6(ip, notify_iana_v6[i])) return false;
    }
    return true;
}

static bool notify_blocked_ip(const char *literal) {
    struct in_addr a4;
    struct in6_addr a6;
    if (inet_pton(AF_INET, literal, &a4) == 1) {
        uint32_t ip;
        memcpy(&ip, &a4, 4);
        ip = ntohl(ip);
        for (size_t i = 0;
             i < sizeof notify_dis_v4 / sizeof notify_dis_v4[0]; i++) {
            if (notify_in_v4(ip, notify_dis_v4[i])) return true;
        }
        return false;
    }
    if (inet_pton(AF_INET6, literal, &a6) == 1) {
        unsigned __int128 ip = 0;
        for (int i = 0; i < 16; i++) {
            ip = (ip << 8) | a6.s6_addr[i];
        }
        bool mapped = false;
        if (notify_v6_translated(ip, &mapped)) {
            if (mapped) return true;
            ip &= (unsigned __int128)0xffffffffu;
            uint32_t v4 = (uint32_t)ip;
            for (size_t i = 0;
                 i < sizeof notify_dis_v4 / sizeof notify_dis_v4[0]; i++) {
                if (notify_in_v4(v4, notify_dis_v4[i])) return true;
            }
            return false;
        }
        return notify_disallowed_v6(ip);
    }
    return true; /* unparsable => blocked */
}

/* First public address for `host`, v4 preferred (guard.rs order), as the
 * model's resolve callback: owned text, or absent for a private or
 * unresolvable host. */


static bool test_notifications_host_usable(cf_str host) {
    if (host.len == 0 || host.len > 255 || host.ptr == NULL) return false;
    for (size_t i = 0; i < host.len; i++) {
        unsigned char c = (unsigned char)host.ptr[i];
        if (c == '\0' || c == '%' || c > 127) return false;
    }
    return true;
}

static cf_optional_str test_notifications_default_resolve(void *arg,
                                                          cf_str host) {
    (void)arg;
    cf_optional_str out = {false, {NULL, 0}};
    if (!test_notifications_host_usable(host)) return out;
    char name[256];
    memcpy(name, host.ptr, host.len);
    name[host.len] = '\0';

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *list = NULL;
    if (getaddrinfo(name, NULL, &hints, &list) != 0) return out;

    for (int pass = 0; pass < 2; pass++) {
        size_t seen = 0;
        for (struct addrinfo *ai = list; ai != NULL && seen < 256;
             ai = ai->ai_next, seen++) {
            if ((pass == 0) != (ai->ai_family == AF_INET)) continue;
            char literal[INET6_ADDRSTRLEN];
            if (getnameinfo(ai->ai_addr, ai->ai_addrlen, literal,
                            sizeof literal, NULL, 0,
                            NI_NUMERICHOST) != 0) {
                continue;
            }
            if (notify_blocked_ip(literal)) continue;
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

/* Test seam (handlers.c weak-hook precedent). */
extern cf_optional_str cf_users_push_test_resolve_host(void *arg,
                                                       cf_str host)
    __attribute__((weak));

static cf_optional_str test_notifications_resolve(void *arg, cf_str host) {
    if (cf_users_push_test_resolve_host != NULL) {
        return cf_users_push_test_resolve_host(arg, host);
    }
    return test_notifications_default_resolve(arg, host);
}

/* Optional deterministic exchange override used by controller tests. The
 * default is always the concrete production cf_push_http_exchange. */
extern cf_err cf_users_push_test_exchange(
    void *ctx, const cf_push_request *request, unsigned *out_status,
    char *reason_buf, size_t reason_cap,
    cf_push_transport_error *transport_err) __attribute__((weak));

cf_err cf_action_users_push_subscriptions_test_notifications_create(
    cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    /* `before_actions(Before::default())`. */
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = test_notifications_current_user(ctx, &user);
    int64_t user_id = user.id;
    cf_user_dispose(&user);
    if (rc != CF_OK) return rc;

    /* `param(push_subscription_id).integer_cast.ok_or(NotFound)`. */
    cf_span id_text = {NULL, 0};
    int64_t subscription_id = 0;
    if (!test_notifications_param_string(ctx, "push_subscription_id",
                                         &id_text) ||
        !cf_views_integer_cast(id_text, &subscription_id)) {
        return CF_NOT_FOUND;
    }

    /* `Current.user.push_subscriptions.find(id)` + unread badge, one read:
     * a missing row and a wrong-owner row are both RecordNotFound. */
    cf_push_subscription subscription = {0};
    int64_t badge = 0;
    rc = cf_push_subscription_find(ctx->reader, subscription_id,
                                   &subscription);
    if (rc == CF_OK && subscription.user_id != user_id) {
        cf_push_subscription_dispose(&subscription);
        memset(&subscription, 0, sizeof subscription);
        rc = CF_NOT_FOUND;
    }
    if (rc == CF_OK) {
        rc = cf_membership_unread_count(ctx->reader, user_id, &badge);
    }
    if (rc != CF_OK) {
        cf_push_subscription_dispose(&subscription);
        return rc;
    }

    /* `user_push_subscriptions_url` doubles as the notification path. */
    cf_builder holder = {0};
    cf_span location = {NULL, 0};
    rc = test_notifications_location(ctx, &holder, &location);
    char *location_text = NULL;
    if (rc == CF_OK) {
        location_text = malloc(location.len + 1);
        if (location_text == NULL) {
            rc = CF_NOMEM;
        } else {
            memcpy(location_text, location.ptr, location.len);
            location_text[location.len] = '\0';
        }
    }

    /* `app.web_push`: missing VAPID keys disable push explicitly. */
    cf_push_vapid vapid;
    memset(&vapid, 0, sizeof vapid);
    if (rc == CF_OK) {
        cf_push_vapid_error detail = CF_PUSH_VAPID_OK;
        if (cf_push_vapid_from_config(cf_app_config(ctx->app), &vapid,
                                      &detail) != CF_OK) {
            rc = CF_INTERNAL; /* "Web Push is off (no valid VAPID keys)" */
        }
    }

    /* Test fixtures may override the existing exchange seam. Production
     * always has the real bounded pinned-IP TLS exchange; no missing hook. */
    cf_push_exchange_fn exchange = cf_users_push_test_exchange != NULL
        ? cf_users_push_test_exchange : cf_push_http_exchange;

    /* `deliver_test_notification`: random UUID body, inline. */
    if (rc == CF_OK) {
        char body[37];
        rc = cf_push_test_body(body);
        if (rc == CF_OK) {
            cf_push_delivery delivery;
            memset(&delivery, 0, sizeof delivery);
            rc = cf_push_deliver(
                &subscription, CF_PUSH_TEST_TITLE, body, location_text,
                badge, &vapid, test_notifications_resolve, NULL, exchange,
                NULL, cf_now_us(NULL) / INT64_C(1000000), &delivery);
            bool delivered = rc == CF_OK &&
                             delivery.kind == CF_PUSH_DELIVERY_OK;
            cf_push_delivery_dispose(&delivery);
            /* Any determined non-OK outcome is the source's 500; the
             * inline path never destroys (the pool owns invalidation). */
            if (rc == CF_OK && !delivered) rc = CF_INTERNAL;
        }
    }
    cf_push_vapid_dispose(&vapid);
    cf_push_subscription_dispose(&subscription);
    free(location_text);
    if (rc != CF_OK) {
        cf_builder_dispose(&holder);
        return rc;
    }
    rc = test_notifications_redirect_to(ctx, location);
    cf_builder_dispose(&holder);
    return rc;
}
