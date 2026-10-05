/* H02 parameter and method-override tests (07-verification.md HTTP-06).
 *
 * Owned tests: this file (runner plus accessor, form, JSON, multipart,
 * bounds, precedence and method-override cases) and params_vectors.c (the
 * full pinned vector corpus, read at runtime). Reference: tmp/rust-ref
 * crates/kit/src/params.rs, body.rs, adapter.rs::method_override.
 *
 * Run from the repository root: the corpus test opens
 * tests/fixtures/crates/kit/tests/params_vectors.json. */
#include "cf.h"
#include "http/params.h"

#include "cf_test.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cf_span S(const char *s) {
    return (cf_span){(const unsigned char *)s, strlen(s)};
}

static cf_span B(const char *s, size_t n) {
    return (cf_span){(const unsigned char *)s, n};
}

/* ------------------------------------------------------------- builders */

struct reqb {
    cf_request r;
};

static void reqb_init(struct reqb *b, cf_method method, cf_method original) {
    memset(&b->r, 0, sizeof(b->r));
    b->r.method = method;
    b->r.original_method = original;
}

static void reqb_header(struct reqb *b, const char *name, const char *value) {
    b->r.headers[b->r.header_count].name = S(name);
    b->r.headers[b->r.header_count].value = S(value);
    b->r.header_count++;
}

static void reqb_ct(struct reqb *b, const char *ct) { reqb_header(b, "Content-Type", ct); }
static void reqb_body(struct reqb *b, const char *body) { b->r.body = S(body); }
static void reqb_body_span(struct reqb *b, cf_span body) { b->r.body = body; }
static void reqb_query(struct reqb *b, const char *q) { b->r.query = S(q); }

static cf_err parse(const cf_request *r, cf_params **out) {
    return cf_params_parse(r, out);
}

/* ------------------------------------------------------------ accessors */

static const cf_param *pget(const cf_params *p, const char *name) {
    return cf_param_get(p, S(name));
}

static const cf_param *pfield(const cf_param *p, const char *name) {
    return cf_param_field(p, S(name));
}

static int absent(const cf_params *p, const char *name) { return pget(p, name) == NULL; }

static int kind_is(const cf_param *p, cf_param_kind k) {
    return p != NULL && cf_param_type(p) == k;
}

static int str_is(const cf_param *p, const char *want) {
    cf_span s;
    if (p == NULL || cf_param_string(p, &s) != CF_OK) return 0;
    size_t n = strlen(want);
    return s.len == n && (n == 0 || memcmp(s.ptr, want, n) == 0);
}

static int str_is_empty(const cf_param *p) {
    cf_span s;
    return p != NULL && cf_param_string(p, &s) == CF_OK && s.len == 0;
}

static int i64_is(const cf_param *p, int64_t want) {
    cf_optional_i64 v;
    return p != NULL && cf_param_i64(p, &v) == CF_OK && v.present && v.value == want;
}

static int i64_kind_present_false(const cf_param *p) {
    cf_optional_i64 v;
    return p != NULL && cf_param_i64(p, &v) == CF_OK && !v.present && v.value == 0;
}

static int bool_is(const cf_param *p, bool want) {
    bool present = false, value = false;
    return p != NULL && cf_param_bool(p, &present, &value) == CF_OK && present && value == want;
}

static int bool_null(const cf_param *p) {
    bool present = true, value = true;
    return p != NULL && cf_param_bool(p, &present, &value) == CF_OK && !present && !value;
}

/* ------------------------------------------------------- small utilities */

static char *alloc_text(size_t n) {
    char *s = malloc(n);
    if (s == NULL) abort();
    return s;
}

/* -------------------------------------------------------------- tests */

CF_TEST(accessor_null_and_absent) {
    struct reqb b;
    reqb_init(&b, CF_GET, CF_GET);
    cf_params *params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);

    CF_CHECK(absent(params, "nope"));
    CF_CHECK(cf_param_type(NULL) == CF_PARAM_NULL);
    CF_CHECK(cf_param_count(NULL) == 0);
    CF_CHECK(cf_param_at(NULL, 0) == NULL);
    CF_CHECK(cf_param_field(NULL, S("x")) == NULL);

    cf_span s = S("untouched");
    CF_CHECK(cf_param_string(NULL, &s) == CF_NOT_FOUND);
    CF_CHECK(s.len == 0 && s.ptr == NULL);
    cf_optional_i64 v = {true, 7};
    CF_CHECK(cf_param_i64(NULL, &v) == CF_NOT_FOUND);
    CF_CHECK(!v.present && v.value == 0);
    bool present = true, value = true;
    CF_CHECK(cf_param_bool(NULL, &present, &value) == CF_NOT_FOUND);
    CF_CHECK(!present && !value);

    CF_CHECK(cf_param_string(NULL, NULL) == CF_INVALID);
    CF_CHECK(cf_param_i64(NULL, NULL) == CF_INVALID);
    CF_CHECK(cf_param_bool(NULL, NULL, &value) == CF_INVALID);
    cf_params *none = NULL;
    CF_CHECK(cf_params_parse(NULL, &none) == CF_INVALID);
    CF_CHECK(none == NULL);
    CF_CHECK(cf_params_parse(&b.r, NULL) == CF_INVALID);

    cf_params_destroy(params);
    cf_params_destroy(NULL); /* destructor accepts NULL */
}

CF_TEST(accessor_null_vs_empty_vs_wrong_kind) {
    struct reqb b;
    reqb_init(&b, CF_GET, CF_GET);
    reqb_query(&b, "a&e=&s=hi");
    cf_params *params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);

    const cf_param *a = pget(params, "a"); /* pair without '=' is JSON null */
    CF_REQUIRE(a != NULL);
    CF_CHECK(kind_is(a, CF_PARAM_NULL));
    CF_CHECK(i64_kind_present_false(a)); /* null -> CF_OK, present=false */
    CF_CHECK(bool_null(a));
    CF_CHECK(cf_param_string(a, &(cf_span){0}) == CF_INVALID); /* null is not a string */

    const cf_param *e = pget(params, "e"); /* empty string */
    CF_REQUIRE(e != NULL);
    CF_CHECK(kind_is(e, CF_PARAM_STRING));
    CF_CHECK(str_is_empty(e));
    CF_CHECK(cf_param_i64(e, &(cf_optional_i64){0}) == CF_INVALID);
    CF_CHECK(cf_param_bool(e, &(bool){0}, &(bool){0}) == CF_INVALID);

    const cf_param *s = pget(params, "s");
    CF_REQUIRE(s != NULL);
    CF_CHECK(str_is(s, "hi"));
    CF_CHECK(cf_param_i64(s, &(cf_optional_i64){0}) == CF_INVALID); /* strings are not numbers */

    cf_params_destroy(params);
}

