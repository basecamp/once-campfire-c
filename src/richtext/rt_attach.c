/* src/richtext/rt_attach.c — resolving <action-text-attachment> nodes, the
 * attachable partials and opengraph embed validation.
 *
 * Ports tmp/rust-ref/crates/richtext/src/attachables.rs.
 */
#include "richtext/internal.h"

#include <stdlib.h>
#include <string.h>

#include "yyjson.h"

#define MENTION_CONTENT_TYPE "application/vnd.campfire.mention"
#define OPENGRAPH_EMBED_CONTENT_TYPE "application/vnd.actiontext.opengraph-embed"
#define TWITTER_AVATAR_URL_PREFIX "https://pbs.twimg.com/profile_images"

/* ---- disposals ----------------------------------------------------------- */

void rt_mention_user_dispose(rt_mention_user *user) {
    if (user == NULL) return;
    free(user->name);
    free(user->title);
    free(user->attachable_sgid);
    free(user->user_path);
    free(user->avatar_path);
    memset(user, 0, sizeof *user);
}

void rt_signed_lookup_dispose(rt_signed_lookup *lookup) {
    if (lookup == NULL) return;
    rt_mention_user_dispose(&lookup->user);
    free(lookup->model_name);
    memset(lookup, 0, sizeof *lookup);
}

void rt_gid_lookup_dispose(rt_gid_lookup *lookup) {
    if (lookup == NULL) return;
    rt_mention_user_dispose(&lookup->user);
    memset(lookup, 0, sizeof *lookup);
}

static void opengraph_embed_dispose(rt_opengraph_embed *embed) {
    free(embed->href);
    free(embed->url);
    free(embed->filename);
    free(embed->description);
    memset(embed, 0, sizeof *embed);
}

void rt_attachable_dispose(rt_attachable *attachable) {
    if (attachable == NULL) return;
    rt_mention_user_dispose(&attachable->user);
    opengraph_embed_dispose(&attachable->embed);
    free(attachable->content);
    free(attachable->url);
    free(attachable->content_type);
    free(attachable->width);
    free(attachable->height);
    free(attachable->filename);
    free(attachable->signed_model);
    memset(attachable, 0, sizeof *attachable);
}

void rt_attachment_dispose(rt_attachment *attachment) {
    if (attachment == NULL) return;
    rt_attachable_dispose(&attachment->attachable);
    free(attachment->caption);
    memset(attachment, 0, sizeof *attachment);
}

/* ---- helpers ------------------------------------------------------------- */

static char *copy_owned(const char *text) {
    if (text == NULL) return NULL;
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (copy != NULL) memcpy(copy, text, len + 1);
    return copy;
}

/* Object#present? for an optional attribute. */
static char *copy_present(const char *text) {
    if (!rt_cstr_present(text)) return NULL;
    return copy_owned(text);
}

static char *strip_cstr(const char *text) {
    if (text == NULL) return copy_owned("");
    const unsigned char *trimmed = (const unsigned char *)text;
    size_t len = 0;
    rt_ruby_strip((const unsigned char *)text, strlen(text), &trimmed, &len);
    char *copy = malloc(len + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, trimmed, len);
    copy[len] = '\0';
    return copy;
}

/* `sgid.split("--").rev().skip_while(empty).last()`: drop trailing empty
 * fields, then take the first field (which may itself be empty, as in Ruby's
 * first); no fields left means no message. */
static bool last_field(const char *text, const char **out, size_t *out_len) {
    size_t len = strlen(text);
    /* Field boundaries: [start, end) pairs in order. */
    struct {
        size_t start, end;
    } *fields = NULL;
    size_t count = 0, cap = 0;
    size_t pos = 0;
    for (;;) {
        const char *sep = strstr(text + pos, "--");
        size_t end = sep != NULL ? (size_t)(sep - text) : len;
        if (count == cap) {
            size_t new_cap = cap != 0 ? cap * 2 : 8;
            void *grown = realloc(fields, new_cap * sizeof *fields);
            if (grown == NULL) {
                free(fields);
                return false;
            }
            fields = grown;
            cap = new_cap;
        }
        fields[count].start = pos;
        fields[count].end = end;
        count++;
        if (sep == NULL) break;
        pos = end + 2;
    }
    while (count > 0 && fields[count - 1].start == fields[count - 1].end) count--;
    bool found = count > 0;
    if (found) {
        *out = text + fields[0].start;
        *out_len = fields[0].end - fields[0].start;
    }
    free(fields);
    return found;
}

/* Lossy UTF-8 conversion: invalid sequences become U+FFFD (String::from_utf8_lossy). */
static rt_status lossy_utf8(rt_buf *out, const unsigned char *bytes, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char b = bytes[i];
        size_t width = 0;
        if (b < 0x80) {
            width = 1;
        } else if ((b & 0xE0) == 0xC0) {
            width = 2;
        } else if ((b & 0xF0) == 0xE0) {
            width = 3;
        } else if ((b & 0xF8) == 0xF0) {
            width = 4;
        }
        uint32_t code = 0;
        bool valid = width != 0 && i + width <= len && rt_utf8_decode(bytes + i, width, &code) == width;
        if (valid) {
            if (rt_buf_append(out, bytes + i, width) != RT_OK) return RT_NOMEM;
            i += width;
        } else {
            if (rt_buf_put_utf8(out, 0xFFFD) != RT_OK) return RT_NOMEM;
            i++;
        }
    }
    return RT_OK;
}

