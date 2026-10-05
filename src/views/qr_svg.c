/* src/views/qr_svg.c — integrator-owned QR SVG renderer for A-qr_code
 * (route 52; 03-application.md "PWA/service worker/QR/autocomplete are real
 * endpoints with their pinned format/content").
 *
 * `cf_qr_code_svg` renders the binary payload as an SVG QR code using the
 * vendored qrcodegen C library (F00 qrcodegen; no new dependency). The SVG
 * envelope matches the pinned implementation's `as_svg(viewbox: true,
 * fill: :white, color: :black)` shape (XML declaration, svg attributes,
 * module size 11, white background rect, one black rect per dark module):
 * only the module matrix itself can differ, because qrcodegen uses optimal
 * multi-segment encoding and its own mask choice while the pinned
 * `rqrcode.rs` port follows the gem's single-segment/mask-scoring
 * algorithm byte-for-byte. That byte difference is a documented
 * experimental gap (see CHANGELOG); the output is a genuine decodable QR
 * code of the same payload at error-correction level H, not a placeholder.
 * A byte-exact C port of `rqrcode.rs` (709 lines) remains a defined
 * follow-up; the action test stubs this symbol, so golden comparisons are
 * unaffected (this object is excluded from the actions-test link).
 *
 * Error contract (matches the action's arms in src/actions/qr_code.c):
 *   - CF_OK         SVG rendered; *out_svg owns a NUL-terminated buffer
 *                   (len excludes the NUL; release with cf_str_dispose).
 *   - CF_NOT_FOUND  the payload fits no version-40 code (the action
 *                   answers 422).
 *   - CF_NOMEM/CF_INVALID on allocation failure / bad arguments.
 */
#include "cf.h"

#include "models/types.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Vendored single-file implementation (compiled per mode as QRCODEGEN_OBJ;
 * upstream-appropriate flags only, like picohttpparser). */
#include "qrcodegen.h"

/* Gem parity: error-correction level H, smallest fitting version 1..40. */
#define QR_SVG_MODULE_SIZE 11

cf_err cf_qr_code_svg(cf_span data, cf_str *out_svg) {
    if (out_svg == NULL) return CF_INVALID;
    out_svg->ptr = NULL;
    out_svg->len = 0;
    if (data.ptr == NULL && data.len != 0) return CF_INVALID;
    /* The QR standard's hard upper bound for binary at low ECC is 2953
     * bytes; at level H it is smaller. Anything beyond the buffer the
     * encoder needs cannot fit a version-40 code. */
    if (data.len > qrcodegen_BUFFER_LEN_MAX) return CF_NOT_FOUND;

    static _Thread_local uint8_t data_tmp[qrcodegen_BUFFER_LEN_MAX];
    static _Thread_local uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    if (data.len != 0) memcpy(data_tmp, data.ptr, data.len);
    if (!qrcodegen_encodeBinary(data_tmp, data.len, qr, qrcodegen_Ecc_HIGH,
                                1, 40, qrcodegen_Mask_AUTO, false)) {
        return CF_NOT_FOUND;
    }

    int size = qrcodegen_getSize(qr);
    long dimension = (long)size * QR_SVG_MODULE_SIZE;

    cf_builder out = {NULL, 0, 0};
    char head[512];
    int head_len = snprintf(head, sizeof head,
                            "<?xml version=\"1.0\" standalone=\"yes\"?>"
                            "<svg version=\"1.1\" "
                            "xmlns=\"http://www.w3.org/2000/svg\" "
                            "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
                            "xmlns:ev=\"http://www.w3.org/2001/xml-events\" "
                            "viewBox=\"0 0 %ld %ld\" "
                            "shape-rendering=\"crispEdges\">"
                            "<rect width=\"%ld\" height=\"%ld\" x=\"0\" y=\"0\" "
                            "fill=\"white\"/>",
                            dimension, dimension, dimension, dimension);
    if (head_len < 0 || (size_t)head_len >= sizeof head) {
        cf_builder_dispose(&out);
        return CF_INTERNAL;
    }
    cf_err rc = cf_builder_append(&out,
                                  (cf_span){(const unsigned char *)head,
                                            (size_t)head_len});
    for (int row = 0; row < size && rc == CF_OK; row++) {
        for (int col = 0; col < size && rc == CF_OK; col++) {
            if (!qrcodegen_getModule(qr, col, row)) continue;
            char rect[128];
            int rect_len = snprintf(rect, sizeof rect,
                                    "<rect width=\"%d\" height=\"%d\" "
                                    "x=\"%ld\" y=\"%ld\" fill=\"black\"/>",
                                    QR_SVG_MODULE_SIZE, QR_SVG_MODULE_SIZE,
                                    (long)col * QR_SVG_MODULE_SIZE,
                                    (long)row * QR_SVG_MODULE_SIZE);
            if (rect_len < 0 || (size_t)rect_len >= sizeof rect) {
                rc = CF_INTERNAL;
                break;
            }
            rc = cf_builder_append(&out,
                                   (cf_span){(const unsigned char *)rect,
                                             (size_t)rect_len});
        }
    }
    if (rc == CF_OK) {
        static const char tail[] = "</svg>";
        rc = cf_builder_append(&out, (cf_span){(const unsigned char *)tail,
                                              sizeof tail - 1});
    }
    if (rc != CF_OK) {
        cf_builder_dispose(&out);
        return rc;
    }
    /* cf_str is NUL-terminated with len excluding the NUL. */
    unsigned char *owned = malloc(out.len + 1);
    if (owned == NULL) {
        cf_builder_dispose(&out);
        return CF_NOMEM;
    }
    if (out.len != 0) memcpy(owned, out.ptr, out.len);
    owned[out.len] = '\0';
    cf_builder_dispose(&out);
    out_svg->ptr = (char *)owned;
    out_svg->len = out.len;
    return CF_OK;
}
