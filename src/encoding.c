/* Accept-Encoding negotiation; see src/encoding.h for the rules and the
 * pinned-reference citations (crates/kit/src/deflater.rs).
 *
 * Pure and allocation-free: token names are spans into the caller's value,
 * at most 16 tokens are kept, and at most 19 candidates are built. */
#include "encoding.h"

#include <stdlib.h>
#include <string.h>

#define CF_ENC_MAX_TOKENS 16
/* 16 explicit tokens + 2 wildcard expansions (gzip, identity) + 1 implicit
 * identity. */
#define CF_ENC_MAX_CANDIDATES (CF_ENC_MAX_TOKENS + 3)

typedef struct {
    cf_span name; /* trimmed, case preserved */
    double q;
} enc_token;

typedef struct {
    cf_span name;
    double q;
    int pref; /* available index: gzip 0, identity 1, unknown 2 */
} enc_cand;

/* One entry of the reference's `expanded_accept_encoding` list: an explicit
 * token or one wildcard expansion. */
typedef struct {
    cf_span name;
    double q;
} enc_entry;

/* HeaderValue::to_str: HTAB or visible ASCII only (context.c:86-93 and
 * assets.c:80-84 use the same byte gate). Unreadable means absent. */
static bool enc_readable(cf_span value) {
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (c != '\t' && (c < 0x20 || c > 0x7E)) return false;
    }
    return true;
}

/* Rust str::trim over a value this module has already gated: the remaining
 * whitespace bytes are space and HTAB. */
static cf_span enc_trim(cf_span s) {
    while (s.len != 0 && (s.ptr[0] == ' ' || s.ptr[0] == '\t')) {
        s.ptr++;
        s.len--;
    }
    while (s.len != 0 && (s.ptr[s.len - 1] == ' ' || s.ptr[s.len - 1] == '\t')) {
        s.len--;
    }
    return s;
}

/* Exact, case-sensitive name equality (deflater.rs compares `String` to
 * `&str` directly: no case folding). */
static bool enc_name_is(cf_span name, const char *literal) {
    size_t n = strlen(literal);
    return name.len == n && memcmp(name.ptr, literal, n) == 0;
}

static int enc_pref(cf_span name) {
    if (enc_name_is(name, "gzip")) return 0;
    if (enc_name_is(name, "identity")) return 1;
    return 2;
}

/* ruby_compat::to_f for the [0-9.] run after "q=" (deflater.rs:151-153).
 * The run is a slice of the request buffer, so it is copied to a bounded
 * stack buffer and parsed with strtod, whose prefix semantics are Ruby's for
 * this grammar (it stops at a second '.'). Runs longer than the buffer are
 * truncated; the reference's own rb_cstr_to_dbl truncates through a 69-byte
 * buffer on its second pass, and no reachable client sends a quality that
 * long (documented in the K01a evidence). */
static double enc_run_to_f(cf_span run) {
    char buf[64];
    size_t n = run.len < sizeof buf - 1 ? run.len : sizeof buf - 1;
    memcpy(buf, run.ptr, n);
    buf[n] = '\0';
    return strtod(buf, NULL);
}

/* parse_http_accept_header (deflater.rs:138-158), including the 16-entry
 * take (deflater.rs:162). Returns the kept token count. */
static size_t enc_parse(cf_span header, enc_token out[CF_ENC_MAX_TOKENS]) {
    size_t n = 0;
    size_t i = 0;
    while (i < header.len && n < CF_ENC_MAX_TOKENS) {
        size_t end = i;
        while (end < header.len && header.ptr[end] != ',') end++;
        cf_span part = enc_trim((cf_span){header.ptr + i, end - i});
        i = end < header.len ? end + 1 : end;
        if (part.len == 0) continue; /* empty parts do not take a slot */

        size_t semi = 0;
        while (semi < part.len && part.ptr[semi] != ';') semi++;
        cf_span attr = enc_trim((cf_span){part.ptr, semi});
        double q = 1.0;
        if (semi < part.len) {
            cf_span params =
                enc_trim((cf_span){part.ptr + semi + 1, part.len - semi - 1});
            /* `/\Aq=([\d.]+)/ =~ parameters`, else 1.0: an empty run keeps
             * the default, matching "q=" and "q=abc". */
            if (params.len >= 2 && params.ptr[0] == 'q' && params.ptr[1] == '=') {
                cf_span run = (cf_span){params.ptr + 2, params.len - 2};
                size_t k = 0;
                while (k < run.len &&
                       ((run.ptr[k] >= '0' && run.ptr[k] <= '9') ||
                        run.ptr[k] == '.')) {
                    k++;
                }
                if (k != 0) q = enc_run_to_f((cf_span){run.ptr, k});
            }
        }
        out[n].name = attr;
        out[n].q = q;
        n++;
    }
    return n;
}

