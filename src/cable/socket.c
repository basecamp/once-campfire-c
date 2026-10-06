/* src/cable/socket.c — task C01: the WebSocket transport of the /cable front
 * mount (04-cable-jobs.md C01; socket.rs, server.rs and the Authenticate
 * trait of tmp/rust-ref/crates/cable/src).
 *
 * Ownership. A live socket has exactly one owner thread and only that thread
 * sends, closes or mutates it. The production server services every upgraded
 * socket of one HTTP loop on that loop's single bounded cable reactor thread
 * (never a thread per connection); cf_cable_socket_run remains the
 * standalone one-socket driver for direct users. Writers may enqueue frames
 * from anywhere (mutex + wake eventfd). The reader is the translation of
 * Reader::next: masked client frames, variable lengths, fragmentation,
 * control frames interleaved with fragments, UTF-8 text validation, the
 * close handshake, and the reference close codes. permessage-deflate is
 * negotiated without context takeover; the writer sends the reference raw
 * deflate framing (RFC 7692) and the same FrameDeflate cache as socket.rs.
 *
 * Admission. The server's upgraded sockets keep the HTTP connection-slot
 * reservation for their whole lifetime (cf_http_upgrade_lease): a live
 * upgrade prevents a replacement connection until its socket ends, and the
 * reactor releases the lease exactly once on every ending (normal close,
 * reset, timeout, shutdown). Each reactor also enforces aggregate
 * input/output byte budgets across all of its sockets.
 */
#include "cable.h"
#include "front/tls.h"

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
#include <sys/epoll.h>
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
/* Reactor fairness: most bytes one socket may read in a single serviced
 * event before the reactor moves on (level-triggered epoll reports the
 * rest). */
#define CF_CABLE_REACTOR_READ_CHUNK ((size_t)256 << 10)

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

/* HeaderValue::to_str (http 1.5.0 src/header/value.rs:558-560, the pin the
 * cable crate reads through): readable means every byte is HTAB or visible
 * ASCII (0x20..=0x7E).  Obs-text (>= 0x80, valid UTF-8 included), DEL and the
 * other controls make the reference's read answer `None` -- the header reads
 * as ABSENT, never as raw bytes.  The sites (cable crate):
 *   server.rs:240,241  Origin, Host          (get -> first value only)
 *   server.rs:288      Connection            (get_all -> every value)
 *   server.rs:289      Upgrade               (get -> first value only)
 *   server.rs:307      Sec-WebSocket-Protocol (get_all)
 *   socket.rs:141      Sec-WebSocket-Extensions (get_all)
 *   socket.rs:129,130  Sec-WebSocket-Version/Key (`get` then raw byte compare /
 *                      base64 of the raw bytes; an unreadable value can never
 *                      equal "13" nor decode, so the gate is outcome-equal)
 *   campfire/src/channels/connection.rs:26 Cookie (get_all -> every value)
 * A later value of the same name is consulted only at the get_all sites. */
static bool cable_header_readable(cf_span value) {
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (c != '\t' && (c < 0x20 || c > 0x7E)) return false;
    }
    return true;
}

/* First value of a header, or false when absent.  The pin's `HeaderMap::get`
 * returns the first value for the name: an unreadable first value reads as
 * absent and a later duplicate is not consulted. */
