/* J02 bounded job handlers: one handler per queueable kind plus the
 * registration helper. See src/jobs/handlers.h for the contract.
 *
 * Reference symbols translated here (controller/model packets):
 *   - PushMessage: cf_message_find_by_id + cf_room_find_by_id re-reads, then
 *     cf_push_subscription_pushes_for (Room::MessagePusher recipient
 *     selection: everything + mentions) with a NULL richtext (no rich-text
 *     record means the reference's None). Delivery itself is I02's
 *     (not landed): proposed weak import cf_push_send below.
 *   - DeliverWebhook: cf_user_find_active_bot (revoked/deactivated bots are
 *     CF_NOT_FOUND) + cf_message_find_by_id + cf_user_webhook re-reads
 *     (User::deliver_webhook_later only enqueues for bots with a webhook).
 *     Posting is I01's (not landed): proposed weak import cf_webhook_post.
 *   - RemoveBannedContent: the A-users-bans REMOVE_BANNED_CONTENT note (ban
 *     emits the event; unban does not restore). The model helper
 *     cf_user_remove_banned_content destroys unboundedly, so this handler
 *     uses its own bounded id scan (fixed SQL, LIMIT 101) and destroys at
 *     most CF_JOBS_REMOVE_BANNED_BATCH rows per cf_write via
 *     cf_message_destroy (which also emits PURGE_BLOB for attachments and
 *     touches rooms, as in the request path). Broadcasts use
 *     cf_broadcast_message_remove only. TODO: promote the bounded scan to a
 *     D01 model helper when the integrator next touches message queries.
 *   - PurgeBlob: cf_blob_find re-read, then S02's purge. TODO(S02): S02
 *     owns the reference recheck (active_storage_attachments by blob) and
 *     the in-transaction row delete; S01's cf_storage_delete runs only
 *     after that recheck, inside S02. Until S02 lands, a present blob is a
 *     recorded no-op: the handler deletes nothing.
 *   - Media: explicit storage-owned stub until S03 lands.
 *
 * Proposed integrator-owned integration symbols (weak imports: NULL until
 * the owning packet lands, in which case the recorded-noop arm calls them):
 *   - I02: cf_push_send delivers one payload to one recipient list.
 *   - I01: cf_webhook_post posts one webhook for one message.
 *   - S02: cf_blob_purge_if_unreferenced rechecks references and purges.
 */
#include "jobs/handlers.h"

#include "app.h"
#include "cable/channels.h"
#include "config.h"
#include "db/db_internal.h"
#include "db/writer.h"
#include "jobs/jobs.h"
#include "models/active_storage.h"
#include "models/message.h"
#include "models/push_subscription.h"
#include "models/room.h"
#include "models/user.h"
#include "models/webhook.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- proposed integration symbols (weak; integrator-owned) -------------- */

/* I02 (not landed): deliver payload to nsubs subscriptions. */
extern cf_err cf_push_send(const cf_push_payload *payload,
                           const cf_push_subscription *subs,
                           size_t nsubs) __attribute__((weak));

/* I01 (not landed): post user_id's webhook for message_id. */
extern cf_err cf_webhook_post(int64_t user_id,
                              int64_t message_id) __attribute__((weak));

/* S02 (not landed): recheck references for blob_id and, when unreferenced,
 * delete the row (and its S01 files) in one transaction. Sets *purged to
 * whether anything was deleted. CF_NOT_FOUND when the row is already gone.
 * TODO(S02): provide this symbol; see the file header. */
extern cf_err cf_blob_purge_if_unreferenced(cf_app *app, int64_t blob_id,
                                            bool *purged) __attribute__((weak));

/* ---- own reader ---------------------------------------------------------- */

/* Open the handler's own read-only connection for this invocation. The
 * path comes from the app config when an app is present, else from the
 * unit-test db_path. Never a request/worker reader. */
static cf_err jobs_reader_open(cf_jobs_handler_ctx *h, cf_db **out) {
    *out = NULL;
    if (h == NULL) return CF_INVALID;
    const char *path = h->db_path;
    if (h->app != NULL) {
        const cf_config *cfg = cf_app_config(h->app);
        if (cfg != NULL && cfg->database_path != NULL) {
            path = cfg->database_path;
        }
    }
    if (path == NULL) return CF_INVALID;
    return cf_db_open(path, true, out);
}

