/* src/auth/password.c — has_secure_password's bcrypt through the pinned
 * libxcrypt build (rails_compat password.rs: `$2a$` digests, cost 12, only
 * the first 72 password bytes count).  The ratified A01 boundary
 * `cf_password_verify` also serves src/models/user.c.
 */
#include "internal.h"

#include <crypt.h>

#include <stdlib.h>
#include <string.h>

/* crypt_r takes C strings; bcrypt-ruby's C implementation reads to the first
 * NUL, which is the behavior both the D01 stand-in and the pinned Rust
 * verifier exhibit. */
static bool auth_crypt_matches(cf_span password, cf_span digest) {
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

bool cf_password_verify(cf_str password, cf_str digest) {
    return auth_crypt_matches(
        (cf_span){(const unsigned char *)password.ptr, password.len},
        (cf_span){(const unsigned char *)digest.ptr, digest.len});
}

cf_err cf_auth_password_digest(cf_str password, int cost, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (cost < CF_AUTH_BCRYPT_MIN_COST || cost > CF_AUTH_BCRYPT_MAX_COST) {
        return CF_INVALID;
    }
    if (password.ptr == NULL && password.len != 0) return CF_INVALID;
    /* BCrypt::Password.create: a fresh 16-byte salt from the OS entropy
     * source, then the `$2a$` hash. */
    unsigned char entropy[16];
    cf_err rc = cf_random_bytes(entropy, sizeof entropy);
    if (rc != CF_OK) return rc;
    char setting[CRYPT_GENSALT_OUTPUT_SIZE];
    memset(setting, 0, sizeof setting);
    if (crypt_gensalt_rn("$2a$", (unsigned long)cost,
                         (const char *)entropy, (int)sizeof entropy, setting,
                         (int)sizeof setting) == NULL) {
        return CF_INTERNAL;
    }
    char *pw = malloc(password.len + 1);
    if (pw == NULL) return CF_NOMEM;
    if (password.len != 0) memcpy(pw, password.ptr, password.len);
    pw[password.len] = '\0';
    struct crypt_data data;
    memset(&data, 0, sizeof data);
    char *got = crypt_r(pw, setting, &data);
    free(pw);
    if (got == NULL) return CF_INTERNAL;
    rc = auth_str_dup(auth_cstr_span(got), out);
    return rc;
}
