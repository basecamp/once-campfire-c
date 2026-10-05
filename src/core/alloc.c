/* Core-private allocation hook; see core/alloc.h. Defaults to malloc/free. */
#include "core/alloc.h"

#include <stdlib.h>

static cf_core_alloc_fn cf_alloc_impl = malloc;
static cf_core_realloc_fn cf_realloc_impl = realloc;
static cf_core_free_fn cf_free_impl = free;

void cf_core_set_allocator(cf_core_alloc_fn alloc, cf_core_realloc_fn realloc_fn,
                           cf_core_free_fn free_fn) {
    if (alloc == NULL || realloc_fn == NULL || free_fn == NULL) return;
    cf_alloc_impl = alloc;
    cf_realloc_impl = realloc_fn;
    cf_free_impl = free_fn;
}

void cf_core_reset_allocator(void) {
    cf_alloc_impl = malloc;
    cf_realloc_impl = realloc;
    cf_free_impl = free;
}

void *cf_core_alloc(size_t size) { return cf_alloc_impl(size); }

void *cf_core_realloc(void *ptr, size_t size) { return cf_realloc_impl(ptr, size); }

void cf_core_free(void *ptr) { cf_free_impl(ptr); }
