/* src/presenters/accounts.c — the account/session presenters the foundation
 * families use (tmp/rust-ref/crates/campfire/src/controllers/presenters/
 * accounts.rs: help_contact, no_users).
 *
 * Queries the model layer does not offer are written here against the
 * reference's SQL, through D01's fixed per-module statement cache (no SQL is
 * assembled at runtime).
 */
#include "views.h"

#include "db/db_internal.h"
#include "models/user.h"

#include <stdlib.h>
#include <string.h>

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

enum { PRESENTER_STMT_HELP_CONTACT };

static const cf_stmt_def presenter_stmt_defs[] = {
    [PRESENTER_STMT_HELP_CONTACT] = {
        "SELECT \"users\".\"name\", \"users\".\"email_address\" "
        "FROM \"users\" WHERE \"users\".\"role\" = 1 "
        "ORDER BY \"users\".\"id\" ASC LIMIT 1"},
};

static const cf_stmt_set presenter_stmt_set = {
    presenter_stmt_defs,
    sizeof presenter_stmt_defs / sizeof presenter_stmt_defs[0]};

cf_err cf_presenter_help_contact(cf_db *db, bool *found,
                                 cf_view_help_contact *out) {
    if (db == NULL || found == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    *found = false;

    cf_err rc = cf_read_begin(db);
    if (rc != CF_OK) return rc;
    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &presenter_stmt_set, PRESENTER_STMT_HELP_CONTACT,
                    &stmt);
    if (rc != CF_OK) goto done;
    int step = sqlite3_step(stmt);
    if (step == SQLITE_DONE) {
        rc = CF_OK;
        goto done;
    }
    if (step != SQLITE_ROW) {
        rc = cf_db_err(step);
        goto done;
    }
    {
        cf_span name = cf_stmt_column_text(stmt, 0);
        rc = str_dup(name, &out->name);
        if (rc != CF_OK) goto done;
        if (cf_stmt_column_is_null(stmt, 1)) {
            out->email_address.ptr = malloc(1);
            if (out->email_address.ptr == NULL) {
                rc = CF_NOMEM;
                goto done;
            }
            out->email_address.ptr[0] = '\0';
            out->email_address.len = 0;
        } else {
            rc = str_dup(cf_stmt_column_text(stmt, 1),
                         &out->email_address);
            if (rc != CF_OK) goto done;
        }
        *found = true;
    }
done:
    if (stmt != NULL) cf_db_stmt_done(stmt);
    cf_err end_rc = cf_read_end(db);
    if (rc == CF_OK) rc = end_rc;
    if (rc != CF_OK && *found) {
        cf_view_help_contact_dispose(out);
        *found = false;
    }
    return rc;
}

cf_err cf_presenter_no_users(cf_db *db, bool *out) {
    if (db == NULL || out == NULL) return CF_INVALID;
    cf_err rc = cf_read_begin(db);
    if (rc != CF_OK) return rc;
    int64_t count = 0;
    rc = cf_user_count(db, &count);
    cf_err end_rc = cf_read_end(db);
    if (rc == CF_OK) rc = end_rc;
    if (rc == CF_OK) *out = count == 0;
    return rc;
}
