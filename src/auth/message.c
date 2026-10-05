/* src/auth/message.c — ActiveSupport::MessageVerifier / MessageEncryptor and
 * their metadata envelopes (rails_compat message_verifier.rs,
 * message_encryptor.rs, metadata.rs).  The current JSON formats only: the
 * legacy Marshal fallback is intentionally absent (D-C01).
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

void auth_value_dispose(auth_value *value) {
    if (value == NULL) return;
    if (value->payload != NULL) cf_buf_release(value->payload);
    if (value->doc != NULL) yyjson_doc_free(value->doc);
    memset(value, 0, sizeof *value);
}

/* --- envelope serialization ------------------------------------------------- */

cf_err auth_metadata_serialize(cf_auth_serializer serializer, cf_span dumped,
                               cf_span purpose, bool has_purpose,
                               bool has_expiry, int64_t expires_us,
                               cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    if (!has_purpose && !has_expiry) {
        return cf_builder_append(out, dumped);
    }
    char expiry[32];
    if (has_expiry && auth_iso8601_millis(expires_us, expiry) != CF_OK) {
        return CF_INVALID;
    }
    cf_err rc;
    if (serializer == CF_AUTH_SERIALIZER_NULL) {
        /* The legacy envelope always carries exp and pur, null when unset. */
        rc = cf_builder_append(out, auth_cstr_span("{\"_rails\":{\"message\":"));
        cf_str encoded = {0};
        if (rc == CF_OK) rc = auth_base64_encode(dumped, CF_AUTH_ENCODING_STRICT, &encoded);
        if (rc == CF_OK) rc = auth_json_string_encode(out, (cf_span){(const unsigned char *)encoded.ptr, encoded.len});
        cf_str_dispose(&encoded);
        if (rc == CF_OK) rc = cf_builder_append(out, auth_cstr_span(",\"exp\":"));
        if (rc == CF_OK) {
            if (has_expiry) rc = auth_json_string_encode(out, auth_cstr_span(expiry));
            else rc = cf_builder_append(out, auth_cstr_span("null"));
        }
        if (rc == CF_OK) rc = cf_builder_append(out, auth_cstr_span(",\"pur\":"));
        if (rc == CF_OK) {
            if (has_purpose) rc = auth_json_string_encode(out, purpose);
            else rc = cf_builder_append(out, auth_cstr_span("null"));
        }
        if (rc == CF_OK) rc = cf_builder_append(out, auth_cstr_span("}}"));
        return rc;
    }
    rc = cf_builder_append(out, auth_cstr_span("{\"_rails\":{\"data\":"));
    if (rc == CF_OK) rc = cf_builder_append(out, dumped);
    if (rc == CF_OK && has_expiry) {
        rc = cf_builder_append(out, auth_cstr_span(",\"exp\":"));
        if (rc == CF_OK) rc = auth_json_string_encode(out, auth_cstr_span(expiry));
    }
    if (rc == CF_OK && has_purpose) {
        rc = cf_builder_append(out, auth_cstr_span(",\"pur\":"));
        if (rc == CF_OK) rc = auth_json_string_encode(out, purpose);
    }
    if (rc == CF_OK) rc = cf_builder_append(out, auth_cstr_span("}}"));
    return rc;
}

/* --- envelope extraction ---------------------------------------------------- */

/* `Object#to_s` for the JSON values a purpose could hold; arrays/hashes never
 * match a purpose this port compares against. */
static bool auth_purpose_matches(yyjson_val *pur, cf_span purpose,
                                 bool has_purpose) {
    cf_span expected = has_purpose ? purpose : (cf_span){NULL, 0};
    if (pur == NULL || yyjson_is_null(pur)) {
        return expected.len == 0;
    }
    if (yyjson_is_str(pur)) {
        cf_span actual = {(const unsigned char *)yyjson_get_str(pur),
                          yyjson_get_len(pur)};
        return auth_span_equal(actual, expected);
    }
    if (yyjson_is_bool(pur)) {
        cf_span actual = auth_cstr_span(yyjson_get_bool(pur) ? "true" : "false");
        return auth_span_equal(actual, expected);
    }
    /* Numbers to_s; no purpose in this port compares equal to one. */
    return false;
}

