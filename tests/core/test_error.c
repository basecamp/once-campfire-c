/* F01 error-name tests: stable short names for sanitized logs. */
#include "cf.h"

#include "core/error.h"

#include "check.h"

#include <string.h>

int main(void) {
    static const struct {
        cf_err err;
        const char *name;
    } table[] = {
        {CF_OK, "ok"},         {CF_NOMEM, "nomem"},
        {CF_LIMIT, "limit"},   {CF_INVALID, "invalid"},
        {CF_NOT_FOUND, "not_found"}, {CF_FORBIDDEN, "forbidden"},
        {CF_BUSY, "busy"},     {CF_IO, "io"},
        {CF_DB, "db"},         {CF_INTERNAL, "internal"},
    };
    const size_t count = sizeof table / sizeof table[0];
    for (size_t i = 0; i < count; i++) {
        const char *name = cf_err_name(table[i].err);
        CHECK(name != NULL && strcmp(name, table[i].name) == 0);
        CHECK(name[0] != '\0');
        for (size_t j = i + 1; j < count; j++) {
            CHECK(strcmp(name, cf_err_name(table[j].err)) != 0);
        }
    }
    CHECK(strcmp(cf_err_name((cf_err)123), "unknown") == 0);
    return check_summary("test_error");
}
