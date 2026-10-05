/* src/richtext/internal.h — private surface of the R02 rich-text module.
 *
 * Everything here is module-private by default: tests under tests/richtext may
 * include it (they link the module sources directly), application code outside
 * src/richtext must use src/richtext.h. Symbols use the rt_ prefix so the
 * cf_ application namespace stays reserved for cross-module contracts.
 *
 * The translation target is the pinned Rust pipeline
 * tmp/rust-ref/crates/richtext/src/{dom,sanitizer,filters,autolink,
 * plain_text,attachables,content,uri,ruby}.rs; comments name the exact Rust
 * function each C function ports.
 */
#ifndef CF_RICHTEXT_INTERNAL_H
#define CF_RICHTEXT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cf.h"
#include "models/types.h"

/* ---- status ------------------------------------------------------------- */

/* Rust `Error` maps to these kinds; RT_RAISED is any Ruby exception the
 * pipeline rescues (parse limits are RT_PARSE, like dom::ParseError).
 * RT_UNRENDERABLE is `Error::Unrenderable`, the one case where the reference
 * `message_presentation` rescue itself raises (invalid UTF-8 in a JSON parse
 * error message). */
typedef enum {
    RT_OK = 0,
    RT_NOMEM,
    RT_PARSE,
    RT_RAISED,
    RT_UNRENDERABLE
} rt_status;

/* ---- byte buffer -------------------------------------------------------- */

typedef struct {
    unsigned char *data;
    size_t len, cap;
} rt_buf;

void rt_buf_init(rt_buf *buf);
void rt_buf_dispose(rt_buf *buf);
void rt_buf_clear(rt_buf *buf);
rt_status rt_buf_reserve(rt_buf *buf, size_t extra);
rt_status rt_buf_append(rt_buf *buf, const void *bytes, size_t len);
rt_status rt_buf_puts(rt_buf *buf, const char *text);
rt_status rt_buf_putc(rt_buf *buf, unsigned char byte);
/* Appends one already-decoded code point as UTF-8. */
rt_status rt_buf_put_utf8(rt_buf *buf, uint32_t code);
/* printf-style append; returns RT_NOMEM on allocation failure. */
rt_status rt_buf_printf(rt_buf *buf, const char *format, ...);
cf_span rt_buf_span(const rt_buf *buf);
/* Detaches the bytes as an owned NUL-terminated cf_str (rt_buf is emptied). */
rt_status rt_buf_to_str(rt_buf *buf, cf_str *out);
/* Detaches as an owned NUL-terminated malloc string (rt_buf is emptied). */
rt_status rt_buf_to_cstr(rt_buf *buf, char **out);

/* ---- node vectors ------------------------------------------------------- */

typedef uint32_t rt_node;
#define RT_NODE_NONE UINT32_C(0xFFFFFFFF)

typedef struct {
    rt_node *items;
    size_t len, cap;
} rt_node_vec;

void rt_node_vec_init(rt_node_vec *vec);
void rt_node_vec_dispose(rt_node_vec *vec);
rt_status rt_node_vec_push(rt_node_vec *vec, rt_node node);
bool rt_node_vec_contains(const rt_node_vec *vec, rt_node node);

/* ---- UTF-8 and Ruby/Active Support string behavior ---------------------- */

/* Decodes one UTF-8 sequence; returns its width (1..4) or 0 on malformed
 * input. Like Ruby's UTF-8 reader it maps an invalid byte to one U+FFFD. */
size_t rt_utf8_decode(const unsigned char *bytes, size_t len, uint32_t *out);
size_t rt_utf8_encode(uint32_t code, unsigned char out[4]);
/* Rust char::is_whitespace (Unicode White_Space). */
bool rt_is_whitespace(uint32_t code);
/* ActiveSupport String#blank? over UTF-8 bytes. */
bool rt_is_blank(const unsigned char *bytes, size_t len);
bool rt_cstr_is_blank(const char *text);
/* Object#present? for an optional read: NULL or blank -> false. */
bool rt_cstr_present(const char *text);
/* Ruby String#strip (NUL and ASCII whitespace). */
void rt_ruby_strip(const unsigned char *bytes, size_t len, const unsigned char **out_ptr,
                   size_t *out_len);
