/* K01c cache-key encoding, weak-ETag and admission decision (src/cache_key.h
 * has the field list and the rules). Deterministic, allocation-free except
 * for the caller's builder: the same fields always produce the same bytes, so
 * a key built at lookup can be reused at admission. */
#include "cache_key.h"

#include <openssl/evp.h>

#include <stdint.h>
#include <string.h>

static cf_err key_u8(cf_builder *out, unsigned value) {
    unsigned char byte = (unsigned char)(value & 0xffu);
    return cf_builder_append(out, (cf_span){&byte, 1});
}

static cf_err key_u32(cf_builder *out, uint32_t value) {
    unsigned char bytes[4];
    for (size_t i = 0; i < 4; i++) {
        bytes[i] = (unsigned char)((value >> (8 * i)) & 0xffu);
    }
    return cf_builder_append(out, (cf_span){bytes, 4});
}

static cf_err key_u64(cf_builder *out, uint64_t value) {
    unsigned char bytes[8];
    for (size_t i = 0; i < 8; i++) {
        bytes[i] = (unsigned char)((value >> (8 * i)) & 0xffu);
    }
    return cf_builder_append(out, (cf_span){bytes, 8});
}

/* u32 length + raw bytes; an absent span is a present empty string. */
static cf_err key_str(cf_builder *out, cf_span value) {
    if (value.len > UINT32_MAX) return CF_INVALID;
    cf_err rc = key_u32(out, (uint32_t)value.len);
    if (rc != CF_OK) return rc;
    if (value.len == 0) return CF_OK;
    if (value.ptr == NULL) return CF_INVALID;
    return cf_builder_append(out, value);
}

cf_err cf_cache_key_build(const cf_cache_key_fields *fields, cf_builder *out) {
    if (fields == NULL || out == NULL) return CF_INVALID;
    if (fields->target.len != 0 && fields->target.ptr == NULL) return CF_INVALID;
    if (fields->turbo_frame.len != 0 && fields->turbo_frame.ptr == NULL) {
        return CF_INVALID;
    }
    if (fields->user_agent.len != 0 && fields->user_agent.ptr == NULL) {
        return CF_INVALID;
    }
    if (fields->public_origin.len != 0 && fields->public_origin.ptr == NULL) {
        return CF_INVALID;
    }
    if (fields->format_symbol == NULL) return CF_INVALID;
    if (fields->encoding != CF_ENC_IDENTITY && fields->encoding != CF_ENC_GZIP) {
        return CF_INVALID;
    }

    cf_err rc;
#define KEY_TRY(expr)      \
    do {                   \
        rc = (expr);       \
        if (rc != CF_OK) return rc; \
    } while (0)

    /* Field 1: route ID (HEAD uses the GET row's ID), data version, user ID. */
    KEY_TRY(key_u8(out, CF_CACHE_KEY_VERSION));
    KEY_TRY(key_u32(out, fields->route_id));
    KEY_TRY(key_u64(out, fields->data_version));
    KEY_TRY(key_u8(out, fields->has_user_id ? 1u : 0u));
    if (fields->has_user_id) {
        KEY_TRY(key_u64(out, (uint64_t)fields->user_id));
    }

    /* Field 2: the raw origin-form target, complete raw query included. */
    KEY_TRY(key_str(out, fields->target));

    /* Field 3: negotiated format and the selected representation coding. */
    KEY_TRY(key_str(out, (cf_span){
                           (const unsigned char *)fields->format_symbol,
                           strlen(fields->format_symbol)}));
    KEY_TRY(key_u8(out, (unsigned)fields->encoding));

    /* Field 4: exact Turbo-Frame value or the absent marker. */
    KEY_TRY(key_u8(out, fields->has_turbo_frame ? 1u : 0u));
    if (fields->has_turbo_frame) KEY_TRY(key_str(out, fields->turbo_frame));

    /* Field 5: the last_room cookie's parsed optional ID. */
    KEY_TRY(key_u8(out, fields->has_last_room ? 1u : 0u));
    if (fields->has_last_room) {
        KEY_TRY(key_u64(out, (uint64_t)fields->last_room_id));
    }

    /* Field 6: the exact User-Agent value. */
    KEY_TRY(key_u8(out, fields->has_user_agent ? 1u : 0u));
    if (fields->has_user_agent) KEY_TRY(key_str(out, fields->user_agent));

    /* Field 7: the configured PUBLIC_ORIGIN. */
    KEY_TRY(key_str(out, fields->public_origin));

#undef KEY_TRY
    return CF_OK;
}

