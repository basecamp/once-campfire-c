/* R01: context-specific output escaping primitives (03-application.md R01).
 *
 * Reference translation (source-as-spec, docs/devel/implementation/README.md):
 *
 *  - cf_html_text / cf_html_attr entity spellings come from
 *    tmp/rust-ref/crates/ruby/src/erb.rs lines 18-34 (`ERB::Util.html_escape`
 *    / askama ErbEscaper: `&amp; &lt; &gt; &quot; &#39;`). The C contract in
 *    03-application.md R01 splits the contexts: text escapes `&<>`; attribute
 *    values additionally escape both quote types. The reference escapes every
 *    `&`, including one that starts an entity-looking sequence, in a single
 *    pass (the golden editor markup in
 *    tests/fixtures/crates/views/tests/golden/b/messages_edit_text.json
 *    contains `&amp;amp;` produced by exactly that pass); already-sanitized
 *    HTML is never passed to these helpers but inserted through
 *    cf_html_trusted. U+00A0 handling (`&nbsp;`) belongs to the richtext DOM
 *    serializer (crates/richtext/src/dom.rs escape_text/escape_attribute,
 *    R02), not to these primitives, which stay byte-transparent otherwise.
 *
 *  - cf_url_component comes from tmp/rust-ref/crates/ruby/src/uri.rs lines
 *    1-27 (`ERB::Util.url_encode`, which keeps Addressable's UNRESERVED set):
 *    only `A-Za-z0-9_.-~` stay literal, every other byte is `%XX` with
 *    uppercase hex. A space is `%20`, not `+`; `+` is `%2B` (the `+` for
 *    space spelling is CGI.escape, used for whole query values elsewhere,
 *    and is not this component encoder).
 *
 *  - cf_json_string comes from tmp/rust-ref/crates/rails_compat/src/json.rs
 *    lines 16-45: the json gem's string escaping (`"` `\` and C0 controls;
 *    `\b \t \n \f \r` named, other controls lowercase `\u00xx`; `/` and
 *    0x7f untouched; non-ASCII UTF-8 passed through raw, U+2028/U+2029 not
 *    escaped) plus ActiveSupport::JSON's escape_html_entities (`<` `>` `&`
 *    become their lowercase-hex `\u` JSON escapes). The emitted value is a
 *    complete JSON
 *    string with its quotes. Input must be valid UTF-8: the Rust reference
 *    operates on `&str`, and invalid UTF-8 in the C port's params is rejected
 *    at the parse boundary; cf_json_string redirects a violated precondition
 *    to CF_INVALID rather than emitting invalid JSON.
 *
 * All four functions append atomically: on any error the builder content is
 * byte-identical to what it was before the call (capacity may have grown).
 */
#include "cf.h"

#include <string.h>

/* Appends one byte run; cf_builder_append reserves before copying, so a
 * failed append never leaves a partial run. */
static cf_err cf_append(cf_builder *out, const unsigned char *bytes, size_t len) {
    return cf_builder_append(out, (cf_span){bytes, len});
}

/* --- HTML text/attribute context ------------------------------------------ */

static const char *const cf_html_text_escapes[256] = {
    ['&'] = "&amp;",
    ['<'] = "&lt;",
    ['>'] = "&gt;",
};

static const char *const cf_html_attr_escapes[256] = {
    ['&'] = "&amp;",
    ['<'] = "&lt;",
    ['>'] = "&gt;",
    ['"'] = "&quot;",
    ['\''] = "&#39;",
};

static cf_err cf_html_escape(cf_builder *out, cf_span in,
                             const char *const escapes[256]) {
    if (out == NULL) return CF_INVALID;
    if (in.len != 0 && in.ptr == NULL) return CF_INVALID;
    size_t mark = out->len;
    size_t start = 0;
    cf_err rc = CF_OK;
    for (size_t i = 0; i < in.len; i++) {
        const char *replacement = escapes[in.ptr[i]];
        if (replacement == NULL) continue;
        rc = cf_append(out, in.ptr + start, i - start);
        if (rc == CF_OK) {
            rc = cf_append(out, (const unsigned char *)replacement, strlen(replacement));
        }
        if (rc != CF_OK) break;
        start = i + 1;
    }
    if (rc == CF_OK && start < in.len) {
        rc = cf_append(out, in.ptr + start, in.len - start);
    }
    if (rc != CF_OK) out->len = mark;
    return rc;
}

cf_err cf_html_text(cf_builder *out, cf_span in) {
    return cf_html_escape(out, in, cf_html_text_escapes);
}

cf_err cf_html_attr(cf_builder *out, cf_span in) {
    return cf_html_escape(out, in, cf_html_attr_escapes);
}

/* --- URL component --------------------------------------------------------- */

static int cf_url_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' ||
           c == '~';
}

cf_err cf_url_component(cf_builder *out, cf_span in) {
    if (out == NULL) return CF_INVALID;
    if (in.len != 0 && in.ptr == NULL) return CF_INVALID;
    static const char hex[] = "0123456789ABCDEF";
    size_t mark = out->len;
    size_t start = 0;
    cf_err rc = CF_OK;
    for (size_t i = 0; i < in.len; i++) {
        unsigned char c = in.ptr[i];
        if (cf_url_unreserved(c)) continue;
        rc = cf_append(out, in.ptr + start, i - start);
        if (rc == CF_OK) {
            unsigned char enc[3] = {'%', (unsigned char)hex[c >> 4],
                                    (unsigned char)hex[c & 0xf]};
            rc = cf_append(out, enc, sizeof enc);
        }
        if (rc != CF_OK) break;
        start = i + 1;
    }
    if (rc == CF_OK && start < in.len) {
        rc = cf_append(out, in.ptr + start, in.len - start);
    }
    if (rc != CF_OK) out->len = mark;
    return rc;
}

