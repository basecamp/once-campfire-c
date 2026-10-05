/* src/richtext/rt_sanitize.c — Rails::HTML5::SafeListSanitizer with a
 * Rails::HTML::PermitScrubber over Loofah's scrubbing helpers.
 *
 * Ports tmp/rust-ref/crates/richtext/src/sanitizer.rs exactly: the allowlist
 * walker (unwrap elements, drop foreign elements with their contents), URL
 * protocol checks, the per-attribute URL re-escaping order, the style
 * scrubber and the SafeList tables.
 */
#include "richtext/internal.h"

#include <stdlib.h>
#include <string.h>

/* --- SafeList tables (sanitizer.rs) -------------------------------------- */

static const char *const DEFAULT_ALLOWED_TAGS[] = {
    "a", "abbr", "acronym", "address", "b", "big", "blockquote", "br", "cite", "code", "dd", "del",
    "dfn", "div", "dl", "dt", "em", "h1", "h2", "h3", "h4", "h5", "h6", "hr", "i", "img", "ins",
    "kbd", "li", "mark", "ol", "p", "pre", "samp", "small", "span", "strong", "sub", "sup", "time",
    "tt", "ul", "var",
};

/* DEFAULT_ALLOWED_ATTRIBUTES without `name` (DOM clobbering). */
static const char *const DEFAULT_ALLOWED_ATTRIBUTES[] = {
    "abbr", "alt", "cite", "class", "datetime", "height", "href", "lang", "src", "title", "width",
    "xml:lang",
};

/* ActionText::Attachment::ATTRIBUTES, in order. */
const char *const rt_attachment_attributes[] = {
    "sgid", "content-type", "url", "href", "filename", "filesize", "width", "height",
    "previewable", "presentation", "caption", "content",
};
const size_t rt_attachment_attributes_len =
    sizeof rt_attachment_attributes / sizeof rt_attachment_attributes[0];

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

static const rt_safe_list DEFAULT_LIST = {DEFAULT_ALLOWED_TAGS, COUNT_OF(DEFAULT_ALLOWED_TAGS),
                                          DEFAULT_ALLOWED_ATTRIBUTES,
                                          COUNT_OF(DEFAULT_ALLOWED_ATTRIBUTES)};

/* action_text(): defaults + Lexxy's additions + Campfire's. */
static const char *const ACTION_TEXT_TAGS[] = {
    "a", "abbr", "acronym", "address", "b", "big", "blockquote", "br", "cite", "code", "dd", "del",
    "dfn", "div", "dl", "dt", "em", "h1", "h2", "h3", "h4", "h5", "h6", "hr", "i", "img", "ins",
    "kbd", "li", "mark", "ol", "p", "pre", "samp", "small", "span", "strong", "sub", "sup", "time",
    "tt", "ul", "var", "action-text-attachment", "figure", "figcaption", "video", "audio", "source",
    "embed", "table", "tbody", "tr", "th", "td", "s", "u", "thead", "tfoot",
};

static const char *const ACTION_TEXT_ATTRIBUTES[] = {
    "abbr", "alt", "cite", "class", "datetime", "height", "href", "lang", "src", "title", "width",
    "xml:lang", "sgid", "content-type", "url", "href", "filename", "filesize", "width", "height",
    "previewable", "presentation", "caption", "content", "controls", "poster", "data-language",
    "style", "value", "start",
};

static const rt_safe_list ACTION_TEXT_LIST = {ACTION_TEXT_TAGS, COUNT_OF(ACTION_TEXT_TAGS),
                                              ACTION_TEXT_ATTRIBUTES,
                                              COUNT_OF(ACTION_TEXT_ATTRIBUTES)};

/* ContentFilters::SanitizeTags::ALLOWED_TAGS. */
static const char *const SANITIZE_TAGS_TAGS[] = {
    "a", "abbr", "acronym", "address", "b", "big", "blockquote", "br", "cite", "code", "dd", "del",
    "dfn", "div", "dl", "dt", "em", "h1", "h2", "h3", "h4", "h5", "h6", "hr", "i", "ins", "kbd",
    "li", "ol", "p", "pre", "samp", "small", "span", "strong", "sub", "sup", "time", "tt", "ul",
    "var", "s", "u", "mark", "table", "thead", "tbody", "tfoot", "tr", "th", "td",
    "action-text-attachment", "figure", "figcaption",
};

