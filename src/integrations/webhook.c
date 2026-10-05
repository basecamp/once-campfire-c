/* src/integrations/webhook.c — I01 webhook delivery.
 *
 * Reference: integrations/webhook.rs (post, reply, mime_lookup, timed_out,
 * deliver_within) and the MIME table (Action Dispatch + turbo-rails).
 */
#include "integrations/webhook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *cf_webhook_err_name(cf_webhook_err e) {
    switch (e) {
    case CF_WEBHOOK_OK: return "ok";
    case CF_WEBHOOK_NOMEM: return "nomem";
    case CF_WEBHOOK_INVALID_URL: return "invalid-url";
    case CF_WEBHOOK_TRANSPORT: return "transport";
    case CF_WEBHOOK_INVALID_MIME: return "invalid-mime";
    case CF_WEBHOOK_REPLY_LARGE: return "reply-too-large";
    case CF_WEBHOOK_IO: return "io";
    }
    return "unknown";
}

void cf_webhook_default_config(cf_webhook_config *c) {
    memset(c, 0, sizeof *c);
    c->http.connect_timeout_ms = CF_WEBHOOK_ENDPOINT_TIMEOUT_SECS * 1000L;
    c->http.read_timeout_ms = CF_WEBHOOK_ENDPOINT_TIMEOUT_SECS * 1000L;
    c->http.deadline_ms = CF_WEBHOOK_DELIVERY_DEADLINE_SECS * 1000L;
    c->http.max_bytes = CF_WEBHOOK_MAX_REPLY_SIZE;
    c->http.length_cap = 0; /* no Content-Length gate; read up to the cap */
    c->http.ca_path = NULL;
}

void cf_webhook_delivery_dispose(cf_webhook_delivery *d) {
    if (!d) return;
    free(d->text);
    free(d->data);
    free(d->filename);
    free(d->content_type);
    memset(d, 0, sizeof *d);
}

/* --- Mime::Type.lookup -------------------------------------------------------
 * MIME_LOOKUP in the reference (key, symbol, to_s). Order matters only for
 * exact matches; keys are unique. Case-sensitive: "IMAGE/PNG" is NOT found
 * (falls through to the unregistered path and stays verbatim).
 */
typedef struct {
    const char *key, *symbol, *spelling;
} mime_entry;

static const mime_entry kMime[] = {
    {"text/html", "html", "text/html"},
    {"application/xhtml+xml", "html", "text/html"},
    {"text/plain", "text", "text/plain"},
    {"text/javascript", "js", "text/javascript"},
    {"application/javascript", "js", "text/javascript"},
    {"application/x-javascript", "js", "text/javascript"},
    {"text/css", "css", "text/css"},
    {"text/calendar", "ics", "text/calendar"},
    {"text/csv", "csv", "text/csv"},
    {"text/vcard", "vcf", "text/vcard"},
    {"text/vtt", "vtt", "text/vtt"},
    {"vtt", "vtt", "text/vtt"},
    {"text/markdown", "md", "text/markdown"},
    {"image/png", "png", "image/png"},
    {"image/jpeg", "jpeg", "image/jpeg"},
    {"image/gif", "gif", "image/gif"},
    {"image/bmp", "bmp", "image/bmp"},
    {"image/tiff", "tiff", "image/tiff"},
    {"image/svg+xml", "svg", "image/svg+xml"},
    {"image/webp", "webp", "image/webp"},
    {"video/mpeg", "mpeg", "video/mpeg"},
    {"audio/mpeg", "mp3", "audio/mpeg"},
    {"audio/ogg", "ogg", "audio/ogg"},
    {"audio/aac", "m4a", "audio/aac"},
    {"audio/mp4", "m4a", "audio/aac"},
    {"video/webm", "webm", "video/webm"},
    {"video/mp4", "mp4", "video/mp4"},
    {"font/otf", "otf", "font/otf"},
    {"font/ttf", "ttf", "font/ttf"},
    {"font/woff", "woff", "font/woff"},
    {"font/woff2", "woff2", "font/woff2"},
    {"application/xml", "xml", "application/xml"},
    {"text/xml", "xml", "application/xml"},
    {"application/x-xml", "xml", "application/xml"},
    {"application/rss+xml", "rss", "application/rss+xml"},
    {"application/atom+xml", "atom", "application/atom+xml"},
    {"application/x-yaml", "yaml", "application/x-yaml"},
    {"text/yaml", "yaml", "application/x-yaml"},
    {"multipart/form-data", "multipart_form", "multipart/form-data"},
    {"application/x-www-form-urlencoded", "url_encoded_form", "application/x-www-form-urlencoded"},
    {"application/json", "json", "application/json"},
    {"text/x-json", "json", "application/json"},
    {"application/jsonrequest", "json", "application/json"},
    {"application/problem+json", "json", "application/json"},
    {"application/pdf", "pdf", "application/pdf"},
    {"application/zip", "zip", "application/zip"},
    {"application/gzip", "gzip", "application/gzip"},
    {"application/x-gzip", "gzip", "application/gzip"},
    {"text/vnd.turbo-stream.html", "turbo_stream", "text/vnd.turbo-stream.html"},
};