/* ---- PushMessage (CF_JOB_PUSH_MESSAGE) ------------------------------------ */

cf_err cf_jobs_handle_push_message(void *ctx, const cf_job *job) {
    cf_jobs_handler_ctx *h = ctx;
    if (h == NULL || job == NULL || job->kind != CF_JOB_PUSH_MESSAGE) {
        return CF_INVALID;
    }
    if (job->room_id <= 0 || job->message_id <= 0) return CF_INVALID;

    cf_db *db = NULL;
    cf_err rc = jobs_reader_open(h, &db);
    if (rc != CF_OK) return rc;
    rc = cf_read_begin(db);
    if (rc != CF_OK) {
        cf_db_close(db);
        return rc;
    }

    /* Re-read the targets: a deleted message or room is a recorded no-op,
     * as is a message that no longer belongs to the event's room. */
    cf_message message = {0};
    bool message_found = false;
    cf_room room = {0};
    bool room_found = false;
    bool gone = false;
    cf_push_payload payload = {0};
    cf_push_subscription_vector everything = {0};
    cf_push_subscription_vector mentions = {0};

    rc = cf_message_find_by_id(db, job->message_id, &message_found, &message);
    if (rc == CF_OK && (!message_found || message.room_id != job->room_id)) {
        gone = true;
    }
    if (rc == CF_OK && !gone) {
        rc = cf_room_find_by_id(db, job->room_id, &room_found, &room);
        if (rc == CF_OK && !room_found) gone = true;
    }
    if (rc == CF_OK && !gone) {
        rc = cf_push_subscription_pushes_for(db, NULL, &message,
                                            cf_now_us(h->app), &payload,
                                            &everything, &mentions);
    }

    /* The read transaction ends before any integration call: delivery may
     * perform network I/O, and no read may span a later cf_write. */
    cf_err end_rc = cf_read_end(db);
    cf_db_close(db);
    if (rc != CF_OK) {
        cf_push_payload_dispose(&payload);
        cf_push_subscription_vector_dispose(&everything);
        cf_push_subscription_vector_dispose(&mentions);
        cf_room_dispose(&room);
        cf_message_dispose(&message);
        return rc;
    }
    if (end_rc != CF_OK) {
        cf_push_payload_dispose(&payload);
        cf_push_subscription_vector_dispose(&everything);
        cf_push_subscription_vector_dispose(&mentions);
        cf_room_dispose(&room);
        cf_message_dispose(&message);
        return end_rc;
    }
    if (gone) {
        atomic_fetch_add_explicit(&h->push_noop_gone, 1,
                                 memory_order_relaxed);
        cf_room_dispose(&room);
        cf_message_dispose(&message);
        return CF_OK;
    }

    /* I02 has not landed: recipients were selected through the model reads
     * above; delivery is a recorded no-op until cf_push_send exists. */
    if (cf_push_send == NULL) {
        atomic_fetch_add_explicit(&h->push_noop_no_integration, 1,
                                 memory_order_relaxed);
        cf_push_payload_dispose(&payload);
        cf_push_subscription_vector_dispose(&everything);
        cf_push_subscription_vector_dispose(&mentions);
        cf_room_dispose(&room);
        cf_message_dispose(&message);
        return CF_OK;
    }
    atomic_fetch_add_explicit(&h->push_attempted, 1, memory_order_relaxed);
    if (everything.len != 0) {
        rc = cf_push_send(&payload, everything.items, everything.len);
    }
    if (rc == CF_OK && mentions.len != 0) {
        rc = cf_push_send(&payload, mentions.items, mentions.len);
    }
    cf_push_payload_dispose(&payload);
    cf_push_subscription_vector_dispose(&everything);
    cf_push_subscription_vector_dispose(&mentions);
    cf_room_dispose(&room);
    cf_message_dispose(&message);
    return rc;
}

/* ---- DeliverWebhook (CF_JOB_DELIVER_WEBHOOK) ------------------------------- */

