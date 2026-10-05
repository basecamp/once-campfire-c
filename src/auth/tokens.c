/* src/auth/tokens.c — the named current-format token constructors:
 * cookies.signed/encrypted (cookies.rs), ActiveRecord::SignedId
 * (signed_id.rs), Rails.application.message_verifier (lib.rs app_verifier),
 * GlobalID/SignedGlobalID (global_id.rs) and Turbo stream names (turbo.rs).
 */
#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- cookie jars ------------------------------------------------------------ */

/* Rack raises for a signed/encrypted value whose name+value exceed 4096 bytes;
 * Rails does not rescue CookieOverflow, so it is a 500 in the reference. */
cf_err auth_check_cookie_overflow(cf_span name, cf_span wire) {
    const size_t max = 4096;
    if (name.len > max || wire.len > max - name.len) {
        return CF_INTERNAL;
    }
    return CF_OK;
}

cf_err auth_signed_cookie_key(cf_span secret_key_base, unsigned char *out64) {
    return auth_pbkdf2_sha256(secret_key_base,
                              auth_cstr_span("signed cookie"), 64, out64);
}

cf_err auth_encrypted_cookie_key(cf_span secret_key_base, unsigned char *out32) {
    return auth_pbkdf2_sha256(secret_key_base,
                              auth_cstr_span("authenticated encrypted cookie"),
                              32, out32);
}

static cf_err auth_cookie_purpose(cf_span name, cf_builder *out) {
    cf_err rc = cf_builder_append(out, auth_cstr_span("cookie."));
    if (rc == CF_OK) rc = cf_builder_append(out, name);
    return rc;
}

cf_err cf_auth_signed_cookie_generate(cf_span secret_key_base, cf_span name,
                                      cf_span value, bool has_expiry,
                                      int64_t expires_us, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    unsigned char key[64];
    cf_err rc = auth_signed_cookie_key(secret_key_base, key);
    cf_builder dumped = {0};
    cf_builder purpose = {0};
    if (rc == CF_OK) rc = auth_json_string_encode(&dumped, value);
    if (rc == CF_OK) rc = auth_cookie_purpose(name, &purpose);
    if (rc == CF_OK) {
        const cf_auth_verifier_config config = {
            .digest = CF_AUTH_DIGEST_SHA1,
            .encoding = CF_AUTH_ENCODING_STRICT,
            .serializer = CF_AUTH_SERIALIZER_NULL};
        rc = cf_auth_verifier_generate(
            (cf_span){key, sizeof key}, &config,
            (cf_span){dumped.ptr, dumped.len},
            (cf_span){purpose.ptr, purpose.len}, true, has_expiry, expires_us,
            out);
    }
    cf_builder_dispose(&dumped);
    cf_builder_dispose(&purpose);
    return rc;
}

/* The signed jar reads Value::String only; anything else is nil. */
static bool auth_json_string_value(cf_span json, cf_str *out) {
    yyjson_read_err err;
    yyjson_doc *doc =
        yyjson_read_opts((char *)json.ptr, json.len, 0, NULL, &err);
    if (doc == NULL) return false;
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool ok = false;
    if (yyjson_is_str(root)) {
        ok = auth_str_dup((cf_span){(const unsigned char *)yyjson_get_str(root),
                                    yyjson_get_len(root)},
                          out) == CF_OK;
    }
    yyjson_doc_free(doc);
    return ok;
}

