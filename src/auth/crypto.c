/* src/auth/crypto.c — OpenSSL-backed primitives for the current-format auth
 * layer (rails_compat key_generator/message_verifier/message_encryptor and
 * Ruby Base64 flavors; encoding.rs).  No cryptographic primitive is written
 * here: PBKDF2/HMAC/AES-GCM come from the pinned OpenSSL build, random bytes
 * from the project's OS entropy core (cf_random_bytes).
 */
#include "internal.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <stdlib.h>
#include <string.h>

cf_err auth_pbkdf2_sha256(cf_span password, cf_span salt, size_t length,
                          unsigned char *out) {
    if (out == NULL || length == 0 || length > 512) return CF_INVALID;
    if ((password.len != 0 && password.ptr == NULL) ||
        (salt.len != 0 && salt.ptr == NULL)) {
        return CF_INVALID;
    }
    if (password.len > (size_t)INT32_MAX || salt.len > (size_t)INT32_MAX) {
        return CF_LIMIT;
    }
    /* KeyGenerator: pbkdf2_hmac::<Sha256>(secret, salt, 1000, &mut key). */
    if (PKCS5_PBKDF2_HMAC((const char *)password.ptr, (int)password.len,
                          salt.ptr, (int)salt.len, 1000, EVP_sha256(),
                          (int)length, out) != 1) {
        return CF_INTERNAL;
    }
    return CF_OK;
}

cf_err auth_hmac(cf_auth_digest digest, cf_span key, cf_span data,
                 unsigned char *out, size_t *out_len) {
    if (out == NULL || out_len == NULL) return CF_INVALID;
    if (key.len > (size_t)INT32_MAX) return CF_LIMIT;
    const EVP_MD *md = digest == CF_AUTH_DIGEST_SHA256 ? EVP_sha256()
                                                       : EVP_sha1();
    unsigned int len = 0;
    if (HMAC(md, key.ptr, (int)key.len, data.ptr, data.len, out, &len) ==
        NULL) {
        return CF_INTERNAL;
    }
    *out_len = len;
    return CF_OK;
}

/* ActiveSupport::SecurityUtils.secure_compare-style: the length leaks, the
 * contents do not. */
bool auth_constant_time_equal(cf_span a, cf_span b) {
    if (a.len != b.len) return false;
    if (a.len == 0) return true;
    if (a.ptr == NULL || b.ptr == NULL) return false;
    return CRYPTO_memcmp(a.ptr, b.ptr, a.len) == 0;
}

/* --- Base64 (Ruby's strict/urlsafe flavors) -------------------------------- */

static const char auth_b64_std[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char auth_b64_url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static cf_err auth_base64_encode_alphabet(cf_span data, const char *alphabet,
                                          bool padding, cf_str *out) {
    size_t lines = (data.len + 2) / 3;
    size_t cap = lines * 4 + 1;
    char *text = malloc(cap);
    if (text == NULL) return CF_NOMEM;
    size_t at = 0;
    for (size_t i = 0; i < data.len; i += 3) {
        size_t remain = data.len - i;
        unsigned b0 = data.ptr[i];
        unsigned b1 = remain > 1 ? data.ptr[i + 1] : 0;
        unsigned b2 = remain > 2 ? data.ptr[i + 2] : 0;
        text[at++] = alphabet[b0 >> 2];
        text[at++] = alphabet[((b0 & 0x03) << 4) | (b1 >> 4)];
        if (remain > 1) {
            text[at++] = alphabet[((b1 & 0x0f) << 2) | (b2 >> 6)];
        } else if (padding) {
            text[at++] = '=';
        }
        if (remain > 2) {
            text[at++] = alphabet[b2 & 0x3f];
        } else if (padding) {
            text[at++] = '=';
        }
    }
    text[at] = '\0';
    out->ptr = text;
    out->len = at;
    return CF_OK;
}

cf_err auth_base64_encode(cf_span data, cf_auth_encoding encoding, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (data.len != 0 && data.ptr == NULL) return CF_INVALID;
    switch (encoding) {
    case CF_AUTH_ENCODING_STRICT:
        return auth_base64_encode_alphabet(data, auth_b64_std, true, out);
    case CF_AUTH_ENCODING_URLSAFE:
        return auth_base64_encode_alphabet(data, auth_b64_url, false, out);
    case CF_AUTH_ENCODING_URLSAFE_PADDED:
        return auth_base64_encode_alphabet(data, auth_b64_url, true, out);
    }
    return CF_INVALID;
}

static int auth_b64_value(unsigned char c, bool urlsafe_alphabet,
                          bool *ok) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (urlsafe_alphabet && c == '-') return 62;
    if (urlsafe_alphabet && c == '_') return 63;
    *ok = false;
    return -1;
}

