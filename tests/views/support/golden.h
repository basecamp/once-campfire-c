/* A02 golden support: the Rust runner's DOM comparison, ported to C.
 *
 * Reference: tests/fixtures/crates/views/tests/support/dom.rs (the pinned
 * copy of crates/views/tests/support/dom.rs) and the runner's use of it in
 * parity_a.rs/rooms_views.rs/searches_views.rs:
 *
 *   - an HTML tokenizer feeds the same "sink" html5ever's tokenizer feeds in
 *     dom.rs (start tags with attribute values decoded from character
 *     references and attribute names lowercased, end tags, doctype, text,
 *     comments dropped, raw text for script/style, RCDATA for title);
 *   - the forgery tokens Rails renders and this app does not are the only
 *     masks: <input name="authenticity_token"> and <meta name="csrf-token">
 *     / <meta name="csrf-param">.  They are dropped WITHOUT flushing pending
 *     text, exactly as dom.rs does, so text on either side merges;
 *   - each start tag renders as "<name k=\"v\" ...>" with the attributes
 *     sorted by (name, value) and values in Rust's Debug string spelling
 *     (format!(" {k}={v:?}"));
 *   - each text run renders as "\"collapsed\"" with ASCII whitespace runs
 *     collapsed to one space (whitespace-only runs are kept as " ");
 *   - comments emit nothing; the doctype renders as "<!DOCTYPE Some(\"name\")>"
 *     (Rust's Option<String> Debug spelling).
 *
 * Two documents are equal when their token lines are equal.  That is exactly
 * the comparison assert_parity performs, so a renderer that passes this
 * comparison passes the Rust runner for the same fixture.
 *
 * Deliberate reduction, stated so it cannot hide a failure: this tokenizer
 * implements the constructs the pinned golden fixtures use (doctype, tags,
 * quoted/unquoted attributes, character references, script/style raw text,
 * title RCDATA, comments).  It is not a full HTML5 tokenizer; a fixture that
 * needs another construct fails the run (or, if it is silently treated as
 * text, the token mismatch fails the run) rather than being skipped.
 *
 * The fixture tree is the pinned copy:
 * tests/fixtures/crates/views/tests/golden/{a,b}/<name>.<ext>
 * (CF_GOLDEN_DIR overrides the directory for out-of-tree runs).
 */
#ifndef CF_VIEWS_TEST_GOLDEN_H
#define CF_VIEWS_TEST_GOLDEN_H

#include "cf.h"

#include <stddef.h>
#include <string.h>

/* NUL-safe substring search over builder bytes (cf_builder content is not
 * NUL-terminated; strstr on it would read past the end). */
static inline int cf_builder_contains(const cf_builder *builder,
                                      const char *needle) {
    size_t len = strlen(needle);
    if (len == 0) return 1;
    if (builder == NULL || builder->len < len) return 0;
    for (size_t i = 0; i + len <= builder->len; i++) {
        if (memcmp(builder->ptr + i, needle, len) == 0) return 1;
    }
    return 0;
}

/* NUL-safe equality of a builder's bytes with a C string. */
static inline int cf_builder_equals(const cf_builder *builder,
                                    const char *text) {
    size_t len = strlen(text);
    return builder != NULL && builder->len == len &&
           (len == 0 || memcmp(builder->ptr, text, len) == 0);
}

/* Token lines of a normalized document. */
typedef struct {
    char **lines;
    size_t count, cap;
} cf_golden_tokens;

/* Read tests/fixtures/crates/views/tests/golden/<variant>/<name>.<ext>.
 * On success *out is an owned, NUL-terminated buffer and *len its length;
 * on failure writes a reason to stderr and returns NULL. */
char *cf_golden_read(const char *variant, const char *name, const char *ext,
                     size_t *out_len);

/* Normalize an HTML document (bytes, length) into token lines.  Returns
 * false only on allocation failure. */
int cf_golden_normalize(const char *html, size_t len, cf_golden_tokens *out);

void cf_golden_tokens_dispose(cf_golden_tokens *tokens);

/* NULL when the two token streams are equal; otherwise an owned report
 * naming the first differing token and its context. */
char *cf_golden_diff(const cf_golden_tokens *want, const cf_golden_tokens *got);

/* Normalize `generated` and compare it with golden/<a>/<name>.html; on any
 * failure prints the reason and the first difference and records a CF_CHECK
 * failure (use inside a CF_TEST).  `fixture` is the case name, e.g.
 * "sessions_new".  Returns 1 when the fixture matched. */
int cf_golden_expect(const char *fixture, const char *generated, size_t len);

#endif /* CF_VIEWS_TEST_GOLDEN_H */
