/* H03 static front mount (HTTP-07 asset half): pinned fixture serving with
 * the reference's Rack::Mime content types, Cache-Control/Last-Modified,
 * If-Modified-Since, HEAD, extensionless fallbacks, .br/.gz negotiation
 * (synthetic bodies where the pinned tree has no siblings), and traversal /
 * NUL / directory-listing refusals. */
#include "cf_test.h"

#include "http/http_internal.h"
#include "routes.h"
#include "../routes/support/h03_test_util.h"

#include <fcntl.h>
#include <ftw.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yyjson.h>

#define FIXTURE_ROOT "tests/fixtures/assets"
#define LAST_MODIFIED "Sat, 26 Sep 2026 11:03:34 GMT"

/* The front mount runs before A00's context exists; it takes the frozen
 * request and the empty response directly. */
static cf_err serve(cf_request *req, cf_response *resp, bool *handled) {
    cf_response_init(resp);
    return cf_assets_serve(req, resp, handled);
}

static void req_get(cf_request *req, const char *path) {
    h03_req_init(req);
    req->path = (cf_span){(const unsigned char *)path, strlen(path)};
}

static void req_get_ae(cf_request *req, const char *path,
                       const char *accept_encoding) {
    req_get(req, path);
    CF_REQUIRE(h03_req_header(req, CF_TEST_SPAN("Accept-Encoding"),
                              (cf_span){(const unsigned char *)accept_encoding,
                                        strlen(accept_encoding)}) == CF_OK);
}

static bool body_is(const cf_response *resp, const char *expected) {
    size_t len = 0;
    unsigned char *body = h03_response_bytes(resp, &len);
    if (body == NULL) return false;
    size_t n = strlen(expected);
    bool same = len == n && (n == 0 || memcmp(body, expected, n) == 0);
    free(body);
    return same;
}

static bool fixture_is(const cf_response *resp, const char *fixture) {
    size_t want_len = 0;
    unsigned char *want = h03_read_file(fixture, &want_len);
    if (want == NULL) return false;
    size_t len = 0;
    unsigned char *body = h03_response_bytes(resp, &len);
    if (body == NULL) {
        free(want);
        return false;
    }
    bool same = len == want_len && (len == 0 || memcmp(body, want, len) == 0);
    free(body);
    free(want);
    return same;
}

/* ---- scratch fixture trees --------------------------------------------- */

static bool write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) return false;
    size_t n = strlen(content);
    bool ok = fwrite(content, 1, n, f) == n;
    fclose(f);
    return ok;
}

static bool make_dir(const char *path) {
    return mkdir(path, 0700) == 0 || access(path, F_OK) == 0;
}

static int remove_cb(const char *path, const struct stat *st, int type,
                     struct FTW *ftw) {
    (void)st;
    (void)type;
    (void)ftw;
    return remove(path);
}

static void remove_tree(const char *dir) {
    (void)nftw(dir, remove_cb, 16, FTW_DEPTH | FTW_PHYS);
}

/* A scratch root with public/assets/ directories; dir holds the template on
 * entry and the new directory on success. */
static bool scratch_root(char *dir) {
    if (mkdtemp(dir) == NULL) return false;
    char path[1024];
    snprintf(path, sizeof path, "%s/public", dir);
    if (!make_dir(path)) return false;
    snprintf(path, sizeof path, "%s/public/assets", dir);
    return make_dir(path);
}

/* ---- pinned fixture serving -------------------------------------------- */

