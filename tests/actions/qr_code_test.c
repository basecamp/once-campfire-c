/* tests/actions/qr_code_test.c — A-qr_code acceptance: `qr_code#show`
 * (route ID 52), per docs/devel/implementation/contracts/controller-packets.md
 * "A-qr_code".
 *
 * Cases:
 *   - route binding: id 52 resolves to the action;
 *   - show: 200, image/svg+xml, the long-lived public Cache-Control
 *     (max-age=31556952), the stubbed SVG body;
 *   - urlsafe decoding follows the pinned Ruby vectors (unpadded, padded,
 *     `-_`/`+/` alphabets, empty): malformed ids ("a", "ab=", "ab=c",
 *     "aB==", "a*bc") are 500s;
 *   - an unencodable payload is 422 with an empty body;
 *   - unauthenticated access is allowed (no sign-in redirect); any request
 *     format answers the SVG.
 *
 * The qrcodegen-backed SVG helper has not landed (F00 archive; integrator
 * R1), so this binary stubs cf_qr_code_svg: bodies echo the payload length
 * ("<svg STUB nnn>"), and payloads over 64 bytes report "too much to
 * encode".  The route double binds row 52 to the real action, so every case
 * runs the real A00 dispatch path (no writer use: read-only).
 *
 * cf.h and src/actions/actions.h are integrator-owned and do not yet declare
 * this packet's symbol, so the entry point is declared here; the
 * integrator's routes.c rebind needs the same declaration (c_symbol
 * cf_action_qr_code_show).
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "cf.h"
#include "config.h"

#include "context.h"
#include "views.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "http/http_internal.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

cf_err cf_action_qr_code_show(cf_ctx *ctx);

#define ORIGIN "http://campfire.test"
#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define VAPID_PUBLIC_KEY                                                     \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8"  \
    "cTriz_qYBVicY02_VxTQ="

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* ---- R1 QR helper stub ---------------------------------------------------- */

cf_err cf_qr_code_svg(cf_span data, cf_str *out_svg) {
    memset(out_svg, 0, sizeof *out_svg);
    /* Too much to encode: the reference's rqrcode failure (a 422 here). */
    if (data.len > 64) return CF_NOT_FOUND;
    char body[64];
    int n = snprintf(body, sizeof body, "<svg STUB %zu>", data.len);
    if (n < 0 || (size_t)n >= sizeof body) return CF_INTERNAL;
    out_svg->ptr = malloc((size_t)n + 1);
    if (out_svg->ptr == NULL) return CF_NOMEM;
    memcpy(out_svg->ptr, body, (size_t)n + 1);
    out_svg->len = (size_t)n;
    return CF_OK;
}

/* ---- scratch app ------------------------------------------------------------ */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
} qr_env;

static bool env_open(qr_env *env) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;
    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", ORIGIN},
        {"SECRET_KEY_BASE", HEX64},
        {"VAPID_PUBLIC_KEY", VAPID_PUBLIC_KEY},
        {"DATABASE_PATH", env->scratch.path},
    };
    if (cf_config_parse(entries, 4, NULL, &env->config) != CF_OK) return false;
    if (cf_app_create(env->config, &env->app) != CF_OK) {
        cf_config_destroy(env->config);
        env->config = NULL;
        return false;
    }
    if (cf_writer_start(env->app, env->config) != CF_OK) return false;
    env->writer_started = true;
    cf_test_routes_reset();
    if (cf_test_routes_add("GET", "/qr_code/:id(.:format)", 52,
                           cf_action_qr_code_show) != CF_OK ||
        cf_test_routes_add("GET", "/qr_code/:id", 52,
                           cf_action_qr_code_show) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(qr_env *env) {
    if (env->writer_started) cf_writer_stop(env->app);
    if (env->app != NULL) cf_app_destroy(env->app);
    env->config = NULL;
    cf_db_scratch_close(&env->scratch);
    memset(env, 0, sizeof *env);
}

static bool run_request(qr_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void get_qr(cf_request *req, const char *id, const char *accept) {
    static char target[4096];
    cf_test_req_init(req);
    snprintf(target, sizeof target, "/qr_code/%s", id);
    req->path = SP(target);
    req->target = SP(target);
    if (accept != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Accept"), SP(accept)) ==
                   CF_OK);
    }
}

static bool span_contains(cf_span span, const char *needle) {
    size_t len = strlen(needle);
    if (len == 0) return true;
    if (span.len < len) return false;
    for (size_t i = 0; i + len <= span.len; i++) {
        if (memcmp(span.ptr + i, needle, len) == 0) return true;
    }
    return false;
}

static bool buf_contains(const cf_buf *buf, const char *needle) {
    return buf != NULL && span_contains(cf_buf_span(buf), needle);
}

static bool head_contains(cf_response *resp, cf_request *req,
                          const char *needle) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    bool found = buf_contains(ser.headers, needle);
    cf_buf_release(ser.headers);
    return found;
}

