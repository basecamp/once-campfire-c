/* src/views/render.c — shared renderer helpers (src/views/internal.h).
 *
 * Ports the parts of tmp/rust-ref/crates/views/src/helpers/{tag,assets,forms,
 * application}.rs the foundation families use.  Attribute order follows the
 * reference helpers (Rails renders options hashes in insertion order), even
 * though the golden comparison sorts attributes.
 */
#include "views/internal.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ guard */

cf_err cf_view_begin(cf_view_guard *guard, cf_builder *out) {
    if (guard == NULL || out == NULL) return CF_INVALID;
    guard->out = out;
    guard->start = out->len;
    return CF_OK;
}

cf_err cf_view_fail(cf_view_guard *guard, cf_err rc) {
    if (guard == NULL || guard->out == NULL) return CF_INVALID;
    guard->out->len = guard->start;
    return rc == CF_OK ? CF_NOMEM : rc;
}

cf_err cf_view_finish(cf_view_guard *guard) {
    if (guard == NULL || guard->out == NULL) return CF_INVALID;
    return CF_OK;
}

/* ------------------------------------------------------------------ writes */

cf_err cf_view_raw(cf_builder *out, cf_span bytes) {
    if (out == NULL) return CF_INVALID;
    if (bytes.len != 0 && bytes.ptr == NULL) return CF_INVALID;
    /* Subtract only when out->len is below the cap: an entry builder already
     * at or over it must not underflow the remaining-room arithmetic. */
    if (bytes.len != 0 &&
        (out->len >= CF_VIEWS_MAX_OUTPUT ||
         bytes.len > CF_VIEWS_MAX_OUTPUT - out->len)) {
        return CF_LIMIT;
    }
    return cf_builder_append(out, bytes);
}

cf_err cf_view_str(cf_builder *out, const char *text) {
    if (text == NULL) return CF_INVALID;
    return cf_view_raw(out, (cf_span){(const unsigned char *)text, strlen(text)});
}

/* Exact number of bytes cf_html_text/cf_html_attr would append for `value`
 * (R01's tables: text escapes & < >; attribute values also escape " and ').
 * Clamped above the cap so the sum cannot overflow on adversarial lengths. */
static size_t escaped_size(cf_span value, bool attr) {
    size_t total = 0;
    for (size_t i = 0; i < value.len; i++) {
        switch (value.ptr[i]) {
        case '&': total += 5; break;         /* &amp; */
        case '<': case '>': total += 4; break; /* &lt; / &gt; */
        case '"': total += attr ? 6 : 1; break;  /* &quot; */
        case '\'': total += attr ? 5 : 1; break; /* &#39; */
        default: total += 1; break;
        }
        if (total > CF_VIEWS_MAX_OUTPUT) return total;
    }
    return total;
}

/* True when escaping `value` (without expanding it) keeps the builder within
 * the cap; an over-cap builder has no room for a non-empty append. */
static bool escaped_fits(const cf_builder *out, cf_span value, bool attr) {
    if (out->len > CF_VIEWS_MAX_OUTPUT) return false;
    size_t room = CF_VIEWS_MAX_OUTPUT - out->len;
    return escaped_size(value, attr) <= room;
}

cf_err cf_view_text(cf_builder *out, cf_span text) {
    if (out == NULL) return CF_INVALID;
    if (text.len != 0 && text.ptr == NULL) return CF_INVALID;
    if (text.len != 0 && !escaped_fits(out, text, false)) return CF_LIMIT;
    return cf_html_text(out, text);
}

cf_err cf_view_html_attr(cf_builder *out, cf_span value) {
    if (out == NULL) return CF_INVALID;
    if (value.len != 0 && value.ptr == NULL) return CF_INVALID;
    if (value.len != 0 && !escaped_fits(out, value, true)) return CF_LIMIT;
    return cf_html_attr(out, value);
}

cf_err cf_view_text_cstr(cf_builder *out, const char *text) {
    if (text == NULL) return CF_INVALID;
    return cf_view_text(out, (cf_span){(const unsigned char *)text, strlen(text)});
}

/* -------------------------------------------------------------- attributes */

static void attrs_memset(cf_view_attrs *attrs) {
    memset(attrs, 0, sizeof *attrs);
}

void cf_view_attrs_init(cf_view_attrs *attrs) { attrs_memset(attrs); }

static size_t attrs_find(const cf_view_attrs *attrs, const char *name) {
    for (size_t i = 0; i < attrs->count; i++) {
        if (strcmp(attrs->items[i].name, name) == 0) return i;
    }
    return SIZE_MAX;
}

cf_err cf_view_attr_set_opt(cf_view_attrs *attrs, const char *name,
                            bool present, cf_span value) {
    if (attrs == NULL || name == NULL) return CF_INVALID;
    size_t index = attrs_find(attrs, name);
    if (index == SIZE_MAX) {
        if (attrs->count == CF_VIEW_ATTRS_MAX) return CF_LIMIT;
        index = attrs->count++;
        attrs->items[index].name = name;
        attrs->items[index].trusted = false;
        attrs->items[index].flag = false;
    }
    attrs->items[index].present = present && value.ptr != NULL;
    attrs->items[index].value = value;
    return CF_OK;
}

cf_err cf_view_attr(cf_view_attrs *attrs, const char *name, cf_span value) {
    return cf_view_attr_set_opt(attrs, name, true, value);
}

