/* P01 H2 implementation; see h2.h for the contract. nghttp2 owns
 * framing/HPACK; this file owns admission policy, budgets, keys and the
 * response data source that only emits under flow-control allowance. */
#include "h2.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include <nghttp2/nghttp2.h>

/* One stream record. Records persist after close so tests and the loop seam
 * can distinguish open / cancelled / refused / drained outcomes. */
struct h2_stream {
    int32_t stream_id;
    uint64_t sequence; /* per-connection request sequence at admission */
    bool open;
    bool refused;    /* never admitted: validation/budget/drain refusal */
    bool cancelled;  /* RST_STREAM after admission */
    bool committed;  /* response submitted (commit point) */
    bool counted_refusal;
    size_t header_bytes;
    size_t header_count;
    size_t body_len;
    size_t queued; /* submitted response bytes not yet framed out */
    size_t sent;   /* bytes handed to nghttp2 framing (committed) */
    unsigned char *resp_body; /* owned response copy until close */
    size_t resp_len;
    size_t resp_off;
    cf_front_h2_headers_result validation;
};

struct cf_front_h2_session {
    nghttp2_session *ng;
    char *public_origin; /* owned copy */
    /* Accumulating decode state for the in-flight header block. nghttp2
     * reuses its inflate scratch area across headers, so names/values are
     * copied into hdr_buf (offsets recorded); pointers are never retained.
     * At most HEADER_COUNT_MAX+1 pairs are stored; the rest is counted so
     * over-count blocks still report TOO_MANY. hdr_over marks a block past
     * the byte budget (contents irrelevant: it is refused). */
    int32_t hdr_stream;
    size_t hdr_name_off[CF_FRONT_H2_HEADER_COUNT_MAX + 1];
    size_t hdr_name_len[CF_FRONT_H2_HEADER_COUNT_MAX + 1];
    size_t hdr_value_off[CF_FRONT_H2_HEADER_COUNT_MAX + 1];
    size_t hdr_value_len[CF_FRONT_H2_HEADER_COUNT_MAX + 1];
    unsigned char *hdr_buf;
    size_t hdr_buf_len;
    size_t hdr_buf_cap;
    size_t hdr_count;
    size_t hdr_bytes;
    bool hdr_active;
    bool hdr_over;
    struct h2_stream *streams;
    size_t stream_count;
    size_t stream_cap;
    uint64_t next_sequence;
    uint64_t admitted_total;
    uint64_t refused_total;
    uint64_t resets_total;
    size_t conn_input_used;
    size_t conn_output_used;
    bool draining;
    bool goaway_sent;
};

bool cf_front_h2_key_valid(const cf_front_h2_key *key, cf_conn_id live_conn,
                            uint64_t live_sequence, bool stream_open) {
    if (key == NULL || !stream_open) return false;
    if (key->conn.loop != live_conn.loop ||
        key->conn.slot != live_conn.slot ||
        key->conn.generation != live_conn.generation) {
        return false;
    }
    if (key->sequence != live_sequence) return false;
    if (key->stream_id <= 0 || (key->stream_id % 2) == 0) return false;
    return true;
}

/* ------------------------------------------------ origin + authority */

static bool parse_origin(const char *origin, char *host, size_t host_cap,
                         size_t *host_len, unsigned *port, bool *is_https) {
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
        if (digits == 0 || v == 0 || digits > 5) return false;
        prt = v;
    }
    if (*p != '\0') return false;
    *host_len = hlen;
    *port = prt;
    *is_https = https;
    return true;
}