cf_err cf_auth_signed_cookie_verify(cf_span secret_key_base, cf_span name,
                                    cf_span raw, int64_t now_us,
                                    cf_str *out_value, bool *found) {
    if (out_value == NULL || found == NULL) return CF_INVALID;
    memset(out_value, 0, sizeof *out_value);
    *found = false;
    unsigned char key[64];
    cf_err rc = auth_signed_cookie_key(secret_key_base, key);
    if (rc != CF_OK) return rc;
    cf_builder purpose = {0};
    rc = auth_cookie_purpose(name, &purpose);
    if (rc != CF_OK) {
        cf_builder_dispose(&purpose);
        return rc;
    }
    const cf_auth_verifier_config config = {
        .digest = CF_AUTH_DIGEST_SHA1,
        .encoding = CF_AUTH_ENCODING_STRICT,
        .serializer = CF_AUTH_SERIALIZER_NULL};
    cf_str value = {0};
    bool ok = false;
    /* verify(purpose).or_else(verify(None)): any failure retries without a
     * purpose, exactly like the pinned source. */
    rc = cf_auth_verifier_verify((cf_span){key, sizeof key}, &config, raw,
                                 (cf_span){purpose.ptr, purpose.len}, true,
                                 now_us, &value, &ok);
    if (rc == CF_OK && !ok) {
        rc = cf_auth_verifier_verify((cf_span){key, sizeof key}, &config, raw,
                                     (cf_span){NULL, 0}, false, now_us, &value,
                                     &ok);
    }
    cf_builder_dispose(&purpose);
    if (rc != CF_OK) return rc;
    if (ok) {
        bool is_string = auth_json_string_value(
            (cf_span){(const unsigned char *)value.ptr, value.len}, out_value);
        cf_str_dispose(&value);
        *found = is_string;
    }
    return CF_OK;
}

cf_err cf_auth_cookie_encrypt(cf_span secret_key_base, cf_span name,
                              cf_span value_json, bool has_expiry,
                              int64_t expires_us, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    unsigned char key[32];
    cf_err rc = auth_encrypted_cookie_key(secret_key_base, key);
    cf_builder purpose = {0};
    if (rc == CF_OK) rc = auth_cookie_purpose(name, &purpose);
    if (rc == CF_OK) {
        rc = cf_auth_encryptor_encrypt((cf_span){key, sizeof key}, value_json,
                                       (cf_span){purpose.ptr, purpose.len},
                                       true, has_expiry, expires_us, out);
    }
    cf_builder_dispose(&purpose);
    return rc;
}

static bool auth_json_parses(cf_span json) {
    yyjson_read_err err;
    yyjson_doc *doc =
        yyjson_read_opts((char *)json.ptr, json.len, 0, NULL, &err);
    if (doc == NULL) return false;
    yyjson_doc_free(doc);
    return true;
}

cf_err cf_auth_cookie_decrypt(cf_span secret_key_base, cf_span name,
                              cf_span raw, int64_t now_us,
                              cf_str *out_value_json, bool *found) {
    if (out_value_json == NULL || found == NULL) return CF_INVALID;
    memset(out_value_json, 0, sizeof *out_value_json);
    *found = false;
    unsigned char key[32];
    cf_err rc = auth_encrypted_cookie_key(secret_key_base, key);
    if (rc != CF_OK) return rc;
    cf_builder purpose = {0};
    rc = auth_cookie_purpose(name, &purpose);
    if (rc != CF_OK) {
        cf_builder_dispose(&purpose);
        return rc;
    }
    cf_str value = {0};
    bool ok = false;
    rc = cf_auth_encryptor_decrypt((cf_span){key, sizeof key}, raw,
                                   (cf_span){purpose.ptr, purpose.len}, true,
                                   now_us, &value, &ok);
    if (rc == CF_OK && !ok) {
        rc = cf_auth_encryptor_decrypt((cf_span){key, sizeof key}, raw,
                                       (cf_span){NULL, 0}, false, now_us,
                                       &value, &ok);
    }
    cf_builder_dispose(&purpose);
    if (rc != CF_OK) return rc;
    if (ok) {
        cf_span span = {(const unsigned char *)value.ptr, value.len};
        if (auth_json_parses(span)) {
            *out_value_json = value;
            *found = true;
            return CF_OK;
        }
        cf_str_dispose(&value);
    }
    return CF_OK;
}

/* --- Active Record signed ids ---------------------------------------------- */

static cf_err auth_signed_id_key(cf_span secret_key_base, unsigned char *out64) {
    return auth_pbkdf2_sha256(secret_key_base,
                              auth_cstr_span("active_record/signed_id"), 64,
                              out64);
}

/* String#underscore for class names (signed_id.rs): `name.replace("::", "/")`
 * first, then the character walk. */
