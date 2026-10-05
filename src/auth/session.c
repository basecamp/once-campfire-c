/* src/auth/session.c — the session lifecycle of the Authentication concern
 * (concerns.rs restore_authentication/resume_session/start_new_session_for/
 * terminate_current_session), the encrypted `_campfire_session` cookie state
 * (kit session.rs) and the flash hand-off.
 */
#include "internal.h"

#include "app.h"
#include "config.h"
#include "db/writer.h"
#include "models/membership.h"
#include "models/push_subscription.h"
#include "models/room.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* User::authenticated's constant dummy digest (user.rs). */
#define AUTH_DUMMY_DIGEST \
    "$2a$12$FiKmSp4UhLvSB4Sd/ZUjQunyKP6.NjDRHdr5LnKUVk.BUn4Mq12WS"

/* --- encrypted session cookie state ---------------------------------------- */

struct auth_kv {
    char *key;
    size_t key_len;
    char *value; /* raw JSON text */
    size_t value_len;
};

struct auth_session_state {
    struct auth_kv *items;
    size_t len, cap;
    bool present; /* the request carried the cookie */
    bool changed;
};

static const char auth_session_id_key[] = "session_id";

/* serde_json Value equality: Session::insert marks the session changed only
 * when the stored value differs.  Comparing parsed values (rather than the raw
 * JSON text) matches that: key order, whitespace, escapes and number spelling
 * do not matter.  Both sides are valid JSON by construction; a text that does
 * not parse falls back to byte equality. */
static bool auth_json_text_equal(cf_span a, cf_span b) {
    if (a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0)) {
        return true;
    }
    yyjson_read_err err;
    yyjson_doc *left =
        yyjson_read_opts((char *)a.ptr, a.len, 0, NULL, &err);
    if (left == NULL) return false;
    yyjson_doc *right =
        yyjson_read_opts((char *)b.ptr, b.len, 0, NULL, &err);
    bool equal = right != NULL &&
                 yyjson_equals(yyjson_doc_get_root(left),
                               yyjson_doc_get_root(right));
    if (right != NULL) yyjson_doc_free(right);
    yyjson_doc_free(left);
    return equal;
}

/* commit() drops null-valued entries before deciding and serializing
 * (`filter(|(_, v)| !v.is_null())`). */