CF_TEST(accessor_json_number_matrix) {
    struct reqb b;
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/json");
    reqb_body(&b, "{\"n\":5,\"neg\":-3,\"f\":1.5,\"exp\":1e2,\"one\":1.0,"
                  "\"big\":9007199254740993,\"oob\":9223372036854775808,"
                  "\"min\":-9223372036854775808,\"max\":9223372036854775807,"
                  "\"small\":1e-1,\"z\":null,\"t\":true,\"str\":\"7\"}");
    cf_params *params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);

    CF_CHECK(i64_is(pget(params, "n"), 5));
    CF_CHECK(i64_is(pget(params, "neg"), -3));
    CF_CHECK(kind_is(pget(params, "f"), CF_PARAM_NUMBER));
    CF_CHECK(cf_param_i64(pget(params, "f"), &(cf_optional_i64){0}) == CF_INVALID); /* 1.5 */
    CF_CHECK(i64_is(pget(params, "exp"), 100));    /* 1e2 is integral, exactly */
    CF_CHECK(i64_is(pget(params, "one"), 1));      /* 1.0 is integral */
    CF_CHECK(i64_is(pget(params, "big"), 9007199254740993LL)); /* no double truncation */
    CF_CHECK(cf_param_i64(pget(params, "oob"), &(cf_optional_i64){0}) == CF_INVALID);
    CF_CHECK(i64_is(pget(params, "min"), INT64_MIN));
    CF_CHECK(i64_is(pget(params, "max"), INT64_MAX));
    CF_CHECK(cf_param_i64(pget(params, "small"), &(cf_optional_i64){0}) == CF_INVALID);
    CF_CHECK(i64_kind_present_false(pget(params, "z")));
    CF_CHECK(cf_param_i64(pget(params, "t"), &(cf_optional_i64){0}) == CF_INVALID);
    CF_CHECK(cf_param_i64(pget(params, "str"), &(cf_optional_i64){0}) == CF_INVALID);
    CF_CHECK(cf_param_bool(pget(params, "t"), &(bool){0}, &(bool){0}) == CF_OK);
    CF_CHECK(bool_is(pget(params, "t"), true));

    cf_params_destroy(params);
}

CF_TEST(accessor_json_bool_matrix) {
    struct reqb b;
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/json");
    reqb_body(&b, "{\"yes\":true,\"no\":false,\"z\":null,\"n\":1,\"s\":\"true\"}");
    cf_params *params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);

    CF_CHECK(bool_is(pget(params, "yes"), true));
    CF_CHECK(bool_is(pget(params, "no"), false));
    CF_CHECK(bool_null(pget(params, "z")));
    CF_CHECK(cf_param_bool(pget(params, "n"), &(bool){0}, &(bool){0}) == CF_INVALID);
    CF_CHECK(cf_param_bool(pget(params, "s"), &(bool){0}, &(bool){0}) == CF_INVALID);
    CF_CHECK(cf_param_bool(pget(params, "yes"), &(bool){0}, NULL) == CF_INVALID);
    CF_CHECK(cf_param_bool(pget(params, "yes"), NULL, &(bool){0}) == CF_INVALID);

    cf_params_destroy(params);
}

CF_TEST(accessor_count_at_field) {
    struct reqb b;
    reqb_init(&b, CF_GET, CF_GET);
    reqb_query(&b, "a[]=1&a[]=2&a[]=3&o[x]=1&o[y]=2&s=v");
    cf_params *params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);

    const cf_param *a = pget(params, "a");
    CF_REQUIRE(a != NULL);
    CF_CHECK(kind_is(a, CF_PARAM_ARRAY));
    CF_CHECK(cf_param_count(a) == 3);
    CF_CHECK(str_is(cf_param_at(a, 0), "1"));
    CF_CHECK(str_is(cf_param_at(a, 1), "2"));
    CF_CHECK(str_is(cf_param_at(a, 2), "3"));
    CF_CHECK(cf_param_at(a, 3) == NULL);
    CF_CHECK(pfield(a, "0") == NULL); /* arrays use at(), not field() */
    CF_CHECK(cf_param_string(a, &(cf_span){0}) == CF_INVALID);

    const cf_param *o = pget(params, "o");
    CF_REQUIRE(o != NULL);
    CF_CHECK(kind_is(o, CF_PARAM_OBJECT));
    CF_CHECK(cf_param_count(o) == 2);
    CF_CHECK(str_is(pfield(o, "x"), "1"));
    CF_CHECK(str_is(pfield(o, "y"), "2"));
    CF_CHECK(pfield(o, "z") == NULL);
    CF_CHECK(cf_param_at(o, 0) == NULL); /* objects use field(), not at() */

    const cf_param *s = pget(params, "s");
    CF_CHECK(cf_param_count(s) == 0);
    CF_CHECK(cf_param_at(s, 0) == NULL);

    cf_params_destroy(params);
}

CF_TEST(form_body_content_type_dispatch) {
    /* urlencoded with parameters */
    struct reqb b;
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded; charset=utf-8");
    reqb_body(&b, "a=1&b=2&a=3");
    cf_params *params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(str_is(pget(params, "a"), "3"));
    CF_CHECK(str_is(pget(params, "b"), "2"));
    cf_params_destroy(params);

    /* no content type + POST: Rack form_data? treats it as a form */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_body(&b, "a=1");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(str_is(pget(params, "a"), "1"));
    cf_params_destroy(params);

    /* no content type + GET: body is left for the action */
    reqb_init(&b, CF_GET, CF_GET);
    reqb_body(&b, "a=1");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(absent(params, "a"));
    cf_params_destroy(params);

    /* urlencoded body on GET still parses (reference form_pairs) */
    reqb_init(&b, CF_GET, CF_GET);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "a=1");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(str_is(pget(params, "a"), "1"));
    cf_params_destroy(params);

    /* other content types are not parameters */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "text/plain");
    reqb_body(&b, "a=1");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(absent(params, "a"));
    cf_params_destroy(params);

    /* whitespace-only content type is present but media-less: not form data */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "   ");
    reqb_body(&b, "a=1");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(absent(params, "a"));
    cf_params_destroy(params);

    /* an empty content type behaves like none for POST */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "");
    reqb_body(&b, "a=1");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(str_is(pget(params, "a"), "1"));
    cf_params_destroy(params);

    /* one trailing NUL is dropped (Safari) */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body_span(&b, B("a=1\0", 4));
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(str_is(pget(params, "a"), "1"));
    cf_params_destroy(params);
}

