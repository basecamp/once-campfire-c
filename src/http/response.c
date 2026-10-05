/* H01 response ownership API (cf_response_*) and the HTTP/1.x serializer.
 *
 * Serializer policy (01 H01): status line, Content-Length (known file length
 * included), Date and Connection are generated here and serialized exactly
 * once, so a queued response's Date and cookies are immutable ("headers stay
 * stable in the pending response"). HEAD follows GET headers and sends no
 * body; 204/304 send no body; chunked is never used, so chunked and
 * Content-Length never appear together. Application header values must not
 * contain CR/LF; Set-Cookie lines stay separate because no merging exists.
 */
#include "http_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------ response objects */

struct cf_header_entry {
    unsigned char *name;
    size_t name_len;
    unsigned char *value;
    size_t value_len;
};

struct cf_headers {
    struct cf_header_entry *items;
    size_t count;
    size_t cap;
};

static void headers_dispose(cf_headers *h) {
    if (h == NULL) return;
    for (size_t i = 0; i < h->count; i++) {
        free(h->items[i].name);
        free(h->items[i].value);
    }
    free(h->items);
    free(h);
}

void cf_request_destroy(cf_request *req) {
    if (req == NULL) return;
    cf_buf_release(req->storage);
    req->storage = NULL;
    cf_params_destroy(req->params);
    req->params = NULL;
    memset(req, 0, sizeof *req);
}

void cf_response_init(cf_response *resp) {
    if (resp == NULL) return;
    memset(resp, 0, sizeof *resp);
    resp->file_fd = -1;
    resp->body_kind = CF_BODY_NONE;
}

void cf_response_dispose(cf_response *resp) {
    if (resp == NULL) return;
    if (resp->headers != NULL) {
        headers_dispose(resp->headers);
        resp->headers = NULL;
    }
    if (resp->body != NULL) {
        cf_buf_release(resp->body);
        resp->body = NULL;
    }
    if (resp->file_fd >= 0) {
        close(resp->file_fd);
    }
    resp->body_kind = CF_BODY_NONE;
    resp->file_fd = -1;
    resp->file_offset = 0;
    resp->file_length = 0;
    resp->status = 0;
    resp->close_after = false;
}

static bool header_name_ok(cf_span name) {
    if (name.len == 0 || name.ptr == NULL) return false;
    for (size_t i = 0; i < name.len; i++) {
        unsigned char c = name.ptr[i];
        if (c <= 0x20 || c >= 0x7f) return false;
        /* Token set is enforced by rejecting separators below. */
        if (strchr("()<>@,;:\\\"/[]?={}", (int)c) != NULL) return false;
    }
    return true;
}

static bool header_value_ok(cf_span value) {
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (c == '\t') continue;
        if (c < 0x20 || c == 0x7f) return false; /* CR/LF/NUL rejected */
    }
    return true;
}

cf_err cf_response_header(cf_response *resp, cf_span name, cf_span value) {
    if (resp == NULL || name.ptr == NULL || value.ptr == NULL) {
        return CF_INVALID;
    }
    if (!header_name_ok(name) || !header_value_ok(value)) return CF_INVALID;

    if (resp->headers == NULL) {
        resp->headers = calloc(1, sizeof *resp->headers);
        if (resp->headers == NULL) return CF_NOMEM;
    }
    cf_headers *h = resp->headers;
    if (h->count == h->cap) {
        size_t cap = h->cap == 0 ? 8 : h->cap * 2;
        if (cap < h->cap) return CF_LIMIT;
        struct cf_header_entry *items =
            realloc(h->items, cap * sizeof *items);
        if (items == NULL) return CF_NOMEM;
        h->items = items;
        h->cap = cap;
    }

    unsigned char *n = malloc(name.len);
    unsigned char *v = malloc(value.len == 0 ? 1 : value.len);
    if (n == NULL || v == NULL) {
        free(n);
        free(v);
        return CF_NOMEM;
    }
    memcpy(n, name.ptr, name.len);
    if (value.len != 0) memcpy(v, value.ptr, value.len);

    h->items[h->count].name = n;
    h->items[h->count].name_len = name.len;
    h->items[h->count].value = v;
    h->items[h->count].value_len = value.len;
    h->count++;
    return CF_OK;
}

cf_err cf_response_body(cf_response *resp, cf_buf *body) {
    if (resp == NULL || body == NULL) return CF_INVALID;
    cf_buf_release(resp->body);
    resp->body = NULL;
    if (resp->file_fd >= 0) {
        close(resp->file_fd);
        resp->file_fd = -1;
    }
    resp->body = cf_buf_retain(body);
    resp->body_kind = CF_BODY_BUFFER;
    resp->file_offset = 0;
    resp->file_length = (uint64_t)cf_buf_span(body).len;
    return CF_OK;
}

cf_err cf_response_file(cf_response *resp, int fd, uint64_t offset,
                        uint64_t len) {
    if (resp == NULL || fd < 0) return CF_INVALID;
    if (len > UINT64_MAX - offset) return CF_INVALID;
    cf_buf_release(resp->body);
    resp->body = NULL;
    if (resp->file_fd >= 0) close(resp->file_fd); /* replaces the prior body */
    resp->file_fd = fd; /* taken only on success */
    resp->body_kind = CF_BODY_FILE;
    resp->file_offset = offset;
    resp->file_length = len;
    return CF_OK;
}

