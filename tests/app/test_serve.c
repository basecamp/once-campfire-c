/* A00 integration tests: full app + writer + request workers behind a real H01
 * loop. Covers CORE-03/CORE-04 through the dispatch path, admission-slot
 * boundaries, shutdown drain, Set-Cookie output on the wire, and the K01c
 * always-on representation at CF_CACHE_BYTES=0 (fresh-connection gzip/
 * identity/406 + Vary probe). */
#include "cf_test.h"

#include "app.h"
#include "app_internal.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "core/testclock.h"

#include "support/route_double.h"
#include "support/serve_harness.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

/* ------------------------------------------------------------- actions */

static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int gate_release;
static int gate_entered;
static int gate_hold;

static cf_err respond_ok(cf_ctx *ctx) {
    cf_buf *body = NULL;
    cf_err rc = cf_buf_copy(CF_TEST_SPAN("ok"), &body);
    if (rc != CF_OK) return rc;
    rc = cf_response_body(ctx->response, body);
    cf_buf_release(body);
    if (rc != CF_OK) return rc;
    rc = cf_response_header(ctx->response, CF_TEST_SPAN("Content-Type"),
                            CF_TEST_SPAN("text/plain"));
    if (rc != CF_OK) return rc;
    ctx->response->status = 200;
    return CF_OK;
}

static cf_err action_ok(cf_ctx *ctx) { return respond_ok(ctx); }

/* A test double for one of the four admitted handlers: the five-call K01c
 * recipe with no auth/gather (the body is fixed). Bound to route ID 101 so
 * the representation pipeline is eligible; the harness config has
 * CF_CACHE_BYTES=0, which is the point of the probe. */
static cf_err action_representation(cf_ctx *ctx) {
    cf_cache_round round;
    cf_cache_round_init(ctx, &round);
    cf_buf *body = NULL;
    cf_err rc = cf_buf_copy(CF_TEST_SPAN("representation probe body"), &body);
    if (rc != CF_OK) {
        cf_cache_round_dispose(&round);
        return rc;
    }
    ctx->response->status = 200;
    rc = cf_response_header(ctx->response, CF_TEST_SPAN("Content-Type"),
                            CF_TEST_SPAN("text/plain"));
    if (rc == CF_OK) rc = cf_response_body(ctx->response, body);
    cf_buf_release(body);
    cf_cache_round_finish(ctx, &round);
    cf_cache_round_dispose(&round);
    return rc;
}

static cf_err action_blocking(cf_ctx *ctx) {
    pthread_mutex_lock(&gate_mu);
    gate_entered++;
    pthread_cond_broadcast(&gate_cv);
    if (gate_hold) {
        while (!gate_release) pthread_cond_wait(&gate_cv, &gate_mu);
    }
    pthread_mutex_unlock(&gate_mu);
    return respond_ok(ctx);
}

static int cookie_mode;