CF_TEST(form_decoding_and_malformed) {
    struct reqb b;
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "x=a+b&y=%2B&a%20b=1&a%2Bb=2");
    cf_params *params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(str_is(pget(params, "x"), "a b"));          /* '+' is space */
    CF_CHECK(str_is(pget(params, "y"), "+"));            /* %2B is a literal plus */
    CF_CHECK(str_is(pget(params, "a b"), "1"));
    CF_CHECK(str_is(pget(params, "a+b"), "2"));
    cf_params_destroy(params);

    /* invalid %-encoding, invalid decoded UTF-8, raw invalid UTF-8 */
    const char *bad[] = {"a=%", "a=%zz", "a=%FF", "%FF=1", "a=ok&b=%q1"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reqb_init(&b, CF_POST, CF_POST);
        reqb_ct(&b, "application/x-www-form-urlencoded");
        reqb_body(&b, bad[i]);
        params = NULL;
        CF_CHECK(parse(&b.r, &params) == CF_INVALID);
        CF_CHECK(params == NULL); /* out stays empty */
        cf_params_destroy(params);
    }

    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body_span(&b, B("a=\xFF", 3));
    params = NULL;
    CF_CHECK(parse(&b.r, &params) == CF_INVALID);
    CF_CHECK(params == NULL);
    cf_params_destroy(params);

    /* an empty top-level key is skipped before its value is validated */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "=%FF");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(cf_param_count(NULL) == 0);
    cf_params_destroy(params);
}

CF_TEST(malformed_shapes_do_not_alias_keys) {
    struct reqb b;
    cf_params *params = NULL;

    /* a scalar later replaces a hash: allowed, no error, no stale child */
    reqb_init(&b, CF_GET, CF_GET);
    reqb_query(&b, "a[b]=1&a=2");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(str_is(pget(params, "a"), "2"));
    CF_CHECK(pfield(pget(params, "a"), "b") == NULL);
    CF_CHECK(absent(params, "a[b]"));
    cf_params_destroy(params);

    /* nil is replaced by structure (Ruby ||=) */
    reqb_init(&b, CF_GET, CF_GET);
    reqb_query(&b, "a&a[b]=1");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(kind_is(pget(params, "a"), CF_PARAM_OBJECT));
    CF_CHECK(str_is(pfield(pget(params, "a"), "b"), "1"));
    cf_params_destroy(params);

    /* wrong-kind nesting is CF_INVALID, and no partial tree escapes */
    const char *type_conflicts[] = {
        "a=1&a[]=2", "a=1&a[b]=2", "a[]=1&a[b]=2", "a[b]=1&a[]=2",
        "a[b]=1&a[b][c]=2", "a[][b]=1&a[][b][c]=2",
    };
    for (size_t i = 0; i < sizeof(type_conflicts) / sizeof(type_conflicts[0]); i++) {
        reqb_init(&b, CF_GET, CF_GET);
        reqb_query(&b, type_conflicts[i]);
        params = NULL;
        CF_CHECK(parse(&b.r, &params) == CF_INVALID);
        CF_CHECK(params == NULL);
        cf_params_destroy(params);
    }
}

CF_TEST(bounds_depth_and_nodes) {
    struct reqb b;
    cf_params *params = NULL;

    /* depth 32: 31 bracket levels are stored, 32 are rejected */
    size_t ok_len = 1 + 31 * 3 + 2 + 1;
    char *ok = alloc_text(ok_len);
    size_t w = 0;
    ok[w++] = 'a';
    for (int i = 0; i < 31; i++) { ok[w++] = '['; ok[w++] = 'b'; ok[w++] = ']'; }
    ok[w++] = '=';
    ok[w++] = '1';
    ok[w] = '\0';
    reqb_init(&b, CF_GET, CF_GET);
    reqb_query(&b, ok);
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    const cf_param *deep = pget(params, "a");
    for (int i = 0; i < 31; i++) {
        CF_REQUIRE(kind_is(deep, CF_PARAM_OBJECT));
        deep = pfield(deep, "b");
    }
    CF_CHECK(str_is(deep, "1"));
    cf_params_destroy(params);
    free(ok);

    size_t bad_len = 1 + 32 * 3 + 2 + 1;
    char *bad = alloc_text(bad_len);
    w = 0;
    bad[w++] = 'a';
    for (int i = 0; i < 32; i++) { bad[w++] = '['; bad[w++] = 'b'; bad[w++] = ']'; }
    bad[w++] = '=';
    bad[w++] = '1';
    bad[w] = '\0';
    reqb_init(&b, CF_GET, CF_GET);
    reqb_query(&b, bad);
    params = NULL;
    CF_CHECK(parse(&b.r, &params) == CF_LIMIT);
    CF_CHECK(params == NULL);
    cf_params_destroy(params);
    free(bad);

    /* 4096 value nodes fit, 4097 are rejected before growing the arena */
    for (size_t count = 4096; count <= 4097; count++) {
        size_t need = count * 8 + 1;
        char *pairs = alloc_text(need);
        w = 0;
        for (size_t i = 0; i < count; i++) {
            int n = snprintf(pairs + w, need - w, i == 0 ? "k%zu=1" : "&k%zu=1", i);
            w += (size_t)n;
        }
        struct reqb nb;
        reqb_init(&nb, CF_GET, CF_GET);
        nb.r.query = B(pairs, w);
        params = NULL;
        cf_err err = parse(&nb.r, &params);
        if (count == 4096) {
            CF_CHECK(err == CF_OK);
            CF_CHECK(params != NULL);
            if (params != NULL) CF_CHECK(str_is(pget(params, "k4095"), "1"));
        } else {
            CF_CHECK(err == CF_LIMIT);
            CF_CHECK(params == NULL);
        }
        cf_params_destroy(params);
        free(pairs);
    }
}

