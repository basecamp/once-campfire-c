#include "integrations/push_http.h"
#include "integrations/http.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

cf_err cf_push_http_exchange(void *ctx, const cf_push_request *request,
    unsigned *out_status, char *reason_buf, size_t reason_cap,
    cf_push_transport_error *transport_err) {
    if (!out_status || !transport_err || (reason_cap && !reason_buf)) return CF_INVALID;
    *out_status = 0;
    *transport_err = CF_PUSH_TRANSPORT_IO;
    if (reason_cap) reason_buf[0] = '\0';
    if (!request || !request->endpoint_url || !request->host || !request->resolved_ip ||
        !request->target || !request->port || (request->body_len && !request->body) ||
        /* Ciphertext cap excludes salt, size and ephemeral public key. */
        request->body_len > CF_PUSH_MAX_RECORD_BYTES + 16u + 4u + 1u + 65u ||
        request->headers.len > CF_PUSH_HTTP_HEADER_BYTES / 3u ||
        (request->headers.len && !request->headers.items)) return CF_INVALID;
    unsigned char address[16];
    bool v6 = inet_pton(AF_INET6, request->resolved_ip, address) == 1;
    if (!v6 && inet_pton(AF_INET, request->resolved_ip, address) != 1) return CF_INVALID;
    cf_http_uri u;
    if (cf_http_uri_parse(request->endpoint_url, &u) != CF_HTTP_OK) return CF_INVALID;
    char *target = cf_http_uri_target(&u);
    bool valid = u.scheme && !strcasecmp(u.scheme,"https") && u.host &&
        !strcasecmp(u.host,request->host) && u.port == request->port &&
        !u.userinfo && !u.fragment && target && !strcmp(target,request->target);
    free(target); cf_http_uri_dispose(&u);
    if (!valid) return CF_INVALID;
    size_t host_len = strlen(request->host);
    /* CURLOPT_RESOLVE expects an unbracketed origin host. */
    const char *host = request->host;
    if (host_len >= 2 && host[0]=='[' && host[host_len-1]==']') {host++;host_len-=2;}
    size_t pin_len = host_len + strlen(request->resolved_ip) + 32;
    char *pin = malloc(pin_len);
    char **headers = calloc(request->headers.len + 1,sizeof *headers);
    if (!pin || !headers) {free(pin);free(headers);return CF_NOMEM;}
    snprintf(pin,pin_len,v6 ? "%.*s:%u:[%s]" : "%.*s:%u:%s",
        (int)host_len,host,request->port,request->resolved_ip);
    cf_err result = CF_OK;
    size_t header_bytes = 0;
    for (size_t i=0;i<request->headers.len;i++) {
        const cf_push_header *h = &request->headers.items[i];
        if (!h->name || !h->value || !*h->name || strpbrk(h->name,":\r\n") ||
            strpbrk(h->value,"\r\n")) {result=CF_INVALID;goto done;}
        size_t n = strlen(h->name) + strlen(h->value) + 3;
        if (n > CF_PUSH_HTTP_HEADER_BYTES - header_bytes) {result=CF_INVALID;goto done;}
        header_bytes += n;
        headers[i]=malloc(n);
        if (!headers[i]) {result=CF_NOMEM;goto done;}
        snprintf(headers[i],n,"%s: %s",h->name,h->value);
    }
    const cf_push_http_context *context = ctx;
    cf_http_config cfg = {.connect_timeout_ms=CF_PUSH_TIMEOUT_CONNECT_S*1000L,
        .read_timeout_ms=CF_PUSH_TIMEOUT_READ_S*1000L,.deadline_ms=CF_PUSH_TIMEOUT_TOTAL_S*1000L,
        .max_bytes=CF_PUSH_HTTP_RESPONSE_BYTES,.length_cap=CF_PUSH_HTTP_RESPONSE_BYTES,
        .max_header_bytes=CF_PUSH_HTTP_HEADER_BYTES,.ca_path=context ? context->ca_path : NULL};
    cf_http_response response;
    const char *pins[]={pin};
    cf_http_err e=cf_http_exchange(&cfg,"POST",request->endpoint_url,
        (const char *const *)headers,request->headers.len,request->body,request->body_len,
        pins,1,&response);
    if (e==CF_HTTP_OK) {
        if (response.status >= 100 && response.status <= 599) {
            *out_status=response.status; *transport_err=CF_PUSH_TRANSPORT_OK;
            if (reason_cap) snprintf(reason_buf,reason_cap,"%s",response.reason);
        }
    } else if (e==CF_HTTP_TLS) *transport_err=CF_PUSH_TRANSPORT_TLS;
    else if (e==CF_HTTP_TIMEOUT_CONN) *transport_err=CF_PUSH_TRANSPORT_OPEN_TIMEOUT;
    else if (e==CF_HTTP_TIMEOUT_READ || e==CF_HTTP_TIMEOUT_ALL) *transport_err=CF_PUSH_TRANSPORT_READ_TIMEOUT;
    cf_http_response_dispose(&response);
done:
    for (size_t i=0;i<request->headers.len;i++) free(headers[i]);
    free(headers); free(pin); return result;
}