static cf_err auth_underscore(cf_span name, cf_builder *out) {
    cf_err rc = CF_OK;
    for (size_t i = 0; rc == CF_OK && i < name.len; i++) {
        char c = (char)name.ptr[i];
        if (c == ':' && i + 1 < name.len && name.ptr[i + 1] == ':') {
            rc = cf_builder_append(out, auth_cstr_span("/"));
            i++;
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            char previous = i > 0 ? (char)name.ptr[i - 1] : '\0';
            char next = i + 1 < name.len ? (char)name.ptr[i + 1] : '\0';
            bool after_lower_or_digit =
                (previous >= 'a' && previous <= 'z') ||
                (previous >= '0' && previous <= '9');
            bool acronym_end = (previous >= 'A' && previous <= 'Z') &&
                               (next >= 'a' && next <= 'z');
            if (after_lower_or_digit || acronym_end) {
                rc = cf_builder_append(out, auth_cstr_span("_"));
            }
            if (rc == CF_OK) {
                char lower = (char)(c - 'A' + 'a');
                rc = cf_builder_append(
                    out, (cf_span){(const unsigned char *)&lower, 1});
            }
            continue;
        }
        char mapped = c == '-' ? '_' : c;
        rc = cf_builder_append(out,
                               (cf_span){(const unsigned char *)&mapped, 1});
    }
    return rc;
}

static bool auth_span_blank(cf_span span) {
    for (size_t i = 0; i < span.len; i++) {
        unsigned char c = span.ptr[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\v' && c != '\f' &&
            c != '\r' && c != '\0') {
            return false;
        }
    }
    return true;
}

/* combine_signed_id_purposes. */
static cf_err auth_combine_purposes(cf_span model_name, cf_span purpose,
                                    bool has_purpose, cf_builder *out) {
    cf_builder base = {0};
    cf_err rc = auth_underscore(model_name, &base);
    cf_span parts[2] = {{base.ptr, base.len},
                        {has_purpose ? purpose.ptr : NULL,
                         has_purpose ? purpose.len : 0}};
    for (size_t i = 0; rc == CF_OK && i < 2; i++) {
        if (parts[i].len == 0 || auth_span_blank(parts[i])) continue;
        if (out->len != 0) rc = cf_builder_append(out, auth_cstr_span("/"));
        if (rc == CF_OK) rc = cf_builder_append(out, parts[i]);
    }
    cf_builder_dispose(&base);
    return rc;
}

cf_err cf_auth_signed_id_generate(cf_span secret_key_base, cf_span model_name,
                                  int64_t id, cf_span purpose, bool has_purpose,
                                  bool has_expiry, int64_t expires_us,
                                  cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    unsigned char key[64];
    cf_err rc = auth_signed_id_key(secret_key_base, key);
    char id_text[32];
    int n = snprintf(id_text, sizeof id_text, "%lld", (long long)id);
    cf_builder purpose_text = {0};
    if (rc == CF_OK) {
        rc = auth_combine_purposes(model_name, purpose, has_purpose,
                                   &purpose_text);
    }
    if (rc == CF_OK && (n <= 0 || (size_t)n >= sizeof id_text)) rc = CF_INVALID;
    if (rc == CF_OK) {
        const cf_auth_verifier_config config = {
            .digest = CF_AUTH_DIGEST_SHA256,
            .encoding = CF_AUTH_ENCODING_URLSAFE,
            .serializer = CF_AUTH_SERIALIZER_JSON};
        rc = cf_auth_verifier_generate(
            (cf_span){key, sizeof key}, &config, auth_cstr_span(id_text),
            (cf_span){purpose_text.ptr, purpose_text.len},
            purpose_text.len != 0, has_expiry, expires_us, out);
    }
    cf_builder_dispose(&purpose_text);
    return rc;
}