cf_err cf_jobs_handle_deliver_webhook(void *ctx, const cf_job *job) {
    cf_jobs_handler_ctx *h = ctx;
    if (h == NULL || job == NULL || job->kind != CF_JOB_DELIVER_WEBHOOK) {
        return CF_INVALID;
    }
    if (job->user_id <= 0 || job->message_id <= 0) return CF_INVALID;

    cf_db *db = NULL;
    cf_err rc = jobs_reader_open(h, &db);
    if (rc != CF_OK) return rc;
    rc = cf_read_begin(db);
    if (rc != CF_OK) {
        cf_db_close(db);
        return rc;
    }

    /* Re-read the bot, the message and the webhook: a deleted or
     * deactivated bot, a deleted message, or a removed/blanked webhook URL
     * is a recorded no-op (never resurrected, never retried). */
    cf_user bot = {0};
    cf_message message = {0};
    cf_webhook hook = {0};
    bool gone = false;

    rc = cf_user_find_active_bot(db, job->user_id, &bot);
    if (rc == CF_NOT_FOUND) {
        gone = true;
        rc = CF_OK;
    }
    if (rc == CF_OK && !gone) {
        bool message_found = false;
        rc = cf_message_find_by_id(db, job->message_id, &message_found,
                                   &message);
        if (rc == CF_OK && !message_found) gone = true;
    }
    if (rc == CF_OK && !gone) {
        bool hook_found = false;
        rc = cf_webhook_find_by_user(db, job->user_id, &hook_found, &hook);
        if (rc == CF_OK) {
            if (!hook_found || !hook.url.present ||
                hook.url.value.len == 0) {
                gone = true;
            }
        }
    }

    cf_err end_rc = cf_read_end(db);
    cf_db_close(db);
    if (rc != CF_OK) {
        cf_webhook_dispose(&hook);
        cf_user_dispose(&bot);
        cf_message_dispose(&message);
        return rc;
    }
    if (end_rc != CF_OK) {
        cf_webhook_dispose(&hook);
        cf_user_dispose(&bot);
        cf_message_dispose(&message);
        return end_rc;
    }
    cf_webhook_dispose(&hook);
    cf_user_dispose(&bot);
    cf_message_dispose(&message);
    if (gone) {
        atomic_fetch_add_explicit(&h->webhook_noop_gone, 1,
                                 memory_order_relaxed);
        return CF_OK;
    }

    /* I01 has not landed: the active bot and message were re-read above;
     * posting is a recorded no-op until cf_webhook_post exists. */
    if (cf_webhook_post == NULL) {
        atomic_fetch_add_explicit(&h->webhook_noop_no_integration, 1,
                                 memory_order_relaxed);
        return CF_OK;
    }
    atomic_fetch_add_explicit(&h->webhook_attempted, 1, memory_order_relaxed);
    return cf_webhook_post(job->user_id, job->message_id);
}

/* ---- RemoveBannedContent (CF_JOB_REMOVE_BANNED_CONTENT) -------------------- */

/* Fixed bounded scan: the oldest CF_JOBS_REMOVE_BANNED_SCAN message ids of
 * one creator. Complete rows (identity fields plus parsed timestamps) so
 * the copies serve both cf_message_destroy (id + room_id) and the
 * post-commit broadcast (id + client_message_id). */
static const char cf_jobs_banned_scan_sql[] =
    "SELECT \"messages\".\"id\", \"messages\".\"client_message_id\", "
    "\"messages\".\"room_id\", \"messages\".\"creator_id\", "
    "\"messages\".\"created_at\", \"messages\".\"updated_at\" "
    "FROM \"messages\" WHERE \"messages\".\"creator_id\" = ?1 "
    "ORDER BY \"messages\".\"id\" LIMIT 101";

