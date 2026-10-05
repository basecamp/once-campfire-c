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

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

/* ---- Rack method override through the real 177-row table --------------- */

static int response_status(const char *resp) {
    int status = 0;
    if (sscanf(resp, "HTTP/1.1 %d", &status) != 1) return -1;
    return status;
}

/* First "Name: value" header line's value, or false when absent. */
static bool response_header_value(const char *resp, const char *name,
                                  char *out, size_t cap) {
    size_t nlen = strlen(name);
    const char *p = resp;
    while ((p = strstr(p, name)) != NULL) {
        if ((p == resp || p[-1] == '\n') && p[nlen] == ':' &&
            p[nlen + 1] == ' ') {
            const char *value = p + nlen + 2;
            const char *end = strstr(value, "\r\n");
            CF_REQUIRE(end != NULL);
            size_t n = (size_t)(end - value) < cap - 1
                           ? (size_t)(end - value)
                           : cap - 1;
            memcpy(out, value, n);
            out[n] = '\0';
            return true;
        }
        p += nlen;
    }
    return false;
}

/* One raw request with an optional urlencoded body and extra header lines. */
static bool build_request(struct srv *s, const char *method, const char *path,
                          const char *body, const char *content_type,
                          const char *extra_headers, char *raw, size_t cap) {
    char ct_line[160] = "";
    if (content_type != NULL) {
        snprintf(ct_line, sizeof ct_line, "Content-Type: %s\r\n",
                 content_type);
    }
    int n = snprintf(raw, cap,
                     "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n%s"
                     "Content-Length: %zu\r\n%s"
                     "Connection: close\r\n\r\n%s",
                     method, path, s->port, ct_line,
                     body != NULL ? strlen(body) : (size_t)0,
                     extra_headers != NULL ? extra_headers : "",
                     body != NULL ? body : "");
    return n > 0 && (size_t)n < cap;
}

static ssize_t send_request(struct srv *s, const char *method, const char *path,
                            const char *body, const char *content_type,
                            const char *extra_headers, char *resp,
                            size_t cap) {
    char raw[2048];
    CF_REQUIRE(build_request(s, method, path, body, content_type,
                             extra_headers, raw, sizeof raw));
    return fetch(s, raw, resp, cap);
}

/* Read until the server closes (every request here sends Connection: close).
 * srv_read_response cannot be used for a HEAD response: it waits for the
 * declared Content-Length body a HEAD response never sends. */
static ssize_t send_request_to_eof(struct srv *s, const char *method,
                                   const char *path, const char *body,
                                   const char *content_type,
                                   const char *extra_headers, char *resp,
                                   size_t cap) {
    char raw[2048];
    CF_REQUIRE(build_request(s, method, path, body, content_type,
                             extra_headers, raw, sizeof raw));
    int fd = srv_connect(s);
    if (fd < 0) return -1;
    if (srv_send_all(fd, raw, strlen(raw)) != 0) {
        close(fd);
        return -1;
    }
    size_t have = 0;
    for (int waited = 0; waited < 5000 && have + 1 < cap;) {
        struct pollfd pf = {fd, POLLIN, 0};
        int prc = poll(&pf, 1, 100);
        if (prc == 0) {
            waited += 100;
            continue;
        }
        if (prc < 0) {
            close(fd);
            return -1;
        }
        ssize_t n = recv(fd, resp + have, cap - 1 - have, 0);
        if (n > 0) {
            have += (size_t)n;
            continue;
        }
        if (n == 0) break; /* closed: Connection: close */
        if (errno == EINTR) continue;
        close(fd);
        return -1;
    }
    close(fd);
    resp[have] = '\0';
    return (ssize_t)have;
}