CF_TEST(precedence_body_then_query_then_path) {
    /* cf_params_parse: body first, query overrides (Rails merge!) */
    struct reqb b;
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "k=body&only=body");
    reqb_query(&b, "k=query&q=1");
    cf_params *merged = NULL;
    CF_REQUIRE(parse(&b.r, &merged) == CF_OK);
    CF_CHECK(str_is(pget(merged, "k"), "query"));
    CF_CHECK(str_is(pget(merged, "only"), "body"));
    CF_CHECK(str_is(pget(merged, "q"), "1"));

    /* A00 merges path parameters last. Path values are not form-decoded:
     * a '+' captured from the path stays literal (H03 owns path decoding). */
    struct reqb p;
    reqb_init(&p, CF_POST, CF_POST);
    reqb_ct(&p, "application/json");
    reqb_body(&p, "{\"k\":\"path+literal\",\"extra\":\"p\"}");
    cf_params *path = NULL;
    CF_REQUIRE(parse(&p.r, &path) == CF_OK);
    CF_REQUIRE(cf_params_merge(merged, path) == CF_OK);
    CF_CHECK(str_is(pget(merged, "k"), "path+literal")); /* merge copies bytes */
    CF_CHECK(str_is(pget(merged, "extra"), "p"));
    CF_CHECK(str_is(pget(merged, "only"), "body"));
    cf_params_destroy(path); /* merged owns its copy */

    /* The same text through a form source decodes '+' as space. */
    reqb_init(&b, CF_GET, CF_GET);
    reqb_query(&b, "k=path+literal");
    cf_params *form = NULL;
    CF_REQUIRE(parse(&b.r, &form) == CF_OK);
    CF_CHECK(str_is(pget(form, "k"), "path literal"));
    cf_params_destroy(form);

    /* Merge deep-copies: replacing a scalar with a hash works and survives. */
    struct reqb p2;
    reqb_init(&p2, CF_POST, CF_POST);
    reqb_ct(&p2, "application/json");
    reqb_body(&p2, "{\"k\":{\"n\":1}}");
    cf_params *path2 = NULL;
    CF_REQUIRE(parse(&p2.r, &path2) == CF_OK);
    CF_REQUIRE(cf_params_merge(merged, path2) == CF_OK);
    cf_params_destroy(path2);
    CF_CHECK(kind_is(pget(merged, "k"), CF_PARAM_OBJECT));
    CF_CHECK(i64_is(pfield(pget(merged, "k"), "n"), 1));

    CF_CHECK(cf_params_merge(NULL, merged) == CF_INVALID);
    CF_CHECK(cf_params_merge(merged, NULL) == CF_INVALID);
    cf_params_destroy(merged);
}

