/* Shared helpers for H01's HTTP tests (auxiliary translation unit, no
 * CF_TEST cases). Real loopback sockets; every wait is poll()-driven on the
 * actual descriptor, and every in-flight wait has a timeout.
 *
 * Prototypes are repeated at the top of each test_http_*.c file because H01
 * owns only tests/http/test_http_*.c (no extra header). */
#include "cf.h"
#include "http/http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct htest {
    cf_http_loop *loop;
    int listen_fd;
    pthread_t thread;
    unsigned port;
    char origin[64];
    cf_http_admit_fn admit;
    void *admit_user;
    size_t conn_cap;
    size_t input_bytes;
    size_t output_bytes;
    bool has_origin;
};

int htest_listen(unsigned *port_out) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(fd, 128) != 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof addr;
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        close(fd);
        return -1;
    }
    *port_out = ntohs(addr.sin_port);
    return fd;
}

static void *htest_run(void *arg) {
    struct htest *t = arg;
    (void)cf_http_loop_run(t->loop);
    return NULL;
}

void htest_finish(struct htest *t, cf_http_counters *out);

struct htest *htest_start(cf_http_admit_fn admit, void *user,
                          size_t conn_cap, size_t input_bytes,
                          size_t output_bytes,
                          const char *origin_override) {
    struct htest *t = calloc(1, sizeof *t);
    if (t == NULL) return NULL;
    t->admit = admit;
    t->admit_user = user;
    t->conn_cap = conn_cap;
    t->input_bytes = input_bytes;
    t->output_bytes = output_bytes;
    t->listen_fd = htest_listen(&t->port);
    if (t->listen_fd < 0) {
        free(t);
        return NULL;
    }
    if (origin_override != NULL) {
        snprintf(t->origin, sizeof t->origin, "%s", origin_override);
        t->has_origin = true;
    } else {
        snprintf(t->origin, sizeof t->origin, "http://127.0.0.1:%u", t->port);
    }
    cf_http_loop_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.listen_fd = t->listen_fd;
    cfg.loop_index = 0;
    cfg.public_origin = t->origin;
    cfg.connections_per_loop = t->conn_cap;
    cfg.input_bytes = t->input_bytes;
    cfg.output_bytes = t->output_bytes;
    cfg.admit = admit;
    cfg.admit_user = user;
    if (cf_http_loop_create(&cfg, &t->loop) != CF_OK) {
        close(t->listen_fd);
        free(t);
        return NULL;
    }
    if (pthread_create(&t->thread, NULL, htest_run, t) != 0) {
        cf_http_loop_destroy(t->loop);
        close(t->listen_fd);
        free(t);
        return NULL;
    }
    return t;
}

void htest_stop(struct htest *t) {
    htest_finish(t, NULL);
}

/* Stop the loop, snapshot its counters, then destroy it. Frees t. */
void htest_finish(struct htest *t, cf_http_counters *out) {
    if (t == NULL) return;
    if (t->loop != NULL) {
        cf_http_loop_stop(t->loop);
        pthread_join(t->thread, NULL);
        if (out != NULL) cf_http_loop_counters(t->loop, out);
        cf_http_loop_destroy(t->loop);
        t->loop = NULL;
    } else if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (t->listen_fd >= 0) close(t->listen_fd);
    free(t);
}

unsigned htest_port(const struct htest *t) {
    return t != NULL ? t->port : 0;
}

cf_http_loop *htest_loop(const struct htest *t) {
    return t != NULL ? t->loop : NULL;
}

