/* src/storage/active_storage.c — task S02: signed tokens, variation keys,
 * range/proxy/direct-upload route helpers, attachment states, variant
 * first-wins and purge file work. See active_storage.h for the contract.
 *
 * Reference behavior is translated, not redesigned; each section names its
 * pinned source. The one deliberate superset is the attachment classifier's
 * SIGNED state: the pinned Assignment treats a plain string as Invalid, but
 * Rails attaches a verified signed blob id, and the S02 card requires
 * "signed blob reference = verified existing blob", so a string that
 * verifies under purpose blob_id classifies SIGNED (existence is still the
 * action's cf_blob_find check). */
#include "storage/active_storage.h"

#include "auth.h"
#include "storage/storage.h"

#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>
#include <yyjson.h>

/* ---- tiny span/builder utilities ---------------------------------------- */

static cf_span span_cstr(const char *text) {
    cf_span span = {(const unsigned char *)text, text == NULL ? 0 : strlen(text)};
    return span;
}

static bool span_equal(cf_span a, cf_span b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

static cf_err append_cstr(cf_builder *out, const char *text) {
    return cf_builder_append(out, span_cstr(text));
}

/* "type/subtype" media type of a Content-Type header: up to the first ';',
 * trimmed of ASCII whitespace. Borrowed from the input. */
static cf_span media_type_of(cf_span header) {
    size_t end = 0;
    while (end < header.len && header.ptr[end] != ';') end++;
    size_t start = 0;
    while (start < end &&
           (header.ptr[start] == ' ' || header.ptr[start] == '\t'))
        start++;
    while (end > start &&
           (header.ptr[end - 1] == ' ' || header.ptr[end - 1] == '\t'))
        end--;
    return (cf_span){header.ptr + start, end - start};
}

/* ---- ActiveSupport JSON writing (rails_compat json.rs) -------------------- */

/* The json gem escapes quote, backslash and control characters (short forms
 * for \b \f \n \r \t, lowercase \u00XX otherwise) and leaves '/' alone;
 * ActiveSupport additionally escapes <, > and & as \u003c/\u003e/\u0026.
 * Non-ASCII bytes pass through raw (UTF-8). */
static cf_err json_write_string(cf_builder *out, cf_span text) {
    static const char hex[] = "0123456789abcdef";
    if (out == NULL || (text.len != 0 && text.ptr == NULL)) return CF_INVALID;
    cf_err rc = cf_builder_append(out, span_cstr("\""));
    for (size_t i = 0; i < text.len && rc == CF_OK; i++) {
        unsigned char c = text.ptr[i];
        switch (c) {
        case '"': rc = append_cstr(out, "\\\""); break;
        case '\\': rc = append_cstr(out, "\\\\"); break;
        case '\b': rc = append_cstr(out, "\\b"); break;
        case '\f': rc = append_cstr(out, "\\f"); break;
        case '\n': rc = append_cstr(out, "\\n"); break;
        case '\r': rc = append_cstr(out, "\\r"); break;
        case '\t': rc = append_cstr(out, "\\t"); break;
        case '<': rc = append_cstr(out, "\\u003c"); break;
        case '>': rc = append_cstr(out, "\\u003e"); break;
        case '&': rc = append_cstr(out, "\\u0026"); break;
        default:
            if (c < 0x20) {
                char esc[7] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15], '\0'};
                rc = cf_builder_append(out, (cf_span){(unsigned char *)esc, 6});
            } else {
                rc = cf_builder_append(out, (cf_span){&c, 1});
            }
            break;
        }
    }
    if (rc == CF_OK) rc = cf_builder_append(out, span_cstr("\""));
    return rc;
}

static cf_err json_write_int(cf_builder *out, int64_t value) {
    char text[32];
    int n = snprintf(text, sizeof text, "%lld", (long long)value);
    if (n <= 0 || (size_t)n >= sizeof text) return CF_INTERNAL;
    return cf_builder_append(out, (cf_span){(unsigned char *)text, (size_t)n});
}

/* ---- SHA1 + standard base64 (variation digest, variation.rs) -------------- */

static cf_err sha1_base64(cf_span input, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx == NULL) return CF_NOMEM;
    cf_err rc = CF_IO;
    if (EVP_DigestInit_ex(ctx, EVP_sha1(), NULL) != 1) goto done;
    if (input.len != 0 && EVP_DigestUpdate(ctx, input.ptr, input.len) != 1)
        goto done;
    if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) goto done;
    if (digest_len != 20) goto done;
    /* 20 bytes -> 28 base64 chars with padding, plus NUL. */
    char *text = malloc(29);
    if (text == NULL) {
        rc = CF_NOMEM;
        goto done;
    }
    int n = EVP_EncodeBlock((unsigned char *)text, digest, digest_len);
    if (n != 28) {
        free(text);
        goto done;
    }
    text[28] = '\0';
    out->ptr = text;
    out->len = 28;
    rc = CF_OK;
done:
    EVP_MD_CTX_free(ctx);
    return rc;
}

/* ---- verifier shorthand (name is always "ActiveStorage") ------------------ */

static cf_span active_storage_name(void) {
    return span_cstr("ActiveStorage");
}

/* ---- signed blob ids (paths.rs) ------------------------------------------ */

cf_err cf_active_blob_sign(cf_span secret_key_base, int64_t blob_id,
                           bool has_expiry, int64_t expires_us, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    char text[32];
    int n = snprintf(text, sizeof text, "%lld", (long long)blob_id);
    if (n <= 0 || (size_t)n >= sizeof text) return CF_INVALID;
    return cf_auth_app_verifier_generate(secret_key_base, active_storage_name(),
                                        (cf_span){(unsigned char *)text, (size_t)n},
                                        span_cstr("blob_id"), true, has_expiry,
                                        expires_us, out);
}

/* Strict i64 document: optional surrounding JSON whitespace, optional sign,
 * digits only (serde_json parses "42" as Int; floats and strings reject). */
