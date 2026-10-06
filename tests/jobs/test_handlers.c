/* J02 handler acceptance tests (07-verification.md JOB-02/JOB-03 and the
 * DB-02 re-read rule; 04-cable-jobs.md "J01/J02: bounded jobs").
 *
 * Exercises src/jobs/handlers.c through real committed state: a scratch
 * database with a started app, writer and jobs queue, the five default
 * handlers registered, and the writer post-commit consumer wired. No case
 * silently skips: a setup failure is a CF_REQUIRE failure. Webhook cases
 * exercise actual loopback HTTP requests, persisted text/attachment replies,
 * in-flight revocation, imported-file policy and transport failures. Other
 * cases cover real push encryption/delivery outcomes, off-writer analysis,
 * atomic purge, bounded ban batches and queue/commit accounting.
 */
#include "cf_test.h"

#include "app.h"
#include "config.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "jobs/handlers.h"
#include "jobs/jobs.h"

#include "jobs/jobs_testutil.h"

#include <sqlite3.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <poll.h>
#include <strings.h>
#include <pthread.h>
#include <time.h>
#include "storage/storage.h"
#include "storage/active_storage.h"
#include "actions/avatar_test.h"

static const char CF_TEST_HEX64[] =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    cf_jobs *jobs;
    cf_jobs_handler_ctx hctx;
    bool writer_started;
    bool jobs_started;
} handlers_fixture;

static cf_config *make_config(const char *db_path) {
    cf_config_entry entries[9] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:3999"},
        {"SECRET_KEY_BASE", CF_TEST_HEX64},
        {"DATABASE_PATH", db_path},
        {"CF_WRITER_QUEUE", "8"},
        {"CF_JOB_QUEUE", "128"},
        {"CF_JOB_WORKERS", "2"},
        {"VAPID_PUBLIC_KEY", "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8cTriz_qYBVicY02_VxTQ="},
        {"VAPID_PRIVATE_KEY", "qfXLHghuG1rSHZUVo9SscNRI-0EIHRbIrfeGCqbAwak="},
        {"VAPID_SUBJECT", "mailto:test@example.com"},
    };
    cf_config *config = NULL;
    cf_config_error err;
    if (cf_config_parse(entries, 9, &err, &config) != CF_OK) return NULL;
    return config;
}

static bool fixture_open(handlers_fixture *f) {
    memset(f, 0, sizeof *f);
    if (!cf_db_scratch_open(&f->scratch)) return false;
    f->config = make_config(f->scratch.path);
    if (f->config == NULL) return false;
    if (cf_app_create(f->config, &f->app) != CF_OK) {
        cf_config_destroy(f->config);
        f->config = NULL;
        return false;
    }
    f->config = NULL; /* owned by the app now */
    if (cf_writer_start(f->app, cf_app_config(f->app)) != CF_OK) return false;
    f->writer_started = true;
    if (cf_jobs_start(cf_app_config(f->app), &f->jobs) != CF_OK) return false;
    f->jobs_started = true;
    memset(&f->hctx, 0, sizeof f->hctx);
    f->hctx.app = f->app;
    f->hctx.cable = NULL; /* broadcasts skipped: recorded, no sockets */
    if (cf_jobs_register_default_handlers(f->jobs, &f->hctx) != CF_OK) {
        return false;
    }
    if (cf_jobs_register_writer(f->jobs, f->app) != CF_OK) return false;
    return true;
}

static void fixture_close(handlers_fixture *f) {
    /* Shutdown order main.c/J02 must use: jobs join before app teardown. */
    if (f->jobs_started) {
        cf_jobs_stop(f->jobs);
        cf_jobs_destroy(f->jobs);
        f->jobs_started = false;
    }
    if (f->writer_started) cf_writer_stop(f->app);
    if (f->app != NULL) cf_app_destroy(f->app);
    cf_db_scratch_close(&f->scratch);
}

static bool seed_sql(handlers_fixture *f, const char *sql) {
    return cf_db_test_exec(cf_db_handle(f->scratch.db), sql) == SQLITE_OK;
}

/* One member user (id 1) and one open room (id 1). role/status: 0/0. */
static bool seed_user_room(handlers_fixture *f) {
    return seed_sql(f,
                    "INSERT INTO users (id,name,created_at,updated_at,role,"
                    "status) VALUES (1,'Seed','2026-01-01 "
                    "00:00:00','2026-01-01 00:00:00',0,0);"
                    "INSERT INTO rooms (id,creator_id,type,created_at,"
                    "updated_at) VALUES (1,1,'Rooms::Open','2026-01-01 "
                    "00:00:00','2026-01-01 00:00:00');");
}

static bool seed_message(handlers_fixture *f, int64_t id, int64_t room_id,
                         int64_t creator_id, int64_t seq) {
    char sql[256];
    snprintf(sql, sizeof sql,
             "INSERT INTO messages (id,client_message_id,created_at,"
             "creator_id,room_id,updated_at) VALUES (%lld,'cmid-%lld',"
             "'2026-01-01 00:00:00',%lld,%lld,'2026-01-01 00:00:00');",
             (long long)id, (long long)seq, (long long)creator_id,
             (long long)room_id);
    return seed_sql(f, sql);
}

static int64_t table_count(handlers_fixture *f, const char *table,
                           const char *where) {
    char sql[256];
    if (where != NULL) {
        snprintf(sql, sizeof sql, "SELECT count(*) FROM %s WHERE %s;", table,
                 where);
    } else {
        snprintf(sql, sizeof sql, "SELECT count(*) FROM %s;", table);
    }
    bool ok = false;
    int64_t n = cf_db_test_i64(cf_db_handle(f->scratch.db), sql, &ok);
    if (!ok) return -1;
    return n;
}

static uint64_t ctx_load(const _Atomic uint64_t *counter) {
    return atomic_load_explicit(counter, memory_order_relaxed);
}

static cf_job push_job(int64_t room_id, int64_t message_id) {
    cf_job job;
    memset(&job, 0, sizeof job);
    job.kind = CF_JOB_PUSH_MESSAGE;
    job.room_id = room_id;
    job.message_id = message_id;
    return job;
}

static cf_job webhook_job(int64_t user_id, int64_t message_id) {
    cf_job job;
    memset(&job, 0, sizeof job);
    job.kind = CF_JOB_DELIVER_WEBHOOK;
    job.user_id = user_id;
    job.message_id = message_id;
    return job;
}

static cf_job banned_job(int64_t user_id) {
    cf_job job;
    memset(&job, 0, sizeof job);
    job.kind = CF_JOB_REMOVE_BANNED_CONTENT;
    job.user_id = user_id;
    return job;
}

