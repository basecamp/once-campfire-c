/* src/richtext/rt_dom.c — the small DOM adapter over the pinned Gumbo parser.
 *
 * Ports tmp/rust-ref/crates/richtext/src/dom.rs: fragment parsing in
 * Nokogiri's context (Gumbo's `fragment_context`), the parse limits Nokogiri
 * applies (tree depth and attributes per element; the globalID check adds one
 * to the depth for a fragment's html element), and Nokogiri's HTML5
 * serialization as dom.rs reproduces it.
 *
 * Gumbo is used only to build the tree; all mutation happens in the arena
 * below, so parse/serialize round trips behave like the reference DOM.
 */
#include "richtext/internal.h"

#include <stdlib.h>
#include <string.h>

#include "nokogiri_gumbo.h"

/* dom.rs MAX_TREE_DEPTH; Nokogiri raises the option by one for a fragment's
 * implied <html> element, which sits on Gumbo's open-element stack. */
#define RT_MAX_TREE_DEPTH 400
#define RT_MAX_ATTRIBUTES 400

/* ---- arena --------------------------------------------------------------- */

void rt_dom_init(rt_dom *dom) {
    dom->nodes = NULL;
    dom->len = 0;
    dom->cap = 0;
}

static void node_clear(rt_dom_node *node) {
    free(node->name);
    for (size_t i = 0; i < node->attrs_len; i++) {
        free(node->attrs[i].name);
        free(node->attrs[i].value);
    }
    free(node->attrs);
    free(node->children);
}

void rt_dom_dispose(rt_dom *dom) {
    if (dom == NULL) return;
    for (size_t i = 0; i < dom->len; i++) node_clear(&dom->nodes[i]);
    free(dom->nodes);
    dom->nodes = NULL;
    dom->len = 0;
    dom->cap = 0;
}

static rt_node dom_push(rt_dom *dom, uint8_t kind) {
    if (dom->len == dom->cap) {
        size_t cap = dom->cap != 0 ? dom->cap * 2 : 16;
        if (cap < dom->cap) return RT_NODE_NONE;
        rt_dom_node *grown = realloc(dom->nodes, cap * sizeof *grown);
        if (grown == NULL) return RT_NODE_NONE;
        dom->nodes = grown;
        dom->cap = cap;
    }
    rt_dom_node *node = &dom->nodes[dom->len];
    memset(node, 0, sizeof *node);
    node->kind = kind;
    node->ns = RT_NS_HTML;
    node->parent = RT_NODE_NONE;
    return (rt_node)dom->len++;
}

static char *dup_bytes(const unsigned char *bytes, size_t len) {
    char *copy = malloc(len + 1);
    if (copy == NULL) return NULL;
    if (len != 0) memcpy(copy, bytes, len);
    copy[len] = '\0';
    return copy;
}

static char *dup_cstr(const char *text) {
    return dup_bytes((const unsigned char *)text, strlen(text));
}

/* ---- annotation-xml integration-point neutralization ----------------------
 *
 * The pinned Rust reference parses with html5ever 0.35 and a TreeSink that
 * leaves TreeSink::is_mathml_annotation_xml_integration_point at its false
 * default (markup5ever 0.35, interface/tree_builder.rs), so a MathML
 * `annotation-xml` is never an HTML integration point there: its children are
 * parsed as foreign content even when the element carries
 * encoding="text/html"/"application/xhtml+xml" (ASCII case-insensitive), the
 * rule HTML5 prescribes and Gumbo implements (parser.c
 * is_html_integration_point -> attribute_matches("encoding", ...)).
 *
 * To make Gumbo build the reference's tree, every real attribute named
 * `encoding` in the fragment source is renamed to RT_ENC_NEUTRAL_NAME before
 * the parse; copying the tree into the arena renames it back. Only attribute
 * names are touched, and an attribute name is spelled literally in the source
 * (character references are not decoded in names), so a parsed name equal to
 * the neutral name is always one this code renamed: a source that already
 * spells the neutral name (any case) is parsed unchanged. The name has no HTML
 * structural meaning besides the integration-point check, so the tree is
 * otherwise exactly the one Gumbo would have built. */

#define RT_ENC_NEUTRAL_NAME "campfire-rt-enc-ni"

static bool enc_fold_eq(unsigned char a, unsigned char b) {
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
    return a == b;
}

static bool enc_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool enc_alpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* Case-insensitive (ASCII) search for `needle` in in[from, limit). */
static size_t enc_find(const unsigned char *in, size_t from, size_t limit, const char *needle) {
    size_t len = strlen(needle);
    for (size_t i = from; i + len <= limit; i++) {
        size_t j = 0;
        while (j < len && enc_fold_eq(in[i + j], (unsigned char)needle[j])) j++;
        if (j == len) return i;
    }
    return SIZE_MAX;
}

static bool enc_name_is(const unsigned char *in, size_t len, const char *name) {
    size_t name_len = strlen(name);
    if (len != name_len) return false;
    for (size_t i = 0; i < len; i++) {
        if (!enc_fold_eq(in[i], (unsigned char)name[i])) return false;
    }
    return true;
}

static bool enc_range_is(const unsigned char *a, const unsigned char *b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (!enc_fold_eq(a[i], b[i])) return false;
    }
    return true;
}

/* Elements whose content is raw text/RCDATA -- but only where the HTML
 * namespace applies (see the foreign-content tracker below). */
static bool enc_raw_text_name(const unsigned char *in, size_t len) {
    static const char *const names[] = {
        "script", "style", "textarea", "title", "xmp", "iframe", "noembed", "noframes",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (enc_name_is(in, len, names[i])) return true;
    }
    return false;
}

/* HTML5's foreign-content breakout start tags: Gumbo pops every foreign
 * element that is not an integration point and handles the tag with the HTML
 * rules. `font` breaks out only with a color/face/size attribute, but popping
 * unconditionally only makes the scan treat more content as HTML content,
 * where skipping can miss a rename but never invent one. */
static bool enc_breakout_name(const unsigned char *in, size_t len) {
    static const char *const names[] = {
        "b", "big", "blockquote", "body", "br", "center", "code", "dd", "div", "dl",
        "dt", "em", "embed", "h1", "h2", "h3", "h4", "h5", "h6", "head", "hr", "i",
        "img", "li", "listing", "menu", "meta", "nobr", "ol", "p", "pre", "ruby", "s",
        "small", "span", "strong", "strike", "sub", "sup", "table", "tt", "u", "ul",
        "var", "font",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (enc_name_is(in, len, names[i])) return true;
    }
    return false;
}

