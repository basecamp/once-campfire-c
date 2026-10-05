/* H03 static front mount: ActionDispatch::Static / Rack::Files over the
 * pinned fixture tree (01-foundation-http.md "H03: exact route table and
 * assets"; docs/devel/evidence/assets-prep.md).
 *
 * Ported reference behavior (tmp/rust-ref/crates/assets/src/serve.rs):
 * clean_path (chomp one trailing '/', percent-decode, reject NUL,
 * Rack::Utils.clean_path_info), Rack::Mime content types, GET/HEAD only,
 * `Cache-Control: public, max-age=2592000`, the single pinned build
 * Last-Modified, If-Modified-Since equality -> 304 with no other headers,
 * and the compressible-type .br/.gz sibling probe with `Vary:
 * accept-encoding` / `Content-Encoding`. The fixture bodies are read from
 * the pinned tree with cf_response_file (H01 streams them); no path
 * traversal, no directory listing, no dynamic cache involvement, and the
 * root is never tmp/.
 */
#include "routes.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* BUILT_AT from docs/devel/evidence/assets-prep.md (SOURCE_DATE_EPOCH
 * 1790420614); the fixtures' single served Last-Modified. */
#define CF_ASSET_LAST_MODIFIED "Sat, 26 Sep 2026 11:03:34 GMT"
#define CF_ASSET_CACHE_CONTROL "public, max-age=2592000"

/* ------------------------------------------------------------ static root */

static char cf_root_buf[CF_STATIC_ROOT_MAX];
static const char *cf_root = CF_STATIC_ROOT;

const char *cf_static_root(void) { return cf_root; }

cf_err cf_static_set_root(const char *root) {
    if (root == NULL || root[0] == '\0') {
        cf_root = CF_STATIC_ROOT;
        return CF_OK;
    }
    size_t n = strlen(root);
    if (n >= CF_STATIC_ROOT_MAX) return CF_LIMIT;
    memcpy(cf_root_buf, root, n + 1);
    cf_root = cf_root_buf;
    return CF_OK;
}

/* --------------------------------------------------------------- helpers */

static cf_span cf_span_lit(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static bool cf_span_ieq_lit(cf_span a, const char *b) {
    size_t n = strlen(b);
    if (a.len != n) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = a.ptr[i];
        unsigned char d = (unsigned char)b[i];
        if (c >= 'A' && c <= 'Z') c += (unsigned char)('a' - 'A');
        if (d >= 'A' && d <= 'Z') d += (unsigned char)('a' - 'A');
        if (c != d) return false;
    }
    return true;
}

static bool cf_request_header(const cf_request *req, const char *name,
                              cf_span *out) {
    for (size_t i = 0; i < req->header_count; i++) {
        if (cf_span_ieq_lit(req->headers[i].name, name)) {
            *out = req->headers[i].value;
            return true;
        }
    }
    return false;
}

/* http 1.5.0 HeaderValue::to_str: HTAB or 0x20..=0x7E.  campfire app.rs
 * reads Accept-Encoding through `headers.get(..).and_then(|v| v.to_str().ok())`,
 * so an unreadable value is None and static serving falls back to the
 * identity file (`accept_encoding.unwrap_or("")`). */
static bool cf_header_value_readable(cf_span value) {
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (c != '\t' && (c < 0x20 || c > 0x7E)) return false;
    }
    return true;
}

static int cf_hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *cf_join3(const char *a, const char *b, const char *c) {
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    if (la > SIZE_MAX - lb - 1 || la + lb > SIZE_MAX - lc - 1) return NULL;
    char *out = malloc(la + lb + lc + 1);
    if (out == NULL) return NULL;
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    memcpy(out + la + lb, c, lc);
    out[la + lb + lc] = '\0';
    return out;
}

static bool cf_is_regular_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISREG(st.st_mode);
}

/* FileHandler#clean_path. CF_OK with *out NULL means "decline": the
 * reference returns None for a NUL byte. */