/* String#chomp(""): strips every trailing \n or \r\n, but not a lone \r. */
void rt_chomp_newlines(const unsigned char *bytes, size_t len, const unsigned char **out_ptr,
                       size_t *out_len);
/* String#chomp with no argument: one trailing \r\n, \n or \r. */
void rt_chomp(const unsigned char *bytes, size_t len, const unsigned char **out_ptr, size_t *out_len);
/* Active View truncate(text, length:, omission:). */
rt_status rt_truncate(const char *text, size_t length, const char *omission, rt_buf *out);
/* Ruby's whitespace trim (trim_matches(char::is_whitespace)). */
void rt_trim_whitespace(const unsigned char *bytes, size_t len, const unsigned char **out_ptr,
                        size_t *out_len);
bool rt_ascii_ieq(const unsigned char *bytes, size_t len, const char *literal);
bool rt_ascii_istarts(const unsigned char *bytes, size_t len, const char *prefix);
bool rt_span_contains(const unsigned char *bytes, size_t len, const char *needle);

/* ERB::Util.html_escape. */
rt_status rt_html_escape(const unsigned char *bytes, size_t len, rt_buf *out);
/* CGI.escape/ERB::Util.url_encode: everything but [A-Za-z0-9_.-~]. */
rt_status rt_url_encode(const unsigned char *bytes, size_t len, rt_buf *out);

/* ---- base64 (Ruby semantics) -------------------------------------------- */

/* Base64.strict_decode64 then Base64.urlsafe_decode64; false on invalid
 * input, like the reference's ArgumentError path. */
bool rt_base64_decode(const unsigned char *bytes, size_t len, rt_buf *out);

/* ---- Ruby JSON / Float#to_s --------------------------------------------- */

/* Ruby JSON.parse (comments stripped), returning a yyjson document or NULL.
 * The doc must be kept alive for its value pointers and freed with
 * rt_json_doc_free. */
struct yyjson_doc;
struct yyjson_val;
struct yyjson_doc *rt_json_parse(const unsigned char *bytes, size_t len);
struct yyjson_val *rt_json_doc_root(struct yyjson_doc *doc);
void rt_json_doc_free(struct yyjson_doc *doc);
/* Ruby `Object#to_s` of a parsed JSON value (Nokogiri attribute coercion). */
rt_status rt_json_value_to_s(const struct yyjson_val *value, rt_buf *out);
/* Ruby `Object#inspect` for parsed JSON values. */
rt_status rt_json_value_inspect(const struct yyjson_val *value, rt_buf *out);
/* ActiveSupport::JSON.encode of a UTF-8 string (JSON escapes plus < etc.). */
rt_status rt_json_encode_string(const unsigned char *bytes, size_t len, rt_buf *out);
/* Ruby Float#to_s (the pinned ruby_compat::float_to_s). */
rt_status rt_float_to_s(double value, rt_buf *out);

/* ---- DOM (dom.rs) -------------------------------------------------------- */

enum {
    RT_NODE_KIND_FRAGMENT = 0,
    RT_NODE_KIND_DOCUMENT,
    RT_NODE_KIND_ELEMENT,
    RT_NODE_KIND_TEXT,
    RT_NODE_KIND_COMMENT,
    RT_NODE_KIND_DOCTYPE
};

enum { RT_NS_HTML = 0, RT_NS_SVG, RT_NS_MATHML };

enum { RT_ATTR_NS_NONE = 0, RT_ATTR_NS_XLINK, RT_ATTR_NS_XML, RT_ATTR_NS_XMLNS };

typedef struct {
    char *name;  /* local name */
    char *value; /* NUL-terminated; may contain any bytes but a NUL */
    uint8_t ns;  /* RT_ATTR_NS_* */
} rt_attr;

