/* H01 event loop: level-triggered epoll, connection slots with generation
 * handles, admission handoff, completion queue (mutex + eventfd), the file
 * chunk worker and orderly shutdown.
 *
 * Connections are confined to the loop thread; integer handles are
 * (loop, slot, generation), and reusing a slot increments its 64-bit
 * generation. Task completions and file chunk results are validated against
 * both connection generation and request sequence, so a stale completion
 * releases its resources without ever touching a reused descriptor
 * (00-contracts.md CORE-03).
 */
#include "http_internal.h"

#include "app_internal.h" /* cf_app_stop_requested (F03 internal surface) */

/* P01 front seam: non-blocking TLS (tls.h), H2 policy (h2.h) and the
 * loop-owned nghttp2 observer session that materializes H2 requests.
 * request.c/response.c/output.c stay untouched: output.c's plaintext flush
 * is suppressed for TLS connections (conn->fd swap, see below) so every
 * byte leaves via TLS from this file. */
#include "front/h2.h"
#include "front/tls.h"

#include <nghttp2/nghttp2.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#define CF_HTTP_MAX_EVENTS 128

/* ------------------------------------------------------- P01 front seam */

/* Suppress output.c's plaintext flush for TLS connections: its send loop
 * no-ops while conn->fd < 0, so every byte leaves via TLS from this file.
 * Every window below is synchronous on the loop thread. conn_close consumes
 * the swap (it closes swap_fd when fd is suppressed), and conn_restore
 * re-arms epoll only when no close ran (state != CLOSED). */
static int conn_suppress(struct cf_http_conn *conn) {
    if (conn->transport == CF_HTTP_TRANSPORT_PLAIN || conn->fd < 0) {
        return -1;
    }
    conn->swap_fd = conn->fd;
    conn->fd = -1;
    return conn->swap_fd;
}

static void conn_restore(struct cf_http_loop *loop,
                         struct cf_http_conn *conn, int saved) {
    (void)loop;
    if (saved < 0) return;
    if (conn->state == CF_HTTP_STATE_CLOSED) {
        conn->swap_fd = -1; /* close consumed the real fd via swap_fd */
        return;
    }
    if (conn->fd < 0) conn->fd = saved;
    conn->swap_fd = -1;
    cf_http_conn_update_events(conn);
}

void cf_http_loop_set_tls_server(struct cf_http_loop *loop,
                                 struct cf_front_tls_server *server) {
    if (loop == NULL) return;
    loop->tls_server = server;
}

/* ------------------------- H2 observer session ------------------------- */

/* The h2 policy module (front/h2.c) owns validation, budgets, concurrency
 * and framing but drops decoded header/body bytes after policy checks, so
 * the loop cannot build a cf_request from it. The observer below is a
 * receive-only nghttp2 server session fed the same decrypted bytes: it
 * records complete header blocks + bodies per stream and never sends (its
 * mem_send output — SETTINGS acks, auto window updates — is discarded; the
 * policy session's own updates govern the peer). A stream is admitted only
 * when BOTH the observer has a complete request AND the policy session
 * reports it open, so validation/budgets/concurrency stay the module's. */
#define CF_H2OBS_MAX_STREAMS 128
#define CF_H2OBS_BODY_MAX ((size_t)16 * 1024 * 1024)
#define CF_H2OBS_FIELD_MAX ((size_t)65536)

struct h2obs_hdr {
    unsigned char *name;
    size_t nlen;
    unsigned char *val;
    size_t vlen;
};

struct h2obs_stream {
    bool in_use;
    int32_t id;
    bool headers_done;
    bool end_stream;
    bool admitted;
    bool dropped;
    bool over; /* over a local budget mirror: RST and drop, never admit */
    unsigned char method[16];
    size_t method_len;
    unsigned char path[8192];
    size_t path_len;
    unsigned char authority[256];
    size_t auth_len;
    bool have_method;
    bool have_scheme;
    bool have_path;
    bool have_authority;
    struct h2obs_hdr hdrs[CF_HTTP_HEADER_COUNT_MAX];
    size_t nhdrs;
    unsigned char *body;
    size_t body_len;
    size_t body_cap;
};

struct h2obs {
    nghttp2_session *sess;
    struct h2obs_stream streams[CF_H2OBS_MAX_STREAMS];
};

static struct h2obs_stream *obs_lookup(struct h2obs *o, int32_t id,
                                       bool create) {
    for (size_t i = 0; i < CF_H2OBS_MAX_STREAMS; i++) {
        if (o->streams[i].in_use && o->streams[i].id == id) {
            return &o->streams[i];
        }
    }
    if (!create) return NULL;
    for (size_t i = 0; i < CF_H2OBS_MAX_STREAMS; i++) {
        if (!o->streams[i].in_use) {
            memset(&o->streams[i], 0, sizeof o->streams[i]);
            o->streams[i].in_use = true;
            o->streams[i].id = id;
            return &o->streams[i];
        }
    }
    return NULL;
}

static void obs_free_bufs(struct h2obs_stream *rec) {
    for (size_t i = 0; i < rec->nhdrs; i++) {
        free(rec->hdrs[i].name);
        free(rec->hdrs[i].val);
        rec->hdrs[i].name = NULL;
        rec->hdrs[i].val = NULL;
    }
    rec->nhdrs = 0;
    free(rec->body);
    rec->body = NULL;
    rec->body_len = 0;
    rec->body_cap = 0;
}

static unsigned char *obs_copy(const uint8_t *p, size_t n) {
    unsigned char *c = malloc(n == 0 ? 1 : n);
    if (c == NULL) return NULL;
    if (n != 0) memcpy(c, p, n);
    return c;
}

static bool obs_pseudo_eq(const uint8_t *name, size_t nlen, const char *lit) {
    size_t m = strlen(lit);
    return nlen == m && memcmp(name, lit, m) == 0;
}

static int obs_begin_headers(nghttp2_session *session,
                             const nghttp2_frame *frame, void *user_data) {
    (void)session;
    struct h2obs *o = user_data;
    if (frame->hd.type != NGHTTP2_HEADERS) return 0;
    struct h2obs_stream *rec = obs_lookup(o, frame->hd.stream_id, true);
    if (rec == NULL) return 0; /* table full: stream is never admitted */
    obs_free_bufs(rec);
    bool in_use = rec->in_use;
    int32_t id = rec->id;
    memset(rec, 0, sizeof *rec);
    rec->in_use = in_use;
    rec->id = id;
    return 0;
}

static int obs_header(nghttp2_session *session, const nghttp2_frame *frame,
                      const uint8_t *name, size_t nlen, const uint8_t *value,
                      size_t vlen, uint8_t flags, void *user_data) {
    (void)session;
    (void)flags;
    struct h2obs *o = user_data;
    if (frame->hd.type != NGHTTP2_HEADERS) return 0;
    struct h2obs_stream *rec = obs_lookup(o, frame->hd.stream_id, false);
    if (rec == NULL || rec->admitted || rec->dropped || rec->over) return 0;
    if (nlen != 0 && name[0] == ':') {
        if (obs_pseudo_eq(name, nlen, ":method")) {
            if (rec->have_method || vlen == 0 || vlen > sizeof rec->method) {
                rec->over = true;
                return 0;
            }
            memcpy(rec->method, value, vlen);
            rec->method_len = vlen;
            rec->have_method = true;
        } else if (obs_pseudo_eq(name, nlen, ":scheme")) {
            if (rec->have_scheme || vlen == 0) {
                rec->over = true;
                return 0;
            }
            rec->have_scheme = true;
        } else if (obs_pseudo_eq(name, nlen, ":path")) {
            if (rec->have_path || vlen == 0 || vlen > sizeof rec->path) {
                rec->over = true;
                return 0;
            }
            memcpy(rec->path, value, vlen);
            rec->path_len = vlen;
            rec->have_path = true;
        } else if (obs_pseudo_eq(name, nlen, ":authority")) {
            if (rec->have_authority || vlen == 0 ||
                vlen > sizeof rec->authority) {
                rec->over = true;
                return 0;
            }
            memcpy(rec->authority, value, vlen);
            rec->auth_len = vlen;
            rec->have_authority = true;
        } else {
            rec->over = true; /* unknown pseudo-header: policy refuses */
        }
        return 0;
    }
    if (nlen == 0 || nlen + vlen > CF_H2OBS_FIELD_MAX ||
        rec->nhdrs >= CF_HTTP_HEADER_COUNT_MAX) {
        rec->over = true;
        return 0;
    }
    unsigned char *nm = obs_copy(name, nlen);
    unsigned char *vl = obs_copy(value, vlen);
    if (nm == NULL || vl == NULL) {
        free(nm);
        free(vl);
        rec->over = true;
        return 0;
    }
    rec->hdrs[rec->nhdrs].name = nm;
    rec->hdrs[rec->nhdrs].nlen = nlen;
    rec->hdrs[rec->nhdrs].val = vl;
    rec->hdrs[rec->nhdrs].vlen = vlen;
    rec->nhdrs++;
    return 0;
}

static int obs_data_chunk(nghttp2_session *session, uint8_t flags,
                          int32_t stream_id, const uint8_t *data, size_t len,
                          void *user_data) {
    (void)session;
    (void)flags;
    struct h2obs *o = user_data;
    struct h2obs_stream *rec = obs_lookup(o, stream_id, false);
    if (rec == NULL || rec->admitted || rec->dropped || rec->over) return 0;
    if (len == 0) return 0;
    if (rec->body_len > CF_H2OBS_BODY_MAX - len ||
        rec->body_len + len > CF_H2OBS_BODY_MAX) {
        rec->over = true;
        free(rec->body);
        rec->body = NULL;
        rec->body_len = 0;
        rec->body_cap = 0;
        return 0;
    }
    if (rec->body_len + len > rec->body_cap) {
        size_t ncap = rec->body_cap == 0 ? 4096 : rec->body_cap * 2;
        while (ncap < rec->body_len + len) ncap *= 2;
        unsigned char *grown = realloc(rec->body, ncap);
        if (grown == NULL) {
            rec->over = true;
            free(rec->body);
            rec->body = NULL;
            rec->body_len = 0;
            rec->body_cap = 0;
            return 0;
        }
        rec->body = grown;
        rec->body_cap = ncap;
    }
    memcpy(rec->body + rec->body_len, data, len);
    rec->body_len += len;
    return 0;
}