static cf_job purge_job(int64_t blob_id) {
    cf_job job;
    memset(&job, 0, sizeof job);
    job.kind = CF_JOB_PURGE_BLOB;
    job.blob_id = blob_id;
    return job;
}

typedef struct {
    cf_event event;
} event_write_arg;

static cf_err append_event_cb(cf_tx *tx, void *arg) {
    event_write_arg *w = arg;
    return cf_tx_event(tx, w->event);
}

static cf_err write_event(handlers_fixture *f, cf_event event) {
    event_write_arg arg = {.event = event};
    return cf_write(f->app, append_event_cb, &arg);
}

/* --- registration -------------------------------------------------------- */

CF_TEST(handlers_register_all_kinds) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    for (int k = 0; k < CF_JOB_KIND_COUNT; k++) {
        cf_job_stats stats;
        CF_REQUIRE(cf_jobs_stats_get(f.jobs, (cf_job_kind)k, &stats) == CF_OK);
        CF_CHECK(stats.handler_registered);
    }
    CF_CHECK(cf_jobs_register_default_handlers(NULL, &f.hctx) == CF_INVALID);
    CF_CHECK(cf_jobs_register_default_handlers(f.jobs, NULL) == CF_INVALID);
    fixture_close(&f);
}

/* --- PushMessage ---------------------------------------------------------- */

CF_TEST(handlers_push_message_missing_message_is_noop) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f));

    cf_job job = push_job(1, 4242); /* no such message */
    CF_CHECK(cf_jobs_handle_push_message(&f.hctx, &job) == CF_OK);
    CF_CHECK(ctx_load(&f.hctx.push_noop_gone) == 1);
    CF_CHECK(ctx_load(&f.hctx.push_attempted) == 0);

    CF_CHECK(cf_jobs_handle_push_message(NULL, &job) == CF_INVALID);
    CF_CHECK(cf_jobs_handle_push_message(&f.hctx, NULL) == CF_INVALID);
    fixture_close(&f);
}

CF_TEST(handlers_push_message_empty_recipients_is_noop) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f));
    CF_REQUIRE(seed_message(&f, 1, 1, 1, 1));

    /* There is no subscriber other than the sender: empty selection needs
     * no network call and is an explicit no-recipient no-op. */
    cf_job job = push_job(1, 1);
    CF_CHECK(cf_jobs_handle_push_message(&f.hctx, &job) == CF_OK);
    CF_CHECK(ctx_load(&f.hctx.push_noop_gone) == 0);
    CF_CHECK(ctx_load(&f.hctx.push_noop_no_recipients) == 1);
    CF_CHECK(ctx_load(&f.hctx.push_attempted) == 0);
    CF_CHECK(table_count(&f, "messages", NULL) == 1); /* untouched */
    fixture_close(&f);
}

static cf_optional_str handlers_push_resolve(void *arg, cf_str host) {
    (void)arg;(void)host;
    char *ip = strdup("8.8.8.8");
    return (cf_optional_str){ip != NULL,{ip,ip != NULL ? strlen(ip) : 0}};
}

typedef struct {unsigned calls,status;cf_push_transport_error error;} push_probe;
static cf_err handlers_push_exchange(void *arg,const cf_push_request *req,
    unsigned *status,char *reason,size_t cap,cf_push_transport_error *error) {
    push_probe *probe = arg;
    probe->calls++;
    CF_CHECK(req->body_len > 100 && req->body_len <= CF_PUSH_MAX_RECORD_BYTES);
    CF_CHECK(strcmp(req->host,"fcm.googleapis.com") == 0);
    CF_CHECK(strcmp(req->resolved_ip,"8.8.8.8") == 0);
    *status=probe->status;*error=probe->error;
    if (cap) reason[0]='\0';
    return CF_OK;
}

static bool seed_push_recipient(handlers_fixture *f) {
    return seed_sql(f,
        "INSERT INTO users(id,name,created_at,updated_at) VALUES(2,'Recipient','2026-01-01 00:00:00','2026-01-01 00:00:00');"
        "INSERT INTO memberships(room_id,user_id,involvement,created_at,updated_at) VALUES(1,2,'everything','2026-01-01 00:00:00','2026-01-01 00:00:00');"
        "INSERT INTO push_subscriptions(id,user_id,endpoint,p256dh_key,auth_key,created_at,updated_at) VALUES(1,2,'https://fcm.googleapis.com/fcm/send/test',"
        "'BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4',"
        "'BTBZMqHH6r4Tts7J_aSIgg','2026-01-01 00:00:00','2026-01-01 00:00:00');");
}

CF_TEST(handlers_push_message_delivers_and_invalidates_gone_subscription) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f));
    CF_REQUIRE(seed_message(&f,1,1,1,1));
    CF_REQUIRE(seed_push_recipient(&f));
    push_probe probe={.status=201};
    f.hctx.push_resolve=handlers_push_resolve;
    f.hctx.push_exchange=handlers_push_exchange;
    f.hctx.push_exchange_ctx=&probe;
    cf_job job=push_job(1,1);
    CF_CHECK(cf_jobs_handle_push_message(&f.hctx,&job)==CF_OK);
    CF_CHECK(probe.calls==1);
    CF_CHECK(table_count(&f,"push_subscriptions",NULL)==1);
    probe.status=410;
    CF_CHECK(cf_jobs_handle_push_message(&f.hctx,&job)==CF_OK);
    CF_CHECK(probe.calls==2);
    CF_CHECK(table_count(&f,"push_subscriptions",NULL)==0);
    fixture_close(&f);
}

CF_TEST(handlers_push_message_tls_failure_preserves_subscription) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f));
    CF_REQUIRE(seed_message(&f,1,1,1,1));
    CF_REQUIRE(seed_push_recipient(&f));
    push_probe probe={.error=CF_PUSH_TRANSPORT_TLS};
    f.hctx.push_resolve=handlers_push_resolve;
    f.hctx.push_exchange=handlers_push_exchange;
    f.hctx.push_exchange_ctx=&probe;
    cf_job job=push_job(1,1);
    CF_CHECK(cf_jobs_handle_push_message(&f.hctx,&job)!=CF_OK);
    CF_CHECK(probe.calls==1);
    CF_CHECK(table_count(&f,"push_subscriptions",NULL)==1);
    fixture_close(&f);
}