/* content_filter(): SanitizeTags' tags, action_text's attributes plus class. */
static const char *const CONTENT_FILTER_ATTRIBUTES[] = {
    "abbr", "alt", "cite", "class", "datetime", "height", "href", "lang", "src", "title", "width",
    "xml:lang", "sgid", "content-type", "url", "href", "filename", "filesize", "width", "height",
    "previewable", "presentation", "caption", "content", "controls", "poster", "data-language",
    "style", "value", "start", "class",
};

static const rt_safe_list CONTENT_FILTER_LIST = {SANITIZE_TAGS_TAGS, COUNT_OF(SANITIZE_TAGS_TAGS),
                                                 CONTENT_FILTER_ATTRIBUTES,
                                                 COUNT_OF(CONTENT_FILTER_ATTRIBUTES)};

/* auto_link(): defaults + editor formatting tags/attributes. */
static const char *const AUTO_LINK_TAGS[] = {
    "a", "abbr", "acronym", "address", "b", "big", "blockquote", "br", "cite", "code", "dd", "del",
    "dfn", "div", "dl", "dt", "em", "h1", "h2", "h3", "h4", "h5", "h6", "hr", "i", "img", "ins",
    "kbd", "li", "mark", "ol", "p", "pre", "samp", "small", "span", "strong", "sub", "sup", "time",
    "tt", "ul", "var", "s", "u", "table", "thead", "tbody", "tfoot", "tr", "th", "td",
};

static const char *const AUTO_LINK_ATTRIBUTES[] = {
    "abbr", "alt", "cite", "class", "datetime", "height", "href", "lang", "src", "title", "width",
    "xml:lang", "data-language",
};

static const rt_safe_list AUTO_LINK_LIST = {AUTO_LINK_TAGS, COUNT_OF(AUTO_LINK_TAGS),
                                            AUTO_LINK_ATTRIBUTES, COUNT_OF(AUTO_LINK_ATTRIBUTES)};

const rt_safe_list *rt_safe_list_defaults(void) { return &DEFAULT_LIST; }
const rt_safe_list *rt_safe_list_action_text(void) { return &ACTION_TEXT_LIST; }
const rt_safe_list *rt_safe_list_content_filter(void) { return &CONTENT_FILTER_LIST; }
const rt_safe_list *rt_safe_list_auto_link(void) { return &AUTO_LINK_LIST; }

static bool list_contains(const char *const *items, size_t len, const char *name) {
    for (size_t i = 0; i < len; i++) {
        if (strcmp(items[i], name) == 0) return true;
    }
    return false;
}

bool rt_safe_list_allows_tag(const rt_safe_list *list, const char *name) {
    return list_contains(list->tags, list->tags_len, name);
}

bool rt_safe_list_allows_attr(const rt_safe_list *list, const char *name) {
    return list_contains(list->attributes, list->attributes_len, name);
}

bool rt_sanitize_tags_allows(const char *name) {
    return list_contains(SANITIZE_TAGS_TAGS, COUNT_OF(SANITIZE_TAGS_TAGS), name);
}

/* --- URI checks ----------------------------------------------------------- */

const char *const RT_ATTR_VAL_IS_URI[] = {"action", "cite",       "href", "longdesc",
                                          "poster", "preload",    "src",  "xlink:href",
                                          "xml:base"};
#define ATTR_VAL_IS_URI_COUNT (sizeof RT_ATTR_VAL_IS_URI / sizeof RT_ATTR_VAL_IS_URI[0])

static const char *const ALLOWED_PROTOCOLS[] = {
    "afs", "aim", "callto", "data", "ed2k", "fax", "ftp", "gopher", "http", "https", "irc", "line",
    "mailto", "modem", "news", "nntp", "rsync", "rtsp", "sftp", "sms", "ssh", "tag", "tel", "telnet",
    "urn", "webcal", "xmpp",
};

static const char *const ALLOWED_URI_DATA_MEDIATYPES[] = {"image/gif", "image/jpeg", "image/png",
                                                          "text/css", "text/plain"};

static bool is_control_character(uint32_t c) {
    return c == '`' || c <= 0x20 || c == 0x7F || (c >= 0x80 && c <= 0x101);
}