static auth_meta_status auth_metadata_extract(yyjson_val *rails,
                                              cf_span purpose, bool has_purpose,
                                              int64_t now_us) {
    if (rails == NULL || !yyjson_is_obj(rails)) return AUTH_META_INVALID;
    yyjson_val *exp = yyjson_obj_get(rails, "exp");
    if (exp != NULL && !yyjson_is_null(exp)) {
        if (!yyjson_is_str(exp)) return AUTH_META_INVALID;
        cf_span text = {(const unsigned char *)yyjson_get_str(exp),
                        yyjson_get_len(exp)};
        int64_t exp_ns = 0;
        if (auth_iso8601_parse_ns(text, &exp_ns) != CF_OK) {
            return AUTH_META_INVALID;
        }
        if (now_us * INT64_C(1000) >= exp_ns) return AUTH_META_EXPIRED;
    }
    if (!auth_purpose_matches(yyjson_obj_get(rails, "pur"), purpose,
                              has_purpose)) {
        return AUTH_META_PURPOSE;
    }
    return AUTH_META_OK;
}

/* serializer.load() of the dumped bytes. */
static auth_meta_status auth_serializer_load(cf_auth_serializer serializer,
                                             cf_span bytes,
                                             auth_value *out) {
    if (serializer == CF_AUTH_SERIALIZER_NULL) {
        if (!auth_utf8_valid(bytes)) return AUTH_META_INVALID;
        if (cf_buf_copy(bytes, &out->payload) != CF_OK) return AUTH_META_INVALID;
        out->doc = NULL;
        out->root = NULL;
        return AUTH_META_OK;
    }
    if (serializer == CF_AUTH_SERIALIZER_JSON_FALLBACK && bytes.len >= 2 &&
        bytes.ptr[0] == 0x04 && bytes.ptr[1] == 0x08) {
        /* Marshal payload: not a format this port reads (D-C01). */
        return AUTH_META_INVALID;
    }
    const char *text = bytes.len == 0 ? "null" : (const char *)bytes.ptr;
    size_t text_len = bytes.len == 0 ? 4 : bytes.len;
    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_opts((char *)text, text_len, 0, NULL, &err);
    if (doc == NULL) return AUTH_META_INVALID;
    out->doc = doc;
    out->root = yyjson_doc_get_root(doc);
    if (bytes.len != 0 && cf_buf_copy(bytes, &out->payload) != CF_OK) {
        return AUTH_META_INVALID;
    }
    return AUTH_META_OK;
}

auth_meta_status auth_metadata_deserialize(cf_auth_serializer serializer,
                                           cf_span bytes, cf_span purpose,
                                           bool has_purpose, int64_t now_us,
                                           bool strict_message,
                                           auth_value *out) {
    memset(out, 0, sizeof *out);
    out->serializer = serializer;
    static const char legacy_prefix[] = "{\"_rails\":{\"message\":";
    bool legacy = bytes.len >= sizeof legacy_prefix - 1 &&
                  memcmp(bytes.ptr, legacy_prefix,
                         sizeof legacy_prefix - 1) == 0;
    if (legacy) {
        yyjson_read_err err;
        yyjson_doc *doc =
            yyjson_read_opts((char *)bytes.ptr, bytes.len, 0, NULL, &err);
        if (doc == NULL) return AUTH_META_INVALID;
        yyjson_val *rails = yyjson_obj_get(yyjson_doc_get_root(doc), "_rails");
        auth_meta_status status =
            auth_metadata_extract(rails, purpose, has_purpose, now_us);
        if (status != AUTH_META_OK) {
            yyjson_doc_free(doc);
            return status;
        }
        yyjson_val *message = yyjson_obj_get(rails, "message");
        if (message == NULL || !yyjson_is_str(message)) {
            yyjson_doc_free(doc);
            return AUTH_META_INVALID;
        }
        cf_span encoded = {(const unsigned char *)yyjson_get_str(message),
                           yyjson_get_len(message)};
        cf_buf *decoded = NULL;
        cf_err rc = strict_message
                        ? auth_base64_strict_decode(encoded, &decoded)
                        : auth_base64_urlsafe_decode(encoded, &decoded);
        auth_meta_status result = AUTH_META_INVALID;
        if (rc == CF_OK) {
            result = auth_serializer_load(serializer, cf_buf_span(decoded), out);
        }
        if (decoded != NULL) cf_buf_release(decoded);
        yyjson_doc_free(doc);
        if (result != AUTH_META_OK) auth_value_dispose(out);
        else out->found = true;
        return result;
    }

    auth_meta_status status = auth_serializer_load(serializer, bytes, out);
    if (status != AUTH_META_OK) {
        auth_value_dispose(out);
        return status;
    }
    if (out->doc != NULL) {
        yyjson_val *root = out->root;
        yyjson_val *rails =
            yyjson_is_obj(root) ? yyjson_obj_get(root, "_rails") : NULL;
        if (rails != NULL && yyjson_is_obj(rails)) {
            status = auth_metadata_extract(rails, purpose, has_purpose, now_us);
            if (status != AUTH_META_OK) {
                auth_value_dispose(out);
                return status;
            }
            yyjson_val *data = yyjson_obj_get(rails, "data");
            out->root = data; /* may be NULL: extract returns null when absent */
            out->found = true;
            return AUTH_META_OK;
        }
    }
    if (has_purpose) {
        auth_value_dispose(out);
        return AUTH_META_PURPOSE;
    }
    out->found = true;
    return AUTH_META_OK;
}