static int obs_frame_recv(nghttp2_session *session,
                          const nghttp2_frame *frame, void *user_data) {
    (void)session;
    struct h2obs *o = user_data;
    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        struct h2obs_stream *rec =
            obs_lookup(o, frame->hd.stream_id, false);
        if (rec == NULL) return 0;
        rec->headers_done = true;
        if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
            rec->end_stream = true;
        }
        return 0;
    }
    if (frame->hd.type == NGHTTP2_HEADERS) {
        /* Trailer block ending the stream: bodies were captured above;
         * trailers themselves are dropped (documented gap). */
        if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
            struct h2obs_stream *rec =
                obs_lookup(o, frame->hd.stream_id, false);
            if (rec != NULL) rec->end_stream = true;
        }
        return 0;
    }
    if (frame->hd.type == NGHTTP2_DATA) {
        if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
            struct h2obs_stream *rec =
                obs_lookup(o, frame->hd.stream_id, false);
            if (rec != NULL) rec->end_stream = true;
        }
        return 0;
    }
    if (frame->hd.type == NGHTTP2_RST_STREAM) {
        struct h2obs_stream *rec =
            obs_lookup(o, frame->hd.stream_id, false);
        if (rec != NULL && !rec->admitted) {
            rec->dropped = true;
            free(rec->body);
            rec->body = NULL;
            rec->body_len = 0;
            rec->body_cap = 0;
        }
        return 0;
    }
    return 0;
}

static int obs_stream_close(nghttp2_session *session, int32_t stream_id,
                            uint32_t error_code, void *user_data) {
    (void)session;
    (void)error_code;
    struct h2obs *o = user_data;
    struct h2obs_stream *rec = obs_lookup(o, stream_id, false);
    if (rec != NULL && !rec->admitted) {
        rec->dropped = true;
        free(rec->body);
        rec->body = NULL;
        rec->body_len = 0;
        rec->body_cap = 0;
    }
    return 0;
}

static struct h2obs *h2obs_create(void) {
    struct h2obs *o = calloc(1, sizeof *o);
    if (o == NULL) return NULL;
    nghttp2_session_callbacks *cbs = NULL;
    if (nghttp2_session_callbacks_new(&cbs) != 0) {
        free(o);
        return NULL;
    }
    nghttp2_session_callbacks_set_on_begin_headers_callback(cbs,
                                                            obs_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(cbs, obs_header);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs,
                                                         obs_frame_recv);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs,
                                                              obs_data_chunk);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs,
                                                           obs_stream_close);
    if (nghttp2_session_server_new(&o->sess, cbs, o) != 0) {
        nghttp2_session_callbacks_del(cbs);
        free(o);
        return NULL;
    }
    nghttp2_session_callbacks_del(cbs);
    /* The peer ACKs the server SETTINGS it receives (from the policy
     * session); an ACK with no inflight SETTINGS is a connection error
     * ("unexpected ACK": GOAWAY, then silence). Submit one empty SETTINGS
     * so the mirrored ACK is expected. Its bytes are discarded by the
     * feed drain; only the policy session governs the peer. */
    if (nghttp2_submit_settings(o->sess, NGHTTP2_FLAG_NONE, NULL, 0) !=
        0) {
        nghttp2_session_del(o->sess);
        free(o);
        return NULL;
    }
    return o;
}

static void h2obs_destroy(struct h2obs *o) {
    if (o == NULL) return;
    for (size_t i = 0; i < CF_H2OBS_MAX_STREAMS; i++) {
        if (o->streams[i].in_use) obs_free_bufs(&o->streams[i]);
    }
    nghttp2_session_del(o->sess);
    free(o);
}

/* Feed decrypted bytes to the observer; discard its outbound queue (SETTINGS
 * ack / auto window updates: internal accounting already applied, and the
 * policy session's own updates govern the peer). False on session error. */
static bool h2obs_feed(struct h2obs *o, const unsigned char *data,
                       size_t len) {
    if (nghttp2_session_mem_recv(o->sess, data, len) < 0) return false;
    const uint8_t *d = NULL;
    for (;;) {
        nghttp2_ssize n = nghttp2_session_mem_send(o->sess, &d);
        if (n == 0) break;
        if (n < 0) return false;
    }
    return true;
}

/* ------------------------- H2 request freezing ------------------------ */

static cf_method h2_classify_method(const unsigned char *p, size_t n) {
    if (n == 3 && memcmp(p, "GET", 3) == 0) return CF_GET;
    if (n == 4 && memcmp(p, "HEAD", 4) == 0) return CF_HEAD;
    if (n == 4 && memcmp(p, "POST", 4) == 0) return CF_POST;
    if (n == 3 && memcmp(p, "PUT", 3) == 0) return CF_PUT;
    if (n == 5 && memcmp(p, "PATCH", 5) == 0) return CF_PATCH;
    if (n == 6 && memcmp(p, "DELETE", 6) == 0) return CF_DELETE;
    if (n == 7 && memcmp(p, "OPTIONS", 7) == 0) return CF_OPTIONS;
    return CF_OTHER;
}

/* Freeze one complete observed stream into an owned cf_request, mirroring
 * request.c's freeze_request (single owned storage, budget reservation).
 * Regular headers pass through in order; a synthetic `host` header carries
 * :authority first so Host-dependent dispatch keeps working. */
static cf_err h2_freeze_request(struct cf_http_conn *conn,
                                struct h2obs_stream *rec, cf_request **out,
                                size_t *reserved) {
    *out = NULL;
    *reserved = 0;
    if (!rec->have_method || !rec->have_scheme || !rec->have_path ||
        !rec->have_authority) {
        return CF_INVALID;
    }
    size_t qoff = rec->path_len;
    for (size_t i = 0; i < rec->path_len; i++) {
        if (rec->path[i] == '?') {
            qoff = i;
            break;
        }
    }
    size_t path_len = qoff < rec->path_len ? qoff : rec->path_len;
    size_t query_len = qoff < rec->path_len ? rec->path_len - qoff - 1 : 0;
    const unsigned char *query_ptr =
        qoff < rec->path_len ? rec->path + qoff + 1 : rec->path;
    bool add_host = rec->nhdrs < CF_HTTP_HEADER_COUNT_MAX;
    size_t peer_len = strlen(conn->peer_ip);

    static const unsigned char host_name[] = "host";
    const unsigned char *pieces_ptr[6 + 2 * CF_HTTP_HEADER_COUNT_MAX];
    size_t pieces_len[6 + 2 * CF_HTTP_HEADER_COUNT_MAX];
    size_t np = 0;
    pieces_ptr[np] = rec->method;
    pieces_len[np++] = rec->method_len;
    pieces_ptr[np] = rec->path; /* target: full :path */
    pieces_len[np++] = rec->path_len;
    pieces_ptr[np] = rec->path; /* path: no query */
    pieces_len[np++] = path_len;
    pieces_ptr[np] = query_ptr;
    pieces_len[np++] = query_len;
    pieces_ptr[np] =
        rec->body != NULL ? rec->body : (const unsigned char *)"";
    pieces_len[np++] = rec->body_len;
    pieces_ptr[np] = (const unsigned char *)conn->peer_ip;
    pieces_len[np++] = peer_len;
    if (add_host) {
        pieces_ptr[np] = host_name;
        pieces_len[np++] = sizeof host_name - 1;
        pieces_ptr[np] = rec->authority;
        pieces_len[np++] = rec->auth_len;
    }
    for (size_t i = 0; i < rec->nhdrs; i++) {
        pieces_ptr[np] = rec->hdrs[i].name;
        pieces_len[np++] = rec->hdrs[i].nlen;
        pieces_ptr[np] = rec->hdrs[i].val;
        pieces_len[np++] = rec->hdrs[i].vlen;
    }

    size_t total = 0;
    for (size_t i = 0; i < np; i++) {
        if (pieces_len[i] > SIZE_MAX - total) return CF_LIMIT;
        total += pieces_len[i];
    }
    if (!cf_http_loop_reserve_input(conn->loop, total)) return CF_LIMIT;

    cf_builder sb;
    memset(&sb, 0, sizeof sb);
    cf_err rc = CF_OK;
    size_t offsets[6 + 2 * CF_HTTP_HEADER_COUNT_MAX];
    size_t cursor = 0;
    for (size_t i = 0; i < np; i++) {
        offsets[i] = cursor;
        if (rc == CF_OK) {
            rc = cf_builder_append(&sb, (cf_span){pieces_ptr[i],
                                                  pieces_len[i]});
        }
        cursor += pieces_len[i];
    }
    if (rc != CF_OK) {
        cf_builder_dispose(&sb);
        cf_http_loop_release_input(conn->loop, total);
        return rc == CF_LIMIT ? CF_LIMIT : CF_NOMEM;
    }
    cf_buf *storage = NULL;
    rc = cf_builder_freeze(&sb, &storage);
    if (rc != CF_OK) {
        cf_builder_dispose(&sb);
        cf_http_loop_release_input(conn->loop, total);
        return rc;
    }
    cf_request *req = calloc(1, sizeof *req);
    if (req == NULL) {
        cf_buf_release(storage);
        cf_http_loop_release_input(conn->loop, total);
        return CF_NOMEM;
    }
    req->storage = storage;
    cf_span all = cf_buf_span(storage);
    size_t idx = 0;
    req->raw_method = (cf_span){all.ptr + offsets[idx], pieces_len[idx]};
    idx++;
    req->target = (cf_span){all.ptr + offsets[idx], pieces_len[idx]};
    idx++;
    req->path = (cf_span){all.ptr + offsets[idx], pieces_len[idx]};
    idx++;
    req->query = (cf_span){all.ptr + offsets[idx], pieces_len[idx]};
    idx++;
    req->body = (cf_span){all.ptr + offsets[idx], pieces_len[idx]};
    idx++;
    req->peer_ip = (cf_span){all.ptr + offsets[idx], pieces_len[idx]};
    idx++;
    size_t hi = 0;
    if (add_host) {
        req->headers[hi].name =
            (cf_span){all.ptr + offsets[idx], pieces_len[idx]};
        idx++;
        req->headers[hi].value =
            (cf_span){all.ptr + offsets[idx], pieces_len[idx]};
        idx++;
        hi++;
    }
    for (size_t i = 0; i < rec->nhdrs; i++) {
        req->headers[hi].name =
            (cf_span){all.ptr + offsets[idx], pieces_len[idx]};
        idx++;
        req->headers[hi].value =
            (cf_span){all.ptr + offsets[idx], pieces_len[idx]};
        idx++;
        hi++;
    }
    req->header_count = hi;
    req->method = h2_classify_method(req->raw_method.ptr,
                                     req->raw_method.len);
    req->original_method = req->method;
    req->close_after = false; /* streams are not connections */
    req->tls = true;
    req->params = NULL;
    *out = req;
    *reserved = total;
    return CF_OK;
}

