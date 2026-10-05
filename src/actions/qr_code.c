/* src/actions/qr_code.c — QrCodeController (task A-qr_code; route ID 52
 * `show`; contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/qr_code.rs, the
 * pinned port of reference/app/controllers/qr_code_controller.rb:
 *
 *   allow_unauthenticated_access
 *
 *   show: url = Base64.urlsafe_decode64(params[:id]) (ArgumentError on
 *         malformed input is a 500); qr_code =
 *         RQRCode::QRCode.new(url).as_svg(viewbox: true, fill: :white,
 *         color: :black) (too much to encode is Unprocessable Entity);
 *         expires_in 1.year, public: true; render the SVG as
 *         image/svg+xml.
 *
 * The SVG bytes are byte-identical to the gem's `as_svg` (single segment in
 * numeric/alphanumeric/8-bit-byte preference order, error correction level
 * H, smallest version with strictly-greater capacity, fewest-lost-points
 * mask; golden vectors in reference-tools/campfire/rqrcode.rb and the
 * pinned rqrcode.rs port).  `expires_in 1.year, public: true` is
 * `Cache-Control: max-age=31556952, public` (kit response.rs
 * CacheControl::to_header with max_age 31556952 and public, no_store off).
 *
 * No format negotiation runs (render_as directly, whatever the request
 * format); `Vary: Accept` is still applied exactly when the format came from
 * the Accept header (kit set_vary_header).
 *
 * c_symbol for the integrator's route rebind (src/routes.c row 52):
 *   cf_action_qr_code_show.
 *
 * Integrator request (F00 qrcodegen; the static shim is marked):
 *  R1. A shared QR SVG helper backed by the vendored qrcodegen v1.8.0
 *      (vendor/DEPS.json "qrcodegen"; releases A-qr_code).  Proposed
 *      declaration:
 *        // RQRCode::QRCode.new(data).as_svg(viewbox, white fill, black):
 *        // None (CF_NOT_FOUND) when data fits no version-40 code.
 *        cf_err cf_qr_code_svg(cf_span data, cf_str *out_svg);
 *      Until it lands (and its archive joins MODE_DEP_LIBS), the action
 *      calls it through the narrowly-named static qr_svg_bytes shim below;
 *      no new dependency is introduced by this packet.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- INTEGRATOR SHIM R1 (delete when the helper lands) -------------------- */

/* `rqrcode::svg_bytes`: None (CF_NOT_FOUND) when the payload fits no
 * version-40-H code. */
cf_err cf_qr_code_svg(cf_span data, cf_str *out_svg);

static cf_err qr_svg_bytes(cf_span url, cf_str *out_svg) {
    return cf_qr_code_svg(url, out_svg);
}

/* ---- small helpers -------------------------------------------------------- */

static cf_span qr_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `Base64.urlsafe_decode64` (rails_compat encoding::urlsafe_decode, with
 * its test vectors): `-_` and `+/` both decode; `None` where Ruby raises
 * ArgumentError — interior `=` (e.g. "ab=c"), more than two trailing `=`,
 * a total length that is not a multiple of 4 once padded ("a", "ab="),
 * a 2-character quantum with nonzero trailing bits ("aB=="), or any byte
 * outside the alphabet ("a*bc").  The empty input decodes to empty. */
static int qr_b64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

