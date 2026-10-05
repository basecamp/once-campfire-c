/* Test-only H03 route-table double; see route_double.h. */
#include "route_double.h"

#include "http/params.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cf_test_route {
    cf_method method;
    char pattern[128];
    uint32_t id;
    cf_action_fn action;
};

static struct cf_test_route routes[CF_TEST_ROUTES_MAX];
static size_t route_count;
static size_t match_count;

void cf_test_routes_reset(void) {
    memset(routes, 0, sizeof routes);
    route_count = 0;
    match_count = 0;
}

void cf_test_routes_reset_matches(void) { match_count = 0; }

size_t cf_test_routes_matches(void) { return match_count; }

static bool cf_test_method(const char *name, cf_method *out) {
    if (strcmp(name, "GET") == 0) *out = CF_GET;
    else if (strcmp(name, "HEAD") == 0) *out = CF_HEAD;
    else if (strcmp(name, "POST") == 0) *out = CF_POST;
    else if (strcmp(name, "PUT") == 0) *out = CF_PUT;
    else if (strcmp(name, "PATCH") == 0) *out = CF_PATCH;
    else if (strcmp(name, "DELETE") == 0) *out = CF_DELETE;
    else if (strcmp(name, "OPTIONS") == 0) *out = CF_OPTIONS;
    else return false;
    return true;
}

cf_err cf_test_routes_add(const char *method, const char *pattern, uint32_t id,
                          cf_action_fn action) {
    if (method == NULL || pattern == NULL || id == 0) return CF_INVALID;
    if (route_count >= CF_TEST_ROUTES_MAX) return CF_LIMIT;
    cf_method parsed;
    if (!cf_test_method(method, &parsed)) return CF_INVALID;
    if (strlen(pattern) >= sizeof routes[0].pattern) return CF_INVALID;
    struct cf_test_route *r = &routes[route_count];
    memset(r, 0, sizeof *r);
    r->method = parsed;
    snprintf(r->pattern, sizeof r->pattern, "%s", pattern);
    r->id = id;
    r->action = action;
    route_count++;
    return CF_OK;
}

static int cf_test_hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool cf_test_utf8_valid(const unsigned char *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = p[i];
        size_t need;
        uint32_t cp;
        if (c < 0x80) {
            i++;
            continue;
        } else if (c >= 0xc2 && c <= 0xdf) {
            need = 1;
            cp = c & 0x1f;
        } else if (c >= 0xe0 && c <= 0xef) {
            need = 2;
            cp = c & 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            need = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + need >= n) return false;
        for (size_t k = 1; k <= need; k++) {
            unsigned char cc = p[i + k];
            if ((cc & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3f);
        }
        if (need == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) {
            return false;
        }
        if (need == 3 && (cp < 0x10000 || cp > 0x10ffff)) return false;
        i += need + 1;
    }
    return true;
}

/* Percent-decode a path segment ('+' stays literal). A malformed escape or
 * non-UTF-8 result is the H03 400 case. */
static cf_err cf_test_path_decode(cf_span in, char **out, size_t *out_len) {
    char *buf = malloc(in.len + 1);
    if (buf == NULL) return CF_NOMEM;
    size_t n = 0;
    for (size_t i = 0; i < in.len;) {
        if (in.ptr[i] == '%') {
            int hi = i + 1 < in.len ? cf_test_hex(in.ptr[i + 1]) : -1;
            int lo = i + 2 < in.len ? cf_test_hex(in.ptr[i + 2]) : -1;
            if (hi < 0 || lo < 0) {
                free(buf);
                return CF_INVALID;
            }
            buf[n++] = (char)((hi << 4) | lo);
            i += 3;
        } else {
            buf[n++] = (char)in.ptr[i++];
        }
    }
    buf[n] = '\0';
    if (!cf_test_utf8_valid((const unsigned char *)buf, n)) {
        free(buf);
        return CF_INVALID;
    }
    *out = buf;
    *out_len = n;
    return CF_OK;
}

/* application/x-www-form-urlencoded value encoding (so cf_params_parse
 * returns exactly the captured bytes). */
static cf_err cf_test_encode(cf_builder *b, cf_span s) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < s.len; i++) {
        unsigned char c = s.ptr[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.' ||
            c == '_') {
            cf_err rc = cf_builder_append(b, (cf_span){&c, 1});
            if (rc != CF_OK) return rc;
        } else {
            unsigned char enc[3] = {'%', (unsigned char)hex[c >> 4],
                                    (unsigned char)hex[c & 0x0f]};
            cf_err rc = cf_builder_append(b, (cf_span){enc, 3});
            if (rc != CF_OK) return rc;
        }
    }
    return CF_OK;
}

#define CF_TEST_SEGMENTS_MAX 16

