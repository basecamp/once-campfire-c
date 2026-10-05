/* H01 completion tests (07-verification.md CORE-03/CORE-04): stale
 * completions after close/slot reuse cannot write to a reused descriptor;
 * admission failure transfers no ownership and an admitted completion cannot
 * be lost. Shutdown drains admitted tasks and rejects new admissions. */
#include "cf.h"
#include "cf_test.h"
#include "http/http.h"

#include <errno.h>
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
void htest_finish(struct htest *t, cf_http_counters *out);
unsigned htest_port(const struct htest *t);
cf_http_loop *htest_loop(const struct htest *t);
int htest_connect(unsigned port, int rcvbuf);
int htest_send_all(int fd, const void *buf, size_t len);
ssize_t htest_read_response(int fd, char *buf, size_t cap, int timeout_ms);
ssize_t htest_read_to_eof(int fd, char *buf, size_t cap, int timeout_ms);
int htest_has_data(int fd, int timeout_ms);

struct comp_state {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned count;
    cf_http_task *held[8];
    bool hold;      /* keep the task instead of answering */
    bool fail_next; /* decline the next admission */
};

static cf_err comp_admit(void *user, cf_http_task *task) {
    struct comp_state *st = user;
    pthread_mutex_lock(&st->mutex);
    if (st->fail_next) {
        pthread_mutex_unlock(&st->mutex);
        return CF_BUSY;
    }
    if (st->count < 8) st->held[st->count] = task;
    st->count++;
    pthread_cond_broadcast(&st->cond);
    bool hold = st->hold;
    pthread_mutex_unlock(&st->mutex);
    if (hold) return CF_OK;

    cf_response resp;
    cf_response_init(&resp);
    resp.status = 200;
    cf_buf *body = NULL;
    if (cf_buf_copy((cf_span){(const unsigned char *)"ok", 2}, &body) !=
        CF_OK) {
        return CF_NOMEM;
    }
    (void)cf_response_body(&resp, body);
    cf_buf_release(body);
    cf_err rc = cf_http_task_submit(task, &resp);
    cf_response_dispose(&resp);
    return rc == CF_OK ? CF_OK : CF_BUSY;
}

static bool wait_count(struct comp_state *st, unsigned want, int timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&st->mutex);
    while (st->count < want) {
        if (pthread_cond_timedwait(&st->cond, &st->mutex, &ts) != 0) break;
    }
    bool ok = st->count >= want;
    pthread_mutex_unlock(&st->mutex);
    return ok;
}

static void submit_body(cf_http_task *task, const char *text) {
    cf_response resp;
    cf_response_init(&resp);
    resp.status = 200;
    cf_buf *body = NULL;
    if (cf_buf_copy((cf_span){(const unsigned char *)text, strlen(text)},
                    &body) != CF_OK) {
        return;
    }
    (void)cf_response_body(&resp, body);
    cf_buf_release(body);
    if (cf_http_task_submit(task, &resp) != CF_OK) {
        fprintf(stderr, "submit_body: submit failed\n");
    }
    cf_response_dispose(&resp);
}

