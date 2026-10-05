/* src/richtext/rt_content.c — ActionText::Content loading/canonicalization,
 * attachment rendering, plain text, and ContentFilters.
 *
 * Ports tmp/rust-ref/crates/richtext/src/content.rs and filters.rs. Every
 * parse/serialize round trip the Ruby steps do is done here in the same
 * context, because those round trips shape the output.
 */
#include "richtext/internal.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "yyjson.h"

#define ATTACHMENT_TAG "action-text-attachment"
#define GALLERY_PRESENTATION "gallery"
#define MENTION_CONTENT_TYPE_VALUE "application/vnd.campfire.mention"
static char *copy_owned_str(const char *text) {
    if (text == NULL) return NULL;
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (copy != NULL) memcpy(copy, text, len + 1);
    return copy;
}

/* Object#present? for an optional attribute value. */
static char *copy_present_attr(const rt_dom *dom, rt_node node, const char *name) {
    const char *value = rt_dom_attr(dom, node, name);
    if (!rt_cstr_present(value)) return NULL;
    return copy_owned_str(value);
}

static void content_init(rt_content *content) {
    rt_dom_init(&content->dom);
    content->root = RT_NODE_NONE;
}

void rt_content_dispose(rt_content *content) {
    if (content == NULL) return;
    rt_dom_dispose(&content->dom);
    content->root = RT_NODE_NONE;
}

/* ---- attachment nodes ---------------------------------------------------- */

static bool is_attachment_node(const rt_dom *dom, rt_node node) {
    const char *name = rt_dom_local_name(dom, node);
    return name != NULL && strcmp(name, ATTACHMENT_TAG) == 0;
}

rt_node_vec rt_content_attachment_nodes(const rt_dom *dom, rt_node root) {
    rt_node_vec out;
    rt_node_vec_init(&out);
    rt_node_vec all = rt_dom_descendants(dom, root);
    for (size_t i = 0; i < all.len; i++) {
        if (is_attachment_node(dom, all.items[i]) &&
            rt_node_vec_push(&out, all.items[i]) != RT_OK) {
            break;
        }
    }
    rt_node_vec_dispose(&all);
    return out;
}

static bool is_gallery_attachment(const rt_dom *dom, rt_node node) {
    if (!is_attachment_node(dom, node)) return false;
    const char *presentation = rt_dom_attr(dom, node, "presentation");
    return presentation != NULL && strcmp(presentation, GALLERY_PRESENTATION) == 0;
}

rt_node_vec rt_content_attachment_gallery_nodes(const rt_dom *dom, rt_node root) {
    rt_node_vec out;
    rt_node_vec_init(&out);
    rt_node_vec all = rt_dom_descendants(dom, root);
    for (size_t i = 0; i < all.len; i++) {
        rt_node div = all.items[i];
        const char *name = rt_dom_local_name(dom, div);
        if (name == NULL || strcmp(name, "div") != 0) continue;
        rt_node_vec descendants = rt_dom_descendants(dom, div);
        bool has_pair = false;
        for (size_t j = 0; j < descendants.len && !has_pair; j++) {
            rt_node n = descendants.items[j];
            if (!is_gallery_attachment(dom, n)) continue;
            rt_node parent = rt_dom_parent(dom, n);
            if (parent == RT_NODE_NONE) continue;
            rt_node_vec siblings = rt_dom_element_children(dom, parent);
            for (size_t k = 1; k < siblings.len; k++) {
                if (siblings.items[k] == n && is_gallery_attachment(dom, siblings.items[k - 1])) {
                    has_pair = true;
                    break;
                }
            }
            rt_node_vec_dispose(&siblings);
        }
        if (!has_pair) {
            rt_node_vec_dispose(&descendants);
            continue;
        }
        bool all_ok = true;
        size_t count = rt_dom_children_len(dom, div);
        for (size_t j = 0; j < count && all_ok; j++) {
            rt_node child = rt_dom_child(dom, div, j);
            const char *text = rt_dom_text(dom, child);
            if (text != NULL) {
                for (const char *p = text; *p != '\0'; p++) {
                    if (*p != '\n' && *p != ' ') {
                        all_ok = false;
                        break;
                    }
                }
            } else if (!is_gallery_attachment(dom, child)) {
                all_ok = false;
            }
        }
        rt_node_vec_dispose(&descendants);
        if (all_ok && rt_node_vec_push(&out, div) != RT_OK) break;
    }
    rt_node_vec_dispose(&all);
    return out;
}