/* --- verifier --------------------------------------------------------------- */

static size_t auth_digest_hex_length(cf_auth_digest digest) {
    return digest == CF_AUTH_DIGEST_SHA256 ? 64 : 40;
}

cf_err auth_hmac_hex(cf_auth_digest digest, cf_span key, cf_span data,
                     cf_str *out) {
    unsigned char mac[64];
    size_t mac_len = 0;
    cf_err rc = auth_hmac(digest, key, data, mac, &mac_len);
    if (rc != CF_OK) return rc;
    return auth_hex_encode((cf_span){mac, mac_len}, out);
}

/* extract_encoded: the digest is the last 2*len(hex) characters, preceded by
 * "--"; both sides must be non-blank and the HMAC must match constant-time. */
static bool auth_extract_encoded(cf_auth_digest digest, cf_span key,
                                 cf_span message, cf_span *out_encoded) {
    size_t hex_len = auth_digest_hex_length(digest);
    if (message.len < hex_len + 2) return false;
    size_t index = message.len - (hex_len + 2);
    if (message.ptr[index] != '-' || message.ptr[index + 1] != '-') {
        return false;
    }
    cf_span encoded = {message.ptr, index};
    cf_span got = {message.ptr + index + 2, hex_len};
    size_t begin = 0;
    while (begin < encoded.len &&
           (encoded.ptr[begin] == ' ' || encoded.ptr[begin] == '\t' ||
            encoded.ptr[begin] == '\n' || encoded.ptr[begin] == '\r')) {
        begin++;
    }
    size_t end = encoded.len;
    while (end > begin && (encoded.ptr[end - 1] == ' ' ||
                           encoded.ptr[end - 1] == '\t' ||
                           encoded.ptr[end - 1] == '\n' ||
                           encoded.ptr[end - 1] == '\r')) {
        end--;
    }
    if (begin == end) return false;
    cf_str want = {0};
    if (auth_hmac_hex(digest, key, encoded, &want) != CF_OK) return false;
    bool match = auth_constant_time_equal(got, (cf_span){(const unsigned char *)want.ptr, want.len});
    cf_str_dispose(&want);
    if (!match) return false;
    *out_encoded = encoded;
    return true;
}