/* Status, Location and body byte-identical to the direct verb's response. */
static void expect_same_response(const char *direct, ssize_t direct_n,
                                 const char *over, ssize_t over_n,
                                 int status) {
    CF_CHECK(response_status(direct) == status);
    CF_CHECK(response_status(over) == status);
    char loc_direct[512], loc_over[512];
    CF_REQUIRE(response_header_value(direct, "Location", loc_direct,
                                     sizeof loc_direct));
    CF_REQUIRE(response_header_value(over, "Location", loc_over,
                                     sizeof loc_over));
    CF_CHECK(strcmp(loc_direct, loc_over) == 0);
    size_t direct_len = 0, over_len = 0;
    const char *direct_body = response_body(direct, direct_n, &direct_len);
    const char *over_body = response_body(over, over_n, &over_len);
    CF_REQUIRE(direct_body != NULL && over_body != NULL);
    CF_CHECK(direct_len == over_len);
    CF_CHECK(direct_len == 0 ||
             memcmp(direct_body, over_body, direct_len) == 0);
}

/* Replace the Date header value (the only clock-stamped header) so two
 * responses from the same server can be compared byte-for-byte. */
static void mask_date_header(char *head) {
    char *p = strstr(head, "Date: ");
    if (p == NULL) return;
    char *end = strstr(p, "\r\n");
    if (end != NULL) memset(p, 'D', (size_t)(end - p));
}

/* Status line, every header except the clock-stamped Date, and the body,
 * byte-for-byte (Content-Length included, as it appears in the header
 * block). */
static void expect_same_response_bytes(const char *direct, ssize_t direct_n,
                                       const char *over, ssize_t over_n) {
    const char *direct_end = strstr(direct, "\r\n\r\n");
    const char *over_end = strstr(over, "\r\n\r\n");
    CF_REQUIRE(direct_end != NULL && over_end != NULL);
    size_t direct_head = (size_t)(direct_end - direct) + 4;
    size_t over_head = (size_t)(over_end - over) + 4;
    CF_REQUIRE(direct_head < 8192 && over_head < 8192);
    char direct_buf[8192], over_buf[8192];
    memcpy(direct_buf, direct, direct_head);
    direct_buf[direct_head] = '\0';
    memcpy(over_buf, over, over_head);
    over_buf[over_head] = '\0';
    mask_date_header(direct_buf);
    mask_date_header(over_buf);
    CF_CHECK(direct_head == over_head);
    CF_CHECK(strcmp(direct_buf, over_buf) == 0);
    size_t direct_len = 0, over_len = 0;
    const char *direct_body = response_body(direct, direct_n, &direct_len);
    const char *over_body = response_body(over, over_n, &over_len);
    CF_REQUIRE(direct_body != NULL && over_body != NULL);
    CF_CHECK(direct_len == over_len);
    CF_CHECK(direct_len == 0 || memcmp(direct_body, over_body, direct_len) == 0);
}

/* An override to HEAD must serialize exactly like wire HEAD: routing picks
 * the GET row and the response keeps its Content-Length with no body. */
CF_TEST(method_override_to_head_serializes_like_wire_head) {
    struct srv s;
    CF_REQUIRE(srv_start(&s, 1, 4, 1) == CF_OK);
    char direct[16384], over[16384];

    /* GET row: /up renders 73 bytes; wire HEAD and POST+_method=HEAD both
     * answer 200, Content-Length: 73, no body. */
    ssize_t direct_n = send_request_to_eof(&s, "HEAD", "/up", "", NULL, NULL,
                                           direct, sizeof direct);
    ssize_t over_n = send_request_to_eof(
        &s, "POST", "/up", "_method=HEAD",
        "application/x-www-form-urlencoded", NULL, over, sizeof over);
    CF_REQUIRE(direct_n > 0 && over_n > 0);
    CF_CHECK(response_status(direct) == 200);
    CF_CHECK(response_status(over) == 200);
    CF_CHECK(strstr(direct, "Content-Length: 73\r\n") != NULL);
    CF_CHECK(strstr(over, "Content-Length: 73\r\n") != NULL);
    expect_same_response_bytes(direct, direct_n, over, over_n);
    size_t direct_body = 0, over_body = 0;
    response_body(direct, direct_n, &direct_body);
    response_body(over, over_n, &over_body);
    CF_CHECK(direct_body == 0);
    CF_CHECK(over_body == 0);

    /* POST-only row: HEAD matches no row (the matcher maps HEAD onto GET), so
     * the effective HEAD falls out of the 177-row table exactly like wire
     * HEAD — both the reference 404, no body. */
    const char *post_only = "/rails/action_mailbox/postmark/inbound_emails";
    direct_n = send_request_to_eof(&s, "HEAD", post_only, "", NULL, NULL,
                                   direct, sizeof direct);
    over_n = send_request_to_eof(&s, "POST", post_only, "_method=HEAD",
                                 "application/x-www-form-urlencoded", NULL,
                                 over, sizeof over);
    CF_REQUIRE(direct_n > 0 && over_n > 0);
    CF_CHECK(response_status(direct) == 404);
    CF_CHECK(response_status(over) == 404);
    expect_same_response_bytes(direct, direct_n, over, over_n);

    /* GET is never overridden: GET /up with a `_method=HEAD` body keeps the
     * plain GET response, 73-byte body included. */
    direct_n = send_request_to_eof(&s, "GET", "/up", "", NULL, NULL, direct,
                                   sizeof direct);
    over_n = send_request_to_eof(&s, "GET", "/up", "_method=HEAD",
                                 "application/x-www-form-urlencoded", NULL,
                                 over, sizeof over);
    CF_REQUIRE(direct_n > 0 && over_n > 0);
    CF_CHECK(response_status(direct) == 200);
    CF_CHECK(response_status(over) == 200);
    expect_same_response_bytes(direct, direct_n, over, over_n);
    response_body(over, over_n, &over_body);
    CF_CHECK(over_body == 73);

    srv_stop(&s);
}

