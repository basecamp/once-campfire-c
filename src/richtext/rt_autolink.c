/* src/richtext/rt_autolink.c — rails_autolink's auto_link over the serialized
 * HTML, as MessagesHelper#message_presentation calls it.
 *
 * Ports tmp/rust-ref/crates/richtext/src/autolink.rs, including the TagIndex
 * that answers auto_linked? in log time and the deliberate divergence that
 * closes rails_autolink's stored XSS: the sanitized input has `<` and `>` in
 * attribute values escaped, so a URL can never look like it is inside text.
 */
#include "richtext/internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- small size_t vector ------------------------------------------------ */

typedef struct {
    size_t *items;
    size_t len, cap;
} rt_pos_vec;

static void pos_init(rt_pos_vec *vec) {
    vec->items = NULL;
    vec->len = 0;
    vec->cap = 0;
}

static void pos_dispose(rt_pos_vec *vec) {
    free(vec->items);
    vec->items = NULL;
    vec->len = 0;
    vec->cap = 0;
}

static bool pos_push(rt_pos_vec *vec, size_t value) {
    if (vec->len == vec->cap) {
        size_t cap = vec->cap != 0 ? vec->cap * 2 : 16;
        if (cap < vec->cap) return false;
        size_t *grown = realloc(vec->items, cap * sizeof *grown);
        if (grown == NULL) return false;
        vec->items = grown;
        vec->cap = cap;
    }
    vec->items[vec->len++] = value;
    return true;
}

