/* H01 limit tests (07-verification.md HTTP-05, HTTP-08): header/body/target
 * and connection budgets reject at the boundary without unbounded
 * allocation; Host/origin enforcement plus ignored proxy headers cannot
 * spoof peer or TLS status. */
#include "cf.h"
#include "cf_test.h"
#include "http/http.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
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
void htest_finish(struct htest *t, cf_http_counters *out);
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

CF_TEST(limits_header_cap_and_count) {
    struct htest_echo *st = echo_new();
    struct htest *t = htest_start(htest_echo_admit, st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char resp[16384];

    /* Exactly 100 headers (Host included) is accepted. */
    int fd = htest_connect(port, 0);
    CF_REQUIRE(fd >= 0);
    char req[4096];
    int n = snprintf(req, sizeof req, "GET /h100 HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n",
                     port);
    for (int i = 1; i <= 99; i++) {
        n += snprintf(req + n, sizeof req - (size_t)n, "X-N%d: v\r\n", i);
    }
    memcpy(req + n, "\r\n", 3);
    CF_CHECK(htest_send_all(fd, req, (size_t)n + 2) == 0);
    CF_CHECK(htest_read_response(fd, resp, sizeof resp, 5000) > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    close(fd);

    /* 101 headers: header-count cap. */
    n = snprintf(req, sizeof req, "GET /h101 HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n",
                 port);
    for (int i = 1; i <= 100; i++) {
        n += snprintf(req + n, sizeof req - (size_t)n, "X-N%d: v\r\n", i);
    }
    memcpy(req + n, "\r\n", 3);
    expect_status(t, (const unsigned char *)req, (size_t)n + 2,
                  "HTTP/1.1 431 Request Header Fields Too Large", 1);

    /* Header bytes over 32 KiB: 431 before unbounded growth. */
    char big[40000];
    big[0] = '\0';
    size_t off = (size_t)snprintf(big, sizeof big,
                                  "GET /big HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
                                  "X-Big: ",
                                  port);
    memset(big + off, 'a', 33000);
    memcpy(big + off + 33000, "\r\n\r\n", 5);
    expect_status(t, (const unsigned char *)big, off + 33000 + 4,
                  "HTTP/1.1 431 Request Header Fields Too Large", 1);

    htest_stop(t);
    echo_free(st);
}

CF_TEST(limits_body_cap) {
    struct htest_echo *st = echo_new();
    struct htest *t = htest_start(htest_echo_admit, st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char req[512];

    /* Content-Length one over the 16 MiB cap: 413 before reading a body. */
    snprintf(req, sizeof req,
             "POST /over HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
             "Content-Length: 16777217\r\n\r\n",
             port);
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 413 Content Too Large", 1);

    /* Exactly 16 MiB is accepted (boundary). */
    int fd = htest_connect(port, 0);
    CF_REQUIRE(fd >= 0);
    snprintf(req, sizeof req,
             "POST /exact HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
             "Content-Length: 16777216\r\n\r\n",
             port);
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    unsigned char *block = malloc(1u << 20);
    CF_REQUIRE(block != NULL);
    memset(block, 'A', 1u << 20);
    for (int i = 0; i < 16; i++) {
        CF_CHECK(htest_send_all(fd, block, 1u << 20) == 0);
    }
    free(block);
    char resp[4096];
    CF_CHECK(htest_read_response(fd, resp, sizeof resp, 20000) > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    pthread_mutex_lock(&st->mutex);
    CF_CHECK(st->body_len == (size_t)16777216);
    pthread_mutex_unlock(&st->mutex);
    close(fd);

    htest_stop(t);
    echo_free(st);
}

/* Send chunked data while watching for the response, so a mid-stream 413 can
 * be observed without a blocked send or an RST losing the reply. */
static ssize_t chunked_over_send_and_read(unsigned port,
                                          const char *chunk_header,
                                          size_t total, char *resp,
                                          size_t cap, int timeout_ms) {
    int fd = htest_connect(port, 0);
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (htest_send_all(fd, chunk_header, strlen(chunk_header)) != 0) {
        close(fd);
        return -1;
    }
    unsigned char block[65536];
    memset(block, 'B', sizeof block);
    size_t sent = 0;
    size_t have = 0;
    size_t header_end = 0;
    long long clen = -1;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t deadline =
        (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + timeout_ms;
    for (;;) {
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        int64_t now = (int64_t)now_ts.tv_sec * 1000 + now_ts.tv_nsec / 1000000;
        if (now >= deadline) break;
        struct pollfd pf = {fd, POLLIN | POLLOUT, 0};
        int rc = poll(&pf, 1, 50);
        if (rc < 0 && errno != EINTR) break;
        if (rc > 0 && (pf.revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = recv(fd, resp + have, cap - 1 - have, 0);
            if (n > 0) {
                have += (size_t)n;
                resp[have] = '\0';
                if (header_end == 0) {
                    char *h = memmem(resp, have, "\r\n\r\n", 4);
                    if (h != NULL) {
                        header_end = (size_t)(h - resp) + 4;
                        char *cl = memmem(resp, header_end,
                                          "Content-Length:", 15);
                        clen = cl != NULL ? strtoll(cl + 15, NULL, 10) : 0;
                    }
                }
                if (header_end != 0 &&
                    have >= header_end + (size_t)(clen < 0 ? 0 : clen)) {
                    close(fd);
                    return (ssize_t)have;
                }
            } else if (n == 0) {
                close(fd);
                return have > 0 ? (ssize_t)have : -1;
            }
        }
        if (rc > 0 && (pf.revents & POLLOUT) && sent < total) {
            size_t want = total - sent;
            if (want > sizeof block) want = sizeof block;
            ssize_t n = send(fd, block, want, MSG_NOSIGNAL);
            if (n > 0) {
                sent += (size_t)n;
            } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                close(fd);
                return have > 0 ? (ssize_t)have : -1;
            }
        }
    }
    close(fd);
    return have > 0 ? (ssize_t)have : -1;
}

CF_TEST(limits_chunked_expansion) {
    struct htest_echo *st = echo_new();
    struct htest *t = htest_start(htest_echo_admit, st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);

    char head[512];
    snprintf(head, sizeof head,
             "POST /expand HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
             "Transfer-Encoding: chunked\r\n\r\n"
             "1000001\r\n", /* 16777217 decoded bytes declared */
             port);
    char resp[8192];
    ssize_t n = chunked_over_send_and_read(
        port, head, (size_t)16777217, resp, sizeof resp, 20000);
    CF_CHECK(n > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 413 Content Too Large") != NULL);

    pthread_mutex_lock(&st->mutex);
    CF_CHECK(st->admits == 0); /* over-limit expansion never reaches a handler */
    pthread_mutex_unlock(&st->mutex);

    htest_stop(t);
    echo_free(st);
}

CF_TEST(limits_connection_cap) {
    struct htest_echo *st = echo_new();
    struct htest *t = htest_start(htest_echo_admit, st, 2, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char req[256], resp[4096];
    snprintf(req, sizeof req, "GET /one HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
             port);

    int fd1 = htest_connect(port, 0);
    int fd2 = htest_connect(port, 0);
    CF_REQUIRE(fd1 >= 0 && fd2 >= 0);
    CF_CHECK(htest_send_all(fd1, req, strlen(req)) == 0);
    CF_CHECK(htest_send_all(fd2, req, strlen(req)) == 0);
    CF_CHECK(htest_read_response(fd1, resp, sizeof resp, 5000) > 0);
    CF_CHECK(htest_read_response(fd2, resp, sizeof resp, 5000) > 0);

    /* Third connection: refused at accept, closed without a response. */
    int fd3 = htest_connect(port, 0);
    CF_REQUIRE(fd3 >= 0);
    CF_CHECK(htest_send_all(fd3, req, strlen(req)) == 0);
    ssize_t eof_n = htest_read_to_eof(fd3, resp, sizeof resp, 5000);
    CF_CHECK(eof_n == 0 || (eof_n < 0 && errno == ECONNRESET));
    close(fd3);

    /* Releasing a slot lets the next connection through. */
    close(fd2);
    int served = 0;
    for (int attempt = 0; attempt < 200 && !served; attempt++) {
        int fd = htest_connect(port, 0);
        if (fd < 0) continue;
        if (htest_send_all(fd, req, strlen(req)) == 0 &&
            htest_read_response(fd, resp, sizeof resp, 500) > 0 &&
            strstr(resp, "HTTP/1.1 200 OK") != NULL) {
            served = 1;
        }
        close(fd);
    }
    CF_CHECK(served == 1);
    close(fd1);

    cf_http_counters counters;
    htest_finish(t, &counters);
    CF_CHECK(counters.rejected >= 1);
    echo_free(st);
}

CF_TEST(limits_input_budget_503) {
    struct htest_echo *st = echo_new();
    struct htest *t = htest_start(htest_echo_admit, st, 0, 16, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char req[256];
    snprintf(req, sizeof req, "GET /budget HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
             port);
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 503 Service Unavailable", 1);
    pthread_mutex_lock(&st->mutex);
    CF_CHECK(st->admits == 0);
    pthread_mutex_unlock(&st->mutex);

    cf_http_counters counters;
    htest_finish(t, &counters);
    CF_CHECK(counters.budget_rejected >= 1);
    echo_free(st);
}

/* Respond with a 1 MiB body regardless of the request. */
static cf_err big_admit(void *user, cf_http_task *task) {
    (void)user;
    cf_buf *body = NULL;
    unsigned char *bytes = malloc(1u << 20);
    if (bytes == NULL) return CF_NOMEM;
    memset(bytes, 'C', 1u << 20);
    cf_err rc = cf_buf_copy((cf_span){bytes, 1u << 20}, &body);
    free(bytes);
    if (rc != CF_OK) return rc;
    cf_response resp;
    cf_response_init(&resp);
    resp.status = 200;
    rc = cf_response_body(&resp, body);
    cf_buf_release(body);
    if (rc == CF_OK) rc = cf_http_task_submit(task, &resp);
    cf_response_dispose(&resp);
    return rc == CF_OK ? CF_OK : CF_BUSY;
}

CF_TEST(limits_output_budget_503) {
    struct htest *t = htest_start(big_admit, NULL, 0, 0, 4096, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char req[256];
    snprintf(req, sizeof req, "GET /bigresp HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
             port);
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 503 Service Unavailable", 1);

    cf_http_counters counters;
    htest_finish(t, &counters);
    CF_CHECK(counters.budget_rejected >= 1);
}

/* Respond with a 9 MiB body: over the 8 MiB pending-output budget. */
static cf_err huge_admit(void *user, cf_http_task *task) {
    (void)user;
    size_t len = 9u * 1024 * 1024;
    unsigned char *bytes = malloc(len);
    if (bytes == NULL) return CF_NOMEM;
    memset(bytes, 'D', len);
    cf_buf *body = NULL;
    cf_err rc = cf_buf_copy((cf_span){bytes, len}, &body);
    free(bytes);
    if (rc != CF_OK) return rc;
    cf_response resp;
    cf_response_init(&resp);
    resp.status = 200;
    rc = cf_response_body(&resp, body);
    cf_buf_release(body);
    if (rc == CF_OK) rc = cf_http_task_submit(task, &resp);
    cf_response_dispose(&resp);
    return rc == CF_OK ? CF_OK : CF_BUSY;
}

CF_TEST(limits_per_connection_output_cap_503) {
    struct htest *t = htest_start(huge_admit, NULL, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char req[256];
    snprintf(req, sizeof req, "GET /huge HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
             port);
    expect_status(t, (const unsigned char *)req, strlen(req),
                  "HTTP/1.1 503 Service Unavailable", 1);
    cf_http_counters counters;
    htest_finish(t, &counters);
    CF_CHECK(counters.budget_rejected >= 1);
}

CF_TEST(limits_target_boundary_exact) {
    struct htest_echo *st = echo_new();
    struct htest *t = htest_start(htest_echo_admit, st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);

    char *target = malloc(8193);
    CF_REQUIRE(target != NULL);
    target[0] = '/';
    memset(target + 1, 't', 8191); /* target length exactly 8192 */
    target[8192] = '\0';
    char *req = malloc(8700);
    CF_REQUIRE(req != NULL);
    snprintf(req, 8700, "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n", target,
             port);
    int fd = htest_connect(port, 0);
    CF_REQUIRE(fd >= 0);
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    char resp[4096];
    CF_CHECK(htest_read_response(fd, resp, sizeof resp, 5000) > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    close(fd);
    free(target);
    free(req);

    htest_stop(t);
    echo_free(st);
}

CF_TEST(limits_ignored_proxy_headers) {
    struct htest_echo *st = echo_new();
    struct htest *t = htest_start(htest_echo_admit, st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);

    int fd = htest_connect(port, 0);
    CF_REQUIRE(fd >= 0);
    char req[1024];
    snprintf(req, sizeof req,
             "GET /real HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
             "Forwarded: for=9.9.9.9;proto=https;host=evil.example\r\n"
             "X-Forwarded-For: 9.9.9.9\r\n"
             "X-Forwarded-Proto: https\r\n"
             "X-Forwarded-Host: evil.example\r\n"
             "X-Real-IP: 9.9.9.9\r\n\r\n",
             port);
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    char resp[4096];
    CF_CHECK(htest_read_response(fd, resp, sizeof resp, 5000) > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    pthread_mutex_lock(&st->mutex);
    CF_CHECK(strcmp(st->peer, "127.0.0.1") == 0); /* real socket peer */
    CF_CHECK(st->tls == false);                   /* listener owns scheme */
    CF_CHECK(strcmp(st->target, "/real") == 0);
    pthread_mutex_unlock(&st->mutex);
    close(fd);

    htest_stop(t);
    echo_free(st);
}

CF_TEST_MAIN()