static bool auth_session_item_is_null(const struct auth_kv *item) {
    if (item->value_len == 4 && memcmp(item->value, "null", 4) == 0) return true;
    yyjson_read_err err;
    yyjson_doc *doc =
        yyjson_read_opts(item->value, item->value_len, 0, NULL, &err);
    if (doc == NULL) return false;
    bool is_null = yyjson_is_null(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    return is_null;
}

static void auth_session_state_dispose(struct auth_session_state *state) {
    if (state == NULL) return;
    for (size_t i = 0; i < state->len; i++) {
        free(state->items[i].key);
        free(state->items[i].value);
    }
    free(state->items);
    memset(state, 0, sizeof *state);
}

static ssize_t auth_session_find(const struct auth_session_state *state,
                                 cf_span key) {
    for (size_t i = 0; i < state->len; i++) {
        if (state->items[i].key_len == key.len &&
            memcmp(state->items[i].key, key.ptr, key.len) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static cf_err auth_session_set_item(struct auth_session_state *state,
                                    cf_span key, cf_span value_json) {
    char *key_copy = malloc(key.len + 1);
    char *value_copy = malloc(value_json.len + 1);
    if (key_copy == NULL || value_copy == NULL) {
        free(key_copy);
        free(value_copy);
        return CF_NOMEM;
    }
    memcpy(key_copy, key.ptr, key.len);
    key_copy[key.len] = '\0';
    if (value_json.len != 0) memcpy(value_copy, value_json.ptr, value_json.len);
    value_copy[value_json.len] = '\0';
    ssize_t at = auth_session_find(state, key);
    if (at >= 0) {
        if (auth_json_text_equal(
                (cf_span){(const unsigned char *)state->items[at].value,
                          state->items[at].value_len},
                value_json)) {
            /* Session::insert leaves the stored representation untouched and
             * does not mark the session changed. */
            free(key_copy);
            free(value_copy);
            return CF_OK;
        }
        free(state->items[at].value);
        state->items[at].value = value_copy;
        state->items[at].value_len = value_json.len;
        free(key_copy);
    } else {
        if (state->len == state->cap) {
            size_t cap = state->cap == 0 ? 8 : state->cap * 2;
            struct auth_kv *grown =
                realloc(state->items, cap * sizeof *grown);
            if (grown == NULL) {
                free(key_copy);
                free(value_copy);
                return CF_NOMEM;
            }
            state->items = grown;
            state->cap = cap;
        }
        state->items[state->len++] =
            (struct auth_kv){key_copy, key.len, value_copy, value_json.len};
    }
    state->changed = true;
    return CF_OK;
}

static void auth_session_drop_item(struct auth_session_state *state,
                                   cf_span key) {
    ssize_t at = auth_session_find(state, key);
    if (at < 0) return;
    free(state->items[at].key);
    free(state->items[at].value);
    state->items[at] = state->items[state->len - 1];
    state->len--;
    state->changed = true;
}

/* SecureRandom.hex(16) (session.rs generate_sid). */
static cf_err auth_session_generate_sid(cf_str *out) {
    unsigned char bytes[16];
    cf_err rc = cf_random_bytes(bytes, sizeof bytes);
    if (rc != CF_OK) return rc;
    return auth_hex_encode((cf_span){bytes, sizeof bytes}, out);
}

static cf_err auth_session_ensure_sid(struct auth_session_state *state) {
    cf_span key = auth_cstr_span(auth_session_id_key);
    ssize_t at = auth_session_find(state, key);
    if (at >= 0) {
        yyjson_read_err err;
        yyjson_doc *doc = yyjson_read_opts(state->items[at].value,
                                           state->items[at].value_len, 0, NULL,
                                           &err);
        /* Session::load only replaces a missing or null session_id. */
        bool has_sid =
            doc != NULL && !yyjson_is_null(yyjson_doc_get_root(doc));
        if (doc != NULL) yyjson_doc_free(doc);
        if (has_sid) return CF_OK;
    }
    cf_str sid = {0};
    cf_err rc = auth_session_generate_sid(&sid);
    cf_builder value = {0};
    if (rc == CF_OK) {
        rc = auth_json_string_encode(&value,
                                     (cf_span){(const unsigned char *)sid.ptr,
                                               sid.len});
    }
    if (rc == CF_OK) {
        rc = auth_session_set_item(
            state, key, (cf_span){value.ptr, value.len});
    }
    cf_builder_dispose(&value);
    cf_str_dispose(&sid);
    return rc;
}

/* Load and decrypt the cookie once; malformed/invalid data reads as empty
 * (Session::load's `Some(Value::Object(map)) => map, _ => Map::new()`). */
static cf_err auth_session_load(cf_ctx *ctx, struct auth_session_state *state) {
    memset(state, 0, sizeof *state);
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL) return CF_INTERNAL;
    cf_span name = auth_cstr_span(AUTH_SESSION_COOKIE);
    cf_span raw;
    cf_err rc = cf_ctx_cookie_get(ctx, name, &raw);
    if (rc == CF_NOT_FOUND) return CF_OK;
    if (rc != CF_OK) return rc;
    state->present = true;
    cf_str json = {0};
    bool found = false;
    rc = cf_auth_cookie_decrypt(
        (cf_span){(const unsigned char *)config->secret_key_base,
                  config->secret_key_base_len},
        name, raw, cf_now_us(ctx->app), &json, &found);
    if (rc != CF_OK) return rc;
    if (!found) return CF_OK;
    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_opts(json.ptr, json.len, 0, NULL, &err);
    if (doc != NULL) {
        yyjson_val *root = yyjson_doc_get_root(doc);
        if (yyjson_is_obj(root)) {
            yyjson_obj_iter iter;
            yyjson_obj_iter_init(root, &iter);
            yyjson_val *key;
            while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
                yyjson_val *value = yyjson_obj_iter_get_val(key);
                size_t value_len = 0;
                char *value_text = yyjson_val_write(value, 0, &value_len);
                if (value_text == NULL) {
                    rc = CF_NOMEM;
                    break;
                }
                rc = auth_session_set_item(
                    state,
                    (cf_span){(const unsigned char *)yyjson_get_str(key),
                              yyjson_get_len(key)},
                    (cf_span){(const unsigned char *)value_text, value_len});
                free(value_text);
                if (rc != CF_OK) break;
            }
        }
        yyjson_doc_free(doc);
    }
    cf_str_dispose(&json);
    if (rc == CF_OK) state->changed = false; /* loading is not a change */
    if (rc != CF_OK) auth_session_state_dispose(state);
    return rc;
}

/* commit_session: only when the data changed; null values are dropped, and a
 * session left with nothing but session_id (or nothing at all) deletes the
 * cookie instead of re-writing it (kit session.rs commit()). */
static cf_err auth_session_commit(cf_ctx *ctx, struct auth_session_state *state) {
    if (!state->changed) return CF_OK;
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL) return CF_INTERNAL;
    cf_span name = auth_cstr_span(AUTH_SESSION_COOKIE);
    bool has_data = false;
    for (size_t i = 0; i < state->len; i++) {
        if (auth_session_item_is_null(&state->items[i])) continue;
        size_t sid_len = sizeof auth_session_id_key - 1;
        if (state->items[i].key_len != sid_len ||
            memcmp(state->items[i].key, auth_session_id_key, sid_len) != 0) {
            has_data = true;
            break;
        }
    }
    if (!has_data) {
        cf_err rc = cf_ctx_cookie_delete(ctx, name, NULL);
        if (rc == CF_OK) state->changed = false;
        return rc;
    }
    cf_err rc = auth_session_ensure_sid(state);
    if (rc != CF_OK) return rc;
    cf_builder json = {0};
    rc = cf_builder_append(&json, auth_cstr_span("{"));
    bool first = true;
    for (size_t i = 0; rc == CF_OK && i < state->len; i++) {
        if (auth_session_item_is_null(&state->items[i])) continue;
        if (!first) rc = cf_builder_append(&json, auth_cstr_span(","));
        first = false;
        if (rc == CF_OK) {
            rc = auth_json_string_encode(
                &json, (cf_span){(const unsigned char *)state->items[i].key,
                                 state->items[i].key_len});
        }
        if (rc == CF_OK) rc = cf_builder_append(&json, auth_cstr_span(":"));
        if (rc == CF_OK) {
            rc = cf_builder_append(
                &json, (cf_span){(const unsigned char *)state->items[i].value,
                                 state->items[i].value_len});
        }
    }
    if (rc == CF_OK) rc = cf_builder_append(&json, auth_cstr_span("}"));
    int64_t expires_us = auth_years_from_us(cf_now_us(ctx->app), 20);
    cf_str wire = {0};
    if (rc == CF_OK) {
        rc = cf_auth_cookie_encrypt(
            (cf_span){(const unsigned char *)config->secret_key_base,
                      config->secret_key_base_len},
            name, (cf_span){json.ptr, json.len}, true, expires_us, &wire);
    }
    if (rc == CF_OK) {
        rc = auth_check_cookie_overflow(
            name, (cf_span){(const unsigned char *)wire.ptr, wire.len});
    }
    if (rc == CF_OK) {
        cf_cookie_options options;
        memset(&options, 0, sizeof options);
        options.has_expires = true;
        options.expires_us = expires_us;
        options.httponly = true;
        rc = cf_ctx_cookie_set(
            ctx, name, (cf_span){(const unsigned char *)wire.ptr, wire.len},
            &options);
    }
    cf_str_dispose(&wire);
    cf_builder_dispose(&json);
    if (rc == CF_OK) state->changed = false;
    return rc;
}

cf_err cf_auth_session_read(cf_ctx *ctx, cf_span key, cf_str *out_json,
                            bool *found) {
    if (ctx == NULL || out_json == NULL || found == NULL || key.len == 0) {
        return CF_INVALID;
    }
    memset(out_json, 0, sizeof *out_json);
    *found = false;
    struct auth_session_state state;
    cf_err rc = auth_session_load(ctx, &state);
    if (rc != CF_OK) return rc;
    ssize_t at = auth_session_find(&state, key);
    if (at >= 0) {
        rc = auth_str_dup((cf_span){(const unsigned char *)state.items[at].value,
                                    state.items[at].value_len},
                          out_json);
        if (rc == CF_OK) *found = true;
    }
    auth_session_state_dispose(&state);
    return rc;
}

cf_err cf_auth_session_write(cf_ctx *ctx, cf_span key, cf_span value_json) {
    if (ctx == NULL || key.len == 0) return CF_INVALID;
    yyjson_read_err err;
    yyjson_doc *doc =
        yyjson_read_opts((char *)value_json.ptr, value_json.len, 0, NULL, &err);
    if (doc == NULL) return CF_INVALID;
    yyjson_doc_free(doc);
    struct auth_session_state state;
    cf_err rc = auth_session_load(ctx, &state);
    if (rc == CF_OK) rc = auth_session_set_item(&state, key, value_json);
    if (rc == CF_OK) rc = auth_session_commit(ctx, &state);
    auth_session_state_dispose(&state);
    return rc;
}

cf_err cf_auth_session_remove(cf_ctx *ctx, cf_span key) {
    if (ctx == NULL || key.len == 0) return CF_INVALID;
    struct auth_session_state state;
    cf_err rc = auth_session_load(ctx, &state);
    if (rc == CF_OK) auth_session_drop_item(&state, key);
    if (rc == CF_OK) rc = auth_session_commit(ctx, &state);
    auth_session_state_dispose(&state);
    return rc;
}

cf_err cf_auth_flash_load(cf_ctx *ctx) {
    if (ctx == NULL) return CF_INVALID;
    struct auth_session_state state;
    cf_err rc = auth_session_load(ctx, &state);
    if (rc != CF_OK) {
        auth_session_state_dispose(&state);
        return rc;
    }
    static const char flash_key[] = "flash";
    ssize_t at = auth_session_find(&state, auth_cstr_span(flash_key));
    if (at >= 0) {
        yyjson_read_err err;
        yyjson_doc *doc = yyjson_read_opts(state.items[at].value,
                                           state.items[at].value_len, 0, NULL,
                                           &err);
        if (doc != NULL) {
            yyjson_val *flashes = yyjson_obj_get(yyjson_doc_get_root(doc),
                                                 "flashes");
            if (flashes != NULL && yyjson_is_obj(flashes)) {
                yyjson_obj_iter iter;
                yyjson_obj_iter_init(flashes, &iter);
                yyjson_val *key;
                while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
                    yyjson_val *value = yyjson_obj_iter_get_val(key);
                    if (!yyjson_is_str(value)) continue;
                    rc = cf_ctx_flash_set(
                        ctx,
                        (cf_span){(const unsigned char *)yyjson_get_str(key),
                                  yyjson_get_len(key)},
                        (cf_span){
                            (const unsigned char *)yyjson_get_str(value),
                            yyjson_get_len(value)});
                    if (rc != CF_OK) break;
                }
            }
            yyjson_doc_free(doc);
        }
        /* A loaded flash is discarded at the end of the request. */
        auth_session_drop_item(&state, auth_cstr_span(flash_key));
        if (rc == CF_OK) rc = auth_session_commit(ctx, &state);
    }
    auth_session_state_dispose(&state);
    return rc;
}

