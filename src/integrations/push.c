/* src/integrations/push.c — Web Push delivery (I02).
 *
 * Port of the pinned `web_push.rs` and `web_push/{encryption,vapid,pool}.rs`
 * using OpenSSL EVP operations only (ECDH/ECDSA via EVP, HKDF via HMAC,
 * AES-128-GCM via the EVP cipher interface). No new crypto format and no
 * unauthenticated encryption: the record framing, HKDF inputs, VAPID claims
 * and headers match the pinned vectors byte for byte.
 *
 * Nothing here performs I/O: the exchange seam (owned by I01) sends the
 * framed request with the mandated timeouts. Nothing here logs endpoint or
 * key material: outcome details keep only the service host and status.
 */
#include "integrations/push.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>
#include <openssl/params.h>
#include <openssl/sha.h>

/* --- small owned-memory helpers ---------------------------------------- */

static cf_err push_nomem(void) { return CF_NOMEM; }

static char *push_dup_n(const char *s, size_t len) {
    char *copy = malloc(len + 1);
    if (copy == NULL) return NULL;
    if (len != 0) memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

static char *push_dup(const char *s) {
    if (s == NULL) return NULL;
    return push_dup_n(s, strlen(s));
}

/* --- base64url ------------------------------------------------------------
 * Encode is urlsafe without padding. Decode accepts either alphabet with
 * optional padding, then runs a strict standard decode (like Ruby's
 * `urlsafe_decode64` via STANDARD): canonical padding only, and the
 * trailing bits of a partial quantum must be zero.
 */

static const char push_b64u_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int push_b64_value(char c, bool *url) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') {
        if (c == '-') *url = true;
        return 62;
    }
    if (c == '/' || c == '_') {
        if (c == '_') *url = true;
        return 63;
    }
    return -1;
}

cf_err cf_push_b64u_encode(const unsigned char *in, size_t len, char **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (len != 0 && in == NULL) return CF_INVALID;
    if (len > (SIZE_MAX - 4) / 4 * 3) return CF_LIMIT;
    size_t cap = (len + 2) / 3 * 4 + 1;
    char *text = malloc(cap);
    if (text == NULL) return push_nomem();
    size_t at = 0;
    for (size_t i = 0; i < len; i += 3) {
        size_t remain = len - i;
        unsigned b0 = in[i];
        unsigned b1 = remain > 1 ? in[i + 1] : 0;
        unsigned b2 = remain > 2 ? in[i + 2] : 0;
        text[at++] = push_b64u_alphabet[b0 >> 2];
        text[at++] = push_b64u_alphabet[((b0 & 0x03) << 4) | (b1 >> 4)];
        if (remain > 1) text[at++] = push_b64u_alphabet[((b1 & 0x0f) << 2) | (b2 >> 6)];
        if (remain > 2) text[at++] = push_b64u_alphabet[b2 & 0x3f];
    }
    text[at] = '\0';
    *out = text;
    return CF_OK;
}

cf_err cf_push_b64u_decode(const char *in, unsigned char **out,
                            size_t *out_len) {
    if (out == NULL || out_len == NULL) return CF_INVALID;
    *out = NULL;
    *out_len = 0;
    if (in == NULL) return CF_INVALID;
    size_t len = strlen(in);
    if (len == 0) {
        unsigned char *empty = malloc(1);
        if (empty == NULL) return push_nomem();
        *out = empty;
        return CF_OK;
    }
    if (len > (SIZE_MAX - 4)) return CF_LIMIT;
    /* Translate `-_` to `+/`, then pad a short unpadded string (but never
     * touch an explicitly padded one), exactly like the reference. */
    char *work = push_dup(in);
    if (work == NULL) return push_nomem();
    bool padded = len != 0 && in[len - 1] == '=';
    for (size_t i = 0; i < len; i++) {
        if (work[i] == '-') work[i] = '+';
        else if (work[i] == '_') work[i] = '/';
    }
    size_t wlen = len;
    if (!padded && (wlen % 4) != 0) {
        size_t need = 4 - (wlen % 4);
        char *grown = realloc(work, wlen + need + 1);
        if (grown == NULL) {
            free(work);
            return push_nomem();
        }
        work = grown;
        for (size_t i = 0; i < need; i++) work[wlen++] = '=';
        work[wlen] = '\0';
    }
    if ((wlen % 4) != 0) {
        free(work);
        return CF_INVALID;
    }
    /* Strict standard decode. */
    size_t pad = 0;
    if (wlen >= 1 && work[wlen - 1] == '=') pad++;
    if (wlen >= 2 && work[wlen - 2] == '=') pad++;
    if (pad > 2) {
        free(work);
        return CF_INVALID;
    }
    for (size_t i = 0; i < wlen - pad; i++) {
        bool url = false;
        if (work[i] == '=' || push_b64_value(work[i], &url) < 0) {
            free(work);
            return CF_INVALID;
        }
    }
    for (size_t i = wlen - pad; i < wlen; i++) {
        if (work[i] != '=') {
            free(work);
            return CF_INVALID;
        }
    }
    size_t raw = wlen / 4 * 3;
    if (pad != 0) raw -= pad;
    else if (wlen != 0) {
        /* No padding is only valid for a full final quantum. */
    }
    unsigned char *bytes = malloc(raw != 0 ? raw : 1);
    if (bytes == NULL) {
        free(work);
        return push_nomem();
    }
    size_t at = 0;
    for (size_t i = 0; i < wlen; i += 4) {
        bool url = false;
        int v[4];
        for (int k = 0; k < 4; k++) {
            if (work[i + (size_t)k] == '=') v[k] = 0;
            else {
                v[k] = push_b64_value(work[i + (size_t)k], &url);
                if (v[k] < 0) {
                    free(work);
                    free(bytes);
                    return CF_INVALID;
                }
            }
        }
        bool last = (i + 4) == wlen;
        if (last && pad == 2 && (v[1] & 0x0f) != 0) {
            free(work);
            free(bytes);
            return CF_INVALID; /* non-zero trailing bits, like Ruby */
        }
        if (last && pad == 1 && (v[2] & 0x03) != 0) {
            free(work);
            free(bytes);
            return CF_INVALID;
        }
        if (last && pad == 0 && wlen != 0) {
            /* Full quantum: nothing to check. */
        }
        bytes[at++] = (unsigned char)((v[0] << 2) | (v[1] >> 4));
        if (at < raw) bytes[at++] = (unsigned char)(((v[1] & 0x0f) << 4) | (v[2] >> 2));
        if (at < raw) bytes[at++] = (unsigned char)(((v[2] & 0x03) << 6) | v[3]);
    }
    if (at != raw) {
        free(work);
        free(bytes);
        return CF_INTERNAL;
    }
    free(work);
    *out = bytes;
    *out_len = raw;
    return CF_OK;
}

/* --- JSON text (serde_json-compatible, plain, no HTML escaping) -----------
 * `"` and `\` plus the short escapes take two bytes; other controls take
 * six (`\u00xx`, lowercase hex, like serde_json); everything else passes
 * through as UTF-8, including non-ASCII and U+2028.
 */

static void push_json_append_escaped(char **at, char c) {
    char *p = *at;
    switch (c) {
    case '"': *p++ = '\\'; *p++ = '"'; break;
    case '\\': *p++ = '\\'; *p++ = '\\'; break;
    case '\n': *p++ = '\\'; *p++ = 'n'; break;
    case '\r': *p++ = '\\'; *p++ = 'r'; break;
    case '\t': *p++ = '\\'; *p++ = 't'; break;
    case '\b': *p++ = '\\'; *p++ = 'b'; break;
    case '\f': *p++ = '\\'; *p++ = 'f'; break;
    default: {
        static const char hex[] = "0123456789abcdef";
        unsigned char u = (unsigned char)c;
        *p++ = '\\'; *p++ = 'u'; *p++ = '0'; *p++ = '0';
        *p++ = hex[(u >> 4) & 0x0f]; *p++ = hex[u & 0x0f];
        break;
    }
    }
    *at = p;
}

static size_t push_json_escaped_len(const char *s, size_t len) {
    size_t total = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t' ||
            c == 0x08 || c == 0x0c) {
            total += 2;
        } else if (c < 0x20) {
            total += 6;
        } else {
            total += 1;
        }
    }
    return total;
}

