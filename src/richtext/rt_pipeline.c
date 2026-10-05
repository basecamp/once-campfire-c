/* src/richtext/rt_pipeline.c — the top-level reference pipeline
 * (lib.rs + Presenter::editable_body/body_html) over an injected resolver.
 *
 * Kept apart from the cf_ module surface so tests and probes can link the
 * pipeline without the A01/model wiring the cf_ wrappers pull in.
 */
#include "richtext/internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- context helpers ----------------------------------------------------- */

/* ---- top-level pipeline (lib.rs) ---------------------------------------- */

rt_status rt_message_presentation(rt_resolver *resolver, const char *request_host_value,
                                  const unsigned char *body, size_t len, rt_buf *out) {
    rt_render_ctx ctx = {resolver, request_host_value};
    rt_content content;
    rt_status rc = rt_content_load(body, len, &ctx, &content);
    if (rc == RT_OK) rc = rt_filters_apply(&content, &ctx);
    if (rc == RT_OK) rc = rt_content_to_layout_html(&content, &ctx, out);
    rt_content_dispose(&content);
    if (rc != RT_OK) return rc;
    rt_buf rendered;
    rt_buf_init(&rendered);
    rt_status link_rc = rt_auto_link(out->data, out->len, rt_safe_list_auto_link(), &rendered);
    if (link_rc == RT_OK) {
        rt_buf_dispose(out);
        *out = rendered;
    } else {
        rt_buf_dispose(&rendered);
    }
    return link_rc;
}

rt_status rt_present_message(rt_resolver *resolver, const char *request_host_value,
                             const unsigned char *body, size_t len, rt_presentation *kind,
                             rt_buf *out) {
    rt_buf rendered;
    rt_buf_init(&rendered);
    rt_status rc = rt_message_presentation(resolver, request_host_value, body, len, &rendered);
    if (rc == RT_OK) {
        *kind = RT_PRESENTATION_HTML;
        *out = rendered;
        return RT_OK;
    }
    rt_buf_dispose(&rendered);
    if (rc == RT_UNRENDERABLE) {
        *kind = RT_PRESENTATION_UNRENDERABLE;
        return RT_OK;
    }
    if (rc == RT_NOMEM) return RT_NOMEM;
    *kind = RT_PRESENTATION_BLANK;
    return RT_OK;
}

rt_status rt_body_html(rt_resolver *resolver, const char *request_host_value,
                       const unsigned char *body, size_t len, rt_buf *out) {
    rt_render_ctx ctx = {resolver, request_host_value};
    rt_content content;
    rt_status rc = rt_content_load(body, len, &ctx, &content);
    if (rc == RT_OK) {
        rc = rt_content_to_layout_html(&content, &ctx, out);
    }
    rt_content_dispose(&content);
    if (rc == RT_OK || rc == RT_NOMEM) return rc;
    /* Presenter::body_html unwrap_or_default: any reference error renders "". */
    rt_buf_clear(out);
    return RT_OK;
}

rt_status rt_to_plain_text(rt_resolver *resolver, const unsigned char *body, size_t len, rt_buf *out) {
    rt_render_ctx ctx = {resolver, NULL};
    rt_content content;
    rt_status rc = rt_content_load(body, len, &ctx, &content);
    if (rc == RT_OK) rc = rt_content_to_plain_text(&content, &ctx, out);
    rt_content_dispose(&content);
    return rc;
}

rt_status rt_mentioned_users(rt_resolver *resolver, const unsigned char *body, size_t len,
                             cf_int64_vector *out) {
    out->items = NULL;
    out->len = 0;
    out->cap = 0;
    rt_render_ctx ctx = {resolver, NULL};
    rt_content content;
    rt_status rc = rt_content_load(body, len, &ctx, &content);
    if (rc != RT_OK) {
        rt_content_dispose(&content);
        return rc;
    }
    rt_node_vec nodes = rt_content_attachment_nodes(&content.dom, content.root);
    for (size_t i = 0; i < nodes.len && rc == RT_OK; i++) {
        rt_attachable attachable;
        rc = rt_action_text_attachable_from_node(&content.dom, nodes.items[i], &ctx, &attachable);
        if (rc != RT_OK) break;
        if (attachable.kind == RT_ATTACHABLE_USER) {
            bool seen = false;
            for (size_t j = 0; j < out->len; j++) {
                if (out->items[j] == attachable.user.id) seen = true;
            }
            if (!seen) {
                int64_t *grown = realloc(out->items, (out->len + 1) * sizeof *grown);
                if (grown == NULL) {
                    rc = RT_NOMEM;
                } else {
                    out->items = grown;
                    out->items[out->len++] = attachable.user.id;
                    out->cap = out->len;
                }
            }
        }
        rt_attachable_dispose(&attachable);
    }
    rt_node_vec_dispose(&nodes);
    rt_content_dispose(&content);
    if (rc != RT_OK) {
        cf_int64_vector_dispose(out);
        out->items = NULL;
        out->len = 0;
        out->cap = 0;
    }
    return rc;
}