/* ------------------------- H2 task bookkeeping ------------------------ */

static struct cf_h2_link *h2_find_link(struct cf_http_conn *conn,
                                       struct cf_http_task *task) {
    for (struct cf_h2_link *l = conn->h2_tasks; l != NULL; l = l->next) {
        if (l->task == task) return l;
    }
    return NULL;
}

static void h2_unlink(struct cf_http_conn *conn, struct cf_h2_link *link) {
    struct cf_h2_link **p = &conn->h2_tasks;
    while (*p != NULL && *p != link) p = &(*p)->next;
    if (*p != NULL) {
        *p = link->next;
        free(link);
    }
}

/* Read a FILE response body synchronously for H2 (the h2 submit API takes
 * the whole body at once, unlike H1's file worker). Bounded by the queued
 * budget; false means serve 500. Blocking disk I/O on the loop thread is a
 * documented gap for large files. */
static bool h2_read_file(const cf_response *resp, unsigned char **out,
                         size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (resp->file_fd < 0) return false;
    if (resp->file_length > CF_FRONT_H2_QUEUED_MAX) return false;
    size_t len = (size_t)resp->file_length;
    if (len == 0) return true;
    unsigned char *buf = malloc(len);
    if (buf == NULL) return false;
    size_t off = 0;
    while (off < len) {
        ssize_t n =
            pread(resp->file_fd, buf + off, len - off,
                  (off_t)(resp->file_offset + off));
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return false;
        }
        if (n == 0) {
            free(buf);
            return false; /* premature EOF */
        }
        off += (size_t)n;
    }
    *out = buf;
    *out_len = len;
    return true;
}

/* Flush policy-session output (SETTINGS acks, HEADERS, flow-gated DATA)
 * through TLS. mem_send bytes are consumed exactly once: each chunk is
 * fully tls_send drained before the next mem_send call. */
static void conn_h2_flush(struct cf_http_loop *loop,
                          struct cf_http_conn *conn) {
    if (conn->h2 == NULL || conn->tls == NULL) return;
    nghttp2_session *h = cf_front_h2_handle(conn->h2);
    if (h == NULL) return;
    for (;;) {
        const uint8_t *d = NULL;
        nghttp2_ssize n = nghttp2_session_mem_send(h, &d);
        if (n < 0) {
            cf_http_conn_close(loop, conn);
            return;
        }
        if (n == 0) break;
        size_t off = 0;
        while (off < (size_t)n) {
            size_t m = 0;
            cf_front_tls_step st =
                cf_front_tls_send(conn->tls, d + off, (size_t)n - off, &m);
            if (st == CF_FRONT_TLS_DONE) {
                if (m == 0) break; /* defensive: retry on writability */
                off += m;
                continue;
            }
            if (st == CF_FRONT_TLS_WANT_READ) {
                conn->tls_want_write = false;
            } else {
                conn->tls_want_write = true;
            }
            cf_http_conn_update_events(conn);
            if (st == CF_FRONT_TLS_FAIL) cf_http_conn_close(loop, conn);
            return;
        }
        if (off < (size_t)n) {
            conn->tls_want_write = true;
            cf_http_conn_update_events(conn);
            return;
        }
    }
    conn->tls_want_write = false;
    cf_http_conn_update_events(conn);
}

/* Drain H1 output segments through TLS, applying results with the same seam
 * output.c uses (cf_http_output_apply). Runs under the fd-suppression
 * window so nested output.c flushes (error statuses, pipelined requests)
 * queue without plaintext-sending. */
static void conn_tls_h1_flush(struct cf_http_conn *conn) {
    int saved = conn_suppress(conn);
    while (conn->tls != NULL && conn->out_head != NULL) {
        struct cf_http_out_seg *s = conn->out_head;
        size_t unsent = s->len - s->sent;
        if (unsent == 0) break; /* defensive */
        size_t m = 0;
        cf_front_tls_step st =
            cf_front_tls_send(conn->tls, s->ptr + s->sent, unsent, &m);
        if (st == CF_FRONT_TLS_DONE) {
            cf_http_write_result r = cf_http_output_apply(
                conn, (ssize_t)m, 0);
            if (r == CF_HTTP_WRITE_PROGRESS ||
                r == CF_HTTP_WRITE_RETRY) {
                continue;
            }
            if (r == CF_HTTP_WRITE_AGAIN) break;
            cf_http_conn_close(conn->loop, conn);
            break;
        }
        if (st == CF_FRONT_TLS_WANT_READ) {
            conn->tls_want_write = false;
        } else {
            conn->tls_want_write = true;
        }
        if (st == CF_FRONT_TLS_FAIL) cf_http_conn_close(conn->loop, conn);
        break;
    }
    conn_restore(conn->loop, conn, saved);
}

/* Suppressed queue_status + immediate TLS flush for TLS-H1 connections;
 * plaintext passes through untouched. */
static cf_err conn_queue_status_tls(struct cf_http_conn *conn,
                                    unsigned status, bool close_after,
                                    const char *retry_after) {
    if (conn->transport != CF_HTTP_TRANSPORT_TLS_H1) {
        return cf_http_conn_queue_status(conn, status, close_after,
                                         retry_after);
    }
    int saved = conn_suppress(conn);
    cf_err rc = cf_http_conn_queue_status(conn, status, close_after,
                                          retry_after);
    conn_restore(conn->loop, conn, saved);
    if (rc == CF_OK && conn->state != CF_HTTP_STATE_CLOSED) {
        conn_tls_h1_flush(conn);
    }
    return rc;
}

/* Admit one complete observed stream through the SAME admit hook workers
 * use for H1. The link carries the transport-private completion token
 * (conn generation + sequence + stream id, cf_front_h2_key). */
static void h2_admit_stream(struct cf_http_loop *loop,
                            struct cf_http_conn *conn,
                            struct h2obs_stream *rec) {
    cf_request *req = NULL;
    size_t reserved = 0;
    if (h2_freeze_request(conn, rec, &req, &reserved) != CF_OK) {
        loop->counters.budget_rejected++;
        (void)cf_front_h2_rst_stream(conn->h2, rec->id,
                                     NGHTTP2_ENHANCE_YOUR_CALM);
        rec->dropped = true;
        obs_free_bufs(rec);
        conn_h2_flush(loop, conn);
        return;
    }
    if (loop->stopping ||
        atomic_load_explicit(&loop->stop_flag, memory_order_relaxed) ||
        conn->h2 == NULL || cf_front_h2_draining(conn->h2)) {
        loop->counters.admission_rejected++;
        cf_http_request_free_input(loop, req, reserved);
        (void)cf_front_h2_rst_stream(conn->h2, rec->id,
                                     NGHTTP2_REFUSED_STREAM);
        rec->dropped = true;
        obs_free_bufs(rec);
        conn_h2_flush(loop, conn);
        return;
    }
    struct cf_http_task *task = calloc(1, sizeof *task);
    struct cf_h2_link *link = calloc(1, sizeof *link);
    if (task == NULL || link == NULL) {
        free(task);
        free(link);
        loop->counters.errors++;
        cf_http_request_free_input(loop, req, reserved);
        (void)cf_front_h2_rst_stream(conn->h2, rec->id,
                                     NGHTTP2_INTERNAL_ERROR);
        rec->dropped = true;
        obs_free_bufs(rec);
        conn_h2_flush(loop, conn);
        return;
    }
    uint64_t seq = ++conn->sequence;
    task->loop = loop;
    task->conn = conn->id;
    task->sequence = seq;
    task->request = req;
    task->input_reserved = reserved;
    cf_response_init(&task->response);
    atomic_init(&task->submit_state, 0);
    task->is_h2 = true;
    task->h2_stream_id = rec->id;
    link->task = task;
    link->stream_id = rec->id;
    link->sequence = seq;
    link->next = conn->h2_tasks;
    conn->h2_tasks = link;

    cf_err rc = loop->cfg.admit(loop->cfg.admit_user, task);
    if (rc != CF_OK) {
        h2_unlink(conn, link);
        loop->counters.admission_rejected++;
        cf_http_request_free_input(loop, req, reserved);
        free(task);
        (void)cf_front_h2_rst_stream(conn->h2, rec->id,
                                     NGHTTP2_REFUSED_STREAM);
        rec->dropped = true;
        obs_free_bufs(rec);
        conn_h2_flush(loop, conn);
        return;
    }
    loop->counters.requests++;
    loop->counters.admissions++;
    loop->outstanding_tasks++;
    rec->admitted = true;
    obs_free_bufs(rec); /* the frozen request owns its copy now */
}

/* Scan observed streams after each input pump: admit what is complete AND
 * policy-open. Over-budget mirrors are RST (the policy already refused or
 * cancelled its side) so no stream hangs unanswered. */
static void h2_scan_admit(struct cf_http_loop *loop,
                          struct cf_http_conn *conn) {
    struct h2obs *obs = (struct h2obs *)conn->h2obs;
    if (obs == NULL || conn->h2 == NULL) return;
    for (size_t i = 0; i < CF_H2OBS_MAX_STREAMS; i++) {
        struct h2obs_stream *rec = &obs->streams[i];
        if (!rec->in_use || rec->admitted || rec->dropped) continue;
        if (rec->over) {
            (void)cf_front_h2_rst_stream(conn->h2, rec->id, NGHTTP2_CANCEL);
            rec->dropped = true;
            obs_free_bufs(rec);
            continue;
        }
        if (!rec->headers_done || !rec->end_stream) continue;
        if (!cf_front_h2_stream_open(conn->h2, rec->id)) {
            rec->dropped = true; /* policy refused: validation/budget/drain */
            obs_free_bufs(rec);
            continue;
        }
        h2_admit_stream(loop, conn, rec);
        if (conn->h2 == NULL) return;
    }
    conn_h2_flush(loop, conn);
}

/* Route a completed H2 stream task back into h2 DATA (flow-control gated),
 * never to the socket. Stale completions (closed/reused connection, reset
 * stream, wrong sequence) release resources without touching the stream.
 * RST cancels queued-not-committed output only: pre-submit cancellation
 * drops the response; post-submit bytes already framed stay framed. */