static bool parse_blob_id(cf_span json, int64_t *out) {
    size_t start = 0, end = json.len;
    while (start < end &&
           (json.ptr[start] == ' ' || json.ptr[start] == '\t' ||
            json.ptr[start] == '\n' || json.ptr[start] == '\r'))
        start++;
    while (end > start &&
           (json.ptr[end - 1] == ' ' || json.ptr[end - 1] == '\t' ||
            json.ptr[end - 1] == '\n' || json.ptr[end - 1] == '\r'))
        end--;
    if (start == end) return false;
    bool negative = false;
    if (json.ptr[start] == '+' || json.ptr[start] == '-') {
        negative = json.ptr[start] == '-';
        start++;
        if (start == end) return false;
    }
    uint64_t magnitude = 0;
    for (size_t i = start; i < end; i++) {
        unsigned char c = json.ptr[i];
        if (c < '0' || c > '9') return false;
        unsigned digit = (unsigned)(c - '0');
        uint64_t limit = negative ? UINT64_C(9223372036854775808)
                                  : UINT64_C(9223372036854775807);
        if (magnitude > (limit - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    }
    if (!negative) {
        *out = (int64_t)magnitude;
    } else if (magnitude == UINT64_C(9223372036854775808)) {
        *out = INT64_MIN;
    } else {
        *out = -(int64_t)magnitude;
    }
    return true;
}

cf_err cf_active_blob_verify(cf_span secret_key_base, cf_span signed_id,
                             int64_t now_us, int64_t *out_id, bool *found) {
    if (out_id == NULL || found == NULL) return CF_INVALID;
    *out_id = 0;
    *found = false;
    if (signed_id.ptr == NULL) return CF_OK;
    cf_str data = {0};
    bool ok = false;
    cf_err rc = cf_auth_app_verifier_verify(secret_key_base, active_storage_name(),
                                            signed_id, span_cstr("blob_id"), true,
                                            now_us, &data, &ok);
    if (rc != CF_OK || !ok) {
        cf_str_dispose(&data);
        return rc;
    }
    int64_t id = 0;
    bool parsed =
        parse_blob_id((cf_span){(unsigned char *)data.ptr, data.len}, &id);
    cf_str_dispose(&data);
    if (!parsed) return CF_OK; /* verified but not an id: still not found */
    *out_id = id;
    *found = true;
    return CF_OK;
}

/* ---- disk keys and tokens (disk.rs) --------------------------------------- */

void cf_active_disk_key_dispose(cf_active_disk_key *key) {
    if (key == NULL) return;
    cf_str_dispose(&key->key);
    cf_str_dispose(&key->disposition);
    cf_optional_str_dispose(&key->content_type);
    cf_str_dispose(&key->service_name);
    memset(key, 0, sizeof *key);
}

void cf_active_disk_token_dispose(cf_active_disk_token *token) {
    if (token == NULL) return;
    cf_str_dispose(&token->key);
    cf_optional_str_dispose(&token->content_type);
    cf_str_dispose(&token->checksum);
    cf_str_dispose(&token->service_name);
    memset(token, 0, sizeof *token);
}

/* Copies one verified JSON string field into an owned cf_str. */
static bool yy_string_copy(yyjson_val *obj, const char *name, cf_str *out) {
    memset(out, 0, sizeof *out);
    yyjson_val *val = yyjson_obj_get(obj, name);
    if (val == NULL || !yyjson_is_str(val)) return false;
    size_t len = 0;
    const char *text = yyjson_get_str(val);
    if (text == NULL) return false;
    len = yyjson_get_len(val);
    char *copy = malloc(len + 1);
    if (copy == NULL) return false;
    memcpy(copy, text, len);
    copy[len] = '\0';
    out->ptr = copy;
    out->len = len;
    return true;
}

/* Optional string-or-null field: missing/null give present=false; a string
 * gives present=true; anything else fails the whole payload. */
static bool yy_optional_string_copy(yyjson_val *obj, const char *name,
                                    cf_optional_str *out) {
    out->present = false;
    out->value.ptr = NULL;
    out->value.len = 0;
    yyjson_val *val = yyjson_obj_get(obj, name);
    if (val == NULL || yyjson_is_null(val)) return true;
    if (!yyjson_is_str(val)) return false;
    out->present = true;
    return yy_string_copy(obj, name, &out->value);
}

static bool yy_int_field(yyjson_val *obj, const char *name, int64_t *out) {
    yyjson_val *val = yyjson_obj_get(obj, name);
    if (val == NULL) return false;
    if (yyjson_is_sint(val)) {
        *out = yyjson_get_sint(val);
        return true;
    }
    if (yyjson_is_uint(val)) {
        uint64_t u = yyjson_get_uint(val);
        if (u > (uint64_t)INT64_MAX) return false;
        *out = (int64_t)u;
        return true;
    }
    return false;
}

cf_err cf_active_disk_key_sign(cf_span secret_key_base, cf_span key,
                               cf_span disposition, cf_span content_type,
                               bool has_content_type, cf_span service_name,
                               bool has_expiry, int64_t expires_us,
                               cf_str *out) {
    if (out == NULL || (key.len != 0 && key.ptr == NULL)) return CF_INVALID;
    memset(out, 0, sizeof *out);
    cf_builder payload = {0};
    cf_err rc = cf_builder_append(&payload, span_cstr("{\"key\":"));
    if (rc == CF_OK) rc = json_write_string(&payload, key);
    if (rc == CF_OK) rc = append_cstr(&payload, ",\"disposition\":");
    if (rc == CF_OK) rc = json_write_string(&payload, disposition);
    if (rc == CF_OK) rc = append_cstr(&payload, ",\"content_type\":");
    if (rc == CF_OK) {
        if (has_content_type) {
            rc = json_write_string(&payload, content_type);
        } else {
            rc = append_cstr(&payload, "null");
        }
    }
    if (rc == CF_OK) rc = append_cstr(&payload, ",\"service_name\":");
    if (rc == CF_OK) rc = json_write_string(&payload, service_name);
    if (rc == CF_OK) rc = append_cstr(&payload, "}");
    if (rc == CF_OK) {
        rc = cf_auth_app_verifier_generate(secret_key_base, active_storage_name(),
                                           (cf_span){payload.ptr, payload.len},
                                           span_cstr("blob_key"), true,
                                           has_expiry, expires_us, out);
    }
    cf_builder_dispose(&payload);
    return rc;
}

cf_err cf_active_disk_key_verify(cf_span secret_key_base, cf_span encoded_key,
                                 int64_t now_us, cf_active_disk_key *out,
                                 bool *found) {
    if (out == NULL || found == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    *found = false;
    if (encoded_key.ptr == NULL) return CF_OK;
    cf_str data = {0};
    bool ok = false;
    cf_err rc = cf_auth_app_verifier_verify(secret_key_base, active_storage_name(),
                                            encoded_key, span_cstr("blob_key"), true,
                                            now_us, &data, &ok);
    if (rc != CF_OK || !ok) {
        cf_str_dispose(&data);
        return rc;
    }
    yyjson_doc *doc = yyjson_read_opts(data.ptr, data.len, 0, NULL, NULL);
    cf_str_dispose(&data);
    if (doc == NULL) return CF_OK;
    bool shaped = false;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (yyjson_is_obj(root) && yy_string_copy(root, "key", &out->key) &&
        yy_string_copy(root, "disposition", &out->disposition) &&
        yy_optional_string_copy(root, "content_type", &out->content_type) &&
        yy_string_copy(root, "service_name", &out->service_name)) {
        shaped = true;
    }
    yyjson_doc_free(doc);
    if (!shaped) {
        cf_active_disk_key_dispose(out);
        return CF_OK;
    }
    *found = true;
    return CF_OK;
}

cf_err cf_active_disk_token_sign(cf_span secret_key_base, cf_span key,
                                 cf_span content_type, bool has_content_type,
                                 int64_t content_length, cf_span checksum,
                                 cf_span service_name, int64_t expires_us,
                                 cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    cf_builder payload = {0};
    cf_err rc = cf_builder_append(&payload, span_cstr("{\"key\":"));
    if (rc == CF_OK) rc = json_write_string(&payload, key);
    if (rc == CF_OK) rc = append_cstr(&payload, ",\"content_type\":");
    if (rc == CF_OK) {
        if (has_content_type) {
            rc = json_write_string(&payload, content_type);
        } else {
            rc = append_cstr(&payload, "null");
        }
    }
    if (rc == CF_OK) rc = append_cstr(&payload, ",\"content_length\":");
    if (rc == CF_OK) rc = json_write_int(&payload, content_length);
    if (rc == CF_OK) rc = append_cstr(&payload, ",\"checksum\":");
    if (rc == CF_OK) rc = json_write_string(&payload, checksum);
    if (rc == CF_OK) rc = append_cstr(&payload, ",\"service_name\":");
    if (rc == CF_OK) rc = json_write_string(&payload, service_name);
    if (rc == CF_OK) rc = append_cstr(&payload, "}");
    if (rc == CF_OK) {
        rc = cf_auth_app_verifier_generate(secret_key_base, active_storage_name(),
                                           (cf_span){payload.ptr, payload.len},
                                           span_cstr("blob_token"), true, true,
                                           expires_us, out);
    }
    cf_builder_dispose(&payload);
    return rc;
}

cf_err cf_active_disk_token_verify(cf_span secret_key_base,
                                    cf_span encoded_token, int64_t now_us,
                                    cf_active_disk_token *out, bool *found) {
    if (out == NULL || found == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    *found = false;
    if (encoded_token.ptr == NULL) return CF_OK;
    cf_str data = {0};
    bool ok = false;
    cf_err rc = cf_auth_app_verifier_verify(secret_key_base, active_storage_name(),
                                            encoded_token, span_cstr("blob_token"),
                                            true, now_us, &data, &ok);
    if (rc != CF_OK || !ok) {
        cf_str_dispose(&data);
        return rc;
    }
    yyjson_doc *doc = yyjson_read_opts(data.ptr, data.len, 0, NULL, NULL);
    cf_str_dispose(&data);
    if (doc == NULL) return CF_OK;
    bool shaped = false;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (yyjson_is_obj(root) && yy_string_copy(root, "key", &out->key) &&
        yy_optional_string_copy(root, "content_type", &out->content_type) &&
        yy_int_field(root, "content_length", &out->content_length) &&
        yy_string_copy(root, "checksum", &out->checksum) &&
        yy_string_copy(root, "service_name", &out->service_name)) {
        shaped = true;
    }
    yyjson_doc_free(doc);
    if (!shaped) {
        cf_active_disk_token_dispose(out);
        return CF_OK;
    }
    *found = true;
    return CF_OK;
}

bool cf_active_disk_token_acceptable(const cf_active_disk_token *token,
                                     cf_span req_content_type,
                                     int64_t req_length, bool has_length) {
    if (token == NULL || !has_length) return false;
    if (token->content_length != req_length) return false;
    /* Both sides lowered after stripping parameters; absent token type only
     * matches an absent request type (reference compares Option==Option). */
    cf_span token_media = {NULL, 0};
    cf_span req_media = media_type_of(req_content_type);
    bool token_has = token->content_type.present;
    bool req_has = req_content_type.ptr != NULL && req_content_type.len != 0;
    if (token_has != req_has) return false;
    if (!token_has) return true;
    token_media = media_type_of((cf_span){(unsigned char *)token->content_type.value.ptr,
                                          token->content_type.value.len});
    if (token_media.len != req_media.len) return false;
    for (size_t i = 0; i < token_media.len; i++) {
        unsigned char a = token_media.ptr[i];
        unsigned char b = req_media.ptr[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
        if (a != b) return false;
    }
    return true;
}

cf_err cf_active_disk_upload(cf_storage *storage, cf_span key,
                             cf_span contents, cf_span checksum) {
    if (storage == NULL) return CF_INVALID;
    if (!cf_storage_key_valid(key)) return CF_INVALID;
    if ((contents.len != 0 && contents.ptr == NULL) ||
        (checksum.len == 0 || checksum.ptr == NULL))
        return CF_INVALID;
    cf_storage_upload *upload = NULL;
    cf_err rc = cf_storage_upload_begin(storage, &upload);
    if (rc != CF_OK) return rc;
    rc = cf_storage_upload_write(upload, contents);
    if (rc != CF_OK) {
        cf_storage_upload_dispose(upload);
        return rc;
    }
    /* The checksum covers the staged bytes (DiskService verifies the file it
     * just wrote); a mismatch deletes the staged bytes: 422, nothing kept. */
    cf_builder actual = {0};
    rc = cf_storage_upload_checksum(upload, &actual);
    if (rc == CF_OK) {
        cf_span have = {actual.ptr, actual.len};
        if (!span_equal(have, checksum)) rc = CF_INVALID;
    }
    if (rc != CF_OK) {
        cf_builder_dispose(&actual);
        cf_storage_upload_dispose(upload);
        return rc;
    }
    cf_builder_dispose(&actual);
    rc = cf_storage_upload_move(upload, key);
    if (rc != CF_OK) {
        cf_storage_upload_dispose(upload);
        return rc;
    }
    rc = cf_storage_upload_commit(upload);
    cf_storage_upload_dispose(upload);
    return rc;
}

/* ---- filenames, dispositions, escaping, content types -------------------- */

/* One UTF-8 code point from lossy input: invalid bytes become U+FFFD, like
 * String::from_utf8_lossy, and overlong/surrogate encodings are rejected. */
static uint32_t utf8_decode_lossy(const unsigned char *p, size_t left,
                                  size_t *out_len) {
    unsigned char c = p[0];
    if (c < 0x80) {
        *out_len = 1;
        return c;
    }
    size_t need = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) {
        need = 2;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        need = 3;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        need = 4;
        cp = c & 0x07;
    } else {
        *out_len = 1;
        return 0xFFFD;
    }
    if (need > left) {
        *out_len = 1;
        return 0xFFFD;
    }
    for (size_t i = 1; i < need; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *out_len = 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    /* Reject overlongs, surrogates and out-of-range values. */
    static const uint32_t min_cp[] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < min_cp[need] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        *out_len = 1;
        return 0xFFFD;
    }
    *out_len = need;
    return cp;
}

/* Filename#sanitized replacement set (filename.rs). */
static bool sanitize_replace(uint32_t cp) {
    switch (cp) {
    case 0x202E: /* RIGHT-TO-LEFT OVERRIDE */
    case '%':
    case '$':
    case '|':
    case ':':
    case ';':
    case '/':
    case '<':
    case '>':
    case '?':
    case '*':
    case '"':
    case '\t':
    case '\r':
    case '\n':
    case '\\': return true;
    default: return false;
    }
}

static bool strip_byte(unsigned char c) {
    return c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '\v' ||
           c == '\f' || c == '\r';
}

/* Encodes one code point as UTF-8 (U+FFFD for lone values is pre-decided by
 * the lossy decoder, so only valid scalar values arrive here). */
static cf_err append_utf8(cf_builder *out, uint32_t cp) {
    unsigned char buf[4];
    size_t n = 0;
    if (cp < 0x80) {
        buf[0] = (unsigned char)cp;
        n = 1;
    } else if (cp < 0x800) {
        buf[0] = (unsigned char)(0xC0 | (cp >> 6));
        buf[1] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        buf[0] = (unsigned char)(0xE0 | (cp >> 12));
        buf[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        buf[0] = (unsigned char)(0xF0 | (cp >> 18));
        buf[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    return cf_builder_append(out, (cf_span){buf, n});
}

cf_err cf_active_filename_sanitize(cf_span raw, cf_builder *out) {
    if (out == NULL || (raw.len != 0 && raw.ptr == NULL)) return CF_INVALID;
    /* Strip NUL/ASCII whitespace at both ends on the raw bytes, like
     * String#strip (NUL is ASCII 0, whitespace is SPACE). */
    size_t start = 0, end = raw.len;
    while (start < end && strip_byte(raw.ptr[start])) start++;
    while (end > start && strip_byte(raw.ptr[end - 1])) end--;
    size_t i = start;
    while (i < end) {
        size_t width = 0;
        uint32_t cp = utf8_decode_lossy(raw.ptr + i, end - i, &width);
        cf_err rc;
        if (sanitize_replace(cp)) {
            unsigned char dash = '-';
            rc = cf_builder_append(out, (cf_span){&dash, 1});
        } else if (width == 1 && cp < 0x80) {
            unsigned char c = (unsigned char)cp;
            rc = cf_builder_append(out, (cf_span){&c, 1});
        } else {
            rc = append_utf8(out, cp);
        }
        if (rc != CF_OK) return rc;
        i += width;
    }
    return CF_OK;
}

/* I18n default approximations for Content-Disposition transliteration
 * (rails_compat content_disposition/approximations.rs, generated from
 * i18n 1.14.7; sorted by code point for binary search). Anything else
 * non-ASCII becomes "?". */
typedef struct {
    uint32_t cp;
    const char *text;
} cf_active_approx;

static const cf_active_approx cf_active_approximations[] = {
    {0x00C0, "A"}, {0x00C1, "A"}, {0x00C2, "A"}, {0x00C3, "A"}, {0x00C4, "A"},
    {0x00C5, "A"}, {0x00C6, "AE"}, {0x00C7, "C"}, {0x00C8, "E"}, {0x00C9, "E"},
    {0x00CA, "E"}, {0x00CB, "E"}, {0x00CC, "I"}, {0x00CD, "I"}, {0x00CE, "I"},
    {0x00CF, "I"}, {0x00D0, "D"}, {0x00D1, "N"}, {0x00D2, "O"}, {0x00D3, "O"},
    {0x00D4, "O"}, {0x00D5, "O"}, {0x00D6, "O"}, {0x00D7, "x"}, {0x00D8, "O"},
    {0x00D9, "U"}, {0x00DA, "U"}, {0x00DB, "U"}, {0x00DC, "U"}, {0x00DD, "Y"},
    {0x00DE, "Th"}, {0x00DF, "ss"}, {0x00E0, "a"}, {0x00E1, "a"},
    {0x00E2, "a"}, {0x00E3, "a"}, {0x00E4, "a"}, {0x00E5, "a"}, {0x00E6, "ae"},
    {0x00E7, "c"}, {0x00E8, "e"}, {0x00E9, "e"}, {0x00EA, "e"}, {0x00EB, "e"},
    {0x00EC, "i"}, {0x00ED, "i"}, {0x00EE, "i"}, {0x00EF, "i"}, {0x00F0, "d"},
    {0x00F1, "n"}, {0x00F2, "o"}, {0x00F3, "o"}, {0x00F4, "o"}, {0x00F5, "o"},
    {0x00F6, "o"}, {0x00F8, "o"}, {0x00F9, "u"}, {0x00FA, "u"}, {0x00FB, "u"},
    {0x00FC, "u"}, {0x00FD, "y"}, {0x00FE, "th"}, {0x00FF, "y"},
    {0x0100, "A"}, {0x0101, "a"}, {0x0102, "A"}, {0x0103, "a"}, {0x0104, "A"},
    {0x0105, "a"}, {0x0106, "C"}, {0x0107, "c"}, {0x0108, "C"}, {0x0109, "c"},
    {0x010A, "C"}, {0x010B, "c"}, {0x010C, "C"}, {0x010D, "c"}, {0x010E, "D"},
    {0x010F, "d"}, {0x0110, "D"}, {0x0111, "d"}, {0x0112, "E"}, {0x0113, "e"},
    {0x0114, "E"}, {0x0115, "e"}, {0x0116, "E"}, {0x0117, "e"}, {0x0118, "E"},
    {0x0119, "e"}, {0x011A, "E"}, {0x011B, "e"}, {0x011C, "G"}, {0x011D, "g"},
    {0x011E, "G"}, {0x011F, "g"}, {0x0120, "G"}, {0x0121, "g"}, {0x0122, "G"},
    {0x0123, "g"}, {0x0124, "H"}, {0x0125, "h"}, {0x0126, "H"}, {0x0127, "h"},
    {0x0128, "I"}, {0x0129, "i"}, {0x012A, "I"}, {0x012B, "i"}, {0x012C, "I"},
    {0x012D, "i"}, {0x012E, "I"}, {0x012F, "i"}, {0x0130, "I"}, {0x0131, "i"},
    {0x0132, "IJ"}, {0x0133, "ij"}, {0x0134, "J"}, {0x0135, "j"},
    {0x0136, "K"}, {0x0137, "k"}, {0x0138, "k"}, {0x0139, "L"}, {0x013A, "l"},
    {0x013B, "L"}, {0x013C, "l"}, {0x013D, "L"}, {0x013E, "l"}, {0x013F, "L"},
    {0x0140, "l"}, {0x0141, "L"}, {0x0142, "l"}, {0x0143, "N"}, {0x0144, "n"},
    {0x0145, "N"}, {0x0146, "n"}, {0x0147, "N"}, {0x0148, "n"}, {0x0149, "'n"},
    {0x014A, "NG"}, {0x014B, "ng"}, {0x014C, "O"}, {0x014D, "o"},
    {0x014E, "O"}, {0x014F, "o"}, {0x0150, "O"}, {0x0151, "o"}, {0x0152, "OE"},
    {0x0153, "oe"}, {0x0154, "R"}, {0x0155, "r"}, {0x0156, "R"}, {0x0157, "r"},
    {0x0158, "R"}, {0x0159, "r"}, {0x015A, "S"}, {0x015B, "s"}, {0x015C, "S"},
    {0x015D, "s"}, {0x015E, "S"}, {0x015F, "s"}, {0x0160, "S"}, {0x0161, "s"},
    {0x0162, "T"}, {0x0163, "t"}, {0x0164, "T"}, {0x0165, "t"}, {0x0166, "T"},
    {0x0167, "t"}, {0x0168, "U"}, {0x0169, "u"}, {0x016A, "U"}, {0x016B, "u"},
    {0x016C, "U"}, {0x016D, "u"}, {0x016E, "U"}, {0x016F, "u"}, {0x0170, "U"},
    {0x0171, "u"}, {0x0172, "U"}, {0x0173, "u"}, {0x0174, "W"}, {0x0175, "w"},
    {0x0176, "Y"}, {0x0177, "y"}, {0x0178, "Y"}, {0x0179, "Z"}, {0x017A, "z"},
    {0x017B, "Z"}, {0x017C, "z"}, {0x017D, "Z"}, {0x017E, "z"}, {0x1E9E, "SS"},
};

static const char *transliterate_lookup(uint32_t cp) {
    size_t lo = 0;
    size_t hi = sizeof cf_active_approximations / sizeof cf_active_approximations[0];
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t at = cf_active_approximations[mid].cp;
        if (at == cp) return cf_active_approximations[mid].text;
        if (at < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return "?";
}

/* Traditional escaping keeps [ A-Za-z0-9!#$+.^_`|~-]; RFC 5987 keeps
 * [A-Za-z0-9!#$&+.^_`|~-] (content_disposition.rs). Escaping works on the
 * transliterated bytes (ASCII) and the raw UTF-8 bytes respectively. */
static bool traditional_keep(unsigned char c) {
    if (c == ' ' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9'))
        return true;
    return strchr("!#$+.^_`|~-", (char)c) != NULL;
}

static bool rfc5987_keep(unsigned char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9'))
        return true;
    return strchr("!#$&+.^_`|~-", (char)c) != NULL;
}

static cf_err append_percent_escaped(cf_builder *out, cf_span bytes,
                                     bool (*keep)(unsigned char)) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < bytes.len; i++) {
        unsigned char c = bytes.ptr[i];
        if (keep(c)) {
            cf_err rc = cf_builder_append(out, (cf_span){&c, 1});
            if (rc != CF_OK) return rc;
        } else {
            char esc[4] = {'%', hex[c >> 4], hex[c & 15], '\0'};
            cf_err rc = cf_builder_append(out, (cf_span){(unsigned char *)esc, 3});
            if (rc != CF_OK) return rc;
        }
    }
    return CF_OK;
}

cf_err cf_active_content_disposition(cf_span disposition, cf_span sanitized,
                                     cf_builder *out) {
    if (out == NULL || (sanitized.len != 0 && sanitized.ptr == NULL))
        return CF_INVALID;
    /* content_disposition_with: anything but "attachment" is inline. */
    const char *kind = span_equal(disposition, span_cstr("attachment"))
                           ? "attachment"
                           : "inline";
    /* Transliterated ASCII copy for filename=. */
    cf_builder latin = {0};
    for (size_t i = 0; i < sanitized.len;) {
        size_t width = 0;
        uint32_t cp = utf8_decode_lossy(sanitized.ptr + i, sanitized.len - i, &width);
        cf_err rc;
        if (cp < 0x80) {
            unsigned char c = (unsigned char)cp;
            rc = cf_builder_append(&latin, (cf_span){&c, 1});
        } else {
            rc = append_cstr(&latin, transliterate_lookup(cp));
        }
        if (rc != CF_OK) {
            cf_builder_dispose(&latin);
            return rc;
        }
        i += width;
    }
    cf_err rc = append_cstr(out, kind);
    if (rc == CF_OK) rc = append_cstr(out, "; filename=\"");
    if (rc == CF_OK)
        rc = append_percent_escaped(out, (cf_span){latin.ptr, latin.len},
                                    traditional_keep);
    if (rc == CF_OK) rc = append_cstr(out, "\"; filename*=UTF-8''");
    if (rc == CF_OK)
        rc = append_percent_escaped(out, sanitized, rfc5987_keep);
    cf_builder_dispose(&latin);
    return rc;
}

/* Journey escaping (disposition.rs): unreserved, sub-delims, ":" "@" and
 * (path only) "/". Percent encoding is uppercase. */
static bool journey_keep(unsigned char c, bool slash) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9'))
        return true;
    if (strchr("-._~!$&'()*+,;=:@", (char)c) != NULL) return true;
    return slash && c == '/';
}

static cf_err journey_escape(cf_span text, bool slash, cf_builder *out) {
    if (out == NULL || (text.len != 0 && text.ptr == NULL)) return CF_INVALID;
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < text.len;) {
        size_t width = 0;
        uint32_t cp = utf8_decode_lossy(text.ptr + i, text.len - i, &width);
        if (width == 1 && journey_keep((unsigned char)cp, slash)) {
            unsigned char c = (unsigned char)cp;
            cf_err rc = cf_builder_append(out, (cf_span){&c, 1});
            if (rc != CF_OK) return rc;
        } else {
            for (size_t k = 0; k < width; k++) {
                unsigned char c = text.ptr[i + k];
                char esc[4] = {'%', hex[c >> 4], hex[c & 15], '\0'};
                cf_err rc =
                    cf_builder_append(out, (cf_span){(unsigned char *)esc, 3});
                if (rc != CF_OK) return rc;
            }
        }
        i += width;
    }
    return CF_OK;
}

cf_err cf_active_escape_path(cf_span text, cf_builder *out) {
    return journey_escape(text, true, out);
}

cf_err cf_active_escape_segment(cf_span text, cf_builder *out) {
    return journey_escape(text, false, out);
}

/* Effective content-type lists (content_types.rs). The reference spells
 * every entry lowercase; comparisons are exact. */
static bool ct_in(cf_span content_type, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (span_equal(content_type, span_cstr(list[i]))) return true;
    }
    return false;
}

static const char *const cf_active_variable_types[] = {
    "image/png", "image/gif", "image/jpeg", "image/tiff",
    "image/webp", "image/avif", "image/heic", "image/heif",
};

static const char *const cf_active_web_image_types[] = {
    "image/png", "image/jpeg", "image/gif", "image/webp",
};

static const char *const cf_active_allowed_inline_types[] = {
    "image/webp", "image/avif", "image/png", "image/gif", "image/jpeg",
    "image/tiff", "image/bmp", "image/vnd.adobe.photoshop",
    "image/vnd.microsoft.icon", "application/pdf",
};

static const char *const cf_active_serve_as_binary_types[] = {
    "text/html", "image/svg+xml", "application/postscript",
    "application/x-shockwave-flash", "text/xml", "application/xml",
    "application/xhtml+xml", "application/mathml+xml", "text/cache-manifest",
};

bool cf_active_content_type_variable(cf_span content_type) {
    return ct_in(content_type, cf_active_variable_types,
                 sizeof cf_active_variable_types / sizeof cf_active_variable_types[0]);
}

bool cf_active_content_type_web_image(cf_span content_type) {
    return ct_in(content_type, cf_active_web_image_types,
                 sizeof cf_active_web_image_types / sizeof cf_active_web_image_types[0]);
}

bool cf_active_content_type_allowed_inline(cf_span content_type) {
    return ct_in(content_type, cf_active_allowed_inline_types,
                 sizeof cf_active_allowed_inline_types / sizeof cf_active_allowed_inline_types[0]);
}

bool cf_active_content_type_serve_as_binary(cf_span content_type) {
    return ct_in(content_type, cf_active_serve_as_binary_types,
                 sizeof cf_active_serve_as_binary_types / sizeof cf_active_serve_as_binary_types[0]);
}

cf_span cf_active_content_type_for_serving(cf_span content_type) {
    if (cf_active_content_type_serve_as_binary(content_type))
        return span_cstr("application/octet-stream");
    return content_type;
}

const char *cf_active_forced_disposition(cf_span content_type) {
    if (cf_active_content_type_serve_as_binary(content_type) ||
        !cf_active_content_type_allowed_inline(content_type))
        return "attachment";
    return NULL;
}

/* ---- variations (variation.rs + marshal.rs) -------------------------------- */

static cf_err vval_new(cf_active_vkind kind, cf_active_vval **out) {
    if (out == NULL) return CF_INVALID;
    cf_active_vval *value = calloc(1, sizeof *value);
    if (value == NULL) return CF_NOMEM;
    value->kind = kind;
    *out = value;
    return CF_OK;
}

/* Deep copy (variation_default outputs own their entries). */
static cf_err vval_clone(const cf_active_vval *src, cf_active_vval **out) {
    if (src == NULL || out == NULL) return CF_INVALID;
    cf_active_vval *copy = NULL;
    cf_err rc;
    switch (src->kind) {
    case CF_ACTIVE_V_NIL: rc = cf_active_vnil(&copy); break;
    case CF_ACTIVE_V_BOOL: rc = cf_active_vbool(src->boolean, &copy); break;
    case CF_ACTIVE_V_INT: rc = cf_active_vint(src->integer, &copy); break;
    case CF_ACTIVE_V_STR:
        rc = cf_active_vstr((cf_span){(unsigned char *)src->bytes, src->len}, &copy);
        break;
    case CF_ACTIVE_V_SYM:
        rc = cf_active_vsym((cf_span){(unsigned char *)src->bytes, src->len}, &copy);
        break;
    case CF_ACTIVE_V_ARR:
        rc = cf_active_varr(&copy);
        for (size_t i = 0; i < src->count && rc == CF_OK; i++) {
            cf_active_vval *item = NULL;
            rc = vval_clone(src->items[i], &item);
            if (rc == CF_OK) {
                rc = cf_active_varr_push(copy, item);
                if (rc != CF_OK) cf_active_vval_dispose(item);
            }
        }
        break;
    case CF_ACTIVE_V_HASH:
        rc = cf_active_vhash(&copy);
        for (size_t i = 0; i < src->hcount && rc == CF_OK; i++) {
            cf_active_vval *item = NULL;
            rc = vval_clone(src->entries[i].value, &item);
            if (rc == CF_OK) {
                rc = cf_active_vhash_set(copy,
                                         (cf_span){(unsigned char *)src->entries[i].key,
                                                   src->entries[i].key_len},
                                         item);
                if (rc != CF_OK) cf_active_vval_dispose(item);
            }
        }
        break;
    default: return CF_INVALID;
    }
    if (rc != CF_OK) {
        cf_active_vval_dispose(copy);
        return rc;
    }
    *out = copy;
    return CF_OK;
}

cf_err cf_active_vnil(cf_active_vval **out) {
    return vval_new(CF_ACTIVE_V_NIL, out);
}

cf_err cf_active_vbool(bool value, cf_active_vval **out) {
    cf_err rc = vval_new(CF_ACTIVE_V_BOOL, out);
    if (rc == CF_OK) (*out)->boolean = value;
    return rc;
}

cf_err cf_active_vint(int64_t value, cf_active_vval **out) {
    cf_err rc = vval_new(CF_ACTIVE_V_INT, out);
    if (rc == CF_OK) (*out)->integer = value;
    return rc;
}

static cf_err vval_new_bytes(cf_active_vkind kind, cf_span text,
                             cf_active_vval **out) {
    if (out == NULL || (text.len != 0 && text.ptr == NULL)) return CF_INVALID;
    cf_active_vval *value = NULL;
    cf_err rc = vval_new(kind, &value);
    if (rc != CF_OK) return rc;
    char *copy = malloc(text.len + 1);
    if (copy == NULL) {
        free(value);
        return CF_NOMEM;
    }
    if (text.len != 0) memcpy(copy, text.ptr, text.len);
    copy[text.len] = '\0';
    value->bytes = copy;
    value->len = text.len;
    *out = value;
    return CF_OK;
}

cf_err cf_active_vstr(cf_span text, cf_active_vval **out) {
    return vval_new_bytes(CF_ACTIVE_V_STR, text, out);
}

cf_err cf_active_vsym(cf_span name, cf_active_vval **out) {
    return vval_new_bytes(CF_ACTIVE_V_SYM, name, out);
}

cf_err cf_active_varr(cf_active_vval **out) {
    return vval_new(CF_ACTIVE_V_ARR, out);
}

cf_err cf_active_varr_push(cf_active_vval *arr, cf_active_vval *item) {
    if (arr == NULL || item == NULL || arr->kind != CF_ACTIVE_V_ARR)
        return CF_INVALID;
    if (arr->count == arr->cap) {
        size_t cap = arr->cap == 0 ? 4 : arr->cap * 2;
        if (cap > 4096) return CF_LIMIT;
        cf_active_vval **items = realloc(arr->items, cap * sizeof *items);
        if (items == NULL) return CF_NOMEM;
        arr->items = items;
        arr->cap = cap;
    }
    arr->items[arr->count++] = item;
    return CF_OK;
}

cf_err cf_active_vhash(cf_active_vval **out) {
    return vval_new(CF_ACTIVE_V_HASH, out);
}

cf_err cf_active_vhash_set(cf_active_vval *hash, cf_span key,
                           cf_active_vval *value) {
    if (hash == NULL || value == NULL || hash->kind != CF_ACTIVE_V_HASH ||
        (key.len != 0 && key.ptr == NULL))
        return CF_INVALID;
    if (hash->hcount == hash->hcap) {
        size_t cap = hash->hcap == 0 ? 4 : hash->hcap * 2;
        if (cap > 4096) return CF_LIMIT;
        cf_active_ventry *entries = realloc(hash->entries, cap * sizeof *entries);
        if (entries == NULL) return CF_NOMEM;
        hash->entries = entries;
        hash->hcap = cap;
    }
    char *copy = malloc(key.len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (key.len != 0) memcpy(copy, key.ptr, key.len);
    copy[key.len] = '\0';
    hash->entries[hash->hcount].key = copy;
    hash->entries[hash->hcount].key_len = key.len;
    hash->entries[hash->hcount].value = value;
    hash->hcount++;
    return CF_OK;
}

void cf_active_vval_dispose(cf_active_vval *value) {
    if (value == NULL) return;
    free(value->bytes);
    for (size_t i = 0; i < value->count; i++)
        cf_active_vval_dispose(value->items[i]);
    free(value->items);
    for (size_t i = 0; i < value->hcount; i++) {
        free(value->entries[i].key);
        cf_active_vval_dispose(value->entries[i].value);
    }
    free(value->entries);
    free(value);
}

bool cf_active_vval_equal(const cf_active_vval *a, const cf_active_vval *b) {
    if (a == b) return true;
    if (a == NULL || b == NULL || a->kind != b->kind) return false;
    switch (a->kind) {
    case CF_ACTIVE_V_NIL: return true;
    case CF_ACTIVE_V_BOOL: return a->boolean == b->boolean;
    case CF_ACTIVE_V_INT: return a->integer == b->integer;
    case CF_ACTIVE_V_STR:
    case CF_ACTIVE_V_SYM:
        return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
    case CF_ACTIVE_V_ARR:
        if (a->count != b->count) return false;
        for (size_t i = 0; i < a->count; i++) {
            if (!cf_active_vval_equal(a->items[i], b->items[i])) return false;
        }
        return true;
    case CF_ACTIVE_V_HASH:
        if (a->hcount != b->hcount) return false;
        for (size_t i = 0; i < a->hcount; i++) {
            if (a->entries[i].key_len != b->entries[i].key_len ||
                memcmp(a->entries[i].key, b->entries[i].key,
                       a->entries[i].key_len) != 0)
                return false;
            if (!cf_active_vval_equal(a->entries[i].value, b->entries[i].value))
                return false;
        }
        return true;
    }
    return false;
}

void cf_active_ventries_dispose(cf_active_ventries *entries) {
    if (entries == NULL) return;
    for (size_t i = 0; i < entries->len; i++) {
        free(entries->items[i].key);
        cf_active_vval_dispose(entries->items[i].value);
    }
    free(entries->items);
    memset(entries, 0, sizeof *entries);
}

bool cf_active_ventries_empty(const cf_active_ventries *entries) {
    return entries == NULL || entries->len == 0;
}

cf_err cf_active_ventries_push(cf_active_ventries *entries, cf_span key,
                               cf_active_vval *value) {
    if (entries == NULL || value == NULL || (key.len != 0 && key.ptr == NULL))
        return CF_INVALID;
    if (entries->len == entries->cap) {
        size_t cap = entries->cap == 0 ? 4 : entries->cap * 2;
        if (cap > 4096) return CF_LIMIT;
        cf_active_ventry *items = realloc(entries->items, cap * sizeof *items);
        if (items == NULL) return CF_NOMEM;
        entries->items = items;
        entries->cap = cap;
    }
    char *copy = malloc(key.len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (key.len != 0) memcpy(copy, key.ptr, key.len);
    copy[key.len] = '\0';
    entries->items[entries->len].key = copy;
    entries->items[entries->len].key_len = key.len;
    entries->items[entries->len].value = value;
    entries->len++;
    return CF_OK;
}

/* default_to: defaults first in position, overridden by the variation's own
 * keys (variation.rs). The output owns cloned values: release it with
 * cf_active_ventries_dispose. */
cf_err cf_active_variation_default(const cf_active_ventries *defaults,
                                   const cf_active_ventries *variation,
                                   cf_active_ventries *out) {
    if (defaults == NULL || variation == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    cf_err rc = CF_OK;
    for (size_t i = 0; i < defaults->len && rc == CF_OK; i++) {
        cf_active_vval *clone = NULL;
        rc = vval_clone(defaults->items[i].value, &clone);
        if (rc == CF_OK) {
            rc = cf_active_ventries_push(out,
                                         (cf_span){(unsigned char *)defaults->items[i].key,
                                                   defaults->items[i].key_len},
                                         clone);
            if (rc != CF_OK) cf_active_vval_dispose(clone);
        }
    }
    for (size_t i = 0; i < variation->len && rc == CF_OK; i++) {
        cf_span key = {(unsigned char *)variation->items[i].key,
                       variation->items[i].key_len};
        size_t at = out->len;
        for (size_t k = 0; k < out->len; k++) {
            if (out->items[k].key_len == key.len &&
                memcmp(out->items[k].key, key.ptr, key.len) == 0) {
                at = k;
                break;
            }
        }
        cf_active_vval *clone = NULL;
        rc = vval_clone(variation->items[i].value, &clone);
        if (rc != CF_OK) break;
        if (at < out->len) {
            cf_active_vval_dispose(out->items[at].value);
            out->items[at].value = clone;
        } else {
            rc = cf_active_ventries_push(out, key, clone);
            if (rc != CF_OK) cf_active_vval_dispose(clone);
        }
    }
    if (rc != CF_OK) cf_active_ventries_dispose(out);
    return rc;
}

/* ---- Marshal 4.8 dump subset (marshal.rs) --------------------------------- */

typedef struct {
    cf_builder *out;
    char **symbols;
    size_t *symbol_lens;
    size_t nsymbols, symcap;
} marshal_writer;

static cf_err marshal_long(marshal_writer *w, int64_t n) {
    unsigned char buf[9];
    if (n == 0) {
        buf[0] = 0;
        return cf_builder_append(w->out, (cf_span){buf, 1});
    }
    if (n > 0 && n < 123) {
        buf[0] = (unsigned char)(n + 5);
        return cf_builder_append(w->out, (cf_span){buf, 1});
    }
    if (n < 0 && n > -124) {
        buf[0] = (unsigned char)(((n - 5) & 0xFF));
        return cf_builder_append(w->out, (cf_span){buf, 1});
    }
    size_t count = 0;
    int64_t x = n;
    for (size_t i = 1; i <= 8; i++) {
        buf[i] = (unsigned char)(x & 0xFF);
        x >>= 8;
        if (x == 0) {
            buf[0] = (unsigned char)i;
            count = i + 1;
            return cf_builder_append(w->out, (cf_span){buf, count});
        }
        if (x == -1) {
            buf[0] = (unsigned char)(-(int)i);
            count = i + 1;
            return cf_builder_append(w->out, (cf_span){buf, count});
        }
    }
    return CF_INTERNAL; /* unreachable for 64-bit values */
}

static cf_err marshal_symbol(marshal_writer *w, const char *name, size_t len) {
    for (size_t i = 0; i < w->nsymbols; i++) {
        if (w->symbol_lens[i] == len && memcmp(w->symbols[i], name, len) == 0) {
            cf_err rc = cf_builder_append(w->out, span_cstr(";"));
            if (rc == CF_OK) rc = marshal_long(w, (int64_t)i);
            return rc;
        }
    }
    if (w->nsymbols == w->symcap) {
        size_t cap = w->symcap == 0 ? 16 : w->symcap * 2;
        char **symbols = realloc(w->symbols, cap * sizeof *symbols);
        if (symbols == NULL) return CF_NOMEM;
        /* Live immediately: nsymbols == symcap still holds, so the stale
         * cap stays consistent if the second allocation fails. */
        w->symbols = symbols;
        size_t *lens = realloc(w->symbol_lens, cap * sizeof *lens);
        if (lens == NULL) return CF_NOMEM;
        w->symbol_lens = lens;
        w->symcap = cap;
    }
    char *copy = malloc(len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (len != 0) memcpy(copy, name, len);
    copy[len] = '\0';
    w->symbols[w->nsymbols] = copy;
    w->symbol_lens[w->nsymbols] = len;
    w->nsymbols++;
    cf_err rc = cf_builder_append(w->out, span_cstr(":"));
    if (rc == CF_OK) rc = marshal_long(w, (int64_t)len);
    if (rc == CF_OK && len != 0)
        rc = cf_builder_append(w->out, (cf_span){(unsigned char *)name, len});
    return rc;
}

static cf_err marshal_value(marshal_writer *w, const cf_active_vval *value);

static cf_err marshal_integer(marshal_writer *w, int64_t n) {
    /* Inline range is -(1<<30)..(1<<30) (marshal.rs). */
    if (n > -(INT64_C(1) << 30) && n < (INT64_C(1) << 30)) {
        cf_err rc = cf_builder_append(w->out, span_cstr("i"));
        if (rc == CF_OK) rc = marshal_long(w, n);
        return rc;
    }
    cf_err rc = cf_builder_append(w->out, span_cstr("l"));
    if (rc != CF_OK) return rc;
    bool negative = n < 0;
    uint64_t magnitude = negative ? (uint64_t)(-(n + 1)) + 1 : (uint64_t)n;
    unsigned char digits[8];
    size_t ndigits = 0;
    while (magnitude > 0) {
        digits[ndigits++] = (unsigned char)(magnitude & 0xFF);
        magnitude >>= 8;
    }
    if ((ndigits % 2) == 1) digits[ndigits++] = 0;
    if (rc == CF_OK)
        rc = cf_builder_append(w->out, negative ? span_cstr("-") : span_cstr("+"));
    if (rc == CF_OK) rc = marshal_long(w, (int64_t)(ndigits / 2));
    if (rc == CF_OK && ndigits != 0)
        rc = cf_builder_append(w->out, (cf_span){digits, ndigits});
    return rc;
}

static cf_err marshal_value(marshal_writer *w, const cf_active_vval *value) {
    if (value == NULL) return CF_INVALID;
    switch (value->kind) {
    case CF_ACTIVE_V_NIL: return cf_builder_append(w->out, span_cstr("0"));
    case CF_ACTIVE_V_BOOL:
        return cf_builder_append(w->out,
                                 value->boolean ? span_cstr("T") : span_cstr("F"));
    case CF_ACTIVE_V_INT: return marshal_integer(w, value->integer);
    case CF_ACTIVE_V_SYM:
        return marshal_symbol(w, value->bytes == NULL ? "" : value->bytes, value->len);
    case CF_ACTIVE_V_STR: {
        cf_err rc = cf_builder_append(w->out, span_cstr("I\""));
        if (rc == CF_OK) rc = marshal_long(w, (int64_t)value->len);
        if (rc == CF_OK && value->len != 0)
            rc = cf_builder_append(w->out,
                                   (cf_span){(unsigned char *)value->bytes, value->len});
        if (rc == CF_OK) rc = marshal_long(w, 1);
        if (rc == CF_OK) rc = marshal_symbol(w, "E", 1);
        if (rc == CF_OK) rc = cf_builder_append(w->out, span_cstr("T"));
        return rc;
    }
    case CF_ACTIVE_V_ARR: {
        cf_err rc = cf_builder_append(w->out, span_cstr("["));
        if (rc == CF_OK) rc = marshal_long(w, (int64_t)value->count);
        for (size_t i = 0; i < value->count && rc == CF_OK; i++)
            rc = marshal_value(w, value->items[i]);
        return rc;
    }
    case CF_ACTIVE_V_HASH: {
        cf_err rc = cf_builder_append(w->out, span_cstr("{"));
        if (rc == CF_OK) rc = marshal_long(w, (int64_t)value->hcount);
        for (size_t i = 0; i < value->hcount && rc == CF_OK; i++) {
            rc = marshal_symbol(w, value->entries[i].key,
                                value->entries[i].key_len);
            if (rc == CF_OK) rc = marshal_value(w, value->entries[i].value);
        }
        return rc;
    }
    }
    return CF_INVALID;
}

/* Dumps a whole variation (a hash of its ordered entries). */
static cf_err marshal_variation(cf_builder *out,
                                const cf_active_ventries *variation,
                                char ***symbols, size_t **lens, size_t *n,
                                size_t *cap) {
    marshal_writer w = {out, *symbols, *lens, *n, *cap};
    cf_err rc = cf_builder_append(out, span_cstr("\x04\x08"));
    if (rc == CF_OK) rc = cf_builder_append(out, span_cstr("{"));
    if (rc == CF_OK) rc = marshal_long(&w, (int64_t)variation->len);
    for (size_t i = 0; i < variation->len && rc == CF_OK; i++) {
        rc = marshal_symbol(&w, variation->items[i].key,
                            variation->items[i].key_len);
        if (rc == CF_OK) rc = marshal_value(&w, variation->items[i].value);
    }
    *symbols = w.symbols;
    *lens = w.symbol_lens;
    *n = w.nsymbols;
    *cap = w.symcap;
    return rc;
}

cf_err cf_active_variation_digest(const cf_active_ventries *variation,
                                  cf_str *out) {
    if (variation == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    cf_builder dumped = {0};
    char **symbols = NULL;
    size_t *lens = NULL, n = 0, cap = 0;
    cf_err rc = marshal_variation(&dumped, variation, &symbols, &lens, &n, &cap);
    for (size_t i = 0; i < n; i++) free(symbols[i]);
    free(symbols);
    free(lens);
    if (rc == CF_OK)
        rc = sha1_base64((cf_span){dumped.ptr, dumped.len}, out);
    cf_builder_dispose(&dumped);
    return rc;
}

/* ---- variation JSON (to_json / from_json, variation.rs) ------------------- */

static cf_err variation_write_value(cf_builder *out,
                                    const cf_active_vval *value) {
    if (value == NULL) return CF_INVALID;
    switch (value->kind) {
    case CF_ACTIVE_V_NIL: return append_cstr(out, "null");
    case CF_ACTIVE_V_BOOL:
        return append_cstr(out, value->boolean ? "true" : "false");
    case CF_ACTIVE_V_INT: return json_write_int(out, value->integer);
    case CF_ACTIVE_V_STR:
    case CF_ACTIVE_V_SYM: /* symbols become strings on the wire */
        return json_write_string(out, (cf_span){(unsigned char *)value->bytes, value->len});
    case CF_ACTIVE_V_ARR: {
        cf_err rc = append_cstr(out, "[");
        for (size_t i = 0; i < value->count && rc == CF_OK; i++) {
            if (i != 0) rc = append_cstr(out, ",");
            if (rc == CF_OK) rc = variation_write_value(out, value->items[i]);
        }
        if (rc == CF_OK) rc = append_cstr(out, "]");
        return rc;
    }
    case CF_ACTIVE_V_HASH: {
        cf_err rc = append_cstr(out, "{");
        for (size_t i = 0; i < value->hcount && rc == CF_OK; i++) {
            if (i != 0) rc = append_cstr(out, ",");
            if (rc == CF_OK)
                rc = json_write_string(out, (cf_span){(unsigned char *)value->entries[i].key,
                                                      value->entries[i].key_len});
            if (rc == CF_OK) rc = append_cstr(out, ":");
            if (rc == CF_OK) rc = variation_write_value(out, value->entries[i].value);
        }
        if (rc == CF_OK) rc = append_cstr(out, "}");
        return rc;
    }
    }
    return CF_INVALID;
}

static cf_err variation_write_entries(cf_builder *out,
                                      const cf_active_ventries *entries) {
    cf_err rc = append_cstr(out, "{");
    for (size_t i = 0; i < entries->len && rc == CF_OK; i++) {
        if (i != 0) rc = append_cstr(out, ",");
        if (rc == CF_OK)
            rc = json_write_string(out, (cf_span){(unsigned char *)entries->items[i].key,
                                                  entries->items[i].key_len});
        if (rc == CF_OK) rc = append_cstr(out, ":");
        if (rc == CF_OK) rc = variation_write_value(out, entries->items[i].value);
    }
    if (rc == CF_OK) rc = append_cstr(out, "}");
    return rc;
}

cf_err cf_active_variation_sign(cf_span secret_key_base,
                                const cf_active_ventries *variation,
                                cf_str *out) {
    if (variation == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    cf_builder data = {0};
    cf_err rc = variation_write_entries(&data, variation);
    if (rc == CF_OK) {
        rc = cf_auth_app_verifier_generate(secret_key_base, active_storage_name(),
                                           (cf_span){data.ptr, data.len},
                                           span_cstr("variation"), true, false,
                                           0, out);
    }
    cf_builder_dispose(&data);
    return rc;
}

/* yyjson document -> variation Value (from_json): strings stay strings,
 * floats and out-of-range integers are shape errors. */
static cf_err variation_from_json(yyjson_val *val, cf_active_vval **out) {
    if (val == NULL || out == NULL) return CF_INTERNAL;
    if (yyjson_is_null(val)) return cf_active_vnil(out);
    if (yyjson_is_true(val)) return cf_active_vbool(true, out);
    if (yyjson_is_false(val)) return cf_active_vbool(false, out);
    if (yyjson_is_sint(val)) return cf_active_vint(yyjson_get_sint(val), out);
    if (yyjson_is_uint(val)) {
        uint64_t u = yyjson_get_uint(val);
        if (u > (uint64_t)INT64_MAX) return CF_INTERNAL;
        return cf_active_vint((int64_t)u, out);
    }
    if (yyjson_is_str(val)) {
        return cf_active_vstr((cf_span){(unsigned char *)yyjson_get_str(val),
                                        yyjson_get_len(val)},
                              out);
    }
    if (yyjson_is_arr(val)) {
        cf_active_vval *arr = NULL;
        cf_err rc = cf_active_varr(&arr);
        size_t count = yyjson_arr_size(val);
        for (size_t i = 0; i < count && rc == CF_OK; i++) {
            cf_active_vval *item = NULL;
            rc = variation_from_json(yyjson_arr_get(val, i), &item);
            if (rc == CF_OK) {
                rc = cf_active_varr_push(arr, item);
                if (rc != CF_OK) cf_active_vval_dispose(item);
            } else {
                cf_active_vval_dispose(item);
            }
        }
        if (rc != CF_OK) {
            cf_active_vval_dispose(arr);
            return rc;
        }
        *out = arr;
        return CF_OK;
    }
    if (yyjson_is_obj(val)) {
        cf_active_vval *hash = NULL;
        cf_err rc = cf_active_vhash(&hash);
        if (rc == CF_OK) {
            yyjson_obj_iter iter;
            if (!yyjson_obj_iter_init(val, &iter)) {
                rc = CF_INTERNAL;
            } else {
                yyjson_val *key = NULL;
                while (rc == CF_OK &&
                       (key = yyjson_obj_iter_next(&iter)) != NULL) {
                    yyjson_val *item = yyjson_obj_iter_get_val(key);
                    cf_active_vval *converted = NULL;
                    rc = variation_from_json(item, &converted);
                    if (rc == CF_OK) {
                        rc = cf_active_vhash_set(hash,
                                                 (cf_span){(unsigned char *)yyjson_get_str(key),
                                                           yyjson_get_len(key)},
                                                 converted);
                        if (rc != CF_OK) cf_active_vval_dispose(converted);
                    } else {
                        cf_active_vval_dispose(converted);
                    }
                }
            }
        }
        if (rc != CF_OK) {
            cf_active_vval_dispose(hash);
            return rc;
        }
        *out = hash;
        return CF_OK;
    }
    /* Numbers that are neither int nor uint are floats: unsupported. */
    return CF_INTERNAL;
}

cf_err cf_active_variation_verify(cf_span secret_key_base, cf_span key,
                                  int64_t now_us, cf_active_ventries *out,
                                  bool *found) {
    if (out == NULL || found == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    *found = false;
    if (key.ptr == NULL) return CF_OK;
    cf_str data = {0};
    bool ok = false;
    cf_err rc = cf_auth_app_verifier_verify(secret_key_base, active_storage_name(),
                                            key, span_cstr("variation"), true,
                                            now_us, &data, &ok);
    if (rc != CF_OK || !ok) {
        cf_str_dispose(&data);
        return rc;
    }
    yyjson_doc *doc = yyjson_read_opts(data.ptr, data.len, 0, NULL, NULL);
    cf_str_dispose(&data);
    if (doc == NULL) return CF_INTERNAL; /* verified but unparseable: 500 */
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool shaped = yyjson_is_obj(root);
    cf_err shape_rc = CF_INTERNAL;
    if (shaped) {
        yyjson_obj_iter iter;
        if (!yyjson_obj_iter_init(root, &iter)) {
            shaped = false;
        } else {
            yyjson_val *k = NULL;
            while (shaped && (k = yyjson_obj_iter_next(&iter)) != NULL) {
                yyjson_val *item = yyjson_obj_iter_get_val(k);
                cf_active_vval *converted = NULL;
                cf_err crc = variation_from_json(item, &converted);
                if (crc != CF_OK) {
                    cf_active_vval_dispose(converted);
                    shape_rc = crc; /* float/out-of-range: 500, like Rails */
                    shaped = false;
                    break;
                }
                crc = cf_active_ventries_push(out,
                                              (cf_span){(unsigned char *)yyjson_get_str(k),
                                                        yyjson_get_len(k)},
                                              converted);
                if (crc != CF_OK) {
                    cf_active_vval_dispose(converted);
                    shape_rc = crc; /* resource failure, not a shape error */
                    shaped = false;
                    break;
                }
            }
        }
    }
    yyjson_doc_free(doc);
    if (!shaped) {
        cf_active_ventries_dispose(out);
        return shape_rc;
    }
    *found = true;
    return CF_OK;
}

/* ---- byte ranges (ruby_compat rack.rs, exact port) ------------------------- */

/* Ruby's ISSPACE for to_i/strip: space, \t, \n, \v(0x0b), \f(0x0c), \r. */
static bool ruby_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\x0b' || c == '\x0c' ||
           c == '\r';
}

/* Saturating wide integer with an overflow flag: values beyond int64 keep
 * their sign in `over` (+1 above, -1 below) so range clamping stays exact
 * without __int128. */
typedef struct {
    int64_t value;
    int over; /* -1, 0, +1 */
} wide_int;

/* String#to_i subset used by ranges (integer.rs): leading SPACE, one sign,
 * an optional 0d/0D prefix, then digits with single underscores between. */
static wide_int ruby_to_i(cf_span text) {
    wide_int result = {0, 0};
    size_t i = 0;
    while (i < text.len && ruby_space((char)text.ptr[i])) i++;
    bool negative = false;
    if (i < text.len && (text.ptr[i] == '-' || text.ptr[i] == '+')) {
        negative = text.ptr[i] == '-';
        i++;
    }
    if (i + 2 <= text.len && text.ptr[i] == '0' &&
        (text.ptr[i + 1] == 'd' || text.ptr[i + 1] == 'D'))
        i += 2;
    uint64_t magnitude = 0;
    bool overflow = false;
    bool previous_digit = false;
    for (; i < text.len; i++) {
        unsigned char c = text.ptr[i];
        if (c >= '0' && c <= '9') {
            unsigned digit = (unsigned)(c - '0');
            if (magnitude > (UINT64_MAX - digit) / 10) overflow = true;
            else magnitude = magnitude * 10 + digit;
            previous_digit = true;
        } else if (c == '_' && previous_digit) {
            previous_digit = false;
        } else {
            break;
        }
    }
    if (overflow || magnitude > (uint64_t)INT64_MAX + (negative ? 1 : 0)) {
        result.over = negative ? -1 : 1;
        result.value = negative ? INT64_MIN : INT64_MAX;
        return result;
    }
    if (!negative) {
        result.value = (int64_t)magnitude;
    } else if (magnitude == (uint64_t)INT64_MAX + 1) {
        result.value = INT64_MIN;
    } else {
        result.value = -(int64_t)magnitude;
    }
    return result;
}

bool cf_active_integer_cast(cf_span text, int64_t *out) {
    if (out == NULL || text.ptr == NULL) return false;
    /* integer_cast: nil unless it starts like a number, nil out of range. */
    size_t i = 0;
    while (i < text.len && ruby_space((char)text.ptr[i])) i++;
    size_t j = i;
    if (j < text.len && (text.ptr[j] == '+' || text.ptr[j] == '-')) j++;
    if (j >= text.len || text.ptr[j] < '0' || text.ptr[j] > '9') return false;
    wide_int parsed = ruby_to_i(text);
    if (parsed.over != 0) return false;
    *out = parsed.value;
    return true;
}

void cf_active_ranges_dispose(cf_active_range *ranges) {
    free(ranges);
}

/* `http_range =~ /bytes=([^;]+)/`: after the first "bytes=" that something
 * other than ";" follows, up to the next ";". */
static bool range_spec_of(cf_span header, cf_span *out) {
    if (header.ptr == NULL) return false;
    for (size_t i = 0; i + 6 <= header.len; i++) {
        if (memcmp(header.ptr + i, "bytes=", 6) != 0) continue;
        size_t start = i + 6;
        size_t end = start;
        while (end < header.len && header.ptr[end] != ';') end++;
        if (end > start) {
            *out = (cf_span){header.ptr + start, end - start};
            return true;
        }
    }
    return false;
}

cf_err cf_active_range_parse(cf_span header, bool has_header, uint64_t size,
                             cf_active_range **out, size_t *out_n,
                             cf_active_range_outcome *outcome) {
    if (out == NULL || out_n == NULL || outcome == NULL) return CF_INVALID;
    *out = NULL;
    *out_n = 0;
    *outcome = CF_ACTIVE_RANGE_FULL;
    if (!has_header || header.ptr == NULL) return CF_OK;
    /* A blank header is filtered before this call in the reference; treat
     * whitespace-only as absent for safety. */
    bool blank = true;
    for (size_t i = 0; i < header.len; i++) {
        if (header.ptr[i] != ' ' && header.ptr[i] != '\t') {
            blank = false;
            break;
        }
    }
    if (blank) return CF_OK;
    if (size == 0) return CF_OK;
    cf_span spec = {NULL, 0};
    if (!range_spec_of(header, &spec)) {
        *outcome = CF_ACTIVE_RANGE_INVALID;
        return CF_OK;
    }
    size_t commas = 0;
    for (size_t i = 0; i < spec.len; i++) {
        if (spec.ptr[i] == ',') {
            commas++;
            if (commas >= 100) {
                *outcome = CF_ACTIVE_RANGE_INVALID;
                return CF_OK;
            }
        }
    }
    (void)0; /* comma counting above is the max_ranges gate */
    /* Split on ',' skipping following spaces/tabs; drop trailing empties. */
    cf_active_range *ranges = NULL;
    size_t count = 0, cap = 0;
    uint64_t total = 0;
    bool unsat = false;
    cf_err rc = CF_OK;
    size_t pos = 0;
    /* Collect field boundaries first (small, bounded by the header). */
    typedef struct {
        size_t start, end;
    } field;
    field *fields = NULL;
    size_t nfields = 0, fcap = 0;
    while (pos <= spec.len) {
        size_t comma = pos;
        while (comma < spec.len && spec.ptr[comma] != ',') comma++;
        size_t resume = comma;
        if (resume < spec.len) {
            resume++; /* skip ',' */
            while (resume < spec.len &&
                   (spec.ptr[resume] == ' ' || spec.ptr[resume] == '\t'))
                resume++;
        }
        if (nfields == fcap) {
            size_t ncap = fcap == 0 ? 8 : fcap * 2;
            if (ncap > 4096) {
                rc = CF_LIMIT;
                break;
            }
            field *grown = realloc(fields, ncap * sizeof *grown);
            if (grown == NULL) {
                rc = CF_NOMEM;
                break;
            }
            fields = grown;
            fcap = ncap;
        }
        fields[nfields].start = pos;
        fields[nfields].end = comma;
        nfields++;
        if (comma == spec.len) break;
        pos = resume;
    }
    while (nfields > 0 && fields[nfields - 1].start == fields[nfields - 1].end)
        nfields--;
    for (size_t f = 0; f < nfields && rc == CF_OK; f++) {
        cf_span part = {spec.ptr + fields[f].start,
                        fields[f].end - fields[f].start};
        const unsigned char *dash = memchr(part.ptr, '-', part.len);
        if (dash == NULL) {
            *outcome = CF_ACTIVE_RANGE_INVALID;
            break;
        }
        size_t di = (size_t)(dash - part.ptr);
        cf_span first = {part.ptr, di};
        /* Split on the FIRST "-": a second dash stays inside `last` and
         * stops to_i, exactly like the reference. */
        cf_span last = {part.ptr + di + 1, part.len - di - 1};
        bool last_absent = (di + 1 >= part.len);
        /* sizes above INT64_MAX cannot occur for real files; clamp so the
         * signed arithmetic below stays exact. */
        int64_t size_i =
            size > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)size;
        uint64_t r0 = 0, r1 = 0;
        bool skip = false;
        if (first.len == 0) {
            /* Suffix range: last must be present ("-" alone was dropped to
             * [""] above and lands here with last absent). */
            if (last_absent) {
                *outcome = CF_ACTIVE_RANGE_INVALID;
                break;
            }
            wide_int suffix = ruby_to_i(last);
            int64_t start_i;
            if (suffix.over > 0 || suffix.value >= size_i) start_i = 0;
            else if (suffix.over < 0 || suffix.value <= 0) start_i = size_i;
            else start_i = size_i - suffix.value;
            if (start_i < 0) start_i = 0;
            r0 = (uint64_t)start_i;
            r1 = size - 1;
            if (r0 > r1) skip = true;
        } else {
            wide_int begin = ruby_to_i(first);
            /* first never contains '-' (split point), so begin >= 0 or
             * hugely positive; huge means past the end. */
            bool begin_past =
                begin.over > 0 || begin.value >= size_i;
            uint64_t b0 = begin.over > 0 ? size : (uint64_t)begin.value;
            if (last_absent) {
                r0 = b0;
                r1 = size - 1;
                if (begin_past || r0 > r1) skip = true;
            } else {
                wide_int end = ruby_to_i(last);
                /* Raw r1 < r0 ignores the whole header (INVALID). Huge
                 * values saturate equal, so over>0 is never below. */
                bool end_below;
                if (end.over < 0) end_below = true;
                else if (end.over > 0) end_below = false;
                else if (begin.over > 0) end_below = true;
                else end_below = end.value < begin.value;
                if (end_below) {
                    *outcome = CF_ACTIVE_RANGE_INVALID;
                    break;
                }
                int64_t e1;
                if (end.over > 0 || end.value >= size_i) e1 = size_i - 1;
                else if (end.value < 0) e1 = -1;
                else e1 = end.value;
                r0 = b0;
                if (e1 < 0 || begin_past || b0 > (uint64_t)e1) skip = true;
                else r1 = (uint64_t)e1;
            }
        }
        if (skip) continue;
        /* Over-broad sets are unsatisfiable (sum of lengths > size). */
        uint64_t len = r1 - r0 + 1;
        if (len > size - total) unsat = true;
        else total += len;
        if (count == cap) {
            size_t ncap = cap == 0 ? 4 : cap * 2;
            if (ncap > 1024) {
                rc = CF_LIMIT;
                break;
            }
            cf_active_range *grown = realloc(ranges, ncap * sizeof *grown);
            if (grown == NULL) {
                rc = CF_NOMEM;
                break;
            }
            ranges = grown;
            cap = ncap;
        }
        ranges[count].start = r0;
        ranges[count].end = r1;
        count++;
    }
    free(fields);
    if (rc != CF_OK) {
        free(ranges);
        return rc;
    }
    if (*outcome == CF_ACTIVE_RANGE_INVALID) {
        free(ranges);
        return CF_OK;
    }
    if (unsat || count == 0) {
        free(ranges);
        *outcome = CF_ACTIVE_RANGE_UNSAT;
        return CF_OK;
    }
    *out = ranges;
    *out_n = count;
    *outcome = CF_ACTIVE_RANGE_PART;
    return CF_OK;
}

/* ---- serve planning (send_blob_*, file_server.rs) -------------------------- */

void cf_active_serve_dispose(cf_active_serve *plan) {
    if (plan == NULL) return;
    cf_str_dispose(&plan->content_type);
    cf_str_dispose(&plan->content_disposition);
    cf_str_dispose(&plan->content_range);
    cf_str_dispose(&plan->last_modified);
    cf_str_dispose(&plan->cache_control);
    cf_str_dispose(&plan->allow);
    cf_str_dispose(&plan->boundary);
    free(plan->ranges);
    memset(plan, 0, sizeof *plan);
}

static cf_err serve_set_str(cf_str *field, cf_span text) {
    char *copy = malloc(text.len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (text.len != 0) memcpy(copy, text.ptr, text.len);
    copy[text.len] = '\0';
    field->ptr = copy;
    field->len = text.len;
    return CF_OK;
}

static cf_err serve_headers_for_blob(cf_active_serve *plan, cf_span content_type,
                                     cf_span sanitized_filename,
                                     cf_span disposition_param,
                                     bool has_disposition) {
    cf_err rc = serve_set_str(&plan->content_type,
                              cf_active_content_type_for_serving(content_type));
    if (rc != CF_OK) return rc;
    const char *forced = cf_active_forced_disposition(content_type);
    cf_span kind;
    if (forced != NULL) kind = span_cstr(forced);
    else if (has_disposition) kind = disposition_param;
    else kind = span_cstr("inline");
    cf_builder disp = {0};
    rc = cf_active_content_disposition(kind, sanitized_filename, &disp);
    if (rc == CF_OK) {
        rc = serve_set_str(&plan->content_disposition,
                           (cf_span){disp.ptr, disp.len});
    }
    cf_builder_dispose(&disp);
    return rc;
}

static cf_err serve_content_range(cf_active_serve *plan, uint64_t start,
                                  uint64_t end, uint64_t size) {
    char text[80];
    int n = snprintf(text, sizeof text, "bytes %llu-%llu/%llu",
                     (unsigned long long)start, (unsigned long long)end,
                     (unsigned long long)size);
    if (n <= 0 || (size_t)n >= sizeof text) return CF_INTERNAL;
    return serve_set_str(&plan->content_range, span_cstr(text));
}

cf_err cf_active_proxy_part_heading(cf_span boundary, cf_span content_type,
                                    uint64_t start, uint64_t end,
                                    uint64_t size, cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    char numbers[80];
    int n = snprintf(numbers, sizeof numbers, "%llu-%llu/%llu",
                     (unsigned long long)start, (unsigned long long)end,
                     (unsigned long long)size);
    if (n <= 0 || (size_t)n >= sizeof numbers) return CF_INTERNAL;
    cf_err rc = append_cstr(out, "\r\n--");
    if (rc == CF_OK) rc = cf_builder_append(out, boundary);
    if (rc == CF_OK) rc = append_cstr(out, "\r\nContent-Type: ");
    if (rc == CF_OK) rc = cf_builder_append(out, content_type);
    if (rc == CF_OK) rc = append_cstr(out, "\r\nContent-Range: bytes ");
    if (rc == CF_OK) rc = append_cstr(out, numbers);
    if (rc == CF_OK) rc = append_cstr(out, "\r\n\r\n");
    return rc;
}

cf_err cf_active_proxy_part_trailer(cf_span boundary, cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    cf_err rc = append_cstr(out, "\r\n--");
    if (rc == CF_OK) rc = cf_builder_append(out, boundary);
    if (rc == CF_OK) rc = append_cstr(out, "--\r\n");
    return rc;
}

cf_err cf_active_disk_part_heading(uint64_t start, uint64_t end,
                                   uint64_t size, cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    char numbers[80];
    int n = snprintf(numbers, sizeof numbers, "%llu-%llu/%llu",
                     (unsigned long long)start, (unsigned long long)end,
                     (unsigned long long)size);
    if (n <= 0 || (size_t)n >= sizeof numbers) return CF_INTERNAL;
    cf_err rc = append_cstr(out, "\r\n--" CF_ACTIVE_DISK_MULTIPART_BOUNDARY
                            "\r\ncontent-type: text/plain\r\ncontent-range: bytes ");
    if (rc == CF_OK) rc = append_cstr(out, numbers);
    if (rc == CF_OK) rc = append_cstr(out, "\r\n\r\n");
    return rc;
}

cf_err cf_active_disk_part_trailer(cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    return append_cstr(out, "\r\n--" CF_ACTIVE_DISK_MULTIPART_BOUNDARY "--\r\n");
}

static size_t u64_digits(uint64_t v) {
    size_t n = 1;
    while (v >= 10) {
        v /= 10;
        n++;
    }
    return n;
}

/* Multipart content length: headings + file slices + trailer, exact. */
static uint64_t proxy_multi_length(cf_span boundary, cf_span content_type,
                                   const cf_active_range *ranges, size_t n,
                                   uint64_t size) {
    uint64_t total = 4 + boundary.len + 4; /* "\r\n--" + boundary + "--\r\n" */
    for (size_t i = 0; i < n; i++) {
        /* "\r\n--" (4) + boundary + "\r\nContent-Type: " (16) + ct +
         * "\r\nContent-Range: bytes " (23) + numbers + "\r\n\r\n" (4). */
        total += 4 + boundary.len + 16 + content_type.len + 23 +
                 u64_digits(ranges[i].start) + 1 + u64_digits(ranges[i].end) +
                 1 + u64_digits(size) + 4 +
                 (ranges[i].end - ranges[i].start + 1);
    }
    return total;
}

static uint64_t disk_multi_length(const cf_active_range *ranges, size_t n,
                                    uint64_t size) {
    static const uint64_t heading_fixed =
        sizeof("\r\n--" CF_ACTIVE_DISK_MULTIPART_BOUNDARY
               "\r\ncontent-type: text/plain\r\ncontent-range: bytes ") -
        1 + sizeof("\r\n\r\n") - 1;
    static const uint64_t trailer =
        sizeof("\r\n--" CF_ACTIVE_DISK_MULTIPART_BOUNDARY "--\r\n") - 1;
    uint64_t total = trailer;
    for (size_t i = 0; i < n; i++) {
        total += heading_fixed + u64_digits(ranges[i].start) + 1 +
                 u64_digits(ranges[i].end) + 1 + u64_digits(size) +
                 (ranges[i].end - ranges[i].start + 1);
    }
    return total;
}

cf_err cf_active_proxy_serve(bool file_exists, bool has_range,
                             cf_span range_header, uint64_t file_size,
                             cf_span content_type, cf_span sanitized_filename,
                             cf_span disposition_param, bool has_disposition,
                             cf_span boundary_hex16, bool has_boundary,
                             cf_active_serve *plan) {
    if (plan == NULL) return CF_INVALID;
    memset(plan, 0, sizeof *plan);
    if (!file_exists) {
        plan->status = 404;
        plan->has_body = false;
        return CF_OK;
    }
    cf_err rc = serve_headers_for_blob(plan, content_type, sanitized_filename,
                                       disposition_param, has_disposition);
    if (rc != CF_OK) {
        cf_active_serve_dispose(plan);
        return rc;
    }
    plan->accept_ranges = true;
    bool blank = true;
    if (has_range && range_header.ptr != NULL) {
        for (size_t i = 0; i < range_header.len; i++) {
            if (range_header.ptr[i] != ' ' && range_header.ptr[i] != '\t') {
                blank = false;
                break;
            }
        }
    }
    if (!has_range || blank) {
        plan->status = 200;
        plan->content_length = file_size;
        plan->has_body = file_size != 0;
        return CF_OK;
    }
    cf_active_range *ranges = NULL;
    size_t n = 0;
    cf_active_range_outcome outcome = CF_ACTIVE_RANGE_FULL;
    rc = cf_active_range_parse(range_header, true, file_size, &ranges, &n,
                               &outcome);
    if (rc != CF_OK) {
        cf_active_serve_dispose(plan);
        return rc;
    }
    if (outcome != CF_ACTIVE_RANGE_PART) {
        free(ranges);
        plan->status = 416; /* malformed and unsatisfiable alike */
        plan->content_length = 0;
        plan->has_body = false;
        plan->accept_ranges = true;
        return CF_OK;
    }
    plan->status = 206;
    if (n == 1) {
        plan->has_single = true;
        plan->single = ranges[0];
        free(ranges);
        rc = serve_content_range(plan, plan->single.start, plan->single.end,
                                 file_size);
        if (rc != CF_OK) {
            cf_active_serve_dispose(plan);
            return rc;
        }
        plan->content_length = plan->single.end - plan->single.start + 1;
        plan->has_body = true;
        return CF_OK;
    }
    /* Multipart: the boundary is 16 random bytes as hex (SecureRandom.hex).
     * The caller supplies the randomness; tests pass a fixed value. */
    static const char fallback[33] = "00000000000000000000000000000000";
    cf_span boundary = has_boundary ? boundary_hex16 : span_cstr(fallback);
    rc = serve_set_str(&plan->boundary, boundary);
    if (rc != CF_OK) {
        free(ranges);
        cf_active_serve_dispose(plan);
        return rc;
    }
    char mimetype[160];
    int m = snprintf(mimetype, sizeof mimetype,
                     "multipart/byteranges; boundary=%.*s", (int)boundary.len,
                     boundary.ptr);
    if (m <= 0 || (size_t)m >= sizeof mimetype) {
        free(ranges);
        cf_active_serve_dispose(plan);
        return CF_INTERNAL;
    }
    cf_str_dispose(&plan->content_type);
    rc = serve_set_str(&plan->content_type, span_cstr(mimetype));
    if (rc != CF_OK) {
        free(ranges);
        cf_active_serve_dispose(plan);
        return rc;
    }
    /* Length uses the serving content type (before the multipart override). */
    plan->content_length = proxy_multi_length(
        boundary, cf_active_content_type_for_serving(content_type), ranges, n,
        file_size);
    plan->ranges = ranges;
    plan->n_ranges = n;
    plan->has_body = true;
    return CF_OK;
}

cf_err cf_active_disk_serve(cf_span method, bool has_range,
                            cf_span range_header, bool has_ims,
                            cf_span if_modified_since, bool file_exists,
                            uint64_t file_size, cf_span mtime_httpdate,
                            cf_span content_type_opt,
                            cf_span disposition_value, cf_active_serve *plan) {
    if (plan == NULL) return CF_INVALID;
    memset(plan, 0, sizeof *plan);
    if (span_equal(method, span_cstr("OPTIONS"))) {
        plan->status = 200;
        plan->has_body = false;
        plan->content_length = 0;
        if (serve_set_str(&plan->allow, span_cstr("GET, HEAD, OPTIONS")) != CF_OK) {
            cf_active_serve_dispose(plan);
            return CF_NOMEM;
        }
        return CF_OK;
    }
    if (!file_exists) {
        plan->status = 404;
        plan->has_body = false;
        return CF_OK;
    }
    bool is_head = span_equal(method, span_cstr("HEAD"));
    /* serve_file sets content-type then content-disposition, each defaulting
     * when the disk key carries none; these ride on every status including
     * 304 and 416 (only x-cascade is stripped for 416). */
    cf_err rc = serve_set_str(&plan->cache_control,
                              span_cstr("max-age=3600, public"));
    if (rc == CF_OK) {
        cf_span ctype = (content_type_opt.ptr != NULL && content_type_opt.len != 0)
                            ? content_type_opt
                            : span_cstr("application/octet-stream");
        rc = serve_set_str(&plan->content_type, ctype);
    }
    if (rc == CF_OK) {
        cf_span disp = (disposition_value.ptr != NULL && disposition_value.len != 0)
                           ? disposition_value
                           : span_cstr("attachment");
        rc = serve_set_str(&plan->content_disposition, disp);
    }
    if (rc != CF_OK) {
        cf_active_serve_dispose(plan);
        return rc;
    }
    if (has_ims && if_modified_since.ptr != NULL &&
        span_equal(if_modified_since, mtime_httpdate)) {
        plan->status = 304;
        plan->has_body = false;
        return CF_OK;
    }
    rc = serve_set_str(&plan->last_modified, mtime_httpdate);
    if (rc != CF_OK) {
        cf_active_serve_dispose(plan);
        return rc;
    }
    bool blank = true;
    if (has_range && range_header.ptr != NULL) {
        for (size_t i = 0; i < range_header.len; i++) {
            if (range_header.ptr[i] != ' ' && range_header.ptr[i] != '\t') {
                blank = false;
                break;
            }
        }
        if (blank) has_range = false;
    }
    if (!has_range || range_header.ptr == NULL) {
        plan->status = 200;
        plan->content_length = file_size;
        /* HEAD and empty files carry no body (serving drops the body for
         * HEAD and when size == 0). */
        plan->has_body = !is_head && file_size != 0;
        return CF_OK;
    }
    cf_active_range *ranges = NULL;
    size_t n = 0;
    cf_active_range_outcome outcome = CF_ACTIVE_RANGE_FULL;
    rc = cf_active_range_parse(range_header, true, file_size, &ranges, &n,
                               &outcome);
    if (rc != CF_OK) {
        cf_active_serve_dispose(plan);
        return rc;
    }
    if (outcome == CF_ACTIVE_RANGE_FULL || outcome == CF_ACTIVE_RANGE_INVALID) {
        /* Malformed ranges are ignored here (200), unlike the proxy's 416. */
        free(ranges);
        plan->status = 200;
        plan->content_length = file_size;
        plan->has_body = !is_head && file_size != 0;
        return CF_OK;
    }
    if (outcome == CF_ACTIVE_RANGE_UNSAT) {
        free(ranges);
        /* serve_file strips x-cascade for 416; the stamped content headers
         * stay, plus the byte-range suffix. Last-Modified is a 200/206
         * header and does not ride on the 416. */
        plan->status = 416;
        cf_str_dispose(&plan->last_modified);
        {
            char text[64];
            int m = snprintf(text, sizeof text, "bytes */%llu",
                             (unsigned long long)file_size);
            if (m <= 0 || (size_t)m >= sizeof text) rc = CF_INTERNAL;
            else rc = serve_set_str(&plan->content_range, span_cstr(text));
        }
        if (rc != CF_OK) {
            cf_active_serve_dispose(plan);
            return rc;
        }
        /* The 416 body is CF_ACTIVE_UNSATISFIABLE_MESSAGE; the action writes
         * it when has_body is set. */
        plan->content_length = strlen(CF_ACTIVE_UNSATISFIABLE_MESSAGE);
        plan->has_body = !is_head;
        return CF_OK;
    }
    plan->status = 206;
    if (n == 1) {
        plan->has_single = true;
        plan->single = ranges[0];
        free(ranges);
        rc = serve_content_range(plan, plan->single.start, plan->single.end,
                                 file_size);
        if (rc != CF_OK) {
            cf_active_serve_dispose(plan);
            return rc;
        }
        plan->content_length = plan->single.end - plan->single.start + 1;
        plan->has_body = !is_head;
        return CF_OK;
    }
    rc = serve_set_str(&plan->boundary,
                       span_cstr(CF_ACTIVE_DISK_MULTIPART_BOUNDARY));
    if (rc != CF_OK) {
        free(ranges);
        cf_active_serve_dispose(plan);
        return rc;
    }
    char mimetype[80];
    int m = snprintf(mimetype, sizeof mimetype,
                     "multipart/byteranges; boundary=%s",
                     CF_ACTIVE_DISK_MULTIPART_BOUNDARY);
    if (m <= 0 || (size_t)m >= sizeof mimetype) {
        free(ranges);
        cf_active_serve_dispose(plan);
        return CF_INTERNAL;
    }
    cf_str_dispose(&plan->content_type);
    rc = serve_set_str(&plan->content_type, span_cstr(mimetype));
    if (rc != CF_OK) {
        free(ranges);
        cf_active_serve_dispose(plan);
        return rc;
    }
    plan->content_length = disk_multi_length(ranges, n, file_size);
    plan->ranges = ranges;
    plan->n_ranges = n;
    plan->has_body = !is_head;
    return CF_OK;
}

cf_err cf_active_httpdate(int64_t unix_seconds, char out[30]) {
    if (out == NULL || unix_seconds < 0) return CF_INVALID;
    static const char *const days[] = {"Sun", "Mon", "Tue", "Wed",
                                       "Thu", "Fri", "Sat"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    /* Civil date from days since epoch (Howard Hinnant's algorithm). */
    int64_t days_since = unix_seconds / 86400;
    int64_t tod = unix_seconds % 86400;
    int64_t z = days_since + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2 ? 1 : 0);
    /* Day of week: 1970-01-01 was a Thursday (index 4). */
    int dow = (int)((days_since + 4) % 7);
    if (dow < 0) dow += 7;
    if (y < 1970 || y > 9999) return CF_LIMIT;
    int n = snprintf(out, 30, "%s, %02lld %s %04lld %02lld:%02lld:%02lld GMT",
                     days[dow], (long long)d, months[m - 1], (long long)y,
                     (long long)(tod / 3600), (long long)((tod % 3600) / 60),
                     (long long)(tod % 60));
    if (n <= 0 || n >= 30) return CF_INTERNAL;
    return CF_OK;
}

/* ---- redirect / direct-upload route helpers ------------------------------- */

cf_err cf_active_service_url_path(cf_span secret_key_base, cf_span key,
                                  int64_t expires_us,
                                  cf_span sanitized_filename,
                                  cf_span content_type, bool has_content_type,
                                  cf_span disposition_kind,
                                  cf_span service_name, cf_builder *out) {
    if (out == NULL || (key.len != 0 && key.ptr == NULL)) return CF_INVALID;
    /* content_disposition_with: "attachment" stays, anything else is inline. */
    const char *kind = span_equal(disposition_kind, span_cstr("attachment"))
                           ? "attachment"
                           : "inline";
    cf_builder disp = {0};
    cf_err rc = cf_active_content_disposition(span_cstr(kind), sanitized_filename,
                                              &disp);
    cf_str encoded = {0};
    if (rc == CF_OK) {
        rc = cf_active_disk_key_sign(secret_key_base, key,
                                     (cf_span){disp.ptr, disp.len}, content_type,
                                     has_content_type, service_name, true,
                                     expires_us, &encoded);
    }
    cf_builder_dispose(&disp);
    if (rc != CF_OK) {
        cf_str_dispose(&encoded);
        return rc;
    }
    rc = append_cstr(out, "/rails/active_storage/disk/");
    if (rc == CF_OK)
        rc = cf_active_escape_segment(
            (cf_span){(unsigned char *)encoded.ptr, encoded.len}, out);
    if (rc == CF_OK) rc = append_cstr(out, "/");
    if (rc == CF_OK) rc = cf_active_escape_path(sanitized_filename, out);
    cf_str_dispose(&encoded);
    return rc;
}

cf_err cf_active_blob_redirect_path(cf_span secret_key_base, int64_t blob_id,
                                    cf_span sanitized_filename,
                                    cf_span disposition, bool has_disposition,
                                    cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    cf_str signed_id = {0};
    cf_err rc = cf_active_blob_sign(secret_key_base, blob_id, false, 0, &signed_id);
    if (rc != CF_OK) return rc;
    rc = append_cstr(out, "/rails/active_storage/blobs/redirect/");
    if (rc == CF_OK)
        rc = cf_active_escape_segment(
            (cf_span){(unsigned char *)signed_id.ptr, signed_id.len}, out);
    if (rc == CF_OK) rc = append_cstr(out, "/");
    if (rc == CF_OK) rc = cf_active_escape_path(sanitized_filename, out);
    if (rc == CF_OK && has_disposition) {
        /* `Hash#to_query` escapes with CGI.escape: alnum kept, space -> +,
         * everything else %XX uppercase. Only reached for explicit
         * disposition query values. */
        rc = append_cstr(out, "?disposition=");
        for (size_t i = 0; i < disposition.len && rc == CF_OK; i++) {
            unsigned char c = disposition.ptr[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9')) {
                rc = cf_builder_append(out, (cf_span){&c, 1});
            } else if (c == ' ') {
                rc = append_cstr(out, "+");
            } else {
                char esc[4] = {'%', "0123456789ABCDEF"[c >> 4],
                               "0123456789ABCDEF"[c & 15], '\0'};
                rc = cf_builder_append(out, (cf_span){(unsigned char *)esc, 3});
            }
        }
    }
    cf_str_dispose(&signed_id);
    return rc;
}

cf_active_upload_decision cf_active_direct_upload_check(cf_span byte_size_text,
                                                        bool has_byte_size,
                                                        bool has_filename,
                                                        bool has_checksum,
                                                        int64_t *out_size) {
    if (out_size != NULL) *out_size = 0;
    if (!has_filename || !has_checksum) {
        return CF_ACTIVE_UPLOAD_UNPROCESSABLE;
    }
    int64_t size = 0;
    if (!has_byte_size || byte_size_text.ptr == NULL ||
        !cf_active_integer_cast(byte_size_text, &size)) {
        return CF_ACTIVE_UPLOAD_UNPROCESSABLE;
    }
    if (size < 0 || (uint64_t)size > CF_ACTIVE_UPLOAD_MAX_BYTES) {
        return CF_ACTIVE_UPLOAD_TOO_LARGE;
    }
    if (out_size != NULL) *out_size = size;
    return CF_ACTIVE_UPLOAD_OK;
}

cf_err cf_active_direct_upload_json(int64_t id, cf_span key,
                                    cf_span filename_raw,
                                    cf_span content_type,
                                    bool has_content_type,
                                    cf_span metadata_json,
                                    cf_span service_name, int64_t byte_size,
                                    cf_span checksum, bool has_checksum,
                                    cf_span created_at_iso, cf_span signed_id,
                                    cf_span url,
                                    cf_span upload_content_type,
                                    bool has_upload_content_type,
                                    cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    cf_err rc = append_cstr(out, "{\"id\":");
    if (rc == CF_OK) rc = json_write_int(out, id);
    if (rc == CF_OK) rc = append_cstr(out, ",\"key\":");
    if (rc == CF_OK) rc = json_write_string(out, key);
    if (rc == CF_OK) rc = append_cstr(out, ",\"filename\":");
    if (rc == CF_OK) rc = json_write_string(out, filename_raw);
    if (rc == CF_OK) rc = append_cstr(out, ",\"content_type\":");
    if (rc == CF_OK) {
        rc = has_content_type ? json_write_string(out, content_type)
                              : append_cstr(out, "null");
    }
    if (rc == CF_OK) rc = append_cstr(out, ",\"metadata\":");
    if (rc == CF_OK) rc = cf_builder_append(out, metadata_json);
    if (rc == CF_OK) rc = append_cstr(out, ",\"service_name\":");
    if (rc == CF_OK) rc = json_write_string(out, service_name);
    if (rc == CF_OK) rc = append_cstr(out, ",\"byte_size\":");
    if (rc == CF_OK) rc = json_write_int(out, byte_size);
    if (rc == CF_OK) rc = append_cstr(out, ",\"checksum\":");
    if (rc == CF_OK) {
        rc = has_checksum ? json_write_string(out, checksum)
                          : append_cstr(out, "null");
    }
    if (rc == CF_OK) rc = append_cstr(out, ",\"created_at\":");
    if (rc == CF_OK) rc = json_write_string(out, created_at_iso);
    if (rc == CF_OK) rc = append_cstr(out, ",\"signed_id\":");
    if (rc == CF_OK) rc = json_write_string(out, signed_id);
    if (rc == CF_OK) rc = append_cstr(out, ",\"direct_upload\":{\"url\":");
    if (rc == CF_OK) rc = json_write_string(out, url);
    if (rc == CF_OK) rc = append_cstr(out, ",\"headers\":{\"Content-Type\":");
    if (rc == CF_OK) {
        rc = has_upload_content_type ? json_write_string(out, upload_content_type)
                                     : append_cstr(out, "null");
    }
    if (rc == CF_OK) rc = append_cstr(out, "}}}");
    return rc;
}

/* Stored "YYYY-MM-DD HH:MM:SS[.ffffff]" (UTC) as ActiveSupport::JSON times:
 * ISO 8601 with milliseconds, truncated (not rounded). */
cf_err cf_active_json_time(cf_span db_time, cf_builder *out) {
    if (out == NULL || (db_time.len != 0 && db_time.ptr == NULL))
        return CF_INVALID;
    /* Prefix must be "YYYY-MM-DD HH:MM:SS" with a '.' fraction or end. */
    if (db_time.len < 19) {
        return cf_builder_append(out, db_time);
    }
    for (size_t i = 0; i < 19; i++) {
        char want = (i == 4 || i == 7) ? '-' : (i == 10 ? ' ' : (i == 13 || i == 16 ? ':' : 'd'));
        unsigned char c = db_time.ptr[i];
        bool ok;
        if (want == 'd') ok = c >= '0' && c <= '9';
        else ok = c == (unsigned char)want;
        if (!ok) return cf_builder_append(out, db_time);
    }
    char date[11];
    memcpy(date, db_time.ptr, 10);
    date[10] = '\0';
    char clock[9];
    memcpy(clock, db_time.ptr + 11, 8);
    clock[8] = '\0';
    /* Fraction digits after '.', milliseconds truncated; anything else is
     * not a timestamp the reference would parse, so it passes through. */
    char millis[4] = {'0', '0', '0', '\0'};
    if (db_time.len > 19) {
        if (db_time.ptr[19] != '.') return cf_builder_append(out, db_time);
        for (size_t at = 20; at < db_time.len; at++) {
            if (db_time.ptr[at] < '0' || db_time.ptr[at] > '9')
                return cf_builder_append(out, db_time);
            if (at - 20 < 3) millis[at - 20] = (char)db_time.ptr[at];
        }
    }
    char text[32];
    int n = snprintf(text, sizeof text, "%sT%s.%sZ", date, clock, millis);
    if (n <= 0 || (size_t)n >= sizeof text) return CF_INTERNAL;
    return cf_builder_append(out, span_cstr(text));
}

/* ---- attachments (presenters/attachments.rs) ------------------------------ */

cf_err cf_active_attach_classify(cf_span secret_key_base,
                                 cf_active_attach_input input,
                                 cf_span string_value, int64_t now_us,
                                 cf_active_attach_kind *out_kind,
                                 int64_t *out_blob_id) {
    if (out_kind == NULL || out_blob_id == NULL) return CF_INVALID;
    *out_kind = CF_ACTIVE_ATTACH_INVALID;
    *out_blob_id = 0;
    switch (input) {
    case CF_ACTIVE_SHAPE_ABSENT:
        *out_kind = CF_ACTIVE_ATTACH_UNCHANGED;
        return CF_OK;
    case CF_ACTIVE_SHAPE_NULL:
    case CF_ACTIVE_SHAPE_EMPTY:
        *out_kind = CF_ACTIVE_ATTACH_DELETE;
        return CF_OK;
    case CF_ACTIVE_SHAPE_UPLOAD:
        *out_kind = CF_ACTIVE_ATTACH_UPLOAD;
        return CF_OK;
    case CF_ACTIVE_SHAPE_STRING:
        break;
    default:
        return CF_INVALID;
    }
    /* A plain string is a signed blob reference or invalid: verify first,
     * never decode-then-trust. The action still checks the blob exists. */
    int64_t id = 0;
    bool found = false;
    cf_err rc = cf_active_blob_verify(secret_key_base, string_value, now_us,
                                      &id, &found);
    if (rc != CF_OK) return rc;
    if (!found) {
        *out_kind = CF_ACTIVE_ATTACH_INVALID;
        return CF_OK;
    }
    *out_kind = CF_ACTIVE_ATTACH_SIGNED;
    *out_blob_id = id;
    return CF_OK;
}

bool cf_active_attach_allowed(cf_span record_type, cf_span name) {
    static const struct {
        const char *record_type;
        const char *name;
    } allowed[] = {
        {"User", "avatar"},
        {"Account", "logo"},
        {"Message", "attachment"},
        {"ActiveStorage::VariantRecord", "image"},
        {"ActiveStorage::Blob", "preview_image"},
    };
    for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++) {
        if (span_equal(record_type, span_cstr(allowed[i].record_type)) &&
            span_equal(name, span_cstr(allowed[i].name)))
            return true;
    }
    return false;
}

/* ---- variant races and purge (STORE-03) ----------------------------------- */

cf_err cf_active_variant_claim(void *ctx, cf_span digest,
                               cf_active_variant_insert_fn try_insert,
                               bool *won) {
    if (try_insert == NULL || won == NULL ||
        (digest.len != 0 && digest.ptr == NULL))
        return CF_INVALID;
    *won = false;
    bool inserted = false;
    cf_err rc = try_insert(ctx, digest, &inserted);
    if (rc != CF_OK) return rc;
    *won = inserted;
    return CF_OK;
}

bool cf_active_purge_proceed(size_t attachment_refs) {
    return attachment_refs == 0;
}

cf_err cf_active_purge_files(cf_storage *storage, cf_span key, bool is_image) {
    if (storage == NULL) return CF_INVALID;
    if (!cf_storage_key_valid(key)) return CF_INVALID;
    /* S01 deletion already maps missing files to success and permission/I-O
     * failures to CF_IO; only validated keys reach the filesystem. */
    cf_err rc = cf_storage_delete(storage, key);
    if (rc == CF_OK && is_image) rc = cf_storage_delete_variants(storage, key);
    return rc;
}

/* ---- S03 boundary (fail loudly, never approximate) ------------------------ */

cf_err cf_active_representation_process(cf_span content_type,
                                        bool has_transformations) {
    (void)content_type;
    (void)has_transformations;
    /* Variant, preview and analysis transforms need the S03 media workers
     * (pinned vips 8.16.1 / ffmpeg 7.1.5 argv builders over the S01 process
     * boundary). No route may serve an approximated transform meanwhile. */
    return CF_INTERNAL;
}











