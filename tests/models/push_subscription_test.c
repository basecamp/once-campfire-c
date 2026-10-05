/* tests/models/push_subscription_test.c — D01 model family push_subscription.
 *
 * Covers every symbol of src/models/push_subscription.h: new, find, count,
 * for_user, find_for_user_by_keys, create, destroy, destroy_by_endpoint,
 * badge, validate, resolved_endpoint_ip, payload_for,
 * for_users_involved_in_everything, for_mentioned_users, pushes_for, plus the
 * record/vector/payload disposals.
 *
 * Seed data mirrors the pinned fixtures
 * (tmp/rails-ref/test/fixtures/{users,rooms,memberships,push/subscriptions}.yml):
 * the four push subscriptions with their endpoints/keys/user agents, the
 * designers room memberships (david+kevin default "mentions",
 * jason+jz "everything") and the direct room david_and_jason. IDs are local;
 * only the fixture values are reused. `now` values are fixed microseconds, no
 * sleeps, and the scratch database is fresh per case.
 *
 * LINK FALLBACKS, read before changing:
 * push_subscription.c calls the declared cross-module D01 APIs
 * (cf_membership_*, cf_message_*, cf_room_*, cf_user_*). When this family
 * landed those sibling .c files and D02's writer.c did not exist yet, so this
 * test binary provides weak definitions with the declared contracts, plus a
 * stand-in writer transaction; the real models override them in the full test
 * link. The cf_message_* fallbacks are gone: the full link uses the real
 * message model, so the mentionees of pushes_for come from the message body
 * through the production R02 rich-text pipeline. The mention case therefore
 * stores real rich-text content: a verified Kevin mention attachment whose
 * signed SGID is built through A01's cf_auth_sgid_generate_attachable and
 * resolved by the pipeline configured with the test SECRET_KEY_BASE, exactly
 * as tests/richtext/test_richtext.c does. push_test.rs's Mentions test
 * support faked the mentionee list because the reference's BasicRichText
 * reports none; here the body carries the mention instead.
 */
#include "cf_test.h"

#include "auth.h"
#include "db/db_testutil.h"
#include "models/membership.h"
#include "models/message.h"
#include "models/push_subscription.h"
#include "models/room.h"
#include "models/user.h"
#include "richtext.h"

#include <stdlib.h>
#include <string.h>

/* ================= weak fallbacks for not-yet-landed siblings ============= */

/* D02 writer.c stand-in: cf_tx is opaque in the frozen contract and the
 * tests are its only consumer here. */
struct cf_tx {
    cf_db *db;
};

__attribute__((weak)) cf_db *cf_tx_db(cf_tx *tx) { return tx->db; }

__attribute__((weak)) int64_t cf_membership_connection_cutoff(int64_t now_us) {
    return now_us - CF_MEMBERSHIP_CONNECTION_TTL_US;
}

__attribute__((weak)) cf_err cf_membership_unread_count(cf_db *db,
                                                        int64_t user_id,
                                                        int64_t *out) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        cf_db_handle(db),
        "SELECT COUNT(*) FROM \"memberships\" WHERE \"memberships\".\"user_id\" "
        "= ?1 AND \"memberships\".\"unread_at\" IS NOT NULL",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return cf_db_err(rc);
    sqlite3_bind_int64(stmt, 1, user_id);
    *out = 0;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int64(stmt, 0);
    } else if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return cf_db_err(rc);
    }
    sqlite3_finalize(stmt);
    return CF_OK;
}

__attribute__((weak)) cf_err cf_room_find(cf_db *db, int64_t id,
                                          cf_room *out) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        cf_db_handle(db),
        "SELECT \"id\", \"name\", \"type\", \"creator_id\", \"created_at\", "
        "\"updated_at\" FROM \"rooms\" WHERE \"id\" = ?1 LIMIT 1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return cf_db_err(rc);
    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return CF_NOT_FOUND;
    }
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return cf_db_err(rc);
    }
    memset(out, 0, sizeof *out);
    out->id = sqlite3_column_int64(stmt, 0);
    if (sqlite3_column_type(stmt, 1) != SQLITE_NULL) {
        const unsigned char *text = sqlite3_column_text(stmt, 1);
        int bytes = sqlite3_column_bytes(stmt, 1);
        char *copy = malloc((size_t)bytes + 1);
        if (copy == NULL) {
            sqlite3_finalize(stmt);
            return CF_NOMEM;
        }
        if (bytes > 0) memcpy(copy, text, (size_t)bytes);
        copy[bytes] = '\0';
        out->name.value.ptr = copy;
        out->name.value.len = (size_t)bytes;
        out->name.present = true;
    }
    const unsigned char *type = sqlite3_column_text(stmt, 2);
    if (type != NULL && strcmp((const char *)type, "Rooms::Open") == 0) {
        out->room_type = CF_ROOM_OPEN;
    } else if (type != NULL &&
               strcmp((const char *)type, "Rooms::Closed") == 0) {
        out->room_type = CF_ROOM_CLOSED;
    } else {
        out->room_type = CF_ROOM_DIRECT;
    }
    out->creator_id = sqlite3_column_int64(stmt, 3);
    {
        cf_span text;
        text.ptr = sqlite3_column_text(stmt, 4);
        text.len = text.ptr != NULL ? (size_t)sqlite3_column_bytes(stmt, 4) : 0;
        if (text.ptr != NULL) cf_db_time_from_text(text, &out->created_at);
        text.ptr = sqlite3_column_text(stmt, 5);
        text.len = text.ptr != NULL ? (size_t)sqlite3_column_bytes(stmt, 5) : 0;
        if (text.ptr != NULL) cf_db_time_from_text(text, &out->updated_at);
    }
    sqlite3_finalize(stmt);
    return CF_OK;
}

__attribute__((weak)) void cf_room_dispose(cf_room *room) {
    if (room == NULL) return;
    cf_optional_str_dispose(&room->name);
    memset(room, 0, sizeof *room);
}

__attribute__((weak)) bool cf_room_direct(const cf_room *room) {
    return room->room_type == CF_ROOM_DIRECT;
}

__attribute__((weak)) void cf_user_dispose(cf_user *user) {
    if (user == NULL) return;
    cf_str_dispose(&user->name);
    cf_optional_str_dispose(&user->email_address);
    cf_optional_str_dispose(&user->password_digest);
    cf_optional_str_dispose(&user->bio);
    cf_optional_str_dispose(&user->bot_token);
    memset(user, 0, sizeof *user);
}

__attribute__((weak)) void cf_user_vector_dispose(cf_user_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_user_dispose(&vector->items[i]);
    }
    free(vector->items);
    memset(vector, 0, sizeof *vector);
}

/* ===================== test helpers ====================================== */

static bool ps_eq_str(cf_str text, const char *expected) {
    size_t len = strlen(expected);
    return text.len == len &&
           (len == 0 || memcmp(text.ptr, expected, len) == 0);
}

static bool ps_eq_opt(cf_optional_str value, const char *expected) {
    return value.present && ps_eq_str(value.value, expected);
}

static cf_optional_str ps_opt(const char *text) {
    cf_optional_str value;
    value.present = true;
    value.value.ptr = (char *)text;
    value.value.len = strlen(text);
    return value;
}

static cf_optional_str ps_absent(void) {
    cf_optional_str value;
    value.present = false;
    value.value.ptr = NULL;
    value.value.len = 0;
    return value;
}

static uint64_t ps_test_time_text(int64_t us, char out[CF_DB_TIME_TEXT_CAP]) {
    return cf_db_time_to_text(us, out) == CF_OK ? strlen(out) : 0;
}

/* JSON length of a string's contents, the same escape accounting as the
 * model's truncation (only the bytes the tests build: ASCII + raw UTF-8). */
static size_t ps_test_json_len(cf_str text) {
    size_t total = 0;
    for (size_t i = 0; i < text.len; i++) {
        unsigned char b = (unsigned char)text.ptr[i];
        if (b == '"' || b == '\\' || b == '\n' || b == '\r' || b == '\t' ||
            b == 8 || b == 12) {
            total += 2;
        } else if (b < 0x20) {
            total += 6;
        } else {
            total += 1;
        }
    }
    return total;
}

static void ps_collect_ids(cf_push_subscription_vector *vector,
                           int64_t *ids) {
    for (size_t i = 0; i < vector->len; i++) ids[i] = vector->items[i].id;
}

static void ps_sort_i64(int64_t *ids, size_t len) {
    for (size_t i = 1; i < len; i++) {
        int64_t value = ids[i];
        size_t j = i;
        while (j > 0 && ids[j - 1] > value) {
            ids[j] = ids[j - 1];
            j--;
        }
        ids[j] = value;
    }
}

/* Fixture rows: users, rooms, memberships and push_subscriptions matching
 * tmp/rails-ref/test/fixtures (values only; local ids). */