/* --- session_token cookie --------------------------------------------------- */

cf_err auth_set_session_token_cookie(cf_ctx *ctx, cf_span token) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL) return CF_INTERNAL;
    cf_str wire = {0};
    int64_t expires_us = auth_years_from_us(cf_now_us(ctx->app), 20);
    cf_err rc = cf_auth_signed_cookie_generate(
        (cf_span){(const unsigned char *)config->secret_key_base,
                  config->secret_key_base_len},
        auth_cstr_span(AUTH_SESSION_TOKEN_COOKIE), token, true, expires_us,
        &wire);
    if (rc == CF_OK) {
        rc = auth_check_cookie_overflow(
            auth_cstr_span(AUTH_SESSION_TOKEN_COOKIE),
            (cf_span){(const unsigned char *)wire.ptr, wire.len});
    }
    if (rc == CF_OK) {
        cf_cookie_options options;
        memset(&options, 0, sizeof options);
        options.has_expires = true;
        options.expires_us = expires_us;
        options.httponly = true;
        rc = cf_ctx_cookie_set(
            ctx, auth_cstr_span(AUTH_SESSION_TOKEN_COOKIE),
            (cf_span){(const unsigned char *)wire.ptr, wire.len}, &options);
    }
    cf_str_dispose(&wire);
    return rc;
}

