/* src/richtext/rt_resolver.c — the production AttachableResolver.
 *
 * Ports crates/campfire/src/controllers/presenters/rich_text.rs (DbResolver):
 * GlobalID::Locator.locate_signed(sgid, for: "attachable") through A01's
 * verifier and GlobalID.find through the D01 user model, over the caller's
 * borrowed connection (never a second checkout).
 */
#include "richtext/internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth.h"
#include "models/user.h"

#define SPAN_LIT(literal) ((cf_span){(const unsigned char *)(literal), sizeof(literal) - 1})

/* Rust `id.parse::<i64>()`: optional sign, ASCII digits, nothing else. */
static bool strict_i64(const char *text, int64_t *out) {
    if (text == NULL || *text == '\0') return false;
    const char *at = text;
    bool negative = false;
    if (*at == '+' || *at == '-') {
        negative = *at == '-';
        at++;
    }
    if (*at == '\0') return false;
    uint64_t value = 0;
    for (; *at != '\0'; at++) {
        if (*at < '0' || *at > '9') return false;
        unsigned digit = (unsigned)(*at - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (negative) {
        if (value > (uint64_t)INT64_MAX + 1) return false;
        *out = value == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)value;
    } else {
        if (value > (uint64_t)INT64_MAX) return false;
        *out = (int64_t)value;
    }
    return true;
}

static char *dup_str(const char *text) {
    if (text == NULL) return NULL;
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (copy != NULL) memcpy(copy, text, len + 1);
    return copy;
}

static rt_status write_id(rt_buf *out, int64_t id) {
    return rt_buf_printf(out, "%lld", (long long)id);
}

/* `fresh_user_avatar_path(user)`: signed id + updated_at.to_fs(:number). */
static char *avatar_path_for(const rt_db_resolver *resolver, const cf_user *user) {
    cf_str token = {0};
    if (cf_auth_signed_id_generate(resolver->secret_key_base, SPAN_LIT("User"), user->id,
                                   SPAN_LIT("avatar"), true, false, 0, &token) != CF_OK) {
        return NULL;
    }
    time_t seconds = (time_t)(user->updated_at / INT64_C(1000000));
    struct tm utc;
    if (gmtime_r(&seconds, &utc) == NULL) {
        cf_str_dispose(&token);
        return NULL;
    }
    rt_buf path;
    rt_buf_init(&path);
    rt_status rc = rt_buf_puts(&path, "/users/");
    if (rc == RT_OK) rc = rt_buf_append(&path, (const unsigned char *)token.ptr, token.len);
    if (rc == RT_OK) {
        rc = rt_buf_printf(&path, "/avatar?v=%04d%02d%02d%02d%02d%02d", utc.tm_year + 1900,
                           utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    }
    cf_str_dispose(&token);
    char *result = NULL;
    if (rc == RT_OK) rc = rt_buf_to_cstr(&path, &result);
    rt_buf_dispose(&path);
    return rc == RT_OK ? result : NULL;
}

static bool mention_user_from_row(const rt_db_resolver *resolver, const cf_user *user,
                                  rt_mention_user *out) {
    memset(out, 0, sizeof *out);
    out->id = user->id;
    out->name = dup_str(user->name.ptr);
    cf_str title = {0};
    if (cf_user_title(user, &title) == CF_OK) {
        out->title = title.ptr;
    } else {
        cf_str_dispose(&title);
    }
    rt_buf gid;
    rt_buf_init(&gid);
    bool ok = out->name != NULL;
    if (ok && rt_buf_printf(&gid, "gid://campfire/User/%lld", (long long)user->id) != RT_OK) ok = false;
    cf_str sgid = {0};
    if (ok && cf_auth_sgid_generate_attachable(resolver->secret_key_base,
                                               (cf_span){gid.data != NULL ? gid.data : (unsigned char *)"",
                                                         gid.data != NULL ? gid.len : 0},
                                               &sgid) != CF_OK) {
        ok = false;
    }
    rt_buf_dispose(&gid);
    if (ok) {
        out->attachable_sgid = sgid.ptr;
    } else {
        cf_str_dispose(&sgid);
    }
    rt_buf user_path;
    rt_buf_init(&user_path);
    if (ok) {
        if (rt_buf_puts(&user_path, "/users/") != RT_OK ||
            write_id(&user_path, user->id) != RT_OK ||
            rt_buf_to_cstr(&user_path, &out->user_path) != RT_OK) {
            ok = false;
        }
    }
    rt_buf_dispose(&user_path);
    if (ok) {
        out->avatar_path = avatar_path_for(resolver, user);
        if (out->avatar_path == NULL) ok = false;
    }
    if (!ok) {
        rt_mention_user_dispose(out);
        return false;
    }
    return true;
}

static rt_signed_lookup locate_signed(rt_resolver *self, const unsigned char *sgid, size_t len) {
    rt_db_resolver *resolver = (rt_db_resolver *)self;
    rt_signed_lookup lookup;
    memset(&lookup, 0, sizeof lookup);
    lookup.kind = RT_SIGNED_INVALID;
    if (resolver->secret_key_base.len == 0) return lookup;
    cf_str gid = {0};
    bool found = false;
    if (cf_auth_sgid_locate(resolver->secret_key_base, (cf_span){sgid, len},
                            SPAN_LIT("attachable"), resolver->now_us, &gid, &found) != CF_OK ||
        !found) {
        cf_str_dispose(&gid);
        return lookup;
    }
    cf_str app = {0}, model = {0}, id = {0};
    bool parsed = false;
    if (cf_auth_global_id_parse((cf_span){(const unsigned char *)gid.ptr, gid.len}, &app, &model, &id,
                                &parsed) != CF_OK ||
        !parsed) {
        cf_str_dispose(&app);
        cf_str_dispose(&model);
        cf_str_dispose(&id);
        cf_str_dispose(&gid);
        return lookup;
    }
    char *model_text = model.ptr != NULL ? dup_str(model.ptr) : NULL;
    char *id_text = id.ptr != NULL ? dup_str(id.ptr) : NULL;
    cf_str_dispose(&app);
    cf_str_dispose(&model);
    cf_str_dispose(&id);
    cf_str_dispose(&gid);
    if (model_text == NULL || id_text == NULL) {
        free(model_text);
        free(id_text);
        return lookup;
    }
    if (strcmp(model_text, "User") == 0) {
        int64_t user_id = 0;
        bool have_id = strict_i64(id_text, &user_id);
        bool user_found = false;
        cf_user user = {0};
        if (have_id && cf_user_find_by_id(resolver->db, user_id, &user_found, &user) == CF_OK &&
            user_found) {
            if (mention_user_from_row(resolver, &user, &lookup.user)) {
                lookup.kind = RT_SIGNED_USER;
            }
            cf_user_dispose(&user);
        } else {
            cf_user_dispose(&user);
            lookup.kind = RT_SIGNED_MISSING;
            lookup.model_name = model_text;
            model_text = NULL;
        }
    } else {
        lookup.kind = RT_SIGNED_MISSING;
        lookup.model_name = model_text;
        model_text = NULL;
    }
    free(model_text);
    free(id_text);
    return lookup;
}

static rt_gid_lookup find_gid(rt_resolver *self, const unsigned char *gid, size_t len) {
    rt_db_resolver *resolver = (rt_db_resolver *)self;
    rt_gid_lookup lookup;
    memset(&lookup, 0, sizeof lookup);
    lookup.kind = RT_GID_NOT_FOUND;
    cf_str app = {0}, model = {0}, id = {0};
    bool parsed = false;
    if (cf_auth_global_id_parse((cf_span){gid, len}, &app, &model, &id, &parsed) != CF_OK || !parsed) {
        cf_str_dispose(&app);
        cf_str_dispose(&model);
        cf_str_dispose(&id);
        return lookup;
    }
    char *model_text = model.ptr != NULL ? dup_str(model.ptr) : NULL;
    char *id_text = id.ptr != NULL ? dup_str(id.ptr) : NULL;
    cf_str_dispose(&app);
    cf_str_dispose(&model);
    cf_str_dispose(&id);
    if (model_text == NULL) {
        free(id_text);
        return lookup;
    }
    if (strcmp(model_text, "User") == 0 && id_text != NULL) {
        int64_t user_id = 0;
        bool user_found = false;
        cf_user user = {0};
        if (strict_i64(id_text, &user_id) &&
            cf_user_find_by_id(resolver->db, user_id, &user_found, &user) == CF_OK && user_found) {
            if (mention_user_from_row(resolver, &user, &lookup.user)) {
                lookup.kind = RT_GID_USER;
            }
        }
        cf_user_dispose(&user);
    } else if (strcmp(model_text, "Message") == 0 || strcmp(model_text, "Room") == 0 ||
               strcmp(model_text, "Rooms::Open") == 0 || strcmp(model_text, "Rooms::Closed") == 0 ||
               strcmp(model_text, "Rooms::Direct") == 0 || strcmp(model_text, "Boost") == 0 ||
               strcmp(model_text, "Account") == 0) {
        lookup.kind = RT_GID_OTHER;
    } else {
        lookup.kind = RT_GID_RAISES;
    }
    free(model_text);
    free(id_text);
    return lookup;
}

rt_resolver *rt_db_resolver_init(rt_db_resolver *storage, cf_db *db, cf_span secret_key_base,
                                 int64_t now_us) {
    memset(storage, 0, sizeof *storage);
    storage->base.locate_signed = locate_signed;
    storage->base.find_gid = find_gid;
    storage->db = db;
    storage->secret_key_base = secret_key_base;
    storage->now_us = now_us;
    return &storage->base;
}