static bool ps_seed_fixture(cf_db_scratch *scratch) {
    sqlite3 *handle = cf_db_handle(scratch->db);
    return cf_db_test_exec(
               handle,
               "INSERT INTO \"users\" (\"id\", \"name\", \"created_at\", "
               "\"updated_at\") VALUES"
               " (1, 'David', '2026-01-01 00:00:00', '2026-01-01 00:00:00'),"
               " (2, 'Jason', '2026-01-01 00:00:00', '2026-01-01 00:00:00'),"
               " (3, 'JZ', '2026-01-01 00:00:00', '2026-01-01 00:00:00'),"
               " (4, 'Kevin', '2026-01-01 00:00:00', '2026-01-01 00:00:00'),"
               " (5, 'Bender Bot', '2026-01-01 00:00:00', '2026-01-01 00:00:00');"
               "INSERT INTO \"rooms\" (\"id\", \"name\", \"type\", "
               "\"creator_id\", \"created_at\", \"updated_at\") VALUES"
               " (100, 'Designers', 'Rooms::Closed', 1, "
               "'2026-01-01 00:00:00', '2026-01-01 00:00:00'),"
               " (101, 'All Pets', 'Rooms::Open', 1, '2026-01-01 00:00:00', "
               "'2026-01-01 00:00:00'),"
               " (102, NULL, 'Rooms::Direct', 1, '2026-01-01 00:00:00', "
               "'2026-01-01 00:00:00'),"
               " (103, 'HQ', 'Rooms::Open', 1, '2026-01-01 00:00:00', "
               "'2026-01-01 00:00:00');"
               "INSERT INTO \"memberships\" (\"id\", \"room_id\", \"user_id\", "
               "\"involvement\", \"connected_at\", \"unread_at\", "
               "\"connections\", \"created_at\", \"updated_at\") VALUES"
               " (200, 100, 1, NULL, NULL, NULL, 0, '2026-01-01 00:00:00', "
               "'2026-01-01 00:00:00'),"
               " (201, 100, 2, 'everything', NULL, NULL, 0, "
               "'2026-01-01 00:00:00', '2026-01-01 00:00:00'),"
               " (202, 100, 3, 'everything', NULL, NULL, 0, "
               "'2026-01-01 00:00:00', '2026-01-01 00:00:00'),"
               " (203, 100, 4, 'mentions', NULL, NULL, 0, "
               "'2026-01-01 00:00:00', '2026-01-01 00:00:00'),"
               " (204, 101, 1, 'everything', NULL, "
               "'2026-01-02 03:04:05.123456', 0, '2026-01-01 00:00:00', "
               "'2026-01-01 00:00:00'),"
               " (205, 103, 2, 'everything', NULL, NULL, 0, "
               "'2026-01-01 00:00:00', '2026-01-01 00:00:00'),"
               " (206, 102, 1, 'everything', NULL, NULL, 0, "
               "'2026-01-01 00:00:00', '2026-01-01 00:00:00'),"
               " (207, 102, 2, 'everything', NULL, NULL, 0, "
               "'2026-01-01 00:00:00', '2026-01-01 00:00:00'),"
               " (208, 100, 5, 'mentions', NULL, NULL, 0, "
               "'2026-01-01 00:00:00', '2026-01-01 00:00:00');"
               "INSERT INTO \"push_subscriptions\" (\"id\", \"user_id\", "
               "\"endpoint\", \"p256dh_key\", \"auth_key\", \"user_agent\", "
               "\"created_at\", \"updated_at\") VALUES"
               " (300, 1, 'https://fcm.googleapis.com/fcm/send/123', "
               "'123-RIX', "
               "'xxx2DtgvmLkevKRwoyahJl0efg', 'Mozilla/5.0 (Macintosh)', "
               "'2026-01-02 03:04:05', '2026-01-02 03:04:05'),"
               " (301, 2, 'https://fcm.googleapis.com/fcm/send/567', "
               "'456-RIXcMgkdjhRnFZaYjjGvo00dydRQbCpQTuXFjLaCPSE7ofxi19awgGc3Doqa1RmYQqsbQDfQTifFZgc', "
               "'xxx2DtgvmLkevKRwoyahJl0efg', NULL, "
               "'2026-01-02 03:04:05.123456', '2026-01-02 03:04:05.123456'),"
               " (302, 3, 'https://fcm.googleapis.com/fcm/send/456', NULL, "
               "NULL, NULL, '2026-01-02 03:04:05', '2026-01-02 03:04:05'),"
               " (303, 4, 'https://fcm.googleapis.com/fcm/send/789', "
               "'456-RIX', 'xxx2DtgvmLkevKRwoyahJl0efg', 'Mozilla/5.0', "
               "'2026-01-02 03:04:05', '2026-01-02 03:04:05');") == SQLITE_OK;
}

static bool ps_scratch_with_fixture(cf_db_scratch *scratch) {
    if (!cf_db_scratch_open(scratch)) return false;
    return ps_seed_fixture(scratch);
}

static bool ps_insert(cf_db_scratch *scratch, const char *sql) {
    return cf_db_test_exec(cf_db_handle(scratch->db), sql) == SQLITE_OK;
}

/* messages row plus its stored body (plain text for the stand-in above). */
static bool ps_insert_message(cf_db_scratch *scratch, int64_t id,
                              int64_t room_id, int64_t creator_id,
                              const char *body) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO \"messages\" (\"id\", \"room_id\", \"creator_id\", "
             "\"client_message_id\", \"created_at\", \"updated_at\") VALUES "
             "(%lld, %lld, %lld, 'earth', '2026-01-01 00:00:00', "
             "'2026-01-01 00:00:00');",
             (long long)id, (long long)room_id, (long long)creator_id);
    if (!ps_insert(scratch, sql)) return false;
    if (body == NULL) return true;
    size_t cap = strlen(body) + 512;
    char *body_sql = malloc(cap);
    if (body_sql == NULL) return false;
    snprintf(body_sql, cap,
             "INSERT INTO \"action_text_rich_texts\" (\"body\", "
             "\"record_type\", \"record_id\", \"name\", \"created_at\", "
             "\"updated_at\") VALUES ('%s', 'Message', %lld, 'body', "
             "'2026-01-01 00:00:00', '2026-01-01 00:00:00');",
             body, (long long)id);
    bool ok = ps_insert(scratch, body_sql);
    free(body_sql);
    return ok;
}

static void ps_message_set(cf_message *message, int64_t id, int64_t room_id,
                           int64_t creator_id) {
    memset(message, 0, sizeof *message);
    message->id = id;
    message->room_id = room_id;
    message->creator_id = creator_id;
}

/* ============ production rich-text mention fixtures (R02 + A01) =========== */

/* The SECRET_KEY_BASE the pipeline singleton verifies SGIDs with; the same
 * span configures the singleton and signs the fixture, mirroring the app's
 * startup cf_richtext_configure. */
#define PS_TEST_SECRET \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static cf_span ps_span(const char *text) {
    cf_span span = {(const unsigned char *)text, strlen(text)};
    return span;
}

/* Body with the text "Hi" plus a verified mention attachment for Kevin; the
 * production PlainTextConversion renders it as "Hi @Kevin" and reports user 4
 * as a mentionee (mirrors push_test.rs's `notifies_subscribed_users` body
 * "Hey @Kevin" + [kevin], which the reference test support had to state
 * separately). Caller frees. */
static char *ps_mention_body(const char *sgid) {
    size_t need = strlen(sgid) + 160;
    char *body = malloc(need);
    if (body == NULL) return NULL;
    snprintf(body, need,
             "<p>Hi <action-text-attachment sgid=\"%s\" "
             "content-type=\"application/vnd.campfire.mention\">"
             "</action-text-attachment></p>",
             sgid);
    return body;
}

static int ps_resolver_calls;
static char ps_resolver_host[256];

static cf_optional_str ps_resolve_public(void *arg, cf_str host) {
    (void)arg;
    ps_resolver_calls++;
    size_t n = host.len < sizeof ps_resolver_host - 1
                   ? host.len
                   : sizeof ps_resolver_host - 1;
    if (n != 0) memcpy(ps_resolver_host, host.ptr, n);
    ps_resolver_host[n] = '\0';
    cf_optional_str out = ps_absent();
    const char *ip = "142.250.185.206";
    size_t len = strlen(ip);
    char *copy = malloc(len + 1);
    if (copy == NULL) return out;
    memcpy(copy, ip, len + 1);
    out.present = true;
    out.value.ptr = copy;
    out.value.len = len;
    return out;
}

static cf_optional_str ps_resolve_private(void *arg, cf_str host) {
    (void)arg;
    (void)host;
    ps_resolver_calls++;
    return ps_absent();
}

static cf_push_subscription ps_make(int64_t user_id, const char *endpoint,
                                    const char *p256dh, const char *auth,
                                    const char *agent) {
    cf_push_subscription subscription;
    memset(&subscription, 0, sizeof subscription);
    subscription.user_id = user_id;
    subscription.endpoint = endpoint != NULL ? ps_opt(endpoint) : ps_absent();
    subscription.p256dh_key = p256dh != NULL ? ps_opt(p256dh) : ps_absent();
    subscription.auth_key = auth != NULL ? ps_opt(auth) : ps_absent();
    subscription.user_agent = agent != NULL ? ps_opt(agent) : ps_absent();
    return subscription;
}

/* ===================== tests ============================================= */

CF_TEST(new_copies_fields_and_starts_unsaved) {
    cf_push_subscription subscription;
    memset(&subscription, 0, sizeof subscription);
    cf_optional_str endpoint = ps_opt("https://fcm.googleapis.com/x");
    cf_optional_str none = ps_absent();
    cf_optional_str empty = ps_opt("");
    CF_REQUIRE(cf_push_subscription_new(42, endpoint, none, empty, none,
                                        &subscription) == CF_OK);
    CF_CHECK(subscription.id == 0);
    CF_CHECK(subscription.user_id == 42);
    CF_CHECK(subscription.created_at == 0 && subscription.updated_at == 0);
    CF_CHECK(ps_eq_opt(subscription.endpoint, "https://fcm.googleapis.com/x"));
    CF_CHECK(!subscription.p256dh_key.present);
    CF_CHECK(subscription.auth_key.present && subscription.auth_key.value.len == 0);
    CF_CHECK(!subscription.user_agent.present);
    cf_push_subscription_dispose(&subscription);
    CF_CHECK(subscription.endpoint.value.ptr == NULL);
}