CF_TEST(core03_stale_completion_cannot_write_to_reused_slot) {
    struct comp_state st;
    memset(&st, 0, sizeof st);
    pthread_mutex_init(&st.mutex, NULL);
    pthread_cond_init(&st.cond, NULL);
    st.hold = true;

    /* One slot: the second connection can only be admitted after the first
     * close was processed and the slot reused (generation incremented). */
    struct htest *t = htest_start(comp_admit, &st, 1, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char req[256];

    int a = htest_connect(port, 0);
    CF_REQUIRE(a >= 0);
    snprintf(req, sizeof req, "GET /a HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
             port);
    CF_CHECK(htest_send_all(a, req, strlen(req)) == 0);
    CF_REQUIRE(wait_count(&st, 1, 5000));

    struct linger lg = {1, 0};
    setsockopt(a, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    close(a); /* reconnect below only succeeds once the slot is reused */

    int b = -1;
    for (int attempt = 0; attempt < 200 && b < 0; attempt++) {
        int fd = htest_connect(port, 0);
        if (fd < 0) continue;
        if (htest_send_all(fd, req, strlen(req)) != 0) {
            close(fd);
            continue;
        }
        if (wait_count(&st, 2, 3000)) {
            b = fd;
        } else {
            close(fd);
        }
    }
    CF_REQUIRE(b >= 0);

    cf_http_task *task_a = st.held[0];
    cf_http_task *task_b = st.held[1];
    CF_REQUIRE(task_a != NULL && task_b != NULL);
    CF_CHECK(cf_http_task_sequence(task_a) == 1);
    CF_CHECK(cf_http_task_sequence(task_b) == 1); /* new request, reused slot */
    cf_conn_id ida = cf_http_task_connection(task_a);
    cf_conn_id idb = cf_http_task_connection(task_b);
    CF_CHECK(ida.slot == idb.slot);
    CF_CHECK(idb.generation == ida.generation + 1);

    submit_body(task_a, "STALE"); /* must be released, never written */
    submit_body(task_b, "fresh");

    char resp[1024];
    ssize_t n = htest_read_response(b, resp, sizeof resp, 10000);
    CF_CHECK(n > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    CF_CHECK(strstr(resp, "\r\n\r\nfresh") != NULL);
    CF_CHECK(strstr(resp, "STALE") == NULL);
    close(b);

    cf_http_counters counters;
    htest_finish(t, &counters);
    CF_CHECK(counters.stale_completions >= 1);
    pthread_mutex_destroy(&st.mutex);
    pthread_cond_destroy(&st.cond);
}

CF_TEST(core04_admission_failure_transfers_nothing) {
    struct comp_state st;
    memset(&st, 0, sizeof st);
    pthread_mutex_init(&st.mutex, NULL);
    pthread_cond_init(&st.cond, NULL);
    st.fail_next = true;

    struct htest *t = htest_start(comp_admit, &st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char req[256];
    snprintf(req, sizeof req, "GET /busy HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
             port);

    int fd = htest_connect(port, 0);
    CF_REQUIRE(fd >= 0);
    char resp[2048];

    /* The queue is full: 503 + Retry-After, no slot consumed, and the
     * connection stays usable for the next request. */
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    CF_CHECK(htest_read_response(fd, resp, sizeof resp, 5000) > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 503 Service Unavailable") != NULL);
    CF_CHECK(strstr(resp, "Retry-After: 1") != NULL);
    CF_CHECK(strstr(resp, "Connection: keep-alive") != NULL);
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    CF_CHECK(htest_read_response(fd, resp, sizeof resp, 5000) > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 503 Service Unavailable") != NULL);

    /* Once the queue drains, the same connection is served. */
    pthread_mutex_lock(&st.mutex);
    st.fail_next = false;
    pthread_mutex_unlock(&st.mutex);
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    CF_CHECK(htest_read_response(fd, resp, sizeof resp, 5000) > 0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    close(fd);

    cf_http_counters counters;
    htest_finish(t, &counters);
    CF_CHECK(counters.admission_rejected == 2);
    CF_CHECK(counters.admissions == 1);
    pthread_mutex_destroy(&st.mutex);
    pthread_cond_destroy(&st.cond);
}

CF_TEST(core04_admitted_completion_is_not_lost) {
    struct comp_state st;
    memset(&st, 0, sizeof st);
    pthread_mutex_init(&st.mutex, NULL);
    pthread_cond_init(&st.cond, NULL);
    st.hold = true;

    struct htest *t = htest_start(comp_admit, &st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char req[256];
    snprintf(req, sizeof req, "GET /x HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
             port);

    int fd = htest_connect(port, 0);
    CF_REQUIRE(fd >= 0);
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    CF_REQUIRE(wait_count(&st, 1, 5000));
    cf_http_task *task = st.held[0];
    CF_REQUIRE(task != NULL);

    /* The completion is preallocated with the task, so submitting it from
     * this (worker-side) thread cannot fail on a second allocation. Exactly
     * one submit call is made; the loop must deliver it. */
    submit_body(task, "winner");

    char resp[1024];
    CF_CHECK(htest_read_response(fd, resp, sizeof resp, 10000) > 0);
    CF_CHECK(strstr(resp, "\r\n\r\nwinner") != NULL);
    close(fd);

    cf_http_counters counters;
    htest_finish(t, &counters);
    CF_CHECK(counters.responses_sent == 1);
    pthread_mutex_destroy(&st.mutex);
    pthread_cond_destroy(&st.cond);
}

CF_TEST(core04_abandon_releases_and_closes) {
    struct comp_state st;
    memset(&st, 0, sizeof st);
    pthread_mutex_init(&st.mutex, NULL);
    pthread_cond_init(&st.cond, NULL);
    st.hold = true;

    struct htest *t = htest_start(comp_admit, &st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char req[256];
    snprintf(req, sizeof req, "GET /gone HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
             port);

    int fd = htest_connect(port, 0);
    CF_REQUIRE(fd >= 0);
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    CF_REQUIRE(wait_count(&st, 1, 5000));
    cf_http_task_abandon(st.held[0]); /* exactly once */

    char resp[256];
    CF_CHECK(htest_read_to_eof(fd, resp, sizeof resp, 5000) == 0);
    close(fd);

    cf_http_counters counters;
    htest_finish(t, &counters);
    CF_CHECK(counters.responses_sent == 0);
    pthread_mutex_destroy(&st.mutex);
    pthread_cond_destroy(&st.cond);
}

CF_TEST(shutdown_drains_admitted_task_and_rejects_new) {
    struct comp_state st;
    memset(&st, 0, sizeof st);
    pthread_mutex_init(&st.mutex, NULL);
    pthread_cond_init(&st.cond, NULL);
    st.hold = true;

    struct htest *t = htest_start(comp_admit, &st, 0, 0, 0, NULL);
    CF_REQUIRE(t != NULL);
    unsigned port = htest_port(t);
    char req[256];
    snprintf(req, sizeof req, "GET /drain HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
             port);

    int fd = htest_connect(port, 0);
    CF_REQUIRE(fd >= 0);
    CF_CHECK(htest_send_all(fd, req, strlen(req)) == 0);
    CF_REQUIRE(wait_count(&st, 1, 5000));

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    cf_http_loop_stop(htest_loop(t));

    /* New connections are not admitted while stopping: the late request is
     * either never accepted or answered with a 503 rejection; it must not
     * reach the handler (no 200). */
    int late = htest_connect(port, 0);
    if (late >= 0) {
        CF_CHECK(htest_send_all(late, req, strlen(req)) == 0);
        if (htest_has_data(late, 300)) {
            char lresp[1024];
            ssize_t ln = htest_read_to_eof(late, lresp, sizeof lresp, 2000);
            bool closed_no_body = ln == 0 || ln < 0;
            CF_CHECK(closed_no_body ||
                     strstr(lresp, "HTTP/1.1 503 Service Unavailable") != NULL);
            CF_CHECK(ln <= 0 || strstr(lresp, "HTTP/1.1 200") == NULL);
        }
        close(late);
    }

    /* The admitted task still completes and the drain does not wait out the
     * full five seconds. */
    submit_body(st.held[0], "drained");
    char resp[1024];
    CF_CHECK(htest_read_response(fd, resp, sizeof resp, 5000) > 0);
    CF_CHECK(strstr(resp, "\r\n\r\ndrained") != NULL);
    close(fd);

    cf_http_counters counters;
    htest_finish(t, &counters);
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long elapsed_ms = (long)(t1.tv_sec - t0.tv_sec) * 1000 +
                      (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CF_CHECK(elapsed_ms < 4500);
    CF_CHECK(counters.responses_sent == 1);
    pthread_mutex_destroy(&st.mutex);
    pthread_cond_destroy(&st.cond);
}

CF_TEST_MAIN()