static void h2_complete_task(struct cf_http_loop *loop,
                             struct cf_http_task *task) {
    struct cf_http_conn *conn = cf_http_loop_find_conn(loop, task->conn);
    struct cf_h2_link *link =
        (conn != NULL) ? h2_find_link(conn, task) : NULL;
    bool open = conn != NULL &&
                conn->transport == CF_HTTP_TRANSPORT_TLS_H2 &&
                conn->h2 != NULL &&
                cf_front_h2_stream_open(conn->h2, task->h2_stream_id);
    bool valid = false;
    if (conn != NULL && link != NULL &&
        link->stream_id == task->h2_stream_id &&
        link->sequence == task->sequence) {
        cf_front_h2_key key;
        key.conn = task->conn;
        key.sequence = task->sequence;
        key.stream_id = task->h2_stream_id;
        valid = cf_front_h2_key_valid(&key, conn->id, task->sequence, open);
    }
    /* The task is freed by release below; capture the transport token. */
    int32_t sid = task->h2_stream_id;
    if (conn != NULL && link != NULL) h2_unlink(conn, link);
    if (!valid) {
        loop->counters.stale_completions++;
        cf_http_loop_release_task(loop, task);
        return;
    }
    if (task->abandoned) {
        /* Revocation/abandon with an outstanding stream: cancel delivery. */
        cf_http_loop_release_task(loop, task);
        (void)cf_front_h2_rst_stream(conn->h2, sid, NGHTTP2_CANCEL);
        conn_h2_flush(loop, conn);
        return;
    }
    if (!task->has_response) {
        cf_http_loop_release_task(loop, task);
        (void)cf_front_h2_rst_stream(conn->h2, sid,
                                     NGHTTP2_INTERNAL_ERROR);
        conn_h2_flush(loop, conn);
        return;
    }
    cf_http_serialized ser;
    cf_err src = cf_http_response_serialize(&task->response, task->request,
                                            &ser);
    const unsigned char *body = NULL;
    size_t body_len = 0;
    unsigned char *file_buf = NULL;
    size_t file_len = 0;
    bool ok = src == CF_OK;
    if (ok && ser.send_body) {
        if (task->response.body_kind == CF_BODY_BUFFER &&
            task->response.body != NULL) {
            cf_span s = cf_buf_span(task->response.body);
            body = s.ptr;
            body_len = s.len;
        } else if (task->response.body_kind == CF_BODY_FILE) {
            ok = h2_read_file(&task->response, &file_buf, &file_len);
            body = file_buf;
            body_len = file_len;
        } else if (task->response.body_kind != CF_BODY_NONE) {
            ok = false;
        }
    }
    if (ok && src == CF_OK) cf_buf_release(ser.headers);
    unsigned status = task->response.status;
    cf_err s2;
    if (ok) {
        /* Forward the app's headers (Content-Type, Set-Cookie, ETag, ...)
         * into h2; the submitter skips HTTP/1-only classes and emits an
         * authoritative content-length from the framed body. */
        size_t hcount = cf_response_header_count(&task->response);
        const unsigned char **hnames = NULL;
        const unsigned char **hvalues = NULL;
        size_t *hnamelens = NULL;
        size_t *hvaluelens = NULL;
        cf_err hrc = CF_OK;
        if (hcount != 0) {
            hnames = malloc(hcount * sizeof *hnames);
            hvalues = malloc(hcount * sizeof *hvalues);
            hnamelens = malloc(hcount * sizeof *hnamelens);
            hvaluelens = malloc(hcount * sizeof *hvaluelens);
            if (hnames == NULL || hvalues == NULL || hnamelens == NULL ||
                hvaluelens == NULL) {
                hrc = CF_NOMEM;
            } else {
                for (size_t i = 0; i < hcount && hrc == CF_OK; i++) {
                    cf_span hn = {NULL, 0}, hv = {NULL, 0};
                    if (!cf_response_header_at(&task->response, i, &hn,
                                              &hv)) {
                        hrc = CF_INTERNAL;
                        break;
                    }
                    hnames[i] = hn.ptr;
                    hnamelens[i] = hn.len;
                    hvalues[i] = hv.ptr;
                    hvaluelens[i] = hv.len;
                }
            }
        }
        if (hrc == CF_OK) {
            s2 = cf_front_h2_submit_response_headers(
                conn->h2, sid, status, hnames, hnamelens, hvalues,
                hvaluelens, hcount, body, body_len);
        } else {
            s2 = hrc;
        }
        free((void *)hnames);
        free((void *)hvalues);
        free(hnamelens);
        free(hvaluelens);
    } else {
        s2 = cf_front_h2_submit_response(conn->h2, sid, 500, NULL, 0);
    }
    free(file_buf);
    cf_http_loop_release_task(loop, task);
    if (s2 == CF_OK) {
        loop->counters.responses_sent++;
    } else {
        loop->counters.errors++;
    }
    conn_h2_flush(loop, conn);
}

/* Drive one non-blocking server handshake step. DONE selects the transport
 * by ALPN (h2 attaches the policy + observer sessions; http/1.1 or no ALPN
 * keeps the H1 parser path) and immediately pumps already-buffered bytes,
 * since level-triggered epoll will not refire for SSL-buffered input. */
static void conn_tls_handshake_step(struct cf_http_loop *loop,
                                    struct cf_http_conn *conn);

/* Forward declarations for the readable handlers (defined after the
 * accept section with the rest of the I/O drivers). */
static void conn_tls_h1_readable(struct cf_http_loop *loop,
                                 struct cf_http_conn *conn);
static void conn_h2_readable(struct cf_http_loop *loop,
                             struct cf_http_conn *conn);

static void conn_tls_handshake_step(struct cf_http_loop *loop,
                                    struct cf_http_conn *conn) {
    if (conn->transport != CF_HTTP_TRANSPORT_TLS_HANDSHAKE ||
        conn->tls == NULL) {
        return;
    }
    cf_front_tls_step s = cf_front_tls_handshake(conn->tls);
    if (s == CF_FRONT_TLS_DONE) {
        conn->tls_want_write = false;
        const char *alpn = cf_front_tls_alpn(conn->tls);
        if (alpn != NULL && strcmp(alpn, CF_FRONT_TLS_ALPN_H2) == 0) {
            cf_front_h2_session *h2 = NULL;
            if (cf_front_h2_session_create(loop->public_origin, &h2) !=
                    CF_OK ||
                h2 == NULL) {
                cf_http_conn_close(loop, conn);
                return;
            }
            struct h2obs *obs = h2obs_create();
            if (obs == NULL) {
                cf_front_h2_session_destroy(h2);
                cf_http_conn_close(loop, conn);
                return;
            }
            conn->h2 = h2;
            conn->h2obs = obs;
            conn->transport = CF_HTTP_TRANSPORT_TLS_H2;
            conn->state = CF_HTTP_STATE_WORKING;
            conn->deadline_kind = CF_HTTP_DL_NONE;
            conn->deadline_ms = 0;
            cf_http_conn_update_events(conn);
            conn_h2_flush(loop, conn); /* server SETTINGS immediately */
            if (conn->state != CF_HTTP_STATE_CLOSED) {
                conn_h2_readable(loop, conn); /* SSL-buffered preface */
            }
            return;
        }
        conn->transport = CF_HTTP_TRANSPORT_TLS_H1;
        cf_http_request_start(conn, false); /* ACCEPTED -> HEADERS */
        cf_http_conn_update_events(conn);
        conn_tls_h1_readable(loop, conn); /* SSL-buffered request bytes */
        return;
    }
    if (s == CF_FRONT_TLS_WANT_READ) {
        conn->tls_want_write = false;
    } else if (s == CF_FRONT_TLS_WANT_WRITE) {
        conn->tls_want_write = true;
    } else {
        cf_http_conn_close(loop, conn);
        return;
    }
    cf_http_conn_update_events(conn);
}

/* TLS-H1 readable: decrypt until WANT (the SSL buffer must drain fully;
 * level-triggered epoll will not refire for SSL-buffered bytes). Each
 * chunk runs under fd suppression so request.c's error/interim replies
 * queue without plaintext-sending, then flushes through TLS. */
static void conn_tls_h1_readable(struct cf_http_loop *loop,
                                 struct cf_http_conn *conn) {
    if (conn->tls == NULL) {
        cf_http_conn_close(loop, conn);
        return;
    }
    for (;;) {
        size_t n = 0;
        cf_front_tls_step st = cf_front_tls_recv(conn->tls, loop->scratch,
                                                 CF_HTTP_READ_MAX, &n);
        if (st == CF_FRONT_TLS_DONE && n > 0) {
            int saved = conn_suppress(conn);
            cf_http_request_input(conn, loop->scratch, n);
            bool closed = conn->state == CF_HTTP_STATE_CLOSED;
            conn_restore(loop, conn, saved);
            if (closed || conn->fd < 0) return;
            conn_tls_h1_flush(conn);
            if (conn->fd < 0) return;
            continue;
        }
        if (st == CF_FRONT_TLS_DONE) {
            cf_http_request_eof(conn); /* clean peer shutdown */
            return;
        }
        if (st == CF_FRONT_TLS_WANT_READ) {
            conn->tls_want_write = false;
        } else if (st == CF_FRONT_TLS_WANT_WRITE) {
            conn->tls_want_write = true;
            conn_tls_h1_flush(conn);
            if (conn->fd < 0) return;
        } else {
            cf_http_conn_close(loop, conn);
            return;
        }
        cf_http_conn_update_events(conn);
        return;
    }
}

/* TLS-H2 readable: decrypt until WANT, feeding the policy session (framing,
 * validation, budgets, flow control) and the observer (request bytes) in
 * lockstep, admitting complete policy-open streams, flushing via TLS. */
static void conn_h2_readable(struct cf_http_loop *loop,
                             struct cf_http_conn *conn) {
    if (conn->tls == NULL || conn->h2 == NULL) {
        cf_http_conn_close(loop, conn);
        return;
    }
    nghttp2_session *h = cf_front_h2_handle(conn->h2);
    if (h == NULL) {
        cf_http_conn_close(loop, conn);
        return;
    }
    for (;;) {
        size_t n = 0;
        cf_front_tls_step st = cf_front_tls_recv(conn->tls, loop->scratch,
                                                 CF_HTTP_READ_MAX, &n);
        if (st == CF_FRONT_TLS_DONE && n > 0) {
            if (nghttp2_session_mem_recv(h, loop->scratch, n) < 0) {
                cf_http_conn_close(loop, conn);
                return;
            }
            struct h2obs *obs = (struct h2obs *)conn->h2obs;
            if (obs != NULL && !h2obs_feed(obs, loop->scratch, n)) {
                cf_http_conn_close(loop, conn);
                return;
            }
            h2_scan_admit(loop, conn);
            if (conn->h2 == NULL) return; /* closed during admission */
            continue;
        }
        if (st == CF_FRONT_TLS_DONE) {
            conn->input_closed = true;
            if (conn->h2_tasks == NULL &&
                cf_front_h2_open_count(conn->h2) == 0 &&
                !nghttp2_session_want_write(h)) {
                cf_http_conn_close(loop, conn);
            } else {
                cf_http_conn_update_events(conn);
            }
            return;
        }
        if (st == CF_FRONT_TLS_WANT_READ) {
            conn->tls_want_write = false;
        } else if (st == CF_FRONT_TLS_WANT_WRITE) {
            conn->tls_want_write = true;
            conn_h2_flush(loop, conn);
            if (conn->h2 == NULL) return;
        } else {
            cf_http_conn_close(loop, conn);
            return;
        }
        cf_http_conn_update_events(conn);
        return;
    }
}

