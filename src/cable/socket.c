/* src/cable/socket.c — task C01: the WebSocket transport of the /cable front
 * mount (04-cable-jobs.md C01; socket.rs, server.rs and the Authenticate
 * trait of tmp/rust-ref/crates/cable/src).
 *
 * One owner thread per accepted connection (the reference's one task per
 * socket): no other thread sends, closes or mutates it. Writers may enqueue
 * frames from anywhere (mutex + eventfd). The reader is the translation of
 * Reader::next: masked client frames, variable lengths, fragmentation,
 * control frames interleaved with fragments, UTF-8 text validation, the
 * close handshake, and the reference close codes. permessage-deflate is
 * negotiated without context takeover; the writer sends the reference raw
 * deflate framing (RFC 7692) and the same FrameDeflate cache as socket.rs.
 */
#include "cable.h"

#include "auth.h"
#include "models/session.h"
#include "models/user.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

/* ---- socket.rs constants -------------------------------------------------- */

#define OP_CONTINUATION 0x0
#define OP_TEXT 0x1
#define OP_BINARY 0x2
#define OP_CLOSE 0x8
#define OP_PING 0x9
#define OP_PONG 0xA

/* RFC 6455's key suffix. */
#define ACCEPT_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
/* RFC 7692 leaves these off the wire; inflate appends them. */
static const unsigned char DEFLATE_TAIL[4] = {0x00, 0x00, 0xff, 0xff};

/* One recv() unit. The parser consumes payloads as they arrive, so the
 * persistent input buffer never holds more than this plus a frame header. */
#define CF_CABLE_READ_CHUNK ((size_t)16384)

/* ------------------------------------------------------------ small helpers */

static cf_span span_trim(cf_span s) {
    while (s.len != 0 && (s.ptr[0] == ' ' || s.ptr[0] == '\t')) {
        s.ptr++;
        s.len--;
    }
    while (s.len != 0 && (s.ptr[s.len - 1] == ' ' || s.ptr[s.len - 1] == '\t')) {
        s.len--;
    }
    return s;
}

static bool bytes_eq_lit(cf_span s, const char *lit) {
    size_t n = strlen(lit);
    return s.len == n && memcmp(s.ptr, lit, n) == 0;
}

/* Case-insensitive compare against an ASCII literal. */
static bool bytes_ieq_lit(cf_span s, const char *lit) {
    size_t n = strlen(lit);
    if (s.len != n) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char a = s.ptr[i];
        unsigned char b = (unsigned char)lit[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

/* Strict UTF-8 (RFC 3629): no overlongs, surrogates or values > U+10FFFF. */
static bool cable_utf8_valid(const unsigned char *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = p[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        unsigned need;
        unsigned min;
        unsigned cp;
        if ((c & 0xE0) == 0xC0) {
            need = 1;
            min = 0x80;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            need = 2;
            min = 0x800;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            need = 3;
            min = 0x10000;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (n - i - 1 < need) return false;
        for (unsigned k = 0; k < need; k++) {
            unsigned char cc = p[i + 1 + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (unsigned)(cc & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += (size_t)need + 1;
    }
    return true;
}

static int b64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Rust base64 engine::general_purpose::STANDARD: standard alphabet,
 * canonical padding, trailing bits must be zero. */
static bool base64_standard_decode(cf_span in, unsigned char *out,
                                   size_t out_cap, size_t *out_len) {
    if (in.len == 0 || in.len % 4 != 0) return false;
    size_t groups = in.len / 4;
    size_t pad = 0;
    if (in.ptr[in.len - 1] == '=') pad++;
    if (pad != 0 && in.len >= 2 && in.ptr[in.len - 2] == '=') pad++;
    size_t olen = groups * 3 - pad;
    if (olen > out_cap) return false;
    size_t o = 0;
    for (size_t g = 0; g < groups; g++) {
        const unsigned char *q = in.ptr + g * 4;
        bool last = g + 1 == groups;
        int v0 = b64_value(q[0]);
        int v1 = b64_value(q[1]);
        if (v0 < 0 || v1 < 0) return false;
        int v2 = 0, v3 = 0;
        if (last && pad == 2) {
            if (q[2] != '=' || q[3] != '=') return false;
            if ((v1 & 0x0F) != 0) return false; /* canonical trailing bits */
        } else if (last && pad == 1) {
            v2 = b64_value(q[2]);
            if (v2 < 0 || q[3] != '=') return false;
            if ((v2 & 0x03) != 0) return false;
        } else {
            v2 = b64_value(q[2]);
            v3 = b64_value(q[3]);
            if (v2 < 0 || v3 < 0) return false;
        }
        if (o < olen) out[o++] = (unsigned char)((v0 << 2) | (v1 >> 4));
        if (o < olen) out[o++] = (unsigned char)((v1 << 4) | (v2 >> 2));
        if (o < olen) out[o++] = (unsigned char)((v2 << 6) | v3);
    }
    *out_len = olen;
    return true;
}

static void base64_standard_encode(const unsigned char *in, size_t n,
                                   char *out, size_t out_cap) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= n && o + 4 < out_cap) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) |
                     (unsigned)in[i + 2];
        out[o++] = alphabet[(v >> 18) & 63];
        out[o++] = alphabet[(v >> 12) & 63];
        out[o++] = alphabet[(v >> 6) & 63];
        out[o++] = alphabet[v & 63];
        i += 3;
    }
    if (o + 4 < out_cap) {
        if (n - i == 1) {
            unsigned v = (unsigned)in[i] << 16;
            out[o++] = alphabet[(v >> 18) & 63];
            out[o++] = alphabet[(v >> 12) & 63];
            out[o++] = '=';
            out[o++] = '=';
        } else if (n - i == 2) {
            unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8);
            out[o++] = alphabet[(v >> 18) & 63];
            out[o++] = alphabet[(v >> 12) & 63];
            out[o++] = alphabet[(v >> 6) & 63];
            out[o++] = '=';
        }
    }
    out[o] = '\0';
}

static bool cable_sha1(cf_span data, unsigned char out[20]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx == NULL) return false;
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha1(), NULL) == 1 &&
              EVP_DigestUpdate(ctx, data.ptr, data.len) == 1;
    unsigned int len = 0;
    if (ok) ok = EVP_DigestFinal_ex(ctx, out, &len) == 1 && len == 20;
    EVP_MD_CTX_free(ctx);
    return ok;
}

/* ---- request header helpers ----------------------------------------------- */

static bool request_header_first(const cf_cable_request *request,
                                 const char *name, cf_span *out) {
    for (size_t i = 0; i < request->header_count; i++) {
        if (bytes_ieq_lit(request->headers[i].name, name)) {
            *out = request->headers[i].value;
            return true;
        }
    }
    return false;
}

/* websocket.rs/Driver: a GET whose Connection list contains "upgrade" and
 * whose Upgrade is "websocket". */
static bool websocket_request(const cf_cable_request *request) {
    if (request->method != CF_GET) return false;
    cf_span upgrade;
    if (!request_header_first(request, "upgrade", &upgrade) ||
        !bytes_ieq_lit(span_trim(upgrade), "websocket")) {
        return false;
    }
    bool connection_upgrade = false;
    for (size_t i = 0; i < request->header_count && !connection_upgrade; i++) {
        if (!bytes_ieq_lit(request->headers[i].name, "connection")) continue;
        cf_span v = request->headers[i].value;
        size_t pos = 0;
        while (pos <= v.len) {
            size_t end = pos;
            while (end < v.len && v.ptr[end] != ',') end++;
            if (bytes_ieq_lit(span_trim((cf_span){v.ptr + pos, end - pos}),
                              "upgrade")) {
                connection_upgrade = true;
                break;
            }
            pos = end + 1;
        }
    }
    return connection_upgrade;
}

/* ---- protocol negotiation (server.rs) ------------------------------------- */

/* `v.parse::<u8>()` for client_max_window_bits: unsigned digits only. */
static bool parse_u8_decimal(cf_span v, unsigned *out) {
    if (v.len == 0 || v.len > 3) return false;
    unsigned n = 0;
    for (size_t i = 0; i < v.len; i++) {
        if (v.ptr[i] < '0' || v.ptr[i] > '9') return false;
        n = n * 10 + (unsigned)(v.ptr[i] - '0');
    }
    *out = n;
    return true;
}

/* acceptable_deflate_offer: any parameters but a server window smaller than
 * zlib's 15 bits. */
