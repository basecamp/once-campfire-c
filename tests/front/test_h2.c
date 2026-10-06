/* P01 FRONT-02: H2 multiplexing, cancellation, flow control, GOAWAY,
 * budgets and completion-key discipline.
 *
 * nghttp2 memory transport (mem_send2/mem_recv2) between a raw client
 * session and the module's server session: real framing/HPACK, no mocks,
 * no sockets. A failing prerequisite FAILS loudly, never skips. */
#include "cf_test.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nghttp2/nghttp2.h>

#include "front/h2.h"

static const char *kOrigin = "https://example.com";

/* ------------------------------------------------ client harness */

#define CLI_MAX_STREAMS 256

typedef struct {
    int32_t id;
    int status; /* -1 when no response HEADERS yet */
    size_t received;
    unsigned char head[1024];
    size_t head_len;
    char hdrs[2048]; /* "name: value\n" per response header, lowercase */
    size_t hdrs_len;
    bool end_stream;
    bool rst;
    uint32_t rst_code;
} cli_stream;

typedef struct {
    cli_stream streams[CLI_MAX_STREAMS];
    size_t count;
    uint64_t rst_total;
    bool goaway;
} cli_state;

static cli_stream *cli_find(cli_state *c, int32_t id, bool create) {
    for (size_t i = 0; i < c->count; i++) {
        if (c->streams[i].id == id) return &c->streams[i];
    }
    if (!create || c->count >= CLI_MAX_STREAMS) return NULL;
    cli_stream *s = &c->streams[c->count++];
    memset(s, 0, sizeof *s);
    s->id = id;
    s->status = -1;
    return s;
}

static int cli_on_header(nghttp2_session *session, const nghttp2_frame *frame,
                         const uint8_t *name, size_t namelen,
                         const uint8_t *value, size_t valuelen, uint8_t flags,
                         void *user_data) {
    (void)session;
    (void)frame;
    (void)flags;
    cli_state *c = user_data;
    if (frame->hd.type != NGHTTP2_HEADERS) return 0;
    cli_stream *s = cli_find(c, frame->hd.stream_id, true);
    if (s == NULL) return 0;
    if (namelen == 7 && memcmp(name, ":status", 7) == 0 && valuelen == 3) {
        s->status = (int)((value[0] - '0') * 100 + (value[1] - '0') * 10 +
                          (value[2] - '0'));
    } else if (namelen > 0 && name[0] != ':') {
        size_t room = s->hdrs_len < sizeof s->hdrs ? sizeof s->hdrs - s->hdrs_len : 0;
        int n = snprintf(s->hdrs + s->hdrs_len, room, "%.*s: %.*s\n", (int)namelen,
                         name, (int)valuelen, value);
        if (n > 0 && (size_t)n < room) s->hdrs_len += (size_t)n;
    }
    return 0;
}

static int cli_on_data(nghttp2_session *session, uint8_t flags,
                       int32_t stream_id, const uint8_t *data, size_t len,
                       void *user_data) {
    (void)session;
    (void)data;
    (void)flags;
    cli_state *c = user_data;
    cli_stream *s = cli_find(c, stream_id, true);
    if (s == NULL) return 0;
    s->received += len;
    size_t room = s->head_len < sizeof s->head ? sizeof s->head - s->head_len
                                               : 0;
    size_t n = len < room ? len : room;
    if (n != 0) {
        memcpy(s->head + s->head_len, data, n);
        s->head_len += n;
    }
    return 0;
}

static int cli_on_frame(nghttp2_session *session, const nghttp2_frame *frame,
                        void *user_data) {
    (void)session;
    cli_state *c = user_data;
    if (frame->hd.type == NGHTTP2_RST_STREAM) {
        cli_stream *s = cli_find(c, frame->hd.stream_id, true);
        if (s != NULL) {
            s->rst = true;
            s->rst_code = frame->rst_stream.error_code;
        }
        c->rst_total++;
    } else if (frame->hd.type == NGHTTP2_DATA) {
        if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
            cli_stream *s = cli_find(c, frame->hd.stream_id, true);
            if (s != NULL) s->end_stream = true;
        }
    } else if (frame->hd.type == NGHTTP2_HEADERS) {
        if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
            cli_stream *s = cli_find(c, frame->hd.stream_id, true);
            if (s != NULL) s->end_stream = true;
        }
    } else if (frame->hd.type == NGHTTP2_GOAWAY) {
        c->goaway = true;
    }
    return 0;
}

static int cli_on_close(nghttp2_session *session, int32_t stream_id,
                        uint32_t error_code, void *user_data) {
    (void)session;
    (void)error_code;
    (void)user_data;
    (void)stream_id;
    return 0;
}

typedef struct {
    nghttp2_session *cli;
    cli_state state;
    cf_front_h2_session *srv;
} rig;

static bool rig_create(rig *r, bool client_no_auto_window) {
    memset(r, 0, sizeof *r);
    if (cf_front_h2_session_create(kOrigin, &r->srv) != CF_OK) return false;
    nghttp2_session_callbacks *cbs = NULL;
    if (nghttp2_session_callbacks_new(&cbs) != 0) return false;
    nghttp2_session_callbacks_set_on_header_callback(cbs, cli_on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, cli_on_data);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, cli_on_frame);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, cli_on_close);
    int rc;
    nghttp2_option *opt = NULL;
    if (client_no_auto_window) {
        if (nghttp2_option_new(&opt) != 0) {
            nghttp2_session_callbacks_del(cbs);
            return false;
        }
        nghttp2_option_set_no_auto_window_update(opt, 1);
        rc = nghttp2_session_client_new2(&r->cli, cbs, &r->state, opt);
        nghttp2_option_del(opt);
    } else {
        rc = nghttp2_session_client_new(&r->cli, cbs, &r->state);
    }
    nghttp2_session_callbacks_del(cbs);
    if (rc != 0) return false;
    /* Client connection preface + server SETTINGS exchange. */
    nghttp2_settings_entry iv;
    iv.settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
    iv.value = 1000;
    if (nghttp2_submit_settings(r->cli, NGHTTP2_FLAG_NONE, &iv, 1) != 0) {
        return false;
    }
    return true;
}