/* File-chunk completion for TLS-H1: mirrors cf_http_file_chunk_complete
 * exactly, except the resident segment flushes through TLS (output.c's
 * flush would plaintext-send on the TLS socket). Plaintext always uses
 * the original. */
static void tls_file_chunk_complete(struct cf_http_loop *loop,
                                    struct cf_http_chunk_req *req) {
    struct cf_http_conn *conn =
        cf_http_loop_find_conn(loop, req->conn);
    if (conn == NULL ||
        conn->transport != CF_HTTP_TRANSPORT_TLS_H1) {
        cf_http_file_chunk_complete(loop, req);
        return;
    }
    struct cf_http_file_stream *s = req->stream;
    if (!s->abandoned) {
        if (conn->file_stream != s || conn->fd < 0 ||
            conn->sequence != req->sequence) {
            conn = NULL;
        }
    } else {
        conn = NULL;
    }
    if (conn == NULL || s->abandoned) {
        free(req->data);
        req->data = NULL;
        s->chunk_pending = false;
        cf_http_file_stream_release(s);
        free(req);
        return;
    }
    s->chunk_pending = false;
    if (req->err != 0 || req->len == 0) {
        free(req->data);
        req->data = NULL;
        cf_http_conn_close(loop, conn);
        cf_http_file_stream_release(s);
        free(req);
        return;
    }
    s->offset += req->len;
    s->remaining -= req->len;
    size_t len = req->len;
    unsigned char *data = req->data;
    req->data = NULL;
    bool fits = len <= CF_HTTP_OUTPUT_MAX - conn->out_pending &&
                cf_http_loop_reserve_output(loop, len);
    if (!fits) {
        free(data);
        cf_http_conn_close(loop, conn);
        cf_http_file_stream_release(s);
        free(req);
        return;
    }
    struct cf_http_out_seg *seg = calloc(1, sizeof *seg);
    if (seg == NULL) {
        cf_http_loop_release_output(loop, len);
        free(data);
        cf_http_conn_close(loop, conn);
        cf_http_file_stream_release(s);
        free(req);
        return;
    }
    seg->ptr = data;
    seg->len = len;
    seg->hold = NULL;
    seg->owned = data;
    if (conn->out_tail != NULL) {
        conn->out_tail->next = seg;
    } else {
        conn->out_head = seg;
    }
    conn->out_tail = seg;
    if (conn->out_pending == 0) conn->stall_ms = cf_monotonic_ms();
    conn->out_pending += len;
    cf_http_file_stream_release(s); /* the resident segment owns the bytes */
    free(req);
    conn_tls_h1_flush(conn);
}

/* ------------------------------------------------------------- budget */

static size_t loop_input_limit(const struct cf_http_loop *loop) {
    return loop->cfg.input_bytes != 0 ? loop->cfg.input_bytes
                                      : CF_HTTP_DEFAULT_BYTES;
}

static size_t loop_output_limit(const struct cf_http_loop *loop) {
    return loop->cfg.output_bytes != 0 ? loop->cfg.output_bytes
                                       : CF_HTTP_DEFAULT_BYTES;
}

bool cf_http_loop_reserve_input(struct cf_http_loop *loop, size_t bytes) {
    size_t limit = loop_input_limit(loop);
    if (bytes > limit - loop->input_used) return false;
    loop->input_used += bytes;
    return true;
}

void cf_http_loop_release_input(struct cf_http_loop *loop, size_t bytes) {
    if (bytes <= loop->input_used) loop->input_used -= bytes;
    else loop->input_used = 0;
}

bool cf_http_loop_reserve_output(struct cf_http_loop *loop, size_t bytes) {
    size_t limit = loop_output_limit(loop);
    if (bytes > limit - loop->output_used) return false;
    loop->output_used += bytes;
    return true;
}

bool cf_http_loop_output_fits(const struct cf_http_loop *loop, size_t bytes) {
    size_t limit = loop_output_limit(loop);
    return bytes <= limit - loop->output_used;
}

void cf_http_loop_release_output(struct cf_http_loop *loop, size_t bytes) {
    if (bytes <= loop->output_used) loop->output_used -= bytes;
    else loop->output_used = 0;
}

/* --------------------------------------------------------- origin parse */

/* Parse the absolute origin into host (without brackets), port and scheme.
 * cf_config.parse_origin already validated the shape; this is the loop-side
 * check so tests and A00 get the same rule. */
static bool parse_origin(const char *origin, char *host, size_t host_cap,
                         size_t *host_len, unsigned *port, bool *explicit_port,
                         bool *is_https) {
    if (origin == NULL) return false;
    const char *p = origin;
    bool https;
    if (strncasecmp(p, "https://", 8) == 0) {
        https = true;
        p += 8;
    } else if (strncasecmp(p, "http://", 7) == 0) {
        https = false;
        p += 7;
    } else {
        return false;
    }
    const char *host_start = p;
    if (*p == '[') {
        p++;
        host_start = p;
        while (*p != '\0' && *p != ']') p++;
        if (*p != ']') return false;
    } else {
        while (*p != '\0' && *p != ':' && *p != '/') p++;
    }
    size_t hlen = (size_t)(p - host_start);
    if (hlen == 0 || hlen >= host_cap || hlen > 255) return false;
    memcpy(host, host_start, hlen);
    host[hlen] = '\0';

    unsigned prt = https ? 443u : 80u;
    bool have = false;
    if (*p == ']') p++;
    if (*p == ':') {
        p++;
        if (*p < '0' || *p > '9') return false;
        unsigned v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (unsigned)(*p - '0');
            if (v > 65535) return false;
            p++;
            digits++;
        }
        if (digits == 0 || v == 0) return false;
        prt = v;
        have = true;
    }
    if (*p != '\0') return false; /* no path/query/userinfo */
    *host_len = hlen;
    *port = prt;
    *explicit_port = have;
    *is_https = https;
    return true;
}

/* ------------------------------------------------------------- wakeups */

static void loop_wake(struct cf_http_loop *loop) {
    uint64_t one = 1;
    ssize_t n = write(loop->wake_fd, &one, sizeof one);
    (void)n; /* EAGAIN means a wake is already pending */
}

static void loop_drain_wake(struct cf_http_loop *loop) {
    uint64_t v;
    while (read(loop->wake_fd, &v, sizeof v) > 0) {
    }
}

/* ------------------------------------------------------------- slots */

struct cf_http_conn *cf_http_loop_find_conn(struct cf_http_loop *loop,
                                            cf_conn_id id) {
    if (id.slot >= loop->conn_cap) return NULL;
    struct cf_http_conn *conn = &loop->conns[id.slot];
    if (conn->id.generation != id.generation) return NULL;
    return conn;
}

void cf_http_conn_update_events(struct cf_http_conn *conn) {
    if (conn->fd < 0) return;
    uint32_t ev = EPOLLRDHUP;
    if (conn->transport == CF_HTTP_TRANSPORT_TLS_HANDSHAKE) {
        ev |= EPOLLIN;
        if (conn->tls_want_write) ev |= EPOLLOUT;
    } else if (conn->transport == CF_HTTP_TRANSPORT_TLS_H2) {
        ev |= EPOLLIN;
        bool want = conn->tls_want_write;
        if (!want && conn->h2 != NULL) {
            nghttp2_session *h = cf_front_h2_handle(conn->h2);
            /* A flow-stalled session must wait for WINDOW_UPDATE on EPOLLIN,
             * never busy-spin on EPOLLOUT. */
            if (h != NULL && nghttp2_session_want_write(h) &&
                !cf_front_h2_flow_stalled(conn->h2)) {
                want = true;
            }
        }
        if (want) ev |= EPOLLOUT;
    } else {
        switch (conn->state) {
        case CF_HTTP_STATE_ACCEPTED:
        case CF_HTTP_STATE_HEADERS:
        case CF_HTTP_STATE_BODY:
            ev |= EPOLLIN;
            break;
        default:
            break;
        }
        if (conn->out_pending > 0) ev |= EPOLLOUT;
        if (conn->transport == CF_HTTP_TRANSPORT_TLS_H1 &&
            conn->tls_want_write) {
            ev |= EPOLLOUT;
        }
    }

    struct epoll_event e = {.events = ev, .data.ptr = conn};
    int op = conn->registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (!conn->registered) {
        if (epoll_ctl(conn->loop->epfd, op, conn->fd, &e) == 0) {
            conn->registered = true;
            conn->events = ev;
        }
        return;
    }
    if (ev == conn->events) return;
    if (epoll_ctl(conn->loop->epfd, EPOLL_CTL_MOD, conn->fd, &e) == 0) {
        conn->events = ev;
    }
}

void cf_http_conn_check_deadline(struct cf_http_conn *conn, uint64_t now_ms) {
    if (conn->fd < 0) return;
    bool expired = false;
    if (conn->deadline_kind != CF_HTTP_DL_NONE && conn->deadline_ms != 0 &&
        now_ms >= conn->deadline_ms) {
        expired = true;
    }
    if (!expired && conn->out_pending > 0 && conn->stall_ms != 0 &&
        now_ms - conn->stall_ms >= CF_HTTP_DEADLINE_MS) {
        expired = true; /* stalled-write: no positive progress */
    }
    if (expired) {
        conn->loop->counters.timeouts++;
        cf_http_conn_close(conn->loop, conn);
    }
}

