/* H03 recognition (HTTP-07): all 111 pinned recognition vectors, the
 * finite matcher's adversarial cases, normalization, defaults, captures,
 * errors and the intentional /rooms/opens shadowing.
 *
 * The vector comparison mirrors controllers.rs's own test: controller and
 * action are part of path_params (as in path_params()) and are removed
 * before comparing with the artifact's map. */
#include "cf_test.h"

#include "routes.h"
#include "support/h03_test_util.h"

/* Read the H02 tree internals for this test only, to compare the exact
 * stored top-level key set (the header documents this for tree-shape
 * tests). */
#define CF_HTTP_PARAMS_INTERNALS
#include "http/params.h"

#include <yyjson.h>

#define RECOGNITION_JSON "docs/devel/implementation/contracts/route-recognition.json"
#define ROUTES_JSON "docs/devel/implementation/contracts/routes.json"

static const char *recognition_method_name(cf_method method) {
    switch (method) {
    case CF_GET: return "GET";
    case CF_HEAD: return "HEAD";
    case CF_POST: return "POST";
    case CF_PUT: return "PUT";
    case CF_PATCH: return "PATCH";
    case CF_DELETE: return "DELETE";
    default: return "?";
    }
}

static cf_method method_from_name(const char *name) {
    if (strcmp(name, "GET") == 0) return CF_GET;
    if (strcmp(name, "HEAD") == 0) return CF_HEAD;
    if (strcmp(name, "POST") == 0) return CF_POST;
    if (strcmp(name, "PUT") == 0) return CF_PUT;
    if (strcmp(name, "PATCH") == 0) return CF_PATCH;
    if (strcmp(name, "DELETE") == 0) return CF_DELETE;
    return CF_OTHER;
}

static cf_err recognize(const char *method, const char *path,
                        cf_route_match *match) {
    cf_request req;
    h03_req_init(&req);
    req.method = method_from_name(method);
    req.original_method = req.method;
    req.path = (cf_span){(const unsigned char *)path, strlen(path)};
    return cf_route_match_request(&req, match);
}

static bool param_string(const cf_params *params, const char *key,
                         const char *expected) {
    cf_span name = {(const unsigned char *)key, strlen(key)};
    const cf_param *p = cf_param_get(params, name);
    if (p == NULL) return false;
    cf_span value;
    if (cf_param_string(p, &value) != CF_OK) return false;
    size_t n = strlen(expected);
    return value.len == n && memcmp(value.ptr, expected, n) == 0;
}

static const struct cf_param_entry *find_entry(const cf_params *params,
                                               const char *key) {
    const struct cf_param *root = &params->root;
    size_t n = strlen(key);
    for (size_t i = 0; i < root->u.object.len; i++) {
        const struct cf_param_entry *e = &root->u.object.entries[i];
        if (e->key.len == n && memcmp(e->key.ptr, key, n) == 0) return e;
    }
    return NULL;
}

