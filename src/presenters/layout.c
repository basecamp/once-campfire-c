/* src/presenters/layout.c — `Layout::load` / `ViewContext` construction
 * (tmp/rust-ref/crates/campfire/src/controllers/presenters/view_context.rs).
 *
 * The presenter reads rows inside one read transaction and assembles the
 * owned layout model; cf_view_ctx_init then adds the request state (flash,
 * base URL) without touching the database.  Rendering performs no SQL.
 */
#include "views.h"
#include "views/internal.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "db/db_internal.h"
#include "models/account.h"
#include "models/active_storage.h"
#include "models/room.h"
#include "models/user.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Owned NUL-terminated copy of a span (the model layer's cf_str contract). */
static cf_err str_dup(cf_span span, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (span.len != 0 && span.ptr == NULL) return CF_INVALID;
    char *copy = malloc(span.len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (span.len != 0) memcpy(copy, span.ptr, span.len);
    copy[span.len] = '\0';
    out->ptr = copy;
    out->len = span.len;
    return CF_OK;
}

static cf_err copy_optional(cf_str *out, cf_optional_str value) {
    memset(out, 0, sizeof *out);
    if (!value.present) return CF_OK;
    if (value.value.len == 0) {
        out->ptr = malloc(1);
        if (out->ptr == NULL) return CF_NOMEM;
        out->ptr[0] = '\0';
        out->len = 0;
        return CF_OK;
    }
    return str_dup((cf_span){(const unsigned char *)value.value.ptr,
                          value.value.len},
                 out);
}

static void str_clear(cf_str *value) {
    free(value->ptr);
    memset(value, 0, sizeof *value);
}

void cf_view_layout_model_dispose(cf_view_layout_model *layout) {
    if (layout == NULL) return;
    str_clear(&layout->current_user.name);
    str_clear(&layout->current_user.avatar_url);
    str_clear(&layout->account.name);
    str_clear(&layout->account.logo_url);
    str_clear(&layout->custom_styles);
    str_clear(&layout->vapid_public_key);
    str_clear(&layout->app_version);
    memset(layout, 0, sizeof *layout);
}

void cf_view_help_contact_dispose(cf_view_help_contact *contact) {
    if (contact == NULL) return;
    str_clear(&contact->name);
    str_clear(&contact->email_address);
}

/* `Time#to_fs(:number)`: %Y%m%d%H%M%S in UTC. */
static void to_fs_number(int64_t us, char out[16]) {
    time_t seconds = (time_t)(us / 1000000);
    struct tm tm;
    if (gmtime_r(&seconds, &tm) == NULL) {
        memcpy(out, "00000000000000", 15);
        return;
    }
    strftime(out, 16, "%Y%m%d%H%M%S", &tm);
}

/* `fresh_account_logo_path(account, None)`: "/account/logo" with `?v=` only
 * when the account exists. */
static cf_err account_logo_url(const cf_account *account, cf_str *out) {
    memset(out, 0, sizeof *out);
    char number[16];
    const char *suffix;
    if (account != NULL) {
        to_fs_number(account->updated_at, number);
        /* "/account/logo?v=" + 14 digits */
        size_t len = strlen("/account/logo?v=") + 14;
        out->ptr = malloc(len + 1);
        if (out->ptr == NULL) return CF_NOMEM;
        memcpy(out->ptr, "/account/logo?v=", strlen("/account/logo?v="));
        memcpy(out->ptr + strlen("/account/logo?v="), number, 15);
        out->len = len;
        return CF_OK;
    }
    suffix = "/account/logo";
    out->ptr = malloc(strlen(suffix) + 1);
    if (out->ptr == NULL) return CF_NOMEM;
    memcpy(out->ptr, suffix, strlen(suffix) + 1);
    out->len = strlen(suffix);
    return CF_OK;
}

/* `fresh_user_avatar_path(user)`: signed avatar token + updated_at number. */
static cf_err user_avatar_url(const cf_app *app, const cf_user *user,
                              cf_str *out) {
    memset(out, 0, sizeof *out);
    const cf_config *config = cf_app_config(app);
    if (config == NULL || config->secret_key_base == NULL) return CF_OK;
    cf_str token = {0};
    cf_err rc = cf_auth_signed_id_generate(
        (cf_span){(const unsigned char *)config->secret_key_base,
                  config->secret_key_base_len},
        (cf_span){(const unsigned char *)"User", 4},
        user->id, (cf_span){(const unsigned char *)"avatar", 6}, true, false,
        0, &token);
    if (rc != CF_OK) return rc;
    char number[16];
    to_fs_number(user->updated_at, number);
    const char *prefix = "/users/";
    const char *middle = "/avatar?v=";
    size_t len = strlen(prefix) + token.len + strlen(middle) + 14;
    out->ptr = malloc(len + 1);
    if (out->ptr == NULL) {
        cf_str_dispose(&token);
        return CF_NOMEM;
    }
    size_t at = 0;
    memcpy(out->ptr + at, prefix, strlen(prefix));
    at += strlen(prefix);
    memcpy(out->ptr + at, token.ptr, token.len);
    at += token.len;
    memcpy(out->ptr + at, middle, strlen(middle));
    at += strlen(middle);
    memcpy(out->ptr + at, number, 15);
    at += 14;
    out->ptr[at] = '\0';
    out->len = at;
    cf_str_dispose(&token);
    return CF_OK;
}

static cf_err load_last_room(cf_ctx *ctx, int64_t user_id, bool *found,
                             int64_t *room_id) {
    *found = false;
    cf_span cookie;
    cf_err rc = cf_ctx_cookie_get(ctx, (cf_span){(const unsigned char *)"last_room", 9},
                                  &cookie);
    if (rc == CF_NOT_FOUND) {
        cookie = (cf_span){NULL, 0};
        rc = CF_OK;
    }
    if (rc != CF_OK) return rc;
    int64_t requested = 0;
    /* The same ruby_compat::integer_cast the welcome action applies to this
     * cookie (cf_views_integer_cast), so both paths select the same room. */
    bool has_requested =
        cookie.len != 0 && cf_views_integer_cast(cookie, &requested);
    if (has_requested) {
        cf_room room = {0};
        bool room_found = false;
        rc = cf_room_find_for_user(ctx->reader, user_id, requested, &room_found,
                                   &room);
        cf_room_dispose(&room);
        if (rc != CF_OK) return rc;
        if (room_found) {
            *found = true;
            *room_id = requested;
            return CF_OK;
        }
    }
    cf_room room = {0};
    bool room_found = false;
    rc = cf_room_original_for_user(ctx->reader, user_id, &room_found, &room);
    if (rc == CF_OK && room_found) {
        *found = true;
        *room_id = room.id;
    }
    cf_room_dispose(&room);
    return rc;
}

cf_err cf_presenter_layout_load(cf_ctx *ctx, const cf_view_platform *platform,
                                cf_view_layout_model *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (platform != NULL) out->platform = *platform;

    const cf_config *config = cf_app_config(ctx->app);
    if (config != NULL && config->vapid_public_key != NULL &&
        config->vapid_public_key[0] != '\0') {
        out->has_vapid_public_key = true;
        if (str_dup((cf_span){(const unsigned char *)config->vapid_public_key,
                              strlen(config->vapid_public_key)},
                    &out->vapid_public_key) != CF_OK) {
            goto nomem;
        }
    }
    if (str_dup((cf_span){(const unsigned char *)CF_VIEWS_APP_VERSION,
                          strlen(CF_VIEWS_APP_VERSION)},
                &out->app_version) != CF_OK) {
        goto nomem;
    }

    cf_err rc = cf_read_begin(ctx->reader);
    if (rc != CF_OK) {
        cf_view_layout_model_dispose(out);
        return rc;
    }

    /* Current.user */
    if (ctx->identity.kind != CF_AUTH_NONE && ctx->identity.user_id != 0) {
        cf_user user = {0};
        bool found = false;
        rc = cf_user_find(ctx->reader, ctx->identity.user_id, &user);
        if (rc == CF_NOT_FOUND) {
            rc = CF_OK;
        } else if (rc != CF_OK) {
            goto fail;
        } else {
            found = true;
        }
        if (found) {
            out->current_user.has_user = true;
            out->current_user.id = user.id;
            out->current_user.administrator =
                user.role == CF_ROLE_ADMINISTRATOR;
            out->current_user.bot = user.role == CF_ROLE_BOT;
            if (str_dup((cf_span){(const unsigned char *)user.name.ptr,
                                  user.name.len},
                        &out->current_user.name) != CF_OK) {
                cf_user_dispose(&user);
                rc = CF_NOMEM;
                goto fail;
            }
            if (user_avatar_url(ctx->app, &user, &out->current_user.avatar_url) !=
                CF_OK) {
                cf_user_dispose(&user);
                rc = CF_NOMEM;
                goto fail;
            }
            cf_user_dispose(&user);
        }
    }

    /* Current.account + logo attachment + custom styles */
    {
        cf_account account = {0};
        bool found = false;
        rc = cf_account_first(ctx->reader, &found, &account);
        if (rc != CF_OK) goto fail;
        if (found) {
            if (str_dup((cf_span){(const unsigned char *)account.name.ptr,
                                  account.name.len},
                        &out->account.name) != CF_OK) {
                cf_account_dispose(&account);
                rc = CF_NOMEM;
                goto fail;
            }
            if (account_logo_url(&account, &out->account.logo_url) != CF_OK) {
                cf_account_dispose(&account);
                rc = CF_NOMEM;
                goto fail;
            }
            cf_attachment attachment = {0};
            bool attached = false;
            rc = cf_attachment_find_for(ctx->reader, CF_STR_LIT("Account"),
                                        account.id, CF_STR_LIT("logo"),
                                        &attached, &attachment);
            cf_attachment_dispose(&attachment);
            if (rc != CF_OK) {
                cf_account_dispose(&account);
                goto fail;
            }
            out->account.has_logo = attached;
            if (account.custom_styles.present) {
                out->has_custom_styles = true;
                if (copy_optional(&out->custom_styles,
                                  account.custom_styles) != CF_OK) {
                    cf_account_dispose(&account);
                    rc = CF_NOMEM;
                    goto fail;
                }
            }
            cf_account_dispose(&account);
        } else {
            /* AccountSummary with no account: an empty name (not NULL) and
             * the default logo path. */
            if (str_dup((cf_span){NULL, 0}, &out->account.name) != CF_OK) {
                rc = CF_NOMEM;
                goto fail;
            }
            if (account_logo_url(NULL, &out->account.logo_url) != CF_OK) {
                rc = CF_NOMEM;
                goto fail;
            }
        }
    }

    /* last_room_visited (only for an authenticated user) */
    if (out->current_user.has_user) {
        bool found = false;
        int64_t room_id = 0;
        rc = load_last_room(ctx, out->current_user.id, &found, &room_id);
        if (rc != CF_OK) goto fail;
        out->has_last_room_visited = found;
        out->last_room_visited_id = room_id;
    }

    rc = cf_read_end(ctx->reader);
    if (rc != CF_OK) {
        cf_view_layout_model_dispose(out);
        return rc;
    }
    return CF_OK;
fail:
    {
        cf_err end_rc = cf_read_end(ctx->reader);
        (void)end_rc;
    }
    cf_view_layout_model_dispose(out);
    return rc;
nomem:
    cf_view_layout_model_dispose(out);
    return CF_NOMEM;
}