void cf_http_conn_close(struct cf_http_loop *loop, struct cf_http_conn *conn) {
    if (conn == NULL || conn->state == CF_HTTP_STATE_CLOSED) return;
    /* The real fd may sit in swap_fd while output.c's plaintext flush is
     * suppressed (conn->fd == -1): close consumes whichever holds it. */
    int fd = conn->fd;
    if (fd < 0) fd = conn->swap_fd;
    conn->fd = -1;
    conn->swap_fd = -1;
    if (fd >= 0) {
        if (conn->registered) {
            (void)epoll_ctl(loop->epfd, EPOLL_CTL_DEL, fd, NULL);
            conn->registered = false;
        }
        close(fd);
        if (loop->active_conns != 0) loop->active_conns--;
    }
    /* P01 teardown: TLS/H2 objects die with the connection (the fd above is
     * already closed; the TLS object never owns it). Outstanding H2 stream
     * tasks stay worker-owned: their completions arrive stale and release. */
    if (conn->tls != NULL) {
        cf_front_tls_conn_destroy(conn->tls);
        conn->tls = NULL;
    }
    if (conn->h2 != NULL) {
        cf_front_h2_session_destroy(conn->h2);
        conn->h2 = NULL;
    }
    if (conn->h2obs != NULL) {
        h2obs_destroy((struct h2obs *)conn->h2obs);
        conn->h2obs = NULL;
    }
    while (conn->h2_tasks != NULL) {
        struct cf_h2_link *dead = conn->h2_tasks;
        conn->h2_tasks = dead->next;
        free(dead);
    }
    conn->transport = CF_HTTP_TRANSPORT_PLAIN;
    conn->tls_want_write = false;
    cf_http_conn_stream_abandon(conn);
    cf_http_output_reset(conn);
    if (conn->pending_task != NULL) {
        if (conn->task_attached_to_output) {
            /* The response bytes are dropped with the socket; the task has no
             * later completion. */
            cf_http_loop_release_task(loop, conn->pending_task);
        }
        /* else: WORKING; the outstanding completion arrives stale. */
        conn->pending_task = NULL;
    }
    conn->task_attached_to_output = false;
    cf_http_request_abort(conn);
    conn->state = CF_HTTP_STATE_CLOSED;
    conn->pending_kind = CF_HTTP_PENDING_NONE;
    conn->deadline_kind = CF_HTTP_DL_NONE;
    conn->deadline_ms = 0;
    conn->events = 0;
    conn->chunk_done = false;
    conn->trailers_done = false;
    if (loop->free_count < loop->conn_cap) {
        loop->free_slots[loop->free_count++] = conn->id.slot;
    }
}

/* --------------------------------------------------------- task release */

void cf_http_loop_release_task(struct cf_http_loop *loop,
                               struct cf_http_task *task) {
    if (task == NULL || task->released) return;
    task->released = true;
    if (task->request != NULL) {
        cf_http_request_free_input(loop, task->request, task->input_reserved);
        task->request = NULL;
    }
    if (task->has_response) {
        cf_response_dispose(&task->response);
        task->has_response = false;
    }
    if (loop->outstanding_tasks != 0) loop->outstanding_tasks--;
    free(task);
}

/* ------------------------------------------------------------- admission */

cf_err cf_http_loop_admit(struct cf_http_loop *loop, struct cf_http_conn *conn,
                          cf_request *req, size_t input_reserved,
                          uint64_t sequence) {
    loop->counters.requests++;
    /* P01: request.c freezes H1 requests with tls=false (untouched); the
     * loop stamps TLS here, before the worker ever sees the task. H2
     * requests already freeze with tls=true. */
    if (conn->transport != CF_HTTP_TRANSPORT_PLAIN) req->tls = true;
    if (loop->stopping || atomic_load_explicit(&loop->stop_flag,
                                               memory_order_relaxed)) {
        loop->counters.admission_rejected++;
        cf_http_request_free_input(loop, req, input_reserved);
        (void)conn_queue_status_tls(conn, 503, true,
                                    "Retry-After: 1\r\n");
        return CF_BUSY;
    }

    struct cf_http_task *task = calloc(1, sizeof *task);
    if (task == NULL) {
        cf_http_request_free_input(loop, req, input_reserved);
        loop->counters.errors++;
        (void)conn_queue_status_tls(conn, 500, true, NULL);
        return CF_NOMEM;
    }
    task->loop = loop;
    task->conn = conn->id;
    task->sequence = sequence;
    task->request = req;
    task->input_reserved = input_reserved;
    cf_response_init(&task->response);
    atomic_init(&task->submit_state, 0);

    conn->pending_task = task;
    conn->deadline_kind = CF_HTTP_DL_NONE;
    conn->deadline_ms = 0;

    cf_err rc = loop->cfg.admit(loop->cfg.admit_user, task);
    if (rc != CF_OK) {
        loop->counters.admission_rejected++;
        conn->pending_task = NULL;
        cf_http_request_free_input(loop, req, input_reserved);
        free(task);
        (void)conn_queue_status_tls(
            conn, 503, conn->close_after || conn->input_closed,
            "Retry-After: 1\r\n");
        return CF_BUSY;
    }
    loop->counters.admissions++;
    loop->outstanding_tasks++;
    conn->state = CF_HTTP_STATE_WORKING;
    cf_http_conn_update_events(conn);
    return CF_OK;
}

cf_err cf_http_task_submit(cf_http_task *task, cf_response *response) {
    if (task == NULL || response == NULL) return CF_INVALID;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&task->submit_state, &expected, 1)) {
        return CF_BUSY; /* already submitted or abandoned */
    }
    task->response = *response; /* ownership moves to the task */
    task->has_response = true;
    cf_response_init(response); /* caller's struct is left empty */

    struct cf_http_loop *loop = task->loop;
    pthread_mutex_lock(&loop->completion_mutex);
    task->next = NULL;
    if (loop->task_tail != NULL) {
        loop->task_tail->next = task;
    } else {
        loop->task_head = task;
    }
    loop->task_tail = task;
    pthread_mutex_unlock(&loop->completion_mutex);
    loop_wake(loop);
    return CF_OK;
}

void cf_http_task_abandon(cf_http_task *task) {
    if (task == NULL) return;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&task->submit_state, &expected, 1)) {
        return; /* already submitted or abandoned */
    }
    task->abandoned = true;
    struct cf_http_loop *loop = task->loop;
    pthread_mutex_lock(&loop->completion_mutex);
    task->next = NULL;
    if (loop->task_tail != NULL) {
        loop->task_tail->next = task;
    } else {
        loop->task_head = task;
    }
    loop->task_tail = task;
    pthread_mutex_unlock(&loop->completion_mutex);
    loop_wake(loop);
}

const cf_request *cf_http_task_request(const cf_http_task *task) {
    return task != NULL ? task->request : NULL;
}

cf_conn_id cf_http_task_connection(const cf_http_task *task) {
    cf_conn_id none = {0, 0, 0};
    return task != NULL ? task->conn : none;
}

uint64_t cf_http_task_sequence(const cf_http_task *task) {
    return task != NULL ? task->sequence : 0;
}

/* ---------------------------------------------------------- completions */

static void handle_task_completion(struct cf_http_loop *loop,
                                   struct cf_http_task *task) {
    if (task->is_h2) {
        h2_complete_task(loop, task);
        return;
    }
    struct cf_http_conn *conn = cf_http_loop_find_conn(loop, task->conn);
    bool usable = conn != NULL && conn->fd >= 0 &&
                  conn->state == CF_HTTP_STATE_WORKING &&
                  conn->sequence == task->sequence &&
                  conn->pending_task == task;
    if (!usable) {
        loop->counters.stale_completions++;
        cf_http_loop_release_task(loop, task); /* CORE-03: resources released */
        return;
    }
    conn->pending_task = task;
    if (task->abandoned) {
        conn->pending_task = NULL;
        cf_http_loop_release_task(loop, task);
        cf_http_conn_close(loop, conn);
        return;
    }
    if (!task->has_response) {
        /* Defensive: submitted without a copied response cannot happen. */
        conn->pending_task = NULL;
        cf_http_loop_release_task(loop, task);
        cf_http_conn_close(loop, conn);
        return;
    }
    cf_err rc;
    bool tls_h1 = conn->transport == CF_HTTP_TRANSPORT_TLS_H1;
    int saved = tls_h1 ? conn_suppress(conn) : -1;
    rc = cf_http_conn_attach_response(conn, task);
    if (tls_h1) conn_restore(loop, conn, saved);
    if (rc != CF_OK) {
        conn->pending_task = NULL;
        conn->task_attached_to_output = false;
        cf_http_loop_release_task(loop, task);
        if (conn->out_head == NULL) {
            /* Budget pressure before any response bytes: 503 is formable. */
            loop->counters.budget_rejected++;
            (void)conn_queue_status_tls(conn, 503, true,
                                        "Retry-After: 1\r\n");
        } else {
            loop->counters.errors++;
            cf_http_conn_close(loop, conn);
        }
        if (tls_h1 && conn->state != CF_HTTP_STATE_CLOSED) {
            conn_tls_h1_flush(conn);
        }
        return;
    }
    if (tls_h1 && conn->state != CF_HTTP_STATE_CLOSED) {
        conn_tls_h1_flush(conn);
    }
}

static void loop_drain_completions(struct cf_http_loop *loop) {
    /* Consume the eventfd first, then detach the queue: an enqueue that wins
     * the race after the read leaves its wake pending, so no completion is
     * lost (01 H00 contract: enqueue/drain race). */
    loop_drain_wake(loop);

    pthread_mutex_lock(&loop->completion_mutex);
    struct cf_http_task *tasks = loop->task_head;
    struct cf_http_chunk_req *chunks = loop->chunk_head;
    loop->task_head = loop->task_tail = NULL;
    loop->chunk_head = loop->chunk_tail = NULL;
    pthread_mutex_unlock(&loop->completion_mutex);

    while (tasks != NULL) {
        struct cf_http_task *next = tasks->next;
        handle_task_completion(loop, tasks);
        tasks = next;
    }
    while (chunks != NULL) {
        struct cf_http_chunk_req *next = chunks->next;
        /* TLS-H1 chunks flush through TLS; everything else is untouched. */
        bool is_tls_h1 = false;
        struct cf_http_conn *peek =
            cf_http_loop_find_conn(loop, chunks->conn);
        if (peek != NULL &&
            peek->transport == CF_HTTP_TRANSPORT_TLS_H1 &&
            peek->file_stream == chunks->stream) {
            is_tls_h1 = true;
        }
        if (is_tls_h1) {
            tls_file_chunk_complete(loop, chunks);
        } else {
            cf_http_file_chunk_complete(loop, chunks);
        }
        chunks = next;
    }
}

/* --------------------------------------------------------- file worker */