CF_TEST(find_reads_every_column_and_missing_is_not_found) {
    cf_db_scratch scratch;
    CF_REQUIRE(ps_scratch_with_fixture(&scratch));

    cf_push_subscription found;
    memset(&found, 0, sizeof found);
    CF_REQUIRE(cf_push_subscription_find(scratch.db, 301, &found) == CF_OK);
    CF_CHECK(found.id == 301);
    CF_CHECK(found.user_id == 2);
    CF_CHECK(ps_eq_opt(found.endpoint, "https://fcm.googleapis.com/fcm/send/567"));
    CF_CHECK(ps_eq_opt(found.p256dh_key,
                       "456-RIXcMgkdjhRnFZaYjjGvo00dydRQbCpQTuXFjLaCPSE7ofxi19awgGc3Doqa1RmYQqsbQDfQTifFZgc"));
    CF_CHECK(ps_eq_opt(found.auth_key, "xxx2DtgvmLkevKRwoyahJl0efg"));
    CF_CHECK(!found.user_agent.present);
    CF_CHECK(found.created_at == 1767323045123456LL);
    CF_CHECK(found.updated_at == 1767323045123456LL);
    cf_push_subscription_dispose(&found);

    /* A NULL column stays absent, whole-second text parses to microseconds. */
    cf_push_subscription second;
    memset(&second, 0, sizeof second);
    CF_REQUIRE(cf_push_subscription_find(scratch.db, 302, &second) == CF_OK);
    CF_CHECK(!second.p256dh_key.present && !second.auth_key.present);
    CF_CHECK(second.created_at == 1767323045000000LL);
    cf_push_subscription_dispose(&second);

    cf_push_subscription missing;
    memset(&missing, 0, sizeof missing);
    CF_CHECK(cf_push_subscription_find(scratch.db, 999999, &missing) ==
             CF_NOT_FOUND);
    CF_CHECK(missing.id == 0 && missing.endpoint.value.ptr == NULL);
    cf_db_scratch_close(&scratch);
}

CF_TEST(count_and_for_user_scope_rows) {
    cf_db_scratch scratch;
    CF_REQUIRE(ps_scratch_with_fixture(&scratch));

    int64_t count = -1;
    CF_REQUIRE(cf_push_subscription_count(scratch.db, &count) == CF_OK);
    CF_CHECK(count == 4);

    cf_push_subscription_vector davids;
    memset(&davids, 0, sizeof davids);
    CF_REQUIRE(cf_push_subscription_for_user(scratch.db, 1, &davids) == CF_OK);
    CF_CHECK(davids.len == 1 && davids.items[0].id == 300);
    cf_push_subscription_vector_dispose(&davids);

    cf_push_subscription_vector jz;
    memset(&jz, 0, sizeof jz);
    CF_REQUIRE(cf_push_subscription_for_user(scratch.db, 3, &jz) == CF_OK);
    CF_CHECK(jz.len == 1 && jz.items[0].id == 302);
    cf_push_subscription_vector_dispose(&jz);

    cf_push_subscription_vector none;
    memset(&none, 0, sizeof none);
    CF_REQUIRE(cf_push_subscription_for_user(scratch.db, 77, &none) == CF_OK);
    CF_CHECK(none.len == 0);
    cf_push_subscription_vector_dispose(&none);
    cf_db_scratch_close(&scratch);
}

CF_TEST(find_for_user_by_keys_matches_all_three_keys) {
    cf_db_scratch scratch;
    CF_REQUIRE(ps_scratch_with_fixture(&scratch));
    CF_REQUIRE(ps_insert(
        &scratch,
        "INSERT INTO \"push_subscriptions\" (\"id\", \"user_id\", "
        "\"endpoint\", \"p256dh_key\", \"auth_key\", \"created_at\", "
        "\"updated_at\") VALUES (310, 1, NULL, NULL, NULL, "
        "'2026-01-02 03:04:05', '2026-01-02 03:04:05');"));

    bool found = true;
    cf_push_subscription subscription;
    memset(&subscription, 0, sizeof subscription);
    CF_REQUIRE(cf_push_subscription_find_for_user_by_keys(
                   scratch.db, 1,
                   CF_STR_LIT("https://fcm.googleapis.com/fcm/send/123"),
                   CF_STR_LIT("123-RIX"),
                   CF_STR_LIT("xxx2DtgvmLkevKRwoyahJl0efg"), &found,
                   &subscription) == CF_OK);
    CF_CHECK(found && subscription.id == 300);
    cf_push_subscription_dispose(&subscription);

    /* Wrong endpoint, wrong user, wrong keys: each is absent, not an error. */
    const struct {
        int64_t user_id;
        cf_str endpoint;
        cf_str p256dh;
        cf_str auth;
    } misses[] = {
        {1, CF_STR_LIT("https://fcm.googleapis.com/fcm/send/123"),
         CF_STR_LIT("123-RIX"), CF_STR_LIT("wrong")},
        {2, CF_STR_LIT("https://fcm.googleapis.com/fcm/send/123"),
         CF_STR_LIT("123-RIX"), CF_STR_LIT("xxx2DtgvmLkevKRwoyahJl0efg")},
        {1, CF_STR_LIT("https://fcm.googleapis.com/fcm/send/999"),
         CF_STR_LIT("123-RIX"), CF_STR_LIT("xxx2DtgvmLkevKRwoyahJl0efg")},
        {1, CF_STR_LIT(""), CF_STR_LIT(""), CF_STR_LIT("")},
    };
    for (size_t i = 0; i < sizeof misses / sizeof misses[0]; i++) {
        memset(&subscription, 0, sizeof subscription);
        found = true;
        CF_CHECK(cf_push_subscription_find_for_user_by_keys(
                     scratch.db, misses[i].user_id, misses[i].endpoint,
                     misses[i].p256dh, misses[i].auth, &found,
                     &subscription) == CF_OK);
        CF_CHECK(!found);
        CF_CHECK(subscription.id == 0 && subscription.endpoint.value.ptr == NULL);
    }
    cf_db_scratch_close(&scratch);
}

CF_TEST(create_validates_and_inserts_with_reference_datetime_text) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    CF_REQUIRE(ps_insert(&scratch,
                         "INSERT INTO \"users\" (\"id\", \"name\", "
                         "\"created_at\", \"updated_at\") VALUES (1, 'David', "
                         "'2026-01-01 00:00:00', '2026-01-01 00:00:00');"));
    cf_tx tx = {scratch.db};

    cf_optional_str endpoint = ps_opt("https://fcm.googleapis.com/fcm/send/x");
    cf_optional_str p256dh = ps_opt("p256dh-key");
    cf_optional_str auth = ps_absent();
    cf_optional_str agent = ps_opt("Mozilla/5.0");
    cf_push_subscription input;
    memset(&input, 0, sizeof input);
    CF_REQUIRE(cf_push_subscription_new(1, endpoint, p256dh, auth, agent,
                                        &input) == CF_OK);

    ps_resolver_calls = 0;
    int64_t before = cf_now_us(NULL);
    cf_push_subscription created;
    memset(&created, 0, sizeof created);
    CF_REQUIRE(cf_push_subscription_create(&tx, &input, ps_resolve_public, NULL,
                                           &created) == CF_OK);
    int64_t after = cf_now_us(NULL);
    CF_CHECK(created.id > 0);
    CF_CHECK(created.user_id == 1);
    CF_CHECK(created.created_at == created.updated_at);
    CF_CHECK(created.created_at >= before && created.created_at <= after);
    CF_CHECK(ps_eq_opt(created.endpoint,
                       "https://fcm.googleapis.com/fcm/send/x"));
    CF_CHECK(ps_eq_opt(created.p256dh_key, "p256dh-key"));
    CF_CHECK(!created.auth_key.present);
    CF_CHECK(ps_eq_opt(created.user_agent, "Mozilla/5.0"));
    CF_CHECK(ps_resolver_calls == 1);

    /* The stored text is exactly the helper's representation of that value. */
    char expected[CF_DB_TIME_TEXT_CAP];
    char sql[128];
    char stored[CF_DB_TIME_TEXT_CAP];
    CF_REQUIRE(cf_db_time_to_text(created.created_at, expected) == CF_OK);
    snprintf(sql, sizeof sql,
             "SELECT \"created_at\" FROM \"push_subscriptions\" WHERE "
             "\"id\" = %lld;",
             (long long)created.id);
    CF_CHECK(cf_db_test_text(cf_db_handle(scratch.db), sql, stored,
                             sizeof stored) != NULL);
    CF_CHECK(strcmp(stored, expected) == 0);
    snprintf(sql, sizeof sql,
             "SELECT \"auth_key\" IS NULL FROM \"push_subscriptions\" WHERE "
             "\"id\" = %lld;",
             (long long)created.id);
    bool ok = false;
    CF_CHECK(cf_db_test_i64(cf_db_handle(scratch.db), sql, &ok) == 1 && ok);

    int64_t count = -1;
    CF_REQUIRE(cf_push_subscription_count(scratch.db, &count) == CF_OK);
    CF_CHECK(count == 1);

    cf_push_subscription_dispose(&created);
    cf_push_subscription_dispose(&input);
    cf_db_scratch_close(&scratch);
}