/* CGI.unescapeHTML. */
rt_status rt_cgi_unescape_html(const unsigned char *bytes, size_t len, rt_buf *out) {
    size_t i = 0;
    while (i < len) {
        if (bytes[i] != '&') {
            if (rt_buf_putc(out, bytes[i]) != RT_OK) return RT_NOMEM;
            i++;
            continue;
        }
        /* The five named entities, in sanitizer.rs order. */
        static const struct {
            const char *name;
            const char *value;
        } named[] = {{"&apos;", "'"}, {"&amp;", "&"}, {"&quot;", "\""}, {"&gt;", ">"}, {"&lt;", "<"}};
        bool matched = false;
        for (size_t k = 0; k < COUNT_OF(named); k++) {
            size_t n = strlen(named[k].name);
            if (i + n <= len && memcmp(bytes + i, named[k].name, n) == 0) {
                if (rt_buf_puts(out, named[k].value) != RT_OK) return RT_NOMEM;
                i += n;
                matched = true;
                break;
            }
        }
        if (matched) continue;
        size_t semi = len;
        for (size_t j = i + 1; j < len; j++) {
            if (bytes[j] == ';') {
                semi = j;
                break;
            }
        }
        uint32_t code = 0;
        bool decoded = false;
        if (semi < len) {
            size_t body_start = i + 1;
            bool hex = false;
            if (body_start + 1 < semi && bytes[body_start] == '#' &&
                (bytes[body_start + 1] == 'x' || bytes[body_start + 1] == 'X')) {
                hex = true;
                body_start += 2;
            } else if (body_start < semi && bytes[body_start] == '#') {
                body_start += 1;
            } else {
                body_start = semi; /* not numeric */
            }
            if (body_start < semi) {
                const unsigned char *digits = bytes + body_start;
                size_t digits_len = semi - body_start;
                bool all_digits = true;
                for (size_t j = 0; j < digits_len; j++) {
                    if (hex ? !((digits[j] >= '0' && digits[j] <= '9') ||
                                (digits[j] >= 'a' && digits[j] <= 'f') ||
                                (digits[j] >= 'A' && digits[j] <= 'F'))
                            : !(digits[j] >= '0' && digits[j] <= '9')) {
                        all_digits = false;
                        break;
                    }
                }
                size_t significant = 0;
                while (significant < digits_len && digits[significant] == '0') significant++;
                size_t significant_len = digits_len - significant;
                size_t limit = hex ? 6 : 7;
                if (all_digits && digits_len != 0 && significant_len <= limit) {
                    uint32_t value = 0;
                    for (size_t j = 0; j < digits_len; j++) {
                        unsigned char c = digits[j];
                        unsigned digit;
                        if (c >= '0' && c <= '9') digit = c - '0';
                        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                        else digit = c - 'A' + 10;
                        value = value * (hex ? 16u : 10u) + digit;
                    }
                    if (value < 0x10FFFF && !(value >= 0xD800 && value <= 0xDFFF)) {
                        code = value;
                        decoded = true;
                    }
                }
            }
        }
        if (decoded) {
            if (rt_buf_put_utf8(out, code) != RT_OK) return RT_NOMEM;
            i = semi + 1;
        } else {
            if (rt_buf_putc(out, '&') != RT_OK) return RT_NOMEM;
            i++;
        }
    }
    return RT_OK;
}

/* Loofah's decode_numeric_character_references. */
static rt_status decode_numeric_character_references(const unsigned char *bytes, size_t len, rt_buf *out) {
    size_t i = 0;
    size_t copied = 0;
    while (i < len) {
        if (bytes[i] == '&' && i + 1 < len && bytes[i + 1] == '#') {
            size_t start = i + 2;
            bool hex = start < len && (bytes[start] == 'x' || bytes[start] == 'X');
            size_t digits_start = hex ? start + 1 : start;
            size_t end = digits_start;
            while (end < len &&
                   (hex ? ((bytes[end] >= '0' && bytes[end] <= '9') ||
                           (bytes[end] >= 'a' && bytes[end] <= 'f') ||
                           (bytes[end] >= 'A' && bytes[end] <= 'F'))
                        : (bytes[end] >= '0' && bytes[end] <= '9'))) {
                end++;
            }
            if (end > digits_start) {
                size_t digits_len = end - digits_start;
                size_t full_end = end < len && bytes[end] == ';' ? end + 1 : end;
                size_t significant = 0;
                while (significant < digits_len && bytes[digits_start + significant] == '0') significant++;
                size_t significant_len = digits_len - significant;
                size_t limit = hex ? 6 : 7;
                bool decoded = false;
                uint32_t code = 0;
                if (significant_len <= limit) {
                    for (size_t j = 0; j < digits_len; j++) {
                        unsigned char c = bytes[digits_start + j];
                        unsigned digit;
                        if (c >= '0' && c <= '9') digit = c - '0';
                        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                        else digit = c - 'A' + 10;
                        code = code * (hex ? 16u : 10u) + digit;
                    }
                    if (code < 0x10FFFF && !(code >= 0xD800 && code <= 0xDFFF)) decoded = true;
                }
                if (rt_buf_append(out, bytes + copied, i - copied) != RT_OK) return RT_NOMEM;
                if (decoded) {
                    if (rt_buf_put_utf8(out, code) != RT_OK) return RT_NOMEM;
                } else {
                    if (rt_buf_append(out, bytes + i, full_end - i) != RT_OK) return RT_NOMEM;
                }
                i = full_end;
                copied = i;
                continue;
            }
        }
        i++;
    }
    return rt_buf_append(out, bytes + copied, len - copied);
}