/* ---- trix attachments ---------------------------------------------------- */

static const char *const TRIX_NAMES[] = {
    "sgid", "contentType", "url", "href", "filename", "filesize",
    "width", "height", "previewable", "content", "caption", "presentation",
};
static const char *const TRIX_DASHED[] = {
    "sgid", "content-type", "url", "href", "filename", "filesize",
    "width", "height", "previewable", "content", "caption", "presentation",
};

static rt_status convert_trix_attachments(rt_dom *dom, rt_node root, const rt_render_ctx *ctx) {
    rt_node_vec nodes;
    rt_node_vec_init(&nodes);
    rt_node_vec all = rt_dom_descendants(dom, root);
    for (size_t i = 0; i < all.len; i++) {
        if (rt_dom_attr(dom, all.items[i], "data-trix-attachment") != NULL) {
            if (rt_node_vec_push(&nodes, all.items[i]) != RT_OK) {
                rt_node_vec_dispose(&all);
                rt_node_vec_dispose(&nodes);
                return RT_NOMEM;
            }
        }
    }
    rt_node_vec_dispose(&all);
    rt_status rc = RT_OK;
    for (size_t i = 0; i < nodes.len && rc == RT_OK; i++) {
        rt_node node = nodes.items[i];
        /* (trix name, JSON value) attributes, in insertion order. */
        const char *keys[32];
        struct yyjson_val *values[32];
        struct yyjson_doc *node_docs[2];
        size_t node_docs_count = 0;
        size_t count = 0;
        static const char *const JSON_ATTRS[] = {"data-trix-attachment", "data-trix-attributes"};
        bool failed = false;
        for (size_t a = 0; a < 2 && !failed; a++) {
            const char *json = rt_dom_attr(dom, node, JSON_ATTRS[a]);
            if (json == NULL) continue;
            struct yyjson_doc *doc = rt_json_parse((const unsigned char *)json, strlen(json));
            struct yyjson_val *value = doc != NULL ? rt_json_doc_root(doc) : NULL;
            bool nullish = value == NULL || yyjson_is_null(value) ||
                           (yyjson_is_bool(value) && !yyjson_get_bool(value));
            if (nullish) {
                if (doc != NULL) rt_json_doc_free(doc);
                continue;
            }
            if (!yyjson_is_obj(value)) {
                if (doc != NULL) rt_json_doc_free(doc);
                rc = RT_RAISED; /* NoMethodError: merge */
                failed = true;
                break;
            }
            if (node_docs_count < 2) node_docs[node_docs_count++] = doc;
            yyjson_obj_iter iter = yyjson_obj_iter_with(value);
            yyjson_val *key;
            while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
                const char *name = yyjson_get_str(key);
                yyjson_val *item = yyjson_obj_iter_get_val(key);
                int trix = -1;
                for (size_t t = 0; t < sizeof TRIX_NAMES / sizeof TRIX_NAMES[0]; t++) {
                    if (strcmp(TRIX_NAMES[t], name) == 0) {
                        trix = (int)t;
                        break;
                    }
                }
                if (trix < 0) continue;
                bool replaced = false;
                for (size_t e = 0; e < count; e++) {
                    if (strcmp(keys[e], TRIX_NAMES[trix]) == 0) {
                        values[e] = item;
                        replaced = true;
                        break;
                    }
                }
                if (replaced) continue;
                if (count >= 32) continue;
                keys[count] = TRIX_NAMES[trix];
                values[count] = item;
                count++;
            }
            /* docs stays alive for all its values; freed after the conversion
             * of this node below. */
        }
        if (failed) {
            for (size_t e = 0; e < node_docs_count; e++) rt_json_doc_free(node_docs[e]);
            break;
        }
        const char *attr_names[32];
        char *attr_values[32];
        size_t attr_count = 0;
        for (size_t n = 0; n < rt_attachment_attributes_len; n++) {
            int trix = -1;
            for (size_t t = 0; t < sizeof TRIX_NAMES / sizeof TRIX_NAMES[0]; t++) {
                if (strcmp(TRIX_DASHED[t], rt_attachment_attributes[n]) == 0) {
                    trix = (int)t;
                    break;
                }
            }
            if (trix < 0) continue;
            for (size_t e = 0; e < count; e++) {
                if (strcmp(keys[e], TRIX_NAMES[trix]) == 0) {
                    rt_buf value;
                    rt_buf_init(&value);
                    rt_status value_rc = rt_json_value_to_s(values[e], &value);
                    char *text = NULL;
                    if (value_rc == RT_OK) value_rc = rt_buf_to_cstr(&value, &text);
                    rt_buf_dispose(&value);
                    if (value_rc != RT_OK) {
                        rc = value_rc;
                        break;
                    }
                    attr_names[attr_count] = rt_attachment_attributes[n];
                    attr_values[attr_count] = text;
                    attr_count++;
                    break;
                }
            }
            if (rc != RT_OK) break;
        }
        if (rc == RT_OK) {
            if (attr_count == 0) {
                rc = rt_dom_replace_with_html(dom, node, (const unsigned char *)"", 0);
            } else {
                const char *names[32];
                const char *texts[32];
                for (size_t e = 0; e < attr_count; e++) {
                    names[e] = attr_names[e];
                    texts[e] = attr_values[e];
                }
                rt_node element = rt_dom_create_element(dom, ATTACHMENT_TAG, names, texts, attr_count);
                if (element == RT_NODE_NONE) {
                    rc = RT_NOMEM;
                } else {
                    rt_attachment attachment;
                    rc = rt_attachment_from_node(dom, element, ctx, &attachment);
                    rt_attachment_dispose(&attachment);
                    rt_buf html;
                    rt_buf_init(&html);
                    if (rc == RT_OK) rc = rt_dom_serialize(dom, element, false, &html);
                    if (rc == RT_OK) {
                        rc = rt_dom_replace_with_html(dom, node, html.data, html.len);
                    }
                    rt_buf_dispose(&html);
                }
            }
        }
        for (size_t e = 0; e < attr_count; e++) free(attr_values[e]);
        for (size_t e = 0; e < node_docs_count; e++) rt_json_doc_free(node_docs[e]);
    }
    rt_node_vec_dispose(&nodes);
    return rc;
}