typedef struct rt_dom_node rt_dom_node;
struct rt_dom_node {
    uint8_t kind;
    uint8_t ns;
    char *name; /* element local name / text bytes / comment bytes / doctype name */
    size_t name_len;
    rt_attr *attrs;
    size_t attrs_len, attrs_cap;
    rt_node parent;
    rt_node *children;
    size_t children_len, children_cap;
};

typedef struct {
    rt_dom_node *nodes;
    size_t len, cap;
} rt_dom;

void rt_dom_init(rt_dom *dom);
void rt_dom_dispose(rt_dom *dom);

/* Nokogiri::HTML5::Document#fragment(html), in a body context. */
rt_status rt_dom_parse_fragment(rt_dom *dom, const unsigned char *html, size_t len, rt_node *out_root);
/* Nokogiri `Node#fragment`/`inner_html=`: parses in the context of an element
 * (its local name and RT_NS_* namespace; body/RT_NS_HTML for anything else)
 * and appends the top-level nodes under parent (RT_NODE_NONE for a detached
 * list). */
rt_status rt_dom_parse_into(rt_dom *dom, const unsigned char *html, size_t len, const char *context,
                            uint8_t context_ns, rt_node parent, rt_node_vec *out_nodes);

const char *rt_dom_name(const rt_dom *dom, rt_node node);       /* Nokogiri Node#name */
const char *rt_dom_local_name(const rt_dom *dom, rt_node node); /* elements only */
bool rt_dom_is_element(const rt_dom *dom, rt_node node);
bool rt_dom_is_text(const rt_dom *dom, rt_node node);
bool rt_dom_is_html_element(const rt_dom *dom, rt_node node);
rt_node rt_dom_parent(const rt_dom *dom, rt_node node);
size_t rt_dom_children_len(const rt_dom *dom, rt_node node);
rt_node rt_dom_child(const rt_dom *dom, rt_node node, size_t index);
rt_node_vec rt_dom_descendants(const rt_dom *dom, rt_node node);
rt_node_vec rt_dom_ancestors(const rt_dom *dom, rt_node node);
rt_node_vec rt_dom_element_children(const rt_dom *dom, rt_node node);
const char *rt_dom_text(const rt_dom *dom, rt_node node); /* text payload or NULL */

/* Attribute access by Nokogiri qualified name ("xlink:href", "href"). */
const char *rt_dom_attr(const rt_dom *dom, rt_node node, const char *name);
bool rt_dom_has_attr(const rt_dom *dom, rt_node node, const char *name);
rt_status rt_dom_set_attr(rt_dom *dom, rt_node node, const char *name, const char *value);
/* Removes the attribute and returns its value as an owned string, or NULL. */
char *rt_dom_remove_attr(rt_dom *dom, rt_node node, const char *name);
/* Qualified name as Nokogiri reports it; writes into buf. */
void rt_dom_attr_qualified_name(const rt_attr *attr, rt_buf *buf);

rt_node rt_dom_create_element(rt_dom *dom, const char *name, const char *const *names,
                              const char *const *values, size_t count);
rt_node rt_dom_create_text(rt_dom *dom, const unsigned char *text, size_t len);
void rt_dom_detach(rt_dom *dom, rt_node node);
rt_status rt_dom_append(rt_dom *dom, rt_node parent, rt_node child);
rt_status rt_dom_insert_before(rt_dom *dom, rt_node reference, rt_node new_node);
void rt_dom_replace_with_nodes(rt_dom *dom, rt_node node, const rt_node *nodes, size_t count);
rt_status rt_dom_set_inner_html(rt_dom *dom, rt_node node, const unsigned char *html, size_t len);
rt_status rt_dom_replace_with_html(rt_dom *dom, rt_node node, const unsigned char *html, size_t len);
/* Deep-copies src's subtree into dom. */
rt_node rt_dom_clone_subtree(rt_dom *dom, const rt_dom *src, rt_node node);
/* Nokogiri html_standard_serialize; escapes < and > inside attribute values
 * when escaped_attribute_brackets (the auto_link-safety variant). */