CF_TEST(digested_fixture_is_served_with_reference_headers) {
    CF_CHECK(cf_static_set_root(FIXTURE_ROOT) == CF_OK);
    cf_request req;
    cf_response resp;
    bool handled = false;

    req_get(&req, "/assets/application-a54c74a7.js");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(handled);
    CF_CHECK(resp.status == 200);
    CF_CHECK(resp.body_kind == CF_BODY_FILE);
    CF_CHECK(fixture_is(&resp,
                        FIXTURE_ROOT "/public/assets/application-a54c74a7.js"));
    {
        char *h = h03_serialize_headers(&resp, &req);
        CF_REQUIRE(h != NULL);
        CF_CHECK(h03_header_is(h, "Content-Type", "text/javascript"));
        CF_CHECK(h03_header_is(h, "Cache-Control",
                               "public, max-age=2592000"));
        CF_CHECK(h03_header_is(h, "Last-Modified", LAST_MODIFIED));
        CF_CHECK(strstr(h, "Content-Length: ") != NULL);
        free(h);
    }
    cf_response_dispose(&resp);

    /* Unknown extensions: .map and .br are text/plain, .gz is
     * application/x-gzip (Rack::Mime), and none of them is negotiated as a
     * compressed twin of anything. */
    req_get(&req, "/assets/lexxy-cf040c43.js.map");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(handled && resp.status == 200);
    {
        char *h = h03_serialize_headers(&resp, &req);
        CF_REQUIRE(h != NULL);
        CF_CHECK(h03_header_is(h, "Content-Type", "text/plain"));
        CF_CHECK(strstr(h, "Content-Encoding: ") == NULL);
        free(h);
    }
    cf_response_dispose(&resp);

    req_get(&req, "/assets/lexxy.min.js-56322c4c.gz");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(handled && resp.status == 200);
    {
        char *h = h03_serialize_headers(&resp, &req);
        CF_REQUIRE(h != NULL);
        CF_CHECK(h03_header_is(h, "Content-Type", "application/x-gzip"));
        free(h);
    }
    cf_response_dispose(&resp);

    /* Public files are served at their own URLs by the same mount. */
    req_get(&req, "/404.html");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(handled && resp.status == 200);
    CF_CHECK(fixture_is(&resp, FIXTURE_ROOT "/public/404.html"));
    cf_response_dispose(&resp);

    req_get(&req, "/robots.txt");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(handled && resp.status == 200);
    CF_CHECK(fixture_is(&resp, FIXTURE_ROOT "/public/robots.txt"));
    cf_response_dispose(&resp);

    req_get(&req, "/assets/.manifest.json");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(handled && resp.status == 200);
    {
        char *h = h03_serialize_headers(&resp, &req);
        CF_REQUIRE(h != NULL);
        CF_CHECK(h03_header_is(h, "Content-Type", "application/json"));
        free(h);
    }
    cf_response_dispose(&resp);
}

CF_TEST(conditional_and_head_requests) {
    CF_CHECK(cf_static_set_root(FIXTURE_ROOT) == CF_OK);
    cf_request req;
    cf_response resp;
    bool handled = false;

    /* If-Modified-Since equal to the pinned build time: 304, no other
     * headers made it onto the response. */
    req_get(&req, "/assets/application-a54c74a7.js");
    CF_REQUIRE(h03_req_header(&req, CF_TEST_SPAN("If-Modified-Since"),
                              CF_TEST_SPAN(LAST_MODIFIED)) == CF_OK);
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(handled && resp.status == 304);
    CF_CHECK(resp.body_kind == CF_BODY_NONE);
    {
        char *h = h03_serialize_headers(&resp, &req);
        CF_REQUIRE(h != NULL);
        CF_CHECK(strstr(h, "HTTP/1.1 304") != NULL);
        CF_CHECK(strstr(h, "Content-Type: ") == NULL);
        CF_CHECK(strstr(h, "Cache-Control: ") == NULL);
        CF_CHECK(strstr(h, "Content-Length: ") == NULL);
        free(h);
    }
    cf_response_dispose(&resp);

    /* Any other value is a full 200. */
    req_get(&req, "/assets/application-a54c74a7.js");
    CF_REQUIRE(h03_req_header(&req, CF_TEST_SPAN("If-Modified-Since"),
                              CF_TEST_SPAN("Thu, 01 Jan 1970 00:00:00 GMT")) ==
               CF_OK);
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(handled && resp.status == 200);
    cf_response_dispose(&resp);

    /* HEAD carries the file's Content-Length and no body. */
    req_get(&req, "/assets/application-a54c74a7.js");
    req.method = CF_HEAD;
    req.original_method = CF_HEAD;
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(handled && resp.status == 200);
    CF_CHECK(resp.body_kind == CF_BODY_FILE);
    {
        char *h = h03_serialize_headers(&resp, &req);
        CF_REQUIRE(h != NULL);
        CF_CHECK(strstr(h, "Content-Length: 179\r\n") != NULL);
        free(h);
    }
    cf_response_dispose(&resp);
}