static rt_status content_load_into(rt_dom *dom, const unsigned char *html, size_t len,
                                   const rt_render_ctx *ctx, rt_node *out_root) {
    const unsigned char *stripped = html;
    size_t stripped_len = len;
    rt_ruby_strip(html, len, &stripped, &stripped_len);
    rt_status rc = rt_dom_parse_fragment(dom, stripped, stripped_len, out_root);
    if (rc != RT_OK) return rc;
    rc = convert_trix_attachments(dom, *out_root, ctx);
    if (rc != RT_OK) return rc;
    rt_node_vec nodes = rt_content_attachment_nodes(dom, *out_root);
    for (size_t i = 0; i < nodes.len && rc == RT_OK; i++) {
        rc = rt_dom_set_inner_html(dom, nodes.items[i], (const unsigned char *)"", 0);
    }
    rt_node_vec_dispose(&nodes);
    if (rc != RT_OK) return rc;
    rt_node_vec galleries = rt_content_attachment_gallery_nodes(dom, *out_root);
    for (size_t i = 0; i < galleries.len && rc == RT_OK; i++) {
        rt_buf inner;
        rt_buf_init(&inner);
        rc = rt_dom_inner_html(dom, galleries.items[i], &inner);
        if (rc == RT_OK) {
            rt_buf wrapped;
            rt_buf_init(&wrapped);
            rc = rt_buf_puts(&wrapped, "<div>");
            if (rc == RT_OK) rc = rt_buf_append(&wrapped, inner.data, inner.len);
            if (rc == RT_OK) rc = rt_buf_puts(&wrapped, "</div>");
            if (rc == RT_OK) {
                rc = rt_dom_replace_with_html(dom, galleries.items[i], wrapped.data, wrapped.len);
            }
            rt_buf_dispose(&wrapped);
        }
        rt_buf_dispose(&inner);
    }
    rt_node_vec_dispose(&galleries);
    return rc;
}

rt_status rt_content_load(const unsigned char *html, size_t len, const rt_render_ctx *ctx,
                          rt_content *out) {
    content_init(out);
    return content_load_into(&out->dom, html, len, ctx, &out->root);
}

