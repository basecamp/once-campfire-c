/* src/actions/users/avatars_destroy.c — Users::AvatarsController#destroy
 * only (task A-users-avatars, destroy half; route ID 54;
 * contracts/routes.json).  Route 53 `show` belongs to another lane and is
 * not implemented here.
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/users/avatars.rs
 * (`destroy`), the pinned port of reference/app/controllers/users/
 * avatars_controller.rb:
 *
 *   destroy: c.use_live_response() -> before_actions(Before::default()) ->
 *            require_current_user ->
 *            write(attachments::destroy(Current.user, "avatar")) ->
 *            redirect_to user_profile
 *
 * `use_live_response` (ActiveStorage::Streaming) only skips the default
 * response headers; the C dispatch applies none of its own, and the
 * chain's X-Version header has no removal API, so the C response keeps
 * the uniform chain headers where the reference skips them.  No C action
 * implements live semantics yet; when one does, this packet adopts it.
 * `attachments::destroy` deletes the (User, avatar) attachment when one
 * exists (DELETE row, touch users.updated_at, emit PurgeBlob after
 * commit), and is a no-op otherwise
 * (`delegate_missing_to :attachment, allow_nil: true`).  The redirect is
 * unconditional (302, absolute user_profile URL, text/html;
 * charset=utf-8, no notice).
 *
 * Where S02/S03 completion would be required — staging a replacement
 * upload, processing variants — this action needs none: destroy is rows
 * plus a best-effort purge event (J02's CF_JOB_PURGE_BLOB kind through
 * the writer's post-commit consumer; unregistered, it is dropped and
 * counted without changing the committed outcome).  The file therefore
 * fails loudly nowhere on its own paths: every loud failure below is a
 * resource error the reference also answers 500.
 *
 * Integrator requests:
 *  R1. A shared record-touch helper (see profiles.c R2); until it lands,
 *      the static users touch below is the local copy.
 *
 * c_symbols for the integrator's route rebind (src/routes.c row 54):
 *   cf_action_users_avatars_destroy.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "models/active_storage.h"
#include "models/user.h"

#include <stdlib.h>
#include <string.h>

static cf_span avatars_destroy_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `require_current_user`: the chain guarantees one (messages.c). */
static cf_err avatars_destroy_current_user(cf_ctx *ctx, cf_user *out) {
    bool found = false;
    cf_err rc = cf_auth_current_user(ctx, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_INTERNAL;
    return CF_OK;
}

enum { AVATARS_DESTROY_STMT_TOUCH_USER };

static const cf_stmt_def avatars_destroy_stmt_defs[] = {
    [AVATARS_DESTROY_STMT_TOUCH_USER] = {
        "UPDATE \"users\" SET \"updated_at\" = ? WHERE \"users\".\"id\" = ?"},
};

static const cf_stmt_set avatars_destroy_stmt_set = {
    avatars_destroy_stmt_defs,
    sizeof avatars_destroy_stmt_defs / sizeof avatars_destroy_stmt_defs[0]};

/* `belongs_to :record, touch: true` for the avatar's user (R1 until a
 * shared helper lands). */
static cf_err avatars_destroy_touch_user(cf_db *db, int64_t user_id) {
    if (db == NULL) return CF_INVALID;
    char timebuf[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(cf_now_us(NULL), timebuf);
    sqlite3_stmt *stmt = NULL;
    if (rc == CF_OK) {
        rc = cf_db_stmt(db, &avatars_destroy_stmt_set,
                        AVATARS_DESTROY_STMT_TOUCH_USER, &stmt);
    }
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(
            stmt, 1,
            (cf_span){(const unsigned char *)timebuf, strlen(timebuf)});
    }
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 2, user_id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step != SQLITE_DONE) rc = cf_db_err(step);
    }
    cf_db_stmt_done(stmt);
    return rc;
}

typedef struct {
    int64_t user_id;
} avatars_destroy_arg;

/* `Current.user.avatar.destroy`: find, delete, touch, purge-later.  No
 * attachment is success without effects. */
static cf_err avatars_destroy_cb(cf_tx *tx, void *arg) {
    avatars_destroy_arg *destroy = arg;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    bool found = false;
    cf_attachment attachment = {0};
    cf_err rc = cf_attachment_find_for(
        db, (cf_str){(char *)"User", 4}, destroy->user_id,
        (cf_str){(char *)"avatar", 6}, &found, &attachment);
    int64_t blob_id = 0;
    if (rc == CF_OK && found) {
        blob_id = attachment.blob_id;
        rc = cf_attachment_delete(tx, &attachment);
    }
    cf_attachment_dispose(&attachment);
    if (rc == CF_OK && found) {
        rc = avatars_destroy_touch_user(db, destroy->user_id);
    }
    if (rc == CF_OK && found) {
        rc = cf_tx_event(tx, (cf_event){
                                  .kind = CF_EVENT_PURGE_BLOB,
                                  .blob_id = blob_id,
                              });
    }
    return rc;
}

cf_err cf_action_users_avatars_destroy(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    /* `before_actions(Before::default())`. */
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = avatars_destroy_current_user(ctx, &user);
    int64_t user_id = user.id;
    cf_user_dispose(&user);
    if (rc != CF_OK) return rc;

    avatars_destroy_arg arg = {user_id};
    rc = cf_write(ctx->app, avatars_destroy_cb, &arg);
    if (rc != CF_OK) return rc;

    /* `redirect_to user_profile` (302, absolute, reference content type). */
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    rc = cf_builder_append(&location,
                           avatars_destroy_span(config->public_origin));
    if (rc == CF_OK) {
        rc = cf_builder_append(&location,
                               avatars_destroy_span("/users/me/profile"));
    }
    if (rc == CF_OK) {
        ctx->response->status = 302;
        rc = cf_response_header(ctx->response,
                                avatars_destroy_span("Location"),
                                (cf_span){location.ptr, location.len});
        if (rc == CF_OK) {
            rc = cf_response_header(ctx->response,
                                    avatars_destroy_span("Content-Type"),
                                    avatars_destroy_span(
                                        "text/html; charset=utf-8"));
        }
    }
    cf_builder_dispose(&location);
    return rc;
}