static bool deflate_offer_ok(cf_span offer) {
    size_t pos = 0;
    bool first = true;
    while (pos <= offer.len) {
        size_t end = pos;
        while (end < offer.len && offer.ptr[end] != ';') end++;
        cf_span part = span_trim((cf_span){offer.ptr + pos, end - pos});
        if (first) {
            if (!bytes_eq_lit(part, "permessage-deflate")) return false;
            first = false;
        } else {
            const unsigned char *eq = memchr(part.ptr, '=', part.len);
            cf_span name;
            bool has_value = false;
            cf_span value = {NULL, 0};
            if (eq != NULL) {
                name = span_trim((cf_span){part.ptr, (size_t)(eq - part.ptr)});
                value = span_trim((cf_span){eq + 1,
                                            part.len - (size_t)(eq - part.ptr) - 1});
                while (value.len != 0 && value.ptr[0] == '"') {
                    value.ptr++;
                    value.len--;
                }
                while (value.len != 0 && value.ptr[value.len - 1] == '"') {
                    value.len--;
                }
                has_value = true;
            } else {
                name = part;
            }
            if (bytes_eq_lit(name, "server_no_context_takeover") ||
                bytes_eq_lit(name, "client_no_context_takeover")) {
                if (has_value) return false;
            } else if (bytes_eq_lit(name, "client_max_window_bits")) {
                if (has_value) {
                    unsigned bits;
                    if (!parse_u8_decimal(value, &bits) || bits < 8 ||
                        bits > 15) {
                        return false;
                    }
                }
            } else if (bytes_eq_lit(name, "server_max_window_bits")) {
                if (!has_value || !bytes_eq_lit(value, "15")) return false;
            } else {
                return false;
            }
        }
        pos = end + 1;
    }
    return true;
}

static bool deflate_acceptable(const cf_cable_request *request) {
    for (size_t i = 0; i < request->header_count; i++) {
        if (!bytes_ieq_lit(request->headers[i].name,
                           "sec-websocket-extensions")) {
            continue;
        }
        cf_span v = request->headers[i].value;
        size_t pos = 0;
        while (pos <= v.len) {
            size_t end = pos;
            while (end < v.len && v.ptr[end] != ',') end++;
            if (deflate_offer_ok(
                    span_trim((cf_span){v.ptr + pos, end - pos}))) {
                return true;
            }
            pos = end + 1;
        }
    }
    return false;
}

/* The first protocol in the client's list that Action Cable supports. */
static const char *negotiate_protocol(const cf_cable_request *request) {
    for (size_t i = 0; i < request->header_count; i++) {
        if (!bytes_ieq_lit(request->headers[i].name,
                           "sec-websocket-protocol")) {
            continue;
        }
        cf_span v = request->headers[i].value;
        size_t pos = 0;
        while (pos <= v.len) {
            size_t end = pos;
            while (end < v.len && v.ptr[end] != ',') end++;
            cf_span tok = span_trim((cf_span){v.ptr + pos, end - pos});
            if (bytes_eq_lit(tok, CF_CABLE_SUBPROTOCOL_V1)) {
                return CF_CABLE_SUBPROTOCOL_V1;
            }
            if (bytes_eq_lit(tok, CF_CABLE_SUBPROTOCOL_UNSUPPORTED)) {
                return CF_CABLE_SUBPROTOCOL_UNSUPPORTED;
            }
            pos = end + 1;
        }
    }
    return NULL;
}

typedef struct {
    char accept[32];
    bool deflate;
} cable_handshake;

/* Handshake::accept: version 13 and a 16-byte key. */
static bool handshake_accept(const cf_cable_request *request,
                             cable_handshake *out) {
    cf_span version;
    cf_span key;
    if (!request_header_first(request, "sec-websocket-version", &version) ||
        !bytes_eq_lit(span_trim(version), "13")) {
        return false;
    }
    if (!request_header_first(request, "sec-websocket-key", &key)) return false;
    unsigned char decoded[32];
    size_t dlen = 0;
    if (!base64_standard_decode(span_trim(key), decoded, sizeof decoded,
                                &dlen) ||
        dlen != 16) {
        return false;
    }
    cf_builder sha_input = {0};
    cf_err rc = cf_builder_append(&sha_input, span_trim(key));
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &sha_input,
            (cf_span){(const unsigned char *)ACCEPT_GUID,
                      sizeof ACCEPT_GUID - 1});
    }
    unsigned char digest[20];
    bool ok = rc == CF_OK &&
              cable_sha1((cf_span){sha_input.ptr, sha_input.len}, digest);
    cf_builder_dispose(&sha_input);
    if (!ok) return false;
    base64_standard_encode(digest, sizeof digest, out->accept,
                           sizeof out->accept);
    out->deflate = deflate_acceptable(request);
    return true;
}

/* ---- RFC 6455 close codes (socket.rs) ------------------------------------- */

static bool close_payload_valid(cf_span payload, bool *has_code,
                                uint16_t *code) {
    *has_code = false;
    if (payload.len == 0) return true;
    if (payload.len == 1) return false;
    uint16_t c = (uint16_t)(((uint16_t)payload.ptr[0] << 8) |
                            (uint16_t)payload.ptr[1]);
    if (!((c >= 1000 && c <= 1003) || (c >= 1007 && c <= 1014) ||
          (c >= 3000 && c <= 4999))) {
        return false;
    }
    if (!cable_utf8_valid(payload.ptr + 2, payload.len - 2)) return false;
    *has_code = true;
    *code = c;
    return true;
}

/* ---- zlib: raw deflate + RFC 7692 framing (socket.rs) --------------------- */

static cf_err cable_deflate(cf_span input, cf_buf **out) {
    *out = NULL;
    z_stream z;
    memset(&z, 0, sizeof z);
    if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return CF_INTERNAL;
    }
    cf_builder built = {0};
    unsigned char chunk[CF_CABLE_READ_CHUNK];
    size_t in_off = 0;
    cf_err rc = CF_OK;
    for (;;) {
        z.next_in = (Bytef *)(uintptr_t)(input.ptr + in_off);
        z.avail_in = (uInt)(input.len - in_off);
        z.next_out = chunk;
        z.avail_out = (uInt)sizeof chunk;
        int zr = deflate(&z, Z_SYNC_FLUSH);
        if (zr != Z_OK) {
            rc = CF_INTERNAL;
            break;
        }
        in_off = input.len - z.avail_in;
        size_t produced = sizeof chunk - z.avail_out;
        if (produced != 0 &&
            cf_builder_append(&built, (cf_span){chunk, produced}) != CF_OK) {
            rc = CF_NOMEM;
            break;
        }
        if (in_off == input.len && z.avail_out != 0) break;
    }
    deflateEnd(&z);
    if (rc != CF_OK) {
        cf_builder_dispose(&built);
        return rc;
    }
    /* RFC 7692 drops the trailing empty block. */
    if (built.len >= 4 &&
        memcmp(built.ptr + built.len - 4, DEFLATE_TAIL, 4) == 0) {
        built.len -= 4;
    }
    rc = cf_builder_freeze(&built, out);
    if (rc != CF_OK) cf_builder_dispose(&built);
    return rc;
}

/* Inflate one message, checking the 1 MiB cap incrementally. CF_LIMIT means
 * the expansion passed the cap; CF_INVALID means invalid deflate data. */
static cf_err cable_inflate(cf_span input, size_t max_out, cf_builder *out) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit2(&z, -15) != Z_OK) return CF_INTERNAL;

    cf_builder framed = {0};
    cf_err rc = cf_builder_append(&framed, input);
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &framed, (cf_span){(const unsigned char *)DEFLATE_TAIL, 4});
    }
    if (rc != CF_OK) {
        cf_builder_dispose(&framed);
        inflateEnd(&z);
        return rc;
    }

    z.next_in = framed.ptr;
    z.avail_in = (uInt)framed.len;
    unsigned char chunk[CF_CABLE_READ_CHUNK];
    rc = CF_OK;
    for (;;) {
        z.next_out = chunk;
        z.avail_out = (uInt)sizeof chunk;
        int zr = inflate(&z, Z_NO_FLUSH);
        size_t produced = sizeof chunk - z.avail_out;
        if (produced != 0) {
            if (produced > max_out - (out->len < max_out ? out->len : max_out)) {
                rc = CF_LIMIT;
                break;
            }
            if (cf_builder_append(out, (cf_span){chunk, produced}) != CF_OK) {
                rc = CF_NOMEM;
                break;
            }
            if (out->len > max_out) {
                rc = CF_LIMIT;
                break;
            }
        }
        if (zr == Z_STREAM_END) break;
        if (zr != Z_OK && zr != Z_BUF_ERROR) {
            rc = CF_INVALID;
            break;
        }
        if (z.avail_in == 0 && produced < sizeof chunk) break;
        if (zr == Z_BUF_ERROR && produced == 0) {
            rc = CF_INVALID;
            break;
        }
    }
    inflateEnd(&z);
    cf_builder_dispose(&framed);
    return rc;
}

/* ---- outbound frames ------------------------------------------------------ */

struct cf_cable_frame {
    atomic_size_t refs;
    cf_buf *text;
    pthread_mutex_t mutex;
    cf_buf *deflated;
    bool deflate_done;
};

cf_err cf_cable_frame_create(cf_span text, cf_cable_frame **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    cf_cable_frame *frame = calloc(1, sizeof *frame);
    if (frame == NULL) return CF_NOMEM;
    if (cf_buf_copy(text, &frame->text) != CF_OK) {
        free(frame);
        return CF_NOMEM;
    }
    atomic_init(&frame->refs, (size_t)1);
    if (pthread_mutex_init(&frame->mutex, NULL) != 0) {
        cf_buf_release(frame->text);
        free(frame);
        return CF_INTERNAL;
    }
    *out = frame;
    return CF_OK;
}

