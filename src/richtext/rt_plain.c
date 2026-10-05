/* src/richtext/rt_plain.c — ActionText::PlainTextConversion.
 *
 * Ports tmp/rust-ref/crates/richtext/src/plain_text.rs: a bottom-up reduction
 * keyed on each node's Nokogiri name.
 */
#include "richtext/internal.h"

#include <stdlib.h>
#include <string.h>

static rt_status plain_text_for(const rt_dom *dom, rt_node node, rt_buf *out);

static rt_status child_values(const rt_dom *dom, rt_node node, rt_buf *out) {
    size_t count = rt_dom_children_len(dom, node);
    for (size_t i = 0; i < count; i++) {
        rt_status rc = plain_text_for(dom, rt_dom_child(dom, node, i), out);
        if (rc != RT_OK) return rc;
    }
    return RT_OK;
}

/* chomp_newlines(child_values.concat()) */
static rt_status child_values_chomped(const rt_dom *dom, rt_node node, rt_buf *out) {
    rt_buf raw;
    rt_buf_init(&raw);
    rt_status rc = child_values(dom, node, &raw);
    if (rc == RT_OK) {
        const unsigned char *trimmed = NULL;
        size_t trimmed_len = 0;
        rt_chomp_newlines(raw.data, raw.len, &trimmed, &trimmed_len);
        rc = rt_buf_append(out, trimmed, trimmed_len);
    }
    rt_buf_dispose(&raw);
    return rc;
}

static rt_status plain_text_for_block(const rt_dom *dom, rt_node node, rt_buf *out) {
    rt_status rc = child_values_chomped(dom, node, out);
    if (rc == RT_OK) rc = rt_buf_puts(out, "\n\n");
    return rc;
}

static bool is_list_name(const char *name) {
    return name != NULL && (strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0);
}

static size_t list_depth(const rt_dom *dom, rt_node node) {
    size_t depth = 0;
    rt_node_vec ancestors = rt_dom_ancestors(dom, node);
    for (size_t i = 0; i < ancestors.len; i++) {
        if (is_list_name(rt_dom_name(dom, ancestors.items[i]))) depth++;
    }
    rt_node_vec_dispose(&ancestors);
    return depth;
}

static rt_status bullet_for_li(const rt_dom *dom, rt_node node, rt_buf *out) {
    rt_node_vec ancestors = rt_dom_ancestors(dom, node);
    const char *list_name = NULL;
    for (size_t i = 0; i < ancestors.len; i++) {
        const char *name = rt_dom_name(dom, ancestors.items[i]);
        if (is_list_name(name)) {
            list_name = name;
            break;
        }
    }
    rt_status rc = RT_OK;
    if (list_name != NULL && strcmp(list_name, "ol") == 0) {
        size_t index = 0;
        rt_node parent = rt_dom_parent(dom, node);
        if (parent != RT_NODE_NONE) {
            rt_node_vec children = rt_dom_element_children(dom, parent);
            for (size_t i = 0; i < children.len; i++) {
                if (children.items[i] == node) {
                    index = i;
                    break;
                }
            }
            rt_node_vec_dispose(&children);
        }
        rc = rt_buf_printf(out, "%zu.", index + 1);
    } else {
        rc = rt_buf_puts(out, "•");
    }
    rt_node_vec_dispose(&ancestors);
    return rc;
}

/* is_space for the blockquote quote insertion: Ruby's /\S/ port in plain_text.rs
 * counts only these ASCII whitespace characters. */
static bool quote_space(uint32_t code) {
    return code == ' ' || code == '\t' || code == '\n' || code == 0x0B || code == 0x0C || code == '\r';
}

