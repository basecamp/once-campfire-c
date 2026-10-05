/* src/richtext/rt_richtext.c — the top-level reference pipeline and the
 * cf_-prefixed module surface.
 *
 * Ports tmp/rust-ref/crates/richtext/src/lib.rs plus the app wiring in
 * crates/campfire/src/rich_text.rs (AppRichText) and Presenter::editable_body
 * / body_html, over the D02/R02 boundary message.c declares.
 */
#include "richtext.h"

#include "richtext/internal.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cf.h"

/* ---- the immutable pipeline singleton ------------------------------------ */

static cf_richtext rt_singleton;

void cf_richtext_configure(cf_span secret_key_base) {
    rt_singleton.secret_key_base = secret_key_base;
}

const cf_richtext *cf_tx_rich_text(cf_tx *tx) {
    (void)tx; /* the pipeline is stateless; the transaction is not consulted */
    return &rt_singleton;
}

/* http 1.5.0 HeaderValue::to_str: HTAB or visible ASCII; obs-text (which H01
 * admits), DEL and the other controls make the header read as absent. */
static bool rt_host_readable(cf_span value) {
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (c != '\t' && (c < 0x20 || c > 0x7E)) return false;
    }
    return true;
}

/* Kit's request.host() (request.rs:160-166): the first readable Host header
 * (HeaderValue::to_str, so an obs-text value reads as absent) with a numeric
 * port stripped.  When it is absent or unreadable -- and when there is no
 * request at all -- the pin falls back to `uri.authority()`, which is always
 * None here because H01 admits origin-form targets only, and then to
 * "localhost".  Controllers pass `Some(c.request.host())` to the renderer, so
 * callers always receive a real, non-NULL host string. */
static void request_host(const cf_ctx *ctx, char *out, size_t cap) {
    out[0] = '\0';
    const cf_request *request = ctx != NULL ? ctx->request : NULL;
    if (request != NULL) {
        for (size_t i = 0; i < request->header_count; i++) {
            const cf_header *header = &request->headers[i];
            if (header->name.len != 4 ||
                !(header->name.ptr[0] == 'h' || header->name.ptr[0] == 'H') ||
                !(header->name.ptr[1] == 'o' || header->name.ptr[1] == 'O') ||
                !(header->name.ptr[2] == 's' || header->name.ptr[2] == 'S') ||
                !(header->name.ptr[3] == 't' || header->name.ptr[3] == 'T')) {
                continue;
            }
            /* `HeaderMap::get` returns the first Host; when it is unreadable
             * the read is None and no later Host header is consulted. */
            if (!rt_host_readable(header->value)) break;
            size_t len = header->value.len;
            if (len >= cap) len = cap - 1;
            memcpy(out, header->value.ptr, len);
            out[len] = '\0';
            char *colon = strrchr(out, ':');
            if (colon != NULL && colon[1] != '\0') {
                bool digits = true;
                for (const char *p = colon + 1; *p != '\0'; p++) {
                    if (*p < '0' || *p > '9') digits = false;
                }
                if (digits) *colon = '\0';
            }
            return;
        }
    }
    /* uri.authority() is always None here (H01 admits origin-form targets
     * only); request.rs falls back to "localhost". */
    static const char fallback[] = "localhost";
    size_t len = sizeof fallback - 1;
    if (len >= cap) len = cap - 1;
    memcpy(out, fallback, len);
    out[len] = '\0';
}

static rt_status make_resolver(rt_db_resolver *storage, const cf_richtext *rich_text, cf_db *db,
                               rt_resolver **out) {
    cf_span secret = {0};
    if (rich_text != NULL) secret = rich_text->secret_key_base;
    *out = rt_db_resolver_init(storage, db, secret, cf_now_us(NULL));
    return RT_OK;
}

/* src/richtext/rt_content.c: the canonicalizing load followed by
 * `Content#to_html` (module-internal; see the definition's comment). */
rt_status rt_content_canonical(const unsigned char *html, size_t len, const rt_render_ctx *ctx,
                               rt_buf *out);

/* ---- cf_ surface --------------------------------------------------------- */