CF_TEST(merge_shallow_top_level_and_full_depth) {
    struct reqb b;
    cf_params *params = NULL;
    const cf_param *a;

    /* cf_params_parse follows Ctx::new (kit/src/ctx.rs): the query tree is
     * parsed independently and merged over the body tree top-level by
     * top-level (ParamMap::merge = Ruby Hash#merge!), so a query value
     * replaces the whole body value. There is no recursive merge. */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "a[b]=1&only=body");
    reqb_query(&b, "a=2");
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(str_is(pget(params, "a"), "2")); /* not {"b":"1", ...} */
    CF_CHECK(str_is(pget(params, "only"), "body"));
    cf_params_destroy(params);

    /* nested body and nested query under the same key: the query object
     * replaces the body object, it does not merge key-by-key with it */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "a[b]=1");
    reqb_query(&b, "a[c]=2");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    a = pget(params, "a");
    CF_REQUIRE(kind_is(a, CF_PARAM_OBJECT));
    CF_CHECK(str_is(pfield(a, "c"), "2"));
    CF_CHECK(pfield(a, "b") == NULL); /* body subkey is not deep-merged in */
    cf_params_destroy(params);

    /* the mirror case: a nested query value replaces a plain body value */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "a=1");
    reqb_query(&b, "a[b]=2");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    a = pget(params, "a");
    CF_REQUIRE(kind_is(a, CF_PARAM_OBJECT));
    CF_CHECK(str_is(pfield(a, "b"), "2"));
    CF_CHECK(absent(params, "a[b]")); /* bracket text is never a flat key */
    cf_params_destroy(params);

    /* deeper nested query replaces a nested body value whole */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "a[b]=1");
    reqb_query(&b, "a[b][c]=2");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    a = pget(params, "a");
    CF_REQUIRE(kind_is(a, CF_PARAM_OBJECT));
    CF_CHECK(pfield(a, "b") != NULL && kind_is(pfield(a, "b"), CF_PARAM_OBJECT));
    CF_CHECK(str_is(pfield(pfield(a, "b"), "c"), "2"));
    cf_params_destroy(params);

    /* arrays are replaced whole too */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "a[]=1&a[]=2");
    reqb_query(&b, "a[]=3");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    a = pget(params, "a");
    CF_REQUIRE(kind_is(a, CF_PARAM_ARRAY));
    CF_CHECK(cf_param_count(a) == 1);
    CF_CHECK(str_is(cf_param_at(a, 0), "3"));
    cf_params_destroy(params);

    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "a[]=1");
    reqb_query(&b, "a[b]=2");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    a = pget(params, "a");
    CF_REQUIRE(kind_is(a, CF_PARAM_OBJECT));
    CF_CHECK(str_is(pfield(a, "b"), "2"));
    cf_params_destroy(params);

    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "a[b]=1");
    reqb_query(&b, "a[]=2");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    a = pget(params, "a");
    CF_REQUIRE(kind_is(a, CF_PARAM_ARRAY));
    CF_CHECK(cf_param_count(a) == 1);
    CF_CHECK(str_is(cf_param_at(a, 0), "2"));
    cf_params_destroy(params);

    /* a JSON object member is replaced whole as well */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/json");
    reqb_body(&b, "{\"a\":{\"x\":1},\"keep\":true}");
    reqb_query(&b, "a[y]=2");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    a = pget(params, "a");
    CF_REQUIRE(kind_is(a, CF_PARAM_OBJECT));
    CF_CHECK(str_is(pfield(a, "y"), "2"));
    CF_CHECK(pfield(a, "x") == NULL); /* the body member is gone, not merged in */
    CF_CHECK(bool_is(pget(params, "keep"), true));
    cf_params_destroy(params);

    /* absent vs null across the merge */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "a=1&n");
    reqb_query(&b, "b=2");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(str_is(pget(params, "a"), "1"));  /* source key absent: no write */
    CF_CHECK(kind_is(pget(params, "n"), CF_PARAM_NULL)); /* body null kept */
    CF_CHECK(str_is(pget(params, "b"), "2"));
    cf_params_destroy(params);

    /* a query null replaces a body value (and a whole body structure) */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "a=1&b");
    reqb_query(&b, "a&b");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(kind_is(pget(params, "a"), CF_PARAM_NULL));
    CF_CHECK(kind_is(pget(params, "b"), CF_PARAM_NULL));
    cf_params_destroy(params);

    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/json");
    reqb_body(&b, "{\"a\":{\"x\":1}}");
    reqb_query(&b, "a");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(kind_is(pget(params, "a"), CF_PARAM_NULL));
    cf_params_destroy(params);

    /* a body null is replaced by a query structure */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "a&keep=1");
    reqb_query(&b, "a[b]=1");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    a = pget(params, "a");
    CF_REQUIRE(kind_is(a, CF_PARAM_OBJECT));
    CF_CHECK(str_is(pfield(a, "b"), "1"));
    CF_CHECK(str_is(pget(params, "keep"), "1"));
    cf_params_destroy(params);

    /* cf_params_merge is directional: the source value replaces the target
     * value whole. cf_params_parse can only produce query-over-body (body is
     * parsed first); A00 uses the same helper for path over body/query, and
     * these direct calls pin both directions. */
    struct reqb tb, sb;
    reqb_init(&tb, CF_POST, CF_POST);
    reqb_ct(&tb, "application/x-www-form-urlencoded");
    reqb_body(&tb, "a=1&keep=body");
    cf_params *target = NULL;
    CF_REQUIRE(parse(&tb.r, &target) == CF_OK);
    reqb_init(&sb, CF_POST, CF_POST);
    reqb_ct(&sb, "application/x-www-form-urlencoded");
    reqb_body(&sb, "a[b]=2");
    cf_params *source = NULL;
    CF_REQUIRE(parse(&sb.r, &source) == CF_OK);
    CF_REQUIRE(cf_params_merge(target, source) == CF_OK);
    a = pget(target, "a");
    CF_REQUIRE(kind_is(a, CF_PARAM_OBJECT));
    CF_CHECK(str_is(pfield(a, "b"), "2"));
    CF_CHECK(str_is(pget(target, "keep"), "body"));
    cf_params_destroy(source);

    /* the reverse direction: a plain source replaces a nested target value */
    reqb_init(&sb, CF_POST, CF_POST);
    reqb_ct(&sb, "application/x-www-form-urlencoded");
    reqb_body(&sb, "a=3");
    source = NULL;
    CF_REQUIRE(parse(&sb.r, &source) == CF_OK);
    CF_REQUIRE(cf_params_merge(target, source) == CF_OK);
    CF_CHECK(str_is(pget(target, "a"), "3"));
    cf_params_destroy(source);

    /* a null source replaces a structure target */
    reqb_init(&sb, CF_POST, CF_POST);
    reqb_ct(&sb, "application/x-www-form-urlencoded");
    reqb_body(&sb, "a");
    source = NULL;
    CF_REQUIRE(parse(&sb.r, &source) == CF_OK);
    CF_REQUIRE(cf_params_merge(target, source) == CF_OK);
    CF_CHECK(kind_is(pget(target, "a"), CF_PARAM_NULL));
    cf_params_destroy(source);
    cf_params_destroy(target);

    /* 31 bracket levels parse (depth-32 bound) and merge: the copy carries
     * the same depth convention (top-level value at depth 0). This returned
     * CF_LIMIT before; 32 levels still fail at parse (bounds test). */
    size_t deep_len = 1 + 31 * 3 + 2 + 1;
    char *deep_key = alloc_text(deep_len);
    size_t w = 0;
    deep_key[w++] = 'a';
    for (int i = 0; i < 31; i++) { deep_key[w++] = '['; deep_key[w++] = 'b'; deep_key[w++] = ']'; }
    deep_key[w++] = '=';
    deep_key[w++] = '1';
    deep_key[w] = '\0';
    reqb_init(&b, CF_GET, CF_GET);
    reqb_query(&b, deep_key);
    cf_params *deep_src = NULL;
    CF_REQUIRE(parse(&b.r, &deep_src) == CF_OK);

    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "a=old&z=9");
    target = NULL;
    CF_REQUIRE(parse(&b.r, &target) == CF_OK);
    CF_REQUIRE(cf_params_merge(target, deep_src) == CF_OK);
    cf_params_destroy(deep_src); /* the merge owns a deep copy */
    a = pget(target, "a");
    for (int i = 0; i < 31; i++) {
        CF_REQUIRE(kind_is(a, CF_PARAM_OBJECT));
        a = pfield(a, "b");
    }
    CF_CHECK(str_is(a, "1"));
    CF_CHECK(str_is(pget(target, "z"), "9"));
    cf_params_destroy(target);
    free(deep_key);

    /* the 4096-node bound is checked against the merged tree: query copies
     * count against the body tree's budget, as when both were parsed into one
     * tree. 4095 body nodes + 1 query node fit; 4096 + 1 is CF_LIMIT. */
    for (size_t body_nodes = 4095; body_nodes <= 4096; body_nodes++) {
        size_t need = body_nodes * 9 + 1;
        char *pairs = alloc_text(need);
        size_t used = 0;
        for (size_t i = 0; i < body_nodes; i++) {
            int n = snprintf(pairs + used, need - used, i == 0 ? "k%zu=1" : "&k%zu=1", i);
            used += (size_t)n;
        }
        struct reqb nb;
        reqb_init(&nb, CF_POST, CF_POST);
        reqb_ct(&nb, "application/x-www-form-urlencoded");
        nb.r.body = B(pairs, used);
        reqb_query(&nb, "q=1");
        params = NULL;
        cf_err e = parse(&nb.r, &params);
        if (body_nodes == 4095) {
            CF_CHECK(e == CF_OK);
            CF_CHECK(params != NULL);
            if (params != NULL) {
                CF_CHECK(str_is(pget(params, "q"), "1"));
                CF_CHECK(str_is(pget(params, "k4094"), "1"));
            }
        } else {
            CF_CHECK(e == CF_LIMIT);
            CF_CHECK(params == NULL);
        }
        cf_params_destroy(params);
        free(pairs);
    }
}

CF_TEST(path_span_is_never_parameterized) {
    /* cf_params_parse has no path-parameter input: req->path stays literal
     * and H03's captures are merged by A00 via cf_params_merge. */
    struct reqb b;
    reqb_init(&b, CF_GET, CF_GET);
    b.r.path = S("/a+b/c");
    reqb_query(&b, "z=1");
    cf_params *params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(absent(params, "a+b"));
    CF_CHECK(absent(params, "a b"));
    CF_CHECK(absent(params, "c"));
    CF_CHECK(str_is(pget(params, "z"), "1"));
    cf_params_destroy(params);
}