cf_cable_frame *cf_cable_frame_retain(cf_cable_frame *frame) {
    if (frame == NULL) return NULL;
    atomic_fetch_add_explicit(&frame->refs, (size_t)1, memory_order_relaxed);
    return frame;
}

void cf_cable_frame_release(cf_cable_frame *frame) {
    if (frame == NULL) return;
    if (atomic_fetch_sub_explicit(&frame->refs, (size_t)1,
                                  memory_order_acq_rel) != 1) {
        return;
    }
    cf_buf_release(frame->text);
    cf_buf_release(frame->deflated);
    pthread_mutex_destroy(&frame->mutex);
    free(frame);
}

/* The deflated variant, computed at most once and shared by every socket
 * that sends this frame (socket.rs Frame::deflated). */
static cf_buf *frame_deflated(cf_cable_frame *frame) {
    cf_span text = cf_buf_span(frame->text);
    if (text.len < CF_CABLE_MIN_COMPRESSED) return NULL;
    pthread_mutex_lock(&frame->mutex);
    if (!frame->deflate_done) {
        frame->deflate_done = true;
        cf_buf *deflated = NULL;
        if (cable_deflate(text, &deflated) == CF_OK) {
            frame->deflated = deflated;
        }
    }
    cf_buf *result =
        frame->deflated != NULL ? cf_buf_retain(frame->deflated) : NULL;
    pthread_mutex_unlock(&frame->mutex);
    return result;
}

/* One queued wire frame: its header bytes and a retained payload, sent in
 * order with a per-frame cursor. */
struct cf_cable_out {
    struct cf_cable_out *next;
    cf_buf *payload;
    unsigned char header[10];
    size_t header_len;
    size_t payload_len;
    size_t sent; /* bytes of header+payload accepted by the kernel */
    unsigned opcode;
};

struct cf_cable_socket {
    int fd;
    bool deflate;
    int wake_fd;
    pthread_mutex_t mutex; /* guards the out queue and the flags below */
    struct cf_cable_out *out_head, *out_tail;
    size_t pending_bytes; /* queued wire bytes not yet accepted */
    bool queue_cap_hit;
    bool closed;          /* run() finished; senders must not enqueue */
    uint64_t last_progress_ms;
    cf_cable_limits limits;
    cf_cable_hooks hooks;
    cf_cable_socket_stats stats;

    /* reader state (owner thread only) */
    cf_builder in;
    size_t in_pos;
    int read_state;
    unsigned char b0, b1;
    uint64_t frame_len;
    unsigned char mask[4];
    bool fin, compressed;
    unsigned opcode;
    uint64_t payload_have;
    unsigned char control[125];
    size_t control_len;
    cf_builder msg;
    bool msg_open;
    unsigned msg_opcode;
    bool msg_compressed;
    /* A continuation with no message open. The reference detects it only after
     * the frame's payload has been read (socket.rs Reader::next), so the
     * header's mask/length checks decide the close code first. */
    bool orphan_continuation;

    /* loop state (owner thread only) */
    bool closing;
    bool wait_peer_close;
    bool finished;
    bool transport_failed;
    uint64_t close_deadline_ms;
    uint64_t next_beat_ms;
};

static void socket_wake(struct cf_cable_socket *s) {
    uint64_t one = 1;
    ssize_t n = write(s->wake_fd, &one, sizeof one);
    (void)n; /* EAGAIN: a wake is already pending */
}

static void socket_drain_wake(struct cf_cable_socket *s) {
    uint64_t v;
    while (read(s->wake_fd, &v, sizeof v) > 0) {
    }
}

static void frame_header(unsigned opcode, bool compressed, size_t len,
                         unsigned char out[10], size_t *out_len) {
    out[0] = (unsigned char)(0x80 | (compressed ? 0x40 : 0) | opcode);
    if (len < 126) {
        out[1] = (unsigned char)len;
        *out_len = 2;
    } else if (len <= 0xFFFF) {
        out[1] = 126;
        out[2] = (unsigned char)(len >> 8);
        out[3] = (unsigned char)(len & 0xFF);
        *out_len = 4;
    } else {
        out[1] = 127;
        for (int i = 0; i < 8; i++) {
            out[2 + i] = (unsigned char)((uint64_t)len >> (56 - 8 * i));
        }
        *out_len = 10;
    }
}

/* Append one frame to the pending queue. Takes ownership of `payload` (a
 * retained reference) on every path. Returns CF_BUSY when the pending cap is
 * reached (the connection closes) and CF_IO when it already closed. */
static cf_err out_queue(struct cf_cable_socket *s, unsigned opcode,
                        cf_buf *payload, bool compressed) {
    size_t len = cf_buf_span(payload).len;
    unsigned char header[10];
    size_t header_len = 0;
    frame_header(opcode, compressed, len, header, &header_len);
    size_t wire = header_len + len;

    pthread_mutex_lock(&s->mutex);
    if (s->closed || s->queue_cap_hit) {
        pthread_mutex_unlock(&s->mutex);
        cf_buf_release(payload);
        return CF_IO;
    }
    if (wire > s->limits.max_pending_bytes ||
        s->pending_bytes > s->limits.max_pending_bytes - wire) {
        s->queue_cap_hit = true;
        s->stats.queue_cap_closes++;
        pthread_mutex_unlock(&s->mutex);
        cf_buf_release(payload);
        socket_wake(s);
        return CF_BUSY;
    }
    struct cf_cable_out *node = calloc(1, sizeof *node);
    if (node == NULL) {
        pthread_mutex_unlock(&s->mutex);
        cf_buf_release(payload);
        return CF_NOMEM;
    }
    node->payload = payload;
    memcpy(node->header, header, header_len);
    node->header_len = header_len;
    node->payload_len = len;
    node->opcode = opcode;
    if (s->out_tail != NULL) {
        s->out_tail->next = node;
    } else {
        s->out_head = node;
    }
    s->out_tail = node;
    if (s->pending_bytes == 0) s->last_progress_ms = cf_monotonic_ms();
    s->pending_bytes += wire;
    s->stats.frames_queued++;
    pthread_mutex_unlock(&s->mutex);
    socket_wake(s);
    return CF_OK;
}

static cf_err queue_text_bytes(struct cf_cable_socket *s, cf_span text) {
    cf_buf *payload = NULL;
    cf_err rc = cf_buf_copy(text, &payload);
    if (rc != CF_OK) return rc;
    return out_queue(s, OP_TEXT, payload, false);
}

static cf_err queue_control(struct cf_cable_socket *s, unsigned opcode,
                            cf_span payload) {
    cf_buf *buf = NULL;
    if (cf_buf_copy(payload, &buf) != CF_OK) return CF_NOMEM;
    return out_queue(s, opcode, buf, false);
}

static void node_finish_stats(struct cf_cable_socket *s,
                              struct cf_cable_out *node) {
    s->stats.frames_sent++;
    s->stats.bytes_sent += node->header_len + node->payload_len;
    if (node->opcode == OP_PING) s->stats.pings_sent++;
    if (node->opcode == OP_PONG) s->stats.pongs_sent++;
    if (node->opcode == OP_CLOSE) s->stats.close_frames_sent++;
}

/* Send as much of the queue as the socket accepts; called on the owner
 * thread. A positive send resets the stalled-write clock. */
static void socket_flush(struct cf_cable_socket *s) {
    while (!s->transport_failed) {
        pthread_mutex_lock(&s->mutex);
        struct cf_cable_out *node = s->out_head;
        if (node != NULL) {
            s->out_head = node->next;
            if (s->out_head == NULL) s->out_tail = NULL;
        }
        pthread_mutex_unlock(&s->mutex);
        if (node == NULL) return;

        ssize_t n;
        if (node->sent < node->header_len) {
            n = send(s->fd, node->header + node->sent,
                     node->header_len - node->sent, MSG_NOSIGNAL);
        } else {
            size_t done = node->sent - node->header_len;
            const unsigned char *p = cf_buf_span(node->payload).ptr + done;
            n = send(s->fd, p, node->payload_len - done, MSG_NOSIGNAL);
        }

        if (n > 0) {
            node->sent += (size_t)n;
            uint64_t now = cf_monotonic_ms();
            s->last_progress_ms = now;
            pthread_mutex_lock(&s->mutex);
            s->pending_bytes -= (size_t)n;
            pthread_mutex_unlock(&s->mutex);
            if (node->sent == node->header_len + node->payload_len) {
                node_finish_stats(s, node);
                cf_buf_release(node->payload);
                free(node);
            } else {
                pthread_mutex_lock(&s->mutex);
                node->next = s->out_head;
                s->out_head = node;
                if (s->out_tail == NULL) s->out_tail = node;
                pthread_mutex_unlock(&s->mutex);
                return; /* kernel buffer full */
            }
            continue;
        }
        if (n < 0 && errno == EINTR) {
            pthread_mutex_lock(&s->mutex);
            node->next = s->out_head;
            s->out_head = node;
            if (s->out_tail == NULL) s->out_tail = node;
            pthread_mutex_unlock(&s->mutex);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pthread_mutex_lock(&s->mutex);
            node->next = s->out_head;
            s->out_head = node;
            if (s->out_tail == NULL) s->out_tail = node;
            pthread_mutex_unlock(&s->mutex);
            return;
        }
        /* Transport failure: the remaining queue is dropped. */
        cf_buf_release(node->payload);
        free(node);
        s->transport_failed = true;
        return;
    }
}

