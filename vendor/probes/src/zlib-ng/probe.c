/* F00 probe: zlib-ng 2.3.3 built with ZLIB_COMPAT=ON, static, clang.
 * Deflates a known payload with the zlib-compatible gzip wrapper (windowBits 15+16),
 * records gzip header/trailer bytes, then inflates (windowBits 15+32) and compares. */
#include <stdio.h>
#include <string.h>
#include <zlib.h>

static int failures = 0;

static void fail(const char *what) {
    failures++;
    fprintf(stderr, "PROBE FAIL: %s\n", what);
}

static void print_hex(const char *label, const unsigned char *p, size_t n) {
    printf("%s (%zu bytes):", label, n);
    for (size_t i = 0; i < n; i++) printf(" %02x", p[i]);
    printf("\n");
}

int main(void) {
    unsigned char in[1024];
    size_t in_len = 0;
    const char *chunk = "The quick brown fox jumps over the lazy dog. campfire gzip probe 0123456789. ";
    while (in_len + strlen(chunk) <= sizeof in) {
        memcpy(in + in_len, chunk, strlen(chunk));
        in_len += strlen(chunk);
    }
    /* keep it at a fixed, non-multiple length so ISIZE is distinguishable */
    in_len -= 7;
    printf("input_len=%zu crc32=0x%08lx\n", in_len, (unsigned long)crc32(0L, in, (uInt)in_len));
    printf("zlibVersion()=%s\n", zlibVersion());

    unsigned char comp[2048];
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    int rc = deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    printf("deflateInit2(gzip) rc=%d (expect 0)\n", rc);
    if (rc != Z_OK) { fail("deflateInit2"); return 1; }

    zs.next_in = in;
    zs.avail_in = (uInt)in_len;
    zs.next_out = comp;
    zs.avail_out = (uInt)sizeof comp;
    rc = deflate(&zs, Z_FINISH);
    size_t comp_len = zs.total_out;
    printf("deflate rc=%d (expect 1=Z_STREAM_END) comp_len=%zu\n", rc, comp_len);
    if (rc != Z_STREAM_END) fail("deflate did not finish");
    deflateEnd(&zs);

    print_hex("gzip_header", comp, 10);
    print_hex("gzip_trailer", comp + comp_len - 8, 8);
    if (comp_len < 18) fail("compressed output too short");
    if (comp[0] != 0x1f || comp[1] != 0x8b) fail("gzip magic mismatch");
    if (comp[2] != 8) fail("gzip CM != 8 (deflate)");
    if (comp[3] != 0) fail("gzip FLG != 0 (no name/comment/extra expected)");

    unsigned long crc = crc32(0L, in, (uInt)in_len);
    unsigned long isize = (unsigned long)(in_len & 0xffffffffu);
    unsigned char *tr = comp + comp_len - 8;
    unsigned long tr_crc = (unsigned long)tr[0] | ((unsigned long)tr[1] << 8) |
                           ((unsigned long)tr[2] << 16) | ((unsigned long)tr[3] << 24);
    unsigned long tr_isize = (unsigned long)tr[4] | ((unsigned long)tr[5] << 8) |
                             ((unsigned long)tr[6] << 16) | ((unsigned long)tr[7] << 24);
    printf("crc32_le_trailer=0x%08lx crc32_computed=0x%08lx isize_le_trailer=%lu isize_computed=%lu\n",
           tr_crc, crc, tr_isize, isize);
    if (tr_crc != crc) fail("gzip trailer CRC32 mismatch");
    if (tr_isize != isize) fail("gzip trailer ISIZE mismatch");

    unsigned char out[2048];
    z_stream ds;
    memset(&ds, 0, sizeof ds);
    rc = inflateInit2(&ds, 15 + 32);
    printf("inflateInit2(auto-gzip) rc=%d (expect 0)\n", rc);
    if (rc != Z_OK) { fail("inflateInit2"); return 1; }
    ds.next_in = comp;
    ds.avail_in = (uInt)comp_len;
    ds.next_out = out;
    ds.avail_out = (uInt)sizeof out;
    rc = inflate(&ds, Z_FINISH);
    size_t out_len = ds.total_out;
    printf("inflate rc=%d (expect 1=Z_STREAM_END) out_len=%zu\n", rc, out_len);
    if (rc != Z_STREAM_END) fail("inflate did not finish");
    inflateEnd(&ds);
    if (out_len != in_len) fail("round trip length mismatch");
    else if (memcmp(out, in, in_len) != 0) fail("round trip byte mismatch");
    else printf("round_trip bytes equal=yes\n");

    printf("probe_result=%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