static void rig_destroy(rig *r) {
    if (r->cli != NULL) nghttp2_session_del(r->cli);
    cf_front_h2_session_destroy(r->srv);
    memset(r, 0, sizeof *r);
}

/* Move every pending byte in both directions. False only on a fatal
 * nghttp2 error. */
static bool pump_step(nghttp2_session *from, nghttp2_session *to,
                      bool *progress) {
    const uint8_t *data = NULL;
    nghttp2_ssize n = nghttp2_session_mem_send2(from, &data);
    if (n < 0) return false;
    if (n == 0) return true;
    nghttp2_ssize m = nghttp2_session_mem_recv2(to, data, (size_t)n);
    if (m < 0) return false;
    *progress = true;
    return true;
}

static bool pump(rig *r, int max_rounds) {
    nghttp2_session *srv = cf_front_h2_handle(r->srv);
    for (int i = 0; i < max_rounds; i++) {
        bool progress = false;
        if (!pump_step(r->cli, srv, &progress)) return false;
        if (!pump_step(srv, r->cli, &progress)) return false;
        if (!progress && !nghttp2_session_want_write(r->cli) &&
            !nghttp2_session_want_write(srv)) {
            return true;
        }
    }
    return true;
}

static int32_t cli_method(rig *r, const char *method, const char *path,
                          const char *authority) {
    nghttp2_nv nva[4];
    nva[0].name = (uint8_t *)":method";
    nva[0].namelen = 7;
    nva[0].value = (uint8_t *)method;
    nva[0].valuelen = strlen(method);
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
    nva[3].value = (uint8_t *)authority;
    nva[3].valuelen = strlen(authority);
    nva[3].flags = NGHTTP2_NV_FLAG_NONE;
    return nghttp2_submit_request(r->cli, NULL, nva, 4, NULL, NULL);
}

static int32_t cli_get(rig *r, const char *path, const char *authority) {
    return cli_method(r, "GET", path, authority);
}

/* ------------------------------------------------ pure header policy */

static cf_front_h2_headers_result check4(const char *m, const char *s,
                                         const char *p, const char *a) {
    const unsigned char *names[4];
    size_t nl[4];
    const unsigned char *vals[4];
    size_t vl[4];
    names[0] = (const unsigned char *)":method";
    nl[0] = 7;
    vals[0] = (const unsigned char *)m;
    vl[0] = strlen(m);
    names[1] = (const unsigned char *)":scheme";
    nl[1] = 7;
    vals[1] = (const unsigned char *)s;
    vl[1] = strlen(s);
    names[2] = (const unsigned char *)":path";
    nl[2] = 5;
    vals[2] = (const unsigned char *)p;
    vl[2] = strlen(p);
    names[3] = (const unsigned char *)":authority";
    nl[3] = 10;
    vals[3] = (const unsigned char *)a;
    vl[3] = strlen(a);
    return cf_front_h2_check_headers(names, nl, vals, vl, 4, kOrigin, NULL,
                                     NULL);
}