static rt_status ascii_lower(const rt_buf *in, rt_buf *out) {
    for (size_t i = 0; i < in->len; i++) {
        unsigned char c = in->data[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if (rt_buf_putc(out, c) != RT_OK) return RT_NOMEM;
    }
    return RT_OK;
}

static bool starts_with(const rt_buf *buf, size_t at, const char *text) {
    size_t n = strlen(text);
    return at + n <= buf->len && memcmp(buf->data + at, text, n) == 0;
}

/* sanitizer.rs separator_len over the lowercased URI. */
static bool separator_at(const rt_buf *s, size_t at) {
    if (at < s->len && s->data[at] == ':') return true;
    if (starts_with(s, at, "&#x")) {
        size_t rest = at + 3;
        size_t zeros = 0;
        while (rest + zeros < s->len && s->data[rest + zeros] == '0') zeros++;
        if (starts_with(s, rest + zeros, "3a")) return true;
    }
    if (starts_with(s, at, "&#")) {
        size_t rest = at + 2;
        size_t zeros = 0;
        while (rest + zeros < s->len && s->data[rest + zeros] == '0') zeros++;
        if (starts_with(s, rest + zeros, "58")) return true;
    }
    if (starts_with(s, at, "%3a")) return true;
    if (starts_with(s, at, "&#37;3a")) return true;
    return false;
}

static rt_status data_uri_mediatype(const rt_buf *s, rt_buf *out) {
    size_t at = 0;
    if (starts_with(s, 0, "data:")) at = 5;
    size_t comma = s->len;
    for (size_t i = at; i < s->len; i++) {
        if (s->data[i] == ',') {
            comma = i;
            break;
        }
    }
    if (comma >= s->len) return RT_RAISED; /* no comma: None */
    size_t meta_end = comma;
    if (meta_end >= at + 7 && memcmp(s->data + meta_end - 7, ";base64", 7) == 0) meta_end -= 7;
    size_t first_semi = meta_end;
    for (size_t i = at; i < meta_end; i++) {
        if (s->data[i] == ';') {
            first_semi = i;
            break;
        }
    }
    size_t start = at;
    size_t end = first_semi;
    while (start < end && (s->data[start] == ' ' || s->data[start] == '\t' || s->data[start] == '\n' ||
                           s->data[start] == 0x0B || s->data[start] == 0x0C || s->data[start] == '\r' ||
                           s->data[start] == '\0')) {
        start++;
    }
    while (end > start && (s->data[end - 1] == ' ' || s->data[end - 1] == '\t' ||
                           s->data[end - 1] == '\n' || s->data[end - 1] == 0x0B ||
                           s->data[end - 1] == 0x0C || s->data[end - 1] == '\r' ||
                           s->data[end - 1] == '\0')) {
        end--;
    }
    size_t slash = end;
    for (size_t i = start; i < end; i++) {
        if (s->data[i] == '/') {
            slash = i;
            break;
        }
    }
    bool valid = slash > start && slash + 1 < end;
    for (size_t i = start; valid && i < end; i++) {
        if (i == slash) continue;
        unsigned char c = s->data[i];
        bool tchar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     strchr("!#$%&'*+-.^_`|~", c) != NULL;
        if (!tchar) valid = false;
    }
    if (valid) return rt_buf_append(out, s->data + start, end - start);
    return rt_buf_puts(out, "text/plain");
}

bool rt_allowed_uri(const unsigned char *bytes, size_t len) {
    rt_buf without;
    rt_buf_init(&without);
    size_t i = 0;
    while (i < len) {
        uint32_t code;
        size_t width = rt_utf8_decode(bytes + i, len - i, &code);
        if (width == 0) width = 1;
        if (!is_control_character(code)) {
            if (rt_buf_append(&without, bytes + i, width) != RT_OK) {
                rt_buf_dispose(&without);
                return false;
            }
        }
        i += width;
    }
    rt_buf unescaped;
    rt_buf_init(&unescaped);
    if (rt_cgi_unescape_html(without.data, without.len, &unescaped) != RT_OK) {
        rt_buf_dispose(&without);
        rt_buf_dispose(&unescaped);
        return false;
    }
    rt_buf disposed;
    rt_buf_init(&disposed);
    if (decode_numeric_character_references(unescaped.data, unescaped.len, &disposed) != RT_OK) {
        rt_buf_dispose(&without);
        rt_buf_dispose(&unescaped);
        rt_buf_dispose(&disposed);
        return false;
    }
    rt_buf filtered;
    rt_buf_init(&filtered);
    i = 0;
    while (i < disposed.len) {
        uint32_t code;
        size_t width = rt_utf8_decode(disposed.data + i, disposed.len - i, &code);
        if (width == 0) width = 1;
        if (!is_control_character(code)) {
            if (rt_buf_append(&filtered, disposed.data + i, width) != RT_OK) {
                rt_buf_dispose(&without);
                rt_buf_dispose(&unescaped);
                rt_buf_dispose(&disposed);
                rt_buf_dispose(&filtered);
                return false;
            }
        }
        i += width;
    }
    rt_buf replaced;
    rt_buf_init(&replaced);
    for (i = 0; i < filtered.len;) {
        if (starts_with(&filtered, i, "&Tab;")) {
            i += 5;
        } else if (starts_with(&filtered, i, "&NewLine;")) {
            i += 9;
        } else if (starts_with(&filtered, i, "&colon;")) {
            i += 7;
            if (rt_buf_putc(&replaced, ':') != RT_OK) break;
        } else {
            if (rt_buf_putc(&replaced, filtered.data[i]) != RT_OK) break;
            i++;
        }
    }
    rt_buf lower;
    rt_buf_init(&lower);
    rt_status rc = ascii_lower(&replaced, &lower);
    bool allowed = true;
    if (rc == RT_OK) {
        /* Protocol is the longest ASCII scheme run followed by a separator. */
        const rt_buf *s = &lower;
        if (s->len != 0 && s->data[0] >= 'a' && s->data[0] <= 'z') {
            size_t end = 1;
            while (end < s->len && ((s->data[end] >= 'a' && s->data[end] <= 'z') ||
                                    (s->data[end] >= '0' && s->data[end] <= '9') ||
                                    s->data[end] == '+' || s->data[end] == '-' || s->data[end] == '.')) {
                end++;
            }
            if (separator_at(s, end)) {
                char protocol[64];
                size_t n = end < sizeof protocol - 1 ? end : sizeof protocol - 1;
                memcpy(protocol, s->data, n);
                protocol[n] = '\0';
                bool known = false;
                for (size_t k = 0; k < COUNT_OF(ALLOWED_PROTOCOLS); k++) {
                    if (strcmp(protocol, ALLOWED_PROTOCOLS[k]) == 0) {
                        known = true;
                        break;
                    }
                }
                if (!known) {
                    allowed = false;
                } else if (strcmp(protocol, "data") == 0) {
                    rt_buf mediatype;
                    rt_buf_init(&mediatype);
                    allowed = false;
                    if (data_uri_mediatype(s, &mediatype) == RT_OK) {
                        for (size_t k = 0; k < COUNT_OF(ALLOWED_URI_DATA_MEDIATYPES); k++) {
                            if (mediatype.len == strlen(ALLOWED_URI_DATA_MEDIATYPES[k]) &&
                                memcmp(mediatype.data, ALLOWED_URI_DATA_MEDIATYPES[k], mediatype.len) == 0) {
                                allowed = true;
                                break;
                            }
                        }
                    }
                    rt_buf_dispose(&mediatype);
                }
            }
        }
    } else {
        allowed = false;
    }
    rt_buf_dispose(&lower);
    rt_buf_dispose(&replaced);
    rt_buf_dispose(&filtered);
    rt_buf_dispose(&disposed);
    rt_buf_dispose(&unescaped);
    rt_buf_dispose(&without);
    return allowed;
}

/* --- attribute escaping (force_correct_attribute_escaping) ---------------- */

static bool needs_escape(unsigned char c) {
    return c == ' ' || c == '"' || (c < ' ' && c != '\t' && c != '\n' && c != '\r');
}

static char *escape_attribute_value(const char *value) {
    rt_buf out;
    rt_buf_init(&out);
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; p++) {
        if (*p == ' ') {
            if (rt_buf_puts(&out, "%20") != RT_OK) goto fail;
        } else if (*p == '"') {
            if (rt_buf_puts(&out, "%22") != RT_OK) goto fail;
        } else if (*p == '\t' || *p == '\n' || *p == '\r') {
            if (rt_buf_putc(&out, *p) != RT_OK) goto fail;
        } else if (*p < ' ') {
            continue;
        } else {
            if (rt_buf_putc(&out, *p) != RT_OK) goto fail;
        }
    }
    char *result = NULL;
    if (rt_buf_to_cstr(&out, &result) != RT_OK) {
        rt_buf_dispose(&out);
        return NULL;
    }
    return result;
fail:
    rt_buf_dispose(&out);
    return NULL;
}