static cf_err action_cookie(cf_ctx *ctx) {
    cf_cookie_options opts;
    memset(&opts, 0, sizeof opts);
    switch (cookie_mode) {
    case 0: /* permanent last_room */
        opts.permanent = true;
        CF_CHECK(cf_ctx_cookie_set(ctx, CF_TEST_SPAN("last_room"),
                                   CF_TEST_SPAN("42"), &opts) == CF_OK);
        break;
    case 1: /* unchanged value: no header */
        CF_CHECK(cf_ctx_cookie_set(ctx, CF_TEST_SPAN("last_room"),
                                   CF_TEST_SPAN("42"), NULL) == CF_OK);
        break;
    case 2: /* delete */
        CF_CHECK(cf_ctx_cookie_delete(ctx, CF_TEST_SPAN("session_token"),
                                      NULL) == CF_OK);
        break;
    case 3: /* escaping */
        CF_CHECK(cf_ctx_cookie_set(ctx, CF_TEST_SPAN("x"),
                                   CF_TEST_SPAN("a b+c/="), NULL) == CF_OK);
        break;
    case 4: /* secure on http: dropped */
        opts.secure = true;
        CF_CHECK(cf_ctx_cookie_set(ctx, CF_TEST_SPAN("s"),
                                   CF_TEST_SPAN("1"), &opts) == CF_OK);
        break;
    case 5: /* explicit expiry (2044-06-01T12:00:00Z) */
        opts.has_expires = true;
        opts.expires_us = INT64_C(2348395200000000);
        CF_CHECK(cf_ctx_cookie_set(ctx, CF_TEST_SPAN("e"),
                                   CF_TEST_SPAN("1"), &opts) == CF_OK);
        break;
    case 6: /* full option surface */
        opts.httponly = true;
        opts.secure = false;
        opts.partitioned = true;
        opts.samesite = CF_COOKIE_SAMESITE_STRICT;
        opts.path = CF_TEST_SPAN("/x");
        opts.domain = CF_TEST_SPAN("example.com");
        CF_CHECK(cf_ctx_cookie_set(ctx, CF_TEST_SPAN("opt"),
                                   CF_TEST_SPAN("v"), &opts) == CF_OK);
        break;
    case 7: /* delete with path and SameSite=None */
        opts.path = CF_TEST_SPAN("/x");
        opts.samesite = CF_COOKIE_SAMESITE_NONE;
        CF_CHECK(cf_ctx_cookie_delete(ctx, CF_TEST_SPAN("session_token"),
                                      &opts) == CF_OK);
        break;
    default:
        break;
    }
    return respond_ok(ctx);
}

/* ------------------------------------------------------------- helpers */

static void gate_reset(bool hold) {
    pthread_mutex_lock(&gate_mu);
    gate_release = 0;
    gate_entered = 0;
    gate_hold = hold;
    pthread_mutex_unlock(&gate_mu);
}

static void gate_release_all(void) {
    pthread_mutex_lock(&gate_mu);
    gate_release = 1;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_mu);
}

static bool wait_gate_entered(int count, int timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&gate_mu);
    while (gate_entered < count) {
        if (pthread_cond_timedwait(&gate_cv, &gate_mu, &ts) == ETIMEDOUT) {
            break;
        }
    }
    bool ok = gate_entered >= count;
    pthread_mutex_unlock(&gate_mu);
    return ok;
}

static bool wait_admitted(cf_app *app, size_t count, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        if (cf_app_admitted_count(app) >= count) return true;
        usleep(5000);
    }
    return cf_app_admitted_count(app) >= count;
}

static int do_request(struct srv *s, const char *path, const char *headers,
                      char *resp, size_t cap) {
    char raw[1024];
    snprintf(raw, sizeof raw,
             "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nConnection: close\r\n%s"
             "\r\n",
             path, s->port, headers != NULL ? headers : "");
    return srv_request(s, raw, NULL, resp, cap, 8000);
}

static int send_request_no_read(struct srv *s, const char *path, int *fd_out) {
    char raw[1024];
    snprintf(raw, sizeof raw,
             "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nConnection: close\r\n"
             "\r\n",
             path, s->port);
    return srv_request(s, raw, fd_out, NULL, 0, 0);
}

/* --------------------------------------------------------------- tests */

CF_TEST(serve_ok_and_404) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/", 1, action_ok) == CF_OK);
    struct srv s;
    CF_REQUIRE(srv_start(&s, 1, 4, 1) == CF_OK);

    char resp[4096];
    CF_REQUIRE(do_request(&s, "/", NULL, resp, sizeof resp) == 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200") != NULL);
    CF_CHECK(strstr(resp, "\r\n\r\nok") != NULL);

    CF_REQUIRE(do_request(&s, "/missing", NULL, resp, sizeof resp) == 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 404") != NULL);

    srv_stop(&s);
}

/* Gunzip `gzip` and compare with the identity text. */
static bool wire_gunzip_equals(cf_span gzip, const char *identity) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, 15 + 16) != Z_OK) return false;
    unsigned char out[4096];
    zs.next_in = (Bytef *)gzip.ptr;
    zs.avail_in = (uInt)gzip.len;
    size_t at = 0;
    size_t identity_len = strlen(identity);
    bool ok = true;
    int rc;
    do {
        zs.next_out = out;
        zs.avail_out = sizeof out;
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            ok = false;
            break;
        }
        size_t got = sizeof out - zs.avail_out;
        if (at + got > identity_len ||
            memcmp(out, identity + at, got) != 0) {
            ok = false;
            break;
        }
        at += got;
    } while (rc != Z_STREAM_END);
    inflateEnd(&zs);
    return ok && at == identity_len;
}

