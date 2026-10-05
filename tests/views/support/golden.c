/* A02 golden support: see support/golden.h for the contract and the mapping
 * to the pinned Rust runner (tests/fixtures/crates/views/tests/support/dom.rs).
 */
#include "support/golden.h"

#include "cf_test.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CF_GOLDEN_DEFAULT_DIR "tests/fixtures/crates/views/tests/golden"

static const char *golden_dir(void) {
    const char *dir = getenv("CF_GOLDEN_DIR");
    return dir != NULL && dir[0] != '\0' ? dir : CF_GOLDEN_DEFAULT_DIR;
}

char *cf_golden_read(const char *variant, const char *name, const char *ext,
                     size_t *out_len) {
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/%s/%s.%s", golden_dir(), variant,
                     name, ext);
    if (n < 0 || (size_t)n >= sizeof path) {
        fprintf(stderr, "golden: path too long for %s/%s.%s\n", variant, name,
                ext);
        return NULL;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "golden: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(buf);
        return NULL;
    }
    buf[got] = '\0';
    if (out_len != NULL) *out_len = got;
    return buf;
}

/* --- token buffer -------------------------------------------------------- */

static int tokens_push(cf_golden_tokens *tokens, const char *line) {
    if (tokens->count == tokens->cap) {
        size_t cap = tokens->cap == 0 ? 64 : tokens->cap * 2;
        char **grown = realloc(tokens->lines, cap * sizeof *grown);
        if (grown == NULL) return 0;
        tokens->lines = grown;
        tokens->cap = cap;
    }
    char *copy = strdup(line);
    if (copy == NULL) return 0;
    tokens->lines[tokens->count++] = copy;
    return 1;
}

void cf_golden_tokens_dispose(cf_golden_tokens *tokens) {
    if (tokens == NULL) return;
    for (size_t i = 0; i < tokens->count; i++) free(tokens->lines[i]);
    free(tokens->lines);
    memset(tokens, 0, sizeof *tokens);
}

/* --- small growable byte buffer ------------------------------------------ */

typedef struct {
    char *data;
    size_t len, cap;
} bytes;

static int bytes_reserve(bytes *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return 1;
    size_t cap = b->cap == 0 ? 256 : b->cap;
    while (cap < b->len + extra + 1) cap *= 2;
    char *grown = realloc(b->data, cap);
    if (grown == NULL) return 0;
    b->data = grown;
    b->cap = cap;
    return 1;
}

static int bytes_put(bytes *b, const char *data, size_t len) {
    if (!bytes_reserve(b, len)) return 0;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
    return 1;
}

static void bytes_clear(bytes *b) {
    b->len = 0;
    if (b->data != NULL) b->data[0] = '\0';
}

/* --- character references ------------------------------------------------ */

/* html5ever decodes named references from the WHATWG table; the pinned
 * fixtures spell only these, all semicolon-terminated, plus numeric
 * references.  Semicolon-less legacy forms are accepted for the names HTML5
 * allows (amp, lt, gt, quot, nbsp, copy, reg) when not followed by more
 * name characters, matching the spec's legacy list. */
struct named_ref {
    const char *name;
    const char *utf8;
};

static const struct named_ref NAMED_REFS[] = {
    {"amp", "&"},      {"lt", "<"},     {"gt", ">"},     {"quot", "\""},
    {"apos", "'"},     {"nbsp", "\xC2\xA0"}, {"trade", "\xE2\x84\xA2"},
    {"copy", "\xC2\xA9"}, {"reg", "\xC2\xAE"},
};

static const struct named_ref *named_ref_lookup(const char *name, size_t len) {
    for (size_t i = 0; i < sizeof NAMED_REFS / sizeof NAMED_REFS[0]; i++) {
        if (strlen(NAMED_REFS[i].name) == len &&
            memcmp(NAMED_REFS[i].name, name, len) == 0) {
            return &NAMED_REFS[i];
        }
    }
    return NULL;
}