static void force_correct_attribute_escaping(rt_dom *dom, rt_node node, bool is_a) {
    rt_dom_node *element = &dom->nodes[node];
    for (size_t i = 0; i < element->attrs_len; i++) {
        rt_attr *attr = &element->attrs[i];
        rt_buf name;
        rt_buf_init(&name);
        rt_dom_attr_qualified_name(attr, &name);
        bool qualified = false;
        if (name.len == 4 && memcmp(name.data, "href", 4) == 0) qualified = true;
        else if (name.len == 6 && memcmp(name.data, "action", 6) == 0) qualified = true;
        else if (name.len == 3 && memcmp(name.data, "src", 3) == 0) qualified = true;
        else if (is_a && name.len == 4 && memcmp(name.data, "name", 4) == 0) qualified = true;
        rt_buf_dispose(&name);
        if (!qualified) continue;
        bool escape = false;
        for (const unsigned char *p = (const unsigned char *)attr->value; *p != '\0'; p++) {
            if (needs_escape(*p)) {
                escape = true;
                break;
            }
        }
        if (escape) {
            char *escaped = escape_attribute_value(attr->value);
            if (escaped != NULL) {
                free(attr->value);
                attr->value = escaped;
            }
        }
    }
}

/* --- style scrubber ------------------------------------------------------- */