int htest_connect(unsigned port, int rcvbuf) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (rcvbuf > 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int htest_send_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = buf;
    while (len != 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pf = {fd, POLLOUT, 0};
            if (poll(&pf, 1, 5000) <= 0) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static ssize_t wait_readable(int fd, int timeout_ms) {
    struct pollfd pf = {fd, POLLIN, 0};
    int rc = poll(&pf, 1, timeout_ms);
    if (rc <= 0) return rc == 0 ? 0 : -1;
    return 1;
}

/* Read until the accumulated bytes contain the needle or the peer closes /
 * timeout. Returns total bytes in buf (NUL-terminated), -1 on timeout. */
ssize_t htest_read_until(int fd, char *buf, size_t cap, const char *needle,
                         int timeout_ms) {
    size_t have = 0;
    int64_t deadline = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    deadline = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + timeout_ms;
    while (have + 1 < cap) {
        ssize_t r = wait_readable(fd, 200);
        if (r == 0) {
            int64_t now;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            if (now >= deadline) break;
            continue;
        }
        if (r < 0) return -1;
        ssize_t n = recv(fd, buf + have, cap - 1 - have, 0);
        if (n > 0) {
            have += (size_t)n;
            buf[have] = '\0';
            if (needle != NULL && memmem(buf, have, needle, strlen(needle)) !=
                                      NULL) {
                return (ssize_t)have;
            }
            if (needle == NULL) return (ssize_t)have;
            continue;
        }
        if (n == 0) break; /* peer closed */
        if (errno == EINTR) continue;
        return -1;
    }
    buf[have] = '\0';
    if (needle != NULL &&
        memmem(buf, have, needle, strlen(needle)) == NULL && have + 1 >= cap) {
        return (ssize_t)have;
    }
    return (ssize_t)have;
}

static int response_complete(const char *buf, size_t have, size_t header_end,
                             long long content_length) {
    (void)buf;
    size_t body = content_length < 0 ? 0 : (size_t)content_length;
    return have >= header_end + body;
}

/* Read one complete HTTP response: headers, then Content-Length bytes.
 * Returns the total size, or -1 when the connection closed/timed out before
 * a complete response. */
ssize_t htest_read_response(int fd, char *buf, size_t cap, int timeout_ms) {
    size_t have = 0;
    size_t header_end = 0;
    long long content_length = -1;
    int64_t deadline;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    deadline = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + timeout_ms;

    while (have + 1 < cap) {
        if (header_end == 0) {
            char *h = memmem(buf, have, "\r\n\r\n", 4);
            if (h != NULL) {
                header_end = (size_t)(h - buf) + 4;
                char *cl = memmem(buf, header_end, "Content-Length:", 15);
                if (cl != NULL) {
                    content_length = strtoll(cl + 15, NULL, 10);
                } else {
                    content_length = 0;
                }
                if (response_complete(buf, have, header_end,
                                      content_length)) {
                    return (ssize_t)have;
                }
            }
        } else if (have >= header_end + (size_t)content_length) {
            return (ssize_t)have;
        }

        struct pollfd pf = {fd, POLLIN, 0};
        int rc = poll(&pf, 1, 200);
        if (rc == 0) {
            int64_t now;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            if (now >= deadline) return -1;
            continue;
        }
        if (rc < 0) return -1;
        /* Never read past the response: one byte until the header block is
         * complete, then exactly the remaining Content-Length bytes. This
         * keeps pipelined responses from being swallowed. */
        size_t want = 1;
        if (header_end != 0) {
            size_t body = content_length < 0 ? 0 : (size_t)content_length;
            want = header_end + body - have;
        }
        if (want > cap - 1 - have) want = cap - 1 - have;
        ssize_t n = recv(fd, buf + have, want, 0);
        if (n > 0) {
            have += (size_t)n;
            buf[have] = '\0';
            continue;
        }
        if (n == 0) break;
        if (errno == EINTR) continue;
        return -1;
    }
    buf[have] = '\0';
    if (header_end != 0 &&
        response_complete(buf, have, header_end, content_length)) {
        return (ssize_t)have;
    }
    return -1;
}

/* Read exactly one header block (through CRLF CRLF); leaves body bytes on
 * the socket. Returns size or -1. */
ssize_t htest_read_headers(int fd, char *buf, size_t cap, int timeout_ms) {
    size_t have = 0;
    int64_t deadline;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    deadline = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + timeout_ms;
    while (have + 1 < cap) {
        struct pollfd pf = {fd, POLLIN, 0};
        int rc = poll(&pf, 1, 200);
        if (rc == 0) {
            int64_t now;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            if (now >= deadline) return -1;
            continue;
        }
        if (rc < 0) return -1;
        ssize_t n = recv(fd, buf + have, 1, 0);
        if (n > 0) {
            have += (size_t)n;
            buf[have] = '\0';
            if (have >= 4 &&
                memcmp(buf + have - 4, "\r\n\r\n", 4) == 0) {
                return (ssize_t)have;
            }
            continue;
        }
        if (n == 0) return -1;
        if (errno == EINTR) continue;
        return -1;
    }
    return -1;
}

/* True when the socket has readable bytes within timeout_ms. */
int htest_has_data(int fd, int timeout_ms) {
    struct pollfd pf = {fd, POLLIN, 0};
    return poll(&pf, 1, timeout_ms) > 0;
}

/* Read until EOF (for close-delimited cases). Returns bytes read, -1 on
 * timeout. */
ssize_t htest_read_to_eof(int fd, char *buf, size_t cap, int timeout_ms) {
    size_t have = 0;
    int64_t deadline;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    deadline = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + timeout_ms;
    while (have + 1 < cap) {
        ssize_t r = wait_readable(fd, 200);
        if (r == 0) {
            int64_t now;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            if (now >= deadline) return -1;
            continue;
        }
        if (r < 0) return -1;
        ssize_t n = recv(fd, buf + have, cap - 1 - have, 0);
        if (n > 0) {
            have += (size_t)n;
            buf[have] = '\0';
            continue;
        }
        if (n == 0) {
            buf[have] = '\0';
            return (ssize_t)have;
        }
        if (errno == EINTR) continue;
        return -1;
    }
    buf[have] = '\0';
    return (ssize_t)have;
}

/* Simple echo admission: records method/target/body and answers 200 "ok"
 * with a fixed header. The task accessors are borrowed; the strings are
 * copied under the state mutex. */
struct htest_echo {
    pthread_mutex_t mutex;
    unsigned admits;
    size_t body_len; /* full decoded length; body[] holds a copy prefix */
    char method[16];
    char target[512];
    char body[512];
    char peer[64];
    bool tls;
};

cf_err htest_echo_admit(void *user, cf_http_task *task) {
    struct htest_echo *st = user;
    const cf_request *req = cf_http_task_request(task);
    if (req == NULL) return CF_BUSY;

    pthread_mutex_lock(&st->mutex);
    st->admits++;
    size_t n = req->raw_method.len < sizeof st->method - 1
                   ? req->raw_method.len
                   : sizeof st->method - 1;
    memcpy(st->method, req->raw_method.ptr, n);
    st->method[n] = '\0';
    n = req->target.len < sizeof st->target - 1 ? req->target.len
                                                : sizeof st->target - 1;
    memcpy(st->target, req->target.ptr, n);
    st->target[n] = '\0';
    st->body_len = req->body.len;
    n = req->body.len < sizeof st->body - 1 ? req->body.len
                                            : sizeof st->body - 1;
    memcpy(st->body, req->body.ptr, n);
    st->body[n] = '\0';
    st->tls = req->tls;
    n = req->peer_ip.len < sizeof st->peer - 1 ? req->peer_ip.len
                                               : sizeof st->peer - 1;
    memcpy(st->peer, req->peer_ip.ptr, n);
    st->peer[n] = '\0';
    pthread_mutex_unlock(&st->mutex);

    cf_response resp;
    cf_response_init(&resp);
    resp.status = 200;
    cf_buf *body = NULL;
    if (cf_buf_copy((cf_span){(const unsigned char *)"ok", 2}, &body) !=
        CF_OK) {
        return CF_NOMEM;
    }
    cf_err rc = cf_response_body(&resp, body);
    cf_buf_release(body);
    if (rc == CF_OK) {
        rc = cf_response_header(&resp, (cf_span){(const unsigned char *)"Content-Type", 12},
                                (cf_span){(const unsigned char *)"text/plain", 10});
    }
    if (rc == CF_OK) {
        rc = cf_http_task_submit(task, &resp);
    }
    cf_response_dispose(&resp);
    return rc == CF_OK ? CF_OK : CF_BUSY;
}