static size_t utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Append `text` to `out`, decoding character references.  Returns the number
 * of input bytes consumed (always `len` unless `stop_amp` is set and a '&'
 * was found, in which case decoding stops there so attribute parsers can
 * resume at the reference).  For raw-text contexts the caller passes
 * decode=false. */
static size_t append_decoded(bytes *out, const char *text, size_t len,
                             bool decode) {
    size_t i = 0;
    while (i < len) {
        char c = text[i];
        if (!decode || c != '&') {
            if (!bytes_put(out, &c, 1)) return i;
            i++;
            continue;
        }
        /* '&' at end of input: literal. */
        if (i + 1 >= len) {
            if (!bytes_put(out, "&", 1)) return i;
            i++;
            continue;
        }
        if (text[i + 1] == '#') {
            size_t j = i + 2;
            uint32_t cp = 0;
            bool hex = false;
            if (j < len && (text[j] == 'x' || text[j] == 'X')) {
                hex = true;
                j++;
            }
            size_t digits = 0;
            while (j < len) {
                char d = text[j];
                uint32_t v;
                if (d >= '0' && d <= '9') {
                    v = (uint32_t)(d - '0');
                } else if (hex && d >= 'a' && d <= 'f') {
                    v = (uint32_t)(d - 'a' + 10);
                } else if (hex && d >= 'A' && d <= 'F') {
                    v = (uint32_t)(d - 'A' + 10);
                } else {
                    break;
                }
                if (cp > 0x10FFFF) cp = 0x110000; /* stay invalid */
                else cp = cp * (hex ? 16u : 10u) + v;
                j++;
                digits++;
            }
            if (digits == 0) {
                if (!bytes_put(out, "&", 1)) return i;
                i++;
                continue;
            }
            bool semicolon = j < len && text[j] == ';';
            if (semicolon) j++;
            /* HTML5 replacement rules: NUL, surrogates and out-of-range
             * become U+FFFD; the C1 range is remapped by the Windows-1252
             * table (the fixtures never use it, but the mapping is the
             * spec's). */
            if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                cp = 0xFFFD;
            }
            char utf8[4];
            size_t n = utf8_encode(cp, utf8);
            if (!bytes_put(out, utf8, n)) return i;
            i = j;
            continue;
        }
        /* named reference */
        size_t j = i + 1;
        while (j < len && ((text[j] >= 'a' && text[j] <= 'z') ||
                           (text[j] >= 'A' && text[j] <= 'Z') ||
                           (text[j] >= '0' && text[j] <= '9'))) {
            j++;
        }
        bool semicolon = j < len && text[j] == ';';
        size_t name_len = j - (i + 1);
        const struct named_ref *ref =
            name_len == 0 ? NULL : named_ref_lookup(text + i + 1, name_len);
        if (ref != NULL && (semicolon || name_len > 0)) {
            /* A semicolon-less match must not swallow name characters that
             * could extend the name (HTML5's longest-match rule); the names
             * above are full words, so a following alphanumeric means this
             * is not the reference. */
            bool legacy_ok = true;
            if (!semicolon && j < len) {
                char next = text[j];
                if ((next >= 'a' && next <= 'z') ||
                    (next >= 'A' && next <= 'Z') ||
                    (next >= '0' && next <= '9')) {
                    legacy_ok = false;
                }
            }
            if (legacy_ok) {
                if (!bytes_put(out, ref->utf8, strlen(ref->utf8))) return i;
                i = semicolon ? j + 1 : j;
                continue;
            }
        }
        if (!bytes_put(out, "&", 1)) return i;
        i++;
    }
    return i;
}

/* --- Rust Debug string spelling (format!("{v:?}")) ------------------------ */

