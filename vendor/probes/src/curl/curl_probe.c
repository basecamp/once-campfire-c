/*
 * F00 clang probe for libcurl 8.22.0-DEV (static build, OpenSSL TLS backend).
 *
 * Required observations from the F00 task:
 *   - curl_version() / curl_version_info() report the OpenSSL backend
 *   - easy-interface GET against a local http server returns the expected body
 *   - TLS verification is never disabled: this probe does not set
 *     CURLOPT_SSL_VERIFYPEER / CURLOPT_SSL_VERIFYHOST at all.
 *
 * Usage: curl_probe <url> <expected-body>
 * Exits 0 only when every check passes.
 */
#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct body {
    char data[4096];
    size_t len;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct body *b = userdata;
    size_t n = size * nmemb;

    if (n > sizeof b->data - 1 - b->len)
        return 0; /* bounded: refuse over-limit bodies */
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

int main(int argc, char **argv)
{
    CURL *curl;
    CURLcode rc;
    curl_version_info_data *vi;
    struct body body;
    long status = 0;
    int failures = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <url> <expected-body>\n", argv[0]);
        return 2;
    }

    printf("curl_version() = %s\n", curl_version());
    vi = curl_version_info(CURLVERSION_NOW);
    printf("curl_version_info: version=%s host=%s ssl_version=%s\n",
           vi->version, vi->host, vi->ssl_version != NULL ? vi->ssl_version : "(none)");

    if ((vi->features & CURL_VERSION_SSL) == 0) {
        printf("FAIL ssl-feature: CURL_VERSION_SSL not set\n");
        failures++;
    } else {
        printf("PASS ssl-feature: CURL_VERSION_SSL set\n");
    }
    if (vi->ssl_version == NULL || strstr(vi->ssl_version, "OpenSSL") == NULL) {
        printf("FAIL ssl-backend: expected OpenSSL in ssl_version\n");
        failures++;
    } else {
        printf("PASS ssl-backend: %s\n", vi->ssl_version);
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        printf("FAIL global-init: curl_global_init failed\n");
        return 1;
    }
    curl = curl_easy_init();
    if (curl == NULL) {
        printf("FAIL easy-init: curl_easy_init returned NULL\n");
        curl_global_cleanup();
        return 1;
    }

    memset(&body, 0, sizeof body);
    /* TLS verification defaults are left untouched (no VERIFYPEER/VERIFYHOST here). */
    curl_easy_setopt(curl, CURLOPT_URL, argv[1]);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        printf("FAIL easy-get: curl_easy_perform: %s\n", curl_easy_strerror(rc));
        failures++;
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        printf("GET %s -> HTTP %ld, %zu bytes\n", argv[1], status, body.len);
        if (status != 200) {
            printf("FAIL easy-get: HTTP %ld != 200\n", status);
            failures++;
        } else if (strcmp(body.data, argv[2]) != 0) {
            printf("FAIL easy-body: got [%s] want [%s]\n", body.data, argv[2]);
            failures++;
        } else {
            printf("PASS easy-get: body matches expected\n");
        }
    }

    curl_easy_cleanup(curl);
    curl_global_cleanup();
    if (failures == 0) {
        printf("CURL_PROBE: ALL CHECKS PASSED\n");
        return 0;
    }
    printf("CURL_PROBE: %d CHECK(S) FAILED\n", failures);
    return 1;
}
