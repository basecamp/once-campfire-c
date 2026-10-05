/* src/richtext/rt_uri.c — Ruby URI.parse (uri 1.1, RFC 3986) for the pieces
 * the opengraph URL checks and tweet-URL normalization use.
 *
 * Ports tmp/rust-ref/crates/richtext/src/uri.rs exactly, including which
 * inputs raise URI::InvalidURIError and URI::InvalidComponentError.
 */
#include "richtext/internal.h"

#include <arpa/inet.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char *dup_range(const unsigned char *bytes, size_t start, size_t end) {
    size_t len = end - start;
    char *copy = malloc(len + 1);
    if (copy == NULL) return NULL;
    if (len != 0) memcpy(copy, bytes + start, len);
    copy[len] = '\0';
    return copy;
}

static char *dup_cstr(const char *text) {
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, text, len + 1);
    return copy;
}

void rt_uri_dispose(rt_uri *uri) {
    if (uri == NULL) return;
    free(uri->scheme);
    free(uri->userinfo);
    free(uri->host);
    free(uri->path);
    free(uri->opaque);
    free(uri->query);
    free(uri->fragment);
    memset(uri, 0, sizeof *uri);
}

static uint64_t default_port(const char *scheme) {
    if (scheme == NULL) return 0;
    if (strcasecmp(scheme, "http") == 0 || strcasecmp(scheme, "ws") == 0) return 80;
    if (strcasecmp(scheme, "https") == 0 || strcasecmp(scheme, "wss") == 0) return 443;
    if (strcasecmp(scheme, "ftp") == 0) return 21;
    if (strcasecmp(scheme, "ldap") == 0) return 389;
    if (strcasecmp(scheme, "ldaps") == 0) return 636;
    return 0;
}

bool rt_uri_is_http(const rt_uri *uri) {
    return uri->scheme != NULL &&
           (strcasecmp(uri->scheme, "http") == 0 || strcasecmp(uri->scheme, "https") == 0);
}

rt_status rt_uri_to_s(const rt_uri *uri, rt_buf *out) {
    if (uri->scheme != NULL) {
        if (rt_buf_puts(out, uri->scheme) != RT_OK || rt_buf_putc(out, ':') != RT_OK) return RT_NOMEM;
    }
    if (uri->opaque != NULL) {
        if (rt_buf_puts(out, uri->opaque) != RT_OK) return RT_NOMEM;
    } else {
        if (uri->host != NULL ||
            (uri->scheme != NULL &&
             (strcmp(uri->scheme, "file") == 0 || strcmp(uri->scheme, "postgres") == 0))) {
            if (rt_buf_puts(out, "//") != RT_OK) return RT_NOMEM;
        }
        if (uri->userinfo != NULL) {
            if (rt_buf_puts(out, uri->userinfo) != RT_OK || rt_buf_putc(out, '@') != RT_OK) {
                return RT_NOMEM;
            }
        }
        if (uri->host != NULL && rt_buf_puts(out, uri->host) != RT_OK) return RT_NOMEM;
        if (uri->has_port && uri->port != default_port(uri->scheme)) {
            if (rt_buf_printf(out, ":%llu", (unsigned long long)uri->port) != RT_OK) return RT_NOMEM;
        }
        if (uri->path != NULL && rt_buf_puts(out, uri->path) != RT_OK) return RT_NOMEM;
        if (uri->query != NULL) {
            if (rt_buf_putc(out, '?') != RT_OK || rt_buf_puts(out, uri->query) != RT_OK) {
                return RT_NOMEM;
            }
        }
    }
    if (uri->fragment != NULL) {
        if (rt_buf_putc(out, '#') != RT_OK || rt_buf_puts(out, uri->fragment) != RT_OK) return RT_NOMEM;
    }
    return RT_OK;
}

/* --- RFC 3986 matching ---------------------------------------------------- */

static bool is_unreserved_or_sub(unsigned char b) {
    return b == '!' || (b >= '$' && b <= '.') || (b >= '0' && b <= '9') || b == ';' || b == '=' ||
           (b >= 'A' && b <= 'Z') || b == '_' || (b >= 'a' && b <= 'z') || b == '~';
}