cf_err cf_cache_etag(cf_span identity, char out[69]) {
    if (out == NULL) return CF_INVALID;
    if (identity.len != 0 && identity.ptr == NULL) return CF_INVALID;
    static const unsigned char empty = 0;
    const unsigned char *data =
        identity.len != 0 ? identity.ptr : &empty;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_Digest(data, identity.len, digest, &digest_len, EVP_sha256(),
                   NULL) != 1 ||
        digest_len != 32) {
        return CF_INTERNAL;
    }
    static const char hex[] = "0123456789abcdef";
    out[0] = 'W';
    out[1] = '/';
    out[2] = '"';
    for (size_t i = 0; i < 32; i++) {
        out[3 + 2 * i] = hex[(digest[i] >> 4) & 0xf];
        out[4 + 2 * i] = hex[digest[i] & 0xf];
    }
    out[67] = '"';
    out[68] = '\0';
    return CF_OK;
}

/* HeaderValue::to_str (http 1.5.0) accepts only HTAB and visible ASCII. */
static bool header_readable(cf_span value) {
    if (value.len != 0 && value.ptr == NULL) return false;
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (!(c == '\t' || (c >= 0x20 && c <= 0x7e))) return false;
    }
    return true;
}

bool cf_cache_if_none_match(cf_span header_value, const char *weak_etag) {
    if (weak_etag == NULL) return false;
    if (header_value.len == 0) return false;
    if (!header_readable(header_value)) return false;
    size_t etag_len = strlen(weak_etag);

    size_t at = 0;
    while (at <= header_value.len) {
        size_t end = at;
        while (end < header_value.len && header_value.ptr[end] != ',') end++;
        size_t start = at, stop = end;
        while (start < stop &&
               (header_value.ptr[start] == ' ' || header_value.ptr[start] == '\t')) {
            start++;
        }
        while (stop > start && (header_value.ptr[stop - 1] == ' ' ||
                                header_value.ptr[stop - 1] == '\t')) {
            stop--;
        }
        cf_span token = {header_value.ptr + start, stop - start};
        bool star = token.len == 1 && token.ptr[0] == '*';
        if (star || (token.len == etag_len &&
                     memcmp(token.ptr, weak_etag, etag_len) == 0)) {
            return true;
        }
        if (end == header_value.len) break;
        at = end + 1;
    }
    return false;
}

static const char *const cf_cache_decision_names[] = {
    [CF_CACHE_ADMIT] = "admit",
    [CF_CACHE_BYPASS_DISABLED] = "disabled",
    [CF_CACHE_BYPASS_ROUTE] = "route",
    [CF_CACHE_BYPASS_METHOD] = "method",
    [CF_CACHE_BYPASS_STATUS] = "status",
    [CF_CACHE_BYPASS_BODY] = "body",
    [CF_CACHE_BYPASS_FLASH] = "flash",
    [CF_CACHE_BYPASS_BOT] = "bot",
    [CF_CACHE_BYPASS_ENCODING] = "content-encoding",
    [CF_CACHE_BYPASS_NO_TRANSFORM] = "no-transform",
};

const char *cf_cache_decision_name(cf_cache_decision decision) {
    if ((size_t)decision >=
        sizeof cf_cache_decision_names / sizeof cf_cache_decision_names[0]) {
        return "?";
    }
    const char *name = cf_cache_decision_names[decision];
    return name != NULL ? name : "?";
}

cf_cache_decision cf_cache_admit_decide(const cf_cache_admit_input *in) {
    if (in == NULL) return CF_CACHE_BYPASS_DISABLED;
    if (!in->cache_enabled) return CF_CACHE_BYPASS_DISABLED;
    if (!in->route_admitted) return CF_CACHE_BYPASS_ROUTE;
    if (!in->method_get) return CF_CACHE_BYPASS_METHOD;
    if (in->status != 200) return CF_CACHE_BYPASS_STATUS;
    if (!in->body_buffer) return CF_CACHE_BYPASS_BODY;
    if (in->flash_present) return CF_CACHE_BYPASS_FLASH;
    if (in->bot) return CF_CACHE_BYPASS_BOT;
    if (in->action_content_encoding) return CF_CACHE_BYPASS_ENCODING;
    if (in->no_transform) return CF_CACHE_BYPASS_NO_TRANSFORM;
    return CF_CACHE_ADMIT;
}

bool cf_cache_representation_eligible(unsigned status, bool has_buffered_body) {
    if (!has_buffered_body) return false;
    if (status == 204 || status == 304) return false;
    if (status < 200 || status > 599) return false;
    return true;
}
