/* H02 module helpers for request parameters (01-foundation-http.md "H02").
 *
 * The public parameter API itself is the pre-approved subset declared in
 * src/cf.h (cf_params_parse, accessors). This header adds the three helpers
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
 *  - cf_param_to_s: kit `Param::to_s`, the text Rails interpolates for a
 *    scalar parameter (strings as-is, numbers as serde_json's Number Display,
 *    true/false, nil as ""); CF_NOT_FOUND is the reference's None for an
 *    array, object or upload. cf_param_string keeps its H02 contract (a
 *    string kind only, CF_INVALID otherwise), because the reference
 *    distinguishes `as_str` from `to_s` (e.g. `params[:bot_key].to_s` reads
 *    a number, `param_str` does not); a number's text comes from the JSON
 *    source lexeme, retained and rendered at parse time because serde_json's
 *    integer/float split and zmij's shortest round-trip rendering cannot be
 *    recovered from an int64 alone.
 *
 * All return cf_err; out/err semantics follow 00-contracts.md. */
#ifndef CF_HTTP_PARAMS_H
#define CF_HTTP_PARAMS_H

#include "cf.h"

/* Module-internal tree layout (00-contracts: "other headers contain
 * module-private or feature-specific types"). Consumers must use the cf.h
 * accessors; only params.c and the H02 tests (the corpus test and
 * tests/http/test_params.c) define CF_HTTP_PARAMS_INTERNALS to read these
 * fields (the corpus test compares the exact stored shape, key order
 * included). */
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
        struct {
            /* Exact integrality for cf_param_i64 (unchanged H02 behavior). */
            bool integral;
            int64_t value;
            /* JSON numbers only: the exact source lexeme and its
             * serde_json Number Display text (`Param::to_s`), both copied
             * into the params arena. */
            cf_span raw;
            cf_span text;
        } number;                                         /* CF_PARAM_NUMBER */
        struct { cf_param **items; size_t len, cap; } array;
        struct { struct cf_param_entry *entries; size_t len, cap; } object;
        struct {
            /* CF_PARAM_UPLOAD: `ActionDispatch::Http::UploadedFile` as the
             * port keeps it — Rack-normalized original filename and the
             * part's declared Content-Type, both copied into the params
             * arena, plus the decoded byte size and a read-only FD on the
             * part's spool file (owned by the params; see cf.h). */
            cf_span filename;
            cf_span content_type; /* empty when has_content_type is false */
            bool has_content_type;
            int64_t size;
            int fd;
        } upload;
    } u;
};

struct cf_params_block;

/* One owned spool FD of the params (closed by cf_params_destroy). Upload
 * nodes borrow their FD from this list so a deep copy (cf_params_merge) can
 * dup it and stay independently disposable. */
struct cf_params_spool {
    struct cf_params_spool *next;
    int fd;
};

struct cf_params {
    cf_param root; /* always CF_PARAM_OBJECT */
    struct cf_params_block *blocks;
    struct cf_params_spool *spools;
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

/* kit `Param::to_s`: the Rails interpolation text of a scalar parameter.
 * strings as-is, numbers as serde_json's Number Display, bools "true"/"false",
 * nil as ""; CF_NOT_FOUND for a NULL param and for an array/object/upload
 * (the reference's None). The returned span is borrowed from the params
 * arena (or a static string for bools). */
cf_err cf_param_to_s(const cf_param *param, cf_span *out);

#endif /* CF_HTTP_PARAMS_H */