CF_TEST(pseudo_header_validation) {
    CF_CHECK(check4("GET", "https", "/", "example.com") ==
             CF_FRONT_H2_HEADERS_OK);
    const unsigned char *auth = NULL;
    size_t auth_len = 0;
    {
        const unsigned char *names[4] = {
            (const unsigned char *)":method",
            (const unsigned char *)":scheme",
            (const unsigned char *)":path",
            (const unsigned char *)":authority",
        };
        size_t nl[4] = {7, 7, 5, 10};
        const unsigned char *vals[4] = {
            (const unsigned char *)"GET", (const unsigned char *)"https",
            (const unsigned char *)"/r", (const unsigned char *)"example.com",
        };
        size_t vl[4] = {3, 5, 2, 11};
        CF_CHECK(cf_front_h2_check_headers(names, nl, vals, vl, 4, kOrigin,
                                           &auth, &auth_len) ==
                 CF_FRONT_H2_HEADERS_OK);
        CF_REQUIRE(auth != NULL && auth_len == 11);
        CF_CHECK(memcmp(auth, "example.com", 11) == 0);
    }
    /* Missing :path. */
    {
        const unsigned char *names[3] = {
            (const unsigned char *)":method",
            (const unsigned char *)":scheme",
            (const unsigned char *)":authority",
        };
        size_t nl[3] = {7, 7, 10};
        const unsigned char *vals[3] = {
            (const unsigned char *)"GET", (const unsigned char *)"https",
            (const unsigned char *)"example.com",
        };
        size_t vl[3] = {3, 5, 11};
        CF_CHECK(cf_front_h2_check_headers(names, nl, vals, vl, 3, kOrigin,
                                           NULL,
                                           NULL) ==
                 CF_FRONT_H2_HEADERS_MISSING_PSEUDO);
    }
    /* Duplicate :method. */
    {
        const unsigned char *names[5] = {
            (const unsigned char *)":method",
            (const unsigned char *)":method",
            (const unsigned char *)":scheme",
            (const unsigned char *)":path",
            (const unsigned char *)":authority",
        };
        size_t nl[5] = {7, 7, 7, 5, 10};
        const unsigned char *vals[5] = {
            (const unsigned char *)"GET", (const unsigned char *)"GET",
            (const unsigned char *)"https", (const unsigned char *)"/",
            (const unsigned char *)"example.com",
        };
        size_t vl[5] = {3, 3, 5, 1, 11};
        CF_CHECK(cf_front_h2_check_headers(names, nl, vals, vl, 5, kOrigin,
                                           NULL,
                                           NULL) ==
                 CF_FRONT_H2_HEADERS_DUPLICATE_PSEUDO);
    }
    /* HTTP/1-only connection header is rejected, never forwarded. */
    {
        const unsigned char *names[5] = {
            (const unsigned char *)":method",
            (const unsigned char *)":scheme",
            (const unsigned char *)":path",
            (const unsigned char *)":authority",
            (const unsigned char *)"connection",
        };
        size_t nl[5] = {7, 7, 5, 10, 10};
        const unsigned char *vals[5] = {
            (const unsigned char *)"GET", (const unsigned char *)"https",
            (const unsigned char *)"/", (const unsigned char *)"example.com",
            (const unsigned char *)"keep-alive",
        };
        size_t vl[5] = {3, 5, 1, 11, 10};
        CF_CHECK(cf_front_h2_check_headers(names, nl, vals, vl, 5, kOrigin,
                                           NULL,
                                           NULL) ==
                 CF_FRONT_H2_HEADERS_CONN_HEADER);
    }
    CF_CHECK(check4("GET", "https", "/", "evil.example") ==
             CF_FRONT_H2_HEADERS_BAD_AUTHORITY);
    CF_CHECK(check4("GET", "https", "http://x/", "example.com") ==
             CF_FRONT_H2_HEADERS_BAD_TARGET);
    /* Over the header-count budget. */
    {
        static const unsigned char *names[101];
        static size_t nl[101];
        static const unsigned char *vals[101];
        static size_t vl[101];
        names[0] = (const unsigned char *)":method";
        nl[0] = 7;
        vals[0] = (const unsigned char *)"GET";
        vl[0] = 3;
        names[1] = (const unsigned char *)":scheme";
        nl[1] = 7;
        vals[1] = (const unsigned char *)"https";
        vl[1] = 5;
        names[2] = (const unsigned char *)":path";
        nl[2] = 5;
        vals[2] = (const unsigned char *)"/";
        vl[2] = 1;
        names[3] = (const unsigned char *)":authority";
        nl[3] = 10;
        vals[3] = (const unsigned char *)"example.com";
        vl[3] = 11;
        for (size_t i = 4; i < 101; i++) {
            names[i] = (const unsigned char *)"x-pad";
            nl[i] = 5;
            vals[i] = (const unsigned char *)"v";
            vl[i] = 1;
        }
        CF_CHECK(cf_front_h2_check_headers(names, nl, vals, vl, 101, kOrigin,
                                           NULL,
                                           NULL) ==
                 CF_FRONT_H2_HEADERS_TOO_MANY);
    }
    /* Over the 32 KiB header budget. */
    {
        static unsigned char big[40 * 1024];
        memset(big, 'a', sizeof big);
        const unsigned char *names[5] = {
            (const unsigned char *)":method",
            (const unsigned char *)":scheme",
            (const unsigned char *)":path",
            (const unsigned char *)":authority",
            (const unsigned char *)"x-big",
        };
        size_t nl[5] = {7, 7, 5, 10, 5};
        const unsigned char *vals[5] = {
            (const unsigned char *)"GET", (const unsigned char *)"https",
            (const unsigned char *)"/", (const unsigned char *)"example.com",
            big,
        };
        size_t vl[5] = {3, 5, 1, 11, sizeof big};
        CF_CHECK(cf_front_h2_check_headers(names, nl, vals, vl, 5, kOrigin,
                                           NULL,
                                           NULL) ==
                 CF_FRONT_H2_HEADERS_TOO_LARGE);
    }
}

CF_TEST(authority_origin_matrix) {
    CF_CHECK(cf_front_h2_authority_allowed("example.com", 11, kOrigin));
    CF_CHECK(cf_front_h2_authority_allowed("example.com:443", 15, kOrigin));
    CF_CHECK(!cf_front_h2_authority_allowed("example.com:8443", 16, kOrigin));
    CF_CHECK(!cf_front_h2_authority_allowed("other.com", 9, kOrigin));
    CF_CHECK(!cf_front_h2_authority_allowed("", 0, kOrigin));
    CF_CHECK(cf_front_h2_authority_allowed("EXAMPLE.com", 11, kOrigin));
    CF_CHECK(cf_front_h2_authority_allowed("[::1]", 5, "http://[::1]"));
    CF_CHECK(!cf_front_h2_authority_allowed("[::1]", 5, kOrigin));
    /* Explicit-port origin: portless authority no longer matches. */
    CF_CHECK(!cf_front_h2_authority_allowed("example.com", 11,
                                            "https://example.com:8443"));
    CF_CHECK(cf_front_h2_authority_allowed("example.com:8443", 16,
                                           "https://example.com:8443"));
    /* Portless origin on a non-default port shape is impossible, but an
     * explicit default port still matches an explicit authority port. */
    CF_CHECK(cf_front_h2_authority_allowed("example.com", 11,
                                          "http://example.com"));
}

CF_TEST(completion_key_discipline) {
    cf_conn_id live = {.loop = 0, .slot = 3, .generation = 9};
    cf_front_h2_key k = {.conn = live, .sequence = 4, .stream_id = 1};
    CF_CHECK(cf_front_h2_key_valid(&k, live, 4, true));
    CF_CHECK(!cf_front_h2_key_valid(&k, live, 4, false)); /* closed */
    CF_CHECK(!cf_front_h2_key_valid(&k, live, 5, true));  /* next request */
    cf_conn_id reused = live;
    reused.generation = 10; /* slot reuse */
    CF_CHECK(!cf_front_h2_key_valid(&k, reused, 4, true));
    cf_front_h2_key even = k;
    even.stream_id = 2; /* server streams are odd */
    CF_CHECK(!cf_front_h2_key_valid(&even, live, 4, true));
    CF_CHECK(!cf_front_h2_key_valid(NULL, live, 4, true));
}