cf_err cf_view_attr_cstr(cf_view_attrs *attrs, const char *name,
                         const char *value) {
    if (value == NULL) return CF_INVALID;
    return cf_view_attr(attrs, name,
                        (cf_span){(const unsigned char *)value, strlen(value)});
}

cf_err cf_view_attr_opt(cf_view_attrs *attrs, const char *name, bool present,
                        cf_span value) {
    return cf_view_attr_set_opt(attrs, name, present, value);
}

cf_err cf_view_attr_i64(cf_view_attrs *attrs, const char *name, int64_t value) {
    if (attrs == NULL || name == NULL) return CF_INVALID;
    if (attrs->count == CF_VIEW_ATTRS_MAX) return CF_LIMIT;
    size_t index = attrs->count++;
    attrs->items[index].name = name;
    attrs->items[index].trusted = false;
    attrs->items[index].flag = false;
    int n = snprintf(attrs->items[index].number,
                     sizeof attrs->items[index].number, "%" PRId64, value);
    if (n < 0 || (size_t)n >= sizeof attrs->items[index].number) {
        return CF_INTERNAL;
    }
    attrs->items[index].value = (cf_span){
        (const unsigned char *)attrs->items[index].number, (size_t)n};
    attrs->items[index].present = true;
    return CF_OK;
}

cf_err cf_view_attr_set_i64(cf_view_attrs *attrs, const char *name,
                            int64_t value) {
    if (attrs == NULL || name == NULL) return CF_INVALID;
    size_t index = attrs_find(attrs, name);
    if (index == SIZE_MAX) return cf_view_attr_i64(attrs, name, value);
    int n = snprintf(attrs->items[index].number,
                     sizeof attrs->items[index].number, "%" PRId64, value);
    if (n < 0 || (size_t)n >= sizeof attrs->items[index].number) {
        return CF_INTERNAL;
    }
    attrs->items[index].value = (cf_span){
        (const unsigned char *)attrs->items[index].number, (size_t)n};
    attrs->items[index].present = true;
    attrs->items[index].trusted = false;
    attrs->items[index].flag = false;
    return CF_OK;
}

cf_err cf_view_attr_set(cf_view_attrs *attrs, const char *name, cf_span value) {
    return cf_view_attr_set_opt(attrs, name, true, value);
}

cf_err cf_view_attr_raw(cf_view_attrs *attrs, const char *name, cf_span value) {
    if (attrs == NULL || name == NULL || value.ptr == NULL) return CF_INVALID;
    size_t index = attrs_find(attrs, name);
    if (index == SIZE_MAX) {
        if (attrs->count == CF_VIEW_ATTRS_MAX) return CF_LIMIT;
        index = attrs->count++;
        attrs->items[index].name = name;
    }
    attrs->items[index].present = true;
    attrs->items[index].trusted = true;
    attrs->items[index].flag = false;
    attrs->items[index].value = value;
    return CF_OK;
}

cf_err cf_view_attr_flag(cf_view_attrs *attrs, const char *name, bool flag) {
    if (attrs == NULL || name == NULL) return CF_INVALID;
    size_t index = attrs_find(attrs, name);
    if (index == SIZE_MAX) {
        if (attrs->count == CF_VIEW_ATTRS_MAX) return CF_LIMIT;
        index = attrs->count++;
        attrs->items[index].name = name;
    }
    attrs->items[index].present = flag;
    attrs->items[index].trusted = false;
    attrs->items[index].flag = true;
    attrs->items[index].value = (cf_span){NULL, 0};
    return CF_OK;
}

cf_err cf_view_attr_remove(cf_view_attrs *attrs, const char *name) {
    if (attrs == NULL || name == NULL) return CF_INVALID;
    size_t index = attrs_find(attrs, name);
    if (index == SIZE_MAX) return CF_OK;
    for (size_t i = index; i + 1 < attrs->count; i++) {
        attrs->items[i] = attrs->items[i + 1];
    }
    attrs->count--;
    return CF_OK;
}

const cf_span *cf_view_attr_get(const cf_view_attrs *attrs, const char *name) {
    size_t index = attrs_find(attrs, name);
    if (index == SIZE_MAX || !attrs->items[index].present) return NULL;
    return &attrs->items[index].value;
}

/* ` name="value"` with ERB escaping, or ` name="value"` with only quotes
 * replaced for html_safe values (helpers/tag.rs render_attr). */
static cf_err write_attr(cf_builder *out, const char *name, cf_span value,
                         bool trusted) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, " "));
    CF_VIEW_TRY(cf_view_str(out, name));
    CF_VIEW_TRY(cf_view_str(out, "=\""));
    if (trusted) {
        /* SafeBuffer: only '"' is replaced. */
        size_t run = 0;
        for (size_t i = 0; i <= value.len; i++) {
            if (i == value.len || value.ptr[i] == '"') {
                if (run != 0) {
                    CF_VIEW_TRY(cf_view_raw(out,
                                            (cf_span){value.ptr + i - run, run}));
                    run = 0;
                }
                if (i != value.len) {
                    CF_VIEW_TRY(cf_view_str(out, "&quot;"));
                }
            } else {
                run++;
            }
        }
    } else {
        /* Attribute values escape five characters (cf_html_attr), unlike
         * text content; the wrapper accounts for the expansion in the cap. */
        CF_VIEW_TRY(cf_view_html_attr(out, value));
    }
    CF_VIEW_TRY(cf_view_str(out, "\""));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* TagHelper::BOOLEAN_ATTRIBUTES: rendered as name="name" when true. */
