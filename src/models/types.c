/* src/models/types.c — canonical definitions of the shared model vocabulary
 * helpers declared in src/models/types.h.
 *
 * The D01 model headers landed before any model body owned these symbols;
 * ban.c / push_subscription.c / message.c carried weak fallbacks so each
 * module could link alone (reported as O3 in
 * docs/devel/evidence/D01-models-A-verify.md and item 2 in
 * docs/devel/evidence/D01-models-C-verify.md).  This translation unit is the
 * single strong definition of each helper; the weak fallbacks are removed.
 *
 * Semantics (frozen convention in types.h):
 *  - owned text is a malloc'd NUL-terminated cf_str; dispose frees it and
 *    resets the pair to {NULL, 0};
 *  - optional text resets to present=false with the value pair cleared;
 *  - model errors release every field/message pair, then the item array;
 *  - int64 vectors release the item array.
 * Every function is NULL-safe, accepts an already-empty value and leaves a
 * freshly zeroed value in place (idempotent dispose).
 *
 * Reference for the shapes: types.h (frozen contract) and the model evidence
 * files; no Rust function is translated here (the reference owns these bytes
 * per value through Drop).
 */
#include "models/types.h"

#include <stdlib.h>
#include <string.h>

void cf_str_dispose(cf_str *value) {
    if (value == NULL) return;
    free(value->ptr);
    value->ptr = NULL;
    value->len = 0;
}

void cf_optional_str_dispose(cf_optional_str *value) {
    if (value == NULL) return;
    cf_str_dispose(&value->value);
    value->present = false;
}

void cf_model_errors_dispose(cf_model_errors *errors) {
    if (errors == NULL) return;
    for (size_t i = 0; i < errors->len; i++) {
        cf_str_dispose(&errors->items[i].field);
        cf_str_dispose(&errors->items[i].message);
    }
    free(errors->items);
    errors->items = NULL;
    errors->len = 0;
    errors->cap = 0;
}

void cf_int64_vector_dispose(cf_int64_vector *vector) {
    if (vector == NULL) return;
    free(vector->items);
    vector->items = NULL;
    vector->len = 0;
    vector->cap = 0;
}
