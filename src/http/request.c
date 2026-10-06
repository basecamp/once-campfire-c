/* H01 request parsing, framing validation and freeze.
 *
 * picohttpparser finds the header block and decodes chunked transfer coding;
 * it is explicitly not the framing authority (01 H01: "Do not treat
 * picohttpparser success as framing validation"). This file owns:
 *  - HTTP/1.1 Host requirement, HTTP/1.0 keep-alive rules, origin-form only;
 *  - rejection of NUL, invalid field names, obs-fold, invalid/repeated/
 *    conflicting Content-Length, TE+CL and unsupported transfer codings
 *    (400 + close);
 *  - incremental decoded-size checks and bounded trailers;
 *  - freezing every span into one owned cf_buf before admission.
 */
#include "http_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <unistd.h>

/* ------------------------------------------------------------ validators */

static bool is_tchar(unsigned char c) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z')) {
        return true;
    }
    return strchr("!#$%&'*+-.^_`|~", (int)c) != NULL;
}

static bool is_token(const unsigned char *p, size_t n) {
    if (n == 0) return false;
    for (size_t i = 0; i < n; i++) {
        if (!is_tchar(p[i])) return false;
    }
    return true;
}

/* field-value bytes: HT, SP..~, and obs-text (>=0x80). */
static bool is_field_value(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = p[i];
        if (c == '\t') continue;
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

#define span_ieq(p, n, lit) cf_http_span_ieq((p), (n), (lit))

static cf_span trim_ows(const unsigned char *p, size_t n) {
    while (n != 0 && (*p == ' ' || *p == '\t')) {
        p++;
        n--;
    }
    while (n != 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) {
        n--;
    }
    return (cf_span){p, n};
}

static cf_method classify_method(const unsigned char *p, size_t n) {
    if (span_ieq(p, n, "GET")) return CF_GET;
    if (span_ieq(p, n, "HEAD")) return CF_HEAD;
    if (span_ieq(p, n, "POST")) return CF_POST;
    if (span_ieq(p, n, "PUT")) return CF_PUT;
    if (span_ieq(p, n, "PATCH")) return CF_PATCH;
    if (span_ieq(p, n, "DELETE")) return CF_DELETE;
    if (span_ieq(p, n, "OPTIONS")) return CF_OPTIONS;
    return CF_OTHER;
}

/* Strict Content-Length: 1..19 digits, no sign/space, no overflow. */
static bool parse_content_length(cf_span v, uint64_t *out) {
    if (v.len == 0 || v.len > 19) return false;
    uint64_t n = 0;
    for (size_t i = 0; i < v.len; i++) {
        unsigned char c = v.ptr[i];
        if (c < '0' || c > '9') return false;
        if (n > (UINT64_MAX - (uint64_t)(c - '0')) / 10) return false;
        n = n * 10 + (uint64_t)(c - '0');
    }
    *out = n;
    return true;
}

/* Host / PUBLIC_ORIGIN consistency (01 H01). Accepts host or host:port,
 * including bracketed IPv6, with no whitespace or userinfo. The origin host
 * is stored without brackets, so a bracketed Host is compared on its literal
 * alone. A portless Host matches only when the origin's effective port is
 * the scheme default; an explicit Host port must equal the origin's. */
static bool host_is_consistent(const struct cf_http_loop *loop, cf_span host) {
    if (host.len == 0 || host.len > 255) return false;
    const unsigned char *p = host.ptr;
    size_t n = host.len;
    size_t host_len;
    unsigned port = 0;
    bool have_port = false;

    if (p[0] == '[') {
        /* [literal] or [literal]:port. The closing ']' is at close_off, so
         * the port colon sits at close_off + 1 and the digits after it. */
        const unsigned char *close = memchr(p, ']', n);
        if (close == NULL) return false;
        size_t close_off = (size_t)(close - p);
        host_len = close_off - 1;
        if (host_len == 0) return false;
        size_t rest = n - close_off - 1; /* bytes after ']' */
        if (rest != 0) {
            if (rest < 2 || p[close_off + 1] != ':') return false;
            const unsigned char *pp = p + close_off + 2;
            size_t pn = rest - 1; /* digits after ':' */
            if (pn > 5) return false;
            for (size_t i = 0; i < pn; i++) {
                if (pp[i] < '0' || pp[i] > '9') return false;
                port = port * 10 + (unsigned)(pp[i] - '0');
            }
            if (port == 0) return false;
            have_port = true;
        }
        p = host.ptr + 1; /* compare the literal without brackets */
    } else {
        const unsigned char *colon = memchr(p, ':', n);
        if (colon != NULL) {
            if (memchr(colon + 1, ':', n - (size_t)(colon - p) - 1) != NULL) {
                return false; /* bare IPv6 needs brackets */
            }
            host_len = (size_t)(colon - p);
            size_t pn = n - host_len - 1;
            if (pn == 0 || pn > 5) return false;
            for (size_t i = 0; i < pn; i++) {
                if (colon[1 + i] < '0' || colon[1 + i] > '9') return false;
                port = port * 10 + (unsigned)(colon[1 + i] - '0');
            }
            if (port == 0) return false;
            have_port = true;
        } else {
            host_len = n;
        }
    }
    if (host_len == 0 || host_len > 255) return false;
    if (host_len != loop->origin_host_len ||
        strncasecmp((const char *)p, loop->origin_host, host_len) != 0) {
        return false;
    }
    unsigned default_port = loop->origin_is_https ? 443u : 80u;
    if (have_port) {
        return port == loop->origin_port;
    }
    /* No port: consistent only with the origin's default port. */
    return loop->origin_port == default_port;
}

/* --------------------------------------------------------- header block */

/* Validate one parsed header block and prepare per-request metadata.
 * Returns 0 on success, an HTTP status to send on rejection, or -1 for an
 * internal allocation failure (caller answers 500). */
static int validate_head(struct cf_http_conn *conn, const char *buf, size_t ret,
                         const char *method, size_t method_len,
                         const char *target, size_t target_len, int minor,
                         const struct phr_header *hdrs, size_t nh) {
    if (minor != 0 && minor != 1) return 400;
    if (method_len == 0 || target_len == 0) return 400;
    if ((unsigned char)target[0] != '/') return 400; /* origin-form only */
    if (target_len > CF_HTTP_TARGET_MAX) return 414;
    if (memchr(target, '#', target_len) != NULL) return 400;
    if (memchr(target, '\0', target_len) != NULL) return 400;

    const unsigned char *tpath = (const unsigned char *)target;
    size_t path_len = target_len;
    size_t query_off = target_len;
    const char *qmark = memchr(target, '?', target_len);
    if (qmark != NULL) {
        path_len = (size_t)(qmark - target);
        query_off = path_len + 1;
    }

    bool have_host = false;
    cf_span host = {NULL, 0};
    unsigned cl_count = 0;
    unsigned te_count = 0;
    unsigned expect_count = 0;
    bool te_chunked = false;
    bool conn_close = false;
    bool conn_keep_alive = false;
    uint64_t content_length = 0;

    for (size_t i = 0; i < nh; i++) {
        const struct phr_header *h = &hdrs[i];
        if (h->name == NULL) return 400; /* obs-fold: continuation line */
        if (!is_token((const unsigned char *)h->name, h->name_len)) return 400;
        if (!is_field_value((const unsigned char *)h->value, h->value_len)) {
            return 400;
        }
        if (span_ieq((const unsigned char *)h->name, h->name_len, "host")) {
            if (have_host) return 400; /* conflicting Host */
            have_host = true;
            host = trim_ows((const unsigned char *)h->value, h->value_len);
            if (host.len == 0) return 400;
        } else if (span_ieq((const unsigned char *)h->name, h->name_len,
                            "content-length")) {
            if (cl_count++ != 0) return 400; /* repeated, even if identical */
            cf_span v = trim_ows((const unsigned char *)h->value,
                                 h->value_len);
            if (!parse_content_length(v, &content_length)) return 400;
        } else if (span_ieq((const unsigned char *)h->name, h->name_len,
                            "transfer-encoding")) {
            if (te_count++ != 0) return 400;
            cf_span v = trim_ows((const unsigned char *)h->value,
                                 h->value_len);
            if (v.len == 0 || memchr(v.ptr, ',', v.len) != NULL) return 400;
            if (!span_ieq(v.ptr, v.len, "chunked")) return 400; /* unsupported */
            te_chunked = true;
        } else if (span_ieq((const unsigned char *)h->name, h->name_len,
                            "expect")) {
            if (expect_count++ != 0) return 417;
            cf_span v = trim_ows((const unsigned char *)h->value,
                                 h->value_len);
            if (!span_ieq(v.ptr, v.len, "100-continue")) return 417;
        } else if (span_ieq((const unsigned char *)h->name, h->name_len,
                            "connection")) {
            /* Connection is a comma list; combine every occurrence. */
            cf_span v = (cf_span){(const unsigned char *)h->value,
                                  h->value_len};
            size_t pos = 0;
            while (pos < v.len) {
                size_t end = pos;
                while (end < v.len && v.ptr[end] != ',') end++;
                cf_span tok = trim_ows(v.ptr + pos, end - pos);
                if (span_ieq(tok.ptr, tok.len, "close")) conn_close = true;
                if (span_ieq(tok.ptr, tok.len, "keep-alive")) {
                    conn_keep_alive = true;
                }
                pos = end + 1;
            }
        }
    }

    if (te_chunked && cl_count != 0) return 400; /* TE + CL */
    if (content_length > CF_HTTP_BODY_MAX) return 413; /* body cap */

    if (have_host) {
        if (!host_is_consistent(conn->loop, host)) return 400;
    } else if (minor == 1) {
        return 400; /* HTTP/1.1 requires Host */
    }

    /* Copy the validated header block into stable memory; every stored span
     * is an offset into that copy. */
    if (cf_builder_append(&conn->head, (cf_span){(const unsigned char *)buf,
                                                 ret}) != CF_OK) {
        return -1;
    }
    const unsigned char *base = (const unsigned char *)buf;
    const unsigned char *hbase = conn->head.ptr;
    if (hbase == NULL) return -1;

    conn->off_method = (size_t)((const unsigned char *)method - base);
    conn->len_method = method_len;
    conn->off_target = (size_t)((const unsigned char *)target - base);
    conn->len_target = target_len;
    conn->off_path = (size_t)(tpath - base);
    conn->len_path = path_len;
    conn->off_query = (size_t)((const unsigned char *)(target + query_off) -
                               base);
    conn->len_query = (size_t)(target_len - query_off);
    /* Offsets must fall inside the copied block. */
    if (conn->off_method + method_len > ret || conn->off_target + target_len > ret) {
        return 400;
    }

    conn->hdr_count = nh;
    for (size_t i = 0; i < nh; i++) {
        conn->hdr_refs[i].name_off = (size_t)((const unsigned char *)hdrs[i].name - base);
        conn->hdr_refs[i].name_len = hdrs[i].name_len;
        conn->hdr_refs[i].value_off = (size_t)((const unsigned char *)hdrs[i].value - base);
        conn->hdr_refs[i].value_len = hdrs[i].value_len;
        if (conn->hdr_refs[i].name_off + hdrs[i].name_len > ret ||
            conn->hdr_refs[i].value_off + hdrs[i].value_len > ret) {
            return 400;
        }
    }

    conn->method = classify_method((const unsigned char *)method, method_len);
    conn->chunked = te_chunked;
    conn->body_expected = te_chunked ? 0 : content_length;
    conn->expect_100 = expect_count != 0;
    if (minor == 1) {
        conn->close_after = conn_close;
    } else {
        conn->close_after = !conn_keep_alive; /* HTTP/1.0 default close */
    }
    return 0;
}

/* --------------------------------------------------- chunked trailers */

/* Returns 0 = trailer section complete (pipelined bytes moved to `in`),
 * -1 = need more, > 0 = status to send. */
static int scan_trailers(struct cf_http_conn *conn) {
    const unsigned char *p = conn->trailers.ptr;
    size_t len = conn->trailers.len;
    size_t pos = 0;
    while (pos < len) {
        if (pos >= CF_HTTP_TRAILER_MAX) return 431;
        const unsigned char *nl = memmem(p + pos, len - pos, "\r\n", 2);
        if (nl == NULL) {
            return len - pos > CF_HTTP_TRAILER_MAX ? 431 : -1;
        }
        size_t line_len = (size_t)(nl - (p + pos));
        if (line_len == 0) {
            size_t consumed = pos + 2;
            if (len > consumed &&
                cf_builder_append(&conn->in,
                                  (cf_span){p + consumed, len - consumed}) !=
                    CF_OK) {
                return 500;
            }
            conn->trailers.len = 0;
            conn->trailers_done = true;
            return 0;
        }
        /* Trailer field-line: token name ":" value; obs-fold forbidden. */
        const unsigned char *line = p + pos;
        if (line[0] == ' ' || line[0] == '\t') return 400;
        const unsigned char *colon = memchr(line, ':', line_len);
        if (colon == NULL) return 400;
        size_t name_len = (size_t)(colon - line);
        if (!is_token(line, name_len)) return 400;
        cf_span v = trim_ows(colon + 1, line_len - name_len - 1);
        if (!is_field_value(v.ptr, v.len)) return 400;
        pos += line_len + 2;
    }
    return -1;
}

/* -------------------------------------------------------------- freeze */

typedef struct {
    const unsigned char *ptr;
    size_t len;
} piece;

/* Build the single owned storage for a complete request. Returns CF_OK with
 * *out and *reserved set (the caller owns the reservation), CF_LIMIT when the
 * loop input budget rejects it, or CF_NOMEM. */
static cf_err freeze_request(struct cf_http_conn *conn, cf_request **out,
                             size_t *reserved) {
    *out = NULL;
    *reserved = 0;

    piece pieces[6 + 2 * CF_HTTP_HEADER_COUNT_MAX];
    size_t np = 0;
    const unsigned char *hb = conn->head.ptr;
    if (hb == NULL && conn->head.len != 0) return CF_INVALID;

    pieces[np++] = (piece){hb + conn->off_method, conn->len_method};
    pieces[np++] = (piece){hb + conn->off_target, conn->len_target};
    pieces[np++] = (piece){hb + conn->off_path, conn->len_path};
    pieces[np++] = (piece){hb + conn->off_query, conn->len_query};
    if (conn->chunked) {
        pieces[np++] = (piece){conn->body.ptr, conn->body.len};
    } else {
        pieces[np++] = (piece){conn->in.ptr, (size_t)conn->body_expected};
    }
    size_t peer_len = strlen(conn->peer_ip);
    pieces[np++] = (piece){(const unsigned char *)conn->peer_ip, peer_len};
    for (size_t i = 0; i < conn->hdr_count; i++) {
        pieces[np++] = (piece){hb + conn->hdr_refs[i].name_off,
                               conn->hdr_refs[i].name_len};
        pieces[np++] = (piece){hb + conn->hdr_refs[i].value_off,
                               conn->hdr_refs[i].value_len};
    }

    size_t total = 0;
    for (size_t i = 0; i < np; i++) {
        if (pieces[i].len > SIZE_MAX - total) return CF_LIMIT;
        total += pieces[i].len;
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
            rc = cf_builder_append(&sb, (cf_span){pieces[i].ptr, pieces[i].len});
        }
        cursor += pieces[i].len;
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
#define PIECE_SPAN() ((cf_span){all.ptr + offsets[idx], pieces[idx].len}), idx++
    req->raw_method = PIECE_SPAN();
    req->target = PIECE_SPAN();
    req->path = PIECE_SPAN();
    req->query = PIECE_SPAN();
    req->body = PIECE_SPAN();
    req->peer_ip = PIECE_SPAN();
#undef PIECE_SPAN
    for (size_t i = 0; i < conn->hdr_count; i++) {
        req->headers[i].name = (cf_span){all.ptr + offsets[idx],
                                         pieces[idx].len};
        idx++;
        req->headers[i].value = (cf_span){all.ptr + offsets[idx],
                                          pieces[idx].len};
        idx++;
    }
    req->header_count = conn->hdr_count;
    req->method = conn->method;
    req->original_method = conn->method;
    req->close_after = conn->close_after || conn->input_closed;
    req->tls = false;
    req->params = NULL;
    *out = req;
    *reserved = total;
    return CF_OK;
}

/* Clear per-request parse state; `in` keeps pipelined bytes. */
static void cf_http_request_reset_parse(struct cf_http_conn *conn);

static void cf_http_request_reset_parse(struct cf_http_conn *conn) {
    conn->head.len = 0;
    conn->body.len = 0;
    conn->trailers.len = 0;
    conn->hdr_count = 0;
    memset(&conn->chunk_dec, 0, sizeof conn->chunk_dec);
    conn->chunk_done = false;
    conn->chunked = false;
    conn->body_expected = 0;
    conn->expect_100 = false;
    conn->trailers_done = false;
    conn->request_offered = false;
    conn->off_method = conn->off_target = conn->off_path = conn->off_query = 0;
    conn->len_method = conn->len_target = conn->len_path = conn->len_query = 0;
}

void cf_http_request_abort(struct cf_http_conn *conn) {
    cf_builder_dispose(&conn->in);
    cf_builder_dispose(&conn->head);
    cf_builder_dispose(&conn->body);
    cf_builder_dispose(&conn->trailers);
}

void cf_http_request_start(struct cf_http_conn *conn, bool idle) {
    cf_http_request_reset_parse(conn);
    conn->state = CF_HTTP_STATE_HEADERS;
    conn->deadline_kind = idle ? CF_HTTP_DL_IDLE : CF_HTTP_DL_HEADER;
    conn->deadline_ms = cf_monotonic_ms() + CF_HTTP_DEADLINE_MS;
}

void cf_http_request_eof(struct cf_http_conn *conn) {
    conn->input_closed = true;
    switch (conn->state) {
    case CF_HTTP_STATE_ACCEPTED:
    case CF_HTTP_STATE_HEADERS:
    case CF_HTTP_STATE_BODY:
        /* Closed/incomplete bodies never reach handlers. */
        cf_http_conn_close(conn->loop, conn);
        break;
    default:
        /* WORKING/WRITING: the client can still receive the response. */
        break;
    }
}

void cf_http_request_free_input(struct cf_http_loop *loop, cf_request *req,
                                size_t reserved) {
    /* cf_request_destroy releases the owned resources and resets the struct
     * (cf.h: callers may hold it inline); H01 allocated this request in
     * freeze_request, so H01 frees the storage too. */
    cf_request_destroy(req);
    free(req);
    cf_http_loop_release_input(loop, reserved);
}

/* ------------------------------------------------------------- driving */

static void input_progress(struct cf_http_conn *conn) {
    if (conn->deadline_kind == CF_HTTP_DL_HEADER ||
        conn->deadline_kind == CF_HTTP_DL_BODY ||
        conn->deadline_kind == CF_HTTP_DL_IDLE) {
        conn->deadline_kind = conn->state == CF_HTTP_STATE_BODY
                                  ? CF_HTTP_DL_BODY
                                  : CF_HTTP_DL_HEADER;
        conn->deadline_ms = cf_monotonic_ms() + CF_HTTP_DEADLINE_MS;
    }
}

/* Count header lines in a complete header block (request line excluded);
 * used only when picohttpparser rejects a block, to answer the header-count
 * cap with 431 instead of 400. Returns 0 when the terminator is absent. */
static size_t count_header_lines(const unsigned char *p, size_t len) {
    const unsigned char *end = memmem(p, len, "\r\n\r\n", 4);
    if (end == NULL) return 0;
    const unsigned char *first = memmem(p, (size_t)(end - p), "\r\n", 2);
    if (first == NULL) return 0;
    size_t count = 0;
    const unsigned char *cur = first + 2;
    while (cur < end) {
        const unsigned char *nl = memmem(cur, (size_t)(end - cur), "\r\n", 2);
        if (nl == NULL) break;
        count++;
        cur = nl + 2;
    }
    return count;
}

static void fail(struct cf_http_conn *conn, unsigned status) {
    conn->must_close_input = true;
    conn->loop->counters.errors++;
    (void)cf_http_conn_queue_status(conn, status, true, NULL);
}

/* Consume this request's body bytes from `in`, keeping only pipelined bytes.
 * Chunked decoding already moved the decoded body out of `in`. */
static void drop_consumed_body(struct cf_http_conn *conn) {
    if (conn->chunked) return;
    size_t body = (size_t)conn->body_expected;
    if (conn->in.len > body) {
        memmove(conn->in.ptr, conn->in.ptr + body, conn->in.len - body);
        conn->in.len -= body;
    } else {
        conn->in.len = 0;
    }
}

/* Run the loop's upgrade hook (C01's /cable front mount) on a complete
 * request, before it is frozen or admitted. See cf_http_upgrade_request in
 * http.h. */
static cf_http_upgrade_result run_upgrade_hook(struct cf_http_conn *conn) {
    struct cf_http_loop *loop = conn->loop;
    const unsigned char *hb = conn->head.ptr;
    if (hb == NULL) return CF_HTTP_UPGRADE_PASS;

    cf_header hdrs[CF_HTTP_HEADER_COUNT_MAX];
    for (size_t i = 0; i < conn->hdr_count; i++) {
        hdrs[i].name = (cf_span){hb + conn->hdr_refs[i].name_off,
                                 conn->hdr_refs[i].name_len};
        hdrs[i].value = (cf_span){hb + conn->hdr_refs[i].value_off,
                                  conn->hdr_refs[i].value_len};
    }
    /* Bytes after the request head: the body (if any) is not pending; for a
     * bodyless GET everything buffered is. */
    const unsigned char *pending = conn->in.ptr;
    size_t pending_len = conn->in.len;
    if (!conn->chunked && conn->body_expected <= conn->in.len) {
        pending += (size_t)conn->body_expected;
        pending_len -= (size_t)conn->body_expected;
    } else if (!conn->chunked) {
        pending_len = 0;
    }

    cf_http_upgrade_request req;
    memset(&req, 0, sizeof req);
    req.fd = conn->fd >= 0 ? conn->fd : conn->swap_fd;
    req.tls = conn->tls;
    req.loop_index = loop->cfg.loop_index;
    req.method = conn->method;
    req.target = (cf_span){hb + conn->off_target, conn->len_target};
    req.path = (cf_span){hb + conn->off_path, conn->len_path};
    req.query = (cf_span){hb + conn->off_query, conn->len_query};
    req.peer_ip = (cf_span){(const unsigned char *)conn->peer_ip,
                            strlen(conn->peer_ip)};
    req.headers = hdrs;
    req.header_count = conn->hdr_count;
    req.pending = pending;
    req.pending_len = pending_len;

    cf_http_upgrade_result result = loop->cfg.upgrade(loop->cfg.upgrade_user,
                                                      &req);

    if (result == CF_HTTP_UPGRADE_REPLY) {
        drop_consumed_body(conn);
        if (req.reply.len != 0) {
            cf_err rc =
                cf_http_conn_queue_bytes(conn, req.reply.ptr, req.reply.len);
            if (rc == CF_OK) {
                conn->pending_kind = CF_HTTP_PENDING_FINAL;
                conn->state = CF_HTTP_STATE_WRITING;
                conn->close_after = conn->close_after || req.close_after;
                /* Flushing happens from the loop's EPOLLOUT branch, so this
                 * call never re-enters the request parser. */
                cf_http_conn_update_events(conn);
            } else {
                loop->counters.errors++;
                cf_http_conn_close(loop, conn);
            }
        } else {
            /* A REPLY with no bytes cannot be sent as a response. */
            loop->counters.errors++;
            cf_http_conn_close(loop, conn);
        }
        cf_builder_dispose(&req.reply);
        cf_http_request_reset_parse(conn);
        return result;
    }

    if (result == CF_HTTP_UPGRADE_TAKEN) {
        cf_http_upgrade_taken_fn taken = req.taken;
        void *taken_user = req.taken_user;
        cf_builder_dispose(&req.reply);
        /* The descriptor leaves HTTP's read/write state machine without being
         * closed. The connection slot stays reserved for the upgraded
         * connection's whole lifetime: the lease is an admitted (never
         * answered) task whose abandon completion, processed on this loop
         * thread, is the single owner-side point that closes the fd and
         * releases the slot. If the lease cannot be allocated, the old
         * no-reservation detach applies: the hook gets the fd with lease ==
         * NULL and owns its close. */
        /* TLS parsing suppresses plaintext output by hiding fd. Restore it
         * before admission and detach; the new owner receives the TLS object
         * only once lifetime admission succeeds. */
        if (conn->fd < 0 && conn->swap_fd >= 0) {
            conn->fd = conn->swap_fd;
            conn->swap_fd = -1;
        }
        if (conn->fd >= 0 && conn->registered) {
            (void)epoll_ctl(loop->epfd, EPOLL_CTL_DEL, conn->fd, NULL);
            conn->registered = false;
        }
        struct cf_http_task *task = calloc(1, sizeof *task);
        cf_http_upgrade_lease *lease = NULL;
        if (task != NULL && conn->fd >= 0) {
            task->loop = loop;
            task->conn = conn->id;
            task->sequence = ++conn->sequence;
            cf_response_init(&task->response);
            atomic_init(&task->submit_state, 0);
            conn->pending_task = task;
            conn->state = CF_HTTP_STATE_WORKING;
            conn->deadline_kind = CF_HTTP_DL_NONE;
            conn->deadline_ms = 0;
            loop->outstanding_tasks++;
            lease = (cf_http_upgrade_lease *)task;
            conn->tls = NULL;
        } else {
            free(task);
            /* No reservation is possible: the upgrade is refused (the
             * connection is retired and its fd closed here) rather than
             * proceeding without lifetime admission. */
            if (conn->fd >= 0) {
                close(conn->fd);
                conn->fd = -1;
                if (loop->active_conns != 0) loop->active_conns--;
            }
            loop->counters.errors++;
            cf_http_conn_close(loop, conn);
            lease = CF_HTTP_UPGRADE_REJECTED;
        }
        /* Only now is the fd free of HTTP read/write state: the hook may use
         * or hand it to another owner (the cable transport). */
        if (taken != NULL) (void)taken(taken_user, lease);
        return result;
    }

    cf_builder_dispose(&req.reply);
    return CF_HTTP_UPGRADE_PASS;
}

/* Release a TAKEN upgrade's lifetime reservation: abandon the never-answered
 * lease task, whose completion (on the loop thread) closes the descriptor and
 * frees the connection slot exactly once. Safe from any thread; the
 * submit_state CAS inside cf_http_task_abandon makes a second release a
 * no-op, so "released exactly once" cannot double-close. */
void cf_http_upgrade_release(cf_http_upgrade_lease *lease) {
    if (lease == NULL) return;
    cf_http_task_abandon((struct cf_http_task *)lease);
}

static void offer_request(struct cf_http_conn *conn) {
    if (conn->request_offered) return;
    conn->request_offered = true;

    if (conn->loop->cfg.upgrade != NULL) {
        cf_http_upgrade_result ur = run_upgrade_hook(conn);
        if (ur != CF_HTTP_UPGRADE_PASS) return; /* answered or moved to cable */
    }

    cf_request *req = NULL;
    size_t reserved = 0;
    cf_err rc = freeze_request(conn, &req, &reserved);
    if (rc != CF_OK) {
        conn->loop->counters.budget_rejected++;
        if (rc == CF_LIMIT) {
            (void)cf_http_conn_queue_status(conn, 503, true, "1");
        } else {
            conn->loop->counters.errors++;
            (void)cf_http_conn_queue_status(conn, 500, true, NULL);
        }
        cf_http_request_reset_parse(conn);
        return;
    }

    /* CL bodies are consumed from `in`; keep only pipelined bytes. */
    drop_consumed_body(conn);

    uint64_t sequence = ++conn->sequence;
    cf_err ar = cf_http_loop_admit(conn->loop, conn, req, reserved, sequence);
    if (ar != CF_OK) {
        /* loop_admit released the request and reservation on failure */
        cf_http_request_reset_parse(conn);
        return;
    }
    cf_http_request_reset_parse(conn);
}

/* Append decoded chunk bytes with the incremental decoded-size check. */
static bool chunked_append(struct cf_http_conn *conn,
                           const unsigned char *data, size_t len) {
    if (len == 0) return true;
    if (len > CF_HTTP_BODY_MAX - conn->body.len) {
        fail(conn, 413);
        return false;
    }
    if (cf_builder_append(&conn->body, (cf_span){data, len}) != CF_OK) {
        fail(conn, 500);
        return false;
    }
    return true;
}

static void chunked_feed(struct cf_http_conn *conn, const unsigned char *data,
                         size_t len) {
    size_t decoded = len;
    ssize_t r = phr_decode_chunked(&conn->chunk_dec, (char *)data, &decoded);
    if (r == -1) {
        fail(conn, 400);
        return;
    }
    if (!chunked_append(conn, data, decoded)) return;
    if (r == -2) return; /* incomplete */
    /* Complete: bytes data[decoded .. decoded + r) are the trailer section. */
    conn->chunk_done = true;
    if (r > 0) {
        if ((size_t)r > CF_HTTP_TRAILER_MAX - conn->trailers.len) {
            fail(conn, 431);
            return;
        }
        if (cf_builder_append(&conn->trailers, (cf_span){data + decoded,
                                                         (size_t)r}) !=
            CF_OK) {
            fail(conn, 500);
            return;
        }
    }
    int st = scan_trailers(conn);
    if (st == -1) return;
    if (st > 0) {
        fail(conn, (unsigned)st);
        return;
    }
    offer_request(conn);
}

static void body_ready(struct cf_http_conn *conn) {
    if (conn->chunked) {
        /* in[] holds raw chunk bytes left over from the header read. */
        if (conn->in.len != 0) {
            size_t n = conn->in.len;
            conn->in.len = 0;
            chunked_feed(conn, conn->in.ptr, n);
            /* note: chunked_feed may modify the buffer in place */
            return;
        }
        return;
    }
    if (conn->in.len >= (size_t)conn->body_expected) {
        offer_request(conn);
    }
}

/* Drive parsing from bytes already buffered (len == 0) or newly read. Called
 * with len == 0 when a response finished and pipelined bytes wait in `in`. */
void cf_http_request_input(struct cf_http_conn *conn,
                           const unsigned char *data, size_t len) {
    if (conn->state == CF_HTTP_STATE_CLOSED) return;
    if (len == 0 && data == NULL && conn->state == CF_HTTP_STATE_HEADERS &&
        conn->in.len == 0) {
        return;
    }
    input_progress(conn);

    if (conn->state == CF_HTTP_STATE_HEADERS) {
        if (len != 0 &&
            cf_builder_append(&conn->in, (cf_span){data, len}) != CF_OK) {
            fail(conn, 500);
            return;
        }
        const char *m = NULL, *t = NULL;
        size_t ml = 0, tl = 0;
        int minor = 0;
        struct phr_header hdrs[CF_HTTP_HEADER_COUNT_MAX + 2];
        size_t nh = CF_HTTP_HEADER_COUNT_MAX + 1;
        int ret = phr_parse_request((const char *)conn->in.ptr, conn->in.len,
                                    &m, &ml, &t, &tl, &minor, hdrs, &nh, 0);
        if (ret == -2) {
            if (conn->in.len > CF_HTTP_HEADER_MAX) fail(conn, 431);
            return;
        }
        if (ret == -1) {
            /* Too many header lines is a header cap (431), not a syntax
             * error; picohttpparser rejects before we can count. */
            if (count_header_lines(conn->in.ptr, conn->in.len) >
                CF_HTTP_HEADER_COUNT_MAX) {
                fail(conn, 431);
            } else {
                fail(conn, 400);
            }
            return;
        }
        if (nh > CF_HTTP_HEADER_COUNT_MAX) {
            fail(conn, 431);
            return;
        }
        if ((size_t)ret > CF_HTTP_HEADER_MAX) {
            fail(conn, 431); /* header bytes cap, complete or not */
            return;
        }
        if (memchr(conn->in.ptr, '\0', (size_t)ret) != NULL) {
            fail(conn, 400);
            return;
        }
        int st = validate_head(conn, (const char *)conn->in.ptr, (size_t)ret,
                               m, ml, t, tl, minor, hdrs, nh);
        if (st == -1) {
            fail(conn, 500);
            return;
        }
        if (st > 0) {
            fail(conn, (unsigned)st);
            return;
        }
        /* Drop the header block from `in`; body/pipelined bytes remain. */
        if (conn->in.len > (size_t)ret) {
            memmove(conn->in.ptr, conn->in.ptr + ret, conn->in.len - (size_t)ret);
            conn->in.len -= (size_t)ret;
        } else {
            conn->in.len = 0;
        }
        conn->state = CF_HTTP_STATE_BODY;
        input_progress(conn);
        if (conn->expect_100 &&
            (conn->chunked || conn->body_expected > 0)) {
            if (cf_http_conn_queue_continue(conn) != CF_OK) {
                fail(conn, 500);
                return;
            }
        }
        if (conn->chunked) {
            if (conn->in.len != 0) {
                size_t n = conn->in.len;
                unsigned char *p = conn->in.ptr;
                conn->in.len = 0;
                chunked_feed(conn, p, n);
            }
            return;
        }
        body_ready(conn);
        return;
    }

    if (conn->state == CF_HTTP_STATE_BODY) {
        if (conn->chunked) {
            if (conn->chunk_done) {
                if (conn->trailers_done) return;
                if (len != 0) {
                    if ((size_t)len >
                        CF_HTTP_TRAILER_MAX - conn->trailers.len) {
                        fail(conn, 431);
                        return;
                    }
                    if (cf_builder_append(&conn->trailers,
                                          (cf_span){data, len}) != CF_OK) {
                        fail(conn, 500);
                        return;
                    }
                }
                int st = scan_trailers(conn);
                if (st == -1) return;
                if (st > 0) {
                    fail(conn, (unsigned)st);
                    return;
                }
                offer_request(conn);
                return;
            }
            if (len != 0) chunked_feed(conn, data, len);
            return;
        }
        if (len != 0 &&
            cf_builder_append(&conn->in, (cf_span){data, len}) != CF_OK) {
            fail(conn, 500);
            return;
        }
        body_ready(conn);
        return;
    }
    /* WORKING/WRITING/CLOSED never read (interest is removed). */
}