cf_err cf_cable_socket_send_frame(struct cf_cable_socket *socket,
                                  cf_cable_frame *frame) {
    if (socket == NULL || frame == NULL) return CF_INVALID;
    if (socket->deflate &&
        cf_buf_span(frame->text).len >= CF_CABLE_MIN_COMPRESSED) {
        cf_buf *deflated = frame_deflated(frame);
        if (deflated != NULL) {
            return out_queue(socket, OP_TEXT, deflated, true);
        }
        /* Compression is not worth failing a message over; the frame goes
         * uncompressed and the attempt is counted. */
        socket->stats.deflate_failures++;
    }
    return out_queue(socket, OP_TEXT, cf_buf_retain(frame->text), false);
}

cf_err cf_cable_socket_send_text(cf_cable_socket *socket, cf_span text) {
    if (socket == NULL) return CF_INVALID;
    cf_cable_frame *frame = NULL;
    cf_err rc = cf_cable_frame_create(text, &frame);
    if (rc != CF_OK) return rc;
    rc = cf_cable_socket_send_frame(socket, frame);
    cf_cable_frame_release(frame);
    return rc;
}

/* ---- reader (socket.rs Reader::next) -------------------------------------- */

enum { R_B0 = 0, R_LEN16, R_LEN64, R_MASK, R_PAYLOAD };

#define READ_AVAIL(s) ((s)->in.len - (s)->in_pos)
#define READ_PTR(s) ((s)->in.ptr + (s)->in_pos)

static void in_take(struct cf_cable_socket *s, size_t n) {
    s->in_pos += n;
    if (s->in_pos == s->in.len) {
        s->in_pos = 0;
        s->in.len = 0;
    } else if (s->in_pos >= 4096) {
        memmove(s->in.ptr, s->in.ptr + s->in_pos, s->in.len - s->in_pos);
        s->in.len -= s->in_pos;
        s->in_pos = 0;
    }
}

enum feed_result { FEED_OK = 0, FEED_PROTOCOL, FEED_CLOSED, FEED_ABORT };

static enum feed_result protocol_fail(struct cf_cable_socket *s,
                                      uint16_t code) {
    s->stats.protocol_closes++;
    s->closing = true;
    s->wait_peer_close = false;
    unsigned char payload[2] = {(unsigned char)(code >> 8),
                                (unsigned char)(code & 0xFF)};
    (void)queue_control(s, OP_CLOSE, (cf_span){payload, 2});
    return FEED_PROTOCOL;
}

static enum feed_result message_done(struct cf_cable_socket *s,
                                     const unsigned char *data, size_t len,
                                     unsigned opcode, bool compressed) {
    cf_builder inflated = {0};
    cf_err rc = CF_OK;
    if (compressed) {
        rc = cable_inflate((cf_span){data, len},
                           s->limits.max_message_bytes, &inflated);
        if (rc == CF_LIMIT) {
            cf_builder_dispose(&inflated);
            return protocol_fail(s, CF_CABLE_TOO_LARGE);
        }
        if (rc != CF_OK) {
            cf_builder_dispose(&inflated);
            return protocol_fail(s, CF_CABLE_PROTOCOL_ERROR);
        }
        data = inflated.ptr;
        len = inflated.len;
    }
    enum feed_result result = FEED_OK;
    if (opcode == OP_TEXT) {
        if (!cable_utf8_valid(data, len)) {
            cf_builder_dispose(&inflated);
            return protocol_fail(s, CF_CABLE_ENCODING_ERROR);
        }
        s->stats.messages_received++;
        if (s->hooks.on_text != NULL) {
            (void)s->hooks.on_text(s->hooks.on_text_user, s, (cf_span){data,
                                                                      len});
        }
    } else {
        /* The reference logs and ignores a binary message. */
        s->stats.binary_messages++;
    }
    cf_builder_dispose(&inflated);
    return result;
}

static bool msg_append(struct cf_cable_socket *s, const unsigned char *p,
                       size_t n) {
    if (n == 0) return true;
    unsigned char tmp[1024];
    size_t off = 0;
    while (off < n) {
        size_t chunk = n - off;
        if (chunk > sizeof tmp) chunk = sizeof tmp;
        for (size_t j = 0; j < chunk; j++) {
            uint64_t idx = s->payload_have + off + j;
            tmp[j] = (unsigned char)(p[off + j] ^ s->mask[idx & 3]);
        }
        if (cf_builder_append(&s->msg, (cf_span){tmp, chunk}) != CF_OK) {
            return false;
        }
        off += chunk;
    }
    return true;
}

static void control_append(struct cf_cable_socket *s,
                           const unsigned char *p, size_t n) {
    for (size_t j = 0; j < n; j++) {
        uint64_t idx = s->payload_have + j;
        s->control[s->control_len++] =
            (unsigned char)(p[j] ^ s->mask[idx & 3]);
    }
}

/* Consume complete frames from the input buffer. */
static enum feed_result reader_feed(struct cf_cable_socket *s) {
    for (;;) {
        switch (s->read_state) {
        case R_B0: {
            if (READ_AVAIL(s) < 2) return FEED_OK;
            const unsigned char *p = READ_PTR(s);
            s->b0 = p[0];
            s->b1 = p[1];
            s->fin = (s->b0 & 0x80) != 0;
            s->compressed = (s->b0 & 0x40) != 0;
            s->opcode = (unsigned)(s->b0 & 0x0F);
            bool control = s->opcode >= 0x8;
            bool known = s->opcode == OP_CONTINUATION ||
                         s->opcode == OP_TEXT || s->opcode == OP_BINARY ||
                         s->opcode == OP_CLOSE || s->opcode == OP_PING ||
                         s->opcode == OP_PONG;
            bool reserved =
                (s->b0 & 0x30) != 0 ||
                (s->compressed &&
                 !(s->deflate && (s->opcode == OP_TEXT ||
                                  s->opcode == OP_BINARY)));
            bool interrupts = (s->opcode == OP_TEXT ||
                               s->opcode == OP_BINARY) &&
                              s->msg_open;
            /* The reference rejects a continuation of nothing once the
             * payload has been read, after its earlier header checks have had
             * their say (socket.rs Reader::{header,next}); record the frame
             * now and fail at frame completion instead. */
            bool orphan_continuation =
                s->opcode == OP_CONTINUATION && !s->msg_open;
            s->orphan_continuation = orphan_continuation;
            if (reserved || !known || (control && !s->fin) || interrupts) {
                return protocol_fail(s, CF_CABLE_PROTOCOL_ERROR);
            }
            if ((s->b1 & 0x80) == 0) {
                return protocol_fail(s, CF_CABLE_UNACCEPTABLE);
            }
            if (!control && (s->opcode == OP_TEXT || s->opcode == OP_BINARY)) {
                /* Start the new message before its payload accumulates. */
                s->msg.len = 0;
                s->msg_open = true;
                s->msg_opcode = s->opcode;
                s->msg_compressed = s->compressed;
            }
            unsigned len7 = s->b1 & 0x7F;
            in_take(s, 2);
            if (len7 == 126) {
                s->read_state = R_LEN16;
                break;
            }
            if (len7 == 127) {
                s->read_state = R_LEN64;
                break;
            }
            s->frame_len = len7;
            goto length_known;
        }
        case R_LEN16: {
            if (READ_AVAIL(s) < 2) return FEED_OK;
            const unsigned char *p = READ_PTR(s);
            s->frame_len =
                ((uint64_t)p[0] << 8) | (uint64_t)p[1];
            in_take(s, 2);
            goto length_known;
        }
        case R_LEN64: {
            if (READ_AVAIL(s) < 8) return FEED_OK;
            const unsigned char *p = READ_PTR(s);
            s->frame_len = 0;
            for (int i = 0; i < 8; i++) {
                s->frame_len = (s->frame_len << 8) | (uint64_t)p[i];
            }
            in_take(s, 8);
            goto length_known;
        }
        length_known: {
            bool control = s->opcode >= 0x8;
            if (control && s->frame_len > 125) {
                return protocol_fail(s, CF_CABLE_PROTOCOL_ERROR);
            }
            uint64_t so_far = (s->msg_open && !control)
                                  ? (uint64_t)s->msg.len
                                  : 0;
            if (so_far > (uint64_t)s->limits.max_message_bytes ||
                s->frame_len >
                    (uint64_t)s->limits.max_message_bytes - so_far) {
                return protocol_fail(s, CF_CABLE_TOO_LARGE);
            }
            s->read_state = R_MASK;
            break;
        }
        case R_MASK: {
            if (READ_AVAIL(s) < 4) return FEED_OK;
            memcpy(s->mask, READ_PTR(s), 4);
            in_take(s, 4);
            s->payload_have = 0;
            s->control_len = 0;
            s->read_state = R_PAYLOAD;
            break;
        }
        case R_PAYLOAD: {
            bool control = s->opcode >= 0x8;
            uint64_t remaining = s->frame_len - s->payload_have;
            size_t take = READ_AVAIL(s);
            if ((uint64_t)take > remaining) take = (size_t)remaining;
            if (take != 0) {
                const unsigned char *p = READ_PTR(s);
                bool ok;
                if (control) {
                    control_append(s, p, take);
                    ok = true;
                } else if (s->orphan_continuation) {
                    /* Read and drop, like the reference's payload Vec. */
                    ok = true;
                } else {
                    ok = msg_append(s, p, take);
                }
                if (!ok) {
                    s->transport_failed = true;
                    return FEED_ABORT;
                }
                s->payload_have += take;
                in_take(s, take);
            }
            if (s->payload_have != s->frame_len) return FEED_OK;

            /* One frame complete. */
            bool fin = s->fin;
            unsigned opcode = s->opcode;
            s->read_state = R_B0;

            if (s->orphan_continuation) {
                /* socket.rs: a continuation of nothing fails here, after the
                 * payload was read. */
                return protocol_fail(s, CF_CABLE_PROTOCOL_ERROR);
            }

            if (control) {
                cf_span payload = {s->control, s->control_len};
                if (opcode == OP_CLOSE) {
                    s->stats.close_frames_received++;
                    bool has_code = false;
                    uint16_t code = 0;
                    if (!close_payload_valid(payload, &has_code, &code)) {
                        return protocol_fail(s, CF_CABLE_PROTOCOL_ERROR);
                    }
                    if (has_code) {
                        unsigned char echo[2] = {
                            (unsigned char)(code >> 8),
                            (unsigned char)(code & 0xFF)};
                        (void)queue_control(s, OP_CLOSE,
                                            (cf_span){echo, 2});
                    } else {
                        (void)queue_control(s, OP_CLOSE,
                                            (cf_span){NULL, 0});
                    }
                    s->closing = true;
                    s->wait_peer_close = false;
                    return FEED_CLOSED;
                }
                if (opcode == OP_PING) {
                    (void)queue_control(s, OP_PONG, payload);
                    continue;
                }
                /* PONG */
                continue;
            }

            if (fin) {
                const unsigned char *data =
                    s->msg.len != 0 ? s->msg.ptr : NULL;
                size_t len = s->msg.len;
                unsigned msg_opcode = s->msg_opcode;
                bool msg_compressed = s->msg_compressed;
                s->msg_open = false;
                s->msg.len = 0;
                enum feed_result r =
                    message_done(s, data, len, msg_opcode, msg_compressed);
                if (r != FEED_OK) return r;
            }
            continue;
        }
        default:
            return protocol_fail(s, CF_CABLE_PROTOCOL_ERROR);
        }
    }
}