/* ------------------------------------------------ session behavior */

CF_TEST(multiplexed_requests) {
    rig r;
    CF_REQUIRE(rig_create(&r, false));
    int32_t ids[5];
    for (int i = 0; i < 5; i++) {
        ids[i] = cli_get(&r, "/", "example.com");
        CF_REQUIRE(ids[i] > 0);
    }
    CF_REQUIRE(pump(&r, 1000));
    CF_CHECK(cf_front_h2_admitted_total(r.srv) == 5);
    CF_CHECK(cf_front_h2_open_count(r.srv) == 5);
    CF_CHECK(cf_front_h2_refused_total(r.srv) == 0);
    for (int i = 0; i < 5; i++) {
        CF_CHECK(cf_front_h2_stream_open(r.srv, ids[i]));
        static const unsigned char body[] = "hello";
        CF_REQUIRE(cf_front_h2_submit_response(r.srv, ids[i], 200, body,
                                               sizeof body - 1) == CF_OK);
        CF_CHECK(cf_front_h2_stream_committed(r.srv, ids[i]));
    }
    CF_REQUIRE(pump(&r, 1000));
    for (int i = 0; i < 5; i++) {
        cli_stream *s = cli_find(&r.state, ids[i], false);
        CF_REQUIRE(s != NULL);
        CF_CHECK(s->status == 200);
        CF_CHECK(s->received == 5);
        CF_CHECK(s->end_stream);
        CF_CHECK(!s->rst);
    }
    rig_destroy(&r);
}

CF_TEST(stream_cancellation) {
    rig r;
    CF_REQUIRE(rig_create(&r, false));
    int32_t a = cli_get(&r, "/a", "example.com");
    int32_t b = cli_get(&r, "/b", "example.com");
    int32_t c = cli_get(&r, "/c", "example.com");
    CF_REQUIRE(a > 0 && b > 0 && c > 0);
    CF_REQUIRE(pump(&r, 1000));
    CF_REQUIRE(cf_front_h2_admitted_total(r.srv) == 3);
    static const unsigned char body[] = "payload";
    CF_REQUIRE(cf_front_h2_submit_response(r.srv, a, 200, body,
                                           sizeof body - 1) == CF_OK);
    CF_REQUIRE(cf_front_h2_submit_response(r.srv, b, 200, body,
                                           sizeof body - 1) == CF_OK);
    CF_REQUIRE(cf_front_h2_submit_response(r.srv, c, 200, body,
                                           sizeof body - 1) == CF_OK);
    /* Peer cancels one stream: queued output dies, the rest is unaffected. */
    CF_REQUIRE(nghttp2_submit_rst_stream(r.cli, NGHTTP2_FLAG_NONE, c,
                                         NGHTTP2_CANCEL) == 0);
    CF_REQUIRE(pump(&r, 1000));
    CF_CHECK(cf_front_h2_stream_cancelled(r.srv, c));
    CF_CHECK(!cf_front_h2_stream_open(r.srv, c));
    CF_CHECK(cf_front_h2_stream_queued(r.srv, c) == 0);
    for (int k = 0; k < 2; k++) {
        int32_t id = k == 0 ? a : b;
        cli_stream *s = cli_find(&r.state, id, false);
        CF_REQUIRE(s != NULL);
        CF_CHECK(s->status == 200 && s->received == sizeof body - 1);
        CF_CHECK(s->end_stream && !s->rst);
        /* Committed bytes stayed committed. */
        CF_CHECK(cf_front_h2_stream_sent(r.srv, id) == sizeof body - 1);
    }
    rig_destroy(&r);
}

CF_TEST(flow_control_stalls_then_resumes) {
    rig r;
    /* No automatic window updates: the client must reopen windows itself,
     * so a large response provably stalls first. */
    CF_REQUIRE(rig_create(&r, true));
    int32_t id = cli_get(&r, "/big", "example.com");
    CF_REQUIRE(id > 0);
    CF_REQUIRE(pump(&r, 1000));
    CF_REQUIRE(cf_front_h2_admitted_total(r.srv) == 1);
    static unsigned char big[256 * 1024];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (unsigned char)(i & 255);
    CF_REQUIRE(cf_front_h2_submit_response(r.srv, id, 200, big, sizeof big) ==
               CF_OK);
    CF_REQUIRE(pump(&r, 200));
    cli_stream *s = cli_find(&r.state, id, false);
    CF_REQUIRE(s != NULL);
    /* Stalled: partially delivered, server still wants to write but has no
     * allowance. */
    CF_CHECK(s->received < sizeof big);
    CF_CHECK(s->received > 0);
    CF_CHECK(cf_front_h2_flow_stalled(r.srv));
    CF_CHECK(cf_front_h2_stream_queued(r.srv, id) > 0);
    /* Reopen both levels; delivery completes. */
    CF_REQUIRE(nghttp2_submit_window_update(r.cli, NGHTTP2_FLAG_NONE, 0,
                                            16 * 1024 * 1024) == 0);
    CF_REQUIRE(nghttp2_submit_window_update(r.cli, NGHTTP2_FLAG_NONE, id,
                                            16 * 1024 * 1024) == 0);
    CF_REQUIRE(pump(&r, 4000));
    CF_CHECK(s->received == sizeof big);
    CF_CHECK(s->end_stream);
    CF_CHECK(cf_front_h2_stream_queued(r.srv, id) == 0);
    CF_CHECK(cf_front_h2_stream_sent(r.srv, id) == sizeof big);
    rig_destroy(&r);
}

