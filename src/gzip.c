/* cf_gzip: the gzip representation of a whole response body (K01a).
 *
 * Contract (docs/devel/implementation/contracts/api.h:157):
 *     cf_err cf_gzip(cf_span identity, cf_buf **out);
 * On CF_OK *out is a fresh, caller-owned cf_buf holding exactly one gzip
 * member that decompresses to `identity`. On any error *out is NULL and no
 * buffer is retained. Nothing else is allocated by the caller.
 *
 * Deterministic representation, per 06-cache-performance.md:84 ("Gzip level
 * 6, deterministic header mtime=0") and c-port-arch.md "Compression and
 * SQLite" (zlib-ng at level 6):
 *
 *  - Level 6, Z_DEFLATED, windowBits 31 (16 + 15: gzip wrapper), memLevel 8,
 *    Z_DEFAULT_STRATEGY. The pinned reference compresses at level 6 in both
 *    layers: flate2's Compression::default() is 6
 *    (tmp/rust-ref crates/kit/src/deflater.rs:217) and the front's
 *    GZIP_LEVEL is 6 (crates/kit/src/front/compression.rs:21,283).
 *  - The gzip header is written explicitly with deflateSetHeader: MTIME 0,
 *    XFL 0 (zlib's level-6 value), OS 3 (Unix), FLG 0 (no FTEXT, no FHCRC,
 *    no FEXTRA/FNAME/FCOMMENT). The reference asks flate2 for exactly this
 *    header, GzBuilder::new().mtime(0).operating_system(3)
 *    (deflater.rs:216-218; 06 fixes mtime to 0, where Rack::Deflater would
 *    use a response's Last-Modified or else 0, deflater.rs:72-77), and zlib
 *    fills XFL/FLG from the level and the absent optional fields
 *    (zlib-ng deflate.c:856-865). Setting OS/time explicitly keeps the bytes
 *    independent of the compile-time OS_CODE (zutil.h:99-120 default 3) so
 *    the clang and Fil-C archives produce the same member.
 *  - The BREACH jitter the front layer adds as a gzip FCOMMENT
 *    (compression.rs:24-27,137-170) is not part of the app's representation
 *    and is not emitted here (06:84 defines the cached gzip bytes).
 *  - No randomness, no clock, fixed parameters: the same identity bytes
 *    always yield the same member, which the exact-vector tests pin.
 *
 * Errors follow the frozen cf_err set: CF_INVALID for a NULL out or a NULL
 * non-empty span, CF_NOMEM when init or a buffer growth cannot allocate,
 * CF_LIMIT from cf_builder overflow accounting, CF_INTERNAL for a zlib
 * stream error. zlib's own allocations are libc malloc/free (z_stream.zalloc
 * is left NULL); only the output buffer goes through cf_core_*. */
#include "cf.h"

#include <stdint.h>
#include <string.h>

#include <zlib.h>

#define CF_GZIP_LEVEL 6
#define CF_GZIP_WINDOW_BITS (15 + 16) /* 15-bit window + gzip wrapper */
#define CF_GZIP_MEM_LEVEL 8
#define CF_GZIP_OS_UNIX 3
/* Output is drained in fixed chunks into the builder, as cable's deflate
 * does (src/cable/socket.c:486-530); no deflateBound-sized scratch is
 * needed and a builder allocation failure is reported, not truncated. */
#define CF_GZIP_CHUNK 16384

cf_err cf_gzip(cf_span identity, cf_buf **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (identity.len != 0 && identity.ptr == NULL) return CF_INVALID;

    z_stream z;
    memset(&z, 0, sizeof z);
    int zr = deflateInit2(&z, CF_GZIP_LEVEL, Z_DEFLATED, CF_GZIP_WINDOW_BITS,
                          CF_GZIP_MEM_LEVEL, Z_DEFAULT_STRATEGY);
    if (zr != Z_OK) return zr == Z_MEM_ERROR ? CF_NOMEM : CF_INTERNAL;

    gz_header header;
    memset(&header, 0, sizeof header);
    header.time = 0;
    header.os = CF_GZIP_OS_UNIX;
    if (deflateSetHeader(&z, &header) != Z_OK) {
        deflateEnd(&z);
        return CF_INTERNAL;
    }

    cf_builder built = {0};
    unsigned char chunk[CF_GZIP_CHUNK];
    size_t in_off = 0;
    cf_err rc = CF_OK;

    for (;;) {
        if (z.avail_in == 0 && in_off < identity.len) {
            size_t take = identity.len - in_off;
            if (take > UINT32_MAX) take = UINT32_MAX; /* avail_in width */
            z.next_in = (Bytef *)(uintptr_t)(identity.ptr + in_off);
            z.avail_in = (uint32_t)take;
            in_off += take;
        }
        int flush = (in_off == identity.len && z.avail_in == 0) ? Z_FINISH
                                                                : Z_NO_FLUSH;
        z.next_out = chunk;
        z.avail_out = (uint32_t)sizeof chunk;
        uint32_t before_in = z.avail_in;
        size_t before_off = in_off;
        zr = deflate(&z, flush);
        size_t produced = sizeof chunk - z.avail_out;
        if (produced != 0) {
            rc = cf_builder_append(&built, (cf_span){chunk, produced});
            if (rc != CF_OK) break;
        }
        if (zr == Z_STREAM_END) break;
        if (zr != Z_OK) {
            rc = CF_INTERNAL;
            break;
        }
        /* deflate must make progress; a stalled call is an internal fault. */
        if (produced == 0 && z.avail_in == before_in && in_off == before_off) {
            rc = CF_INTERNAL;
            break;
        }
    }

    int zend = deflateEnd(&z);
    if (rc == CF_OK && zend != Z_OK) rc = CF_INTERNAL;
    if (rc != CF_OK) {
        cf_builder_dispose(&built);
        return rc;
    }
    rc = cf_builder_freeze(&built, out);
    if (rc != CF_OK) cf_builder_dispose(&built); /* builder survives failure */
    return rc;
}