CF_TEST(every_manifest_url_is_served_with_its_recorded_type_and_bytes) {
    /* The fixture MANIFEST is the asset artifact: every one of its 321
     * served URLs must be reachable, with the recorded content type and the
     * recorded bytes. */
    yyjson_doc *doc = yyjson_read_file(
        FIXTURE_ROOT "/MANIFEST.json", 0, NULL, NULL);
    CF_REQUIRE(doc != NULL);
    yyjson_val *entries = yyjson_obj_get(yyjson_doc_get_root(doc), "entries");
    CF_REQUIRE(entries != NULL && yyjson_is_arr(entries));
    CF_REQUIRE(yyjson_arr_size(entries) == 321);

    CF_REQUIRE(cf_static_set_root(FIXTURE_ROOT) == CF_OK);
    size_t index, max, served = 0;
    yyjson_val *entry;
    yyjson_arr_foreach(entries, index, max, entry) {
        const char *url = yyjson_get_str(yyjson_obj_get(entry, "url"));
        const char *fixture = yyjson_get_str(yyjson_obj_get(entry, "fixture"));
        const char *content_type =
            yyjson_get_str(yyjson_obj_get(entry, "content_type"));
        CF_REQUIRE(url != NULL && fixture != NULL && content_type != NULL);

        cf_request req;
        cf_response resp;
        bool handled = false;
        req_get(&req, url);
        CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
        CF_CHECK(handled);
        CF_CHECK(resp.status == 200);
        CF_CHECK(fixture_is(&resp, fixture));
        char *headers = h03_serialize_headers(&resp, &req);
        CF_REQUIRE(headers != NULL);
        CF_CHECK(h03_header_is(headers, "Content-Type", content_type));
        CF_CHECK(h03_header_is(headers, "Cache-Control",
                               "public, max-age=2592000"));
        CF_CHECK(h03_header_is(headers, "Last-Modified", LAST_MODIFIED));
        free(headers);
        if (handled && resp.status == 200) served++;
        cf_response_dispose(&resp);
    }
    CF_CHECK(served == 321);
    yyjson_doc_free(doc);
}

/* ---- negotiation with synthetic bodies --------------------------------- */