static bool auth_parse_i64_text(cf_span text, int64_t *out) {
    size_t begin = 0, end = text.len;
    while (begin < end && (text.ptr[begin] == ' ' || text.ptr[begin] == '\t' ||
                           text.ptr[begin] == '\n' || text.ptr[begin] == '\r' ||
                           text.ptr[begin] == '\v' || text.ptr[begin] == '\f')) {
        begin++;
    }
    while (end > begin && (text.ptr[end - 1] == ' ' ||
                           text.ptr[end - 1] == '\t' ||
                           text.ptr[end - 1] == '\n' ||
                           text.ptr[end - 1] == '\r' ||
                           text.ptr[end - 1] == '\v' ||
                           text.ptr[end - 1] == '\f')) {
        end--;
    }
    if (begin == end || end - begin > 31) return false;
    char buffer[32];
    memcpy(buffer, text.ptr + begin, end - begin);
    buffer[end - begin] = '\0';
    errno = 0;
    char *stop = NULL;
    long long value = strtoll(buffer, &stop, 10);
    if (errno != 0 || stop == buffer || *stop != '\0') return false;
    *out = (int64_t)value;
    return true;
}

cf_err cf_auth_signed_id_verify(cf_span secret_key_base, cf_span model_name,
                                cf_span signed_id, cf_span purpose,
                                bool has_purpose, int64_t now_us,
                                cf_optional_i64 *out, bool *found) {
    if (out == NULL || found == NULL) return CF_INVALID;
    out->present = false;
    out->value = 0;
    *found = false;
    unsigned char key[64];
    cf_err rc = auth_signed_id_key(secret_key_base, key);
    if (rc != CF_OK) return rc;
    cf_builder purpose_text = {0};
    rc = auth_combine_purposes(model_name, purpose, has_purpose, &purpose_text);
    if (rc != CF_OK) {
        cf_builder_dispose(&purpose_text);
        return rc;
    }
    const cf_auth_verifier_config config = {
        .digest = CF_AUTH_DIGEST_SHA256,
        .encoding = CF_AUTH_ENCODING_URLSAFE,
        .serializer = CF_AUTH_SERIALIZER_JSON};
    cf_str value = {0};
    bool ok = false;
    rc = cf_auth_verifier_verify((cf_span){key, sizeof key}, &config, signed_id,
                                 (cf_span){purpose_text.ptr, purpose_text.len},
                                 purpose_text.len != 0, now_us, &value, &ok);
    cf_builder_dispose(&purpose_text);
    if (rc != CF_OK) return rc;
    if (ok) {
        yyjson_read_err err;
        yyjson_doc *doc = yyjson_read_opts((char *)value.ptr, value.len, 0, NULL,
                                           &err);
        if (doc != NULL) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            if (yyjson_is_sint(root)) {
                out->present = true;
                out->value = (int64_t)yyjson_get_sint(root);
                *found = true;
            } else if (yyjson_is_uint(root)) {
                uint64_t u = yyjson_get_uint(root);
                if (u <= (uint64_t)INT64_MAX) {
                    out->present = true;
                    out->value = (int64_t)u;
                    *found = true;
                }
            } else if (yyjson_is_str(root)) {
                cf_span text = {
                    (const unsigned char *)yyjson_get_str(root),
                    yyjson_get_len(root)};
                int64_t parsed = 0;
                if (auth_parse_i64_text(text, &parsed)) {
                    out->present = true;
                    out->value = parsed;
                    *found = true;
                }
            }
            yyjson_doc_free(doc);
        }
        cf_str_dispose(&value);
    }
    return CF_OK;
}

/* --- Rails.application.message_verifier(name) ------------------------------- */

static cf_err auth_app_verifier_key(cf_span secret_key_base, cf_span name,
                                    unsigned char *out64) {
    return auth_pbkdf2_sha256(secret_key_base, name, 64, out64);
}

cf_err cf_auth_app_verifier_generate(cf_span secret_key_base, cf_span name,
                                     cf_span data_json, cf_span purpose,
                                     bool has_purpose, bool has_expiry,
                                     int64_t expires_us, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    unsigned char key[64];
    cf_err rc = auth_app_verifier_key(secret_key_base, name, key);
    if (rc == CF_OK) {
        const cf_auth_verifier_config config = {
            .digest = CF_AUTH_DIGEST_SHA1,
            .encoding = CF_AUTH_ENCODING_STRICT,
            .serializer = CF_AUTH_SERIALIZER_JSON_FALLBACK};
        rc = cf_auth_verifier_generate((cf_span){key, sizeof key}, &config,
                                       data_json, purpose, has_purpose,
                                       has_expiry, expires_us, out);
    }
    return rc;
}

