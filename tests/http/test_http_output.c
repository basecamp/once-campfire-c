/* H01 output tests: deterministic short-write/EINTR/EAGAIN state machine
 * (HTTP-03), serializer contract (status line, Content-Length, Date,
 * Connection, HEAD/204/304, Set-Cookie separation, CR/LF rejection) and
 * response ownership. */
#include "cf.h"
#include "cf_test.h"
#include "core/testclock.h"
#include "http/http.h"
#include "http/http_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
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
int htest_connect(unsigned port, int rcvbuf);
int htest_send_all(int fd, const void *buf, size_t len);
ssize_t htest_read_headers(int fd, char *buf, size_t cap, int timeout_ms);
ssize_t htest_read_response(int fd, char *buf, size_t cap, int timeout_ms);
int htest_has_data(int fd, int timeout_ms);

static int count_substr(const char *hay, const char *needle) {
    int n = 0;
    const char *p = hay;
    size_t len = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += len;
    }
    return n;
}

CF_TEST(output_apply_short_write_eintr_eagain) {
    static cf_http_loop loop; /* zeroed: apply touches budget + conn only */
    memset(&loop, 0, sizeof loop);
    loop.cfg.output_bytes = 1u << 20;
    struct cf_http_conn conn;
    memset(&conn, 0, sizeof conn);
    conn.loop = &loop;
    conn.fd = -1;

    const char payload[] = "abcdef";
    CF_REQUIRE(cf_http_conn_queue_bytes(&conn, (const unsigned char *)payload,
                                        sizeof payload - 1) == CF_OK);
    CF_CHECK(cf_http_output_pending(&conn) == 6);
    CF_CHECK(conn.stall_ms != 0); /* pending became nonzero */

    /* Short write: two of six bytes accepted, then EAGAIN, then EINTR, then
     * the rest resumes exactly where it stopped. */
    CF_CHECK(cf_http_output_apply(&conn, 2, 0) == CF_HTTP_WRITE_PROGRESS);
    CF_CHECK(cf_http_output_pending(&conn) == 4);
    CF_CHECK(cf_http_output_apply(&conn, -1, EAGAIN) == CF_HTTP_WRITE_AGAIN);
    CF_CHECK(cf_http_output_pending(&conn) == 4);
    CF_CHECK(cf_http_output_apply(&conn, -1, EINTR) == CF_HTTP_WRITE_RETRY);
    CF_CHECK(cf_http_output_pending(&conn) == 4);
    CF_CHECK(cf_http_output_apply(&conn, 0, 0) == CF_HTTP_WRITE_AGAIN);
    CF_CHECK(cf_http_output_apply(&conn, 4, 0) == CF_HTTP_WRITE_PROGRESS);
    CF_CHECK(cf_http_output_pending(&conn) == 0);
    CF_CHECK(conn.out_head == NULL);

    /* Oversized progress consumes several segments in one call. */
    CF_REQUIRE(cf_http_conn_queue_bytes(&conn, (const unsigned char *)"xy", 2) ==
               CF_OK);
    CF_REQUIRE(cf_http_conn_queue_bytes(&conn, (const unsigned char *)"z", 1) ==
               CF_OK);
    CF_CHECK(cf_http_output_pending(&conn) == 3);
    CF_CHECK(cf_http_output_apply(&conn, 99, 0) == CF_HTTP_WRITE_PROGRESS);
    CF_CHECK(cf_http_output_pending(&conn) == 0);

    /* Transport failure is reported without touching the fd. */
    CF_REQUIRE(cf_http_conn_queue_bytes(&conn, (const unsigned char *)"q", 1) ==
               CF_OK);
    CF_CHECK(cf_http_output_apply(&conn, -1, EPIPE) == CF_HTTP_WRITE_FAILED);
    cf_http_output_reset(&conn); /* releases the remaining segment */
    CF_CHECK(cf_http_output_pending(&conn) == 0);

    /* Per-connection budget rejects before growing storage. */
    CF_CHECK(cf_http_output_apply(&conn, 0, 0) == CF_HTTP_WRITE_AGAIN);
    cf_http_output_reset(&conn);
}

static cf_err serialize_to_string(const cf_response *resp,
                                  const cf_request *req, char *out,
                                  size_t cap) {
    cf_http_serialized ser;
    cf_err rc = cf_http_response_serialize(resp, req, &ser);
    if (rc != CF_OK) return rc;
    cf_span s = cf_buf_span(ser.headers);
    CF_REQUIRE(s.len < cap);
    memcpy(out, s.ptr, s.len);
    out[s.len] = '\0';
    cf_buf_release(ser.headers);
    return CF_OK;
}