static bool span_ieq(const unsigned char *p, size_t n, const char *lit) {
    size_t m = strlen(lit);
    if (n != m) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char a = p[i];
        unsigned char b = (unsigned char)lit[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool cf_front_h2_authority_allowed(const char *authority, size_t authority_len,
                                    const char *public_origin) {
    /* Same rule as the HTTP/1 loop's host_is_consistent (request.c). */
    char origin_host[256];
    size_t origin_host_len = 0;
    unsigned origin_port = 0;
    bool origin_is_https = false;
    if (authority == NULL || authority_len == 0 || authority_len > 255) {
        return false;
    }
    if (!parse_origin(public_origin, origin_host, sizeof origin_host,
                      &origin_host_len, &origin_port, &origin_is_https)) {
        return false;
    }
    const unsigned char *p = (const unsigned char *)authority;
    size_t n = authority_len;
    size_t host_len = 0;
    unsigned port = 0;
    bool have_port = false;
    const unsigned char *cmp = NULL;
    if (p[0] == '[') {
        const unsigned char *close =
            memchr(p, ']', n);
        if (close == NULL) return false;
        size_t close_off = (size_t)(close - p);
        host_len = close_off - 1;
        if (host_len == 0) return false;
        size_t rest = n - close_off - 1;
        if (rest != 0) {
            if (rest < 2 || p[close_off + 1] != ':') return false;
            const unsigned char *pp = p + close_off + 2;
            size_t pn = rest - 1;
            if (pn == 0 || pn > 5) return false;
            for (size_t i = 0; i < pn; i++) {
                if (pp[i] < '0' || pp[i] > '9') return false;
                port = port * 10 + (unsigned)(pp[i] - '0');
            }
            if (port == 0) return false;
            have_port = true;
        }
        cmp = p + 1;
    } else {
        const unsigned char *colon = memchr(p, ':', n);
        if (colon != NULL) {
            if (memchr(colon + 1, ':', n - (size_t)(colon - p) - 1) != NULL) {
                return false;
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
        cmp = p;
    }
    if (host_len == 0 || host_len > 255) return false;
    if (host_len != origin_host_len ||
        strncasecmp((const char *)cmp, origin_host, host_len) != 0) {
        return false;
    }
    unsigned default_port = origin_is_https ? 443u : 80u;
    if (have_port) return port == origin_port;
    return origin_port == default_port;
}

/* ------------------------------------------------ header validation */

static bool is_conn_header(const unsigned char *name, size_t len) {
    return span_ieq(name, len, "connection") ||
           span_ieq(name, len, "keep-alive") ||
           span_ieq(name, len, "proxy-authenticate") ||
           span_ieq(name, len, "proxy-authorization") ||
           span_ieq(name, len, "te") || span_ieq(name, len, "trailer") ||
           span_ieq(name, len, "transfer-encoding") ||
           span_ieq(name, len, "upgrade");
}

cf_front_h2_headers_result cf_front_h2_check_headers(
    const unsigned char **names, const size_t *name_lens,
    const unsigned char **values, const size_t *value_lens, size_t count,
    const char *public_origin, const unsigned char **authority_out,
    size_t *authority_len_out) {
    if (authority_out != NULL) *authority_out = NULL;
    if (authority_len_out != NULL) *authority_len_out = 0;
    if (names == NULL || name_lens == NULL || values == NULL ||
        value_lens == NULL) {
        return CF_FRONT_H2_HEADERS_MISSING_PSEUDO;
    }
    if (count > CF_FRONT_H2_HEADER_COUNT_MAX) {
        return CF_FRONT_H2_HEADERS_TOO_MANY;
    }
    bool have_method = false, have_scheme = false, have_path = false,
         have_authority = false;
    bool pseudo_ended = false;
    const unsigned char *authority = NULL;
    size_t authority_len = 0;
    const unsigned char *path = NULL;
    size_t path_len = 0;
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        const unsigned char *nm = names[i];
        size_t nl = name_lens[i];
        const unsigned char *vl = values[i];
        size_t vll = value_lens[i];
        if (nm == NULL || vl == NULL || nl == 0) {
            return CF_FRONT_H2_HEADERS_BAD_NAME;
        }
        if (nl > SIZE_MAX - vll || total + nl + vll < total) {
            return CF_FRONT_H2_HEADERS_TOO_LARGE;
        }
        total += nl + vll;
        if (total > CF_FRONT_H2_HEADER_MAX) {
            return CF_FRONT_H2_HEADERS_TOO_LARGE;
        }
        if (nm[0] == ':') {
            if (pseudo_ended) {
                return CF_FRONT_H2_HEADERS_DUPLICATE_PSEUDO;
            }
            if (span_ieq(nm, nl, ":method")) {
                if (have_method || vll == 0) {
                    return CF_FRONT_H2_HEADERS_DUPLICATE_PSEUDO;
                }
                have_method = true;
            } else if (span_ieq(nm, nl, ":scheme")) {
                if (have_scheme || vll == 0) {
                    return CF_FRONT_H2_HEADERS_DUPLICATE_PSEUDO;
                }
                have_scheme = true;
            } else if (span_ieq(nm, nl, ":path")) {
                if (have_path) {
                    return CF_FRONT_H2_HEADERS_DUPLICATE_PSEUDO;
                }
                have_path = true;
                path = vl;
                path_len = vll;
            } else if (span_ieq(nm, nl, ":authority")) {
                if (have_authority || vll == 0) {
                    return CF_FRONT_H2_HEADERS_DUPLICATE_PSEUDO;
                }
                have_authority = true;
                authority = vl;
                authority_len = vll;
            } else {
                return CF_FRONT_H2_HEADERS_DUPLICATE_PSEUDO;
            }
        } else {
            pseudo_ended = true;
            if (is_conn_header(nm, nl)) {
                return CF_FRONT_H2_HEADERS_CONN_HEADER;
            }
        }
    }
    if (!have_method || !have_scheme || !have_path || !have_authority) {
        return CF_FRONT_H2_HEADERS_MISSING_PSEUDO;
    }
    /* Origin-form target within the HTTP/1 target budget. */
    if (path_len == 0 || path_len > CF_FRONT_H2_TARGET_MAX ||
        path[0] != '/') {
        return CF_FRONT_H2_HEADERS_BAD_TARGET;
    }
    if (memchr(path, '#', path_len) != NULL ||
        memchr(path, '\0', path_len) != NULL) {
        return CF_FRONT_H2_HEADERS_BAD_TARGET;
    }
    if (!cf_front_h2_authority_allowed((const char *)authority, authority_len,
                                       public_origin)) {
        return CF_FRONT_H2_HEADERS_BAD_AUTHORITY;
    }
    if (authority_out != NULL) *authority_out = authority;
    if (authority_len_out != NULL) *authority_len_out = authority_len;
    return CF_FRONT_H2_HEADERS_OK;
}

/* ------------------------------------------------ session internals */

static struct h2_stream *find_stream(cf_front_h2_session *s,
                                     int32_t stream_id) {
    for (size_t i = 0; i < s->stream_count; i++) {
        if (s->streams[i].stream_id == stream_id) return &s->streams[i];
    }
    return NULL;
}

static struct h2_stream *get_or_add_stream(cf_front_h2_session *s,
                                           int32_t stream_id) {
    struct h2_stream *st = find_stream(s, stream_id);
    if (st != NULL) return st;
    if (s->stream_count == s->stream_cap) {
        size_t ncap =
            s->stream_cap == 0 ? 16 : s->stream_cap * 2;
        if (ncap > 16384) return NULL;
        struct h2_stream * grown =
            realloc(s->streams, ncap * sizeof *grown);
        if (grown == NULL) return NULL;
        s->streams = grown;
        s->stream_cap = ncap;
    }
    st = &s->streams[s->stream_count++];
    memset(st, 0, sizeof *st);
    st->stream_id = stream_id;
    return st;
}

static void refuse_stream(cf_front_h2_session *s, struct h2_stream *st,
                           uint32_t code) {
    if (st != NULL && !st->counted_refusal) {
        st->counted_refusal = true;
        st->refused = true;
        st->open = false;
        s->refused_total++;
    } else if (st == NULL) {
        s->refused_total++;
    }
    (void)nghttp2_submit_rst_stream(s->ng, NGHTTP2_FLAG_NONE,
                                    st != NULL ? st->stream_id : 0, code);
}

static size_t live_open_count(const cf_front_h2_session *s) {
    size_t n = 0;
    for (size_t i = 0; i < s->stream_count; i++) {
        if (s->streams[i].open) n++;
    }
    return n;
}

/* nghttp2 data-source read: fires only when connection and stream windows
 * allow, so DATA leaves strictly under flow-control allowance. */
static ssize_t response_read(nghttp2_session *session, int32_t stream_id,
                             uint8_t *buf, size_t length, uint32_t *data_flags,
                             nghttp2_data_source *source, void *user_data) {
    (void)session;
    cf_front_h2_session *s = user_data;
    struct h2_stream *st = find_stream(s, stream_id);
    (void)source;
    if (st == NULL || st->resp_body == NULL) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    size_t remaining = st->resp_len - st->resp_off;
    size_t n = remaining < length ? remaining : length;
    if (n != 0) memcpy(buf, st->resp_body + st->resp_off, n);
    st->resp_off += n;
    st->sent += n;
    if (st->queued >= n) {
        st->queued -= n;
    } else {
        st->queued = 0;
    }
    if (s->conn_output_used >= n) s->conn_output_used -= n;
    if (st->resp_off >= st->resp_len) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return (ssize_t)n;
}

/* nghttp2-initiated refusals (e.g. its own MAX_CONCURRENT_STREAMS
 * enforcement) never reach on_frame_recv: no stream object is created and
 * no recv callback fires, but the RST_STREAM goes out on the wire. This
 * send hook attributes those admission denials to refused_total exactly
 * once. Records already counted (our own refuse_stream), admitted streams
 * (sequence != 0: later resets are cancellations, not refusals) and
 * committed streams are never double counted. */
static int on_frame_send(nghttp2_session *session, const nghttp2_frame *frame,
                         void *user_data) {
    (void)session;
    cf_front_h2_session *s = user_data;
    if (frame->hd.type == NGHTTP2_GOAWAY) {
        /* Whoever initiated it (our cf_front_h2_goaway or nghttp2's own
         * enforcement, e.g. RFC 7540 section 5.1.2 connection error when
         * concurrent streams exceed the advertised maximum): accepted
         * streams drain, nothing new is admitted. */
        s->draining = true;
        s->goaway_sent = true;
        return 0;
    }
    if (frame->hd.type != NGHTTP2_RST_STREAM) return 0;
    struct h2_stream *st = find_stream(s, frame->hd.stream_id);
    if (st == NULL) {
        struct h2_stream *tomb =
            get_or_add_stream(s, frame->hd.stream_id);
        if (tomb != NULL && !tomb->counted_refusal) {
            tomb->counted_refusal = true;
            tomb->refused = true;
            s->refused_total++;
        } else if (tomb == NULL) {
            s->refused_total++;
        }
        return 0;
    }
    if (!st->counted_refusal && !st->open && !st->committed &&
        st->sequence == 0) {
        st->counted_refusal = true;
        st->refused = true;
        s->refused_total++;
    }
    return 0;
}

static int on_begin_headers(nghttp2_session *session,
                            const nghttp2_frame *frame, void *user_data) {
    (void)session;
    cf_front_h2_session *s = user_data;
    if (frame->hd.type != NGHTTP2_HEADERS) return 0;
    s->hdr_active = true;
    s->hdr_stream = frame->hd.stream_id;
    s->hdr_count = 0;
    s->hdr_bytes = 0;
    s->hdr_buf_len = 0;
    s->hdr_over = false;
    return 0;
}

static int on_header(nghttp2_session *session, const nghttp2_frame *frame,
                     const uint8_t *name, size_t namelen, const uint8_t *value,
                     size_t valuelen, uint8_t flags, void *user_data) {
    (void)session;
    (void)frame;
    (void)flags;
    cf_front_h2_session *s = user_data;
    if (!s->hdr_active) return 0;
    if (s->hdr_count == SIZE_MAX) return 0;
    s->hdr_count++;
    if (namelen > SIZE_MAX - valuelen ||
        s->hdr_bytes > CF_FRONT_H2_HEADER_MAX ||
        s->hdr_bytes + namelen + valuelen > CF_FRONT_H2_HEADER_MAX) {
        s->hdr_over = true;
        s->hdr_bytes = CF_FRONT_H2_HEADER_MAX + 1;
        return 0;
    }
    s->hdr_bytes += namelen + valuelen;
    if (s->hdr_count > CF_FRONT_H2_HEADER_COUNT_MAX + 1) {
        return 0; /* counted above, not stored */
    }
    size_t need = namelen + valuelen;
    if (need > SIZE_MAX - s->hdr_buf_len ||
        s->hdr_buf_len + need > 48 * 1024) {
        s->hdr_over = true;
        s->hdr_bytes = CF_FRONT_H2_HEADER_MAX + 1;
        return 0;
    }
    if (s->hdr_buf_len + need > s->hdr_buf_cap) {
        size_t ncap = s->hdr_buf_cap == 0 ? 1024 : s->hdr_buf_cap * 2;
        while (ncap < s->hdr_buf_len + need) ncap *= 2;
        if (ncap > 48 * 1024) ncap = 48 * 1024;
        if (ncap < s->hdr_buf_len + need) {
            s->hdr_over = true;
            s->hdr_bytes = CF_FRONT_H2_HEADER_MAX + 1;
            return 0;
        }
        unsigned char *grown = realloc(s->hdr_buf, ncap);
        if (grown == NULL) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        s->hdr_buf = grown;
        s->hdr_buf_cap = ncap;
    }
    size_t idx = s->hdr_count - 1;
    s->hdr_name_off[idx] = s->hdr_buf_len;
    s->hdr_name_len[idx] = namelen;
    if (namelen != 0) {
        memcpy(s->hdr_buf + s->hdr_buf_len, name, namelen);
        s->hdr_buf_len += namelen;
    }
    s->hdr_value_off[idx] = s->hdr_buf_len;
    s->hdr_value_len[idx] = valuelen;
    if (valuelen != 0) {
        memcpy(s->hdr_buf + s->hdr_buf_len, value, valuelen);
        s->hdr_buf_len += valuelen;
    }
    return 0;
}

static int on_data_chunk(nghttp2_session *session, uint8_t flags,
                         int32_t stream_id, const uint8_t *data, size_t len,
                         void *user_data) {
    (void)session;
    (void)flags;
    (void)data;
    cf_front_h2_session *s = user_data;
    struct h2_stream *st = find_stream(s, stream_id);
    if (st == NULL || !st->open || st->refused || st->cancelled) return 0;
    if (st->body_len > CF_FRONT_H2_BODY_MAX - len ||
        st->body_len + len > CF_FRONT_H2_BODY_MAX) {
        /* Body cap (413-class): cancel delivery, keep the connection. */
        st->cancelled = true;
        st->open = false;
        st->validation = CF_FRONT_H2_HEADERS_TOO_LARGE;
        refuse_stream(s, st, NGHTTP2_CANCEL);
        s->resets_total++;
        return 0;
    }
    if (s->conn_input_used > CF_FRONT_H2_CONN_INPUT_MAX - len ||
        s->conn_input_used + len > CF_FRONT_H2_CONN_INPUT_MAX) {
        st->cancelled = true;
        st->open = false;
        refuse_stream(s, st, NGHTTP2_REFUSED_STREAM);
        s->resets_total++;
        return 0;
    }
    st->body_len += len;
    s->conn_input_used += len;
    return 0;
}

static int on_frame_recv(nghttp2_session *session, const nghttp2_frame *frame,
                         void *user_data) {
    (void)session;
    cf_front_h2_session *s = user_data;
    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        int32_t sid = frame->hd.stream_id;
        struct h2_stream *st = get_or_add_stream(s, sid);
        if (st == NULL) {
            (void)nghttp2_submit_rst_stream(s->ng, NGHTTP2_FLAG_NONE, sid,
                                            NGHTTP2_INTERNAL_ERROR);
            s->refused_total++;
            return 0;
        }
        if (s->draining) {
            /* GOAWAY drains accepted streams; nothing new is admitted. */
            refuse_stream(s, st, NGHTTP2_REFUSED_STREAM);
            return 0;
        }
        if (live_open_count(s) >= CF_FRONT_H2_MAX_CONCURRENT_STREAMS) {
            refuse_stream(s, st, NGHTTP2_REFUSED_STREAM);
            return 0;
        }
        if (s->hdr_over || s->hdr_count > CF_FRONT_H2_HEADER_COUNT_MAX) {
            st->validation = s->hdr_count > CF_FRONT_H2_HEADER_COUNT_MAX
                                 ? CF_FRONT_H2_HEADERS_TOO_MANY
                                 : CF_FRONT_H2_HEADERS_TOO_LARGE;
            refuse_stream(s, st, NGHTTP2_ENHANCE_YOUR_CALM);
            return 0;
        }
        size_t ncheck = s->hdr_count;
        const unsigned char *names[CF_FRONT_H2_HEADER_COUNT_MAX + 1];
        size_t name_lens[CF_FRONT_H2_HEADER_COUNT_MAX + 1];
        const unsigned char *values[CF_FRONT_H2_HEADER_COUNT_MAX + 1];
        size_t value_lens[CF_FRONT_H2_HEADER_COUNT_MAX + 1];
        for (size_t i = 0; i < ncheck; i++) {
            names[i] = s->hdr_buf + s->hdr_name_off[i];
            name_lens[i] = s->hdr_name_len[i];
            values[i] = s->hdr_buf + s->hdr_value_off[i];
            value_lens[i] = s->hdr_value_len[i];
        }
        cf_front_h2_headers_result vr = cf_front_h2_check_headers(
            names, name_lens, values, value_lens, ncheck,
            s->public_origin, NULL, NULL);
        st->validation = vr;
        st->header_count = s->hdr_count;
        st->header_bytes = s->hdr_bytes;
        if (vr != CF_FRONT_H2_HEADERS_OK) {
            uint32_t code = (vr == CF_FRONT_H2_HEADERS_TOO_MANY ||
                             vr == CF_FRONT_H2_HEADERS_TOO_LARGE)
                                ? NGHTTP2_ENHANCE_YOUR_CALM
                                : NGHTTP2_PROTOCOL_ERROR;
            refuse_stream(s, st, code);
            return 0;
        }
        if (s->conn_input_used + s->hdr_bytes > CF_FRONT_H2_CONN_INPUT_MAX) {
            refuse_stream(s, st, NGHTTP2_REFUSED_STREAM);
            return 0;
        }
        s->conn_input_used += s->hdr_bytes;
        st->open = true;
        st->sequence = s->next_sequence++;
        s->admitted_total++;
        return 0;
    }
    if (frame->hd.type == NGHTTP2_RST_STREAM) {
        /* Peer reset: mark cancelled; queued (unframed) output is dropped
         * at close, committed bytes already left through mem_send. */
        struct h2_stream *st = find_stream(s, frame->hd.stream_id);
        if (st != NULL && st->open) {
            st->open = false;
            st->cancelled = true;
            s->resets_total++;
        }
        return 0;
    }
    if (frame->hd.type == NGHTTP2_GOAWAY) {
        s->draining = true;
        return 0;
    }
    if (frame->hd.type == NGHTTP2_DATA) {
        struct h2_stream *st = find_stream(s, frame->hd.stream_id);
        if (st != NULL && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
            (void)st;
        }
        return 0;
    }
    return 0;
}

static int on_stream_close(nghttp2_session *session, int32_t stream_id,
                           uint32_t error_code, void *user_data) {
    (void)session;
    cf_front_h2_session *s = user_data;
    struct h2_stream *st = find_stream(s, stream_id);
    if (st == NULL) {
        /* nghttp2-initiated refusal (e.g. its own concurrency enforcement):
         * count once so refused_total covers both layers. */
        if (error_code != NGHTTP2_NO_ERROR) {
            struct h2_stream *tomb = get_or_add_stream(s, stream_id);
            if (tomb != NULL && !tomb->counted_refusal) {
                tomb->counted_refusal = true;
                tomb->refused = true;
                s->refused_total++;
            } else if (tomb == NULL) {
                s->refused_total++;
            }
        }
        return 0;
    }
    if (st->open) {
        st->open = false;
        if (error_code != NGHTTP2_NO_ERROR && !st->committed) {
            st->cancelled = true;
        }
    }
    /* Queued-but-unframed output dies with the stream; committed bytes
     * already left through mem_send and are never recalled. */
    if (st->queued != 0) {
        if (s->conn_output_used >= st->queued) {
            s->conn_output_used -= st->queued;
        } else {
            s->conn_output_used = 0;
        }
        st->queued = 0;
    }
    free(st->resp_body);
    st->resp_body = NULL;
    return 0;
}

cf_err cf_front_h2_session_create(const char *public_origin,
                                   cf_front_h2_session **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (public_origin == NULL || *public_origin == '\0') return CF_INVALID;
    cf_front_h2_session *s = calloc(1, sizeof *s);
    if (s == NULL) return CF_NOMEM;
    s->public_origin = strdup(public_origin);
    if (s->public_origin == NULL) {
        free(s);
        return CF_NOMEM;
    }
    nghttp2_session_callbacks *cbs = NULL;
    if (nghttp2_session_callbacks_new(&cbs) != 0) {
        free(s->public_origin);
        free(s);
        return CF_NOMEM;
    }
    nghttp2_session_callbacks_set_on_begin_headers_callback(cbs,
                                                             on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(cbs, on_header);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, on_frame_recv);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs,
                                                              on_data_chunk);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs,
                                                           on_stream_close);
    nghttp2_session_callbacks_set_on_frame_send_callback(cbs, on_frame_send);
    if (nghttp2_session_server_new(&s->ng, cbs, s) != 0) {
        nghttp2_session_callbacks_del(cbs);
        free(s->public_origin);
        free(s);
        return CF_INTERNAL;
    }
    nghttp2_session_callbacks_del(cbs);
    nghttp2_settings_entry iv;
    iv.settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
    iv.value = CF_FRONT_H2_MAX_CONCURRENT_STREAMS;
    if (nghttp2_submit_settings(s->ng, NGHTTP2_FLAG_NONE, &iv, 1) != 0) {
        nghttp2_session_del(s->ng);
        free(s->public_origin);
        free(s);
        return CF_INTERNAL;
    }
    s->next_sequence = 1;
    *out = s;
    return CF_OK;
}

