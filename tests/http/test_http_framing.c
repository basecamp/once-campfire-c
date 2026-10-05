/* H01 framing tests (07-verification.md HTTP-02): TE+CL, repeated/overflowed
 * Content-Length, obs-fold, invalid field names/NUL, unsupported transfer
 * codings and chunked decoding errors reject with the specified status and
 * close; valid chunked bodies decode with bounded trailers that cannot
 * override headers. */
#include "cf.h"
#include "cf_test.h"
#include "http/http.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- tests/http/test_http_common.c (repeat prototypes per owned file) -- */
struct htest;
struct htest *htest_start(cf_http_admit_fn admit, void *user, size_t conn_cap,
                          size_t input_bytes, size_t output_bytes,
                          const char *origin_override);
void htest_stop(struct htest *t);
unsigned htest_port(const struct htest *t);
int htest_connect(unsigned port, int rcvbuf);
int htest_send_all(int fd, const void *buf, size_t len);
ssize_t htest_read_response(int fd, char *buf, size_t cap, int timeout_ms);
ssize_t htest_read_to_eof(int fd, char *buf, size_t cap, int timeout_ms);

/* Must match tests/http/test_http_common.c's layout. */
struct htest_echo {
    pthread_mutex_t mutex;
    unsigned admits;
    size_t body_len;
    char method[16];
    char target[512];
    char body[512];
    char peer[64];
    bool tls;
};

cf_err htest_echo_admit(void *user, cf_http_task *task);

static struct htest_echo *echo_new(void) {
    struct htest_echo *st = calloc(1, sizeof *st);
    pthread_mutex_init(&st->mutex, NULL);
    return st;
}

static void echo_free(struct htest_echo *st) {
    pthread_mutex_destroy(&st->mutex);
    free(st);
}

/* Send raw bytes, require a status line, optionally require a close. */
static void expect_status(struct htest *t, const unsigned char *raw,
                          size_t raw_len, const char *status, int closed) {
    int fd = htest_connect(htest_port(t), 0);
    CF_REQUIRE(fd >= 0);
    CF_CHECK(htest_send_all(fd, raw, raw_len) == 0);
    char resp[16384];
    ssize_t n = htest_read_response(fd, resp, sizeof resp, 5000);
    CF_CHECK(n > 0);
    CF_CHECK(strstr(resp, status) != NULL);
    if (closed) {
        char tail[8];
        CF_CHECK(htest_read_to_eof(fd, tail, sizeof tail, 5000) == 0);
    }
    close(fd);
}