static int append_rust_debug(bytes *out, const char *value, size_t len) {
    if (!bytes_put(out, "\"", 1)) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        switch (c) {
        case '\\': if (!bytes_put(out, "\\\\", 2)) return 0; break;
        case '"': if (!bytes_put(out, "\\\"", 2)) return 0; break;
        case '\n': if (!bytes_put(out, "\\n", 2)) return 0; break;
        case '\r': if (!bytes_put(out, "\\r", 2)) return 0; break;
        case '\t': if (!bytes_put(out, "\\t", 2)) return 0; break;
        default:
            if (c < 0x20 || c == 0x7F) {
                char esc[16];
                int n = snprintf(esc, sizeof esc, "\\u{%02x}", c);
                if (!bytes_put(out, esc, (size_t)n)) return 0;
            } else {
                char ch = (char)c;
                if (!bytes_put(out, &ch, 1)) return 0;
            }
        }
    }
    return bytes_put(out, "\"", 1);
}

/* --- text flush ----------------------------------------------------------- */

static int collapse_append(bytes *out, const char *text, size_t len) {
    bool in_space = false;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        bool ws = c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                  c == '\f';
        if (ws) {
            if (!in_space) {
                if (!bytes_put(out, " ", 1)) return 0;
            }
            in_space = true;
        } else {
            if (!bytes_put(out, &c, 1)) return 0;
            in_space = false;
        }
    }
    return 1;
}

static int flush_text(cf_golden_tokens *tokens, bytes *text) {
    if (text->len == 0) return 1;
    bytes collapsed = {0};
    if (!collapse_append(&collapsed, text->data, text->len)) {
        free(collapsed.data);
        return 0;
    }
    bytes line = {0};
    bool ok = bytes_put(&line, "\"", 1) &&
              bytes_put(&line, collapsed.data, collapsed.len) &&
              bytes_put(&line, "\"", 1) && tokens_push(tokens, line.data);
    free(line.data);
    free(collapsed.data);
    bytes_clear(text);
    return ok;
}

/* --- attributes ----------------------------------------------------------- */

struct attr {
    char *name;
    char *value;
    size_t value_len;
};

struct attrs {
    struct attr *items;
    size_t count, cap;
};

static void attrs_dispose(struct attrs *attrs) {
    for (size_t i = 0; i < attrs->count; i++) {
        free(attrs->items[i].name);
        free(attrs->items[i].value);
    }
    free(attrs->items);
    memset(attrs, 0, sizeof *attrs);
}

static struct attr *attrs_find(struct attrs *attrs, const char *name,
                               size_t len) {
    for (size_t i = 0; i < attrs->count; i++) {
        if (strlen(attrs->items[i].name) == len &&
            memcmp(attrs->items[i].name, name, len) == 0) {
            return &attrs->items[i];
        }
    }
    return NULL;
}

static struct attr *attrs_add(struct attrs *attrs, const char *name,
                              size_t len) {
    if (attrs->count == attrs->cap) {
        size_t cap = attrs->cap == 0 ? 8 : attrs->cap * 2;
        struct attr *grown = realloc(attrs->items, cap * sizeof *grown);
        if (grown == NULL) return NULL;
        attrs->items = grown;
        attrs->cap = cap;
    }
    struct attr *slot = &attrs->items[attrs->count++];
    memset(slot, 0, sizeof *slot);
    slot->name = malloc(len + 1);
    if (slot->name == NULL) return NULL;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        slot->name[i] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
    }
    slot->name[len] = '\0';
    slot->value = strdup("");
    if (slot->value == NULL) return NULL;
    return slot;
}

static void attr_set_value(struct attr *attr, const char *value,
                           size_t len) {
    free(attr->value);
    attr->value = malloc(len + 1);
    if (attr->value == NULL) {
        attr->value = NULL;
        attr->value_len = 0;
        return;
    }
    memcpy(attr->value, value, len);
    attr->value[len] = '\0';
    attr->value_len = len;
}

static int attr_cmp(const void *a, const void *b) {
    const struct attr *x = a, *y = b;
    int by_name = strcmp(x->name, y->name);
    if (by_name != 0) return by_name;
    return strcmp(x->value != NULL ? x->value : "",
                  y->value != NULL ? y->value : "");
}