struct cf_test_segments {
    cf_span seg[CF_TEST_SEGMENTS_MAX];
    size_t count;
    bool truncated;
};

static void cf_test_split(cf_span path, struct cf_test_segments *out) {
    size_t i = 0;
    out->count = 0;
    out->truncated = false;
    while (i <= path.len) {
        size_t start = i;
        while (i < path.len && path.ptr[i] != '/') i++;
        cf_span seg = {path.ptr + start, i - start};
        if (seg.len > 0) {
            if (out->count == CF_TEST_SEGMENTS_MAX) {
                out->truncated = true;
                return;
            }
            out->seg[out->count++] = seg;
        }
        if (i == path.len) break;
        i++;
    }
}

/* Match one row against the split path. On CF_OK with *matched, captures have
 * been appended to `query`. */
static cf_err cf_test_match_one(const struct cf_test_route *r, cf_span path,
                                cf_builder *query, bool *matched) {
    *matched = false;
    char pattern[sizeof r->pattern];
    snprintf(pattern, sizeof pattern, "%s", r->pattern);
    bool optional_format = false;
    size_t plen = strlen(pattern);
    const char *suffix = "(.:format)";
    size_t slen = strlen(suffix);
    if (plen >= slen && strcmp(pattern + plen - slen, suffix) == 0) {
        optional_format = true;
        pattern[plen - slen] = '\0';
    }
    struct cf_test_segments pat;
    struct cf_test_segments seg;
    cf_test_split((cf_span){(const unsigned char *)pattern, strlen(pattern)},
                  &pat);
    cf_test_split(path, &seg);
    if (pat.truncated || seg.truncated) return CF_OK;
    bool has_glob = pat.count > 0 && pat.seg[pat.count - 1].ptr[0] == '*';
    if (has_glob) {
        /* The terminal glob needs at least one nonempty segment (nonempty
         * capture); it consumes every remaining segment. */
        if (seg.count < pat.count) return CF_OK;
    } else if (pat.count != seg.count) {
        return CF_OK;
    }
    if (!optional_format && pat.count == 0 && seg.count == 0) {
        *matched = true;
        return CF_OK;
    }
    for (size_t i = 0; i < pat.count; i++) {
        cf_span p = pat.seg[i];
        cf_span s = seg.seg[i];
        if (p.ptr[0] == '*') {
            /* Terminal glob: the rest of the path joined with '/'. */
            if (i + 1 != pat.count) return CF_OK;
            cf_span name = {p.ptr + 1, p.len - 1};
            if (name.len == 0) return CF_OK;
            cf_span rest = {s.ptr, path.len - (size_t)(s.ptr - path.ptr)};
            cf_err rc = cf_test_encode(query, name);
            if (rc != CF_OK) return rc;
            if (cf_builder_append(query, (cf_span){(const unsigned char *)"=", 1}) !=
                CF_OK) {
                return CF_NOMEM;
            }
            rc = cf_test_encode(query, rest);
            if (rc != CF_OK) return rc;
            if (cf_builder_append(query,
                                  (cf_span){(const unsigned char *)"&", 1}) !=
                CF_OK) {
                return CF_NOMEM;
            }
            *matched = true;
            return CF_OK;
        }
        cf_span literal = p;
        cf_span value = s;
        if (p.ptr[0] == ':') {
            literal = (cf_span){NULL, 0};
        }
        if (i + 1 == pat.count && optional_format) {
            /* Allow `literal.ext` / `:capture.ext`: split at the last '.' in
             * the path segment, or the last '.' in the literal. */
            size_t dot = SIZE_MAX;
            if (literal.len > 0) {
                for (size_t k = s.len; k > 0; k--) {
                    if (s.ptr[k - 1] == '.') {
                        dot = k - 1;
                        break;
                    }
                }
                if (dot == SIZE_MAX || dot < literal.len) return CF_OK;
                if (dot != literal.len) return CF_OK;
                cf_span ext = {s.ptr + dot + 1, s.len - dot - 1};
                if (ext.len == 0) return CF_OK;
                for (size_t k = 0; k < ext.len; k++) {
                    unsigned char c = ext.ptr[k];
                    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '_')) {
                        return CF_OK;
                    }
                }
                cf_err rc = cf_test_encode(query, CF_TEST_SPAN("format"));
                if (rc != CF_OK) return rc;
                if (cf_builder_append(
                        query, (cf_span){(const unsigned char *)"=", 1}) !=
                    CF_OK) {
                    return CF_NOMEM;
                }
                rc = cf_test_encode(query, ext);
                if (rc != CF_OK) return rc;
                if (cf_builder_append(
                        query, (cf_span){(const unsigned char *)"&", 1}) !=
                    CF_OK) {
                    return CF_NOMEM;
                }
                value = (cf_span){s.ptr, dot};
            } else {
                for (size_t k = s.len; k > 0; k--) {
                    if (s.ptr[k - 1] == '.') {
                        dot = k - 1;
                        break;
                    }
                }
                if (dot == SIZE_MAX) {
                    /* No extension: the capture takes the whole segment. */
                    value = s;
                    goto have_value;
                }
                if (dot == 0) return CF_OK;
                cf_span ext = {s.ptr + dot + 1, s.len - dot - 1};
                if (ext.len == 0) return CF_OK;
                for (size_t k = 0; k < ext.len; k++) {
                    unsigned char c = ext.ptr[k];
                    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '_')) {
                        return CF_OK;
                    }
                }
                cf_err rc = cf_test_encode(
                    query, (cf_span){(const unsigned char *)"format", 6});
                if (rc != CF_OK) return rc;
                if (cf_builder_append(
                        query, (cf_span){(const unsigned char *)"=", 1}) !=
                    CF_OK) {
                    return CF_NOMEM;
                }
                rc = cf_test_encode(query, ext);
                if (rc != CF_OK) return rc;
                if (cf_builder_append(
                        query, (cf_span){(const unsigned char *)"&", 1}) !=
                    CF_OK) {
                    return CF_NOMEM;
                }
                value = (cf_span){s.ptr, dot};
            }
        }
    have_value:
        if (literal.len > 0) {
            if (literal.len != value.len ||
                memcmp(literal.ptr, value.ptr, literal.len) != 0) {
                return CF_OK;
            }
            continue;
        }
        /* `:name` capture: percent-decode (with '+' literal), then re-encode
         * for the parameter tree so cf_params_parse returns the decoded
         * bytes. Bad escapes/UTF-8 are the H03 400 case. */
        if (value.len == 0) return CF_OK;
        char *decoded = NULL;
        size_t decoded_len = 0;
        cf_err rc = cf_test_path_decode(value, &decoded, &decoded_len);
        if (rc != CF_OK) return rc;
        rc = cf_test_encode(query, (cf_span){p.ptr + 1, p.len - 1});
        if (rc != CF_OK) {
            free(decoded);
            return rc;
        }
        if (cf_builder_append(query, (cf_span){(const unsigned char *)"=", 1}) !=
            CF_OK) {
            free(decoded);
            return CF_NOMEM;
        }
        rc = cf_test_encode(query,
                            (cf_span){(const unsigned char *)decoded,
                                      decoded_len});
        free(decoded);
        if (rc != CF_OK) return rc;
        if (cf_builder_append(query, (cf_span){(const unsigned char *)"&", 1}) !=
            CF_OK) {
            return CF_NOMEM;
        }
    }
    *matched = true;
    return CF_OK;
}