CF_TEST(output_serializer_headers) {
    cf_test_clock_set_fixed_us(0); /* Deterministic Date. */

    cf_request req;
    memset(&req, 0, sizeof req);
    req.method = CF_GET;

    cf_response resp;
    cf_response_init(&resp);
    resp.status = 200;
    cf_buf *body = NULL;
    CF_REQUIRE(cf_buf_copy((cf_span){(const unsigned char *)"hello", 5},
                           &body) == CF_OK);
    CF_REQUIRE(cf_response_body(&resp, body) == CF_OK);
    cf_buf_release(body);
    CF_REQUIRE(cf_response_header(
                   &resp,
                   (cf_span){(const unsigned char *)"Set-Cookie", 10},
                   (cf_span){(const unsigned char *)"a=1", 3}) == CF_OK);
    CF_REQUIRE(cf_response_header(
                   &resp,
                   (cf_span){(const unsigned char *)"Set-Cookie", 10},
                   (cf_span){(const unsigned char *)"b=2", 3}) == CF_OK);
    CF_REQUIRE(cf_response_header(
                   &resp,
                   (cf_span){(const unsigned char *)"Content-Type", 12},
                   (cf_span){(const unsigned char *)"text/plain", 10}) == CF_OK);
    /* Application framing headers are owned by the serializer and skipped. */
    CF_REQUIRE(cf_response_header(
                   &resp,
                   (cf_span){(const unsigned char *)"Content-Length", 14},
                   (cf_span){(const unsigned char *)"999", 3}) == CF_OK);

    char out[2048];
    CF_REQUIRE(serialize_to_string(&resp, &req, out, sizeof out) == CF_OK);
    CF_CHECK(strstr(out, "HTTP/1.1 200 OK\r\n") == out);
    CF_CHECK(strstr(out, "Date: Thu, 01 Jan 1970 00:00:00 GMT\r\n") != NULL);
    CF_CHECK(strstr(out, "Content-Length: 5\r\n") != NULL);
    CF_CHECK(strstr(out, "Content-Length: 999") == NULL);
    CF_CHECK(count_substr(out, "Content-Length:") == 1);
    CF_CHECK(strstr(out, "Connection: keep-alive\r\n") != NULL);
    /* Set-Cookie lines are never merged. */
    CF_CHECK(count_substr(out, "Set-Cookie:") == 2);
    CF_CHECK(strstr(out, "Set-Cookie: a=1\r\nSet-Cookie: b=2\r\n") != NULL);
    CF_CHECK(strstr(out, "Transfer-Encoding") == NULL);
    cf_response_dispose(&resp);

    /* HEAD: headers follow GET, no body is transmitted. */
    resp.status = 200;
    CF_REQUIRE(cf_buf_copy((cf_span){(const unsigned char *)"hello", 5},
                           &body) == CF_OK);
    CF_REQUIRE(cf_response_body(&resp, body) == CF_OK);
    cf_buf_release(body);
    req.method = CF_HEAD;
    cf_http_serialized ser;
    CF_REQUIRE(cf_http_response_serialize(&resp, &req, &ser) == CF_OK);
    CF_CHECK(ser.send_body == false);
    CF_CHECK(ser.body_length == 5);
    CF_CHECK(memmem(cf_buf_span(ser.headers).ptr, cf_buf_span(ser.headers).len,
                    "Content-Length: 5", 17) != NULL);
    cf_buf_release(ser.headers);
    req.method = CF_GET;

    /* 204 and 304: no Content-Length, no body. */
    resp.status = 204;
    CF_REQUIRE(serialize_to_string(&resp, &req, out, sizeof out) == CF_OK);
    CF_CHECK(strstr(out, "HTTP/1.1 204 No Content") != NULL);
    CF_CHECK(strstr(out, "Content-Length") == NULL);
    resp.status = 304;
    CF_REQUIRE(serialize_to_string(&resp, &req, out, sizeof out) == CF_OK);
    CF_CHECK(strstr(out, "HTTP/1.1 304 Not Modified") != NULL);
    CF_CHECK(strstr(out, "Content-Length") == NULL);

    cf_response_dispose(&resp);
    cf_test_clock_clear();
}