static cf_err push_json_string(const char *s, char **cursor, char *end) {
    size_t len = s != NULL ? strlen(s) : 0;
    char *p = *cursor;
    if ((size_t)(end - p) < push_json_escaped_len(s != NULL ? s : "", len) + 2) return CF_INTERNAL;
    *p++ = '"';
    if (s != NULL) {
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t' ||
                c == 0x08 || c == 0x0c || c < 0x20) {
                push_json_append_escaped(&p, (char)c);
            } else {
                *p++ = (char)c;
            }
        }
    }
    *p++ = '"';
    *cursor = p;
    return CF_OK;
}

cf_err cf_push_notification_json(const char *title, const char *body,
                                  const char *path, int64_t badge, char **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (title == NULL || body == NULL || path == NULL) return CF_INVALID;
    char badge_text[32];
    int written = snprintf(badge_text, sizeof badge_text, "%" PRId64, badge);
    if (written < 0 || (size_t)written >= sizeof badge_text) return CF_INTERNAL;
    /* {"title":"...","options":{"body":"...","icon":"...","data":{"path":"...","badge":N}}} */
    static const char prefix_title[] = "{\"title\":";
    static const char prefix_options[] = ",\"options\":{\"body\":";
    static const char prefix_icon[] = ",\"icon\":\"";
    static const char prefix_data[] = "\",\"data\":{\"path\":";
    static const char prefix_badge[] = ",\"badge\":";
    static const char suffix[] = "}}}";
    size_t need = sizeof(prefix_title) - 1 + push_json_escaped_len(title, strlen(title)) + 2 +
        sizeof(prefix_options) - 1 + push_json_escaped_len(body, strlen(body)) + 2 +
        sizeof(prefix_icon) - 1 + strlen(CF_PUSH_ICON_PATH) +
        sizeof(prefix_data) - 1 + push_json_escaped_len(path, strlen(path)) + 2 +
        sizeof(prefix_badge) - 1 + (size_t)written + sizeof(suffix) - 1 + 1;
    char *json = malloc(need);
    if (json == NULL) return push_nomem();
    char *p = json;
    char *end = json + need;
    size_t prefix_len = sizeof(prefix_title) - 1;
    memcpy(p, prefix_title, prefix_len);
    p += prefix_len;
    cf_err err = push_json_string(title, &p, end);
    if (err == CF_OK) {
        prefix_len = sizeof(prefix_options) - 1;
        memcpy(p, prefix_options, prefix_len);
        p += prefix_len;
        err = push_json_string(body, &p, end);
    }
    if (err == CF_OK) {
        prefix_len = sizeof(prefix_icon) - 1;
        memcpy(p, prefix_icon, prefix_len);
        p += prefix_len;
        prefix_len = strlen(CF_PUSH_ICON_PATH);
        memcpy(p, CF_PUSH_ICON_PATH, prefix_len);
        p += prefix_len;
        prefix_len = sizeof(prefix_data) - 1;
        memcpy(p, prefix_data, prefix_len);
        p += prefix_len;
        err = push_json_string(path, &p, end);
    }
    if (err == CF_OK) {
        prefix_len = sizeof(prefix_badge) - 1;
        memcpy(p, prefix_badge, prefix_len);
        p += prefix_len;
        prefix_len = (size_t)written;
        memcpy(p, badge_text, prefix_len);
        p += prefix_len;
        prefix_len = sizeof(suffix) - 1;
        memcpy(p, suffix, prefix_len);
        p += prefix_len;
        *p = '\0';
    }
    if (err != CF_OK) {
        free(json);
        return err;
    }
    *out = json;
    return CF_OK;
}

cf_err cf_push_test_body(char out[37]) {
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[16];
    if (out == NULL) return CF_INVALID;
    if (cf_random_bytes(raw, sizeof raw) != CF_OK) return CF_IO;
    raw[6] = (unsigned char)((raw[6] & 0x0f) | 0x40); /* version 4 */
    raw[8] = (unsigned char)((raw[8] & 0x3f) | 0x80); /* variant 10 */
    size_t at = 0;
    for (size_t i = 0; i < sizeof raw; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[at++] = '-';
        out[at++] = hex[(raw[i] >> 4) & 0x0f];
        out[at++] = hex[raw[i] & 0x0f];
    }
    out[at] = '\0';
    return CF_OK;
}

cf_err cf_push_audience(const char *scheme, const char *host, char **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (scheme == NULL || host == NULL) return CF_INVALID;
    size_t scheme_len = strlen(scheme);
    size_t host_len = strlen(host);
    if (scheme_len > SIZE_MAX - host_len - 4) return CF_LIMIT;
    char *audience = malloc(scheme_len + 3 + host_len + 1);
    if (audience == NULL) return push_nomem();
    for (size_t i = 0; i < scheme_len; i++) {
        char c = scheme[i];
        audience[i] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
    }
    audience[scheme_len] = ':';
    audience[scheme_len + 1] = '/';
    audience[scheme_len + 2] = '/';
    memcpy(audience + scheme_len + 3, host, host_len + 1);
    *out = audience;
    return CF_OK;
}

/* --- P-256 helpers (OpenSSL EVP, no hand-rolled ECC) --------------------- */

static EC_GROUP *push_group(void) {
    return EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
}

/* Parse an uncompressed P-256 point; false when it is not one. */
static bool push_parse_point(EC_GROUP *group, const unsigned char *bytes,
                              size_t len, EC_POINT *point) {
    if (len == 0 || len > 256 || bytes == NULL) return false;
    BN_CTX *ctx = BN_CTX_new();
    if (ctx == NULL) return false;
    bool ok = EC_POINT_oct2point(group, point, bytes, len, ctx) == 1 &&
        EC_POINT_is_on_curve(group, point, ctx) == 1;
    BN_CTX_free(ctx);
    return ok;
}

/* Canonical 65-byte uncompressed encoding; false on failure. */
static bool push_point_oct(EC_GROUP *group, const EC_POINT *point,
                            unsigned char out[65]) {
    BN_CTX *ctx = BN_CTX_new();
    if (ctx == NULL) return false;
    size_t len = EC_POINT_point2oct(group, point, POINT_CONVERSION_UNCOMPRESSED,
                                     out, 65, ctx);
    BN_CTX_free(ctx);
    return len == 65;
}

/* Whether the 32-byte big-endian scalar is a usable P-256 private key. */
static bool push_scalar_ok(EC_GROUP *group, const unsigned char scalar[32],
                            BIGNUM **bn_out) {
    bool ok = false;
    BIGNUM *bn = BN_bin2bn(scalar, 32, NULL);
    BIGNUM *order = BN_new();
    BN_CTX *ctx = BN_CTX_new();
    if (bn == NULL || order == NULL || ctx == NULL) goto done;
    if (EC_GROUP_get_order(group, order, ctx) != 1) goto done;
    if (BN_is_zero(bn) || BN_cmp(bn, order) >= 0) goto done;
    ok = true;
done:
    if (!ok) BN_free(bn);
    else if (bn_out != NULL) *bn_out = bn;
    else BN_free(bn);
    BN_free(order);
    BN_CTX_free(ctx);
    return ok;
}

/* Public point for a valid scalar; false on failure. */
static bool push_derive_pub(EC_GROUP *group, const BIGNUM *priv_bn,
                             EC_POINT *pub) {
    BN_CTX *ctx = BN_CTX_new();
    if (ctx == NULL) return false;
    bool ok = EC_POINT_mul(group, pub, priv_bn, NULL, NULL, ctx) == 1;
    BN_CTX_free(ctx);
    return ok;
}

/* ECDH: the 32-byte big-endian x-coordinate of priv*peer. */
static bool push_ecdh_x(EC_GROUP *group, const BIGNUM *priv_bn,
                         const EC_POINT *peer, unsigned char out[32]) {
    bool ok = false;
    BN_CTX *ctx = BN_CTX_new();
    EC_POINT *shared = EC_POINT_new(group);
    BIGNUM *x = BN_new();
    if (ctx == NULL || shared == NULL || x == NULL) goto done;
    if (EC_POINT_mul(group, shared, NULL, peer, priv_bn, ctx) != 1) goto done;
    if (EC_POINT_is_at_infinity(group, shared) == 1) goto done;
    if (EC_POINT_get_affine_coordinates(group, shared, x, NULL, ctx) != 1) goto done;
    if (BN_bn2binpad(x, out, 32) != 32) goto done;
    ok = true;
done:
    EC_POINT_free(shared);
    BN_free(x);
    BN_CTX_free(ctx);
    return ok;
}