/* ---- socket lifecycle ----------------------------------------------------- */

static cf_err socket_write_all(struct cf_cable_socket *s,
                               const unsigned char *bytes, size_t len,
                               uint64_t deadline_ms) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(s->fd, bytes + off, len - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            uint64_t now = cf_monotonic_ms();
            if (now >= deadline_ms) return CF_IO;
            struct pollfd pf = {s->fd, POLLOUT, 0};
            int pr = poll(&pf, 1, (int)(deadline_ms - now));
            if (pr < 0 && errno != EINTR) return CF_IO;
            continue;
        }
        return CF_IO;
    }
    return CF_OK;
}

static bool socket_readable(struct cf_cable_socket *s) {
    unsigned char buf[CF_CABLE_READ_CHUNK];
    for (;;) {
        ssize_t n = recv(s->fd, buf, sizeof buf, 0);
        if (n > 0) {
            if (cf_builder_append(&s->in, (cf_span){buf, (size_t)n}) !=
                CF_OK) {
                s->transport_failed = true;
                return false;
            }
            continue;
        }
        if (n == 0) {
            s->finished = true; /* peer closed */
            return false;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        s->transport_failed = true;
        return false;
    }
}

static void socket_close_queue(struct cf_cable_socket *s) {
    pthread_mutex_lock(&s->mutex);
    s->closed = true;
    struct cf_cable_out *node = s->out_head;
    s->out_head = s->out_tail = NULL;
    s->pending_bytes = 0;
    pthread_mutex_unlock(&s->mutex);
    while (node != NULL) {
        struct cf_cable_out *next = node->next;
        cf_buf_release(node->payload);
        free(node);
        node = next;
    }
}

cf_err cf_cable_socket_run(const cf_cable_socket_config *config,
                           const cf_cable_hooks *hooks,
                           const cf_cable_limits *limits,
                           cf_cable_socket_stats *stats) {
    if (stats != NULL) memset(stats, 0, sizeof *stats);
    if (config == NULL || config->fd < 0) return CF_INVALID;

    cf_cable_socket *s = calloc(1, sizeof *s);
    if (s == NULL) return CF_NOMEM;
    s->fd = config->fd;
    s->deflate = config->deflate;
    s->limits = limits != NULL ? *limits : (cf_cable_limits){0};
    if (s->limits.max_message_bytes == 0) {
        s->limits.max_message_bytes = CF_CABLE_MAX_MESSAGE;
    }
    if (s->limits.max_pending_bytes == 0) {
        s->limits.max_pending_bytes = CF_CABLE_MAX_PENDING_BYTES;
    }
    if (s->limits.write_timeout_ms == 0) {
        s->limits.write_timeout_ms = CF_CABLE_WRITE_STALL_MS;
    }
    if (s->limits.close_timeout_ms == 0) {
        s->limits.close_timeout_ms = CF_CABLE_CLOSE_TIMEOUT_MS;
    }
    if (s->limits.beat_interval_ms == 0) {
        s->limits.beat_interval_ms = CF_CABLE_BEAT_INTERVAL_MS;
    }
    if (hooks != NULL) s->hooks = *hooks;
    s->read_state = R_B0;
    s->next_beat_ms = cf_monotonic_ms() + s->limits.beat_interval_ms;

    if (pthread_mutex_init(&s->mutex, NULL) != 0) {
        free(s);
        return CF_INTERNAL;
    }
    s->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (s->wake_fd < 0) {
        pthread_mutex_destroy(&s->mutex);
        free(s);
        return CF_INTERNAL;
    }
    /* The HTTP loop hands over a nonblocking fd; a direct caller may pass a
     * blocking one, which would defeat the poll-loop. */
    int flags = fcntl(s->fd, F_GETFL, 0);
    if (flags >= 0 && (flags & O_NONBLOCK) == 0) {
        (void)fcntl(s->fd, F_SETFL, flags | O_NONBLOCK);
    }

    uint64_t deadline = cf_monotonic_ms() + s->limits.close_timeout_ms;
    cf_err result = CF_OK;

    /* The 101 response is written before anything else, exactly where hyper
     * would write it for the reference. */
    if (config->handshake_len != 0 &&
        socket_write_all(s, config->handshake, config->handshake_len,
                         deadline) != CF_OK) {
        result = CF_IO;
        goto done;
    }

    bool authenticated = false;
    int64_t user_id = 0;
    if (s->hooks.authenticate != NULL) {
        cf_err rc = s->hooks.authenticate(s->hooks.authenticate_user,
                                          config->request, &authenticated,
                                          &user_id);
        if (rc != CF_OK) authenticated = false;
    }
    (void)user_id;

    /* Bytes the client pipelined after the handshake request are consumed
     * before anything the socket reads later. */
    if (config->pending_len != 0) {
        if (cf_builder_append(
                &s->in, (cf_span){config->pending,
                                  config->pending_len}) != CF_OK) {
            result = CF_NOMEM;
            goto done;
        }
    }

    if (!authenticated) {
        /* respond_to_invalid_request: disconnect, no reconnect, then close. */
        cf_str frame = {0};
        if (cf_cable_disconnect_frame(
                CF_CABLE_REASON_UNAUTHORIZED,
                (cf_span){(const unsigned char *)"false", 5}, &frame) ==
            CF_OK) {
            s->stats.unauthorized_closes++;
            (void)queue_text_bytes(s, (cf_span){
                                           (const unsigned char *)frame.ptr,
                                           frame.len});
            cf_str_dispose(&frame);
        } else {
            s->stats.unauthorized_closes++;
        }
        unsigned char close_code[2] = {0x03, 0xE8}; /* 1000 */
        (void)queue_control(s, OP_CLOSE, (cf_span){close_code, 2});
        s->closing = true;
        s->wait_peer_close = true;
        s->close_deadline_ms = cf_monotonic_ms() + s->limits.close_timeout_ms;
    } else {
        cf_str welcome = {0};
        if (cf_cable_welcome(&welcome) == CF_OK) {
            (void)queue_text_bytes(s, (cf_span){
                                           (const unsigned char *)welcome.ptr,
                                           welcome.len});
            cf_str_dispose(&welcome);
        }
    }

    /* Consume pipelined frames before waiting for new socket input. */
    if (s->in.len != 0) (void)reader_feed(s);

    while (!s->finished && !s->transport_failed && !s->queue_cap_hit) {
        uint64_t now = cf_monotonic_ms();
        if (s->closing) {
            if (s->wait_peer_close) {
                if (now >= s->close_deadline_ms) break;
            } else if (s->pending_bytes == 0) {
                break;
            }
        } else {
            if (s->pending_bytes > 0 &&
                now - s->last_progress_ms >= s->limits.write_timeout_ms) {
                s->stats.write_timeout_closes++;
                break;
            }
            if (s->limits.beat_interval_ms != 0 && now >= s->next_beat_ms) {
                cf_str ping = {0};
                if (cf_cable_ping(cf_now_us(NULL) / INT64_C(1000000),
                                  &ping) == CF_OK) {
                    if (queue_text_bytes(
                            s, (cf_span){(const unsigned char *)ping.ptr,
                                         ping.len}) == CF_OK) {
                        s->stats.pings_sent++;
                    }
                    cf_str_dispose(&ping);
                }
                s->next_beat_ms = now + s->limits.beat_interval_ms;
            }
        }

        int timeout = 1000;
        if (s->closing && s->wait_peer_close) {
            uint64_t left = s->close_deadline_ms > now
                                ? s->close_deadline_ms - now
                                : 0;
            if (left < (uint64_t)timeout) timeout = (int)left;
        } else if (!s->closing && s->pending_bytes > 0) {
            uint64_t left = s->last_progress_ms + s->limits.write_timeout_ms;
            left = left > now ? left - now : 0;
            if (left < (uint64_t)timeout) timeout = (int)left;
        }
        if (timeout < 0) timeout = 0;

        struct pollfd pf[2];
        pf[0].fd = s->fd;
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        if (s->pending_bytes > 0 || s->out_head != NULL) {
            pf[0].events |= POLLOUT;
        }
        pf[1].fd = s->wake_fd;
        pf[1].events = POLLIN;
        pf[1].revents = 0;
        int pr = poll(pf, 2, timeout);
        if (pr < 0) {
            if (errno == EINTR) continue;
            s->transport_failed = true;
            break;
        }
        if (pr == 0) continue;

        if (pf[1].revents != 0) socket_drain_wake(s);
        if (s->queue_cap_hit) break;
        if (pf[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            /* Drain what was already buffered, then finish. */
            if (pf[0].revents & POLLIN) {
                (void)socket_readable(s);
                (void)reader_feed(s);
            }
            break;
        }
        if (pf[0].revents & POLLIN) {
            if (socket_readable(s)) {
                enum feed_result r = reader_feed(s);
                (void)r; /* closing/transport flags carry the outcome */
            }
        }
        socket_flush(s);
    }

    /* Complete the close handshake best-effort: flush what is left. */
    if (!s->transport_failed && !s->queue_cap_hit) socket_flush(s);

done:
    socket_close_queue(s);
    pthread_mutex_destroy(&s->mutex);
    close(s->wake_fd);
    cf_builder_dispose(&s->in);
    cf_builder_dispose(&s->msg);
    if (stats != NULL) *stats = s->stats;
    free(s);
    return result;
}

/* ---- server: the /cable front mount (server.rs Server::call) -------------- */

struct cf_cable_server;

struct cable_conn {
    struct cable_conn *next, *prev;
    cf_cable_server *server;
    int fd; /* -1 once closed (under the server mutex) */
    pthread_t thread;
    cf_cable_socket_stats stats;

    /* Owned copy of the upgrade request. */
    cf_cable_request request;
    unsigned char *strings; /* one block: target/path/query/peer/headers */
    size_t strings_len;
    cf_header *headers;

    unsigned char *handshake;
    size_t handshake_len;
    unsigned char *pending;
    size_t pending_len;
    bool deflate;
    const char *subprotocol;
};

struct cf_cable_server {
    char *mount_path;
    size_t mount_len;
    bool allow_same_origin_as_host;
    bool assume_ssl;
    bool disable_request_forgery_protection;
    char **allowed_origins;
    size_t allowed_origins_len;
    cf_cable_hooks hooks;
    cf_cable_limits limits;

    pthread_mutex_t mutex;
    pthread_cond_t idle_cv;
    struct cable_conn *conns;
    size_t live;
    bool stopping;
    cf_cable_socket_stats totals;
};

void cf_cable_limits_default(cf_cable_limits *out) {
    if (out == NULL) return;
    out->max_message_bytes = CF_CABLE_MAX_MESSAGE;
    out->max_pending_bytes = CF_CABLE_MAX_PENDING_BYTES;
    out->write_timeout_ms = CF_CABLE_WRITE_STALL_MS;
    out->close_timeout_ms = CF_CABLE_CLOSE_TIMEOUT_MS;
    out->beat_interval_ms = CF_CABLE_BEAT_INTERVAL_MS;
}

void cf_cable_server_config_default(cf_cable_server_config *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof *out);
    out->mount_path = CF_CABLE_DEFAULT_MOUNT;
    out->allow_same_origin_as_host = true;
    out->assume_ssl = true;
    cf_cable_limits_default(&out->limits);
}

void cf_cable_socket_stats_add(cf_cable_socket_stats *acc,
                               const cf_cable_socket_stats *part) {
    if (acc == NULL || part == NULL) return;
    acc->messages_received += part->messages_received;
    acc->binary_messages += part->binary_messages;
    acc->frames_queued += part->frames_queued;
    acc->frames_sent += part->frames_sent;
    acc->bytes_sent += part->bytes_sent;
    acc->pings_sent += part->pings_sent;
    acc->pongs_sent += part->pongs_sent;
    acc->close_frames_sent += part->close_frames_sent;
    acc->close_frames_received += part->close_frames_received;
    acc->protocol_closes += part->protocol_closes;
    acc->queue_cap_closes += part->queue_cap_closes;
    acc->write_timeout_closes += part->write_timeout_closes;
    acc->unauthorized_closes += part->unauthorized_closes;
    acc->deflate_failures += part->deflate_failures;
}

static cf_err reply_404(cf_builder *reply) {
    static const char body[] = "Page not found";
    static const char head[] =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: 14\r\n\r\n";
    cf_err rc = cf_builder_append(
        reply, (cf_span){(const unsigned char *)head, sizeof head - 1});
    if (rc == CF_OK) {
        rc = cf_builder_append(
            reply, (cf_span){(const unsigned char *)body, sizeof body - 1});
    }
    return rc;
}

static cf_err reply_503(cf_builder *reply) {
    static const char body[] = "Service Unavailable";
    static const char head[] =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: 19\r\n"
        "Connection: close\r\n\r\n";
    cf_err rc = cf_builder_append(
        reply, (cf_span){(const unsigned char *)head, sizeof head - 1});
    if (rc == CF_OK) {
        rc = cf_builder_append(
            reply, (cf_span){(const unsigned char *)body, sizeof body - 1});
    }
    return rc;
}

static bool origin_allowed(const cf_cable_server *server,
                           const cf_cable_request *request) {
    if (server->disable_request_forgery_protection) return true;
    cf_span origin;
    bool has_origin = request_header_first(request, "origin", &origin);
    cf_span host;
    bool has_host = request_header_first(request, "host", &host);
    /* D-C07: proxy headers are untrusted and ignored; the listener owns the
     * scheme, expressed by assume_ssl when the app has no TLS listener yet. */
    const char *scheme = server->assume_ssl ? "https" : "http";
    if (server->allow_same_origin_as_host && has_origin && has_host) {
        cf_builder expected = {0};
        cf_err rc = cf_builder_append(
            &expected,
            (cf_span){(const unsigned char *)scheme, strlen(scheme)});
        if (rc == CF_OK) {
            rc = cf_builder_append(
                &expected,
                (cf_span){(const unsigned char *)"://", 3});
        }
        if (rc == CF_OK) rc = cf_builder_append(&expected, host);
        if (rc == CF_OK && expected.len == origin.len &&
            memcmp(expected.ptr, origin.ptr, origin.len) == 0) {
            cf_builder_dispose(&expected);
            return true;
        }
        cf_builder_dispose(&expected);
    }
    if (has_origin) {
        for (size_t i = 0; i < server->allowed_origins_len; i++) {
            cf_span allowed = {
                (const unsigned char *)server->allowed_origins[i],
                strlen(server->allowed_origins[i])};
            if (allowed.len == origin.len &&
                memcmp(allowed.ptr, origin.ptr, origin.len) == 0) {
                return true;
            }
        }
    }
    return false;
}

static void conn_free(struct cable_conn *conn) {
    free(conn->handshake);
    free(conn->pending);
    free(conn->strings);
    free(conn->headers);
    free(conn);
}

static void *cable_conn_thread(void *arg) {
    struct cable_conn *conn = arg;
    cf_cable_server *server = conn->server;

    cf_cable_socket_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.fd = conn->fd;
    cfg.deflate = conn->deflate;
    cfg.subprotocol = conn->subprotocol;
    cfg.handshake = conn->handshake;
    cfg.handshake_len = conn->handshake_len;
    cfg.pending = conn->pending;
    cfg.pending_len = conn->pending_len;
    cfg.request = &conn->request;

    cf_cable_socket_stats stats;
    (void)cf_cable_socket_run(&cfg, &server->hooks, &server->limits, &stats);

    pthread_mutex_lock(&server->mutex);
    close(conn->fd);
    conn->fd = -1;
    if (conn->prev != NULL) {
        conn->prev->next = conn->next;
    } else {
        server->conns = conn->next;
    }
    if (conn->next != NULL) conn->next->prev = conn->prev;
    server->live--;
    cf_cable_socket_stats_add(&server->totals, &stats);
    pthread_cond_broadcast(&server->idle_cv);
    pthread_mutex_unlock(&server->mutex);

    conn_free(conn);
    return NULL;
}

/* The TAKEN continuation: register the connection and start its owner
 * thread once the HTTP loop has released the fd. Runs on the loop thread. */
static cf_err cable_conn_start(void *user) {
    struct cable_conn *conn = user;
    cf_cable_server *server = conn->server;
    pthread_mutex_lock(&server->mutex);
    if (server->stopping) {
        pthread_mutex_unlock(&server->mutex);
        close(conn->fd);
        conn->fd = -1;
        conn_free(conn);
        return CF_BUSY;
    }
    conn->prev = NULL;
    conn->next = server->conns;
    if (server->conns != NULL) server->conns->prev = conn;
    server->conns = conn;
    server->live++;
    pthread_mutex_unlock(&server->mutex);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int create_rc =
        pthread_create(&conn->thread, &attr, cable_conn_thread, conn);
    pthread_attr_destroy(&attr);
    if (create_rc != 0) {
        pthread_mutex_lock(&server->mutex);
        if (conn->prev != NULL) {
            conn->prev->next = conn->next;
        } else {
            server->conns = conn->next;
        }
        if (conn->next != NULL) conn->next->prev = conn->prev;
        server->live--;
        pthread_mutex_unlock(&server->mutex);
        close(conn->fd);
        conn->fd = -1;
        conn_free(conn);
        return CF_INTERNAL;
    }
    return CF_OK;
}

cf_err cf_cable_server_create(const cf_cable_server_config *config,
                              cf_cable_server **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (config == NULL) return CF_INVALID;
    cf_cable_server *server = calloc(1, sizeof *server);
    if (server == NULL) return CF_NOMEM;
    server->mount_path = strdup(config->mount_path != NULL
                                    ? config->mount_path
                                    : CF_CABLE_DEFAULT_MOUNT);
    if (server->mount_path == NULL) {
        free(server);
        return CF_NOMEM;
    }
    server->mount_len = strlen(server->mount_path);
    server->allow_same_origin_as_host = config->allow_same_origin_as_host;
    server->assume_ssl = config->assume_ssl;
    server->disable_request_forgery_protection =
        config->disable_request_forgery_protection;
    server->hooks = config->hooks;
    server->limits = config->limits;
    if (server->limits.max_message_bytes == 0) {
        server->limits.max_message_bytes = CF_CABLE_MAX_MESSAGE;
    }
    if (server->limits.max_pending_bytes == 0) {
        server->limits.max_pending_bytes = CF_CABLE_MAX_PENDING_BYTES;
    }
    if (server->limits.write_timeout_ms == 0) {
        server->limits.write_timeout_ms = CF_CABLE_WRITE_STALL_MS;
    }
    if (server->limits.close_timeout_ms == 0) {
        server->limits.close_timeout_ms = CF_CABLE_CLOSE_TIMEOUT_MS;
    }
    if (server->limits.beat_interval_ms == 0) {
        server->limits.beat_interval_ms = CF_CABLE_BEAT_INTERVAL_MS;
    }
    if (config->allowed_request_origins_len != 0) {
        server->allowed_origins =
            calloc(config->allowed_request_origins_len,
                   sizeof *server->allowed_origins);
        if (server->allowed_origins == NULL) {
            free(server->mount_path);
            free(server);
            return CF_NOMEM;
        }
        for (size_t i = 0; i < config->allowed_request_origins_len; i++) {
            server->allowed_origins[i] =
                strdup(config->allowed_request_origins[i]);
            if (server->allowed_origins[i] == NULL) {
                for (size_t j = 0; j < i; j++) {
                    free(server->allowed_origins[j]);
                }
                free(server->allowed_origins);
                free(server->mount_path);
                free(server);
                return CF_NOMEM;
            }
        }
        server->allowed_origins_len = config->allowed_request_origins_len;
    }
    if (pthread_mutex_init(&server->mutex, NULL) != 0) {
        for (size_t i = 0; i < server->allowed_origins_len; i++) {
            free(server->allowed_origins[i]);
        }
        free(server->allowed_origins);
        free(server->mount_path);
        free(server);
        return CF_INTERNAL;
    }
    if (pthread_cond_init(&server->idle_cv, NULL) != 0) {
        pthread_mutex_destroy(&server->mutex);
        for (size_t i = 0; i < server->allowed_origins_len; i++) {
            free(server->allowed_origins[i]);
        }
        free(server->allowed_origins);
        free(server->mount_path);
        free(server);
        return CF_INTERNAL;
    }
    *out = server;
    return CF_OK;
}

void cf_cable_server_destroy(cf_cable_server *server) {
    if (server == NULL) return;
    pthread_mutex_lock(&server->mutex);
    server->stopping = true;
    for (struct cable_conn *c = server->conns; c != NULL; c = c->next) {
        if (c->fd >= 0) shutdown(c->fd, SHUT_RDWR);
    }
    while (server->live != 0) {
        pthread_cond_wait(&server->idle_cv, &server->mutex);
    }
    pthread_mutex_unlock(&server->mutex);
    pthread_cond_destroy(&server->idle_cv);
    pthread_mutex_destroy(&server->mutex);
    for (size_t i = 0; i < server->allowed_origins_len; i++) {
        free(server->allowed_origins[i]);
    }
    free(server->allowed_origins);
    free(server->mount_path);
    free(server);
}

size_t cf_cable_server_connections(const cf_cable_server *server) {
    if (server == NULL) return 0;
    cf_cable_server *mutable_server = (cf_cable_server *)server;
    pthread_mutex_lock(&mutable_server->mutex);
    size_t live = server->live;
    pthread_mutex_unlock(&mutable_server->mutex);
    return live;
}

void cf_cable_server_stats(const cf_cable_server *server,
                           cf_cable_socket_stats *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof *out);
    if (server == NULL) return;
    cf_cable_server *mutable_server = (cf_cable_server *)server;
    pthread_mutex_lock(&mutable_server->mutex);
    *out = server->totals;
    pthread_mutex_unlock(&mutable_server->mutex);
}

