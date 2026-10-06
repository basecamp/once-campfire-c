/* J02 bounded job handlers: one handler per queueable kind plus the
 * registration helper. See src/jobs/handlers.h for the contract.
 *
 * Reference symbols translated here (controller/model packets):
 *   - PushMessage: cf_message_find_by_id + cf_room_find_by_id re-reads, then
 *     cf_push_subscription_pushes_for (Room::MessagePusher recipient
 *     selection: everything + mentions) using the configured rich-text
 *     pipeline. Delivery uses I02
 *     through the configured real pinned-IP TLS exchange.
 *   - DeliverWebhook: current bot, room membership, source message and
 *     webhook checks; real I01 HTTP delivery, canonical replies, writer
 *     revalidation, attachment processing and production broadcast.
 *   - RemoveBannedContent: the A-users-bans REMOVE_BANNED_CONTENT note (ban
 *     emits the event; unban does not restore). The model helper
 *     cf_user_remove_banned_content destroys unboundedly, so this handler
 *     uses its own bounded id scan (fixed SQL, LIMIT 101) and destroys at
 *     most CF_JOBS_REMOVE_BANNED_BATCH rows per cf_write via
 *     cf_message_destroy (which also emits PURGE_BLOB for attachments and
 *     touches rooms, as in the request path). Broadcasts use
 *     cf_broadcast_message_remove only. TODO: promote the bounded scan to a
 *     D01 model helper when the integrator next touches message queries.
 *   - PurgeBlob: recheck references and remove parent/derived metadata in
 *     the writer, then delete owned files on this job worker. Referenced
 *     descendants remain intact; I/O errors report failed cleanup.
 *   - Media: bounded real analysis before committing metadata and touches.
 *
 */
#include "jobs/handlers.h"

#include "app.h"
#include "cable/channels.h"
#include "config.h"
#include "db/db_internal.h"
#include "db/writer.h"
#include "jobs/jobs.h"
#include "integrations/push_http.h"
#include "integrations/webhook.h"
#include "models/membership.h"
#include "views.h"
#include "integrations/host_resolve.h"
#include "richtext.h"
#include "models/active_storage.h"
#include "models/message.h"
#include "models/push_subscription.h"
#include "models/room.h"
#include "models/user.h"
#include "models/webhook.h"
#include "storage/media.h"
#include "storage/active_storage.h"
#include "storage/storage.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

struct jobs_push_invalidate {
    const cf_push_subscription *sent;
};

static bool jobs_push_same_key(cf_optional_str a, cf_optional_str b) {
    return a.present == b.present && (!a.present ||
        (a.value.len == b.value.len &&
         (a.value.len == 0 || memcmp(a.value.ptr,b.value.ptr,a.value.len) == 0)));
}

static cf_err jobs_push_invalidate_cb(cf_tx *tx, void *arg) {
    const cf_push_subscription *sent = ((struct jobs_push_invalidate *)arg)->sent;
    cf_push_subscription current = {0};
    cf_err rc = cf_push_subscription_find(cf_tx_db(tx),sent->id,&current);
    if (rc == CF_NOT_FOUND) return CF_OK;
    if (rc == CF_OK && current.user_id == sent->user_id &&
        jobs_push_same_key(current.endpoint,sent->endpoint) &&
        jobs_push_same_key(current.p256dh_key,sent->p256dh_key) &&
        jobs_push_same_key(current.auth_key,sent->auth_key)) {
        rc = cf_push_subscription_destroy(tx,&current);
    }
    cf_push_subscription_dispose(&current);
    return rc;
}