rt_status rt_content_wrap(const unsigned char *html, size_t len, rt_content *out) {
    content_init(out);
    const unsigned char *stripped = html;
    size_t stripped_len = len;
    rt_ruby_strip(html, len, &stripped, &stripped_len);
    return rt_dom_parse_fragment(&out->dom, stripped, stripped_len, &out->root);
}

/* `Content#to_html` over the canonicalizing load: exactly what
 * `ActionText::Content.new(body, canonicalize: true).to_html()` stores
 * (content.rs `Content::load(html, ctx)?.to_html()`). The reference's
 * `unwrap_or_else(|_| body.to_string())` rescue is the public wrapper's.
 * rt_richtext.c declares this module-internal function where it is used. */
rt_status rt_content_canonical(const unsigned char *html, size_t len, const rt_render_ctx *ctx,
                               rt_buf *out) {
    rt_content content;
    rt_status rc = rt_content_load(html, len, ctx, &content);
    if (rc == RT_OK) rc = rt_dom_serialize(&content.dom, content.root, false, out);
    rt_content_dispose(&content);
    return rc;
}

/* ---- rendering ----------------------------------------------------------- */

static rt_status sanitize_content_attribute(rt_dom *dom, rt_node node) {
    char *content = rt_dom_remove_attr(dom, node, "content");
    if (content == NULL) return RT_OK;
    rt_buf sanitized;
    rt_buf_init(&sanitized);
    rt_status rc = rt_sanitize((const unsigned char *)content, strlen(content),
                               rt_safe_list_action_text(), &sanitized);
    free(content);
    if (rc != RT_OK) {
        rt_buf_dispose(&sanitized);
        return rc;
    }
    bool blank = rt_is_blank(sanitized.data, sanitized.len);
    if (!blank) {
        char *text = NULL;
        rc = rt_buf_to_cstr(&sanitized, &text);
        if (rc == RT_OK) {
            rc = rt_dom_set_attr(dom, node, "content", text);
            free(text);
        }
    }
    rt_buf_dispose(&sanitized);
    return rc;
}

/* Attachment#with_full_attributes. */
static rt_node node_with_full_attributes(rt_dom *dom, rt_node node, const rt_attachable *attachable) {
    const char *names[32];
    const char *values[32];
    size_t count = 0;
    for (size_t i = 0; i < rt_attachment_attributes_len && count < 32; i++) {
        const char *name = rt_attachment_attributes[i];
        const char *value = NULL;
        if (attachable->kind == RT_ATTACHABLE_USER && strcmp(name, "sgid") == 0) {
            const char *node_sgid = rt_dom_attr(dom, node, "sgid");
            value = node_sgid != NULL ? node_sgid : attachable->user.attachable_sgid;
        } else if (attachable->kind == RT_ATTACHABLE_USER && strcmp(name, "content-type") == 0) {
            value = MENTION_CONTENT_TYPE_VALUE;
        } else {
            value = rt_dom_attr(dom, node, name);
        }
        if (value != NULL) {
            names[count] = name;
            values[count] = value;
            count++;
        }
    }
    if (count == 0) return RT_NODE_NONE;
    return rt_dom_create_element(dom, ATTACHMENT_TAG, names, values, count);
}

/* render_attachment_html_at: depth-bounded nested content attachments. */
typedef struct {
    const rt_render_ctx *ctx;
    size_t depth;
} render_depth_ctx;

static rt_status render_nested(const rt_content *content, const rt_render_ctx *ctx, size_t depth,
                               rt_buf *out);

static rt_status render_content_callback(void *arg, const char *content, rt_buf *out) {
    render_depth_ctx *depth = arg;
    if (depth->depth >= RT_MAX_CONTENT_ATTACHMENT_DEPTH) return RT_OK; /* empty */
    rt_content nested;
    rt_status rc = rt_content_load((const unsigned char *)content, strlen(content), depth->ctx, &nested);
    if (rc == RT_OK) rc = render_nested(&nested, depth->ctx, depth->depth + 1, out);
    if (rc == RT_OK) rc = rt_buf_putc(out, '\n');
    rt_content_dispose(&nested);
    return rc;
}

static rt_status render_attachment_html_at(const rt_attachment *attachment, const rt_render_ctx *ctx,
                                           size_t depth, rt_buf *out) {
    render_depth_ctx depth_ctx = {ctx, depth};
    return rt_render_attachment(attachment, render_content_callback, &depth_ctx, out);
}