static bool starts_with_str(const unsigned char *bytes, size_t len, size_t at, const char *text) {
    size_t n = strlen(text);
    return at + n <= len && memcmp(bytes + at, text, n) == 0;
}

static bool is_plain_color(const char *value) {
    size_t len = strlen(value);
    if (len == 0) return false;
    const unsigned char *s = (const unsigned char *)value;
    /* [a-z]+ (case-insensitive) */
    if ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')) {
        for (size_t i = 1; i <= len; i++) {
            if (i == len) return true;
            if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z'))) break;
        }
    }
    /* #[0-9a-f]{3,8} */
    if (s[0] == '#') {
        size_t digits = 0;
        while (digits < len - 1) {
            unsigned char c = s[1 + digits];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) break;
            digits++;
        }
        if (digits >= 3 && digits <= 8 && 1 + digits == len) return true;
    }
    /* var( \s* --[a-z0-9_-]+ \s* ) */
    if (rt_ascii_istarts(s, len, "var(")) {
        size_t at = 4;
        while (at < len && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r' ||
                            s[at] == 0x0B || s[at] == 0x0C)) {
            at++;
        }
        if (starts_with_str(s, len, at, "--")) {
            at += 2;
            size_t name_start = at;
            while (at < len && ((s[at] >= 'a' && s[at] <= 'z') || (s[at] >= 'A' && s[at] <= 'Z') ||
                                (s[at] >= '0' && s[at] <= '9') || s[at] == '_' || s[at] == '-')) {
                at++;
            }
            if (at > name_start) {
                while (at < len && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r' ||
                                    s[at] == 0x0B || s[at] == 0x0C)) {
                    at++;
                }
                if (at + 1 == len && s[at] == ')') return true;
            }
        }
    }
    /* rgb()/rgba()/hsl()/hsla() with [0-9a-z.,%\s/+-]* */
    static const char *const funcs[] = {"rgb(", "rgba(", "hsl(", "hsla("};
    for (size_t f = 0; f < COUNT_OF(funcs); f++) {
        if (rt_ascii_istarts(s, len, funcs[f])) {
            size_t at = strlen(funcs[f]);
            size_t end = len;
            if (end > at && s[end - 1] == ')') {
                bool ok = true;
                for (size_t i = at; i < end - 1 && ok; i++) {
                    unsigned char c = s[i];
                    bool allowed = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                                   (c >= 'A' && c <= 'Z') || c == '.' || c == ',' || c == '%' ||
                                   c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0B ||
                                   c == 0x0C || c == '/' || c == '+' || c == '-';
                    if (!allowed) ok = false;
                }
                if (ok) return true;
            }
        }
    }
    return false;
}

