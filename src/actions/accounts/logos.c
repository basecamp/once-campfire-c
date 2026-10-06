/* src/actions/accounts/logos.c — Accounts::LogosController (packet
 * A-accounts-logos; route IDs 38 `show`, 39 `destroy`;
 * contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/accounts/logos.rs,
 * the pinned port of reference/app/controllers/accounts/logos_controller.rb:
 *
 *   show:    use_live_response; before_actions(allow_unauthenticated);
 *            account = Account.first (nil allowed); stale?(etag: account)
 *            answers 304; expires_in 5.minutes public stale_while_revalidate
 *            1.week; size = params[:size] == "small" ? 192 : 512; the
 *            logo.variant(size).processed when variable, else the stock icon
 *            (logos/app-icon[-192].png); send_file inline image/png.
 *   destroy: use_live_response; before_actions(Before::default());
 *            ensure_can_administer; Current.account;
 *            write attachments::destroy(Account, "logo");
 *            redirect_to edit_account.
 *
 * Callback order, cache headers, size selection and row effects are the
 * reference's. Logo URLs stay signed/cached through S02: an attached logo
 * uses the bounded production representation processor. Nonvariable
 * attachments fall back to the stock icon. The stock fallback resolves through the
 * landed asset manifest (cf_views_asset_path) to the pinned bytes.
 *
 * ETag note (D-C03 allows differing validator values): the reference
 * combines the account cache key with the template digest, Turbo-Frame
 * state and flash; this port emits the weak validator
 * W/"accounts/<id>-<updated_at_us>" and answers 304 on an exact (or "*")
 * If-None-Match. Conditional semantics are preserved.
 *
 * SHIM: `belongs_to :record, touch: true` for accounts is fixed SQL
 * (duplicated from the accounts packet; packets cannot share code through
 * shared files). Integrator request: a model or presenter touch helper.
 *
 * Integrator requests:
 *  1. Rebind src/routes.c rows 38 -> cf_action_accounts_logos_show,
 *     39 -> cf_action_accounts_logos_destroy, e.g.:
 *       {38, CF_GET, "/account/logo(.:format)", ...,
 *        cf_action_accounts_logos_show},
 *       {39, CF_DELETE, "/account/logo(.:format)", ...,
 *        cf_action_accounts_logos_destroy},
 *  2. Model/presenter account touch helper.
 *
 * c_symbols: cf_action_accounts_logos_show,
 * cf_action_accounts_logos_destroy.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "models/account.h"
#include "models/active_storage.h"
#include "models/user.h"
#include "routes.h"
#include "views.h"
#include "storage/active_storage.h"
#include "storage/storage.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOGOS_MAX_AGE_SECS 300
#define LOGOS_STALE_WHILE_REVALIDATE_SECS 604800

static cf_span logos_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static cf_str logos_cstr(const char *text) {
    return (cf_str){(char *)(uintptr_t)text, strlen(text)};
}

/* `c.param_str(key)`: Some only for a string param. */
static bool logos_param_str(cf_ctx *ctx, const char *name, cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, logos_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) return false;
    return cf_param_string(param, out) == CF_OK;
}

/* First request header value with a case-insensitive name match. */
static bool logos_request_header(const cf_request *request, const char *name,
                                 cf_span *out) {
    if (request == NULL) return false;
    size_t name_len = strlen(name);
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != name_len) continue;
        bool match = true;
        for (size_t k = 0; k < name_len; k++) {
            unsigned char a = header->name.ptr[k];
            unsigned char b = (unsigned char)name[k];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) {
            *out = header->value;
            return true;
        }
    }
    return false;
}

/* `ensure_can_administer`: 403 head unless the current user can administer
 * (no record). */
static cf_err logos_ensure_can_administer(cf_ctx *ctx, int64_t *actor_id) {
    bool found = false;
    cf_user current = {0};
    cf_err rc = cf_auth_current_user(ctx, &found, &current);
    bool allowed =
        rc == CF_OK && found &&
        cf_user_can_administer(&current, (cf_optional_i64){false, 0}, false);
    if (allowed && actor_id != NULL) *actor_id = current.id;
    cf_user_dispose(&current);
    if (rc != CF_OK) return rc;
    if (!allowed) {
        ctx->response->status = 403;
        const cf_format *format = cf_ctx_rendered_format(ctx);
        return cf_response_header(ctx->response,
                                  logos_span("Content-Type"),
                                  logos_span(format->string));
    }
    return CF_OK;
}

/* `Current.account` dereference: Account::first, internal error when absent
 * (the reference raises NoMethodError on nil). */