/* EVP private key for signing from a valid scalar. Built with the classic
 * EC_KEY constructor (deprecation-suppressed for the pinned OpenSSL 3
 * build): the BIGNUM import is explicitly big-endian. The provider's
 * OSSL_PARAM "priv" import is NOT used here — this build reads it
 * little-endian (verified empirically against the VAPID test pair), so a
 * fromdata key would sign with the wrong scalar. */
static EVP_PKEY *push_signing_key(const unsigned char scalar[32]) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    EVP_PKEY *key = NULL;
    EC_KEY *ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    BIGNUM *bn = BN_bin2bn(scalar, 32, NULL);
    EC_POINT *pub = NULL;
    BN_CTX *ctx = BN_CTX_new();
    const EC_GROUP *group = NULL;
    if (ec == NULL || bn == NULL || ctx == NULL) goto done;
    group = EC_KEY_get0_group(ec);
    pub = EC_POINT_new(group);
    if (pub == NULL) goto done;
    if (EC_POINT_mul(group, pub, bn, NULL, NULL, ctx) != 1) goto done;
    if (EC_KEY_set_private_key(ec, bn) != 1) goto done;
    if (EC_KEY_set_public_key(ec, pub) != 1) goto done;
    key = EVP_PKEY_new();
    if (key == NULL) goto done;
    if (EVP_PKEY_set1_EC_KEY(key, ec) != 1) {
        EVP_PKEY_free(key);
        key = NULL;
    }
done:
    EC_KEY_free(ec);
    BN_free(bn);
    EC_POINT_free(pub);
    BN_CTX_free(ctx);
#pragma GCC diagnostic pop
    return key;
}

/* --- HKDF-SHA256 (RFC 5869, via HMAC) ------------------------------------- */

static void push_hmac_sha256(const unsigned char *key, size_t key_len,
                              const unsigned char *data, size_t data_len,
                              unsigned char out[32]) {
    unsigned int len = 0;
    HMAC(EVP_sha256(), key, (int)key_len, data, data_len, out, &len);
}

static cf_err push_hkdf(const unsigned char *salt, size_t salt_len,
                         const unsigned char *ikm, size_t ikm_len,
                         const unsigned char *info, size_t info_len,
                         unsigned char *okm, size_t okm_len) {
    static const size_t hash_len = 32;
    unsigned char zeros[32];
    unsigned char prk[32];
    unsigned char previous[32];
    size_t previous_len = 0;
    size_t done = 0;
    unsigned char counter = 1;
    if (okm == NULL || okm_len == 0 || okm_len > 255 * hash_len) return CF_INVALID;
    if ((salt_len != 0 && salt == NULL) || (ikm_len != 0 && ikm == NULL) ||
        (info_len != 0 && info == NULL)) {
        return CF_INVALID;
    }
    if (salt_len == 0) {
        memset(zeros, 0, sizeof zeros);
        salt = zeros;
        salt_len = sizeof zeros;
    }
    push_hmac_sha256(salt, salt_len, ikm_len != 0 ? ikm : zeros, ikm_len, prk);
    while (done < okm_len) {
        unsigned char *block = malloc(previous_len + info_len + 1);
        if (block == NULL) {
            OPENSSL_cleanse(prk, sizeof prk);
            return push_nomem();
        }
        if (previous_len != 0) memcpy(block, previous, previous_len);
        if (info_len != 0) memcpy(block + previous_len, info, info_len);
        block[previous_len + info_len] = counter;
        push_hmac_sha256(prk, sizeof prk, block, previous_len + info_len + 1, previous);
        free(block);
        size_t take = okm_len - done < hash_len ? okm_len - done : hash_len;
        memcpy(okm + done, previous, take);
        done += take;
        previous_len = hash_len;
        counter++;
    }
    OPENSSL_cleanse(prk, sizeof prk);
    OPENSSL_cleanse(previous, sizeof previous);
    return CF_OK;
}

/* --- AES-128-GCM ------------------------------------------------------------ */

static cf_err push_aes128gcm_encrypt(const unsigned char key[16],
                                      const unsigned char nonce[12],
                                      const unsigned char *plain, size_t plain_len,
                                      unsigned char **out, size_t *out_len) {
    cf_err err = CF_OK;
    EVP_CIPHER_CTX *ctx = NULL;
    unsigned char *cipher = NULL;
    unsigned char tag[16];
    int step = 0;
    int final_step = 0;
    if (out == NULL || out_len == NULL) return CF_INVALID;
    *out = NULL;
    *out_len = 0;
    if (plain_len != 0 && plain == NULL) return CF_INVALID;
    if (plain_len > SIZE_MAX - 16) return CF_LIMIT;
    cipher = malloc(plain_len + 16);
    if (cipher == NULL) return push_nomem();
    ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) {
        err = CF_INTERNAL;
        goto done;
    }
    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1 ||
        EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1 ||
        EVP_EncryptUpdate(ctx, cipher, &step, plain, (int)plain_len) != 1 ||
        EVP_EncryptFinal_ex(ctx, cipher + step, &final_step) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) {
        err = CF_INTERNAL;
        goto done;
    }
    memcpy(cipher + step + final_step, tag, sizeof tag);
    *out = cipher;
    *out_len = (size_t)step + (size_t)final_step + sizeof tag;
    cipher = NULL;
done:
    EVP_CIPHER_CTX_free(ctx);
    free(cipher);
    return err;
}

/* --- encryption ---------------------------------------------------------------
 * `info`: "WebPush: info\0" + client point (leading zeros stripped, as
 * `OpenSSL::BN.new(bytes, 2)` decodes it) + server point.
 */

static bool push_blank(const char *s) { return s == NULL || s[0] == '\0'; }

/* Strip leading zero bytes (OpenSSL BN decoding); returns a fresh buffer. */
static cf_err push_strip_zeros(const unsigned char *in, size_t len,
                                unsigned char **out, size_t *out_len) {
    size_t zeros = 0;
    unsigned char *copy;
    if (out == NULL || out_len == NULL) return CF_INVALID;
    *out = NULL;
    *out_len = 0;
    while (zeros < len && in[zeros] == 0) zeros++;
    copy = malloc(len - zeros != 0 ? len - zeros : 1);
    if (copy == NULL) return push_nomem();
    if (len - zeros != 0) memcpy(copy, in + zeros, len - zeros);
    *out = copy;
    *out_len = len - zeros;
    return CF_OK;
}

const char *cf_push_crypt_error_name(cf_push_crypt_error error) {
    switch (error) {
    case CF_PUSH_CRYPT_OK: return "ok";
    case CF_PUSH_CRYPT_ARGUMENT: return "argument";
    case CF_PUSH_CRYPT_INVALID_KEY: return "invalid-key";
    }
    return "unknown";
}

/* Shared core: on refusal `*fail_msg` names the reference message and
 * `*fail_cause` its class; on success both are untouched. */