static bool enc_listed(const enc_token *tokens, size_t n, const char *literal) {
    for (size_t i = 0; i < n; i++) {
        if (enc_name_is(tokens[i].name, literal)) return true;
    }
    return false;
}

static bool enc_same_name(cf_span a, cf_span b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

/* select_best_encoding (deflater.rs:160-190), specialized to the available
 * set {gzip, identity} but keeping the general candidate rules (unknown
 * tokens, wildcard expansion, reject-if-any-zero). */
static cf_encoding enc_select_tokens(const enc_token *tokens, size_t n) {
    /* expanded_accept_encoding (deflater.rs:163-177). */
    enc_entry expanded[CF_ENC_MAX_TOKENS + 2];
    size_t expanded_n = 0;
    bool wildcard_seen = false;
    static const char *const available[2] = {"gzip", "identity"};
    for (size_t t = 0; t < n; t++) {
        if (enc_name_is(tokens[t].name, "*")) {
            if (wildcard_seen) continue; /* only the first wildcard expands */
            wildcard_seen = true;
            for (int a = 0; a < 2; a++) {
                if (enc_listed(tokens, n, available[a])) continue;
                expanded[expanded_n].name =
                    (cf_span){(const unsigned char *)available[a],
                              strlen(available[a])};
                expanded[expanded_n].q = tokens[t].q;
                expanded_n++;
            }
            continue;
        }
        expanded[expanded_n].name = tokens[t].name;
        expanded[expanded_n].q = tokens[t].q;
        expanded_n++;
    }

    /* candidate_encoding: expanded entries sorted by [-q, preference]
     * (deflater.rs:178-179). The sort is stable in the reference; the
     * preference breaks every tie that can reach a selection, and ties
     * among unknown tokens are unreachable. */
    enc_cand cands[CF_ENC_MAX_CANDIDATES];
    for (size_t i = 0; i < expanded_n; i++) {
        cands[i].name = expanded[i].name;
        cands[i].q = expanded[i].q;
        cands[i].pref = enc_pref(expanded[i].name);
    }
    size_t count = expanded_n;
    for (size_t i = 1; i < count; i++) {
        enc_cand key = cands[i];
        size_t j = i;
        while (j > 0 && (cands[j - 1].q < key.q ||
                         (cands[j - 1].q == key.q &&
                          cands[j - 1].pref > key.pref))) {
            cands[j] = cands[j - 1];
            j--;
        }
        cands[j] = key;
    }

    /* candidate_encoding << "identity" unless included (deflater.rs:181). */
    bool have_identity = false;
    for (size_t i = 0; i < count; i++) {
        if (enc_name_is(cands[i].name, "identity")) {
            have_identity = true;
            break;
        }
    }
    if (!have_identity) {
        cands[count].name =
            (cf_span){(const unsigned char *)"identity", strlen("identity")};
        cands[count].q = 1.0; /* implicit: no q=0 entry names it */
        cands[count].pref = 1;
        count++;
    }

    /* candidates.reject! { |m| expanded.any? { |m2, q| m2 == m && q == 0.0 } }
     * (deflater.rs:184-188): any q=0 entry drains every candidate of that
     * exact, case-sensitive name. */
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        bool drained = false;
        for (size_t e = 0; e < expanded_n && !drained; e++) {
            if (expanded[e].q == 0.0 &&
                enc_same_name(expanded[e].name, cands[i].name)) {
                drained = true;
            }
        }
        if (!drained) cands[kept++] = cands[i];
    }
    count = kept;

    /* candidate_encoding.find { |encoding| available.include?(encoding) } */
    for (size_t i = 0; i < count; i++) {
        if (enc_name_is(cands[i].name, "gzip")) return CF_ENC_GZIP;
        if (enc_name_is(cands[i].name, "identity")) return CF_ENC_IDENTITY;
    }
    return CF_ENC_UNACCEPTABLE;
}

cf_encoding cf_encoding_select(cf_span first_accept_encoding_value) {
    if (!enc_readable(first_accept_encoding_value)) {
        first_accept_encoding_value = (cf_span){NULL, 0};
    }
    enc_token tokens[CF_ENC_MAX_TOKENS];
    size_t n = enc_parse(first_accept_encoding_value, tokens);
    return enc_select_tokens(tokens, n);
}

const char *cf_encoding_name(cf_encoding encoding) {
    switch (encoding) {
    case CF_ENC_GZIP:
        return "gzip";
    case CF_ENC_IDENTITY:
        return "identity";
    case CF_ENC_UNACCEPTABLE:
        break;
    }
    return NULL;
}