static cf_err logos_current_account(cf_ctx *ctx, cf_account *out) {
    bool found = false;
    memset(out, 0, sizeof *out);
    cf_err rc = cf_account_first(ctx->reader, &found, out);
    if (rc != CF_OK) {
        cf_account_dispose(out);
        return rc;
    }
    if (!found) {
        cf_account_dispose(out);
        return CF_INTERNAL;
    }
    return CF_OK;
}

/* In-transaction administrator revalidation (02-data-auth.md D02). */
static cf_err logos_revalidate_admin(cf_tx *tx, int64_t actor_id) {
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    bool found = false;
    cf_user actor = {0};
    cf_err rc = cf_user_find_by_id(db, actor_id, &found, &actor);
    if (rc != CF_OK) {
        cf_user_dispose(&actor);
        return rc;
    }
    bool allowed = found && cf_user_is_active(&actor) &&
                   cf_user_can_administer(&actor, (cf_optional_i64){false, 0},
                                          false);
    cf_user_dispose(&actor);
    return allowed ? CF_OK : CF_FORBIDDEN;
}

/* ---- show ----------------------------------------------------------------- */

/* The weak ETag for an account: W/"accounts/<id>-<updated_at_us>". */
static cf_err logos_etag(int64_t account_id, int64_t updated_at_us,
                         char *out, size_t cap) {
    int n = snprintf(out, cap, "W/\"accounts/%" PRId64 "-%" PRId64 "\"",
                     account_id, updated_at_us);
    if (n < 0 || (size_t)n >= cap) return CF_INTERNAL;
    return CF_OK;
}

/* `fresh_when`: a matching If-None-Match entry (or "*") is fresh. */
static bool logos_not_modified(const cf_request *request, cf_span etag) {
    cf_span header = {NULL, 0};
    if (!logos_request_header(request, "If-None-Match", &header)) return false;
    size_t i = 0;
    while (i < header.len) {
        while (i < header.len &&
               (header.ptr[i] == ' ' || header.ptr[i] == '\t' ||
                header.ptr[i] == ',')) {
            i++;
        }
        size_t start = i;
        while (i < header.len && header.ptr[i] != ',') i++;
        size_t end = i;
        while (end > start && (header.ptr[end - 1] == ' ' ||
                               header.ptr[end - 1] == '\t')) {
            end--;
        }
        size_t len = end - start;
        if (len == 1 && header.ptr[start] == '*') return true;
        if (len == etag.len && memcmp(header.ptr + start, etag.ptr, len) == 0) {
            return true;
        }
    }
    return false;
}

/* `expires_in 5.minutes, public: true, stale_while_revalidate: 1.week`. */
static cf_err logos_cache_header(cf_ctx *ctx) {
    char value[96];
    int n = snprintf(value, sizeof value,
                     "public, max-age=%d, stale-while-revalidate=%d",
                     LOGOS_MAX_AGE_SECS, LOGOS_STALE_WHILE_REVALIDATE_SECS);
    if (n < 0 || (size_t)n >= sizeof value) return CF_INTERNAL;
    return cf_response_header(ctx->response, logos_span("Cache-Control"),
                              logos_span(value));
}

/* The stock icon file for `send_stock_icon`: the pinned logical asset
 * resolved through the manifest, read under the static root. */
static cf_err logos_stock_file(bool small, int *out_fd, uint64_t *out_size) {
    *out_fd = -1;
    *out_size = 0;
    cf_builder url = {0};
    cf_err rc = cf_views_asset_path(
        logos_span(small ? "logos/app-icon-192.png" : "logos/app-icon.png"),
        &url);
    if (rc != CF_OK) {
        cf_builder_dispose(&url);
        return rc;
    }
    const char *root = cf_static_root();
    if (root == NULL) {
        cf_builder_dispose(&url);
        return CF_INTERNAL;
    }
    cf_builder path = {0};
    rc = cf_builder_append(&path, logos_span(root));
    if (rc == CF_OK) rc = cf_builder_append(&path, logos_span("/public"));
    if (rc == CF_OK) {
        rc = cf_builder_append(&path, (cf_span){url.ptr, url.len});
    }
    cf_builder_dispose(&url);
    if (rc != CF_OK) {
        cf_builder_dispose(&path);
        return rc;
    }
    char *cpath = malloc(path.len + 1);
    if (cpath == NULL) {
        cf_builder_dispose(&path);
        return CF_NOMEM;
    }
    memcpy(cpath, path.ptr, path.len);
    cpath[path.len] = '\0';
    cf_builder_dispose(&path);
    int fd = open(cpath, O_RDONLY | O_NOFOLLOW);
    free(cpath);
    if (fd < 0) return CF_INTERNAL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(fd);
        return CF_INTERNAL;
    }
    *out_fd = fd;
    *out_size = (uint64_t)st.st_size;
    return CF_OK;
}