static cf_err push_encrypt_core(const unsigned char *message, size_t message_len,
                                 const char *p256dh_b64, const char *auth_b64,
                                 const unsigned char server_priv[32],
                                 const unsigned char salt[16], int64_t record_size,
                                 const unsigned char *padding, size_t padding_len,
                                 unsigned char **out, size_t *out_len,
                                 const char **fail_msg,
                                 cf_push_crypt_error *fail_cause) {
    static const unsigned char info_prefix[] = "WebPush: info";
    static const unsigned char cek_info[] = "Content-Encoding: aes128gcm";
    static const unsigned char nonce_info[] = "Content-Encoding: nonce";
    cf_err err = CF_OK;
    cf_push_crypt_error cause = CF_PUSH_CRYPT_ARGUMENT;
    const char *msg = "message cannot be blank";
    EC_GROUP *group = NULL;
    EC_POINT *peer = NULL;
    BIGNUM *server_bn = NULL;
    unsigned char *client_raw = NULL;
    size_t client_raw_len = 0;
    unsigned char *client_bytes = NULL;
    size_t client_len = 0;
    unsigned char *auth = NULL;
    size_t auth_len = 0;
    unsigned char shared[32];
    unsigned char prk[32];
    unsigned char cek[16];
    unsigned char nonce[12];
    unsigned char server_pub[65];
    unsigned char *info = NULL;
    unsigned char *plain = NULL;
    unsigned char *cipher = NULL;
    size_t cipher_len = 0;
    unsigned char *record = NULL;
#define PUSH_REFUSE(text, class) \
    do {                         \
        msg = (text);            \
        cause = (class);         \
        err = CF_INVALID;        \
        goto fail;               \
    } while (0)
    if (out == NULL || out_len == NULL) return CF_INVALID;
    *out = NULL;
    *out_len = 0;
    if ((message_len != 0 && message == NULL) || server_priv == NULL ||
        salt == NULL || (padding_len != 0 && padding == NULL)) {
        return CF_INVALID;
    }
    if (message_len == 0) PUSH_REFUSE("message cannot be blank", CF_PUSH_CRYPT_ARGUMENT);
    if (push_blank(p256dh_b64)) PUSH_REFUSE("p256dh cannot be blank", CF_PUSH_CRYPT_ARGUMENT);
    if (push_blank(auth_b64)) PUSH_REFUSE("auth cannot be blank", CF_PUSH_CRYPT_ARGUMENT);
    if (message_len > SIZE_MAX - padding_len - 16) {
        msg = "encrypted payload is too big";
        err = CF_LIMIT;
        goto fail;
    }
    group = push_group();
    if (group != NULL) peer = EC_POINT_new(group);
    if (group == NULL || peer == NULL) {
        msg = "out of memory";
        err = CF_INTERNAL;
        goto fail;
    }
    if (cf_push_b64u_decode(p256dh_b64, &client_raw, &client_raw_len) != CF_OK ||
        cf_push_b64u_decode(auth_b64, &auth, &auth_len) != CF_OK) {
        PUSH_REFUSE("invalid base64", CF_PUSH_CRYPT_ARGUMENT);
    }
    if (push_strip_zeros(client_raw, client_raw_len, &client_bytes,
                         &client_len) != CF_OK) {
        msg = "out of memory";
        err = CF_NOMEM;
        goto fail;
    }
    if (!push_scalar_ok(group, server_priv, &server_bn)) {
        PUSH_REFUSE("invalid server key", CF_PUSH_CRYPT_ARGUMENT);
    }
    if (!push_parse_point(group, client_bytes, client_len, peer)) {
        PUSH_REFUSE("invalid encoding", CF_PUSH_CRYPT_INVALID_KEY);
    }
    {
        EC_POINT *server_point = EC_POINT_new(group);
        bool ok = server_point != NULL &&
            push_derive_pub(group, server_bn, server_point) &&
            push_point_oct(group, server_point, server_pub);
        EC_POINT_free(server_point);
        if (!ok || !push_ecdh_x(group, server_bn, peer, shared)) {
            msg = "key agreement failed";
            err = CF_INTERNAL;
            goto fail;
        }
    }
    {
        size_t info_len = sizeof(info_prefix) + client_len + sizeof server_pub;
        info = malloc(info_len);
        if (info == NULL) {
            msg = "out of memory";
            err = CF_NOMEM;
            goto fail;
        }
        memcpy(info, info_prefix, sizeof(info_prefix) - 1);
        info[sizeof(info_prefix) - 1] = 0;
        memcpy(info + sizeof info_prefix, client_bytes, client_len);
        memcpy(info + sizeof info_prefix + client_len, server_pub,
               sizeof server_pub);
        if (push_hkdf(auth, auth_len, shared, sizeof shared, info, info_len,
                       prk, sizeof prk) != CF_OK ||
            push_hkdf(salt, 16, prk, sizeof prk, cek_info,
                       sizeof(cek_info), cek, sizeof cek) != CF_OK ||
            push_hkdf(salt, 16, prk, sizeof prk, nonce_info,
                       sizeof(nonce_info), nonce, sizeof nonce) != CF_OK) {
            msg = "key derivation failed";
            err = CF_INTERNAL;
            goto fail;
        }
    }
    plain = malloc(message_len + padding_len);
    if (plain == NULL) {
        msg = "out of memory";
        err = CF_NOMEM;
        goto fail;
    }
    if (message_len != 0) memcpy(plain, message, message_len);
    if (padding_len != 0) memcpy(plain + message_len, padding, padding_len);
    if (push_aes128gcm_encrypt(cek, nonce, plain, message_len + padding_len,
                               &cipher, &cipher_len) != CF_OK) {
        msg = "encryption failed";
        err = CF_INTERNAL;
        goto fail;
    }
    if (cipher_len > CF_PUSH_MAX_RECORD_BYTES) {
        PUSH_REFUSE("encrypted payload is too big", CF_PUSH_CRYPT_ARGUMENT);
    }
    {
        uint32_t size_field =
            record_size >= 0 ? (uint32_t)record_size : (uint32_t)cipher_len;
        size_t total = 16 + 4 + 1 + sizeof server_pub + cipher_len;
        record = malloc(total);
        if (record == NULL) {
            msg = "out of memory";
            err = CF_NOMEM;
            goto fail;
        }
        memcpy(record, salt, 16);
        record[16] = (unsigned char)((size_field >> 24) & 0xff);
        record[17] = (unsigned char)((size_field >> 16) & 0xff);
        record[18] = (unsigned char)((size_field >> 8) & 0xff);
        record[19] = (unsigned char)(size_field & 0xff);
        record[20] = (unsigned char)sizeof server_pub;
        memcpy(record + 21, server_pub, sizeof server_pub);
        memcpy(record + 21 + sizeof server_pub, cipher, cipher_len);
        *out = record;
        *out_len = total;
        record = NULL;
    }
fail:
    if (err != CF_OK) {
        if (fail_cause != NULL) *fail_cause = cause;
        if (fail_msg != NULL) *fail_msg = msg;
    }
    EC_GROUP_free(group);
    EC_POINT_free(peer);
    BN_free(server_bn);
    free(client_raw);
    free(client_bytes);
    free(auth);
    free(info);
    free(plain);
    free(cipher);
    free(record);
    OPENSSL_cleanse(shared, sizeof shared);
    OPENSSL_cleanse(prk, sizeof prk);
    OPENSSL_cleanse(cek, sizeof cek);
    OPENSSL_cleanse(nonce, sizeof nonce);
    return err;
}

cf_err cf_push_encrypt_with(const unsigned char *message, size_t message_len,
                             const char *p256dh_b64, const char *auth_b64,
                             const unsigned char server_priv[32],
                             const unsigned char salt[16], int64_t record_size,
                             const unsigned char *padding, size_t padding_len,
                             unsigned char **out, size_t *out_len,
                             cf_push_crypt_error *detail) {
    const char *msg = NULL;
    cf_push_crypt_error cause = CF_PUSH_CRYPT_OK;
    cf_err err;
    if (detail != NULL) *detail = CF_PUSH_CRYPT_OK;
    err = push_encrypt_core(message, message_len, p256dh_b64, auth_b64,
                            server_priv, salt, record_size, padding,
                            padding_len, out, out_len, &msg, &cause);
    if (err != CF_OK && detail != NULL) *detail = cause;
    return err;
}

cf_err cf_push_encrypt(const unsigned char *message, size_t message_len,
                        const char *p256dh_b64, const char *auth_b64,
                        unsigned char **out, size_t *out_len,
                        cf_push_crypt_error *detail) {
    static const unsigned char gem_padding[] = {2, 0};
    unsigned char salt[16];
    unsigned char server_priv[32];
    cf_err err;
    EC_GROUP *group = NULL;
    BIGNUM *bn = NULL;
    if (out == NULL || out_len == NULL) return CF_INVALID;
    *out = NULL;
    *out_len = 0;
    if (detail != NULL) *detail = CF_PUSH_CRYPT_OK;
    if (message_len != 0 && message == NULL) return CF_INVALID;
    if (message_len == 0 || push_blank(p256dh_b64) || push_blank(auth_b64)) {
        if (detail != NULL) *detail = CF_PUSH_CRYPT_ARGUMENT;
        return CF_INVALID;
    }
    if (cf_random_bytes(salt, sizeof salt) != CF_OK) return CF_IO;
    group = push_group();
    if (group == NULL) {
        err = CF_INTERNAL;
        goto done;
    }
    /* Fresh P-256 server key from OS entropy (bounded retries). */
    err = CF_INTERNAL;
    for (int attempt = 0; attempt < 128; attempt++) {
        if (cf_random_bytes(server_priv, sizeof server_priv) != CF_OK) {
            err = CF_IO;
            goto done;
        }
        if (push_scalar_ok(group, server_priv, &bn)) {
            err = CF_OK;
            break;
        }
    }
    if (err != CF_OK) goto done;
    err = cf_push_encrypt_with(message, message_len, p256dh_b64, auth_b64,
                               server_priv, salt, -1, gem_padding,
                               sizeof gem_padding, out, out_len, detail);
done:
    EC_GROUP_free(group);
    BN_free(bn);
    OPENSSL_cleanse(server_priv, sizeof server_priv);
    return err;
}

/* --- VAPID ------------------------------------------------------------------ */