CF_TEST(json_shapes_and_deep_munge) {
    struct reqb b;
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/json");
    reqb_body(&b, "{\"a\":[1,null,\"x\"],\"b\":{\"c\":null},\"d\":true}");
    cf_params *params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    const cf_param *a = pget(params, "a");
    CF_REQUIRE(kind_is(a, CF_PARAM_ARRAY));
    CF_CHECK(cf_param_count(a) == 2); /* NoNilParamEncoder drops null elements */
    CF_CHECK(i64_is(cf_param_at(a, 0), 1));
    CF_CHECK(str_is(cf_param_at(a, 1), "x"));
    const cf_param *bc = pfield(pget(params, "b"), "c");
    CF_CHECK(kind_is(bc, CF_PARAM_NULL)); /* object nulls are kept */
    CF_CHECK(bool_is(pget(params, "d"), true));
    cf_params_destroy(params);

    /* non-object documents wrap in _json (with the deep munge applied) */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/json");
    reqb_body(&b, "[1,null,2]");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    const cf_param *wrapped = pget(params, "_json");
    CF_REQUIRE(kind_is(wrapped, CF_PARAM_ARRAY));
    CF_CHECK(cf_param_count(wrapped) == 2);
    cf_params_destroy(params);

    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/json");
    reqb_body(&b, "\"str\"");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(str_is(pget(params, "_json"), "str"));
    cf_params_destroy(params);

    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/json");
    reqb_body(&b, "null");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(kind_is(pget(params, "_json"), CF_PARAM_NULL));
    cf_params_destroy(params);

    /* duplicate keys: last value wins, like serde_json preserve_order */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/json");
    reqb_body(&b, "{\"b\":1,\"a\":2,\"b\":3}");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(i64_is(pget(params, "b"), 3));
    CF_CHECK(i64_is(pget(params, "a"), 2));
    cf_params_destroy(params);

    /* JSON synonyms and charset parameters */
    const char *json_cts[] = {"application/json; charset=utf-8", "text/x-json",
                              "application/jsonrequest", "application/problem+json"};
    for (size_t i = 0; i < sizeof(json_cts) / sizeof(json_cts[0]); i++) {
        reqb_init(&b, CF_POST, CF_POST);
        reqb_ct(&b, json_cts[i]);
        reqb_body(&b, "{\"a\":1}");
        params = NULL;
        CF_REQUIRE(parse(&b.r, &params) == CF_OK);
        CF_CHECK(i64_is(pget(params, "a"), 1));
        cf_params_destroy(params);
    }

    /* empty JSON body: no parameters, no error (body.rs is_json && !empty) */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/json");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(absent(params, "_json"));
    cf_params_destroy(params);
}

CF_TEST(json_strictness) {
    struct reqb b;
    cf_params *params = NULL;
    const char *bad[] = {
        "{\"a\":",           /* truncated */
        "{\"a\":NaN}",       /* NaN literal */
        "{\"a\":Infinity}",  /* Infinity literal */
        "{\"a\":-Infinity}",
        "{\"a\":1e999}",     /* double overflow */
        "{\"a\":01}",        /* bad number */
        "{\"a\":\"\\uD800\"}", /* lone surrogate */
        " ",                 /* whitespace is not empty */
        "{\"a\":1}",         /* trailing roots are handled below too */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]) - 1; i++) {
        reqb_init(&b, CF_POST, CF_POST);
        reqb_ct(&b, "application/json");
        reqb_body(&b, bad[i]);
        params = NULL;
        CF_CHECK(parse(&b.r, &params) == CF_INVALID);
        CF_CHECK(params == NULL);
        cf_params_destroy(params);
    }
    /* a second root document is malformed for a strict reader */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/json");
    reqb_body(&b, "{\"a\":1} {\"b\":2}");
    params = NULL;
    CF_CHECK(parse(&b.r, &params) == CF_INVALID);
    cf_params_destroy(params);

    /* invalid UTF-8 inside a JSON string is rejected by yyjson */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/json");
    reqb_body_span(&b, B("{\"a\":\"\xFF\"}", 10));
    params = NULL;
    CF_CHECK(parse(&b.r, &params) == CF_INVALID);
    cf_params_destroy(params);

    /* deep JSON is bounded by the same depth 32 (a scalar inside n nested
     * containers sits at depth n) */
    for (int depth = 31; depth <= 32; depth++) {
        size_t need = (size_t)depth * 2 + 2;
        char *doc = alloc_text(need);
        size_t w = 0;
        for (int i = 0; i < depth; i++) doc[w++] = '[';
        doc[w++] = '1';
        for (int i = 0; i < depth; i++) doc[w++] = ']';
        doc[w] = '\0';
        reqb_init(&b, CF_POST, CF_POST);
        reqb_ct(&b, "application/json");
        reqb_body(&b, doc);
        params = NULL;
        cf_err err = parse(&b.r, &params);
        if (depth == 31) {
            CF_CHECK(err == CF_OK);
        } else {
            CF_CHECK(err == CF_LIMIT);
            CF_CHECK(params == NULL);
        }
        cf_params_destroy(params);
        free(doc);
    }

    /* JSON key count is bounded by the node limit */
    {
        size_t count = 4097;
        size_t need = count * 12 + 4;
        char *doc = alloc_text(need);
        size_t w = 0;
        doc[w++] = '{';
        for (size_t i = 0; i < count; i++) {
            int n = snprintf(doc + w, need - w, i == 0 ? "\"k%zu\":1" : ",\"k%zu\":1", i);
            w += (size_t)n;
        }
        doc[w++] = '}';
        doc[w] = '\0';
        reqb_init(&b, CF_POST, CF_POST);
        reqb_ct(&b, "application/json");
        reqb_body(&b, doc);
        params = NULL;
        CF_CHECK(parse(&b.r, &params) == CF_LIMIT);
        CF_CHECK(params == NULL);
        cf_params_destroy(params);
        free(doc);
    }
}

/* --------------------------------------------------------- multipart */

struct mbuf {
    char *p;
    size_t len, cap;
    const char *boundary;
};

static void mb_need(struct mbuf *m, size_t extra) {
    if (m->len + extra + 1 <= m->cap) return;
    size_t ncap = m->cap ? m->cap * 2 : 256;
    while (ncap < m->len + extra + 1) ncap *= 2;
    char *np = realloc(m->p, ncap);
    if (np == NULL) abort();
    m->p = np;
    m->cap = ncap;
}

static void mb_addb(struct mbuf *m, const void *s, size_t n) {
    mb_need(m, n);
    memcpy(m->p + m->len, s, n);
    m->len += n;
    m->p[m->len] = '\0';
}

static void mb_add(struct mbuf *m, const char *s) { mb_addb(m, s, strlen(s)); }

static void mb_start(struct mbuf *m, const char *boundary) {
    memset(m, 0, sizeof(*m));
    m->boundary = boundary;
    mb_add(m, "--");
    mb_add(m, boundary);
    mb_add(m, "\r\n");
}

/* Start the next part: call between parts, never after the last one. */
static void mb_next(struct mbuf *m) {
    mb_add(m, "--");
    mb_add(m, m->boundary);
    mb_add(m, "\r\n");
}