static cf_err jobs_scan_banned_messages(cf_db *db, int64_t user_id,
                                        cf_message_vector *out) {
    memset(out, 0, sizeof *out);
    sqlite3 *handle = cf_db_handle(db);
    if (handle == NULL) return CF_INTERNAL;
    sqlite3_stmt *stmt = NULL;
    int step_rc =
        sqlite3_prepare_v2(handle, cf_jobs_banned_scan_sql, -1, &stmt, NULL);
    if (step_rc != SQLITE_OK) return cf_db_err(step_rc);
    cf_err rc = CF_OK;
    if (sqlite3_bind_int64(stmt, 1, user_id) != SQLITE_OK) {
        rc = cf_db_err(sqlite3_errcode(handle));
    }
    while (rc == CF_OK) {
        step_rc = sqlite3_step(stmt);
        if (step_rc == SQLITE_DONE) break;
        if (step_rc != SQLITE_ROW) {
            rc = cf_db_err(step_rc);
            break;
        }
        cf_message row = {0};
        row.id = sqlite3_column_int64(stmt, 0);
        const unsigned char *client_id = sqlite3_column_text(stmt, 1);
        int client_id_bytes = sqlite3_column_bytes(stmt, 1);
        row.room_id = sqlite3_column_int64(stmt, 2);
        row.creator_id = sqlite3_column_int64(stmt, 3);
        if (client_id == NULL || client_id_bytes < 0) {
            rc = CF_DB;
            break;
        }
        char *id_copy = malloc((size_t)client_id_bytes + 1);
        if (id_copy == NULL) {
            rc = CF_NOMEM;
            break;
        }
        memcpy(id_copy, client_id, (size_t)client_id_bytes);
        id_copy[client_id_bytes] = '\0';
        row.client_message_id.ptr = id_copy;
        row.client_message_id.len = (size_t)client_id_bytes;
        const unsigned char *created = sqlite3_column_text(stmt, 4);
        const unsigned char *updated = sqlite3_column_text(stmt, 5);
        rc = cf_db_time_from_text(
            (cf_span){created, created != NULL ? strlen((const char *)created)
                                              : 0},
            &row.created_at);
        if (rc == CF_OK) {
            rc = cf_db_time_from_text(
                (cf_span){updated,
                          updated != NULL ? strlen((const char *)updated)
                                          : 0},
                &row.updated_at);
        }
        if (rc != CF_OK) {
            cf_message_dispose(&row);
            break;
        }
        if (out->len == out->cap) {
            size_t cap = out->cap != 0 ? out->cap * 2 : 16;
            cf_message *items = realloc(out->items, cap * sizeof *items);
            if (items == NULL) {
                cf_message_dispose(&row);
                rc = CF_NOMEM;
                break;
            }
            out->items = items;
            out->cap = cap;
        }
        out->items[out->len++] = row;
    }
    (void)sqlite3_finalize(stmt);
    if (rc != CF_OK) cf_message_vector_dispose(out);
    return rc;
}

typedef struct {
    cf_message_vector *batch; /* destroys items[0, count) */
    size_t count;
} jobs_destroy_arg;

static cf_err jobs_destroy_batch_cb(cf_tx *tx, void *arg) {
    jobs_destroy_arg *destroy = arg;
    for (size_t i = 0; i < destroy->count; i++) {
        cf_err rc = cf_message_destroy(tx, &destroy->batch->items[i]);
        if (rc != CF_OK) return rc;
    }
    return CF_OK;
}

/* Post-commit broadcast policy mirrors the request path's
 * msg_broadcast_outcome: a full loop queue (CF_BUSY) is logged and treated
 * as delivered; any other failure is returned after attempting the rest. */
static cf_err jobs_broadcast_removed(cf_jobs_handler_ctx *h, cf_db *db,
                                     const cf_message_vector *batch,
                                     size_t count) {
    cf_err first_rc = CF_OK;
    for (size_t i = 0; i < count; i++) {
        const cf_message *message = &batch->items[i];
        cf_room room = {0};
        bool found = false;
        cf_err rc =
            cf_room_find_by_id(db, message->room_id, &found, &room);
        if (rc == CF_OK && !found) continue; /* room went away: skip */
        if (rc == CF_OK) {
            atomic_fetch_add_explicit(&h->banned_broadcasts, 1,
                                     memory_order_relaxed);
            rc = cf_broadcast_message_remove(h->cable, &room, message);
            if (rc == CF_BUSY) {
                cf_cable_log("dropped broadcast",
                             "remove_banned_content");
                rc = CF_OK;
            }
        }
        cf_room_dispose(&room);
        if (rc != CF_OK && first_rc == CF_OK) first_rc = rc;
    }
    return first_rc;
}