CF_TEST(framing_host_rules) {
    struct htest_echo *st = echo_new();
    struct htest *t = htest_start(htest_echo_admit, st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char req[512];

    snprintf(req, sizeof req, "GET / HTTP/1.1\r\n\r\n"); /* no Host */
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 400 Bad Request", 1);

    snprintf(req, sizeof req, "GET / HTTP/1.1\r\nHost: evil.example\r\n\r\n");
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 400 Bad Request", 1);

    /* Origin carries an explicit port; a portless Host is inconsistent. */
    snprintf(req, sizeof req, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 400 Bad Request", 1);

    snprintf(req, sizeof req,
             "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n");
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 400 Bad Request", 1);

    /* Matching Host is accepted (with explicit port). */
    snprintf(req, sizeof req, "GET /ok HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
             port);
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 200 OK", 0);

    /* Bracketed literal compared against the origin host (here a reg-name);
     * the bracket only delimits the optional port. */
    snprintf(req, sizeof req, "GET / HTTP/1.1\r\nHost: [::1]:%u\r\n\r\n", port);
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 400 Bad Request", 1);
    snprintf(req, sizeof req,
             "GET / HTTP/1.1\r\nHost: [127.0.0.2]:%u\r\n\r\n", port);
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 400 Bad Request", 1);
    snprintf(req, sizeof req,
             "GET / HTTP/1.1\r\nHost: [127.0.0.1]\r\n\r\n");
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 400 Bad Request", 1); /* portless vs explicit */

    /* HTTP/1.0: Host optional; when present it must still match. */
    snprintf(req, sizeof req, "GET / HTTP/1.0\r\n\r\n");
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 200 OK", 1);
    snprintf(req, sizeof req, "GET / HTTP/1.0\r\nHost: evil.example\r\n\r\n");
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 400 Bad Request", 1);

    htest_stop(t);
    echo_free(st);
}

CF_TEST(framing_field_names_obs_fold_and_nul) {
    struct htest_echo *st = echo_new();
    struct htest *t = htest_start(htest_echo_admit, st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);

    char req[1024];
    snprintf(req, sizeof req,
             "GET / HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nX-A: one\r\n two\r\n\r\n",
             port);
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 400 Bad Request", 1);

    snprintf(req, sizeof req,
             "GET / HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nBad Name: x\r\n\r\n",
             port);
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 400 Bad Request", 1);

    snprintf(req, sizeof req,
             "GET / HTTP/1.1\r\nHost : 127.0.0.1:%u\r\n\r\n", port);
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 400 Bad Request", 1);

    /* NUL inside a header value. */
    char raw[256];
    int n = snprintf(raw, sizeof raw,
                     "GET / HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nX-A: a", port);
    raw[n++] = '\0';
    memcpy(raw + n, "b\r\n\r\n", 6);
    expect_status(t, (const unsigned char *)raw, (size_t)n + 6,
                  "HTTP/1.1 400 Bad Request", 1);

    htest_stop(t);
    echo_free(st);
}

CF_TEST(framing_content_length_rules) {
    struct htest_echo *st = echo_new();
    struct htest *t = htest_start(htest_echo_admit, st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char req[1024];
    const unsigned char *cl_cases[] = {
        (const unsigned char *)"Content-Length: 5\r\nContent-Length: 6\r\n",
        (const unsigned char *)"Content-Length: 5\r\nContent-Length: 5\r\n",
        (const unsigned char *)"Content-Length: abc\r\n",
        (const unsigned char *)"Content-Length: +5\r\n",
        (const unsigned char *)"Content-Length: 5 5\r\n",
        (const unsigned char *)"Content-Length: 99999999999999999999\r\n",
    };
    for (size_t i = 0; i < sizeof cl_cases / sizeof cl_cases[0]; i++) {
        int n = snprintf(req, sizeof req,
                         "POST /cl HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n",
                         port);
        size_t need = strlen((const char *)cl_cases[i]);
        memcpy(req + n, cl_cases[i], need);
        memcpy(req + n + need, "\r\n", 3);
        expect_status(t, (const unsigned char *)req,
                      (size_t)n + need + 2, "HTTP/1.1 400 Bad Request", 1);
    }

    /* An incomplete honest body never reaches a handler. */
    int fd = htest_connect(port, 0);
    CF_REQUIRE(fd >= 0);
    snprintf(req, sizeof req,
             "POST /partial HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
             "Content-Length: 5\r\n\r\nab",
             port);
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    CF_CHECK(shutdown(fd, SHUT_WR) == 0);
    char resp[256];
    CF_CHECK(htest_read_to_eof(fd, resp, sizeof resp, 5000) == 0);
    close(fd);
    pthread_mutex_lock(&st->mutex);
    CF_CHECK(st->admits == 0);
    pthread_mutex_unlock(&st->mutex);

    htest_stop(t);
    echo_free(st);
}

CF_TEST(framing_transfer_encoding_rules) {
    struct htest_echo *st = echo_new();
    struct htest *t = htest_start(htest_echo_admit, st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char req[1024];
    const char *cases[] = {
        "Transfer-Encoding: gzip\r\n",
        "Transfer-Encoding: chunked\r\nContent-Length: 4\r\n",
        "Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n",
        "Transfer-Encoding: chunked, chunked\r\n",
        "Transfer-Encoding: identity\r\n",
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        snprintf(req, sizeof req,
                 "POST /te HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n%s\r\n", port,
                 cases[i]);
        expect_status(t, (const unsigned char *)req, strlen(req),
                      "HTTP/1.1 400 Bad Request", 1);
    }
    htest_stop(t);
    echo_free(st);
}

CF_TEST(framing_targets) {
    struct htest_echo *st = echo_new();
    struct htest *t = htest_start(htest_echo_admit, st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char req[512];
    const char *targets[] = {"http://example.com/", "*", "/x#frag"};
    for (size_t i = 0; i < 3; i++) {
        snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
                 targets[i], port);
        expect_status(t, (const unsigned char *)req, strlen(req),
                      "HTTP/1.1 400 Bad Request", 1);
    }

    /* Over the 8 KiB target cap: 414 and close (boundary checked before
       growing storage). */
    char big[8300];
    big[0] = '/';
    memset(big + 1, 'a', 8193); /* target length 8194 */
    big[8194] = '\0';
    char *reqbig = malloc(8600);
    CF_REQUIRE(reqbig != NULL);
    snprintf(reqbig, 8600, "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n", big,
             port);
    expect_status(t, (const unsigned char *)reqbig, strlen(reqbig),
                  "HTTP/1.1 414 URI Too Long", 1);
    free(reqbig);

    htest_stop(t);
    echo_free(st);
}

CF_TEST(framing_chunked_decodes_and_ignores_trailers) {
    struct htest_echo *st = echo_new();
    struct htest *t = htest_start(htest_echo_admit, st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);

    int fd = htest_connect(port, 0);
    CF_REQUIRE(fd >= 0);
    char req[1024];
    snprintf(req, sizeof req,
             "POST /chunk HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
             "Transfer-Encoding: chunked\r\n\r\n"
             "4;ext=1\r\nWiki\r\n5\r\npedia\r\n0\r\n"
             "X-Trailer: yes\r\nHost: evil.example\r\n\r\n",
             port);
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    char resp[4096];
    ssize_t n = htest_read_response(fd, resp, sizeof resp, 5000);
    CF_CHECK(n > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    pthread_mutex_lock(&st->mutex);
    CF_CHECK(st->admits == 1);
    CF_CHECK(st->body_len == 9);
    CF_CHECK(strcmp(st->body, "Wikipedia") == 0);
    CF_CHECK(strcmp(st->target, "/chunk") == 0); /* trailer cannot override */
    pthread_mutex_unlock(&st->mutex);
    close(fd);

    htest_stop(t);
    echo_free(st);
}

CF_TEST(framing_chunked_errors) {
    struct htest_echo *st = echo_new();
    struct htest *t = htest_start(htest_echo_admit, st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char req[512];

    /* Invalid chunk size. */
    snprintf(req, sizeof req,
             "POST / HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
             "Transfer-Encoding: chunked\r\n\r\nzz\r\n",
             port);
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 400 Bad Request", 1);

    /* Chunk-size line longer than any size_t. */
    snprintf(req, sizeof req,
             "POST / HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
             "Transfer-Encoding: chunked\r\n\r\n"
             "11111111111111111\r\n",
             port);
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 400 Bad Request", 1);

    /* Missing CRLF after chunk data. */
    snprintf(req, sizeof req,
             "POST / HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
             "Transfer-Encoding: chunked\r\n\r\n4\r\nWikiXX",
             port);
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 400 Bad Request", 1);

    /* Trailer section over the bounded trailer size: 431. */
    {
        unsigned char *big = malloc(1024 + 9000);
        CF_REQUIRE(big != NULL);
        int n = snprintf((char *)big, 1024,
                         "POST / HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
                         "Transfer-Encoding: chunked\r\n\r\n0\r\n",
                         port);
        memset(big + n, 'x', 9000); /* no CRLF: bounded trailer bytes */
        expect_status(t, big, (size_t)n + 9000,
                      "HTTP/1.1 431 Request Header Fields Too Large", 1);
        free(big);
    }

    pthread_mutex_lock(&st->mutex);
    CF_CHECK(st->admits == 0);
    pthread_mutex_unlock(&st->mutex);

    htest_stop(t);
    echo_free(st);
}

CF_TEST_MAIN()