static void mb_field(struct mbuf *m, const char *name, const char *value) {
    mb_add(m, "Content-Disposition: form-data; name=\"");
    mb_add(m, name);
    mb_add(m, "\"\r\n\r\n");
    mb_add(m, value);
    mb_add(m, "\r\n");
}

static void mb_part_raw(struct mbuf *m, const char *headers, const char *value) {
    mb_add(m, headers);
    mb_add(m, "\r\n\r\n"); /* headers (no trailing CRLF) + blank line */
    mb_add(m, value);
    mb_add(m, "\r\n");
}

static void mb_finish(struct mbuf *m) {
    mb_add(m, "--");
    mb_add(m, m->boundary);
    mb_add(m, "--\r\n");
}

CF_TEST(multipart_fields_and_structure) {
    struct mbuf m;
    mb_start(&m, "B");
    mb_field(&m, "plain", "x");
    mb_next(&m);
    mb_field(&m, "message[body]", "hello");
    mb_next(&m);
    mb_field(&m, "tags[]", "a");
    mb_next(&m);
    mb_field(&m, "tags[]", "b");
    mb_next(&m);
    mb_field(&m, "q", "a+b"); /* multipart values are not form-decoded */
    mb_next(&m);
    mb_part_raw(&m, "Content-Disposition: form-data; name=\"skip\"; filename=\"\"", "ignored");
    mb_finish(&m);

    struct reqb b;
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "multipart/form-data; boundary=B");
    reqb_body_span(&b, B(m.p, m.len));
    cf_params *params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(str_is(pget(params, "plain"), "x"));
    CF_CHECK(str_is(pget(params, "q"), "a+b"));
    CF_CHECK(str_is(pfield(pget(params, "message"), "body"), "hello"));
    const cf_param *tags = pget(params, "tags");
    CF_REQUIRE(kind_is(tags, CF_PARAM_ARRAY));
    CF_CHECK(cf_param_count(tags) == 2);
    CF_CHECK(str_is(cf_param_at(tags, 0), "a"));
    CF_CHECK(absent(params, "skip")); /* blank filename is dropped, not stored */
    cf_params_destroy(params);
    free(m.p);

    /* quoted boundary parameter */
    struct mbuf q;
    mb_start(&q, "XyZ");
    mb_field(&q, "a", "1");
    mb_finish(&q);
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "multipart/form-data; charset=utf-8; boundary=\"XyZ\"");
    reqb_body_span(&b, B(q.p, q.len));
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(str_is(pget(params, "a"), "1"));
    cf_params_destroy(params);
    free(q.p);

    /* LF-only framing is accepted, like Rack's parser */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "multipart/form-data; boundary=L");
    reqb_body(&b, "--L\nContent-Disposition: form-data; name=\"a\"\n\n1\n--L--\n");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(str_is(pget(params, "a"), "1"));
    cf_params_destroy(params);
}

CF_TEST(multipart_upload_rejected_until_s01) {
    struct mbuf m;
    mb_start(&m, "B");
    mb_field(&m, "note", "n");
    mb_next(&m);
    mb_part_raw(&m, "Content-Disposition: form-data; name=\"file\"; filename=\"cat.png\"\r\n"
                     "Content-Type: image/png", "bytes");
    mb_finish(&m);

    struct reqb b;
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "multipart/form-data; boundary=B");
    reqb_body_span(&b, B(m.p, m.len));
    cf_params *params = NULL;
    /* H02 rejects upload staging explicitly; S01 lands file parts. */
    CF_CHECK(parse(&b.r, &params) == CF_INVALID);
    CF_CHECK(params == NULL);
    cf_params_destroy(params);
    free(m.p);

    /* more than 16 file parts trips the fixed file bound first */
    mb_start(&m, "B");
    for (int i = 0; i < 17; i++) {
        if (i > 0) mb_next(&m);
        mb_part_raw(&m, "Content-Disposition: form-data; name=\"x\"; filename=\"a.bin\"", "x");
    }
    mb_finish(&m);
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "multipart/form-data; boundary=B");
    reqb_body_span(&b, B(m.p, m.len));
    params = NULL;
    CF_CHECK(parse(&b.r, &params) == CF_LIMIT);
    CF_CHECK(params == NULL);
    cf_params_destroy(params);
    free(m.p);

    /* 100 fields fit, 101 are rejected before storage grows */
    for (size_t count = 100; count <= 101; count++) {
        mb_start(&m, "B");
        for (size_t i = 0; i < count; i++) {
            char name[16];
            snprintf(name, sizeof(name), "f%zu", i);
            if (i > 0) mb_next(&m);
            mb_field(&m, name, "1");
        }
        mb_finish(&m);
        reqb_init(&b, CF_POST, CF_POST);
        reqb_ct(&b, "multipart/form-data; boundary=B");
        reqb_body_span(&b, B(m.p, m.len));
        params = NULL;
        cf_err err = parse(&b.r, &params);
        if (count == 100) {
            CF_CHECK(err == CF_OK);
            CF_CHECK(str_is(pget(params, "f99"), "1"));
        } else {
            CF_CHECK(err == CF_LIMIT);
            CF_CHECK(params == NULL);
        }
        cf_params_destroy(params);
        free(m.p);
    }
}

CF_TEST(multipart_malformed) {
    struct reqb b;
    cf_params *params = NULL;

    /* no boundary means the reference falls through to form decoding */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "multipart/form-data");
    reqb_body(&b, "a=1");
    params = NULL;
    CF_REQUIRE(parse(&b.r, &params) == CF_OK);
    CF_CHECK(str_is(pget(params, "a"), "1"));
    cf_params_destroy(params);

    /* unterminated part */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "multipart/form-data; boundary=B");
    reqb_body(&b, "--B\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\n1");
    params = NULL;
    CF_CHECK(parse(&b.r, &params) == CF_INVALID);
    CF_CHECK(params == NULL);
    cf_params_destroy(params);

    /* part without a usable name */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "multipart/form-data; boundary=B");
    reqb_body(&b, "--B\r\nContent-Type: text/plain\r\n\r\n1\r\n--B--\r\n");
    params = NULL;
    CF_CHECK(parse(&b.r, &params) == CF_INVALID);
    CF_CHECK(params == NULL);
    cf_params_destroy(params);

    /* invalid UTF-8 in a field value */
    {
        struct mbuf m;
        mb_start(&m, "B");
        mb_add(&m, "Content-Disposition: form-data; name=\"a\"\r\n\r\n");
        mb_addb(&m, "\xFF", 1);
        mb_add(&m, "\r\n");
        mb_finish(&m);
        reqb_init(&b, CF_POST, CF_POST);
        reqb_ct(&b, "multipart/form-data; boundary=B");
        reqb_body_span(&b, B(m.p, m.len));
        params = NULL;
        CF_CHECK(parse(&b.r, &params) == CF_INVALID);
        CF_CHECK(params == NULL);
        cf_params_destroy(params);
        free(m.p);
    }
}