/* Strict decode: one alphabet, canonical `=` padding, no whitespace,
 * canonical trailing bits (base64 crate STANDARD semantics). */
static cf_err auth_base64_decode_alphabet(cf_span encoded,
                                          bool urlsafe_alphabet,
                                          cf_buf **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (encoded.len == 0) {
        return cf_buf_copy((cf_span){NULL, 0}, out);
    }
    if (encoded.ptr == NULL) return CF_INVALID;
    if (encoded.len % 4 != 0) return CF_INVALID;

    size_t pad = 0;
    if (encoded.ptr[encoded.len - 1] == '=') {
        pad = 1;
        if (encoded.len >= 2 && encoded.ptr[encoded.len - 2] == '=') pad = 2;
    }
    for (size_t i = 0; i < encoded.len - pad; i++) {
        if (encoded.ptr[i] == '=') return CF_INVALID;
    }
    size_t data_len = encoded.len - pad;
    size_t out_len = data_len / 4 * 3;
    size_t rem = data_len % 4;
    if (rem == 1) return CF_INVALID;
    out_len += rem >= 2 ? rem - 1 : 0;

    unsigned char *bytes = malloc(out_len != 0 ? out_len : 1);
    if (bytes == NULL) return CF_NOMEM;
    size_t at = 0;
    unsigned acc = 0;
    int bits = 0;
    bool ok = true;
    for (size_t i = 0; i < data_len; i++) {
        int v = auth_b64_value(encoded.ptr[i], urlsafe_alphabet, &ok);
        if (!ok) {
            free(bytes);
            return CF_INVALID;
        }
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes[at++] = (unsigned char)((acc >> bits) & 0xff);
        }
    }
    /* Canonical trailing bits: whatever is left must be zero, and the number of
     * encoded characters must be consistent with the padding. */
    if (bits != 0 && (acc & ((1u << bits) - 1)) != 0) {
        free(bytes);
        return CF_INVALID;
    }
    if ((pad == 1 && rem != 3) || (pad == 2 && rem != 2)) {
        free(bytes);
        return CF_INVALID;
    }
    if (pad == 0 && rem == 0 && at != out_len) {
        free(bytes);
        return CF_INVALID;
    }
    cf_err rc = cf_buf_copy((cf_span){bytes, at}, out);
    free(bytes);
    return rc;
}

cf_err auth_base64_strict_decode(cf_span encoded, cf_buf **out) {
    return auth_base64_decode_alphabet(encoded, false, out);
}

/* Base64.urlsafe_decode64: translate -_ to +/, pad when the input is unpadded
 * and short, then a strict decode (encoding.rs). */
cf_err auth_base64_urlsafe_decode(cf_span encoded, cf_buf **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (encoded.len != 0 && encoded.ptr == NULL) return CF_INVALID;
    char *translated = malloc(encoded.len + 3 + 1);
    if (translated == NULL) return CF_NOMEM;
    for (size_t i = 0; i < encoded.len; i++) {
        char c = (char)encoded.ptr[i];
        translated[i] = c == '-' ? '+' : (c == '_' ? '/' : c);
    }
    size_t len = encoded.len;
    if (len != 0 && translated[len - 1] != '=' && len % 4 != 0) {
        while (len % 4 != 0) {
            translated[len++] = '=';
        }
    }
    translated[len] = '\0';
    cf_err rc = auth_base64_strict_decode(
        (cf_span){(const unsigned char *)translated, len}, out);
    free(translated);
    return rc;
}

