/* H01 file-body tests (07-verification.md HTTP-09): 64 KiB streamed chunks
 * survive disconnect, owned fds close, and the loop stays responsive while a
 * file is being read. */
#include "cf.h"
#include "cf_test.h"
#include "http/http.h"

#include <dirent.h>
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

static int count_open_fds(void) {
    DIR *d = opendir("/proc/self/fd");
    if (d == NULL) return -1;
    int n = 0;
    while (readdir(d) != NULL) n++;
    closedir(d);
    return n;
}

struct file_state {
    char path[128];
    size_t len;
    pthread_mutex_t mutex;
    unsigned file_admits;
};

static cf_err file_admit(void *user, cf_http_task *task) {
    struct file_state *st = user;
    const cf_request *req = cf_http_task_request(task);
    bool quick = req->target.len == 6 &&
                 memcmp(req->target.ptr, "/quick", 6) == 0;
    cf_response resp;
    cf_response_init(&resp);
    cf_err rc;
    if (quick) {
        resp.status = 200;
        cf_buf *body = NULL;
        rc = cf_buf_copy((cf_span){(const unsigned char *)"quick", 5}, &body);
        if (rc != CF_OK) return rc;
        rc = cf_response_body(&resp, body);
        cf_buf_release(body);
    } else {
        int fd = open(st->path, O_RDONLY);
        if (fd < 0) return CF_NOT_FOUND;
        pthread_mutex_lock(&st->mutex);
        st->file_admits++;
        pthread_mutex_unlock(&st->mutex);
        resp.status = 200;
        rc = cf_response_file(&resp, fd, 0, (uint64_t)st->len);
        if (rc != CF_OK) close(fd); /* response_file takes fd only on OK */
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

static const unsigned char k_pattern = 'f';

static void fill_pattern(unsigned char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = (unsigned char)(k_pattern + (i % 13));
    }
}

CF_TEST(file_streams_exact_bytes_and_closes_fd) {
    size_t len = 300 * 1024;
    unsigned char *expect = malloc(len);
    CF_REQUIRE(expect != NULL);
    fill_pattern(expect, len);

    char path[] = "/tmp/h01_file_XXXXXX";
    int tmp = mkstemp(path);
    CF_REQUIRE(tmp >= 0);
    CF_REQUIRE(write(tmp, expect, len) == (ssize_t)len);
    fsync(tmp);
    close(tmp);

    int baseline = count_open_fds();
    struct file_state st;
    memset(&st, 0, sizeof st);
    pthread_mutex_init(&st.mutex, NULL);
    snprintf(st.path, sizeof st.path, "%s", path);
    st.len = len;

    struct htest *t = htest_start(file_admit, &st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char req[256];
    snprintf(req, sizeof req, "GET /file HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
             port);

    int fd = htest_connect(port, 0);
    CF_REQUIRE(fd >= 0);
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    char head[1024];
    CF_REQUIRE(htest_read_headers(fd, head, sizeof head, 10000) > 0);
    CF_CHECK(strstr(head, "HTTP/1.1 200 OK") != NULL);
    char cl[32];
    snprintf(cl, sizeof cl, "Content-Length: %zu", len);
    CF_CHECK(strstr(head, cl) != NULL);

    unsigned char *got = malloc(len);
    CF_REQUIRE(got != NULL);
    CF_CHECK(read_exact(fd, got, len, 30000) == (ssize_t)len);
    CF_CHECK(memcmp(got, expect, len) == 0);
    free(got);
    close(fd);

    htest_stop(t);
    pthread_mutex_destroy(&st.mutex);
    unlink(path);
    free(expect);

    /* Owned fds are closed again: no descriptor leak across the whole run. */
    CF_CHECK(count_open_fds() == baseline);
}

CF_TEST(file_stream_survives_disconnect_and_loop_stays_responsive) {
    size_t len = 1024 * 1024;
    unsigned char *expect = malloc(len);
    CF_REQUIRE(expect != NULL);
    fill_pattern(expect, len);

    char path[] = "/tmp/h01_file2_XXXXXX";
    int tmp = mkstemp(path);
    CF_REQUIRE(tmp >= 0);
    CF_REQUIRE(write(tmp, expect, len) == (ssize_t)len);
    fsync(tmp);
    close(tmp);

    int baseline = count_open_fds();
    struct file_state st;
    memset(&st, 0, sizeof st);
    pthread_mutex_init(&st.mutex, NULL);
    snprintf(st.path, sizeof st.path, "%s", path);
    st.len = len;

    struct htest *t = htest_start(file_admit, &st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char req[256];
    snprintf(req, sizeof req, "GET /file HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
             port);

    /* Slow client: read the header block only, then keep a tiny window. */
    int slow = htest_connect(port, 2048);
    CF_REQUIRE(slow >= 0);
    CF_CHECK(htest_send_all(slow, req, strlen(req)) == 0);
    char head[1024];
    CF_REQUIRE(htest_read_headers(slow, head, sizeof head, 10000) > 0);
    unsigned char dribble[512];
    CF_CHECK(read_exact(slow, dribble, sizeof dribble, 10000) ==
             (ssize_t)sizeof dribble);
    CF_CHECK(htest_has_data(slow, 100) != 0); /* more body is pending */

    /* While that file is mid-stream, a second connection is served. */
    int quick = htest_connect(port, 0);
    CF_REQUIRE(quick >= 0);
    snprintf(req, sizeof req, "GET /quick HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
             port);
    CF_CHECK(htest_send_all(quick, req, strlen(req)) == 0);
    char resp[1024];
    CF_CHECK(htest_read_response(quick, resp, sizeof resp, 10000) > 0);
    CF_CHECK(strstr(resp, "\r\n\r\nquick") != NULL);
    close(quick);
    CF_CHECK(htest_has_data(slow, 100) != 0); /* file still streaming */

    /* Disconnect mid-file with an RST; the loop must clean up and continue. */
    struct linger lg = {1, 0};
    setsockopt(slow, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    close(slow);

    /* Repeated quick requests keep succeeding while cleanup happens. */
    for (int i = 0; i < 20; i++) {
        int c = htest_connect(port, 0);
        CF_REQUIRE(c >= 0);
        CF_CHECK(htest_send_all(c, req, strlen(req)) == 0);
        ssize_t n = htest_read_response(c, resp, sizeof resp, 5000);
        CF_CHECK(n > 0 && strstr(resp, "\r\n\r\nquick") != NULL);
        close(c);
    }

    htest_stop(t);
    pthread_mutex_destroy(&st.mutex);
    unlink(path);
    free(expect);
    CF_CHECK(count_open_fds() == baseline);
}

CF_TEST(file_head_sends_length_and_closes_fd) {
    size_t len = 128 * 1024;
    unsigned char *filler = malloc(len);
    CF_REQUIRE(filler != NULL);
    fill_pattern(filler, len);
    char path[] = "/tmp/h01_file3_XXXXXX";
    int tmp = mkstemp(path);
    CF_REQUIRE(tmp >= 0);
    CF_REQUIRE(write(tmp, filler, len) == (ssize_t)len);
    fsync(tmp);
    close(tmp);
    free(filler);
    (void)len;

    int baseline = count_open_fds();
    struct file_state st;
    memset(&st, 0, sizeof st);
    pthread_mutex_init(&st.mutex, NULL);
    snprintf(st.path, sizeof st.path, "%s", path);
    st.len = len;

    struct htest *t = htest_start(file_admit, &st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char req[256];
    snprintf(req, sizeof req, "HEAD /file HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
             port);
    int fd = htest_connect(port, 0);
    CF_REQUIRE(fd >= 0);
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    char head[1024];
    CF_REQUIRE(htest_read_headers(fd, head, sizeof head, 10000) > 0);
    CF_CHECK(strstr(head, "HTTP/1.1 200 OK") != NULL);
    char cl[32];
    snprintf(cl, sizeof cl, "Content-Length: %zu", len);
    CF_CHECK(strstr(head, cl) != NULL);
    CF_CHECK(htest_has_data(fd, 200) == 0); /* HEAD sends no body */
    close(fd);

    htest_stop(t);
    pthread_mutex_destroy(&st.mutex);
    unlink(path);
    CF_CHECK(count_open_fds() == baseline);
}

CF_TEST_MAIN()