CF_TEST(create_rejects_invalid_endpoints_without_inserting) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    CF_REQUIRE(ps_insert(&scratch,
                         "INSERT INTO \"users\" (\"id\", \"name\", "
                         "\"created_at\", \"updated_at\") VALUES (1, 'David', "
                         "'2026-01-01 00:00:00', '2026-01-01 00:00:00');"));
    cf_tx tx = {scratch.db};

    const char *bad[] = {
        "http://fcm.googleapis.com/fcm/send/x",      /* not HTTPS */
        "https://fcm.googleapis.com:8443/x",         /* wrong port */
        "https://attacker.example.com/collect",      /* not permitted */
        "",                                          /* blank/invalid */
        "not a url",                                 /* invalid URL */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        cf_optional_str endpoint = ps_opt(bad[i]);
        cf_push_subscription input;
        memset(&input, 0, sizeof input);
        CF_REQUIRE(cf_push_subscription_new(1, endpoint, ps_absent(),
                                            ps_absent(), ps_absent(),
                                            &input) == CF_OK);
        cf_push_subscription created;
        memset(&created, 0, sizeof created);
        CF_CHECK(cf_push_subscription_create(&tx, &input, ps_resolve_public,
                                             NULL, &created) == CF_INVALID);
        CF_CHECK(created.id == 0 && created.endpoint.value.ptr == NULL);
        cf_push_subscription_dispose(&input);
    }

    /* No resolver means the endpoint cannot be checked: invalid, not OK. */
    cf_optional_str endpoint = ps_opt("https://fcm.googleapis.com/fcm/send/x");
    cf_push_subscription input;
    memset(&input, 0, sizeof input);
    CF_REQUIRE(cf_push_subscription_new(1, endpoint, ps_absent(), ps_absent(),
                                        ps_absent(), &input) == CF_OK);
    cf_push_subscription created;
    memset(&created, 0, sizeof created);
    CF_CHECK(cf_push_subscription_create(&tx, &input, NULL, NULL, &created) ==
             CF_INVALID);
    cf_push_subscription_dispose(&input);

    int64_t count = -1;
    CF_REQUIRE(cf_push_subscription_count(scratch.db, &count) == CF_OK);
    CF_CHECK(count == 0);
    cf_db_scratch_close(&scratch);
}

CF_TEST(destroy_removes_the_row_and_tolerates_a_missing_id) {
    cf_db_scratch scratch;
    CF_REQUIRE(ps_scratch_with_fixture(&scratch));
    cf_tx tx = {scratch.db};

    cf_push_subscription target;
    memset(&target, 0, sizeof target);
    CF_REQUIRE(cf_push_subscription_find(scratch.db, 301, &target) == CF_OK);
    CF_REQUIRE(cf_push_subscription_destroy(&tx, &target) == CF_OK);
    cf_push_subscription_dispose(&target);

    CF_CHECK(cf_push_subscription_find(scratch.db, 301, &target) ==
             CF_NOT_FOUND);
    int64_t count = -1;
    CF_REQUIRE(cf_push_subscription_count(scratch.db, &count) == CF_OK);
    CF_CHECK(count == 3);

    cf_push_subscription missing;
    memset(&missing, 0, sizeof missing);
    missing.id = 999999;
    CF_CHECK(cf_push_subscription_destroy(&tx, &missing) == CF_OK);
    CF_REQUIRE(cf_push_subscription_count(scratch.db, &count) == CF_OK);
    CF_CHECK(count == 3);
    cf_db_scratch_close(&scratch);
}

CF_TEST(destroy_by_endpoint_removes_only_that_users_rows) {
    cf_db_scratch scratch;
    CF_REQUIRE(ps_scratch_with_fixture(&scratch));
    CF_REQUIRE(ps_insert(
        &scratch,
        "INSERT INTO \"push_subscriptions\" (\"id\", \"user_id\", "
        "\"endpoint\", \"p256dh_key\", \"auth_key\", \"created_at\", "
        "\"updated_at\") VALUES (304, 3, "
        "'https://fcm.googleapis.com/fcm/send/456', 'second', 'second', "
        "'2026-01-02 03:04:05', '2026-01-02 03:04:05');"));
    cf_tx tx = {scratch.db};

    CF_REQUIRE(cf_push_subscription_destroy_by_endpoint(
                   &tx, 3,
                   CF_STR_LIT("https://fcm.googleapis.com/fcm/send/456")) ==
               CF_OK);
    cf_push_subscription_vector jz;
    memset(&jz, 0, sizeof jz);
    CF_REQUIRE(cf_push_subscription_for_user(scratch.db, 3, &jz) == CF_OK);
    CF_CHECK(jz.len == 0);
    cf_push_subscription_vector_dispose(&jz);

    cf_push_subscription other;
    memset(&other, 0, sizeof other);
    CF_REQUIRE(cf_push_subscription_find(scratch.db, 303, &other) == CF_OK);
    CF_CHECK(ps_eq_opt(other.endpoint,
                       "https://fcm.googleapis.com/fcm/send/789"));
    cf_push_subscription_dispose(&other);

    int64_t count = -1;
    CF_REQUIRE(cf_push_subscription_count(scratch.db, &count) == CF_OK);
    CF_CHECK(count == 3); /* 300, 301, 303 */

    /* Repeated or unknown deletes are no-ops. */
    CF_CHECK(cf_push_subscription_destroy_by_endpoint(
                  &tx, 3,
                  CF_STR_LIT("https://fcm.googleapis.com/fcm/send/456")) ==
              CF_OK);
    CF_CHECK(cf_push_subscription_destroy_by_endpoint(
                  &tx, 3, CF_STR_LIT("https://example.com/none")) == CF_OK);
    CF_REQUIRE(cf_push_subscription_count(scratch.db, &count) == CF_OK);
    CF_CHECK(count == 3);
    cf_db_scratch_close(&scratch);
}

CF_TEST(badge_counts_unread_memberships) {
    cf_db_scratch scratch;
    CF_REQUIRE(ps_scratch_with_fixture(&scratch));

    cf_push_subscription davids = ps_make(1, NULL, NULL, NULL, NULL);
    int64_t badge = -1;
    CF_REQUIRE(cf_push_subscription_badge(scratch.db, &davids, &badge) ==
               CF_OK);
    CF_CHECK(badge == 1); /* membership 204 has unread_at */

    CF_REQUIRE(ps_insert(&scratch,
                         "UPDATE \"memberships\" SET \"unread_at\" = "
                         "'2026-01-03 00:00:00' WHERE \"id\" = 200;"));
    CF_REQUIRE(cf_push_subscription_badge(scratch.db, &davids, &badge) ==
               CF_OK);
    CF_CHECK(badge == 2);

    cf_push_subscription bender = ps_make(5, NULL, NULL, NULL, NULL);
    CF_REQUIRE(cf_push_subscription_badge(scratch.db, &bender, &badge) ==
               CF_OK);
    CF_CHECK(badge == 0);
    cf_db_scratch_close(&scratch);
}

CF_TEST(validate_accepts_permitted_public_endpoints) {
    const char *valid[] = {
        "https://fcm.googleapis.com/fcm/send/token123",
        "https://jmt17.google.com/fcm/send/token123",
        "https://updates.push.services.mozilla.com/wpush/v2/token123",
        "https://web.push.apple.com/QaBC123",
        "https://wns2-db5p.notify.windows.com/w/?token=abc123",
        "https://fcm.googleapis.com",
        "https://fcm.googleapis.com:443/fcm/send/x",
        "https://user:pass@fcm.googleapis.com/fcm/send/x",
        /* Boundary of the reference's strict ".permitted" suffix rule. */
        "https://.fcm.googleapis.com/x",
    };
    for (size_t i = 0; i < sizeof valid / sizeof valid[0]; i++) {
        cf_push_subscription subscription =
            ps_make(1, valid[i], "k", "a", NULL);
        cf_model_errors errors;
        memset(&errors, 0, sizeof errors);
        CF_REQUIRE(cf_push_subscription_validate(&subscription,
                                                 ps_resolve_public, NULL,
                                                 &errors) == CF_OK);
        CF_CHECK(errors.len == 0);
        cf_model_errors_dispose(&errors);
    }

    /* The resolver is called with the parsed host, original case preserved
     * (the reference resolves `uri.host`, not a lowercased copy). */
    ps_resolver_calls = 0;
    cf_push_subscription mixed =
        ps_make(1, "https://FCM.GoogleAPIs.com/fcm/send/x", "k", "a", NULL);
    cf_model_errors errors;
    memset(&errors, 0, sizeof errors);
    CF_REQUIRE(cf_push_subscription_validate(&mixed, ps_resolve_public, NULL,
                                             &errors) == CF_OK);
    CF_CHECK(errors.len == 0);
    CF_CHECK(strcmp(ps_resolver_host, "FCM.GoogleAPIs.com") == 0);
    cf_model_errors_dispose(&errors);
}