/* --- identity / current user ------------------------------------------------ */

cf_err cf_auth_current_user(cf_ctx *ctx, bool *found, cf_user *out) {
    if (ctx == NULL || found == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    *found = false;
    if (ctx->identity.kind == CF_AUTH_NONE || ctx->reader == NULL) {
        return CF_OK;
    }
    return cf_user_find_by_id(ctx->reader, ctx->identity.user_id, found, out);
}

static void auth_set_identity(cf_ctx *ctx, cf_auth_kind kind, int64_t user_id,
                              int64_t session_id, int role, int status,
                              bool activity_due) {
    ctx->identity.kind = kind;
    ctx->identity.user_id = user_id;
    ctx->identity.session_id = session_id;
    ctx->identity.role = role;
    ctx->identity.status = status;
    ctx->identity.activity_due = activity_due;
}

static void auth_clear_identity(cf_ctx *ctx) {
    memset(&ctx->identity, 0, sizeof ctx->identity);
}

/* --- auth policy helpers ---------------------------------------------------- */

static bool auth_param_present(const cf_param *param) {
    switch (cf_param_type(param)) {
    case CF_PARAM_NULL:
        return false;
    case CF_PARAM_BOOL: {
        bool present = false, value = false;
        if (cf_param_bool(param, &present, &value) != CF_OK) return false;
        return present && value;
    }
    case CF_PARAM_STRING: {
        cf_span text;
        if (cf_param_string(param, &text) != CF_OK) return false;
        for (size_t i = 0; i < text.len; i++) {
            unsigned char c = text.ptr[i];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\v' && c != '\f' &&
                c != '\r') {
                return true;
            }
        }
        return false;
    }
    case CF_PARAM_ARRAY:
        return cf_param_count(param) != 0;
    case CF_PARAM_OBJECT:
        return cf_param_count(param) != 0;
    default:
        return true; /* numbers, uploads */
    }
}