CF_TEST(output_header_validation) {
    cf_response resp;
    cf_response_init(&resp);
    resp.status = 200;

    CF_CHECK(cf_response_header(
                 &resp, (cf_span){(const unsigned char *)"X-A", 3},
                 (cf_span){(const unsigned char *)"bad\r\nvalue", 10}) ==
             CF_INVALID);
    CF_CHECK(cf_response_header(
                 &resp, (cf_span){(const unsigned char *)"X-A", 3},
                 (cf_span){(const unsigned char *)"bad\nvalue", 9}) ==
             CF_INVALID);
    CF_CHECK(cf_response_header(
                 &resp, (cf_span){(const unsigned char *)"X-A", 3},
                 (cf_span){(const unsigned char *)"bad\rvalue", 9}) ==
             CF_INVALID);
    CF_CHECK(cf_response_header(
                 &resp, (cf_span){(const unsigned char *)"Bad Name", 8},
                 (cf_span){(const unsigned char *)"v", 1}) == CF_INVALID);
    CF_CHECK(cf_response_header(
                 &resp, (cf_span){(const unsigned char *)"", 0},
                 (cf_span){(const unsigned char *)"v", 1}) == CF_INVALID);
    /* A normal header is accepted after the rejections. */
    CF_CHECK(cf_response_header(
                 &resp, (cf_span){(const unsigned char *)"X-Ok", 4},
                 (cf_span){(const unsigned char *)"v", 1}) == CF_OK);
    cf_response_dispose(&resp);
    cf_response_dispose(NULL); /* NULL-safe */
}

CF_TEST(output_file_ownership_and_dispose) {
    char path[] = "/tmp/h01_resp_XXXXXX";
    int tmp = mkstemp(path);
    CF_REQUIRE(tmp >= 0);
    CF_REQUIRE(write(tmp, "filedata", 8) == 8);
    fsync(tmp);
    close(tmp);

    int fd = open(path, O_RDONLY);
    CF_REQUIRE(fd >= 0);
    unlink(path);

    cf_response resp;
    cf_response_init(&resp);
    resp.status = 200;
    CF_CHECK(cf_response_file(&resp, -1, 0, 8) == CF_INVALID);
    CF_CHECK(cf_response_file(&resp, fd, 0, 8) == CF_OK);
    CF_CHECK(resp.body_kind == CF_BODY_FILE);
    CF_CHECK(resp.file_length == 8);

    cf_request req;
    memset(&req, 0, sizeof req);
    req.method = CF_GET;
    cf_http_serialized ser;
    CF_REQUIRE(cf_http_response_serialize(&resp, &req, &ser) == CF_OK);
    CF_CHECK(ser.body_length == 8);
    CF_CHECK(ser.send_body == true);
    cf_buf_release(ser.headers);

    /* dispose closes the fd it owns (01: closes on send end/error/cancel). */
    cf_response_dispose(&resp);
    errno = 0;
    CF_CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
}

/* Application callback used by the end-to-end partial-write case: a 512 KiB
 * patterned body with two Set-Cookie headers. */
static cf_err partial_admit(void *user, cf_http_task *task) {
    (void)user;
    size_t len = 512 * 1024;
    unsigned char *bytes = malloc(len);
    if (bytes == NULL) return CF_NOMEM;
    for (size_t i = 0; i < len; i++) bytes[i] = (unsigned char)('A' + i % 7);
    cf_buf *body = NULL;
    cf_err rc = cf_buf_copy((cf_span){bytes, len}, &body);
    free(bytes);
    if (rc != CF_OK) return rc;
    cf_response resp;
    cf_response_init(&resp);
    resp.status = 200;
    rc = cf_response_body(&resp, body);
    cf_buf_release(body);
    if (rc == CF_OK) {
        rc = cf_response_header(
            &resp, (cf_span){(const unsigned char *)"Set-Cookie", 10},
            (cf_span){(const unsigned char *)"one=1", 5});
    }
    if (rc == CF_OK) {
        rc = cf_response_header(
            &resp, (cf_span){(const unsigned char *)"Set-Cookie", 10},
            (cf_span){(const unsigned char *)"two=2", 5});
    }
    if (rc == CF_OK) rc = cf_http_task_submit(task, &resp);
    cf_response_dispose(&resp);
    return rc == CF_OK ? CF_OK : CF_BUSY;
}

