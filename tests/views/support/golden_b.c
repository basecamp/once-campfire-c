/* A02 rooms/messages test support: golden/b loading, the runner's tokenizer
 * and comparison, and the fixture -> view model builders.  See golden_b.h. */
#include "support/golden_b.h"

#include "cf_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------- strings */

static char *strndup_bytes(const unsigned char *ptr, size_t len) {
    char *copy = malloc(len + 1);
    if (copy == NULL) return NULL;
    if (len != 0) memcpy(copy, ptr, len);
    copy[len] = '\0';
    return copy;
}

static cf_err copy_str(const char *value, cf_str *out) {
    memset(out, 0, sizeof *out);
    size_t len = value != NULL ? strlen(value) : 0;
    out->ptr = strndup_bytes((const unsigned char *)(value != NULL ? value : ""),
                             len);
    if (out->ptr == NULL) return CF_NOMEM;
    out->len = len;
    return CF_OK;
}

/* ----------------------------------------------------------- tokens */

typedef struct {
    char **lines;
    size_t len, cap;
} gb_tokens;

static void gb_dispose(gb_tokens *tokens) {
    for (size_t i = 0; i < tokens->len; i++) free(tokens->lines[i]);
    free(tokens->lines);
    memset(tokens, 0, sizeof *tokens);
}

static int gb_push(gb_tokens *tokens, char *owned) {
    if (owned == NULL) return 0;
    if (tokens->len == tokens->cap) {
        size_t cap = tokens->cap != 0 ? tokens->cap * 2 : 64;
        char **grown = realloc(tokens->lines, cap * sizeof *grown);
        if (grown == NULL) {
            free(owned);
            return 0;
        }
        tokens->lines = grown;
        tokens->cap = cap;
    }
    tokens->lines[tokens->len++] = owned;
    return 1;
}

static int gb_push_cstr(gb_tokens *tokens, const char *text) {
    return gb_push(tokens, strndup_bytes((const unsigned char *)text,
                                         strlen(text)));
}

/* Ruby/`messages_support` whitespace collapse. */
static char *collapse(const char *text, size_t len) {
    char *out = malloc(len + 1);
    if (out == NULL) return NULL;
    size_t at = 0;
    int in_space = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\x0c') {
            if (!in_space) out[at++] = ' ';
            in_space = 1;
        } else {
            out[at++] = (char)c;
            in_space = 0;
        }
    }
    out[at] = '\0';
    return out;
}

/* `push_text`: collapse and merge with a trailing text token. */
static int gb_push_text(gb_tokens *tokens, const char *text, size_t len) {
    char *collapsed = collapse(text, len);
    if (collapsed == NULL) return 0;
    if (tokens->len != 0 && tokens->lines[tokens->len - 1][0] == '#') {
        char *previous = tokens->lines[tokens->len - 1];
        size_t previous_len = strlen(previous + 1);
        size_t collapsed_len = strlen(collapsed);
        char *joined = malloc(previous_len + collapsed_len + 2);
        if (joined == NULL) {
            free(collapsed);
            return 0;
        }
        joined[0] = '#';
        memcpy(joined + 1, previous + 1, previous_len);
        memcpy(joined + 1 + previous_len, collapsed, collapsed_len);
        joined[1 + previous_len + collapsed_len] = '\0';
        free(collapsed);
        char *merged = collapse(joined + 1, strlen(joined + 1));
        free(joined);
        if (merged == NULL) return 0;
        char *result = malloc(strlen(merged) + 2);
        if (result == NULL) {
            free(merged);
            return 0;
        }
        result[0] = '#';
        strcpy(result + 1, merged);
        free(merged);
        free(previous);
        tokens->lines[tokens->len - 1] = result;
        return 1;
    }
    char *result = malloc(strlen(collapsed) + 2);
    if (result == NULL) {
        free(collapsed);
        return 0;
    }
    result[0] = '#';
    strcpy(result + 1, collapsed);
    free(collapsed);
    return gb_push(tokens, result);
}