CF_TEST(max_concurrent_streams) {
    rig r;
    CF_REQUIRE(rig_create(&r, false));
    for (int i = 0; i < 105; i++) {
        CF_REQUIRE(cli_get(&r, "/", "example.com") > 0);
    }
    /* Directional drain first: move every client byte to the server before
     * the client processes the server's SETTINGS. A compliant nghttp2
     * client caps itself at the advertised 100 locally, so it holds five
     * back; the server must hold the budget regardless. */
    nghttp2_session *srv = cf_front_h2_handle(r.srv);
    for (int i = 0; i < 10000; i++) {
        if (!nghttp2_session_want_write(r.cli)) break;
        const uint8_t *d = NULL;
        nghttp2_ssize n = nghttp2_session_mem_send2(r.cli, &d);
        CF_REQUIRE(n >= 0);
        if (n == 0) break;
        CF_REQUIRE(nghttp2_session_mem_recv2(srv, d, (size_t)n) >= 0);
    }
    CF_REQUIRE(pump(&r, 4000));
    CF_CHECK(cf_front_h2_admitted_total(r.srv) == 100);
    CF_CHECK(cf_front_h2_open_count(r.srv) == 100);
    /* A 101st concurrent stream (client stream 201, next after 1..199)
     * must be refused by the server itself. It is encoded with a fresh
     * HPACK deflater (empty dynamic table, literals never indexed), so
     * the block decodes independently of the session's HPACK context. */
    nghttp2_hd_deflater *deflater = NULL;
    CF_REQUIRE(nghttp2_hd_deflate_new(&deflater, 4096) == 0);
    nghttp2_nv rnva[4];
    rnva[0].name = (uint8_t *)":method";
    rnva[0].namelen = 7;
    rnva[0].value = (uint8_t *)"GET";
    rnva[0].valuelen = 3;
    rnva[0].flags = NGHTTP2_NV_FLAG_NO_INDEX;
    rnva[1].name = (uint8_t *)":scheme";
    rnva[1].namelen = 7;
    rnva[1].value = (uint8_t *)"https";
    rnva[1].valuelen = 5;
    rnva[1].flags = NGHTTP2_NV_FLAG_NO_INDEX;
    rnva[2].name = (uint8_t *)":path";
    rnva[2].namelen = 5;
    rnva[2].value = (uint8_t *)"/x";
    rnva[2].valuelen = 2;
    rnva[2].flags = NGHTTP2_NV_FLAG_NO_INDEX;
    rnva[3].name = (uint8_t *)":authority";
    rnva[3].namelen = 10;
    rnva[3].value = (uint8_t *)"example.com";
    rnva[3].valuelen = 11;
    rnva[3].flags = NGHTTP2_NV_FLAG_NO_INDEX;
    size_t bound = nghttp2_hd_deflate_bound(deflater, rnva, 4);
    CF_REQUIRE(bound > 0 && bound < 1024 * 1024);
    unsigned char *hpack = malloc(bound);
    CF_REQUIRE(hpack != NULL);
    nghttp2_ssize hpack_len =
        nghttp2_hd_deflate_hd(deflater, hpack, bound, rnva, 4);
    nghttp2_hd_deflate_del(deflater);
    CF_REQUIRE(hpack_len > 0);
    unsigned char *frame = malloc(9 + (size_t)hpack_len);
    CF_REQUIRE(frame != NULL);
    size_t hlen = (size_t)hpack_len;
    frame[0] = (unsigned char)((hlen >> 16) & 0xff);
    frame[1] = (unsigned char)((hlen >> 8) & 0xff);
    frame[2] = (unsigned char)(hlen & 0xff);
    frame[3] = 0x01; /* HEADERS */
    frame[4] = 0x05; /* END_STREAM | END_HEADERS */
    frame[5] = 0x00;
    frame[6] = 0x00;
    frame[7] = 0x00;
    frame[8] = 0xc9; /* stream 201 */
    memcpy(frame + 9, hpack, hlen);
    free(hpack);
    CF_REQUIRE(nghttp2_session_mem_recv2(srv, frame, 9 + hlen) ==
               (nghttp2_ssize)(9 + hlen));
    free(frame);
    /* Drain server output, scanning for the RST_STREAM covering 201. The
     * forged peer never sent 201 through its own session, so its bytes are
     * discarded, never fed back. */
    bool saw_rst_201 = false;
    bool saw_goaway = false;
    unsigned char *out = NULL;
    size_t out_len = 0;
    for (int i = 0; i < 1000; i++) {
        const uint8_t *d = NULL;
        nghttp2_ssize n = nghttp2_session_mem_send2(srv, &d);
        CF_REQUIRE(n >= 0);
        if (n == 0) break;
        unsigned char *grown = realloc(out, out_len + (size_t)n);
        CF_REQUIRE(grown != NULL);
        out = grown;
        memcpy(out + out_len, d, (size_t)n);
        out_len += (size_t)n;
    }
    for (size_t i = 0; i + 9 <= out_len;) {
        size_t len = ((size_t)out[i] << 16) | ((size_t)out[i + 1] << 8) |
                     (size_t)out[i + 2];
        unsigned type = out[i + 3];
        uint32_t sid = ((uint32_t)(out[i + 5] & 0x7f) << 24) |
                       ((uint32_t)out[i + 6] << 16) |
                       ((uint32_t)out[i + 7] << 8) | (uint32_t)out[i + 8];
        if (type == 0x03 && sid == 201 && len == 4) saw_rst_201 = true;
        if (type == 0x07) saw_goaway = true;
        if (len > out_len) break;
        i += 9 + len;
    }
    free(out);
    /* nghttp2 enforces the 100-stream budget itself: RST_STREAM while our
     * SETTINGS is unacknowledged, RFC 7540 section 5.1.2 GOAWAY once the
     * steady state is reached. Either way stream 201 is never admitted
     * and the connection budget holds. */
    CF_CHECK(saw_rst_201 || saw_goaway);
    if (saw_goaway) {
        CF_CHECK(cf_front_h2_draining(r.srv));
    } else {
        CF_CHECK(cf_front_h2_refused_total(r.srv) >= 1);
    }
    CF_CHECK(cf_front_h2_admitted_total(r.srv) == 100);
    CF_CHECK(cf_front_h2_open_count(r.srv) <= 100);
    CF_CHECK(!cf_front_h2_stream_open(r.srv, 201));
    rig_destroy(&r);
}