/* One request on a fresh connection; *body points at the Content-Length
 * bytes after the header block when it is non-NULL (a gzip body contains
 * NULs, so its length comes from the header, never from strlen). */
static bool wire_request(struct srv *s, const char *headers, char *resp,
                         size_t cap, cf_span *body) {
    if (do_request(s, "/repr", headers, resp, cap) != 0) return false;
    char *sep = strstr(resp, "\r\n\r\n");
    if (sep == NULL) return false;
    if (body != NULL) {
        size_t len = 0;
        char *cl = strstr(resp, "Content-Length: ");
        if (cl != NULL) {
            len = (size_t)strtoul(cl + strlen("Content-Length: "), NULL, 10);
        }
        body->ptr = (const unsigned char *)(sep + 4);
        body->len = len;
    }
    return true;
}

/* Fresh-connection probe, CF_CACHE_BYTES=0 (the harness config): an eligible
 * body must still honor Accept-Encoding and carry Vary: Accept-Encoding. */
CF_TEST(serve_representation_is_uncached_always_on) {
    static const char payload[] = "representation probe body";
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/repr", 101,
                                  action_representation) == CF_OK);
    struct srv s;
    CF_REQUIRE(srv_start(&s, 1, 4, 1) == CF_OK);

    char resp[8192];
    /* Identity: Vary present, no encoding. */
    CF_REQUIRE(wire_request(&s, NULL, resp, sizeof resp, NULL));
    CF_CHECK(strstr(resp, "HTTP/1.1 200") != NULL);
    CF_CHECK(strstr(resp, "Content-Encoding:") == NULL);
    CF_CHECK(strstr(resp, "Vary: Accept-Encoding") != NULL);
    CF_CHECK(strstr(resp, "\r\n\r\n") != NULL &&
             strstr(strstr(resp, "\r\n\r\n") + 4, payload) != NULL);

    /* gzip on a fresh connection: gzip body + Content-Encoding + Vary. */
    cf_span gz_body = {NULL, 0};
    CF_REQUIRE(wire_request(&s,
                            "Accept-Encoding: gzip\r\n"
                            "Connection: close\r\n",
                            resp, sizeof resp, &gz_body));
    CF_CHECK(strstr(resp, "HTTP/1.1 200") != NULL);
    CF_CHECK(strstr(resp, "Content-Encoding: gzip") != NULL);
    CF_CHECK(strstr(resp, "Vary: Accept-Encoding") != NULL);
    CF_CHECK(wire_gunzip_equals(gz_body, payload));

    /* gzip;q=0 selects identity. */
    CF_REQUIRE(wire_request(&s, "Accept-Encoding: gzip;q=0\r\n", resp,
                            sizeof resp, NULL));
    CF_CHECK(strstr(resp, "HTTP/1.1 200") != NULL);
    CF_CHECK(strstr(resp, "Content-Encoding:") == NULL);

    /* Both forbidden: the pin's 406 with a text/plain body. */
    CF_REQUIRE(wire_request(&s,
                            "Accept-Encoding: identity;q=0, gzip;q=0\r\n",
                            resp, sizeof resp, NULL));
    CF_CHECK(strstr(resp, "HTTP/1.1 406") != NULL);
    CF_CHECK(strstr(resp, "Content-Type: text/plain") != NULL);
    CF_CHECK(strstr(resp, "An acceptable encoding") != NULL);

    srv_stop(&s);
}

static int count_occurrences(const char *hay, const char *needle) {
    int count = 0;
    size_t n = strlen(needle);
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += n) {
        count++;
    }
    return count;
}

/* CORE-04: the CF_REQUEST_SLOTS pool admits running + queued up to the bound
 * and declines the next request without losing a completion; both admitted
 * requests finish after the handler is released. */