cf_err cf_richtext_render(cf_ctx *ctx, cf_span input, cf_safe_html *out) {
    if (out == NULL) return CF_INVALID;
    out->bytes = NULL;
    if (ctx == NULL || input.ptr == NULL) return CF_INVALID;
    char host[256];
    request_host(ctx, host, sizeof host);
    rt_db_resolver storage;
    rt_resolver *resolver = NULL;
    make_resolver(&storage, cf_tx_rich_text(NULL), ctx->reader, &resolver);
    rt_buf rendered;
    rt_buf_init(&rendered);
    rt_presentation kind = RT_PRESENTATION_HTML;
    rt_status rc = rt_present_message(resolver, host, input.ptr, input.len, &kind,
                                      &rendered);
    if (rc == RT_NOMEM) {
        rt_buf_dispose(&rendered);
        return CF_NOMEM;
    }
    if (rc != RT_OK) {
        rt_buf_dispose(&rendered);
        return CF_INTERNAL;
    }
    if (kind == RT_PRESENTATION_UNRENDERABLE) {
        rt_buf_dispose(&rendered);
        return CF_INVALID;
    }
    if (kind == RT_PRESENTATION_BLANK) {
        rt_buf_dispose(&rendered);
        return CF_OK; /* out->bytes stays NULL: the rescue-to-empty branch */
    }
    cf_span span = rt_buf_span(&rendered);
    cf_err err = cf_buf_copy(span, &out->bytes);
    rt_buf_dispose(&rendered);
    return err;
}

cf_err cf_richtext_body_html(cf_ctx *ctx, cf_span input, cf_safe_html *out) {
    if (out == NULL) return CF_INVALID;
    out->bytes = NULL;
    if (ctx == NULL || input.ptr == NULL) return CF_INVALID;
    char host[256];
    request_host(ctx, host, sizeof host);
    rt_db_resolver storage;
    rt_resolver *resolver = NULL;
    make_resolver(&storage, cf_tx_rich_text(NULL), ctx->reader, &resolver);
    rt_buf rendered;
    rt_buf_init(&rendered);
    rt_status rc = rt_body_html(resolver, host, input.ptr, input.len, &rendered);
    if (rc == RT_NOMEM) {
        rt_buf_dispose(&rendered);
        return CF_NOMEM;
    }
    if (rc != RT_OK) {
        rt_buf_clear(&rendered);
    }
    cf_span span = rt_buf_span(&rendered);
    cf_err err = cf_buf_copy(span, &out->bytes);
    rt_buf_dispose(&rendered);
    return err;
}

cf_err cf_richtext_editable(cf_ctx *ctx, cf_span input, bool *found, cf_str *out) {
    if (ctx == NULL || found == NULL || out == NULL || input.ptr == NULL) return CF_INVALID;
    *found = false;
    out->ptr = NULL;
    out->len = 0;
    char host[256];
    request_host(ctx, host, sizeof host);
    rt_db_resolver storage;
    rt_resolver *resolver = NULL;
    make_resolver(&storage, cf_tx_rich_text(NULL), ctx->reader, &resolver);
    rt_buf value;
    rt_buf_init(&value);
    rt_status rc =
        rt_editable_value(resolver, host, input.ptr, input.len, found, &value);
    if (rc == RT_OK && *found) {
        rt_status str_rc = rt_buf_to_str(&value, out);
        rt_buf_dispose(&value);
        return str_rc == RT_OK ? CF_OK : CF_NOMEM;
    }
    rt_buf_dispose(&value);
    if (rc == RT_NOMEM) return CF_NOMEM;
    if (rc != RT_OK) return CF_INVALID; /* the edit page raises */
    return CF_OK;
}

cf_err cf_richtext_canonical_body(cf_ctx *ctx, cf_span input, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    out->ptr = NULL;
    out->len = 0;
    /* A present-but-empty body param is {NULL, 0} (params.c's
     * param_string_copy); it is the reference's empty String, not an error. */
    if (ctx == NULL || (input.ptr == NULL && input.len != 0)) return CF_INVALID;
    static const unsigned char empty_html[1] = {0};
    const unsigned char *html = input.ptr != NULL ? input.ptr : empty_html;
    char host[256];
    request_host(ctx, host, sizeof host);
    rt_db_resolver storage;
    rt_resolver *resolver = NULL;
    make_resolver(&storage, cf_tx_rich_text(NULL), ctx->reader, &resolver);
    rt_render_ctx render_ctx = {resolver, host};
    rt_buf canonical;
    rt_buf_init(&canonical);
    rt_status rc = rt_content_canonical(html, input.len, &render_ctx, &canonical);
    if (rc == RT_NOMEM) {
        rt_buf_dispose(&canonical);
        return CF_NOMEM;
    }
    if (rc != RT_OK) {
        /* `unwrap_or_else(|_| body.to_string())`: a body the pipeline cannot
         * load (a raise, a parse failure) is stored exactly as given. */
        rt_buf_clear(&canonical);
        rc = rt_buf_append(&canonical, html, input.len);
        if (rc != RT_OK) {
            rt_buf_dispose(&canonical);
            return CF_NOMEM;
        }
    }
    rt_status str_rc = rt_buf_to_str(&canonical, out);
    rt_buf_dispose(&canonical);
    return str_rc == RT_OK ? CF_OK : CF_NOMEM;
}