CF_TEST(recognition_vectors_match_the_artifact) {
    yyjson_doc *doc = yyjson_read_file(RECOGNITION_JSON, 0, NULL, NULL);
    CF_REQUIRE(doc != NULL);
    yyjson_val *vectors = yyjson_doc_get_root(doc);
    CF_REQUIRE(vectors != NULL && yyjson_is_arr(vectors));
    CF_REQUIRE(yyjson_arr_size(vectors) == 111);

    size_t index, max;
    yyjson_val *vec;
    yyjson_arr_foreach(vectors, index, max, vec) {
        const char *verb = yyjson_get_str(yyjson_obj_get(vec, "verb"));
        const char *path = yyjson_get_str(yyjson_obj_get(vec, "path"));
        yyjson_val *endpoint = yyjson_obj_get(vec, "endpoint");
        yyjson_val *params = yyjson_obj_get(vec, "params");
        CF_REQUIRE(verb != NULL && path != NULL && endpoint != NULL &&
                   params != NULL && yyjson_is_obj(params));

        cf_route_match match;
        memset(&match, 0, sizeof match);
        cf_err rc = recognize(verb, path, &match);
        if (yyjson_is_null(endpoint)) {
            /* rooms/settings is routed but its controller is missing; the
             * Rust test accepts a match there without comparing params. */
            if (rc == CF_OK) {
                const cf_route *row = cf_route_by_id(match.id);
                CF_REQUIRE(row != NULL);
                CF_CHECK(strcmp(row->endpoint, "rooms/settings#show") == 0);
            } else {
                CF_CHECK(rc == CF_NOT_FOUND);
            }
        } else {
            CF_REQUIRE(rc == CF_OK);
            const cf_route *row = cf_route_by_id(match.id);
            CF_REQUIRE(row != NULL);
            CF_CHECK(strcmp(row->endpoint, yyjson_get_str(endpoint)) == 0);
            CF_REQUIRE(match.path_params != NULL);
            size_t expected_keys = yyjson_obj_size(params) + 2;
            const struct cf_param *root = &match.path_params->root;
            CF_CHECK(root->u.object.len == expected_keys);
            yyjson_val *key, *value;
            size_t ki, kmax;
            yyjson_obj_foreach(params, ki, kmax, key, value) {
                CF_CHECK(param_string(match.path_params, yyjson_get_str(key),
                                      yyjson_get_str(value)));
                CF_CHECK(find_entry(match.path_params,
                                    yyjson_get_str(key)) != NULL);
            }
            /* controller/action come from the artifact endpoint. */
            const char *hash = strchr(row->endpoint, '#');
            CF_REQUIRE(hash != NULL);
            char controller[64];
            size_t clen = (size_t)(hash - row->endpoint);
            CF_REQUIRE(clen < sizeof controller);
            memcpy(controller, row->endpoint, clen);
            controller[clen] = '\0';
            CF_CHECK(param_string(match.path_params, "controller", controller));
            CF_CHECK(param_string(match.path_params, "action", hash + 1));
        }
        cf_route_match_dispose(&match);
    }
    yyjson_doc_free(doc);
}

CF_TEST(matrix_methods_missing_routes_and_cable) {
    cf_route_match m;
    /* PATCH /rooms, GET /ROOMS/1 and /cable are the vectors' negative
     * cases; /cable is a front mount outside the table. */
    CF_CHECK(recognize("PATCH", "/rooms", &m) == CF_NOT_FOUND);
    CF_CHECK(recognize("GET", "/ROOMS/1", &m) == CF_NOT_FOUND);
    CF_CHECK(recognize("GET", "/cable", &m) == CF_NOT_FOUND);
    CF_CHECK(recognize("OPTIONS", "/", &m) == CF_NOT_FOUND);
    CF_CHECK(recognize("DELETE", "/", &m) == CF_NOT_FOUND);
    /* HEAD matches GET rows. */
    CF_CHECK(recognize("HEAD", "/", &m) == CF_OK);
    CF_CHECK(cf_route_by_id(m.id)->method == CF_GET);
    cf_route_match_dispose(&m);
    CF_CHECK(recognize("HEAD", "/rooms/1", &m) == CF_OK);
    CF_CHECK(strcmp(cf_route_by_id(m.id)->pattern, "/rooms/:id(.:format)") == 0);
    cf_route_match_dispose(&m);
    /* A route with no HEAD-specific row still matches its GET row. */
    CF_CHECK(recognize("HEAD", "/up", &m) == CF_OK);
    CF_CHECK(strcmp(cf_route_by_id(m.id)->endpoint, "rails/health#show") == 0);
    cf_route_match_dispose(&m);
}

CF_TEST(recognition_normalizes_paths) {
    cf_route_match m;
    CF_CHECK(recognize("GET", "//rooms//1//", &m) == CF_OK);
    CF_CHECK(strcmp(cf_route_by_id(m.id)->pattern, "/rooms/:id(.:format)") == 0);
    CF_CHECK(param_string(m.path_params, "id", "1"));
    cf_route_match_dispose(&m);

    CF_CHECK(recognize("GET", "/", &m) == CF_OK);
    CF_CHECK(m.id == 1);
    cf_route_match_dispose(&m);

    /* Empty and slash-only paths normalize to "/". */
    CF_CHECK(recognize("GET", "", &m) == CF_OK);
    CF_CHECK(m.id == 1);
    cf_route_match_dispose(&m);
    CF_CHECK(recognize("GET", "///", &m) == CF_OK);
    CF_CHECK(m.id == 1);
    cf_route_match_dispose(&m);

    /* /rooms/ is the index, not a show with an empty id. */
    CF_CHECK(recognize("GET", "/rooms/", &m) == CF_OK);
    CF_CHECK(strcmp(cf_route_by_id(m.id)->endpoint, "rooms#index") == 0);
    cf_route_match_dispose(&m);
}