static ssize_t read_exact(int fd, unsigned char *buf, size_t n, int timeout_ms) {
    size_t have = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t deadline =
        (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + timeout_ms;
    while (have < n) {
        struct pollfd pf = {fd, POLLIN, 0};
        int rc = poll(&pf, 1, 200);
        if (rc == 0) {
            struct timespec now_ts;
            clock_gettime(CLOCK_MONOTONIC, &now_ts);
            int64_t now =
                (int64_t)now_ts.tv_sec * 1000 + now_ts.tv_nsec / 1000000;
            if (now >= deadline) return -1;
            continue;
        }
        if (rc < 0) return -1;
        ssize_t got = recv(fd, buf + have, n - have, 0);
        if (got > 0) {
            have += (size_t)got;
            continue;
        }
        if (got == 0) return -1;
        if (errno == EINTR) continue;
        return -1;
    }
    return (ssize_t)have;
}

CF_TEST(output_partial_write_resumes_and_date_is_stable) {
    cf_test_clock_set_fixed_us(0);
    struct htest *t = htest_start(partial_admit, NULL, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);

    /* Tiny receive buffer forces short writes and EPOLLOUT resumption. */
    int fd = htest_connect(port, 1024);
    CF_REQUIRE(fd >= 0);
    char req[256];
    snprintf(req, sizeof req, "GET /big HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
             port);
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);

    char head[2048];
    ssize_t hn = htest_read_headers(fd, head, sizeof head, 10000);
    CF_REQUIRE(hn > 0);
    CF_CHECK(strstr(head, "HTTP/1.1 200 OK") != NULL);
    CF_CHECK(strstr(head, "Date: Thu, 01 Jan 1970 00:00:00 GMT") != NULL);
    CF_CHECK(strstr(head, "Content-Length: 524288") != NULL);
    CF_CHECK(count_substr(head, "Set-Cookie:") == 2);

    /* Move the injected clock; the queued response must not change. */
    cf_test_clock_set_fixed_us(INT64_C(1000000000000000));

    size_t body_len = 512 * 1024;
    unsigned char *body = malloc(body_len);
    CF_REQUIRE(body != NULL);
    CF_CHECK(read_exact(fd, body, body_len, 20000) == (ssize_t)body_len);
    int ok = 1;
    for (size_t i = 0; i < body_len; i++) {
        if (body[i] != (unsigned char)('A' + i % 7)) {
            ok = 0;
            break;
        }
    }
    CF_CHECK(ok == 1);
    CF_CHECK(htest_has_data(fd, 150) == 0); /* exactly one response body */
    free(body);
    close(fd);
    cf_test_clock_clear();
    htest_stop(t);
}

CF_TEST(output_deadline_and_stall_timer) {
    cf_test_clock_set_monotonic_ms(5000);
    static cf_http_loop loop;
    memset(&loop, 0, sizeof loop);
    loop.cfg.output_bytes = 1u << 20;

    /* Header/body/idle deadline: expires exactly at the deadline. */
    struct cf_http_conn conn;
    memset(&conn, 0, sizeof conn);
    conn.loop = &loop;
    conn.fd = open("/dev/null", O_RDONLY);
    CF_REQUIRE(conn.fd >= 0);
    conn.state = CF_HTTP_STATE_HEADERS;
    conn.deadline_kind = CF_HTTP_DL_HEADER;
    conn.deadline_ms = 35000;
    cf_http_conn_check_deadline(&conn, 34999);
    CF_CHECK(conn.state == CF_HTTP_STATE_HEADERS);
    cf_http_conn_check_deadline(&conn, 35000);
    CF_CHECK(conn.state == CF_HTTP_STATE_CLOSED);
    CF_CHECK(loop.counters.timeouts == 1);

    /* Idle deadline on a keep-alive connection. */
    memset(&conn, 0, sizeof conn);
    conn.loop = &loop;
    conn.fd = open("/dev/null", O_RDONLY);
    CF_REQUIRE(conn.fd >= 0);
    conn.state = CF_HTTP_STATE_HEADERS;
    conn.deadline_kind = CF_HTTP_DL_IDLE;
    conn.deadline_ms = 9000;
    cf_http_conn_check_deadline(&conn, 8999);
    CF_CHECK(conn.state == CF_HTTP_STATE_HEADERS);
    cf_http_conn_check_deadline(&conn, 9000);
    CF_CHECK(conn.state == CF_HTTP_STATE_CLOSED);

    /* Stalled write: arms when pending becomes nonzero, resets only on
     * positive send progress. */
    memset(&conn, 0, sizeof conn);
    conn.loop = &loop;
    conn.fd = open("/dev/null", O_RDONLY);
    CF_REQUIRE(conn.fd >= 0);
    conn.state = CF_HTTP_STATE_WRITING;
    CF_REQUIRE(cf_http_conn_queue_bytes(
                   &conn, (const unsigned char *)"123456", 6) == CF_OK);
    conn.stall_ms = 1000; /* pretend it armed at t=1000 */
    cf_http_conn_check_deadline(&conn, 1000 + CF_HTTP_DEADLINE_MS - 1);
    CF_CHECK(conn.state == CF_HTTP_STATE_WRITING);
    CF_CHECK(cf_http_output_apply(&conn, 1, 0) == CF_HTTP_WRITE_PROGRESS);
    CF_CHECK(conn.stall_ms == 5000); /* positive progress reset it */
    cf_http_conn_check_deadline(&conn, 5000 + CF_HTTP_DEADLINE_MS - 1);
    CF_CHECK(conn.state == CF_HTTP_STATE_WRITING);
    cf_http_conn_check_deadline(&conn, 5000 + CF_HTTP_DEADLINE_MS);
    CF_CHECK(conn.state == CF_HTTP_STATE_CLOSED);
    cf_http_output_reset(&conn);
    cf_test_clock_clear();
}

CF_TEST_MAIN()
