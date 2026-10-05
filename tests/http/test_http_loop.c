/* H01 loop tests: HTTP-01 (split headers/body, pipelined ordering), HTTP-04
 * (HEAD/204/304, Expect, keep-alive, half-close). Real loopback sockets and
 * a real loop thread; all waits are poll() based. */
#include "app.h"
#include "app_internal.h"
#include "cf.h"
#include "cf_test.h"
#include "config.h"
#include "http/http.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ---- tests/http/test_http_common.c (repeat prototypes per owned file) -- */
struct htest;
struct htest *htest_start(cf_http_admit_fn admit, void *user, size_t conn_cap,
                          size_t input_bytes, size_t output_bytes,
                          const char *origin_override);
void htest_stop(struct htest *t);
unsigned htest_port(const struct htest *t);
cf_http_loop *htest_loop(const struct htest *t);
int htest_listen(unsigned *port_out);
int htest_connect(unsigned port, int rcvbuf);
int htest_send_all(int fd, const void *buf, size_t len);
ssize_t htest_read_response(int fd, char *buf, size_t cap, int timeout_ms);
ssize_t htest_read_headers(int fd, char *buf, size_t cap, int timeout_ms);
ssize_t htest_read_to_eof(int fd, char *buf, size_t cap, int timeout_ms);
int htest_has_data(int fd, int timeout_ms);

/* Admission that answers 200 and echoes the request target as the body, or
 * returns a canned status for special targets. */
struct reply_state {
    pthread_mutex_t mutex;
    unsigned count;
    char last_target[64];
};

static cf_response make_reply(unsigned status, const char *body) {
    cf_response r;
    cf_response_init(&r);
    r.status = status;
    cf_buf *buf = NULL;
    if (cf_buf_copy((cf_span){(const unsigned char *)body, strlen(body)},
                    &buf) != CF_OK) {
        return r;
    }
    (void)cf_response_body(&r, buf);
    cf_buf_release(buf);
    (void)cf_response_header(&r,
                             (cf_span){(const unsigned char *)"Content-Type", 12},
                             (cf_span){(const unsigned char *)"text/plain", 10});
    return r;
}

static cf_err reply_admit(void *user, cf_http_task *task) {
    struct reply_state *st = user;
    const cf_request *req = cf_http_task_request(task);
    pthread_mutex_lock(&st->mutex);
    st->count++;
    size_t n = req->target.len < sizeof st->last_target - 1
                   ? req->target.len
                   : sizeof st->last_target - 1;
    memcpy(st->last_target, req->target.ptr, n);
    st->last_target[n] = '\0';
    pthread_mutex_unlock(&st->mutex);

    cf_response r;
    if (req->target.len == 5 && memcmp(req->target.ptr, "/head", 5) == 0) {
        r = make_reply(200, "hello");
    } else if (req->target.len == 5 &&
               memcmp(req->target.ptr, "/none", 5) == 0) {
        r = make_reply(204, "ignored-body");
    } else if (req->target.len == 3 &&
               memcmp(req->target.ptr, "/nm", 3) == 0) {
        r = make_reply(304, "ignored-body");
    } else {
        char tbuf[96];
        size_t m = req->target.len < sizeof tbuf - 1 ? req->target.len
                                                     : sizeof tbuf - 1;
        memcpy(tbuf, req->target.ptr, m);
        tbuf[m] = '\0';
        r = make_reply(200, tbuf); /* body echoes the target */
    }
    cf_err rc = cf_http_task_submit(task, &r);
    cf_response_dispose(&r);
    return rc == CF_OK ? CF_OK : CF_BUSY;
}

static void request(char *dst, size_t cap, const char *method,
                    const char *target, unsigned port, const char *extra) {
    snprintf(dst, cap,
             "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n%s\r\n", method, target,
             port, extra);
}