cf_err cf_auth_app_verifier_verify(cf_span secret_key_base, cf_span name,
                                   cf_span message, cf_span purpose,
                                   bool has_purpose, int64_t now_us,
                                   cf_str *out_json, bool *found) {
    if (out_json == NULL || found == NULL) return CF_INVALID;
    memset(out_json, 0, sizeof *out_json);
    *found = false;
    unsigned char key[64];
    cf_err rc = auth_app_verifier_key(secret_key_base, name, key);
    if (rc != CF_OK) return rc;
    const cf_auth_verifier_config config = {
        .digest = CF_AUTH_DIGEST_SHA1,
        .encoding = CF_AUTH_ENCODING_STRICT,
        .serializer = CF_AUTH_SERIALIZER_JSON_FALLBACK};
    return cf_auth_verifier_verify((cf_span){key, sizeof key}, &config, message,
                                   purpose, has_purpose, now_us, out_json,
                                   found);
}

/* --- GlobalID / SignedGlobalID --------------------------------------------- */

cf_err cf_auth_global_id_parse(cf_span gid, cf_str *out_app, cf_str *out_model,
                               cf_str *out_id, bool *found) {
    if (out_app == NULL || out_model == NULL || out_id == NULL ||
        found == NULL) {
        return CF_INVALID;
    }
    memset(out_app, 0, sizeof *out_app);
    memset(out_model, 0, sizeof *out_model);
    memset(out_id, 0, sizeof *out_id);
    *found = false;
    static const char prefix[] = "gid://";
    if (gid.len < sizeof prefix - 1 ||
        memcmp(gid.ptr, prefix, sizeof prefix - 1) != 0) {
        return CF_OK;
    }
    cf_span rest = {gid.ptr + sizeof prefix - 1, gid.len - (sizeof prefix - 1)};
    for (size_t i = 0; i < rest.len; i++) {
        if (rest.ptr[i] == '?') {
            rest.len = i;
            break;
        }
    }
    const unsigned char *slash = memchr(rest.ptr, '/', rest.len);
    if (slash == NULL) return CF_OK;
    cf_span app = {rest.ptr, (size_t)(slash - rest.ptr)};
    cf_span after = {slash + 1, rest.len - app.len - 1};
    const unsigned char *slash2 = memchr(after.ptr, '/', after.len);
    if (slash2 == NULL) return CF_OK;
    cf_span model = {after.ptr, (size_t)(slash2 - after.ptr)};
    cf_span id = {slash2 + 1, after.len - model.len - 1};
    if (app.len == 0 || model.len == 0 || id.len == 0) return CF_OK;
    cf_err rc = auth_str_dup(app, out_app);
    if (rc == CF_OK) rc = auth_str_dup(model, out_model);
    if (rc == CF_OK) rc = auth_str_dup(id, out_id);
    if (rc != CF_OK) {
        cf_str_dispose(out_app);
        cf_str_dispose(out_model);
        cf_str_dispose(out_id);
        return rc;
    }
    *found = true;
    return CF_OK;
}

cf_err cf_auth_global_id_param(cf_span gid_uri, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    return auth_base64_encode(gid_uri, CF_AUTH_ENCODING_URLSAFE, out);
}

static cf_err auth_sgid_key(cf_span secret_key_base, unsigned char *out64) {
    return auth_pbkdf2_sha256(secret_key_base,
                              auth_cstr_span("signed_global_ids"), 64, out64);
}

