/* F00 probe: yyjson 0.13.0 amalgamation (src/yyjson.c), clang.
 * Checks: object iteration order preserved, invalid UTF-8 rejected,
 * NaN/Infinity rejected by the reader, non-finite double rejected by the
 * default writer, and exact writer output. */
#include <stdio.h>
#include <string.h>
#include "yyjson.h"

static int failures = 0;

static void fail(const char *what) {
    failures++;
    fprintf(stderr, "PROBE FAIL: %s\n", what);
}

int main(void) {
    /* 1) object iteration order preserved (keys deliberately unsorted) */
    const char *src = "{\"zeta\":1,\"alpha\":2,\"middle\":[true,null,3.5],\"beta\":{\"y\":1,\"a\":2}}";
    yyjson_doc *doc = yyjson_read(src, strlen(src), 0);
    if (!doc) {
        fail("parse ordering document");
    } else {
        yyjson_val *root = yyjson_doc_get_root(doc);
        const char *expect[] = {"zeta", "alpha", "middle", "beta"};
        size_t i = 0;
        printf("parsed object iteration order:\n");
        yyjson_obj_iter it = yyjson_obj_iter_with(root);
        yyjson_val *key;
        int order_ok = 1;
        while ((key = yyjson_obj_iter_next(&it)) != NULL) {
            yyjson_val *val = yyjson_obj_iter_get_val(key);
            const char *k = yyjson_get_str(key);
            printf("  [%zu] %s type=%s\n", i, k, yyjson_get_type_desc(val));
            if (i < 4 && strcmp(k, expect[i]) != 0) order_ok = 0;
            i++;
        }
        printf("iteration count=%zu expect=4\n", i);
        if (i != 4) fail("iteration count mismatch");
        if (!order_ok) fail("object iteration order not preserved");
        /* direct lookups still work */
        if (!yyjson_obj_get(root, "beta") || !yyjson_obj_get(root, "zeta"))
            fail("yyjson_obj_get lookup failed");
        yyjson_doc_free(doc);
    }

    /* 2) invalid UTF-8 must be rejected by default */
    const unsigned char bad_utf8[] = "{\"s\":\"a\xff""b\"}";
    yyjson_read_err err;
    memset(&err, 0, sizeof err);
    yyjson_doc *d2 = yyjson_read_opts((char *)bad_utf8, sizeof bad_utf8 - 1, 0, NULL, &err);
    printf("invalid_utf8 parse -> %s code=%u msg=\"%s\" pos=%zu\n",
           d2 ? "ACCEPTED" : "REJECTED", (unsigned)err.code, err.msg ? err.msg : "", err.pos);
    if (d2) {
        fail("invalid UTF-8 accepted by default");
        yyjson_doc_free(d2);
    }

    /* 3) NaN / Infinity literals rejected by default */
    const char *nan_src = "{\"a\":NaN}";
    memset(&err, 0, sizeof err);
    yyjson_doc *d3 = yyjson_read_opts((char *)nan_src, strlen(nan_src), 0, NULL, &err);
    printf("NaN literal parse -> %s code=%u msg=\"%s\"\n",
           d3 ? "ACCEPTED" : "REJECTED", (unsigned)err.code, err.msg ? err.msg : "");
    if (d3) { fail("NaN accepted"); yyjson_doc_free(d3); }

    const char *inf_src = "{\"a\":Infinity}";
    memset(&err, 0, sizeof err);
    yyjson_doc *d4 = yyjson_read_opts((char *)inf_src, strlen(inf_src), 0, NULL, &err);
    printf("Infinity literal parse -> %s code=%u msg=\"%s\"\n",
           d4 ? "ACCEPTED" : "REJECTED", (unsigned)err.code, err.msg ? err.msg : "");
    if (d4) { fail("Infinity accepted"); yyjson_doc_free(d4); }

    const char *ovf_src = "[1e999]";
    memset(&err, 0, sizeof err);
    yyjson_doc *d5 = yyjson_read_opts((char *)ovf_src, strlen(ovf_src), 0, NULL, &err);
    printf("1e999 overflow literal parse -> %s code=%u msg=\"%s\"\n",
           d5 ? "ACCEPTED" : "REJECTED", (unsigned)err.code, err.msg ? err.msg : "");
    if (d5) { fail("1e999 accepted"); yyjson_doc_free(d5); }

    /* 4) exact writer output */
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *mroot = yyjson_mut_obj(mdoc);
    yyjson_mut_doc_set_root(mdoc, mroot);
    yyjson_mut_obj_add_int(mdoc, mroot, "b", 7);
    yyjson_mut_obj_add_str(mdoc, mroot, "a", "x\"y");
    yyjson_mut_val *arr = yyjson_mut_arr(mdoc);
    yyjson_mut_arr_add_bool(mdoc, arr, true);
    yyjson_mut_arr_add_null(mdoc, arr);
    yyjson_mut_arr_add_real(mdoc, arr, 3.5);
    yyjson_mut_obj_add_val(mdoc, mroot, "arr", arr);
    char *out = yyjson_mut_write(mdoc, 0, NULL);
    const char *expected = "{\"b\":7,\"a\":\"x\\\"y\",\"arr\":[true,null,3.5]}";
    printf("writer_output=%s\n", out ? out : "(null)");
    printf("writer_expected=%s\n", expected);
    if (!out || strcmp(out, expected) != 0) fail("writer output mismatch");
    free(out);

    /* 5) default writer rejects a non-finite double */
    yyjson_mut_doc *ndoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *nroot = yyjson_mut_obj(ndoc);
    yyjson_mut_doc_set_root(ndoc, nroot);
    yyjson_mut_obj_add_real(ndoc, nroot, "n", (double)NAN);
    yyjson_write_err werr;
    memset(&werr, 0, sizeof werr);
    char *nout = yyjson_mut_write_opts(ndoc, 0, NULL, NULL, &werr);
    printf("writer_NaN -> %s code=%u msg=\"%s\"\n",
           nout ? nout : "(null)", (unsigned)werr.code, werr.msg ? werr.msg : "");
    if (nout) { fail("writer accepted NaN by default"); free(nout); }
    yyjson_mut_doc_free(ndoc);
    yyjson_mut_doc_free(mdoc);

    printf("probe_result=%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