const char *cf_push_vapid_error_name(cf_push_vapid_error error) {
    switch (error) {
    case CF_PUSH_VAPID_OK: return "ok";
    case CF_PUSH_VAPID_MISSING: return "missing";
    case CF_PUSH_VAPID_INVALID_PUBLIC_KEY: return "invalid-public-key";
    case CF_PUSH_VAPID_INVALID_PRIVATE_KEY: return "invalid-private-key";
    case CF_PUSH_VAPID_MISMATCHED: return "mismatched";
    }
    return "unknown";
}

void cf_push_vapid_dispose(cf_push_vapid *vapid) {
    if (vapid == NULL) return;
    free(vapid->subject);
    OPENSSL_cleanse(vapid->priv, sizeof vapid->priv);
    memset(vapid, 0, sizeof *vapid);
}

cf_err cf_push_vapid_init(const char *subject, const char *public_b64,
                           const char *private_b64, cf_push_vapid *out,
                           cf_push_vapid_error *detail) {
    cf_push_vapid_error cause = CF_PUSH_VAPID_INVALID_PUBLIC_KEY;
    cf_err err = CF_OK;
    EC_GROUP *group = NULL;
    EC_POINT *given = NULL;
    EC_POINT *derived = NULL;
    BIGNUM *priv_bn = NULL;
    unsigned char *pub = NULL;
    size_t pub_len = 0;
    unsigned char *priv = NULL;
    size_t priv_len = 0;
    unsigned char scalar[32];
    unsigned char canonical[65];
    unsigned char check[65];
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (detail != NULL) *detail = CF_PUSH_VAPID_OK;
    if (push_blank(public_b64)) goto invalid_public;
    if (cf_push_b64u_decode(public_b64, &pub, &pub_len) != CF_OK) goto invalid_public;
    group = push_group();
    if (group == NULL) {
        err = CF_INTERNAL;
        goto fail;
    }
    given = EC_POINT_new(group);
    derived = EC_POINT_new(group);
    if (given == NULL || derived == NULL) {
        err = CF_INTERNAL;
        goto fail;
    }
    if (!push_parse_point(group, pub, pub_len, given) ||
        !push_point_oct(group, given, canonical)) {
        goto invalid_public;
    }
    cause = CF_PUSH_VAPID_INVALID_PRIVATE_KEY;
    if (push_blank(private_b64)) goto fail_with_cause;
    if (cf_push_b64u_decode(private_b64, &priv, &priv_len) != CF_OK) goto fail_with_cause;
    if (priv_len == 0 || priv_len > 32) goto fail_with_cause;
    memset(scalar, 0, sizeof scalar);
    memcpy(scalar + 32 - priv_len, priv, priv_len);
    if (!push_scalar_ok(group, scalar, &priv_bn)) goto fail_with_cause;
    cause = CF_PUSH_VAPID_MISMATCHED;
    if (!push_derive_pub(group, priv_bn, derived) ||
        !push_point_oct(group, derived, check) ||
        memcmp(check, canonical, sizeof canonical) != 0) {
        goto fail_with_cause;
    }
    out->subject = push_dup(subject != NULL ? subject : "");
    if (out->subject == NULL) {
        err = CF_NOMEM;
        goto fail;
    }
    if (pub_len > sizeof out->pub) {
        err = CF_INTERNAL; /* canonical 65 bytes always fit */
        goto fail;
    }
    memcpy(out->pub, pub, pub_len);
    out->pub_len = pub_len;
    memcpy(out->priv, scalar, sizeof out->priv);
    err = CF_OK;
    goto done;
invalid_public:
    cause = CF_PUSH_VAPID_INVALID_PUBLIC_KEY;
    err = CF_INVALID;
    goto fail;
fail_with_cause:
    err = CF_INVALID;
    goto fail;
fail:
    if (err != CF_OK) {
        /* Keep `*out` empty on every failure. */
        if (detail != NULL) *detail = cause;
        free(out->subject);
        memset(out, 0, sizeof *out);
    }
done:
    EC_GROUP_free(group);
    EC_POINT_free(given);
    EC_POINT_free(derived);
    BN_free(priv_bn);
    free(pub);
    free(priv);
    OPENSSL_cleanse(scalar, sizeof scalar);
    return err;
}

cf_err cf_push_vapid_from_config(const cf_config *config, cf_push_vapid *out,
                                  cf_push_vapid_error *detail) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (detail != NULL) *detail = CF_PUSH_VAPID_OK;
    if (config == NULL) return CF_INVALID;
    if (config->vapid_public_key == NULL || config->vapid_private_key == NULL ||
        config->vapid_subject == NULL || config->vapid_public_key[0] == '\0' ||
        config->vapid_private_key[0] == '\0' || config->vapid_subject[0] == '\0') {
        if (detail != NULL) *detail = CF_PUSH_VAPID_MISSING;
        return CF_INVALID;
    }
    return cf_push_vapid_init(config->vapid_subject, config->vapid_public_key,
                              config->vapid_private_key, out, detail);
}

cf_err cf_push_vapid_authorization(const cf_push_vapid *vapid,
                                    const char *audience, int64_t now_unix,
                                    char **out) {
    static const char header_json[] = "{\"typ\":\"JWT\",\"alg\":\"ES256\"}";
    cf_err err = CF_OK;
    EVP_PKEY *key = NULL;
    EVP_MD_CTX *mctx = NULL;
    ECDSA_SIG *sig = NULL;
    char *header_seg = NULL;
    char *claims_seg = NULL;
    char *key_seg = NULL;
    char *input = NULL;
    char *sig_seg = NULL;
    unsigned char raw[64];
    unsigned char *der = NULL;
    size_t der_len = 0;
    char exp_text[32];
    char *claims = NULL;
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (vapid == NULL || audience == NULL || vapid->pub_len == 0) return CF_INVALID;
    if (now_unix > INT64_MAX - CF_PUSH_VAPID_EXPIRATION_S ||
        now_unix < INT64_MIN + CF_PUSH_VAPID_EXPIRATION_S) {
        return CF_LIMIT;
    }
    {
        int written = snprintf(exp_text, sizeof exp_text, "%" PRId64,
                               now_unix + CF_PUSH_VAPID_EXPIRATION_S);
        if (written < 0 || (size_t)written >= sizeof exp_text) return CF_INTERNAL;
    }
    {
        size_t aud_len = strlen(audience);
        size_t sub_len = vapid->subject != NULL ? strlen(vapid->subject) : 0;
        size_t need = strlen("{\"aud\":,\"exp\":,\"sub\":}") +
            push_json_escaped_len(audience, aud_len) + 2 + strlen(exp_text) +
            push_json_escaped_len(vapid->subject != NULL ? vapid->subject : "",
                                  sub_len) + 2 + 1;
        claims = malloc(need);
        if (claims == NULL) return push_nomem();
        char *p = claims;
        char *end = claims + need;
        static const char c0[] = "{\"aud\":";
        static const char c1[] = ",\"exp\":";
        static const char c2[] = ",\"sub\":";
        memcpy(p, c0, sizeof(c0) - 1);
        p += sizeof(c0) - 1;
        err = push_json_string(audience, &p, end);
        if (err == CF_OK) {
            memcpy(p, c1, sizeof(c1) - 1);
            p += sizeof(c1) - 1;
            size_t explen = strlen(exp_text);
            memcpy(p, exp_text, explen);
            p += explen;
            memcpy(p, c2, sizeof(c2) - 1);
            p += sizeof(c2) - 1;
            err = push_json_string(vapid->subject != NULL ? vapid->subject : "",
                                   &p, end);
        }
        if (err == CF_OK) {
            *p++ = '}';
            *p = '\0';
        }
        if (err != CF_OK) {
            free(claims);
            return err;
        }
    }
    if (cf_push_b64u_encode((const unsigned char *)header_json,
                            strlen(header_json), &header_seg) != CF_OK ||
        cf_push_b64u_encode((const unsigned char *)claims, strlen(claims),
                            &claims_seg) != CF_OK ||
        cf_push_b64u_encode(vapid->pub, vapid->pub_len, &key_seg) != CF_OK) {
        err = CF_NOMEM;
        goto done;
    }
    {
        size_t need = strlen(header_seg) + 1 + strlen(claims_seg) + 1;
        input = malloc(need);
        if (input == NULL) {
            err = CF_NOMEM;
            goto done;
        }
        snprintf(input, need, "%s.%s", header_seg, claims_seg);
    }
    key = push_signing_key(vapid->priv);
    mctx = EVP_MD_CTX_new();
    if (key == NULL || mctx == NULL) {
        err = CF_INTERNAL;
        goto done;
    }
    if (EVP_DigestSignInit(mctx, NULL, EVP_sha256(), NULL, key) != 1 ||
        EVP_DigestSign(mctx, NULL, &der_len, (const unsigned char *)input,
                       strlen(input)) != 1) {
        err = CF_INTERNAL;
        goto done;
    }
    der = malloc(der_len != 0 ? der_len : 1);
    if (der == NULL) {
        err = CF_NOMEM;
        goto done;
    }
    if (EVP_DigestSign(mctx, der, &der_len, (const unsigned char *)input,
                       strlen(input)) != 1) {
        err = CF_INTERNAL;
        goto done;
    }
    {
        const unsigned char *cursor = der;
        sig = d2i_ECDSA_SIG(NULL, &cursor, (long)der_len);
        if (sig == NULL) {
            err = CF_INTERNAL;
            goto done;
        }
        /* Borrowed; freed with `sig`. */
        const BIGNUM *br = NULL;
        const BIGNUM *bs = NULL;
        ECDSA_SIG_get0(sig, &br, &bs);
        if (br == NULL || bs == NULL || BN_num_bytes(br) > 32 ||
            BN_num_bytes(bs) > 32 || BN_is_negative(br) || BN_is_negative(bs)) {
            err = CF_INTERNAL;
            goto done;
        }
        memset(raw, 0, sizeof raw);
        if (BN_bn2binpad(br, raw, 32) != 32 ||
            BN_bn2binpad(bs, raw + 32, 32) != 32) {
            err = CF_INTERNAL;
            goto done;
        }
    }
    if (cf_push_b64u_encode(raw, sizeof raw, &sig_seg) != CF_OK) {
        err = CF_NOMEM;
        goto done;
    }
    {
        size_t need = strlen("vapid t=.") + strlen(input) + 1 + strlen(sig_seg) +
            strlen(",k=") + strlen(key_seg) + 1;
        char *value = malloc(need);
        if (value == NULL) {
            err = CF_NOMEM;
            goto done;
        }
        snprintf(value, need, "vapid t=%s.%s,k=%s", input, sig_seg, key_seg);
        *out = value;
    }
done:
    free(claims);
    free(header_seg);
    free(claims_seg);
    free(key_seg);
    free(input);
    free(sig_seg);
    free(der);
    EVP_PKEY_free(key);
    EVP_MD_CTX_free(mctx);
    ECDSA_SIG_free(sig);
    OPENSSL_cleanse(raw, sizeof raw);
    return err;
}

