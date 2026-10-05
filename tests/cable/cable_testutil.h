/* tests/cable/cable_testutil.h — C01 test harness. Test-only; never part of
 * the application. Real socketpairs; every wait is poll() driven with a
 * timeout.
 *
 * The wire helpers mirror the reference's tests/fixtures/crates/cable
 * (masked client frames, unmasked server frames) and use raw zlib for the
 * test-side deflate vectors. */
#ifndef CABLE_TESTUTIL_H
#define CABLE_TESTUTIL_H

#include "cable/cable.h"
#include "cf_test.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

/* ---- time ----------------------------------------------------------------- */

static inline int64_t ct_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- sockets -------------------------------------------------------------- */

static inline bool ct_socketpair(int fds[2]) {
    return socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0;
}

/* Send all bytes, enforcing the 5 s deadline even when the peer stops
 * draining: every send is non-blocking (MSG_DONTWAIT) and a full send buffer
 * is waited on with a bounded poll(), so a non-draining peer ends in a clean
 * false (EAGAIN until the deadline, or EPIPE) instead of an unbounded
 * blocking send() the deadline can never interrupt. */
static inline bool ct_send_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = buf;
    int64_t deadline = ct_now_ms() + 5000;
    while (len != 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int64_t left = deadline - ct_now_ms();
            if (left <= 0) return false;
            struct pollfd pf = {fd, POLLOUT, 0};
            int slice = left < 50 ? (int)left : 50;
            if (poll(&pf, 1, slice) < 0 && errno != EINTR) return false;
            continue;
        }
        return false;
    }
    return true;
}

static inline bool ct_send_str(int fd, const char *text) {
    return ct_send_all(fd, text, strlen(text));
}

