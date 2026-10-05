/* tests/models/support/richtext.c — test-only rich-text/pipeline doubles.
 *
 * Test-support doubles, NOT production code. They stand in for the R02
 * rich-text pipeline (src/richtext.c later) and the D02 transaction accessor
 * while those phases land, so each tests/models test binary links and runs
 * from committed files alone.
 *
 * Reference:
 *  - `BasicRichText::to_plain_text` (tmp/rust-ref/crates/db/src/testing.rs:
 *    tag stripping with newline rules for br/p/div/li/h1/blockquote/pre, trim,
 *    then entity decoding). The action-text-attachment SGID branch is not
 *    implemented here: it needs the Rails/VB/ERB machinery, the reference test
 *    support only uses it for unsigned mention fixtures, and the C model tests
 *    do not exercise attachment bodies (same disclosure as the message
 *    family's previous in-test stand-in; see
 *    docs/devel/evidence/D01-model-message.md).
 *  - `Mentions` (tmp/rust-ref/crates/db/src/tests/push_test.rs): a test struct
 *    whose `mentioned_user_ids` returns the stated ids and whose plain text
 *    delegates to BasicRichText. BasicRichText itself reports no mentions
 *    (testing.rs documents: mentions need verified SGIDs), so this double
 *    takes the list from cf_test_richtext_set_mentions().
 *  - `Tx::rich_text` (tmp/rust-ref/crates/db/src/database.rs): the pipeline a
 *    transaction carries; the double returns a non-NULL opaque instance. The
 *    frozen D01 headers expose `const cf_richtext *` only.
 */
#include "support.h"

#include "cf.h"
#include "db/db_internal.h"
#include "models/types.h"

#include <stdlib.h>
#include <string.h>

/* --- test-controlled mentionee list (Rust: Mentions(Vec<i64>)) ------------ */

#define CF_TEST_RICHTEXT_MAX_MENTIONS 16

static int64_t support_mentions[CF_TEST_RICHTEXT_MAX_MENTIONS];
static size_t support_mentions_len;

void cf_test_richtext_set_mentions(const int64_t *ids, size_t len) {
    if (len > CF_TEST_RICHTEXT_MAX_MENTIONS) len = CF_TEST_RICHTEXT_MAX_MENTIONS;
    support_mentions_len = len;
    if (len != 0 && ids != NULL) {
        memcpy(support_mentions, ids, len * sizeof *ids);
    } else {
        support_mentions_len = 0;
    }
}

/* --- BasicRichText::to_plain_text ---------------------------------------- */

static cf_err support_append(char **buf, size_t *len, size_t *cap,
                             const char *bytes, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t want = *cap != 0 ? *cap : 64;
        while (want < *len + n + 1) {
            if (want > SIZE_MAX / 2) return CF_NOMEM;
            want *= 2;
        }
        char *grown = realloc(*buf, want);
        if (grown == NULL) return CF_NOMEM;
        *buf = grown;
        *cap = want;
    }
    if (n != 0) memcpy(*buf + *len, bytes, n);
    *len += n;
    (*buf)[*len] = '\0';
    return CF_OK;
}

static cf_err support_replace_all(char **buf, size_t *len, size_t *cap,
                                  const char *from, const char *to) {
    size_t from_len = strlen(from);
    if (from_len == 0) return CF_OK;
    size_t pos = 0;
    while (pos + from_len <= *len) {
        if (memcmp(*buf + pos, from, from_len) == 0) {
            size_t to_len = strlen(to);
            size_t rest = *len - pos - from_len;
            if (to_len != from_len) {
                char *tail = malloc(rest);
                if (tail == NULL && rest != 0) return CF_NOMEM;
                if (rest != 0) memcpy(tail, *buf + pos + from_len, rest);
                *len = pos;
                cf_err err = support_append(buf, len, cap, to, to_len);
                if (err == CF_OK) {
                    err = support_append(buf, len, cap, tail, rest);
                }
                free(tail);
                if (err != CF_OK) return err;
            } else {
                memcpy(*buf + pos, to, to_len);
                pos += to_len;
            }
        } else {
            pos++;
        }
    }
    return CF_OK;
}