cf_err cf_jobs_handle_remove_banned_content(void *ctx, const cf_job *job) {
    cf_jobs_handler_ctx *h = ctx;
    if (h == NULL || job == NULL ||
        job->kind != CF_JOB_REMOVE_BANNED_CONTENT) {
        return CF_INVALID;
    }
    if (job->user_id <= 0) return CF_INVALID;

    cf_db *db = NULL;
    cf_err rc = jobs_reader_open(h, &db);
    if (rc != CF_OK) return rc;
    rc = cf_read_begin(db);
    if (rc != CF_OK) {
        cf_db_close(db);
        return rc;
    }

    /* Only a still-banned user loses messages: a missing user, or one
     * unbanned while the job waited, is a recorded no-op. */
    cf_user user = {0};
    bool user_found = false;
    cf_message_vector scanned = {0};
    bool scanned_ok = false;
    bool gone = false;

    rc = cf_user_find_by_id(db, job->user_id, &user_found, &user);
    if (rc == CF_OK && (!user_found || !cf_user_is_banned(&user))) {
        gone = true;
    }
    if (rc == CF_OK && !gone) {
        rc = jobs_scan_banned_messages(db, job->user_id, &scanned);
        if (rc == CF_OK) scanned_ok = true;
    }

    cf_err end_rc = cf_read_end(db);
    cf_db_close(db);
    if (rc != CF_OK) {
        if (scanned_ok) cf_message_vector_dispose(&scanned);
        cf_user_dispose(&user);
        return rc;
    }
    if (end_rc != CF_OK) {
        if (scanned_ok) cf_message_vector_dispose(&scanned);
        cf_user_dispose(&user);
        return end_rc;
    }
    if (gone) {
        atomic_fetch_add_explicit(&h->banned_noop_gone, 1,
                                 memory_order_relaxed);
        cf_user_dispose(&user);
        return CF_OK;
    }
    cf_user_dispose(&user);

    size_t destroy_count = scanned.len < CF_JOBS_REMOVE_BANNED_BATCH
                               ? scanned.len
                               : CF_JOBS_REMOVE_BANNED_BATCH;
    bool have_more = scanned.len > CF_JOBS_REMOVE_BANNED_BATCH;
    if (destroy_count == 0) {
        cf_message_vector_dispose(&scanned);
        return CF_OK; /* banned, but nothing left to destroy */
    }
    if (h->app == NULL) {
        cf_message_vector_dispose(&scanned);
        return CF_INTERNAL; /* mutations require the writer's app */
    }

    jobs_destroy_arg destroy = {.batch = &scanned, .count = destroy_count};
    rc = cf_write(h->app, jobs_destroy_batch_cb, &destroy);
    if (rc != CF_OK) {
        cf_message_vector_dispose(&scanned);
        return rc; /* counted failed, no retry */
    }
    atomic_fetch_add_explicit(&h->banned_batches, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->banned_destroyed, destroy_count,
                             memory_order_relaxed);

    /* Broadcast-after-commit through the cable API only. A NULL cable
     * (unit tests, cable not attached) skips delivery. */
    cf_err broadcast_rc = CF_OK;
    if (h->cable != NULL) {
        cf_db *post_db = NULL;
        broadcast_rc = jobs_reader_open(h, &post_db);
        if (broadcast_rc == CF_OK) {
            broadcast_rc =
                jobs_broadcast_removed(h, post_db, &scanned, destroy_count);
            cf_db_close(post_db);
        }
    }

    /* Requeue-remainder: one follow-up event carries the same user_id.
     * A full queue (CF_BUSY) or a missing jobs ctx drops the remainder and
     * counts it; the committed batch still reports CF_OK. */
    if (have_more) {
        if (h->jobs == NULL) {
            atomic_fetch_add_explicit(&h->banned_requeue_dropped, 1,
                                     memory_order_relaxed);
            fprintf(stderr,
                    "campfire: jobs: remove_banned_content remainder dropped "
                    "(no queue)\n");
        } else {
            cf_event follow = {0};
            follow.kind = CF_EVENT_REMOVE_BANNED_CONTENT;
            follow.user_id = job->user_id;
            cf_err queue_rc = cf_job_enqueue(h->jobs, follow);
            if (queue_rc == CF_OK) {
                atomic_fetch_add_explicit(&h->banned_requeued, 1,
                                         memory_order_relaxed);
            } else {
                atomic_fetch_add_explicit(&h->banned_requeue_dropped, 1,
                                         memory_order_relaxed);
                fprintf(stderr,
                        "campfire: jobs: remove_banned_content remainder "
                        "dropped (%s)\n",
                        cf_err_name(queue_rc));
            }
        }
    }

    cf_message_vector_dispose(&scanned);
    return broadcast_rc;
}