CF_TEST(identity_and_compressed_sibling_negotiation) {
    char dir[256] = "/tmp/cf_h03_assets_XXXXXX";
    CF_REQUIRE(scratch_root(dir));
    char base[512];
    snprintf(base, sizeof base, "%s/public/assets", dir);
    char css[600], br[600], gz[600], png[600], png_gz[600];
    snprintf(css, sizeof css, "%s/app-11111111.css", base);
    snprintf(br, sizeof br, "%s/app-11111111.css.br", base);
    snprintf(gz, sizeof gz, "%s/app-11111111.css.gz", base);
    snprintf(png, sizeof png, "%s/pic-22222222.png", base);
    snprintf(png_gz, sizeof png_gz, "%s/pic-22222222.png.gz", base);
    CF_REQUIRE(write_file(css, "IDENTITY-CSS"));
    CF_REQUIRE(write_file(br, "BROTLI-CSS"));
    CF_REQUIRE(write_file(gz, "GZIP-CSS"));
    CF_REQUIRE(write_file(png, "PNG-BYTES"));
    CF_REQUIRE(write_file(png_gz, "PNG-GZIP-SHOULD-NOT-BE-USED"));

    CF_REQUIRE(cf_static_set_root(dir) == CF_OK);
    cf_request req;
    cf_response resp;
    bool handled = false;

    /* No Accept-Encoding: identity, but Vary is set because a twin exists. */
    req_get(&req, "/assets/app-11111111.css");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(handled && resp.status == 200);
    CF_CHECK(body_is(&resp, "IDENTITY-CSS"));
    {
        char *h = h03_serialize_headers(&resp, &req);
        CF_REQUIRE(h != NULL);
        CF_CHECK(h03_header_is(h, "Vary", "accept-encoding"));
        CF_CHECK(strstr(h, "Content-Encoding: ") == NULL);
        CF_CHECK(h03_header_is(h, "Content-Type", "text/css"));
        free(h);
    }
    cf_response_dispose(&resp);

    req_get_ae(&req, "/assets/app-11111111.css", "br");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(handled && body_is(&resp, "BROTLI-CSS"));
    {
        char *h = h03_serialize_headers(&resp, &req);
        CF_REQUIRE(h != NULL);
        CF_CHECK(h03_header_is(h, "Content-Encoding", "br"));
        CF_CHECK(h03_header_is(h, "Vary", "accept-encoding"));
        free(h);
    }
    cf_response_dispose(&resp);

    req_get_ae(&req, "/assets/app-11111111.css", "gzip");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(handled && body_is(&resp, "GZIP-CSS"));
    {
        char *h = h03_serialize_headers(&resp, &req);
        CF_REQUIRE(h != NULL);
        CF_CHECK(h03_header_is(h, "Content-Encoding", "gzip"));
        free(h);
    }
    cf_response_dispose(&resp);

    /* Brotli is probed first; `br;q=0` still counts (the reference reads the
     * encoding token, not its q). */
    req_get_ae(&req, "/assets/app-11111111.css", "gzip, br;q=0");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(body_is(&resp, "BROTLI-CSS"));
    cf_response_dispose(&resp);

    /* `x-gzip` is a word-bounded gzip token; uppercase too. */
    req_get_ae(&req, "/assets/app-11111111.css", "x-gzip");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(body_is(&resp, "GZIP-CSS"));
    cf_response_dispose(&resp);
    req_get_ae(&req, "/assets/app-11111111.css", "GZIP, identity");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(body_is(&resp, "GZIP-CSS"));
    cf_response_dispose(&resp);

    /* gzip is not a token inside "xgzip". */
    req_get_ae(&req, "/assets/app-11111111.css", "xgzip");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(body_is(&resp, "IDENTITY-CSS"));
    cf_response_dispose(&resp);

    /* A non-compressible type never probes its siblings. */
    req_get_ae(&req, "/assets/pic-22222222.png", "gzip");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(handled);
    CF_CHECK(body_is(&resp, "PNG-BYTES"));
    {
        char *h = h03_serialize_headers(&resp, &req);
        CF_REQUIRE(h != NULL);
        CF_CHECK(strstr(h, "Content-Encoding: ") == NULL);
        CF_CHECK(strstr(h, "Vary: ") == NULL);
        free(h);
    }
    cf_response_dispose(&resp);

    CF_REQUIRE(cf_static_set_root(FIXTURE_ROOT) == CF_OK);
    remove_tree(dir);
}