CF_TEST(handlers_push_message_room_mismatch_is_noop) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f));
    CF_REQUIRE(seed_sql(&f,
                        "INSERT INTO rooms (id,creator_id,type,created_at,"
                        "updated_at) VALUES (2,1,'Rooms::Open','2026-01-01 "
                        "00:00:00','2026-01-01 00:00:00');"));
    CF_REQUIRE(seed_message(&f, 1, 1, 1, 1));

    cf_job job = push_job(2, 1); /* message 1 lives in room 1 */
    CF_CHECK(cf_jobs_handle_push_message(&f.hctx, &job) == CF_OK);
    CF_CHECK(ctx_load(&f.hctx.push_noop_gone) == 1);
    fixture_close(&f);
}

/* --- DeliverWebhook -------------------------------------------------------- */

CF_TEST(handlers_deliver_webhook_missing_bot_is_noop) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f));

    cf_job job = webhook_job(55, 1); /* no such bot, no such message */
    CF_CHECK(cf_jobs_handle_deliver_webhook(&f.hctx, &job) == CF_OK);
    CF_CHECK(ctx_load(&f.hctx.webhook_noop_gone) == 1);
    CF_CHECK(ctx_load(&f.hctx.webhook_attempted) == 0);
    fixture_close(&f);
}

/* The real webhook transport receives the canonical job payload. */
typedef struct { int fd; unsigned port; pthread_t thread; char request[16384]; size_t len, body_len; const char *mime, *body, *db_path, *mutation; bool mutated; } webhook_server;
static void *webhook_server_run(void *arg) {
    webhook_server *s=arg;
    struct pollfd p={s->fd,POLLIN,0};
    if (poll(&p,1,1500)<=0) return NULL;
    int client=accept(s->fd,NULL,NULL); if(client<0)return NULL;
    struct timeval timeout={2,0};
    setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
    size_t wanted=0;
    while(s->len+1<sizeof s->request) {
        ssize_t n=recv(client,s->request+s->len,sizeof s->request-s->len-1,0);
        if(n<=0)break;
        s->len+=(size_t)n;s->request[s->len]=0;
        char *end=strstr(s->request,"\r\n\r\n");
        if(end && !wanted) {
            char *cl=strcasestr(s->request,"Content-Length:");
            wanted=(size_t)(end-s->request)+4+(cl?strtoul(cl+15,NULL,10):0);
        }
        if(wanted && s->len>=wanted)break;
    }
    if(s->mutation) {
        sqlite3 *db=NULL;
        if(sqlite3_open(s->db_path,&db)==SQLITE_OK) {
            s->mutated=sqlite3_exec(db,s->mutation,NULL,NULL,NULL)==SQLITE_OK;
        }
        sqlite3_close(db);
    }
    const char *body=s->body?s->body:"Hello from bot!";
    const char *mime=s->mime?s->mime:"text/plain";
    size_t body_len=s->body_len?s->body_len:strlen(body);
    char response[512];int n=snprintf(response,sizeof response,
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",mime,body_len);
    (void)send(client,response,(size_t)n,MSG_NOSIGNAL);
    size_t sent=0;
    while(sent<body_len){ssize_t count=send(client,body+sent,body_len-sent,MSG_NOSIGNAL);if(count<=0)break;sent+=(size_t)count;}
    close(client);return NULL;
}
static bool webhook_server_start(webhook_server *s) {
    s->fd=socket(AF_INET,SOCK_STREAM,0);if(s->fd<0)return false;
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    socklen_t size=sizeof addr;
    if(bind(s->fd,(struct sockaddr*)&addr,sizeof addr)||listen(s->fd,1)||getsockname(s->fd,(struct sockaddr*)&addr,&size)) {close(s->fd);return false;}
    s->port=ntohs(addr.sin_port);
    if(pthread_create(&s->thread,NULL,webhook_server_run,s)!=0){close(s->fd);return false;}
    return true;
}
CF_TEST(handlers_deliver_webhook_real_http_persists_reply) {
    handlers_fixture f;CF_REQUIRE(fixture_open(&f));CF_REQUIRE(seed_user_room(&f));
    CF_REQUIRE(seed_message(&f,1,1,1,1));
    webhook_server server={0};CF_REQUIRE(webhook_server_start(&server));
    char sql[1024];snprintf(sql,sizeof sql,
        "INSERT INTO users(id,name,bot_token,created_at,updated_at,role,status) VALUES(5,'Bot','bot-secret','2026-01-01 00:00:00','2026-01-01 00:00:00',2,0);"
        "INSERT INTO memberships(room_id,user_id,involvement,created_at,updated_at) VALUES(1,5,'everything','2026-01-01 00:00:00','2026-01-01 00:00:00');"
        "INSERT INTO webhooks(user_id,url,created_at,updated_at) VALUES(5,'http://127.0.0.1:%u/hook','2026-01-01 00:00:00','2026-01-01 00:00:00');",server.port);
    CF_CHECK(seed_sql(&f,sql));
    cf_job job=webhook_job(5,1);
    CF_CHECK(cf_jobs_handle_deliver_webhook(&f.hctx,&job)==CF_OK);
    pthread_join(server.thread,NULL);close(server.fd);
    CF_CHECK(strstr(server.request,"POST /hook HTTP/1.1")!=NULL);
    CF_CHECK(strstr(server.request,"\"user\":{\"id\":1,\"name\":\"Seed\"}")!=NULL);
    CF_CHECK(strstr(server.request,"/rooms/1/5-bot-secret/messages")!=NULL);
    CF_CHECK(strstr(server.request,"/rooms/1@1")!=NULL);
    CF_CHECK(table_count(&f,"messages","creator_id=5")==1);
    CF_CHECK(table_count(&f,"action_text_rich_texts","body LIKE '%Hello from bot!%'")==1);
    CF_CHECK(ctx_load(&f.hctx.webhook_attempted)==1);
    fixture_close(&f);
}

static bool webhook_seed(handlers_fixture *f,unsigned port) {
    char sql[1024];snprintf(sql,sizeof sql,
        "INSERT INTO users(id,name,bot_token,created_at,updated_at,role,status) VALUES(5,'Bot','bot-secret','2026-01-01 00:00:00','2026-01-01 00:00:00',2,0);"
        "INSERT INTO memberships(room_id,user_id,involvement,created_at,updated_at) VALUES(1,5,'everything','2026-01-01 00:00:00','2026-01-01 00:00:00');"
        "INSERT INTO webhooks(user_id,url,created_at,updated_at) VALUES(5,'http://127.0.0.1:%u/hook','2026-01-01 00:00:00','2026-01-01 00:00:00');",port);
    return seed_sql(f,sql);
}
CF_TEST(handlers_deliver_webhook_real_attachment_is_staged_and_analyzed) {
    handlers_fixture f;CF_REQUIRE(fixture_open(&f));CF_REQUIRE(seed_user_room(&f));
    CF_REQUIRE(seed_message(&f,1,1,1,1));
    char root[]="/tmp/cf-webhook-attachment-XXXXXX";
    avatar_test_root((cf_config*)cf_app_config(f.app),root);
    webhook_server server={.mime="application/pdf",.body="%PDF-1.7\nreply data"};
    CF_REQUIRE(webhook_server_start(&server));CF_CHECK(webhook_seed(&f,server.port));
    cf_job job=webhook_job(5,1);
    CF_CHECK(cf_jobs_handle_deliver_webhook(&f.hctx,&job)==CF_OK);
    pthread_join(server.thread,NULL);close(server.fd);
    CF_CHECK(table_count(&f,"messages","creator_id=5")==1);
    CF_CHECK(table_count(&f,"active_storage_blobs","filename='attachment.pdf' AND metadata LIKE '%analyzed%'")==1);
    CF_CHECK(table_count(&f,"active_storage_attachments","record_type='Message' AND name='attachment'")==1);
    sqlite3_stmt *stmt=NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(f.scratch.db),"SELECT key FROM active_storage_blobs",-1,&stmt,NULL)==SQLITE_OK);
    CF_REQUIRE(sqlite3_step(stmt)==SQLITE_ROW);
    const char *key=(const char*)sqlite3_column_text(stmt,0);
    cf_storage *storage=NULL;int fd=-1;uint64_t size=0;
    CF_REQUIRE(cf_storage_open(root,&storage)==CF_OK);
    CF_CHECK(cf_storage_open_read(storage,(cf_span){(const unsigned char*)key,strlen(key)},&fd,&size)==CF_OK);
    char bytes[64]={0};ssize_t count=fd<0?-1:read(fd,bytes,sizeof bytes);
    CF_CHECK(count==(ssize_t)strlen(server.body));CF_CHECK(size==strlen(server.body));
    CF_CHECK(count>0&&memcmp(bytes,server.body,(size_t)count)==0);
    if(fd>=0)close(fd);cf_storage_close(storage);sqlite3_finalize(stmt);
    fixture_close(&f);avatar_test_remove_tree(root);
}
CF_TEST(handlers_deliver_webhook_text_keeps_bytes_after_nul) {
    handlers_fixture f;CF_REQUIRE(fixture_open(&f));CF_REQUIRE(seed_user_room(&f));
    CF_REQUIRE(seed_message(&f,1,1,1,1));
    webhook_server server={.body="before\0after",.body_len=12};
    CF_REQUIRE(webhook_server_start(&server));CF_CHECK(webhook_seed(&f,server.port));
    cf_job job=webhook_job(5,1);CF_CHECK(cf_jobs_handle_deliver_webhook(&f.hctx,&job)==CF_OK);
    pthread_join(server.thread,NULL);close(server.fd);
    CF_CHECK(table_count(&f,"action_text_rich_texts","body LIKE '%after%'")==1);
    fixture_close(&f);
}
CF_TEST(handlers_deliver_webhook_imports_above_browser_upload_cap) {
    handlers_fixture f;CF_REQUIRE(fixture_open(&f));CF_REQUIRE(seed_user_room(&f));
    CF_REQUIRE(seed_message(&f,1,1,1,1));
    char root[]="/tmp/cf-webhook-large-XXXXXX";
    avatar_test_root((cf_config*)cf_app_config(f.app),root);
    size_t len=17*1024*1024;char *body=malloc(len);CF_REQUIRE(body!=NULL);
    memset(body,'x',len);memcpy(body,"%PDF-1.7\n",9);
    /* The user-upload entry still rejects identical bytes at16MiB. */
    cf_storage *storage=NULL;CF_REQUIRE(cf_storage_open(root,&storage)==CF_OK);
    FILE *spool=tmpfile();CF_REQUIRE(spool!=NULL);
    CF_REQUIRE(fwrite(body,1,len,spool)==len);CF_REQUIRE(fflush(spool)==0);
    cf_active_staged staged={0};
    CF_CHECK(cf_active_stage_upload(storage,fileno(spool),(cf_span){(const unsigned char*)"attachment.pdf",14},
        (cf_span){(const unsigned char*)"application/pdf",15},true,&staged)==CF_LIMIT);
    cf_active_staged_dispose(&staged);fclose(spool);cf_storage_close(storage);
    webhook_server server={.mime="application/pdf",.body=body,.body_len=len};
    CF_REQUIRE(webhook_server_start(&server));CF_CHECK(webhook_seed(&f,server.port));
    cf_job job=webhook_job(5,1);CF_CHECK(cf_jobs_handle_deliver_webhook(&f.hctx,&job)==CF_OK);
    pthread_join(server.thread,NULL);close(server.fd);free(body);
    CF_CHECK(table_count(&f,"active_storage_blobs","byte_size=17825792 AND metadata LIKE '%analyzed%'")==1);
    CF_CHECK(table_count(&f,"messages","creator_id=5")==1);
    CF_CHECK(avatar_test_file_count(root)==1);
    fixture_close(&f);avatar_test_remove_tree(root);
}

