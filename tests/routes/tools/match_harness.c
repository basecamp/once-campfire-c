/* H03 differential harness (tests/routes/tools/diff_matcher.py builds and
 * drives it): read "VERB PATH" lines, print the recognized row id and its
 * decoded params (captures/defaults only, controller/action omitted). The
 * tree internals are read to compare the exact key set, like the corpus
 * test; this binary is a tool, never a test binary or application source. */
#define CF_HTTP_PARAMS_INTERNALS
#include "routes.h"
#include "http/params.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static cf_method verb_of(const char *s) {
    if (!strcmp(s,"GET")) return CF_GET; if (!strcmp(s,"HEAD")) return CF_HEAD;
    if (!strcmp(s,"POST")) return CF_POST; if (!strcmp(s,"PUT")) return CF_PUT;
    if (!strcmp(s,"PATCH")) return CF_PATCH; if (!strcmp(s,"DELETE")) return CF_DELETE;
    return CF_OTHER;
}
int main(void) {
    char line[16384];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n-1]=='\n' || line[n-1]=='\r')) line[--n]=0;
        char *sp = strchr(line, ' '); if (!sp) continue; *sp = 0;
        const char *path = sp+1;
        cf_request req; memset(&req,0,sizeof req);
        req.method = verb_of(line); req.original_method = req.method;
        req.path = (cf_span){(const unsigned char*)path, strlen(path)};
        cf_route_match m; memset(&m,0,sizeof m);
        cf_err rc = cf_route_match_request(&req,&m);
        if (rc == CF_NOT_FOUND) { printf("none\n"); continue; }
        if (rc != CF_OK) { printf("err=%d\n", (int)rc); continue; }
        printf("id=%u", m.id);
        if (m.path_params) {
            /* top-level entries in insertion order */
            for (size_t i=0;i<m.path_params->root.u.object.len;i++) {
                const struct cf_param_entry *e=&m.path_params->root.u.object.entries[i];
                const cf_param *v=e->value;
                cf_span s; if (cf_param_string(v,&s)!=CF_OK) continue;
                if ((e->key.len==10 && !memcmp(e->key.ptr,"controller",10)) ||
                    (e->key.len==6 && !memcmp(e->key.ptr,"action",6))) continue;
                printf("|%.*s=", (int)e->key.len, e->key.ptr);
                /* hex-encode value for safe transport */
                for (size_t k=0;k<s.len;k++) printf("%02x", s.ptr[k]);
            }
        }
        printf("\n");
        cf_route_match_dispose(&m);
    }
    return 0;
}
