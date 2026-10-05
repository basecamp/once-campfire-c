/* A00/H03 integration wiring: the static front mount runs on the request
 * worker before cf_ctx_process (src/app.c), unmatched routes answer the
 * reference public 404 (src/context.c), and a request without an Accept
 * header survives format negotiation end to end (A00-verify §8).
 *
 * Unlike tests/app/test_serve.c this case links the real 177-row table
 * (src/routes.c), so the whole request path under test is the production
 * dispatch, driven through the real H01 loop by the serve harness. */
#include "cf_test.h"

#include "app.h"
#include "cf.h"
#include "config.h"

#include "support/serve_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HEALTH_HTML \
    "<!DOCTYPE html><html><body style=\"background-color: green\"></body></html>"

#define DIGESTED_JS "/assets/application-a54c74a7.js"
#define DIGESTED_JS_FILE \
    "tests/fixtures/assets/public/assets/application-a54c74a7.js"
#define NOT_FOUND_FILE "tests/fixtures/assets/public/404.html"

/* Send one raw request and return the full response length (headers+body). */
static ssize_t fetch(struct srv *s, const char *raw, char *resp, size_t cap) {
    int fd = srv_connect(s);
    if (fd < 0) return -1;
    if (srv_send_all(fd, raw, strlen(raw)) != 0) {
        close(fd);
        return -1;
    }
    ssize_t n = srv_read_response(fd, resp, cap, 5000);
    close(fd);
    return n;
}

static ssize_t get(struct srv *s, const char *path, const char *headers,
                   char *resp, size_t cap) {
    char raw[1024];
    int n = snprintf(raw, sizeof raw,
                     "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n%s"
                     "Connection: close\r\n\r\n",
                     path, s->port, headers != NULL ? headers : "");
    CF_REQUIRE(n > 0 && (size_t)n < sizeof raw);
    return fetch(s, raw, resp, cap);
}

/* Body start + length, or NULL on a malformed response. */
static const char *response_body(const char *resp, ssize_t total,
                                 size_t *body_len) {
    const char *h = strstr(resp, "\r\n\r\n");
    if (h == NULL) return NULL;
    const char *body = h + 4;
    ssize_t header_len = body - resp;
    if (header_len > total) return NULL;
    *body_len = (size_t)(total - header_len);
    return body;
}

static unsigned char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    unsigned char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        return NULL;
    }
    *len = got;
    return buf;
}

static void expect_body_literal(const char *resp, ssize_t total,
                                const char *expected) {
    size_t got_len = 0;
    const char *got = response_body(resp, total, &got_len);
    CF_REQUIRE(got != NULL);
    size_t want_len = strlen(expected);
    CF_CHECK(got_len == want_len);
    CF_CHECK(want_len == 0 || memcmp(got, expected, want_len) == 0);
}

static void expect_fixture(const char *resp, ssize_t total,
                           const char *fixture) {
    size_t want_len = 0;
    unsigned char *want = read_file(fixture, &want_len);
    CF_REQUIRE(want != NULL); /* a missing pinned fixture fails the case */
    size_t got_len = 0;
    const char *got = response_body(resp, total, &got_len);
    CF_REQUIRE(got != NULL);
    CF_CHECK(got_len == want_len);
    CF_CHECK(want_len == 0 || memcmp(got, want, want_len) == 0);
    free(want);
}

CF_TEST(static_asset_is_served_by_the_front_mount) {
    struct srv s;
    CF_REQUIRE(srv_start(&s, 1, 4, 1) == CF_OK);
    char resp[16384];
    ssize_t n = get(&s, DIGESTED_JS, NULL, resp, sizeof resp);
    CF_REQUIRE(n > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200") != NULL);
    CF_CHECK(strstr(resp, "Content-Type: text/javascript") != NULL);
    CF_CHECK(strstr(resp, "Cache-Control: public, max-age=2592000") != NULL);
    expect_fixture(resp, n, DIGESTED_JS_FILE);

    /* The front mount is GET/HEAD only: a POST to the same path falls
     * through to the application and its unmatched-route 404. */
    char raw[512];
    snprintf(raw, sizeof raw,
             "POST %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
             "Connection: close\r\n\r\n", DIGESTED_JS, s.port);
    n = fetch(&s, raw, resp, sizeof resp);
    CF_REQUIRE(n > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 404") != NULL);
    expect_fixture(resp, n, NOT_FOUND_FILE);
    srv_stop(&s);
}

CF_TEST(unmatched_route_serves_the_reference_404_body) {
    struct srv s;
    CF_REQUIRE(srv_start(&s, 1, 4, 1) == CF_OK);
    char resp[16384];
    ssize_t n = get(&s, "/nope", NULL, resp, sizeof resp);
    CF_REQUIRE(n > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 404") != NULL);
    CF_CHECK(strstr(resp, "Content-Type: text/html; charset=UTF-8") != NULL);
    expect_fixture(resp, n, NOT_FOUND_FILE);
    srv_stop(&s);
}

CF_TEST(health_without_accept_header_does_not_crash) {
    struct srv s;
    CF_REQUIRE(srv_start(&s, 1, 4, 1) == CF_OK);
    char resp[4096];

    /* No Accept header at all (A00-verify §8 crash: valgrind uninitialized
     * span; SIGSEGV in the -O3/-flto build). */
    ssize_t n = get(&s, "/up", NULL, resp, sizeof resp);
    CF_REQUIRE(n > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200") != NULL);
    CF_CHECK(strstr(resp, "Content-Length: 73") != NULL);
    expect_body_literal(resp, n, HEALTH_HTML);

    /* An empty Accept value takes the same documented HTML fallback. */
    n = get(&s, "/up", "Accept: \r\n", resp, sizeof resp);
    CF_REQUIRE(n > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200") != NULL);

    /* The server is still alive and serving after both requests. */
    n = get(&s, "/up", "Accept: */*\r\n", resp, sizeof resp);
    CF_REQUIRE(n > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200") != NULL);
    srv_stop(&s);
}

CF_TEST_MAIN()
