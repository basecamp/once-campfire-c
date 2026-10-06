#ifndef CF_INTEGRATIONS_PUSH_HTTP_H
#define CF_INTEGRATIONS_PUSH_HTTP_H
#include "integrations/push.h"
/* Optional context supplies a CA path for an explicitly trusted test origin.
 * Production passes NULL and uses system trust. DNS/public-address guarding
 * is performed by cf_push_deliver before this prepared-request seam. */
typedef struct { const char *ca_path; } cf_push_http_context;
#define CF_PUSH_HTTP_RESPONSE_BYTES 65536u
#define CF_PUSH_HTTP_HEADER_BYTES 16384u
cf_err cf_push_http_exchange(void *ctx, const cf_push_request *request,
    unsigned *out_status, char *reason_buf, size_t reason_cap,
    cf_push_transport_error *transport_err);
#endif