static bool attr_named(const struct attrs *attrs, const char *name,
                       const char *value) {
    for (size_t i = 0; i < attrs->count; i++) {
        if (strcmp(attrs->items[i].name, name) == 0 &&
            strcmp(attrs->items[i].value != NULL ? attrs->items[i].value : "",
                   value) == 0) {
            return true;
        }
    }
    return false;
}

/* --- tag names ------------------------------------------------------------ */

static size_t tag_name_end(const char *html, size_t len, size_t i) {
    while (i < len) {
        char c = html[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            i++;
            continue;
        }
        break;
    }
    return i;
}

static bool name_is(const char *html, size_t start, size_t end,
                    const char *name) {
    size_t n = strlen(name);
    if (end - start != n) return false;
    for (size_t i = 0; i < n; i++) {
        char c = html[start + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        if (c != name[i]) return false;
    }
    return true;
}

/* Is `start` (after '<' and '/') an end tag for `name`?  html5ever accepts
 * ASCII whitespace or '/' or '>' after the name. */
static bool is_end_tag_for(const char *html, size_t len, size_t start,
                           const char *name) {
    size_t end = start;
    while (end < len) {
        char c = html[end];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            end++;
            continue;
        }
        break;
    }
    if (!name_is(html, start, end, name)) return false;
    if (end >= len) return false;
    char c = html[end];
    return c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\n' ||
           c == '\r' || c == '\f';
}

/* --- normalize ------------------------------------------------------------ */

typedef struct {
    cf_golden_tokens *tokens;
    bytes text;
    bool failed;
} sink;

static void emit_start_tag(sink *s, struct attrs *attrs, const char *name) {
    /* Forgery masks (dom.rs::is_forgery_token): dropped before the text
     * flush, so text on either side merges. */
    if (strcmp(name, "input") == 0 &&
        attr_named(attrs, "name", "authenticity_token")) {
        return;
    }
    if (strcmp(name, "meta") == 0 &&
        (attr_named(attrs, "name", "csrf-token") ||
         attr_named(attrs, "name", "csrf-param"))) {
        return;
    }
    if (!flush_text(s->tokens, &s->text)) {
        s->failed = true;
        return;
    }
    if (attrs->count > 1) {
        qsort(attrs->items, attrs->count, sizeof *attrs->items, attr_cmp);
    }
    bytes line = {0};
    if (!bytes_put(&line, "<", 1) || !bytes_put(&line, name, strlen(name))) {
        s->failed = true;
        free(line.data);
        return;
    }
    for (size_t i = 0; i < attrs->count; i++) {
        if (attrs->items[i].value == NULL) continue;
        if (!bytes_put(&line, " ", 1) ||
            !bytes_put(&line, attrs->items[i].name,
                       strlen(attrs->items[i].name)) ||
            !bytes_put(&line, "=", 1) ||
            !append_rust_debug(&line, attrs->items[i].value,
                               attrs->items[i].value_len) ||
            !bytes_put(&line, "", 0)) {
            s->failed = true;
            break;
        }
    }
    if (!s->failed && !bytes_put(&line, ">", 1)) s->failed = true;
    if (!s->failed && !tokens_push(s->tokens, line.data)) s->failed = true;
    free(line.data);
}

static void emit_end_tag(sink *s, const char *name) {
    if (!flush_text(s->tokens, &s->text)) {
        s->failed = true;
        return;
    }
    bytes line = {0};
    if (!bytes_put(&line, "</", 2) || !bytes_put(&line, name, strlen(name)) ||
        !bytes_put(&line, ">", 1)) {
        s->failed = true;
        free(line.data);
        return;
    }
    if (!tokens_push(s->tokens, line.data)) s->failed = true;
    free(line.data);
}

