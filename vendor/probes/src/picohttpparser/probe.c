/* F00 probe: picohttpparser pinned commit 465a7ff09fbd3432fe56c673451f5460154d1f07
 * Parses one valid request, records every parsed field, and records the exact
 * parser return value for an incomplete and a malformed request. */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "picohttpparser.h"

static int failures = 0;

static void fail(const char *what) {
    failures++;
    fprintf(stderr, "PROBE FAIL: %s\n", what);
}

static void print_headers(const struct phr_header *headers, size_t n) {
    for (size_t i = 0; i < n; i++) {
        printf("  header[%zu] name_len=%zu name=\"%.*s\" value_len=%zu value=\"%.*s\"\n",
               i, headers[i].name_len, (int)headers[i].name_len, headers[i].name,
               headers[i].value_len, (int)headers[i].value_len, headers[i].value);
    }
}

int main(void) {
    const char *req =
        "GET /hello?x=1 HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "X-Test: abc def\r\n"
        "\r\n";
    const char *method = NULL;
    const char *path = NULL;
    size_t method_len = 0, path_len = 0, num_headers = 16;
    int minor_version = -99;
    struct phr_header headers[16];

    int n = phr_parse_request(req, strlen(req), &method, &method_len, &path, &path_len,
                              &minor_version, headers, &num_headers, 0);
    printf("valid_request ret=%d input_len=%zu\n", n, strlen(req));
    if (n <= 0) {
        fail("valid request did not parse");
    } else {
        printf("  consumed=%d method=\"%.*s\" path=\"%.*s\" minor_version=%d num_headers=%zu\n",
               n, (int)method_len, method, (int)path_len, path, minor_version, num_headers);
        print_headers(headers, num_headers);
        if (n != (int)strlen(req)) fail("valid request byte count mismatch");
        if (method_len != 3 || memcmp(method, "GET", 3) != 0) fail("method mismatch");
        if (path_len != 10 || memcmp(path, "/hello?x=1", 10) != 0) fail("path mismatch");
        if (minor_version != 1) fail("minor_version mismatch");
        if (num_headers != 2) fail("header count mismatch");
    }

    const char *partial = req;
    size_t partial_len = 10;
    method = NULL; path = NULL; method_len = 0; path_len = 0; num_headers = 16;
    minor_version = -99;
    int r_partial = phr_parse_request(partial, partial_len, &method, &method_len, &path,
                                      &path_len, &minor_version, headers, &num_headers, 0);
    printf("incomplete_request(%zu bytes) ret=%d (observed)\n", partial_len, r_partial);
    if (r_partial != -2) fail("incomplete request should return -2");

    const char *bad = "GET /\r\n\r\n";
    method = NULL; path = NULL; method_len = 0; path_len = 0; num_headers = 16;
    minor_version = -99;
    int r_bad = phr_parse_request(bad, strlen(bad), &method, &method_len, &path, &path_len,
                                  &minor_version, headers, &num_headers, 0);
    printf("malformed_request(no HTTP version) ret=%d (observed)\n", r_bad);
    if (r_bad != -1) fail("malformed request should return -1");

    const char *ctl = "GET / HTTP/1.1\r\nBad\x01Name: x\r\n\r\n";
    method = NULL; path = NULL; method_len = 0; path_len = 0; num_headers = 16;
    minor_version = -99;
    int r_ctl = phr_parse_request(ctl, strlen(ctl), &method, &method_len, &path, &path_len,
                                  &minor_version, headers, &num_headers, 0);
    printf("control_char_header ret=%d (observed, informational)\n", r_ctl);
    if (r_ctl > 0) print_headers(headers, num_headers);

    printf("probe_result=%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