rt_status rt_dom_serialize(const rt_dom *dom, rt_node node, bool escaped_attribute_brackets, rt_buf *out);
rt_status rt_dom_inner_html(const rt_dom *dom, rt_node node, rt_buf *out);
rt_status rt_dom_text_content(const rt_dom *dom, rt_node node, rt_buf *out);
/* True for HTML namespace void elements (Nokogiri serializes them without an
 * end tag and without children). */
bool rt_dom_is_void(const rt_dom *dom, rt_node node);

/* ---- sanitizer (sanitizer.rs) ------------------------------------------- */

typedef struct {
    const char *const *tags;
    size_t tags_len;
    const char *const *attributes;
    size_t attributes_len;
} rt_safe_list;

const rt_safe_list *rt_safe_list_defaults(void);
const rt_safe_list *rt_safe_list_action_text(void);
const rt_safe_list *rt_safe_list_content_filter(void);
const rt_safe_list *rt_safe_list_auto_link(void);
bool rt_safe_list_allows_tag(const rt_safe_list *list, const char *name);
bool rt_safe_list_allows_attr(const rt_safe_list *list, const char *name);
/* ContentFilters::SanitizeTags::ALLOWED_TAGS. */
bool rt_sanitize_tags_allows(const char *name);

rt_status rt_sanitize(const unsigned char *html, size_t len, const rt_safe_list *list, rt_buf *out);
/* ActionText::Attachment::ATTRIBUTES, in order (sanitizer.rs). */
extern const char *const rt_attachment_attributes[];
extern const size_t rt_attachment_attributes_len;
rt_status rt_sanitize_escaped_attributes(const unsigned char *html, size_t len, const rt_safe_list *list,
                                         rt_buf *out);
/* Loofah's URI protocol check. */
bool rt_allowed_uri(const unsigned char *bytes, size_t len);
/* CGI.unescapeHTML. */
rt_status rt_cgi_unescape_html(const unsigned char *bytes, size_t len, rt_buf *out);

/* ---- URI (uri.rs) -------------------------------------------------------- */

typedef enum { RT_URI_OK = 0, RT_URI_INVALID_URI, RT_URI_INVALID_COMPONENT } rt_uri_status;

typedef struct {
    char *scheme;  /* owned, lowercase, or NULL */
    char *userinfo;/* owned or NULL */
    char *host;    /* owned or NULL */
    bool has_port;
    uint64_t port;
    char *path;    /* owned or NULL */
    char *opaque;  /* owned or NULL */
    char *query;   /* owned or NULL */
    char *fragment;/* owned or NULL */
} rt_uri;

void rt_uri_dispose(rt_uri *uri);
rt_uri_status rt_uri_parse(const unsigned char *bytes, size_t len, rt_uri *out);
rt_status rt_uri_to_s(const rt_uri *uri, rt_buf *out);
bool rt_uri_is_http(const rt_uri *uri);

/* ---- autolink (autolink.rs) --------------------------------------------- */

rt_status rt_auto_link(const unsigned char *html, size_t len, const rt_safe_list *list, rt_buf *out);

/* ---- plain text (plain_text.rs) ------------------------------------------ */

rt_status rt_node_to_plain_text(const rt_dom *dom, rt_node node, rt_buf *out);

/* ---- attachables (attachables.rs) --------------------------------------- */

typedef struct {
    int64_t id;
    char *name;
    char *title;
    char *attachable_sgid;
    char *user_path;
    char *avatar_path;
} rt_mention_user;

void rt_mention_user_dispose(rt_mention_user *user);

typedef enum { RT_SIGNED_USER = 0, RT_SIGNED_MISSING, RT_SIGNED_INVALID } rt_signed_kind;
typedef struct {
    rt_signed_kind kind;
    rt_mention_user user; /* RT_SIGNED_USER */
    char *model_name;     /* RT_SIGNED_MISSING, owned */
} rt_signed_lookup;