typedef struct {
    const uint8_t *base;
    size_t len;
    size_t off;
} up_state;

static ssize_t up_read(nghttp2_session *session, int32_t stream_id,
                       uint8_t *buf, size_t length, uint32_t *data_flags,
                       nghttp2_data_source *source, void *user_data) {
    (void)session;
    (void)stream_id;
    (void)user_data;
    up_state *u = source != NULL ? source->ptr : NULL;
    if (u == NULL) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    size_t remaining = u->len - u->off;
    size_t n = remaining < length ? remaining : length;
    if (n != 0) {
        memcpy(buf, u->base + u->off, n);
        u->off += n;
    }
    if (u->off >= u->len) *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return (ssize_t)n;
}

CF_TEST(body_budget_cancels_stream) {
    rig r;
    CF_REQUIRE(rig_create(&r, false));
    /* 17 MiB upload against the 16 MiB decoded-body cap. */
    static unsigned char *bulk = NULL;
    const size_t bulk_len = 17 * 1024 * 1024;
    bulk = malloc(bulk_len);
    CF_REQUIRE(bulk != NULL);
    memset(bulk, 'x', bulk_len);
    nghttp2_nv nva[5];
    nva[0].name = (uint8_t *)":method";
    nva[0].namelen = 7;
    nva[0].value = (uint8_t *)"POST";
    nva[0].valuelen = 4;
    nva[0].flags = NGHTTP2_NV_FLAG_NONE;
    nva[1].name = (uint8_t *)":scheme";
    nva[1].namelen = 7;
    nva[1].value = (uint8_t *)"https";
    nva[1].valuelen = 5;
    nva[1].flags = NGHTTP2_NV_FLAG_NONE;
    nva[2].name = (uint8_t *)":path";
    nva[2].namelen = 5;
    nva[2].value = (uint8_t *)"/up";
    nva[2].valuelen = 3;
    nva[2].flags = NGHTTP2_NV_FLAG_NONE;
    nva[3].name = (uint8_t *)":authority";
    nva[3].namelen = 10;
    nva[3].value = (uint8_t *)"example.com";
    nva[3].valuelen = 11;
    nva[3].flags = NGHTTP2_NV_FLAG_NONE;
    nva[4].name = (uint8_t *)"content-length";
    nva[4].namelen = 14;
    nva[4].value = (uint8_t *)"17825792";
    nva[4].valuelen = 8;
    nva[4].flags = NGHTTP2_NV_FLAG_NONE;
    up_state u = {.base = bulk, .len = bulk_len, .off = 0};
    nghttp2_data_provider prd;
    prd.source.ptr = &u;
    prd.read_callback = up_read;
    int32_t id = nghttp2_submit_request(r.cli, NULL, nva, 5, &prd, &u);
    CF_REQUIRE(id > 0);
    CF_REQUIRE(pump(&r, 20000));
    /* Over-budget body cancels the stream; the session survives. */
    CF_CHECK(cf_front_h2_stream_body_len(r.srv, id) <= 16 * 1024 * 1024);
    CF_CHECK(cf_front_h2_stream_cancelled(r.srv, id));
    CF_CHECK(cf_front_h2_resets_total(r.srv) >= 1);
    free(bulk);
    rig_destroy(&r);
}

CF_TEST(response_headers_reach_the_wire_lowercased) {
    rig r;
    CF_REQUIRE(rig_create(&r, false));
    int32_t id = cli_get(&r, "/", "example.com");
    CF_REQUIRE(id > 0);
    CF_REQUIRE(pump(&r, 1000));
    CF_REQUIRE(cf_front_h2_admitted_total(r.srv) == 1);
    static const unsigned char N0[] = "Content-Type";
    static const unsigned char V0[] = "text/html; charset=utf-8";
    static const unsigned char N1[] = "Set-Cookie";
    static const unsigned char V1[] = "a=1";
    static const unsigned char V2[] = "b=2";
    static const unsigned char N3[] = "Connection";
    static const unsigned char V3[] = "keep-alive";
    static const unsigned char N4[] = "Content-Length";
    static const unsigned char V4[] = "9999";
    static const unsigned char body[] = "hi";
    const unsigned char *names[] = {N0, N1, N1, N3, N4};
    size_t nlens[] = {12, 10, 10, 10, 14};
    const unsigned char *vals[] = {V0, V1, V2, V3, V4};
    size_t vlens[] = {24, 3, 3, 10, 4};
    CF_REQUIRE(cf_front_h2_submit_response_headers(
                   r.srv, id, 200, names, nlens, vals, vlens, 5, body,
                   sizeof body - 1) == CF_OK);
    CF_REQUIRE(pump(&r, 1000));
    cli_stream *st = cli_find(&r.state, id, false);
    CF_REQUIRE(st != NULL);
    CF_CHECK(st->status == 200);
    CF_CHECK(st->received == sizeof body - 1);
    /* Lowercased on the wire; duplicates kept; HTTP/1-only classes and
     * the app content-length skipped (authoritative length emitted). */
    CF_CHECK(strstr(st->hdrs, "content-type: text/html; charset=utf-8\n") !=
             NULL);
    CF_CHECK(strstr(st->hdrs, "set-cookie: a=1\n") != NULL);
    CF_CHECK(strstr(st->hdrs, "set-cookie: b=2\n") != NULL);
    CF_CHECK(strstr(st->hdrs, "content-length: 2\n") != NULL);
    CF_CHECK(strstr(st->hdrs, "connection") == NULL);
    CF_CHECK(strstr(st->hdrs, "9999") == NULL);
    CF_CHECK(st->end_stream);
    rig_destroy(&r);
}