/* --- acceptance ------------------------------------------------------------ */

CF_TEST(qr_route_id_binds_the_action) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/qr_code/:id", 52,
                                  cf_action_qr_code_show) == CF_OK);
    CF_CHECK(cf_route_action(52) == cf_action_qr_code_show);
}

CF_TEST(qr_show_renders_the_svg_with_long_cache) {
    qr_env env;
    CF_REQUIRE(env_open(&env));

    /* "http://campfire.test", unpadded urlsafe form. */
    cf_request req;
    cf_response resp;
    get_qr(&req, "aHR0cDovL2NhbXBmaXJlLnRlc3Q", NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: image/svg+xml; charset=utf-8\r\n"));
    /* expires_in 1.year, public: true. */
    CF_CHECK(head_contains(&resp, &req,
                           "Cache-Control: max-age=31556952, public\r\n"));
    CF_CHECK(buf_contains(resp.body, "<svg STUB 20>"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(qr_show_accepts_padded_and_alt_alphabet_ids) {
    qr_env env;
    CF_REQUIRE(env_open(&env));

    /* ("+/8" decodes identically per the pinned vectors, but a "/" never
     * survives route recognition as one :id segment, so it is not
     * dispatch-reachable.) */
    const char *ids[] = {
        "aHR0cDovL2NhbXBmaXJlLnRlc3Q=", /* padded */
        "-_8",                          /* urlsafe pair */
    };
    const char *bodies[] = {
        "<svg STUB 20>",
        "<svg STUB 2>",
    };
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        cf_request req;
        cf_response resp;
        get_qr(&req, ids[i], NULL);
        CF_REQUIRE(run_request(&env, &req, &resp));
        if (resp.status != 200 || !buf_contains(resp.body, bodies[i])) {
            fprintf(stderr, "  id '%s': status=%u\n", ids[i], resp.status);
        }
        CF_CHECK(resp.status == 200);
        CF_CHECK(buf_contains(resp.body, bodies[i]));
        cf_response_dispose(&resp);
    }
    env_close(&env);
}

CF_TEST(qr_show_malformed_ids_are_500) {
    qr_env env;
    CF_REQUIRE(env_open(&env));

    /* The pinned urlsafe_decode64 vectors: None where Ruby raises. */
    const char *ids[] = {"a", "ab=", "ab=c", "aB==", "a*bc"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        cf_request req;
        cf_response resp;
        get_qr(&req, ids[i], NULL);
        CF_REQUIRE(run_request(&env, &req, &resp));
        if (resp.status != 500) {
            fprintf(stderr, "  id '%s': status=%u\n", ids[i], resp.status);
        }
        CF_CHECK(resp.status == 500);
        cf_response_dispose(&resp);
    }
    env_close(&env);
}

CF_TEST(qr_show_too_much_to_encode_is_422) {
    qr_env env;
    CF_REQUIRE(env_open(&env));

    /* 296 urlsafe chars -> 222 decoded bytes: past the stub's 64 limit.
     * ("QUFA" repeats keep the final quantum's trailing bits zero.) */
    static char big[300];
    for (size_t i = 0; i + 4 <= sizeof big - 1; i += 4) {
        memcpy(big + i, "QUFA", 4);
    }
    big[sizeof big - 1] = '\0';
    cf_request req;
    cf_response resp;
    get_qr(&req, big, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 422);
    CF_CHECK(resp.body == NULL);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(qr_show_answers_any_format_unauthenticated) {
    qr_env env;
    CF_REQUIRE(env_open(&env));

    cf_request req;
    cf_response resp;
    get_qr(&req, "aHR0cDovL2NhbXBmaXJlLnRlc3Q", "application/json");
    CF_REQUIRE(run_request(&env, &req, &resp));
    /* No format negotiation: the SVG answers whatever was asked. */
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: image/svg+xml; charset=utf-8\r\n"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST_MAIN()