/* Count of positions strictly less than `value` (partition_point). */
static size_t pos_partition_lt(const rt_pos_vec *vec, size_t value) {
    size_t lo = 0, hi = vec->len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (vec->items[mid] < value) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static size_t pos_partition_le(const rt_pos_vec *vec, size_t value) {
    size_t lo = 0, hi = vec->len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (vec->items[mid] <= value) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* ---- Unicode \w (Rust regex's word class) -------------------------------- */

static bool rt_is_word_cp(uint32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (c == 0x200C || c == 0x200D) return true; /* Join_Control */
    if (rt_is_whitespace(c)) return false;
    /* Non-word ranges for the URL-trailing characters the corpus and real
     * messages contain: Latin-1 punctuation, general punctuation, currency,
     * arrows/math/technical/geometric/misc symbols, CJK punctuation, emoji. */
    if (c >= 0x00A1 && c <= 0x00BF) {
        return c == 0x00AA || c == 0x00B2 || c == 0x00B3 || c == 0x00B5 || c == 0x00B9 ||
               c == 0x00BA;
    }
    if (c == 0x00D7 || c == 0x00F7) return false;
    if (c >= 0x2000 && c <= 0x206F) return false;
    if (c >= 0x20A0 && c <= 0x20CF) return false;
    if (c >= 0x2100 && c <= 0x214F) return false;
    if (c >= 0x2190 && c <= 0x2BFF) return false;
    if (c >= 0x3000 && c <= 0x303F) return false;
    if (c >= 0xFE10 && c <= 0xFE6F) return false; /* vertical/vertical forms, small forms */
    if (c >= 0x1F000 && c <= 0x1FAFF) return false; /* emoji, mahjong, dominos, ... */
    if (c >= 0xFF01 && c <= 0xFF0F) return false;
    if (c >= 0xFF1A && c <= 0xFF20) return false;
    if (c >= 0xFF3B && c <= 0xFF40) return false;
    if (c >= 0xFF5B && c <= 0xFF65) return false;
    return true;
}

static bool is_word_char_cp(uint32_t c) { return rt_is_word_cp(c); }

static bool is_email_local_char(uint32_t c) {
    if (c < 0x80) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               strchr("_.!#$%&'*/=?^`{|}~+-", (int)c) != NULL;
    }
    return false;
}

/* ---- TagIndex ------------------------------------------------------------ */

typedef struct {
    rt_pos_vec lts, gts;
    bool has_dangling_newline;
    size_t dangling_newline;
    rt_pos_vec open_anchors;  /* (start, end) pairs flattened */
    rt_pos_vec close_anchors; /* positions of the '<' of </a> */
} rt_tag_index;

static void tag_index_init(rt_tag_index *index) {
    pos_init(&index->lts);
    pos_init(&index->gts);
    index->has_dangling_newline = false;
    index->dangling_newline = 0;
    pos_init(&index->open_anchors);
    pos_init(&index->close_anchors);
}

static void tag_index_dispose(rt_tag_index *index) {
    pos_dispose(&index->lts);
    pos_dispose(&index->gts);
    pos_dispose(&index->open_anchors);
    pos_dispose(&index->close_anchors);
}

/* `/<a\b.*?>/i` anchored at a '<', `.` not crossing a newline; returns the end
 * (one past '>') or 0. */
static size_t open_anchor_end(const unsigned char *text, size_t len, size_t at) {
    if (at + 2 > len) return 0;
    if (text[at] != '<' || (text[at + 1] != 'a' && text[at + 1] != 'A')) return 0;
    size_t next = at + 2;
    if (next == len) return 0;
    uint32_t code;
    size_t width = rt_utf8_decode(text + next, len - next, &code);
    if (width == 0) width = 1;
    if (is_word_char_cp(code)) return 0; /* missing \b */
    size_t j = next;
    while (j < len && text[j] != '\n' && text[j] != '>') j++;
    if (j < len && text[j] == '>') return j + 1;
    return 0;
}

static void tag_index_build(rt_tag_index *index, const unsigned char *text, size_t len) {
    bool unclosed_lt = false;
    size_t unclosed_lt_pos = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char b = text[i];
        if (b == '<') {
            if (!pos_push(&index->lts, i)) return;
            if (!unclosed_lt) {
                unclosed_lt = true;
                unclosed_lt_pos = i;
            }
            if (i + 4 <= len && text[i + 1] == '/' && (text[i + 2] == 'a' || text[i + 2] == 'A') &&
                text[i + 3] == '>') {
                if (!pos_push(&index->close_anchors, i)) return;
            }
            bool after_previous =
                index->open_anchors.len == 0 || index->open_anchors.items[index->open_anchors.len - 1] <= i;
            if (after_previous) {
                size_t end = open_anchor_end(text, len, i);
                if (end != 0) {
                    if (!pos_push(&index->open_anchors, i) || !pos_push(&index->open_anchors, end)) return;
                }
            }
        } else if (b == '>') {
            if (!pos_push(&index->gts, i)) return;
            unclosed_lt = false;
        } else if (b == '\n' && !index->has_dangling_newline && unclosed_lt &&
                   unclosed_lt_pos + 2 <= i) {
            index->has_dangling_newline = true;
            index->dangling_newline = i;
        }
    }
}

/* auto_linked?(left, right) for a match text[start..end]. */
static bool tag_index_auto_linked(const rt_tag_index *index, size_t start, size_t end) {
    bool open_at_line_end;
    if (index->has_dangling_newline && index->dangling_newline < start) {
        open_at_line_end = true;
    } else {
        size_t last_gt_index = pos_partition_lt(&index->gts, start);
        bool has_last_gt = last_gt_index != 0;
        size_t last_gt = has_last_gt ? index->gts.items[last_gt_index - 1] : 0;
        size_t lt_index = has_last_gt ? pos_partition_le(&index->lts, last_gt) : 0;
        open_at_line_end =
            lt_index < index->lts.len && index->lts.items[lt_index] + 2 <= start;
    }
    bool closes_tag = index->gts.len != 0 && index->gts.items[index->gts.len - 1] >= end;
    if (open_at_line_end && closes_tag) return true;
    /* inside_anchor: the last <a ...> wholly in left isn't closed before
     * left ends. open_anchors stores (start, end) pairs. */
    size_t count = index->open_anchors.len / 2;
    size_t pair = 0;
    while (pair < count && index->open_anchors.items[pair * 2 + 1] <= start) pair++;
    if (pair == 0) return false;
    size_t anchor_end = index->open_anchors.items[(pair - 1) * 2 + 1];
    size_t close_index = pos_partition_lt(&index->close_anchors, anchor_end);
    if (close_index < index->close_anchors.len &&
        index->close_anchors.items[close_index] + 4 <= start) {
        return false;
    }
    return true;
}

/* ---- AUTO_LINK_RE -------------------------------------------------------- */

static const char *const AUTO_LINK_SCHEMES[] = {
    "ed2k", "ftp", "http", "https", "irc", "mailto", "news", "gopher", "nntp", "telnet", "webcal",
    "xmpp", "callto", "feed", "svn", "urn", "aim", "rsync", "tag", "ssh", "sftp", "rtsp", "afs",
    "file",
};

static bool tail_char_allowed(uint32_t code) {
    return code != ' ' && code != '\t' && code != '\r' && code != '\n' && code != 0x0B &&
           code != 0x0C && code != '<' && code != 0xA0 && code != '"';
}

/* Matches the regex at `at`; returns the end and whether group 1 (a scheme)
 * participated. */
static bool match_auto_link(const unsigned char *text, size_t len, size_t at, size_t *out_end,
                            bool *out_has_scheme) {
    size_t start = at;
    bool has_scheme = false;
    size_t pos = at;
    size_t scheme_len = 0;
    for (size_t s = 0; s < sizeof AUTO_LINK_SCHEMES / sizeof AUTO_LINK_SCHEMES[0]; s++) {
        const char *scheme = AUTO_LINK_SCHEMES[s];
        size_t n = strlen(scheme);
        if (pos + n + 3 <= len && rt_ascii_ieq(text + pos, n, scheme) && text[pos + n] == ':' &&
            text[pos + n + 1] == '/' && text[pos + n + 2] == '/') {
            has_scheme = true;
            scheme_len = n + 1;
            break;
        }
    }
    if (has_scheme) {
        pos = start + scheme_len + 2;
    } else {
        if (!(pos + 5 <= len && rt_ascii_istarts(text + pos, 4, "www."))) return false;
        unsigned char next = text[pos + 4];
        bool label = (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
                     (next >= '0' && next <= '9') || next == '_';
        if (!label) return false;
        pos = start + 5;
    }
    size_t tail_start = pos;
    while (pos < len) {
        uint32_t code;
        size_t width = rt_utf8_decode(text + pos, len - pos, &code);
        if (width == 0) width = 1;
        if (!tail_char_allowed(code)) break;
        pos += width;
    }
    if (pos == tail_start) return false;
    *out_end = pos;
    *out_has_scheme = has_scheme;
    return true;
}

/* AUTO_EMAIL_RE, anchored at `at`; returns the end or 0. */
static size_t match_auto_email(const unsigned char *text, size_t len, size_t at) {
    if (at >= len) return 0;
    unsigned char c = text[at];
    bool first = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 strchr("_.!#$%+-", (int)c) != NULL;
    if (!first) return 0;
    size_t pos = at + 1;
    if (pos < len && text[pos] == '.') pos++;
    while (pos < len && is_email_local_char(text[pos])) pos++;
    if (pos >= len || text[pos] != '@') return 0;
    pos++;
    size_t label_start = pos;
    while (pos < len && ((text[pos] >= 'a' && text[pos] <= 'z') ||
                         (text[pos] >= 'A' && text[pos] <= 'Z') ||
                         (text[pos] >= '0' && text[pos] <= '9') || text[pos] == '_' || text[pos] == '-')) {
        pos++;
    }
    if (pos == label_start) return 0;
    /* (?:\.[a-zA-Z0-9_-]+)+ : at least one dotted label is required. */
    size_t end = 0;
    while (pos < len && text[pos] == '.') {
        size_t after_dot = pos + 1;
        size_t label = after_dot;
        while (label < len && ((text[label] >= 'a' && text[label] <= 'z') ||
                               (text[label] >= 'A' && text[label] <= 'Z') ||
                               (text[label] >= '0' && text[label] <= '9') || text[label] == '_' ||
                               text[label] == '-')) {
            label++;
        }
        if (label == after_dot) break;
        end = label;
        pos = label;
    }
    return end;
}

/* ---- bracket counts ------------------------------------------------------ */

static const char BRACKETS[6] = {'[', ']', '(', ')', '{', '}'};

static int bracket_index(char c) {
    for (int i = 0; i < 6; i++) {
        if (BRACKETS[i] == c) return i;
    }
    return -1;
}

typedef struct {
    size_t counts[6];
} bracket_counts;

static void bracket_counts_of(bracket_counts *counts, const unsigned char *text, size_t len) {
    memset(counts, 0, sizeof *counts);
    for (size_t i = 0; i < len;) {
        uint32_t code;
        size_t width = rt_utf8_decode(text + i, len - i, &code);
        if (width == 0) width = 1;
        if (code < 0x80) {
            int index = bracket_index((char)code);
            if (index >= 0) counts->counts[index]++;
        }
        i += width;
    }
}

static char opening_bracket(char closing) {
    switch (closing) {
    case ']': return '[';
    case ')': return '(';
    case '}': return '{';
    default: return 0;
    }
}

/* ---- auto_link ----------------------------------------------------------- */

static rt_status sanitize_defaults(const unsigned char *bytes, size_t len, rt_buf *out) {
    return rt_sanitize(bytes, len, rt_safe_list_defaults(), out);
}

static rt_status auto_link_urls(const unsigned char *text, size_t len, rt_buf *out) {
    rt_tag_index index;
    tag_index_init(&index);
    tag_index_build(&index, text, len);
    size_t last = 0;
    size_t at = 0;
    rt_status rc = RT_OK;
    while (at < len && rc == RT_OK) {
        size_t end = 0;
        bool has_scheme = false;
        if (!match_auto_link(text, len, at, &end, &has_scheme)) {
            at++;
            continue;
        }
        if (rt_buf_append(out, text + last, at - last) != RT_OK) {
            rc = RT_NOMEM;
            break;
        }
        last = end;
        /* href starts as the whole match; strip trailing punctuation. */
        rt_buf href;
        rt_buf_init(&href);
        if (rt_buf_append(&href, text + at, end - at) != RT_OK) {
            rt_buf_dispose(&href);
            rc = RT_NOMEM;
            break;
        }
        if (tag_index_auto_linked(&index, at, end)) {
            rc = rt_buf_append(out, href.data, href.len);
            rt_buf_dispose(&href);
            at = end;
            continue;
        }
        /* Trailing punctuation, in chars. */
        rt_buf punctuation;
        rt_buf_init(&punctuation);
        bracket_counts brackets;
        bracket_counts_of(&brackets, href.data, href.len);
        while (href.len != 0) {
            /* Last char. */
            size_t prev = href.len - 1;
            while (prev > 0 && (href.data[prev] & 0xC0) == 0x80) prev--;
            uint32_t code;
            size_t width = rt_utf8_decode(href.data + prev, href.len - prev, &code);
            if (width == 0) width = 1;
            bool keep = is_word_char_cp(code) || (code < 0x80 && (code == '/' || code == '-' ||
                                                                  code == '=' || code == ';'));
            if (keep) break;
            /* Drop one char. */
            if (rt_buf_append(&punctuation, href.data + prev, href.len - prev) != RT_OK) {
                rc = RT_NOMEM;
                break;
            }
            href.len = prev;
            if (code < 0x80) {
                int bi = bracket_index((char)code);
                if (bi >= 0 && brackets.counts[bi] > 0) brackets.counts[bi]--;
            }
            if (code < 0x80) {
                char opening = opening_bracket((char)code);
                int oi = opening != 0 ? bracket_index(opening) : -1;
                int ci = bracket_index((char)code);
                if (oi >= 0 && ci >= 0 && brackets.counts[oi] > brackets.counts[ci]) {
                    /* Put the closing bracket back. */
                    size_t plen = 1; /* ASCII */
                    href.len += plen;
                    punctuation.len -= plen;
                    break;
                }
            }
        }
        if (rc == RT_OK) {
            /* Trailing &gt; moves outside the link. */
            bool trailing_gt = false;
            if (href.len >= 4 && memcmp(href.data + href.len - 4, "&gt;", 4) == 0) {
                href.len -= 4;
                trailing_gt = true;
            }
            rt_buf link_text;
            rt_buf_init(&link_text);
            rc = rt_buf_append(&link_text, href.data, href.len);
            if (rc == RT_OK) {
                rt_buf href_sanitized;
                rt_buf_init(&href_sanitized);
                if (!has_scheme) {
                    rt_status prefix = rt_buf_puts(&href_sanitized, "http://");
                    if (prefix == RT_OK) prefix = rt_buf_append(&href_sanitized, href.data, href.len);
                    rc = prefix;
                } else {
                    rc = rt_buf_append(&href_sanitized, href.data, href.len);
                }
                rt_buf href_clean;
                rt_buf_init(&href_clean);
                rt_buf text_clean;
                rt_buf_init(&text_clean);
                if (rc == RT_OK) {
                    rc = sanitize_defaults(href_sanitized.data, href_sanitized.len, &href_clean);
                }
                if (rc == RT_OK) {
                    rc = sanitize_defaults(link_text.data, link_text.len, &text_clean);
                }
                if (rc == RT_OK) {
                    rc = rt_buf_puts(out, "<a target=\"_blank\" href=\"");
                }
                if (rc == RT_OK) {
                    for (size_t i = 0; i < href_clean.len; i++) {
                        if (href_clean.data[i] == '"') {
                            rc = rt_buf_puts(out, "&quot;");
                        } else {
                            rc = rt_buf_putc(out, href_clean.data[i]);
                        }
                        if (rc != RT_OK) break;
                    }
                }
                if (rc == RT_OK) rc = rt_buf_puts(out, "\">");
                if (rc == RT_OK) rc = rt_buf_append(out, text_clean.data, text_clean.len);
                if (rc == RT_OK) rc = rt_buf_puts(out, "</a>");
                if (rc == RT_OK) {
                    /* SafeBuffer#+ escapes the (unsafe) punctuation string;
                     * punctuation holds chars from last to first. */
                    for (size_t i = punctuation.len; i > 0;) {
                        size_t prev = i - 1;
                        while (prev > 0 && (punctuation.data[prev] & 0xC0) == 0x80) prev--;
                        rc = rt_html_escape(punctuation.data + prev, i - prev, out);
                        if (rc != RT_OK) break;
                        i = prev;
                    }
                }
                if (rc == RT_OK && trailing_gt) rc = rt_buf_puts(out, "&gt;");
                rt_buf_dispose(&text_clean);
                rt_buf_dispose(&href_clean);
                rt_buf_dispose(&href_sanitized);
            }
            rt_buf_dispose(&link_text);
        }
        rt_buf_dispose(&punctuation);
        rt_buf_dispose(&href);
        if (rc != RT_OK) break;
        at = end;
    }
    if (rc == RT_OK) rc = rt_buf_append(out, text + last, len - last);
    tag_index_dispose(&index);
    return rc;
}

static rt_status auto_link_email_addresses(const unsigned char *text, size_t len, rt_buf *out) {
    rt_tag_index index;
    tag_index_init(&index);
    tag_index_build(&index, text, len);
    rt_status rc = RT_OK;
    size_t copied = 0;
    size_t position = 0;
    while (position < len && rc == RT_OK) {
        uint32_t prev_code = 0;
        if (position > 0) {
            size_t prev = position - 1;
            while (prev > 0 && (text[prev] & 0xC0) == 0x80) prev--;
            rt_utf8_decode(text + prev, position - prev, &prev_code);
        }
        bool preceded = position > 0 && is_email_local_char(prev_code);
        size_t end = preceded ? 0 : match_auto_email(text, len, position);
        if (end == 0) {
            uint32_t code;
            size_t width = rt_utf8_decode(text + position, len - position, &code);
            if (width == 0) width = 1;
            position += width;
            continue;
        }
        size_t start = position;
        if (rt_buf_append(out, text + copied, start - copied) != RT_OK) {
            rc = RT_NOMEM;
            break;
        }
        if (tag_index_auto_linked(&index, start, end)) {
            rc = rt_buf_append(out, text + start, end - start);
        } else {
            rt_buf sanitized;
            rt_buf_init(&sanitized);
            rc = sanitize_defaults(text + start, end - start, &sanitized);
            if (rc == RT_OK) {
                rt_buf display;
                rt_buf_init(&display);
                if (sanitized.len == end - start &&
                    memcmp(sanitized.data, text + start, sanitized.len) == 0) {
                    rc = rt_html_escape(text + start, end - start, &display);
                } else {
                    rc = rt_buf_append(&display, sanitized.data, sanitized.len);
                }
                rt_buf encoded;
                rt_buf_init(&encoded);
                if (rc == RT_OK) rc = rt_url_encode(sanitized.data, sanitized.len, &encoded);
                if (rc == RT_OK) {
                    /* The url_encode of a sanitized address never contains a
                     * literal '@'; replace the escape if one appears. */
                    rt_buf href;
                    rt_buf_init(&href);
                    rc = rt_buf_puts(&href, "mailto:");
                    for (size_t i = 0; i < encoded.len && rc == RT_OK; i++) {
                        if (i + 3 <= encoded.len && memcmp(encoded.data + i, "%40", 3) == 0) {
                            rc = rt_buf_putc(&href, '@');
                            i += 2;
                        } else {
                            rc = rt_buf_putc(&href, encoded.data[i]);
                        }
                    }
                    if (rc == RT_OK) rc = rt_buf_puts(out, "<a target=\"_blank\" href=\"");
                    if (rc == RT_OK) rc = rt_html_escape(href.data, href.len, out);
                    if (rc == RT_OK) rc = rt_buf_puts(out, "\">");
                    if (rc == RT_OK) rc = rt_buf_append(out, display.data, display.len);
                    if (rc == RT_OK) rc = rt_buf_puts(out, "</a>");
                    rt_buf_dispose(&href);
                }
                rt_buf_dispose(&encoded);
                rt_buf_dispose(&display);
            }
            rt_buf_dispose(&sanitized);
        }
        copied = end;
        position = end > position + 1 ? end : position + 1;
    }
    if (rc == RT_OK) rc = rt_buf_append(out, text + copied, len - copied);
    tag_index_dispose(&index);
    return rc;
}

rt_status rt_auto_link(const unsigned char *html, size_t len, const rt_safe_list *list, rt_buf *out) {
    if (rt_is_blank(html, len)) return RT_OK;
    rt_buf sanitized;
    rt_buf_init(&sanitized);
    rt_status rc = rt_sanitize_escaped_attributes(html, len, list, &sanitized);
    if (rc == RT_OK) {
        rt_buf linked;
        rt_buf_init(&linked);
        rc = auto_link_urls(sanitized.data, sanitized.len, &linked);
        if (rc == RT_OK) rc = auto_link_email_addresses(linked.data, linked.len, out);
        rt_buf_dispose(&linked);
    }
    rt_buf_dispose(&sanitized);
    return rc;
}
