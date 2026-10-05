/* src/actions/autocompletable/users.c — Autocompletable::UsersController
 * (task A-autocompletable-users; route ID 75 `index`;
 * contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/autocompletable.rs
 * (the `users` submodule), the pinned port of
 * reference/app/controllers/autocompletable/users_controller.rb:
 *
 *   index: require_current_user; room scope =
 *          params[:room_id].present? ? Current.user.rooms.find(id).users
 *          : User.all (a non-castable id or an unreachable room is 404);
 *          query = params[:filter].presence || params[:query].presence (the
 *          mentions prompt filters with `filter`, autocomplete inputs with
 *          `query`); users_scope.active[.filtered_by(query)].ordered; page
 *          the ordered rows (per_page 20); present each row as a mention
 *          user; respond_to HTML/JSON; the after-action paginated headers
 *          for JSON (X-Total-Count, plus a `Link: <url>; rel="next"` unless
 *          last); JSON renders the jbuilder array, HTML renders the
 *          <lexxy-prompt-item> list with no layout.
 *
 * The room-scoped SQL (`memberships.room_id = ? AND status = 0 [AND name
 * like ?] ORDER BY LOWER(name)`) is composed here from the landed model
 * primitives (cf_room_users for the membership join, then the active-status,
 * LIKE and LOWER(name) predicates inline); the global scope uses the landed
 * cf_user_active[_filtered_by]_ordered queries directly.  SQLite LIKE is
 * ASCII case-insensitive with unescaped `%`/`_` wildcards, which the inline
 * matcher reproduces exactly.
 *
 * c_symbol for the integrator's route rebind (src/routes.c row 75):
 *   cf_action_autocompletable_users_index.
 *
 * Integrator requests (views/presenters packet; each static shim is marked):
 *  R1. cf_view_autocompletable_users_index — the
 *      autocompletable/users/index.html.erb list (`render layout: false`).
 *      Proposed declaration (src/views.h):
 *        typedef struct {
 *            int64_t id; cf_str name;             // borrowed
 *            cf_str avatar_path;                  // borrowed
 *            cf_str attachable_sgid;              // borrowed
 *        } cf_view_mention_user;                  // cf. users::MentionUser
 *        cf_err cf_view_autocompletable_users_index(
 *            const cf_view_ctx *,
 *            const cf_view_mention_user *users, size_t users_len,
 *            cf_builder *);
 *      Semantics: the <lexxy-prompt-item> elements for the mentions prompt,
 *      with no layout (no Link preload header either).
 *  R2. (optional) shared mention-user shaping + the jbuilder array
 *      (presenters mention_user, views users_index_json): the static
 *      auto_* helpers below are that exact construction (HTML-escaped name
 *      via ERB::Util.html_escape, absolute fresh avatar URL, attachable
 *      SGID, serde_json string escaping and [name,value,avatar_url,sgid]
 *      field order); a shared renderer can replace them verbatim.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "http/params.h" /* cf_param_to_s (Param::to_s lexemes) */
#include "models/room.h"
#include "models/user.h"
#include "views.h"
#include "views/internal.h" /* cf_views_integer_cast */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- INTEGRATOR SHIM R1 proposal (delete when views.h lands it) ----------
 * (Integrator: title field added with the G1 close; keep in sync with
 * src/views/users_models.h until the dedup lands.) */

typedef struct {
    int64_t id;
    cf_str name; /* borrowed */
    cf_str title; /* borrowed: User#title, may be empty (falls back to name) */
    cf_str avatar_path; /* borrowed */
    cf_str attachable_sgid; /* borrowed */
} cf_view_mention_user;

cf_err cf_view_autocompletable_users_index(
    const cf_view_ctx *ctx, const cf_view_mention_user *users,
    size_t users_len, cf_builder *out);

/* ---- small helpers -------------------------------------------------------- */

static cf_span auto_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `c.param_str(key)`: Some only for a string param. */
static bool auto_param_str(cf_ctx *ctx, const char *name, cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, auto_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) return false;
    return cf_param_string(param, out) == CF_OK;
}

/* `Param::is_present` (blank?: nil, false, whitespace-only strings, empty
 * collections).  Numbers, uploads and true are present; false is blank. */
static bool auto_param_present(const cf_param *param) {
    if (param == NULL) return false;
    switch (cf_param_type(param)) {
    case CF_PARAM_NULL:
        return false;
    case CF_PARAM_BOOL: {
        bool present = false;
        bool value = false;
        if (cf_param_bool(param, &present, &value) != CF_OK) return false;
        return value;
    }
    case CF_PARAM_STRING: {
        cf_span text;
        if (cf_param_string(param, &text) != CF_OK) return false;
        for (size_t i = 0; i < text.len; i++) {
            unsigned char c = text.ptr[i];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\v' &&
                c != '\f' && c != '\r') {
                return true;
            }
        }
        return false;
    }
    case CF_PARAM_NUMBER:
    case CF_PARAM_UPLOAD:
        return true;
    case CF_PARAM_ARRAY:
    case CF_PARAM_OBJECT:
        return cf_param_count(param) > 0;
    }
    return false;
}

/* `Param::to_s` (kit params.rs): None for arrays/hashes/uploads, "" for
 * null, "true"/"false" for bools, the lexeme for numbers. */