/* --- JSON string ----------------------------------------------------------- */

static int cf_utf8_continuation(unsigned char c) {
    return c >= 0x80 && c <= 0xBF;
}

/* Length of the well-formed UTF-8 sequence at p, or 0 when invalid: rejects
 * overlongs, surrogates (U+D800..U+DFFF) and code points above U+10FFFF. */
static size_t cf_utf8_sequence_length(const unsigned char *p, size_t remaining) {
    unsigned char b = p[0];
    if (b < 0x80) return 1;
    if (b < 0xC2) return 0; /* continuation byte or overlong lead */
    if (b <= 0xDF) {
        return remaining >= 2 && cf_utf8_continuation(p[1]) ? 2 : 0;
    }
    if (b <= 0xEF) {
        if (remaining < 3 || !cf_utf8_continuation(p[1]) ||
            !cf_utf8_continuation(p[2])) {
            return 0;
        }
        if (b == 0xE0) return p[1] >= 0xA0 ? 3 : 0; /* overlong below U+0800 */
        if (b == 0xED) return p[1] <= 0x9F ? 3 : 0; /* UTF-16 surrogates */
        return 3;
    }
    if (b <= 0xF4) {
        if (remaining < 4 || !cf_utf8_continuation(p[1]) ||
            !cf_utf8_continuation(p[2]) || !cf_utf8_continuation(p[3])) {
            return 0;
        }
        if (b == 0xF0) return p[1] >= 0x90 ? 4 : 0; /* overlong below U+10000 */
        if (b == 0xF4) return p[1] <= 0x8F ? 4 : 0; /* above U+10FFFF */
        return 4;
    }
    return 0;
}

static int cf_json_is_escaped(unsigned char c) {
    return c < 0x20 || c == '"' || c == '\\' || c == '<' || c == '>' ||
           c == '&';
}

static cf_err cf_json_append_escape(cf_builder *out, unsigned char c) {
    static const char hex[] = "0123456789abcdef";
    switch (c) {
    case '"': return cf_append(out, (const unsigned char *)"\\\"", 2);
    case '\\': return cf_append(out, (const unsigned char *)"\\\\", 2);
    case '\b': return cf_append(out, (const unsigned char *)"\\b", 2);
    case '\t': return cf_append(out, (const unsigned char *)"\\t", 2);
    case '\n': return cf_append(out, (const unsigned char *)"\\n", 2);
    case '\f': return cf_append(out, (const unsigned char *)"\\f", 2);
    case '\r': return cf_append(out, (const unsigned char *)"\\r", 2);
    case '<': return cf_append(out, (const unsigned char *)"\\u003c", 6);
    case '>': return cf_append(out, (const unsigned char *)"\\u003e", 6);
    case '&': return cf_append(out, (const unsigned char *)"\\u0026", 6);
    default: break;
    }
    unsigned char buf[6] = {'\\', 'u', '0', '0', (unsigned char)hex[c >> 4],
                            (unsigned char)hex[c & 0xf]};
    return cf_append(out, buf, sizeof buf);
}

cf_err cf_json_string(cf_builder *out, cf_span in) {
    if (out == NULL) return CF_INVALID;
    if (in.len != 0 && in.ptr == NULL) return CF_INVALID;
    size_t mark = out->len;
    cf_err rc = cf_append(out, (const unsigned char *)"\"", 1);
    size_t start = 0;
    size_t i = 0;
    while (rc == CF_OK && i < in.len) {
        unsigned char c = in.ptr[i];
        if (c < 0x80) {
            if (!cf_json_is_escaped(c)) {
                i++;
                continue;
            }
            rc = cf_append(out, in.ptr + start, i - start);
            if (rc == CF_OK) rc = cf_json_append_escape(out, c);
            if (rc != CF_OK) break;
            i++;
            start = i;
            continue;
        }
        size_t n = cf_utf8_sequence_length(in.ptr + i, in.len - i);
        if (n == 0) {
            rc = CF_INVALID;
            break;
        }
        i += n;
    }
    if (rc == CF_OK && start < in.len) {
        rc = cf_append(out, in.ptr + start, in.len - start);
    }
    if (rc == CF_OK) rc = cf_append(out, (const unsigned char *)"\"", 1);
    if (rc != CF_OK) out->len = mark;
    return rc;
}

/* --- Trusted HTML ----------------------------------------------------------- */

/* Appends the sanitized bytes verbatim; escaping them here would double
 * escape already-sanitized HTML. */
cf_err cf_html_trusted(cf_builder *out, const cf_safe_html *safe) {
    if (out == NULL || safe == NULL) return CF_INVALID;
    return cf_builder_append(out, cf_buf_span(safe->bytes));
}

void cf_safe_html_dispose(cf_safe_html *safe) {
    if (safe == NULL) return;
    cf_buf_release(safe->bytes);
    safe->bytes = NULL;
}