CF_TEST(admission_slots_bound_running_plus_queued) {
    gate_reset(true);
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/slow", 2, action_blocking) == CF_OK);
    struct srv s;
    CF_REQUIRE(srv_start(&s, 1, 2, 1) == CF_OK); /* 1 reader, 2 slots */

    int fd1 = -1, fd2 = -1;
    CF_REQUIRE(send_request_no_read(&s, "/slow", &fd1) == 0);
    CF_REQUIRE(wait_gate_entered(1, 3000));

    CF_REQUIRE(send_request_no_read(&s, "/slow", &fd2) == 0);
    CF_REQUIRE(wait_admitted(s.app, 2, 3000)); /* 1 running + 1 queued */

    char resp[4096];
    CF_REQUIRE(do_request(&s, "/slow", NULL, resp, sizeof resp) == 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 503") != NULL);
    CF_CHECK(strstr(resp, "Retry-After: 1") != NULL);

    gate_release_all();
    CF_REQUIRE(srv_read_response(fd1, resp, sizeof resp, 8000) > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200") != NULL);
    CF_REQUIRE(srv_read_response(fd2, resp, sizeof resp, 8000) > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200") != NULL);
    close(fd1);
    close(fd2);

    /* both completions accounted, admission slots released */
    for (int i = 0; i < 400 && cf_app_completed_requests(s.app) < 2; i++) {
        usleep(5000);
    }
    CF_CHECK(cf_app_completed_requests(s.app) == 2);
    CF_CHECK(cf_app_admitted_count(s.app) == 0);

    srv_stop(&s);
    cf_http_counters counters;
    srv_counters(&s, &counters);
    /* H01 counts successful admissions and declines separately. */
    CF_CHECK(counters.admissions == 2);
    CF_CHECK(counters.admission_rejected == 1);
    CF_CHECK(counters.responses_sent >= 2);
}

/* CORE-03 through dispatch: a completion for a client that disconnected is
 * released without touching the descriptor, and a connection reusing the slot
 * receives only its own response. */
CF_TEST(closed_client_completion_does_not_touch_reused_slot) {
    gate_reset(true);
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/slow", 2, action_blocking) == CF_OK);
    struct srv s;
    CF_REQUIRE(srv_start(&s, 1, 2, 1) == CF_OK);

    int fd1 = -1;
    CF_REQUIRE(send_request_no_read(&s, "/slow", &fd1) == 0);
    CF_REQUIRE(wait_gate_entered(1, 3000));

    /* The first client goes away while its handler is still running; the
     * second connection reuses the freed slot/generation. */
    close(fd1);
    usleep(50000);

    int fd2 = -1;
    CF_REQUIRE(send_request_no_read(&s, "/slow", &fd2) == 0);
    CF_REQUIRE(wait_admitted(s.app, 2, 3000));

    gate_release_all(); /* first task completes stale, then the second runs */
    char resp[4096];
    ssize_t n = srv_read_response(fd2, resp, sizeof resp, 8000);
    CF_REQUIRE(n > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200") != NULL);
    CF_CHECK(strstr(resp, "\r\n\r\nok") != NULL);
    /* exactly one response on the reused slot */
    CF_CHECK(count_occurrences(resp, "HTTP/1.1") == 1);
    close(fd2);

    for (int i = 0; i < 400 && cf_app_completed_requests(s.app) < 2; i++) {
        usleep(5000);
    }
    CF_CHECK(cf_app_completed_requests(s.app) == 2);
    srv_stop(&s);
}

struct releaser_arg {
    int delay_ms;
};

static void *delayed_release(void *arg) {
    struct releaser_arg *ra = arg;
    usleep((useconds_t)ra->delay_ms * 1000);
    gate_release_all();
    return NULL;
}

/* Shutdown drain: cf_http_loop_stop waits for the admitted (running) task;
 * the response still reaches the client before run() returns. */
CF_TEST(shutdown_drains_admitted_task) {
    gate_reset(true);
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/slow", 2, action_blocking) == CF_OK);
    struct srv s;
    CF_REQUIRE(srv_start(&s, 1, 1, 1) == CF_OK);

    int fd1 = -1;
    CF_REQUIRE(send_request_no_read(&s, "/slow", &fd1) == 0);
    CF_REQUIRE(wait_gate_entered(1, 3000));

    struct releaser_arg ra = {150};
    pthread_t releaser;
    CF_REQUIRE(pthread_create(&releaser, NULL, delayed_release, &ra) == 0);
    cf_http_loop_stop(s.loop);
    CF_REQUIRE(pthread_join(s.thread, NULL) == 0);
    s.running = false;
    CF_REQUIRE(pthread_join(releaser, NULL) == 0);

    char resp[4096];
    ssize_t n = srv_read_response(fd1, resp, sizeof resp, 2000);
    CF_REQUIRE(n > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200") != NULL);
    close(fd1);
    srv_stop(&s);
}

/* Set-Cookie output on the wire: Rails jar semantics (unchanged values write
 * nothing, deletes only when present, Rack spelling and escaping, permanent
 * expiry from the injected clock, secure dropped on plain HTTP). */
CF_TEST(set_cookie_headers_on_the_wire) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/", 3, action_cookie) == CF_OK);
    struct srv s;
    CF_REQUIRE(srv_start(&s, 1, 4, 1) == CF_OK);
    cf_test_clock_set_fixed_us(INT64_C(1717243200000000)); /* 2024-06-01Z */

    char resp[4096];
    cookie_mode = 0;
    CF_REQUIRE(do_request(&s, "/", NULL, resp, sizeof resp) == 0);
    CF_CHECK(strstr(resp,
                    "Set-Cookie: last_room=42; path=/; "
                    "expires=Wed, 01 Jun 2044 12:00:00 GMT; samesite=lax") !=
             NULL);

    cookie_mode = 1;
    CF_REQUIRE(do_request(&s, "/", "Cookie: last_room=42\r\n", resp,
                          sizeof resp) == 0);
    CF_CHECK(strstr(resp, "Set-Cookie") == NULL);

    cookie_mode = 2;
    CF_REQUIRE(do_request(&s, "/", "Cookie: session_token=abc\r\n", resp,
                          sizeof resp) == 0);
    CF_CHECK(strstr(resp,
                    "Set-Cookie: session_token=; path=/; max-age=0; "
                    "expires=Thu, 01 Jan 1970 00:00:00 GMT; samesite=lax") !=
             NULL);

    cookie_mode = 3;
    CF_REQUIRE(do_request(&s, "/", NULL, resp, sizeof resp) == 0);
    CF_CHECK(strstr(resp, "Set-Cookie: x=a+b%2Bc%2F%3D; path=/; samesite=lax") !=
             NULL);

    cookie_mode = 4;
    CF_REQUIRE(do_request(&s, "/", NULL, resp, sizeof resp) == 0);
    CF_CHECK(strstr(resp, "Set-Cookie") == NULL);

    cookie_mode = 5;
    CF_REQUIRE(do_request(&s, "/", NULL, resp, sizeof resp) == 0);
    CF_CHECK(strstr(resp,
                    "Set-Cookie: e=1; path=/; "
                    "expires=Wed, 01 Jun 2044 12:00:00 GMT; samesite=lax") !=
             NULL);

    cookie_mode = 6;
    CF_REQUIRE(do_request(&s, "/", NULL, resp, sizeof resp) == 0);
    CF_CHECK(strstr(resp,
                    "Set-Cookie: opt=v; domain=example.com; path=/x; "
                    "httponly; samesite=strict; partitioned") != NULL);

    cookie_mode = 7;
    CF_REQUIRE(do_request(&s, "/", "Cookie: session_token=abc\r\n", resp,
                          sizeof resp) == 0);
    CF_CHECK(strstr(resp,
                    "Set-Cookie: session_token=; path=/x; max-age=0; "
                    "expires=Thu, 01 Jan 1970 00:00:00 GMT; samesite=none") !=
             NULL);

    cf_test_clock_clear();
    srv_stop(&s);
}

CF_TEST_MAIN()