static bool is_hex(unsigned char b) {
    return (b >= '0' && b <= '9') || (b >= 'a' && b <= 'f') || (b >= 'A' && b <= 'F');
}

static bool pct_at(const unsigned char *s, size_t len, size_t i) {
    return i + 2 < len && s[i] == '%' && is_hex(s[i + 1]) && is_hex(s[i + 2]);
}

static size_t take_while_class(const unsigned char *s, size_t len, size_t i, bool (*class_fn)(unsigned char)) {
    while (true) {
        if (pct_at(s, len, i)) {
            i += 3;
        } else if (i < len && class_fn(s[i])) {
            i++;
        } else {
            return i;
        }
    }
}

static bool seg_char(unsigned char b) {
    return is_unreserved_or_sub(b) || b == ':' || b == '@' || b == '/';
}

static bool seg_nc_char(unsigned char b) {
    return is_unreserved_or_sub(b) || b == '@';
}

static bool fragment_char(unsigned char b) {
    return is_unreserved_or_sub(b) || b == ':' || b == '@' || b == '/' || b == '?';
}

static bool userinfo_char(unsigned char b) {
    return is_unreserved_or_sub(b) || b == ':';
}

/* A bracketed IP literal; returns its end (past the ']') or 0. */
static size_t ip_literal(const unsigned char *s, size_t len, size_t i) {
    if (i >= len || s[i] != '[') return 0;
    size_t close = 0;
    for (size_t j = i; j < len; j++) {
        if (s[j] == ']') {
            close = j;
            break;
        }
    }
    if (close == 0) return 0;
    size_t inner_start = i + 1;
    size_t inner_len = close - inner_start;
    bool valid = false;
    if (inner_len >= 2 && (s[inner_start] == 'v' || s[inner_start] == 'V')) {
        size_t dot = 0;
        bool found_dot = false;
        for (size_t j = inner_start + 1; j < close; j++) {
            if (s[j] == '.') {
                dot = j;
                found_dot = true;
                break;
            }
        }
        if (found_dot && dot > inner_start + 1) {
            bool hex_ok = true;
            for (size_t j = inner_start + 1; j < dot; j++) {
                if (!is_hex(s[j])) hex_ok = false;
            }
            bool rest_ok = dot + 1 < close;
            for (size_t j = dot + 1; j < close && rest_ok; j++) {
                if (!(is_unreserved_or_sub(s[j]) || s[j] == ':')) rest_ok = false;
            }
            valid = hex_ok && rest_ok;
        }
    } else {
        /* std::net::Ipv6Addr::parse == inet_pton(AF_INET6); both reject '%'
         * zone ids. */
        char *inner = dup_range(s, inner_start, close);
        if (inner == NULL) return 0;
        struct in6_addr address;
        valid = inet_pton(AF_INET6, inner, &address) == 1;
        free(inner);
    }
    return valid ? close + 1 : 0;
}

