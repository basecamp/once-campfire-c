#ifndef CF_TEST_AUTH_WRITE_RACE_H
#define CF_TEST_AUTH_WRITE_RACE_H
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include "context.h"
#include "db/writer.h"
/* Hold an uncommitted authorization change until the protected write is
 * admitted. Reader checks see the old snapshot; the writer sees the commit. */
typedef struct {
    cf_app *app;
    sqlite3 *lock;
    uint64_t admitted_before;
    _Atomic bool request_done;
    bool commit_ok;
    bool observed_writer_admission;
} auth_write_race;

static int64_t auth_race_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *auth_race_commit(void *arg) {
    auth_write_race *race = arg;
    int64_t deadline = auth_race_ms() + 10000;
    for (;;) {
        cf_writer_stats stats = {0};
        if (cf_writer_stats_get(race->app, &stats) != CF_OK) break;
        if (stats.admitted > race->admitted_before) {
            race->observed_writer_admission = true;
            break;
        }
        if (atomic_load_explicit(&race->request_done,
                                 memory_order_relaxed)) {
            break; /* the request failed before queueing a write */
        }
        if (auth_race_ms() >= deadline) break;
        struct timespec pause = {0, 500000}; /* 0.5 ms */
        nanosleep(&pause, NULL);
    }
    char *message = NULL;
    int rc = sqlite3_exec(race->lock, "COMMIT", NULL, NULL, &message);
    race->commit_ok = rc == SQLITE_OK;
    if (!race->commit_ok) {
        fprintf(stderr, "  race commit failed (%d): %s\n", rc,
                message != NULL ? message : "?");
    }
    sqlite3_free(message);
    return NULL;
}

/* Open the second connection, take the write lock and apply `change_sql`
 * without committing.  The baseline admission count is captured before the
 * request, so the committer recognizes the action's callback. */
static bool auth_race_begin(cf_app *app, const char *path, auth_write_race *race,
                             const char *change_sql) {
    memset(race, 0, sizeof *race);
    atomic_init(&race->request_done, false);
    race->app = app;
    if (sqlite3_open(path, &race->lock) != SQLITE_OK) {
        fprintf(stderr, "  race: second connection failed\n");
        return false;
    }
    sqlite3_busy_timeout(race->lock, 1000);
    char *message = NULL;
    if (sqlite3_exec(race->lock, "BEGIN IMMEDIATE", NULL, NULL, &message) !=
        SQLITE_OK) {
        fprintf(stderr, "  race: BEGIN IMMEDIATE failed: %s\n",
                message != NULL ? message : "?");
        sqlite3_free(message);
        sqlite3_close(race->lock);
        race->lock = NULL;
        return false;
    }
    if (sqlite3_exec(race->lock, change_sql, NULL, NULL, &message) !=
        SQLITE_OK) {
        fprintf(stderr, "  race: change failed: %s\n",
                message != NULL ? message : "?");
        sqlite3_free(message);
        sqlite3_exec(race->lock, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(race->lock);
        race->lock = NULL;
        return false;
    }
    cf_writer_stats stats = {0};
    if (cf_writer_stats_get(app, &stats) != CF_OK) {
        sqlite3_exec(race->lock, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(race->lock);
        race->lock = NULL;
        return false;
    }
    race->admitted_before = stats.admitted;
    return true;
}

static void auth_race_end(auth_write_race *race) {
    if (race->lock != NULL) sqlite3_close(race->lock);
    race->lock = NULL;
}

/* Run `req` while the held change commits as soon as its write is queued;
 * always joins the committer before returning (also on a request failure, so
 * no thread outlives the case). */
static bool auth_race_request(cf_app *app, cf_db *reader, auth_write_race *race,
                               cf_request *req, cf_response *resp) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, auth_race_commit, race) != 0) {
        return false;
    }
    cf_err rc = cf_ctx_process(app, reader, req, resp);
    atomic_store_explicit(&race->request_done, true, memory_order_relaxed);
    bool joined = pthread_join(thread, NULL) == 0;
    if (rc != CF_OK) {
        fprintf(stderr, "  race request failed: rc=%d\n", rc);
        return false;
    }
    return joined && race->commit_ok && race->observed_writer_admission;
}


#endif