/* Parse a start tag's attributes starting at `i` (just after the tag name).
 * Returns the index just past '>' and sets *self_closing. */
static size_t parse_attrs(const char *html, size_t len, size_t i,
                          struct attrs *attrs) {
    while (i < len) {
        char c = html[i];
        if (c == '>') return i + 1;
        if (c == '/') {
            i++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
            i++;
            continue;
        }
        size_t name_start = i;
        while (i < len) {
            char d = html[i];
            if (d == '=' || d == '>' || d == '/' || d == ' ' || d == '\t' ||
                d == '\n' || d == '\r' || d == '\f') {
                break;
            }
            i++;
        }
        size_t name_len = i - name_start;
        if (name_len == 0) {
            i++; /* stray character; skip to progress */
            continue;
        }
        struct attr *slot = attrs_find(attrs, html + name_start, name_len);
        bool duplicate = slot != NULL;
        if (!duplicate) slot = attrs_add(attrs, html + name_start, name_len);
        /* Skip whitespace before a possible '='. */
        size_t j = i;
        while (j < len && (html[j] == ' ' || html[j] == '\t' ||
                           html[j] == '\n' || html[j] == '\r' ||
                           html[j] == '\f')) {
            j++;
        }
        if (slot == NULL) {
            i = j;
            continue;
        }
        if (j < len && html[j] == '=') {
            j++;
            while (j < len && (html[j] == ' ' || html[j] == '\t' ||
                               html[j] == '\n' || html[j] == '\r' ||
                               html[j] == '\f')) {
                j++;
            }
            bytes value = {0};
            size_t value_end;
            if (j < len && (html[j] == '"' || html[j] == '\'')) {
                char quote = html[j];
                size_t k = j + 1;
                while (k < len && html[k] != quote) k++;
                append_decoded(&value, html + j + 1, k - (j + 1), true);
                value_end = k < len ? k + 1 : k;
            } else {
                size_t k = j;
                while (k < len) {
                    char d = html[k];
                    if (d == ' ' || d == '\t' || d == '\n' || d == '\r' ||
                        d == '\f' || d == '>') {
                        break;
                    }
                    k++;
                }
                append_decoded(&value, html + j, k - j, true);
                value_end = k;
            }
            if (!duplicate) {
                attr_set_value(slot, value.data != NULL ? value.data : "",
                               value.len);
            }
            free(value.data);
            i = value_end;
        } else {
            if (!duplicate) attr_set_value(slot, "", 0);
            i = j;
        }
    }
    return i;
}