/* Character-reference decoding (the runner's `decode`). */
static char *gb_decode(const char *text, size_t len) {
    char *out = malloc(len * 4 + 1);
    if (out == NULL) return NULL;
    size_t at = 0;
    size_t i = 0;
    while (i < len) {
        const char *amp = memchr(text + i, '&', len - i);
        if (amp == NULL) {
            memcpy(out + at, text + i, len - i);
            at += len - i;
            break;
        }
        size_t amp_index = (size_t)(amp - text);
        memcpy(out + at, text + i, amp_index - i);
        at += amp_index - i;
        i = amp_index;
        size_t window = len - i < 12 ? len - i : 12;
        const char *semi = memchr(text + i, ';', window);
        if (semi == NULL) {
            out[at++] = '&';
            i += 1;
            continue;
        }
        size_t entity_len = (size_t)(semi - (text + i)) - 1;
        const char *entity = text + i + 1;
        unsigned long cp = 0;
        int have = 0;
        if (entity_len == 3 && memcmp(entity, "amp", 3) == 0) {
            cp = '&';
            have = 1;
        } else if (entity_len == 2 && memcmp(entity, "lt", 2) == 0) {
            cp = '<';
            have = 1;
        } else if (entity_len == 2 && memcmp(entity, "gt", 2) == 0) {
            cp = '>';
            have = 1;
        } else if (entity_len == 4 && memcmp(entity, "quot", 4) == 0) {
            cp = '"';
            have = 1;
        } else if (entity_len == 4 && memcmp(entity, "apos", 4) == 0) {
            cp = '\'';
            have = 1;
        } else if (entity_len == 4 && memcmp(entity, "nbsp", 4) == 0) {
            cp = 0xA0;
            have = 1;
        } else if (entity_len > 2 && (entity[0] == '#')) {
            char buffer[16];
            if (entity[1] == 'x' || entity[1] == 'X') {
                if (entity_len - 2 < sizeof buffer) {
                    memcpy(buffer, entity + 2, entity_len - 2);
                    buffer[entity_len - 2] = '\0';
                    cp = strtoul(buffer, NULL, 16);
                    have = cp != 0 || (entity_len == 3 && entity[2] == '0');
                }
            } else if (entity_len - 1 < sizeof buffer) {
                memcpy(buffer, entity + 1, entity_len - 1);
                buffer[entity_len - 1] = '\0';
                cp = strtoul(buffer, NULL, 10);
                have = 1;
            }
        }
        if (!have) {
            out[at++] = '&';
            i += 1;
            continue;
        }
        /* UTF-8 encode. */
        if (cp < 0x80) {
            out[at++] = (char)cp;
        } else if (cp < 0x800) {
            out[at++] = (char)(0xC0 | (cp >> 6));
            out[at++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[at++] = (char)(0xE0 | (cp >> 12));
            out[at++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[at++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[at++] = (char)(0xF0 | (cp >> 18));
            out[at++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[at++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[at++] = (char)(0x80 | (cp & 0x3F));
        }
        i += entity_len + 2;
    }
    out[at] = '\0';
    return out;
}

static int ascii_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static const char *const VOID_ELEMENTS[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input",
    "keygen", "link", "meta", "source", "track", "wbr"};
static const char *const RAW_TEXT[] = {"script", "style", "textarea", "title"};

static int in_list(const char *name, const char *const *list, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(name, list[i]) == 0) return 1;
    }
    return 0;
}

/* A start tag at `html[i]` ('<'), exactly as `messages_support::start_tag`:
 * returns the rendered token (owned; "" for a forged token), the lowercased
 * name and the index past the tag. */
static char *gb_start_tag(const char *html, size_t len, size_t i,
                          char name[64], size_t *end) {
    size_t at = i + 1;
    size_t name_end = at;
    while (name_end < len && !ascii_space((unsigned char)html[name_end]) &&
           html[name_end] != '>' && html[name_end] != '/') {
        name_end++;
    }
    size_t name_len = name_end - at;
    if (name_len >= 64) name_len = 63;
    for (size_t k = 0; k < name_len; k++) {
        char c = html[at + k];
        name[k] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    name[name_len] = '\0';
    at = name_end;

    struct {
        char key[64];
        char *value;
    } attrs[64];
    size_t attr_count = 0;
    while (1) {
        while (at < len && (ascii_space((unsigned char)html[at]) ||
                            html[at] == '/')) {
            at++;
        }
        if (at >= len || html[at] == '>') {
            at++;
            break;
        }
        size_t key_end = at;
        while (key_end < len && !ascii_space((unsigned char)html[key_end]) &&
               html[key_end] != '=' && html[key_end] != '>') {
            key_end++;
        }
        char key[64];
        size_t key_len = key_end - at;
        if (key_len >= 64) key_len = 63;
        for (size_t k = 0; k < key_len; k++) {
            char c = html[at + k];
            key[k] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        }
        key[key_len] = '\0';
        at = key_end;
        while (at < len && ascii_space((unsigned char)html[at])) at++;
        char *value = NULL;
        if (at < len && html[at] == '=') {
            at++;
            while (at < len && ascii_space((unsigned char)html[at])) at++;
            if (at < len && (html[at] == '"' || html[at] == '\'')) {
                char quote = html[at];
                size_t value_end = at + 1;
                while (value_end < len && html[value_end] != quote) {
                    value_end++;
                }
                value = gb_decode(html + at + 1, value_end - (at + 1));
                at = value_end < len ? value_end + 1 : len;
            } else {
                size_t value_end = at;
                while (value_end < len &&
                       !ascii_space((unsigned char)html[value_end]) &&
                       html[value_end] != '>') {
                    value_end++;
                }
                value = gb_decode(html + at, value_end - at);
                at = value_end;
            }
        } else {
            value = strndup_bytes((const unsigned char *)"", 0);
        }
        if (value == NULL) return NULL;
        int duplicate = 0;
        for (size_t k = 0; k < attr_count; k++) {
            if (strcmp(attrs[k].key, key) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate && attr_count < 64) {
            snprintf(attrs[attr_count].key, sizeof attrs[attr_count].key, "%s",
                     key);
            attrs[attr_count].value = value;
            attr_count++;
        } else {
            free(value);
        }
    }

    int is_forgery = 0;
    if (strcmp(name, "input") == 0) {
        for (size_t k = 0; k < attr_count; k++) {
            if (strcmp(attrs[k].key, "name") == 0 &&
                strcmp(attrs[k].value, "authenticity_token") == 0) {
                is_forgery = 1;
            }
        }
    } else if (strcmp(name, "meta") == 0) {
        for (size_t k = 0; k < attr_count; k++) {
            if (strcmp(attrs[k].key, "name") == 0 &&
                (strcmp(attrs[k].value, "csrf-token") == 0 ||
                 strcmp(attrs[k].value, "csrf-param") == 0)) {
                is_forgery = 1;
            }
        }
    }
    char *token = NULL;
    if (!is_forgery) {
        size_t capacity = strlen(name) + 3;
        for (size_t k = 0; k < attr_count; k++) {
            capacity += strlen(attrs[k].key) +
                        strlen(attrs[k].value) * 6 + 8;
        }
        token = malloc(capacity);
        if (token == NULL) {
            for (size_t k = 0; k < attr_count; k++) free(attrs[k].value);
            return NULL;
        }
        size_t wrote = 0;
        wrote += (size_t)snprintf(token + wrote, capacity - wrote, "<%s",
                                  name);
        for (size_t k = 0; k < attr_count; k++) {
            wrote += (size_t)snprintf(token + wrote, capacity - wrote, " %s=\"",
                                      attrs[k].key);
            for (const char *p = attrs[k].value; *p != '\0'; p++) {
                const char *replacement = NULL;
                if (*p == '&') replacement = "&amp;";
                else if (*p == '"') replacement = "&quot;";
                else if (*p == '<') replacement = "&lt;";
                if (replacement != NULL) {
                    memcpy(token + wrote, replacement, strlen(replacement));
                    wrote += strlen(replacement);
                } else {
                    token[wrote++] = *p;
                }
            }
            token[wrote++] = '"';
        }
        token[wrote++] = '>';
        token[wrote] = '\0';
    }
    for (size_t k = 0; k < attr_count; k++) free(attrs[k].value);
    *end = at;
    return token;
}

/* The runner's whole-document tokenizer. */
static int gb_tokenize(const char *html, size_t len, gb_tokens *out) {
    char *text = malloc(len + 1);
    if (text == NULL) return 0;
    size_t text_len = 0;
    char *open[256];
    size_t open_count = 0;
    size_t i = 0;
    int ok = 1;
#define GB_FLUSH()                                                          \
    do {                                                                    \
        if (text_len != 0) {                                                \
            char *decoded = gb_decode(text, text_len);                      \
            if (decoded == NULL) {                                          \
                ok = 0;                                                     \
                goto done;                                                  \
            }                                                               \
            if (!gb_push_text(out, decoded, strlen(decoded))) {             \
                free(decoded);                                              \
                ok = 0;                                                     \
                goto done;                                                  \
            }                                                               \
            free(decoded);                                                  \
            text_len = 0;                                                   \
        }                                                                   \
    } while (0)
    while (i < len) {
        if (html[i] == '<') {
            if (len - i >= 4 && memcmp(html + i, "<!--", 4) == 0) {
                GB_FLUSH();
                const char *close = NULL;
                for (size_t k = i; k + 3 <= len; k++) {
                    if (memcmp(html + k, "-->", 3) == 0) {
                        close = html + k;
                        break;
                    }
                }
                i = close != NULL ? (size_t)(close - html) + 3 : len;
                continue;
            }
            if (len - i >= 2 && html[i + 1] == '!') {
                GB_FLUSH();
                const char *close = memchr(html + i, '>', len - i);
                i = close != NULL ? (size_t)(close - html) + 1 : len;
                continue;
            }
            if (len - i >= 2 && html[i + 1] == '/') {
                GB_FLUSH();
                const char *close = memchr(html + i, '>', len - i);
                size_t end = close != NULL ? (size_t)(close - html) : len;
                char name[64];
                size_t at = i + 2;
                while (at < end && !ascii_space((unsigned char)html[at])) at++;
                size_t name_len = at - (i + 2);
                if (name_len >= 64) name_len = 63;
                for (size_t k = 0; k < name_len; k++) {
                    char c = html[i + 2 + k];
                    name[k] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
                }
                name[name_len] = '\0';
                size_t depth = open_count;
                for (size_t k = open_count; k > 0; k--) {
                    if (strcmp(open[k - 1], name) == 0) {
                        depth = k - 1;
                        break;
                    }
                }
                if (depth < open_count) {
                    for (size_t k = open_count; k > depth; k--) {
                        char close_token[80];
                        snprintf(close_token, sizeof close_token, "</%s>",
                                 open[k - 1]);
                        free(open[k - 1]);
                        if (!gb_push_cstr(out, close_token)) {
                            ok = 0;
                            goto done;
                        }
                    }
                    open_count = depth;
                }
                i = end + 1;
                continue;
            }
            if (i + 1 < len &&
                ((html[i + 1] >= 'a' && html[i + 1] <= 'z') ||
                 (html[i + 1] >= 'A' && html[i + 1] <= 'Z'))) {
                GB_FLUSH();
                char name[64];
                size_t end = 0;
                char *token = gb_start_tag(html, len, i, name, &end);
                if (token == NULL) {
                    ok = 0;
                    goto done;
                }
                if (token[0] != '\0' && !gb_push(out, token)) {
                    ok = 0;
                    goto done;
                }
                if (token[0] == '\0') free(token);
                i = end;
                if (!in_list(name, VOID_ELEMENTS,
                             sizeof VOID_ELEMENTS / sizeof VOID_ELEMENTS[0])) {
                    if (open_count < 256) {
                        open[open_count] = strndup_bytes(
                            (const unsigned char *)name, strlen(name));
                        if (open[open_count] == NULL) {
                            ok = 0;
                            goto done;
                        }
                        open_count++;
                    }
                }
                if (in_list(name, RAW_TEXT,
                            sizeof RAW_TEXT / sizeof RAW_TEXT[0])) {
                    char close[80];
                    snprintf(close, sizeof close, "</%s", name);
                    size_t stop = len;
                    for (size_t k = i; k + strlen(close) <= len; k++) {
                        int match = 1;
                        for (size_t m = 0; m < strlen(close); m++) {
                            char c = html[k + m];
                            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                            if (c != close[m]) {
                                match = 0;
                                break;
                            }
                        }
                        if (match) {
                            stop = k;
                            break;
                        }
                    }
                    const char *raw = html + i;
                    size_t raw_len = stop - i;
                    if (raw_len != 0) {
                        if (strcmp(name, "script") == 0 ||
                            strcmp(name, "style") == 0) {
                            if (!gb_push_text(out, raw, raw_len)) {
                                ok = 0;
                                goto done;
                            }
                        } else {
                            char *decoded = gb_decode(raw, raw_len);
                            if (decoded == NULL ||
                                !gb_push_text(out, decoded, strlen(decoded))) {
                                free(decoded);
                                ok = 0;
                                goto done;
                            }
                            free(decoded);
                        }
                    }
                    i = stop;
                }
                continue;
            }
        }
        {
            const char *next = memchr(html + i, '<', len - i);
            size_t stop = next != NULL ? (size_t)(next - html) : len;
            if (stop <= i) stop = i + 1;
            memcpy(text + text_len, html + i, stop - i);
            text_len += stop - i;
            i = stop;
        }
    }
    GB_FLUSH();
    for (size_t k = open_count; k > 0; k--) {
        char close_token[80];
        snprintf(close_token, sizeof close_token, "</%s>", open[k - 1]);
        free(open[k - 1]);
        if (!gb_push_cstr(out, close_token)) {
            ok = 0;
            goto done;
        }
    }
    open_count = 0;
done:
    for (size_t k = 0; k < open_count; k++) free(open[k]);
    free(text);
    if (!ok) {
        gb_dispose(out);
        return 0;
    }
    /* `out.retain(|t| t != "#")` */
    {
        size_t at = 0;
        for (size_t k = 0; k < out->len; k++) {
            if (strcmp(out->lines[k], "#") == 0) {
                free(out->lines[k]);
            } else {
                out->lines[at++] = out->lines[k];
            }
        }
        out->len = at;
    }
    return 1;
#undef GB_FLUSH
}

/* ------------------------------------------------------- expected masks */

static char *with_relative_copy_link(const char *token) {
    const char *absolute = "data-copy-to-clipboard-content-value=\"http";
    if (strstr(token, "title=\"Copy link\"") == NULL) {
        return strndup_bytes((const unsigned char *)token, strlen(token));
    }
    const char *start = strstr(token, absolute);
    if (start == NULL) {
        return strndup_bytes((const unsigned char *)token, strlen(token));
    }
    size_t value_start = (size_t)(start - token) + strlen(absolute);
    const char *value_end_at = strchr(token + value_start, '"');
    if (value_end_at == NULL) {
        return strndup_bytes((const unsigned char *)token, strlen(token));
    }
    size_t value_end = (size_t)(value_end_at - token);
    const char *scheme = strstr(token + value_start, "://");
    const char *path = scheme != NULL ? strchr(scheme + 3, '/') : NULL;
    if (path == NULL || (size_t)(path - token) > value_end) {
        return strndup_bytes((const unsigned char *)token, strlen(token));
    }
    size_t path_len = value_end - (size_t)(path - token);
    size_t prefix_len = (size_t)(start - token);
    const char *replacement = "data-copy-to-clipboard-url-value=\"";
    size_t suffix_at = value_end + 1;
    size_t total = prefix_len + strlen(replacement) + path_len + 1 +
                   strlen(token + suffix_at) + 1;
    char *out = malloc(total);
    if (out == NULL) return NULL;
    size_t at = 0;
    memcpy(out + at, token, prefix_len);
    at += prefix_len;
    memcpy(out + at, replacement, strlen(replacement));
    at += strlen(replacement);
    memcpy(out + at, path, path_len);
    at += path_len;
    out[at++] = '"';
    strcpy(out + at, token + suffix_at);
    return out;
}

static int is_forgery_token(const char *token) {
    if (strncmp(token, "<input ", 7) == 0 &&
        strstr(token, "name=\"authenticity_token\"") != NULL) {
        return 1;
    }
    if (strncmp(token, "<meta ", 6) == 0 &&
        (strstr(token, "name=\"csrf-token\"") != NULL ||
         strstr(token, "name=\"csrf-param\"") != NULL)) {
        return 1;
    }
    return 0;
}

/* `without_forgery_tokens`: drop the CSRF tokens, apply the copy-link mask,
 * merge text runs. */
static int without_forgery(char **expected, size_t count, gb_tokens *out) {
    for (size_t i = 0; i < count; i++) {
        const char *token = expected[i];
        if (is_forgery_token(token)) continue;
        char *masked = with_relative_copy_link(token);
        if (masked == NULL) return 0;
        if (masked[0] == '#') {
            if (!gb_push_text(out, masked + 1, strlen(masked + 1))) {
                free(masked);
                return 0;
            }
            free(masked);
        } else {
            if (!gb_push(out, masked)) return 0;
        }
    }
    return 1;
}

/* --------------------------------------------------------- region logic */

static void trim_whitespace(gb_tokens *tokens) {
    while (tokens->len != 0 && strcmp(tokens->lines[0], "# ") == 0) {
        free(tokens->lines[0]);
        memmove(tokens->lines, tokens->lines + 1,
                (tokens->len - 1) * sizeof *tokens->lines);
        tokens->len--;
    }
    while (tokens->len != 0 &&
           strcmp(tokens->lines[tokens->len - 1], "# ") == 0) {
        free(tokens->lines[tokens->len - 1]);
        tokens->len--;
    }
}

static void tag_name_of(const char *token, char out[64]) {
    size_t at = 0;
    const char *p = token;
    if (p[0] == '<') p++;
    if (p[0] == '/') p++;
    while (*p != '\0' && *p != ' ' && *p != '>' && at < 63) {
        char c = *p;
        out[at++] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        p++;
    }
    out[at] = '\0';
}

/* `element_children(tokens, is_start)`, copied into out. */
static void element_children(const gb_tokens *tokens, const char *start_marker,
                             gb_tokens *out) {
    size_t start = (size_t)-1;
    for (size_t i = 0; i < tokens->len; i++) {
        if (strncmp(tokens->lines[i], start_marker, strlen(start_marker)) ==
            0) {
            start = i;
            break;
        }
    }
    if (start == (size_t)-1) return;
    char name[64];
    tag_name_of(tokens->lines[start], name);
    int is_void = in_list(name, VOID_ELEMENTS,
                          sizeof VOID_ELEMENTS / sizeof VOID_ELEMENTS[0]);
    size_t depth = 0;
    size_t stop = tokens->len;
    for (size_t offset = start + 1; offset < tokens->len; offset++) {
        const char *token = tokens->lines[offset];
        if (token[0] == '<' && token[1] != '/') {
            char other[64];
            tag_name_of(token, other);
            if (strcmp(other, name) == 0 && !is_void) depth++;
        } else {
            char close[80];
            snprintf(close, sizeof close, "</%s>", name);
            if (strcmp(token, close) == 0) {
                if (depth == 0) {
                    stop = offset;
                    break;
                }
                depth--;
            }
        }
    }
    for (size_t i = start + 1; i < stop; i++) {
        if (!gb_push_cstr(out, tokens->lines[i])) return;
    }
}

static void head_block(const gb_tokens *tokens, gb_tokens *out) {
    gb_tokens head = {0};
    element_children(tokens, "<head>", &head);
    size_t start = head.len;
    for (size_t k = head.len; k > 0; k--) {
        if (strncmp(head.lines[k - 1], "<script type=\"module\"", 21) == 0) {
            start = k + 2;
            break;
        }
    }
    if (start > head.len) start = head.len;
    for (size_t i = start; i < head.len; i++) {
        gb_push_cstr(out, head.lines[i]);
    }
    gb_dispose(&head);
}

/* One named region (the runner's regions_named); caller disposes. */
static void region(const gb_tokens *tokens, const char *name, gb_tokens *out) {
    if (strcmp(name, "title") == 0) {
        element_children(tokens, "<title>", out);
    } else if (strcmp(name, "head") == 0) {
        head_block(tokens, out);
    } else if (strcmp(name, "nav") == 0) {
        element_children(tokens, "<nav id=\"nav\"", out);
    } else if (strcmp(name, "main") == 0) {
        int has_main = 0;
        for (size_t i = 0; i < tokens->len; i++) {
            if (strncmp(tokens->lines[i], "<main id=\"main-content\"", 23) ==
                0) {
                has_main = 1;
                break;
            }
        }
        if (!has_main) {
            element_children(tokens, "<body", out);
            trim_whitespace(out);
            return;
        }
        element_children(tokens, "<main id=\"main-content\"", out);
        for (size_t i = 0; i < out->len; i++) {
            if (strncmp(out->lines[i], "<footer id=\"footer\"", 20) == 0) {
                while (out->len > i) {
                    free(out->lines[out->len - 1]);
                    out->len--;
                }
                break;
            }
        }
    } else if (strcmp(name, "footer") == 0) {
        element_children(tokens, "<footer id=\"footer\"", out);
    } else if (strcmp(name, "sidebar") == 0) {
        element_children(tokens, "<aside id=\"sidebar\"", out);
    }
    trim_whitespace(out);
}

/* ------------------------------------------------------------- comparison */

static const char *const REGION_NAMES[6] = {"title", "head", "nav",
                                            "main",  "footer", "sidebar"};

static int same_tokens(const gb_tokens *want, const gb_tokens *got) {
    if (want->len != got->len) return 0;
    for (size_t i = 0; i < want->len; i++) {
        if (strcmp(want->lines[i], got->lines[i]) != 0) return 0;
    }
    return 1;
}

static void report_diff(const char *label, const gb_tokens *want,
                        const gb_tokens *got) {
    size_t index = 0;
    while (index < want->len && index < got->len &&
           strcmp(want->lines[index], got->lines[index]) == 0) {
        index++;
    }
    if (index == want->len && index == got->len) return;
    printf("    %s: DOM differs at token %zu (expected %zu tokens, got "
           "%zu)\n",
           label, index, want->len, got->len);
    size_t from = index > 4 ? index - 4 : 0;
    printf("      expected:\n");
    for (size_t i = from; i < index + 6 && i < want->len; i++) {
        printf("        %s\n", want->lines[i]);
    }
    printf("      actual:\n");
    for (size_t i = from; i < index + 6 && i < got->len; i++) {
        printf("        %s\n", got->lines[i]);
    }
}

static void compare(const char *label, gb_tokens *want, gb_tokens *got) {
    trim_whitespace(want);
    trim_whitespace(got);
    if (same_tokens(want, got)) return;
    report_diff(label, want, got);
    CF_CHECK(0);
}

static void dump_actual(const char *name, const unsigned char *actual,
                        size_t len) {
    const char *dir = getenv("CF_GOLDEN_DUMP");
    if (dir == NULL || dir[0] == '\0') return;
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/%s.actual.html", dir, name);
    if (n < 0 || (size_t)n >= sizeof path) return;
    FILE *file = fopen(path, "wb");
    if (file == NULL) return;
    fwrite(actual, 1, len, file);
    fclose(file);
}

void cf_golden_b_expect(yyjson_doc *doc, const char *name, int dom,
                        const unsigned char *actual, size_t actual_len) {
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *kind = yyjson_get_str(yyjson_obj_get(root, "kind"));
    yyjson_val *expected = yyjson_obj_get(root, "expected");
    if (kind == NULL || !yyjson_is_arr(expected)) {
        printf("    %s: fixture has no token stream\n", name);
        CF_CHECK(0);
        return;
    }
    dump_actual(name, actual, actual_len);

    size_t count = yyjson_arr_size(expected);
    char **raw = malloc((count != 0 ? count : 1) * sizeof *raw);
    if (raw == NULL) {
        CF_CHECK(0);
        return;
    }
    size_t at = 0;
    yyjson_val *item;
    yyjson_arr_iter iter = yyjson_arr_iter_with(expected);
    while ((item = yyjson_arr_iter_next(&iter)) != NULL) {
        if (yyjson_is_str(item)) raw[at++] = (char *)yyjson_get_str(item);
    }
    gb_tokens want = {0};
    int ok = without_forgery(raw, at, &want);
    free(raw);
    if (!ok) {
        gb_dispose(&want);
        CF_CHECK(0);
        return;
    }

    gb_tokens got = {0};
    if (!gb_tokenize((const char *)actual, actual_len, &got)) {
        gb_dispose(&want);
        CF_CHECK(0);
        return;
    }

    int page = strcmp(kind, "page") == 0;
    if (dom) {
        if (page) {
            /* The reference's region list: the six named regions for a page
             * with a main, else the frame layout's body. */
            int has_main = 0;
            for (size_t i = 0; i < want.len; i++) {
                if (strncmp(want.lines[i], "<main id=\"main-content\"", 23) ==
                    0) {
                    has_main = 1;
                    break;
                }
            }
            const char *names[6];
            size_t name_count = 0;
            if (has_main) {
                for (size_t r = 0; r < 6; r++) {
                    names[name_count++] = REGION_NAMES[r];
                }
            } else {
                names[name_count++] = "main"; /* the body fallback */
            }
            for (size_t r = 0; r < name_count; r++) {
                gb_tokens want_region = {0}, got_region = {0};
                region(&want, names[r], &want_region);
                region(&got, names[r], &got_region);
                char label[128];
                snprintf(label, sizeof label, "%s [%s]", name, names[r]);
                compare(label, &want_region, &got_region);
                gb_dispose(&want_region);
                gb_dispose(&got_region);
            }
        } else {
            compare(name, &want, &got);
        }
    } else {
        if (page) {
            gb_tokens want_main = {0};
            region(&want, "main", &want_main);
            compare(name, &want_main, &got);
            gb_dispose(&want_main);
        } else {
            compare(name, &want, &got);
        }
    }
    gb_dispose(&want);
    gb_dispose(&got);
}

/* ------------------------------------------------------------ loading */

static const char *golden_b_dir(void) {
    const char *dir = getenv("CF_GOLDEN_DIR");
    return dir != NULL && dir[0] != '\0'
               ? dir
               : "tests/fixtures/crates/views/tests/golden";
}

yyjson_doc *cf_golden_b_load(const char *name) {
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/b/%s.json", golden_b_dir(), name);
    if (n < 0 || (size_t)n >= sizeof path) return NULL;
    yyjson_doc *doc = yyjson_read_file(path, 0, NULL, NULL);
    if (doc == NULL) {
        fprintf(stderr, "golden_b: cannot parse %s\n", path);
    }
    return doc;
}

yyjson_val *cf_golden_b_input_at(yyjson_val *input, const char *key) {
    return input != NULL ? yyjson_obj_get(input, key) : NULL;
}

/* ----------------------------------------------------------- context */

static yyjson_val *g_assets;

static cf_err golden_b_asset_path(void *user, cf_span logical,
                                  cf_builder *out) {
    yyjson_val *assets = user;
    if (assets == NULL || logical.len >= 512) return CF_INVALID;
    char key[512];
    memcpy(key, logical.ptr, logical.len);
    key[logical.len] = '\0';
    yyjson_val *value = yyjson_obj_get(assets, key);
    if (value == NULL || !yyjson_is_str(value)) return CF_NOT_FOUND;
    const char *url = yyjson_get_str(value);
    return cf_builder_append(out, (cf_span){(const unsigned char *)url,
                                            strlen(url)});
}

void cf_golden_b_ctx(cf_view_ctx *out, yyjson_doc *doc) {
    memset(out, 0, sizeof *out);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *context = yyjson_obj_get(root, "context");
    out->base_url = (cf_span){
        (const unsigned char *)yyjson_get_str(yyjson_obj_get(context,
                                                             "base_url")),
        strlen(yyjson_get_str(yyjson_obj_get(context, "base_url")))};
    yyjson_val *user = yyjson_obj_get(context, "current_user");
    if (yyjson_is_obj(user)) {
        out->current_user.has_user = true;
        out->current_user.id = yyjson_get_sint(yyjson_obj_get(user, "id"));
        const char *name = yyjson_get_str(yyjson_obj_get(user, "name"));
        const char *avatar =
            yyjson_get_str(yyjson_obj_get(user, "avatar_url"));
        out->current_user.name =
            (cf_str){(char *)(uintptr_t)name, strlen(name)};
        out->current_user.avatar_url =
            (cf_str){(char *)(uintptr_t)avatar, strlen(avatar)};
        out->current_user.administrator =
            yyjson_get_bool(yyjson_obj_get(user, "administrator"));
        out->current_user.bot = yyjson_get_bool(yyjson_obj_get(user, "bot"));
    }
    yyjson_val *account = yyjson_obj_get(context, "account");
    if (yyjson_is_obj(account)) {
        const char *name = yyjson_get_str(yyjson_obj_get(account, "name"));
        const char *logo = yyjson_get_str(yyjson_obj_get(account, "logo_url"));
        out->account.name = (cf_str){(char *)(uintptr_t)name, strlen(name)};
        out->account.logo_url =
            (cf_str){(char *)(uintptr_t)logo, strlen(logo)};
        out->account.has_logo =
            yyjson_get_bool(yyjson_obj_get(account, "has_logo"));
    }
    yyjson_val *platform = yyjson_obj_get(context, "platform");
    if (yyjson_is_obj(platform)) {
        const char *flag_names[11] = {"ios",     "android", "mac",
                                      "windows", "chrome",  "firefox",
                                      "safari",  "edge",    "mobile",
                                      "desktop", "apple_messages"};
        bool *flags[11] = {&out->platform.ios,     &out->platform.android,
                           &out->platform.mac,     &out->platform.windows,
                           &out->platform.chrome,  &out->platform.firefox,
                           &out->platform.safari,  &out->platform.edge,
                           &out->platform.mobile,  &out->platform.desktop,
                           &out->platform.apple_messages};
        for (size_t i = 0; i < 11; i++) {
            *flags[i] = yyjson_get_bool(yyjson_obj_get(platform,
                                                       flag_names[i]));
        }
        yyjson_val *browser = yyjson_obj_get(platform, "browser");
        yyjson_val *os = yyjson_obj_get(platform, "operating_system");
        if (yyjson_is_str(browser)) {
            out->platform.browser =
                (cf_span){(const unsigned char *)yyjson_get_str(browser),
                          strlen(yyjson_get_str(browser))};
        }
        if (yyjson_is_str(os)) {
            out->platform.operating_system =
                (cf_span){(const unsigned char *)yyjson_get_str(os),
                          strlen(yyjson_get_str(os))};
        }
    }
    g_assets = yyjson_obj_get(context, "assets");
    out->asset_path = golden_b_asset_path;
    out->asset_path_user = g_assets;
    /* The runner's fixed tags: the test compares after the importmap. */
    out->importmap_tags = (cf_span){
        (const unsigned char *)"<script type=\"module\">import "
                               "\"application\"</script>",
        strlen("<script type=\"module\">import \"application\"</script>")};
    out->stylesheet_tags = (cf_span){NULL, 0};
    out->app_version = (cf_span){(const unsigned char *)"0", 1};
}

/* ------------------------------------------------------- model builders */

void cf_golden_b_user(yyjson_val *object, cf_view_user *out) {
    memset(out, 0, sizeof *out);
    if (!yyjson_is_obj(object)) return;
    out->id = yyjson_get_sint(yyjson_obj_get(object, "id"));
    copy_str(yyjson_get_str(yyjson_obj_get(object, "name")), &out->name);
    copy_str(yyjson_get_str(yyjson_obj_get(object, "title")), &out->title);
    copy_str(yyjson_get_str(yyjson_obj_get(object, "avatar_url")),
             &out->avatar_url);
}

void cf_golden_b_room(yyjson_val *object, cf_view_room *out) {
    memset(out, 0, sizeof *out);
    if (!yyjson_is_obj(object)) return;
    out->id = yyjson_get_sint(yyjson_obj_get(object, "id"));
    const char *kind = yyjson_get_str(yyjson_obj_get(object, "kind"));
    out->kind = kind != NULL && strcmp(kind, "closed") == 0
                    ? CF_ROOM_CLOSED
                    : (kind != NULL && strcmp(kind, "direct") == 0
                           ? CF_ROOM_DIRECT
                           : CF_ROOM_OPEN);
    yyjson_val *name = yyjson_obj_get(object, "name");
    if (yyjson_is_str(name)) {
        out->has_name = true;
        copy_str(yyjson_get_str(name), &out->name);
    }
    copy_str(yyjson_get_str(yyjson_obj_get(object, "display_name")),
             &out->display_name);
}

static cf_view_number gb_number(yyjson_val *value) {
    cf_view_number number = {0};
    if (yyjson_is_int(value)) {
        number.integer = yyjson_get_sint(value);
    } else if (yyjson_is_real(value)) {
        number.is_float = true;
        number.real = yyjson_get_real(value);
    }
    return number;
}

void cf_golden_b_boost(yyjson_val *object, cf_view_boost *out) {
    memset(out, 0, sizeof *out);
    if (!yyjson_is_obj(object)) return;
    out->id = yyjson_get_sint(yyjson_obj_get(object, "id"));
    yyjson_val *updated = yyjson_obj_get(object, "updated_at");
    if (yyjson_is_str(updated)) out->updated_at_us = 1; /* version present */
    out->message_id = yyjson_get_sint(yyjson_obj_get(object, "message_id"));
    copy_str(yyjson_get_str(yyjson_obj_get(object, "content")), &out->content);
    out->all_emoji = yyjson_get_bool(yyjson_obj_get(object, "all_emoji"));
    cf_golden_b_user(yyjson_obj_get(object, "booster"), &out->booster);
}

static int64_t parse_iso_us(const char *text) {
    if (text == NULL) return 0;
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (sscanf(text, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute,
               &second) != 6) {
        return 0;
    }
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    time_t seconds = timegm(&tm);
    int64_t micros = 0;
    const char *dot = strchr(text, '.');
    if (dot != NULL) {
        int digits = 0;
        int64_t value = 0;
        for (const char *p = dot + 1; *p >= '0' && *p <= '9' && digits < 6;
             p++, digits++) {
            value = value * 10 + (*p - '0');
        }
        while (digits < 6) {
            value *= 10;
            digits++;
        }
        micros = value;
    }
    return (int64_t)seconds * 1000000 + micros;
}

static void gb_message_content(yyjson_val *content, cf_view_message *out) {
    const char *type =
        yyjson_is_obj(content)
            ? yyjson_get_str(yyjson_obj_get(content, "type"))
            : NULL;
    if (type != NULL && strcmp(type, "text") == 0) {
        out->content_kind = CF_VIEW_CONTENT_TEXT;
        copy_str(yyjson_get_str(yyjson_obj_get(content, "html")),
                 &out->text_html);
    } else if (type != NULL && strcmp(type, "sound") == 0) {
        out->content_kind = CF_VIEW_CONTENT_SOUND;
        copy_str(yyjson_get_str(yyjson_obj_get(content, "url")),
                 &out->sound.url);
        yyjson_val *image = yyjson_obj_get(content, "image");
        if (yyjson_is_obj(image)) {
            out->sound.has_image = true;
            copy_str(yyjson_get_str(yyjson_obj_get(image, "src")),
                     &out->sound.image.src);
            out->sound.image.width = yyjson_get_sint(
                yyjson_obj_get(image, "width"));
            out->sound.image.height = yyjson_get_sint(
                yyjson_obj_get(image, "height"));
        }
        yyjson_val *text = yyjson_obj_get(content, "text");
        if (yyjson_is_str(text)) {
            out->sound.has_text = true;
            copy_str(yyjson_get_str(text), &out->sound.text);
        }
    } else if (type != NULL && strcmp(type, "attachment") == 0) {
        out->content_kind = CF_VIEW_CONTENT_ATTACHMENT;
        copy_str(yyjson_get_str(yyjson_obj_get(content, "filename")),
                 &out->attachment.filename);
        copy_str(yyjson_get_str(yyjson_obj_get(content, "blob_path")),
                 &out->attachment.blob_path);
        copy_str(yyjson_get_str(yyjson_obj_get(content, "download_path")),
                 &out->attachment.download_path);
        yyjson_val *preview = yyjson_obj_get(content, "preview");
        const char *preview_type =
            yyjson_is_obj(preview)
                ? yyjson_get_str(yyjson_obj_get(preview, "type"))
                : NULL;
        if (preview_type != NULL && strcmp(preview_type, "video") == 0) {
            out->attachment.preview = CF_VIEW_PREVIEW_VIDEO;
            copy_str(yyjson_get_str(yyjson_obj_get(preview, "poster_url")),
                     &out->attachment.preview_url);
        } else if (preview_type != NULL &&
                   strcmp(preview_type, "image") == 0) {
            out->attachment.preview = CF_VIEW_PREVIEW_IMAGE;
            copy_str(yyjson_get_str(yyjson_obj_get(preview, "thumb_url")),
                     &out->attachment.preview_url);
        } else {
            out->attachment.preview = CF_VIEW_PREVIEW_FILE;
        }
        yyjson_val *width = yyjson_obj_get(content, "width");
        if (yyjson_is_int(width) || yyjson_is_real(width)) {
            out->attachment.has_width = true;
            out->attachment.width = gb_number(width);
        }
        yyjson_val *height = yyjson_obj_get(content, "height");
        if (yyjson_is_int(height) || yyjson_is_real(height)) {
            out->attachment.has_height = true;
            out->attachment.height = gb_number(height);
        }
    } else {
        out->content_kind = CF_VIEW_CONTENT_UNRENDERABLE;
    }
}

void cf_golden_b_message(yyjson_val *object, cf_view_message *out) {
    memset(out, 0, sizeof *out);
    if (!yyjson_is_obj(object)) return;
    out->id = yyjson_get_sint(yyjson_obj_get(object, "id"));
    copy_str(yyjson_get_str(yyjson_obj_get(object, "client_message_id")),
             &out->client_message_id);
    out->room_id = yyjson_get_sint(yyjson_obj_get(object, "room_id"));
    copy_str(yyjson_get_str(yyjson_obj_get(object, "room_name")),
             &out->room_name);
    cf_golden_b_user(yyjson_obj_get(object, "creator"), &out->creator);
    out->created_at_us = parse_iso_us(
        yyjson_get_str(yyjson_obj_get(object, "created_at")));
    out->updated_at_us = parse_iso_us(
        yyjson_get_str(yyjson_obj_get(object, "updated_at")));
    out->all_emoji = yyjson_get_bool(yyjson_obj_get(object, "all_emoji"));
    gb_message_content(yyjson_obj_get(object, "content"), out);
    yyjson_val *boosts = yyjson_obj_get(object, "boosts");
    if (yyjson_is_arr(boosts)) {
        size_t count = yyjson_arr_size(boosts);
        if (count != 0) {
            out->boosts.items = calloc(count, sizeof *out->boosts.items);
            if (out->boosts.items == NULL) return;
            out->boosts.cap = count;
            yyjson_val *boost;
            yyjson_arr_iter iter = yyjson_arr_iter_with(boosts);
            while ((boost = yyjson_arr_iter_next(&iter)) != NULL) {
                cf_golden_b_boost(boost,
                                  &out->boosts.items[out->boosts.len]);
                out->boosts.len++;
            }
        }
    }
}

void cf_golden_b_message_item(yyjson_val *object, cf_view_message_item *out) {
    memset(out, 0, sizeof *out);
    cf_golden_b_message(object, &out->message);
}

void cf_golden_b_message_items(yyjson_val *array,
                               cf_view_message_item_vector *out) {
    memset(out, 0, sizeof *out);
    if (!yyjson_is_arr(array)) return;
    size_t count = yyjson_arr_size(array);
    if (count == 0) return;
    out->items = calloc(count, sizeof *out->items);
    if (out->items == NULL) return;
    out->cap = count;
    yyjson_val *item;
    yyjson_arr_iter iter = yyjson_arr_iter_with(array);
    while ((item = yyjson_arr_iter_next(&iter)) != NULL) {
        cf_golden_b_message_item(item, &out->items[out->len]);
        out->len++;
    }
}

void cf_golden_b_show(yyjson_val *input, cf_view_room_show_model *out) {
    memset(out, 0, sizeof *out);
    if (!yyjson_is_obj(input)) return;
    cf_golden_b_room(yyjson_obj_get(input, "room"), &out->room);
    out->updated_at_us = parse_iso_us(
        yyjson_get_str(yyjson_obj_get(input, "updated_at")));
    cf_golden_b_user(yyjson_obj_get(input, "user"), &out->user);
    cf_golden_b_message_items(yyjson_obj_get(input, "messages"),
                              &out->messages);
    out->invitation = yyjson_get_bool(yyjson_obj_get(input, "invitation"));
    copy_str(yyjson_get_str(yyjson_obj_get(input, "join_code")),
             &out->join_code);
    copy_str(yyjson_get_str(yyjson_obj_get(input, "messages_stream_name")),
             &out->messages_stream_name);
}

void cf_golden_b_edit(yyjson_val *input, cf_view_message_edit_model *out) {
    memset(out, 0, sizeof *out);
    if (!yyjson_is_obj(input)) return;
    cf_golden_b_message(yyjson_obj_get(input, "message"), &out->message);
    yyjson_val *body = yyjson_obj_get(input, "editable_body_html");
    if (yyjson_is_str(body)) {
        copy_str(yyjson_get_str(body), &out->editable_body_html);
    } else {
        copy_str("", &out->editable_body_html);
    }
}
