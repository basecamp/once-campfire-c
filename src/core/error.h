/* Core-private error-name helper. cf_err_name is not in the frozen shared
 * header cf.h; F01 proposes adding `const char *cf_err_name(cf_err);` to
 * contracts/api.h so later modules can log sanitized operation+error names
 * without stringifying integers themselves. Until then this header is the
 * only declaration. */
#ifndef CF_CORE_ERROR_H
#define CF_CORE_ERROR_H

#include "cf.h"

/* Stable short lowercase name for a cf_err value; "unknown" for values
 * outside the enum. The returned string is static and never NULL. */
const char *cf_err_name(cf_err err);

#endif