static void *file_worker_main(void *arg) {
    struct cf_http_loop *loop = arg;
    struct cf_http_file_worker *w = &loop->file_worker;
    for (;;) {
        pthread_mutex_lock(&w->mutex);
        while (w->head == NULL && !w->stop) {
            pthread_cond_wait(&w->cond, &w->mutex);
        }
        if (w->head == NULL && w->stop) {
            pthread_mutex_unlock(&w->mutex);
            break;
        }
        struct cf_http_chunk_req *req = w->head;
        w->head = req->next;
        if (w->head == NULL) w->tail = NULL;
        req->next = NULL;
        bool stopping = w->stop;
        pthread_mutex_unlock(&w->mutex);

        if (stopping) {
            /* Teardown: no loop reader remains; drop the read reference. */
            req->stream->chunk_pending = false;
            cf_http_file_stream_release(req->stream);
            free(req);
            continue;
        }

        size_t want = req->want;
        unsigned char *data = malloc(want == 0 ? 1 : want);
        if (data == NULL) {
            req->err = ENOMEM;
            req->len = 0;
        } else {
            ssize_t n;
            do {
                n = pread(req->stream->fd, data, want, (off_t)req->offset);
            } while (n < 0 && errno == EINTR);
            if (n < 0) {
                req->err = errno;
                req->len = 0;
            } else {
                req->err = 0;
                req->len = (size_t)n; /* 0 means premature EOF */
            }
            req->data = data;
        }

        pthread_mutex_lock(&loop->completion_mutex);
        req->next = NULL;
        if (loop->chunk_tail != NULL) {
            loop->chunk_tail->next = req;
        } else {
            loop->chunk_head = req;
        }
        loop->chunk_tail = req;
        pthread_mutex_unlock(&loop->completion_mutex);
        loop_wake(loop);
    }
    return NULL;
}

void cf_http_file_worker_submit(struct cf_http_loop *loop,
                                struct cf_http_chunk_req *req) {
    struct cf_http_file_worker *w = &loop->file_worker;
    pthread_mutex_lock(&w->mutex);
    req->next = NULL;
    if (w->tail != NULL) {
        w->tail->next = req;
    } else {
        w->head = req;
    }
    w->tail = req;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mutex);
}

/* --------------------------------------------------------------- accept */

static void conn_init(struct cf_http_loop *loop, struct cf_http_conn *conn,
                      int fd) {
    uint64_t gen = conn->id.generation + 1;
    if (gen == 0) {
        /* Exhausted: retire the slot for the process lifetime. */
        close(fd);
        return;
    }
    conn->loop = loop;
    conn->id.loop = loop->cfg.loop_index;
    conn->id.slot = (uint32_t)(conn - loop->conns);
    conn->id.generation = gen;
    conn->fd = fd;
    conn->state = CF_HTTP_STATE_ACCEPTED;
    conn->events = 0;
    conn->registered = false;
    conn->input_closed = false;
    conn->close_after = false;
    conn->must_close_input = false;
    conn->sequence = 0;
    conn->deadline_kind = CF_HTTP_DL_NONE;
    conn->deadline_ms = 0;
    conn->pending_task = NULL;
    conn->task_attached_to_output = false;
    conn->pending_kind = CF_HTTP_PENDING_NONE;
    conn->out_head = conn->out_tail = NULL;
    conn->out_pending = 0;
    conn->stall_ms = 0;
    conn->file_stream = NULL;
    conn->input_reserved = 0;
    conn->output_reserved = 0;
    conn->hdr_count = 0;
    conn->chunk_done = false;
    conn->trailers_done = false;
    conn->request_offered = false;
    /* P01 slot state (close tears the objects down; init is defensive). */
    conn->transport = CF_HTTP_TRANSPORT_PLAIN;
    conn->tls = NULL;
    conn->h2 = NULL;
    conn->h2obs = NULL;
    conn->tls_want_write = false;
    conn->swap_fd = -1;
    conn->h2_tasks = NULL;
    /* in/head/body/trailers were disposed on close; clear lengths anyway. */
    conn->in.len = conn->head.len = conn->body.len = conn->trailers.len = 0;

    conn->peer_ip[0] = '\0';
    struct sockaddr_storage ss;
    socklen_t slen = sizeof ss;
    if (getpeername(fd, (struct sockaddr *)&ss, &slen) == 0) {
        if (ss.ss_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
            (void)inet_ntop(AF_INET, &sin->sin_addr, conn->peer_ip,
                            sizeof conn->peer_ip);
        } else if (ss.ss_family == AF_INET6) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;
            (void)inet_ntop(AF_INET6, &sin6->sin6_addr, conn->peer_ip,
                            sizeof conn->peer_ip);
        }
    }
    if (conn->peer_ip[0] == '\0') {
        memcpy(conn->peer_ip, "local", 6);
    }

    loop->active_conns++;
    if (loop->tls_server != NULL) {
        /* P01: TLS is mandatory on this loop. Wrap before any byte moves;
         * the handshake runs off readiness below, never blocking. */
        cf_front_tls_conn *wrapped = NULL;
        if (cf_front_tls_conn_wrap(loop->tls_server, fd, &wrapped) !=
                CF_OK ||
            wrapped == NULL) {
            close(fd);
            conn->fd = -1;
            if (loop->active_conns != 0) loop->active_conns--;
            conn->state = CF_HTTP_STATE_CLOSED;
            if (loop->free_count < loop->conn_cap) {
                loop->free_slots[loop->free_count++] = conn->id.slot;
            }
            return;
        }
        conn->tls = wrapped;
        conn->transport = CF_HTTP_TRANSPORT_TLS_HANDSHAKE;
        conn->state = CF_HTTP_STATE_ACCEPTED;
        conn->deadline_kind = CF_HTTP_DL_HEADER;
        conn->deadline_ms = cf_monotonic_ms() + CF_HTTP_DEADLINE_MS;
        cf_http_conn_update_events(conn);
        conn_tls_handshake_step(loop, conn);
        return;
    }
    cf_http_request_start(conn, false); /* ACCEPTED -> HEADERS */
    cf_http_conn_update_events(conn);
}

static void loop_accept(struct cf_http_loop *loop) {
    for (;;) {
        int fd = accept4(loop->cfg.listen_fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break; /* EAGAIN or a transient accept failure */
        }
        /* The reference front server disables Nagle on every accepted
         * connection (tmp/rust-ref/crates/kit/src/front/conn.rs accept_loop:
         * `let _ = stream.set_nodelay(true)`), matching Go's net/http. Without
         * it a response split across two sends (serialized head, then body)
         * leaves the small second send held by Nagle until the client's
         * delayed ACK, stalling keep-alive responses for ~40 ms
         * (docs/devel/evidence/keepalive-stall.md). Best-effort, exactly as
         * the reference ignores the result. */
        int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (loop->active_conns >= loop->conn_cap ||
            loop->free_count == 0) {
            loop->counters.rejected++;
            close(fd);
            continue;
        }
        size_t slot = (size_t)loop->free_slots[--loop->free_count];
        struct cf_http_conn *conn = &loop->conns[slot];
        loop->counters.accepted++;
        conn_init(loop, conn, fd);
    }
}

/* ------------------------------------------------------------ requests */