/* ----------------------------------------------------------- serializer */

const char *cf_http_status_reason(unsigned status) {
    switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 417: return "Expectation Failed";
    case 422: return "Unprocessable Content";
    case 426: return "Upgrade Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "";
    }
}

static void format_http_date(char out[30], int64_t us) {
    time_t secs = (time_t)(us / INT64_C(1000000));
    if (us < 0 && us % INT64_C(1000000) != 0) secs--;
    struct tm tm;
    if (gmtime_r(&secs, &tm) == NULL) {
        memcpy(out, "Thu, 01 Jan 1970 00:00:00 GMT", 30);
        return;
    }
    if (strftime(out, 30, "%a, %d %b %Y %H:%M:%S GMT", &tm) == 0) {
        memcpy(out, "Thu, 01 Jan 1970 00:00:00 GMT", 30);
    }
}

static bool is_serializer_owned(const unsigned char *name, size_t len) {
    return cf_http_span_ieq(name, len, "content-length") ||
           cf_http_span_ieq(name, len, "transfer-encoding") ||
           cf_http_span_ieq(name, len, "connection") ||
           cf_http_span_ieq(name, len, "date") ||
           cf_http_span_ieq(name, len, "trailer");
}

static cf_err append_printf(cf_builder *b, const char *fmt, ...) {
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) return CF_LIMIT;
    return cf_builder_append(b, (cf_span){(const unsigned char *)tmp,
                                          (size_t)n});
}

cf_err cf_http_response_serialize(const cf_response *resp,
                                  const cf_request *req,
                                  cf_http_serialized *out) {
    if (out == NULL) return CF_INVALID;
    out->headers = NULL;
    out->send_body = false;
    out->body_length = 0;
    if (resp == NULL || req == NULL) return CF_INVALID;
    if (resp->status < 100 || resp->status > 599) return CF_INVALID;

    uint64_t content_length = 0;
    switch (resp->body_kind) {
    case CF_BODY_NONE:
        break;
    case CF_BODY_BUFFER: {
        if (resp->body == NULL) return CF_INVALID;
        cf_span s = cf_buf_span(resp->body);
        content_length = (uint64_t)s.len;
        break;
    }
    case CF_BODY_FILE:
        if (resp->file_fd < 0) return CF_INVALID;
        content_length = resp->file_length;
        break;
    default:
        return CF_INVALID;
    }

    bool head = req->method == CF_HEAD;
    bool no_body_status = resp->status == 204 || resp->status == 304 ||
                          resp->status < 200;
    bool send_body = !head && !no_body_status && content_length != 0;
    bool emit_length = !no_body_status;

    cf_builder b;
    memset(&b, 0, sizeof b);
    cf_err rc;

    const char *reason = cf_http_status_reason(resp->status);
    if (reason[0] != '\0') {
        rc = append_printf(&b, "HTTP/1.1 %u %s\r\n", resp->status, reason);
    } else {
        rc = append_printf(&b, "HTTP/1.1 %u\r\n", resp->status);
    }
    if (rc != CF_OK) goto fail;

    char date[30];
    format_http_date(date, cf_now_us(NULL));
    rc = append_printf(&b, "Date: %s\r\n", date);
    if (rc != CF_OK) goto fail;

    if (emit_length && resp->status >= 200) {
        rc = append_printf(&b, "Content-Length: %llu\r\n",
                           (unsigned long long)content_length);
        if (rc != CF_OK) goto fail;
    }

    bool close_after = resp->close_after || req->close_after;
    rc = append_printf(&b, "Connection: %s\r\n",
                       close_after ? "close" : "keep-alive");
    if (rc != CF_OK) goto fail;

    if (resp->headers != NULL) {
        for (size_t i = 0; i < resp->headers->count; i++) {
            const struct cf_header_entry *e = &resp->headers->items[i];
            if (is_serializer_owned(e->name, e->name_len)) continue;
            rc = cf_builder_append(&b, (cf_span){e->name, e->name_len});
            if (rc == CF_OK) {
                rc = cf_builder_append(&b, (cf_span){(const unsigned char *)": ",
                                                     2});
            }
            if (rc == CF_OK) {
                rc = cf_builder_append(&b, (cf_span){e->value, e->value_len});
            }
            if (rc == CF_OK) {
                rc = cf_builder_append(&b,
                                       (cf_span){(const unsigned char *)"\r\n",
                                                 2});
            }
            if (rc != CF_OK) goto fail;
        }
    }

    rc = append_printf(&b, "\r\n");
    if (rc != CF_OK) goto fail;

    cf_buf *headers = NULL;
    rc = cf_builder_freeze(&b, &headers);
    if (rc != CF_OK) {
        cf_builder_dispose(&b);
        return rc;
    }
    out->headers = headers;
    out->send_body = send_body;
    out->body_length = content_length;
    return CF_OK;

fail:
    cf_builder_dispose(&b);
    return rc;
}