static bool is_boolean_attribute(const char *name) {
    static const char *const BOOLEAN[] = {
        "allowfullscreen", "allowpaymentrequest", "async", "autofocus",
        "autoplay", "checked", "compact", "controls", "declare", "default",
        "defaultchecked", "defaultmuted", "defaultselected", "defer",
        "disabled", "enabled", "formnovalidate", "hidden", "indeterminate",
        "inert", "ismap", "itemscope", "loop", "multiple", "muted",
        "nohref", "nomodule", "noresize", "noshade", "novalidate", "nowrap",
        "open", "pauseonexit", "playsinline", "readonly", "required",
        "reversed", "scoped", "seamless", "selected", "sortable",
        "truespeed", "typemustmatch", "visible"};
    for (size_t i = 0; i < sizeof BOOLEAN / sizeof BOOLEAN[0]; i++) {
        if (strcmp(name, BOOLEAN[i]) == 0) return true;
    }
    return false;
}

static cf_err write_attrs(cf_builder *out, const cf_view_attrs *attrs) {
    for (size_t i = 0; i < attrs->count; i++) {
        const char *name = attrs->items[i].name;
        if (!attrs->items[i].present) continue;
        cf_err rc;
        if (attrs->items[i].flag) {
            rc = cf_view_str(out, " ");
            if (rc != CF_OK) return rc;
            rc = cf_view_str(out, name);
            if (rc != CF_OK) return rc;
            rc = cf_view_str(out, "=\"");
            if (rc != CF_OK) return rc;
            rc = cf_view_str(out, is_boolean_attribute(name) ? name : "true");
            if (rc != CF_OK) return rc;
            rc = cf_view_str(out, "\"");
            if (rc != CF_OK) return rc;
        } else {
            rc = write_attr(out, name, attrs->items[i].value,
                            attrs->items[i].trusted);
            if (rc != CF_OK) return rc;
        }
    }
    return CF_OK;
}

/* ---------------------------------------------------------------- elements */

cf_err cf_view_open_start(cf_builder *out, const char *name,
                          const cf_view_attrs *attrs) {
    cf_err rc = cf_view_str(out, "<");
    if (rc != CF_OK) return rc;
    rc = cf_view_str(out, name);
    if (rc != CF_OK) return rc;
    rc = write_attrs(out, attrs);
    if (rc != CF_OK) return rc;
    return cf_view_str(out, ">");
}

cf_err cf_view_legacy_tag(cf_builder *out, const char *name,
                          const cf_view_attrs *attrs) {
    cf_err rc = cf_view_open_start(out, name, attrs);
    if (rc != CF_OK) return rc;
    /* legacy_tag is `<... />`: drop the closing '>' open_start wrote. */
    if (out->len == 0 || out->ptr[out->len - 1] != '>') return CF_INTERNAL;
    out->len--;
    return cf_view_str(out, " />");
}

static bool is_void_element(const char *name) {
    static const char *const VOID[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "keygen", "link", "meta", "source", "track", "wbr"};
    for (size_t i = 0; i < sizeof VOID / sizeof VOID[0]; i++) {
        if (strcmp(name, VOID[i]) == 0) return true;
    }
    return false;
}

cf_err cf_view_builder_tag(cf_builder *out, const char *name,
                           const cf_view_attrs *attrs) {
    cf_err rc = cf_view_open_start(out, name, attrs);
    if (rc != CF_OK) return rc;
    if (is_void_element(name)) return CF_OK;
    return cf_view_close_tag(out, name);
}

cf_err cf_view_close_tag(cf_builder *out, const char *name) {
    cf_err rc = cf_view_str(out, "</");
    if (rc != CF_OK) return rc;
    rc = cf_view_str(out, name);
    if (rc != CF_OK) return rc;
    return cf_view_str(out, ">");
}

cf_err cf_view_content(cf_builder *out, const char *name,
                       const cf_view_attrs *attrs, cf_span content) {
    cf_err rc = cf_view_open_start(out, name, attrs);
    if (rc != CF_OK) return rc;
    rc = cf_view_raw(out, content);
    if (rc != CF_OK) return rc;
    return cf_view_close_tag(out, name);
}

cf_err cf_view_content_text(cf_builder *out, const char *name,
                            const cf_view_attrs *attrs, cf_span text) {
    cf_err rc = cf_view_open_start(out, name, attrs);
    if (rc != CF_OK) return rc;
    rc = cf_view_text(out, text);
    if (rc != CF_OK) return rc;
    return cf_view_close_tag(out, name);
}

/* -------------------------------------------------------------- image_tag */

/* helpers/assets.rs `asset_path(ctx, source)`: URLs and absolute paths pass
 * through; a logical path resolves through the context's closure. */
static bool asset_source_is_url(cf_span source) {
    if (source.len == 0) return false;
    if (source.ptr[0] == '/') return true;
    if (source.len >= 5 && memcmp(source.ptr, "data:", 5) == 0) return true;
    if (source.len >= 4 && memcmp(source.ptr, "cid:", 4) == 0) return true;
    for (size_t i = 0; i + 2 < source.len; i++) {
        if (source.ptr[i] == ':' && source.ptr[i + 1] == '/' &&
            source.ptr[i + 2] == '/') {
            bool scheme = i != 0;
            for (size_t k = 0; k < i; k++) {
                char c = (char)source.ptr[k];
                if (!((c >= 'a' && c <= 'z') || c == '-')) {
                    scheme = false;
                    break;
                }
            }
            if (scheme) return true;
        }
    }
    return false;
}