static bool valid_utf8(const unsigned char *bytes, size_t len) {
    rt_buf scratch;
    rt_buf_init(&scratch);
    rt_status rc = lossy_utf8(&scratch, bytes, len);
    bool ok = rc == RT_OK && scratch.len == len;
    rt_buf_dispose(&scratch);
    return ok;
}

/* ---- opengraph embeds ---------------------------------------------------- */

static bool has_class(const rt_dom *dom, rt_node node, const char *class_name) {
    const char *value = rt_dom_attr(dom, node, "class");
    if (value == NULL) return false;
    size_t want = strlen(class_name);
    const char *at = value;
    while (*at != '\0') {
        const char *end = at;
        while (*end != '\0' && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r') end++;
        if ((size_t)(end - at) == want && memcmp(at, class_name, want) == 0) return true;
        at = *end != '\0' ? end + 1 : end;
    }
    return false;
}

static rt_status web_url_impl(const char *value, const char *request_host, char **out, bool *found) {
    *found = false;
    *out = NULL;
    if (!rt_cstr_present(value)) return RT_OK;
    rt_uri uri;
    rt_uri_status status = rt_uri_parse((const unsigned char *)value, strlen(value), &uri);
    if (status == RT_URI_INVALID_URI) return RT_OK;
    if (status == RT_URI_INVALID_COMPONENT) return RT_RAISED;
    bool http = rt_uri_is_http(&uri);
    bool elsewhere = false;
    if (http && uri.host != NULL) {
        if (!rt_cstr_is_blank(uri.host) && strchr(uri.host, '%') == NULL && strchr(uri.host, '.') != NULL) {
            size_t host_len = strlen(uri.host);
            while (host_len > 0 && uri.host[host_len - 1] == '.') host_len--;
            if (host_len == 0) {
                rt_uri_dispose(&uri);
                return RT_RAISED; /* NoMethodError: match? */
            }
            const char *label = uri.host + host_len;
            while (label > uri.host && label[-1] != '.') label--;
            bool alpha = false;
            for (const char *p = label; p < uri.host + host_len; p++) {
                if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) alpha = true;
            }
            bool hex_prefix = (uri.host + host_len) - label >= 2 &&
                              label[0] == '0' && (label[1] == 'x' || label[1] == 'X');
            if (alpha && !hex_prefix) {
                /* canonical_host = host.to_lowercase(), one trailing '.' off. */
                char host_canon[1024];
                size_t canon_len = strlen(uri.host);
                if (canon_len > sizeof host_canon - 1) canon_len = sizeof host_canon - 1;
                for (size_t i = 0; i < canon_len; i++) {
                    char c = uri.host[i];
                    host_canon[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
                }
                host_canon[canon_len] = '\0';
                if (canon_len > 0 && host_canon[canon_len - 1] == '.') host_canon[canon_len - 1] = '\0';
                char request_canon[1024];
                size_t request_len = request_host != NULL ? strlen(request_host) : 0;
                if (request_len > sizeof request_canon - 1) request_len = sizeof request_canon - 1;
                for (size_t i = 0; i < request_len; i++) {
                    char c = request_host[i];
                    request_canon[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
                }
                request_canon[request_len] = '\0';
                if (request_len > 0 && request_canon[request_len - 1] == '.') {
                    request_canon[request_len - 1] = '\0';
                }
                elsewhere = strcmp(host_canon, request_canon) != 0;
            }
        }
    }
    if (http && elsewhere) {
        *out = copy_owned(value);
        if (*out == NULL) {
            rt_uri_dispose(&uri);
            return RT_NOMEM;
        }
        *found = true;
    }
    rt_uri_dispose(&uri);
    return RT_OK;
}

rt_status rt_web_url(const unsigned char *bytes, size_t len, const char *request_host, char **out,
                     bool *found) {
    char *value = malloc(len + 1);
    if (value == NULL) return RT_NOMEM;
    memcpy(value, bytes, len);
    value[len] = '\0';
    rt_status rc = web_url_impl(value, request_host, out, found);
    free(value);
    return rc;
}

/* OPENGRAPH_CONTENT_TYPE_RE: /application\/vnd.actiontext.opengraph-embed/,
 * where `.` matches any character. */
static bool content_type_has_opengraph_embed(const char *content_type) {
    static const char first[] = "application/vnd";
    static const char second[] = "actiontext";
    static const char third[] = "opengraph-embed";
    const char *at = content_type;
    while ((at = strstr(at, first)) != NULL) {
        const char *p2 = at + sizeof first - 1; /* the '.' position */
        if (*p2 != '\0' && strncmp(p2 + 1, second, sizeof second - 1) == 0) {
            const char *p3 = p2 + 1 + sizeof second - 1;
            if (*p3 != '\0' && strncmp(p3 + 1, third, sizeof third - 1) == 0) return true;
        }
        at++;
    }
    return false;
}

static rt_status embed_from_content(const char *content, const char *host, rt_opengraph_embed *embed);

static rt_status opengraph_embed_from_node(const rt_dom *dom, rt_node node, const rt_render_ctx *ctx,
                                           bool *found, rt_opengraph_embed *embed) {
    *found = false;
    const char *content_type = rt_dom_attr(dom, node, "content-type");
    if (content_type == NULL || !content_type_has_opengraph_embed(content_type)) return RT_OK;
    const char *host = ctx->request_host != NULL ? ctx->request_host : "";
    rt_status rc;
    if (rt_cstr_present(rt_dom_attr(dom, node, "filename"))) {
        memset(embed, 0, sizeof *embed);
        bool href_found = false, url_found = false;
        const char *href = rt_dom_attr(dom, node, "href");
        const char *url = rt_dom_attr(dom, node, "url");
        rc = rt_web_url((const unsigned char *)(href != NULL ? href : ""),
                        href != NULL ? strlen(href) : 0, host, &embed->href, &href_found);
        if (rc == RT_OK) {
            rc = rt_web_url((const unsigned char *)(url != NULL ? url : ""),
                            url != NULL ? strlen(url) : 0, host, &embed->url, &url_found);
        }
        if (rc != RT_OK) {
            opengraph_embed_dispose(embed);
            return rc;
        }
        embed->filename = copy_owned(rt_dom_attr(dom, node, "filename"));
        embed->description = copy_owned(rt_dom_attr(dom, node, "caption"));
    } else {
        const char *content = rt_dom_attr(dom, node, "content");
        rc = embed_from_content(content != NULL ? content : "", host, embed);
        if (rc != RT_OK) return rc;
    }
    *found = true;
    return RT_OK;
}

static rt_status embed_from_content(const char *content, const char *host, rt_opengraph_embed *embed) {
    memset(embed, 0, sizeof *embed);
    rt_dom dom;
    rt_dom_init(&dom);
    rt_node fragment = RT_NODE_NONE;
    rt_status rc = rt_dom_parse_fragment(&dom, (const unsigned char *)content, strlen(content), &fragment);
    if (rc != RT_OK) {
        rt_dom_dispose(&dom);
        return rc;
    }
    rt_node_vec all = rt_dom_descendants(&dom, fragment);
    rt_node title = RT_NODE_NONE, description = RT_NODE_NONE, image = RT_NODE_NONE;
    for (size_t i = 0; i < all.len; i++) {
        rt_node candidate = all.items[i];
        if (title == RT_NODE_NONE && has_class(&dom, candidate, "og-embed__title")) title = candidate;
        if (description == RT_NODE_NONE && has_class(&dom, candidate, "og-embed__description")) {
            description = candidate;
        }
        if (image == RT_NODE_NONE) {
            const char *name = rt_dom_local_name(&dom, candidate);
            if (name != NULL && strcmp(name, "img") == 0) {
                rt_node_vec ancestors = rt_dom_ancestors(&dom, candidate);
                bool in_image = false;
                for (size_t j = 0; j < ancestors.len; j++) {
                    if (has_class(&dom, ancestors.items[j], "og-embed__image")) in_image = true;
                }
                rt_node_vec_dispose(&ancestors);
                if (in_image) image = candidate;
            }
        }
    }
    rt_node link = RT_NODE_NONE;
    if (title != RT_NODE_NONE) {
        rt_node_vec descendants = rt_dom_descendants(&dom, title);
        for (size_t i = 0; i < descendants.len; i++) {
            const char *name = rt_dom_local_name(&dom, descendants.items[i]);
            if (name != NULL && strcmp(name, "a") == 0) {
                link = descendants.items[i];
                break;
            }
        }
        rt_node_vec_dispose(&descendants);
    }
    bool found = false;
    if (link != RT_NODE_NONE) {
        const char *href = rt_dom_attr(&dom, link, "href");
        rc = rt_web_url((const unsigned char *)(href != NULL ? href : ""), href != NULL ? strlen(href) : 0,
                        host, &embed->href, &found);
    }
    if (rc == RT_OK) {
        const char *src = image != RT_NODE_NONE ? rt_dom_attr(&dom, image, "src") : NULL;
        rc = rt_web_url((const unsigned char *)(src != NULL ? src : ""), src != NULL ? strlen(src) : 0,
                        host, &embed->url, &found);
    }
    if (rc == RT_OK) {
        rt_node source = link != RT_NODE_NONE ? link : title;
        if (source != RT_NODE_NONE) {
            rt_buf text;
            rt_buf_init(&text);
            rc = rt_dom_text_content(&dom, source, &text);
            if (rc == RT_OK) {
                char *raw = NULL;
                if (rt_buf_to_cstr(&text, &raw) == RT_OK) {
                    embed->filename = strip_cstr(raw);
                    free(raw);
                    if (embed->filename == NULL) rc = RT_NOMEM;
                } else {
                    rc = RT_NOMEM;
                }
            }
            rt_buf_dispose(&text);
        }
    }
    if (rc == RT_OK && description != RT_NODE_NONE) {
        rt_buf text;
        rt_buf_init(&text);
        rc = rt_dom_text_content(&dom, description, &text);
        if (rc == RT_OK) {
            char *raw = NULL;
            if (rt_buf_to_cstr(&text, &raw) == RT_OK) {
                embed->description = strip_cstr(raw);
                free(raw);
                if (embed->description == NULL) rc = RT_NOMEM;
            } else {
                rc = RT_NOMEM;
            }
        }
        rt_buf_dispose(&text);
    }
    rt_node_vec_dispose(&all);
    rt_dom_dispose(&dom);
    if (rc != RT_OK) opengraph_embed_dispose(embed);
    return rc;
}

/* ---- resolution ---------------------------------------------------------- */

static bool content_type_line_matches(const char *content_type, const char *word) {
    if (content_type == NULL) return false;
    size_t word_len = strlen(word);
    const char *line = content_type;
    while (line != NULL) {
        if (strncmp(line, word, word_len) == 0) {
            const char *rest = line + word_len;
            if (*rest == '/') {
                if (rest[1] != '\0' && rest[1] != '\n') return true;
            } else if (*rest == '\0' || *rest == '\n') {
                return true;
            }
        }
        const char *newline = strchr(line, '\n');
        line = newline != NULL ? newline + 1 : NULL;
    }
    return false;
}

static void attachable_init(rt_attachable *attachable) {
    memset(attachable, 0, sizeof *attachable);
}

static rt_attachable user_attachable(rt_mention_user *user) {
    rt_attachable attachable;
    memset(&attachable, 0, sizeof attachable);
    attachable.kind = RT_ATTACHABLE_USER;
    attachable.user = *user;
    memset(user, 0, sizeof *user);
    return attachable;
}

rt_status rt_action_text_attachable_from_node(const rt_dom *dom, rt_node node,
                                              const rt_render_ctx *ctx, rt_attachable *out) {
    attachable_init(out);
    const char *sgid = rt_dom_attr(dom, node, "sgid");
    rt_signed_lookup lookup;
    memset(&lookup, 0, sizeof lookup);
    lookup.kind = RT_SIGNED_INVALID;
    if (sgid != NULL) {
        lookup = ctx->resolver->locate_signed(ctx->resolver, (const unsigned char *)sgid, strlen(sgid));
    }
    if (lookup.kind == RT_SIGNED_USER) {
        *out = user_attachable(&lookup.user);
        rt_signed_lookup_dispose(&lookup);
        return RT_OK;
    }
    const char *content_type = rt_dom_attr(dom, node, "content-type");
    const char *content = rt_dom_attr(dom, node, "content");
    if (content != NULL && content_type != NULL && strstr(content_type, "html") != NULL &&
        !rt_cstr_is_blank(content)) {
        out->kind = RT_ATTACHABLE_CONTENT;
        out->content = copy_owned(content);
        rt_signed_lookup_dispose(&lookup);
        if (out->content == NULL) {
            rt_attachable_dispose(out);
            return RT_NOMEM;
        }
        return RT_OK;
    }
    const char *url = rt_dom_attr(dom, node, "url");
    if (url != NULL) {
        if (content_type_line_matches(content_type, "image")) {
            out->kind = RT_ATTACHABLE_REMOTE_IMAGE;
            out->url = copy_owned(url);
            out->width = copy_owned(rt_dom_attr(dom, node, "width"));
            out->height = copy_owned(rt_dom_attr(dom, node, "height"));
            rt_signed_lookup_dispose(&lookup);
            if (out->url == NULL) {
                rt_attachable_dispose(out);
                return RT_NOMEM;
            }
            return RT_OK;
        }
        if (content_type_line_matches(content_type, "video")) {
            out->kind = RT_ATTACHABLE_REMOTE_VIDEO;
            out->url = copy_owned(url);
            out->content_type = copy_owned(content_type);
            out->width = copy_owned(rt_dom_attr(dom, node, "width"));
            out->height = copy_owned(rt_dom_attr(dom, node, "height"));
            out->filename = copy_owned(rt_dom_attr(dom, node, "filename"));
            rt_signed_lookup_dispose(&lookup);
            if (out->url == NULL) {
                rt_attachable_dispose(out);
                return RT_NOMEM;
            }
            return RT_OK;
        }
    }
    out->kind = RT_ATTACHABLE_MISSING;
    if (lookup.kind == RT_SIGNED_MISSING) out->signed_model = lookup.model_name;
    else free(lookup.model_name);
    return RT_OK;
}

/* MARSHALED_GID_RE: (gid://campfire/[^/]+/[0-9]+) over raw bytes. */
static bool find_marshaled_gid(const unsigned char *bytes, size_t len, rt_buf *out) {
    static const char prefix[] = "gid://campfire/";
    const size_t prefix_len = sizeof prefix - 1;
    for (size_t i = 0; i + prefix_len < len; i++) {
        if (memcmp(bytes + i, prefix, prefix_len) != 0) continue;
        size_t j = i + prefix_len;
        size_t model_start = j;
        while (j < len && bytes[j] != '/') j++;
        if (j == model_start || j >= len) continue;
        j++;
        size_t digits_start = j;
        while (j < len && bytes[j] >= '0' && bytes[j] <= '9') j++;
        if (j == digits_start) continue;
        return lossy_utf8(out, bytes + i, j - i) == RT_OK;
    }
    return false;
}

/* attachable_from_possibly_expired_sgid: reads the GID out of an SGID without
 * checking the signature; only ever a User. *found reports a user; RT_RAISED
 * propagates Ruby exceptions; RT_UNRENDERABLE is the invalid-UTF-8 log case. */
static rt_status possibly_expired_user(const char *sgid, const rt_render_ctx *ctx, bool *found,
                                       rt_mention_user *out) {
    *found = false;
    if (sgid == NULL) return RT_OK;
    const char *message = NULL;
    size_t message_len = 0;
    if (!last_field(sgid, &message, &message_len)) return RT_OK;
    rt_buf decoded;
    rt_buf_init(&decoded);
    if (!rt_base64_decode((const unsigned char *)message, message_len, &decoded)) {
        rt_buf_dispose(&decoded);
        return RT_RAISED; /* ArgumentError: invalid base64 */
    }
    bool is_valid_utf8 = valid_utf8(decoded.data, decoded.len);
    rt_buf lossy;
    rt_buf_init(&lossy);
    if (lossy_utf8(&lossy, decoded.data, decoded.len) != RT_OK) {
        rt_buf_dispose(&lossy);
        rt_buf_dispose(&decoded);
        return RT_NOMEM;
    }
    char *lossy_text = NULL;
    if (rt_buf_to_cstr(&lossy, &lossy_text) != RT_OK) {
        rt_buf_dispose(&lossy);
        rt_buf_dispose(&decoded);
        return RT_NOMEM;
    }
    rt_buf_dispose(&lossy);
    rt_buf_dispose(&decoded);
    struct yyjson_doc *doc = rt_json_parse((const unsigned char *)lossy_text, strlen(lossy_text));
    free(lossy_text);
    if (doc == NULL) {
        return is_valid_utf8 ? RT_RAISED : RT_UNRENDERABLE; /* JSON::ParserError */
    }
    struct yyjson_val *root = rt_json_doc_root(doc);
    if (!yyjson_is_obj(root)) {
        rt_json_doc_free(doc);
        return RT_RAISED; /* NoMethodError: dig */
    }
    struct yyjson_val *rails = yyjson_obj_get(root, "_rails");
    if (rails != NULL && !yyjson_is_obj(rails) && !yyjson_is_null(rails)) {
        rt_json_doc_free(doc);
        return RT_RAISED; /* TypeError: dig */
    }
    struct yyjson_val *data = rails != NULL && yyjson_is_obj(rails) ? yyjson_obj_get(rails, "data") : NULL;
    struct yyjson_val *message_value =
        rails != NULL && yyjson_is_obj(rails) ? yyjson_obj_get(rails, "message") : NULL;
    rt_status rc = RT_OK;
    char *gid = NULL;
    bool truthy_data = data != NULL && !yyjson_is_null(data) &&
                       !(yyjson_is_bool(data) && !yyjson_get_bool(data));
    bool truthy_message = message_value != NULL && !yyjson_is_null(message_value) &&
                          !(yyjson_is_bool(message_value) && !yyjson_get_bool(message_value));
    if (truthy_data) {
        if (yyjson_is_str(data)) gid = copy_owned(yyjson_get_str(data));
    } else if (truthy_message) {
        if (!yyjson_is_str(message_value)) {
            rc = RT_RAISED; /* NoMethodError: unpack1 */
        } else {
            rt_buf marshaled;
            rt_buf_init(&marshaled);
            if (!rt_base64_decode((const unsigned char *)yyjson_get_str(message_value),
                                  strlen(yyjson_get_str(message_value)), &marshaled)) {
                rt_buf_dispose(&marshaled);
                rc = RT_RAISED; /* ArgumentError: invalid base64 */
            } else {
                rt_buf gid_buf;
                rt_buf_init(&gid_buf);
                if (find_marshaled_gid(marshaled.data, marshaled.len, &gid_buf)) {
                    rc = rt_buf_to_cstr(&gid_buf, &gid);
                }
                rt_buf_dispose(&gid_buf);
                rt_buf_dispose(&marshaled);
            }
        }
    }
    rt_json_doc_free(doc);
    if (rc != RT_OK) return rc;
    if (gid == NULL) return RT_OK;
    rt_gid_lookup lookup = ctx->resolver->find_gid(ctx->resolver, (const unsigned char *)gid, strlen(gid));
    free(gid);
    switch (lookup.kind) {
    case RT_GID_USER:
        *out = lookup.user;
        *found = true;
        memset(&lookup.user, 0, sizeof lookup.user);
        rt_gid_lookup_dispose(&lookup);
        return RT_OK;
    case RT_GID_OTHER:
    case RT_GID_NOT_FOUND:
        rt_gid_lookup_dispose(&lookup);
        return RT_OK;
    default:
        rt_gid_lookup_dispose(&lookup);
        return RT_RAISED; /* GlobalID.find raised */
    }
}

rt_status rt_attachment_from_node(const rt_dom *dom, rt_node node, const rt_render_ctx *ctx,
                                  rt_attachment *out) {
    memset(out, 0, sizeof *out);
    bool embed_found = false;
    rt_status rc = opengraph_embed_from_node(dom, node, ctx, &embed_found, &out->attachable.embed);
    if (rc != RT_OK) {
        rt_attachment_dispose(out);
        return rc;
    }
    if (embed_found) {
        out->attachable.kind = RT_ATTACHABLE_OPENGRAPH;
        out->caption = copy_present(rt_dom_attr(dom, node, "caption"));
        return RT_OK;
    }
    const char *sgid = rt_dom_attr(dom, node, "sgid");
    bool user_found = false;
    rc = possibly_expired_user(sgid, ctx, &user_found, &out->attachable.user);
    if (rc != RT_OK) {
        rt_attachment_dispose(out);
        return rc;
    }
    if (user_found) {
        out->attachable.kind = RT_ATTACHABLE_USER;
        out->caption = copy_present(rt_dom_attr(dom, node, "caption"));
        return RT_OK;
    }
    rt_attachable attachable;
    rc = rt_action_text_attachable_from_node(dom, node, ctx, &attachable);
    if (rc != RT_OK) {
        rt_attachment_dispose(out);
        return rc;
    }
    out->attachable = attachable;
    out->caption = copy_present(rt_dom_attr(dom, node, "caption"));
    return RT_OK;
}

rt_status rt_attachable_content_type(const rt_attachable *attachable, const char **out) {
    *out = rt_attachable_content_type_str(attachable);
    return *out != NULL ? RT_OK : RT_RAISED;
}

const char *rt_attachable_content_type_str(const rt_attachable *attachable) {
    switch (attachable->kind) {
    case RT_ATTACHABLE_USER: return MENTION_CONTENT_TYPE;
    case RT_ATTACHABLE_OPENGRAPH: return OPENGRAPH_EMBED_CONTENT_TYPE;
    default: return NULL;
    }
}

/* ---- partials ------------------------------------------------------------ */

static const char *html_escape_cstr(const char *text, rt_buf *scratch) {
    rt_buf_clear(scratch);
    if (rt_html_escape((const unsigned char *)text, strlen(text), scratch) != RT_OK) return "";
    if (rt_buf_reserve(scratch, 1) != RT_OK) return "";
    scratch->data[scratch->len] = '\0';
    return (const char *)scratch->data;
}

static rt_status render_mention(const rt_mention_user *user, rt_buf *out) {
    rt_buf scratch;
    rt_buf_init(&scratch);
    rt_status rc = rt_buf_puts(out, "<span class=\"mention\" sgid=\"");
    if (rc == RT_OK) rc = rt_buf_puts(out, html_escape_cstr(user->attachable_sgid, &scratch));
    if (rc == RT_OK) rc = rt_buf_puts(out, "\"><a title=\"");
    if (rc == RT_OK) rc = rt_buf_puts(out, html_escape_cstr(user->title, &scratch));
    if (rc == RT_OK) rc = rt_buf_puts(out, "\" class=\"btn avatar\" data-turbo-frame=\"_top\" href=\"");
    if (rc == RT_OK) rc = rt_buf_puts(out, html_escape_cstr(user->user_path, &scratch));
    if (rc == RT_OK) rc = rt_buf_puts(out, "\"><img aria-hidden=\"true\" src=\"");
    if (rc == RT_OK) rc = rt_buf_puts(out, html_escape_cstr(user->avatar_path, &scratch));
    if (rc == RT_OK) rc = rt_buf_puts(out, "\" width=\"48\" height=\"48\" /></a> ");
    if (rc == RT_OK) rc = rt_buf_puts(out, html_escape_cstr(user->name, &scratch));
    if (rc == RT_OK) rc = rt_buf_puts(out, "</span>\n");
    rt_buf_dispose(&scratch);
    return rc;
}

/* The exact opengraph partial needs the title computed first; this wrapper
 * produces the same bytes in the order the Rust format! writes them. */
static rt_status render_opengraph_embed_full(const rt_opengraph_embed *embed, rt_buf *out) {
    rt_buf title;
    rt_buf_init(&title);
    rt_status rc = RT_OK;
    if (embed->href != NULL) {
        const char *text = embed->href;
        rt_buf truncated;
        rt_buf_init(&truncated);
        if (embed->filename != NULL) {
            rc = rt_truncate(embed->filename, 280, "…", &truncated);
            if (rc == RT_OK && rt_buf_reserve(&truncated, 1) == RT_OK) {
                truncated.data[truncated.len] = '\0';
                text = (const char *)truncated.data;
            } else if (rc == RT_OK) {
                text = "";
            }
        }
        rt_buf scratch;
        rt_buf_init(&scratch);
        if (rc == RT_OK) rc = rt_buf_puts(&title, "<a rel=\"noreferrer\" target=\"_blank\" href=\"");
        if (rc == RT_OK) rc = rt_buf_puts(&title, html_escape_cstr(embed->href, &scratch));
        if (rc == RT_OK) rc = rt_buf_puts(&title, "\">");
        if (rc == RT_OK) rc = rt_buf_puts(&title, html_escape_cstr(text, &scratch));
        if (rc == RT_OK) rc = rt_buf_puts(&title, "</a>");
        rt_buf_dispose(&scratch);
        rt_buf_dispose(&truncated);
    } else if (embed->filename != NULL) {
        rt_buf truncated;
        rt_buf_init(&truncated);
        rc = rt_truncate(embed->filename, 280, "…", &truncated);
        if (rc == RT_OK && rt_buf_reserve(&truncated, 1) == RT_OK) {
            truncated.data[truncated.len] = '\0';
            rt_buf scratch;
            rt_buf_init(&scratch);
            rc = rt_buf_puts(&title, html_escape_cstr((const char *)truncated.data, &scratch));
            rt_buf_dispose(&scratch);
        }
        rt_buf_dispose(&truncated);
    }
    if (rc == RT_OK) {
        bool twitter_avatar = embed->url != NULL &&
                              strncmp(embed->url, TWITTER_AVATAR_URL_PREFIX,
                                      strlen(TWITTER_AVATAR_URL_PREFIX)) == 0;
        rc = rt_buf_printf(out,
                           "<figure class=\"attachment attachment--content attachment--og\">\n"
                           "  <actiontext-opengraph-embed>\n"
                           "    <div class=\"og-embed gap %s\">\n"
                           "      <div class=\"og-embed__content\">\n"
                           "        <div class=\"og-embed__title\">\n"
                           "          ",
                           twitter_avatar ? "og-embed--twitter-avatar" : "");
    }
    if (rc == RT_OK) rc = rt_buf_append(out, title.data, title.len);
    if (rc == RT_OK) rc = rt_buf_puts(out, "\n        </div>\n        <div class=\"og-embed__description\">");
    if (rc == RT_OK) {
        rt_buf description;
        rt_buf_init(&description);
        rc = rt_truncate(embed->description != NULL ? embed->description : "", 560, "…", &description);
        if (rc == RT_OK) {
            rt_buf scratch;
            rt_buf_init(&scratch);
            if (rt_buf_reserve(&description, 1) == RT_OK) {
                description.data[description.len] = '\0';
                rc = rt_buf_puts(out, html_escape_cstr((const char *)description.data, &scratch));
            } else {
                rc = RT_NOMEM;
            }
            rt_buf_dispose(&scratch);
        }
        rt_buf_dispose(&description);
    }
    if (rc == RT_OK) rc = rt_buf_puts(out, "</div>\n      </div>\n");
    if (rc == RT_OK && embed->url != NULL) {
        rt_buf scratch;
        rt_buf_init(&scratch);
        rc = rt_buf_puts(out,
                         "        <div class=\"og-embed__image\">\n          <img src=\"");
        if (rc == RT_OK) rc = rt_buf_puts(out, html_escape_cstr(embed->url, &scratch));
        if (rc == RT_OK) rc = rt_buf_puts(out, "\" class=\"image center\" alt=\"\">\n        </div>\n");
        rt_buf_dispose(&scratch);
    }
    if (rc == RT_OK) rc = rt_buf_puts(out, "    </div>\n  </actiontext-opengraph-embed>\n</figure>\n");
    rt_buf_dispose(&title);
    return rc;
}

/* image_tag(url, width:, height:) */
static rt_status image_tag(const char *url, const char *width, const char *height, rt_buf *out) {
    rt_buf scratch;
    rt_buf_init(&scratch);
    const char *src;
    if (url == NULL || rt_cstr_is_blank(url)) {
        src = "";
    } else {
        bool asset_uri = false;
        /* ASSET_URI_RE: (?mi)^[-a-z]+://|^(?:cid|data):|^// */
        const char *line = url;
        while (line != NULL && !asset_uri) {
            size_t line_len = strcspn(line, "\n");
            if (line_len >= 4 && rt_ascii_istarts((const unsigned char *)line, line_len, "cid:")) {
                asset_uri = true;
            } else if (line_len >= 5 &&
                       rt_ascii_istarts((const unsigned char *)line, line_len, "data:")) {
                asset_uri = true;
            } else if (line_len >= 2 && line[0] == '/' && line[1] == '/') {
                asset_uri = true;
            } else {
                const char *p = line;
                while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '-') p++;
                if (p != line && (size_t)(p - line) + 2 < line_len && p[0] == ':' && p[1] == '/' &&
                    p[2] == '/') {
                    asset_uri = true;
                }
            }
            const char *newline = strchr(line, '\n');
            line = newline != NULL ? newline + 1 : NULL;
        }
        if (asset_uri || url[0] == '/') {
            src = url;
        } else {
            return RT_RAISED; /* Propshaft::MissingAssetError */
        }
    }
    rt_status rc = rt_buf_puts(out, "<img");
    if (rc == RT_OK && width != NULL) {
        rc = rt_buf_puts(out, " width=\"");
        if (rc == RT_OK) rc = rt_buf_puts(out, html_escape_cstr(width, &scratch));
        if (rc == RT_OK) rc = rt_buf_putc(out, '"');
    }
    if (rc == RT_OK && height != NULL) {
        rc = rt_buf_puts(out, " height=\"");
        if (rc == RT_OK) rc = rt_buf_puts(out, html_escape_cstr(height, &scratch));
        if (rc == RT_OK) rc = rt_buf_putc(out, '"');
    }
    if (rc == RT_OK) rc = rt_buf_puts(out, " src=\"");
    if (rc == RT_OK) rc = rt_buf_puts(out, html_escape_cstr(src, &scratch));
    if (rc == RT_OK) rc = rt_buf_puts(out, "\" />");
    rt_buf_dispose(&scratch);
    return rc;
}

rt_status rt_render_attachment(const rt_attachment *attachment,
                               rt_status (*render_content)(void *arg, const char *content, rt_buf *out),
                               void *arg, rt_buf *out) {
    rt_status rc = RT_OK;
    switch (attachment->attachable.kind) {
    case RT_ATTACHABLE_USER:
        rc = render_mention(&attachment->attachable.user, out);
        break;
    case RT_ATTACHABLE_OPENGRAPH:
        rc = render_opengraph_embed_full(&attachment->attachable.embed, out);
        break;
    case RT_ATTACHABLE_MISSING:
        rc = rt_buf_puts(out, "☒");
        break;
    case RT_ATTACHABLE_CONTENT:
        rc = rt_buf_puts(out, "<figure class=\"attachment attachment--content\">\n  ");
        if (rc == RT_OK) rc = render_content(arg, attachment->attachable.content, out);
        if (rc == RT_OK) rc = rt_buf_puts(out, "\n</figure>\n");
        break;
    case RT_ATTACHABLE_REMOTE_IMAGE: {
        rc = rt_buf_puts(out, "<figure class=\"attachment attachment--preview\">\n  ");
        if (rc == RT_OK) {
            rc = image_tag(attachment->attachable.url, attachment->attachable.width,
                           attachment->attachable.height, out);
        }
        if (rc == RT_OK) rc = rt_buf_putc(out, '\n');
        if (rc == RT_OK && attachment->caption != NULL) {
            rt_buf scratch;
            rt_buf_init(&scratch);
            rc = rt_buf_puts(out, "    <figcaption class=\"attachment__caption\">\n      ");
            if (rc == RT_OK) rc = rt_buf_puts(out, html_escape_cstr(attachment->caption, &scratch));
            if (rc == RT_OK) rc = rt_buf_puts(out, "\n    </figcaption>\n");
            rt_buf_dispose(&scratch);
        }
        if (rc == RT_OK) rc = rt_buf_puts(out, "</figure>\n");
        break;
    }
    case RT_ATTACHABLE_REMOTE_VIDEO: {
        rc = rt_buf_puts(out,
                         "<figure class=\"attachment attachment--preview attachment--video\">\n"
                         "  <video controls=\"controls\"");
        rt_buf scratch;
        rt_buf_init(&scratch);
        if (rc == RT_OK && attachment->attachable.width != NULL) {
            rc = rt_buf_puts(out, " width=\"");
            if (rc == RT_OK) {
                rc = rt_buf_puts(out, html_escape_cstr(attachment->attachable.width, &scratch));
            }
            if (rc == RT_OK) rc = rt_buf_putc(out, '"');
        }
        if (rc == RT_OK && attachment->attachable.height != NULL) {
            rc = rt_buf_puts(out, " height=\"");
            if (rc == RT_OK) {
                rc = rt_buf_puts(out, html_escape_cstr(attachment->attachable.height, &scratch));
            }
            if (rc == RT_OK) rc = rt_buf_putc(out, '"');
        }
        if (rc == RT_OK) rc = rt_buf_puts(out, ">\n    <source src=\"");
        if (rc == RT_OK) {
            rc = rt_buf_puts(out, html_escape_cstr(attachment->attachable.url != NULL
                                                       ? attachment->attachable.url
                                                       : "",
                                                   &scratch));
        }
        if (rc == RT_OK) rc = rt_buf_puts(out, "\" type=\"");
        if (rc == RT_OK) {
            rc = rt_buf_puts(out, html_escape_cstr(attachment->attachable.content_type != NULL
                                                       ? attachment->attachable.content_type
                                                       : "",
                                                   &scratch));
        }
        if (rc == RT_OK) rc = rt_buf_puts(out, "\">\n</video>");
        if (rc == RT_OK && attachment->caption != NULL) {
            rc = rt_buf_puts(out, "    <figcaption class=\"attachment__caption\">\n      ");
            if (rc == RT_OK) rc = rt_buf_puts(out, html_escape_cstr(attachment->caption, &scratch));
            if (rc == RT_OK) rc = rt_buf_puts(out, "\n    </figcaption>\n");
        }
        if (rc == RT_OK) rc = rt_buf_puts(out, "</figure>\n");
        rt_buf_dispose(&scratch);
        break;
    }
    }
    if (rc != RT_OK) return rc;
    /* chomp */
    const unsigned char *trimmed = NULL;
    size_t trimmed_len = 0;
    rt_chomp(out->data, out->len, &trimmed, &trimmed_len);
    out->len = trimmed_len;
    return RT_OK;
}

void rt_attachment_plain_text(const rt_attachment *attachment, rt_buf *out, bool *is_content) {
    *is_content = false;
    switch (attachment->attachable.kind) {
    case RT_ATTACHABLE_USER:
        rt_buf_putc(out, '@');
        rt_buf_puts(out, attachment->attachable.user.name != NULL ? attachment->attachable.user.name : "");
        break;
    case RT_ATTACHABLE_OPENGRAPH:
        break;
    case RT_ATTACHABLE_CONTENT:
        *is_content = true;
        rt_buf_puts(out, attachment->attachable.content != NULL ? attachment->attachable.content : "");
        break;
    case RT_ATTACHABLE_REMOTE_IMAGE:
        rt_buf_putc(out, '[');
        rt_buf_puts(out, attachment->caption != NULL ? attachment->caption : "Image");
        rt_buf_putc(out, ']');
        break;
    case RT_ATTACHABLE_REMOTE_VIDEO:
        rt_buf_putc(out, '[');
        if (attachment->caption != NULL) rt_buf_puts(out, attachment->caption);
        else if (attachment->attachable.filename != NULL) rt_buf_puts(out, attachment->attachable.filename);
        else rt_buf_puts(out, "Video");
        rt_buf_putc(out, ']');
        break;
    case RT_ATTACHABLE_MISSING:
        if (attachment->caption != NULL) rt_buf_puts(out, attachment->caption);
        break;
    }
}