int cf_golden_normalize(const char *html, size_t len, cf_golden_tokens *out) {
    memset(out, 0, sizeof *out);
    sink s = {0};
    s.tokens = out;
    size_t i = 0;
    while (i < len && !s.failed) {
        char c = html[i];
        if (c != '<') {
            /* One character-token run up to the next '<'; character
             * references decode here, exactly as html5ever's tokenizer
             * decodes them into character tokens. */
            size_t next = i;
            while (next < len && html[next] != '<') next++;
            size_t consumed = append_decoded(&s.text, html + i, next - i, true);
            if (consumed != next - i) s.failed = true;
            i = next;
            continue;
        }
        /* comment */
        if (i + 3 < len && html[i + 1] == '!' && html[i + 2] == '-' &&
            html[i + 3] == '-') {
            if (!flush_text(out, &s.text)) s.failed = true;
            size_t j = i + 4;
            while (j + 2 < len &&
                   !(html[j] == '-' && html[j + 1] == '-' &&
                     html[j + 2] == '>')) {
                j++;
            }
            i = j + 3 <= len ? j + 3 : len;
            continue;
        }
        /* doctype / bogus comment */
        if (i + 1 < len && html[i + 1] == '!') {
            size_t j = i + 2;
            if (j + 7 <= len && name_is(html, j, j + 7, "doctype")) {
                size_t k = j + 7;
                while (k < len && (html[k] == ' ' || html[k] == '\t' ||
                                   html[k] == '\n' || html[k] == '\r')) {
                    k++;
                }
                size_t name_start = k;
                while (k < len && html[k] != '>' && html[k] != ' ' &&
                       html[k] != '\t' && html[k] != '\n' &&
                       html[k] != '\r') {
                    k++;
                }
                if (!flush_text(out, &s.text)) s.failed = true;
                bytes line = {0};
                if (name_start == k) {
                    bytes_put(&line, "<!DOCTYPE None>",
                              sizeof("<!DOCTYPE None>") - 1);
                } else {
                    bytes_put(&line, "<!DOCTYPE Some(",
                              sizeof("<!DOCTYPE Some(") - 1);
                    append_rust_debug(&line, html + name_start,
                                      k - name_start);
                    bytes_put(&line, ")>", sizeof(")>") - 1);
                }
                if (!tokens_push(out, line.data)) s.failed = true;
                free(line.data);
            } else {
                /* bogus comment: comments emit nothing, but flush pending
                 * text the way CommentToken does in dom.rs */
                if (!flush_text(out, &s.text)) s.failed = true;
            }
            while (j < len && html[j] != '>') j++;
            i = j < len ? j + 1 : len;
            continue;
        }
        /* end tag */
        if (i + 1 < len && html[i + 1] == '/') {
            size_t start = i + 2;
            size_t end = tag_name_end(html, len, start);
            if (end == start) {
                /* "</>" or "</ x": html5ever emits nothing for a missing
                 * name; treat as text to stay conservative. */
                if (!bytes_put(&s.text, "<", 1)) s.failed = true;
                i++;
                continue;
            }
            char name[64];
            size_t n = end - start;
            if (n >= sizeof name) n = sizeof name - 1;
            for (size_t k = 0; k < n; k++) {
                char d = html[start + k];
                name[k] = (d >= 'A' && d <= 'Z') ? (char)(d + ('a' - 'A')) : d;
            }
            name[n] = '\0';
            emit_end_tag(&s, name);
            size_t j = end;
            while (j < len && html[j] != '>') j++;
            i = j < len ? j + 1 : len;
            continue;
        }
        /* start tag */
        if (i + 1 < len && ((html[i + 1] >= 'a' && html[i + 1] <= 'z') ||
                            (html[i + 1] >= 'A' && html[i + 1] <= 'Z'))) {
            size_t start = i + 1;
            size_t end = tag_name_end(html, len, start);
            char name[64];
            size_t n = end - start;
            if (n >= sizeof name) n = sizeof name - 1;
            for (size_t k = 0; k < n; k++) {
                char d = html[start + k];
                name[k] = (d >= 'A' && d <= 'Z') ? (char)(d + ('a' - 'A')) : d;
            }
            name[n] = '\0';
            struct attrs attrs = {0};
            size_t after = parse_attrs(html, len, end, &attrs);
            emit_start_tag(&s, &attrs, name);
            attrs_dispose(&attrs);
            i = after;
            if (s.failed) break;
            /* raw text (script, style) / RCDATA (title, textarea) */
            bool raw = strcmp(name, "script") == 0 ||
                       strcmp(name, "style") == 0;
            bool rcdata = strcmp(name, "title") == 0 ||
                          strcmp(name, "textarea") == 0;
            if (raw || rcdata) {
                size_t j = i;
                while (j < len) {
                    if (html[j] == '<' && j + 1 < len && html[j + 1] == '/') {
                        size_t s2 = j + 2;
                        if (is_end_tag_for(html, len, s2, name)) {
                            size_t t = s2;
                            while (t < len && html[t] != '>') t++;
                            bytes raw_text = {0};
                            append_decoded(&raw_text, html + i, j - i,
                                           rcdata);
                            if (!flush_text(out, &raw_text)) {
                                s.failed = true;
                                free(raw_text.data);
                                break;
                            }
                            free(raw_text.data);
                            emit_end_tag(&s, name);
                            i = t < len ? t + 1 : len;
                            break;
                        }
                    }
                    j++;
                }
                if (j >= len) {
                    /* unterminated: the rest is text */
                    bytes raw_text = {0};
                    append_decoded(&raw_text, html + i, len - i, rcdata);
                    if (!flush_text(out, &raw_text)) s.failed = true;
                    free(raw_text.data);
                    i = len;
                }
            }
            continue;
        }
        /* literal '<' */
        if (!bytes_put(&s.text, "<", 1)) s.failed = true;
        i++;
    }
    if (!s.failed && !flush_text(out, &s.text)) s.failed = true;
    free(s.text.data);
    if (s.failed) {
        cf_golden_tokens_dispose(out);
        return 0;
    }
    return 1;
}