static bool auto_param_to_s(const cf_param *param, cf_span *out) {
    if (param == NULL) return false;
    switch (cf_param_type(param)) {
    case CF_PARAM_NULL:
        *out = (cf_span){NULL, 0};
        return true;
    case CF_PARAM_STRING:
        return cf_param_string(param, out) == CF_OK;
    case CF_PARAM_BOOL: {
        bool present = false;
        bool value = false;
        if (cf_param_bool(param, &present, &value) != CF_OK) return false;
        *out = value ? auto_span("true") : auto_span("false");
        return true;
    }
    case CF_PARAM_NUMBER: {
        cf_span text = {0};
        if (cf_param_to_s(param, &text) != CF_OK) return false;
        *out = text;
        return true;
    }
    default:
        return false;
    }
}

/* `Ctx::render_as` (kit ctx.rs render_as -> set_vary_header): status,
 * content type, body, plus `Vary: Accept` exactly when the format came from
 * the Accept header. */
static cf_err auto_render_as(cf_ctx *ctx, unsigned status,
                             cf_span content_type, cf_builder *body) {
    cf_buf *buf = NULL;
    cf_err rc = cf_builder_freeze(body, &buf);
    if (rc != CF_OK) {
        cf_builder_dispose(body);
        return rc;
    }
    ctx->response->status = status;
    rc = cf_response_body(ctx->response, buf);
    cf_buf_release(buf);
    if (rc != CF_OK) return rc;
    rc = cf_response_header(ctx->response, auto_span("Content-Type"),
                            content_type);
    if (rc == CF_OK && cf_ctx_vary_accept(ctx)) {
        rc = cf_response_header(ctx->response, auto_span("Vary"),
                                auto_span("Accept"));
    }
    return rc;
}

/* ---- pagination (presenters/pagination.rs, geared_pagination) ------------- */

/* `String#to_i` (ruby integer.rs to_i128): leading SPACE, one sign, one 0d,
 * digits with `_` allowed between two; saturating. */
static int64_t auto_to_i(cf_span text) {
    static const char space[] = " \t\n\v\f\r";
    size_t i = 0;
    while (i < text.len && memchr(space, text.ptr[i], sizeof space - 1) !=
                               NULL) {
        i++;
    }
    bool negative = false;
    if (i < text.len && (text.ptr[i] == '-' || text.ptr[i] == '+')) {
        negative = text.ptr[i] == '-';
        i++;
    }
    if (i + 2 <= text.len && text.ptr[i] == '0' &&
        (text.ptr[i + 1] == 'd' || text.ptr[i + 1] == 'D')) {
        i += 2;
    }
    __int128 number = 0;
    bool previous_digit = false;
    for (; i < text.len; i++) {
        unsigned char c = text.ptr[i];
        if (c >= '0' && c <= '9') {
            number = number * 10 + (c - '0');
            if (number > (__int128)INT64_MAX + 1) {
                number = (__int128)INT64_MAX + 1;
            }
            previous_digit = true;
        } else if (c == '_' && previous_digit) {
            previous_digit = false;
        } else {
            break;
        }
    }
    if (negative) number = -number;
    if (number > (__int128)INT64_MAX) return INT64_MAX;
    if (number < (__int128)INT64_MIN) return INT64_MIN;
    return (int64_t)number;
}

/* `param.to_i > 0 ? param.to_i : 1`, capped at 1e9. */
static int64_t auto_page_number(cf_ctx *ctx) {
    cf_span text = {NULL, 0};
    int64_t number =
        auto_param_str(ctx, "page", &text) ? auto_to_i(text) : 0;
    if (number < 1) return 1;
    if (number > 1000000000) return 1000000000;
    return number;
}

/* Single-ratio page (ratios [20]): limit 20, offset (n-1)*20. */
#define AUTO_PER_PAGE INT64_C(20)

/* `page_count`: ratios consumed until nothing is left, at least 1. */
static int64_t auto_page_count(int64_t records_count) {
    int64_t count = 0;
    int64_t residual = records_count;
    while (residual > 0) {
        count++;
        residual -= AUTO_PER_PAGE;
    }
    return count < 1 ? 1 : count;
}

/* Addressable query-component encoding (uri.rs url_encode / Addressable
 * UNRESERVED): `A-Za-z0-9_.-~` literal, every other byte %XX uppercase. */
static cf_err auto_encode_component(cf_span in, cf_builder *out) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < in.len; i++) {
        unsigned char c = in.ptr[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' ||
            c == '~') {
            cf_err rc = cf_builder_append(out, (cf_span){&in.ptr[i], 1});
            if (rc != CF_OK) return rc;
        } else {
            char esc[3] = {'%', hex[c >> 4], hex[c & 15]};
            cf_err rc = cf_builder_append(
                out, (cf_span){(const unsigned char *)esc, 3});
            if (rc != CF_OK) return rc;
        }
    }
    return CF_OK;
}

/* Percent-decode a query component (`+` is space; malformed escapes stay
 * raw, matching Addressable's parse which never fails here). */