rt_status rt_render_attachment_html(const rt_attachment *attachment, const rt_render_ctx *ctx,
                                    rt_buf *out) {
    return render_attachment_html_at(attachment, ctx, 0, out);
}

static rt_status render_attachments(rt_dom *dom, rt_node root, const rt_render_ctx *ctx, size_t depth) {
    rt_node_vec nodes = rt_content_attachment_nodes(dom, root);
    rt_status rc = RT_OK;
    for (size_t i = 0; i < nodes.len && rc == RT_OK; i++) {
        rt_node node = nodes.items[i];
        rc = sanitize_content_attribute(dom, node);
        if (rc != RT_OK) break;
        rt_attachment attachment;
        rc = rt_attachment_from_node(dom, node, ctx, &attachment);
        if (rc != RT_OK) break;
        rt_node full = node_with_full_attributes(dom, node, &attachment.attachable);
        if (full == RT_NODE_NONE) {
            rt_attachment_dispose(&attachment);
            rc = RT_RAISED; /* NoMethodError: node for nil */
            break;
        }
        rt_attachment with_caption;
        memset(&with_caption, 0, sizeof with_caption);
        with_caption.attachable = attachment.attachable;
        memset(&attachment.attachable, 0, sizeof attachment.attachable);
        rt_attachment_dispose(&attachment);
        with_caption.caption = copy_present_attr(dom, full, "caption");
        rt_buf rendered;
        rt_buf_init(&rendered);
        rc = render_attachment_html_at(&with_caption, ctx, depth, &rendered);
        rt_attachment_dispose(&with_caption);
        if (rc == RT_OK) rc = rt_dom_set_inner_html(dom, full, rendered.data, rendered.len);
        rt_buf replacement;
        rt_buf_init(&replacement);
        if (rc == RT_OK) rc = rt_dom_serialize(dom, full, false, &replacement);
        if (rc == RT_OK) rc = rt_dom_replace_with_html(dom, node, replacement.data, replacement.len);
        rt_buf_dispose(&replacement);
        rt_buf_dispose(&rendered);
    }
    rt_node_vec_dispose(&nodes);
    return rc;
}

static rt_status render_attachment_galleries(rt_dom *dom, rt_node root, const rt_render_ctx *ctx,
                                             size_t depth) {
    rt_node_vec galleries = rt_content_attachment_gallery_nodes(dom, root);
    rt_status rc = RT_OK;
    for (size_t g = 0; g < galleries.len && rc == RT_OK; g++) {
        rt_node gallery = galleries.items[g];
        rt_node_vec members;
        rt_node_vec_init(&members);
        rt_node_vec all = rt_dom_descendants(dom, gallery);
        for (size_t i = 0; i < all.len; i++) {
            if (is_gallery_attachment(dom, all.items[i]) &&
                rt_node_vec_push(&members, all.items[i]) != RT_OK) {
                rc = RT_NOMEM;
                break;
            }
        }
        rt_node_vec_dispose(&all);
        rt_buf rendered;
        rt_buf_init(&rendered);
        for (size_t i = 0; i < members.len && rc == RT_OK; i++) {
            rt_node member = members.items[i];
            rt_attachment attachment;
            rc = rt_attachment_from_node(dom, member, ctx, &attachment);
            if (rc != RT_OK) break;
            rt_node full = node_with_full_attributes(dom, member, &attachment.attachable);
            if (full == RT_NODE_NONE) {
                rt_attachment_dispose(&attachment);
                rc = RT_RAISED;
                break;
            }
            rt_buf html;
            rt_buf_init(&html);
            rc = render_attachment_html_at(&attachment, ctx, depth, &html);
            rt_attachment_dispose(&attachment);
            if (rc == RT_OK) rc = rt_dom_set_inner_html(dom, full, html.data, html.len);
            if (rc == RT_OK) rc = rt_dom_serialize(dom, full, false, &rendered);
            rt_buf_dispose(&html);
        }
        if (rc == RT_OK) {
            rt_buf wrapped;
            rt_buf_init(&wrapped);
            rc = rt_buf_printf(&wrapped,
                               "<div class=\"attachment-gallery attachment-gallery--%zu\">\n  ",
                               members.len);
            if (rc == RT_OK) rc = rt_buf_append(&wrapped, rendered.data, rendered.len);
            if (rc == RT_OK) rc = rt_buf_puts(&wrapped, "\n</div>");
            if (rc == RT_OK) {
                rc = rt_dom_replace_with_html(dom, gallery, wrapped.data, wrapped.len);
            }
            rt_buf_dispose(&wrapped);
        }
        rt_buf_dispose(&rendered);
        rt_node_vec_dispose(&members);
    }
    rt_node_vec_dispose(&galleries);
    return rc;
}