char *cf_golden_diff(const cf_golden_tokens *want, const cf_golden_tokens *got) {
    size_t limit = want->count < got->count ? want->count : got->count;
    size_t first = limit;
    for (size_t i = 0; i < limit; i++) {
        if (strcmp(want->lines[i], got->lines[i]) != 0) {
            first = i;
            break;
        }
    }
    if (first == limit && want->count == got->count) return NULL;

    bytes report = {0};
    char header[160];
    int hn = snprintf(header, sizeof header,
                      "first difference at token %zu (golden %zu tokens, "
                      "generated %zu tokens)\n",
                      first, want->count, got->count);
    bytes_put(&report, header, (size_t)hn);
    size_t from = first > 4 ? first - 4 : 0;
    for (size_t i = from; i < first + 6; i++) {
        const char *w = i < want->count ? want->lines[i] : "<end>";
        const char *g = i < got->count ? got->lines[i] : "<end>";
        char line[1200];
        int n = snprintf(line, sizeof line, "%c golden: %s\n%c generated: %s\n",
                         strcmp(w, g) == 0 ? ' ' : '!', w,
                         strcmp(w, g) == 0 ? ' ' : '!', g);
        if (n < 0) n = 0;
        if ((size_t)n > sizeof line - 1) n = (int)sizeof line - 1;
        bytes_put(&report, line, (size_t)n);
    }
    return report.data;
}

int cf_golden_expect(const char *fixture, const char *generated, size_t len) {
    /* Opt-in debugging aid: dump the generated bytes for diffing by hand. */
    const char *dump_dir = getenv("CF_GOLDEN_DUMP");
    if (dump_dir != NULL && dump_dir[0] != '\0') {
        char path[1024];
        int n = snprintf(path, sizeof path, "%s/%s.html", dump_dir, fixture);
        if (n > 0 && (size_t)n < sizeof path) {
            FILE *dump = fopen(path, "wb");
            if (dump != NULL) {
                fwrite(generated, 1, len, dump);
                fclose(dump);
            }
        }
    }
    size_t golden_len = 0;
    char *golden = cf_golden_read("a", fixture, "html", &golden_len);
    if (golden == NULL) {
        CF_CHECK(0 && "golden fixture missing (not a skip)");
        return 0;
    }
    cf_golden_tokens want = {0}, got = {0};
    if (!cf_golden_normalize(golden, golden_len, &want) ||
        !cf_golden_normalize(generated, len, &got)) {
        fprintf(stderr, "golden %s: normalization allocation failure\n",
                fixture);
        CF_CHECK(0 && "golden normalization failed");
        free(golden);
        cf_golden_tokens_dispose(&want);
        cf_golden_tokens_dispose(&got);
        return 0;
    }
    char *report = cf_golden_diff(&want, &got);
    int ok = report == NULL;
    if (!ok) {
        fprintf(stderr, "golden %s: DOM differs from the reference\n%s",
                fixture, report);
        CF_CHECK(0 && "golden DOM mismatch");
    }
    free(report);
    free(golden);
    cf_golden_tokens_dispose(&want);
    cf_golden_tokens_dispose(&got);
    return ok;
}