cf_err auth_hex_encode(cf_span data, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (data.len != 0 && data.ptr == NULL) return CF_INVALID;
    char *text = malloc(data.len * 2 + 1);
    if (text == NULL) return CF_NOMEM;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < data.len; i++) {
        text[i * 2] = hex[data.ptr[i] >> 4];
        text[i * 2 + 1] = hex[data.ptr[i] & 0x0f];
    }
    text[data.len * 2] = '\0';
    out->ptr = text;
    out->len = data.len * 2;
    return CF_OK;
}

/* --- aes-256-gcm ----------------------------------------------------------- */

cf_err auth_aes256gcm_encrypt(cf_span key32, cf_span iv12, cf_span plaintext,
                              cf_buf **out_ciphertext, unsigned char tag[16]) {
    if (out_ciphertext == NULL || tag == NULL) return CF_INVALID;
    *out_ciphertext = NULL;
    if (key32.len != 32 || iv12.len != 12) return CF_INVALID;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) return CF_NOMEM;
    cf_err rc = CF_INTERNAL;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
        goto done;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1) {
        goto done;
    }
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key32.ptr, iv12.ptr) != 1) {
        goto done;
    }
    if (plaintext.len > (size_t)INT_MAX) {
        rc = CF_LIMIT;
        goto done;
    }
    {
        size_t cap = plaintext.len + 32; /* GCM writes inl bytes; keep slack */
        unsigned char *buf = malloc(cap != 0 ? cap : 1);
        if (buf == NULL) {
            rc = CF_NOMEM;
            goto done;
        }
        int len = 0;
        int total = 0;
        if (EVP_EncryptUpdate(ctx, buf, &len, plaintext.ptr,
                              (int)plaintext.len) != 1) {
            free(buf);
            goto done;
        }
        total = len;
        if (EVP_EncryptFinal_ex(ctx, buf + total, &len) != 1) {
            free(buf);
            goto done;
        }
        total += len;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) {
            free(buf);
            goto done;
        }
        rc = cf_buf_copy((cf_span){buf, (size_t)total}, out_ciphertext);
        free(buf);
    }
done:
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

bool auth_aes256gcm_decrypt(cf_span key32, cf_span iv12, cf_span ciphertext,
                            cf_span tag16, cf_buf **out_plaintext) {
    if (out_plaintext == NULL) return false;
    *out_plaintext = NULL;
    if (key32.len != 32 || iv12.len != 12 || tag16.len != 16) return false;
    if (ciphertext.len > (size_t)INT_MAX) return false;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) return false;
    bool ok = false;
    unsigned char *buf = malloc(ciphertext.len + 16 + 1);
    if (buf == NULL) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    int len = 0;
    int total = 0;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
        goto done;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1) {
        goto done;
    }
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key32.ptr, iv12.ptr) != 1) {
        goto done;
    }
    if (EVP_DecryptUpdate(ctx, buf, &len, ciphertext.ptr,
                          (int)ciphertext.len) != 1) {
        goto done;
    }
    total = len;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                            (void *)tag16.ptr) != 1) {
        goto done;
    }
    if (EVP_DecryptFinal_ex(ctx, buf + total, &len) != 1) {
        goto done;
    }
    total += len;
    ok = cf_buf_copy((cf_span){buf, (size_t)total}, out_plaintext) == CF_OK;
done:
    if (!ok && *out_plaintext != NULL) {
        cf_buf_release(*out_plaintext);
        *out_plaintext = NULL;
    }
    free(buf);
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}