cf_err cf_route_match_request(const cf_request *request, cf_route_match *out) {
    if (request == NULL || out == NULL) return CF_INVALID;
    out->id = 0;
    out->path_params = NULL;
    cf_builder query = {0};
    for (size_t i = 0; i < route_count; i++) {
        const struct cf_test_route *r = &routes[i];
        bool method_ok = r->method == request->method ||
                         (request->method == CF_HEAD && r->method == CF_GET);
        if (!method_ok) continue;
        bool matched = false;
        cf_err rc = cf_test_match_one(r, request->path, &query, &matched);
        if (rc != CF_OK) {
            cf_builder_dispose(&query);
            return rc;
        }
        if (!matched) {
            query.len = 0;
            continue;
        }
        cf_request params_req;
        memset(&params_req, 0, sizeof params_req);
        params_req.query = (cf_span){query.ptr, query.len};
        cf_err prc = cf_params_parse(&params_req, &out->path_params);
        cf_builder_dispose(&query);
        if (prc != CF_OK) {
            out->path_params = NULL;
            return prc;
        }
        out->id = r->id;
        match_count++;
        return CF_OK;
    }
    cf_builder_dispose(&query);
    return CF_NOT_FOUND;
}

void cf_route_match_dispose(cf_route_match *match) {
    if (match == NULL) return;
    cf_params_destroy(match->path_params);
    match->path_params = NULL;
    match->id = 0;
}

cf_action_fn cf_route_action(uint32_t route_id) {
    for (size_t i = 0; i < route_count; i++) {
        if (routes[i].id == route_id) return routes[i].action;
    }
    return NULL;
}

/* Stand-in for H03's reference 404 handler: cf_ctx_process calls it for an
 * unmatched route (and dispatch for a matched row without an action), so the
 * double must provide the symbol. Body parity with public/404.html belongs to
 * the real routes.c; A00's tests only pin status/ownership here. */
cf_err cf_action_reference_action_not_found(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    ctx->response->status = 404;
    return CF_OK;
}