static rt_uri_status escape_query(const unsigned char *bytes, size_t len, char **out) {
    rt_buf cleaned;
    rt_buf_init(&cleaned);
    for (size_t i = 0; i < len; i++) {
        if (bytes[i] == '\t' || bytes[i] == '\r' || bytes[i] == '\n') continue;
        if (rt_buf_putc(&cleaned, bytes[i]) != RT_OK) {
            rt_buf_dispose(&cleaned);
            return RT_URI_INVALID_URI;
        }
    }
    for (size_t i = 0; i + 2 < cleaned.len; i++) {
        if (cleaned.data[i] == '%' && !is_hex(cleaned.data[i + 1]) && !is_hex(cleaned.data[i + 2])) {
            rt_buf_dispose(&cleaned);
            return RT_URI_INVALID_URI;
        }
    }
    rt_buf out_buf;
    rt_buf_init(&out_buf);
    for (size_t i = 0; i < cleaned.len; i++) {
        unsigned char b = cleaned.data[i];
        bool escape = i + 2 < cleaned.len && b == '%' && is_hex(cleaned.data[i + 1]) &&
                      is_hex(cleaned.data[i + 2]);
        bool plain = (b >= '!' && b <= '&') || (b >= '(' && b <= ';') || b == '=' ||
                     (b >= '?' && b <= '_') || (b >= 'a' && b <= '~');
        if (escape || plain) {
            if (rt_buf_putc(&out_buf, b) != RT_OK) {
                rt_buf_dispose(&out_buf);
                rt_buf_dispose(&cleaned);
                return RT_URI_INVALID_URI;
            }
        } else {
            if (rt_buf_printf(&out_buf, "%%%02X", (unsigned)b) != RT_OK) {
                rt_buf_dispose(&out_buf);
                rt_buf_dispose(&cleaned);
                return RT_URI_INVALID_URI;
            }
        }
    }
    rt_buf_dispose(&cleaned);
    rt_status rc = rt_buf_to_cstr(&out_buf, out);
    rt_buf_dispose(&out_buf);
    return rc == RT_OK ? RT_URI_OK : RT_URI_INVALID_URI;
}

/* URI::MailTo validation: /\A(?:[^@,;]+@[^@,;]+(?:\z|[,;]))*\z/. */
static bool mailto_to_valid(const char *to) {
    const unsigned char *bytes = (const unsigned char *)to;
    size_t len = strlen(to);
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && bytes[i] != '@' && bytes[i] != ',' && bytes[i] != ';') i++;
        if (i == start || i >= len || bytes[i] != '@') return false;
        i++;
        size_t domain = i;
        while (i < len && bytes[i] != '@' && bytes[i] != ',' && bytes[i] != ';') i++;
        if (i == domain) return false;
        if (i < len) {
            if (bytes[i] == '@') return false;
            i++;
        }
    }
    return true;
}