static cf_err image_source(const cf_view_ctx *ctx, cf_span logical,
                           cf_builder *out) {
    if (logical.len == 0) return CF_OK;
    if (asset_source_is_url(logical)) {
        return cf_view_raw(out, logical);
    }
    if (ctx->asset_path == NULL) return CF_INVALID;
    return ctx->asset_path(ctx->asset_path_user, logical, out);
}

cf_err cf_view_image_tag(const cf_view_ctx *ctx, cf_span logical,
                         const cf_view_attrs *attrs, cf_builder *out) {
    if (ctx == NULL || attrs == NULL || out == NULL) return CF_INVALID;
    if (logical.ptr == NULL && logical.len != 0) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder src = {0};
    cf_view_attrs copy;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    if (ctx->asset_path == NULL) {
        rc = CF_INVALID;
        goto fail;
    }
    rc = image_source(ctx, logical, &src);
    if (rc != CF_OK) goto fail;

    memcpy(&copy, attrs, sizeof copy);
    const cf_span *size = cf_view_attr_get(&copy, "size");
    char size_buf[32];
    bool has_size = size != NULL;
    if (has_size) {
        if (size->len >= sizeof size_buf) {
            rc = CF_INVALID;
            goto fail;
        }
        memcpy(size_buf, size->ptr, size->len);
        size_buf[size->len] = '\0';
        rc = cf_view_attr_remove(&copy, "size");
        if (rc != CF_OK) goto fail;
    }
    rc = cf_view_attr_set_opt(&copy, "src", true, cf_view_span_of(&src));
    if (rc != CF_OK) goto fail;
    if (has_size) {
        char *x = strchr(size_buf, 'x');
        if (x != NULL) {
            *x = '\0';
            rc = cf_view_attr_cstr(&copy, "width", size_buf);
            if (rc == CF_OK) rc = cf_view_attr_cstr(&copy, "height", x + 1);
        } else {
            rc = cf_view_attr_cstr(&copy, "width", size_buf);
            if (rc == CF_OK) rc = cf_view_attr_cstr(&copy, "height", size_buf);
        }
        if (rc != CF_OK) goto fail;
    }
    rc = cf_view_legacy_tag(out, "img", &copy);
    if (rc != CF_OK) goto fail;
    cf_builder_dispose(&src);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&src);
    return cf_view_fail(&guard, rc);
}

/* ------------------------------------------------------------- fragments */