CF_TEST(validate_reports_the_reference_messages) {
    struct {
        const char *endpoint;
        cf_push_resolve_fn resolve;
        size_t error_count;
        const char *first;
        const char *second;
    } cases[] = {
        {"", ps_resolve_public, 2, "can't be blank", "is not a valid URL"},
        {"   ", ps_resolve_public, 2, "can't be blank", "is not a valid URL"},
        {"http://fcm.googleapis.com/x", ps_resolve_public, 1,
         "must use HTTPS", NULL},
        {"https://fcm.googleapis.com:8443/x", ps_resolve_public, 1,
         "must use the default HTTPS port", NULL},
        {"https://attacker.example.com/webhook", ps_resolve_public, 1,
         "is not a permitted push service", NULL},
        {"https://evilfcm.googleapis.com.attacker.example/webhook",
         ps_resolve_public, 1, "is not a permitted push service", NULL},
        {"fcm.googleapis.com/x", ps_resolve_public, 1, "is not a valid URL",
         NULL},
        {"https://fcm.googleapis.com:/x", ps_resolve_public, 1,
         "is not a valid URL", NULL},
        {"https://fcm.googleapis.com:65536/x", ps_resolve_public, 1,
         "is not a valid URL", NULL},
        {"https://fcm.googlea pis.com/x", ps_resolve_public, 1,
         "is not a valid URL", NULL},
        {"https://fcm.googleapis.com/fcm/send/x", ps_resolve_private, 1,
         "resolves to a private or invalid IP address", NULL},
        {"https://fcm.googleapis.com/fcm/send/x", NULL, 1,
         "resolves to a private or invalid IP address", NULL},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cf_push_subscription subscription =
            ps_make(1, cases[i].endpoint, "k", "a", NULL);
        cf_model_errors errors;
        memset(&errors, 0, sizeof errors);
        CF_REQUIRE(cf_push_subscription_validate(
                       &subscription, cases[i].resolve, NULL, &errors) ==
                   CF_OK);
        CF_CHECK(errors.len == cases[i].error_count);
        if (errors.len > 0) {
            CF_CHECK(ps_eq_str(errors.items[0].field, "endpoint"));
            CF_CHECK(ps_eq_str(errors.items[0].message, cases[i].first));
            if (cases[i].second != NULL) {
                CF_CHECK(ps_eq_str(errors.items[1].field, "endpoint"));
                CF_CHECK(
                    ps_eq_str(errors.items[1].message, cases[i].second));
            }
        }
        cf_model_errors_dispose(&errors);
    }

    /* An absent endpoint behaves like the empty string. */
    cf_push_subscription absent =
        ps_make(1, NULL, "k", "a", NULL);
    cf_model_errors errors;
    memset(&errors, 0, sizeof errors);
    CF_REQUIRE(cf_push_subscription_validate(&absent, ps_resolve_public, NULL,
                                             &errors) == CF_OK);
    CF_CHECK(errors.len == 2);
    CF_CHECK(ps_eq_str(errors.items[0].message, "can't be blank"));
    CF_CHECK(ps_eq_str(errors.items[1].message, "is not a valid URL"));
    cf_model_errors_dispose(&errors);
}

CF_TEST(malformed_endpoint_value_is_rejected_without_crash) {
    /* endpoint.present == true with value {ptr = NULL, len = 1}: a by-value
     * cf_str violating the len != 0 => ptr != NULL invariant. Siblings reject
     * the shape without dereferencing (ban.c:293, sound.c:119); validation
     * reports it like any other unparsable endpoint, and neither the resolver
     * nor the database is consulted. */
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    CF_REQUIRE(ps_insert(&scratch,
                         "INSERT INTO \"users\" (\"id\", \"name\", "
                         "\"created_at\", \"updated_at\") VALUES (1, 'David', "
                         "'2026-01-01 00:00:00', '2026-01-01 00:00:00');"));
    cf_tx tx = {scratch.db};

    cf_push_subscription malformed;
    memset(&malformed, 0, sizeof malformed);
    malformed.user_id = 1;
    malformed.endpoint.present = true;
    malformed.endpoint.value.ptr = NULL;
    malformed.endpoint.value.len = 1;

    /* validate: one URL error, as for any unparsable endpoint; no crash. */
    ps_resolver_calls = 0;
    cf_model_errors errors;
    memset(&errors, 0, sizeof errors);
    CF_REQUIRE(cf_push_subscription_validate(&malformed, ps_resolve_public,
                                             NULL, &errors) == CF_OK);
    CF_CHECK(errors.len == 1);
    CF_CHECK(ps_eq_str(errors.items[0].field, "endpoint"));
    CF_CHECK(ps_eq_str(errors.items[0].message, "is not a valid URL"));
    CF_CHECK(ps_resolver_calls == 0);
    cf_model_errors_dispose(&errors);

    /* resolved_endpoint_ip: false, output cleared, no dereference. */
    cf_optional_str ip;
    ip.present = true;
    ip.value.ptr = NULL;
    ip.value.len = 0;
    ps_resolver_calls = 0;
    CF_CHECK(!cf_push_subscription_resolved_endpoint_ip(
        &malformed, ps_resolve_public, NULL, &ip));
    CF_CHECK(!ip.present && ip.value.ptr == NULL);
    CF_CHECK(ps_resolver_calls == 0);

    /* create (validates first, against a real transaction): CF_INVALID, the
     * output record stays cleared and no row is inserted. */
    cf_push_subscription created;
    memset(&created, 0, sizeof created);
    CF_CHECK(cf_push_subscription_create(&tx, &malformed, ps_resolve_public,
                                         NULL, &created) == CF_INVALID);
    CF_CHECK(created.id == 0 && created.endpoint.value.ptr == NULL);

    int64_t count = -1;
    CF_REQUIRE(cf_push_subscription_count(scratch.db, &count) == CF_OK);
    CF_CHECK(count == 0);
    cf_db_scratch_close(&scratch);
}

CF_TEST(resolved_endpoint_ip_pins_public_permitted_hosts_only) {
    cf_push_subscription good =
        ps_make(1, "https://fcm.googleapis.com/fcm/send/x", "k", "a", NULL);
    cf_optional_str ip;
    memset(&ip, 0, sizeof ip);
    ps_resolver_calls = 0;
    CF_CHECK(cf_push_subscription_resolved_endpoint_ip(&good,
                                                       ps_resolve_public,
                                                       NULL, &ip));
    CF_CHECK(ps_resolver_calls == 1);
    CF_CHECK(ps_eq_opt(ip, "142.250.185.206"));
    cf_optional_str_dispose(&ip);

    memset(&ip, 0, sizeof ip);
    CF_CHECK(!cf_push_subscription_resolved_endpoint_ip(&good,
                                                        ps_resolve_private,
                                                        NULL, &ip));
    CF_CHECK(!ip.present);

    memset(&ip, 0, sizeof ip);
    CF_CHECK(!cf_push_subscription_resolved_endpoint_ip(&good, NULL, NULL,
                                                        &ip));

    /* Non-permitted host, non-default port and non-HTTPS never resolve, so
     * the callback is not consulted. */
    cf_push_subscription rejected[] = {
        ps_make(1, "https://attacker.example.com/collect", "k", "a", NULL),
        ps_make(1, "https://fcm.googleapis.com:22/x", "k", "a", NULL),
        ps_make(1, "http://fcm.googleapis.com/x", "k", "a", NULL),
        ps_make(1, "evilfcm.googleapis.com.attacker.example", "k", "a", NULL),
    };
    for (size_t i = 0; i < sizeof rejected / sizeof rejected[0]; i++) {
        memset(&ip, 0, sizeof ip);
        ps_resolver_calls = 0;
        CF_CHECK(!cf_push_subscription_resolved_endpoint_ip(
            &rejected[i], ps_resolve_public, NULL, &ip));
        CF_CHECK(ps_resolver_calls == 0);
        CF_CHECK(!ip.present);
    }

    cf_push_subscription absent = ps_make(1, NULL, "k", "a", NULL);
    memset(&ip, 0, sizeof ip);
    CF_CHECK(!cf_push_subscription_resolved_endpoint_ip(&absent,
                                                        ps_resolve_public,
                                                        NULL, &ip));
}

CF_TEST(payload_for_builds_room_relative_titles_and_paths) {
    cf_db_scratch scratch;
    CF_REQUIRE(ps_scratch_with_fixture(&scratch));
    CF_REQUIRE(ps_insert_message(&scratch, 500, 100, 1, "Hi"));

    cf_message message;
    ps_message_set(&message, 500, 100, 1);

    /* Non-direct room: room name and "Sender: body". */
    cf_room room;
    memset(&room, 0, sizeof room);
    room.id = 100;
    room.room_type = CF_ROOM_CLOSED;
    room.name = ps_opt("Designers");
    cf_push_payload payload;
    memset(&payload, 0, sizeof payload);
    CF_REQUIRE(cf_push_subscription_payload_for(scratch.db, NULL, &room,
                                                &message, &payload) == CF_OK);
    CF_CHECK(ps_eq_str(payload.title, "Designers"));
    CF_CHECK(ps_eq_str(payload.body, "David: Hi"));
    CF_CHECK(ps_eq_str(payload.path, "/rooms/100"));
    cf_push_payload_dispose(&payload);

    /* Direct room: sender as title, body unprefixed; its name is NULL. */
    CF_REQUIRE(ps_insert_message(&scratch, 501, 102, 1, "Hi"));
    ps_message_set(&message, 501, 102, 1);
    memset(&room, 0, sizeof room);
    room.id = 102;
    room.room_type = CF_ROOM_DIRECT;
    memset(&payload, 0, sizeof payload);
    CF_REQUIRE(cf_push_subscription_payload_for(scratch.db, NULL, &room,
                                                &message, &payload) == CF_OK);
    CF_CHECK(ps_eq_str(payload.title, "David"));
    CF_CHECK(ps_eq_str(payload.body, "Hi"));
    CF_CHECK(ps_eq_str(payload.path, "/rooms/102"));
    cf_push_payload_dispose(&payload);

    /* Missing body: empty plain text, still "Sender: ". */
    CF_REQUIRE(ps_insert_message(&scratch, 502, 100, 1, NULL));
    ps_message_set(&message, 502, 100, 1);
    memset(&room, 0, sizeof room);
    room.id = 100;
    room.room_type = CF_ROOM_CLOSED;
    room.name = ps_opt("Designers");
    memset(&payload, 0, sizeof payload);
    CF_REQUIRE(cf_push_subscription_payload_for(scratch.db, NULL, &room,
                                                &message, &payload) == CF_OK);
    CF_CHECK(ps_eq_str(payload.body, "David: "));
    cf_push_payload_dispose(&payload);
    cf_db_scratch_close(&scratch);
}