static cf_http_upgrade_result answer_404(cf_http_upgrade_request *request) {
    if (reply_404(&request->reply) != CF_OK) {
        /* An unanswerable /cable request never becomes ordinary routing; the
         * empty reply makes HTTP close the connection. */
        cf_builder_dispose(&request->reply);
        request->close_after = true;
        return CF_HTTP_UPGRADE_REPLY;
    }
    return CF_HTTP_UPGRADE_REPLY;
}

cf_http_upgrade_result cf_cable_server_upgrade(void *user,
                                               cf_http_upgrade_request *request) {
    cf_cable_server *server = user;
    if (server == NULL || request == NULL) return CF_HTTP_UPGRADE_PASS;
    if (request->path.len != server->mount_len ||
        memcmp(request->path.ptr, server->mount_path, server->mount_len) !=
            0) {
        return CF_HTTP_UPGRADE_PASS;
    }

    /* A crude request view for the shared helpers. */
    cf_cable_request view;
    memset(&view, 0, sizeof view);
    view.method = request->method;
    view.target = request->target;
    view.path = request->path;
    view.query = request->query;
    view.peer_ip = request->peer_ip;
    view.headers = request->headers;
    view.header_count = request->header_count;

    if (!websocket_request(&view)) return answer_404(request);
    if (!origin_allowed(server, &view)) return answer_404(request);

    cable_handshake handshake;
    memset(&handshake, 0, sizeof handshake);
    if (!handshake_accept(&view, &handshake)) return answer_404(request);

    const char *subprotocol = negotiate_protocol(&view);

    cf_builder response = {0};
    static const char head[] =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "upgrade: websocket\r\n"
        "connection: upgrade\r\n"
        "sec-websocket-accept: ";
    cf_err rc = cf_builder_append(
        &response,
        (cf_span){(const unsigned char *)head, sizeof head - 1});
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &response,
            (cf_span){(const unsigned char *)handshake.accept,
                      strlen(handshake.accept)});
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(&response,
                               (cf_span){(const unsigned char *)"\r\n", 2});
    }
    if (rc == CF_OK && handshake.deflate) {
        static const char ext[] = "sec-websocket-extensions: " CF_CABLE_DEFLATE_RESPONSE
                                  "\r\n";
        rc = cf_builder_append(
            &response, (cf_span){(const unsigned char *)ext, sizeof ext - 1});
    }
    if (rc == CF_OK && subprotocol != NULL) {
        static const char name[] = "sec-websocket-protocol: ";
        rc = cf_builder_append(
            &response, (cf_span){(const unsigned char *)name,
                                 sizeof name - 1});
        if (rc == CF_OK) {
            rc = cf_builder_append(
                &response,
                (cf_span){(const unsigned char *)subprotocol,
                          strlen(subprotocol)});
        }
        if (rc == CF_OK) {
            rc = cf_builder_append(
                &response, (cf_span){(const unsigned char *)"\r\n", 2});
        }
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(&response,
                               (cf_span){(const unsigned char *)"\r\n", 2});
    }
    if (rc != CF_OK) {
        cf_builder_dispose(&response);
        (void)reply_503(&request->reply);
        request->close_after = true;
        return CF_HTTP_UPGRADE_REPLY;
    }

    struct cable_conn *conn = calloc(1, sizeof *conn);
    if (conn == NULL) {
        cf_builder_dispose(&response);
        (void)reply_503(&request->reply);
        request->close_after = true;
        return CF_HTTP_UPGRADE_REPLY;
    }
    conn->server = server;
    conn->fd = request->fd;
    conn->deflate = handshake.deflate;
    conn->subprotocol = subprotocol;
    conn->handshake = response.ptr;
    conn->handshake_len = response.len;
    /* response.ptr is builder storage; move it into the connection. */
    response.ptr = NULL;
    response.len = response.cap = 0;

    /* Copy the request headers, spans and pipelined bytes the socket may
     * read after this call. */
    size_t strings_len = view.target.len + view.path.len + view.query.len +
                         view.peer_ip.len;
    for (size_t i = 0; i < view.header_count; i++) {
        strings_len += view.headers[i].name.len + view.headers[i].value.len;
    }
    conn->strings = malloc(strings_len != 0 ? strings_len : 1);
    conn->headers = calloc(view.header_count != 0 ? view.header_count : 1,
                           sizeof *conn->headers);
    if (request->pending_len != 0) {
        conn->pending = malloc(request->pending_len);
        if (conn->pending != NULL) {
            memcpy(conn->pending, request->pending, request->pending_len);
            conn->pending_len = request->pending_len;
        }
    }
    if (conn->strings == NULL || conn->headers == NULL ||
        (request->pending_len != 0 && conn->pending == NULL)) {
        conn_free(conn);
        (void)reply_503(&request->reply);
        request->close_after = true;
        return CF_HTTP_UPGRADE_REPLY;
    }
    conn->strings_len = strings_len;
    size_t off = 0;