void cf_front_h2_session_destroy(cf_front_h2_session *session) {
    if (session == NULL) return;
    for (size_t i = 0; i < session->stream_count; i++) {
        free(session->streams[i].resp_body);
    }
    free(session->streams);
    free(session->hdr_buf);
    nghttp2_session_del(session->ng);
    free(session->public_origin);
    free(session);
}

nghttp2_session *cf_front_h2_handle(cf_front_h2_session *session) {
    return session != NULL ? session->ng : NULL;
}

size_t cf_front_h2_open_count(const cf_front_h2_session *session) {
    return session != NULL ? live_open_count(session) : 0;
}

uint64_t cf_front_h2_admitted_total(const cf_front_h2_session *session) {
    return session != NULL ? session->admitted_total : 0;
}

uint64_t cf_front_h2_refused_total(const cf_front_h2_session *session) {
    return session != NULL ? session->refused_total : 0;
}

uint64_t cf_front_h2_resets_total(const cf_front_h2_session *session) {
    return session != NULL ? session->resets_total : 0;
}

bool cf_front_h2_stream_open(const cf_front_h2_session *session,
                              int32_t stream_id) {
    if (session == NULL) return false;
    const struct h2_stream *st = NULL;
    for (size_t i = 0; i < session->stream_count; i++) {
        if (session->streams[i].stream_id == stream_id) {
            st = &session->streams[i];
            break;
        }
    }
    return st != NULL && st->open;
}

