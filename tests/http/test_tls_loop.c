/* P01 loop-level TLS/H2 tests (FRONT-01/02): the real serving loop with a
 * loop-shared TLS server over loopback.
 *
 *  - TLS handshake against the pinned test CA, ALPN http/1.1 vs h2
 *    selection, and a plaintext-rejection probe (no silent HTTP fallback).
 *  - One multiplexed H2 pair through the REAL admit hook (frozen requests
 *    carry tls=true, bodies intact; responses route back into h2 DATA).
 *  - Revocation with an outstanding H2 stream: abandon (worker-side cancel)
 *    RSTs the stream while a sibling stream still completes, the session
 *    survives, and a peer-RST completion releases without writing.
 *
 * Real loopback sockets + a real loop thread; every wait is poll()-based
 * with a deadline. Run from the worktree root so the fixtures resolve; a
 * missing prerequisite FAILS, never skips. Reuses the I01 test-CA fixtures
 * (SAN: www.example.com); no new framework. */
#include "cf.h"
#include "cf_test.h"
#include "front/h2.h"
#include "front/tls.h"
#include "http/http.h"
#include "http/http_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <nghttp2/nghttp2.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const char *kCert =
    "tests/fixtures/crates/campfire/src/integrations/testdata/tls/server.pem";
static const char *kKey =
    "tests/fixtures/crates/campfire/src/integrations/testdata/tls/server.key";
static const char *kCA =
    "tests/fixtures/crates/campfire/src/integrations/testdata/tls/ca.pem";

#define TLS_LOOP_DEADLINE_MS 10000

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------- loop harness ------------------------- */

struct tloop {
    cf_http_loop *loop;
    cf_front_tls_server *tls;
    int listen_fd;
    pthread_t thread;
    unsigned port;
    char origin[96];
};

static void *tloop_run(void *arg) {
    struct tloop *t = arg;
    (void)cf_http_loop_run(t->loop);
    return NULL;
}

static int tloop_listen(unsigned *port_out) {
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

/* Admit hook: echoes the request target as the body. Hold mode captures
 * tasks for the revocation test instead of answering. */
struct echo_state {
    pthread_mutex_t mutex;
    unsigned admits;
    unsigned tls_count;
    size_t last_body_len;
    char last_target[256];
    bool hold;
    cf_http_task *held[16];
    size_t held_count;
};

static cf_response echo_reply(const char *body, size_t len) {
    cf_response r;
    cf_response_init(&r);
    r.status = 200;
    cf_buf *buf = NULL;
    if (cf_buf_copy((cf_span){(const unsigned char *)body, len}, &buf) ==
        CF_OK) {
        (void)cf_response_body(&r, buf);
        cf_buf_release(buf);
    }
    (void)cf_response_header(
        &r, (cf_span){(const unsigned char *)"Content-Type", 12},
        (cf_span){(const unsigned char *)"text/plain", 10});
    return r;
}

static cf_err echo_admit(void *user, cf_http_task *task) {
    struct echo_state *st = user;
    const cf_request *req = cf_http_task_request(task);
    if (req == NULL) return CF_BUSY;
    pthread_mutex_lock(&st->mutex);
    st->admits++;
    if (req->tls) st->tls_count++;
    st->last_body_len = req->body.len;
    size_t n = req->target.len < sizeof st->last_target - 1
                   ? req->target.len
                   : sizeof st->last_target - 1;
    memcpy(st->last_target, req->target.ptr, n);
    st->last_target[n] = '\0';
    bool hold = st->hold && st->held_count < 16;
    if (hold) st->held[st->held_count++] = task;
    pthread_mutex_unlock(&st->mutex);
    if (hold) return CF_OK; /* test completes it later (or abandons) */
    cf_response r = echo_reply((const char *)req->target.ptr,
                               req->target.len);
    cf_err rc = cf_http_task_submit(task, &r);
    cf_response_dispose(&r);
    return rc == CF_OK ? CF_OK : CF_BUSY;
}

static struct tloop *tloop_start(struct echo_state *st) {
    struct tloop *t = calloc(1, sizeof *t);
    if (t == NULL) return NULL;
    t->listen_fd = -1;
    FILE *f = fopen(kCert, "r");
    if (f == NULL) goto fail;
    fclose(f);
    char err[256];
    if (cf_front_tls_server_create(kCert, kKey, &t->tls, err, sizeof err) !=
        CF_OK) {
        goto fail;
    }
    t->listen_fd = tloop_listen(&t->port);
    if (t->listen_fd < 0) goto fail;
    snprintf(t->origin, sizeof t->origin, "https://127.0.0.1:%u", t->port);
    cf_http_loop_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.listen_fd = t->listen_fd;
    cfg.public_origin = t->origin;
    cfg.admit = echo_admit;
    cfg.admit_user = st;
    if (cf_http_loop_create(&cfg, &t->loop) != CF_OK) goto fail;
    cf_http_loop_set_tls_server(t->loop, t->tls);
    if (pthread_create(&t->thread, NULL, tloop_run, t) != 0) goto fail;
    return t;
fail: {
    if (t->loop != NULL) cf_http_loop_destroy(t->loop);
    if (t->listen_fd >= 0) close(t->listen_fd);
    if (t->tls != NULL) cf_front_tls_server_destroy(t->tls);
    free(t);
    return NULL;
}
}

static void tloop_stop(struct tloop *t, cf_http_counters *out) {
    if (t == NULL) return;
    if (t->loop != NULL) {
        cf_http_loop_stop(t->loop);
        pthread_join(t->thread, NULL);
        if (out != NULL) cf_http_loop_counters(t->loop, out);
        cf_http_loop_destroy(t->loop);
    }
    if (t->listen_fd >= 0) close(t->listen_fd);
    if (t->tls != NULL) cf_front_tls_server_destroy(t->tls);
    free(t);
}

/* ------------------------- TLS client helpers ------------------------- */

static SSL_CTX *client_ctx(const unsigned char *alpn, unsigned alpn_len) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) return NULL;
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1) goto fail;
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_load_verify_locations(ctx, kCA, NULL) != 1) goto fail;
    if (SSL_CTX_set_alpn_protos(ctx, alpn, alpn_len) != 0) goto fail;
    /* The fixture cert names www.example.com; verify it while dialing
     * loopback. */
    {
        X509_VERIFY_PARAM *param = SSL_CTX_get0_param(ctx);
        X509_VERIFY_PARAM_set_hostflags(param, 0);
        if (X509_VERIFY_PARAM_set1_host(param, "www.example.com", 15) !=
            1) {
            goto fail;
        }
    }
    return ctx;