static void scrub_style(rt_dom *dom, rt_node node) {
    const char *style = rt_dom_attr(dom, node, "style");
    if (style == NULL) return;
    typedef struct {
        char *property;
        char *value;
    } declaration;
    declaration *declarations = NULL;
    size_t count = 0, cap = 0;
    const char *at = style;
    while (*at != '\0') {
        const char *semi = strchr(at, ';');
        size_t decl_len = semi != NULL ? (size_t)(semi - at) : strlen(at);
        const unsigned char *trimmed = (const unsigned char *)at;
        size_t trimmed_len = decl_len;
        rt_trim_whitespace(trimmed, trimmed_len, &trimmed, &trimmed_len);
        if (trimmed_len != 0) {
            const unsigned char *colon = memchr(trimmed, ':', trimmed_len);
            const unsigned char *prop = trimmed;
            size_t prop_len = trimmed_len;
            const unsigned char *value = (const unsigned char *)"";
            size_t value_len = 0;
            if (colon != NULL) {
                prop_len = (size_t)(colon - trimmed);
                value = colon + 1;
                value_len = trimmed_len - prop_len - 1;
            }
            rt_trim_whitespace(prop, prop_len, &prop, &prop_len);
            rt_trim_whitespace(value, value_len, &value, &value_len);
            if (count == cap) {
                size_t new_cap = cap != 0 ? cap * 2 : 8;
                declaration *grown = realloc(declarations, new_cap * sizeof *grown);
                if (grown == NULL) break;
                declarations = grown;
                cap = new_cap;
            }
            char *property = malloc(prop_len + 1);
            char *value_copy = malloc(value_len + 1);
            if (property == NULL || value_copy == NULL) {
                free(property);
                free(value_copy);
                break;
            }
            for (size_t i = 0; i < prop_len; i++) {
                unsigned char c = prop[i];
                if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
                property[i] = (char)c;
            }
            property[prop_len] = '\0';
            memcpy(value_copy, value, value_len);
            value_copy[value_len] = '\0';
            declarations[count].property = property;
            declarations[count].value = value_copy;
            count++;
        }
        if (semi == NULL) break;
        at = semi + 1;
    }
    bool all_allowed = count != 0;
    for (size_t i = 0; i < count; i++) {
        bool allowed = (strcmp(declarations[i].property, "color") == 0 ||
                        strcmp(declarations[i].property, "background-color") == 0) &&
                       is_plain_color(declarations[i].value);
        if (!allowed) all_allowed = false;
    }
    if (!all_allowed) {
        rt_buf scrubbed;
        rt_buf_init(&scrubbed);
        bool failed = false;
        for (size_t i = 0; i < count; i++) {
            bool allowed = (strcmp(declarations[i].property, "color") == 0 ||
                            strcmp(declarations[i].property, "background-color") == 0) &&
                           is_plain_color(declarations[i].value);
            if (allowed) {
                if (rt_buf_printf(&scrubbed, "%s: %s;", declarations[i].property,
                                  declarations[i].value) != RT_OK) {
                    failed = true;
                    break;
                }
            }
        }
        if (!failed) {
            if (scrubbed.len == 0) {
                char *removed = rt_dom_remove_attr(dom, node, "style");
                free(removed);
            } else {
                char *text = NULL;
                if (rt_buf_to_cstr(&scrubbed, &text) == RT_OK) {
                    rt_dom_set_attr(dom, node, "style", text);
                    free(text);
                }
            }
        }
        rt_buf_dispose(&scrubbed);
    }
    for (size_t i = 0; i < count; i++) {
        free(declarations[i].property);
        free(declarations[i].value);
    }
    free(declarations);
}

/* --- scrubber ------------------------------------------------------------- */

static void scrub_bottom_up(rt_dom *dom, rt_node node, const rt_safe_list *list);

/* Removes the attribute at `index` (scrub_attributes iterates by position so
 * names are never re-searched). */
static void remove_attr_at(rt_dom *dom, rt_node node, size_t index) {
    rt_dom_node *element = &dom->nodes[node];
    free(element->attrs[index].name);
    free(element->attrs[index].value);
    memmove(&element->attrs[index], &element->attrs[index + 1],
            (element->attrs_len - index - 1) * sizeof *element->attrs);
    element->attrs_len--;
}