CF_TEST(payload_for_truncates_by_json_length) {
    cf_db_scratch scratch;
    CF_REQUIRE(ps_scratch_with_fixture(&scratch));

    /* Body: 1000 x 'é' (2 bytes each) then a quote then 2000 x 'x'. */
    size_t body_len = 2000 + 1 + 2000;
    char *body = malloc(body_len + 1);
    CF_REQUIRE(body != NULL);
    size_t at = 0;
    for (int i = 0; i < 1000; i++) {
        body[at++] = (char)0xC3;
        body[at++] = (char)0xA9;
    }
    body[at++] = '"';
    for (int i = 0; i < 2000; i++) body[at++] = 'x';
    body[at] = '\0';
    CF_REQUIRE(ps_insert_message(&scratch, 500, 100, 1, body));
    free(body);
    cf_message message;
    ps_message_set(&message, 500, 100, 1);
    cf_room room;
    memset(&room, 0, sizeof room);
    room.id = 100;
    room.room_type = CF_ROOM_CLOSED;
    room.name = ps_opt("Designers");
    cf_push_payload payload;
    memset(&payload, 0, sizeof payload);
    CF_REQUIRE(cf_push_subscription_payload_for(scratch.db, NULL, &room,
                                                &message, &payload) == CF_OK);
    CF_CHECK(payload.body.len >= 7 &&
             memcmp(payload.body.ptr, "David: ", 7) == 0);
    CF_CHECK(payload.body.len >= 9 &&
             memcmp(payload.body.ptr + 7, "\xC3\xA9\xC3\xA9", 4) == 0);
    CF_CHECK(payload.body.len >= 3 &&
             memcmp(payload.body.ptr + payload.body.len - 3,
                    "\xE2\x80\xA6", 3) == 0);
    CF_CHECK(payload.body.len >= 5 &&
             memcmp(payload.body.ptr + payload.body.len - 5, "xx", 2) == 0);
    size_t json_len = ps_test_json_len(payload.body);
    CF_CHECK(json_len <= CF_PUSH_SUBSCRIPTION_MAX_PAYLOAD_BODY_BYTES);
    CF_CHECK(json_len > CF_PUSH_SUBSCRIPTION_MAX_PAYLOAD_BODY_BYTES - 4);
    cf_push_payload_dispose(&payload);

    /* Long room name title is cut short the same way: 200 x 'é' = 400 bytes
     * of JSON content against the 256-byte title budget. */
    size_t name_len = 200 * 2;
    char *name = malloc(name_len + 1);
    CF_REQUIRE(name != NULL);
    for (size_t i = 0; i < name_len; i += 2) {
        name[i] = (char)0xC3;
        name[i + 1] = (char)0xA9;
    }
    name[name_len] = '\0';
    cf_room long_room;
    memset(&long_room, 0, sizeof long_room);
    long_room.id = 100;
    long_room.room_type = CF_ROOM_CLOSED;
    long_room.name.present = true;
    long_room.name.value.ptr = name;
    long_room.name.value.len = name_len;
    memset(&payload, 0, sizeof payload);
    CF_REQUIRE(cf_push_subscription_payload_for(scratch.db, NULL, &long_room,
                                                &message, &payload) == CF_OK);
    CF_CHECK(payload.title.len >= 3 &&
             memcmp(payload.title.ptr + payload.title.len - 3,
                    "\xE2\x80\xA6", 3) == 0);
    size_t title_json_len = ps_test_json_len(payload.title);
    CF_CHECK(title_json_len <= CF_PUSH_SUBSCRIPTION_MAX_PAYLOAD_TITLE_BYTES);
    CF_CHECK(title_json_len >
             CF_PUSH_SUBSCRIPTION_MAX_PAYLOAD_TITLE_BYTES - 4);
    cf_push_payload_dispose(&payload);
    free(name);
    cf_db_scratch_close(&scratch);
}

CF_TEST(for_users_involved_in_everything_selects_unconnected_everything_members) {
    cf_db_scratch scratch;
    CF_REQUIRE(ps_scratch_with_fixture(&scratch));
    int64_t now_us = 1700000000000000LL;

    cf_push_subscription_vector everything;
    memset(&everything, 0, sizeof everything);
    CF_REQUIRE(cf_push_subscription_for_users_involved_in_everything(
                   scratch.db, 100, 1, now_us, &everything) == CF_OK);
    CF_CHECK(everything.len == 2);
    int64_t ids[4] = {0};
    ps_collect_ids(&everything, ids);
    ps_sort_i64(ids, everything.len);
    CF_CHECK(ids[0] == 301 && ids[1] == 302);
    cf_push_subscription_vector_dispose(&everything);

    /* The creator is never notified of their own message. */
    memset(&everything, 0, sizeof everything);
    CF_REQUIRE(cf_push_subscription_for_users_involved_in_everything(
                   scratch.db, 100, 2, now_us, &everything) == CF_OK);
    CF_CHECK(everything.len == 1 && everything.items[0].id == 302);
    cf_push_subscription_vector_dispose(&everything);

    /* A connected membership is excluded until the 60 s cutoff passes. */
    char connected[CF_DB_TIME_TEXT_CAP];
    CF_REQUIRE(ps_test_time_text(now_us, connected) > 0);
    char sql[256];
    snprintf(sql, sizeof sql,
             "UPDATE \"memberships\" SET \"connected_at\" = '%s' WHERE "
             "\"id\" = 201;",
             connected);
    CF_REQUIRE(ps_insert(&scratch, sql));
    memset(&everything, 0, sizeof everything);
    CF_REQUIRE(cf_push_subscription_for_users_involved_in_everything(
                   scratch.db, 100, 1, now_us, &everything) == CF_OK);
    CF_CHECK(everything.len == 1 && everything.items[0].id == 302);
    cf_push_subscription_vector_dispose(&everything);

    char old[CF_DB_TIME_TEXT_CAP];
    CF_REQUIRE(ps_test_time_text(now_us - 2 * CF_MEMBERSHIP_CONNECTION_TTL_US,
                                 old) > 0);
    snprintf(sql, sizeof sql,
             "UPDATE \"memberships\" SET \"connected_at\" = '%s' WHERE "
             "\"id\" = 201;",
             old);
    CF_REQUIRE(ps_insert(&scratch, sql));
    memset(&everything, 0, sizeof everything);
    CF_REQUIRE(cf_push_subscription_for_users_involved_in_everything(
                   scratch.db, 100, 1, now_us, &everything) == CF_OK);
    CF_CHECK(everything.len == 2);
    cf_push_subscription_vector_dispose(&everything);

    /* A user without a subscription contributes no row. */
    CF_REQUIRE(ps_insert(&scratch,
                         "DELETE FROM \"push_subscriptions\" WHERE \"id\" = "
                         "301;"));
    memset(&everything, 0, sizeof everything);
    CF_REQUIRE(cf_push_subscription_for_users_involved_in_everything(
                   scratch.db, 100, 1, now_us, &everything) == CF_OK);
    CF_CHECK(everything.len == 1 && everything.items[0].id == 302);
    cf_push_subscription_vector_dispose(&everything);
    cf_db_scratch_close(&scratch);
}

