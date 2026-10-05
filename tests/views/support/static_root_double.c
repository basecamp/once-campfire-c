/* Test double for H03's cf_static_root (src/routes.c is not linked into the
 * views render tests): the pinned fixture tree, relative to the repo root,
 * like the documented CF_STATIC_ROOT default. */
#include "routes.h"

const char *cf_static_root(void) { return "tests/fixtures/assets"; }