CF_TEST(recognition_percent_decoding_and_errors) {
    cf_route_match m;
    /* Percent escapes are decoded only after recognition. */
    CF_CHECK(recognize("GET", "/rooms/%31", &m) == CF_OK);
    CF_CHECK(param_string(m.path_params, "id", "1"));
    cf_route_match_dispose(&m);
    CF_CHECK(recognize("GET", "/rooms/a%2Fb", &m) == CF_OK);
    CF_CHECK(param_string(m.path_params, "id", "a/b"));
    cf_route_match_dispose(&m);
    CF_CHECK(recognize("GET", "/rooms/caf%C3%A9", &m) == CF_OK);
    CF_CHECK(param_string(m.path_params, "id", "caf\xC3\xA9"));
    cf_route_match_dispose(&m);
    /* Lowercase escapes match; they are upcased before recognition. */
    CF_CHECK(recognize("GET", "/rooms/1%2E2", &m) == CF_OK);
    CF_CHECK(param_string(m.path_params, "id", "1.2"));
    cf_route_match_dispose(&m);
    /* '+' is literal in the path (form decoding is query-only). */
    CF_CHECK(recognize("GET", "/rooms/a+b", &m) == CF_OK);
    CF_CHECK(param_string(m.path_params, "id", "a+b"));
    cf_route_match_dispose(&m);
    /* A malformed escape is kept literally (percent_decode_str). */
    CF_CHECK(recognize("GET", "/rooms/%zz", &m) == CF_OK);
    CF_CHECK(param_string(m.path_params, "id", "%zz"));
    cf_route_match_dispose(&m);
    CF_CHECK(recognize("GET", "/rooms/%4", &m) == CF_OK);
    CF_CHECK(param_string(m.path_params, "id", "%4"));
    cf_route_match_dispose(&m);

    /* Bad UTF-8 path parameters fail the match attempt with CF_INVALID
     * (A00 maps it to 400); they never fall through to a later row. */
    static const char *bad[] = {
        "/rooms/%FF",           /* lone 0xFF */
        "/rooms/%C0%AF",        /* overlong '/' */
        "/rooms/%ED%A0%80",     /* UTF-16 surrogate */
        "/rooms/%F4%90%80%80",  /* above U+10FFFF */
        "/rooms/%E9",           /* truncated 3-byte sequence */
        "/rooms/caf%C3",        /* truncated 2-byte sequence */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        memset(&m, 0, sizeof m);
        CF_CHECK(recognize("GET", bad[i], &m) == CF_INVALID);
        CF_CHECK(m.id == 0 && m.path_params == NULL);
        cf_route_match_dispose(&m);
    }
    /* A raw 0xFF byte in the path is the same failure. */
    {
        cf_request req;
        h03_req_init(&req);
        static const unsigned char raw[] = {'/', 'r', 'o', 'o', 'm', 's',
                                            '/', 0xFF};
        req.path = (cf_span){raw, sizeof raw};
        memset(&m, 0, sizeof m);
        CF_CHECK(cf_route_match_request(&req, &m) == CF_INVALID);
        cf_route_match_dispose(&m);
    }
}