#define COPY_SPAN(dst, src)                     \
    do {                                        \
        (dst) = (cf_span){conn->strings + off, (src).len}; \
        if ((src).len != 0) {                   \
            memcpy(conn->strings + off, (src).ptr, (src).len); \
            off += (src).len;                   \
        }                                       \
    } while (0)
    COPY_SPAN(conn->request.target, view.target);
    COPY_SPAN(conn->request.path, view.path);
    COPY_SPAN(conn->request.query, view.query);
    COPY_SPAN(conn->request.peer_ip, view.peer_ip);
#undef COPY_SPAN
    for (size_t i = 0; i < view.header_count; i++) {
        cf_span name = {conn->strings + off, view.headers[i].name.len};
        if (name.len != 0) {
            memcpy(conn->strings + off, view.headers[i].name.ptr, name.len);
            off += name.len;
        }
        cf_span value = {conn->strings + off, view.headers[i].value.len};
        if (value.len != 0) {
            memcpy(conn->strings + off, view.headers[i].value.ptr, value.len);
            off += value.len;
        }
        conn->headers[i].name = name;
        conn->headers[i].value = value;
    }
    conn->request.method = view.method;
    conn->request.headers = conn->headers;
    conn->request.header_count = view.header_count;

    pthread_mutex_lock(&server->mutex);
    if (server->stopping) {
        pthread_mutex_unlock(&server->mutex);
        conn_free(conn);
        (void)reply_503(&request->reply);
        request->close_after = true;
        return CF_HTTP_UPGRADE_REPLY;
    }
    pthread_mutex_unlock(&server->mutex);

    /* The thread starts only after the loop reports the fd fully detached
     * (see cf_http_upgrade_request.taken), so it can close the fd without
     * racing the HTTP epoll registration. */
    request->taken = cable_conn_start;
    request->taken_user = conn;
    return CF_HTTP_UPGRADE_TAKEN;
}