/* Mime::Type::MIME_REGEXP (in Ruby, backslash-s inside the double-quoted
 * parameter pattern is a literal space): start, then either star-slash-star
 * or name, slash, star-or-name, then parameters, trailing blanks, end.
 * name = [a-zA-Z0-9][a-zA-Z0-9!#$&\-^_.+]{0,126}
 * param = space-star SEMICOLON space-star name, optional =value
 * value = name or a quoted string without CR or backslash */
static bool is_name_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static bool is_name_char(char c) {
    if (is_name_start(c)) return true;
    switch (c) {
    case '!': case '#': case '$': case '&': case '-': case '^': case '_': case '.': case '+': return true;
    default: return false;
    }
}

/* Parse a `name` at *pp (advancing past it). Returns length or 0. */
static size_t scan_name(const char **pp, const char *end) {
    const char *p = *pp;
    if (p >= end || !is_name_start(*p)) return 0;
    size_t n = 0;
    while (p < end && is_name_char(*p) && n < 127) {
        p++;
        n++;
    }
    /* A 128th+ name char fails the {0,126} bound. */
    if (p < end && is_name_char(*p)) return 0;
    *pp = p;
    return n;
}

static bool is_blank_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\x0b' || c == '\x0c' || c == '\r';
}

static bool mime_regexp_match(const char *s, size_t n) {
    const char *end = s + n;
    if (n == 3 && !memcmp(s, "*/*", 3)) return true;
    const char *p = s;
    if (scan_name(&p, end) == 0) return false;
    if (p >= end || *p != '/') return false;
    p++;
    if (p < end && *p == '*') {
        p++;
    } else {
        if (scan_name(&p, end) == 0) return false;
    }
    for (;;) {
        const char *q = p;
        while (q < end && *q == ' ') q++;
        if (q >= end || *q != ';') break;
        q++;
        while (q < end && *q == ' ') q++;
        const char *r = q;
        if (scan_name(&r, end) == 0) return false;
        q = r;
        if (q < end && *q == '=') {
            q++;
            if (q < end && *q == '"') {
                q++;
                while (q < end && *q != '"' && *q != '\r' && *q != '\\') q++;
                if (q >= end || *q != '"') return false;
                q++;
            } else {
                const char *v = q;
                if (scan_name(&v, end) == 0) return false;
                q = v;
            }
        }
        p = q;
    }
    while (p < end && is_blank_ws(*p)) p++;
    return p == end;
}