cf_err cf_richtext_to_plain_text(cf_db *db, const cf_richtext *rich_text, cf_span input,
                                 cf_str *out) {
    if (out == NULL) return CF_INVALID;
    out->ptr = NULL;
    out->len = 0;
    if (input.ptr == NULL) return CF_OK;
    rt_db_resolver storage;
    rt_resolver *resolver = NULL;
    make_resolver(&storage, rich_text, db, &resolver);
    rt_buf text;
    rt_buf_init(&text);
    rt_status rc = rt_to_plain_text(resolver, input.ptr, input.len, &text);
    if (rc == RT_NOMEM) {
        rt_buf_dispose(&text);
        return CF_NOMEM;
    }
    if (rc != RT_OK) {
        /* AppRichText logs and continues with no text. */
        rt_buf_dispose(&text);
        return CF_OK;
    }
    rt_status str_rc = rt_buf_to_str(&text, out);
    rt_buf_dispose(&text);
    return str_rc == RT_OK ? CF_OK : CF_NOMEM;
}

cf_err cf_richtext_to_plain_text_outcome(cf_db *db, const cf_richtext *rich_text, cf_span input,
                                         cf_str *out, cf_richtext_plain_outcome *outcome) {
    if (out == NULL || outcome == NULL) return CF_INVALID;
    out->ptr = NULL;
    out->len = 0;
    *outcome = CF_RICHTEXT_PLAIN_TEXT;
    rt_buf text;
    rt_buf_init(&text);
    rt_status rc = RT_OK;
    if (input.ptr != NULL) {
        rt_db_resolver storage;
        rt_resolver *resolver = NULL;
        make_resolver(&storage, rich_text, db, &resolver);
        rc = rt_to_plain_text(resolver, input.ptr, input.len, &text);
    }
    if (rc == RT_UNRENDERABLE) {
        /* The reference raise's own message is not valid UTF-8, so the
         * rescue's logging raises again: message_tag's rescue fails and the
         * whole page fails (Rust Error::Internal, HTTP 500). */
        rt_buf_dispose(&text);
        return CF_INTERNAL;
    }
    if (rc == RT_NOMEM) {
        rt_buf_dispose(&text);
        return CF_NOMEM;
    }
    if (rc != RT_OK) {
        /* Any other raise was rescued by message_tag: the message renders
         * messages/_unrenderable in place of all of it. */
        rt_buf_dispose(&text);
        *outcome = CF_RICHTEXT_PLAIN_UNRENDERABLE;
        return CF_OK;
    }
    rt_status str_rc = rt_buf_to_str(&text, out);
    rt_buf_dispose(&text);
    return str_rc == RT_OK ? CF_OK : CF_NOMEM;
}

cf_err cf_richtext_mentioned_user_ids(cf_db *db, const cf_richtext *rich_text, cf_span input,
                                      cf_int64_vector *out) {
    if (out == NULL) return CF_INVALID;
    out->items = NULL;
    out->len = 0;
    out->cap = 0;
    if (input.ptr == NULL) return CF_OK;
    rt_db_resolver storage;
    rt_resolver *resolver = NULL;
    make_resolver(&storage, rich_text, db, &resolver);
    cf_int64_vector ids = {0};
    rt_status rc = rt_mentioned_users(resolver, input.ptr, input.len, &ids);
    if (rc == RT_NOMEM) {
        cf_int64_vector_dispose(&ids);
        return CF_NOMEM;
    }
    if (rc != RT_OK) {
        cf_int64_vector_dispose(&ids);
        return CF_OK; /* raises become an empty list */
    }
    *out = ids;
    return CF_OK;
}

cf_err cf_richtext_without_recipient_mentions(cf_span plain_text, cf_span recipient_name,
                                              cf_str *out) {
    if (out == NULL) return CF_INVALID;
    out->ptr = NULL;
    out->len = 0;
    char *name = malloc(recipient_name.len + 1);
    if (name == NULL) return CF_NOMEM;
    memcpy(name, recipient_name.ptr, recipient_name.len);
    name[recipient_name.len] = '\0';
    rt_buf text;
    rt_buf_init(&text);
    rt_status rc = rt_without_recipient_mentions(plain_text.ptr, plain_text.len, name, &text);
    free(name);
    if (rc != RT_OK) {
        rt_buf_dispose(&text);
        return rc == RT_NOMEM ? CF_NOMEM : CF_INTERNAL;
    }
    rt_status str_rc = rt_buf_to_str(&text, out);
    rt_buf_dispose(&text);
    return str_rc == RT_OK ? CF_OK : CF_NOMEM;
}