static void scrub_attributes(rt_dom *dom, rt_node node, const rt_safe_list *list) {
    if (!rt_dom_is_element(dom, node)) return;
    const char *local = rt_dom_local_name(dom, node);
    bool is_a = local != NULL && strcmp(local, "a") == 0;
    rt_dom_node *element = &dom->nodes[node];
    size_t i = 0;
    while (i < element->attrs_len) {
        rt_attr *attr = &element->attrs[i];
        rt_buf qualified;
        rt_buf_init(&qualified);
        rt_dom_attr_qualified_name(attr, &qualified);
        char *name = NULL;
        if (rt_buf_to_cstr(&qualified, &name) != RT_OK) {
            rt_buf_dispose(&qualified);
            return;
        }
        rt_buf_dispose(&qualified);

        bool is_uri_attr = false;
        for (size_t k = 0; k < ATTR_VAL_IS_URI_COUNT; k++) {
            if (strcmp(name, RT_ATTR_VAL_IS_URI[k]) == 0) {
                is_uri_attr = true;
                break;
            }
        }
        bool scrubbed = !rt_safe_list_allows_attr(list, name) ||
                        (is_uri_attr && !rt_allowed_uri((const unsigned char *)attr->value,
                                                        strlen(attr->value)));
        bool blank_src = strcmp(name, "src") == 0 && rt_cstr_is_blank(attr->value);
        free(name);
        if (scrubbed) {
            remove_attr_at(dom, node, i);
            continue;
        }
        if (blank_src) {
            remove_attr_at(dom, node, i);
        } else {
            i++;
        }
        force_correct_attribute_escaping(dom, node, is_a);
    }
    scrub_style(dom, node);
}

static void scrub(rt_dom *dom, rt_node node, const rt_safe_list *list) {
    if (rt_dom_is_text(dom, node)) return;
    const char *local = rt_dom_local_name(dom, node);
    bool keep = local != NULL && rt_safe_list_allows_tag(list, local);
    if (!keep) {
        bool foreign = rt_dom_is_element(dom, node) && !rt_dom_is_html_element(dom, node);
        if (!foreign) {
            /* dom.rs: children(node).to_vec() before any insert moves them. */
            size_t count = rt_dom_children_len(dom, node);
            rt_node *children = count != 0 ? malloc(count * sizeof *children) : NULL;
            if (count != 0 && children == NULL) {
                rt_dom_detach(dom, node);
                return;
            }
            for (size_t i = 0; i < count; i++) children[i] = rt_dom_child(dom, node, i);
            for (size_t i = 0; i < count; i++) rt_dom_insert_before(dom, node, children[i]);
            free(children);
        }
        rt_dom_detach(dom, node);
        return;
    }
    scrub_attributes(dom, node, list);
}

static void scrub_bottom_up(rt_dom *dom, rt_node node, const rt_safe_list *list) {
    size_t count = rt_dom_children_len(dom, node);
    rt_node *children = NULL;
    if (count != 0) {
        children = malloc(count * sizeof *children);
        if (children == NULL) return;
        for (size_t i = 0; i < count; i++) children[i] = rt_dom_child(dom, node, i);
    }
    for (size_t i = 0; i < count; i++) scrub_bottom_up(dom, children[i], list);
    free(children);
    scrub(dom, node, list);
}

static rt_status sanitize_impl(const unsigned char *html, size_t len, const rt_safe_list *list,
                               bool escaped_brackets, rt_buf *out) {
    if (len == 0) return RT_OK;
    rt_dom dom;
    rt_dom_init(&dom);
    rt_node fragment = RT_NODE_NONE;
    rt_status rc = rt_dom_parse_fragment(&dom, html, len, &fragment);
    if (rc == RT_OK) {
        size_t count = rt_dom_children_len(&dom, fragment);
        rt_node *children = NULL;
        if (count != 0) {
            children = malloc(count * sizeof *children);
            if (children == NULL) rc = RT_NOMEM;
            else {
                for (size_t i = 0; i < count; i++) children[i] = rt_dom_child(&dom, fragment, i);
            }
        }
        for (size_t i = 0; rc == RT_OK && i < count; i++) scrub_bottom_up(&dom, children[i], list);
        free(children);
        if (rc == RT_OK) rc = rt_dom_serialize(&dom, fragment, escaped_brackets, out);
    }
    rt_dom_dispose(&dom);
    return rc;
}

rt_status rt_sanitize(const unsigned char *html, size_t len, const rt_safe_list *list, rt_buf *out) {
    return sanitize_impl(html, len, list, false, out);
}

rt_status rt_sanitize_escaped_attributes(const unsigned char *html, size_t len, const rt_safe_list *list,
                                         rt_buf *out) {
    return sanitize_impl(html, len, list, true, out);
}