static cf_err jobs_push_deliver_list(cf_jobs_handler_ctx *h,
    const cf_push_payload *payload, cf_push_subscription_vector *subscriptions,
    const cf_push_vapid *vapid) {
    cf_push_sort_by_id(subscriptions);
    cf_err first_error = CF_OK;
    for (size_t i=0;i<subscriptions->len;i++) {
        cf_push_subscription *subscription = &subscriptions->items[i];
        cf_db *db = NULL;
        int64_t badge = 0;
        cf_err rc = jobs_reader_open(h,&db);
        if (rc == CF_OK) rc = cf_push_subscription_badge(db,subscription,&badge);
        cf_db_close(db);
        if (rc != CF_OK) {if(first_error==CF_OK)first_error=rc;continue;}
        cf_push_delivery outcome = {0};
        atomic_fetch_add_explicit(&h->push_attempted,1,memory_order_relaxed);
        rc = cf_push_deliver(subscription,
            payload->title.ptr != NULL ? payload->title.ptr : "",
            payload->body.ptr != NULL ? payload->body.ptr : "",
            payload->path.ptr != NULL ? payload->path.ptr : "",badge,vapid,
            h->push_resolve != NULL ? h->push_resolve : cf_host_resolve_fn,
            h->push_resolve_ctx,
            h->push_exchange != NULL ? h->push_exchange : cf_push_http_exchange,
            h->push_exchange_ctx,cf_now_us(h->app)/INT64_C(1000000),&outcome);
        if (rc == CF_OK && outcome.invalidate) {
            struct jobs_push_invalidate write = {.sent=subscription};
            rc = cf_write(h->app,jobs_push_invalidate_cb,&write);
        } else if (rc == CF_OK && outcome.kind != CF_PUSH_DELIVERY_OK) {
            char line[256];
            cf_push_format_log(&outcome,line,sizeof line);
            fprintf(stderr,"campfire: push delivery failed: %s\n",line);
            rc = CF_IO;
        }
        cf_push_delivery_dispose(&outcome);
        if (rc != CF_OK && first_error == CF_OK) first_error = rc;
        /* A failed recipient never suppresses independent remaining ones. */
    }
    return first_error;
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
        rc = cf_push_subscription_pushes_for(db, cf_tx_rich_text(NULL), &message,
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

    /* No recipient is a normal empty delivery. Every present recipient uses
     * the real configured VAPID pair and pinned-IP TLS transport by default. */
    if (everything.len == 0 && mentions.len == 0) {
        atomic_fetch_add_explicit(&h->push_noop_no_recipients,1,memory_order_relaxed);
    } else {
        cf_push_vapid vapid = {0};
        cf_push_vapid_error detail = CF_PUSH_VAPID_OK;
        rc = cf_push_vapid_from_config(cf_app_config(h->app),&vapid,&detail);
        if (rc == CF_OK) {
            rc = jobs_push_deliver_list(h,&payload,&everything,&vapid);
            cf_err mention_rc = jobs_push_deliver_list(h,&payload,&mentions,&vapid);
            if (rc == CF_OK) rc = mention_rc;
        } else {
            fprintf(stderr,"campfire: push disabled: %s\n",cf_push_vapid_error_name(detail));
        }
        cf_push_vapid_dispose(&vapid);
    }
    cf_push_payload_dispose(&payload);
    cf_push_subscription_vector_dispose(&everything);
    cf_push_subscription_vector_dispose(&mentions);
    cf_room_dispose(&room);
    cf_message_dispose(&message);
    return rc;
}

/* ---- DeliverWebhook (CF_JOB_DELIVER_WEBHOOK) ------------------------------- */

/* Snapshot checks are repeated under the writer after HTTP/file staging. */
static cf_err jobs_webhook_targets(cf_db *db, const cf_job *job,
                                  cf_user *bot, cf_message *message,
                                  cf_webhook *hook, bool *gone) {
    *gone=false;
    cf_err rc=cf_user_find_active_bot(db,job->user_id,bot);
    if(rc==CF_NOT_FOUND){*gone=true;return CF_OK;}
    bool found=false;
    if(rc==CF_OK)rc=cf_message_find_by_id(db,job->message_id,&found,message);
    if(rc==CF_OK&&!found){*gone=true;return CF_OK;}
    if(rc==CF_OK)rc=cf_webhook_find_by_user(db,job->user_id,&found,hook);
    if(rc==CF_OK&&(!found||!hook->url.present||hook->url.value.len==0)){*gone=true;return CF_OK;}
    cf_room room={0};
    if(rc==CF_OK)rc=cf_room_find_by_id(db,message->room_id,&found,&room);
    cf_room_dispose(&room);
    if(rc==CF_OK&&!found){*gone=true;return CF_OK;}
    cf_membership membership={0};
    if(rc==CF_OK)rc=cf_membership_find_by_room_and_user(db,message->room_id,bot->id,&found,&membership);
    cf_membership_dispose(&membership);
    if(rc==CF_OK&&!found)*gone=true;
    return rc;
}

struct jobs_webhook_reply {
    const cf_job *job;
    const cf_message *source;
    const cf_webhook *sent;
    cf_optional_str body;
    const cf_active_staged *staged;
    cf_message reply;
    cf_blob blob;
    bool gone;
};
static cf_err jobs_webhook_reply_write(cf_tx *tx, void *arg) {
    struct jobs_webhook_reply *w=arg;
    cf_user bot={0};cf_message source={0};cf_webhook hook={0};
    cf_err rc=jobs_webhook_targets(cf_tx_db(tx),w->job,&bot,&source,&hook,&w->gone);
    if(rc==CF_OK&&!w->gone && (source.room_id!=w->source->room_id ||
        source.creator_id!=w->source->creator_id || hook.id!=w->sent->id ||
        !jobs_push_same_key(hook.url,w->sent->url)))w->gone=true;
    cf_new_message input={.room_id=source.room_id,.creator_id=bot.id,.body=w->body};
    if(rc==CF_OK&&!w->gone&&w->staged){
        const cf_active_staged *s=w->staged;
        cf_blob blob={.key=s->key,.filename=s->filename,.content_type=s->content_type,
            .metadata={true,s->metadata},.service_name=s->service_name,
            .byte_size=s->byte_size,.checksum={true,s->checksum}};
        rc=cf_blob_create(tx,&blob,&w->blob);
        input.attachment_blob_id=(cf_optional_i64){true,w->blob.id};
    }
    if(rc==CF_OK&&!w->gone)rc=cf_message_create(tx,&input,&w->reply);
    cf_user_dispose(&bot);cf_message_dispose(&source);cf_webhook_dispose(&hook);
    return rc;
}

static cf_err jobs_webhook_broadcast(cf_jobs_handler_ctx *h,cf_db *db,const cf_message *message){
    if(h->cable==NULL)return CF_OK; /* Explicit context policy, e.g. unit jobs. */
    cf_room room={0};cf_err rc=cf_room_find(db,message->room_id,&room);
    cf_ctx ctx={.app=h->app,.reader=db};
    cf_view_ctx view;cf_view_ctx_init(&view,&ctx,NULL);
    cf_broadcast_views views={&ctx,&view};cf_broadcast_partials partials;
    cf_broadcast_partials_views(&partials,&views);
    if(rc==CF_OK)rc=cf_broadcast_message_create(db,h->cable,&room,message,&partials);
    cf_room_dispose(&room);
    if(rc==CF_BUSY){cf_cable_log("dropped broadcast","webhook reply");return CF_OK;}
    return rc;
}

cf_err cf_jobs_handle_deliver_webhook(void *ctx, const cf_job *job) {
    cf_jobs_handler_ctx *h=ctx;
    if(!h||!job||job->kind!=CF_JOB_DELIVER_WEBHOOK||job->user_id<=0||job->message_id<=0)return CF_INVALID;
    cf_db *db=NULL;cf_err rc=jobs_reader_open(h,&db);if(rc!=CF_OK)return rc;
    cf_user bot={0};cf_message message={0};cf_webhook hook={0};
    cf_str payload={0},body={0};bool gone=false;
    cf_webhook_delivery delivery={0};cf_storage *storage=NULL;cf_active_staged staged={0};
    struct jobs_webhook_reply reply={.job=job,.source=&message,.sent=&hook};
    rc=cf_read_begin(db);
    if(rc!=CF_OK)goto done;
    rc=jobs_webhook_targets(db,job,&bot,&message,&hook,&gone);
    if(rc==CF_OK&&!gone){
        cf_str key={0};rc=cf_user_bot_key(&bot,&key);
        cf_builder path={0};char prefix[64],message_path[96];
        int n=snprintf(prefix,sizeof prefix,"/rooms/%lld/",(long long)message.room_id);
        if(rc==CF_OK)rc=cf_builder_append(&path,(cf_span){(const unsigned char*)prefix,(size_t)n});
        if(rc==CF_OK)rc=cf_builder_append(&path,(cf_span){(const unsigned char*)key.ptr,key.len});
        if(rc==CF_OK)rc=cf_builder_append(&path,(cf_span){(const unsigned char*)"/messages",9});
        n=snprintf(message_path,sizeof message_path,"/rooms/%lld@%lld",(long long)message.room_id,(long long)message.id);
        if(rc==CF_OK)rc=cf_webhook_payload(db,&hook,cf_tx_rich_text(NULL),&message,
            (cf_str){(char*)path.ptr,path.len},(cf_str){message_path,(size_t)n},&payload);
        cf_builder_dispose(&path);cf_str_dispose(&key);
    }
    {cf_err end=cf_read_end(db);if(rc==CF_OK)rc=end;}
    if(rc!=CF_OK)goto done;
    if(gone){atomic_fetch_add_explicit(&h->webhook_noop_gone,1,memory_order_relaxed);goto done;}
    if(!h->app){rc=CF_INVALID;goto done;}
    atomic_fetch_add_explicit(&h->webhook_attempted,1,memory_order_relaxed);
    cf_webhook_config config;cf_webhook_default_config(&config);
    cf_webhook_err error=cf_webhook_deliver(&config,hook.url.value.ptr,
        (const unsigned char*)payload.ptr,payload.len,&delivery);
    if(error!=CF_WEBHOOK_OK){rc=error==CF_WEBHOOK_NOMEM?CF_NOMEM:CF_IO;goto done;}
    cf_ctx jobctx={.app=h->app,.reader=db};
    if(delivery.timed_out || delivery.kind==CF_WEBHOOK_REPLY_TEXT){
        char timeout[96];const char *text=delivery.text;
        if(delivery.timed_out){snprintf(timeout,sizeof timeout,"Failed to respond within %u seconds",delivery.timeout_secs);text=timeout;}
        size_t text_len=delivery.timed_out?strlen(text):delivery.text_len;
        rc=cf_richtext_canonical_body(&jobctx,(cf_span){(const unsigned char*)text,text_len},&body);
        reply.body=(cf_optional_str){true,body};
    }else if(delivery.kind==CF_WEBHOOK_REPLY_ATTACHMENT){
        rc=cf_storage_open(cf_app_config(h->app)->storage_path,&storage);
        FILE *file=rc==CF_OK?tmpfile():NULL;
        if(rc==CF_OK&&!file)rc=CF_IO;
        if(rc==CF_OK && fwrite(delivery.data,1,delivery.data_len,file)!=delivery.data_len)rc=CF_IO;
        if(rc==CF_OK && fflush(file)!=0)rc=CF_IO;
        if(rc==CF_OK)rc=cf_active_stage_import(storage,fileno(file),
            (cf_span){(const unsigned char*)delivery.filename,strlen(delivery.filename)},
            (cf_span){(const unsigned char*)delivery.content_type,strlen(delivery.content_type)},true,CF_WEBHOOK_MAX_REPLY_SIZE,&staged);
        if(file)fclose(file);
        reply.staged=&staged;
    }else goto done;
    if(rc==CF_OK)rc=cf_write(h->app,jobs_webhook_reply_write,&reply);
    if(rc!=CF_OK)goto done;
    if(reply.gone){atomic_fetch_add_explicit(&h->webhook_noop_gone,1,memory_order_relaxed);goto done;}
    if(reply.staged){
        rc=cf_active_staged_commit(&staged);
        if(rc==CF_OK)rc=cf_media_process_attachment(&jobctx,&reply.blob);
        if(rc==CF_OK) {
            int64_t id=reply.reply.id;cf_message_dispose(&reply.reply);
            bool found=false;rc=cf_message_find_by_id(db,id,&found,&reply.reply);
            if(rc==CF_OK&&!found)goto done;
        }
    }
    if(rc==CF_OK)rc=jobs_webhook_broadcast(h,db,&reply.reply);
done:
    cf_active_staged_dispose(&staged);cf_storage_close(storage);
    cf_webhook_delivery_dispose(&delivery);cf_str_dispose(&payload);cf_str_dispose(&body);
    cf_message_dispose(&reply.reply);cf_blob_dispose(&reply.blob);
    cf_user_dispose(&bot);cf_message_dispose(&message);cf_webhook_dispose(&hook);
    cf_db_close(db);return rc;
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

typedef struct {
    int64_t root_id;
    int64_t *candidates;
    size_t count, capacity;
    cf_blob_vector deleted;
} jobs_purge_arg;

static cf_err jobs_purge_candidate(jobs_purge_arg *arg, int64_t id) {
    /* A shared descendant may need a later recheck after another parent
     * is removed. Already deleted candidates become harmless missing rows. */
    if (arg->count == arg->capacity) {
        size_t cap = arg->capacity ? arg->capacity * 2 : 8;
        if (cap < arg->capacity || cap > SIZE_MAX / sizeof *arg->candidates)
            return CF_NOMEM;
        int64_t *items = realloc(arg->candidates, cap * sizeof *items);
        if (items == NULL)
            return CF_NOMEM;
        arg->candidates = items;
        arg->capacity = cap;
    }
    arg->candidates[arg->count++] = id;
    return CF_OK;
}

static cf_err jobs_purge_sql(cf_db *db, const char *sql, int64_t id,
                             jobs_purge_arg *children) {
    sqlite3_stmt *stmt = NULL;
    int step = sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL);
    cf_err rc = cf_db_err(step);
    if (rc == CF_OK)
        rc = cf_db_err(sqlite3_bind_int64(stmt, 1, id));
    while (rc == CF_OK && (step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (children != NULL)
            rc = jobs_purge_candidate(children, sqlite3_column_int64(stmt, 0));
    }
    if (rc == CF_OK && step != SQLITE_DONE)
        rc = cf_db_err(step);
    int final_rc = sqlite3_finalize(stmt);
    return rc != CF_OK ? rc : cf_db_err(final_rc);
}

static cf_err jobs_purge_write(cf_tx *tx, void *opaque) {
    jobs_purge_arg *arg = opaque;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL)
        return CF_INTERNAL;
    cf_err rc = jobs_purge_candidate(arg, arg->root_id);
    for (size_t i = 0; rc == CF_OK && i < arg->count; i++) {
        cf_blob blob = {0};
        rc = cf_blob_find(db, arg->candidates[i], &blob);
        if (rc == CF_NOT_FOUND) {
            rc = CF_OK;
            continue;
        }
        if (rc != CF_OK) {
            cf_blob_dispose(&blob);
            break;
        }
        cf_attachment_vector refs = {0};
        rc = cf_attachment_records_for_blob(db, blob.id, &refs);
        bool unreferenced = rc == CF_OK && cf_active_purge_proceed(refs.len);
        cf_attachment_vector_dispose(&refs);
        if (rc != CF_OK || !unreferenced) {
            cf_blob_dispose(&blob);
            continue;
        }
        if (!cf_storage_key_valid(
                (cf_span){(unsigned char *)blob.key.ptr, blob.key.len})) {
            cf_blob_dispose(&blob);
            return CF_INVALID;
        }
        if (arg->deleted.len == arg->deleted.cap) {
            size_t cap = arg->deleted.cap ? arg->deleted.cap * 2 : 8;
            if (cap < arg->deleted.cap || cap > SIZE_MAX / sizeof(cf_blob)) {
                cf_blob_dispose(&blob);
                return CF_NOMEM;
            }
            cf_blob *items = realloc(arg->deleted.items, cap * sizeof *items);
            if (items == NULL) {
                cf_blob_dispose(&blob);
                return CF_NOMEM;
            }
            arg->deleted.items = items;
            arg->deleted.cap = cap;
        }
        /* Derived images are separate blobs. Remove only the parent's
         * ownership edges, then recheck every child before deleting it. */
        const char *owned =
            "SELECT blob_id FROM active_storage_attachments WHERE "
            "(record_type='ActiveStorage::Blob' AND record_id=?1) OR "
            "(record_type='ActiveStorage::VariantRecord' AND record_id IN "
            "(SELECT id FROM active_storage_variant_records WHERE blob_id=?1))";
        rc = jobs_purge_sql(db, owned, blob.id, arg);
        if (rc == CF_OK)
            rc = jobs_purge_sql(
                db,
                "DELETE FROM active_storage_attachments WHERE "
                "(record_type='ActiveStorage::Blob' AND record_id=?1) OR "
                "(record_type='ActiveStorage::VariantRecord' AND record_id IN "
                "(SELECT id FROM active_storage_variant_records WHERE "
                "blob_id=?1))",
                blob.id, NULL);
        if (rc == CF_OK)
            rc = jobs_purge_sql(
                db,
                "DELETE FROM active_storage_variant_records WHERE blob_id=?1",
                blob.id, NULL);
        if (rc == CF_OK)
            rc = jobs_purge_sql(db,
                                "DELETE FROM active_storage_blobs WHERE id=?1",
                                blob.id, NULL);
        if (rc == CF_OK)
            arg->deleted.items[arg->deleted.len++] = blob;
        else
            cf_blob_dispose(&blob);
    }
    return rc;
}

cf_err cf_jobs_handle_purge_blob(void *ctx, const cf_job *job) {
    cf_jobs_handler_ctx *h = ctx;
    if (h == NULL || job == NULL || job->kind != CF_JOB_PURGE_BLOB ||
        job->blob_id <= 0)
        return CF_INVALID;
    if (h->app == NULL)
        return CF_INTERNAL;
    const cf_config *config = cf_app_config(h->app);
    if (config == NULL || config->storage_path == NULL)
        return CF_INTERNAL;
    cf_storage *storage = NULL;
    cf_err rc = cf_storage_open(config->storage_path, &storage);
    if (rc != CF_OK)
        return rc;
    jobs_purge_arg arg = {.root_id = job->blob_id};
    rc = cf_write(h->app, jobs_purge_write, &arg);
    if (rc == CF_OK && arg.deleted.len == 0)
        atomic_fetch_add_explicit(&h->purge_noop_gone, 1, memory_order_relaxed);
    if (rc == CF_OK) {
        /* File I/O runs on this job worker after the metadata transaction
         * commits. Continue cleanup after one failure, but report it.
         * Jobs have no retry: a filesystem failure can retain orphan files;
         * the metadata remains deleted to prevent unsafe reattachment. */
        for (size_t i = 0; i < arg.deleted.len; i++) {
            cf_blob *blob = &arg.deleted.items[i];
            bool image = blob->content_type.present &&
                         blob->content_type.value.len >= 6 &&
                         memcmp(blob->content_type.value.ptr, "image/", 6) == 0;
            cf_err file_rc = cf_active_purge_files(
                storage,
                (cf_span){(unsigned char *)blob->key.ptr, blob->key.len},
                image);
            if (file_rc != CF_OK && rc == CF_OK)
                rc = file_rc;
        }
        if (rc == CF_OK && arg.deleted.len > 0)
            atomic_fetch_add_explicit(&h->purge_deleted, 1,
                                      memory_order_relaxed);
    }
    cf_blob_vector_dispose(&arg.deleted);
    free(arg.candidates);
    cf_storage_close(storage);
    return rc;
}

/* ---- Media analysis (CF_JOB_MEDIA) -------------------------------------- */

struct jobs_media_write {
    int64_t blob_id;
    cf_str metadata;
    bool gone;
};

static cf_err jobs_media_write_cb(cf_tx *tx, void *arg) {
    struct jobs_media_write *write = arg;
    cf_blob current = {0};
    cf_err rc = cf_blob_find(cf_tx_db(tx), write->blob_id, &current);
    if (rc == CF_NOT_FOUND) {write->gone = true;return CF_OK;}
    cf_blob_dispose(&current);
    if (rc != CF_OK) return rc;
    rc = cf_blob_update_metadata(tx, write->blob_id, write->metadata);
    /* Analyze attachment touches the current attached records, never stale
     * request objects. This is the message analyzer's source behavior. */
    cf_attachment_vector records = {0};
    if (rc == CF_OK) rc = cf_attachment_records_for_blob(cf_tx_db(tx), write->blob_id, &records);
    for (size_t i = 0; rc == CF_OK && i < records.len; i++) {
        cf_attachment *record = &records.items[i];
        if (record->record_type.len != 7 || memcmp(record->record_type.ptr,"Message",7) != 0) continue;
        cf_message message = {0};
        rc = cf_message_find(cf_tx_db(tx), record->record_id, &message);
        if (rc == CF_NOT_FOUND) rc = CF_OK;
        else if (rc == CF_OK) rc = cf_message_touch(tx,&message);
        cf_message_dispose(&message);
    }
    cf_attachment_vector_dispose(&records);
    return rc;
}

cf_err cf_jobs_handle_media(void *ctx, const cf_job *job) {
    cf_jobs_handler_ctx *h = ctx;
    if (h == NULL || job == NULL || job->kind != CF_JOB_MEDIA ||
        job->blob_id <= 0 || job->task_type == NULL ||
        strcmp(job->task_type,"analyze") != 0) return CF_INVALID;
    cf_db *db = NULL;
    cf_blob blob = {0};
    cf_err rc = jobs_reader_open(h,&db);
    if (rc == CF_OK) rc = cf_blob_find(db,job->blob_id,&blob);
    cf_db_close(db);
    if (rc == CF_NOT_FOUND) {
        atomic_fetch_add_explicit(&h->media_noop,1,memory_order_relaxed);
        return CF_OK;
    }
    if (rc != CF_OK) return rc;
    cf_builder metadata = {0};
    /* Subprocess wait happens before cf_write; no writer or read transaction
     * is held while one of the four media slots is occupied. */
    rc = cf_media_analyze_blob(h->app,&blob,&metadata);
    cf_blob_dispose(&blob);
    if (rc == CF_OK) {
        struct jobs_media_write write = {.blob_id=job->blob_id,
            .metadata={(char *)metadata.ptr,metadata.len}};
        rc = cf_write(h->app,jobs_media_write_cb,&write);
        if (rc == CF_OK && write.gone)
            atomic_fetch_add_explicit(&h->media_noop,1,memory_order_relaxed);
    }
    cf_builder_dispose(&metadata);
    return rc;
}

/* ---- registration ---------------------------------------------------------- */

cf_err cf_jobs_register_default_handlers(cf_jobs *jobs,
                                         cf_jobs_handler_ctx *ctx) {
    if (jobs == NULL || ctx == NULL) return CF_INVALID;
    ctx->jobs = jobs;
    if (ctx->app != NULL) cf_app_set_jobs(ctx->app, jobs);
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