typedef enum { RT_GID_USER = 0, RT_GID_OTHER, RT_GID_NOT_FOUND, RT_GID_RAISES } rt_gid_kind;
typedef struct {
    rt_gid_kind kind;
    rt_mention_user user;
} rt_gid_lookup;

void rt_signed_lookup_dispose(rt_signed_lookup *lookup);
void rt_gid_lookup_dispose(rt_gid_lookup *lookup);

/* The attachables.rs trait; the database implementation lives in
 * rt_resolver.c, tests provide their own. */
typedef struct rt_resolver rt_resolver;
struct rt_resolver {
    rt_signed_lookup (*locate_signed)(rt_resolver *self, const unsigned char *sgid, size_t len);
    rt_gid_lookup (*find_gid)(rt_resolver *self, const unsigned char *gid, size_t len);
};

/* The production resolver: GlobalID lookups through A01's verifier and the
 * D01 user model, over the caller's borrowed connection. */
typedef struct {
    rt_resolver base;
    cf_db *db;
    cf_span secret_key_base;
    int64_t now_us;
} rt_db_resolver;

rt_resolver *rt_db_resolver_init(rt_db_resolver *storage, cf_db *db, cf_span secret_key_base,
                                 int64_t now_us);

typedef struct {
    rt_resolver *resolver;
    const char *request_host; /* NUL-terminated or NULL */
} rt_render_ctx;

typedef enum {
    RT_ATTACHABLE_USER = 0,
    RT_ATTACHABLE_OPENGRAPH,
    RT_ATTACHABLE_CONTENT,
    RT_ATTACHABLE_REMOTE_IMAGE,
    RT_ATTACHABLE_REMOTE_VIDEO,
    RT_ATTACHABLE_MISSING
} rt_attachable_kind;

typedef struct {
    char *href, *url, *filename, *description; /* owned or NULL */
} rt_opengraph_embed;

typedef struct {
    rt_attachable_kind kind;
    rt_mention_user user;        /* USER */
    rt_opengraph_embed embed;    /* OPENGRAPH */
    char *content;               /* CONTENT */
    char *url;                   /* REMOTE_IMAGE / REMOTE_VIDEO */
    char *content_type;          /* REMOTE_VIDEO */
    char *width;                 /* REMOTE_IMAGE / REMOTE_VIDEO (owned or NULL) */
    char *height;                /* REMOTE_IMAGE / REMOTE_VIDEO (owned or NULL) */
    char *filename;              /* REMOTE_VIDEO (owned or NULL) */
    char *signed_model;          /* MISSING (owned or NULL) */
} rt_attachable;

typedef struct {
    rt_attachable attachable;
    char *caption; /* owned or NULL */
} rt_attachment;

void rt_attachment_dispose(rt_attachment *attachment);
void rt_attachable_dispose(rt_attachable *attachable);

/* ActionText::Attachment.from_node (Campfire's extension). */
rt_status rt_attachment_from_node(const rt_dom *dom, rt_node node, const rt_render_ctx *ctx,
                                  rt_attachment *out);
/* ActionText::Attachable.from_node (no Campfire fallback). */
rt_status rt_action_text_attachable_from_node(const rt_dom *dom, rt_node node, const rt_render_ctx *ctx,
                                              rt_attachable *out);
/* attachable_content_type, Err for the attachables that don't define it. */
rt_status rt_attachable_content_type(const rt_attachable *attachable, const char **out);
const char *rt_attachable_content_type_str(const rt_attachable *attachable);

/* render_action_text_attachment; render_content renders a nested content
 * attachment (ContentAttachment#to_html). */
rt_status rt_render_attachment(const rt_attachment *attachment,
                               rt_status (*render_content)(void *arg, const char *content, rt_buf *out),
                               void *arg, rt_buf *out);