static cf_err cf_clean_path(cf_span raw, char **out) {
    *out = NULL;
    cf_span t = raw;
    if (t.len > 0 && t.ptr[t.len - 1] == '/') t.len--;

    /* URI::RFC2396_Parser#unescape (bytes, malformed escapes kept). */
    unsigned char *decoded = malloc(t.len == 0 ? 1 : t.len);
    if (decoded == NULL) return CF_NOMEM;
    size_t n = 0;
    for (size_t i = 0; i < t.len;) {
        if (t.ptr[i] == '%' && i + 2 < t.len) {
            int hi = cf_hex_value(t.ptr[i + 1]);
            int lo = cf_hex_value(t.ptr[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded[n++] = (unsigned char)((hi << 4) | lo);
                i += 3;
                continue;
            }
        }
        decoded[n++] = t.ptr[i];
        i++;
    }
    for (size_t i = 0; i < n; i++) {
        if (decoded[i] == 0) {
            free(decoded);
            return CF_OK; /* NUL: never servable */
        }
    }

    /* Rack::Utils.clean_path_info: drop ""/"." segments, ".." pops. */
    char *clean = malloc(n + 2);
    if (clean == NULL) {
        free(decoded);
        return CF_NOMEM;
    }
    bool leading = n == 0 || decoded[0] == '/';
    size_t m = 0;
    for (size_t i = 0;;) {
        size_t j = i;
        while (j < n && decoded[j] != '/') j++;
        size_t len = j - i;
        const unsigned char *part = decoded + i;
        if (len == 1 && part[0] == '.') {
            /* skip */
        } else if (len == 2 && part[0] == '.' && part[1] == '.') {
            if (m > 0) {
                while (m > 0 && clean[m - 1] != '/') m--;
                if (m > 0) m--;
            }
        } else if (len != 0) {
            if (m > 0) clean[m++] = '/';
            memcpy(clean + m, part, len);
            m += len;
        }
        if (j >= n) break;
        i = j + 1;
    }
    if (leading) {
        memmove(clean + 1, clean, m);
        clean[0] = '/';
        m++;
    }
    clean[m] = '\0';
    free(decoded);
    *out = clean;
    return CF_OK;
}

/* File.extname on bytes (leading dots are not extensions). */
static void cf_extname(const char *path, char *out, size_t cap) {
    const char *base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    size_t start = 0;
    while (base[start] == '.') start++;
    const char *trimmed = base + start;
    const char *dot = strrchr(trimmed, '.');
    out[0] = '\0';
    if (dot == NULL) return;
    size_t len = strlen(dot);
    if (len >= cap) len = cap - 1;
    memcpy(out, dot, len);
    out[len] = '\0';
}

/* Rack::Mime.mime_type for the served extensions, case-insensitive. */
static const char *cf_mime_type(const char *ext) {
    char lower[16];
    size_t len = strlen(ext);
    if (len >= sizeof lower) return NULL;
    for (size_t i = 0; i < len; i++) {
        char c = ext[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
    }
    lower[len] = '\0';
    if (strcmp(lower, ".avif") == 0) return "image/avif";
    if (strcmp(lower, ".css") == 0) return "text/css";
    if (strcmp(lower, ".csv") == 0) return "text/csv";
    if (strcmp(lower, ".gif") == 0) return "image/gif";
    if (strcmp(lower, ".gz") == 0) return "application/x-gzip";
    if (strcmp(lower, ".htm") == 0 || strcmp(lower, ".html") == 0) {
        return "text/html";
    }
    if (strcmp(lower, ".ico") == 0) return "image/vnd.microsoft.icon";
    if (strcmp(lower, ".jpeg") == 0 || strcmp(lower, ".jpg") == 0) {
        return "image/jpeg";
    }
    if (strcmp(lower, ".js") == 0 || strcmp(lower, ".mjs") == 0) {
        return "text/javascript";
    }
    if (strcmp(lower, ".json") == 0) return "application/json";
    if (strcmp(lower, ".m4a") == 0) return "audio/mp4a-latm";
    if (strcmp(lower, ".mp3") == 0) return "audio/mpeg";
    if (strcmp(lower, ".mp4") == 0) return "video/mp4";
    if (strcmp(lower, ".ogg") == 0) return "application/ogg";
    if (strcmp(lower, ".otf") == 0) return "font/otf";
    if (strcmp(lower, ".pdf") == 0) return "application/pdf";
    if (strcmp(lower, ".png") == 0) return "image/png";
    if (strcmp(lower, ".svg") == 0) return "image/svg+xml";
    if (strcmp(lower, ".ttf") == 0) return "font/ttf";
    if (strcmp(lower, ".txt") == 0) return "text/plain";
    if (strcmp(lower, ".wav") == 0) return "audio/x-wav";
    if (strcmp(lower, ".webm") == 0) return "video/webm";
    if (strcmp(lower, ".webp") == 0) return "image/webp";
    if (strcmp(lower, ".woff") == 0) return "font/woff";
    if (strcmp(lower, ".woff2") == 0) return "font/woff2";
    if (strcmp(lower, ".xml") == 0) return "application/xml";
    if (strcmp(lower, ".zip") == 0) return "application/zip";
    return NULL;
}

/* FileHandler's compressible_content_types regexp. */
static bool cf_compressible(const char *content_type) {
    return strncmp(content_type, "text/", 5) == 0 ||
           strncmp(content_type, "application/javascript", 22) == 0 ||
           strncmp(content_type, "image/svg+xml", 13) == 0;
}

static bool cf_word_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Rack's parsed accept_encoding `any? { |enc, _| /\b#{encoding}\b/i }`,
 * with str::match_indices' non-overlapping scan. */
static bool cf_accepts(const char *header, const char *encoding) {
    size_t enc_len = strlen(encoding);
    const char *p = header;
    while (*p != '\0') {
        const char *comma = strchr(p, ',');
        size_t part_len = comma != NULL ? (size_t)(comma - p) : strlen(p);
        const char *semi = memchr(p, ';', part_len);
        size_t val_len = semi != NULL ? (size_t)(semi - p) : part_len;
        while (val_len > 0 &&
               (unsigned char)p[0] <= ' ') { /* ASCII trim */
            p++;
            val_len--;
        }
        while (val_len > 0 && (unsigned char)p[val_len - 1] <= ' ') val_len--;
        size_t i = 0;
        while (i + enc_len <= val_len) {
            bool match = true;
            for (size_t k = 0; k < enc_len; k++) {
                unsigned char c = (unsigned char)p[i + k];
                if (c >= 'A' && c <= 'Z') c += (unsigned char)('a' - 'A');
                if (c != (unsigned char)encoding[k]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                unsigned char prev = i > 0 ? (unsigned char)p[i - 1] : 0;
                unsigned char next =
                    i + enc_len < val_len ? (unsigned char)p[i + enc_len] : 0;
                bool prev_word = prev != 0 && cf_word_char(prev);
                bool next_word = next != 0 && cf_word_char(next);
                if (!prev_word && !next_word) return true;
                i += enc_len; /* match_indices: non-overlapping */
                continue;
            }
            i++;
        }
        if (comma == NULL) break;
        p = comma + 1;
    }
    return false;
}

/* ------------------------------------------------------------ try_files */

struct cf_asset_hit {
    char *path;                  /* owned */
    const char *content_type;    /* static string */
    const char *content_encoding; /* NULL, "br" or "gzip" */
    bool vary;
};

static cf_err cf_try_files(const char *candidate, const char *content_type,
                           const char *accept_encoding,
                           struct cf_asset_hit *hit, bool *found) {
    *found = false;
    char *full = cf_join3(cf_static_root(), "/public", candidate);
    if (full == NULL) return CF_NOMEM;
    const char *encoding = NULL;
    bool vary = false;
    if (cf_compressible(content_type)) {
        static const struct {
            const char *encoding;
            const char *extension;
        } probes[2] = {{"br", ".br"}, {"gzip", ".gz"}};
        for (size_t k = 0; k < 2; k++) {
            char *sibling = cf_join3(full, probes[k].extension, "");
            if (sibling == NULL) {
                free(full);
                return CF_NOMEM;
            }
            if (cf_is_regular_file(sibling)) {
                vary = true;
                if (cf_accepts(accept_encoding, probes[k].encoding)) {
                    /* The compressed sibling is the body. */
                    encoding = probes[k].encoding;
                    free(full);
                    full = sibling;
                    break;
                }
            }
            free(sibling);
        }
    }
    if (encoding != NULL || cf_is_regular_file(full)) {
        hit->path = full;
        hit->content_type = content_type;
        hit->content_encoding = encoding;
        hit->vary = vary;
        *found = true;
        return CF_OK;
    }
    free(full);
    return CF_OK;
}

static cf_err cf_serve_hit(const cf_request *request, cf_response *resp,
                           const struct cf_asset_hit *hit) {
    cf_span ims;
    if (cf_request_header(request, "if-modified-since", &ims) &&
        ims.len == strlen(CF_ASSET_LAST_MODIFIED) &&
        memcmp(ims.ptr, CF_ASSET_LAST_MODIFIED, ims.len) == 0) {
        resp->status = 304;
        return CF_OK;
    }
    int fd = open(hit->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return CF_IO;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(fd);
        return CF_IO;
    }
    resp->status = 200;
    cf_err rc = cf_response_header(resp, cf_span_lit("Last-Modified"),
                                   cf_span_lit(CF_ASSET_LAST_MODIFIED));
    if (rc == CF_OK) {
        rc = cf_response_header(resp, cf_span_lit("Content-Type"),
                                cf_span_lit(hit->content_type));
    }
    if (rc == CF_OK) {
        rc = cf_response_header(resp, cf_span_lit("Cache-Control"),
                                cf_span_lit(CF_ASSET_CACHE_CONTROL));
    }
    if (rc == CF_OK && hit->vary) {
        rc = cf_response_header(resp, cf_span_lit("Vary"),
                                cf_span_lit("accept-encoding"));
    }
    if (rc == CF_OK && hit->content_encoding != NULL) {
        rc = cf_response_header(resp, cf_span_lit("Content-Encoding"),
                                cf_span_lit(hit->content_encoding));
    }
    if (rc != CF_OK) {
        close(fd);
        return rc;
    }
    rc = cf_response_file(resp, fd, 0, (uint64_t)st.st_size);
    if (rc != CF_OK) close(fd);
    return rc;
}

/* --------------------------------------------------------------- front */

cf_err cf_assets_serve(const cf_request *request, cf_response *response,
                       bool *handled) {
    if (request == NULL || response == NULL || handled == NULL) {
        return CF_INVALID;
    }
    *handled = false;
    cf_method method = request->method;
    if (method != CF_GET && method != CF_HEAD) return CF_OK;

    char *clean = NULL;
    cf_err rc = cf_clean_path(request->path, &clean);
    if (rc != CF_OK) return rc;
    if (clean == NULL) return CF_OK; /* NUL: Static declines, app routes */
    if (clean[0] != '/') { /* only the absolute URL space exists */
        free(clean);
        return CF_OK;
    }

    cf_span accept;
    char *accept_encoding = NULL;
    if (cf_request_header(request, "accept-encoding", &accept) &&
        cf_header_value_readable(accept)) {
        accept_encoding = malloc(accept.len + 1);
        if (accept_encoding == NULL) {
            free(clean);
            return CF_NOMEM;
        }
        memcpy(accept_encoding, accept.ptr, accept.len);
        accept_encoding[accept.len] = '\0';
    }

    char ext[16];
    cf_extname(clean, ext, sizeof ext);
    const char *mime = cf_mime_type(ext);
    bool known = mime != NULL;
    const char *content_type = known ? mime : "text/plain";

    struct cf_asset_hit hit;
    memset(&hit, 0, sizeof hit);
    bool found = false;
    rc = cf_try_files(clean, content_type, accept_encoding != NULL ?
                      accept_encoding : "", &hit, &found);

    /* Only paths without a resolvable extension also try .html and
     * /index.html (find_file's candidate order). */
    if (rc == CF_OK && !found && !known && strcmp(ext, ".html") != 0) {
        char *html = cf_join3(clean, ".html", "");
        if (html == NULL) {
            rc = CF_NOMEM;
        } else {
            rc = cf_try_files(html, "text/html", accept_encoding != NULL ?
                              accept_encoding : "", &hit, &found);
            free(html);
        }
    }
    if (rc == CF_OK && !found && !known && strcmp(ext, ".html") != 0) {
        char *index = cf_join3(clean, "/index.html", "");
        if (index == NULL) {
            rc = CF_NOMEM;
        } else {
            rc = cf_try_files(index, "text/html", accept_encoding != NULL ?
                              accept_encoding : "", &hit, &found);
            free(index);
        }
    }
    free(clean);
    free(accept_encoding);
    if (rc != CF_OK) return rc;
    if (!found) return CF_OK;

    rc = cf_serve_hit(request, response, &hit);
    free(hit.path);
    if (rc != CF_OK) return rc;
    *handled = true;
    return CF_OK;
}