static cf_err auto_decode_component(cf_span in, cf_builder *out) {
    for (size_t i = 0; i < in.len;) {
        unsigned char c = in.ptr[i];
        if (c == '+') {
            cf_err rc = cf_builder_append(out, auto_span(" "));
            if (rc != CF_OK) return rc;
            i++;
        } else if (c == '%' && i + 2 < in.len + 1) {
            unsigned hi = 0, lo = 0;
            unsigned char a = i + 1 < in.len ? in.ptr[i + 1] : 0;
            unsigned char b = i + 2 < in.len ? in.ptr[i + 2] : 0;
            bool ok = true;
            if (a >= '0' && a <= '9') hi = (unsigned)(a - '0');
            else if (a >= 'a' && a <= 'f') hi = (unsigned)(a - 'a' + 10);
            else if (a >= 'A' && a <= 'F') hi = (unsigned)(a - 'A' + 10);
            else ok = false;
            if (b >= '0' && b <= '9') lo = (unsigned)(b - '0');
            else if (b >= 'a' && b <= 'f') lo = (unsigned)(b - 'a' + 10);
            else if (b >= 'A' && b <= 'F') lo = (unsigned)(b - 'A' + 10);
            else ok = false;
            if (ok) {
                unsigned char byte = (unsigned char)((hi << 4) | lo);
                cf_err rc =
                    cf_builder_append(out, (cf_span){&byte, 1});
                if (rc != CF_OK) return rc;
                i += 3;
            } else {
                cf_err rc =
                    cf_builder_append(out, (cf_span){&in.ptr[i], 1});
                if (rc != CF_OK) return rc;
                i++;
            }
        } else {
            cf_err rc = cf_builder_append(out, (cf_span){&in.ptr[i], 1});
            if (rc != CF_OK) return rc;
            i++;
        }
    }
    return CF_OK;
}

typedef struct {
    cf_builder key;
    cf_builder value;
    bool has_value;
} auto_query_pair;

static void auto_query_pair_dispose(auto_query_pair *pair) {
    cf_builder_dispose(&pair->key);
    cf_builder_dispose(&pair->value);
}

static int auto_query_pair_compare(const void *a, const void *b) {
    const auto_query_pair *pa = a;
    const auto_query_pair *pb = b;
    size_t n = pa->key.len < pb->key.len ? pa->key.len : pb->key.len;
    int cmp = n == 0 ? 0 : memcmp(pa->key.ptr, pb->key.ptr, n);
    if (cmp != 0) return cmp;
    if (pa->key.len < pb->key.len) return -1;
    if (pa->key.len > pb->key.len) return 1;
    return 0;
}

/* `with_page(url, next)`: the request URL with its query re-encoded from a
 * hash (keys sorted, duplicates collapse to the last value) plus
 * page=<next>, keeping any #fragment. */