bool cf_front_h2_stream_cancelled(const cf_front_h2_session *session,
                                   int32_t stream_id) {
    if (session == NULL) return false;
    for (size_t i = 0; i < session->stream_count; i++) {
        if (session->streams[i].stream_id == stream_id) {
            return session->streams[i].cancelled;
        }
    }
    return false;
}

size_t cf_front_h2_stream_body_len(const cf_front_h2_session *session,
                                    int32_t stream_id) {
    if (session == NULL) return 0;
    for (size_t i = 0; i < session->stream_count; i++) {
        if (session->streams[i].stream_id == stream_id) {
            return session->streams[i].body_len;
        }
    }
    return 0;
}

size_t cf_front_h2_stream_queued(const cf_front_h2_session *session,
                                  int32_t stream_id) {
    if (session == NULL) return 0;
    for (size_t i = 0; i < session->stream_count; i++) {
        if (session->streams[i].stream_id == stream_id) {
            return session->streams[i].queued;
        }
    }
    return 0;
}

size_t cf_front_h2_stream_sent(const cf_front_h2_session *session,
                                int32_t stream_id) {
    if (session == NULL) return 0;
    for (size_t i = 0; i < session->stream_count; i++) {
        if (session->streams[i].stream_id == stream_id) {
            return session->streams[i].sent;
        }
    }
    return 0;
}