/* ---- A01 session authentication ------------------------------------------- */

/* Rack cookie parsing: split pairs on ';', trim, name before the first '=';
 * a later duplicate of the same name wins. */
static bool request_cookie_value(const cf_cable_request *request,
                                 const char *name, cf_str *out) {
    size_t name_len = strlen(name);
    bool found = false;
    for (size_t h = 0; h < request->header_count; h++) {
        if (!bytes_ieq_lit(request->headers[h].name, "cookie")) continue;
        cf_span v = request->headers[h].value;
        size_t pos = 0;
        while (pos <= v.len) {
            size_t end = pos;
            while (end < v.len && v.ptr[end] != ';') end++;
            cf_span pair = span_trim((cf_span){v.ptr + pos, end - pos});
            const unsigned char *eq = memchr(pair.ptr, '=', pair.len);
            if (eq != NULL) {
                cf_span cname =
                    span_trim((cf_span){pair.ptr, (size_t)(eq - pair.ptr)});
                if (cname.len == name_len &&
                    memcmp(cname.ptr, name, name_len) == 0) {
                    cf_span raw = span_trim((cf_span){
                        eq + 1, pair.len - (size_t)(eq - pair.ptr) - 1});
                    /* URL-unescape the wire value, as Rack does. */
                    cf_builder decoded = {0};
                    cf_err rc = CF_OK;
                    for (size_t i = 0; i < raw.len; i++) {
                        unsigned char c = raw.ptr[i];
                        int value = c;
                        if (c == '+') {
                            value = ' ';
                        } else if (c == '%' && i + 2 < raw.len) {
                            int hi = -1, lo = -1;
                            unsigned char a = raw.ptr[i + 1];
                            unsigned char b = raw.ptr[i + 2];
                            if (a >= '0' && a <= '9') hi = a - '0';
                            else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
                            else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
                            if (b >= '0' && b <= '9') lo = b - '0';
                            else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
                            else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
                            if (hi >= 0 && lo >= 0) {
                                value = (hi << 4) | lo;
                                i += 2;
                            }
                        }
                        unsigned char byte = (unsigned char)value;
                        if (rc == CF_OK) {
                            rc = cf_builder_append(
                                &decoded, (cf_span){&byte, 1});
                        }
                    }
                    if (rc != CF_OK) {
                        cf_builder_dispose(&decoded);
                        return false;
                    }
                    size_t decoded_len = decoded.len;
                    char *text = malloc(decoded_len + 1);
                    if (text == NULL) {
                        cf_builder_dispose(&decoded);
                        return false;
                    }
                    if (decoded_len != 0) {
                        memcpy(text, decoded.ptr, decoded_len);
                    }
                    text[decoded_len] = '\0';
                    cf_builder_dispose(&decoded);
                    cf_str_dispose(out);
                    *out = (cf_str){text, decoded_len};
                    found = true;
                }
            }
            pos = end + 1;
        }
    }
    return found;
}

cf_err cf_cable_session_authenticate(void *user,
                                     const cf_cable_request *request,
                                     bool *authenticated, int64_t *user_id) {
    if (user == NULL || request == NULL || authenticated == NULL ||
        user_id == NULL) {
        return CF_INVALID;
    }
    *authenticated = false;
    *user_id = 0;
    const cf_cable_session_auth *auth = user;
    if (auth->reader == NULL) return CF_OK;

    cf_str raw = {0};
    bool have_cookie =
        request_cookie_value(request, "session_token", &raw);
    if (!have_cookie) return CF_OK;

    cf_str token = {0};
    bool found = false;
    cf_err rc = cf_auth_signed_cookie_verify(
        auth->secret_key_base,
        (cf_span){(const unsigned char *)"session_token", 13},
        (cf_span){(const unsigned char *)raw.ptr, raw.len}, cf_now_us(NULL),
        &token, &found);
    cf_str_dispose(&raw);
    if (rc != CF_OK) return rc;
    if (!found) return CF_OK;

    cf_session session;
    bool session_found = false;
    rc = cf_session_find_by_token(auth->reader, token, &session_found,
                                  &session);
    cf_str_dispose(&token);
    if (rc != CF_OK) return rc;
    if (!session_found) return CF_OK;

    cf_user user_row;
    bool user_found = false;
    rc = cf_user_find_by_id(auth->reader, session.user_id, &user_found,
                            &user_row);
    cf_session_dispose(&session);
    if (rc != CF_OK) return rc;
    if (user_found) {
        int64_t id = user_row.id;
        cf_user_dispose(&user_row);
        *authenticated = true;
        *user_id = id;
    }
    return CF_OK;
}
