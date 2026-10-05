/* Test-only A00 integration harness; see serve_harness.h. */
#include "serve_harness.h"

#include "app.h"
#include "config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static void *srv_loop_main(void *arg) {
    cf_http_loop *loop = arg;
    (void)cf_http_loop_run(loop);
    return NULL;
}

cf_err srv_start(struct srv *s, size_t readers, size_t slots, size_t loops) {
    memset(s, 0, sizeof *s);
    s->listen_fd = -1;
    snprintf(s->dir, sizeof s->dir, "/tmp/a00srvXXXXXX");
    if (mkdtemp(s->dir) == NULL) return CF_IO;
    snprintf(s->db_path, sizeof s->db_path, "%s/campfire.sqlite3", s->dir);

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return CF_IO;
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(fd, 64) != 0) {
        close(fd);
        return CF_IO;
    }
    socklen_t len = sizeof addr;
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        close(fd);
        return CF_IO;
    }
    s->port = ntohs(addr.sin_port);
    s->listen_fd = fd;
    snprintf(s->origin, sizeof s->origin, "http://127.0.0.1:%u", s->port);

    char readers_text[16], slots_text[16], loops_text[16];
    snprintf(readers_text, sizeof readers_text, "%zu", readers);
    snprintf(slots_text, sizeof slots_text, "%zu", slots);
    snprintf(loops_text, sizeof loops_text, "%zu", loops);
    cf_config_entry entries[6] = {
        {"PUBLIC_ORIGIN", s->origin},
        {"SECRET_KEY_BASE", HEX64},
        {"DATABASE_PATH", s->db_path},
        {"CF_READERS", readers_text},
        {"CF_REQUEST_SLOTS", slots_text},
        {"CF_LOOPS", loops_text},
    };
    cf_config *config = NULL;
    cf_err rc = cf_config_parse(entries, 6, NULL, &config);
    if (rc != CF_OK) return rc;
    rc = cf_app_create(config, &s->app);
    if (rc != CF_OK) {
        cf_config_destroy(config);
        return rc;
    }
    rc = cf_app_start(s->app);
    if (rc != CF_OK) {
        cf_app_destroy(s->app);
        s->app = NULL;
        return rc;
    }

    cf_http_loop_config loop_config;
    memset(&loop_config, 0, sizeof loop_config);
    loop_config.listen_fd = s->listen_fd;
    loop_config.loop_index = 0;
    loop_config.public_origin = s->origin;
    loop_config.input_bytes = cf_app_loop_input_bytes(s->app, 0);
    loop_config.output_bytes = cf_app_loop_output_bytes(s->app, 0);
    loop_config.app = s->app;
    loop_config.admit = cf_app_admit;
    loop_config.admit_user = s->app;
    rc = cf_http_loop_create(&loop_config, &s->loop);
    if (rc != CF_OK) {
        cf_app_stop(s->app);
        cf_app_destroy(s->app);
        s->app = NULL;
        return rc;
    }
    if (pthread_create(&s->thread, NULL, srv_loop_main, s->loop) != 0) {
        cf_http_loop_destroy(s->loop);
        cf_app_stop(s->app);
        cf_app_destroy(s->app);
        s->app = NULL;
        return CF_INTERNAL;
    }
    s->running = true;
    return CF_OK;
}

void srv_stop(struct srv *s) {
    if (s == NULL) return;
    if (s->loop != NULL) {
        cf_http_loop_stop(s->loop);
        if (s->running) pthread_join(s->thread, NULL);
        s->running = false;
        cf_http_loop_counters(s->loop, &s->counters);
        s->counters_valid = true;
    }
    if (s->app != NULL) {
        cf_app_stop(s->app);
    }
    if (s->loop != NULL) {
        cf_http_loop_destroy(s->loop);
        s->loop = NULL;
    }
    if (s->listen_fd >= 0) {
        close(s->listen_fd);
        s->listen_fd = -1;
    }
    if (s->app != NULL) {
        cf_app_destroy(s->app);
        s->app = NULL;
    }
    if (s->dir[0] != '\0') {
        char path[192];
        snprintf(path, sizeof path, "%s/campfire.sqlite3", s->dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/campfire.sqlite3-wal", s->dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/campfire.sqlite3-shm", s->dir);
        unlink(path);
        rmdir(s->dir);
    }
}

void srv_counters(const struct srv *s, cf_http_counters *out) {
    if (!s->counters_valid) {
        memset(out, 0, sizeof *out);
        return;
    }
    *out = s->counters;
}

int srv_connect(const struct srv *s) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)s->port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int srv_send_all(int fd, const void *buf, size_t len) {
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

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

ssize_t srv_read_response(int fd, char *buf, size_t cap, int timeout_ms) {
    size_t have = 0;
    size_t header_end = 0;
    long long content_length = -1;
    int64_t deadline = now_ms() + timeout_ms;
    while (have + 1 < cap) {
        if (header_end == 0) {
            char *h = memmem(buf, have, "\r\n\r\n", 4);
            if (h != NULL) {
                header_end = (size_t)(h - buf) + 4;
                char *cl = memmem(buf, header_end, "Content-Length:", 15);
                content_length = cl != NULL ? strtoll(cl + 15, NULL, 10) : 0;
            }
        }
        if (header_end != 0 &&
            have >= header_end + (size_t)content_length) {
            buf[have] = '\0';
            return (ssize_t)have;
        }
        struct pollfd pf = {fd, POLLIN, 0};
        int rc = poll(&pf, 1, 100);
        if (rc == 0) {
            if (now_ms() >= deadline) break;
            continue;
        }
        if (rc < 0) return -1;
        size_t want = header_end == 0
                          ? 1
                          : header_end + (size_t)content_length - have;
        if (want > cap - 1 - have) want = cap - 1 - have;
        ssize_t n = recv(fd, buf + have, want, 0);
        if (n > 0) {
            have += (size_t)n;
            continue;
        }
        if (n == 0) break; /* closed: close_after responses */
        if (errno == EINTR) continue;
        return -1;
    }
    buf[have] = '\0';
    if (header_end != 0 &&
        have >= header_end + (size_t)content_length) {
        return (ssize_t)have;
    }
    return -1;
}

int srv_request(struct srv *s, const char *raw, int *fd_out, char *resp,
                size_t cap, int timeout_ms) {
    int fd = srv_connect(s);
    if (fd < 0) return -1;
    if (srv_send_all(fd, raw, strlen(raw)) != 0) {
        close(fd);
        return -1;
    }
    if (fd_out != NULL) *fd_out = fd;
    if (resp != NULL) {
        if (srv_read_response(fd, resp, cap, timeout_ms) < 0) {
            close(fd);
            if (fd_out != NULL) *fd_out = -1;
            return -1;
        }
    }
    return 0;
}