bool cf_front_h2_stream_committed(const cf_front_h2_session *session,
                                   int32_t stream_id) {
    if (session == NULL) return false;
    for (size_t i = 0; i < session->stream_count; i++) {
        if (session->streams[i].stream_id == stream_id) {
            return session->streams[i].committed;
        }
    }
    return false;
}

bool cf_front_h2_draining(const cf_front_h2_session *session) {
    return session != NULL && session->draining;
}

bool cf_front_h2_flow_stalled(cf_front_h2_session *session) {
    if (session == NULL || session->ng == NULL) return false;
    /* nghttp2 defers DATA under a zero window and want_write goes quiet,
     * so the stall is defined by bytes without allowance, not by the
     * write flag: queued response bytes remain while the connection or
     * their stream has no remote window. The loop must then wait for
     * WINDOW_UPDATE, never busy-spin mem_send. */
    bool conn_zero =
        nghttp2_session_get_remote_window_size(session->ng) == 0;
    for (size_t i = 0; i < session->stream_count; i++) {
        if (session->streams[i].queued == 0) continue;
        if (conn_zero) return true;
        if (nghttp2_session_get_stream_remote_window_size(
                session->ng, session->streams[i].stream_id) == 0) {
            return true;
        }
    }
    return false;
}

cf_err cf_front_h2_submit_response(cf_front_h2_session *session,
                                    int32_t stream_id, unsigned status,
                                    const unsigned char *body,
                                    size_t body_len) {
    if (session == NULL || session->ng == NULL) return CF_INVALID;
    struct h2_stream *st = find_stream(session, stream_id);
    if (st == NULL || !st->open || st->refused || st->cancelled ||
        st->committed) {
        return CF_INVALID;
    }
    if (status < 100 || status > 999) return CF_INVALID;
    if (body_len != 0 && body == NULL) return CF_INVALID;
    if (body_len > CF_FRONT_H2_QUEUED_MAX) return CF_LIMIT;
    if (session->conn_output_used > CF_FRONT_H2_CONN_OUTPUT_MAX - body_len ||
        session->conn_output_used + body_len >
            CF_FRONT_H2_CONN_OUTPUT_MAX) {
        return CF_LIMIT;
    }
    unsigned char *copy = NULL;
    if (body_len != 0) {
        copy = malloc(body_len == 0 ? 1 : body_len);
        if (copy == NULL) return CF_NOMEM;
        memcpy(copy, body, body_len);
    }
    char status_text[4];
    snprintf(status_text, sizeof status_text, "%03u", status);
    nghttp2_nv nva[2];
    nva[0].name = (uint8_t *)":status";
    nva[0].namelen = 7;
    nva[0].value = (uint8_t *)status_text;
    nva[0].valuelen = 3;
    nva[0].flags = NGHTTP2_NV_FLAG_NONE;
    nva[1].name = (uint8_t *)"content-length";
    nva[1].namelen = 14;
    char length_text[24];
    snprintf(length_text, sizeof length_text, "%zu", body_len);
    nva[1].value = (uint8_t *)length_text;
    nva[1].valuelen = strlen(length_text);
    nva[1].flags = NGHTTP2_NV_FLAG_NONE;
    nghttp2_data_provider prd;
    nghttp2_data_provider *prdp = NULL;
    if (body_len != 0) {
        free(st->resp_body);
        st->resp_body = copy;
        st->resp_len = body_len;
        st->resp_off = 0;
        copy = NULL;
        prd.source.ptr = NULL;
        prd.read_callback = response_read;
        prdp = &prd;
    }
    int rc = nghttp2_submit_response(session->ng, stream_id, nva, 2, prdp);
    free(copy);
    if (rc != 0) {
        free(st->resp_body);
        st->resp_body = NULL;
        st->resp_len = st->resp_off = 0;
        return CF_INTERNAL;
    }
    st->committed = true;
    st->queued += body_len;
    session->conn_output_used += body_len;
    return CF_OK;
}