bool cf_webhook_mime_lookup(const char *content_type, const char **symbol, const char **registered_spelling) {
    static const char empty[] = "";
    if (!content_type) content_type = empty;
    for (size_t i = 0; i < sizeof kMime / sizeof kMime[0]; i++) {
        if (!strcmp(content_type, kMime[i].key)) {
            if (symbol) *symbol = kMime[i].symbol;
            if (registered_spelling) *registered_spelling = kMime[i].spelling;
            return true;
        }
    }
    /* Strip ";..." params with trailing blank trimming (reference trims
     * ' ', \t, \n, \x0b, \x0c, \r, \0 at the end). */
    const char *semi = strchr(content_type, ';');
    size_t n = semi ? (size_t)(semi - content_type) : strlen(content_type);
    while (n > 0) {
        char c = content_type[n - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\x0b' || c == '\x0c' || c == '\r' || c == '\0') n--;
        else break;
    }
    char *stripped = malloc(n + 1);
    if (!stripped) return false; /* OOM: caller treats as invalid; documented */
    memcpy(stripped, content_type, n);
    stripped[n] = '\0';
    for (size_t i = 0; i < sizeof kMime / sizeof kMime[0]; i++) {
        if (!strcmp(stripped, kMime[i].key)) {
            free(stripped);
            if (symbol) *symbol = kMime[i].symbol;
            if (registered_spelling) *registered_spelling = kMime[i].spelling;
            return true;
        }
    }
    bool ok = mime_regexp_match(stripped, n);
    free(stripped);
    if (!ok) return false;
    if (symbol) *symbol = NULL;
    if (registered_spelling) *registered_spelling = NULL; /* unregistered: keep verbatim */
    return true;
}

/* Lossy UTF-8 (String#from_utf8_lossy): invalid sequences become U+FFFD. */
static char *lossy_utf8(const unsigned char *p, size_t n) {
    /* Worst case: every byte becomes 3-byte U+FFFD. */
    char *o = malloc(n * 3 + 1);
    if (!o) return NULL;
    size_t w = 0, i = 0;
    while (i < n) {
        unsigned char c = p[i];
        size_t want = 0;
        uint32_t cp = 0;
        if (c < 0x80) {
            o[w++] = (char)c;
            i++;
            continue;
        } else if ((c & 0xe0) == 0xc0) {
            want = 2;
            cp = c & 0x1f;
        } else if ((c & 0xf0) == 0xe0) {
            want = 3;
            cp = c & 0x0f;
        } else if ((c & 0xf8) == 0xf0) {
            want = 4;
            cp = c & 0x07;
        } else {
            goto bad;
        }
        if (i + want > n) goto bad;
        bool ok = true;
        for (size_t k = 1; k < want; k++) {
            if ((p[i + k] & 0xc0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (p[i + k] & 0x3f);
        }
        if (!ok) goto bad;
        /* Overlong / surrogate / range checks. */
        bool valid = true;
        if (want == 2 && cp < 0x80) valid = false;
        else if (want == 3 && cp < 0x800) valid = false;
        else if (want == 4 && cp < 0x10000) valid = false;
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) valid = false;
        if (!valid) goto bad;
        memcpy(o + w, p + i, want);
        w += want;
        i += want;
        continue;
    bad:
        o[w++] = (char)0xef;
        o[w++] = (char)0xbf;
        o[w++] = (char)0xbd;
        i++;
    }
    o[w] = '\0';
    return o;
}

cf_webhook_err cf_webhook_classify(unsigned status, const char *content_type,
                                   const unsigned char *body, size_t body_len,
                                   cf_webhook_delivery *out) {
    memset(out, 0, sizeof *out);
    out->has_status = true;
    out->status = status;
    if (!content_type) {
        out->kind = CF_WEBHOOK_REPLY_NONE;
        return CF_WEBHOOK_OK;
    }
    if (status == 200 && (!strcmp(content_type, "text/html") || !strcmp(content_type, "text/plain"))) {
        out->kind = CF_WEBHOOK_REPLY_TEXT;
        out->text = lossy_utf8(body ? body : (const unsigned char *)"", body_len);
        if (!out->text) return CF_WEBHOOK_NOMEM;
        return CF_WEBHOOK_OK;
    }
    const char *symbol = NULL, *spelling = NULL;
    if (!cf_webhook_mime_lookup(content_type, &symbol, &spelling)) return CF_WEBHOOK_INVALID_MIME;
    const char *use_symbol = symbol ? symbol : "";
    const char *use_type = symbol ? spelling : content_type;
    out->kind = CF_WEBHOOK_REPLY_ATTACHMENT;
    out->filename = malloc(strlen("attachment.") + strlen(use_symbol) + 1);
    out->content_type = strdup(use_type);
    if (body_len) {
        out->data = malloc(body_len ? body_len : 1);
        if (out->data) memcpy(out->data, body, body_len);
    }
    if (!out->filename || !out->content_type || (body_len && !out->data)) {
        cf_webhook_delivery_dispose(out);
        return CF_WEBHOOK_NOMEM;
    }
    sprintf(out->filename, "attachment.%s", use_symbol);
    out->data_len = body_len;
    return CF_WEBHOOK_OK;
}

cf_webhook_err cf_webhook_deliver(const cf_webhook_config *cfg, const char *url,
                                  const unsigned char *payload, size_t payload_len,
                                  cf_webhook_delivery *out) {
    memset(out, 0, sizeof *out);
    if (!cfg || !url) return CF_WEBHOOK_IO;
    static const char *hdrs[] = {"Content-Type: application/json"};
    cf_http_response res;
    memset(&res, 0, sizeof res);
    long total_ms = cfg->http.deadline_ms;
    cf_http_err herr = cf_http_exchange(&cfg->http, "POST", url, hdrs, 1, payload, payload_len,
                                       NULL, 0, &res);
    if (herr == CF_HTTP_INVALID_URL || herr == CF_HTTP_INVALID_PART || herr == CF_HTTP_SCHEME) {
        return CF_WEBHOOK_INVALID_URL;
    }
    if (herr == CF_HTTP_TIMEOUT_CONN || herr == CF_HTTP_TIMEOUT_READ) {
        out->timed_out = true;
        out->timeout_secs = CF_WEBHOOK_ENDPOINT_TIMEOUT_SECS;
        return CF_WEBHOOK_OK;
    }
    if (herr == CF_HTTP_TIMEOUT_ALL) {
        out->timed_out = true;
        out->timeout_secs = (unsigned)(total_ms / 1000);
        return CF_WEBHOOK_OK;
    }
    if (herr == CF_HTTP_TOO_LARGE) return CF_WEBHOOK_REPLY_LARGE;
    if (herr != CF_HTTP_OK) {
        cf_http_response_dispose(&res);
        return (herr == CF_HTTP_NOMEM) ? CF_WEBHOOK_NOMEM : CF_WEBHOOK_TRANSPORT;
    }
    char *ct = cf_http_content_type(&res);
    /* The classifier takes the stripped media type (reference
     * Response#content_type): "text/plain; charset=utf-8" classifies as
     * text, while "Text/Plain" stays verbatim into the MIME table. */
    cf_webhook_err werr = cf_webhook_classify(res.status, ct, res.body, res.body_len, out);
    free(ct);
    cf_http_response_dispose(&res);
    return werr;
}