cf_err cf_auth_verifier_generate(cf_span key, const cf_auth_verifier_config *config,
                                 cf_span data, cf_span purpose, bool has_purpose,
                                 bool has_expiry, int64_t expires_us,
                                 cf_str *out) {
    if (out == NULL || config == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    cf_builder serialized = {0};
    cf_err rc = auth_metadata_serialize(config->serializer, data, purpose,
                                        has_purpose, has_expiry, expires_us,
                                        &serialized);
    if (rc != CF_OK) {
        cf_builder_dispose(&serialized);
        return rc;
    }
    cf_span plain = {serialized.ptr, serialized.len};
    cf_str encoded = {0};
    rc = auth_base64_encode(plain, config->encoding, &encoded);
    if (rc == CF_OK) {
        cf_str hex = {0};
        rc = auth_hmac_hex(config->digest, key,
                           (cf_span){(const unsigned char *)encoded.ptr,
                                     encoded.len},
                           &hex);
        if (rc == CF_OK) {
            cf_builder message = {0};
            rc = cf_builder_append(&message,
                                   (cf_span){(const unsigned char *)encoded.ptr,
                                             encoded.len});
            if (rc == CF_OK) rc = cf_builder_append(&message, auth_cstr_span("--"));
            if (rc == CF_OK) {
                rc = cf_builder_append(
                    &message,
                    (cf_span){(const unsigned char *)hex.ptr, hex.len});
            }
            if (rc == CF_OK) {
                cf_buf *frozen = NULL;
                rc = cf_builder_freeze(&message, &frozen);
                if (rc == CF_OK) {
                    cf_span span = cf_buf_span(frozen);
                    rc = auth_str_dup(span, out);
                    cf_buf_release(frozen);
                }
            }
            cf_builder_dispose(&message);
            cf_str_dispose(&hex);
        }
        cf_str_dispose(&encoded);
    }
    cf_builder_dispose(&serialized);
    return rc;
}

cf_err cf_auth_verifier_verify(cf_span key, const cf_auth_verifier_config *config,
                               cf_span message, cf_span purpose, bool has_purpose,
                               int64_t now_us, cf_str *out_json, bool *found) {
    if (out_json == NULL || found == NULL || config == NULL) return CF_INVALID;
    memset(out_json, 0, sizeof *out_json);
    *found = false;
    cf_span encoded = {0};
    if (!auth_extract_encoded(config->digest, key, message, &encoded)) {
        return CF_OK;
    }
    cf_buf *bytes = NULL;
    cf_err rc = auth_base64_urlsafe_decode(encoded, &bytes);
    if (rc == CF_NOMEM) return CF_NOMEM;
    if (rc != CF_OK) {
        /* A non-Base64 payload under a valid HMAC is InvalidSignature in the
         * reference (`read_message`: `urlsafe_decode(encoded).ok_or(...)`),
         * so it reads as unauthenticated like a bad digest; malformed input
         * leaves *found=false with CF_OK (auth.h). */
        return CF_OK;
    }
    auth_value value;
    auth_meta_status status = auth_metadata_deserialize(
        config->serializer, cf_buf_span(bytes), purpose, has_purpose, now_us,
        false, &value);
    cf_buf_release(bytes);
    if (status != AUTH_META_OK) return CF_OK;
    if (config->serializer == CF_AUTH_SERIALIZER_NULL) {
        rc = auth_str_dup(cf_buf_span(value.payload), out_json);
    } else if (value.root != NULL) {
        rc = auth_json_write_value(
            value.root, config->serializer == CF_AUTH_SERIALIZER_JSON_FALLBACK,
            out_json);
    } else {
        rc = auth_str_dup(auth_cstr_span("null"), out_json);
    }
    auth_value_dispose(&value);
    if (rc != CF_OK) return rc;
    *found = true;
    return CF_OK;
}

/* --- encryptor -------------------------------------------------------------- */

cf_err cf_auth_encryptor_encrypt(cf_span key32, cf_span dumped, cf_span purpose,
                                 bool has_purpose, bool has_expiry,
                                 int64_t expires_us, cf_str *out) {
    if (out == NULL || key32.len != 32) return CF_INVALID;
    memset(out, 0, sizeof *out);
    cf_builder plaintext = {0};
    cf_err rc = auth_metadata_serialize(CF_AUTH_SERIALIZER_NULL, dumped, purpose,
                                        has_purpose, has_expiry, expires_us,
                                        &plaintext);
    unsigned char iv[12];
    unsigned char tag[16];
    cf_buf *ciphertext = NULL;
    if (rc == CF_OK) rc = cf_random_bytes(iv, sizeof iv);
    if (rc == CF_OK) {
        rc = auth_aes256gcm_encrypt(
            key32, (cf_span){iv, sizeof iv},
            (cf_span){plaintext.ptr, plaintext.len}, &ciphertext, tag);
    }
    if (rc == CF_OK) {
        cf_str ct_b64 = {0}, iv_b64 = {0}, tag_b64 = {0};
        rc = auth_base64_encode(cf_buf_span(ciphertext), CF_AUTH_ENCODING_STRICT,
                                &ct_b64);
        if (rc == CF_OK) {
            rc = auth_base64_encode((cf_span){iv, sizeof iv},
                                    CF_AUTH_ENCODING_STRICT, &iv_b64);
        }
        if (rc == CF_OK) {
            rc = auth_base64_encode((cf_span){tag, sizeof tag},
                                    CF_AUTH_ENCODING_STRICT, &tag_b64);
        }
        if (rc == CF_OK) {
            cf_builder joined = {0};
            rc = cf_builder_append(&joined, (cf_span){(const unsigned char *)ct_b64.ptr, ct_b64.len});
            if (rc == CF_OK) rc = cf_builder_append(&joined, auth_cstr_span("--"));
            if (rc == CF_OK) rc = cf_builder_append(&joined, (cf_span){(const unsigned char *)iv_b64.ptr, iv_b64.len});
            if (rc == CF_OK) rc = cf_builder_append(&joined, auth_cstr_span("--"));
            if (rc == CF_OK) rc = cf_builder_append(&joined, (cf_span){(const unsigned char *)tag_b64.ptr, tag_b64.len});
            if (rc == CF_OK) {
                cf_buf *frozen = NULL;
                rc = cf_builder_freeze(&joined, &frozen);
                if (rc == CF_OK) {
                    rc = auth_str_dup(cf_buf_span(frozen), out);
                    cf_buf_release(frozen);
                }
            }
            cf_builder_dispose(&joined);
        }
        cf_str_dispose(&ct_b64);
        cf_str_dispose(&iv_b64);
        cf_str_dispose(&tag_b64);
    }
    if (ciphertext != NULL) cf_buf_release(ciphertext);
    cf_builder_dispose(&plaintext);
    return rc;
}

/* extract_parts: fixed-length IV and tag at the end, each preceded by "--". */
static bool auth_encryptor_extract_parts(cf_span message, cf_span *ct,
                                         cf_span *iv, cf_span *tag) {
    if (message.len < 24 + 2 + 16 + 2) return false;
    size_t tag_start = message.len - 24;
    size_t iv_start = tag_start - 2 - 16;
    size_t ct_end = iv_start - 2;
    if (message.ptr[tag_start - 2] != '-' ||
        message.ptr[tag_start - 1] != '-' ||
        message.ptr[ct_end] != '-' || message.ptr[ct_end + 1] != '-') {
        return false;
    }
    *ct = (cf_span){message.ptr, ct_end};
    *iv = (cf_span){message.ptr + iv_start, 16};
    *tag = (cf_span){message.ptr + tag_start, 24};
    return true;
}

cf_err cf_auth_encryptor_decrypt(cf_span key32, cf_span message, cf_span purpose,
                                 bool has_purpose, int64_t now_us,
                                 cf_str *out_json, bool *found) {
    if (out_json == NULL || found == NULL) return CF_INVALID;
    memset(out_json, 0, sizeof *out_json);
    *found = false;
    if (key32.len != 32) return CF_INVALID;
    cf_span ct = {0}, iv = {0}, tag = {0};
    if (!auth_encryptor_extract_parts(message, &ct, &iv, &tag)) return CF_OK;
    cf_buf *ct_bytes = NULL, *iv_bytes = NULL, *tag_bytes = NULL;
    cf_err rc = auth_base64_strict_decode(ct, &ct_bytes);
    if (rc == CF_OK) rc = auth_base64_strict_decode(iv, &iv_bytes);
    if (rc == CF_OK) rc = auth_base64_strict_decode(tag, &tag_bytes);
    if (rc == CF_NOMEM) {
        if (ct_bytes != NULL) cf_buf_release(ct_bytes);
        if (iv_bytes != NULL) cf_buf_release(iv_bytes);
        if (tag_bytes != NULL) cf_buf_release(tag_bytes);
        return CF_NOMEM;
    }
    if (rc != CF_OK) {
        /* Malformed encoding reads as unauthenticated, like the reference. */
        if (ct_bytes != NULL) cf_buf_release(ct_bytes);
        if (iv_bytes != NULL) cf_buf_release(iv_bytes);
        if (tag_bytes != NULL) cf_buf_release(tag_bytes);
        return CF_OK;
    }
    cf_buf *plaintext = NULL;
    if (cf_buf_span(iv_bytes).len == 12 && cf_buf_span(tag_bytes).len == 16) {
        (void)auth_aes256gcm_decrypt(
            key32, cf_buf_span(iv_bytes), cf_buf_span(ct_bytes),
            cf_buf_span(tag_bytes), &plaintext);
    }
    if (plaintext != NULL) {
        auth_value value;
        auth_meta_status status = auth_metadata_deserialize(
            CF_AUTH_SERIALIZER_NULL, cf_buf_span(plaintext), purpose,
            has_purpose, now_us, true, &value);
        if (status == AUTH_META_OK) {
            rc = auth_str_dup(cf_buf_span(value.payload), out_json);
            if (rc == CF_OK) *found = true;
            auth_value_dispose(&value);
        }
    }
    if (plaintext != NULL) cf_buf_release(plaintext);
    if (ct_bytes != NULL) cf_buf_release(ct_bytes);
    if (iv_bytes != NULL) cf_buf_release(iv_bytes);
    if (tag_bytes != NULL) cf_buf_release(tag_bytes);
    return rc;
}