cf_err cf_view_account_logo(const cf_view_ctx *ctx, const char *style,
                            bool has_style, cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    cf_builder figure = {0};
    CF_VIEW_TRY(cf_view_str(&figure, "<figure class=\"account-logo avatar "));
    if (has_style && style != NULL) CF_VIEW_TRY(cf_view_text_cstr(&figure, style));
    CF_VIEW_TRY(cf_view_str(&figure, "\">"));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "alt", "Account logo"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "size", "300"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_str_span(ctx->account.logo_url),
                                      &attrs, &figure));
    }
    CF_VIEW_TRY(cf_view_str(&figure, "</figure>"));
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&figure)));
    cf_builder_dispose(&figure);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&figure);
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_version_badge(const cf_view_ctx *ctx, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_view_attrs attrs;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "version-badge"));
    CF_VIEW_TRY(cf_view_content_text(out, "span", &attrs, ctx->app_version));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_current_user_meta_tags(const cf_view_ctx *ctx,
                                      cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    if (!ctx->current_user.has_user) return CF_OK;
    cf_err rc;
    cf_view_guard guard;
    cf_view_attrs attrs;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "name", "current-user-id"));
    CF_VIEW_TRY(cf_view_attr_i64(&attrs, "content", ctx->current_user.id));
    CF_VIEW_TRY(cf_view_legacy_tag(out, "meta", &attrs));
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "name", "current-user-name"));
    CF_VIEW_TRY(cf_view_attr(&attrs, "content", cf_str_span(ctx->current_user.name)));
    CF_VIEW_TRY(cf_view_legacy_tag(out, "meta", &attrs));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_action_cable_meta_tag(const cf_view_ctx *ctx, cf_builder *out) {
    (void)ctx;
    cf_err rc;
    cf_view_guard guard;
    cf_view_attrs attrs;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "name", "action-cable-url"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "content", "/cable"));
    CF_VIEW_TRY(cf_view_builder_tag(out, "meta", &attrs));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_body_classes(const cf_view_ctx *ctx, cf_span body_class,
                            bool has_body_class, cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    bool first = true;
    if (has_body_class && body_class.len != 0) {
        CF_VIEW_TRY(cf_view_raw(out, body_class));
        first = false;
    }
    if (ctx->current_user.has_user && ctx->current_user.administrator) {
        if (!first) CF_VIEW_TRY(cf_view_str(out, " "));
        CF_VIEW_TRY(cf_view_str(out, "admin"));
        first = false;
    }
    if (ctx->account.has_logo) {
        if (!first) CF_VIEW_TRY(cf_view_str(out, " "));
        CF_VIEW_TRY(cf_view_str(out, "account-has-logo"));
    }
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* ------------------------------------------------------------------ forms */

static cf_err form_tag_name(const cf_view_form *form, const char *method,
                            cf_builder *out) {
    if (form->param_key == NULL) return cf_view_str(out, method);
    cf_err rc = cf_view_str(out, form->param_key);
    if (rc != CF_OK) return rc;
    rc = cf_view_str(out, "[");
    if (rc != CF_OK) return rc;
    rc = cf_view_str(out, method);
    if (rc != CF_OK) return rc;
    return cf_view_str(out, "]");
}

static cf_err form_tag_id(const cf_view_form *form, const char *method,
                          cf_builder *out) {
    if (form->param_key == NULL) return cf_view_str(out, method);
    for (const char *p = form->param_key; *p != '\0'; p++) {
        char c = *p;
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == ':' || c == '.';
        if (keep) {
            cf_err rc = cf_view_raw(out, (cf_span){(const unsigned char *)p, 1});
            if (rc != CF_OK) return rc;
        } else {
            cf_err rc = cf_view_str(out, "_");
            if (rc != CF_OK) return rc;
        }
    }
    cf_err rc = cf_view_str(out, "_");
    if (rc != CF_OK) return rc;
    return cf_view_str(out, method);
}

void cf_view_form_init(cf_view_form *form, cf_builder *out) {
    memset(form, 0, sizeof *form);
    form->out = out;
    form->method = "post";
    form->has_data = false;
}

cf_err cf_view_form_open(const cf_view_form *form) {
    if (form == NULL || form->out == NULL || form->action.ptr == NULL) {
        return CF_INVALID;
    }
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, form->out);
    if (rc != CF_OK) return rc;

    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    if (form->class_attr != NULL) {
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", form->class_attr));
    }
    if (form->has_data) {
        for (size_t i = 0; i < form->data.count; i++) {
            if (form->data.items[i].trusted) {
                CF_VIEW_TRY(cf_view_attr_raw(&attrs, form->data.items[i].name,
                                             form->data.items[i].value));
            } else {
                CF_VIEW_TRY(cf_view_attr(&attrs, form->data.items[i].name,
                                         form->data.items[i].value));
            }
        }
    }
    if (form->multipart) {
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "enctype", "multipart/form-data"));
    }
    CF_VIEW_TRY(cf_view_attr(&attrs, "action", form->action));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "accept-charset", "UTF-8"));
    const char *method = form->method != NULL ? form->method : "post";
    bool form_get = strcmp(method, "get") == 0;
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "method", form_get ? "get" : "post"));
    CF_VIEW_TRY(cf_view_open_start(form->out, "form", &attrs));
    if (!form_get && strcmp(method, "post") != 0) {
        CF_VIEW_TRY(cf_view_method_tag(form->out, method));
    }
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_method_tag(cf_builder *out, const char *method) {
    cf_err rc;
    cf_view_guard guard;
    cf_view_attrs attrs;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "type", "hidden"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "name", "_method"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "value", method));
    CF_VIEW_TRY(cf_view_legacy_tag(out, "input", &attrs));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_form_field(const cf_view_form *form, const char *type,
                          const char *method, cf_span value, bool has_value,
                          const cf_view_attrs *options) {
    if (form == NULL || form->out == NULL || type == NULL || method == NULL) {
        return CF_INVALID;
    }
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, form->out);
    if (rc != CF_OK) return rc;

    cf_view_attrs attrs;
    if (options != NULL) {
        memcpy(&attrs, options, sizeof attrs);
    } else {
        cf_view_attrs_init(&attrs);
    }
    /* `if !options.has("size") { size = options["maxlength"] }` */
    const cf_span *maxlength = cf_view_attr_get(&attrs, "maxlength");
    const cf_span *size = cf_view_attr_get(&attrs, "size");
    if (size == NULL) {
        CF_VIEW_TRY(cf_view_attr_set_opt(&attrs, "size", maxlength != NULL,
                                         maxlength != NULL ? *maxlength
                                                           : (cf_span){NULL, 0}));
    }
    CF_VIEW_TRY(cf_view_attr_set_opt(&attrs, "type", true,
                                     (cf_span){(const unsigned char *)type,
                                               strlen(type)}));
    bool is_file = strcmp(type, "file") == 0;
    if (!is_file) {
        CF_VIEW_TRY(cf_view_attr_set_opt(&attrs, "value", has_value, value));
    }
    /* name/id last (fetch_or_set). */
    {
        cf_builder name = {0}, id = {0};
        rc = form_tag_name(form, method, &name);
        if (rc == CF_OK) rc = form_tag_id(form, method, &id);
        if (rc == CF_OK) {
            rc = cf_view_attr_set_opt(&attrs, "name", true,
                                      cf_view_span_of(&name));
        }
        if (rc == CF_OK) {
            rc = cf_view_attr_set_opt(&attrs, "id", true,
                                      cf_view_span_of(&id));
        }
        if (rc == CF_OK) rc = cf_view_legacy_tag(form->out, "input", &attrs);
        cf_builder_dispose(&name);
        cf_builder_dispose(&id);
        if (rc != CF_OK) return cf_view_fail(&guard, rc);
    }
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* Ruby hash merge: an option replaces the same-named entry in place carrying
 * every property (value, flag, trusted, numeric buffer). */
static cf_err attrs_merge(cf_view_attrs *dst, const cf_view_attrs *src) {
    for (size_t i = 0; i < src->count; i++) {
        size_t index = attrs_find(dst, src->items[i].name);
        if (index == SIZE_MAX) {
            if (dst->count == CF_VIEW_ATTRS_MAX) return CF_LIMIT;
            dst->items[dst->count] = src->items[i];
            dst->count++;
        } else {
            const char *name = dst->items[index].name;
            dst->items[index] = src->items[i];
            dst->items[index].name = name;
        }
    }
    return CF_OK;
}

