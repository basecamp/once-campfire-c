/* See core/error.h. */
#include "core/error.h"

const char *cf_err_name(cf_err err) {
    switch (err) {
    case CF_OK:
        return "ok";
    case CF_NOMEM:
        return "nomem";
    case CF_LIMIT:
        return "limit";
    case CF_INVALID:
        return "invalid";
    case CF_NOT_FOUND:
        return "not_found";
    case CF_FORBIDDEN:
        return "forbidden";
    case CF_BUSY:
        return "busy";
    case CF_IO:
        return "io";
    case CF_DB:
        return "db";
    case CF_INTERNAL:
        return "internal";
    }
    return "unknown";
}