/* Poll for readability. */
static inline bool ct_wait_readable(int fd, int timeout_ms) {
    struct pollfd pf = {fd, POLLIN, 0};
    int rc = poll(&pf, 1, timeout_ms);
    return rc > 0 && (pf.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
}

/* Read exactly n bytes or fail. */
static inline bool ct_read_exact(int fd, void *buf, size_t n, int timeout_ms) {
    unsigned char *p = buf;
    int64_t deadline = ct_now_ms() + timeout_ms;
    while (n != 0) {
        if (!ct_wait_readable(fd, 200)) {
            if (ct_now_ms() >= deadline) return false;
            continue;
        }
        ssize_t r = recv(fd, p, n, 0);
        if (r > 0) {
            p += r;
            n -= (size_t)r;
            continue;
        }
        if (r == 0) return false;
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

/* Read until the accumulated text contains `needle`, the peer closes, or the
 * timeout fires. Always NUL-terminates. */
static inline ssize_t ct_read_until(int fd, char *buf, size_t cap,
                                    const char *needle, int timeout_ms) {
    size_t have = 0;
    int64_t deadline = ct_now_ms() + timeout_ms;
    buf[0] = '\0';
    while (have + 1 < cap) {
        if (!ct_wait_readable(fd, 200)) {
            if (ct_now_ms() >= deadline) break;
            continue;
        }
        ssize_t r = recv(fd, buf + have, cap - 1 - have, 0);
        if (r > 0) {
            have += (size_t)r;
            buf[have] = '\0';
            if (needle != NULL &&
                memmem(buf, have, needle, strlen(needle)) != NULL) {
                return (ssize_t)have;
            }
            if (needle == NULL) return (ssize_t)have;
            continue;
        }
        if (r == 0) break;
        if (errno == EINTR) continue;
        return -1;
    }
    buf[have] = '\0';
    return (ssize_t)have;
}

/* Read whatever arrives within the window (NUL-terminated). */
static inline ssize_t ct_read_some(int fd, char *buf, size_t cap,
                                   int timeout_ms) {
    return ct_read_until(fd, buf, cap, NULL, timeout_ms);
}

/* Read exactly one header block (through CRLF CRLF) one byte at a time, so
 * frame bytes sent right after the headers stay in the socket. */
static inline ssize_t ct_read_http_head(int fd, char *buf, size_t cap,
                                        int timeout_ms) {
    size_t have = 0;
    int64_t deadline = ct_now_ms() + timeout_ms;
    buf[0] = '\0';
    while (have + 1 < cap) {
        if (!ct_wait_readable(fd, 200)) {
            if (ct_now_ms() >= deadline) break;
            continue;
        }
        ssize_t r = recv(fd, buf + have, 1, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            break;
        }
        have += (size_t)r;
        buf[have] = '\0';
        if (have >= 4 && memcmp(buf + have - 4, "\r\n\r\n", 4) == 0) {
            return (ssize_t)have;
        }
    }
    return -1;
}

static inline bool ct_has_data(int fd, int timeout_ms) {
    return ct_wait_readable(fd, timeout_ms);
}

/* ---- client frames (reference test helper) --------------------------------- */

static const unsigned char CT_MASK[4] = {0x12, 0x34, 0x56, 0x78};

static inline size_t ct_client_frame(unsigned char *out, unsigned opcode,
                                     bool fin, bool rsv1,
                                     const unsigned char *payload,
                                     size_t len) {
    size_t o = 0;
    out[o++] = (unsigned char)((fin ? 0x80 : 0) | (rsv1 ? 0x40 : 0) | opcode);
    if (len < 126) {
        out[o++] = (unsigned char)(0x80 | len);
    } else if (len <= 65535) {
        out[o++] = 0x80 | 126;
        out[o++] = (unsigned char)(len >> 8);
        out[o++] = (unsigned char)(len & 0xFF);
    } else {
        out[o++] = 0x80 | 127;
        for (int i = 0; i < 8; i++) {
            out[o++] = (unsigned char)((uint64_t)len >> (56 - 8 * i));
        }
    }
    memcpy(out + o, CT_MASK, 4);
    o += 4;
    for (size_t i = 0; i < len; i++) {
        out[o++] = (unsigned char)(payload[i] ^ CT_MASK[i & 3]);
    }
    return o;
}

static inline bool ct_send_client_frame(int fd, unsigned opcode, bool fin,
                                        bool rsv1,
                                        const unsigned char *payload,
                                        size_t len) {
    unsigned char *frame = malloc(len + 16);
    if (frame == NULL) return false;
    size_t n = ct_client_frame(frame, opcode, fin, rsv1, payload, len);
    bool ok = ct_send_all(fd, frame, n);
    free(frame);
    return ok;
}

/* One unmasked server frame. Returns payload length, or -1. */
static inline ssize_t ct_read_server_frame(int fd, unsigned *opcode,
                                           bool *rsv1, unsigned char *payload,
                                           size_t cap, int timeout_ms) {
    unsigned char head[2];
    if (!ct_read_exact(fd, head, 2, timeout_ms)) return -1;
    *opcode = head[0] & 0x0F;
    *rsv1 = (head[0] & 0x40) != 0;
    if ((head[1] & 0x80) != 0) return -1; /* server frames are unmasked */
    uint64_t len = head[1] & 0x7F;
    if (len == 126) {
        unsigned char ext[2];
        if (!ct_read_exact(fd, ext, 2, timeout_ms)) return -1;
        len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        unsigned char ext[8];
        if (!ct_read_exact(fd, ext, 8, timeout_ms)) return -1;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }
    if (len > cap) return -1;
    if (len != 0 && !ct_read_exact(fd, payload, (size_t)len, timeout_ms)) {
        return -1;
    }
    return (ssize_t)len;
}

/* ---- raw deflate/inflate for vectors -------------------------------------- */

static inline bool ct_deflate_raw(const unsigned char *in, size_t in_len,
                                  unsigned char *out, size_t out_cap,
                                  size_t *out_len) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }
    z.next_in = (Bytef *)(uintptr_t)in;
    z.avail_in = (uInt)in_len;
    z.next_out = out;
    z.avail_out = (uInt)out_cap;
    int rc = deflate(&z, Z_SYNC_FLUSH);
    size_t produced = out_cap - z.avail_out;
    /* RFC 7692: drop the sync flush's trailing empty block. */
    if (produced >= 4 && memcmp(out + produced - 4, "\x00\x00\xff\xff", 4) == 0) {
        produced -= 4;
    }
    deflateEnd(&z);
    if (rc != Z_OK) return false;
    *out_len = produced;
    return true;
}

static inline bool ct_inflate_raw(const unsigned char *in, size_t in_len,
                                  unsigned char *out, size_t out_cap,
                                  size_t *out_len) {
    unsigned char *framed = malloc(in_len + 4);
    if (framed == NULL) return false;
    memcpy(framed, in, in_len);
    memcpy(framed + in_len, "\x00\x00\xff\xff", 4);
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit2(&z, -15) != Z_OK) {
        free(framed);
        return false;
    }
    z.next_in = framed;
    z.avail_in = (uInt)(in_len + 4);
    z.next_out = out;
    z.avail_out = (uInt)out_cap;
    int rc = inflate(&z, Z_NO_FLUSH);
    size_t produced = out_cap - z.avail_out;
    inflateEnd(&z);
    free(framed);
    if (rc != Z_OK && rc != Z_STREAM_END) return false;
    *out_len = produced;
    return true;
}