cf_err cf_auth_sgid_generate(cf_span secret_key_base, cf_span gid_uri,
                             cf_span purpose, bool has_expiry,
                             int64_t expires_us, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    unsigned char key[64];
    cf_err rc = auth_sgid_key(secret_key_base, key);
    cf_builder data = {0};
    if (rc == CF_OK) rc = auth_json_string_encode(&data, gid_uri);
    if (rc == CF_OK) {
        const cf_auth_verifier_config config = {
            .digest = CF_AUTH_DIGEST_SHA1,
            .encoding = CF_AUTH_ENCODING_URLSAFE_PADDED,
            .serializer = CF_AUTH_SERIALIZER_JSON_FALLBACK};
        rc = cf_auth_verifier_generate((cf_span){key, sizeof key}, &config,
                                       (cf_span){data.ptr, data.len}, purpose,
                                       true, has_expiry, expires_us, out);
    }
    cf_builder_dispose(&data);
    return rc;
}

cf_err cf_auth_sgid_generate_attachable(cf_span secret_key_base, cf_span gid_uri,
                                        cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    cf_builder uri = {0};
    /* GlobalID turns the leftover `expires_in: nil` option into a query
     * param: `gid://campfire/User/1?expires_in`. */
    cf_err rc = cf_builder_append(&uri, gid_uri);
    if (rc == CF_OK) rc = cf_builder_append(&uri, auth_cstr_span("?expires_in"));
    if (rc == CF_OK) {
        rc = cf_auth_sgid_generate(secret_key_base,
                                   (cf_span){uri.ptr, uri.len},
                                   auth_cstr_span("attachable"), false, 0, out);
    }
    cf_builder_dispose(&uri);
    return rc;
}

cf_err cf_auth_sgid_locate(cf_span secret_key_base, cf_span sgid,
                           cf_span purpose, int64_t now_us, cf_str *out_gid,
                           bool *found) {
    if (out_gid == NULL || found == NULL) return CF_INVALID;
    memset(out_gid, 0, sizeof *out_gid);
    *found = false;
    unsigned char key[64];
    cf_err rc = auth_sgid_key(secret_key_base, key);
    if (rc != CF_OK) return rc;
    const cf_auth_verifier_config config = {
        .digest = CF_AUTH_DIGEST_SHA1,
        .encoding = CF_AUTH_ENCODING_URLSAFE_PADDED,
        .serializer = CF_AUTH_SERIALIZER_JSON_FALLBACK};
    cf_str value = {0};
    bool ok = false;
    rc = cf_auth_verifier_verify((cf_span){key, sizeof key}, &config, sgid,
                                 purpose, true, now_us, &value, &ok);
    if (rc != CF_OK) return rc;
    if (ok) {
        yyjson_read_err err;
        yyjson_doc *doc =
            yyjson_read_opts((char *)value.ptr, value.len, 0, NULL, &err);
        if (doc != NULL) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            if (yyjson_is_str(root)) {
                cf_span text = {(const unsigned char *)yyjson_get_str(root),
                                yyjson_get_len(root)};
                cf_str app = {0}, model = {0}, id = {0};
                bool parsed = false;
                rc = cf_auth_global_id_parse(text, &app, &model, &id, &parsed);
                if (rc == CF_OK && !parsed) {
                    /* GlobalID::from_param: URL-safe Base64 of the gid URI. */
                    cf_buf *decoded = NULL;
                    rc = auth_base64_urlsafe_decode(text, &decoded);
                    if (rc == CF_OK) {
                        if (auth_utf8_valid(cf_buf_span(decoded))) {
                            cf_span span = cf_buf_span(decoded);
                            rc = cf_auth_global_id_parse(span, &app, &model, &id,
                                                         &parsed);
                        }
                        cf_buf_release(decoded);
                    } else if (rc == CF_INVALID) {
                        rc = CF_OK;
                    }
                }
                if (rc == CF_OK && parsed) {
                    cf_builder canonical = {0};
                    rc = cf_builder_append(&canonical, auth_cstr_span("gid://"));
                    if (rc == CF_OK) rc = cf_builder_append(&canonical, (cf_span){(const unsigned char *)app.ptr, app.len});
                    if (rc == CF_OK) rc = cf_builder_append(&canonical, auth_cstr_span("/"));
                    if (rc == CF_OK) rc = cf_builder_append(&canonical, (cf_span){(const unsigned char *)model.ptr, model.len});
                    if (rc == CF_OK) rc = cf_builder_append(&canonical, auth_cstr_span("/"));
                    if (rc == CF_OK) rc = cf_builder_append(&canonical, (cf_span){(const unsigned char *)id.ptr, id.len});
                    if (rc == CF_OK) {
                        rc = auth_str_dup(
                            (cf_span){canonical.ptr, canonical.len}, out_gid);
                    }
                    if (rc == CF_OK) *found = true;
                    cf_builder_dispose(&canonical);
                }
                cf_str_dispose(&app);
                cf_str_dispose(&model);
                cf_str_dispose(&id);
            }
            yyjson_doc_free(doc);
        }
        cf_str_dispose(&value);
    }
    return rc;
}