fail:
    SSL_CTX_free(ctx);
    ERR_clear_error();
    return NULL;
}

static int tcp_connect(unsigned port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
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

/* SSL_write of exactly len bytes (poll-driven, deadline). 0 on success. */
static int ssl_write_all(SSL *ssl, int fd, const void *buf, size_t len,
                         int64_t deadline) {
    const unsigned char *p = buf;
    while (len != 0) {
        int r = SSL_write(ssl, p, len > 16384 ? 16384 : (int)len);
        if (r > 0) {
            p += r;
            len -= (size_t)r;
            continue;
        }
        int e = SSL_get_error(ssl, r);
        short want = 0;
        if (e == SSL_ERROR_WANT_READ) {
            want = POLLIN;
        } else if (e == SSL_ERROR_WANT_WRITE) {
            want = POLLOUT;
        } else {
            ERR_clear_error();
            return -1;
        }
        int64_t left = deadline - now_ms();
        if (left <= 0) return -1;
        struct pollfd pf = {fd, want, 0};
        if (poll(&pf, 1, (int)(left > 60000 ? 60000 : left)) <= 0) return -1;
    }
    return 0;
}

/* One SSL_read chunk (poll-driven). >0 bytes, 0 clean shutdown, -1 error. */
static ssize_t ssl_read_some(SSL *ssl, int fd, void *buf, size_t cap,
                             int64_t deadline) {
    for (;;) {
        int r = SSL_read(ssl, buf, cap > 16384 ? 16384 : (int)cap);
        if (r > 0) return r;
        if (r == 0) {
            ERR_clear_error();
            return 0;
        }
        int e = SSL_get_error(ssl, r);
        short want = 0;
        if (e == SSL_ERROR_WANT_READ) {
            want = POLLIN;
        } else if (e == SSL_ERROR_WANT_WRITE) {
            want = POLLOUT;
        } else {
            ERR_clear_error();
            return -1;
        }
        int64_t left = deadline - now_ms();
        if (left <= 0) return -1;
        struct pollfd pf = {fd, want, 0};
        if (poll(&pf, 1, (int)(left > 60000 ? 60000 : left)) <= 0) return -1;
    }
}

/* Read one H1 response over TLS: headers, then Content-Length bytes. */
static ssize_t tls_read_response(SSL *ssl, int fd, char *buf, size_t cap,
                                 int64_t deadline) {
    size_t have = 0;
    size_t header_end = 0;
    long long content_length = -1;
    while (have + 1 < cap) {
        if (header_end == 0) {
            char *h = memmem(buf, have, "\r\n\r\n", 4);
            if (h != NULL) {
                header_end = (size_t)(h - buf) + 4;
                char *cl = memmem(buf, header_end, "Content-Length:", 15);
                content_length = cl != NULL ? strtoll(cl + 15, NULL, 10)
                                            : 0;
                if (have >= header_end + (size_t)content_length) break;
            }
        } else if (have >= header_end + (size_t)content_length) {
            break;
        }
        if (now_ms() >= deadline) return -1;
        ssize_t n = ssl_read_some(ssl, fd, buf + have, cap - 1 - have,
                                  deadline);
        if (n <= 0) return -1;
        have += (size_t)n;
        buf[have] = '\0';
    }
    buf[have] = '\0';
    return (ssize_t)have;
}

static int ssl_handshake_client(SSL *ssl, int fd, int64_t deadline) {
    for (;;) {
        int r = SSL_connect(ssl);
        if (r == 1) return 0;
        int e = SSL_get_error(ssl, r);
        short want = 0;
        if (e == SSL_ERROR_WANT_READ) {
            want = POLLIN;
        } else if (e == SSL_ERROR_WANT_WRITE) {
            want = POLLOUT;
        } else {
            ERR_clear_error();
            return -1;
        }
        int64_t left = deadline - now_ms();
        if (left <= 0) return -1;
        struct pollfd pf = {fd, want, 0};
        if (poll(&pf, 1, (int)(left > 60000 ? 60000 : left)) <= 0) return -1;
    }
}

/* ------------------------- H2 client harness ------------------------- */

#define CLI_MAX_STREAMS 32

struct cli_stream {
    int32_t id;
    bool used;
    int status; /* -1 until response HEADERS */
    unsigned char data[8192];
    size_t received;
    bool end_stream;
    bool rst;
};

struct cli_state {
    struct cli_stream streams[CLI_MAX_STREAMS];
    bool goaway;
};

static struct cli_stream *cli_find(struct cli_state *c, int32_t id,
                                   bool create) {
    for (size_t i = 0; i < CLI_MAX_STREAMS; i++) {
        if (c->streams[i].used && c->streams[i].id == id) {
            return &c->streams[i];
        }
    }
    if (!create) return NULL;
    for (size_t i = 0; i < CLI_MAX_STREAMS; i++) {
        if (!c->streams[i].used) {
            memset(&c->streams[i], 0, sizeof c->streams[i]);
            c->streams[i].used = true;
            c->streams[i].id = id;
            c->streams[i].status = -1;
            return &c->streams[i];
        }
    }
    return NULL;
}

static int h2c_on_header(nghttp2_session *s, const nghttp2_frame *f,
                         const uint8_t *name, size_t nlen,
                         const uint8_t *value, size_t vlen, uint8_t flags,
                         void *ud) {
    (void)s;
    (void)f;
    (void)flags;
    if (f->hd.type != NGHTTP2_HEADERS) return 0;
    struct cli_stream *st = cli_find(ud, f->hd.stream_id, true);
    if (st == NULL) return 0;
    if (nlen == 7 && memcmp(name, ":status", 7) == 0 && vlen == 3) {
        st->status = (int)((value[0] - '0') * 100 + (value[1] - '0') * 10 +
                           (value[2] - '0'));
    }
    return 0;
}

static int h2c_on_data(nghttp2_session *s, uint8_t flags, int32_t sid,
                       const uint8_t *data, size_t len, void *ud) {
    (void)s;
    (void)flags;
    struct cli_stream *st = cli_find(ud, sid, true);
    if (st == NULL) return 0;
    size_t room = st->received < sizeof st->data
                      ? sizeof st->data - st->received
                      : 0;
    size_t n = len < room ? len : room;
    if (n != 0) {
        memcpy(st->data + st->received, data, n);
        st->received += n;
    }
    return 0;
}

static int h2c_on_frame(nghttp2_session *s, const nghttp2_frame *f,
                        void *ud) {
    (void)s;
    struct cli_state *c = ud;
    if (f->hd.type == NGHTTP2_RST_STREAM) {
        struct cli_stream *st = cli_find(c, f->hd.stream_id, true);
        if (st != NULL) st->rst = true;
    } else if (f->hd.type == NGHTTP2_DATA ||
               f->hd.type == NGHTTP2_HEADERS) {
        if ((f->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
            struct cli_stream *st = cli_find(c, f->hd.stream_id, true);
            if (st != NULL) st->end_stream = true;
        }
    } else if (f->hd.type == NGHTTP2_GOAWAY) {
        c->goaway = true;
    }
    return 0;
}

struct h2client {
    SSL *ssl;
    int fd;
    nghttp2_session *sess;
    struct cli_state state;
    char authority[64];
};

/* Move every pending byte in both directions once. */
static int h2pump_once(struct h2client *c, int64_t deadline) {
    const uint8_t *d = NULL;
    nghttp2_ssize n = nghttp2_session_mem_send(c->sess, &d);
    if (n < 0) return -1;
    if (n > 0 &&
        ssl_write_all(c->ssl, c->fd, d, (size_t)n, deadline) != 0) {
        return -1;
    }
    struct pollfd pf = {c->fd, POLLIN, 0};
    int64_t left = deadline - now_ms();
    if (left <= 0) return 1; /* out of time, not fatal yet */
    if (poll(&pf, 1, (int)(left > 200 ? 200 : left)) > 0) {
        unsigned char buf[65536];
        ssize_t r = ssl_read_some(c->ssl, c->fd, buf, sizeof buf,
                                  deadline);
        if (r < 0) return -1;
        if (r > 0 &&
            nghttp2_session_mem_recv(c->sess, buf, (size_t)r) < 0) {
            return -1;
        }
    }
    return 0;
}

static int h2pump_until(struct h2client *c, int64_t deadline) {
    for (;;) {
        if (h2pump_once(c, deadline) != 0) return -1;
        if (!nghttp2_session_want_write(c->sess)) {
            struct pollfd pf = {c->fd, POLLIN, 0};
            if (poll(&pf, 1, 50) <= 0) return 0; /* quiescent */
        }
        if (now_ms() >= deadline) return -1;
    }
}

/* Upload state for client POST bodies. The provider borrows the caller's
 * body until the request flushes; tests are synchronous, and the small
 * static pool is never exhausted by the few streams used here. */
struct h2c_upload {
    const uint8_t *base;
    size_t len;
    size_t off;
};

static struct h2c_upload h2c_uploads[CLI_MAX_STREAMS];
static size_t h2c_upload_next = 0;

static ssize_t h2c_up_read(nghttp2_session *session, int32_t stream_id,
                           uint8_t *buf, size_t length,
                           uint32_t *data_flags,
                           nghttp2_data_source *source, void *user_data) {
    (void)session;
    (void)stream_id;
    (void)user_data;
    struct h2c_upload *u = source != NULL ? source->ptr : NULL;
    if (u == NULL) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    size_t rem = u->len - u->off;
    size_t n = rem < length ? rem : length;
    if (n != 0) {
        memcpy(buf, u->base + u->off, n);
        u->off += n;
    }
    if (u->off >= u->len) *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return (ssize_t)n;
}

static int32_t h2c_get(struct h2client *c, const char *path,
                       const uint8_t *body, size_t body_len) {
    nghttp2_nv nva[4];
    nva[0].name = (uint8_t *)":method";
    nva[0].namelen = 7;
    nva[0].value = (uint8_t *)(body != NULL ? "POST" : "GET");
    nva[0].valuelen = body != NULL ? 4 : 3;
    nva[0].flags = NGHTTP2_NV_FLAG_NONE;
    nva[1].name = (uint8_t *)":scheme";
    nva[1].namelen = 7;
    nva[1].value = (uint8_t *)"https";
    nva[1].valuelen = 5;
    nva[1].flags = NGHTTP2_NV_FLAG_NONE;
    nva[2].name = (uint8_t *)":path";
    nva[2].namelen = 5;
    nva[2].value = (uint8_t *)path;
    nva[2].valuelen = strlen(path);
    nva[2].flags = NGHTTP2_NV_FLAG_NONE;
    nva[3].name = (uint8_t *)":authority";
    nva[3].namelen = 10;
    nva[3].value = (uint8_t *)c->authority;
    nva[3].valuelen = strlen(c->authority);
    nva[3].flags = NGHTTP2_NV_FLAG_NONE;
    nghttp2_data_provider prd;
    nghttp2_data_provider *prdp = NULL;
    if (body != NULL) {
        struct h2c_upload *u =
            &h2c_uploads[h2c_upload_next++ % CLI_MAX_STREAMS];
        u->base = body;
        u->len = body_len;
        u->off = 0;
        prd.source.ptr = u;
        prd.read_callback = h2c_up_read;
        prdp = &prd;
    }
    int32_t id = nghttp2_submit_request(c->sess, NULL, nva, 4, prdp,
                                        NULL);
    if (id > 0) cli_find(&c->state, id, true); /* observe from submit */
    return id;
}

static bool h2client_start(struct h2client *c, unsigned port,
                           const unsigned char *alpn, unsigned alpn_len) {
    memset(c, 0, sizeof *c);
    SSL_CTX *ctx = client_ctx(alpn, alpn_len);
    if (ctx == NULL) return false;
    c->fd = tcp_connect(port);
    if (c->fd < 0) {
        SSL_CTX_free(ctx);
        return false;
    }
    c->ssl = SSL_new(ctx);
    SSL_CTX_free(ctx);
    if (c->ssl == NULL) {
        close(c->fd);
        return false;
    }
    SSL_set_connect_state(c->ssl);
    if (SSL_set_fd(c->ssl, c->fd) != 1) goto fail;
    if (ssl_handshake_client(c->ssl, c->fd,
                             now_ms() + TLS_LOOP_DEADLINE_MS) != 0) {
        goto fail;
    }
    snprintf(c->authority, sizeof c->authority, "127.0.0.1:%u", port);
    nghttp2_session_callbacks *cbs = NULL;
    if (nghttp2_session_callbacks_new(&cbs) != 0) goto fail;
    nghttp2_session_callbacks_set_on_header_callback(cbs, h2c_on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs,
                                                              h2c_on_data);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs,
                                                         h2c_on_frame);
    int rc = nghttp2_session_client_new(&c->sess, cbs, &c->state);
    nghttp2_session_callbacks_del(cbs);
    if (rc != 0) goto fail;
    if (nghttp2_submit_settings(c->sess, NGHTTP2_FLAG_NONE, NULL, 0) !=
        0) {
        goto fail;
    }
    /* Client preface + server SETTINGS exchange. */
    if (h2pump_until(c, now_ms() + TLS_LOOP_DEADLINE_MS) != 0) goto fail;
    return true;
fail:
    if (c->sess != NULL) nghttp2_session_del(c->sess);
    if (c->ssl != NULL) SSL_free(c->ssl);
    if (c->fd >= 0) close(c->fd);
    memset(c, 0, sizeof *c);
    c->fd = -1;
    return false;
}

static void h2client_stop(struct h2client *c) {
    if (c->sess != NULL) nghttp2_session_del(c->sess);
    if (c->ssl != NULL) SSL_free(c->ssl);
    if (c->fd >= 0) close(c->fd);
    memset(c, 0, sizeof *c);
}

/* Wait until cond(state) or deadline. */
static bool wait_for(const char *what, struct echo_state *st, unsigned want,
                     int64_t deadline) {
    (void)what;
    for (;;) {
        pthread_mutex_lock(&st->mutex);
        unsigned n = st->admits;
        pthread_mutex_unlock(&st->mutex);
        if (n >= want) return true;
        if (now_ms() >= deadline) return false;
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
}

/* ------------------------- cases ------------------------- */

static const unsigned char kAlpnH2[] = {2, 'h', '2'};
static const unsigned char kAlpnH1[] = {8, 'h', 't', 't',
                                        'p', '/', '1', '.', '1'};

CF_TEST(tls_h1_handshake_and_echo) {
    struct echo_state st;
    memset(&st, 0, sizeof st);
    pthread_mutex_init(&st.mutex, NULL);
    struct tloop *t = tloop_start(&st);
    CF_REQUIRE(t != NULL);
    int64_t deadline = now_ms() + TLS_LOOP_DEADLINE_MS;

    SSL_CTX *ctx = client_ctx(kAlpnH1, (unsigned)sizeof kAlpnH1);
    CF_REQUIRE(ctx != NULL);
    int fd = tcp_connect(t->port);
    CF_REQUIRE(fd >= 0);
    SSL *ssl = SSL_new(ctx);
    CF_REQUIRE(ssl != NULL);
    SSL_set_connect_state(ssl);
    CF_REQUIRE(SSL_set_fd(ssl, fd) == 1);
    CF_REQUIRE(ssl_handshake_client(ssl, fd, deadline) == 0);
    const unsigned char *sel = NULL;
    unsigned int sel_len = 0;
    SSL_get0_alpn_selected(ssl, &sel, &sel_len);
    CF_REQUIRE(sel != NULL && sel_len == 8 &&
               memcmp(sel, "http/1.1", 8) == 0);

    char req[256];
    snprintf(req, sizeof req,
             "GET /hello HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n", t->port);
    CF_REQUIRE(ssl_write_all(ssl, fd, req, strlen(req), deadline) == 0);
    char resp[4096];
    CF_REQUIRE(tls_read_response(ssl, fd, resp, sizeof resp, deadline) >
               0);
    CF_CHECK(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    CF_CHECK(strstr(resp, "\r\n\r\n/hello") != NULL);

    pthread_mutex_lock(&st.mutex);
    CF_CHECK(st.admits == 1);
    CF_CHECK(st.tls_count == 1); /* the request observed TLS */
    pthread_mutex_unlock(&st.mutex);

    /* Server-side transport for the live keep-alive connection. The scan
     * reads loop-owned slots without the loop lock (the same pattern as
     * loop_accept_sets_tcp_nodelay); retry briefly so loop-thread
     * scheduling lag cannot flake it. */
    {
        cf_http_loop *loop = t->loop;
        bool saw_h1 = false;
        int64_t scan_deadline = now_ms() + 2000;
        while (!saw_h1 && now_ms() < scan_deadline) {
            for (size_t i = 0; i < loop->conn_cap; i++) {
                if (loop->conns[i].fd >= 0 &&
                    loop->conns[i].transport ==
                        CF_HTTP_TRANSPORT_TLS_H1) {
                    saw_h1 = true;
                    break;
                }
            }
            if (!saw_h1) {
                struct timespec ts = {.tv_sec = 0,
                                      .tv_nsec = 5 * 1000 * 1000};
                nanosleep(&ts, NULL);
            }
        }
        CF_CHECK(saw_h1);
    }

    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);
    cf_http_counters cnt;
    tloop_stop(t, &cnt);
    CF_CHECK(cnt.requests == 1);
    CF_CHECK(cnt.admissions == 1);
    CF_CHECK(cnt.responses_sent == 1);
    pthread_mutex_destroy(&st.mutex);
}

CF_TEST(tls_rejects_plaintext) {
    struct echo_state st;
    memset(&st, 0, sizeof st);
    pthread_mutex_init(&st.mutex, NULL);
    struct tloop *t = tloop_start(&st);
    CF_REQUIRE(t != NULL);

    /* Plaintext HTTP bytes on a TLS-mandatory loop: the handshake fails,
     * the request is never admitted, and no plaintext HTTP response is
     * ever served (the server's only output, if any, is TLS handshake
     * bytes — never "HTTP/..."). */
    int fd = tcp_connect(t->port);
    CF_REQUIRE(fd >= 0);
    static const char plain[] = "GET /plain HTTP/1.1\r\nHost: x\r\n\r\n";
    ssize_t w = send(fd, plain, sizeof plain - 1, MSG_NOSIGNAL);
    CF_REQUIRE(w == (ssize_t)(sizeof plain - 1));
    char buf[512];
    bool saw_http = false;
    int64_t deadline = now_ms() + 2000;
    for (;;) {
        struct pollfd pf = {fd, POLLIN, 0};
        int64_t left = deadline - now_ms();
        if (left <= 0) break;
        if (poll(&pf, 1, (int)(left > 500 ? 500 : left)) <= 0) break;
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n == 0) break; /* EOF: closed, never served */
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if ((size_t)n >= 5 && memcmp(buf, "HTTP/", 5) == 0) saw_http = true;
    }
    CF_CHECK(!saw_http);
    close(fd);

    pthread_mutex_lock(&st.mutex);
    CF_CHECK(st.admits == 0);
    pthread_mutex_unlock(&st.mutex);
    tloop_stop(t, NULL);
    pthread_mutex_destroy(&st.mutex);
}

CF_TEST(h2_multiplexed_pair_through_admit) {
    struct echo_state st;
    memset(&st, 0, sizeof st);
    pthread_mutex_init(&st.mutex, NULL);
    struct tloop *t = tloop_start(&st);
    CF_REQUIRE(t != NULL);
    int64_t deadline = now_ms() + TLS_LOOP_DEADLINE_MS;

    struct h2client c;
    CF_REQUIRE(h2client_start(&c, t->port, kAlpnH2,
                             (unsigned)sizeof kAlpnH2));
    {
        const unsigned char *sel = NULL;
        unsigned int sel_len = 0;
        SSL_get0_alpn_selected(c.ssl, &sel, &sel_len);
        CF_REQUIRE(sel != NULL && sel_len == 2 &&
                   memcmp(sel, "h2", 2) == 0);
    }
    static const unsigned char post_body[] = "post-bytes";
    int32_t a = h2c_get(&c, "/a", NULL, 0);
    int32_t b = h2c_get(&c, "/b", post_body, sizeof post_body - 1);
    CF_REQUIRE(a > 0 && b > 0 && a != b);
    /* Drive until both responses complete. */
    bool both_done = false;
    for (int i = 0; i < 200 && !both_done; i++) {
        CF_REQUIRE(h2pump_once(&c, deadline) == 0);
        struct cli_stream *sa = cli_find(&c.state, a, false);
        struct cli_stream *sb = cli_find(&c.state, b, false);
        both_done = sa != NULL && sb != NULL && sa->end_stream &&
                    sb->end_stream;
        if (!both_done && now_ms() >= deadline) break;
    }
    CF_REQUIRE(both_done);
    struct cli_stream *sa = cli_find(&c.state, a, false);
    struct cli_stream *sb = cli_find(&c.state, b, false);
    CF_REQUIRE(sa != NULL && sb != NULL);
    CF_CHECK(sa->status == 200 && !sa->rst);
    CF_CHECK(sb->status == 200 && !sb->rst);
    CF_CHECK(sa->received == 2 && memcmp(sa->data, "/a", 2) == 0);
    CF_CHECK(sb->received == 2 && memcmp(sb->data, "/b", 2) == 0);

    /* Both streams passed the real admit path with bodies intact. */
    CF_REQUIRE(wait_for("admits", &st, 2, deadline));
    pthread_mutex_lock(&st.mutex);
    CF_CHECK(st.admits == 2);
    CF_CHECK(st.tls_count == 2);
    pthread_mutex_unlock(&st.mutex);

    {
        bool saw_h2 = false;
        int64_t scan_deadline = now_ms() + 2000;
        while (!saw_h2 && now_ms() < scan_deadline) {
            for (size_t i = 0; i < t->loop->conn_cap; i++) {
                if (t->loop->conns[i].fd >= 0 &&
                    t->loop->conns[i].transport ==
                        CF_HTTP_TRANSPORT_TLS_H2) {
                    saw_h2 = true;
                    break;
                }
            }
            if (!saw_h2) {
                struct timespec ts = {.tv_sec = 0,
                                      .tv_nsec = 5 * 1000 * 1000};
                nanosleep(&ts, NULL);
            }
        }
        CF_CHECK(saw_h2);
    }
    h2client_stop(&c);
    cf_http_counters cnt;
    tloop_stop(t, &cnt);
    CF_CHECK(cnt.requests == 2);
    CF_CHECK(cnt.admissions == 2);
    CF_CHECK(cnt.responses_sent == 2);
    pthread_mutex_destroy(&st.mutex);
}

CF_TEST(h2_revocation_with_outstanding_stream) {
    struct echo_state st;
    memset(&st, 0, sizeof st);
    pthread_mutex_init(&st.mutex, NULL);
    st.hold = true; /* the test completes tasks by hand */
    struct tloop *t = tloop_start(&st);
    CF_REQUIRE(t != NULL);
    int64_t deadline = now_ms() + TLS_LOOP_DEADLINE_MS;

    struct h2client c;
    CF_REQUIRE(h2client_start(&c, t->port, kAlpnH2,
                             (unsigned)sizeof kAlpnH2));
    int32_t a = h2c_get(&c, "/held-a", NULL, 0);
    int32_t b = h2c_get(&c, "/held-b", NULL, 0);
    CF_REQUIRE(a > 0 && b > 0);
    /* Flush the requests out; the server admits and holds both tasks. */
    for (int i = 0; i < 50; i++) {
        CF_REQUIRE(h2pump_once(&c, deadline) == 0);
        pthread_mutex_lock(&st.mutex);
        bool have = st.held_count >= 2;
        pthread_mutex_unlock(&st.mutex);
        if (have) break;
    }
    CF_REQUIRE(wait_for("admits", &st, 2, deadline));
    pthread_mutex_lock(&st.mutex);
    CF_REQUIRE(st.held_count == 2);
    cf_http_task *ta = st.held[0];
    cf_http_task *tb = st.held[1];
    pthread_mutex_unlock(&st.mutex);

    /* Revocation sweep, worker-side: abandon the outstanding stream-A task
     * (cf_http_task_abandon is thread-safe). The loop must RST the stream
     * and release without writing; stream B still completes. */
    cf_http_task_abandon(ta);
    cf_response rb = echo_reply("/held-b", 7);
    CF_REQUIRE(cf_http_task_submit(tb, &rb) == CF_OK);
    cf_response_dispose(&rb);
    bool a_rst = false, b_done = false;
    for (int i = 0; i < 200 && !(a_rst && b_done); i++) {
        CF_REQUIRE(h2pump_once(&c, deadline) == 0);
        struct cli_stream *sra = cli_find(&c.state, a, false);
        struct cli_stream *srb = cli_find(&c.state, b, false);
        if (sra != NULL && sra->rst) a_rst = true;
        if (srb != NULL && srb->end_stream && srb->status == 200) {
            b_done = true;
        }
        if (!(a_rst && b_done) && now_ms() >= deadline) break;
    }
    CF_CHECK(a_rst); /* abandoned stream-A was cancelled, not answered */
    CF_CHECK(b_done);
    if (b_done) {
        struct cli_stream *srb = cli_find(&c.state, b, false);
        CF_CHECK(srb->received == 7 &&
                 memcmp(srb->data, "/held-b", 7) == 0);
    }

    /* Peer RST with an outstanding stream: open C, reset it client-side,
     * then submit its (already admitted) response. The completion must go
     * stale — released, never written — and the session must survive. */
    int32_t cstream = h2c_get(&c, "/held-c", NULL, 0);
    CF_REQUIRE(cstream > 0);
    for (int i = 0; i < 50; i++) {
        CF_REQUIRE(h2pump_once(&c, deadline) == 0);
        pthread_mutex_lock(&st.mutex);
        bool have = st.held_count >= 3;
        pthread_mutex_unlock(&st.mutex);
        if (have) break;
    }
    CF_REQUIRE(wait_for("admits", &st, 3, deadline));
    CF_REQUIRE(nghttp2_submit_rst_stream(c.sess, NGHTTP2_FLAG_NONE,
                                         cstream,
                                         NGHTTP2_CANCEL) == 0);
    CF_REQUIRE(h2pump_until(&c, now_ms() + 3000) == 0);
    pthread_mutex_lock(&st.mutex);
    cf_http_task *tc = st.held[2];
    pthread_mutex_unlock(&st.mutex);
    cf_response rcresp = echo_reply("/held-c", 7);
    CF_REQUIRE(cf_http_task_submit(tc, &rcresp) == CF_OK);
    cf_response_dispose(&rcresp);
    CF_REQUIRE(h2pump_until(&c, now_ms() + 3000) == 0);
    struct cli_stream *src = cli_find(&c.state, cstream, false);
    CF_REQUIRE(src != NULL);
    CF_CHECK(src->status == -1);  /* reset stream: never answered */
    CF_CHECK(src->received == 0); /* nothing written to it */

    /* The session itself survives revocation: a new stream is served. */
    pthread_mutex_lock(&st.mutex);
    st.hold = false;
    pthread_mutex_unlock(&st.mutex);
    int32_t d = h2c_get(&c, "/after", NULL, 0);
    CF_REQUIRE(d > 0);
    bool d_done = false;
    for (int i = 0; i < 200 && !d_done; i++) {
        CF_REQUIRE(h2pump_once(&c, deadline) == 0);
        struct cli_stream *srd = cli_find(&c.state, d, false);
        d_done = srd != NULL && srd->end_stream && srd->status == 200;
        if (!d_done && now_ms() >= deadline) break;
    }
    CF_CHECK(d_done);

    h2client_stop(&c);
    cf_http_counters cnt;
    tloop_stop(t, &cnt);
    CF_CHECK(cnt.admissions == 4);
    CF_CHECK(cnt.stale_completions >= 1); /* the peer-reset submit */
    CF_CHECK(cnt.responses_sent == 2);    /* B and D only */
    pthread_mutex_destroy(&st.mutex);
}

CF_TEST_MAIN()
