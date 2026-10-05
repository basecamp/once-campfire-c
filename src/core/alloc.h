/* Core-private allocation hook (F01 test support).
 *
 * buffer.c routes every allocation through these wrappers. Production builds
 * use malloc/realloc/free; tests in tests/core may install a counting or
 * failing allocator to exercise CORE-01 allocation-failure paths. This header
 * is confined to src/core and is not part of the shared contract in cf.h. */
#ifndef CF_CORE_ALLOC_H
#define CF_CORE_ALLOC_H

#include <stddef.h>

typedef void *(*cf_core_alloc_fn)(size_t size);
typedef void *(*cf_core_realloc_fn)(void *ptr, size_t size);
typedef void (*cf_core_free_fn)(void *ptr);

/* Install a complete replacement allocator; all three callbacks must be
 * non-NULL. Intended for single-threaded test setup only: do not change the
 * allocator while other threads may be allocating. */
void cf_core_set_allocator(cf_core_alloc_fn alloc, cf_core_realloc_fn realloc_fn,
                           cf_core_free_fn free_fn);

/* Restore malloc/realloc/free. */
void cf_core_reset_allocator(void);

void *cf_core_alloc(size_t size);
void *cf_core_realloc(void *ptr, size_t size);
void cf_core_free(void *ptr);

#endif
