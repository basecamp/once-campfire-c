/* A00 integration tests: full app + writer + request workers behind a real H01
 * loop. Covers CORE-03/CORE-04 through the dispatch path, admission-slot
 * boundaries, shutdown drain, and Set-Cookie output on the wire. */
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
#include <string.h>
#include <time.h>
#include <unistd.h>

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