static cf_err auto_with_page(cf_ctx *ctx, int64_t next, cf_builder *out) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    const cf_request *request = ctx->request;

    cf_builder url = {0};
    cf_err rc = cf_builder_append(&url, auto_span(config->public_origin));
    if (rc == CF_OK && request != NULL) {
        rc = cf_builder_append(&url, request->path);
    }
    if (rc == CF_OK && request != NULL && request->query.len != 0) {
        rc = cf_builder_append(&url, auto_span("?"));
        if (rc == CF_OK) {
            rc = cf_builder_append(&url, request->query);
        }
    }
    if (rc != CF_OK) {
        cf_builder_dispose(&url);
        return rc;
    }
    cf_span full = {url.ptr, url.len};

    /* Split off the fragment, then the query. */
    cf_span base = full;
    cf_span fragment = {NULL, 0};
    for (size_t i = 0; i < full.len; i++) {
        if (full.ptr[i] == '#') {
            base.len = i;
            fragment.ptr = full.ptr + i + 1;
            fragment.len = full.len - i - 1;
            break;
        }
    }
    cf_span path = base;
    cf_span query = {NULL, 0};
    for (size_t i = 0; i < base.len; i++) {
        if (base.ptr[i] == '?') {
            path.len = i;
            query.ptr = base.ptr + i + 1;
            query.len = base.len - i - 1;
            break;
        }
    }

    auto_query_pair pairs[64];
    size_t pairs_len = 0;
    memset(pairs, 0, sizeof pairs);
    /* Split the query on '&', skipping empties. */
    size_t start = 0;
    for (size_t i = 0; i <= query.len && rc == CF_OK; i++) {
        if (i < query.len && query.ptr[i] != '&') continue;
        cf_span item = {query.ptr + start, i - start};
        start = i + 1;
        if (item.len == 0) continue;
        cf_span raw_key = item;
        cf_span raw_value = {NULL, 0};
        bool has_value = false;
        for (size_t k = 0; k < item.len; k++) {
            if (item.ptr[k] == '=') {
                raw_key.len = k;
                raw_value.ptr = item.ptr + k + 1;
                raw_value.len = item.len - k - 1;
                has_value = true;
                break;
            }
        }
        /* `value.replace('+', " ")` before decoding. */
        cf_builder plus = {0};
        for (size_t k = 0; k < raw_value.len && rc == CF_OK; k++) {
            unsigned char c =
                raw_value.ptr[k] == '+' ? ' ' : raw_value.ptr[k];
            rc = cf_builder_append(&plus, (cf_span){&c, 1});
        }
        cf_builder key = {0}, value = {0};
        if (rc == CF_OK) rc = auto_decode_component(raw_key, &key);
        if (rc == CF_OK && has_value) {
            rc = auto_decode_component((cf_span){plus.ptr, plus.len},
                                       &value);
        }
        cf_builder_dispose(&plus);
        if (rc != CF_OK) {
            cf_builder_dispose(&key);
            cf_builder_dispose(&value);
            break;
        }
        /* Duplicates collapse to the last value. */
        auto_query_pair *existing = NULL;
        for (size_t k = 0; k < pairs_len; k++) {
            if (pairs[k].key.len == key.len &&
                (key.len == 0 ||
                 memcmp(pairs[k].key.ptr, key.ptr, key.len) == 0)) {
                existing = &pairs[k];
                break;
            }
        }
        if (existing != NULL) {
            cf_builder_dispose(&existing->value);
            existing->value = value;
            existing->has_value = has_value;
            cf_builder_dispose(&key);
        } else if (pairs_len < sizeof pairs / sizeof pairs[0]) {
            pairs[pairs_len].key = key;
            pairs[pairs_len].value = value;
            pairs[pairs_len].has_value = has_value;
            pairs_len++;
        } else {
            cf_builder_dispose(&key);
            cf_builder_dispose(&value);
            rc = CF_LIMIT;
        }
    }
    /* Merge page=<next>. */
    if (rc == CF_OK) {
        char digits[24];
        int n = snprintf(digits, sizeof digits, "%lld", (long long)next);
        if (n < 0 || (size_t)n >= sizeof digits) {
            rc = CF_INTERNAL;
        } else {
            cf_builder page_value = {0};
            rc = cf_builder_append(
                &page_value,
                (cf_span){(const unsigned char *)digits, (size_t)n});
            if (rc == CF_OK) {
                auto_query_pair *existing = NULL;
                for (size_t k = 0; k < pairs_len; k++) {
                    if (pairs[k].key.len == 4 &&
                        memcmp(pairs[k].key.ptr, "page", 4) == 0) {
                        existing = &pairs[k];
                        break;
                    }
                }
                if (existing != NULL) {
                    cf_builder_dispose(&existing->value);
                    existing->value = page_value;
                    existing->has_value = true;
                } else if (pairs_len < sizeof pairs / sizeof pairs[0]) {
                    rc = cf_builder_append(&pairs[pairs_len].key,
                                           auto_span("page"));
                    pairs[pairs_len].value = page_value;
                    pairs[pairs_len].has_value = true;
                    if (rc == CF_OK) pairs_len++;
                    else cf_builder_dispose(&page_value);
                } else {
                    cf_builder_dispose(&page_value);
                    rc = CF_LIMIT;
                }
            } else {
                cf_builder_dispose(&page_value);
            }
        }
    }
    if (rc == CF_OK) {
        qsort(pairs, pairs_len, sizeof pairs[0], auto_query_pair_compare);
        rc = cf_builder_append(out, path);
        for (size_t k = 0; k < pairs_len && rc == CF_OK; k++) {
            rc = cf_builder_append(out, k == 0 ? auto_span("?")
                                               : auto_span("&"));
            if (rc == CF_OK) {
                rc = auto_encode_component(
                    (cf_span){pairs[k].key.ptr, pairs[k].key.len}, out);
            }
            if (rc == CF_OK && pairs[k].has_value) {
                rc = cf_builder_append(out, auto_span("="));
                if (rc == CF_OK) {
                    rc = auto_encode_component(
                        (cf_span){pairs[k].value.ptr, pairs[k].value.len},
                        out);
                }
            }
        }
        if (rc == CF_OK && fragment.ptr != NULL) {
            rc = cf_builder_append(out, auto_span("#"));
            if (rc == CF_OK) {
                rc = cf_builder_append(out, fragment);
            }
        }
    }
    for (size_t k = 0; k < pairs_len; k++) auto_query_pair_dispose(&pairs[k]);
    cf_builder_dispose(&url);
    return rc;
}

/* `set_paginated_headers` for JSON: X-Total-Count, plus Link to the next
 * page unless last. */
static cf_err auto_apply_headers(cf_ctx *ctx, int64_t records_count,
                                 int64_t page_number) {
    char total[24];
    int n = snprintf(total, sizeof total, "%lld", (long long)records_count);
    if (n < 0 || (size_t)n >= sizeof total) return CF_INTERNAL;
    cf_err rc = cf_response_header(
        ctx->response, auto_span("X-Total-Count"),
        (cf_span){(const unsigned char *)total, (size_t)n});
    if (rc != CF_OK) return rc;
    if (page_number != auto_page_count(records_count)) {
        cf_builder next = {0};
        rc = auto_with_page(ctx, page_number + 1, &next);
        if (rc == CF_OK) {
            cf_builder link = {0};
            rc = cf_builder_append(&link, auto_span("<"));
            if (rc == CF_OK) {
                rc = cf_builder_append(&link,
                                       (cf_span){next.ptr, next.len});
            }
            if (rc == CF_OK) {
                rc = cf_builder_append(&link,
                                       auto_span(">; rel=\"next\""));
            }
            if (rc == CF_OK) {
                rc = cf_response_header(ctx->response, auto_span("Link"),
                                        (cf_span){link.ptr, link.len});
            }
            cf_builder_dispose(&link);
        }
        cf_builder_dispose(&next);
    }
    return rc;
}