static rt_status render_nested(const rt_content *content, const rt_render_ctx *ctx, size_t depth,
                               rt_buf *out) {
    rt_dom dom;
    rt_dom_init(&dom);
    rt_node root = rt_dom_clone_subtree(&dom, &content->dom, content->root);
    rt_status rc = RT_OK;
    if (root == RT_NODE_NONE) {
        rc = RT_NOMEM;
    } else {
        rc = render_attachments(&dom, root, ctx, depth);
        if (rc == RT_OK) rc = render_attachment_galleries(&dom, root, ctx, depth);
    }
    rt_buf html;
    rt_buf_init(&html);
    if (rc == RT_OK) rc = rt_dom_serialize(&dom, root, false, &html);
    if (rc == RT_OK) {
        rc = rt_sanitize(html.data, html.len, rt_safe_list_action_text(), out);
    }
    rt_buf_dispose(&html);
    rt_dom_dispose(&dom);
    return rc;
}

rt_status rt_content_render(const rt_content *content, const rt_render_ctx *ctx, rt_buf *out) {
    return render_nested(content, ctx, 0, out);
}

rt_status rt_content_to_layout_html(const rt_content *content, const rt_render_ctx *ctx, rt_buf *out) {
    rt_buf rendered;
    rt_buf_init(&rendered);
    rt_status rc = rt_content_render(content, ctx, &rendered);
    if (rc == RT_OK) {
        rc = rt_buf_puts(out, "<div class=\"lexxy-content\">\n  ");
        if (rc == RT_OK) rc = rt_buf_append(out, rendered.data, rendered.len);
        if (rc == RT_OK) rc = rt_buf_puts(out, "\n</div>\n");
    }
    rt_buf_dispose(&rendered);
    return rc;
}

/* ---- plain text ---------------------------------------------------------- */

rt_status rt_content_to_plain_text(const rt_content *content, const rt_render_ctx *ctx, rt_buf *out) {
    rt_dom dom;
    rt_dom_init(&dom);
    rt_node root = rt_dom_clone_subtree(&dom, &content->dom, content->root);
    rt_status rc = RT_OK;
    if (root == RT_NODE_NONE) rc = RT_NOMEM;
    rt_node_vec nodes;
    rt_node_vec_init(&nodes);
    if (rc == RT_OK) nodes = rt_content_attachment_nodes(&dom, root);
    for (size_t i = 0; i < nodes.len && rc == RT_OK; i++) {
        rt_node node = nodes.items[i];
        rc = sanitize_content_attribute(&dom, node);
        if (rc != RT_OK) break;
        rt_attachment attachment;
        rc = rt_attachment_from_node(&dom, node, ctx, &attachment);
        if (rc != RT_OK) break;
        rt_buf text;
        rt_buf_init(&text);
        bool is_content = false;
        rt_attachment_plain_text(&attachment, &text, &is_content);
        if (is_content) {
            rt_content nested;
            rc = rt_content_load(text.data, text.len, ctx, &nested);
            if (rc == RT_OK) {
                size_t count = rt_dom_children_len(&nested.dom, nested.root);
                rt_node *children = NULL;
                if (count != 0) {
                    children = malloc(count * sizeof *children);
                    if (children == NULL) rc = RT_NOMEM;
                }
                for (size_t c = 0; rc == RT_OK && c < count; c++) {
                    children[c] = rt_dom_child(&nested.dom, nested.root, c);
                }
                if (rc == RT_OK) {
                    /* Move the fragment's children into dom before node. */
                    for (size_t c = 0; c < count && rc == RT_OK; c++) {
                        rt_node moved = rt_dom_clone_subtree(&dom, &nested.dom, children[c]);
                        if (moved == RT_NODE_NONE) {
                            rc = RT_NOMEM;
                            break;
                        }
                        rc = rt_dom_insert_before(&dom, node, moved);
                    }
                    if (rc == RT_OK) rt_dom_detach(&dom, node);
                }
                free(children);
            }
            rt_content_dispose(&nested);
        } else {
            rc = rt_dom_replace_with_html(&dom, node, text.data, text.len);
        }
        rt_buf_dispose(&text);
        rt_attachment_dispose(&attachment);
    }
    rt_node_vec_dispose(&nodes);
    if (rc == RT_OK) rc = rt_node_to_plain_text(&dom, root, out);
    rt_dom_dispose(&dom);
    return rc;
}