/* ---- PurgeBlob (CF_JOB_PURGE_BLOB) ----------------------------------------- */

cf_err cf_jobs_handle_purge_blob(void *ctx, const cf_job *job) {
    cf_jobs_handler_ctx *h = ctx;
    if (h == NULL || job == NULL || job->kind != CF_JOB_PURGE_BLOB) {
        return CF_INVALID;
    }
    if (job->blob_id <= 0) return CF_INVALID;

    cf_db *db = NULL;
    cf_err rc = jobs_reader_open(h, &db);
    if (rc != CF_OK) return rc;
    rc = cf_read_begin(db);
    if (rc != CF_OK) {
        cf_db_close(db);
        return rc;
    }

    /* A missing blob row is a recorded no-op (already purged or never
     * attached): nothing is resurrected. */
    cf_blob blob = {0};
    rc = cf_blob_find(db, job->blob_id, &blob);
    bool gone = rc == CF_NOT_FOUND;
    if (gone) rc = CF_OK;

    cf_err end_rc = cf_read_end(db);
    cf_db_close(db);
    if (rc != CF_OK) return rc;
    if (end_rc != CF_OK) {
        cf_blob_dispose(&blob);
        return end_rc;
    }
    if (gone) {
        atomic_fetch_add_explicit(&h->purge_noop_gone, 1,
                                 memory_order_relaxed);
        return CF_OK;
    }
    cf_blob_dispose(&blob);

    /* TODO(S02): S02 owns the reference recheck and the in-transaction row
     * delete (S01 file deletes run only after that recheck). Until
     * cf_blob_purge_if_unreferenced exists, a present blob is a recorded
     * no-op and nothing on disk is touched. */
    if (cf_blob_purge_if_unreferenced == NULL) {
        atomic_fetch_add_explicit(&h->purge_deferred_no_s02, 1,
                                 memory_order_relaxed);
        return CF_OK;
    }
    bool purged = false;
    rc = cf_blob_purge_if_unreferenced(h->app, job->blob_id, &purged);
    if (rc == CF_NOT_FOUND) {
        atomic_fetch_add_explicit(&h->purge_noop_gone, 1,
                                 memory_order_relaxed);
        return CF_OK;
    }
    if (rc != CF_OK) return rc;
    if (purged) {
        atomic_fetch_add_explicit(&h->purge_deleted, 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&h->purge_noop_gone, 1,
                                 memory_order_relaxed);
    }
    return CF_OK;
}

/* ---- Media (CF_JOB_MEDIA): explicit storage-owned stub --------------------- */

cf_err cf_jobs_handle_media(void *ctx, const cf_job *job) {
    cf_jobs_handler_ctx *h = ctx;
    if (h == NULL || job == NULL || job->kind != CF_JOB_MEDIA) {
        return CF_INVALID;
    }
    /* S03 owns the real analyze/transform worker (task_type/variation are
     * borrowed queue-owned strings for the call only). Until S03 lands,
     * every media job is a recorded no-op. */
    atomic_fetch_add_explicit(&h->media_noop, 1, memory_order_relaxed);
    return CF_OK;
}

/* ---- registration ---------------------------------------------------------- */

cf_err cf_jobs_register_default_handlers(cf_jobs *jobs,
                                         cf_jobs_handler_ctx *ctx) {
    if (jobs == NULL || ctx == NULL) return CF_INVALID;
    ctx->jobs = jobs;
    static const struct {
        cf_job_kind kind;
        cf_job_handler_fn fn;
    } table[] = {
        {CF_JOB_PUSH_MESSAGE, cf_jobs_handle_push_message},
        {CF_JOB_DELIVER_WEBHOOK, cf_jobs_handle_deliver_webhook},
        {CF_JOB_REMOVE_BANNED_CONTENT,
         cf_jobs_handle_remove_banned_content},
        {CF_JOB_PURGE_BLOB, cf_jobs_handle_purge_blob},
        {CF_JOB_MEDIA, cf_jobs_handle_media},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        cf_err rc = cf_jobs_set_handler(jobs, table[i].kind, table[i].fn, ctx);
        if (rc != CF_OK) return rc;
    }
    return CF_OK;
}