/* Ruby String#strip: NUL and ASCII whitespace off both ends. */
static cf_span auth_strip(cf_span text) {
    size_t begin = 0, end = text.len;
    while (begin < end &&
           (text.ptr[begin] == '\0' || text.ptr[begin] == ' ' ||
            text.ptr[begin] == '\t' || text.ptr[begin] == '\n' ||
            text.ptr[begin] == '\v' || text.ptr[begin] == '\f' ||
            text.ptr[begin] == '\r')) {
        begin++;
    }
    while (end > begin &&
           (text.ptr[end - 1] == '\0' || text.ptr[end - 1] == ' ' ||
            text.ptr[end - 1] == '\t' || text.ptr[end - 1] == '\n' ||
            text.ptr[end - 1] == '\v' || text.ptr[end - 1] == '\f' ||
            text.ptr[end - 1] == '\r')) {
        end--;
    }
    return (cf_span){text.ptr + begin, end - begin};
}

typedef struct {
    cf_session session;
    cf_optional_str user_agent;
    cf_optional_str ip_address;
} auth_resume_arg;

static cf_err auth_resume_cb(cf_tx *tx, void *user) {
    auth_resume_arg *arg = user;
    return cf_session_resume(tx, &arg->session, arg->user_agent,
                             arg->ip_address);
}

typedef struct {
    int64_t user_id;
    cf_optional_str user_agent;
    cf_optional_str ip_address;
    cf_session session;
} auth_start_arg;

static cf_err auth_start_cb(cf_tx *tx, void *user) {
    auth_start_arg *arg = user;
    return cf_session_start(tx, arg->user_id, arg->user_agent,
                            arg->ip_address, &arg->session);
}

static cf_optional_str auth_optional_header(cf_ctx *ctx, const char *name) {
    cf_span value;
    if (!auth_request_header(ctx->request, name, &value)) {
        return (cf_optional_str){0};
    }
    /* The session model copies immediate values (SQLITE_TRANSIENT); the
     * request's immutable storage outlives the cf_write call. */
    return (cf_optional_str){.present = true,
                             .value = {(char *)(uintptr_t)value.ptr,
                                       value.len}};
}

static cf_optional_str auth_optional_peer_ip(cf_ctx *ctx) {
    cf_span value = ctx->request->peer_ip;
    if (value.len == 0) return (cf_optional_str){0};
    return (cf_optional_str){.present = true,
                             .value = {(char *)(uintptr_t)value.ptr,
                                       value.len}};
}

/* --- restore_authentication / resume_session -------------------------------- */

static cf_err auth_restore_session(cf_ctx *ctx, cf_session *session,
                                   bool *signed_in) {
    int64_t now = cf_now_us(ctx->app);
    bool activity_due = cf_session_needs_resume(session, now);
    if (activity_due) {
        auth_resume_arg arg = {.session = *session,
                               .user_agent = auth_optional_header(ctx, "user-agent"),
                               .ip_address = auth_optional_peer_ip(ctx)};
        cf_err rc = cf_write(ctx->app, auth_resume_cb, &arg);
        if (rc != CF_OK) return rc;
        *session = arg.session;
        rc = auth_set_session_token_cookie(
            ctx, (cf_span){(const unsigned char *)session->token.ptr,
                           session->token.len});
        if (rc != CF_OK) return rc;
    }
    auth_set_identity(ctx, CF_AUTH_SESSION, session->user_id, session->id,
                      CF_ROLE_MEMBER, CF_STATUS_ACTIVE, activity_due);
    if (signed_in != NULL) *signed_in = false;
    if (ctx->reader != NULL) {
        cf_user user;
        bool found = false;
        cf_err rc = cf_user_find_by_id(ctx->reader, session->user_id, &found,
                                       &user);
        if (rc != CF_OK) return rc;
        if (found) {
            ctx->identity.role = (int)user.role;
            ctx->identity.status = (int)user.status;
            cf_user_dispose(&user);
            if (signed_in != NULL) *signed_in = true;
        }
    }
    return CF_OK;
}

cf_err auth_restore_session_only(cf_ctx *ctx, bool *restored,
                                 bool *signed_in) {
    if (ctx == NULL || restored == NULL || signed_in == NULL) {
        return CF_INVALID;
    }
    *restored = false;
    *signed_in = false;
    if (ctx->reader == NULL) return CF_OK;
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL) return CF_INTERNAL;
    cf_span raw;
    if (cf_ctx_cookie_get(ctx, auth_cstr_span(AUTH_SESSION_TOKEN_COOKIE),
                          &raw) != CF_OK) {
        return CF_OK;
    }
    cf_str token = {0};
    bool found = false;
    cf_err rc = cf_auth_signed_cookie_verify(
        (cf_span){(const unsigned char *)config->secret_key_base,
                  config->secret_key_base_len},
        auth_cstr_span(AUTH_SESSION_TOKEN_COOKIE), raw, cf_now_us(ctx->app),
        &token, &found);
    if (rc != CF_OK) return rc;
    if (!found) return CF_OK;
    cf_session session;
    bool session_found = false;
    rc = cf_session_find_by_token(ctx->reader, token, &session_found, &session);
    cf_str_dispose(&token);
    if (rc != CF_OK) return rc;
    if (session_found) {
        *restored = true;
        rc = auth_restore_session(ctx, &session, signed_in);
        cf_session_dispose(&session);
    }
    return rc;
}