/* render_attachment_html (depth 0), for the editor representation. */
rt_status rt_render_attachment_html(const rt_attachment *attachment, const rt_render_ctx *ctx,
                                    rt_buf *out);
/* The plain-text representation; *is_content selects Content vs Html. */
void rt_attachment_plain_text(const rt_attachment *attachment, rt_buf *out, bool *is_content);

/* web_url: absolute http(s) on a named host other than request_host. */
rt_status rt_web_url(const unsigned char *bytes, size_t len, const char *request_host, char **out,
                     bool *found);

/* ---- content (content.rs) ------------------------------------------------ */

typedef struct {
    rt_dom dom;
    rt_node root;
} rt_content;

void rt_content_dispose(rt_content *content);
/* ActionText::Content.new(html): canonicalizing load. */
rt_status rt_content_load(const unsigned char *html, size_t len, const rt_render_ctx *ctx,
                          rt_content *out);
/* ActionText::Content.new(html, canonicalize: false) / Fragment.from_html. */
rt_status rt_content_wrap(const unsigned char *html, size_t len, rt_content *out);
rt_node_vec rt_content_attachment_nodes(const rt_dom *dom, rt_node root);
rt_node_vec rt_content_attachment_gallery_nodes(const rt_dom *dom, rt_node root);
/* Content#render (attachments + galleries + action-text sanitize). */
rt_status rt_content_render(const rt_content *content, const rt_render_ctx *ctx, rt_buf *out);
/* Content#to_s / to_rendered_html_with_layout. */
rt_status rt_content_to_layout_html(const rt_content *content, const rt_render_ctx *ctx, rt_buf *out);
/* Content#to_plain_text. */
rt_status rt_content_to_plain_text(const rt_content *content, const rt_render_ctx *ctx, rt_buf *out);

#define RT_MAX_CONTENT_ATTACHMENT_DEPTH 8

/* ---- filters (filters.rs) ------------------------------------------------ */

typedef enum { RT_FILTERS_ALL = 0, RT_FILTERS_SOLO_UNFURL } rt_filters_stage;
rt_status rt_filters_apply(rt_content *content, const rt_render_ctx *ctx);

/* ---- top-level pipeline (lib.rs) ---------------------------------------- */

/* What `messages/_message.html.erb` shows (present_message). */
typedef enum { RT_PRESENTATION_HTML = 0, RT_PRESENTATION_BLANK, RT_PRESENTATION_UNRENDERABLE } rt_presentation;

/* message_presentation plus its rescue. */
rt_status rt_present_message(rt_resolver *resolver, const char *request_host, const unsigned char *body,
                             size_t len, rt_presentation *kind, rt_buf *out);
/* The text branch: filters + layout + auto_link; an error is returned, not
 * rescued. */
rt_status rt_message_presentation(rt_resolver *resolver, const char *request_host,
                                  const unsigned char *body, size_t len, rt_buf *out);
/* Presenter::body_html: Content#to_s, errors become "" (unwrap_or_default). */
rt_status rt_body_html(rt_resolver *resolver, const char *request_host, const unsigned char *body,
                       size_t len, rt_buf *out);
/* editable_value: found=false for a blank body; the reference raises for an
 * attachable without attachable_content_type. */
rt_status rt_editable_value(rt_resolver *resolver, const char *request_host, const unsigned char *body,
                            size_t len, bool *found, rt_buf *out);
/* to_plain_text (errors returned, not swallowed). */
rt_status rt_to_plain_text(rt_resolver *resolver, const unsigned char *body, size_t len, rt_buf *out);
/* mentioned_users (errors returned, not swallowed). */
rt_status rt_mentioned_users(rt_resolver *resolver, const unsigned char *body, size_t len,
                             cf_int64_vector *out);

/* webhook helper: the plain body with the bot's own "@Name" removed. */
rt_status rt_without_recipient_mentions(const unsigned char *plain, size_t len, const char *name,
                                        rt_buf *out);

#endif /* CF_RICHTEXT_INTERNAL_H */