/* ---- mention shaping (presenters mention_user + views UserJson) ------------ */

struct auto_mention {
    int64_t id;
    cf_str name; /* owned */
    cf_str title; /* owned: User#title (name plus bio) */
    cf_str avatar_path; /* owned, absolute fresh_user_avatar_url */
    cf_str attachable_sgid; /* owned */
};

static void auto_mention_dispose(struct auto_mention *mention) {
    cf_str_dispose(&mention->name);
    cf_str_dispose(&mention->title);
    cf_str_dispose(&mention->avatar_path);
    cf_str_dispose(&mention->attachable_sgid);
    memset(mention, 0, sizeof *mention);
}

static void auto_to_fs_number(int64_t us, char out[15]) {
    time_t seconds = (time_t)(us / 1000000);
    if (us < 0 && us % 1000000 != 0) seconds -= 1;
    struct tm tm;
    if (gmtime_r(&seconds, &tm) == NULL) {
        memcpy(out, "00000000000000", 14);
        return;
    }
    if (strftime(out, 15, "%Y%m%d%H%M%S", &tm) == 0) {
        memcpy(out, "00000000000000", 14);
    }
}

/* `user.attachable_sgid` for `gid://campfire/User/<id>` (global_id.rs). */
static cf_err auto_attachable_sgid(cf_ctx *ctx, int64_t user_id, cf_str *out) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->secret_key_base == NULL) return CF_INTERNAL;
    char gid[64];
    int n = snprintf(gid, sizeof gid, "gid://campfire/User/%lld",
                     (long long)user_id);
    if (n < 0 || (size_t)n >= sizeof gid) return CF_INTERNAL;
    return cf_auth_sgid_generate_attachable(
        (cf_span){(const unsigned char *)config->secret_key_base,
                  config->secret_key_base_len},
        (cf_span){(const unsigned char *)gid, (size_t)n}, out);
}

/* `mention_user`: the summary, its attachable SGID and its absolute fresh
 * avatar URL (base_url + fresh_user_avatar_path). */
static cf_err auto_mention_user(cf_ctx *ctx, const cf_user *user,
                                struct auto_mention *out) {
    memset(out, 0, sizeof *out);
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->secret_key_base == NULL ||
        config->public_origin == NULL) {
        return CF_INTERNAL;
    }
    if (user->name.ptr == NULL) return CF_INTERNAL;
    out->id = user->id;
    out->name.ptr = malloc(user->name.len + 1);
    if (out->name.ptr == NULL) return CF_NOMEM;
    memcpy(out->name.ptr, user->name.ptr, user->name.len);
    out->name.ptr[user->name.len] = '\0';
    out->name.len = user->name.len;

    cf_err rc = cf_user_title(user, &out->title);
    if (rc != CF_OK) {
        auto_mention_dispose(out);
        return rc;
    }

    rc = auto_attachable_sgid(ctx, user->id, &out->attachable_sgid);
    if (rc != CF_OK) {
        auto_mention_dispose(out);
        return rc;
    }

    cf_str token = {0};
    rc = cf_auth_signed_id_generate(
        (cf_span){(const unsigned char *)config->secret_key_base,
                  config->secret_key_base_len},
        auto_span("User"), user->id, auto_span("avatar"), true, false, 0,
        &token);
    if (rc != CF_OK) {
        auto_mention_dispose(out);
        return rc;
    }
    char number[15];
    auto_to_fs_number(user->updated_at, number);
    const char *prefix = "/users/";
    const char *middle = "/avatar?v=";
    size_t origin_len = strlen(config->public_origin);
    size_t len = origin_len + strlen(prefix) + token.len + strlen(middle) +
                 sizeof number - 1;
    out->avatar_path.ptr = malloc(len + 1);
    if (out->avatar_path.ptr == NULL) {
        cf_str_dispose(&token);
        auto_mention_dispose(out);
        return CF_NOMEM;
    }
    size_t at = 0;
    memcpy(out->avatar_path.ptr + at, config->public_origin, origin_len);
    at += origin_len;
    memcpy(out->avatar_path.ptr + at, prefix, strlen(prefix));
    at += strlen(prefix);
    memcpy(out->avatar_path.ptr + at, token.ptr, token.len);
    at += token.len;
    cf_str_dispose(&token);
    memcpy(out->avatar_path.ptr + at, middle, strlen(middle));
    at += strlen(middle);
    memcpy(out->avatar_path.ptr + at, number, sizeof number - 1);
    at += sizeof number - 1;
    out->avatar_path.ptr[at] = '\0';
    out->avatar_path.len = at;
    return CF_OK;
}

/* serde_json string escaping (the template's `self.json()`, cf. pwa.c):
 * `"` `\` and the C0 controls (`\b \t \n \f \r` named, the rest lowercase
 * `\u00xx`); `/`, 0x7f and non-ASCII pass through raw. */