/* ---- filters ------------------------------------------------------------- */

/* normalize_tweet_url */
static rt_status normalize_tweet_url(const char *url, char **out) {
    *out = NULL;
    if (url == NULL) return RT_OK;
    if (!rt_cstr_is_blank(url) &&
        (rt_span_contains((const unsigned char *)url, strlen(url), "x.com") ||
         rt_span_contains((const unsigned char *)url, strlen(url), "twitter.com"))) {
        rt_uri uri;
        rt_uri_status status = rt_uri_parse((const unsigned char *)url, strlen(url), &uri);
        if (status == RT_URI_INVALID_URI) {
            *out = copy_owned_str(url);
            return *out != NULL ? RT_OK : RT_NOMEM;
        }
        if (status == RT_URI_INVALID_COMPONENT) return RT_RAISED;
        if (uri.host != NULL && strcasecmp(uri.host, "x.com") == 0) {
            free(uri.host);
            uri.host = copy_owned_str("twitter.com");
            if (uri.host == NULL) {
                rt_uri_dispose(&uri);
                return RT_NOMEM;
            }
        }
        free(uri.query);
        uri.query = NULL;
        rt_buf text;
        rt_buf_init(&text);
        rt_status rc = rt_uri_to_s(&uri, &text);
        rt_uri_dispose(&uri);
        if (rc == RT_OK) rc = rt_buf_to_cstr(&text, out);
        rt_buf_dispose(&text);
        return rc;
    }
    *out = copy_owned_str(url);
    return *out != NULL ? RT_OK : RT_NOMEM;
}

static rt_status urls_equal_normalized(const char *a, const char *b, bool *equal) {
    char *na = NULL, *nb = NULL;
    rt_status rc = normalize_tweet_url(a, &na);
    if (rc == RT_OK) rc = normalize_tweet_url(b, &nb);
    if (rc != RT_OK) {
        free(na);
        free(nb);
        return rc;
    }
    *equal = (na == NULL && nb == NULL) || (na != NULL && nb != NULL && strcmp(na, nb) == 0);
    free(na);
    free(nb);
    return RT_OK;
}

static rt_status content_to_html(const rt_content *content, rt_buf *out) {
    return rt_dom_serialize(&content->dom, content->root, false, out);
}