CF_TEST(handlers_deliver_webhook_rechecks_inflight_revocation_and_rolls_back_staging) {
    const char *mutations[]={"UPDATE users SET status=1 WHERE id=5",
        "DELETE FROM memberships WHERE user_id=5", "DELETE FROM messages WHERE id=1",
        "UPDATE webhooks SET url='http://127.0.0.1/replacement' WHERE user_id=5",
        "DELETE FROM webhooks WHERE user_id=5"};
    for(size_t i=0;i<sizeof mutations/sizeof mutations[0];i++) {
        handlers_fixture f;CF_REQUIRE(fixture_open(&f));CF_REQUIRE(seed_user_room(&f));
        CF_REQUIRE(seed_message(&f,1,1,1,1));
        char root[]="/tmp/cf-webhook-revoked-XXXXXX";
        avatar_test_root((cf_config*)cf_app_config(f.app),root);
        webhook_server server={.mime="application/pdf",.body="%PDF-1.7\nreply data",
            .db_path=f.scratch.path,.mutation=mutations[i]};
        CF_REQUIRE(webhook_server_start(&server));CF_CHECK(webhook_seed(&f,server.port));
        cf_job job=webhook_job(5,1);
        CF_CHECK(cf_jobs_handle_deliver_webhook(&f.hctx,&job)==CF_OK);
        pthread_join(server.thread,NULL);close(server.fd);
        CF_CHECK(server.mutated);CF_CHECK(server.len>0);
        CF_CHECK(ctx_load(&f.hctx.webhook_attempted)==1);
        CF_CHECK(ctx_load(&f.hctx.webhook_noop_gone)==1);
        CF_CHECK(table_count(&f,"messages","creator_id=5")==0);
        CF_CHECK(table_count(&f,"active_storage_blobs",NULL)==0);
        CF_CHECK(avatar_test_file_count(root)==0);
        fixture_close(&f);avatar_test_remove_tree(root);
    }
}
CF_TEST(handlers_deliver_webhook_unreachable_transport_fails) {
    handlers_fixture f;CF_REQUIRE(fixture_open(&f));CF_REQUIRE(seed_user_room(&f));
    CF_REQUIRE(seed_message(&f,1,1,1,1));CF_REQUIRE(webhook_seed(&f,1));
    cf_job job=webhook_job(5,1);
    CF_CHECK(cf_jobs_handle_deliver_webhook(&f.hctx,&job)==CF_IO);
    CF_CHECK(ctx_load(&f.hctx.webhook_attempted)==1);
    CF_CHECK(table_count(&f,"messages","creator_id=5")==0);fixture_close(&f);
}