/* --- headers --------------------------------------------------------------- */

void cf_push_headers_dispose(cf_push_headers *headers) {
    size_t i;
    if (headers == NULL) return;
    for (i = 0; i < headers->len; i++) {
        free(headers->items[i].name);
        free(headers->items[i].value);
    }
    free(headers->items);
    memset(headers, 0, sizeof *headers);
}

static cf_err push_header_add(cf_push_headers *headers, const char *name,
                               const char *value) {
    cf_push_header *grown = realloc(headers->items,
                                    (headers->len + 1) * sizeof *grown);
    if (grown == NULL) return push_nomem();
    headers->items = grown;
    headers->items[headers->len].name = push_dup(name);
    headers->items[headers->len].value = push_dup(value);
    if (headers->items[headers->len].name == NULL ||
        headers->items[headers->len].value == NULL) {
        free(headers->items[headers->len].name);
        free(headers->items[headers->len].value);
        return push_nomem();
    }
    headers->len++;
    return CF_OK;
}

cf_err cf_push_build_headers(const cf_push_vapid *vapid, const char *audience,
                              int64_t now_unix, size_t payload_len,
                              cf_push_headers *out) {
    cf_err err;
    char length_text[32];
    char *authorization = NULL;
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (vapid == NULL || audience == NULL) return CF_INVALID;
    {
        int written = snprintf(length_text, sizeof length_text, "%zu", payload_len);
        if (written < 0 || (size_t)written >= sizeof length_text) return CF_INTERNAL;
    }
    err = cf_push_vapid_authorization(vapid, audience, now_unix, &authorization);
    if (err != CF_OK) return err;
    err = push_header_add(out, "Content-Type", "application/octet-stream");
    if (err == CF_OK) {
        char ttl_text[32];
        snprintf(ttl_text, sizeof ttl_text, "%u", CF_PUSH_TTL_SECONDS);
        err = push_header_add(out, "Ttl", ttl_text);
    }
    if (err == CF_OK) err = push_header_add(out, "Urgency", CF_PUSH_URGENCY);
    if (err == CF_OK) {
        err = push_header_add(out, "Content-Encoding", CF_PUSH_CONTENT_ENCODING);
    }
    if (err == CF_OK) err = push_header_add(out, "Content-Length", length_text);
    if (err == CF_OK) err = push_header_add(out, "Authorization", authorization);
    free(authorization);
    if (err != CF_OK) cf_push_headers_dispose(out);
    return err;
}

/* --- endpoint URI ------------------------------------------------------------
 * The parts of the endpoint `payload_send` looks at: scheme, host, port and
 * the origin-form target. Mirrors the reference URI parse closely enough
 * for already-validated https endpoints; anything else is an Argument.
 */

typedef struct {
    char *scheme; /* lowercased */
    char *host; /* as-is, without port or brackets stripped */
    uint16_t port;
    char *target; /* origin-form path[?query] */
} push_uri;

static void push_uri_dispose(push_uri *uri) {
    if (uri == NULL) return;
    free(uri->scheme);
    free(uri->host);
    free(uri->target);
    memset(uri, 0, sizeof *uri);
}

/* Rust `port.parse::<u16>()`: optional '+', digits, no overflow. */
static bool push_parse_port(const char *s, size_t len, uint16_t *out) {
    size_t i = 0;
    uint32_t value = 0;
    if (len == 0) return false;
    if (s[0] == '+') i = 1;
    if (i >= len) return false;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        value = value * 10 + (uint32_t)(s[i] - '0');
        if (value > 65535) return false;
    }
    *out = (uint16_t)value;
    return true;
}

static bool push_has_whitespace(const char *s, size_t len) {
    /* Reference rejects any `char::is_whitespace`; ASCII covers the
     * endpoints that reach delivery, and multibyte whitespace stays legal
     * here but fails the permitted-host gate upstream. */
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
            c == '\v') {
            return true;
        }
    }
    return false;
}

static cf_err push_uri_parse(const char *endpoint, push_uri *out) {
    size_t len;
    const char *sep;
    const char *rest;
    size_t rest_len;
    const char *authority_end;
    size_t authority_len;
    const char *authority;
    memset(out, 0, sizeof *out);
    if (endpoint == NULL) return CF_INVALID;
    len = strlen(endpoint);
    if (len == 0 || push_has_whitespace(endpoint, len)) return CF_INVALID;
    sep = strstr(endpoint, "://");
    if (sep == NULL || sep == endpoint) return CF_INVALID;
    rest = sep + 3;
    rest_len = len - (size_t)(rest - endpoint);
    if (rest_len == 0) return CF_INVALID;
    authority_end = rest + rest_len;
    for (const char *p = rest; p < rest + rest_len; p++) {
        if (*p == '/' || *p == '?' || *p == '#') {
            authority_end = p;
            break;
        }
    }
    authority_len = (size_t)(authority_end - rest);
    authority = rest;
    {
        const char *at = NULL;
        for (size_t i = 0; i < authority_len; i++) {
            if (authority[i] == '@') at = authority + i;
        }
        if (at != NULL) {
            authority = at + 1;
            authority_len = (size_t)(authority_end - authority);
        }
    }
    out->scheme = push_dup_n(endpoint, (size_t)(sep - endpoint));
    if (out->scheme == NULL) {
        push_uri_dispose(out);
        return push_nomem();
    }
    for (char *p = out->scheme; *p != '\0'; p++) {
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + ('a' - 'A'));
    }
    if (authority_len != 0 && authority[authority_len - 1] == ']') {
        out->host = push_dup_n(authority, authority_len);
        if (out->host == NULL) {
            push_uri_dispose(out);
            return push_nomem();
        }
        out->port = 0; /* literal: no port on record */
    } else {
        const char *colon = NULL;
        for (size_t i = 0; i < authority_len; i++) {
            if (authority[i] == ':') colon = authority + i;
        }
        if (colon != NULL) {
            size_t host_len = (size_t)(colon - authority);
            size_t port_len = authority_len - host_len - 1;
            uint16_t port = 0;
            if (!push_parse_port(colon + 1, port_len, &port)) {
                push_uri_dispose(out);
                return CF_INVALID;
            }
            out->host = push_dup_n(authority, host_len);
            if (out->host == NULL) {
                push_uri_dispose(out);
                return push_nomem();
            }
            out->port = port;
        } else {
            out->host = push_dup_n(authority, authority_len);
            if (out->host == NULL) {
                push_uri_dispose(out);
                return push_nomem();
            }
            if (strcmp(out->scheme, "https") == 0) out->port = 443;
            else if (strcmp(out->scheme, "http") == 0) out->port = 80;
            else out->port = 0;
        }
    }
    if (out->host[0] == '\0') {
        push_uri_dispose(out);
        return CF_INVALID;
    }
    {
        const char *path = authority_end;
        const char *end = endpoint + len;
        const char *query_end = end;
        for (const char *p = path; p < end; p++) {
            if (*p == '#') {
                query_end = p;
                break;
            }
        }
        if (path == query_end || *path != '/') {
            /* No path: origin-form "/" plus any query. */
            if (*path == '?' && query_end > path + 1) {
                size_t qlen = (size_t)(query_end - path);
                out->target = malloc(1 + qlen + 1);
                if (out->target == NULL) {
                    push_uri_dispose(out);
                    return push_nomem();
                }
                out->target[0] = '/';
                memcpy(out->target + 1, path, qlen);
                out->target[1 + qlen] = '\0';
            } else {
                out->target = push_dup("/");
                if (out->target == NULL) {
                    push_uri_dispose(out);
                    return push_nomem();
                }
            }
        } else {
            out->target = push_dup_n(path, (size_t)(query_end - path));
            if (out->target == NULL) {
                push_uri_dispose(out);
                return push_nomem();
            }
        }
    }
    return CF_OK;
}

