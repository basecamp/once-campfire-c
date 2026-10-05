/* H02 module helpers for request parameters (01-foundation-http.md "H02").
 *
 * The public parameter API itself is the pre-approved subset declared in
 * src/cf.h (cf_params_parse, accessors). This header adds the two helpers
 * H02 contributes at module scope; they stay here until the integrator
 * approves their promotion into the shared contract:
 *
 *  - cf_effective_method: method override, computed before routing/CSRF.
 *    Mirrors kit/src/adapter.rs::method_override. POST only; a string form
 *    field `_method` when the body is form data, else the
 *    X-HTTP-Method-Override header, restricted to adapter.rs's fixed
 *    OVERRIDABLE_METHODS list. Never turns a non-POST request into anything.
 *
 *  - cf_params_merge: Rails Hash#merge! (ActiveSupport) over two parameter
 *    objects: a source entry replaces the same-named target value whole (in
 *    place, keeping its position; no recursive merge of object/array values)
 *    and appends otherwise. cf_params_parse uses it to merge the query tree
 *    over the body tree; A00 merges the route match's path parameters over
 *    the body/query tree with it (path wins), because cf_params_parse only
 *    sees body and query spans in cf_request.
 *
 * Both return cf_err; out/err semantics follow 00-contracts.md. */
#ifndef CF_HTTP_PARAMS_H
#define CF_HTTP_PARAMS_H

#include "cf.h"

/* Module-internal tree layout (00-contracts: "other headers contain
 * module-private or feature-specific types"). Consumers must use the cf.h
 * accessors; only params.c and H02's corpus test define
 * CF_HTTP_PARAMS_INTERNALS to read these fields (the corpus test compares the
 * exact stored shape, key order included). */
#ifdef CF_HTTP_PARAMS_INTERNALS

struct cf_param_entry {
    cf_span key; /* copied into the params arena */
    cf_param *value;
};

struct cf_param {
    cf_param_kind kind;
    union {
        cf_span string;                                   /* CF_PARAM_STRING */
        bool boolean;                                     /* CF_PARAM_BOOL */
        struct { bool integral; int64_t value; } number;  /* CF_PARAM_NUMBER */
        struct { cf_param **items; size_t len, cap; } array;
        struct { struct cf_param_entry *entries; size_t len, cap; } object;
    } u;
};

struct cf_params_block;

struct cf_params {
    cf_param root; /* always CF_PARAM_OBJECT */
    struct cf_params_block *blocks;
    size_t node_count; /* stored value nodes; the root is not counted */
};

#endif /* CF_HTTP_PARAMS_INTERNALS */

/* Effective request method. `req->method` is the method on the wire (or the
 * already-overridden method on a repeated call); the helper never mutates
 * `req`. On POST with form media (no/empty content type, urlencoded, or
 * multipart), the first string `_method` body field wins over the header;
 * when `_method` is present but not usable the header is NOT consulted
 * (reference or_else order). JSON and other non-form bodies never supply
 * `_method`, but the header fallback still applies.
 *
 * Returns CF_OK with *out set, CF_INVALID on NULL arguments or an unparsable
 * form body, CF_NOMEM/CF_LIMIT on resource/bound failures of that parse. */
cf_err cf_effective_method(const cf_request *req, cf_method *out);

/* Merge every top-level entry of `source` into `target` (deep copies into
 * target's storage; `source` may be destroyed afterwards). */
cf_err cf_params_merge(cf_params *target, const cf_params *source);

#endif /* CF_HTTP_PARAMS_H */