static bool request_header_first(const cf_cable_request *request,
                                 const char *name, cf_span *out) {
    for (size_t i = 0; i < request->header_count; i++) {
        if (bytes_ieq_lit(request->headers[i].name, name)) {
            if (!cable_header_readable(request->headers[i].value)) return false;
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
        /* get_all(CONNECTION).iter().any(to_str): an unreadable value is
         * skipped whole, a later readable duplicate still counts. */
        if (!cable_header_readable(v)) continue;
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
        /* socket.rs:138-143 get_all(EXTENSIONS).filter_map(to_str): an
         * unreadable value is dropped whole, a later readable one counts. */
        if (!cable_header_readable(v)) continue;
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
        /* server.rs:303-311 get_all(PROTOCOL).filter_map(to_str): an
         * unreadable value is dropped whole (its readable-looking tokens
         * included), a later readable duplicate still counts. */
        if (!cable_header_readable(v)) continue;
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
    unsigned opcode; /* OP_RAW = pre-serialized bytes (the 101 response) */
};

/* The pre-serialized handshake marker, never a real WebSocket opcode. */
#define OP_RAW 0x100u

/* The per-HTTP-loop cable reactor (one bounded thread for every upgraded
 * socket of that loop). Defined here because the socket accounting hooks
 * below need its fields; its behavior lives in the server section. */
struct cable_conn;
struct cable_reactor {
    struct cf_cable_server *server;
    uint32_t loop_index;
    pthread_t thread;
    bool started;
    bool owner_started; /* owner_start hook ran (owner_stop must run) */
    int epfd;
    int wake_fd;
    pthread_mutex_t mutex; /* enrollment queue + aggregate byte budgets */
    struct cable_conn *conns; /* enrolled, linked under mutex */
    struct cable_conn *enroll_head, *enroll_tail; /* waiting enrollment */
    size_t live;
    size_t input_used;  /* aggregate buffered input bytes held by sockets */
    size_t output_used; /* aggregate queued wire bytes across sockets */
    size_t input_limit;
    size_t output_limit;
    bool stopping;
};

static void reactor_wake(struct cable_reactor *reactor);

/* The token-form wake the owner_start hook receives: the C03 barrier calls it
 * from the writer thread, so it only writes the eventfd. */
static void reactor_wake_token(void *token) {
    reactor_wake(token);
}

struct cf_cable_socket {
    int fd;
    cf_front_tls_conn *tls; /* borrowed from cable_conn */
    bool read_want_write, write_want_read;
    bool deflate;
    int wake_fd; /* standalone only; reactors own one wake eventfd */
    struct cable_reactor *reactor; /* non-NULL: multiplexed transport */
    const cf_cable_request *request; /* borrowed, sockets's lifetime */
    void *app_user; /* hooks' per-connection context */
    bool reactor_mode; /* fairness/budget behavior */
    /* Owner thread: the authenticate hook handed its work to a worker and the
     * post-auth step waits for cf_cable_socket_complete_auth. Input is not
     * parsed before that (the worker wakes the socket when ready). */
    bool auth_pending;
    /* Owner thread: an on_text hook returned CF_BUSY; no further input is
     * read or parsed until cf_cable_socket_resume. Buffered bytes stay
     * charged to the aggregate input budget. */
    bool paused;
    size_t input_charged;  /* aggregate input bytes held (owner thread) */
    size_t output_charged; /* aggregate output bytes queued (owner thread) */
    size_t msg_charged;    /* part of input_charged held in s->msg */
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

/* Aggregate budget accounting: charged when bytes are buffered or queued,
 * released when they are consumed or sent. The accounting fields
 * (s->input_charged / s->output_charged) belong to the socket's owner
 * thread; the reactor totals are guarded by the reactor mutex. A standalone
 * socket (no reactor) makes all of these constant-time no-ops. */
static bool reactor_input_charge(cf_cable_socket *s, size_t n) {
    if (s->reactor == NULL || n == 0) return true;
    struct cable_reactor *r = s->reactor;
    bool ok = true;
    pthread_mutex_lock(&r->mutex);
    if (n > r->input_limit - r->input_used) {
        ok = false;
    } else {
        r->input_used += n;
        s->input_charged += n;
    }
    pthread_mutex_unlock(&r->mutex);
    return ok;
}

static void reactor_input_release(cf_cable_socket *s, size_t n) {
    if (s->reactor == NULL || n == 0) return;
    if (n > s->input_charged) n = s->input_charged;
    struct cable_reactor *r = s->reactor;
    pthread_mutex_lock(&r->mutex);
    r->input_used -= n;
    pthread_mutex_unlock(&r->mutex);
    s->input_charged -= n;
}

static bool reactor_output_charge(cf_cable_socket *s, size_t n) {
    if (s->reactor == NULL || n == 0) return true;
    struct cable_reactor *r = s->reactor;
    bool ok = true;
    pthread_mutex_lock(&r->mutex);
    if (n > r->output_limit - r->output_used) {
        ok = false;
    } else {
        r->output_used += n;
        s->output_charged += n;
    }
    pthread_mutex_unlock(&r->mutex);
    return ok;
}

static void reactor_output_release(cf_cable_socket *s, size_t n) {
    if (s->reactor == NULL || n == 0) return;
    if (n > s->output_charged) n = s->output_charged;
    struct cable_reactor *r = s->reactor;
    pthread_mutex_lock(&r->mutex);
    r->output_used -= n;
    pthread_mutex_unlock(&r->mutex);
    s->output_charged -= n;
}


static void socket_wake(struct cf_cable_socket *s) {
    if (s->reactor != NULL) {
        reactor_wake(s->reactor);
        return;
    }
    uint64_t one = 1;
    ssize_t n = write(s->wake_fd, &one, sizeof one);
    (void)n; /* EAGAIN: a wake is already pending */
}

static void socket_drain_wake(struct cf_cable_socket *s) {
    uint64_t v;
    while (read(s->wake_fd, &v, sizeof v) > 0) {
    }
}

/* One consistent snapshot of the queue state that senders mutate under the
 * socket mutex (out queue, pending bytes, progress clock, cap flag). The
 * owner loop reads it once per iteration; every access to those fields is
 * under the mutex. */
static void socket_queue_state(struct cf_cable_socket *s, size_t *pending,
                               bool *has_out, uint64_t *last_progress,
                               bool *cap_hit) {
    pthread_mutex_lock(&s->mutex);
    if (pending != NULL) *pending = s->pending_bytes;
    if (has_out != NULL) *has_out = s->out_head != NULL;
    if (last_progress != NULL) *last_progress = s->last_progress_ms;
    if (cap_hit != NULL) *cap_hit = s->queue_cap_hit;
    pthread_mutex_unlock(&s->mutex);
}

void cf_cable_socket_wake(cf_cable_socket *socket) {
    if (socket == NULL) return;
    socket_wake(socket);
}

bool cf_cable_socket_frame_partial(const cf_cable_socket *socket) {
    if (socket == NULL) return false;
    cf_cable_socket *s = (cf_cable_socket *)socket;
    pthread_mutex_lock(&s->mutex);
    struct cf_cable_out *head = s->out_head;
    bool partial = head != NULL && head->sent != 0 &&
                   head->sent < head->header_len + head->payload_len;
    pthread_mutex_unlock(&s->mutex);
    return partial;
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
 * retained reference) on every path. Returns CF_BUSY when the pending cap or
 * the reactor's aggregate output budget is reached (the connection closes)
 * and CF_IO when it already closed. */
static cf_err out_queue_node(struct cf_cable_socket *s, unsigned opcode,
                             cf_buf *payload, const unsigned char *header,
                             size_t header_len) {
    size_t len = cf_buf_span(payload).len;
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
    /* Aggregate reactor budget: charged under the socket mutex so a close
     * that drains the queue cannot interleave with this enqueue. */
    if (!reactor_output_charge(s, wire)) {
        s->queue_cap_hit = true;
        s->stats.over_budget_closes++;
        pthread_mutex_unlock(&s->mutex);
        cf_buf_release(payload);
        socket_wake(s);
        return CF_BUSY;
    }
    struct cf_cable_out *node = calloc(1, sizeof *node);
    if (node == NULL) {
        reactor_output_release(s, wire);
        pthread_mutex_unlock(&s->mutex);
        cf_buf_release(payload);
        return CF_NOMEM;
    }
    node->payload = payload;
    if (header_len != 0) memcpy(node->header, header, header_len);
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
    if (opcode != OP_RAW) s->stats.frames_queued++;
    pthread_mutex_unlock(&s->mutex);
    socket_wake(s);
    return CF_OK;
}

static cf_err out_queue(struct cf_cable_socket *s, unsigned opcode,
                        cf_buf *payload, bool compressed) {
    size_t len = cf_buf_span(payload).len;
    unsigned char header[10];
    size_t header_len = 0;
    frame_header(opcode, compressed, len, header, &header_len);
    return out_queue_node(s, opcode, payload, header, header_len);
}

/* Pre-serialized bytes (the 101 handshake response) go out before any
 * WebSocket frame; they are counted in the aggregate output budget but not
 * against the WebSocket pending-frame cap. */
static cf_err queue_raw(struct cf_cable_socket *s, const unsigned char *bytes,
                        size_t len) {
    if (len == 0) return CF_OK;
    cf_buf *payload = NULL;
    if (cf_buf_copy((cf_span){bytes, len}, &payload) != CF_OK) return CF_NOMEM;
    cf_err rc = out_queue_node(s, OP_RAW, payload, NULL, 0);
    if (rc != CF_OK) cf_buf_release(payload);
    return rc;
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

void cf_cable_socket_request_close(cf_cable_socket *socket) {
    if (socket == NULL) return;
    /* The reference close is a normal 1000 close frame after everything
     * already queued, then a bounded wait for the peer's close. Owner
     * thread only. */
    unsigned char close_code[2] = {0x03, 0xE8}; /* 1000 */
    (void)queue_control(socket, OP_CLOSE, (cf_span){close_code, 2});
    socket->closing = true;
    socket->wait_peer_close = true;
    socket->close_deadline_ms =
        cf_monotonic_ms() + socket->limits.close_timeout_ms;
}

static void node_finish_stats(struct cf_cable_socket *s,
                              struct cf_cable_out *node) {
    if (node->opcode == OP_RAW) return; /* the 101 response is not a frame */
    s->stats.frames_sent++;
    s->stats.bytes_sent += node->header_len + node->payload_len;
    if (node->opcode == OP_PING) s->stats.pings_sent++;
    if (node->opcode == OP_PONG) s->stats.pongs_sent++;
    if (node->opcode == OP_CLOSE) s->stats.close_frames_sent++;
}

/* The reactor is the sole TLS owner after the HTTP handoff. Preserve WANT
 * direction so epoll retries the same operation without spinning on writable
 * sockets when SSL_write needs input. */
static ssize_t socket_send(struct cf_cable_socket *s,
                           const unsigned char *bytes, size_t len) {
    if (s->tls == NULL) return send(s->fd, bytes, len, MSG_NOSIGNAL);
    size_t n = 0;
    cf_front_tls_step step = cf_front_tls_send(s->tls, bytes, len, &n);
    s->write_want_read = step == CF_FRONT_TLS_WANT_READ;
    if (step == CF_FRONT_TLS_DONE) return (ssize_t)n;
    errno = step == CF_FRONT_TLS_FAIL ? EIO : EAGAIN;
    return -1;
}

static ssize_t socket_recv(struct cf_cable_socket *s,
                           unsigned char *bytes, size_t len) {
    if (s->tls == NULL) return recv(s->fd, bytes, len, 0);
    size_t n = 0;
    cf_front_tls_step step = cf_front_tls_recv(s->tls, bytes, len, &n);
    s->read_want_write = step == CF_FRONT_TLS_WANT_WRITE;
    if (step == CF_FRONT_TLS_DONE) return (ssize_t)n;
    errno = step == CF_FRONT_TLS_FAIL ? EIO : EAGAIN;
    return -1;
}

/* Send as much of the queue as the socket accepts; called on the owner
 * thread. A positive send resets the stalled-write clock and releases the
 * aggregate output budget for the accepted bytes. */
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
            n = socket_send(s, node->header + node->sent,
                            node->header_len - node->sent);
        } else {
            size_t done = node->sent - node->header_len;
            const unsigned char *p = cf_buf_span(node->payload).ptr + done;
            n = socket_send(s, p, node->payload_len - done);
        }

        if (n > 0) {
            node->sent += (size_t)n;
            uint64_t now = cf_monotonic_ms();
            pthread_mutex_lock(&s->mutex);
            s->last_progress_ms = now;
            s->pending_bytes -= (size_t)n;
            pthread_mutex_unlock(&s->mutex);
            reactor_output_release(s, (size_t)n);
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
         * uncompressed and the attempt is counted. The counter is guarded by
         * the socket mutex because senders run on foreign threads. */
        pthread_mutex_lock(&socket->mutex);
        socket->stats.deflate_failures++;
        pthread_mutex_unlock(&socket->mutex);
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
    reactor_input_release(s, n);
    if (s->in_pos == s->in.len) {
        s->in_pos = 0;
        s->in.len = 0;
    } else if (s->in_pos >= 4096) {
        memmove(s->in.ptr, s->in.ptr + s->in_pos, s->in.len - s->in_pos);
        s->in.len -= s->in_pos;
        s->in_pos = 0;
    }
}

enum feed_result {
    FEED_OK = 0,
    FEED_PROTOCOL,
    FEED_CLOSED,
    FEED_ABORT,
    /* The on_text hook took the command's model work to a worker: stop
     * consuming frames (ordering) until the owner resumes the socket. */
    FEED_PAUSED
};

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
            cf_err hrc = s->hooks.on_text(s->hooks.on_text_user, s,
                                          (cf_span){data, len});
            /* CF_BUSY: the command's model work runs on a worker; the owner
             * must not parse the frames already buffered behind it until the
             * wiring resumes the socket (the hook copied what it needs). */
            if (hrc == CF_BUSY) result = FEED_PAUSED;
        }
    } else {
        /* The reference logs and ignores a binary message. */
        s->stats.binary_messages++;
    }
    cf_builder_dispose(&inflated);
    /* The assembled message bytes leave the aggregate input accounting; the
     * hook above observed them while they were charged. */
    reactor_input_release(s, s->msg_charged);
    s->msg_charged = 0;
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
        if (!reactor_input_charge(s, chunk)) return false;
        if (cf_builder_append(&s->msg, (cf_span){tmp, chunk}) != CF_OK) {
            reactor_input_release(s, chunk);
            return false;
        }
        s->msg_charged += chunk;
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
        ssize_t n = socket_send(s, bytes + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            uint64_t now = cf_monotonic_ms();
            if (now >= deadline_ms) return CF_IO;
            struct pollfd pf = {s->fd, s->write_want_read ? POLLIN : POLLOUT, 0};
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
    /* A reactor bounds one socket's read per serviced event so a busy peer
     * cannot starve the other sockets; level-triggered epoll reports the
     * rest. A standalone run drains until EAGAIN as before. */
    size_t budget =
        s->reactor_mode ? CF_CABLE_REACTOR_READ_CHUNK : (size_t)-1;
    while (budget != 0) {
        size_t want = sizeof buf;
        if (want > budget) want = budget;
        ssize_t n = socket_recv(s, buf, want);
        if (n > 0) {
            if (!reactor_input_charge(s, (size_t)n)) {
                s->stats.over_budget_closes++;
                s->transport_failed = true;
                return false;
            }
            if (cf_builder_append(&s->in, (cf_span){buf, (size_t)n}) !=
                CF_OK) {
                reactor_input_release(s, (size_t)n);
                s->transport_failed = true;
                return false;
            }
            budget -= (size_t)n;
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
    return true; /* read budget spent; the next event continues */
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
    /* Return every residual aggregate reservation: unsent queue bytes and
     * buffered input (the assembled message is part of input_charged). */
    reactor_output_release(s, s->output_charged);
    reactor_input_release(s, s->input_charged);
}

/* Allocate and initialize a socket. reactor != NULL selects the multiplexed
 * (reactor-serviced) transport; NULL is the standalone blocking driver.
 * Returns NULL when allocation or setup fails. */
static cf_cable_socket *socket_create(const cf_cable_socket_config *config,
                                      const cf_cable_hooks *hooks,
                                      const cf_cable_limits *limits,
                                      struct cable_reactor *reactor) {
    cf_cable_socket *s = calloc(1, sizeof *s);
    if (s == NULL) return NULL;
    s->fd = config->fd;
    s->tls = config->tls;
    s->deflate = config->deflate;
    s->reactor = reactor;
    s->reactor_mode = reactor != NULL;
    s->request = config->request;
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
        return NULL;
    }
    if (reactor == NULL) {
        s->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (s->wake_fd < 0) {
            pthread_mutex_destroy(&s->mutex);
            free(s);
            return NULL;
        }
    } else {
        s->wake_fd = -1;
    }
    /* The HTTP loop hands over a nonblocking fd; a direct caller may pass a
     * blocking one, which would defeat the poll loop. */
    int flags = fcntl(s->fd, F_GETFL, 0);
    if (flags >= 0 && (flags & O_NONBLOCK) == 0) {
        (void)fcntl(s->fd, F_SETFL, flags | O_NONBLOCK);
    }
    return s;
}

/* The owner-thread step once the connection's authentication resolved: the
 * wiring's on_open (C03 install gate), the reference welcome (or the
 * unauthorized disconnect + close), then the frames the client pipelined
 * behind the handshake. Shared by the synchronous authenticate path and the
 * asynchronous cf_cable_socket_complete_auth path. */
static cf_err socket_after_auth(cf_cable_socket *s, bool authenticated) {
    /* The wiring attaches its application loop here, while the socket is live
     * and before any output; refusing the connection takes the unauthorized
     * path (C03's connection install gate). */
    if (authenticated && s->hooks.on_open != NULL &&
        s->hooks.on_open(s->hooks.on_open_user, s) != CF_OK) {
        authenticated = false;
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

    /* Consume pipelined frames before waiting for new socket input. The hook
     * may pause the socket for its first worker-backed command; the rest of
     * the buffer stays charged and unparsed until resume. */
    if (s->in.len != 0 && reader_feed(s) == FEED_PAUSED) {
        s->paused = true;
    }
    s->auth_pending = false;
    return CF_OK;
}

cf_err cf_cable_socket_complete_auth(cf_cable_socket *socket,
                                     bool authenticated, int64_t user_id) {
    if (socket == NULL) return CF_INVALID;
    cf_cable_socket *s = socket;
    if (!s->auth_pending) return CF_OK; /* idempotent */
    (void)user_id; /* the identity lives on the socket's app_user */
    return socket_after_auth(s, authenticated);
}

void cf_cable_socket_resume(cf_cable_socket *socket) {
    if (socket == NULL) return;
    cf_cable_socket *s = socket;
    if (!s->paused) return;
    s->paused = false;
    if (s->auth_pending || s->closing || s->finished || s->transport_failed) {
        return;
    }
    if (s->in.len != 0 && reader_feed(s) == FEED_PAUSED) {
        s->paused = true; /* the next command is worker-backed too */
    }
}

/* Handshake, authentication, application open and the first frames. Runs once
 * on the owner thread. The 101 response precedes every WebSocket frame on the
 * wire; a standalone run writes it synchronously (as before), while a reactor
 * queues the pre-serialized bytes so a slow peer cannot stall the reactor.
 * An authenticate hook that returns CF_BUSY leaves the socket in the
 * auth_pending state: the welcome/unauthorized step then runs from
 * cf_cable_socket_complete_auth on this same thread. Returns a cf_err on a
 * fatal transport/hook failure; the caller finishes. */
static cf_err socket_boot(cf_cable_socket *s,
                          const cf_cable_socket_config *config) {
    uint64_t deadline = cf_monotonic_ms() + s->limits.close_timeout_ms;
    if (config->handshake_len != 0) {
        cf_err hrc;
        if (s->reactor != NULL) {
            hrc = queue_raw(s, config->handshake, config->handshake_len);
        } else {
            hrc = socket_write_all(s, config->handshake, config->handshake_len,
                                   deadline);
        }
        if (hrc != CF_OK) return CF_IO;
    }

    /* Bytes the client pipelined after the handshake request are consumed
     * before anything the socket reads later; they count against the
     * reactor's aggregate input budget from here on (HTTP released its own
     * accounting when the connection left its read/write state machine).
     * They are buffered in both the synchronous and asynchronous paths, so
     * the budget covers them while authentication is in flight. */
    if (config->pending_len != 0) {
        if (!reactor_input_charge(s, config->pending_len)) {
            s->stats.over_budget_closes++;
            return CF_LIMIT;
        }
        if (cf_builder_append(
                &s->in, (cf_span){config->pending,
                                  config->pending_len}) != CF_OK) {
            reactor_input_release(s, config->pending_len);
            return CF_NOMEM;
        }
    }

    bool authenticated = false;
    int64_t user_id = 0;
    if (s->hooks.authenticate != NULL) {
        cf_err rc = s->hooks.authenticate(s->hooks.authenticate_user, s,
                                          config->request, &authenticated,
                                          &user_id);
        if (rc == CF_BUSY) {
            /* The wiring submitted the model work to the app's bounded
             * worker pool and owns the result; it completes the connection
             * from the owner thread (service hook ->
             * cf_cable_socket_complete_auth). */
            s->auth_pending = true;
            return CF_OK;
        }
        if (rc != CF_OK) authenticated = false;
    }
    (void)user_id;
    return socket_after_auth(s, authenticated);
}

/* What the owner must wait for after one pump pass. */
typedef struct {
    int timeout_ms;
    bool want_write;
    /* False while an async authentication result or a paused command keeps
     * input from being parsed; the owner then ignores the fd's readability
     * (level-triggered epoll reports it again once reading resumes). */
    bool want_read;
} socket_wait;

/* One owner pass. revents == 0 is a tick: timeouts, ping cadence, the
 * per-socket service hook. wake reports a drained reactor/socket wake.
 * Returns true when the socket has finished (the caller flushes what is left
 * and tears down). *wait, when given, receives the next wait description. */
static bool socket_pump(cf_cable_socket *s, short revents, bool wake,
                        socket_wait *wait) {
    /* One locked snapshot per pass: the out queue, the pending bytes, the
     * progress clock and the cap flag are shared with foreign senders and are
     * only ever read or written under the socket mutex. */
    size_t pending = 0;
    bool has_out = false, cap_hit = false;
    uint64_t last_progress = 0;
    socket_queue_state(s, &pending, &has_out, &last_progress, &cap_hit);
    if (s->finished || s->transport_failed || cap_hit) return true;

    /* Consume pending revocation control even when nothing else is ready
     * (C03), then again right after a wake is observed. On the production
     * path this hook also installs finished worker results (authentication,
     * subscribe validation/effects) through the C03 gate. */
    if (s->hooks.service != NULL) {
        s->hooks.service(s->hooks.service_user, s);
    }
    if (s->finished || s->transport_failed) return true;

    uint64_t now = cf_monotonic_ms();
    if (s->closing) {
        if (s->wait_peer_close) {
            if (now >= s->close_deadline_ms) return true;
        } else if (pending == 0) {
            return true;
        }
    } else {
        if (pending > 0 &&
            now - last_progress >= s->limits.write_timeout_ms) {
            s->stats.write_timeout_closes++;
            return true;
        }
        if (s->limits.beat_interval_ms != 0 && now >= s->next_beat_ms) {
            cf_str ping = {0};
            if (cf_cable_ping(cf_now_us(NULL) / INT64_C(1000000), &ping) ==
                CF_OK) {
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

    if (wait != NULL) {
        int timeout = 1000;
        if (s->closing && s->wait_peer_close) {
            uint64_t left =
                s->close_deadline_ms > now ? s->close_deadline_ms - now : 0;
            if (left < (uint64_t)timeout) timeout = (int)left;
        } else if (!s->closing && pending > 0) {
            uint64_t left = last_progress + s->limits.write_timeout_ms;
            left = left > now ? left - now : 0;
            if (left < (uint64_t)timeout) timeout = (int)left;
        }
        if (timeout < 0) timeout = 0;
        wait->timeout_ms = timeout;
        wait->want_write = ((pending > 0 || has_out) &&
                            !s->write_want_read) || s->read_want_write;
        /* While an async authentication or a paused command owns the next
         * input frames, the owner must not parse (or read ahead) new ones. */
        wait->want_read = (!s->auth_pending && !s->paused) ||
                          s->write_want_read;
    }

    bool tls_pending = cf_front_tls_pending(s->tls);
    if (revents == 0 && !wake && !tls_pending) return false;

    if (wake) {
        if (s->reactor == NULL) socket_drain_wake(s);
        if (s->hooks.service != NULL) {
            s->hooks.service(s->hooks.service_user, s);
        }
        socket_queue_state(s, NULL, NULL, NULL, &cap_hit);
        if (cap_hit) return true;
    }
    if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
        /* Drain what was already buffered, then finish. */
        if (revents & POLLIN) {
            (void)socket_readable(s);
            (void)reader_feed(s);
        }
        return true;
    }
    if (((revents & POLLIN) || tls_pending ||
         ((revents & POLLOUT) && s->read_want_write)) &&
        !s->auth_pending && !s->paused) {
        if (socket_readable(s)) {
            if (reader_feed(s) == FEED_PAUSED) s->paused = true;
        }
    }
    socket_flush(s);
    return false;
}

/* Owner thread: flush what a finished socket still has, run on_close before
 * the memory is released, return the aggregate reservations and free it. */
static cf_err socket_finish(cf_cable_socket *s,
                            cf_cable_socket_stats *stats) {
    /* Detach anything that may still send to this socket (the C03 loop)
     * before the memory is released: after this returns no foreign send can
     * observe the freed socket. */
    if (s->hooks.on_close != NULL) {
        s->hooks.on_close(s->hooks.on_close_user, s);
    }
    socket_close_queue(s);
    if (stats != NULL) {
        /* Under the mutex: counters written by foreign senders (queued,
         * queue-cap, deflate failures) are synchronized with this read. */
        pthread_mutex_lock(&s->mutex);
        *stats = s->stats;
        pthread_mutex_unlock(&s->mutex);
    }
    pthread_mutex_destroy(&s->mutex);
    if (s->wake_fd >= 0) close(s->wake_fd);
    cf_builder_dispose(&s->in);
    cf_builder_dispose(&s->msg);
    free(s);
    return CF_OK;
}

void *cf_cable_socket_app_user(const cf_cable_socket *socket) {
    return socket != NULL ? socket->app_user : NULL;
}

void cf_cable_socket_set_app_user(cf_cable_socket *socket, void *user) {
    if (socket != NULL) socket->app_user = user;
}

const cf_cable_request *cf_cable_socket_request(const cf_cable_socket *socket) {
    return socket != NULL ? socket->request : NULL;
}

void *cf_cable_socket_transport_token(const cf_cable_socket *socket) {
    return socket != NULL ? (void *)socket->reactor : NULL;
}

cf_err cf_cable_socket_run(const cf_cable_socket_config *config,
                           const cf_cable_hooks *hooks,
                           const cf_cable_limits *limits,
                           cf_cable_socket_stats *stats) {
    if (stats != NULL) memset(stats, 0, sizeof *stats);
    if (config == NULL || config->fd < 0) return CF_INVALID;

    cf_cable_socket *s = socket_create(config, hooks, limits, NULL);
    if (s == NULL) return CF_NOMEM;

    cf_err result = CF_OK;
    if (socket_boot(s, config) != CF_OK) {
        result = CF_IO;
        goto done;
    }

    for (;;) {
        socket_wait w;
        if (socket_pump(s, 0, false, &w)) break;

        struct pollfd pf[2];
        pf[0].fd = s->fd;
        pf[0].events = w.want_read ? POLLIN : 0;
        if (w.want_write) pf[0].events |= POLLOUT;
        pf[0].revents = 0;
        pf[1].fd = s->wake_fd;
        pf[1].events = POLLIN;
        pf[1].revents = 0;
        int pr = poll(pf, 2, w.timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            s->transport_failed = true;
            break;
        }
        if (pr == 0) continue;

        bool wake = pf[1].revents != 0;
        if (socket_pump(s, pf[0].revents, wake, NULL)) break;
    }

done:;
    /* Complete the close handshake best-effort: flush what is left. */
    bool cap_hit_final = false;
    socket_queue_state(s, NULL, NULL, NULL, &cap_hit_final);
    if (!s->transport_failed && !cap_hit_final) socket_flush(s);
    socket_finish(s, stats);
    return result;
}

/* ---- server: the /cable front mount (server.rs Server::call) -------------- */

struct cable_conn {
    struct cable_conn *next, *prev;   /* the reactor's enrolled list */
    struct cable_conn *enroll_next;   /* enrollment queue link */
    struct cable_conn *server_next;   /* the server's live list */
    cf_cable_server *server;
    struct cable_reactor *reactor;
    cf_cable_socket *socket;          /* set on the reactor thread */
    uint32_t loop_index;              /* HTTP loop that handed it over */
    int fd;                           /* -1 once closed */
    cf_front_tls_conn *tls; /* owned only after successful TAKEN continuation */
    cf_http_upgrade_lease *lease;     /* lifetime connection-slot reservation */
    bool enrolled;                    /* linked into reactor->conns */
    bool registered;                  /* fd registered in the reactor epoll */
    uint32_t events;
    bool shutdown_done;
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

    /* One bounded reactor thread per HTTP loop index; never per connection. */
    size_t max_loops;
    size_t input_bytes, output_bytes; /* per-reactor aggregate limits */
    struct cable_reactor **reactors;
    size_t reactor_count;

    pthread_mutex_t mutex;
    pthread_cond_t idle_cv;
    struct cable_conn *conns;
    size_t live;
    bool stopping;
    bool stopped; /* reactors joined; only the free remains */
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
    acc->over_budget_closes += part->over_budget_closes;
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
    cf_front_tls_conn_destroy(conn->tls);
    free(conn->handshake);
    free(conn->pending);
    free(conn->strings);
    free(conn->headers);
    free(conn);
}

/* ---------------------------------------------------------- the reactor */

static void reactor_wake(struct cable_reactor *reactor) {
    uint64_t one = 1;
    ssize_t n = write(reactor->wake_fd, &one, sizeof one);
    (void)n; /* EAGAIN: a wake is already pending */
}

/* Keep the epoll interest in step with the socket's pending output and its
 * pause state. While an async authentication or a worker-backed command is in
 * flight the fd is not read (events without EPOLLIN): epoll still reports
 * ERR/HUP, and the worker's wake makes the reactor revisit the socket. */
static void reactor_update_events(struct cable_reactor *reactor,
                                  struct cable_conn *conn) {
    if (conn->socket == NULL || conn->fd < 0) return;
    size_t pending = 0;
    bool has_out = false;
    socket_queue_state(conn->socket, &pending, &has_out, NULL, NULL);
    uint32_t events = 0;
    if ((!conn->socket->auth_pending && !conn->socket->paused) ||
        conn->socket->write_want_read) {
        events |= EPOLLIN;
    }
    if (((pending > 0 || has_out) && !conn->socket->write_want_read) ||
        conn->socket->read_want_write) events |= EPOLLOUT;
    if (conn->registered) {
        if (events == conn->events) return;
        struct epoll_event e = {.events = events, .data.ptr = conn};
        if (epoll_ctl(reactor->epfd, EPOLL_CTL_MOD, conn->fd, &e) == 0) {
            conn->events = events;
        }
        return;
    }
    struct epoll_event e = {.events = events, .data.ptr = conn};
    if (epoll_ctl(reactor->epfd, EPOLL_CTL_ADD, conn->fd, &e) == 0) {
        conn->registered = true;
        conn->events = events;
    } else {
        /* Without a registration the socket can never be serviced again. */
        conn->socket->transport_failed = true;
    }
}

/* Owner thread (the reactor): stop, detach and free one connection. The
 * lifetime reservation is released exactly once here, whatever ended the
 * socket; with no lease (allocation failure or a direct hook caller) this
 * thread closes the descriptor itself. */
static void reactor_conn_finish(struct cable_reactor *reactor,
                                struct cable_conn *conn) {
    if (conn->enrolled) {
        if (conn->prev != NULL) {
            conn->prev->next = conn->next;
        } else {
            reactor->conns = conn->next;
        }
        if (conn->next != NULL) conn->next->prev = conn->prev;
        conn->enrolled = false;
    }
    if (conn->socket != NULL) {
        cf_cable_socket *s = conn->socket;
        bool cap_hit_final = false;
        socket_queue_state(s, NULL, NULL, NULL, &cap_hit_final);
        if (!s->transport_failed && !cap_hit_final) socket_flush(s);
        socket_finish(s, &conn->stats);
        conn->socket = NULL;
    }
    cf_front_tls_conn_destroy(conn->tls);
    conn->tls = NULL;
    if (conn->fd >= 0) {
        if (conn->registered) {
            (void)epoll_ctl(reactor->epfd, EPOLL_CTL_DEL, conn->fd, NULL);
            conn->registered = false;
        }
        if (conn->lease != NULL) {
            /* The HTTP loop thread closes the fd when it processes the
             * release, exactly once, and frees the connection slot. */
            conn->fd = -1;
            cf_http_upgrade_release(conn->lease);
            conn->lease = NULL;
        } else {
            close(conn->fd);
            conn->fd = -1;
        }
    }
    cf_cable_server *server = conn->server;
    pthread_mutex_lock(&server->mutex);
    {
        struct cable_conn **link = &server->conns;
        while (*link != NULL && *link != conn) link = &(*link)->server_next;
        if (*link == conn) *link = conn->server_next;
    }
    if (server->live != 0) server->live--;
    cf_cable_socket_stats_add(&server->totals, &conn->stats);
    pthread_cond_broadcast(&server->idle_cv);
    pthread_mutex_unlock(&server->mutex);
    conn_free(conn);
}

/* Reactor thread: boot and register one connection handed over by the HTTP
 * loop thread. */
static void reactor_enroll(struct cable_reactor *reactor,
                           struct cable_conn *conn) {
    cf_cable_server *server = reactor->server;
    bool stopping;
    pthread_mutex_lock(&reactor->mutex);
    stopping = reactor->stopping;
    pthread_mutex_unlock(&reactor->mutex);
    if (stopping) {
        reactor_conn_finish(reactor, conn);
        return;
    }
    cf_cable_socket_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.fd = conn->fd;
    cfg.tls = conn->tls;
    cfg.deflate = conn->deflate;
    cfg.subprotocol = conn->subprotocol;
    cfg.handshake = conn->handshake;
    cfg.handshake_len = conn->handshake_len;
    cfg.pending = conn->pending;
    cfg.pending_len = conn->pending_len;
    cfg.request = &conn->request;
    conn->socket = socket_create(&cfg, &server->hooks, &server->limits,
                                 reactor);
    if (conn->socket == NULL || socket_boot(conn->socket, &cfg) != CF_OK) {
        reactor_conn_finish(reactor, conn);
        return;
    }
    conn->enrolled = true;
    conn->prev = NULL;
    conn->next = reactor->conns;
    if (reactor->conns != NULL) reactor->conns->prev = conn;
    reactor->conns = conn;
    reactor_update_events(reactor, conn);
}

static void *reactor_main(void *arg) {
    struct cable_reactor *reactor = arg;
    struct cf_cable_server *server = reactor->server;
    struct epoll_event events[64];

    /* Owner-loop wiring (C03): register this loop's one preallocated control
     * slot before any connection is enrolled, and remove it after the last
     * socket is finished. A failure makes the loop refuse its enrollments
     * (stopping) rather than serve connections a revocation cannot reach. */
    if (server->hooks.owner_start != NULL) {
        cf_err rc = server->hooks.owner_start(
            server->hooks.owner_start_user, reactor, reactor_wake_token);
        if (rc == CF_OK) {
            pthread_mutex_lock(&reactor->mutex);
            reactor->owner_started = true;
            pthread_mutex_unlock(&reactor->mutex);
        } else {
            pthread_mutex_lock(&reactor->mutex);
            reactor->stopping = true;
            pthread_mutex_unlock(&reactor->mutex);
        }
    }

    for (;;) {
        /* Enrollments handed over by HTTP loop threads. */
        pthread_mutex_lock(&reactor->mutex);
        struct cable_conn *enroll = reactor->enroll_head;
        reactor->enroll_head = reactor->enroll_tail = NULL;
        bool stopping = reactor->stopping;
        pthread_mutex_unlock(&reactor->mutex);
        while (enroll != NULL) {
            struct cable_conn *next = enroll->enroll_next;
            enroll->enroll_next = NULL;
            reactor_enroll(reactor, enroll);
            enroll = next;
        }

        if (stopping) {
            for (struct cable_conn *c = reactor->conns; c != NULL;
                 c = c->next) {
                if (!c->shutdown_done && c->fd >= 0) {
                    shutdown(c->fd, SHUT_RDWR);
                    c->shutdown_done = true;
                    /* shutdown() makes the socket readable immediately even
                     * if no epoll event follows; consume it here so the
                     * socket observes EOF and finishes. */
                    if (c->socket != NULL) (void)socket_readable(c->socket);
                }
            }
        }

        /* One tick per socket: deadlines, ping cadence, C03 service. */
        int timeout = -1;
        for (struct cable_conn *c = reactor->conns; c != NULL;) {
            struct cable_conn *next = c->next;
            socket_wait w;
            memset(&w, 0, sizeof w);
            if (socket_pump(c->socket, 0, false, &w)) {
                reactor_conn_finish(reactor, c);
            } else {
                reactor_update_events(reactor, c);
                if (timeout < 0 || w.timeout_ms < timeout) {
                    timeout = w.timeout_ms;
                }
            }
            c = next;
        }
        if (stopping && reactor->conns == NULL) {
            pthread_mutex_lock(&reactor->mutex);
            bool more = reactor->enroll_head != NULL;
            pthread_mutex_unlock(&reactor->mutex);
            if (!more) break;
            continue;
        }

        int n = epoll_wait(reactor->epfd, events, 64, timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        bool wake = false;
        for (int i = 0; i < n; i++) {
            struct epoll_event *e = &events[i];
            if (e->data.ptr == reactor) { /* the wake eventfd */
                uint64_t v;
                while (read(reactor->wake_fd, &v, sizeof v) > 0) {
                }
                wake = true;
                continue;
            }
            struct cable_conn *c = e->data.ptr;
            if (!c->enrolled || c->socket == NULL) continue;
            if (socket_pump(c->socket, e->events, wake, NULL)) {
                reactor_conn_finish(reactor, c);
            }
        }
        if (wake && reactor->conns != NULL) {
            /* A foreign sender queued frames: flush every socket that now
             * has pending output (or is due a timeout). */
            for (struct cable_conn *c = reactor->conns; c != NULL;) {
                struct cable_conn *next = c->next;
                if (socket_pump(c->socket, 0, true, NULL)) {
                    reactor_conn_finish(reactor, c);
                }
                c = next;
            }
        }
    }

    if (reactor->owner_started && server->hooks.owner_stop != NULL) {
        server->hooks.owner_stop(server->hooks.owner_stop_user, reactor);
        reactor->owner_started = false;
    }
    return NULL;
}

/* The reactor start/stop, serialized by the server mutex for start. */
static struct cable_reactor *reactor_get(struct cf_cable_server *server,
                                         uint32_t loop_index) {
    if ((size_t)loop_index >= server->max_loops) return NULL;
    struct cable_reactor *reactor = server->reactors[loop_index];
    if (reactor != NULL) return reactor;
    reactor = calloc(1, sizeof *reactor);
    if (reactor == NULL) return NULL;
    reactor->server = server;
    reactor->loop_index = loop_index;
    reactor->input_limit = server->input_bytes;
    reactor->output_limit = server->output_bytes;
    reactor->epfd = -1;
    reactor->wake_fd = -1;
    if (pthread_mutex_init(&reactor->mutex, NULL) != 0) {
        free(reactor);
        return NULL;
    }
    reactor->epfd = epoll_create1(EPOLL_CLOEXEC);
    reactor->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (reactor->epfd >= 0 && reactor->wake_fd >= 0) {
        struct epoll_event e = {.events = EPOLLIN, .data.ptr = reactor};
        if (epoll_ctl(reactor->epfd, EPOLL_CTL_ADD, reactor->wake_fd, &e) !=
            0) {
            close(reactor->wake_fd);
            reactor->wake_fd = -1;
        }
    }
    if (reactor->epfd < 0 || reactor->wake_fd < 0 ||
        pthread_create(&reactor->thread, NULL, reactor_main, reactor) != 0) {
        if (reactor->epfd >= 0) close(reactor->epfd);
        if (reactor->wake_fd >= 0) close(reactor->wake_fd);
        pthread_mutex_destroy(&reactor->mutex);
        free(reactor);
        return NULL;
    }
    reactor->started = true;
    server->reactors[loop_index] = reactor;
    server->reactor_count++;
    return reactor;
}

static void reactor_stop_and_join(struct cable_reactor *reactor) {
    pthread_mutex_lock(&reactor->mutex);
    reactor->stopping = true;
    pthread_mutex_unlock(&reactor->mutex);
    reactor_wake(reactor);
    pthread_join(reactor->thread, NULL);
    if (reactor->epfd >= 0) close(reactor->epfd);
    if (reactor->wake_fd >= 0) close(reactor->wake_fd);
    pthread_mutex_destroy(&reactor->mutex);
    free(reactor);
}

/* The TAKEN continuation: enroll the connection on its HTTP loop's reactor
 * once the loop has fully detached the fd. Runs on the loop thread. */
static cf_err cable_conn_start(void *user, cf_http_upgrade_lease *lease) {
    struct cable_conn *conn = user;
    cf_cable_server *server = conn->server;
    if (lease == CF_HTTP_UPGRADE_REJECTED) {
        /* The loop refused the upgrade and already closed the fd/freed the
         * slot; only this hook's own state remains. */
        conn->tls = NULL;
        conn_free(conn);
        return CF_BUSY;
    }
    conn->lease = lease;
    pthread_mutex_lock(&server->mutex);
    if (server->stopping) {
        pthread_mutex_unlock(&server->mutex);
        cf_front_tls_conn_destroy(conn->tls);
        conn->tls = NULL;
        if (lease != NULL) {
            conn->fd = -1;
            cf_http_upgrade_release(lease);
            conn->lease = NULL;
        } else if (conn->fd >= 0) {
            close(conn->fd);
            conn->fd = -1;
        }
        conn_free(conn);
        return CF_BUSY;
    }
    struct cable_reactor *reactor = reactor_get(server, conn->loop_index);
    if (reactor == NULL) {
        pthread_mutex_unlock(&server->mutex);
        cf_front_tls_conn_destroy(conn->tls);
        conn->tls = NULL;
        if (lease != NULL) {
            conn->fd = -1;
            cf_http_upgrade_release(lease);
            conn->lease = NULL;
        } else if (conn->fd >= 0) {
            close(conn->fd);
            conn->fd = -1;
        }
        conn_free(conn);
        return CF_INTERNAL;
    }
    conn->server_next = server->conns;
    server->conns = conn;
    server->live++;
    pthread_mutex_lock(&reactor->mutex);
    conn->enroll_next = NULL;
    if (reactor->enroll_tail != NULL) {
        reactor->enroll_tail->enroll_next = conn;
    } else {
        reactor->enroll_head = conn;
    }
    reactor->enroll_tail = conn;
    /* Wake while the server mutex still pins the reactor: server stop joins
     * (and frees) reactors only while holding it, so this cannot touch a
     * freed reactor. */
    reactor_wake(reactor);
    pthread_mutex_unlock(&reactor->mutex);
    pthread_mutex_unlock(&server->mutex);
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
    server->max_loops = config->loops != 0 ? config->loops
                                           : CF_CABLE_MAX_REACTORS;
    if (server->max_loops > CF_CABLE_MAX_REACTORS) {
        server->max_loops = CF_CABLE_MAX_REACTORS;
    }
    server->reactors = calloc(server->max_loops, sizeof *server->reactors);
    if (server->reactors == NULL) {
        free(server->mount_path);
        free(server);
        return CF_NOMEM;
    }
    /* Process-wide budget divided across the loops, with a floor of two
     * message/pending caps per reactor so one maximum-size socket object can
     * never trip the aggregate bound by itself. */
    size_t in_total = config->input_bytes != 0 ? config->input_bytes
                                               : CF_CABLE_DEFAULT_REACTOR_BYTES;
    size_t out_total = config->output_bytes != 0
                           ? config->output_bytes
                           : CF_CABLE_DEFAULT_REACTOR_BYTES;
    server->input_bytes = in_total / server->max_loops;
    server->output_bytes = out_total / server->max_loops;
    if (server->input_bytes < 2 * CF_CABLE_MAX_MESSAGE) {
        server->input_bytes = 2 * CF_CABLE_MAX_MESSAGE;
    }
    if (server->output_bytes < 2 * CF_CABLE_MAX_PENDING_BYTES) {
        server->output_bytes = 2 * CF_CABLE_MAX_PENDING_BYTES;
    }
    if (config->allowed_request_origins_len != 0) {
        server->allowed_origins =
            calloc(config->allowed_request_origins_len,
                   sizeof *server->allowed_origins);
        if (server->allowed_origins == NULL) {
            free(server->reactors);
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
                free(server->reactors);
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
        free(server->reactors);
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
        free(server->reactors);
        free(server->mount_path);
        free(server);
        return CF_INTERNAL;
    }
    *out = server;
    return CF_OK;
}

void cf_cable_server_stop(cf_cable_server *server) {
    if (server == NULL) return;
    pthread_mutex_lock(&server->mutex);
    server->stopping = true;
    bool already = server->stopped;
    server->stopped = true;
    pthread_mutex_unlock(&server->mutex);
    if (already) return;
    /* Stop and join every reactor; each shuts its sockets down and releases
     * their lifetime reservations exactly once. The object stays alive: HTTP
     * loop threads may still be inside the upgrade hook until the caller has
     * joined them. */
    for (size_t i = 0; i < server->max_loops; i++) {
        if (server->reactors[i] != NULL) {
            reactor_stop_and_join(server->reactors[i]);
            server->reactors[i] = NULL;
        }
    }
}

void cf_cable_server_destroy(cf_cable_server *server) {
    if (server == NULL) return;
    cf_cable_server_stop(server);
    pthread_cond_destroy(&server->idle_cv);
    pthread_mutex_destroy(&server->mutex);
    for (size_t i = 0; i < server->allowed_origins_len; i++) {
        free(server->allowed_origins[i]);
    }
    free(server->allowed_origins);
    free(server->reactors);
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

size_t cf_cable_server_reactors(const cf_cable_server *server) {
    if (server == NULL) return 0;
    cf_cable_server *mutable_server = (cf_cable_server *)server;
    pthread_mutex_lock(&mutable_server->mutex);
    size_t count = server->reactor_count;
    pthread_mutex_unlock(&mutable_server->mutex);
    return count;
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
    conn->loop_index = request->loop_index;
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

    /* The connection is enrolled only after the loop reports the fd fully
     * detached (see cf_http_upgrade_request.taken), so the reactor can
     * register it without racing the HTTP epoll registration. */
    conn->tls = request->tls;
    request->taken = cable_conn_start;
    request->taken_user = conn;
    return CF_HTTP_UPGRADE_TAKEN;
}

/* ---- A01 session authentication ------------------------------------------- */

static int cookie_hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* params::decode_www_form_component (params.rs:510-535) plus the
 * String::from_utf8 fallback in parse_cookie_header (cookies.rs:282-285):
 * '+' is a space, '%' must be followed by two ASCII hex digits or the whole
 * decode fails, and a failed decode or a non-UTF-8 result keeps the raw
 * value.  On true `out` owns the result; false is allocation failure. */
static bool cookie_decode_component(cf_span raw, cf_str *out) {
    unsigned char *decoded = malloc(raw.len + 1);
    if (decoded == NULL) return false;
    size_t n = 0;
    bool malformed = false;
    for (size_t i = 0; i < raw.len;) {
        unsigned char c = raw.ptr[i];
        if (c == '+') {
            decoded[n++] = ' ';
            i++;
        } else if (c == '%') {
            int hi = i + 1 < raw.len ? cookie_hex_digit(raw.ptr[i + 1]) : -1;
            int lo = i + 2 < raw.len ? cookie_hex_digit(raw.ptr[i + 2]) : -1;
            if (hi < 0 || lo < 0) {
                malformed = true;
                break;
            }
            decoded[n++] = (unsigned char)((hi << 4) | lo);
            i += 3;
        } else {
            decoded[n++] = c;
            i++;
        }
    }
    if (!malformed && cable_utf8_valid(decoded, n)) {
        decoded[n] = '\0';
        *out = (cf_str){(char *)decoded, n};
        return true;
    }
    free(decoded);
    char *copy = malloc(raw.len + 1);
    if (copy == NULL) return false;
    if (raw.len != 0) memcpy(copy, raw.ptr, raw.len);
    copy[raw.len] = '\0';
    *out = (cf_str){copy, raw.len};
    return true;
}

/* Rack cookie parsing, byte-for-byte kit's parse_cookie_header
 * (cookies.rs:269-289): split on ';'; the first part is taken as-is and only
 * later parts lose leading ASCII spaces; an empty part contributes nothing;
 * the name is everything before the first '=' (or the whole part, with an
 * empty value, when there is no '='); the value is everything after it,
 * decoded by cookie_decode_component.  The first occurrence of `name` wins:
 * connection.rs:26-27 feeds every readable Cookie value, in header order, to
 * CookieJar::from_headers (cookies.rs:148-159), whose seen set keeps the
 * first pair per name across headers, and parse_cookie_header's own seen
 * guard (cookies.rs:279-281) does the same within one header.  A later
 * duplicate — in the same header or a later one — never overrides. */
static bool request_cookie_value(const cf_cable_request *request,
                                 const char *name, cf_str *out) {
    size_t name_len = strlen(name);
    for (size_t h = 0; h < request->header_count; h++) {
        if (!bytes_ieq_lit(request->headers[h].name, "cookie")) continue;
        cf_span v = request->headers[h].value;
        /* get_all("cookie").filter_map(to_str): an unreadable value is
         * dropped whole, so a later readable header still participates —
         * but only while no earlier occurrence has matched. */
        if (!cable_header_readable(v)) continue;
        size_t pos = 0;
        size_t part_index = 0;
        while (pos <= v.len) {
            size_t end = pos;
            while (end < v.len && v.ptr[end] != ';') end++;
            cf_span pair = {v.ptr + pos, end - pos};
            if (part_index > 0) {
                while (pair.len > 0 && pair.ptr[0] == ' ') {
                    pair.ptr++;
                    pair.len--;
                }
            }
            part_index++;
            pos = end + 1;
            if (pair.len == 0) continue;
            cf_span cname = pair;
            cf_span raw = {pair.ptr + pair.len, 0};
            for (size_t k = 0; k < pair.len; k++) {
                if (pair.ptr[k] == '=') {
                    cname = (cf_span){pair.ptr, k};
                    raw = (cf_span){pair.ptr + k + 1, pair.len - k - 1};
                    break;
                }
            }
            if (cname.len != name_len ||
                memcmp(cname.ptr, name, name_len) != 0) {
                continue;
            }
            cf_str value = {0};
            if (!cookie_decode_component(raw, &value)) return false;
            cf_str_dispose(out);
            *out = value;
            return true;
        }
    }
    return false;
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