CF_TEST(handlers_deliver_webhook_revoked_targets_are_noop) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f));
    CF_REQUIRE(seed_sql(
        &f,
        /* Active bot with no webhook row: hook revoked. */
        "INSERT INTO users (id,name,created_at,updated_at,role,status) "
        "VALUES (6,'Bot6','2026-01-01 00:00:00','2026-01-01 00:00:00',2,0);"
        /* Deactivated bot with a webhook row: no longer active. */
        "INSERT INTO users (id,name,created_at,updated_at,role,status) "
        "VALUES (7,'Bot7','2026-01-01 00:00:00','2026-01-01 00:00:00',2,1);"
        "INSERT INTO webhooks (user_id,url,created_at,updated_at) VALUES "
        "(7,'https://bot.example/hook7','2026-01-01 "
        "00:00:00','2026-01-01 00:00:00');"));
    CF_REQUIRE(seed_message(&f, 1, 1, 1, 1));

    CF_CHECK(cf_jobs_handle_deliver_webhook(&f.hctx,
                                            &(cf_job){0}) == CF_INVALID);
    cf_job no_hook = webhook_job(6, 1);
    CF_CHECK(cf_jobs_handle_deliver_webhook(&f.hctx, &no_hook) == CF_OK);
    cf_job inactive = webhook_job(7, 1);
    CF_CHECK(cf_jobs_handle_deliver_webhook(&f.hctx, &inactive) == CF_OK);
    CF_CHECK(ctx_load(&f.hctx.webhook_noop_gone) == 2);
    fixture_close(&f);
}

/* --- RemoveBannedContent ---------------------------------------------------- */

CF_TEST(handlers_remove_banned_batches_100_and_requeues) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_sql(
        &f,
        "INSERT INTO users (id,name,created_at,updated_at,role,status) "
        "VALUES (9,'Banned','2026-01-01 00:00:00','2026-01-01 "
        "00:00:00',0,2);"
        "INSERT INTO rooms (id,creator_id,type,created_at,updated_at) "
        "VALUES (1,1,'Rooms::Open','2026-01-01 "
        "00:00:00','2026-01-01 00:00:00');"));
    for (int64_t i = 1; i <= 205; i++) {
        CF_REQUIRE(seed_message(&f, i, 1, 9, i));
    }
    CF_REQUIRE(table_count(&f, "messages", "creator_id = 9") == 205);

    /* Hold the queue's own consumer so the requeue lands observably: the
     * direct handler call below must destroy exactly one batch of 100 and
     * leave one pending remainder. */
    job_gate gate;
    job_gate_init(&gate);
    job_gate_block(&gate, CF_OK);
    CF_REQUIRE(cf_jobs_set_handler(f.jobs, CF_JOB_REMOVE_BANNED_CONTENT,
                                   job_gate_handler, &gate) == CF_OK);

    cf_job job = banned_job(9);
    CF_REQUIRE(cf_jobs_handle_remove_banned_content(&f.hctx, &job) == CF_OK);
    CF_CHECK(table_count(&f, "messages", "creator_id = 9") == 105);
    CF_CHECK(ctx_load(&f.hctx.banned_batches) == 1);
    CF_CHECK(ctx_load(&f.hctx.banned_destroyed) == 100);
    CF_CHECK(ctx_load(&f.hctx.banned_requeued) == 1);
    CF_CHECK(ctx_load(&f.hctx.banned_requeue_dropped) == 0);
    CF_REQUIRE(job_gate_wait_entered(&gate, 1, 5000));
    cf_job_stats stats;
    CF_REQUIRE(cf_jobs_stats_get(f.jobs, CF_JOB_REMOVE_BANNED_CONTENT,
                                 &stats) == CF_OK);
    CF_CHECK(stats.accepted == 1);
    CF_CHECK(stats.running == 1);

    /* Release the gate (its job completes without touching rows), hand the
     * kind back to the default handler, and run the second batch directly:
     * the final remainder converges through the queue worker. */
    job_gate_release(&gate);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs, CF_JOB_REMOVE_BANNED_CONTENT,
                                cf_job_stat_completed, 1, 5000));
    CF_REQUIRE(cf_jobs_set_handler(
                   f.jobs, CF_JOB_REMOVE_BANNED_CONTENT,
                   cf_jobs_handle_remove_banned_content, &f.hctx) == CF_OK);
    CF_REQUIRE(cf_jobs_handle_remove_banned_content(&f.hctx, &job) == CF_OK);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs, CF_JOB_REMOVE_BANNED_CONTENT,
                                cf_job_stat_completed, 2, 5000));
    CF_CHECK(table_count(&f, "messages", "creator_id = 9") == 0);
    CF_CHECK(ctx_load(&f.hctx.banned_batches) == 3);
    CF_CHECK(ctx_load(&f.hctx.banned_destroyed) == 205);
    CF_CHECK(ctx_load(&f.hctx.banned_requeued) == 2);
    CF_CHECK(ctx_load(&f.hctx.banned_requeue_dropped) == 0);
    CF_REQUIRE(cf_jobs_stats_get(f.jobs, CF_JOB_REMOVE_BANNED_CONTENT,
                                 &stats) == CF_OK);
    CF_CHECK(stats.accepted == 2);
    CF_CHECK(stats.completed == 2);
    CF_CHECK(stats.failed == 0);

    job_gate_dispose(&gate);
    fixture_close(&f);
}