/* --- Turbo stream names ----------------------------------------------------- */

static cf_err auth_turbo_key(cf_span secret_key_base, unsigned char *out64) {
    return auth_pbkdf2_sha256(secret_key_base,
                              auth_cstr_span("turbo/signed_stream_verifier_key"),
                              64, out64);
}

cf_err cf_auth_turbo_signed_stream_name(cf_span secret_key_base,
                                        const cf_span *parts, size_t parts_len,
                                        cf_str *out) {
    if (out == NULL || (parts_len != 0 && parts == NULL)) return CF_INVALID;
    memset(out, 0, sizeof *out);
    unsigned char key[64];
    cf_err rc = auth_turbo_key(secret_key_base, key);
    cf_builder joined = {0};
    for (size_t i = 0; rc == CF_OK && i < parts_len; i++) {
        if (i != 0) rc = cf_builder_append(&joined, auth_cstr_span(":"));
        if (rc == CF_OK) rc = cf_builder_append(&joined, parts[i]);
    }
    cf_builder data = {0};
    /* turbo.rs uses the JSON serializer, so the dump is JSON.generate. */
    if (rc == CF_OK) rc = auth_json_string_generate(
        &data, (cf_span){joined.ptr, joined.len});
    if (rc == CF_OK) {
        const cf_auth_verifier_config config = {
            .digest = CF_AUTH_DIGEST_SHA256,
            .encoding = CF_AUTH_ENCODING_STRICT,
            .serializer = CF_AUTH_SERIALIZER_JSON};
        rc = cf_auth_verifier_generate((cf_span){key, sizeof key}, &config,
                                       (cf_span){data.ptr, data.len},
                                       (cf_span){NULL, 0}, false, false, 0,
                                       out);
    }
    cf_builder_dispose(&joined);
    cf_builder_dispose(&data);
    return rc;
}

cf_err cf_auth_turbo_verified_stream_name(cf_span secret_key_base, cf_span message,
                                          cf_str *out, bool *found) {
    if (out == NULL || found == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    *found = false;
    unsigned char key[64];
    cf_err rc = auth_turbo_key(secret_key_base, key);
    if (rc != CF_OK) return rc;
    const cf_auth_verifier_config config = {
        .digest = CF_AUTH_DIGEST_SHA256,
        .encoding = CF_AUTH_ENCODING_STRICT,
        .serializer = CF_AUTH_SERIALIZER_JSON};
    cf_str value = {0};
    bool ok = false;
    rc = cf_auth_verifier_verify((cf_span){key, sizeof key}, &config, message,
                                 (cf_span){NULL, 0}, false, 0, &value, &ok);
    if (rc != CF_OK) return rc;
    if (ok) {
        yyjson_read_err err;
        yyjson_doc *doc =
            yyjson_read_opts((char *)value.ptr, value.len, 0, NULL, &err);
        if (doc != NULL) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            if (yyjson_is_str(root)) {
                rc = auth_str_dup(
                    (cf_span){(const unsigned char *)yyjson_get_str(root),
                              yyjson_get_len(root)},
                    out);
                if (rc == CF_OK) *found = true;
            } else if (yyjson_is_int(root) || yyjson_is_real(root)) {
                rc = auth_json_write_value(root, false, out);
                if (rc == CF_OK) *found = true;
            }
            yyjson_doc_free(doc);
        }
        cf_str_dispose(&value);
    }
    return rc;
}
