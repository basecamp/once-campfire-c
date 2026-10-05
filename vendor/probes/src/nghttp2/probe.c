/*
 * F00 probe for nghttp2 v1.70.0 (static library, no apps/server).
 *
 * Verifies the library API:
 *   - nghttp2_version() reports the pinned release 1.70.0
 *     (string and numeric form);
 *   - a client session can be created and destroyed with the library
 *     callbacks API;
 *   - HPACK deflater/inflater can be initialized and round-trip a
 *     request pseudo-header (":method: GET") through deflate_hd /
 *     inflate_hd2.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <nghttp2/nghttp2.h>

static int failures;

static void check(bool cond, const char *what)
{
    if (cond) {
        printf("ok: %s\n", what);
    } else {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

int main(void)
{
    const nghttp2_info *info = nghttp2_version(0);

    check(info != NULL, "nghttp2_version() returns info");
    if (info != NULL) {
        check(strcmp(info->version_str, "1.70.0") == 0,
              "version_str is 1.70.0");
        check(info->version_num == 0x014600,
              "version_num is 0x014600 (1.70.0)");
    }

    /* Client session creation. */
    nghttp2_session_callbacks *cbs = NULL;
    int rv = nghttp2_session_callbacks_new(&cbs);
    check(rv == 0 && cbs != NULL, "session callbacks allocated");
    nghttp2_session *session = NULL;
    rv = nghttp2_session_client_new(&session, cbs, NULL);
    check(rv == 0 && session != NULL, "client session created");
    if (session != NULL)
        nghttp2_session_del(session);
    nghttp2_session_callbacks_del(cbs);

    /* HPACK initialization and a round trip. */
    nghttp2_hd_deflater *deflater = NULL;
    nghttp2_hd_inflater *inflater = NULL;
    rv = nghttp2_hd_deflate_new(&deflater, 4096);
    check(rv == 0 && deflater != NULL, "HPACK deflater initialized");
    rv = nghttp2_hd_inflate_new(&inflater);
    check(rv == 0 && inflater != NULL, "HPACK inflater initialized");

    if (deflater != NULL && inflater != NULL) {
        nghttp2_nv nv;
        uint8_t block[4096];

        memset(&nv, 0, sizeof(nv));
        nv.name = (uint8_t *)":method";
        nv.value = (uint8_t *)"GET";
        nv.namelen = 7;
        nv.valuelen = 3;
        nv.flags = NGHTTP2_NV_FLAG_NONE;

        ssize_t blocklen = nghttp2_hd_deflate_hd(deflater, block,
                                                 sizeof(block), &nv, 1);
        check(blocklen > 0, "HPACK deflate produces a block");

        if (blocklen > 0) {
            size_t off = 0;
            bool got_header = false;
            bool got_final = false;

            /* Documented drain loop: emit headers until FINAL is
             * reported, then finish the block. */
            for (;;) {
                nghttp2_nv out;
                int inflate_flags = 0;
                ssize_t used = nghttp2_hd_inflate_hd2(inflater, &out,
                                                      &inflate_flags,
                                                      block + off,
                                                      (size_t)blocklen - off,
                                                      1);
                if (used < 0) {
                    fprintf(stderr, "FAIL: HPACK inflate error %zd\n", used);
                    failures++;
                    break;
                }
                off += (size_t)used;
                if (inflate_flags & NGHTTP2_HD_INFLATE_EMIT) {
                    check(out.namelen == 7 && out.valuelen == 3 &&
                              memcmp(out.name, ":method", 7) == 0 &&
                              memcmp(out.value, "GET", 3) == 0,
                          "HPACK round-trip is :method: GET");
                    got_header = true;
                }
                if (inflate_flags & NGHTTP2_HD_INFLATE_FINAL) {
                    got_final = true;
                    nghttp2_hd_inflate_end_headers(inflater);
                    break;
                }
                if (used == 0 && !(inflate_flags & NGHTTP2_HD_INFLATE_EMIT))
                    break;
            }
            check(off == (size_t)blocklen, "HPACK inflate consumes the block");
            check(got_header, "HPACK inflate emits the header");
            check(got_final, "HPACK inflate reports FINAL");
        }
    }
    if (deflater != NULL)
        nghttp2_hd_deflate_del(deflater);
    if (inflater != NULL)
        nghttp2_hd_inflate_del(inflater);

    printf("probe_result: %s (%d failure(s))\n",
           failures ? "FAIL" : "READY", failures);
    return failures ? 1 : 0;
}