/* --- request_authentication ------------------------------------------------- */

static cf_err auth_request_authentication(cf_ctx *ctx) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL) return CF_INTERNAL;
    cf_span origin = auth_cstr_span(config->public_origin);
    cf_builder url = {0};
    cf_err rc = cf_builder_append(&url, origin);
    if (rc == CF_OK) rc = cf_builder_append(&url, ctx->request->path);
    if (rc == CF_OK && ctx->request->query.len != 0) {
        rc = cf_builder_append(&url, auth_cstr_span("?"));
        if (rc == CF_OK) rc = cf_builder_append(&url, ctx->request->query);
    }
    cf_builder value = {0};
    if (rc == CF_OK) {
        rc = auth_json_string_encode(&value, (cf_span){url.ptr, url.len});
    }
    if (rc == CF_OK) {
        rc = cf_auth_session_write(
            ctx, auth_cstr_span("return_to_after_authenticating"),
            (cf_span){value.ptr, value.len});
    }
    cf_builder location = {0};
    if (rc == CF_OK) rc = cf_builder_append(&location, origin);
    if (rc == CF_OK) rc = cf_builder_append(&location, auth_cstr_span("/session/new"));
    if (rc == CF_OK) {
        cf_response *response = ctx->response;
        response->status = 302;
        rc = cf_response_header(
            response, auth_cstr_span("Location"),
            (cf_span){location.ptr, location.len});
        if (rc == CF_OK) {
            rc = cf_response_header(response, auth_cstr_span("Content-Type"),
                                    auth_cstr_span("text/html; charset=utf-8"));
        }
    }
    cf_builder_dispose(&url);
    cf_builder_dispose(&value);
    cf_builder_dispose(&location);
    return rc;
}

cf_err cf_authenticate(cf_ctx *ctx) {
    if (ctx == NULL || ctx->app == NULL || ctx->response == NULL) {
        return CF_INVALID;
    }
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL) return CF_INTERNAL;
    int64_t now = cf_now_us(ctx->app);

    /* restore_authentication */
    cf_span raw;
    if (cf_ctx_cookie_get(ctx, auth_cstr_span(AUTH_SESSION_TOKEN_COOKIE),
                          &raw) == CF_OK) {
        cf_str token = {0};
        bool found = false;
        cf_err rc = cf_auth_signed_cookie_verify(
            (cf_span){(const unsigned char *)config->secret_key_base,
                      config->secret_key_base_len},
            auth_cstr_span(AUTH_SESSION_TOKEN_COOKIE), raw, now, &token,
            &found);
        if (rc != CF_OK) return rc;
        if (found) {
            cf_session session;
            bool session_found = false;
            rc = cf_session_find_by_token(ctx->reader, token, &session_found,
                                          &session);
            cf_str_dispose(&token);
            if (rc != CF_OK) return rc;
            if (session_found) {
                rc = auth_restore_session(ctx, &session, NULL);
                cf_session_dispose(&session);
                return rc;
            }
        }
    }

    /* bot_authentication */
    const cf_param *param = cf_ctx_param(ctx, auth_cstr_span("bot_key"));
    if (param != NULL && auth_param_present(param)) {
        if (cf_param_type(param) != CF_PARAM_STRING) {
            /* `params[:bot_key].strip` raises NoMethodError for a hash/array
             * (concerns.rs); the C port maps it to the internal error. */
            return CF_INTERNAL;
        }
        cf_span key;
        if (cf_param_string(param, &key) != CF_OK) return CF_INTERNAL;
        key = auth_strip(key);
        cf_user bot;
        bool found = false;
        cf_err rc = cf_user_authenticate_bot(ctx->reader, (cf_str){.ptr = (char *)(uintptr_t)key.ptr, .len = key.len},
                                             &found, &bot);
        if (rc != CF_OK) return rc;
        if (found) {
            auth_set_identity(ctx, CF_AUTH_BOT, bot.id, 0, (int)bot.role,
                              (int)bot.status, false);
            cf_user_dispose(&bot);
            return CF_OK;
        }
    }

    /* request_authentication: remember where we were, then sign in. */
    return auth_request_authentication(ctx);
}

