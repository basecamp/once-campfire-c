/* cf_builder / cf_buf implementation.
 *
 * Semantics per cf.h comments and 00-contracts.md "Fixed ownership":
 *  - cf_buf is ONE allocation: header (atomic refcount, size) followed by the
 *    immutable payload. retain/release only move the reference count.
 *  - Growth is checked: arithmetic overflow is CF_LIMIT before any
 *    allocation; allocation failure is CF_NOMEM with out pointers left empty
 *    and the builder unchanged.
 *  - cf_builder_freeze consumes the builder only on CF_OK.
 *  - Destructors accept NULL/empty state and reset what they destroy.
 *  - retain is valid only for a caller that already owns a reference (or
 *    holds the owning cache/queue lock); it never resurrects a pointer. */
#include "cf.h"

#include "core/alloc.h"

#include <stdatomic.h>
#include <string.h>

struct cf_buf {
    atomic_size_t refs;
    size_t len;
    unsigned char bytes[];
};

#define CF_BUILDER_MIN_CAP ((size_t)64)

/* Ensures capacity for b->len + add without changing b on failure. */
static cf_err cf_builder_reserve(cf_builder *b, size_t add) {
    if (add > SIZE_MAX - b->len) return CF_LIMIT; /* len + add overflow */
    size_t need = b->len + add;
    if (need <= b->cap) return CF_OK;
    size_t cap = b->cap != 0 ? b->cap : CF_BUILDER_MIN_CAP;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    unsigned char *ptr = cf_core_realloc(b->ptr, cap);
    if (ptr == NULL) return CF_NOMEM;
    b->ptr = ptr;
    b->cap = cap;
    return CF_OK;
}

cf_err cf_builder_append(cf_builder *b, cf_span s) {
    if (b == NULL) return CF_INVALID;
    if (s.len != 0 && s.ptr == NULL) return CF_INVALID;
    cf_err rc = cf_builder_reserve(b, s.len);
    if (rc != CF_OK) return rc;
    if (s.len != 0) {
        if (b->ptr == NULL) return CF_INVALID; /* inconsistent builder */
        memcpy(b->ptr + b->len, s.ptr, s.len);
    }
    b->len += s.len;
    return CF_OK;
}

void cf_builder_dispose(cf_builder *b) {
    if (b == NULL) return;
    cf_core_free(b->ptr);
    b->ptr = NULL;
    b->len = 0;
    b->cap = 0;
}

cf_err cf_buf_copy(cf_span s, cf_buf **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (s.len != 0 && s.ptr == NULL) return CF_INVALID;
    if (s.len > SIZE_MAX - sizeof(struct cf_buf)) return CF_LIMIT;
    cf_buf *buf = cf_core_alloc(sizeof(struct cf_buf) + s.len);
    if (buf == NULL) return CF_NOMEM;
    atomic_init(&buf->refs, (size_t)1);
    buf->len = s.len;
    if (s.len != 0) memcpy(buf->bytes, s.ptr, s.len);
    *out = buf;
    return CF_OK;
}

cf_err cf_builder_freeze(cf_builder *b, cf_buf **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (b == NULL) return CF_INVALID;
    if (b->len > SIZE_MAX - sizeof(struct cf_buf)) return CF_LIMIT;
    if (b->len != 0 && b->ptr == NULL) return CF_INVALID;
    cf_buf *buf = cf_core_alloc(sizeof(struct cf_buf) + b->len);
    if (buf == NULL) return CF_NOMEM; /* builder remains usable */
    atomic_init(&buf->refs, (size_t)1);
    buf->len = b->len;
    if (b->len != 0) memcpy(buf->bytes, b->ptr, b->len);
    cf_core_free(b->ptr); /* consume the builder only on success */
    b->ptr = NULL;
    b->len = 0;
    b->cap = 0;
    *out = buf;
    return CF_OK;
}

cf_buf *cf_buf_retain(cf_buf *b) {
    if (b == NULL) return NULL;
    atomic_fetch_add_explicit(&b->refs, (size_t)1, memory_order_relaxed);
    return b;
}

void cf_buf_release(cf_buf *b) {
    if (b == NULL) return;
    if (atomic_fetch_sub_explicit(&b->refs, (size_t)1, memory_order_acq_rel) == 1) {
        cf_core_free(b); /* the last reference frees the single allocation once */
    }
}

cf_span cf_buf_span(const cf_buf *b) {
    cf_span s;
    s.ptr = NULL;
    s.len = 0;
    if (b != NULL) {
        s.ptr = b->bytes;
        s.len = b->len;
    }
    return s;
}
