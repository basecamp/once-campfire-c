/* src/actions/unfurl_links.c — UnfurlLinksController (task A-unfurl_links;
 * route ID 148 `create`; contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/unfurl_links.rs, the
 * pinned port of reference/app/controllers/unfurl_links_controller.rb (the
 * composer asks for a pasted URL's OpenGraph metadata):
 *
 *   before_action (Before::default(): authentication REQUIRED, deny_bots,
 *   forgery protection).
 *
 *   create: `params.require(:url)` (a missing or blank url is the
 *           controller's 400); a hash or an array passes `require`, but the
 *           metadata read cannot take it, so the unfurl has no content; the
 *           unfurl's JSON renders at 200 (`application/json`), while no
 *           content answers `head :no_content`.
 *
 * The unfurl itself is I01's cf_unfurl (src/integrations/unfurl.h), the
 * exact port of Opengraph::Metadata.from_url over Fetch/Location/Document:
 * at most 16 concurrent unfurls, 5 s connect/read inactivity with a 10 s
 * overall deadline (a URL that runs out of time unfurls nothing: 204, not
 * an error page), every address through the private-network guard and
 * pinned per hop, documents capped at 5 MiB within 10 redirects, and the
 * title/description/image validation deciding JSON vs no-content.  The
 * NoMethodError/URI::InvalidComponentError raise arm maps to the
 * reference's 500.
 *
 * c_symbol for the integrator's route rebind (src/routes.c row 148,
 * currently the development 501): cf_action_unfurl_links_create.
 */
#include "cf.h"

#include "auth.h"
#include "context.h"
#include "integrations/unfurl.h"

#include <stdlib.h>
#include <string.h>

#define UNFURL_CONTENT_JSON "application/json; charset=utf-8"

static cf_span unfurl_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `String#blank?` over Unicode White_Space (kit params.rs
 * `Param::is_blank` for strings): empty or only whitespace. */
static bool unfurl_utf8_next(cf_span text, size_t *index, uint32_t *cp) {
    size_t i = *index;
    if (i >= text.len) return false;
    unsigned char b = text.ptr[i];
    size_t len;
    uint32_t value;
    if (b < 0x80) {
        len = 1;
        value = b;
    } else if ((b & 0xE0) == 0xC0) {
        len = 2;
        value = b & 0x1F;
    } else if ((b & 0xF0) == 0xE0) {
        len = 3;
        value = b & 0x0F;
    } else if ((b & 0xF8) == 0xF0) {
        len = 4;
        value = b & 0x07;
    } else {
        return false;
    }
    if (i + len > text.len) return false;
    for (size_t k = 1; k < len; k++) {
        unsigned char c = text.ptr[i + k];
        if ((c & 0xC0) != 0x80) return false;
        value = (value << 6) | (c & 0x3F);
    }
    *index = i + len;
    *cp = value;
    return true;
}

static bool unfurl_cp_whitespace(uint32_t cp) {
    if (cp < 0x80) {
        return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\v' ||
               cp == '\f' || cp == '\r';
    }
    return cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

static bool unfurl_span_blank(cf_span text) {
    size_t i = 0;
    while (i < text.len) {
        uint32_t cp = 0;
        if (!unfurl_utf8_next(text, &i, &cp)) return false;
        if (!unfurl_cp_whitespace(cp)) return false;
    }
    return true;
}

static bool unfurl_param_present(const cf_param *param) {
    if (param == NULL) return false;
    switch (cf_param_type(param)) {
    case CF_PARAM_NULL:
        return false;
    case CF_PARAM_BOOL: {
        bool present = false, value = false;
        if (cf_param_bool(param, &present, &value) != CF_OK) return false;
        return value;
    }
    case CF_PARAM_NUMBER:
    case CF_PARAM_UPLOAD:
        return true;
    case CF_PARAM_STRING: {
        cf_span text;
        if (cf_param_string(param, &text) != CF_OK) return false;
        return !unfurl_span_blank(text);
    }
    case CF_PARAM_ARRAY:
    case CF_PARAM_OBJECT:
        return cf_param_count(param) > 0;
    }
    return false;
}

/* `params.require(:url)`: the value when it is present (or `false`), else
 * ParameterMissing (dispatch answers 400).  On success `*is_string`
 * reports whether it is a string; a hash or an array passes `require` but
 * is not a string, so the metadata has no title and is not valid (204). */
static cf_err unfurl_url_param(cf_ctx *ctx, bool *is_string, cf_span *url) {
    *is_string = false;
    *url = unfurl_span("");
    const cf_param *param = cf_ctx_param(ctx, unfurl_span("url"));
    bool required = unfurl_param_present(param);
    if (!required && param != NULL &&
        cf_param_type(param) == CF_PARAM_BOOL) {
        required = true; /* `false` is what require accepts */
    }
    if (!required) return CF_INVALID;
    if (cf_param_type(param) != CF_PARAM_STRING) return CF_OK;
    if (cf_param_string(param, url) != CF_OK) return CF_INTERNAL;
    *is_string = true;
    return CF_OK;
}

cf_err cf_action_unfurl_links_create(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc =
        cf_before_actions(ctx, (cf_before){CF_AUTH_REQUIRED, true, true});
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    bool is_string = false;
    cf_span url = unfurl_span("");
    rc = unfurl_url_param(ctx, &is_string, &url);
    if (rc != CF_OK) return rc;
    if (!is_string) {
        ctx->response->status = 204;
        return CF_OK;
    }

    /* cf_unfurl takes a NUL-terminated URL. */
    char *url_text = malloc(url.len + 1);
    if (url_text == NULL) return CF_NOMEM;
    if (url.len != 0) memcpy(url_text, url.ptr, url.len);
    url_text[url.len] = '\0';

    cf_unfurl_config config;
    cf_unfurl_default_config(&config);
    cf_unfurl_out out;
    memset(&out, 0, sizeof out);
    int unfurled = cf_unfurl(&config, url_text, &out);
    free(url_text);
    if (unfurled != 0) {
        /* Resource failure: out untouched. */
        return CF_INTERNAL;
    }
    if (out.kind == CF_UNFURL_NONE) {
        cf_unfurl_out_dispose(&out);
        ctx->response->status = 204;
        return CF_OK;
    }
    if (out.kind == CF_UNFURL_RAISED) {
        cf_unfurl_out_dispose(&out);
        return CF_INTERNAL;
    }
    /* `render json: opengraph`. */
    cf_buf *buf = NULL;
    cf_span json = {(const unsigned char *)out.json,
                    out.json != NULL ? strlen(out.json) : 0};
    rc = cf_buf_copy(json, &buf);
    cf_unfurl_out_dispose(&out);
    if (rc != CF_OK) return rc;
    ctx->response->status = 200;
    rc = cf_response_header(ctx->response, unfurl_span("Content-Type"),
                            unfurl_span(UNFURL_CONTENT_JSON));
    if (rc == CF_OK && cf_ctx_vary_accept(ctx)) {
        rc = cf_response_header(ctx->response, unfurl_span("Vary"),
                                unfurl_span("Accept"));
    }
    if (rc == CF_OK) rc = cf_response_body(ctx->response, buf);
    cf_buf_release(buf);
    return rc;
}