/* V01's UI-form repros: POST + `_method` must reach the direct verb's row
 * with a byte-identical response (they were 404/401 before the override was
 * wired into dispatch). */
CF_TEST(method_override_matches_the_direct_verb_for_ui_forms) {
    struct srv s;
    CF_REQUIRE(srv_start(&s, 1, 4, 1) == CF_OK);
    char direct[16384], over[16384];

    /* Message edit form: POST /rooms/1/messages/1 _method=patch. Fresh app:
     * the chain redirects to sign-in before the action. Before the fix this
     * was an unmatched-route 404 against the direct PATCH's 302. */
    ssize_t direct_n = send_request(&s, "PATCH", "/rooms/1/messages/1", "",
                                    NULL, NULL, direct, sizeof direct);
    ssize_t over_n = send_request(
        &s, "POST", "/rooms/1/messages/1", "_method=patch",
        "application/x-www-form-urlencoded", NULL, over, sizeof over);
    CF_REQUIRE(direct_n > 0 && over_n > 0);
    CF_CHECK(response_status(direct) == 302);
    expect_same_response(direct, direct_n, over, over_n, 302);

    /* Logout form: POST /session _method=delete. Before the fix the POST row
     * (sessions#create) answered its 401 sign-in page instead. */
    direct_n = send_request(&s, "DELETE", "/session", "", NULL, NULL, direct,
                            sizeof direct);
    over_n = send_request(&s, "POST", "/session", "_method=delete",
                          "application/x-www-form-urlencoded", NULL, over,
                          sizeof over);
    CF_REQUIRE(direct_n > 0 && over_n > 0);
    CF_CHECK(response_status(direct) == 302);
    expect_same_response(direct, direct_n, over, over_n, 302);

    /* X-HTTP-Method-Override rides the same middleware. */
    over_n = send_request(&s, "POST", "/session", "", NULL,
                          "X-HTTP-Method-Override: delete\r\n", over,
                          sizeof over);
    CF_REQUIRE(over_n > 0);
    expect_same_response(direct, direct_n, over, over_n, 302);

    /* GET is never overridden: GET /session/new carrying _method=delete in
     * its body stays the GET row (sessions#new redirects to /first_run with
     * no users; a DELETE row would not match /session/new at all). */
    direct_n = send_request(&s, "GET", "/session/new", "", NULL, NULL, direct,
                            sizeof direct);
    over_n = send_request(&s, "GET", "/session/new", "_method=delete",
                          "application/x-www-form-urlencoded", NULL, over,
                          sizeof over);
    CF_REQUIRE(direct_n > 0 && over_n > 0);
    CF_CHECK(response_status(direct) == 302);
    expect_same_response(direct, direct_n, over, over_n, 302);

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