CF_TEST(recognition_formats_globs_and_dots) {
    cf_route_match m;
    CF_CHECK(recognize("GET", "/rooms/1.json", &m) == CF_OK);
    CF_CHECK(param_string(m.path_params, "id", "1"));
    CF_CHECK(param_string(m.path_params, "format", "json"));
    cf_route_match_dispose(&m);
    CF_CHECK(recognize("GET", "/rooms/1.turbo_stream", &m) == CF_OK);
    CF_CHECK(param_string(m.path_params, "format", "turbo_stream"));
    cf_route_match_dispose(&m);
    /* ':id' excludes dots, so a second dot is not a format. */
    CF_CHECK(recognize("GET", "/rooms/1.2.3", &m) == CF_NOT_FOUND);
    CF_CHECK(recognize("GET", "/rooms/1.", &m) == CF_NOT_FOUND);
    CF_CHECK(recognize("GET", "/rooms/.json", &m) == CF_NOT_FOUND);

    /* Non-greedy glob: the last dotted suffix wins as :format. */
    CF_CHECK(recognize("GET",
                       "/rails/active_storage/blobs/redirect/abc--def/dir/photo.tar.gz",
                       &m) == CF_OK);
    CF_CHECK(param_string(m.path_params, "signed_id", "abc--def"));
    CF_CHECK(param_string(m.path_params, "filename", "dir/photo.tar"));
    CF_CHECK(param_string(m.path_params, "format", "gz"));
    cf_route_match_dispose(&m);
    CF_CHECK(recognize("GET", "/rails/active_storage/blobs/abc--def/photo",
                       &m) == CF_OK);
    CF_CHECK(param_string(m.path_params, "filename", "photo"));
    CF_CHECK(cf_param_get(m.path_params, CF_TEST_SPAN("format")) == NULL);
    cf_route_match_dispose(&m);

    /* A failed optional-format attempt must not leak a stale `format` when
     * the group turns out to be absent (found by the differential corpus:
     * a trailing dot or a dotted directory is part of the glob). */
    CF_CHECK(recognize("GET", "/rails/active_storage/blobs/1/photo.json.",
                       &m) == CF_OK);
    CF_CHECK(param_string(m.path_params, "filename", "photo.json."));
    CF_CHECK(cf_param_get(m.path_params, CF_TEST_SPAN("format")) == NULL);
    cf_route_match_dispose(&m);
    CF_CHECK(recognize("GET", "/rails/active_storage/blobs/1/photo.json/edit",
                       &m) == CF_OK);
    CF_CHECK(param_string(m.path_params, "filename", "photo.json/edit"));
    CF_CHECK(cf_param_get(m.path_params, CF_TEST_SPAN("format")) == NULL);
    cf_route_match_dispose(&m);

    /* Defaults: the bot scope's format=json is a path parameter. */
    CF_CHECK(recognize("GET", "/rooms/1/1-abc/messages", &m) == CF_OK);
    CF_CHECK(param_string(m.path_params, "format", "json"));
    CF_CHECK(param_string(m.path_params, "room_id", "1"));
    CF_CHECK(param_string(m.path_params, "bot_key", "1-abc"));
    cf_route_match_dispose(&m);
    CF_CHECK(recognize("POST", "/rooms/1/1-abc/messages.txt", &m) == CF_OK);
    CF_CHECK(param_string(m.path_params, "format", "txt")); /* capture wins */
    cf_route_match_dispose(&m);

    /* The me-scope default applies where no capture overrides it. */
    CF_CHECK(recognize("GET", "/users/me/sidebar", &m) == CF_OK);
    CF_CHECK(param_string(m.path_params, "user_id", "me"));
    cf_route_match_dispose(&m);
    CF_CHECK(recognize("GET", "/users/3/sidebar", &m) == CF_OK);
    CF_CHECK(param_string(m.path_params, "user_id", "3"));
    cf_route_match_dispose(&m);
}

CF_TEST(rooms_opens_is_intentionally_shadowed) {
    cf_route_match m;
    /* GET /rooms/opens is matched by the earlier /rooms/:id, per spec. */
    CF_CHECK(recognize("GET", "/rooms/opens", &m) == CF_OK);
    CF_CHECK(strcmp(cf_route_by_id(m.id)->endpoint, "rooms#show") == 0);
    CF_CHECK(param_string(m.path_params, "id", "opens"));
    cf_route_match_dispose(&m);
    CF_CHECK(recognize("GET", "/rooms/closeds", &m) == CF_OK);
    CF_CHECK(strcmp(cf_route_by_id(m.id)->endpoint, "rooms#show") == 0);
    cf_route_match_dispose(&m);
    CF_CHECK(recognize("GET", "/rooms/directs", &m) == CF_OK);
    CF_CHECK(strcmp(cf_route_by_id(m.id)->endpoint, "rooms#show") == 0);
    cf_route_match_dispose(&m);
    /* The namespace endpoints behind them are still reachable where the
     * earlier row does not swallow them. */
    CF_CHECK(recognize("GET", "/rooms/opens/new", &m) == CF_OK);
    CF_CHECK(strcmp(cf_route_by_id(m.id)->endpoint, "rooms/opens#new") == 0);
    cf_route_match_dispose(&m);
    CF_CHECK(recognize("POST", "/rooms/opens", &m) == CF_OK);
    CF_CHECK(strcmp(cf_route_by_id(m.id)->endpoint, "rooms/opens#create") == 0);
    cf_route_match_dispose(&m);
    CF_CHECK(recognize("GET", "/rooms/opens/5", &m) == CF_OK);
    CF_CHECK(strcmp(cf_route_by_id(m.id)->endpoint, "rooms/opens#show") == 0);
    cf_route_match_dispose(&m);
}