/* ---- foreign-content tracking for the raw-text scan ----------------------
 *
 * Raw-text skipping is only correct in the HTML namespace: Gumbo switches the
 * tokenizer to RCDATA/RAWTEXT/SCRIPT_DATA for these names when its HTML rules
 * handle the start tag. In SVG/MathML foreign content it keeps the DATA state,
 * so `<svg><title>` is ordinary markup -- a `<math><annotation-xml
 * encoding=...>` nested in it is real markup, and its attribute rename must
 * still happen (the reference never applies the annotation-xml integration
 * point either, so a missed rename drops the mention). The scan therefore
 * tracks enough tree-builder state to know where HTML content applies:
 *
 *  - `svg`/`math` start tags in HTML content open a foreign frame;
 *  - SVG `foreignObject`/`desc`/`title` and MathML `mi`/`mo`/`mn`/`ms`/`mtext`
 *    are integration points whose children are HTML again;
 *  - a MathML `annotation-xml` is modeled as if its `encoding` attribute were
 *    already renamed (it is, whenever the scan rewrites at all), so it is not
 *    an integration point here; its child `svg` start tag is HTML-handled;
 *  - Gumbo's foreign-content breakout tags return to HTML content, as do
 *    `mglyph`/`malignmark` inside a MathML text integration point;
 *  - a nested `<select>` closes the open select frame, and while an HTML
 *    select frame is open `svg`/`math` are ignored (Gumbo's in-select mode
 *    drops them); `input`/`keygen`/`textarea` close it too, and an open
 *    `template` frame (the one element that can sit inside a select and still
 *    be parsed) puts the select out of scope.
 *
 * Only frames that can change the content mode are pushed. The model is
 * deliberately coarse and errs toward HTML content: an end tag in foreign
 * content that matches no tracked frame pops back to HTML (Gumbo's HTML
 * end-tag handling can pop foreign elements too), so anything it cannot follow
 * makes the scan skip raw text rather than rename inside text Gumbo tokenizes
 * as raw. The one thing that must never happen is a rename inside such text. */

typedef enum {
    ENC_NS_HTML = 0,
    ENC_NS_SVG,
    ENC_NS_MATHML,
} enc_ns;

typedef struct {
    const unsigned char *name; /* points into the scanned input */
    size_t name_len;
    uint8_t ns;          /* enc_ns */
    bool html_ip;        /* children parsed with HTML rules (SVG integration point) */
    bool mathml_tip;     /* children parsed with HTML rules (MathML text integration point) */
    bool annotation_xml; /* MathML annotation-xml: a child `svg` start tag is HTML-handled */
} enc_frame;

typedef struct {
    enc_frame *items;
    size_t len, cap;
} enc_frames;

static enc_frame *enc_frames_top(enc_frames *frames) {
    return &frames->items[frames->len - 1];
}

static bool enc_frames_push(enc_frames *frames, enc_frame frame) {
    if (frames->len == frames->cap) {
        size_t cap = frames->cap != 0 ? frames->cap * 2 : 8;
        enc_frame *grown = realloc(frames->items, cap * sizeof *grown);
        if (grown == NULL) return false;
        frames->items = grown;
        frames->cap = cap;
    }
    frames->items[frames->len++] = frame;
    return true;
}

/* Content handled by Gumbo's HTML rules: the HTML namespace, or an integration
 * point whose children are HTML again. */
static bool enc_frame_html(const enc_frame *frame) {
    return frame->ns == ENC_NS_HTML || frame->html_ip || frame->mathml_tip;
}