cf_err cf_view_form_password_field(const cf_view_form *form,
                                   const char *method,
                                   const cf_view_attrs *options) {
    /* `{ value: nil }.merge!(options)`: a nil value keeps the key's position
     * but never renders, and options override it. */
    cf_view_attrs merged;
    cf_view_attrs_init(&merged);
    cf_view_attr_set_opt(&merged, "value", false, (cf_span){NULL, 0});
    if (options != NULL) {
        cf_err rc = attrs_merge(&merged, options);
        if (rc != CF_OK) return rc;
    }
    return cf_view_form_field(form, "password", method, (cf_span){NULL, 0},
                              false, &merged);
}

cf_err cf_view_form_file_field(const cf_view_form *form, const char *method,
                               const cf_view_attrs *options,
                               bool *multipart_out) {
    if (multipart_out != NULL) *multipart_out = true;
    return cf_view_form_field(form, "file", method, (cf_span){NULL, 0}, false,
                              options);
}

cf_err cf_view_button(cf_builder *out, const cf_view_attrs *options,
                      cf_span content) {
    if (out == NULL) return CF_INVALID;
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    cf_err rc = cf_view_attr_cstr(&attrs, "name", "button");
    if (rc != CF_OK) return rc;
    rc = cf_view_attr_cstr(&attrs, "type", "submit");
    if (rc != CF_OK) return rc;
    if (options != NULL) {
        cf_err rc = attrs_merge(&attrs, options);
        if (rc != CF_OK) return rc;
    }
    return cf_view_content(out, "button", &attrs, content);
}

/* `button_to(url, options) { content }` (helpers/forms.rs): the form, its
 * `_method` field when the method is delete/patch/put, then the button with
 * the caller's options and `type="submit"`. */