CF_TEST(extensionless_fallbacks) {
    char dir[256] = "/tmp/cf_h03_assets_XXXXXX";
    CF_REQUIRE(scratch_root(dir));
    char path[512];
    snprintf(path, sizeof path, "%s/public/foo.html", dir);
    CF_REQUIRE(write_file(path, "FOO-HTML"));
    snprintf(path, sizeof path, "%s/public/plain", dir);
    CF_REQUIRE(write_file(path, "PLAIN-BYTES"));
    snprintf(path, sizeof path, "%s/public/dir", dir);
    CF_REQUIRE(make_dir(path));
    snprintf(path, sizeof path, "%s/public/dir/index.html", dir);
    CF_REQUIRE(write_file(path, "DIR-INDEX"));
    snprintf(path, sizeof path, "%s/public/unknown.zzz", dir);
    CF_REQUIRE(write_file(path, "UNKNOWN-EXT"));

    CF_REQUIRE(cf_static_set_root(dir) == CF_OK);
    cf_request req;
    cf_response resp;
    bool handled = false;

    req_get(&req, "/foo");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(handled && body_is(&resp, "FOO-HTML"));
    {
        char *h = h03_serialize_headers(&resp, &req);
        CF_REQUIRE(h != NULL);
        CF_CHECK(h03_header_is(h, "Content-Type", "text/html"));
        free(h);
    }
    cf_response_dispose(&resp);

    /* The direct candidate wins and is text/plain for an unknown path. */
    req_get(&req, "/plain");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(handled && body_is(&resp, "PLAIN-BYTES"));
    {
        char *h = h03_serialize_headers(&resp, &req);
        CF_REQUIRE(h != NULL);
        CF_CHECK(h03_header_is(h, "Content-Type", "text/plain"));
        free(h);
    }
    cf_response_dispose(&resp);

    req_get(&req, "/dir");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(handled && body_is(&resp, "DIR-INDEX"));
    cf_response_dispose(&resp);

    /* An unknown extension is not replaced by .html candidates. */
    req_get(&req, "/unknown.zzz");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(handled && body_is(&resp, "UNKNOWN-EXT"));
    cf_response_dispose(&resp);

    /* Nothing matches: the app routes the request. */
    req_get(&req, "/missing");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(!handled);
    CF_CHECK(resp.status == 0);
    cf_response_dispose(&resp);

    req_get(&req, "/assets/");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(!handled); /* no directory listing */
    cf_response_dispose(&resp);

    CF_REQUIRE(cf_static_set_root(FIXTURE_ROOT) == CF_OK);
    remove_tree(dir);
}

CF_TEST(traversal_nul_and_methods_are_refused) {
    char dir[256] = "/tmp/cf_h03_assets_XXXXXX";
    CF_REQUIRE(scratch_root(dir));
    char path[512];
    snprintf(path, sizeof path, "%s/secret.txt", dir);
    CF_REQUIRE(write_file(path, "TOPSECRET"));
    snprintf(path, sizeof path, "%s/public/assets/ok-33333333.js", dir);
    CF_REQUIRE(write_file(path, "OK"));

    CF_REQUIRE(cf_static_set_root(dir) == CF_OK);
    cf_request req;
    cf_response resp;
    bool handled = false;

    /* `..` segments collapse inside the cleaned URL and never escape
     * public/. */
    req_get(&req, "/../secret.txt");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(!handled);
    cf_response_dispose(&resp);
    req_get(&req, "/assets/%2e%2e/%2e%2e/secret.txt");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(!handled);
    cf_response_dispose(&resp);
    req_get(&req, "/assets/../../../../etc/passwd");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(!handled);
    cf_response_dispose(&resp);

    /* A NUL byte after percent-decoding declines the request. */
    req_get(&req, "/assets/ok-33333333.js%00.png");
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(!handled);
    cf_response_dispose(&resp);

    /* Non-GET/HEAD never enters the static mount. */
    req_get(&req, "/assets/ok-33333333.js");
    req.method = CF_POST;
    req.original_method = CF_POST;
    CF_REQUIRE(serve(&req, &resp, &handled) == CF_OK);
    CF_CHECK(!handled);
    cf_response_dispose(&resp);

    CF_REQUIRE(cf_static_set_root(FIXTURE_ROOT) == CF_OK);
    remove_tree(dir);
}

CF_TEST_MAIN()