/* The topmost frame (never the root) with this name, or SIZE_MAX. */
static size_t enc_frames_find(const enc_frames *frames, const unsigned char *name, size_t len) {
    for (size_t i = frames->len; i-- > 1;) {
        if (frames->items[i].name_len == len &&
            enc_range_is(frames->items[i].name, name, len)) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* Pops the frame at `index` and everything above it. */
static void enc_frames_pop_to(enc_frames *frames, size_t index) {
    frames->len = index;
}

/* Pops foreign frames until the content is HTML again (the root stays). */
static void enc_frames_pop_html(enc_frames *frames) {
    while (frames->len > 1 && !enc_frame_html(&frames->items[frames->len - 1])) {
        frames->len--;
    }
}

/* The topmost open in-scope HTML-namespace select frame, or SIZE_MAX. A
 * `template` frame between it and the current node puts it out of select
 * scope (Gumbo's has_an_element_in_select_scope). */
static size_t enc_frames_select(const enc_frames *frames) {
    for (size_t i = frames->len; i-- > 1;) {
        if (frames->items[i].ns != ENC_NS_HTML) continue;
        if (enc_name_is(frames->items[i].name, frames->items[i].name_len, "template")) {
            return SIZE_MAX;
        }
        if (enc_name_is(frames->items[i].name, frames->items[i].name_len, "select")) {
            return i;
        }
    }
    return SIZE_MAX;
}

typedef struct {
    size_t *items;
    size_t len, cap;
    bool saw_neutral; /* a real attribute already named RT_ENC_NEUTRAL_NAME */
} enc_offsets;

static bool enc_offsets_push(enc_offsets *offsets, size_t off) {
    if (offsets->len == offsets->cap) {
        size_t cap = offsets->cap != 0 ? offsets->cap * 2 : 8;
        size_t *grown = realloc(offsets->items, cap * sizeof *grown);
        if (grown == NULL) return false;
        offsets->items = grown;
        offsets->cap = cap;
    }
    offsets->items[offsets->len++] = off;
    return true;
}

/* Collects the byte offsets of real attribute names that are exactly
 * `encoding` (ASCII case-insensitive). The scan is conservative: it only
 * classifies tag syntax, skips comments, bogus comments, CDATA and raw text
 * content where the HTML namespace applies (see the foreign-content tracker
 * above), and gives up (keeping the offsets found so far) on anything it
 * cannot classify, so it can miss a rename but never invent one. */
static bool enc_scan(const unsigned char *in, size_t len, enc_offsets *offsets) {
    enc_frames frames = {0};
    const enc_frame root = {NULL, 0, ENC_NS_HTML, false, false, false};
    bool ok = enc_frames_push(&frames, root);
    size_t i = 0;
    while (ok && i < len) {
        size_t lt = i;
        while (lt < len && in[lt] != '<') lt++;
        if (lt == len) break;
        i = lt;
        if (i + 4 <= len && memcmp(in + i, "<!--", 4) == 0) {
            size_t end = enc_find(in, i + 4, len, "-->");
            size_t alt = enc_find(in, i + 4, len, "--!>");
            if (alt != SIZE_MAX && (end == SIZE_MAX || alt < end)) end = alt;
            if (end == SIZE_MAX) break;
            i = end + 3;
            continue;
        }
        if (i + 9 <= len && memcmp(in + i, "<![CDATA[", 9) == 0) {
            size_t end = enc_find(in, i + 9, len, "]]>");
            if (end == SIZE_MAX) break;
            i = end + 3;
            continue;
        }
        if (i + 2 <= len && (in[i + 1] == '!' || in[i + 1] == '?')) {
            size_t gt = i + 2;
            while (gt < len && in[gt] != '>') gt++;
            if (gt == len) break;
            i = gt + 1;
            continue;
        }
        if (i + 2 <= len && in[i + 1] == '/') {
            size_t name_start = i + 2;
            size_t name_len = 0;
            while (name_start + name_len < len && !enc_space(in[name_start + name_len]) &&
                   in[name_start + name_len] != '/' && in[name_start + name_len] != '>') {
                name_len++;
            }
            size_t gt = name_start + name_len;
            while (gt < len && in[gt] != '>') gt++;
            if (gt == len) break;
            size_t match = enc_frames_find(&frames, in + name_start, name_len);
            if (match != SIZE_MAX) {
                enc_frames_pop_to(&frames, match);
            } else if (!enc_frame_html(enc_frames_top(&frames))) {
                /* An HTML end tag Gumbo handles can pop foreign elements. */
                enc_frames_pop_html(&frames);
            }
            i = gt + 1;
            continue;
        }
        if (i + 1 >= len || !enc_alpha(in[i + 1])) {
            i++; /* a lone '<' is text */
            continue;
        }
        /* A start tag: its name, then its attributes. */
        size_t name_len = 0;
        while (i + 1 + name_len < len && !enc_space(in[i + 1 + name_len]) &&
               in[i + 1 + name_len] != '/' && in[i + 1 + name_len] != '>') {
            name_len++;
        }
        const unsigned char *tag_name = in + i + 1;
        bool self_closing = false;
        size_t k = i + 1 + name_len;
        while (k < len) {
            while (k < len && enc_space(in[k])) k++;
            if (k >= len) break;
            if (in[k] == '>') {
                k++;
                break;
            }
            if (in[k] == '/') {
                self_closing = true;
                k++;
                continue;
            }
            self_closing = false;
            size_t attr_start = k;
            while (k < len && !enc_space(in[k]) && in[k] != '=' && in[k] != '/' && in[k] != '>') k++;
            if (enc_name_is(in + attr_start, k - attr_start, "encoding")) {
                if (!enc_offsets_push(offsets, attr_start)) {
                    ok = false;
                    break;
                }
            } else if (enc_name_is(in + attr_start, k - attr_start, RT_ENC_NEUTRAL_NAME)) {
                offsets->saw_neutral = true;
            }
            while (k < len && enc_space(in[k])) k++;
            if (k < len && in[k] == '=') {
                k++;
                while (k < len && enc_space(in[k])) k++;
                if (k < len && (in[k] == '"' || in[k] == '\'')) {
                    unsigned char quote = in[k++];
                    while (k < len && in[k] != quote) k++;
                    if (k < len) k++;
                } else {
                    while (k < len && !enc_space(in[k]) && in[k] != '>') k++;
                }
            }
        }
        if (!ok) break;
        enc_frame *top = enc_frames_top(&frames);
        bool in_html = enc_frame_html(top);
        if (in_html && top->mathml_tip &&
            (enc_name_is(tag_name, name_len, "mglyph") ||
             enc_name_is(tag_name, name_len, "malignmark"))) {
            in_html = false; /* MathML text integration point exception */
        }
        if (!in_html && enc_breakout_name(tag_name, name_len)) {
            enc_frames_pop_html(&frames);
            in_html = true;
        }
        if (!in_html && top->annotation_xml && enc_name_is(tag_name, name_len, "svg")) {
            /* MathML annotation-xml child `svg`: Gumbo's HTML rules run it. */
            if (!self_closing) {
                const enc_frame svg = {tag_name, name_len, ENC_NS_SVG, false, false, false};
                if (!enc_frames_push(&frames, svg)) {
                    ok = false;
                    break;
                }
            }
            i = k;
            continue;
        }
        if (in_html) {
            /* Gumbo's in-select mode closes an in-scope select on these. */
            if (enc_name_is(tag_name, name_len, "input") ||
                enc_name_is(tag_name, name_len, "keygen") ||
                enc_name_is(tag_name, name_len, "textarea")) {
                size_t open = enc_frames_select(&frames);
                if (open != SIZE_MAX) enc_frames_pop_to(&frames, open);
            }
            if (enc_raw_text_name(tag_name, name_len)) {
                /* Skip the raw content to the matching end tag. Script's
                 * escaped/double-escaped states are not modelled, so a script
                 * whose content could enter them ends the scan. */
                size_t close = SIZE_MAX;
                for (size_t at = k; at + 2 + name_len <= len; at++) {
                    if (in[at] != '<' || in[at + 1] != '/') continue;
                    if (!enc_range_is(in + at + 2, tag_name, name_len)) continue;
                    size_t after = at + 2 + name_len;
                    if (after >= len || enc_space(in[after]) || in[after] == '/' ||
                        in[after] == '>') {
                        close = at;
                        break;
                    }
                }
                if (close == SIZE_MAX) break;
                if (enc_name_is(tag_name, name_len, "script") &&
                    (enc_find(in, k, close, "<script") != SIZE_MAX ||
                     enc_find(in, k, close, "<!--") != SIZE_MAX)) {
                    break;
                }
                i = close;
                continue;
            }
            if (enc_name_is(tag_name, name_len, "plaintext")) break; /* rest is text */
            if (enc_name_is(tag_name, name_len, "svg") ||
                enc_name_is(tag_name, name_len, "math")) {
                /* Gumbo's in-select mode drops foreign start tags. */
                if (!self_closing && enc_frames_select(&frames) == SIZE_MAX) {
                    uint8_t ns =
                        enc_name_is(tag_name, name_len, "svg") ? ENC_NS_SVG : ENC_NS_MATHML;
                    const enc_frame frame = {tag_name, name_len, ns, false, false, false};
                    if (!enc_frames_push(&frames, frame)) {
                        ok = false;
                        break;
                    }
                }
                i = k;
                continue;
            }
            if (enc_name_is(tag_name, name_len, "select")) {
                size_t open = enc_frames_select(&frames);
                if (open != SIZE_MAX) {
                    enc_frames_pop_to(&frames, open); /* a nested select closes it */
                } else {
                    const enc_frame frame = {tag_name, name_len, ENC_NS_HTML, false, false, false};
                    if (!enc_frames_push(&frames, frame)) {
                        ok = false;
                        break;
                    }
                }
                i = k;
                continue;
            }
            if (enc_name_is(tag_name, name_len, "template")) {
                const enc_frame frame = {tag_name, name_len, ENC_NS_HTML, false, false, false};
                if (!enc_frames_push(&frames, frame)) {
                    ok = false;
                    break;
                }
                i = k;
                continue;
            }
            i = k;
            continue;
        }
        /* Foreign insertion: the new element stays in the current namespace. */
        if (!self_closing) {
            uint8_t ns = top->ns;
            bool html_ip =
                ns == ENC_NS_SVG &&
                (enc_name_is(tag_name, name_len, "foreignobject") ||
                 enc_name_is(tag_name, name_len, "desc") ||
                 enc_name_is(tag_name, name_len, "title"));
            bool mathml_tip =
                ns == ENC_NS_MATHML &&
                (enc_name_is(tag_name, name_len, "mi") ||
                 enc_name_is(tag_name, name_len, "mo") ||
                 enc_name_is(tag_name, name_len, "mn") ||
                 enc_name_is(tag_name, name_len, "ms") ||
                 enc_name_is(tag_name, name_len, "mtext"));
            bool annotation_xml =
                ns == ENC_NS_MATHML && enc_name_is(tag_name, name_len, "annotation-xml");
            const enc_frame frame = {tag_name, name_len, ns, html_ip, mathml_tip, annotation_xml};
            if (!enc_frames_push(&frames, frame)) {
                ok = false;
                break;
            }
        }
        i = k;
    }
    free(frames.items);
    return ok;
}

/* Builds the parse source with every `encoding` attribute name replaced:
 * 1 when the source changed, 0 when it is parsed unchanged (no such attribute,
 * or a real attribute already carries the neutral name, which would make the
 * restore ambiguous), -1 on allocation failure. */
static int enc_neutralize(const unsigned char *in, size_t len, rt_buf *out) {
    enc_offsets offsets = {0};
    if (!enc_scan(in, len, &offsets)) {
        free(offsets.items);
        return -1;
    }
    int rc = offsets.len != 0 && !offsets.saw_neutral ? 1 : 0;
    size_t copied = 0;
    for (size_t i = 0; rc == 1 && i < offsets.len; i++) {
        size_t off = offsets.items[i];
        if (rt_buf_append(out, in + copied, off - copied) != RT_OK ||
            rt_buf_puts(out, RT_ENC_NEUTRAL_NAME) != RT_OK) {
            rc = -1;
            break;
        }
        copied = off + 8; /* the `encoding` attribute name */
    }
    if (rc == 1 && rt_buf_append(out, in + copied, len - copied) != RT_OK) rc = -1;
    free(offsets.items);
    return rc;
}

static bool children_push(rt_dom_node *node, rt_node child) {
    if (node->children_len == node->children_cap) {
        size_t cap = node->children_cap != 0 ? node->children_cap * 2 : 4;
        if (cap < node->children_cap) return false;
        rt_node *grown = realloc(node->children, cap * sizeof *grown);
        if (grown == NULL) return false;
        node->children = grown;
        node->children_cap = cap;
    }
    node->children[node->children_len++] = child;
    return true;
}

static bool attrs_push(rt_dom_node *node, const char *name, const char *value, uint8_t ns,
                       bool restore) {
    if (node->attrs_len == node->attrs_cap) {
        size_t cap = node->attrs_cap != 0 ? node->attrs_cap * 2 : 4;
        if (cap < node->attrs_cap) return false;
        rt_attr *grown = realloc(node->attrs, cap * sizeof *grown);
        if (grown == NULL) return false;
        node->attrs = grown;
        node->attrs_cap = cap;
    }
    rt_attr *attr = &node->attrs[node->attrs_len];
    if (restore && strcmp(name, RT_ENC_NEUTRAL_NAME) == 0) name = "encoding";
    attr->name = dup_cstr(name);
    attr->value = dup_cstr(value);
    if (attr->name == NULL || attr->value == NULL) {
        free(attr->name);
        free(attr->value);
        return false;
    }
    attr->ns = ns;
    node->attrs_len++;
    return true;
}

/* ---- accessors ----------------------------------------------------------- */

const char *rt_dom_name(const rt_dom *dom, rt_node node) {
    if (node >= dom->len) return NULL;
    switch (dom->nodes[node].kind) {
    case RT_NODE_KIND_ELEMENT: return dom->nodes[node].name;
    case RT_NODE_KIND_TEXT: return "text";
    case RT_NODE_KIND_COMMENT: return "comment";
    case RT_NODE_KIND_FRAGMENT: return "#document-fragment";
    case RT_NODE_KIND_DOCUMENT: return "document";
    case RT_NODE_KIND_DOCTYPE: return dom->nodes[node].name;
    default: return NULL;
    }
}

const char *rt_dom_local_name(const rt_dom *dom, rt_node node) {
    if (node >= dom->len || dom->nodes[node].kind != RT_NODE_KIND_ELEMENT) return NULL;
    return dom->nodes[node].name;
}

bool rt_dom_is_element(const rt_dom *dom, rt_node node) {
    return node < dom->len && dom->nodes[node].kind == RT_NODE_KIND_ELEMENT;
}

bool rt_dom_is_text(const rt_dom *dom, rt_node node) {
    return node < dom->len && dom->nodes[node].kind == RT_NODE_KIND_TEXT;
}

bool rt_dom_is_html_element(const rt_dom *dom, rt_node node) {
    return rt_dom_is_element(dom, node) && dom->nodes[node].ns == RT_NS_HTML;
}

rt_node rt_dom_parent(const rt_dom *dom, rt_node node) {
    if (node >= dom->len) return RT_NODE_NONE;
    return dom->nodes[node].parent;
}

size_t rt_dom_children_len(const rt_dom *dom, rt_node node) {
    if (node >= dom->len) return 0;
    return dom->nodes[node].children_len;
}

rt_node rt_dom_child(const rt_dom *dom, rt_node node, size_t index) {
    if (node >= dom->len || index >= dom->nodes[node].children_len) return RT_NODE_NONE;
    return dom->nodes[node].children[index];
}

const char *rt_dom_text(const rt_dom *dom, rt_node node) {
    if (node >= dom->len || dom->nodes[node].kind != RT_NODE_KIND_TEXT) return NULL;
    return dom->nodes[node].name;
}

rt_node_vec rt_dom_descendants(const rt_dom *dom, rt_node node) {
    rt_node_vec out;
    rt_node_vec_init(&out);
    if (node >= dom->len) return out;
    /* Iterative preorder, document order (dom.rs descendants). */
    rt_node_vec stack;
    rt_node_vec_init(&stack);
    const rt_dom_node *n = &dom->nodes[node];
    for (size_t i = n->children_len; i > 0; i--) {
        if (rt_node_vec_push(&stack, n->children[i - 1]) != RT_OK) {
            rt_node_vec_dispose(&out);
            rt_node_vec_dispose(&stack);
            return out;
        }
    }
    while (stack.len != 0) {
        rt_node current = stack.items[--stack.len];
        if (rt_node_vec_push(&out, current) != RT_OK) {
            rt_node_vec_dispose(&out);
            rt_node_vec_dispose(&stack);
            return out;
        }
        const rt_dom_node *cn = &dom->nodes[current];
        for (size_t i = cn->children_len; i > 0; i--) {
            if (rt_node_vec_push(&stack, cn->children[i - 1]) != RT_OK) {
                rt_node_vec_dispose(&out);
                rt_node_vec_dispose(&stack);
                return out;
            }
        }
    }
    rt_node_vec_dispose(&stack);
    return out;
}

rt_node_vec rt_dom_ancestors(const rt_dom *dom, rt_node node) {
    rt_node_vec out;
    rt_node_vec_init(&out);
    if (node >= dom->len) return out;
    rt_node current = dom->nodes[node].parent;
    while (current != RT_NODE_NONE) {
        if (rt_node_vec_push(&out, current) != RT_OK) break;
        current = dom->nodes[current].parent;
    }
    return out;
}

rt_node_vec rt_dom_element_children(const rt_dom *dom, rt_node node) {
    rt_node_vec out;
    rt_node_vec_init(&out);
    if (node >= dom->len) return out;
    const rt_dom_node *n = &dom->nodes[node];
    for (size_t i = 0; i < n->children_len; i++) {
        if (rt_dom_is_element(dom, n->children[i]) &&
            rt_node_vec_push(&out, n->children[i]) != RT_OK) {
            break;
        }
    }
    return out;
}

/* ---- attribute names ----------------------------------------------------- */

void rt_dom_attr_qualified_name(const rt_attr *attr, rt_buf *buf) {
    switch (attr->ns) {
    case RT_ATTR_NS_XLINK: rt_buf_puts(buf, "xlink:"); break;
    case RT_ATTR_NS_XML: rt_buf_puts(buf, "xml:"); break;
    case RT_ATTR_NS_XMLNS:
        if (strcmp(attr->name, "xmlns") != 0) rt_buf_puts(buf, "xmlns:");
        break;
    default: break;
    }
    rt_buf_puts(buf, attr->name);
}

/* dom.rs Attr::has_name: an unprefixed name matches only a no-namespace
 * attribute; "prefix:local" matches the mapped namespace's local name. */
static bool attr_has_name(const rt_attr *attr, const char *name) {
    const char *colon = strchr(name, ':');
    if (colon == NULL) {
        return attr->ns == RT_ATTR_NS_NONE && strcmp(attr->name, name) == 0;
    }
    size_t prefix_len = (size_t)(colon - name);
    const char *local = colon + 1;
    uint8_t ns = RT_ATTR_NS_NONE;
    if (prefix_len == 5 && strncmp(name, "xlink", 5) == 0) ns = RT_ATTR_NS_XLINK;
    else if (prefix_len == 3 && strncmp(name, "xml", 3) == 0) ns = RT_ATTR_NS_XML;
    else if (prefix_len == 5 && strncmp(name, "xmlns", 5) == 0) ns = RT_ATTR_NS_XMLNS;
    else return false;
    return attr->ns == ns && strcmp(attr->name, local) == 0;
}

const char *rt_dom_attr(const rt_dom *dom, rt_node node, const char *name) {
    if (node >= dom->len || dom->nodes[node].kind != RT_NODE_KIND_ELEMENT) return NULL;
    const rt_dom_node *n = &dom->nodes[node];
    for (size_t i = 0; i < n->attrs_len; i++) {
        if (attr_has_name(&n->attrs[i], name)) return n->attrs[i].value;
    }
    return NULL;
}

bool rt_dom_has_attr(const rt_dom *dom, rt_node node, const char *name) {
    return rt_dom_attr(dom, node, name) != NULL;
}

static rt_attr *attr_find(rt_dom *dom, rt_node node, const char *name) {
    if (node >= dom->len || dom->nodes[node].kind != RT_NODE_KIND_ELEMENT) return NULL;
    rt_dom_node *n = &dom->nodes[node];
    for (size_t i = 0; i < n->attrs_len; i++) {
        if (attr_has_name(&n->attrs[i], name)) return &n->attrs[i];
    }
    return NULL;
}

rt_status rt_dom_set_attr(rt_dom *dom, rt_node node, const char *name, const char *value) {
    if (node >= dom->len || dom->nodes[node].kind != RT_NODE_KIND_ELEMENT) return RT_RAISED;
    rt_attr *attr = attr_find(dom, node, name);
    if (attr != NULL) {
        char *copy = dup_cstr(value);
        if (copy == NULL) return RT_NOMEM;
        free(attr->value);
        attr->value = copy;
        return RT_OK;
    }
    /* dom.rs set_attr appends with no prefix namespace. */
    return attrs_push(&dom->nodes[node], name, value, RT_ATTR_NS_NONE, false) ? RT_OK : RT_NOMEM;
}

char *rt_dom_remove_attr(rt_dom *dom, rt_node node, const char *name) {
    if (node >= dom->len || dom->nodes[node].kind != RT_NODE_KIND_ELEMENT) return NULL;
    rt_dom_node *n = &dom->nodes[node];
    for (size_t i = 0; i < n->attrs_len; i++) {
        if (attr_has_name(&n->attrs[i], name)) {
            char *value = n->attrs[i].value;
            free(n->attrs[i].name);
            memmove(&n->attrs[i], &n->attrs[i + 1], (n->attrs_len - i - 1) * sizeof *n->attrs);
            n->attrs_len--;
            return value;
        }
    }
    return NULL;
}

/* ---- construction and mutation ------------------------------------------ */

rt_node rt_dom_create_element(rt_dom *dom, const char *name, const char *const *names,
                              const char *const *values, size_t count) {
    rt_node node = dom_push(dom, RT_NODE_KIND_ELEMENT);
    if (node == RT_NODE_NONE) return RT_NODE_NONE;
    dom->nodes[node].name = dup_cstr(name);
    dom->nodes[node].name_len = strlen(name);
    if (dom->nodes[node].name == NULL) return RT_NODE_NONE;
    for (size_t i = 0; i < count; i++) {
        if (!attrs_push(&dom->nodes[node], names[i], values[i], RT_ATTR_NS_NONE, false)) {
            return RT_NODE_NONE;
        }
    }
    return node;
}

rt_node rt_dom_create_text(rt_dom *dom, const unsigned char *text, size_t len) {
    rt_node node = dom_push(dom, RT_NODE_KIND_TEXT);
    if (node == RT_NODE_NONE) return RT_NODE_NONE;
    dom->nodes[node].name = dup_bytes(text, len);
    dom->nodes[node].name_len = len;
    if (dom->nodes[node].name == NULL) return RT_NODE_NONE;
    return node;
}

void rt_dom_detach(rt_dom *dom, rt_node node) {
    if (node >= dom->len) return;
    rt_node parent = dom->nodes[node].parent;
    if (parent == RT_NODE_NONE) return;
    rt_dom_node *p = &dom->nodes[parent];
    for (size_t i = 0; i < p->children_len; i++) {
        if (p->children[i] == node) {
            memmove(&p->children[i], &p->children[i + 1], (p->children_len - i - 1) * sizeof *p->children);
            p->children_len--;
            break;
        }
    }
    dom->nodes[node].parent = RT_NODE_NONE;
}

rt_status rt_dom_append(rt_dom *dom, rt_node parent, rt_node child) {
    if (parent >= dom->len || child >= dom->len || parent == child) return RT_RAISED;
    rt_dom_detach(dom, child);
    if (!children_push(&dom->nodes[parent], child)) return RT_NOMEM;
    dom->nodes[child].parent = parent;
    return RT_OK;
}

rt_status rt_dom_insert_before(rt_dom *dom, rt_node reference, rt_node new_node) {
    if (reference >= dom->len || new_node >= dom->len) return RT_RAISED;
    rt_node parent = dom->nodes[reference].parent;
    if (parent == RT_NODE_NONE) return RT_RAISED;
    rt_dom_detach(dom, new_node);
    rt_dom_node *p = &dom->nodes[parent];
    size_t index = 0;
    while (index < p->children_len && p->children[index] != reference) index++;
    if (index == p->children_len) return RT_RAISED;
    if (p->children_len == p->children_cap) {
        size_t cap = p->children_cap != 0 ? p->children_cap * 2 : 4;
        if (cap < p->children_cap) return RT_NOMEM;
        rt_node *grown = realloc(p->children, cap * sizeof *grown);
        if (grown == NULL) return RT_NOMEM;
        p->children = grown;
        p->children_cap = cap;
        p = &dom->nodes[parent];
    }
    memmove(&p->children[index + 1], &p->children[index], (p->children_len - index) * sizeof *p->children);
    p->children[index] = new_node;
    p->children_len++;
    dom->nodes[new_node].parent = parent;
    return RT_OK;
}

void rt_dom_replace_with_nodes(rt_dom *dom, rt_node node, const rt_node *nodes, size_t count) {
    if (node >= dom->len) return;
    if (dom->nodes[node].parent == RT_NODE_NONE) return;
    for (size_t i = 0; i < count; i++) {
        if (nodes[i] != node) rt_dom_insert_before(dom, node, nodes[i]);
    }
    rt_dom_detach(dom, node);
}

/* ---- Gumbo parsing ------------------------------------------------------- */

static rt_node copy_gumbo_node(rt_dom *dom, const GumboNode *source, bool restore);

static bool copy_gumbo_children(rt_dom *dom, rt_node target, const GumboNode *source,
                                bool restore) {
    const GumboVector *children;
    if (source->type == GUMBO_NODE_DOCUMENT) {
        children = &source->v.document.children;
    } else {
        children = &source->v.element.children;
    }
    for (unsigned i = 0; i < children->length; i++) {
        rt_node child = copy_gumbo_node(dom, (const GumboNode *)children->data[i], restore);
        if (child == RT_NODE_NONE) return false;
        if (!children_push(&dom->nodes[target], child)) return false;
        dom->nodes[child].parent = target;
    }
    return true;
}

static rt_node copy_gumbo_node(rt_dom *dom, const GumboNode *source, bool restore) {
    rt_node node;
    switch (source->type) {
    case GUMBO_NODE_ELEMENT:
    case GUMBO_NODE_TEMPLATE: {
        const GumboElement *element = &source->v.element;
        node = dom_push(dom, RT_NODE_KIND_ELEMENT);
        if (node == RT_NODE_NONE) return RT_NODE_NONE;
        const char *name = element->name;
        if (name == NULL) name = gumbo_normalized_tagname(element->tag);
        if (name == NULL) name = "";
        dom->nodes[node].name = dup_cstr(name);
        if (dom->nodes[node].name == NULL) return RT_NODE_NONE;
        dom->nodes[node].name_len = strlen(dom->nodes[node].name);
        switch (element->tag_namespace) {
        case GUMBO_NAMESPACE_SVG: dom->nodes[node].ns = RT_NS_SVG; break;
        case GUMBO_NAMESPACE_MATHML: dom->nodes[node].ns = RT_NS_MATHML; break;
        default: dom->nodes[node].ns = RT_NS_HTML; break;
        }
        for (unsigned i = 0; i < element->attributes.length; i++) {
            const GumboAttribute *attr = (const GumboAttribute *)element->attributes.data[i];
            uint8_t ns = RT_ATTR_NS_NONE;
            switch (attr->attr_namespace) {
            case GUMBO_ATTR_NAMESPACE_XLINK: ns = RT_ATTR_NS_XLINK; break;
            case GUMBO_ATTR_NAMESPACE_XML: ns = RT_ATTR_NS_XML; break;
            case GUMBO_ATTR_NAMESPACE_XMLNS: ns = RT_ATTR_NS_XMLNS; break;
            default: ns = RT_ATTR_NS_NONE; break;
            }
            if (!attrs_push(&dom->nodes[node], attr->name, attr->value, ns, restore)) {
                return RT_NODE_NONE;
            }
        }
        if (!copy_gumbo_children(dom, node, source, restore)) return RT_NODE_NONE;
        return node;
    }
    case GUMBO_NODE_TEXT:
    case GUMBO_NODE_WHITESPACE:
    case GUMBO_NODE_CDATA: {
        /* html5ever has no CDATA node; a foreign-content CDATA section arrives
         * as text (dom.rs NodeData::Text). */
        const char *text = source->v.text.text;
        node = rt_dom_create_text(dom, (const unsigned char *)(text != NULL ? text : ""),
                                  text != NULL ? strlen(text) : 0);
        return node;
    }
    case GUMBO_NODE_COMMENT: {
        const char *text = source->v.text.text;
        node = dom_push(dom, RT_NODE_KIND_COMMENT);
        if (node == RT_NODE_NONE) return RT_NODE_NONE;
        dom->nodes[node].name = dup_cstr(text != NULL ? text : "");
        dom->nodes[node].name_len = strlen(dom->nodes[node].name);
        if (dom->nodes[node].name == NULL) return RT_NODE_NONE;
        return node;
    }
    default:
        return RT_NODE_NONE;
    }
}

/* RT_NS_* to Gumbo's namespace enum (the reference Context carries the
 * context element's full QualName, namespace included). */
static GumboNamespaceEnum context_namespace(uint8_t ns) {
    switch (ns) {
    case RT_NS_SVG: return GUMBO_NAMESPACE_SVG;
    case RT_NS_MATHML: return GUMBO_NAMESPACE_MATHML;
    default: return GUMBO_NAMESPACE_HTML;
    }
}

/* Parses `html` in the given fragment context (local name and namespace) and
 * returns the top-level nodes (detached, in document order). Gumbo's fragment
 * root is the implied <html> element; its children are the fragment's nodes. */
static rt_status parse_top_nodes(rt_dom *dom, const unsigned char *html, size_t len,
                                 const char *context, uint8_t context_ns, rt_node_vec *out) {
    GumboOptions options = {
        .tab_stop = 8,
        .stop_on_first_error = false,
        .max_attributes = RT_MAX_ATTRIBUTES,
        /* Nokogiri's gumbo.c parses fragments with one extra level: the
         * implied <html> element counts as an open element. */
        .max_tree_depth = RT_MAX_TREE_DEPTH + 1,
        .max_errors = -1,
        .fragment_context = context,
        .fragment_namespace = context_namespace(context_ns),
        .fragment_encoding = NULL,
        .quirks_mode = GUMBO_DOCTYPE_NO_QUIRKS,
        .fragment_context_has_form_ancestor = false,
        .parse_noscript_content_as_text = false,
    };
    /* Gumbo's annotation-xml integration point is neutralized first (see
     * enc_neutralize); the arena copy restores the rewritten bytes. */
    rt_buf neutral;
    rt_buf_init(&neutral);
    int neutralized = enc_neutralize(html, len, &neutral);
    if (neutralized < 0) {
        rt_buf_dispose(&neutral);
        return RT_NOMEM;
    }
    const unsigned char *source = neutralized > 0 ? neutral.data : html;
    size_t source_len = neutralized > 0 ? neutral.len : len;
    bool restore = neutralized > 0;
    /* Gumbo reads NUL-terminated input; message bodies may contain NUL bytes,
     * so copy into a buffer with a terminator. */
    char *input = malloc(source_len + 1);
    if (input == NULL) {
        rt_buf_dispose(&neutral);
        return RT_NOMEM;
    }
    if (source_len != 0) memcpy(input, source, source_len);
    input[source_len] = '\0';
    GumboOutput *output = gumbo_parse_with_options(&options, input, source_len);
    rt_status rc = RT_OK;
    if (output == NULL) {
        rc = RT_NOMEM;
    } else if (output->status != GUMBO_STATUS_OK) {
        rc = RT_PARSE;
    } else {
        const GumboNode *root = output->root;
        const GumboVector *children = root != NULL ? &root->v.element.children : NULL;
        for (unsigned i = 0; children != NULL && i < children->length; i++) {
            rt_node node = copy_gumbo_node(dom, (const GumboNode *)children->data[i], restore);
            if (node == RT_NODE_NONE) {
                rc = RT_NOMEM;
                break;
            }
            dom->nodes[node].parent = RT_NODE_NONE;
            if (rt_node_vec_push(out, node) != RT_OK) {
                rc = RT_NOMEM;
                break;
            }
        }
    }
    if (output != NULL) gumbo_destroy_output(output);
    free(input);
    rt_buf_dispose(&neutral);
    return rc;
}

rt_status rt_dom_parse_fragment(rt_dom *dom, const unsigned char *html, size_t len, rt_node *out_root) {
    *out_root = RT_NODE_NONE;
    rt_node root = dom_push(dom, RT_NODE_KIND_FRAGMENT);
    if (root == RT_NODE_NONE) return RT_NOMEM;
    rt_node_vec top;
    rt_node_vec_init(&top);
    rt_status rc = parse_top_nodes(dom, html, len, "body", RT_NS_HTML, &top);
    if (rc == RT_OK) {
        for (size_t i = 0; i < top.len && rc == RT_OK; i++) {
            rc = rt_dom_append(dom, root, top.items[i]);
        }
    }
    rt_node_vec_dispose(&top);
    if (rc != RT_OK) return rc;
    *out_root = root;
    return RT_OK;
}

/* The context Nokogiri uses when parsing markup for a node (dom.rs
 * context_for): an element's local name and namespace, else body in HTML. */
static const char *context_for(const rt_dom *dom, rt_node node, uint8_t *out_ns) {
    const char *name = rt_dom_local_name(dom, node);
    if (name == NULL) {
        *out_ns = RT_NS_HTML;
        return "body";
    }
    *out_ns = dom->nodes[node].ns;
    return name;
}

rt_status rt_dom_parse_into(rt_dom *dom, const unsigned char *html, size_t len, const char *context,
                            uint8_t context_ns, rt_node parent, rt_node_vec *out_nodes) {
    rt_node_vec top;
    rt_node_vec_init(&top);
    rt_status rc = parse_top_nodes(dom, html, len, context, context_ns, &top);
    if (rc == RT_OK && parent != RT_NODE_NONE) {
        for (size_t i = 0; i < top.len; i++) {
            rc = rt_dom_append(dom, parent, top.items[i]);
            if (rc != RT_OK) break;
        }
    }
    if (rc != RT_OK) {
        rt_node_vec_dispose(&top);
        return rc;
    }
    if (out_nodes != NULL) {
        *out_nodes = top;
    } else {
        rt_node_vec_dispose(&top);
    }
    return RT_OK;
}

rt_status rt_dom_set_inner_html(rt_dom *dom, rt_node node, const unsigned char *html, size_t len) {
    if (node >= dom->len) return RT_RAISED;
    uint8_t context_ns = RT_NS_HTML;
    const char *context = context_for(dom, node, &context_ns);
    rt_node_vec parsed;
    rt_node_vec_init(&parsed);
    rt_status rc = rt_dom_parse_into(dom, html, len, context, context_ns, RT_NODE_NONE, &parsed);
    if (rc != RT_OK) {
        rt_node_vec_dispose(&parsed);
        return rc;
    }
    rt_dom_node *n = &dom->nodes[node];
    for (size_t i = 0; i < n->children_len; i++) dom->nodes[n->children[i]].parent = RT_NODE_NONE;
    n->children_len = 0;
    for (size_t i = 0; i < parsed.len; i++) {
        rc = rt_dom_append(dom, node, parsed.items[i]);
        if (rc != RT_OK) break;
    }
    rt_node_vec_dispose(&parsed);
    return rc;
}

rt_status rt_dom_replace_with_html(rt_dom *dom, rt_node node, const unsigned char *html, size_t len) {
    if (node >= dom->len) return RT_OK;
    rt_node parent = dom->nodes[node].parent;
    if (parent == RT_NODE_NONE) return RT_OK;
    uint8_t context_ns = RT_NS_HTML;
    const char *context = context_for(dom, parent, &context_ns);
    rt_node_vec replacements;
    rt_node_vec_init(&replacements);
    rt_status rc = rt_dom_parse_into(dom, html, len, context, context_ns, RT_NODE_NONE, &replacements);
    if (rc == RT_OK) {
        for (size_t i = 0; i < replacements.len; i++) {
            rc = rt_dom_insert_before(dom, node, replacements.items[i]);
            if (rc != RT_OK) break;
        }
        if (rc == RT_OK) rt_dom_detach(dom, node);
    }
    rt_node_vec_dispose(&replacements);
    return rc;
}

rt_node rt_dom_clone_subtree(rt_dom *dst, const rt_dom *src, rt_node node) {
    if (node >= src->len) return RT_NODE_NONE;
    const rt_dom_node *source = &src->nodes[node];
    rt_node copy = dom_push(dst, source->kind);
    if (copy == RT_NODE_NONE) return RT_NODE_NONE;
    dst->nodes[copy].ns = source->ns;
    if (source->name != NULL) {
        dst->nodes[copy].name = dup_bytes((const unsigned char *)source->name, source->name_len);
        dst->nodes[copy].name_len = source->name_len;
        if (dst->nodes[copy].name == NULL) return RT_NODE_NONE;
    }
    for (size_t i = 0; i < source->attrs_len; i++) {
        if (!attrs_push(&dst->nodes[copy], source->attrs[i].name, source->attrs[i].value,
                        source->attrs[i].ns, false)) {
            return RT_NODE_NONE;
        }
    }
    for (size_t i = 0; i < source->children_len; i++) {
        rt_node child = rt_dom_clone_subtree(dst, src, source->children[i]);
        if (child == RT_NODE_NONE) return RT_NODE_NONE;
        if (!children_push(&dst->nodes[copy], child)) return RT_NODE_NONE;
        dst->nodes[child].parent = copy;
    }
    return copy;
}

/* ---- text content -------------------------------------------------------- */

static void collect_text_descendants(const rt_dom *dom, rt_node node, rt_buf *out) {
    const rt_dom_node *n = &dom->nodes[node];
    for (size_t i = 0; i < n->children_len; i++) {
        rt_node child = n->children[i];
        if (dom->nodes[child].kind == RT_NODE_KIND_TEXT) {
            rt_buf_append(out, dom->nodes[child].name, dom->nodes[child].name_len);
        }
        collect_text_descendants(dom, child, out);
    }
}

rt_status rt_dom_text_content(const rt_dom *dom, rt_node node, rt_buf *out) {
    if (node >= dom->len) return RT_OK;
    const rt_dom_node *n = &dom->nodes[node];
    if (n->kind == RT_NODE_KIND_TEXT || n->kind == RT_NODE_KIND_COMMENT) {
        return rt_buf_append(out, n->name, n->name_len);
    }
    collect_text_descendants(dom, node, out);
    return RT_OK;
}

/* ---- serialization ------------------------------------------------------- */

static bool is_void_element(const char *name) {
    static const char *const voids[] = {
        "area", "base", "basefont", "bgsound", "br", "col", "embed", "frame", "hr",
        "img", "input", "keygen", "link", "meta", "param", "source", "track", "wbr",
    };
    for (size_t i = 0; i < sizeof voids / sizeof voids[0]; i++) {
        if (strcmp(name, voids[i]) == 0) return true;
    }
    return false;
}

static bool is_raw_text_element(const char *name) {
    static const char *const raws[] = {
        "style", "script", "xmp", "iframe", "noembed", "noframes", "plaintext", "noscript",
    };
    for (size_t i = 0; i < sizeof raws / sizeof raws[0]; i++) {
        if (strcmp(name, raws[i]) == 0) return true;
    }
    return false;
}

bool rt_dom_is_void(const rt_dom *dom, rt_node node) {
    const char *name = rt_dom_local_name(dom, node);
    if (name == NULL) return false;
    return dom->nodes[node].ns == RT_NS_HTML && is_void_element(name);
}

static rt_status write_text_escaped(const unsigned char *bytes, size_t len, rt_buf *out) {
    size_t i = 0;
    while (i < len) {
        unsigned char b = bytes[i];
        if (b == '&') {
            if (rt_buf_puts(out, "&amp;") != RT_OK) return RT_NOMEM;
            i++;
        } else if (b == '<') {
            if (rt_buf_puts(out, "&lt;") != RT_OK) return RT_NOMEM;
            i++;
        } else if (b == '>') {
            if (rt_buf_puts(out, "&gt;") != RT_OK) return RT_NOMEM;
            i++;
        } else if (b == 0xC2 && i + 1 < len && bytes[i + 1] == 0xA0) {
            if (rt_buf_puts(out, "&nbsp;") != RT_OK) return RT_NOMEM;
            i += 2;
        } else {
            if (rt_buf_putc(out, b) != RT_OK) return RT_NOMEM;
            i++;
        }
    }
    return RT_OK;
}

static rt_status write_attr_escaped(const unsigned char *bytes, size_t len, bool brackets, rt_buf *out) {
    size_t i = 0;
    while (i < len) {
        unsigned char b = bytes[i];
        if (b == '&') {
            if (rt_buf_puts(out, "&amp;") != RT_OK) return RT_NOMEM;
            i++;
        } else if (b == '"') {
            if (rt_buf_puts(out, "&quot;") != RT_OK) return RT_NOMEM;
            i++;
        } else if (b == 0xC2 && i + 1 < len && bytes[i + 1] == 0xA0) {
            if (rt_buf_puts(out, "&nbsp;") != RT_OK) return RT_NOMEM;
            i += 2;
        } else if (brackets && b == '<') {
            if (rt_buf_puts(out, "&lt;") != RT_OK) return RT_NOMEM;
            i++;
        } else if (brackets && b == '>') {
            if (rt_buf_puts(out, "&gt;") != RT_OK) return RT_NOMEM;
            i++;
        } else {
            if (rt_buf_putc(out, b) != RT_OK) return RT_NOMEM;
            i++;
        }
    }
    return RT_OK;
}

static rt_status serialize_node(const rt_dom *dom, rt_node node, bool brackets, rt_buf *out);

static rt_status serialize_children(const rt_dom *dom, rt_node node, bool brackets, rt_buf *out) {
    const rt_dom_node *n = &dom->nodes[node];
    for (size_t i = 0; i < n->children_len; i++) {
        rt_status rc = serialize_node(dom, n->children[i], brackets, out);
        if (rc != RT_OK) return rc;
    }
    return RT_OK;
}

static rt_status serialize_node(const rt_dom *dom, rt_node node, bool brackets, rt_buf *out) {
    const rt_dom_node *n = &dom->nodes[node];
    switch (n->kind) {
    case RT_NODE_KIND_FRAGMENT:
    case RT_NODE_KIND_DOCUMENT:
        return serialize_children(dom, node, brackets, out);
    case RT_NODE_KIND_ELEMENT: {
        if (rt_buf_putc(out, '<') != RT_OK || rt_buf_puts(out, n->name) != RT_OK) return RT_NOMEM;
        for (size_t i = 0; i < n->attrs_len; i++) {
            if (rt_buf_putc(out, ' ') != RT_OK) return RT_NOMEM;
            rt_dom_attr_qualified_name(&n->attrs[i], out);
            if (rt_buf_puts(out, "=\"") != RT_OK) return RT_NOMEM;
            if (write_attr_escaped((const unsigned char *)n->attrs[i].value,
                                   strlen(n->attrs[i].value), brackets, out) != RT_OK) {
                return RT_NOMEM;
            }
            if (rt_buf_putc(out, '"') != RT_OK) return RT_NOMEM;
        }
        if (rt_buf_putc(out, '>') != RT_OK) return RT_NOMEM;
        if (n->ns == RT_NS_HTML && is_void_element(n->name)) return RT_OK;
        if (serialize_children(dom, node, brackets, out) != RT_OK) return RT_NOMEM;
        if (rt_buf_puts(out, "</") != RT_OK || rt_buf_puts(out, n->name) != RT_OK ||
            rt_buf_putc(out, '>') != RT_OK) {
            return RT_NOMEM;
        }
        return RT_OK;
    }
    case RT_NODE_KIND_TEXT: {
        rt_node parent = n->parent;
        bool raw = false;
        if (parent != RT_NODE_NONE && dom->nodes[parent].kind == RT_NODE_KIND_ELEMENT &&
            dom->nodes[parent].ns == RT_NS_HTML) {
            raw = is_raw_text_element(dom->nodes[parent].name);
        }
        if (raw) return rt_buf_append(out, n->name, n->name_len);
        return write_text_escaped((const unsigned char *)n->name, n->name_len, out);
    }
    case RT_NODE_KIND_COMMENT:
        if (rt_buf_puts(out, "<!--") != RT_OK) return RT_NOMEM;
        if (rt_buf_append(out, n->name, n->name_len) != RT_OK) return RT_NOMEM;
        return rt_buf_puts(out, "-->");
    case RT_NODE_KIND_DOCTYPE:
        if (rt_buf_puts(out, "<!DOCTYPE ") != RT_OK) return RT_NOMEM;
        if (rt_buf_puts(out, n->name != NULL ? n->name : "") != RT_OK) return RT_NOMEM;
        return rt_buf_putc(out, '>');
    default:
        return RT_OK;
    }
}

rt_status rt_dom_serialize(const rt_dom *dom, rt_node node, bool escaped_attribute_brackets, rt_buf *out) {
    if (node >= dom->len) return RT_OK;
    return serialize_node(dom, node, escaped_attribute_brackets, out);
}

rt_status rt_dom_inner_html(const rt_dom *dom, rt_node node, rt_buf *out) {
    if (node >= dom->len) return RT_OK;
    return serialize_children(dom, node, false, out);
}