CF_TEST(loop01_split_headers_and_body) {
    struct reply_state st;
    memset(&st, 0, sizeof st);
    pthread_mutex_init(&st.mutex, NULL);
    struct htest *t = htest_start(reply_admit, &st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);

    int fd = htest_connect(port, 0);
    CF_REQUIRE(fd >= 0);
    char head[512];
    request(head, sizeof head, "POST", "/split", port,
            "Content-Length: 4\r\n");
    CF_CHECK(htest_send_all(fd, head, strlen(head)) == 0);
    /* Second write: the body arrives in a separate read. */
    CF_CHECK(htest_send_all(fd, "body", 4) == 0);

    char resp[4096];
    ssize_t n = htest_read_response(fd, resp, sizeof resp, 5000);
    CF_CHECK(n > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    CF_CHECK(strstr(resp, "Content-Length: 6") != NULL);
    CF_CHECK(strstr(resp, "Connection: keep-alive") != NULL);
    CF_CHECK(strstr(resp, "\r\n\r\n/split") != NULL);

    pthread_mutex_lock(&st.mutex);
    CF_CHECK(st.count == 1);
    pthread_mutex_unlock(&st.mutex);

    close(fd);
    htest_stop(t);
    pthread_mutex_destroy(&st.mutex);
}

CF_TEST(loop01_pipelined_ordering) {
    struct reply_state st;
    memset(&st, 0, sizeof st);
    pthread_mutex_init(&st.mutex, NULL);
    struct htest *t = htest_start(reply_admit, &st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);

    int fd = htest_connect(port, 0);
    CF_REQUIRE(fd >= 0);
    char req1[256], req2[256], both[640];
    request(req1, sizeof req1, "GET", "/two", port, "");
    request(req2, sizeof req2, "GET", "/three", port, "");
    snprintf(both, sizeof both, "%s%s", req1, req2);
    CF_CHECK(htest_send_all(fd, both, strlen(both)) == 0);

    char resp[4096];
    CF_CHECK(htest_read_response(fd, resp, sizeof resp, 5000) > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    CF_CHECK(strstr(resp, "\r\n\r\n/two") != NULL); /* first response */

    CF_CHECK(htest_read_response(fd, resp, sizeof resp, 5000) > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    CF_CHECK(strstr(resp, "\r\n\r\n/three") != NULL); /* second response */
    pthread_mutex_lock(&st.mutex);
    CF_CHECK(st.count == 2);
    pthread_mutex_unlock(&st.mutex);

    close(fd);
    htest_stop(t);
    pthread_mutex_destroy(&st.mutex);
}

CF_TEST(loop04_head_204_304_no_body) {
    struct reply_state st;
    memset(&st, 0, sizeof st);
    pthread_mutex_init(&st.mutex, NULL);
    struct htest *t = htest_start(reply_admit, &st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);

    int fd = htest_connect(port, 0);
    CF_REQUIRE(fd >= 0);
    char req[256], resp[4096];

    /* HEAD: GET headers, no body, and the connection stays usable. */
    request(req, sizeof req, "HEAD", "/head", port, "");
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    ssize_t n = htest_read_headers(fd, resp, sizeof resp, 5000);
    CF_REQUIRE(n > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    CF_CHECK(strstr(resp, "Content-Length: 5") != NULL);
    CF_CHECK(htest_has_data(fd, 150) == 0); /* no body bytes */

    request(req, sizeof req, "GET", "/plain", port, "");
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    n = htest_read_response(fd, resp, sizeof resp, 5000);
    CF_CHECK(n > 0);
    CF_CHECK(strstr(resp, "Content-Length: 6") != NULL);
    CF_CHECK(strstr(resp, "\r\n\r\n/plain") != NULL);

    /* 204 and 304: no body, no Content-Length, next request still works. */
    const char *paths[] = {"/none", "/nm"};
    const char *statuses[] = {"HTTP/1.1 204 No Content",
                              "HTTP/1.1 304 Not Modified"};
    for (size_t i = 0; i < 2; i++) {
        request(req, sizeof req, "GET", paths[i], port, "");
        CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
        n = htest_read_headers(fd, resp, sizeof resp, 5000);
        CF_REQUIRE(n > 0);
        CF_CHECK(strstr(resp, statuses[i]) != NULL);
        CF_CHECK(strstr(resp, "Content-Length") == NULL);
        CF_CHECK(htest_has_data(fd, 150) == 0);

        request(req, sizeof req, "GET", "/plain", port, "");
        CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
        n = htest_read_response(fd, resp, sizeof resp, 5000);
        CF_CHECK(n > 0 && strstr(resp, "\r\n\r\n/plain") != NULL);
    }

    close(fd);
    htest_stop(t);
    pthread_mutex_destroy(&st.mutex);
}

CF_TEST(loop04_expect_continue_and_417) {
    struct reply_state st;
    memset(&st, 0, sizeof st);
    pthread_mutex_init(&st.mutex, NULL);
    struct htest *t = htest_start(reply_admit, &st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char resp[4096];

    int fd = htest_connect(port, 0);
    CF_REQUIRE(fd >= 0);
    char req[512];
    request(req, sizeof req, "POST", "/expect", port,
            "Expect: 100-continue\r\nContent-Length: 4\r\n");
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    ssize_t n = htest_read_headers(fd, resp, sizeof resp, 5000);
    CF_REQUIRE(n > 0);
    CF_CHECK(strncmp(resp, "HTTP/1.1 100 Continue\r\n\r\n", 25) == 0);
    CF_CHECK(htest_send_all(fd, "data", 4) == 0);
    n = htest_read_response(fd, resp, sizeof resp, 5000);
    CF_CHECK(n > 0 && strstr(resp, "HTTP/1.1 200 OK") != NULL);
    close(fd);

    /* Any other expectation is 417 and closes. */
    int fd2 = htest_connect(port, 0);
    CF_REQUIRE(fd2 >= 0);
    request(req, sizeof req, "POST", "/expect", port,
            "Expect: 200-ok\r\nContent-Length: 0\r\n");
    CF_CHECK(htest_send_all(fd2, req, strlen(req)) == 0);
    n = htest_read_response(fd2, resp, sizeof resp, 5000);
    CF_CHECK(n > 0 && strstr(resp, "HTTP/1.1 417 Expectation Failed") != NULL);
    char eofbuf[16];
    CF_CHECK(htest_read_to_eof(fd2, eofbuf, sizeof eofbuf, 5000) == 0);
    close(fd2);

    htest_stop(t);
    pthread_mutex_destroy(&st.mutex);
}

CF_TEST(loop04_half_close_still_gets_response) {
    struct reply_state st;
    memset(&st, 0, sizeof st);
    pthread_mutex_init(&st.mutex, NULL);
    struct htest *t = htest_start(reply_admit, &st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);

    int fd = htest_connect(port, 0);
    CF_REQUIRE(fd >= 0);
    char req[256];
    request(req, sizeof req, "GET", "/half", port, "");
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    CF_CHECK(shutdown(fd, SHUT_WR) == 0);

    char resp[4096];
    ssize_t n = htest_read_response(fd, resp, sizeof resp, 5000);
    CF_CHECK(n > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    /* The response is delivered even though the peer stopped writing; no
     * further request can follow, so the server closes. */
    char eofbuf[16];
    CF_CHECK(htest_read_to_eof(fd, eofbuf, sizeof eofbuf, 5000) == 0);
    close(fd);

    htest_stop(t);
    pthread_mutex_destroy(&st.mutex);
}

CF_TEST(loop04_keepalive_and_http10) {
    struct reply_state st;
    memset(&st, 0, sizeof st);
    pthread_mutex_init(&st.mutex, NULL);
    struct htest *t = htest_start(reply_admit, &st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char resp[4096];

    /* HTTP/1.1 keep-alive: two sequential requests. */
    int fd = htest_connect(port, 0);
    CF_REQUIRE(fd >= 0);
    char req[256];
    request(req, sizeof req, "GET", "/first", port, "");
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    CF_CHECK(htest_read_response(fd, resp, sizeof resp, 5000) > 0);
    CF_CHECK(strstr(resp, "Connection: keep-alive") != NULL);
    request(req, sizeof req, "GET", "/second", port, "");
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    CF_CHECK(htest_read_response(fd, resp, sizeof resp, 5000) > 0);
    close(fd);

    /* HTTP/1.0 without Connection: close after one response. */
    int fd10 = htest_connect(port, 0);
    CF_REQUIRE(fd10 >= 0);
    const char *r10 = "GET /old HTTP/1.0\r\n\r\n"; /* Host optional in 1.0 */
    CF_CHECK(htest_send_all(fd10, r10, strlen(r10)) == 0);
    ssize_t n = htest_read_response(fd10, resp, sizeof resp, 5000);
    CF_CHECK(n > 0 && strstr(resp, "Connection: close") != NULL);
    char eofbuf[16];
    CF_CHECK(htest_read_to_eof(fd10, eofbuf, sizeof eofbuf, 5000) == 0);
    close(fd10);

    /* HTTP/1.0 with keep-alive stays open for a second request. */
    int fdka = htest_connect(port, 0);
    CF_REQUIRE(fdka >= 0);
    char reqka[300];
    snprintf(reqka, sizeof reqka,
             "GET /oldka HTTP/1.0\r\nHost: 127.0.0.1:%u\r\n"
             "Connection: keep-alive\r\n\r\n",
             port);
    CF_CHECK(htest_send_all(fdka, reqka, strlen(reqka)) == 0);
    n = htest_read_response(fdka, resp, sizeof resp, 5000);
    CF_CHECK(n > 0 && strstr(resp, "Connection: keep-alive") != NULL);
    CF_CHECK(htest_send_all(fdka, reqka, strlen(reqka)) == 0);
    n = htest_read_response(fdka, resp, sizeof resp, 5000);
    CF_CHECK(n > 0 && strstr(resp, "HTTP/1.1 200 OK") != NULL);
    close(fdka);

    htest_stop(t);
    pthread_mutex_destroy(&st.mutex);
}

#define H01_HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static cf_err never_admit(void *user, cf_http_task *task) {
    (void)user;
    (void)task;
    return CF_BUSY;
}

static void *run_loop(void *arg) {
    cf_http_loop *loop = arg;
    (void)cf_http_loop_run(loop);
    return NULL;
}

CF_TEST(loop_app_stop_requested_ends_run) {
    cf_config_entry entries[3] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:9"},
        {"SECRET_KEY_BASE", H01_HEX64},
        {"PORT", "9"},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 3, NULL, &config) == CF_OK);
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);

    unsigned port = 0;
    int listen_fd = htest_listen(&port);
    CF_REQUIRE(listen_fd >= 0);
    cf_http_loop_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.listen_fd = listen_fd;
    cfg.public_origin = "http://127.0.0.1:9";
    cfg.app = app;
    cfg.admit = never_admit;
    cf_http_loop *loop = NULL;
    CF_REQUIRE(cf_http_loop_create(&cfg, &loop) == CF_OK);

    pthread_t thread;
    CF_REQUIRE(pthread_create(&thread, NULL, run_loop, loop) == 0);
    /* A stop request through the app must end run() without loop_stop. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5;
    cf_app_request_stop(app);
    int rc = pthread_timedjoin_np(thread, NULL, &ts);
    CF_CHECK(rc == 0);
    if (rc != 0) {
        cf_http_loop_stop(loop);
        pthread_join(thread, NULL);
    }
    cf_http_loop_destroy(loop);
    close(listen_fd);
    cf_app_destroy(app); /* joins workers before freeing config */
}

CF_TEST_MAIN()