static cf_err auto_json_string(cf_builder *out, cf_span in) {
    static const char hex[] = "0123456789abcdef";
    cf_err rc = cf_builder_append(out, auto_span("\""));
    for (size_t i = 0; i < in.len && rc == CF_OK; i++) {
        unsigned char c = in.ptr[i];
        if (c == '"' || c == '\\') {
            char esc[2] = {'\\', (char)c};
            rc = cf_builder_append(out,
                                   (cf_span){
                                       (const unsigned char *)esc, 2});
        } else if (c == '\b') {
            rc = cf_builder_append(out, auto_span("\\b"));
        } else if (c == '\t') {
            rc = cf_builder_append(out, auto_span("\\t"));
        } else if (c == '\n') {
            rc = cf_builder_append(out, auto_span("\\n"));
        } else if (c == '\f') {
            rc = cf_builder_append(out, auto_span("\\f"));
        } else if (c == '\r') {
            rc = cf_builder_append(out, auto_span("\\r"));
        } else if (c < 0x20) {
            char esc[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
            rc = cf_builder_append(out,
                                   (cf_span){
                                       (const unsigned char *)esc, 6});
        } else {
            rc = cf_builder_append(out, (cf_span){&in.ptr[i], 1});
        }
    }
    if (rc == CF_OK) rc = cf_builder_append(out, auto_span("\""));
    return rc;
}

/* One `_user.json.jbuilder` entry: name is `h(user.name)` (ERB html escape)
 * and the fields go out in struct order (name, value, avatar_url, sgid). */
static cf_err auto_user_json_entry(cf_builder *out,
                                   const struct auto_mention *mention) {
    cf_builder escaped = {0};
    cf_err rc = cf_html_attr(
        &escaped, (cf_span){(const unsigned char *)mention->name.ptr,
                            mention->name.len});
    if (rc == CF_OK) {
        rc = cf_builder_append(out, auto_span("{\"name\":"));
    }
    if (rc == CF_OK) {
        rc = auto_json_string(out, (cf_span){escaped.ptr, escaped.len});
    }
    cf_builder_dispose(&escaped);
    if (rc == CF_OK) rc = cf_builder_append(out, auto_span(",\"value\":"));
    if (rc == CF_OK) {
        char digits[24];
        int n =
            snprintf(digits, sizeof digits, "%lld", (long long)mention->id);
        if (n < 0 || (size_t)n >= sizeof digits) return CF_INTERNAL;
        rc = cf_builder_append(
            out, (cf_span){(const unsigned char *)digits, (size_t)n});
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(out, auto_span(",\"avatar_url\":"));
    }
    if (rc == CF_OK) {
        rc = auto_json_string(out, (cf_span){
                                         (const unsigned char *)
                                             mention->avatar_path.ptr,
                                         mention->avatar_path.len,
                                     });
    }
    if (rc == CF_OK) rc = cf_builder_append(out, auto_span(",\"sgid\":"));
    if (rc == CF_OK) {
        rc = auto_json_string(out, (cf_span){
                                         (const unsigned char *)
                                             mention->attachable_sgid.ptr,
                                         mention->attachable_sgid.len,
                                     });
    }
    if (rc == CF_OK) rc = cf_builder_append(out, auto_span("}"));
    return rc;
}

/* `users_index_json`: the JSON array of the page's entries. */
static cf_err auto_users_index_json(const struct auto_mention *mentions,
                                    size_t mentions_len, cf_builder *out) {
    cf_err rc = cf_builder_append(out, auto_span("["));
    for (size_t i = 0; i < mentions_len && rc == CF_OK; i++) {
        if (i != 0) rc = cf_builder_append(out, auto_span(","));
        if (rc == CF_OK) rc = auto_user_json_entry(out, &mentions[i]);
    }
    if (rc == CF_OK) rc = cf_builder_append(out, auto_span("]"));
    return rc;
}

/* INTEGRATOR SHIM R1 (see the header): the future
 * cf_view_autocompletable_users_index call (no layout). */
static cf_err auto_render_html(const cf_view_ctx *view_ctx,
                               const struct auto_mention *mentions,
                               size_t mentions_len, int64_t total,
                               cf_builder *out) {
    (void)total;
    cf_view_mention_user *users = NULL;
    if (mentions_len != 0) {
        users = calloc(mentions_len, sizeof *users);
        if (users == NULL) return CF_NOMEM;
        for (size_t i = 0; i < mentions_len; i++) {
            users[i].id = mentions[i].id;
            users[i].name = (cf_str){mentions[i].name.ptr,
                                    mentions[i].name.len};
            users[i].title = (cf_str){mentions[i].title.ptr,
                                      mentions[i].title.len};
            users[i].avatar_path = (cf_str){mentions[i].avatar_path.ptr,
                                            mentions[i].avatar_path.len};
            users[i].attachable_sgid =
                (cf_str){mentions[i].attachable_sgid.ptr,
                         mentions[i].attachable_sgid.len};
        }
    }
    cf_err rc = cf_view_autocompletable_users_index(view_ctx, users,
                                                    mentions_len, out);
    free(users);
    return rc;
}

/* ---- the ordered scope ----------------------------------------------------- */

static unsigned char auto_ascii_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

/* SQLite `LIKE` (case-insensitive, no ESCAPE): `%` spans anything, `_` one
 * character, other bytes match ASCII-case-insensitively. */
static bool auto_like_match(const unsigned char *text, size_t text_len,
                            const unsigned char *pattern,
                            size_t pattern_len) {
    if (pattern_len == 0) return true;
    if (pattern[0] == '%') {
        for (size_t skip = 0; skip <= text_len; skip++) {
            if (auto_like_match(text + skip, text_len - skip, pattern + 1,
                                pattern_len - 1)) {
                return true;
            }
        }
        return false;
    }
    if (text_len == 0) return false;
    if (pattern[0] == '_' ||
        auto_ascii_lower(pattern[0]) == auto_ascii_lower(text[0])) {
        return auto_like_match(text + 1, text_len - 1, pattern + 1,
                               pattern_len - 1);
    }
    return false;
}

static bool auto_name_matches(const cf_user *user, cf_span query) {
    if (query.len == 0) return true;
    if (user->name.ptr == NULL) return false;
    /* `name like '%<query>%'`: wrap the query in the % pair and run the
     * full LIKE matcher (which already tries every offset). */
    size_t plen = query.len + 2;
    if (plen < query.len) return false;
    unsigned char *pattern = malloc(plen);
    if (pattern == NULL) return false;
    pattern[0] = '%';
    memcpy(pattern + 1, query.ptr, query.len);
    pattern[plen - 1] = '%';
    bool match = auto_like_match((const unsigned char *)user->name.ptr,
                                 user->name.len, pattern, plen);
    free(pattern);
    return match;
}

static int auto_name_compare(const void *a, const void *b) {
    const cf_user *ua = a;
    const cf_user *ub = b;
    cf_span na = {(const unsigned char *)ua->name.ptr,
                  ua->name.ptr != NULL ? ua->name.len : 0};
    cf_span nb = {(const unsigned char *)ub->name.ptr,
                  ub->name.ptr != NULL ? ub->name.len : 0};
    /* `ORDER BY LOWER(name)`: SQLite lowercases ASCII only. */
    size_t n = na.len < nb.len ? na.len : nb.len;
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = auto_ascii_lower(na.ptr[i]);
        unsigned char cb = auto_ascii_lower(nb.ptr[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (na.len < nb.len) return -1;
    if (na.len > nb.len) return 1;
    return 0;
}

/* The room-scoped ordered rows: room members, active only, filtered by the
 * query, ordered by LOWER(name). */
static cf_err auto_room_users(cf_db *db, const cf_room *room, cf_span query,
                              cf_user_vector *out) {
    memset(out, 0, sizeof *out);
    cf_user_vector members = {0};
    cf_err rc = cf_room_users(db, room, &members);
    if (rc != CF_OK) return rc;
    cf_user_vector kept = {0};
    for (size_t i = 0; i < members.len && rc == CF_OK; i++) {
        if (!cf_user_is_active(&members.items[i])) continue;
        if (!auto_name_matches(&members.items[i], query)) continue;
        if (kept.len == kept.cap) {
            size_t cap = kept.cap == 0 ? 16 : kept.cap * 2;
            cf_user *items = realloc(kept.items, cap * sizeof *items);
            if (items == NULL) {
                rc = CF_NOMEM;
                break;
            }
            kept.items = items;
            kept.cap = cap;
        }
        kept.items[kept.len++] = members.items[i];
        memset(&members.items[i], 0, sizeof members.items[i]);
    }
    for (size_t i = 0; i < members.len; i++) {
        cf_user_dispose(&members.items[i]);
    }
    free(members.items);
    if (rc != CF_OK) {
        for (size_t i = 0; i < kept.len; i++) cf_user_dispose(&kept.items[i]);
        free(kept.items);
        return rc;
    }
    qsort(kept.items, kept.len, sizeof kept.items[0], auto_name_compare);
    *out = kept;
    return CF_OK;
}

/* ---- action ----------------------------------------------------------------- */

cf_err cf_action_autocompletable_users_index(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    /* `require_current_user`: the chain guarantees an identity; a missing
     * row is the reference's internal error. */
    bool identity_found = false;
    cf_user current = {0};
    rc = cf_auth_current_user(ctx, &identity_found, &current);
    if (rc != CF_OK) {
        cf_user_dispose(&current);
        return rc;
    }
    if (!identity_found) {
        cf_user_dispose(&current);
        return CF_INTERNAL;
    }
    int64_t current_id = current.id;
    cf_user_dispose(&current);

    /* `params[:room_id].present? ? Current.user.rooms.find(...) : User.all`.
     * A present but non-string/non-castable id, or a room outside the
     * memberships, is 404 (RecordNotFound): `as_str` is Some only for a
     * string param, so numbers never cast. */
    bool scoped = false;
    cf_room room = {0};
    const cf_param *room_param = cf_ctx_param(ctx, auto_span("room_id"));
    if (auto_param_present(room_param)) {
        cf_span text = {NULL, 0};
        bool castable =
            auto_param_str(ctx, "room_id", &text) && text.ptr != NULL;
        int64_t room_id = 0;
        if (castable) castable = cf_views_integer_cast(text, &room_id);
        bool found = false;
        if (castable) {
            rc = cf_room_find_for_user(ctx->reader, current_id, room_id,
                                       &found, &room);
            if (rc != CF_OK) return rc;
        }
        if (!castable || !found) {
            cf_room_dispose(&room);
            return CF_NOT_FOUND;
        }
        scoped = true;
    }

    /* `params[:filter].presence || params[:query].presence`: the first
     * present param wins; an upload is present but not stringable, so the
     * whole query reads as absent (the reference's `and_then(to_s)`). */
    cf_span query = {NULL, 0};
    bool has_query = false;
    static const char *const query_keys[] = {"filter", "query"};
    for (size_t i = 0; i < 2; i++) {
        const cf_param *param = cf_ctx_param(ctx, auto_span(query_keys[i]));
        if (!auto_param_present(param)) continue;
        cf_span text = {NULL, 0};
        if (!auto_param_to_s(param, &text)) break;
        if (text.ptr == NULL && text.len != 0) break;
        query = text;
        has_query = text.len != 0;
        break;
    }

    /* `find_autocompletable_users.with_attached_avatar.ordered`. */
    cf_user_vector users = {0};
    if (scoped) {
        rc = auto_room_users(ctx->reader, &room, query, &users);
        cf_room_dispose(&room);
    } else if (has_query) {
        cf_str pattern = {(char *)(uintptr_t)query.ptr, query.len};
        rc = cf_user_active_filtered_by_ordered(ctx->reader, pattern, &users);
    } else {
        rc = cf_user_active_ordered(ctx->reader, &users);
    }
    if (rc != CF_OK) {
        cf_user_vector_dispose(&users);
        return rc;
    }
    int64_t records_count = (int64_t)users.len;

    /* `Page::new(params[:page], count, [20])` + the page's slice. */
    int64_t page_number = auto_page_number(ctx);
    int64_t offset = (page_number - 1) * AUTO_PER_PAGE;
    size_t start =
        offset > records_count ? (size_t)records_count : (size_t)offset;
    size_t end = start + (size_t)AUTO_PER_PAGE;
    if (end > users.len) end = users.len;

    struct auto_mention *mentions = NULL;
    size_t mentions_len = end - start;
    if (mentions_len != 0) {
        mentions = calloc(mentions_len, sizeof *mentions);
        if (mentions == NULL) {
            cf_user_vector_dispose(&users);
            return CF_NOMEM;
        }
        for (size_t i = 0; i < mentions_len && rc == CF_OK; i++) {
            rc = auto_mention_user(ctx, &users.items[start + i],
                                   &mentions[i]);
        }
    }
    cf_user_vector_dispose(&users);
    if (rc != CF_OK) {
        if (mentions != NULL) {
            for (size_t i = 0; i < mentions_len; i++) {
                auto_mention_dispose(&mentions[i]);
            }
            free(mentions);
        }
        return rc;
    }

    const cf_format *offered[2] = {&cf_format_html, &cf_format_json};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 2, &chosen);
    if (rc != CF_OK) {
        for (size_t i = 0; i < mentions_len; i++) {
            auto_mention_dispose(&mentions[i]);
        }
        free(mentions);
        return rc;
    }

    /* `page.apply_headers`: JSON only (first request format is JSON exactly
     * when the negotiation chose it). */
    if (chosen == &cf_format_json) {
        rc = auto_apply_headers(ctx, records_count, page_number);
        if (rc != CF_OK) {
            for (size_t i = 0; i < mentions_len; i++) {
                auto_mention_dispose(&mentions[i]);
            }
            free(mentions);
            return rc;
        }
        cf_builder body = {0};
        rc = auto_users_index_json(mentions, mentions_len, &body);
        for (size_t i = 0; i < mentions_len; i++) {
            auto_mention_dispose(&mentions[i]);
        }
        free(mentions);
        if (rc != CF_OK) {
            cf_builder_dispose(&body);
            return rc;
        }
        return auto_render_as(ctx, 200,
                              auto_span("application/json; charset=utf-8"),
                              &body);
    }

    /* HTML: `render layout: false` (no Link preload header). */
    cf_view_layout_model layout = {0};
    rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) {
        for (size_t i = 0; i < mentions_len; i++) {
            auto_mention_dispose(&mentions[i]);
        }
        free(mentions);
        return rc;
    }
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);
    cf_builder body = {0};
    rc = auto_render_html(&view_ctx, mentions, mentions_len, records_count,
                          &body);
    for (size_t i = 0; i < mentions_len; i++) {
        auto_mention_dispose(&mentions[i]);
    }
    free(mentions);
    cf_view_layout_model_dispose(&layout);
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        return rc;
    }
    ctx->response->status = 200;
    cf_buf *buf = NULL;
    rc = cf_builder_freeze(&body, &buf);
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        return rc;
    }
    rc = cf_response_body(ctx->response, buf);
    cf_buf_release(buf);
    if (rc != CF_OK) return rc;
    rc = cf_response_header(ctx->response, auto_span("Content-Type"),
                            auto_span("text/html; charset=utf-8"));
    if (rc == CF_OK && cf_ctx_vary_accept(ctx)) {
        rc = cf_response_header(ctx->response, auto_span("Vary"),
                                auto_span("Accept"));
    }
    return rc;
}