/* Rust `{:?}` quoting for the bad-URI detail (prefix asserted by tests). */
static char *push_debug_quoted(const char *s) {
    size_t extra = 0;
    char *quoted;
    char *p;
    if (s == NULL) s = "";
    for (const unsigned char *q = (const unsigned char *)s; *q != '\0'; q++) {
        if (*q == '"' || *q == '\\' || *q == '\n' || *q == '\r' || *q == '\t' ||
            *q == '\0' || *q < 0x20 || *q == 0x7f) {
            extra += (*q < 0x20 && *q != '\n' && *q != '\r' && *q != '\t' &&
                      *q != '\0' && *q != '"' && *q != '\\')
                ? 5
                : 1;
        }
    }
    quoted = malloc(strlen(s) + extra + 3);
    if (quoted == NULL) return NULL;
    p = quoted;
    *p++ = '"';
    for (const unsigned char *q = (const unsigned char *)s; *q != '\0'; q++) {
        switch (*q) {
        case '"': *p++ = '\\'; *p++ = '"'; break;
        case '\\': *p++ = '\\'; *p++ = '\\'; break;
        case '\n': *p++ = '\\'; *p++ = 'n'; break;
        case '\r': *p++ = '\\'; *p++ = 'r'; break;
        case '\t': *p++ = '\\'; *p++ = 't'; break;
        case '\0': *p++ = '\\'; *p++ = '0'; break;
        default:
            if (*q < 0x20 || *q == 0x7f) {
                static const char hex[] = "0123456789abcdef";
                unsigned v = *q;
                char digits[8];
                size_t nd = 0;
                *p++ = '\\'; *p++ = 'u'; *p++ = '{';
                do {
                    digits[nd++] = hex[v % 16];
                    v /= 16;
                } while (v != 0);
                while (nd != 0) *p++ = digits[--nd];
                *p++ = '}';
            } else {
                *p++ = (char)*q;
            }
            break;
        }
    }
    *p++ = '"';
    *p = '\0';
    return quoted;
}

/* --- request --------------------------------------------------------------- */

void cf_push_request_dispose(cf_push_request *request) {
    if (request == NULL) return;
    free(request->resolved_ip);
    free(request->host);
    free(request->target);
    cf_push_headers_dispose(&request->headers);
    free(request->body);
    memset(request, 0, sizeof *request);
}

cf_err cf_push_build_request(const char *endpoint,
                              const char *p256dh_b64, const char *auth_b64,
                              const char *resolved_ip, const char *title,
                              const char *body, const char *path, int64_t badge,
                              const cf_push_vapid *vapid, int64_t now_unix,
                              cf_push_request *out,
                              cf_push_crypt_error *crypt_detail,
                              char **error_text) {
    cf_err err = CF_OK;
    push_uri uri;
    char *message = NULL;
    char *audience = NULL;
    unsigned char *payload = NULL;
    size_t payload_len = 0;
    cf_push_crypt_error crypt_cause = CF_PUSH_CRYPT_ARGUMENT;
    memset(&uri, 0, sizeof uri);
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (crypt_detail != NULL) *crypt_detail = CF_PUSH_CRYPT_OK;
    if (error_text != NULL) *error_text = NULL;
    if (endpoint == NULL || resolved_ip == NULL || title == NULL ||
        body == NULL || path == NULL || vapid == NULL) {
        return CF_INVALID;
    }
    if (push_uri_parse(endpoint, &uri) != CF_OK || uri.port == 0) {
        char *quoted = push_debug_quoted(endpoint);
        const char *prefix = "bad URI(is not URI?): ";
        size_t need = strlen(prefix) + (quoted != NULL ? strlen(quoted) : 2) + 1;
        char *text = malloc(need);
        if (text != NULL) {
            snprintf(text, need, "%s%s", prefix, quoted != NULL ? quoted : "\"\"");
            if (error_text != NULL) {
                *error_text = text;
                text = NULL;
            }
        }
        free(text);
        free(quoted);
        push_uri_dispose(&uri);
        return CF_INVALID;
    }
    err = cf_push_notification_json(title, body, path, badge, &message);
    if (err != CF_OK) {
        push_uri_dispose(&uri);
        return err;
    }
    {
        static const unsigned char gem_padding[] = {2, 0};
        unsigned char salt[16];
        unsigned char server_priv[32];
        const char *fail_msg = "encryption failed";
        EC_GROUP *key_group = push_group();
        BIGNUM *key_bn = NULL;
        if (key_group == NULL) {
            err = CF_INTERNAL;
        } else if (cf_random_bytes(salt, sizeof salt) != CF_OK) {
            err = CF_IO;
        } else {
            err = CF_INTERNAL;
            for (int attempt = 0; attempt < 128; attempt++) {
                if (cf_random_bytes(server_priv, sizeof server_priv) != CF_OK) {
                    err = CF_IO;
                    break;
                }
                if (push_scalar_ok(key_group, server_priv, &key_bn)) {
                    err = CF_OK;
                    break;
                }
            }
        }
        if (err == CF_OK) {
            err = push_encrypt_core((const unsigned char *)message,
                                    strlen(message), p256dh_b64, auth_b64,
                                    server_priv, salt, -1, gem_padding,
                                    sizeof gem_padding, &payload, &payload_len,
                                    &fail_msg, &crypt_cause);
        }
        EC_GROUP_free(key_group);
        BN_free(key_bn);
        OPENSSL_cleanse(server_priv, sizeof server_priv);
        if (err != CF_OK) {
            if (crypt_detail != NULL) *crypt_detail = crypt_cause;
            if (error_text != NULL) *error_text = push_dup(fail_msg);
            free(message);
            push_uri_dispose(&uri);
            return err;
        }
    }
    err = cf_push_audience(uri.scheme, uri.host, &audience);
    if (err == CF_OK) {
        err = cf_push_build_headers(vapid, audience, now_unix, payload_len,
                                    &out->headers);
    }
    if (err == CF_OK) {
        out->endpoint_url = endpoint;
        out->resolved_ip = push_dup(resolved_ip);
        out->host = uri.host;
        uri.host = NULL;
        out->port = uri.port;
        out->target = uri.target;
        uri.target = NULL;
        out->body = payload;
        out->body_len = payload_len;
        payload = NULL;
    }
    free(message);
    free(audience);
    free(payload);
    push_uri_dispose(&uri);
    if (err != CF_OK) cf_push_request_dispose(out);
    return err;
}

void cf_push_sort_by_id(cf_push_subscription_vector *subscriptions) {
    size_t i;
    size_t j;
    if (subscriptions == NULL || subscriptions->len < 2) return;
    for (i = 1; i < subscriptions->len; i++) {
        cf_push_subscription key = subscriptions->items[i];
        j = i;
        while (j > 0 && subscriptions->items[j - 1].id > key.id) {
            subscriptions->items[j] = subscriptions->items[j - 1];
            j--;
        }
        subscriptions->items[j] = key;
    }
}