cf_err cf_view_button_to(cf_builder *out, cf_span url,
                         const cf_view_attrs *options, cf_span method,
                         cf_span form_class, cf_span content) {
    if (out == NULL || url.ptr == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    char method_buf[16];
    const char *method_text = "post";
    if (method.ptr != NULL && method.len != 0) {
        if (method.len >= sizeof method_buf) {
            rc = CF_INVALID;
            goto fail;
        }
        memcpy(method_buf, method.ptr, method.len);
        method_buf[method.len] = '\0';
        method_text = method_buf;
    }
    char class_buf[64];
    const char *class_text = "button_to";
    if (form_class.ptr != NULL && form_class.len != 0) {
        if (form_class.len >= sizeof class_buf) {
            rc = CF_INVALID;
            goto fail;
        }
        memcpy(class_buf, form_class.ptr, form_class.len);
        class_buf[form_class.len] = '\0';
        class_text = class_buf;
    }
    bool form_get = strcmp(method_text, "get") == 0;

    cf_view_attrs form;
    cf_view_attrs_init(&form);
    CF_VIEW_TRY(cf_view_attr_cstr(&form, "class", class_text));
    CF_VIEW_TRY(cf_view_attr_cstr(&form, "method", form_get ? "get" : "post"));
    CF_VIEW_TRY(cf_view_attr(&form, "action", url));
    CF_VIEW_TRY(cf_view_open_start(out, "form", &form));
    if (strcmp(method_text, "delete") == 0 || strcmp(method_text, "patch") == 0 ||
        strcmp(method_text, "put") == 0) {
        CF_VIEW_TRY(cf_view_method_tag(out, method_text));
    }
    {
        cf_view_attrs button;
        cf_view_attrs_init(&button);
        if (options != NULL) {
            CF_VIEW_TRY(attrs_merge(&button, options));
        }
        CF_VIEW_TRY(cf_view_attr_set_opt(&button, "type", true,
                                         cf_span_of_lit("submit")));
        CF_VIEW_TRY(cf_view_open_start(out, "button", &button));
    }
    CF_VIEW_TRY(cf_view_raw(out, content));
    CF_VIEW_TRY(cf_view_close_tag(out, "button"));
    CF_VIEW_TRY(cf_view_close_tag(out, "form"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* ------------------------------------------------------------ fragments */

cf_err cf_view_ctx_url(const cf_view_ctx *ctx, cf_span path, cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    if (path.ptr == NULL) return CF_INVALID;
    if (path.len >= 3 && path.ptr[0] == 'h' && path.ptr[1] == 't' &&
        path.ptr[2] == 't') {
        return cf_view_raw(out, path); /* already absolute */
    }
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_raw(out, ctx->base_url));
    CF_VIEW_TRY(cf_view_raw(out, path));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_ctx_asset(const cf_view_ctx *ctx, cf_span logical,
                         cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    if (logical.len == 0) return CF_OK;
    if (logical.ptr == NULL) return CF_INVALID;
    if (logical.ptr[0] == '/') return cf_view_raw(out, logical);
    if (ctx->asset_path == NULL) return CF_INVALID;
    return ctx->asset_path(ctx->asset_path_user, logical, out);
}

bool cf_view_ctx_can_administer(const cf_view_ctx *ctx) {
    if (ctx == NULL || !ctx->current_user.has_user) return false;
    return ctx->current_user.administrator || ctx->current_user.bot;
}

/* Ruby Float#to_s (ruby_compat::float_to_s, Ruby 3.4): the shortest decimal
 * digits that read back as the value, with ".0" when integral, the exponent
 * form when the decimal point is below -3 or (above 15 with no fraction),
 * and a two-digit signed exponent. */
cf_err cf_view_float_format(double f, cf_builder *out) {
    if (f != f) return cf_view_str(out, "NaN");
    if (f > 1.7976931348623157e308) return cf_view_str(out, "Infinity");
    if (f < -1.7976931348623157e308) return cf_view_str(out, "-Infinity");
    if (f == 0.0) return cf_view_str(out, signbit(f) ? "-0.0" : "0.0");
    bool negative = signbit(f);
    double magnitude = negative ? -f : f;
    char buf[64];
    int precision = 0;
    for (int p = 1; p <= 17; p++) {
        int n = snprintf(buf, sizeof buf, "%.*e", p - 1, magnitude);
        if (n < 0 || (size_t)n >= sizeof buf) return CF_INTERNAL;
        if (strtod(buf, NULL) == magnitude) {
            precision = p;
            break;
        }
    }
    if (precision == 0) return CF_INTERNAL;
    /* buf is "d[.ddd]e±XX": split into digits and the exponent. */
    char digits[32];
    size_t digit_count = 0;
    int exponent = 0;
    {
        const char *p = buf;
        while (*p != '\0' && *p != 'e' && *p != 'E') {
            if (*p >= '0' && *p <= '9') {
                if (digit_count + 1 >= sizeof digits) return CF_INTERNAL;
                digits[digit_count++] = *p;
            }
            p++;
        }
        if (*p == '\0') return CF_INTERNAL;
        exponent = (int)strtol(p + 1, NULL, 10);
    }
    digits[digit_count] = '\0';
    int decpt = exponent + 1;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    if (negative) CF_VIEW_TRY(cf_view_str(out, "-"));
    if (decpt < -3 || (decpt > 15 && (int)digit_count <= decpt)) {
        /* {first}.{rest}e±NN, rest "0" when there is only one digit. */
        CF_VIEW_TRY(cf_view_raw(out, (cf_span){
            (const unsigned char *)digits, 1}));
        CF_VIEW_TRY(cf_view_str(out, "."));
        if (digit_count > 1) {
            CF_VIEW_TRY(cf_view_raw(out, (cf_span){
                (const unsigned char *)digits + 1, digit_count - 1}));
        } else {
            CF_VIEW_TRY(cf_view_str(out, "0"));
        }
        char exp_buf[16];
        int e = decpt - 1;
        int n = snprintf(exp_buf, sizeof exp_buf, "e%c%02d",
                         e < 0 ? '-' : '+', e < 0 ? -e : e);
        if (n < 0 || (size_t)n >= sizeof exp_buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_raw(out, (cf_span){
            (const unsigned char *)exp_buf, (size_t)n}));
    } else if (decpt <= 0) {
        CF_VIEW_TRY(cf_view_str(out, "0."));
        for (int i = 0; i < -decpt; i++) CF_VIEW_TRY(cf_view_str(out, "0"));
        CF_VIEW_TRY(cf_view_raw(out, (cf_span){
            (const unsigned char *)digits, digit_count}));
    } else if ((size_t)decpt >= digit_count) {
        CF_VIEW_TRY(cf_view_raw(out, (cf_span){
            (const unsigned char *)digits, digit_count}));
        for (size_t i = digit_count; i < (size_t)decpt; i++) {
            CF_VIEW_TRY(cf_view_str(out, "0"));
        }
        CF_VIEW_TRY(cf_view_str(out, ".0"));
    } else {
        CF_VIEW_TRY(cf_view_raw(out, (cf_span){
            (const unsigned char *)digits, (size_t)decpt}));
        CF_VIEW_TRY(cf_view_str(out, "."));
        CF_VIEW_TRY(cf_view_raw(out, (cf_span){
            (const unsigned char *)digits + decpt,
            digit_count - (size_t)decpt}));
    }
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_number_format(cf_view_number value, cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    char buf[32];
    if (!value.is_float) {
        int n = snprintf(buf, sizeof buf, "%lld", (long long)value.integer);
        if (n < 0 || (size_t)n >= sizeof buf) return CF_INTERNAL;
        return cf_view_raw(out, (cf_span){(const unsigned char *)buf,
                                          (size_t)n});
    }
    return cf_view_float_format(value.real, out);
}

cf_view_number cf_view_number_half(cf_view_number value) {
    if (!value.is_float) {
        /* Ruby Integer#/ 2: floor division (Rust's div_euclid(2)). */
        int64_t v = value.integer;
        value.integer = v >= 0 ? v / 2 : -((-v + 1) / 2);
        return value;
    }
    value.real = value.real / 2.0;
    return value;
}

cf_err cf_view_capitalize(cf_span text, cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    if (text.len != 0 && text.ptr == NULL) return CF_INVALID;
    /* The reference capitalizes Unicode; the platform names are ASCII. */
    for (size_t i = 0; i < text.len; i++) {
        unsigned char c = text.ptr[i];
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        cf_err rc = cf_view_raw(out, (cf_span){&c, 1});
        if (rc != CF_OK) return rc;
        /* The rest lowercases. */
        for (size_t k = i + 1; k < text.len; k++) {
            unsigned char rest = text.ptr[k];
            if (rest >= 'A' && rest <= 'Z') {
                rest = (unsigned char)(rest - 'A' + 'a');
            }
            rc = cf_view_raw(out, (cf_span){&rest, 1});
            if (rc != CF_OK) return rc;
        }
        return CF_OK;
    }
    return CF_OK;
}

cf_err cf_view_to_sentence(const cf_span *items, size_t count,
                           cf_span connector, cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    if (count != 0 && items == NULL) return CF_INVALID;
    if (count == 0) return CF_OK;
    cf_err rc = cf_view_raw(out, items[0]);
    if (rc != CF_OK) return rc;
    if (count == 1) return CF_OK;
    if (count == 2) {
        rc = cf_view_raw(out, connector);
        if (rc != CF_OK) return rc;
        return cf_view_raw(out, items[1]);
    }
    for (size_t i = 1; i + 1 < count; i++) {
        rc = cf_view_str(out, ", ");
        if (rc == CF_OK) rc = cf_view_raw(out, items[i]);
        if (rc != CF_OK) return rc;
    }
    rc = cf_view_str(out, ", and ");
    if (rc != CF_OK) return rc;
    return cf_view_raw(out, items[count - 1]);
}

/* `sidebar_turbo_frame_tag(src:) { content }` (helpers/users.rs): the frame
 * options are data-* first, then id, src, target. */
cf_err cf_view_sidebar_frame(cf_span src, cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_view_attrs attrs;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-turbo-permanent", "true"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-controller",
                                  "rooms-list read-rooms turbo-frame"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-rooms-list-unread-class",
                                  "unread"));
    CF_VIEW_TRY(cf_view_attr_raw(
        &attrs, "data-action",
        cf_span_of_lit("presence:present@window->rooms-list#read "
                       "read-rooms:read->rooms-list#read "
                       "turbo:frame-load->rooms-list#loaded "
                       "refresh-room:visible@window->turbo-frame#reload")));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "id", "user_sidebar"));
    CF_VIEW_TRY(cf_view_attr_opt(&attrs, "src", src.ptr != NULL && src.len != 0,
                                 src));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "target", "_top"));
    CF_VIEW_TRY(cf_view_content(out, "turbo-frame", &attrs, (cf_span){NULL, 0}));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* messages/support.rs epoch_ms, including the deliberate float round trip. */
