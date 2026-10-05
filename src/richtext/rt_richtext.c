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

/* Kit's request.host(): the Host header with a numeric port stripped. */
static bool request_host(const cf_ctx *ctx, char *out, size_t cap) {
    out[0] = '\0';
    if (ctx == NULL || ctx->request == NULL) return false;
    const cf_request *request = ctx->request;
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != 4 ||
            !(header->name.ptr[0] == 'h' || header->name.ptr[0] == 'H') ||
            !(header->name.ptr[1] == 'o' || header->name.ptr[1] == 'O') ||
            !(header->name.ptr[2] == 's' || header->name.ptr[2] == 'S') ||
            !(header->name.ptr[3] == 't' || header->name.ptr[3] == 'T')) {
            continue;
        }
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
        return true;
    }
    return false;
}

static rt_status make_resolver(rt_db_resolver *storage, const cf_richtext *rich_text, cf_db *db,
                               rt_resolver **out) {
    cf_span secret = {0};
    if (rich_text != NULL) secret = rich_text->secret_key_base;
    *out = rt_db_resolver_init(storage, db, secret, cf_now_us(NULL));
    return RT_OK;
}

/* ---- cf_ surface --------------------------------------------------------- */

cf_err cf_richtext_render(cf_ctx *ctx, cf_span input, cf_safe_html *out) {
    if (out == NULL) return CF_INVALID;
    out->bytes = NULL;
    if (ctx == NULL || input.ptr == NULL) return CF_INVALID;
    char host[256];
    bool have_host = request_host(ctx, host, sizeof host);
    rt_db_resolver storage;
    rt_resolver *resolver = NULL;
    make_resolver(&storage, cf_tx_rich_text(NULL), ctx->reader, &resolver);
    rt_buf rendered;
    rt_buf_init(&rendered);
    rt_presentation kind = RT_PRESENTATION_HTML;
    rt_status rc = rt_present_message(resolver, have_host ? host : NULL, input.ptr, input.len, &kind,
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
    bool have_host = request_host(ctx, host, sizeof host);
    rt_db_resolver storage;
    rt_resolver *resolver = NULL;
    make_resolver(&storage, cf_tx_rich_text(NULL), ctx->reader, &resolver);
    rt_buf rendered;
    rt_buf_init(&rendered);
    rt_status rc = rt_body_html(resolver, have_host ? host : NULL, input.ptr, input.len, &rendered);
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
    bool have_host = request_host(ctx, host, sizeof host);
    rt_db_resolver storage;
    rt_resolver *resolver = NULL;
    make_resolver(&storage, cf_tx_rich_text(NULL), ctx->reader, &resolver);
    rt_buf value;
    rt_buf_init(&value);
    rt_status rc =
        rt_editable_value(resolver, have_host ? host : NULL, input.ptr, input.len, found, &value);
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