cf_err cf_front_h2_rst_stream(cf_front_h2_session *session, int32_t stream_id,
                               uint32_t error_code) {
    if (session == NULL || session->ng == NULL) return CF_INVALID;
    struct h2_stream *st = find_stream(session, stream_id);
    if (st == NULL || !st->open) return CF_NOT_FOUND;
    /* Queued-but-unframed output dies here; committed bytes already left
     * through mem_send and are never recalled. */
    if (nghttp2_submit_rst_stream(session->ng, NGHTTP2_FLAG_NONE, stream_id,
                                  error_code) != 0) {
        return CF_INTERNAL;
    }
    st->open = false;
    st->cancelled = true;
    session->resets_total++;
    return CF_OK;
}

cf_err cf_front_h2_goaway(cf_front_h2_session *session) {
    if (session == NULL || session->ng == NULL) return CF_INVALID;
    if (!session->goaway_sent) {
        int32_t last = 0;
        for (size_t i = 0; i < session->stream_count; i++) {
            if (session->streams[i].stream_id > last) {
                last = session->streams[i].stream_id;
            }
        }
        if (nghttp2_submit_goaway(session->ng, NGHTTP2_FLAG_NONE, last,
                                  NGHTTP2_NO_ERROR, NULL, 0) != 0) {
            return CF_INTERNAL;
        }
        session->goaway_sent = true;
    }
    session->draining = true;
    return CF_OK;
}
