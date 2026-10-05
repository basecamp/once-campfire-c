/* Test-only integration harness for A00: a full app (writer + request workers)
 * behind one real H01 loop on a loopback socket, driven by tests/app actions
 * through the route double. Not part of the application. */
#ifndef CF_TEST_SERVE_HARNESS_H
#define CF_TEST_SERVE_HARNESS_H

#include "cf.h"
#include "http/http.h"

#include <pthread.h>
#include <stddef.h>
#include <sys/types.h>

struct srv {
    cf_app *app;
    int listen_fd;
    unsigned port;
    char origin[64];
    char dir[128];
    char db_path[160];
    cf_http_loop *loop;
    pthread_t thread;
    bool running;
    cf_http_counters counters; /* snapshotted by srv_stop before destroy */
    bool counters_valid;
};

/* Bind first (so PUBLIC_ORIGIN matches the real port), then create and start
 * the app with its temporary database and one HTTP loop. readers/slots size
 * the pool; loops is passed to the config (the harness runs one loop). */
cf_err srv_start(struct srv *s, size_t readers, size_t slots, size_t loops);

/* Production shutdown order: loop stop + join, app stop, loop destroy,
 * listener close, app destroy, temp database removal. */
void srv_stop(struct srv *s);

/* Loop counters; only valid after srv_stop's loop join. */
void srv_counters(const struct srv *s, cf_http_counters *out);

int srv_connect(const struct srv *s);
int srv_send_all(int fd, const void *buf, size_t len);

/* Read one complete HTTP response (headers + Content-Length body), or until
 * EOF; returns bytes or -1 on timeout. */
ssize_t srv_read_response(int fd, char *buf, size_t cap, int timeout_ms);

/* Send `request` as a raw HTTP/1.1 request (request must include \r\n\r\n). */
int srv_request(struct srv *s, const char *raw, int *fd_out, char *resp,
                size_t cap, int timeout_ms);

#endif /* CF_TEST_SERVE_HARNESS_H */