CF_TEST(goaway_drains_accepted_streams) {
    rig r;
    CF_REQUIRE(rig_create(&r, false));
    int32_t a = cli_get(&r, "/a", "example.com");
    int32_t b = cli_get(&r, "/b", "example.com");
    CF_REQUIRE(a > 0 && b > 0);
    CF_REQUIRE(pump(&r, 1000));
    CF_REQUIRE(cf_front_h2_admitted_total(r.srv) == 2);
    static const unsigned char body[] = "drain";
    CF_REQUIRE(cf_front_h2_submit_response(r.srv, a, 200, body,
                                           sizeof body - 1) == CF_OK);
    CF_REQUIRE(cf_front_h2_submit_response(r.srv, b, 200, body,
                                           sizeof body - 1) == CF_OK);
    CF_REQUIRE(cf_front_h2_goaway(r.srv) == CF_OK);
    CF_CHECK(cf_front_h2_draining(r.srv));
    /* A request already in flight past the GOAWAY point is refused. */
    int32_t c = cli_get(&r, "/c", "example.com");
    CF_REQUIRE(c > 0);
    CF_REQUIRE(pump(&r, 2000));
    cli_stream *sa = cli_find(&r.state, a, false);
    cli_stream *sb = cli_find(&r.state, b, false);
    CF_REQUIRE(sa != NULL && sb != NULL);
    CF_CHECK(sa->status == 200 && sa->end_stream);
    CF_CHECK(sb->status == 200 && sb->end_stream);
    CF_CHECK(r.state.goaway);
    CF_CHECK(cf_front_h2_refused_total(r.srv) >= 1);
    CF_CHECK(!cf_front_h2_stream_open(r.srv, c));
    rig_destroy(&r);
}

CF_TEST(revocation_with_outstanding_streams) {
    rig r;
    CF_REQUIRE(rig_create(&r, false));
    int32_t ids[3];
    for (int i = 0; i < 3; i++) {
        ids[i] = cli_get(&r, "/", "example.com");
        CF_REQUIRE(ids[i] > 0);
    }
    CF_REQUIRE(pump(&r, 1000));
    CF_REQUIRE(cf_front_h2_admitted_total(r.srv) == 3);
    static const unsigned char body[] = "revoked?";
    for (int i = 0; i < 3; i++) {
        CF_REQUIRE(cf_front_h2_submit_response(r.srv, ids[i], 200, body,
                                               sizeof body - 1) == CF_OK);
    }
    /* Revocation sweep: cancel every outstanding stream (RST CANCEL). */
    for (int i = 0; i < 3; i++) {
        CF_REQUIRE(cf_front_h2_rst_stream(r.srv, ids[i], NGHTTP2_CANCEL) ==
                   CF_OK);
    }
    CF_REQUIRE(pump(&r, 1000));
    for (int i = 0; i < 3; i++) {
        CF_CHECK(cf_front_h2_stream_cancelled(r.srv, ids[i]));
        CF_CHECK(cf_front_h2_stream_queued(r.srv, ids[i]) == 0);
    }
    /* The session itself survives revocation: a new stream is admitted
     * and served. */
    int32_t next = cli_get(&r, "/after", "example.com");
    CF_REQUIRE(next > 0);
    CF_REQUIRE(pump(&r, 1000));
    CF_CHECK(cf_front_h2_stream_open(r.srv, next));
    CF_REQUIRE(cf_front_h2_submit_response(r.srv, next, 200, body,
                                           sizeof body - 1) == CF_OK);
    CF_REQUIRE(pump(&r, 1000));
    cli_stream *s = cli_find(&r.state, next, false);
    CF_REQUIRE(s != NULL);
    CF_CHECK(s->status == 200 && s->end_stream);
    rig_destroy(&r);
}


struct provider_state {
    uint64_t length, offset;
    bool ready;
    unsigned closed;
};

static ssize_t deferred_provider_read(void *user, unsigned char *dst,
                                     size_t max, uint32_t *flags) {
    struct provider_state *p = user;
    if (!p->ready) return NGHTTP2_ERR_DEFERRED;
    size_t n = p->length - p->offset < max ? (size_t)(p->length - p->offset) : max;
    if (n > 4096) n = 4096;
    memset(dst, 'x', n);
    p->offset += n;
    p->ready = false;
    if (p->offset == p->length) *flags |= NGHTTP2_DATA_FLAG_EOF;
    return (ssize_t)n;
}

static void deferred_provider_close(void *user) {
    ((struct provider_state *)user)->closed++;
}