/* --- authenticate_by / start_new_session_for -------------------------------- */

cf_err cf_auth_authenticate_by(cf_ctx *ctx, cf_span email_address,
                               cf_span password, bool *authenticated,
                               cf_user *out_user) {
    if (ctx == NULL || authenticated == NULL || out_user == NULL) {
        return CF_INVALID;
    }
    memset(out_user, 0, sizeof *out_user);
    *authenticated = false;
    if (password.len != 0 && password.ptr == NULL) return CF_INVALID;
    /* `authenticate_by` returns nil for a blank password before looking
     * anything up. */
    if (password.len == 0) return CF_OK;
    cf_user candidate;
    bool found = false;
    cf_err rc = cf_user_find_active_by_email_address(
        ctx->reader, (cf_str){.ptr = (char *)(uintptr_t)email_address.ptr,
                              .len = email_address.len},
        &found, &candidate);
    if (rc != CF_OK) return rc;
    bool ok = false;
    if (found) {
        rc = cf_user_authenticated(&candidate, (cf_str){.ptr = (char *)(uintptr_t)password.ptr, .len = password.len}, &ok);
    } else {
        /* A missing account still runs the constant dummy verification. */
        rc = cf_user_authenticated(NULL, (cf_str){.ptr = (char *)(uintptr_t)password.ptr, .len = password.len}, &ok);
    }
    if (rc != CF_OK) {
        if (found) cf_user_dispose(&candidate);
        return rc;
    }
    if (found && ok) {
        *out_user = candidate;
        *authenticated = true;
    } else if (found) {
        cf_user_dispose(&candidate);
    }
    return CF_OK;
}

cf_err cf_auth_start_new_session_for(cf_ctx *ctx, const cf_user *user,
                                     cf_session *out_session) {
    if (ctx == NULL || user == NULL) return CF_INVALID;
    auth_start_arg arg = {.user_id = user->id,
                          .user_agent = auth_optional_header(ctx, "user-agent"),
                          .ip_address = auth_optional_peer_ip(ctx)};
    cf_err rc = cf_write(ctx->app, auth_start_cb, &arg);
    if (rc != CF_OK) return rc;
    rc = auth_set_session_token_cookie(
        ctx, (cf_span){(const unsigned char *)arg.session.token.ptr,
                       arg.session.token.len});
    if (rc != CF_OK) {
        cf_session_dispose(&arg.session);
        return rc;
    }
    auth_set_identity(ctx, CF_AUTH_SESSION, user->id, arg.session.id,
                      (int)user->role, (int)user->status, false);
    if (out_session != NULL) {
        *out_session = arg.session;
    } else {
        cf_session_dispose(&arg.session);
    }
    return CF_OK;
}

cf_err cf_auth_post_authenticating_url(cf_ctx *ctx, cf_str *out_url) {
    if (ctx == NULL || out_url == NULL) return CF_INVALID;
    memset(out_url, 0, sizeof *out_url);
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL) return CF_INTERNAL;
    cf_str value = {0};
    bool found = false;
    cf_err rc = cf_auth_session_read(
        ctx, auth_cstr_span("return_to_after_authenticating"), &value, &found);
    if (rc == CF_OK) {
        rc = cf_auth_session_remove(
            ctx, auth_cstr_span("return_to_after_authenticating"));
    }
    cf_span origin = auth_cstr_span(config->public_origin);
    cf_builder root = {0};
    if (rc == CF_OK) rc = cf_builder_append(&root, origin);
    if (rc == CF_OK) rc = cf_builder_append(&root, auth_cstr_span("/"));

    /* Only a URL string that stays under PUBLIC_ORIGIN may be redirected to
     * (02-data-auth.md A01); absent, null or corrupted values fall back to the
     * root.  In the reference the value is always a request.url() string, so
     * the non-string fallback is a safety net, not a behavior path. */
    cf_str stored = {0};
    bool under_origin = false;
    if (rc == CF_OK && found) {
        yyjson_read_err err;
        yyjson_doc *doc = yyjson_read_opts(value.ptr, value.len, 0, NULL, &err);
        if (doc != NULL) {
            yyjson_val *root_val = yyjson_doc_get_root(doc);
            if (yyjson_is_str(root_val)) {
                cf_span url = {
                    (const unsigned char *)yyjson_get_str(root_val),
                    yyjson_get_len(root_val)};
                rc = auth_str_dup(url, &stored);
                if (rc == CF_OK && url.len >= origin.len &&
                    memcmp(url.ptr, origin.ptr, origin.len) == 0 &&
                    (url.len == origin.len || url.ptr[origin.len] == '/' ||
                     url.ptr[origin.len] == '?')) {
                    under_origin = true;
                }
            }
            yyjson_doc_free(doc);
        }
    }
    if (rc == CF_OK) {
        if (found && under_origin && stored.ptr != NULL) {
            rc = auth_str_dup(
                (cf_span){(const unsigned char *)stored.ptr, stored.len},
                out_url);
        } else {
            rc = auth_str_dup((cf_span){root.ptr, root.len}, out_url);
        }
    }
    cf_str_dispose(&value);
    cf_str_dispose(&stored);
    cf_builder_dispose(&root);
    return rc;
}