CF_TEST(for_mentioned_users_only_mentions_involvement) {
    cf_db_scratch scratch;
    CF_REQUIRE(ps_scratch_with_fixture(&scratch));
    int64_t now_us = 1700000000000000LL;

    /* kevin (mentions) yes, jz (everything) no. */
    int64_t mentionees[] = {3, 4};
    cf_push_subscription_vector mentions;
    memset(&mentions, 0, sizeof mentions);
    CF_REQUIRE(cf_push_subscription_for_mentioned_users(
                   scratch.db, 100, 1, mentionees, 2, now_us, &mentions) ==
               CF_OK);
    CF_CHECK(mentions.len == 1 && mentions.items[0].id == 303);
    cf_push_subscription_vector_dispose(&mentions);

    /* The creator is excluded even when mentioned. */
    int64_t creator[] = {1, 4};
    memset(&mentions, 0, sizeof mentions);
    CF_REQUIRE(cf_push_subscription_for_mentioned_users(
                   scratch.db, 100, 1, creator, 2, now_us, &mentions) ==
               CF_OK);
    CF_CHECK(mentions.len == 1 && mentions.items[0].id == 303);
    cf_push_subscription_vector_dispose(&mentions);

    /* Empty list short-circuits without a query. */
    memset(&mentions, 0, sizeof mentions);
    CF_REQUIRE(cf_push_subscription_for_mentioned_users(
                   scratch.db, 100, 1, NULL, 0, now_us, &mentions) == CF_OK);
    CF_CHECK(mentions.len == 0);
    cf_push_subscription_vector_dispose(&mentions);

    /* Duplicate ids keep IN set semantics; unknown ids add nothing. */
    int64_t duplicates[] = {4, 4, 999};
    memset(&mentions, 0, sizeof mentions);
    CF_REQUIRE(cf_push_subscription_for_mentioned_users(
                   scratch.db, 100, 1, duplicates, 3, now_us, &mentions) ==
               CF_OK);
    CF_CHECK(mentions.len == 1 && mentions.items[0].id == 303);
    cf_push_subscription_vector_dispose(&mentions);

    /* A connected mentionee is excluded (same cutoff as everything). */
    char connected[CF_DB_TIME_TEXT_CAP];
    CF_REQUIRE(ps_test_time_text(now_us, connected) > 0);
    char sql[256];
    snprintf(sql, sizeof sql,
             "UPDATE \"memberships\" SET \"connected_at\" = '%s' WHERE "
             "\"id\" = 203;",
             connected);
    CF_REQUIRE(ps_insert(&scratch, sql));
    int64_t kevin[] = {4};
    memset(&mentions, 0, sizeof mentions);
    CF_REQUIRE(cf_push_subscription_for_mentioned_users(
                   scratch.db, 100, 1, kevin, 1, now_us, &mentions) == CF_OK);
    CF_CHECK(mentions.len == 0);
    cf_push_subscription_vector_dispose(&mentions);

    /* A mentionee with no subscription or not in the room adds nothing. */
    int64_t outsiders[] = {5, 999};
    memset(&mentions, 0, sizeof mentions);
    CF_REQUIRE(cf_push_subscription_for_mentioned_users(
                   scratch.db, 100, 1, outsiders, 2, now_us, &mentions) ==
               CF_OK);
    CF_CHECK(mentions.len == 0);
    cf_push_subscription_vector_dispose(&mentions);
    cf_db_scratch_close(&scratch);
}

CF_TEST(pushes_for_returns_payload_everything_and_mentions) {
    cf_db_scratch scratch;
    CF_REQUIRE(ps_scratch_with_fixture(&scratch));
    /* Kevin is mentioned through the stored body, not a test-stated list:
     * sign the A01 attachable SGID with the pipeline's key, then store it as
     * a real mention attachment. */
    cf_richtext_configure(ps_span(PS_TEST_SECRET));
    cf_str sgid = {0};
    CF_REQUIRE(cf_auth_sgid_generate_attachable(
                   ps_span(PS_TEST_SECRET), ps_span("gid://campfire/User/4"),
                   &sgid) == CF_OK);
    char *body = ps_mention_body(sgid.ptr);
    cf_str_dispose(&sgid);
    CF_REQUIRE(body != NULL);
    CF_REQUIRE(ps_insert_message(&scratch, 500, 100, 1, body));
    free(body);
    int64_t now_us = 1700000000000000LL;

    cf_message message;
    ps_message_set(&message, 500, 100, 1);

    cf_push_payload payload;
    memset(&payload, 0, sizeof payload);
    cf_push_subscription_vector everything;
    memset(&everything, 0, sizeof everything);
    cf_push_subscription_vector mentions;
    memset(&mentions, 0, sizeof mentions);
    CF_REQUIRE(cf_push_subscription_pushes_for(
                   scratch.db, cf_tx_rich_text(NULL), &message, now_us,
                   &payload, &everything, &mentions) == CF_OK);
    CF_CHECK(ps_eq_str(payload.title, "Designers"));
    /* PlainTextConversion renders the verified mention attachment as "@Kevin"
     * (rt_attach.c rt_attachment_plain_text; plain_text.rs). */
    CF_CHECK(ps_eq_str(payload.body, "David: Hi @Kevin"));
    CF_CHECK(ps_eq_str(payload.path, "/rooms/100"));
    CF_CHECK(everything.len == 2);
    int64_t ids[4] = {0};
    ps_collect_ids(&everything, ids);
    ps_sort_i64(ids, everything.len);
    CF_CHECK(ids[0] == 301 && ids[1] == 302);
    CF_CHECK(mentions.len == 1 && mentions.items[0].id == 303);
    cf_push_payload_dispose(&payload);
    cf_push_subscription_vector_dispose(&everything);
    cf_push_subscription_vector_dispose(&mentions);

    /* Direct room, no mentionees: only everything, sender as title. */
    CF_REQUIRE(ps_insert_message(&scratch, 501, 102, 1, "Hi"));
    ps_message_set(&message, 501, 102, 1);
    memset(&payload, 0, sizeof payload);
    memset(&everything, 0, sizeof everything);
    memset(&mentions, 0, sizeof mentions);
    CF_REQUIRE(cf_push_subscription_pushes_for(
                   scratch.db, cf_tx_rich_text(NULL), &message, now_us,
                   &payload, &everything, &mentions) == CF_OK);
    CF_CHECK(ps_eq_str(payload.title, "David"));
    CF_CHECK(ps_eq_str(payload.body, "Hi"));
    CF_CHECK(everything.len == 1 && everything.items[0].id == 301);
    CF_CHECK(mentions.len == 0);
    cf_push_payload_dispose(&payload);
    cf_push_subscription_vector_dispose(&everything);
    cf_push_subscription_vector_dispose(&mentions);

    /* Missing room: pushes_for fails, outputs stay empty. */
    ps_message_set(&message, 500, 9999, 1);
    memset(&payload, 0, sizeof payload);
    memset(&everything, 0, sizeof everything);
    memset(&mentions, 0, sizeof mentions);
    CF_CHECK(cf_push_subscription_pushes_for(
                 scratch.db, cf_tx_rich_text(NULL), &message, now_us, &payload,
                 &everything, &mentions) == CF_NOT_FOUND);
    CF_CHECK(payload.title.ptr == NULL && everything.len == 0 &&
             mentions.len == 0);
    cf_push_payload_dispose(&payload);
    cf_push_subscription_vector_dispose(&everything);
    cf_push_subscription_vector_dispose(&mentions);
    cf_db_scratch_close(&scratch);
}

CF_TEST(null_arguments_are_guarded_and_outputs_clear_on_entry) {
    /* validate/create/destroy take borrowed pointers: a NULL subscription or
     * transaction is rejected, never dereferenced (sibling convention,
     * cf_ban / cf_webhook). */
    cf_model_errors errors = {NULL, 7, 7};
    cf_push_subscription subscription;
    memset(&subscription, 0, sizeof subscription);
    cf_push_subscription out;
    memset(&out, 0, sizeof out);

    CF_CHECK(cf_push_subscription_validate(NULL, NULL, NULL, &errors) ==
             CF_INVALID);
    CF_CHECK(errors.len == 0 && errors.items == NULL && errors.cap == 0);
    CF_CHECK(cf_push_subscription_validate(&subscription, NULL, NULL, NULL) ==
             CF_INVALID);
    CF_CHECK(cf_push_subscription_create(NULL, &subscription, NULL, NULL,
                                         &out) == CF_INVALID);
    CF_CHECK(out.id == 0 && !out.endpoint.present);
    CF_CHECK(cf_push_subscription_create(NULL, NULL, NULL, NULL, &out) ==
             CF_INVALID);
    CF_CHECK(cf_push_subscription_create(NULL, NULL, NULL, NULL, NULL) ==
             CF_INVALID);
    CF_CHECK(cf_push_subscription_destroy(NULL, NULL) == CF_INVALID);
    CF_CHECK(cf_push_subscription_destroy(NULL, &subscription) == CF_INVALID);

    /* Vector outputs clear *out on entry (cf_ban_for_user convention): an
     * unzeroed caller vector is normalized instead of appended to, and the
     * outputs stay empty on failure and on the empty-list early return. */
    cf_db_scratch scratch;
    CF_REQUIRE(ps_scratch_with_fixture(&scratch));
    int64_t now_us = 1700000000000000LL;

    cf_push_subscription_vector for_user = {NULL, 0, 3};
    CF_REQUIRE(cf_push_subscription_for_user(scratch.db, 999, &for_user) ==
               CF_OK);
    CF_CHECK(for_user.len == 0 && for_user.items == NULL && for_user.cap == 0);

    cf_push_subscription_vector everything = {NULL, 7, 7};
    CF_REQUIRE(cf_push_subscription_for_users_involved_in_everything(
                   scratch.db, 100, 1, now_us, &everything) == CF_OK);
    CF_CHECK(everything.len == 2);
    cf_push_subscription_vector_dispose(&everything);

    cf_push_subscription_vector mentions = {NULL, 7, 7};
    CF_REQUIRE(cf_push_subscription_for_mentioned_users(
                   scratch.db, 100, 1, NULL, 0, now_us, &mentions) == CF_OK);
    CF_CHECK(mentions.len == 0 && mentions.items == NULL && mentions.cap == 0);

    cf_push_subscription_vector bad = {NULL, 0, 0};
    CF_CHECK(cf_push_subscription_for_mentioned_users(
                  NULL, 100, 1, NULL, 0, now_us, &bad) == CF_INVALID);
    CF_CHECK(bad.len == 0);
    cf_db_scratch_close(&scratch);
}

/* Every public entry point rejects NULL for every pointer parameter that the
 * sibling models guard (outputs, found flags, databases, borrowed records,
 * transactions) with CF_INVALID (or false for the bool-returning resolver
 * check), never dereferencing it; record/vector outputs that siblings clear
 * on entry start zeroed. `rich_text` is deliberately absent: NULL is its
 * documented "no rich text record" value (see payload_for above). */