CF_TEST(provider_deferred_chunks_exceed_copy_limit_and_close_once) {
    rig r;
    CF_REQUIRE(rig_create(&r, false));
    int32_t id = cli_get(&r, "/file", "example.com");
    CF_REQUIRE(pump(&r, 1000));
    struct provider_state p = {.length = 16 * 1024 * 1024};
    CF_REQUIRE(cf_front_h2_submit_response_provider(r.srv, id, 200,
        NULL, NULL, NULL, NULL, 0, p.length, deferred_provider_read,
        deferred_provider_close, &p) == CF_OK);
    CF_REQUIRE(pump(&r, 1000));
    cli_stream *st = cli_find(&r.state, id, false);
    CF_REQUIRE(st != NULL);
    CF_CHECK(st->status == 200 && !st->end_stream && st->received == 0);
    CF_CHECK(strstr(st->hdrs, "content-length: 16777216\n") != NULL);
    while (p.offset < p.length) {
        p.ready = true;
        CF_REQUIRE(cf_front_h2_resume_data(r.srv, id) == CF_OK);
        CF_REQUIRE(pump(&r, 1000));
        CF_CHECK(cf_front_h2_stream_queued(r.srv, id) == 0);
    }
    CF_CHECK(st->end_stream && st->received == p.length && !st->rst);
    CF_CHECK(p.closed == 1);
    rig_destroy(&r);
    CF_CHECK(p.closed == 1);
}

CF_TEST(provider_reset_and_destroy_release_once) {
    for (unsigned i = 0; i < 2; i++) {
        rig r;
        CF_REQUIRE(rig_create(&r, false));
        int32_t id = cli_get(&r, "/file", "example.com");
        CF_REQUIRE(pump(&r, 1000));
        struct provider_state p = {.length = 16 * 1024 * 1024};
        CF_REQUIRE(cf_front_h2_submit_response_provider(r.srv, id, 200,
            NULL, NULL, NULL, NULL, 0, p.length, deferred_provider_read,
            deferred_provider_close, &p) == CF_OK);
        CF_REQUIRE(pump(&r, 1000));
        if (i == 0) {
            CF_REQUIRE(nghttp2_submit_rst_stream(r.cli, NGHTTP2_FLAG_NONE,
                                                id, NGHTTP2_CANCEL) == 0);
            CF_REQUIRE(pump(&r, 1000));
            CF_CHECK(p.closed == 1);
            CF_CHECK(cf_front_h2_resume_data(r.srv, id) == CF_NOT_FOUND);
        }
        rig_destroy(&r);
        CF_CHECK(p.closed == 1);
    }
}

CF_TEST(provider_headers_only_preserve_representation_length) {
    rig r;
    CF_REQUIRE(rig_create(&r, false));
    int32_t id = cli_method(&r, "HEAD", "/head", "example.com");
    CF_REQUIRE(pump(&r, 1000));
    CF_REQUIRE(cf_front_h2_submit_response_provider(r.srv, id, 200,
        NULL, NULL, NULL, NULL, 0, 16777216, NULL, NULL, NULL) == CF_OK);
    CF_REQUIRE(pump(&r, 1000));
    cli_stream *st = cli_find(&r.state, id, false);
    CF_REQUIRE(st != NULL);
    CF_CHECK(st->status == 200 && st->end_stream && st->received == 0);
    CF_CHECK(strstr(st->hdrs, "content-length: 16777216\n") != NULL);
    rig_destroy(&r);
}


CF_TEST(provider_submission_failure_keeps_caller_ownership) {
    rig r;
    CF_REQUIRE(rig_create(&r, false));
    int32_t id = cli_get(&r, "/file", "example.com");
    CF_REQUIRE(pump(&r, 1000));
    struct provider_state p = {.length = 16 * 1024 * 1024};
    CF_CHECK(cf_front_h2_submit_response_provider(r.srv, id, 200,
        NULL, NULL, NULL, NULL, 129, p.length, deferred_provider_read,
        deferred_provider_close, &p) == CF_LIMIT);
    CF_CHECK(p.closed == 0);
    CF_CHECK(!cf_front_h2_stream_committed(r.srv, id));
    CF_REQUIRE(cf_front_h2_submit_response_provider(r.srv, id, 200,
        NULL, NULL, NULL, NULL, 0, p.length, deferred_provider_read,
        deferred_provider_close, &p) == CF_OK);
    CF_CHECK(cf_front_h2_submit_response_provider(r.srv, id, 200,
        NULL, NULL, NULL, NULL, 0, p.length, deferred_provider_read,
        deferred_provider_close, &p) == CF_INVALID);
    CF_CHECK(p.closed == 0);
    rig_destroy(&r);
    CF_CHECK(p.closed == 1);
}

CF_TEST(provider_flow_control_stalls_and_reset_releases_owner) {
    rig r;
    CF_REQUIRE(rig_create(&r, true));
    int32_t id = cli_get(&r, "/file", "example.com");
    CF_REQUIRE(pump(&r, 1000));
    struct provider_state p = {.length = 16 * 1024 * 1024};
    CF_REQUIRE(cf_front_h2_submit_response_provider(r.srv, id, 200,
        NULL, NULL, NULL, NULL, 0, p.length, deferred_provider_read,
        deferred_provider_close, &p) == CF_OK);
    CF_REQUIRE(pump(&r, 1000));
    for (unsigned i = 0; i < 16; i++) {
        p.ready = true;
        CF_REQUIRE(cf_front_h2_resume_data(r.srv, id) == CF_OK);
        CF_REQUIRE(pump(&r, 1000));
    }
    CF_CHECK(p.offset == 65535);
    CF_CHECK(cf_front_h2_flow_stalled(r.srv));
    uint64_t before = p.offset;
    CF_REQUIRE(pump(&r, 1000));
    CF_CHECK(p.offset == before);
    CF_REQUIRE(nghttp2_submit_rst_stream(r.cli, NGHTTP2_FLAG_NONE, id,
                                        NGHTTP2_CANCEL) == 0);
    CF_REQUIRE(pump(&r, 1000));
    CF_CHECK(p.closed == 1);
    rig_destroy(&r);
    CF_CHECK(p.closed == 1);
}

CF_TEST_MAIN()