/* ---------------------------------------------------- method override */

static int eff_is(const cf_request *r, cf_method want) {
    cf_method out = CF_OTHER;
    return cf_effective_method(r, &out) == CF_OK && out == want;
}

static cf_err eff_err(const cf_request *r) {
    cf_method out = CF_OTHER;
    return cf_effective_method(r, &out);
}

CF_TEST(method_override_matrix) {
    struct reqb b;

    /* form _method wins; case-insensitive; must be in the fixed list */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "_method=PUT&x=1");
    CF_CHECK(eff_is(&b.r, CF_PUT));

    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "_method=delete");
    CF_CHECK(eff_is(&b.r, CF_DELETE));

    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "_method=TRACE");
    CF_CHECK(eff_is(&b.r, CF_POST));

    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "_method=LINK");
    CF_CHECK(eff_is(&b.r, CF_OTHER)); /* allowed list has LINK/UNLINK */

    /* header fallback */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_header(&b, "X-HTTP-Method-Override", "patch");
    CF_CHECK(eff_is(&b.r, CF_PATCH));

    /* form candidate is present but unusable: header is NOT consulted */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "_method=TRACE");
    reqb_header(&b, "X-HTTP-Method-Override", "DELETE");
    CF_CHECK(eff_is(&b.r, CF_POST));

    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "_method="); /* empty string is present */
    reqb_header(&b, "X-HTTP-Method-Override", "DELETE");
    CF_CHECK(eff_is(&b.r, CF_POST));

    /* a bare `_method` decodes to null: not a string, header applies */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "_method&x=1");
    reqb_header(&b, "X-HTTP-Method-Override", "DELETE");
    CF_CHECK(eff_is(&b.r, CF_DELETE));

    /* non-string forms of _method do not block the header either */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "_method[]=PUT");
    reqb_header(&b, "X-HTTP-Method-Override", "DELETE");
    CF_CHECK(eff_is(&b.r, CF_DELETE));

    /* the query string never supplies _method */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_query(&b, "_method=DELETE");
    CF_CHECK(eff_is(&b.r, CF_POST));
    reqb_init(&b, CF_POST, CF_POST);
    reqb_query(&b, "_method=DELETE");
    reqb_header(&b, "X-HTTP-Method-Override", "PATCH");
    CF_CHECK(eff_is(&b.r, CF_PATCH));

    /* GET can never become a write; body and header are ignored */
    reqb_init(&b, CF_GET, CF_GET);
    reqb_header(&b, "X-HTTP-Method-Override", "DELETE");
    CF_CHECK(eff_is(&b.r, CF_GET));
    reqb_init(&b, CF_GET, CF_GET);
    reqb_body(&b, "_method=PUT");
    CF_CHECK(eff_is(&b.r, CF_GET));

    /* POST -> GET is in the reference allow-list */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_header(&b, "X-HTTP-Method-Override", "GET");
    CF_CHECK(eff_is(&b.r, CF_GET));

    /* an already-overridden request is idempotent */
    reqb_init(&b, CF_DELETE, CF_POST);
    reqb_header(&b, "X-HTTP-Method-Override", "PUT");
    CF_CHECK(eff_is(&b.r, CF_DELETE));

    /* header value validity: lowercase ok, spaces/controls not a token */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_header(&b, "X-HTTP-Method-Override", "PUT ");
    CF_CHECK(eff_is(&b.r, CF_POST));
    reqb_init(&b, CF_POST, CF_POST);
    reqb_header(&b, "X-HTTP-Method-Override", "PUT\t");
    CF_CHECK(eff_is(&b.r, CF_POST));

    CF_CHECK(cf_effective_method(NULL, &(cf_method){0}) == CF_INVALID);
    reqb_init(&b, CF_POST, CF_POST);
    CF_CHECK(cf_effective_method(&b.r, NULL) == CF_INVALID);
}

CF_TEST(method_override_media_types) {
    struct reqb b;

    /* no content type is form data */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_body(&b, "_method=PATCH");
    CF_CHECK(eff_is(&b.r, CF_PATCH));

    /* multipart is form data (fields only until S01) */
    struct mbuf m;
    mb_start(&m, "B");
    mb_field(&m, "_method", "DELETE");
    mb_finish(&m);
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "multipart/form-data; boundary=B");
    reqb_body_span(&b, B(m.p, m.len));
    CF_CHECK(eff_is(&b.r, CF_DELETE));
    free(m.p);

    /* a body under text/plain supplies nothing, header still applies */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "text/plain");
    reqb_body(&b, "_method=DELETE");
    CF_CHECK(eff_is(&b.r, CF_POST));
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "text/plain");
    reqb_body(&b, "_method=DELETE");
    reqb_header(&b, "X-HTTP-Method-Override", "PATCH");
    CF_CHECK(eff_is(&b.r, CF_PATCH));

    /* JSON bodies never supply _method, but the header fallback applies */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/json");
    reqb_body(&b, "{\"_method\":\"DELETE\"}");
    CF_CHECK(eff_is(&b.r, CF_POST));
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/json");
    reqb_body(&b, "{\"_method\":\"DELETE\"}");
    reqb_header(&b, "X-HTTP-Method-Override", "PATCH");
    CF_CHECK(eff_is(&b.r, CF_PATCH));

    /* method override parses the form body it reads: malformed form data is
     * a bad request before any override, as in adapter.rs */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "a=%zz");
    reqb_header(&b, "X-HTTP-Method-Override", "DELETE");
    CF_CHECK(eff_err(&b.r) == CF_INVALID);

    /* malformed JSON does not fail the override (JSON is not parsed here) */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/json");
    reqb_body(&b, "{\"_method\":");
    reqb_header(&b, "X-HTTP-Method-Override", "PATCH");
    CF_CHECK(eff_is(&b.r, CF_PATCH));

    /* the helper never mutates the request */
    reqb_init(&b, CF_POST, CF_POST);
    reqb_ct(&b, "application/x-www-form-urlencoded");
    reqb_body(&b, "_method=DELETE");
    cf_method out = CF_OTHER;
    CF_REQUIRE(cf_effective_method(&b.r, &out) == CF_OK);
    CF_CHECK(out == CF_DELETE);
    CF_CHECK(b.r.method == CF_POST);
    CF_CHECK(b.r.original_method == CF_POST);
}

CF_TEST_MAIN()