static rt_status plain_text_for(const rt_dom *dom, rt_node node, rt_buf *out) {
    const char *name = rt_dom_name(dom, node);
    if (name == NULL) return RT_OK;
    if (strcmp(name, "script") == 0 || strcmp(name, "style") == 0 ||
        strcmp(name, "unsupported") == 0) {
        return RT_OK;
    }
    if (strcmp(name, "h1") == 0 || strcmp(name, "p") == 0) {
        return plain_text_for_block(dom, node, out);
    }
    if (strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0) {
        rt_buf text;
        rt_buf_init(&text);
        rt_status rc = plain_text_for_block(dom, node, &text);
        if (rc == RT_OK && list_depth(dom, node) > 0) rc = rt_buf_putc(out, '\n');
        if (rc == RT_OK) rc = rt_buf_append(out, text.data, text.len);
        rt_buf_dispose(&text);
        return rc;
    }
    if (strcmp(name, "br") == 0) return rt_buf_putc(out, '\n');
    if (strcmp(name, "text") == 0) {
        rt_buf text;
        rt_buf_init(&text);
        rt_status rc = rt_dom_text_content(dom, node, &text);
        if (rc == RT_OK) {
            const unsigned char *trimmed = NULL;
            size_t trimmed_len = 0;
            rt_chomp_newlines(text.data, text.len, &trimmed, &trimmed_len);
            rc = rt_buf_append(out, trimmed, trimmed_len);
        }
        rt_buf_dispose(&text);
        return rc;
    }
    if (strcmp(name, "div") == 0) {
        rt_status rc = child_values_chomped(dom, node, out);
        if (rc == RT_OK) rc = rt_buf_putc(out, '\n');
        return rc;
    }
    if (strcmp(name, "figcaption") == 0) {
        rt_status rc = rt_buf_putc(out, '[');
        if (rc == RT_OK) rc = child_values_chomped(dom, node, out);
        if (rc == RT_OK) rc = rt_buf_putc(out, ']');
        return rc;
    }
    if (strcmp(name, "blockquote") == 0) {
        rt_buf text;
        rt_buf_init(&text);
        rt_status rc = plain_text_for_block(dom, node, &text);
        if (rc != RT_OK) {
            rt_buf_dispose(&text);
            return rc;
        }
        if (rt_is_blank(text.data, text.len)) {
            rt_buf_dispose(&text);
            return rt_buf_puts(out, "“”");
        }
        size_t last_end = 0;
        for (size_t i = 0; i < text.len;) {
            uint32_t code;
            size_t width = rt_utf8_decode(text.data + i, text.len - i, &code);
            if (width == 0) width = 1;
            if (!quote_space(code)) last_end = i + width;
            i += width;
        }
        size_t first_start = 0;
        for (size_t i = 0; i < text.len;) {
            uint32_t code;
            size_t width = rt_utf8_decode(text.data + i, text.len - i, &code);
            if (width == 0) width = 1;
            if (!quote_space(code)) {
                first_start = i;
                break;
            }
            i += width;
        }
        /* Insert ” after the last non-space, then “ before the first, exactly
         * as the two String#insert calls do. Build in a local buffer so the
         * insertion point is known. */
        rt_buf quoted;
        rt_buf_init(&quoted);
        rc = rt_buf_append(&quoted, text.data, last_end);
        if (rc == RT_OK) rc = rt_buf_puts(&quoted, "”");
        if (rc == RT_OK) rc = rt_buf_append(&quoted, text.data + last_end, text.len - last_end);
        if (rc == RT_OK) {
            if (first_start <= quoted.len) {
                rt_buf final_buf;
                rt_buf_init(&final_buf);
                rc = rt_buf_append(&final_buf, quoted.data, first_start);
                if (rc == RT_OK) rc = rt_buf_puts(&final_buf, "“");
                if (rc == RT_OK) {
                    rc = rt_buf_append(&final_buf, quoted.data + first_start, quoted.len - first_start);
                }
                if (rc == RT_OK) rc = rt_buf_append(out, final_buf.data, final_buf.len);
                rt_buf_dispose(&final_buf);
            } else {
                rc = rt_buf_append(out, quoted.data, quoted.len);
            }
        }
        rt_buf_dispose(&quoted);
        rt_buf_dispose(&text);
        return rc;
    }
    if (strcmp(name, "li") == 0) {
        size_t depth = list_depth(dom, node);
        for (size_t i = 0; i + 1 < depth; i++) {
            rt_status rc = rt_buf_puts(out, "  ");
            if (rc != RT_OK) return rc;
        }
        rt_status rc = bullet_for_li(dom, node, out);
        if (rc == RT_OK) rc = rt_buf_putc(out, ' ');
        if (rc == RT_OK) rc = child_values_chomped(dom, node, out);
        if (rc == RT_OK) rc = rt_buf_putc(out, '\n');
        return rc;
    }
    return child_values(dom, node, out);
}

rt_status rt_node_to_plain_text(const rt_dom *dom, rt_node node, rt_buf *out) {
    rt_buf raw;
    rt_buf_init(&raw);
    rt_status rc = plain_text_for(dom, node, &raw);
    if (rc == RT_OK) {
        const unsigned char *trimmed = NULL;
        size_t trimmed_len = 0;
        rt_chomp_newlines(raw.data, raw.len, &trimmed, &trimmed_len);
        rc = rt_buf_append(out, trimmed, trimmed_len);
    }
    rt_buf_dispose(&raw);
    return rc;
}