CF_TEST(handlers_remove_banned_unbanned_user_is_noop) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f)); /* user 1 is active, not banned */
    for (int64_t i = 1; i <= 3; i++) {
        CF_REQUIRE(seed_message(&f, i, 1, 1, i));
    }

    cf_job job = banned_job(1);
    CF_CHECK(cf_jobs_handle_remove_banned_content(&f.hctx, &job) == CF_OK);
    CF_CHECK(ctx_load(&f.hctx.banned_noop_gone) == 1);
    CF_CHECK(ctx_load(&f.hctx.banned_batches) == 0);
    CF_CHECK(table_count(&f, "messages", NULL) == 3); /* kept */

    cf_job missing = banned_job(404);
    CF_CHECK(cf_jobs_handle_remove_banned_content(&f.hctx, &missing) ==
             CF_OK);
    CF_CHECK(ctx_load(&f.hctx.banned_noop_gone) == 2);
    fixture_close(&f);
}

/* --- PurgeBlob --------------------------------------------------------------- */

CF_TEST(handlers_purge_blob_missing_is_noop) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));

    cf_job job = purge_job(4242); /* no such blob */
    CF_CHECK(cf_jobs_handle_purge_blob(&f.hctx, &job) == CF_OK);
    CF_CHECK(ctx_load(&f.hctx.purge_noop_gone) == 1);
    fixture_close(&f);
}

static void purge_storage(handlers_fixture *f, char *root) {
    CF_REQUIRE(mkdtemp(root) != NULL);
    cf_config *config = (cf_config *)cf_app_config(f->app);
    free(config->storage_path);
    config->storage_path = strdup(root);
    CF_REQUIRE(config->storage_path != NULL);
}
static void purge_file(const char *root, const char *key) {
    cf_storage *storage = NULL;
    cf_storage_upload *upload = NULL;
    CF_REQUIRE(cf_storage_open(root, &storage) == CF_OK);
    CF_REQUIRE(cf_storage_upload_begin(storage, &upload) == CF_OK);
    CF_REQUIRE(cf_storage_upload_write(
                   upload, (cf_span){(unsigned char *)"bytes", 5}) == CF_OK);
    CF_REQUIRE(cf_storage_upload_move(upload, (cf_span){(unsigned char *)key,
                                                        strlen(key)}) == CF_OK);
    CF_REQUIRE(cf_storage_upload_commit(upload) == CF_OK);
    cf_storage_upload_dispose(upload);
    cf_storage_close(storage);
}
static bool purge_file_exists(const char *root, const char *key) {
    cf_storage *storage = NULL;
    int fd = -1;
    uint64_t size = 0;
    CF_REQUIRE(cf_storage_open(root, &storage) == CF_OK);
    cf_err rc = cf_storage_open_read(
        storage, (cf_span){(unsigned char *)key, strlen(key)}, &fd, &size);
    if (fd >= 0)
        close(fd);
    cf_storage_close(storage);
    return rc == CF_OK;
}
static void purge_seed(handlers_fixture *f, int64_t id, const char *key) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO active_storage_blobs "
             "(id,byte_size,created_at,filename,key,service_name,content_type) "
             "VALUES (%lld,5,'2026-01-01 "
             "00:00:00','file.png','%s','local','image/png')",
             (long long)id, key);
    CF_REQUIRE(seed_sql(f, sql));
}
CF_TEST(handlers_purge_blob_deletes_unreferenced_metadata_and_file) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    char root[] = "/tmp/cf-purge-XXXXXX";
    purge_storage(&f, root);
    const char *key = "abcdefghijklmnopqrstuvwxyz01";
    purge_seed(&f, 7, key);
    purge_file(root, key);
    cf_job job = purge_job(7);
    CF_CHECK(cf_jobs_handle_purge_blob(&f.hctx, &job) == CF_OK);
    CF_CHECK(table_count(&f, "active_storage_blobs", NULL) == 0);
    CF_CHECK(!purge_file_exists(root, key));
    CF_CHECK(ctx_load(&f.hctx.purge_deleted) == 1);
    fixture_close(&f);
    avatar_test_remove_tree(root);
}
CF_TEST(handlers_purge_preserves_referenced_blob_and_file) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    char root[] = "/tmp/cf-purge-ref-XXXXXX";
    purge_storage(&f, root);
    const char *key = "abcdefghijklmnopqrstuvwxyz01";
    purge_seed(&f, 7, key);
    purge_file(root, key);
    CF_REQUIRE(seed_sql(
        &f, "INSERT INTO "
            "active_storage_attachments(blob_id,created_at,name,record_id,"
            "record_type) VALUES(7,'2026-01-01 00:00:00','avatar',1,'User')"));
    cf_job job = purge_job(7);
    CF_CHECK(cf_jobs_handle_purge_blob(&f.hctx, &job) == CF_OK);
    CF_CHECK(table_count(&f, "active_storage_blobs", NULL) == 1);
    CF_CHECK(purge_file_exists(root, key));
    CF_CHECK(ctx_load(&f.hctx.purge_noop_gone) == 1);
    fixture_close(&f);
    avatar_test_remove_tree(root);
}
CF_TEST(handlers_purge_removes_variants_and_preserves_shared_children) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    char root[] = "/tmp/cf-purge-variants-XXXXXX";
    purge_storage(&f, root);
    const char *keys[] = {"abcdefghijklmnopqrstuvwxyz01",
                          "bcdefghijklmnopqrstuvwxyz012",
                          "cdefghijklmnopqrstuvwxyz0123"};
    for (int i = 0; i < 3; i++) {
        purge_seed(&f, 7 + i, keys[i]);
        purge_file(root, keys[i]);
    }
    CF_REQUIRE(seed_sql(
        &f,
        "INSERT INTO "
        "active_storage_variant_records(id,blob_id,variation_digest) "
        "VALUES(11,7,'digest');INSERT INTO "
        "active_storage_attachments(blob_id,created_at,name,record_id,record_"
        "type) VALUES(8,'2026-01-01 "
        "00:00:00','image',11,'ActiveStorage::VariantRecord'),(9,'2026-01-01 "
        "00:00:00','preview_image',7,'ActiveStorage::Blob'),(9,'2026-01-01 "
        "00:00:00','avatar',1,'User')"));
    cf_job job = purge_job(7);
    CF_CHECK(cf_jobs_handle_purge_blob(&f.hctx, &job) == CF_OK);
    CF_CHECK(table_count(&f, "active_storage_blobs", NULL) == 1);
    CF_CHECK(table_count(&f, "active_storage_variant_records", NULL) == 0);
    CF_CHECK(table_count(&f, "active_storage_attachments", NULL) == 1);
    CF_CHECK(!purge_file_exists(root, keys[0]));
    CF_CHECK(!purge_file_exists(root, keys[1]));
    CF_CHECK(purge_file_exists(root, keys[2]));
    fixture_close(&f);
    avatar_test_remove_tree(root);
}