/* ---- running cf_cable_socket_run on a socketpair --------------------------- */

typedef struct {
    pthread_t thread;
    int fd;      /* server side; run uses it and leaves it open */
    int peer_fd; /* the test's client side */
    bool deflate;
    const unsigned char *handshake;
    size_t handshake_len;
    const unsigned char *pending;
    size_t pending_len;
    cf_cable_request request;
    cf_cable_hooks hooks;
    cf_cable_limits limits;
    cf_cable_socket_stats stats;
    cf_err run_rc;
} ct_run;

static inline void *ct_run_main(void *arg) {
    ct_run *run = arg;
    cf_cable_socket_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.fd = run->fd;
    cfg.deflate = run->deflate;
    cfg.handshake = run->handshake;
    cfg.handshake_len = run->handshake_len;
    cfg.pending = run->pending;
    cfg.pending_len = run->pending_len;
    cfg.request = &run->request;
    run->run_rc = cf_cable_socket_run(&cfg, &run->hooks, &run->limits,
                                      &run->stats);
    return NULL;
}

/* Start a run with a caller-supplied request (headers the authenticate hook
 * reads). `request` must stay valid until cf_cable_socket_run's authenticate
 * call; the caller keeps it in the test frame. */
static inline bool ct_run_start_with_request(ct_run *run,
                                             const cf_cable_hooks *hooks,
                                             const cf_cable_limits *limits,
                                             bool deflate,
                                             const cf_cable_request *request) {
    memset(run, 0, sizeof *run);
    int fds[2];
    if (!ct_socketpair(fds)) return false;
    run->fd = fds[0];
    run->peer_fd = fds[1];
    run->deflate = deflate;
    if (hooks != NULL) run->hooks = *hooks;
    if (limits != NULL) {
        run->limits = *limits;
    } else {
        cf_cable_limits_default(&run->limits);
    }
    if (request != NULL) {
        run->request = *request;
    } else {
        run->request.method = CF_GET;
    }
    if (pthread_create(&run->thread, NULL, ct_run_main, run) != 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    return true;
}

static inline bool ct_run_start(ct_run *run, const cf_cable_hooks *hooks,
                                const cf_cable_limits *limits, bool deflate) {
    return ct_run_start_with_request(run, hooks, limits, deflate, NULL);
}

/* Join the run thread (the peer must have been closed or the socket shut
 * down first when the connection would otherwise stay open). */
static inline void ct_run_join(ct_run *run) {
    pthread_join(run->thread, NULL);
}

static inline void ct_run_close(ct_run *run) {
    close(run->peer_fd);
    run->peer_fd = -1;
}

static inline void ct_run_finish(ct_run *run) {
    if (run->fd >= 0) close(run->fd);
    if (run->peer_fd >= 0) close(run->peer_fd);
}

/* ---- common hook doubles --------------------------------------------------- */

typedef struct {
    bool authenticated;
    int64_t user_id;
    int auth_calls;
    char text[64][256];
    size_t text_len[64];
    int text_count;
} ct_capture;

static inline cf_err ct_auth_capture(void *user,
                                     const cf_cable_request *request,
                                     bool *authenticated, int64_t *user_id) {
    ct_capture *cap = user;
    (void)request;
    cap->auth_calls++;
    *authenticated = cap->authenticated;
    *user_id = cap->user_id;
    return CF_OK;
}

static inline cf_err ct_on_text_capture(void *user, cf_cable_socket *socket,
                                        cf_span text) {
    ct_capture *cap = user;
    if (cap->text_count < 64) {
        size_t n = text.len < sizeof cap->text[0] - 1 ? text.len
                                                      : sizeof cap->text[0] - 1;
        memcpy(cap->text[cap->text_count], text.ptr, n);
        cap->text[cap->text_count][n] = '\0';
        cap->text_len[cap->text_count] = text.len;
        cap->text_count++;
    }
    (void)socket;
    return CF_OK;
}

static inline cf_err ct_on_text_echo(void *user, cf_cable_socket *socket,
                                     cf_span text) {
    (void)user;
    return cf_cable_socket_send_text(socket, text);
}

#endif /* CABLE_TESTUTIL_H */