static rt_uri_status check_scheme_class(const rt_uri *uri) {
    if (uri->scheme == NULL) return RT_URI_OK;
    char upper[16];
    size_t len = strlen(uri->scheme);
    if (len >= sizeof upper) return RT_URI_OK;
    for (size_t i = 0; i <= len; i++) {
        char c = uri->scheme[i];
        upper[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    if (strcmp(upper, "MAILTO") == 0) {
        rt_buf combined;
        rt_buf_init(&combined);
        if (uri->opaque != NULL) {
            rt_buf_puts(&combined, uri->opaque);
        } else if (uri->query != NULL) {
            rt_buf_putc(&combined, '?');
            rt_buf_puts(&combined, uri->query);
        } else {
            rt_buf_dispose(&combined);
            return RT_URI_INVALID_COMPONENT;
        }
        size_t to_len = combined.len;
        for (size_t i = 0; i < combined.len; i++) {
            if (combined.data[i] == '?') {
                to_len = i;
                break;
            }
        }
        char *to = malloc(to_len + 1);
        if (to == NULL) {
            rt_buf_dispose(&combined);
            return RT_URI_INVALID_URI;
        }
        memcpy(to, combined.data, to_len);
        to[to_len] = '\0';
        bool valid = mailto_to_valid(to);
        free(to);
        rt_buf_dispose(&combined);
        return valid ? RT_URI_OK : RT_URI_INVALID_COMPONENT;
    }
    if (strcmp(upper, "LDAP") == 0 || strcmp(upper, "LDAPS") == 0) {
        return (uri->fragment != NULL || uri->path == NULL) ? RT_URI_INVALID_URI : RT_URI_OK;
    }
    if (strcmp(upper, "FTP") == 0) {
        return uri->path == NULL ? RT_URI_INVALID_URI : RT_URI_OK;
    }
    return RT_URI_OK;
}

struct tail {
    size_t hier_end;
    bool has_query;
    size_t query_start, query_end;
    bool has_fragment;
    size_t fragment_start;
};

static bool parse_query_fragment_positions(const unsigned char *s, size_t len, size_t start,
                                           struct tail *out) {
    size_t hier_end = len;
    for (size_t i = start; i < len; i++) {
        if (s[i] == '?' || s[i] == '#') {
            hier_end = i;
            break;
        }
    }
    size_t i = hier_end;
    out->has_query = false;
    out->has_fragment = false;
    if (i < len && s[i] == '?') {
        size_t q_end = len;
        for (size_t j = i + 1; j < len; j++) {
            if (s[j] == '#') {
                q_end = j;
                break;
            }
        }
        out->has_query = true;
        out->query_start = i + 1;
        out->query_end = q_end;
        i = q_end;
    }
    if (i < len && s[i] == '#') {
        size_t f_end = take_while_class(s, len, i + 1, fragment_char);
        if (f_end != len) return false;
        out->has_fragment = true;
        out->fragment_start = i + 1;
        i = len;
    }
    out->hier_end = hier_end;
    return i == len;
}

struct authority {
    bool has_userinfo;
    size_t userinfo_start, userinfo_end;
    size_t host_start, host_end;
    bool has_port;
    uint64_t port;
    size_t end;
};

static bool parse_authority(const unsigned char *value, size_t len, size_t start, struct authority *out) {
    size_t i = start;
    out->has_userinfo = false;
    out->has_port = false;
    out->port = 0;
    size_t ui_end = take_while_class(value, len, i, userinfo_char);
    if (ui_end < len && value[ui_end] == '@') {
        out->has_userinfo = true;
        out->userinfo_start = i;
        out->userinfo_end = ui_end;
        i = ui_end + 1;
    }
    size_t host_end = ip_literal(value, len, i);
    if (host_end == 0) host_end = take_while_class(value, len, i, is_unreserved_or_sub);
    out->host_start = i;
    out->host_end = host_end;
    size_t end = host_end;
    if (end < len && value[end] == ':') {
        size_t digits_end = len;
        for (size_t j = end + 1; j < len; j++) {
            if (!(value[j] >= '0' && value[j] <= '9')) {
                digits_end = j;
                break;
            }
        }
        if (digits_end > end + 1) {
            uint64_t port = 0;
            bool overflow = false;
            for (size_t j = end + 1; j < digits_end; j++) {
                unsigned digit = (unsigned)(value[j] - '0');
                if (port > (UINT64_MAX - digit) / 10) overflow = true;
                else port = port * 10 + digit;
            }
            out->has_port = true;
            out->port = overflow ? UINT64_MAX : port;
        }
        end = digits_end;
    }
    if (end < len && value[end] != '/') return false;
    out->end = end;
    return true;
}

static bool split_absolute(const unsigned char *s, size_t len, rt_uri *uri) {
    if (len == 0 || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) return false;
    size_t i = 1;
    while (i < len && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
                       (s[i] >= '0' && s[i] <= '9') || s[i] == '+' || s[i] == '-' || s[i] == '.')) {
        i++;
    }
    if (i >= len || s[i] != ':') return false;
    size_t rest_start = i + 1;
    struct tail tail;
    if (!parse_query_fragment_positions(s, len, rest_start, &tail)) return false;
    /* URI::Generic#set_scheme downcases it. */
    uri->scheme = dup_range(s, 0, i);
    if (uri->scheme == NULL) return false;
    for (char *p = uri->scheme; *p != '\0'; p++) {
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    }
    if (tail.has_query) {
        char *escaped = NULL;
        if (escape_query(s + tail.query_start, tail.query_end - tail.query_start, &escaped) != RT_URI_OK) {
            return false;
        }
        uri->query = escaped;
    }
    if (tail.has_fragment) {
        uri->fragment = dup_range(s, tail.fragment_start, len);
        if (uri->fragment == NULL) return false;
    }
    size_t hier_start = rest_start;
    size_t hier_end = tail.hier_end;
    if (hier_end - hier_start >= 2 && s[hier_start] == '/' && s[hier_start + 1] == '/') {
        struct authority authority;
        if (!parse_authority(s, tail.hier_end, hier_start + 2, &authority)) return false;
        if (authority.has_userinfo) {
            uri->userinfo = dup_range(s, authority.userinfo_start, authority.userinfo_end);
            if (uri->userinfo == NULL) return false;
        }
        uri->host = dup_range(s, authority.host_start, authority.host_end);
        if (uri->host == NULL) return false;
        uri->has_port = authority.has_port;
        uri->port = authority.port;
        uri->path = dup_range(s, authority.end, hier_end);
        if (uri->path == NULL) return false;
        if (authority.end != hier_end) {
            if (s[authority.end] != '/' || take_while_class(s, hier_end, authority.end, seg_char) != hier_end) {
                return false;
            }
        }
    } else if (hier_end > hier_start && s[hier_start] == '/') {
        if (take_while_class(s, hier_end, hier_start, seg_char) != hier_end) return false;
        uri->path = dup_range(s, hier_start, hier_end);
        if (uri->path == NULL) return false;
    } else if (hier_end > hier_start) {
        if (take_while_class(s, hier_end, hier_start, seg_char) != hier_end) return false;
        rt_buf opaque;
        rt_buf_init(&opaque);
        if (rt_buf_append(&opaque, s + hier_start, hier_end - hier_start) != RT_OK) {
            rt_buf_dispose(&opaque);
            return false;
        }
        if (uri->query != NULL) {
            rt_buf_putc(&opaque, '?');
            rt_buf_puts(&opaque, uri->query);
            free(uri->query);
            uri->query = NULL;
        }
        char *value = NULL;
        rt_status rc = rt_buf_to_cstr(&opaque, &value);
        rt_buf_dispose(&opaque);
        if (rc != RT_OK) return false;
        uri->opaque = value;
    } else {
        uri->path = dup_cstr("");
        if (uri->path == NULL) return false;
    }
    /* `port.or_else(default_port)`: an explicit port wins, including ":0". */
    if (!uri->has_port && default_port(uri->scheme) != 0) {
        uri->has_port = true;
        uri->port = default_port(uri->scheme);
    }
    return true;
}

static bool split_relative(const unsigned char *s, size_t len, rt_uri *uri) {
    struct tail tail;
    if (!parse_query_fragment_positions(s, len, 0, &tail)) return false;
    size_t hier_end = tail.hier_end;
    bool valid;
    if (hier_end >= 2 && s[0] == '/' && s[1] == '/') {
        struct authority authority;
        if (!parse_authority(s, hier_end, 2, &authority)) return false;
        valid = take_while_class(s, hier_end, authority.end, seg_char) == hier_end;
    } else if ((hier_end >= 1 && s[0] == '/') || hier_end == 0) {
        valid = take_while_class(s, hier_end, 0, seg_char) == hier_end;
    } else {
        size_t first = take_while_class(s, hier_end, 0, seg_nc_char);
        valid = first > 0 && (first == hier_end ||
                              (s[first] == '/' && take_while_class(s, hier_end, first, seg_char) == hier_end));
    }
    if (!valid) return false;
    uri->path = dup_range(s, 0, hier_end);
    if (uri->path == NULL) return false;
    if (tail.has_query) {
        char *escaped = NULL;
        if (escape_query(s + tail.query_start, tail.query_end - tail.query_start, &escaped) != RT_URI_OK) {
            return false;
        }
        uri->query = escaped;
    }
    if (tail.has_fragment) {
        size_t f_start = 0;
        for (size_t j = 0; j < len; j++) {
            if (s[j] == '#') {
                f_start = j + 1;
                break;
            }
        }
        uri->fragment = dup_range(s, f_start, len);
        if (uri->fragment == NULL) return false;
    }
    return true;
}

rt_uri_status rt_uri_parse(const unsigned char *bytes, size_t len, rt_uri *out) {
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < len; i++) {
        if (bytes[i] >= 0x80) return RT_URI_INVALID_URI;
    }
    if (!split_absolute(bytes, len, out)) {
        rt_uri_dispose(out);
        if (!split_relative(bytes, len, out)) {
            rt_uri_dispose(out);
            return RT_URI_INVALID_URI;
        }
    }
    rt_uri_status rc = check_scheme_class(out);
    if (rc != RT_URI_OK) rt_uri_dispose(out);
    return rc;
}