/* --- delivery outcome ---------------------------------------------------------- */

void cf_push_delivery_dispose(cf_push_delivery *delivery) {
    if (delivery == NULL) return;
    free(delivery->class_name);
    free(delivery->host);
    free(delivery->detail);
    memset(delivery, 0, sizeof *delivery);
}

bool cf_push_delivery_invalidates(const cf_push_delivery *delivery) {
    if (delivery == NULL) return false;
    return delivery->invalidate;
}

void cf_push_format_log(const cf_push_delivery *delivery, char *buf,
                         size_t cap) {
    const char *line = "(no delivery)";
    if (buf == NULL || cap == 0) return;
    if (delivery != NULL && delivery->detail != NULL) line = delivery->detail;
    snprintf(buf, cap, "%s", line);
    buf[cap - 1] = '\0';
}

static cf_err push_delivery_set(cf_push_delivery *out,
                                 cf_push_delivery_kind kind,
                                 const char *class_name, const char *host,
                                 unsigned status, const char *detail,
                                 bool delivered, bool skipped, bool invalidate) {
    memset(out, 0, sizeof *out);
    out->kind = kind;
    out->status = status;
    out->delivered = delivered;
    out->skipped = skipped;
    out->invalidate = invalidate;
    out->class_name = push_dup(class_name != NULL ? class_name : "");
    out->host = push_dup(host != NULL ? host : "");
    out->detail = push_dup(detail != NULL ? detail : "");
    if (out->class_name == NULL || out->host == NULL || out->detail == NULL) {
        cf_push_delivery_dispose(out);
        return push_nomem();
    }
    return CF_OK;
}

cf_err cf_push_classify_response(unsigned status, const char *reason,
                                  const char *host, cf_push_delivery *out) {
    const char *kind = "WebPush::ResponseError";
    cf_push_delivery_kind code = CF_PUSH_DELIVERY_RESPONSE;
    bool gone = false;
    char detail[256];
    if (out == NULL) return CF_INVALID;
    if (reason == NULL) reason = "";
    if (host == NULL) host = "";
    switch (status) {
    case 410:
        kind = "WebPush::ExpiredSubscription";
        code = CF_PUSH_DELIVERY_GONE;
        gone = true;
        break;
    case 404:
        kind = "WebPush::InvalidSubscription";
        code = CF_PUSH_DELIVERY_GONE;
        gone = true;
        break;
    case 401:
    case 403:
        kind = "WebPush::Unauthorized";
        break;
    case 400:
        kind = strcmp(reason, "UnauthorizedRegistration") == 0
            ? "WebPush::Unauthorized"
            : "WebPush::ResponseError";
        break;
    case 413: kind = "WebPush::PayloadTooLarge"; break;
    case 429: kind = "WebPush::TooManyRequests"; break;
    default:
        if (status >= 500 && status <= 599) kind = "WebPush::PushServiceError";
        else if (status >= 200 && status <= 299) kind = "";
        break;
    }
    if (kind[0] == '\0') {
        snprintf(detail, sizeof detail, "delivered: host: %s, status: %u", host,
                 status);
        return push_delivery_set(out, CF_PUSH_DELIVERY_OK, "Ok", host, status,
                                 detail, true, false, false);
    }
    snprintf(detail, sizeof detail, "%s: host: %s, status: %u", kind, host,
             status);
    return push_delivery_set(out, code, kind, host, status, detail, false,
                             false, gone);
}

static cf_err push_transport_outcome(cf_push_transport_error transport,
                                      const char *host, cf_push_delivery *out) {
    const char *class_name = "SystemCallError";
    cf_push_delivery_kind kind = CF_PUSH_DELIVERY_HTTP;
    char detail[256];
    if (transport == CF_PUSH_TRANSPORT_TLS) {
        class_name = "OpenSSL::SSL::SSLError";
        kind = CF_PUSH_DELIVERY_TLS;
    } else if (transport == CF_PUSH_TRANSPORT_OPEN_TIMEOUT) {
        class_name = "Net::OpenTimeout";
    } else if (transport == CF_PUSH_TRANSPORT_READ_TIMEOUT) {
        class_name = "Net::ReadTimeout";
    }
    snprintf(detail, sizeof detail, "%s: host: %s", class_name,
             host != NULL ? host : "");
    return push_delivery_set(out, kind, class_name, host, 0, detail, false,
                             false, false);
}

cf_err cf_push_deliver(const cf_push_subscription *subscription,
                        const char *title, const char *body, const char *path,
                        int64_t badge, const cf_push_vapid *vapid,
                        cf_push_resolve_fn resolve, void *resolve_arg,
                        cf_push_exchange_fn exchange, void *exchange_ctx,
                        int64_t now_unix, cf_push_delivery *out) {
    cf_err err = CF_OK;
    cf_optional_str ip;
    cf_push_request request;
    cf_push_crypt_error crypt_cause = CF_PUSH_CRYPT_OK;
    char *error_text = NULL;
    const char *endpoint = "";
    memset(&ip, 0, sizeof ip);
    memset(&request, 0, sizeof request);
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (subscription == NULL || title == NULL || body == NULL || path == NULL ||
        exchange == NULL) {
        return CF_INVALID;
    }
    if (vapid == NULL) {
        return push_delivery_set(out, CF_PUSH_DELIVERY_ARGUMENT,
                                 "ArgumentError", "", 0,
                                 "VAPID_PUBLIC_KEY and VAPID_PRIVATE_KEY "
                                 "aren't set",
                                 false, false, false);
    }
    if (!cf_push_subscription_resolved_endpoint_ip(subscription, resolve,
                                                   resolve_arg, &ip)) {
        err = push_delivery_set(out, CF_PUSH_DELIVERY_OK, "Ok", "", 0,
                                "ok: skipped", false, true, false);
        return err;
    }
    endpoint = subscription->endpoint.present
        ? (subscription->endpoint.value.ptr != NULL
               ? subscription->endpoint.value.ptr
               : "")
        : "";
    err = cf_push_build_request(endpoint,
                                subscription->p256dh_key.present
                                    ? subscription->p256dh_key.value.ptr
                                    : NULL,
                                subscription->auth_key.present
                                    ? subscription->auth_key.value.ptr
                                    : NULL,
                                ip.value.ptr != NULL ? ip.value.ptr : "", title,
                                body, path, badge, vapid, now_unix, &request,
                                &crypt_cause, &error_text);
    if (err != CF_OK) {
        cf_push_delivery_kind kind = CF_PUSH_DELIVERY_ARGUMENT;
        const char *class_name = "ArgumentError";
        bool invalidate = false;
        char detail[256];
        if (crypt_cause == CF_PUSH_CRYPT_INVALID_KEY) {
            kind = CF_PUSH_DELIVERY_INVALID_KEY;
            class_name = "OpenSSL::PKey::EC::Point::Error";
            invalidate = true;
        }
        /* Sanitized: never carry the endpoint or key text into the log
         * line. URI failures keep only the fixed prefix. */
        if (crypt_cause == CF_PUSH_CRYPT_OK) {
            snprintf(detail, sizeof detail, "%s: bad endpoint", class_name);
        } else if (crypt_cause == CF_PUSH_CRYPT_INVALID_KEY) {
            snprintf(detail, sizeof detail, "%s: invalid encoding", class_name);
        } else {
            snprintf(detail, sizeof detail, "%s: invalid subscription keys",
                     class_name);
        }
        free(error_text);
        cf_optional_str_dispose(&ip);
        cf_push_request_dispose(&request);
        return push_delivery_set(out, kind, class_name, request.host != NULL ? request.host : "", 0,
                                 detail, false, false, invalidate);
    }
    cf_optional_str_dispose(&ip);
    {
        unsigned status = 0;
        char reason[128];
        cf_push_transport_error transport = CF_PUSH_TRANSPORT_IO;
        memset(reason, 0, sizeof reason);
        err = exchange(exchange_ctx, &request, &status, reason, sizeof reason,
                       &transport);
        if (err != CF_OK) {
            cf_push_request_dispose(&request);
            return err;
        }
        if (transport != CF_PUSH_TRANSPORT_OK) {
            err = push_transport_outcome(transport, request.host, out);
        } else {
            err = cf_push_classify_response(status, reason, request.host, out);
        }
        cf_push_request_dispose(&request);
        return err;
    }
}