static cf_err logos_variant_file(cf_ctx *ctx, const cf_blob *blob, bool small,
                                  int *fd, uint64_t *size) {
    cf_active_ventries variation = {0};
    cf_active_vval *resize = NULL, *value = NULL;
    cf_err rc = cf_active_varr(&resize);
    if (rc == CF_OK) rc = cf_active_vint(small ? 192 : 512, &value);
    if (rc == CF_OK) { rc = cf_active_varr_push(resize, value); if (rc != CF_OK) cf_active_vval_dispose(value); }
    value = NULL;
    if (rc == CF_OK) rc = cf_active_vint(small ? 192 : 512, &value);
    if (rc == CF_OK) { rc = cf_active_varr_push(resize, value); if (rc != CF_OK) cf_active_vval_dispose(value); }
    if (rc == CF_OK) { rc = cf_active_ventries_push(&variation, logos_span("resize_to_limit"), resize); if (rc == CF_OK) resize = NULL; }
    cf_active_vval_dispose(resize);
    value = NULL;
    if (rc == CF_OK) rc = cf_active_vstr(logos_span("png"), &value);
    if (rc == CF_OK) { rc = cf_active_ventries_push(&variation, logos_span("format"), value); if (rc != CF_OK) cf_active_vval_dispose(value); }
    cf_blob image = {0};
    if (rc == CF_OK) rc = cf_active_processed_representation(ctx, blob, &variation, &image);
    cf_active_ventries_dispose(&variation);
    cf_storage *storage = NULL;
    const cf_config *config = cf_app_config(ctx->app);
    if (rc == CF_OK) rc = cf_storage_open(config->storage_path, &storage);
    if (rc == CF_OK) rc = cf_storage_open_read(storage, (cf_span){(unsigned char *)image.key.ptr, image.key.len}, fd, size);
    cf_storage_close(storage);
    cf_blob_dispose(&image);
    return rc;
}

cf_err cf_action_accounts_logos_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    /* `allow_unauthenticated_access only: :show`: the logo is the PWA icon.
     * use_live_response only changes default headers, which this action
     * sets explicitly. */
    cf_before policy = {CF_AUTH_SKIPPED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    /* Account::first as an Option: a missing account serves the stock icon
     * with no ETag. */
    bool has_account = false;
    cf_account account = {0};
    rc = cf_account_first(ctx->reader, &has_account, &account);
    if (rc != CF_OK) {
        cf_account_dispose(&account);
        return rc;
    }

    /* `stale?(etag: Current.account)`: 304 carries the ETag, like the
     * reference's fresh_when (which runs before expires_in). */
    char etagbuf[96];
    cf_span etag = {NULL, 0};
    if (has_account) {
        rc = logos_etag(account.id, account.updated_at, etagbuf,
                         sizeof etagbuf);
        if (rc != CF_OK) {
            cf_account_dispose(&account);
            return rc;
        }
        etag = logos_span(etagbuf);
        if (logos_not_modified(ctx->request, etag)) {
            cf_account_dispose(&account);
            ctx->response->status = 304;
            return cf_response_header(ctx->response, logos_span("ETag"),
                                      etag);
        }
    }
    rc = logos_cache_header(ctx);
    if (rc != CF_OK) {
        cf_account_dispose(&account);
        return rc;
    }
    if (has_account) {
        rc = cf_response_header(ctx->response, logos_span("ETag"), etag);
        if (rc != CF_OK) {
            cf_account_dispose(&account);
            return rc;
        }
    }

    cf_span size = {NULL, 0};
    bool small = logos_param_str(ctx, "size", &size) && size.len == 5 &&
                 memcmp(size.ptr, "small", 5) == 0;

    int fd = -1;
    uint64_t size_bytes = 0;
    bool have_variant = false;
    if (has_account) {
        bool found = false;
        cf_attachment attachment = {0};
        cf_blob blob = {0};
        rc = cf_attachment_find_for(ctx->reader, logos_cstr("Account"),
                                    account.id, logos_cstr("logo"), &found,
                                    &attachment);
        if (rc == CF_OK && found) rc = cf_attachment_blob(ctx->reader, &attachment, &blob);
        cf_attachment_dispose(&attachment);
        if (rc == CF_OK && found && blob.content_type.present &&
            cf_active_content_type_variable((cf_span){(unsigned char *)blob.content_type.value.ptr, blob.content_type.value.len})) {
            rc = logos_variant_file(ctx, &blob, small, &fd, &size_bytes);
            have_variant = rc == CF_OK;
        }
        cf_blob_dispose(&blob);
    }
    cf_account_dispose(&account);
    if (rc == CF_OK && !have_variant) rc = logos_stock_file(small, &fd, &size_bytes);
    if (rc != CF_OK) {
        if (fd >= 0) close(fd);
        return rc;
    }
    ctx->response->status = 200;
    rc = cf_response_header(ctx->response, logos_span("Content-Type"),
                            logos_span("image/png"));
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response,
                                logos_span("Content-Disposition"),
                                logos_span("inline"));
    }
    if (rc == CF_OK) rc = cf_response_file(ctx->response, fd, 0, size_bytes);
    if (rc != CF_OK && fd >= 0) close(fd);
    return rc;
}

