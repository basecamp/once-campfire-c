/* H02 corpus test: the entire pinned parameters vector corpus
 * (tests/fixtures/crates/kit/tests/params_vectors.json, 2,755 Rails-generated
 * query-string cases; params_vectors.rs in the reference runs the same file
 * through kit::parse_nested and compares against Rails).
 *
 * Reads the JSON at runtime with the vendored yyjson (vendor/DEPS.json) and
 * compares the stored tree exactly, key order included, using the
 * module-internal layout from src/http/params.h.
 *
 * One corpus vector is a depth-99 bracket key that Rails (DEPTH_LIMIT 100)
 * accepts while 01-foundation-http.md fixes the C parameter depth at 32. That
 * single case is asserted to fail with CF_LIMIT and is reported separately;
 * every other case must match Rails exactly. Run from the repository root. */
#include "cf.h"
#define CF_HTTP_PARAMS_INTERNALS 1
#include "http/params.h"

#include "cf_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yyjson.h"

#define VECTORS_PATH "tests/fixtures/crates/kit/tests/params_vectors.json"
#define VECTORS_TOTAL 2755

/* Exact structural equality against a JSON value, preserving order: object
 * entries compare key-by-key at the same position, array elements in order. */
static bool node_eq(const cf_param *p, yyjson_val *j) {
    if (p == NULL) return false;
    if (yyjson_is_null(j)) return p->kind == CF_PARAM_NULL;
    if (yyjson_is_bool(j)) {
        return p->kind == CF_PARAM_BOOL && p->u.boolean == yyjson_get_bool(j);
    }
    if (yyjson_is_str(j)) {
        if (p->kind != CF_PARAM_STRING) return false;
        size_t n = yyjson_get_len(j);
        return p->u.string.len == n &&
               (n == 0 || memcmp(p->u.string.ptr, yyjson_get_str(j), n) == 0);
    }
    if (yyjson_is_num(j)) { /* no corpus case reaches this branch */
        if (p->kind != CF_PARAM_NUMBER) return false;
        double d = yyjson_get_real(j);
        return p->u.number.integral && (double)p->u.number.value == d;
    }
    if (yyjson_is_arr(j)) {
        if (p->kind != CF_PARAM_ARRAY || p->u.array.len != yyjson_arr_size(j)) return false;
        yyjson_arr_iter it = yyjson_arr_iter_with(j);
        yyjson_val *item;
        size_t i = 0;
        while ((item = yyjson_arr_iter_next(&it)) != NULL) {
            if (!node_eq(p->u.array.items[i], item)) return false;
            i++;
        }
        return true;
    }
    if (yyjson_is_obj(j)) {
        if (p->kind != CF_PARAM_OBJECT) return false;
        if (p->u.object.len != yyjson_obj_size(j)) return false;
        yyjson_obj_iter it = yyjson_obj_iter_with(j);
        yyjson_val *key;
        size_t i = 0;
        while ((key = yyjson_obj_iter_next(&it)) != NULL) {
            cf_span expected_key = {(const unsigned char *)yyjson_get_str(key), yyjson_get_len(key)};
            cf_span stored = p->u.object.entries[i].key;
            if (stored.len != expected_key.len ||
                (stored.len > 0 && memcmp(stored.ptr, expected_key.ptr, stored.len) != 0)) {
                return false;
            }
            if (!node_eq(p->u.object.entries[i].value, yyjson_obj_iter_get_val(key))) return false;
            i++;
        }
        return true;
    }
    return false;
}

static size_t bracket_count(const char *s) {
    size_t n = 0;
    for (const char *c = s; *c != '\0'; c++) {
        if (*c == '[') n++;
    }
    return n;
}

CF_TEST(params_query_vectors) {
    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_file(VECTORS_PATH, 0, NULL, &err);
    if (doc == NULL) {
        fprintf(stderr, "params_vectors: cannot read %s: %s; run from the repository root\n",
                VECTORS_PATH, err.msg);
        CF_REQUIRE(doc != NULL);
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    CF_REQUIRE(yyjson_is_arr(root));
    size_t total = yyjson_arr_size(root);
    CF_CHECK(total == VECTORS_TOTAL);

    size_t success_cases = 0, error_cases = 0;
    size_t compared = 0, errors_matched = 0, divergence = 0, mismatches = 0;
    yyjson_arr_iter it = yyjson_arr_iter_with(root);
    yyjson_val *vector;
    while ((vector = yyjson_arr_iter_next(&it)) != NULL) {
        yyjson_val *input = yyjson_obj_get(vector, "input");
        yyjson_val *expected = yyjson_obj_get(vector, "output");
        CF_REQUIRE(yyjson_is_str(input));
        CF_REQUIRE(expected != NULL);
        const char *text = yyjson_get_str(input);

        cf_request req;
        memset(&req, 0, sizeof(req));
        req.method = CF_GET;
        req.original_method = CF_GET;
        req.query = (cf_span){(const unsigned char *)text, strlen(text)};

        cf_params *params = NULL;
        cf_err e = cf_params_parse(&req, &params);
        if (yyjson_is_null(expected)) {
            error_cases++;
            if (e != CF_OK) {
                errors_matched++;
            } else {
                mismatches++;
                if (mismatches <= 5) printf("  mismatch (expected an error): %s\n", text);
            }
            CF_CHECK(params == NULL);
            cf_params_destroy(params);
        } else {
            success_cases++;
            if (e == CF_LIMIT && params == NULL) {
                /* Only the depth-99 vector may hit the fixed C depth bound. */
                divergence++;
                if (divergence <= 2) printf("  fixed-depth divergence: %s\n", text);
                CF_CHECK(bracket_count(text) >= 32);
            } else if (e != CF_OK) {
                mismatches++;
                if (mismatches <= 5) printf("  mismatch (%s): %s\n", cf_err_name(e), text);
                cf_params_destroy(params);
            } else {
                if (node_eq(&params->root, expected)) {
                    compared++;
                } else {
                    mismatches++;
                    if (mismatches <= 5) printf("  mismatch (tree differs): %s\n", text);
                }
                cf_params_destroy(params);
            }
        }
    }
    yyjson_doc_free(doc);

    printf("params vectors: total=%zu success=%zu compared=%zu errors=%zu errors_matched=%zu "
           "depth_divergence=%zu mismatches=%zu\n",
           total, success_cases, compared, error_cases, errors_matched, divergence, mismatches);
    CF_CHECK(success_cases + error_cases == VECTORS_TOTAL);
    CF_CHECK(compared + divergence == success_cases); /* every success is accounted for */
    CF_CHECK(errors_matched == error_cases);
    CF_CHECK(divergence == 1);  /* exactly the known depth-32 bound conflict */
    CF_CHECK(mismatches == 0);
}