static void conn_on_readable(struct cf_http_loop *loop,
                             struct cf_http_conn *conn) {
    if (conn->transport == CF_HTTP_TRANSPORT_TLS_HANDSHAKE) {
        conn_tls_handshake_step(loop, conn);
        return;
    }
    if (conn->transport == CF_HTTP_TRANSPORT_TLS_H1) {
        conn_tls_h1_readable(loop, conn);
        return;
    }
    if (conn->transport == CF_HTTP_TRANSPORT_TLS_H2) {
        conn_h2_readable(loop, conn);
        return;
    }
    unsigned char *buf = loop->scratch;
    for (;;) {
        ssize_t n = recv(conn->fd, buf, CF_HTTP_READ_MAX, 0);
        if (n > 0) {
            cf_http_request_input(conn, buf, (size_t)n);
            if (conn->fd < 0) return;
            /* One recv per event keeps a single read bounded; level-triggered
             * epoll reports the rest. */
            return;
        }
        if (n == 0) {
            cf_http_request_eof(conn);
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        cf_http_conn_close(loop, conn);
        return;
    }
}

/* Peer half-close while a response is in flight: peek without consuming so
 * the response still goes out, but keep-alive is disallowed (01 H01
 * half-close handling). */
static void conn_note_half_close(struct cf_http_loop *loop,
                                 struct cf_http_conn *conn) {
    if (conn->input_closed || conn->fd < 0) return;
    unsigned char byte;
    ssize_t n = recv(conn->fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) {
        conn->input_closed = true;
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
               errno != EINTR) {
        cf_http_conn_close(loop, conn);
    }
    (void)loop;
}

/* ----------------------------------------------------------- shutdown */

static void loop_begin_stop(struct cf_http_loop *loop) {
    loop->stopping = true;
    (void)epoll_ctl(loop->epfd, EPOLL_CTL_DEL, loop->cfg.listen_fd, NULL);
    /* Reject new admissions; connections still reading a request cannot be
     * answered reliably, so they close. Working/writing connections drain. */
    for (size_t i = 0; i < loop->conn_cap; i++) {
        struct cf_http_conn *conn = &loop->conns[i];
        if (conn->state == CF_HTTP_STATE_ACCEPTED ||
            conn->state == CF_HTTP_STATE_HEADERS ||
            conn->state == CF_HTTP_STATE_BODY) {
            cf_http_conn_close(loop, conn);
        } else if (conn->transport == CF_HTTP_TRANSPORT_TLS_H2 &&
                   conn->h2 != NULL) {
            /* P01: accepted H2 streams drain; new streams are refused. */
            (void)cf_front_h2_goaway(conn->h2);
            conn_h2_flush(loop, conn);
        }
    }
}

static bool loop_drain_done(const struct cf_http_loop *loop) {
    if (loop->outstanding_tasks != 0) return false;
    for (size_t i = 0; i < loop->conn_cap; i++) {
        const struct cf_http_conn *conn = &loop->conns[i];
        if (conn->transport == CF_HTTP_TRANSPORT_TLS_H2 &&
            conn->h2 != NULL) {
            /* P01: H2 task bytes are released at submit, but framed DATA
             * may still wait for flow-control allowance; admitted but
             * uncompleted streams are covered by outstanding_tasks above. */
            if (conn->h2_tasks != NULL) return false;
            if (nghttp2_session_want_write(
                    cf_front_h2_handle(conn->h2))) {
                return false;
            }
            continue; /* an idle H2 connection never blocks the drain */
        }
        if (conn->state == CF_HTTP_STATE_WORKING ||
            conn->out_pending > 0 || conn->file_stream != NULL) {
            return false;
        }
    }
    return true;
}

static int loop_next_timeout(const struct cf_http_loop *loop, uint64_t now) {
    uint64_t best = UINT64_MAX;
    if (loop->stopping && loop->drain_deadline_ms != 0) {
        best = loop->drain_deadline_ms; /* drain deadline */
    }
    for (size_t i = 0; i < loop->conn_cap; i++) {
        const struct cf_http_conn *conn = &loop->conns[i];
        if (conn->fd < 0) continue;
        if (conn->deadline_kind != CF_HTTP_DL_NONE && conn->deadline_ms != 0 &&
            conn->deadline_ms < best) {
            best = conn->deadline_ms;
        }
        if (conn->out_pending > 0 && conn->stall_ms != 0) {
            uint64_t stall = conn->stall_ms + CF_HTTP_DEADLINE_MS;
            if (stall < best) best = stall;
        }
    }
    if (best == UINT64_MAX) {
        /* With an app attached, wake periodically to observe its stop
         * request (there is no eventfd from cf_app). */
        return loop->cfg.app != NULL ? 250 : -1;
    }
    if (best <= now) return 0;
    uint64_t diff = best - now;
    if (diff > 250) diff = 250; /* re-check injected/advanced clocks */
    return (int)diff;
}

/* --------------------------------------------------------------- create */

cf_err cf_http_loop_create(const cf_http_loop_config *config,
                           cf_http_loop **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (config == NULL || config->listen_fd < 0 || config->admit == NULL ||
        config->public_origin == NULL) {
        return CF_INVALID;
    }

    struct cf_http_loop *loop = calloc(1, sizeof *loop);
    if (loop == NULL) return CF_NOMEM;
    loop->epfd = -1;
    loop->wake_fd = -1;
    loop->cfg = *config;
    loop->cfg.connections_per_loop = config->connections_per_loop != 0
                                         ? config->connections_per_loop
                                         : CF_HTTP_DEFAULT_CONNECTIONS;
    loop->public_origin = strdup(config->public_origin);
    if (loop->public_origin == NULL) {
        free(loop);
        return CF_NOMEM;
    }
    if (!parse_origin(loop->public_origin, loop->origin_host,
                      sizeof loop->origin_host, &loop->origin_host_len,
                      &loop->origin_port, &loop->origin_explicit_port,
                      &loop->origin_is_https)) {
        free(loop->public_origin);
        free(loop);
        return CF_INVALID;
    }

    loop->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epfd < 0) goto fail;
    loop->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (loop->wake_fd < 0) goto fail;

    if (loop->cfg.connections_per_loop > UINT32_MAX) goto fail;
    loop->conn_cap = loop->cfg.connections_per_loop;
    loop->conns = calloc(loop->conn_cap, sizeof *loop->conns);
    loop->free_slots = malloc(loop->conn_cap * sizeof *loop->free_slots);
    loop->scratch = malloc(CF_HTTP_READ_MAX);
    if (loop->conns == NULL || loop->free_slots == NULL ||
        loop->scratch == NULL) {
        goto fail;
    }
    for (size_t i = 0; i < loop->conn_cap; i++) {
        loop->free_slots[i] = (int32_t)i;
        loop->conns[i].fd = -1;
        loop->conns[i].state = CF_HTTP_STATE_CLOSED;
        loop->conns[i].loop = loop;
        loop->conns[i].id.slot = (uint32_t)i;
    }
    loop->free_count = loop->conn_cap;

    if (pthread_mutex_init(&loop->completion_mutex, NULL) != 0) goto fail;
    if (pthread_mutex_init(&loop->file_worker.mutex, NULL) != 0) goto fail;
    if (pthread_cond_init(&loop->file_worker.cond, NULL) != 0) goto fail;

    int flags = fcntl(loop->cfg.listen_fd, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(loop->cfg.listen_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        goto fail;
    }
    struct epoll_event le = {.events = EPOLLIN};
    le.data.u64 = 2; /* listener tag; connection pointers are never 1 or 2 */
    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, loop->cfg.listen_fd, &le) != 0) {
        goto fail;
    }
    struct epoll_event we = {.events = EPOLLIN};
    we.data.u64 = 1; /* wake tag */
    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, loop->wake_fd, &we) != 0) {
        goto fail;
    }

    if (pthread_create(&loop->file_worker.thread, NULL, file_worker_main,
                       loop) != 0) {
        goto fail;
    }
    loop->file_worker.started = true;
    atomic_init(&loop->stop_flag, false);

    *out = loop;
    return CF_OK;

fail: {
    cf_err rc = CF_NOMEM;
    if (loop->scratch != NULL) rc = CF_INTERNAL;
    if (loop->file_worker.started) {
        pthread_mutex_lock(&loop->file_worker.mutex);
        loop->file_worker.stop = true;
        pthread_cond_broadcast(&loop->file_worker.cond);
        pthread_mutex_unlock(&loop->file_worker.mutex);
        pthread_join(loop->file_worker.thread, NULL);
    }
    if (loop->epfd >= 0) close(loop->epfd);
    if (loop->wake_fd >= 0) close(loop->wake_fd);
    free(loop->scratch);
    free(loop->conns);
    free(loop->free_slots);
    free(loop->public_origin);
    free(loop);
    return rc;
}
}

void cf_http_loop_stop(struct cf_http_loop *loop) {
    if (loop == NULL) return;
    atomic_store_explicit(&loop->stop_flag, true, memory_order_relaxed);
    loop_wake(loop);
}

cf_err cf_http_loop_run(struct cf_http_loop *loop) {
    if (loop == NULL || loop->run_called) return CF_INVALID;
    loop->run_called = true;

    struct epoll_event events[CF_HTTP_MAX_EVENTS];
    while (true) {
        if (loop->cfg.app != NULL &&
            cf_app_stop_requested(loop->cfg.app)) {
            atomic_store_explicit(&loop->stop_flag, true,
                                  memory_order_relaxed);
        }
        if (atomic_load_explicit(&loop->stop_flag, memory_order_relaxed) &&
            !loop->stopping) {
            loop_begin_stop(loop);
            loop->drain_deadline_ms = cf_monotonic_ms() + CF_HTTP_DRAIN_MS;
        }
        if (loop->stopping && loop_drain_done(loop)) {
            break;
        }
        if (loop->stopping) {
            uint64_t now = cf_monotonic_ms();
            if (now >= loop->drain_deadline_ms) break;
            /* Re-check any connection whose deadline expired while waiting
             * for completions. */
        }

        uint64_t now = cf_monotonic_ms();
        for (size_t i = 0; i < loop->conn_cap; i++) {
            cf_http_conn_check_deadline(&loop->conns[i], now);
        }

        int timeout = loop_next_timeout(loop, now);
        int n = epoll_wait(loop->epfd, events, CF_HTTP_MAX_EVENTS, timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            struct epoll_event *e = &events[i];
            if (e->data.u64 == 1) { /* wake fd */
                loop_drain_completions(loop);
                continue;
            }
            if (e->data.u64 == 2) { /* listener */
                if (!loop->stopping) loop_accept(loop);
                continue;
            }
            if (e->data.ptr != NULL) {
                /* Connections register their own pointer. */
                struct cf_http_conn *conn = e->data.ptr;
                if (conn->fd < 0) continue;
                if (e->events & (EPOLLERR | EPOLLHUP)) {
                    cf_http_conn_close(loop, conn);
                    continue;
                }
                if (conn->transport == CF_HTTP_TRANSPORT_TLS_HANDSHAKE) {
                    if (e->events & (EPOLLIN | EPOLLOUT)) {
                        conn_tls_handshake_step(loop, conn);
                    }
                    continue;
                }
                if (conn->transport == CF_HTTP_TRANSPORT_TLS_H1 ||
                    conn->transport == CF_HTTP_TRANSPORT_TLS_H2) {
                    if (e->events & EPOLLOUT) {
                        if (conn->transport ==
                            CF_HTTP_TRANSPORT_TLS_H1) {
                            conn_tls_h1_flush(conn);
                        } else {
                            conn_h2_flush(loop, conn);
                        }
                        if (conn->fd < 0) continue;
                    }
                    /* No MSG_PEEK half-close probe on TLS sockets: the
                     * shutdown arrives as decrypted EOF via tls_recv. */
                    if (e->events & (EPOLLIN | EPOLLRDHUP)) {
                        conn_on_readable(loop, conn);
                    }
                    continue;
                }
                if (e->events & EPOLLOUT) {
                    cf_http_conn_flush(conn);
                    if (conn->fd < 0) continue;
                }
                if ((e->events & (EPOLLIN | EPOLLRDHUP)) &&
                    (conn->state == CF_HTTP_STATE_ACCEPTED ||
                     conn->state == CF_HTTP_STATE_HEADERS ||
                     conn->state == CF_HTTP_STATE_BODY)) {
                    conn_on_readable(loop, conn);
                } else if (e->events & EPOLLRDHUP) {
                    conn_note_half_close(loop, conn);
                }
                continue;
            }
        }
    }

    /* Final teardown of live connections: after the drain window every
     * admitted task still outstanding is released through its completion. */
    for (size_t i = 0; i < loop->conn_cap; i++) {
        struct cf_http_conn *conn = &loop->conns[i];
        if (conn->state != CF_HTTP_STATE_CLOSED) cf_http_conn_close(loop, conn);
    }
    loop->run_returned = true;
    return CF_OK;
}

void cf_http_loop_destroy(cf_http_loop *loop) {
    if (loop == NULL) return;
    cf_http_loop_stop(loop);
    if (loop->file_worker.started) {
        pthread_mutex_lock(&loop->file_worker.mutex);
        loop->file_worker.stop = true;
        pthread_cond_broadcast(&loop->file_worker.cond);
        pthread_mutex_unlock(&loop->file_worker.mutex);
        pthread_join(loop->file_worker.thread, NULL);
        loop->file_worker.started = false;
    }

    /* Release connections (closes stream fds), then anything queued. */
    for (size_t i = 0; i < loop->conn_cap; i++) {
        cf_http_conn_close(loop, &loop->conns[i]);
    }
    loop_drain_completions(loop);

    pthread_mutex_destroy(&loop->file_worker.mutex);
    pthread_cond_destroy(&loop->file_worker.cond);
    pthread_mutex_destroy(&loop->completion_mutex);
    if (loop->epfd >= 0) close(loop->epfd);
    if (loop->wake_fd >= 0) close(loop->wake_fd);
    free(loop->scratch);
    free(loop->conns);
    free(loop->free_slots);
    free(loop->public_origin);
    free(loop);
}

void cf_http_loop_counters(const cf_http_loop *loop, cf_http_counters *out) {
    if (out == NULL) return;
    if (loop == NULL) {
        memset(out, 0, sizeof *out);
        return;
    }
    *out = loop->counters;
}