CF_TEST(handlers_purge_rechecks_shared_descendants_after_later_parent_delete) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    char root[] = "/tmp/cf-purge-shared-XXXXXX";
    purge_storage(&f, root);
    const char *keys[] = {"abcdefghijklmnopqrstuvwxyz01",
                          "bcdefghijklmnopqrstuvwxyz012",
                          "cdefghijklmnopqrstuvwxyz0123",
                          "defghijklmnopqrstuvwxyz01234",
                          "efghijklmnopqrstuvwxyz012345"};
    for (int i = 0; i < 5; i++) {
        purge_seed(&f, 7+i, keys[i]);
        purge_file(root, keys[i]);
    }
    /* Queue order is root,A,X,C,B. C is still referenced by B on its first
     * visit and must be reconsidered when B's ownership edge is deleted. */
    CF_REQUIRE(seed_sql(&f,
        "INSERT INTO active_storage_attachments(blob_id,created_at,name,record_id,record_type) VALUES"
        "(8,'2026-01-01 00:00:00','first',7,'ActiveStorage::Blob'),"
        "(9,'2026-01-01 00:00:00','second',7,'ActiveStorage::Blob'),"
        "(11,'2026-01-01 00:00:00','image',8,'ActiveStorage::Blob'),"
        "(10,'2026-01-01 00:00:00','image',9,'ActiveStorage::Blob'),"
        "(11,'2026-01-01 00:00:00','image',10,'ActiveStorage::Blob')"));
    cf_job job = purge_job(7);
    CF_CHECK(cf_jobs_handle_purge_blob(&f.hctx, &job) == CF_OK);
    CF_CHECK(table_count(&f, "active_storage_blobs", NULL) == 0);
    CF_CHECK(table_count(&f, "active_storage_attachments", NULL) == 0);
    for (size_t i = 0; i < 5; i++) CF_CHECK(!purge_file_exists(root, keys[i]));
    fixture_close(&f);
    avatar_test_remove_tree(root);
}

typedef struct {
    cf_jobs_handler_ctx *handler;
    cf_job job;
    cf_err result;
} purge_thread_arg;
static void *purge_thread(void *opaque) {
    purge_thread_arg *arg = opaque;
    arg->result = cf_jobs_handle_purge_blob(arg->handler, &arg->job);
    return NULL;
}
CF_TEST(handlers_purge_rechecks_a_reference_committed_before_the_write) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    char root[] = "/tmp/cf-purge-race-XXXXXX";
    purge_storage(&f, root);
    const char *key = "abcdefghijklmnopqrstuvwxyz01";
    purge_seed(&f, 7, key);
    purge_file(root, key);
    sqlite3 *lock = NULL;
    CF_REQUIRE(sqlite3_open(f.scratch.path, &lock) == SQLITE_OK);
    CF_REQUIRE(
        sqlite3_exec(
            lock,
            "BEGIN IMMEDIATE;INSERT INTO "
            "active_storage_attachments(blob_id,created_at,name,record_id,"
            "record_type) VALUES(7,'2026-01-01 00:00:00','avatar',1,'User')",
            NULL, NULL, NULL) == SQLITE_OK);
    cf_writer_stats before = {0}, now = {0};
    CF_REQUIRE(cf_writer_stats_get(f.app, &before) == CF_OK);
    purge_thread_arg arg = {.handler = &f.hctx, .job = purge_job(7)};
    pthread_t thread;
    CF_REQUIRE(pthread_create(&thread, NULL, purge_thread, &arg) == 0);
    bool admitted = false;
    for (size_t i = 0; i < 10000; i++) {
        if (cf_writer_stats_get(f.app, &now) == CF_OK &&
            now.admitted > before.admitted) {
            admitted = true;
            break;
        }
        struct timespec pause = {0, 500000};
        nanosleep(&pause, NULL);
    }
    CF_REQUIRE(sqlite3_exec(lock, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    CF_REQUIRE(pthread_join(thread, NULL) == 0);
    sqlite3_close(lock);
    CF_CHECK(admitted);
    CF_CHECK(arg.result == CF_OK);
    CF_CHECK(table_count(&f, "active_storage_blobs", NULL) == 1);
    CF_CHECK(table_count(&f, "active_storage_attachments", NULL) == 1);
    CF_CHECK(purge_file_exists(root, key));
    CF_CHECK(ctx_load(&f.hctx.purge_deleted) == 0);
    fixture_close(&f);
    avatar_test_remove_tree(root);
}
CF_TEST(handlers_purge_filesystem_failure_is_reported_after_metadata_commit) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    char root[] = "/tmp/cf-purge-error-XXXXXX";
    purge_storage(&f, root);
    const char *key = "abcdefghijklmnopqrstuvwxyz01";
    purge_seed(&f, 7, key);
    purge_file(root, key);
    char path[4096];
    snprintf(path, sizeof path, "%s/ab/cd/%s", root, key);
    CF_REQUIRE(unlink(path) == 0);
    CF_REQUIRE(mkdir(path, 0700) == 0);
    cf_job job = purge_job(7);
    CF_CHECK(cf_jobs_handle_purge_blob(&f.hctx, &job) == CF_IO);
    CF_CHECK(table_count(&f, "active_storage_blobs", NULL) == 0);
    CF_CHECK(ctx_load(&f.hctx.purge_deleted) == 0);
    struct stat st;
    CF_CHECK(lstat(path, &st) == 0 && S_ISDIR(st.st_mode));
    sqlite3 *writer = NULL;
    CF_REQUIRE(sqlite3_open(f.scratch.path, &writer) == SQLITE_OK);
    CF_REQUIRE(sqlite3_exec(writer, "PRAGMA foreign_keys=ON", NULL, NULL,
                            NULL) == SQLITE_OK);
    CF_CHECK(
        sqlite3_exec(
            writer,
            "INSERT INTO "
            "active_storage_attachments(blob_id,created_at,name,record_id,"
            "record_type) VALUES(7,'2026-01-01 00:00:00','avatar',1,'User')",
            NULL, NULL, NULL) == SQLITE_CONSTRAINT);
    sqlite3_close(writer);
    CF_CHECK(cf_jobs_handle_purge_blob(&f.hctx, &job) == CF_OK);
    CF_CHECK(ctx_load(&f.hctx.purge_deleted) == 0);
    CF_CHECK(ctx_load(&f.hctx.purge_noop_gone) == 1);
    fixture_close(&f);
    avatar_test_remove_tree(root);
}

/* --- Media ------------------------------------------------------------------ */