/* ---- destroy --------------------------------------------------------------- */

/* SHIM: `belongs_to :record, touch: true` for accounts (see accounts.c). */
enum { LOGOS_STMT_TOUCH };
static const cf_stmt_def logos_stmt_defs[] = {
    [LOGOS_STMT_TOUCH] = {
        "UPDATE \"accounts\" SET \"updated_at\" = ? "
        "WHERE \"accounts\".\"id\" = ?"},
};
static const cf_stmt_set logos_stmt_set = {
    logos_stmt_defs, sizeof logos_stmt_defs / sizeof logos_stmt_defs[0]};

static cf_err logos_touch_account(cf_tx *tx, int64_t account_id) {
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) return rc;
    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &logos_stmt_set, LOGOS_STMT_TOUCH, &stmt);
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(
            stmt, 1,
            (cf_span){(const unsigned char *)now_text, strlen(now_text)});
    }
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 2, account_id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step != SQLITE_DONE) rc = cf_db_err(step);
    }
    if (stmt != NULL) cf_db_stmt_done(stmt);
    return rc;
}

typedef struct {
    int64_t actor_id;
    int64_t account_id;
} logos_write_arg;

static cf_err logos_write_cb(cf_tx *tx, void *arg) {
    const logos_write_arg *write = arg;
    cf_err rc = logos_revalidate_admin(tx, write->actor_id);
    if (rc != CF_OK) return rc;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    cf_account account = {0};
    rc = cf_account_find(db, write->account_id, &account);
    if (rc != CF_OK) {
        cf_account_dispose(&account);
        return rc;
    }
    /* `Current.account.logo.destroy`: the attachment is destroyed when
     * present and nothing happens without one
     * (`delegate_missing_to :attachment, allow_nil: true`). */
    bool found = false;
    cf_attachment attachment = {0};
    rc = cf_attachment_find_for(db, logos_cstr("Account"), account.id,
                                logos_cstr("logo"), &found, &attachment);
    if (rc != CF_OK) {
        cf_account_dispose(&account);
        return rc;
    }
    if (found) {
        int64_t blob_id = attachment.blob_id;
        rc = cf_attachment_delete(tx, &attachment);
        cf_attachment_dispose(&attachment);
        if (rc != CF_OK) {
            cf_account_dispose(&account);
            return rc;
        }
        rc = logos_touch_account(tx, account.id);
        if (rc != CF_OK) {
            cf_account_dispose(&account);
            return rc;
        }
        cf_event event;
        memset(&event, 0, sizeof event);
        event.kind = CF_EVENT_PURGE_BLOB;
        event.blob_id = blob_id;
        rc = cf_tx_event(tx, event);
    } else {
        cf_attachment_dispose(&attachment);
    }
    cf_account_dispose(&account);
    return rc;
}

cf_err cf_action_accounts_logos_destroy(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    int64_t actor_id = 0;
    rc = logos_ensure_can_administer(ctx, &actor_id);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_account account = {0};
    rc = logos_current_account(ctx, &account);
    if (rc != CF_OK) return rc;
    logos_write_arg arg = {.actor_id = actor_id, .account_id = account.id};
    cf_account_dispose(&account);

    rc = cf_write(ctx->app, logos_write_cb, &arg);
    if (rc != CF_OK) return rc;

    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    rc = cf_builder_append(&location, logos_span(config->public_origin));
    if (rc == CF_OK) {
        rc = cf_builder_append(&location, logos_span("/account/edit"));
    }
    if (rc == CF_OK) {
        ctx->response->status = 302;
        rc = cf_response_header(ctx->response, logos_span("Location"),
                                (cf_span){location.ptr, location.len});
        if (rc == CF_OK) {
            rc = cf_response_header(ctx->response,
                                    logos_span("Content-Type"),
                                    logos_span("text/html; charset=utf-8"));
        }
    }
    cf_builder_dispose(&location);
    return rc;
}