int64_t cf_view_epoch_ms(int64_t time_us) {
    int64_t seconds = time_us / 1000000;
    int64_t micros = time_us % 1000000;
    if (micros < 0) {
        seconds -= 1;
        micros += 1000000;
    }
    int64_t nanos = micros * 1000;
    char decimal[64];
    if (seconds < 0 && nanos != 0) {
        int64_t whole = seconds + 1;
        int64_t frac = 1000000000 - nanos;
        snprintf(decimal, sizeof decimal, "%s%lld.%09lld",
                 whole == 0 ? "-" : "", (long long)whole, (long long)frac);
    } else {
        snprintf(decimal, sizeof decimal, "%lld.%09lld", (long long)seconds,
                 (long long)nanos);
    }
    double to_f = strtod(decimal, NULL);
    return (int64_t)(to_f * 1000.0);
}

cf_err cf_view_iso8601(int64_t time_us, char out[21]) {
    if (out == NULL) return CF_INVALID;
    time_t seconds = (time_t)(time_us / 1000000);
    if (time_us < 0 && time_us % 1000000 != 0) seconds -= 1;
    struct tm tm;
    if (gmtime_r(&seconds, &tm) == NULL) return CF_INTERNAL;
    size_t n = strftime(out, 21, "%Y-%m-%dT%H:%M:%SZ", &tm);
    if (n != 20) return CF_INTERNAL;
    return CF_OK;
}

cf_err cf_view_str_dup(cf_span span, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (span.len != 0 && span.ptr == NULL) return CF_INVALID;
    char *copy = malloc(span.len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (span.len != 0) memcpy(copy, span.ptr, span.len);
    copy[span.len] = '\0';
    out->ptr = copy;
    out->len = span.len;
    return CF_OK;
}

/* ------------------------------------------------------- ruby compatibility */

/* Ruby's ISSPACE: what `String#to_i` skips (ruby_compat's SPACE). */
static bool integer_cast_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
           c == '\r';
}

/* Moved verbatim from src/actions/welcome.c's welcome_integer_cast (itself
 * the port of ruby_compat::integer_cast); the semantics are documented at the
 * declaration in src/views/internal.h. */
bool cf_views_integer_cast(cf_span text, int64_t *out) {
    size_t lead = 0;
    while (lead < text.len && integer_cast_space(text.ptr[lead])) lead++;
    size_t at = lead;
    if (at < text.len && (text.ptr[at] == '+' || text.ptr[at] == '-')) at++;
    if (at >= text.len || text.ptr[at] < '0' || text.ptr[at] > '9') {
        return false;
    }

    at = lead;
    bool negative = false;
    if (at < text.len && (text.ptr[at] == '+' || text.ptr[at] == '-')) {
        negative = text.ptr[at] == '-';
        at++;
    }
    if (at + 1 < text.len && text.ptr[at] == '0' &&
        (text.ptr[at + 1] == 'd' || text.ptr[at + 1] == 'D')) {
        at += 2;
    }
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    uint64_t acc = 0;
    bool overflow = false;
    bool previous_digit = false;
    for (; at < text.len; at++) {
        unsigned char c = text.ptr[at];
        if (c >= '0' && c <= '9') {
            unsigned digit = (unsigned)(c - '0');
            if (acc > (limit - digit) / 10u) {
                overflow = true; /* past i64: integer_cast answers nil */
            } else {
                acc = acc * 10u + digit;
            }
            previous_digit = true;
        } else if (c == '_' && previous_digit) {
            previous_digit = false;
        } else {
            break;
        }
    }
    if (overflow) return false;
    if (negative) {
        *out = acc == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)acc;
    } else {
        *out = (int64_t)acc;
    }
    return true;
}