static rt_status remove_solo_unfurled_link_text(rt_content *content, const rt_render_ctx *ctx) {
    rt_node_vec nodes = rt_content_attachment_nodes(&content->dom, content->root);
    rt_node_vec unfurled;
    rt_node_vec_init(&unfurled);
    for (size_t i = 0; i < nodes.len; i++) {
        const char *content_type = rt_dom_attr(&content->dom, nodes.items[i], "content-type");
        if (content_type != NULL &&
            strcmp(content_type, "application/vnd.actiontext.opengraph-embed") == 0) {
            if (rt_node_vec_push(&unfurled, nodes.items[i]) != RT_OK) {
                rt_node_vec_dispose(&unfurled);
                rt_node_vec_dispose(&nodes);
                return RT_NOMEM;
            }
        }
    }
    rt_node_vec_dispose(&nodes);
    rt_status rc = RT_OK;
    char *solo_unfurled_url = NULL;
    if (unfurled.len == 1) {
        rt_render_ctx local_ctx = *ctx;
        if (local_ctx.request_host == NULL) local_ctx.request_host = "";
        rt_attachment attachment;
        rt_status embed_rc =
            rt_attachment_from_node(&content->dom, unfurled.items[0], &local_ctx, &attachment);
        if (embed_rc != RT_OK) {
            rt_attachment_dispose(&attachment);
            rt_node_vec_dispose(&unfurled);
            return embed_rc;
        }
        if (attachment.attachable.kind == RT_ATTACHABLE_OPENGRAPH &&
            attachment.attachable.embed.href != NULL) {
            solo_unfurled_url = copy_owned_str(attachment.attachable.embed.href);
        }
        rt_attachment_dispose(&attachment);
    }
    rt_buf plain;
    rt_buf_init(&plain);
    if (rc == RT_OK) rc = rt_content_to_plain_text(content, ctx, &plain);
    char *plain_text = NULL;
    if (rc == RT_OK) rc = rt_buf_to_cstr(&plain, &plain_text);
    rt_buf_dispose(&plain);
    bool applicable = false;
    if (rc == RT_OK) {
        rc = urls_equal_normalized(solo_unfurled_url, plain_text, &applicable);
    }
    free(plain_text);
    if (rc != RT_OK || !applicable) {
        free(solo_unfurled_url);
        rt_node_vec_dispose(&unfurled);
        return rc;
    }
    /* is_trix_body: any div descendant. */
    rt_node_vec all = rt_dom_descendants(&content->dom, content->root);
    bool is_trix_body = false;
    for (size_t i = 0; i < all.len; i++) {
        const char *name = rt_dom_local_name(&content->dom, all.items[i]);
        if (name != NULL && strcmp(name, "div") == 0) {
            is_trix_body = true;
            break;
        }
    }
    if (is_trix_body) {
        rt_buf unfurl;
        rt_buf_init(&unfurl);
        rc = rt_dom_serialize(&content->dom, unfurled.items[0], false, &unfurl);
        for (size_t i = 0; i < all.len && rc == RT_OK; i++) {
            const char *name = rt_dom_local_name(&content->dom, all.items[i]);
            if (name != NULL && strcmp(name, "div") == 0) {
                rc = rt_dom_set_inner_html(&content->dom, all.items[i], unfurl.data, unfurl.len);
            }
        }
        rt_buf_dispose(&unfurl);
    } else {
        for (size_t i = 0; i < all.len && rc == RT_OK; i++) {
            rt_node p = all.items[i];
            const char *name = rt_dom_local_name(&content->dom, p);
            if (name == NULL || strcmp(name, "p") != 0) continue;
            rt_node_vec descendants = rt_dom_descendants(&content->dom, p);
            bool has_attachment = false;
            for (size_t j = 0; j < descendants.len; j++) {
                if (is_attachment_node(&content->dom, descendants.items[j])) has_attachment = true;
            }
            rt_node_vec_dispose(&descendants);
            if (!has_attachment) rt_dom_detach(&content->dom, p);
        }
    }
    rt_node_vec_dispose(&all);
    free(solo_unfurled_url);
    rt_node_vec_dispose(&unfurled);
    return rc;
}

static rt_status sanitize_tags(rt_content *content) {
    rt_node_vec all = rt_dom_descendants(&content->dom, content->root);
    for (size_t i = 0; i < all.len; i++) {
        const char *name = rt_dom_local_name(&content->dom, all.items[i]);
        if (name != NULL && !rt_sanitize_tags_allows(name)) {
            rt_dom_detach(&content->dom, all.items[i]);
        }
    }
    rt_node_vec_dispose(&all);
    return RT_OK;
}

static rt_status sanitize_attributes(rt_content *content, rt_content *out) {
    rt_buf html;
    rt_buf_init(&html);
    rt_status rc = content_to_html(content, &html);
    rt_buf sanitized;
    rt_buf_init(&sanitized);
    if (rc == RT_OK) {
        rc = rt_sanitize(html.data, html.len, rt_safe_list_content_filter(), &sanitized);
    }
    if (rc == RT_OK) rc = rt_content_wrap(sanitized.data, sanitized.len, out);
    rt_buf_dispose(&sanitized);
    rt_buf_dispose(&html);
    return rc;
}

rt_status rt_filters_apply(rt_content *content, const rt_render_ctx *ctx) {
    rt_status rc = remove_solo_unfurled_link_text(content, ctx);
    if (rc != RT_OK) return rc;
    rc = sanitize_tags(content);
    if (rc != RT_OK) return rc;
    rt_content filtered;
    content_init(&filtered);
    rc = sanitize_attributes(content, &filtered);
    if (rc != RT_OK) {
        rt_content_dispose(&filtered);
        return rc;
    }
    rt_content_dispose(content);
    *content = filtered;
    return RT_OK;
}