cf_err cf_richtext_to_plain_text(cf_db *db, const cf_richtext *rich_text,
                                 cf_span html, cf_str *out) {
    (void)db;
    (void)rich_text;
    if (out == NULL) return CF_INVALID;
    out->ptr = NULL;
    out->len = 0;

    char *buf = NULL;
    size_t len = 0, cap = 0;
    size_t pos = 0;
    cf_err err = CF_OK;
    while (err == CF_OK && pos < html.len) {
        size_t start = pos;
        while (start < html.len && html.ptr[start] != '<') start++;
        err = support_append(&buf, &len, &cap,
                             (const char *)html.ptr + pos, start - pos);
        if (err != CF_OK || start >= html.len) break;
        size_t end = start + 1;
        while (end < html.len && html.ptr[end] != '>') end++;
        if (end >= html.len) break; /* unterminated tag: rest dropped */
        const char *tag = (const char *)html.ptr + start + 1;
        size_t tag_len = end - start - 1;
        bool closing = tag_len != 0 && tag[0] == '/';
        size_t name_start = closing ? 1 : 0;
        size_t name_len = 0;
        while (name_start + name_len < tag_len) {
            char c = tag[name_start + name_len];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-')) {
                break;
            }
            name_len++;
        }
        char name[32];
        size_t copy_len = name_len < sizeof name - 1 ? name_len
                                                     : sizeof name - 1;
        memcpy(name, tag + name_start, copy_len);
        name[copy_len] = '\0';
        bool newline_tag = strcmp(name, "br") == 0 || strcmp(name, "p") == 0 ||
                           strcmp(name, "div") == 0 || strcmp(name, "li") == 0 ||
                           strcmp(name, "h1") == 0 ||
                           strcmp(name, "blockquote") == 0 ||
                           strcmp(name, "pre") == 0;
        if (newline_tag && !closing && len != 0) {
            err = support_append(&buf, &len, &cap, "\n", 1);
        }
        pos = end + 1;
    }

    if (err == CF_OK && buf == NULL) {
        buf = malloc(1);
        if (buf == NULL) err = CF_NOMEM;
        else {
            buf[0] = '\0';
            cap = 1;
        }
    }
    if (err != CF_OK) {
        free(buf);
        return err;
    }

    /* BasicRichText trims, then decodes entities. */
    while (len != 0 && (buf[0] == ' ' || buf[0] == '\t' || buf[0] == '\n' ||
                        buf[0] == '\r')) {
        memmove(buf, buf + 1, --len);
    }
    while (len != 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\t' ||
                        buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    err = support_replace_all(&buf, &len, &cap, "&nbsp;", "\xc2\xa0");
    if (err == CF_OK) err = support_replace_all(&buf, &len, &cap, "&lt;", "<");
    if (err == CF_OK) err = support_replace_all(&buf, &len, &cap, "&gt;", ">");
    if (err == CF_OK) err = support_replace_all(&buf, &len, &cap, "&quot;", "\"");
    if (err == CF_OK) err = support_replace_all(&buf, &len, &cap, "&#39;", "'");
    if (err == CF_OK) err = support_replace_all(&buf, &len, &cap, "&amp;", "&");
    if (err != CF_OK) {
        free(buf);
        return err;
    }
    out->ptr = buf;
    out->len = len;
    return CF_OK;
}

/* BasicRichText::mentioned_user_ids reports no mentions; the test says which
 * (Mentions). */
cf_err cf_richtext_mentioned_user_ids(cf_db *db, const cf_richtext *rich_text,
                                      cf_span html, cf_int64_vector *out) {
    (void)db;
    (void)rich_text;
    (void)html;
    if (out == NULL) return CF_INVALID;
    *out = (cf_int64_vector){NULL, 0, 0};
    if (support_mentions_len == 0) return CF_OK;
    if (support_mentions_len > SIZE_MAX / sizeof *out->items) {
        return CF_NOMEM;
    }
    int64_t *items = malloc(support_mentions_len * sizeof *items);
    if (items == NULL) return CF_NOMEM;
    memcpy(items, support_mentions, support_mentions_len * sizeof *items);
    out->items = items;
    out->len = support_mentions_len;
    out->cap = support_mentions_len;
    return CF_OK;
}

/* --- Tx::rich_text -------------------------------------------------------- */

const cf_richtext *cf_tx_rich_text(cf_tx *tx) {
    (void)tx;
    static const unsigned char instance;
    return (const cf_richtext *)&instance;
}