rt_status rt_editable_value(rt_resolver *resolver, const char *request_host_value,
                            const unsigned char *body, size_t len, bool *found, rt_buf *out) {
    *found = false;
    rt_render_ctx ctx = {resolver, request_host_value};

    /* editable_body: every attachment rebuilt from its attachable, on the
     * stored markup as is. */
    rt_dom dom;
    rt_dom_init(&dom);
    const unsigned char *stripped = body;
    size_t stripped_len = len;
    rt_ruby_strip(body, len, &stripped, &stripped_len);
    rt_node root = RT_NODE_NONE;
    rt_status rc = rt_dom_parse_fragment(&dom, stripped, stripped_len, &root);
    rt_node_vec nodes;
    rt_node_vec_init(&nodes);
    if (rc == RT_OK) {
        nodes = rt_content_attachment_nodes(&dom, root);
    }
    for (size_t i = 0; i < nodes.len && rc == RT_OK; i++) {
        rt_node node = nodes.items[i];
        rt_attachment attachment;
        rc = rt_attachment_from_node(&dom, node, &ctx, &attachment);
        if (rc != RT_OK) break;
        if (attachment.attachable.kind == RT_ATTACHABLE_MISSING) {
            rt_dom_detach(&dom, node);
            rt_attachment_dispose(&attachment);
            continue;
        }
        const char *content_type = rt_attachable_content_type_str(&attachment.attachable);
        if (content_type == NULL) {
            rt_attachment_dispose(&attachment);
            rc = RT_RAISED; /* NoMethodError: attachable_content_type */
            break;
        }
        rt_buf content;
        rt_buf_init(&content);
        rc = rt_render_attachment_html(&attachment, &ctx, &content);
        if (rc == RT_OK) {
            char *text = NULL;
            rc = rt_buf_to_cstr(&content, &text);
            if (rc == RT_OK) {
                rc = rt_dom_set_attr(&dom, node, "content-type", content_type);
            }
            if (rc == RT_OK) rc = rt_dom_set_attr(&dom, node, "content", text);
            free(text);
        }
        rt_buf_dispose(&content);
        rt_attachment_dispose(&attachment);
    }
    rt_buf editable;
    rt_buf_init(&editable);
    if (rc == RT_OK) rc = rt_dom_serialize(&dom, root, false, &editable);
    rt_node_vec_dispose(&nodes);
    if (rc != RT_OK) {
        rt_dom_dispose(&dom);
        rt_buf_dispose(&editable);
        return rc;
    }
    rt_dom_dispose(&dom);
    if (rt_is_blank(editable.data, editable.len)) {
        rt_buf_dispose(&editable);
        return RT_OK; /* found stays false */
    }

    /* Lexxy: attachments without a url get their rendered partial as a JSON
     * string in the content attribute. */
    rt_dom dom2;
    rt_dom_init(&dom2);
    const unsigned char *stripped2 = editable.data;
    size_t stripped2_len = editable.len;
    rt_ruby_strip(editable.data, editable.len, &stripped2, &stripped2_len);
    rt_node root2 = RT_NODE_NONE;
    rc = rt_dom_parse_fragment(&dom2, stripped2, stripped2_len, &root2);
    rt_node_vec nodes2;
    rt_node_vec_init(&nodes2);
    if (rc == RT_OK) nodes2 = rt_content_attachment_nodes(&dom2, root2);
    for (size_t i = 0; i < nodes2.len && rc == RT_OK; i++) {
        rt_node node = nodes2.items[i];
        const char *url = rt_dom_attr(&dom2, node, "url");
        if (url != NULL && !rt_cstr_is_blank(url)) continue;
        rt_attachment attachment;
        rc = rt_attachment_from_node(&dom2, node, &ctx, &attachment);
        if (rc != RT_OK) break;
        rt_buf content;
        rt_buf_init(&content);
        rc = rt_render_attachment_html(&attachment, &ctx, &content);
        rt_attachment_dispose(&attachment);
        if (rc == RT_OK) {
            rt_buf encoded;
            rt_buf_init(&encoded);
            rc = rt_json_encode_string(content.data, content.len, &encoded);
            if (rc == RT_OK) {
                char *text = NULL;
                rc = rt_buf_to_cstr(&encoded, &text);
                if (rc == RT_OK) rc = rt_dom_set_attr(&dom2, node, "content", text);
                free(text);
            }
            rt_buf_dispose(&encoded);
        }
        rt_buf_dispose(&content);
    }
    if (rc == RT_OK) rc = rt_dom_serialize(&dom2, root2, false, out);
    rt_node_vec_dispose(&nodes2);
    rt_dom_dispose(&dom2);
    rt_buf_dispose(&editable);
    if (rc == RT_OK) *found = true;
    return rc;
}

rt_status rt_without_recipient_mentions(const unsigned char *plain, size_t len, const char *name,
                                        rt_buf *out) {
    rt_buf needle;
    rt_buf_init(&needle);
    rt_status rc = rt_buf_putc(&needle, '@');
    if (rc == RT_OK) rc = rt_buf_puts(&needle, name);
    if (rc != RT_OK) {
        rt_buf_dispose(&needle);
        return rc;
    }
    rt_buf removed;
    rt_buf_init(&removed);
    size_t i = 0;
    while (i < len) {
        if (i + needle.len <= len && memcmp(plain + i, needle.data, needle.len) == 0) {
            i += needle.len;
            continue;
        }
        if (rt_buf_putc(&removed, plain[i]) != RT_OK) {
            rc = RT_NOMEM;
            break;
        }
        i++;
    }
    rt_buf_dispose(&needle);
    if (rc == RT_OK) {
        const unsigned char *trimmed = NULL;
        size_t trimmed_len = 0;
        rt_trim_whitespace(removed.data, removed.len, &trimmed, &trimmed_len);
        rc = rt_buf_append(out, trimmed, trimmed_len);
    }
    rt_buf_dispose(&removed);
    return rc;
}