CF_TEST(handlers_media_analyze_commits_metadata_and_rejects_unknown_tasks) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_sql(&f,
        "INSERT INTO active_storage_blobs (id,byte_size,created_at,filename,key,"
        "service_name,content_type,metadata) VALUES (7,1,'2026-01-01 00:00:00',"
        "'file.txt','abcdefghijklmnopqrstuvwxyz01','local','text/plain',"
        "'{\"identified\":true}');"));
    cf_job job = {.kind = CF_JOB_MEDIA, .blob_id = 7, .task_type = "analyze"};
    CF_CHECK(cf_app_enqueue_media(f.app,7,cf_span_lit("analyze"),cf_span_lit("")) == CF_OK);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs,CF_JOB_MEDIA,cf_job_stat_completed,1,5000));
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(f.scratch.db),
        "SELECT metadata FROM active_storage_blobs WHERE id=7", -1, &stmt, NULL) == SQLITE_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CF_CHECK(strcmp((const char *)sqlite3_column_text(stmt, 0),
                    "{\"identified\":true,\"analyzed\":true}") == 0);
    sqlite3_finalize(stmt);
    job.task_type = "unknown";
    CF_CHECK(cf_jobs_handle_media(&f.hctx, &job) == CF_INVALID);
    job.task_type = "analyze";
    job.blob_id = 99;
    CF_CHECK(cf_jobs_handle_media(&f.hctx, &job) == CF_OK); /* deleted target */
    CF_CHECK(cf_jobs_handle_media(NULL, &job) == CF_INVALID);
    fixture_close(&f);
}

CF_TEST(handlers_media_analysis_failure_is_failed_and_preserves_metadata) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_sql(&f,
        "INSERT INTO active_storage_blobs (id,byte_size,created_at,filename,key,"
        "service_name,content_type,metadata) VALUES (7,1,'2026-01-01 00:00:00',"
        "'file.txt','abcdefghijklmnopqrstuvwxyz01','local','text/plain','invalid-json');"));
    CF_REQUIRE(cf_app_enqueue_media(f.app,7,cf_span_lit("analyze"),cf_span_lit("")) == CF_OK);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs,CF_JOB_MEDIA,cf_job_stat_failed,1,5000));
    cf_job_stats stats;
    CF_REQUIRE(cf_jobs_stats_get(f.jobs,CF_JOB_MEDIA,&stats) == CF_OK);
    CF_CHECK(stats.failed == 1 && stats.completed == 0);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(f.scratch.db),
        "SELECT metadata FROM active_storage_blobs WHERE id=7",-1,&stmt,NULL)==SQLITE_OK);
    CF_REQUIRE(sqlite3_step(stmt)==SQLITE_ROW);
    CF_CHECK(strcmp((const char *)sqlite3_column_text(stmt,0),"invalid-json")==0);
    sqlite3_finalize(stmt);
    fixture_close(&f);
}

/* --- queue-level accounting --------------------------------------------------- */

CF_TEST(handlers_queue_level_completed_and_failed_accounting) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f));

    /* A committed push for a deleted message completes as a recorded
     * no-op through the queue worker (JOB-03: completed, not failed). */
    cf_event gone;
    memset(&gone, 0, sizeof gone);
    gone.kind = CF_EVENT_PUSH_MESSAGE;
    gone.room_id = 1;
    gone.message_id = 4242;
    CF_REQUIRE(cf_job_enqueue(f.jobs, gone) == CF_OK);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs, CF_JOB_PUSH_MESSAGE,
                                cf_job_stat_completed, 1, 5000));
    CF_CHECK(ctx_load(&f.hctx.push_noop_gone) == 1);

    /* An invalid payload fails the handler: counted once, never retried. */
    cf_event invalid = gone;
    invalid.message_id = 0;
    CF_REQUIRE(cf_job_enqueue(f.jobs, invalid) == CF_OK);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs, CF_JOB_PUSH_MESSAGE,
                                cf_job_stat_failed, 1, 5000));
    cf_job_stats stats;
    CF_REQUIRE(cf_jobs_stats_get(f.jobs, CF_JOB_PUSH_MESSAGE, &stats) ==
               CF_OK);
    CF_CHECK(stats.accepted == 2);
    CF_CHECK(stats.completed == 1);
    CF_CHECK(stats.failed == 1);
    CF_CHECK(stats.pending == 0);
    fixture_close(&f);
}

CF_TEST(handlers_queue_full_drop_never_rewrites_commit) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f));

    /* Occupy both webhook workers with the default handler replaced by a
     * blocking gate, then fill the 128-slot FIFO exactly. */
    job_gate gate;
    job_gate_init(&gate);
    job_gate_block(&gate, CF_OK);
    CF_REQUIRE(cf_jobs_set_handler(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                   job_gate_handler, &gate) == CF_OK);
    cf_event hook;
    memset(&hook, 0, sizeof hook);
    hook.kind = CF_EVENT_DELIVER_WEBHOOK;
    hook.user_id = 1;
    hook.message_id = 1;
    CF_REQUIRE(cf_job_enqueue(f.jobs, hook) == CF_OK);
    CF_REQUIRE(cf_job_enqueue(f.jobs, hook) == CF_OK);
    CF_REQUIRE(job_gate_wait_entered(&gate, 2, 5000));
    for (int i = 0; i < 128; i++) {
        CF_REQUIRE(cf_job_enqueue(f.jobs, hook) == CF_OK);
    }
    CF_CHECK(cf_job_enqueue(f.jobs, hook) == CF_BUSY);

    /* The committed write still returns CF_OK: the drop is counted on both
     * sides (queue rejected, writer best_effort_dropped) and the commit
     * outcome is never rewritten. */
    cf_writer_stats writer_before;
    CF_REQUIRE(cf_writer_stats_get(f.app, &writer_before) == CF_OK);
    CF_CHECK(write_event(&f, hook) == CF_OK);
    cf_writer_stats writer_after;
    CF_REQUIRE(cf_writer_stats_get(f.app, &writer_after) == CF_OK);
    CF_CHECK(writer_after.committed - writer_before.committed == 1);
    CF_CHECK(writer_after.best_effort_dropped[CF_EVENT_DELIVER_WEBHOOK] -
                 writer_before.best_effort_dropped[CF_EVENT_DELIVER_WEBHOOK] ==
             1);
    cf_job_stats stats;
    CF_REQUIRE(cf_jobs_stats_get(f.jobs, CF_JOB_DELIVER_WEBHOOK, &stats) ==
               CF_OK);
    CF_CHECK(stats.accepted == 130);
    CF_CHECK(stats.rejected == 2); /* 1 direct + 1 post-commit */

    job_gate_release(&gate);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                cf_job_stat_completed, 130, 5000));
    job_gate_dispose(&gate);
    fixture_close(&f);
}

CF_TEST_MAIN()