/* Fill each artifact pattern with concrete parameters, with and without the
 * optional format; the first match must be that row, except the three
 * GET namespace index rows swallowed by GET /rooms/:id. */
static void fill_pattern(const cf_route *row, bool with_format, char *out,
                         size_t cap) {
    size_t n = 0;
    const char *p = row->pattern;
#define APPEND(ch)                                                            \
    do {                                                                      \
        if (n + 1 < cap) out[n] = (ch);                                       \
        n++;                                                                  \
    } while (0)
    while (*p != '\0') {
        if (*p == '(') {
            if (with_format) {
                APPEND('.');
                APPEND('j');
                APPEND('s');
                APPEND('o');
                APPEND('n');
            }
            const char *close = strchr(p, ')');
            CF_REQUIRE(close != NULL);
            p = close + 1;
            continue;
        }
        if (*p == ':' || *p == '*') {
            const char *name = p + 1;
            while ((*name >= 'A' && *name <= 'Z') ||
                   (*name >= 'a' && *name <= 'z') ||
                   (*name >= '0' && *name <= '9') || *name == '_') {
                name++;
            }
            if (*p == '*') {
                APPEND('p');
                APPEND('h');
                APPEND('o');
                APPEND('t');
                APPEND('o');
            } else if (name - p - 1 == 4 &&
                       memcmp(p + 1, "name", 4) == 0) {
                APPEND('x');
            } else {
                APPEND('1');
            }
            p = name;
            continue;
        }
        APPEND(*p);
        p++;
    }
    if (n < cap) out[n] = '\0';
    n++;
    CF_REQUIRE(n <= cap);
#undef APPEND
}

static bool row_is_shadowed(const cf_route *row) {
    return row->method == CF_GET &&
           (strcmp(row->pattern, "/rooms/opens(.:format)") == 0 ||
            strcmp(row->pattern, "/rooms/closeds(.:format)") == 0 ||
            strcmp(row->pattern, "/rooms/directs(.:format)") == 0);
}

CF_TEST(every_row_is_reachable_in_order) {
    size_t count = 0;
    const cf_route *table = cf_routes(&count);
    CF_REQUIRE(table != NULL && count == 177);
    char path[256];
    for (size_t i = 0; i < count; i++) {
        const cf_route *row = &table[i];
        bool has_format = strstr(row->pattern, "(.:format)") != NULL;
        for (int variant = 0; variant < (has_format ? 2 : 1); variant++) {
            fill_pattern(row, variant == 1, path, sizeof path);
            cf_route_match m;
            memset(&m, 0, sizeof m);
            cf_err rc = recognize(recognition_method_name(row->method), path,
                                  &m);
            CF_REQUIRE(rc == CF_OK);
            if (row_is_shadowed(row)) {
                const cf_route *matched = cf_route_by_id(m.id);
                CF_CHECK(matched != NULL);
                CF_CHECK(strcmp(matched->endpoint, "rooms#show") == 0);
            } else {
                CF_CHECK(m.id == row->id);
            }
            cf_route_match_dispose(&m);
        }
    }
}

CF_TEST(long_glob_paths_are_bounded) {
    /* A long filename still matches its row once: no fallback, no quadratic
     * blowup beyond the path length. */
    char path[4096];
    int n = snprintf(path, sizeof path,
                     "/rails/active_storage/blobs/redirect/abc--def/");
    CF_REQUIRE(n > 0);
    size_t len = (size_t)n;
    while (len + 5 < sizeof path) {
        path[len] = (len % 97 == 0) ? '/' : (char)('a' + (len % 26));
        len++;
    }
    memcpy(path + len, ".png", 4);
    len += 4;
    path[len] = '\0';
    cf_route_match m;
    memset(&m, 0, sizeof m);
    CF_REQUIRE(recognize("GET", path, &m) == CF_OK);
    CF_CHECK(strcmp(cf_route_by_id(m.id)->endpoint,
                    "active_storage/blobs/redirect#show") == 0);
    cf_route_match_dispose(&m);
}

CF_TEST_MAIN()
