/* Test-only helpers for building synthetic cf_request values (A00 tests).
 * Spans borrow the caller's storage and must outlive the request. Not part of
 * the application. */
#ifndef CF_TEST_REQUEST_H
#define CF_TEST_REQUEST_H

#include "cf.h"

#include <string.h>

static inline void cf_test_req_init(cf_request *req) {
    memset(req, 0, sizeof *req);
    req->method = CF_GET;
    req->original_method = CF_GET;
    req->path = (cf_span){(const unsigned char *)"/", 1};
    req->peer_ip = (cf_span){(const unsigned char *)"127.0.0.1", 9};
}

static inline cf_err cf_test_req_header(cf_request *req, cf_span name,
                                        cf_span value) {
    if (req->header_count >= 100) return CF_LIMIT;
    req->headers[req->header_count].name = name;
    req->headers[req->header_count].value = value;
    req->header_count++;
    return CF_OK;
}

#endif /* CF_TEST_REQUEST_H */
