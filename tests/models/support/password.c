/* tests/models/support/password.c — test-only cf_password_verify double.
 *
 * Test-support double, NOT production code. It stands in for A01's password
 * verifier so the tests/models binaries satisfy the user model's boundary
 * (src/models/user.c declares cf_password_verify; A01 owns the production
 * implementation) and link from committed files alone.
 *
 * Reference: tmp/rust-ref/crates/rails_compat/src/password.rs `verify`:
 * `bcrypt::verify(password, digest).unwrap_or(false)` — bcrypt-ruby `$2a$`
 * digests, only the first 72 password bytes count, a malformed digest is
 * false. Implemented with crypt_r from the pinned libxcrypt static artifact
 * (vendor/DEPS.json libxcrypt entry, vendor/build/libxcrypt-clang/.libs/
 * libcrypt.a); libxcrypt's bcrypt engines truncate at 72 bytes and return
 * NULL for malformed digests, which maps to the reference's false.
 */
#include "support.h"

#include "models/types.h"

#include <crypt.h>
#include <stdlib.h>
#include <string.h>

bool cf_password_verify(cf_str password, cf_str digest);

bool cf_password_verify(cf_str password, cf_str digest) {
    if (digest.ptr == NULL || digest.len == 0) return false;
    if (password.ptr == NULL && password.len != 0) return false;
    char *pw = malloc(password.len + 1);
    char *dg = malloc(digest.len + 1);
    if (pw == NULL || dg == NULL) {
        free(pw);
        free(dg);
        return false;
    }
    if (password.len != 0) memcpy(pw, password.ptr, password.len);
    pw[password.len] = '\0';
    memcpy(dg, digest.ptr, digest.len);
    dg[digest.len] = '\0';

    struct crypt_data data;
    memset(&data, 0, sizeof data);
    char *got = crypt_r(pw, dg, &data);
    bool ok = got != NULL && strcmp(got, dg) == 0;
    free(pw);
    free(dg);
    return ok;
}