/* --- terminate_current_session ---------------------------------------------- */

typedef struct {
    cf_session session;
} auth_destroy_arg;

static cf_err auth_destroy_cb(cf_tx *tx, void *user) {
    auth_destroy_arg *arg = user;
    return cf_session_destroy(tx, &arg->session);
}

/* reset_session: drop the data, start a new session id, and let commit's
 * "nothing but session_id" rule delete the cookie. */
static cf_err auth_session_reset(cf_ctx *ctx) {
    struct auth_session_state state;
    cf_err rc = auth_session_load(ctx, &state);
    if (rc == CF_OK) {
        for (size_t i = 0; i < state.len; i++) {
            free(state.items[i].key);
            free(state.items[i].value);
        }
        state.len = 0;
        state.changed = true;
        rc = auth_session_ensure_sid(&state);
    }
    if (rc == CF_OK) rc = auth_session_commit(ctx, &state);
    auth_session_state_dispose(&state);
    return rc;
}

typedef struct {
    int64_t user_id;
    cf_str endpoint;
} auth_push_delete_arg;

static cf_err auth_push_delete_cb(cf_tx *tx, void *user) {
    auth_push_delete_arg *arg = user;
    return cf_push_subscription_destroy_by_endpoint(tx, arg->user_id,
                                                    arg->endpoint);
}

typedef struct {
    cf_user user;
} auth_disconnect_arg;

static cf_err auth_disconnect_cb(cf_tx *tx, void *user) {
    auth_disconnect_arg *arg = user;
    return cf_user_reset_remote_connections(tx, &arg->user);
}

cf_err cf_auth_terminate_current_session(cf_ctx *ctx) {
    if (ctx == NULL) return CF_INVALID;
    cf_err rc = CF_OK;
    int64_t user_id = ctx->identity.kind != CF_AUTH_NONE ? ctx->identity.user_id : 0;

    /* Push::Subscription.destroy_by(endpoint: params[...], user_id: Current.user.id) */
    cf_span endpoint_param;
    const cf_param *endpoint =
        cf_ctx_param(ctx, auth_cstr_span("push_subscription_endpoint"));
    if (endpoint != NULL && user_id != 0 &&
        cf_param_string(endpoint, &endpoint_param) == CF_OK) {
        auth_push_delete_arg arg = {.user_id = user_id,
                                    .endpoint = {
                                        (char *)(uintptr_t)endpoint_param.ptr,
                                        endpoint_param.len}};
        rc = cf_write(ctx->app, auth_push_delete_cb, &arg);
        if (rc != CF_OK) return rc;
    }

    if (ctx->identity.kind == CF_AUTH_SESSION && ctx->identity.session_id != 0) {
        cf_session session;
        /* CF_NOT_FOUND means the row is already gone. */
        rc = cf_session_find(ctx->reader, ctx->identity.session_id, &session);
        if (rc == CF_OK) {
            auth_destroy_arg arg = {.session = session};
            rc = cf_write(ctx->app, auth_destroy_cb, &arg);
            cf_session_dispose(&session);
            if (rc != CF_OK) return rc;
        } else if (rc != CF_NOT_FOUND) {
            return rc;
        }
    }

    /* reset_session + cookies.delete("session_token") */
    rc = auth_session_reset(ctx);
    if (rc != CF_OK) return rc;
    rc = cf_ctx_cookie_delete(ctx, auth_cstr_span(AUTH_SESSION_TOKEN_COOKIE),
                              NULL);
    if (rc != CF_OK) return rc;

    /* reset_remote_connections (reconnect=true); errors only logged. */
    if (user_id != 0 && ctx->reader != NULL) {
        cf_user user;
        bool found = false;
        cf_err find_rc = cf_user_find_by_id(ctx->reader, user_id, &found, &user);
        if (find_rc == CF_OK && found) {
            auth_disconnect_arg arg = {.user = user};
            cf_err write_rc = cf_write(ctx->app, auth_disconnect_cb, &arg);
            cf_user_dispose(&user);
            if (write_rc != CF_OK) {
                fprintf(stderr,
                        "campfire: auth: disconnect on sign out failed: %s\n",
                        cf_err_name(write_rc));
            }
        }
    }
    auth_clear_identity(ctx);
    return CF_OK;
}