static cf_err qr_urlsafe_decode(cf_span in, unsigned char **out,
                                size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    /* `urlsafe_decode64`: without trailing `=`, the input is padded out to
     * a multiple of 4 (a remainder of 1 is malformed); with padding, the
     * strict form applies. */
    bool padded = in.len != 0 && in.ptr[in.len - 1] == '=';
    size_t total = in.len;
    size_t pad = 0;
    if (padded) {
        while (pad < total && in.ptr[total - 1 - pad] == '=') pad++;
        if (pad > 2 || total % 4 != 0) return CF_INVALID;
    } else {
        if (total % 4 == 1) return CF_INVALID;
        pad = (4 - total % 4) % 4;
        total += pad;
    }
    size_t content = in.len - (padded ? pad : 0);
    for (size_t i = 0; i < content; i++) {
        if (in.ptr[i] == '=' || qr_b64_value(in.ptr[i]) < 0) {
            return CF_INVALID;
        }
    }
    (void)total;
    if (content % 4 == 1) return CF_INVALID;
    size_t decoded = content / 4 * 3;
    if (content % 4 == 2) decoded += 1;
    else if (content % 4 == 3) decoded += 2;
    unsigned char *bytes = NULL;
    if (decoded != 0) {
        bytes = malloc(decoded);
        if (bytes == NULL) return CF_NOMEM;
    }
    size_t at = 0;
    for (size_t i = 0; i + 4 <= content; i += 4) {
        int v0 = qr_b64_value(in.ptr[i]);
        int v1 = qr_b64_value(in.ptr[i + 1]);
        int v2 = qr_b64_value(in.ptr[i + 2]);
        int v3 = qr_b64_value(in.ptr[i + 3]);
        bytes[at++] = (unsigned char)((v0 << 2) | (v1 >> 4));
        bytes[at++] = (unsigned char)(((v1 & 15) << 4) | (v2 >> 2));
        bytes[at++] = (unsigned char)(((v2 & 3) << 6) | v3);
    }
    size_t tail = content % 4;
    if (tail == 2) {
        int v0 = qr_b64_value(in.ptr[content - 2]);
        int v1 = qr_b64_value(in.ptr[content - 1]);
        if ((v1 & 15) != 0) {
            free(bytes);
            return CF_INVALID;
        }
        bytes[at++] = (unsigned char)((v0 << 2) | (v1 >> 4));
    } else if (tail == 3) {
        int v0 = qr_b64_value(in.ptr[content - 3]);
        int v1 = qr_b64_value(in.ptr[content - 2]);
        int v2 = qr_b64_value(in.ptr[content - 1]);
        if ((v2 & 3) != 0) {
            free(bytes);
            return CF_INVALID;
        }
        bytes[at++] = (unsigned char)((v0 << 2) | (v1 >> 4));
        bytes[at++] = (unsigned char)(((v1 & 15) << 4) | (v2 >> 2));
    }
    *out = bytes;
    *out_len = at;
    return CF_OK;
}

/* `Ctx::render_as` (kit ctx.rs render_as -> set_vary_header). */
static cf_err qr_render_as(cf_ctx *ctx, unsigned status,
                           cf_span content_type, const unsigned char *body,
                           size_t body_len) {
    cf_buf *buf = NULL;
    cf_err rc = cf_buf_copy((cf_span){body, body_len}, &buf);
    if (rc != CF_OK) return rc;
    ctx->response->status = status;
    rc = cf_response_body(ctx->response, buf);
    cf_buf_release(buf);
    if (rc != CF_OK) return rc;
    rc = cf_response_header(ctx->response, qr_span("Content-Type"),
                            content_type);
    if (rc == CF_OK && cf_ctx_vary_accept(ctx)) {
        rc = cf_response_header(ctx->response, qr_span("Vary"),
                                qr_span("Accept"));
    }
    return rc;
}

/* ---- action ----------------------------------------------------------------- */

cf_err cf_action_qr_code_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    /* `allow_unauthenticated_access`. */
    cf_before policy = {CF_AUTH_SKIPPED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    /* `params[:id]` ("" when absent/non-string), then the urlsafe decode:
     * malformed input is the reference's ArgumentError (a 500 here). */
    const cf_param *param = cf_ctx_param(ctx, qr_span("id"));
    cf_span id = {NULL, 0};
    if (param != NULL && cf_param_type(param) == CF_PARAM_STRING) {
        if (cf_param_string(param, &id) != CF_OK) return CF_INTERNAL;
    }
    unsigned char *url = NULL;
    size_t url_len = 0;
    rc = qr_urlsafe_decode(id, &url, &url_len);
    if (rc != CF_OK) {
        free(url);
        return CF_INTERNAL;
    }

    /* Too much to encode is the client's 422. */
    cf_str svg = {0};
    rc = qr_svg_bytes((cf_span){url, url_len}, &svg);
    free(url);
    if (rc == CF_NOT_FOUND) {
        cf_str_dispose(&svg);
        cf_ctx_set_error_status(ctx, 422);
        return CF_INVALID;
    }
    if (rc != CF_OK) {
        cf_str_dispose(&svg);
        return rc;
    }

    /* `expires_in 1.year, public: true` (31_556_952 seconds). */
    rc = cf_response_header(ctx->response, qr_span("Cache-Control"),
                            qr_span("max-age=31556952, public"));
    if (rc != CF_OK) {
        cf_str_dispose(&svg);
        return rc;
    }
    rc = qr_render_as(ctx, 200, qr_span("image/svg+xml; charset=utf-8"),
                      (const unsigned char *)svg.ptr, svg.len);
    cf_str_dispose(&svg);
    return rc;
}