CF_TEST(all_public_null_arguments_are_guarded) {
    cf_db_scratch scratch;
    CF_REQUIRE(ps_scratch_with_fixture(&scratch));
    int64_t now_us = 1700000000000000LL;
    cf_optional_str absent = ps_absent();

    /* Disposers are no-ops on NULL. */
    cf_push_subscription_dispose(NULL);
    cf_push_subscription_vector_dispose(NULL);
    cf_push_payload_dispose(NULL);

    /* new: output record required. */
    CF_CHECK(cf_push_subscription_new(1, absent, absent, absent, absent,
                                      NULL) == CF_INVALID);

    /* find: output record and database required; the record clears first. */
    cf_push_subscription row;
    memset(&row, 0x5A, sizeof row);
    CF_CHECK(cf_push_subscription_find(NULL, 300, NULL) == CF_INVALID);
    CF_CHECK(cf_push_subscription_find(NULL, 300, &row) == CF_INVALID);
    CF_CHECK(row.id == 0 && !row.endpoint.present &&
             row.endpoint.value.ptr == NULL);
    CF_CHECK(cf_push_subscription_find(scratch.db, 300, NULL) == CF_INVALID);

    /* count: output pointer and database required; the count clears first. */
    int64_t count = 12345;
    CF_CHECK(cf_push_subscription_count(NULL, &count) == CF_INVALID);
    CF_CHECK(count == 0);
    CF_CHECK(cf_push_subscription_count(scratch.db, NULL) == CF_INVALID);

    /* for_user: output vector and database required. */
    cf_push_subscription_vector vector = {NULL, 0, 0};
    CF_CHECK(cf_push_subscription_for_user(NULL, 1, &vector) == CF_INVALID);
    CF_CHECK(vector.len == 0);
    CF_CHECK(cf_push_subscription_for_user(scratch.db, 1, NULL) == CF_INVALID);

    /* find_for_user_by_keys: found flag and record required; a NULL database
     * is rejected after both have cleared. */
    bool found = true;
    memset(&row, 0x5A, sizeof row);
    CF_CHECK(cf_push_subscription_find_for_user_by_keys(
                  NULL, 1, CF_STR_LIT("e"), CF_STR_LIT("p"), CF_STR_LIT("a"),
                  &found, &row) == CF_INVALID);
    CF_CHECK(!found && row.id == 0 && row.endpoint.value.ptr == NULL);
    CF_CHECK(cf_push_subscription_find_for_user_by_keys(
                  scratch.db, 1, CF_STR_LIT("e"), CF_STR_LIT("p"),
                  CF_STR_LIT("a"), NULL, &row) == CF_INVALID);
    CF_CHECK(cf_push_subscription_find_for_user_by_keys(
                  scratch.db, 1, CF_STR_LIT("e"), CF_STR_LIT("p"),
                  CF_STR_LIT("a"), &found, NULL) == CF_INVALID);

    /* create/destroy: transaction, record and output guards. */
    cf_push_subscription subscription = ps_make(1, NULL, NULL, NULL, NULL);
    cf_tx tx = {scratch.db};
    cf_push_subscription out;
    memset(&out, 0, sizeof out);
    CF_CHECK(cf_push_subscription_create(NULL, &subscription, NULL, NULL,
                                         &out) == CF_INVALID);
    CF_CHECK(cf_push_subscription_create(&tx, NULL, NULL, NULL, &out) ==
             CF_INVALID);
    CF_CHECK(cf_push_subscription_create(&tx, &subscription, NULL, NULL,
                                         NULL) == CF_INVALID);
    CF_CHECK(cf_push_subscription_destroy(NULL, &subscription) == CF_INVALID);
    CF_CHECK(cf_push_subscription_destroy(&tx, NULL) == CF_INVALID);

    /* destroy_by_endpoint: transaction required. */
    CF_CHECK(cf_push_subscription_destroy_by_endpoint(
                  NULL, 3, CF_STR_LIT("https://fcm.googleapis.com/x")) ==
             CF_INVALID);

    /* badge: output, database and record required; the badge clears first. */
    int64_t badge = 12345;
    CF_CHECK(cf_push_subscription_badge(NULL, &subscription, &badge) ==
             CF_INVALID);
    CF_CHECK(badge == 0);
    CF_CHECK(cf_push_subscription_badge(scratch.db, NULL, &badge) ==
             CF_INVALID);
    CF_CHECK(badge == 0);
    CF_CHECK(cf_push_subscription_badge(scratch.db, &subscription, NULL) ==
             CF_INVALID);

    /* validate: record and error list required (see also the case above). */
    cf_model_errors errors = {NULL, 0, 0};
    CF_CHECK(cf_push_subscription_validate(NULL, NULL, NULL, &errors) ==
             CF_INVALID);
    CF_CHECK(cf_push_subscription_validate(&subscription, NULL, NULL, NULL) ==
             CF_INVALID);

    /* resolved_endpoint_ip: a NULL record is unresolvable and false. */
    cf_optional_str ip;
    ip.present = true;
    ip.value.ptr = NULL;
    ip.value.len = 0;
    CF_CHECK(!cf_push_subscription_resolved_endpoint_ip(
        NULL, ps_resolve_public, NULL, &ip));
    CF_CHECK(!ip.present);
    CF_CHECK(!cf_push_subscription_resolved_endpoint_ip(NULL, NULL, NULL,
                                                        NULL));
    CF_CHECK(!cf_push_subscription_resolved_endpoint_ip(
        &subscription, ps_resolve_public, NULL, NULL));

    /* payload_for: output, database and borrowed records required; the
     * payload clears first. NULL rich_text stays legal (not probed here). */
    cf_room room;
    memset(&room, 0, sizeof room);
    room.id = 100;
    room.room_type = CF_ROOM_CLOSED;
    room.name = ps_opt("Designers");
    cf_message message;
    ps_message_set(&message, 500, 100, 1);
    cf_push_payload payload;
    memset(&payload, 0, sizeof payload);
    CF_CHECK(cf_push_subscription_payload_for(NULL, NULL, &room, &message,
                                              &payload) == CF_INVALID);
    CF_CHECK(payload.title.ptr == NULL && payload.body.ptr == NULL &&
             payload.path.ptr == NULL);
    CF_CHECK(cf_push_subscription_payload_for(scratch.db, NULL, NULL,
                                              &message, &payload) ==
             CF_INVALID);
    CF_CHECK(cf_push_subscription_payload_for(scratch.db, NULL, &room, NULL,
                                              &payload) == CF_INVALID);
    CF_CHECK(cf_push_subscription_payload_for(scratch.db, NULL, &room,
                                              &message, NULL) == CF_INVALID);

    /* for_users_involved_in_everything: output and database required. */
    CF_CHECK(cf_push_subscription_for_users_involved_in_everything(
                  NULL, 100, 1, now_us, &vector) == CF_INVALID);
    CF_CHECK(vector.len == 0);
    CF_CHECK(cf_push_subscription_for_users_involved_in_everything(
                  scratch.db, 100, 1, now_us, NULL) == CF_INVALID);

    /* for_mentioned_users: output, database and id list required for len > 0;
     * a NULL list with length 0 keeps the reference's empty short-circuit. */
    int64_t ids[] = {1};
    CF_CHECK(cf_push_subscription_for_mentioned_users(
                  NULL, 100, 1, ids, 1, now_us, &vector) == CF_INVALID);
    CF_CHECK(cf_push_subscription_for_mentioned_users(
                  scratch.db, 100, 1, ids, 1, now_us, NULL) == CF_INVALID);
    CF_CHECK(cf_push_subscription_for_mentioned_users(
                  scratch.db, 100, 1, NULL, 1, now_us, &vector) ==
             CF_INVALID);
    vector.len = 0;
    CF_CHECK(cf_push_subscription_for_mentioned_users(
                  scratch.db, 100, 1, NULL, 0, now_us, &vector) == CF_OK);
    CF_CHECK(vector.len == 0 && vector.items == NULL && vector.cap == 0);

    /* pushes_for: all three outputs, database and message required; the
     * outputs clear first (and then the NULL database is rejected). */
    memset(&payload, 0, sizeof payload);
    vector = (cf_push_subscription_vector){NULL, 0, 0};
    cf_push_subscription_vector mentions = {NULL, 0, 0};
    CF_CHECK(cf_push_subscription_pushes_for(
                  scratch.db, NULL, &message, now_us, NULL, &vector,
                  &mentions) == CF_INVALID);
    CF_CHECK(cf_push_subscription_pushes_for(
                  scratch.db, NULL, &message, now_us, &payload, NULL,
                  &mentions) == CF_INVALID);
    CF_CHECK(cf_push_subscription_pushes_for(
                  scratch.db, NULL, &message, now_us, &payload, &vector,
                  NULL) == CF_INVALID);
    CF_CHECK(cf_push_subscription_pushes_for(
                  NULL, NULL, &message, now_us, &payload, &vector,
                  &mentions) == CF_INVALID);
    CF_CHECK(payload.title.ptr == NULL && vector.len == 0 &&
             mentions.len == 0);
    CF_CHECK(cf_push_subscription_pushes_for(
                  scratch.db, NULL, NULL, now_us, &payload, &vector,
                  &mentions) == CF_INVALID);
    CF_CHECK(payload.title.ptr == NULL && vector.len == 0 &&
             mentions.len == 0);
    cf_push_subscription_vector_dispose(&vector);
    cf_push_subscription_vector_dispose(&mentions);
    cf_db_scratch_close(&scratch);
}

CF_TEST_MAIN()
