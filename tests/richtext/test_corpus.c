/* R02 rich-text differential corpus (07-verification.md VIEW-02).
 *
 * Replays tests/fixtures/crates/richtext/tests/corpus/expected.json (the F02
 * copy; RICHTEXT_CORPUS overrides) exactly like the reference
 * crates/richtext/tests/corpus.rs: presentation, plain text, editable,
 * mentioned users and web_url, plus the security assertions on every rendered
 * output and on the oracle's own outputs.
 *
 * The resolver is the production one (rt_resolver.c): A01's SGID verifier
 * with the pinned reference SECRET_KEY_BASE over a scratch SQLite database
 * seeded with the corpus users. The deliberate port divergences (escaped
 * attribute brackets, dropped name attributes, rails_autolink's in-attribute
 * links left as text) are applied to the Rails expectations the same way the
 * reference test applies them.
 *
 * Build (plain):
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -Isrc -Itests -Ivendor/src/yyjson/src
 *         -Ivendor/src/sqlite/sqlite-amalgamation-3530400
 *         -Ivendor/src/gumbo/gumbo-parser/src
 *         tests/richtext/test_corpus.c src/richtext sources + models/db/auth/core
 *         vendor/build/{gumbo-clang/libgumbo.a,yyjson-clang/libyyjson.a,
 *         sqlite-clang/libsqlite3.a,openssl-clang/install/lib/libcrypto.a,
 *         libxcrypt-clang/.libs/libcrypt.a} -lm
 */
#include "cf_test.h"

#include "auth.h"
#include "core/testclock.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "models/types.h"
#include "models/user.h"
#include "richtext.h"
#include "richtext/internal.h"

#include "yyjson.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* parity/.env.reference (the corpus generator's fixed secret). */
#define CORPUS_SECRET \
    "5335c3b1ad35b4ad170c3413bd651ef3b6ed64e257261871a6de3f978cf3868e" \
    "e417a927040935fb30b0f7debdedb34a2a403e9f34b16cf594c917c2ecd4a995"
#define CORPUS_DEFAULT_PATH "tests/fixtures/crates/richtext/tests/corpus/expected.json"
#define CORPUS_HOST_DEFAULT "once.campfire.test"
/* 2026-01-01T00:00:00Z: after every expiry the corpus mints. */
#define CORPUS_NOW_US INT64_C(1767225600000000)

/* ---- corpus loading ------------------------------------------------------ */

static char *read_file(const char *path, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    char *bytes = malloc((size_t)size + 1);
    if (bytes == NULL) {
        fclose(file);
        return NULL;
    }
    if (fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    bytes[size] = '\0';
    fclose(file);
    *out_len = (size_t)size;
    return bytes;
}

/* ---- scratch database with the corpus users ------------------------------ */

typedef struct {
    char *dir;
    char path[CF_DB_TEST_PATH_CAP];
    cf_db *reader;
    rt_db_resolver resolver;
    bool have_resolver;
} corpus_world;

static void world_seed_users(cf_db *db, yyjson_val *users) {
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_exec(handle, "BEGIN", NULL, NULL, NULL);
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO users (id, name, bio, created_at, updated_at) VALUES (?, ?, ?, ?, ?)";
    CF_REQUIRE(sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) == SQLITE_OK);
    size_t count = yyjson_arr_size(users);
    for (size_t i = 0; i < count; i++) {
        yyjson_val *user = yyjson_arr_get(users, i);
        int64_t id = yyjson_get_sint(yyjson_obj_get(user, "id"));
        const char *name = yyjson_get_str(yyjson_obj_get(user, "name"));
        const char *title = yyjson_get_str(yyjson_obj_get(user, "title"));
        /* User#title = [name, bio].compact_blank.join(" – "); invert it. */
        char *bio = NULL;
        size_t name_len = strlen(name);
        if (title != NULL && strncmp(title, name, name_len) == 0 &&
            strncmp(title + name_len, " – ", strlen(" – ")) == 0) {
            bio = (char *)title + name_len + strlen(" – ");
        }
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, id);
        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
        if (bio != NULL) sqlite3_bind_text(stmt, 3, bio, -1, SQLITE_STATIC);
        else sqlite3_bind_null(stmt, 3);
        /* avatar_path v=20240102030405 in the corpus. */
        sqlite3_bind_text(stmt, 4, "2024-01-02 03:04:05", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, "2024-01-02 03:04:05", -1, SQLITE_STATIC);
        CF_REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(handle, "COMMIT", NULL, NULL, NULL);
}

static void world_open(corpus_world *world, yyjson_val *users) {
    memset(world, 0, sizeof *world);
    world->dir = cf_db_test_dir();
    CF_REQUIRE(world->dir != NULL);
    cf_db_test_path(world->path, sizeof world->path, world->dir);
    cf_db *db = NULL;
    CF_REQUIRE(cf_db_open(world->path, false, &db) == CF_OK);
    world_seed_users(db, users);
    cf_db_close(db);
    CF_REQUIRE(cf_db_open(world->path, true, &world->reader) == CF_OK);
    cf_span secret = {(const unsigned char *)CORPUS_SECRET, strlen(CORPUS_SECRET)};
    rt_db_resolver_init(&world->resolver, world->reader, secret, CORPUS_NOW_US);
    world->have_resolver = true;
}

static void world_close(corpus_world *world) {
    if (world->reader != NULL) cf_db_close(world->reader);
    world->reader = NULL;
    if (world->dir != NULL) {
        cf_db_test_cleanup(world->dir);
        free(world->dir);
        world->dir = NULL;
    }
}

/* ---- port divergences (corpus.rs with_port_divergences) ------------------- */

static const char CORPUS_INSERTED_LINK[] = "<a target=\"_blank\" href=\"";

static void diverged_into(const char *rails, rt_buf *out) {
    enum { STATE_TEXT, STATE_TAG, STATE_VALUE } state = STATE_TEXT;
    const char *rest = rails;
    while (*rest != '\0') {
        if (state == STATE_VALUE &&
            strncmp(rest, CORPUS_INSERTED_LINK, sizeof CORPUS_INSERTED_LINK - 1) == 0) {
            const char *close = strstr(rest, "\">");
            CF_REQUIRE(close != NULL);
            size_t text_start = (size_t)(close - rest) + 2;
            const char *text_end_rel = strstr(rest + text_start, "</a>");
            CF_REQUIRE(text_end_rel != NULL);
            size_t text_end = (size_t)(text_end_rel - rest);
            for (size_t i = text_start; i < text_end; i++) {
                if (rest[i] == '>') rt_buf_puts(out, "&gt;");
                else rt_buf_putc(out, (unsigned char)rest[i]);
            }
            rest += text_end + 4;
            continue;
        }
        if (state == STATE_TAG && strncmp(rest, " name=\"", 7) == 0) {
            const char *value_end = strchr(rest + 7, '"');
            CF_REQUIRE(value_end != NULL);
            rest = value_end + 1;
            continue;
        }
        unsigned char c = (unsigned char)*rest;
        size_t width = 1;
        if (c >= 0xF0) width = 4;
        else if (c >= 0xE0) width = 3;
        else if (c >= 0xC0) width = 2;
        switch (state) {
        case STATE_TEXT:
            if (c == '<') state = STATE_TAG;
            break;
        case STATE_TAG:
            if (c == '>') state = STATE_TEXT;
            else if (c == '"') state = STATE_VALUE;
            break;
        case STATE_VALUE:
            if (c == '"') state = STATE_TAG;
            break;
        }
        if (state == STATE_VALUE && c == '<') rt_buf_puts(out, "&lt;");
        else if (state == STATE_VALUE && c == '>') rt_buf_puts(out, "&gt;");
        else rt_buf_append(out, rest, width);
        rest += width;
    }
}

/* ---- security assertions (corpus.rs security_violations) ----------------- */

static const char *const DANGEROUS_ELEMENTS[] = {
    "script", "style", "iframe", "frame", "frameset", "object", "embed", "applet", "base", "meta",
    "link", "form", "input", "button", "textarea", "select", "svg", "math", "template", "noscript",
    "xmp", "plaintext", "noembed",
};
static const char *const URL_ATTRIBUTES[] = {"href", "src", "action", "formaction", "poster",
                                             "cite", "background", "xlink:href", "srcset", "data"};

static bool list_has(const char *const *items, size_t count, const char *value) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(items[i], value) == 0) return true;
    }
    return false;
}

static bool dangerous_url(const char *value) {
    rt_buf cleaned;
    rt_buf_init(&cleaned);
    const unsigned char *bytes = (const unsigned char *)value;
    size_t len = strlen(value);
    for (size_t i = 0; i < len;) {
        uint32_t code;
        size_t width = rt_utf8_decode(bytes + i, len - i, &code);
        if (width == 0) width = 1;
        if (!rt_is_whitespace(code) && code >= 0x20 && code != 0x7F) {
            if (code < 0x80 && code >= 'A' && code <= 'Z') code = code - 'A' + 'a';
            rt_buf_put_utf8(&cleaned, code);
        }
        i += width;
    }
    char lower[4096];
    size_t n = cleaned.len < sizeof lower - 1 ? cleaned.len : sizeof lower - 1;
    memcpy(lower, cleaned.data, n);
    lower[n] = '\0';
    rt_buf_dispose(&cleaned);
    if (strncmp(lower, "javascript:", 11) == 0 || strncmp(lower, "vbscript:", 9) == 0 ||
        strncmp(lower, "livescript:", 11) == 0) {
        return true;
    }
    if (strncmp(lower, "data:", 5) == 0) {
        const char *rest = lower + 5;
        const char *comma = strchr(rest, ',');
        size_t meta_len = comma != NULL ? (size_t)(comma - rest) : strlen(rest);
        const char *semi = memchr(rest, ';', meta_len);
        size_t mediatype_len = semi != NULL ? (size_t)(semi - rest) : meta_len;
        char mediatype[256];
        size_t copy = mediatype_len < sizeof mediatype - 1 ? mediatype_len : sizeof mediatype - 1;
        memcpy(mediatype, rest, copy);
        mediatype[copy] = '\0';
        const char *slash = strchr(mediatype, '/');
        bool valid = slash != NULL && slash != mediatype && slash[1] != '\0';
        for (size_t i = 0; i < copy && valid; i++) {
            if (mediatype[i] == '/' && slash == mediatype + i) continue;
            unsigned char ch = (unsigned char)mediatype[i];
            bool token = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                         (ch >= '0' && ch <= '9') || strchr("!#$%&'*+-.^_`|~", ch) != NULL;
            if (!token) valid = false;
        }
        if (valid && (strstr(mediatype, "html") || strstr(mediatype, "xml") ||
                      strstr(mediatype, "svg") || strstr(mediatype, "script") ||
                      strstr(mediatype, "ecmascript"))) {
            return true;
        }
    }
    return false;
}

static void security_violations_into(const unsigned char *html, size_t len, bool allow_style,
                                     rt_buf *report) {
    rt_dom dom;
    rt_dom_init(&dom);
    rt_node root = RT_NODE_NONE;
    rt_status rc = rt_dom_parse_fragment(&dom, html, len, &root);
    if (rc != RT_OK) {
        rt_buf_puts(report, "unparseable output\n");
        rt_dom_dispose(&dom);
        return;
    }
    const rt_safe_list *allowed = rt_safe_list_auto_link();
    rt_node_vec all = rt_dom_descendants(&dom, root);
    for (size_t i = 0; i < all.len; i++) {
        rt_node node = all.items[i];
        const char *name = rt_dom_local_name(&dom, node);
        if (name == NULL) continue;
        if (list_has(DANGEROUS_ELEMENTS, sizeof DANGEROUS_ELEMENTS / sizeof DANGEROUS_ELEMENTS[0],
                     name)) {
            rt_buf_printf(report, "<%s> element\n", name);
        }
        if (!rt_safe_list_allows_tag(allowed, name)) {
            rt_buf_printf(report, "<%s> not in the allowlist\n", name);
        }
        rt_dom_node *element = &dom.nodes[node];
        for (size_t a = 0; a < element->attrs_len; a++) {
            rt_buf qualified;
            rt_buf_init(&qualified);
            rt_dom_attr_qualified_name(&element->attrs[a], &qualified);
            char attr[256];
            size_t copy = qualified.len < sizeof attr - 1 ? qualified.len : sizeof attr - 1;
            memcpy(attr, qualified.data, copy);
            attr[copy] = '\0';
            rt_buf_dispose(&qualified);
            if (!rt_safe_list_allows_attr(allowed, attr) && strcmp(attr, "target") != 0 &&
                !(strcmp(attr, "style") == 0 && allow_style)) {
                rt_buf_printf(report, "%s on <%s> not in the allowlist\n", attr, name);
            }
            char lower[256];
            for (size_t k = 0; k <= copy; k++) {
                char c = attr[k];
                lower[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            }
            if (strncmp(lower, "on", 2) == 0) {
                rt_buf_printf(report, "%s attribute on <%s>\n", attr, name);
            }
            if (list_has(URL_ATTRIBUTES, sizeof URL_ATTRIBUTES / sizeof URL_ATTRIBUTES[0], lower) &&
                dangerous_url(element->attrs[a].value)) {
                rt_buf_printf(report, "%s=%s on <%s>\n", attr, element->attrs[a].value, name);
            }
            if (strcmp(lower, "style") == 0 && !allow_style) {
                rt_buf_printf(report, "style on <%s>\n", name);
            }
            if (strncmp(lower, "data-", 5) == 0 && strcmp(lower, "data-language") != 0) {
                rt_buf_printf(report, "%s on <%s>\n", attr, name);
            }
        }
    }
    rt_node_vec_dispose(&all);
    rt_dom_dispose(&dom);
}

static bool security_ok(const unsigned char *html, size_t len, bool allow_style) {
    rt_buf report;
    rt_buf_init(&report);
    security_violations_into(html, len, allow_style, &report);
    bool clean = report.len == 0;
    if (!clean) printf("    SECURITY: %.*s", (int)report.len, report.data);
    rt_buf_dispose(&report);
    return clean;
}

/* ---- tallies ------------------------------------------------------------- */

typedef struct {
    size_t total;
    size_t mismatched;
    size_t printed;
} tally;

static void record_mismatch(tally *t, const char *label, const char *expected, const char *actual) {
    t->total++;
    if (strcmp(expected, actual) == 0) return;
    t->mismatched++;
    if (t->printed < 12) {
        t->printed++;
        size_t expected_len = strlen(expected);
        size_t actual_len = strlen(actual);
        size_t at = 0;
        while (at < expected_len && at < actual_len && expected[at] == actual[at]) at++;
        size_t from = at > 60 ? at - 60 : 0;
        printf("  MISMATCH %s (first diff at %zu, lengths %zu/%zu)\n", label, at, expected_len,
               actual_len);
        printf("    expected: ...%.*s\n", 260, expected + from);
        printf("    actual:   ...%.*s\n", 260, actual + from);
    }
}

static void record_bool(tally *t, const char *label, bool equal, const char *expected,
                        const char *actual) {
    t->total++;
    if (equal) return;
    t->mismatched++;
    if (t->printed < 12) {
        t->printed++;
        printf("  MISMATCH %s\n    expected: %s\n    actual:   %s\n", label, expected, actual);
    }
}

static const char *hex_or_null(const rt_buf *buf) {
    /* Rotating buffers: call sites sometimes pass two results in one call. */
    static char scratch[4][65536];
    static unsigned next;
    char *out = scratch[next++ % 4];
    if (buf->data == NULL) {
        out[0] = '\0';
        return out;
    }
    size_t n = buf->len < 65535 ? buf->len : 65535;
    memcpy(out, buf->data, n);
    out[n] = '\0';
    return out;
}

/* ---- corpus replay ------------------------------------------------------- */

static bool case_missing_partial(yyjson_val *case_value) {
    yyjson_val *message = yyjson_obj_get(case_value, "presentation_raised_message");
    const char *text = message != NULL ? yyjson_get_str(message) : NULL;
    return text != NULL && strstr(text, "to_missing_attachable_partial_path") != NULL;
}

static void replay_case(corpus_world *world, yyjson_val *case_value, tally tallies[4],
                        size_t *security_checked, size_t *security_failed) {
    const char *name = yyjson_get_str(yyjson_obj_get(case_value, "name"));
    const char *body = yyjson_get_str(yyjson_obj_get(case_value, "body"));
    size_t body_len = yyjson_get_len(yyjson_obj_get(case_value, "body"));
    yyjson_val *host_value = yyjson_obj_get(case_value, "host");
    const char *host = host_value != NULL ? yyjson_get_str(host_value) : CORPUS_HOST_DEFAULT;
    rt_resolver *resolver = &world->resolver.base;

    /* presentation */
    yyjson_val *presentation = yyjson_obj_get(case_value, "presentation");
    bool presentation_error = yyjson_obj_get(presentation, "error") != NULL;
    rt_buf expected_presentation;
    rt_buf_init(&expected_presentation);
    const char *expected_text = NULL;
    if (!presentation_error) {
        yyjson_val *ok = yyjson_obj_get(presentation, "ok");
        const char *rails = ok != NULL ? yyjson_get_str(ok) : NULL;
        diverged_into(rails != NULL ? rails : "", &expected_presentation);
        expected_text = hex_or_null(&expected_presentation);
    }
    rt_buf actual_presentation;
    rt_buf_init(&actual_presentation);
    rt_presentation kind = RT_PRESENTATION_HTML;
    rt_status rc = rt_present_message(resolver, host, (const unsigned char *)body, body_len, &kind,
                                      &actual_presentation);
    char label[512];
    if (case_missing_partial(case_value)) {
        bool has_cross = actual_presentation.len != 0 &&
                         memmem(actual_presentation.data, actual_presentation.len, "\xe2\x98\x92", 3) != NULL;
        record_bool(&tallies[0], name, rc == RT_OK && kind == RT_PRESENTATION_HTML && has_cross,
                    "☒ mention placeholder", hex_or_null(&actual_presentation));
    } else if (presentation_error) {
        /* corpus.rs maps every expected presentation error to Unrenderable. */
        record_bool(&tallies[0], name, rc == RT_OK && kind == RT_PRESENTATION_UNRENDERABLE, "error",
                    "ok");
    } else {
        const char *actual = NULL;
        if (rc == RT_OK && kind == RT_PRESENTATION_HTML) actual = hex_or_null(&actual_presentation);
        else actual = kind == RT_PRESENTATION_BLANK ? "" : "<error>";
        snprintf(label, sizeof label, "[presentation] %s", name);
        record_mismatch(&tallies[0], label, expected_text != NULL ? expected_text : "", actual);
    }
    if (rc == RT_OK && kind == RT_PRESENTATION_HTML && actual_presentation.len != 0) {
        (*security_checked)++;
        if (!security_ok(actual_presentation.data, actual_presentation.len, false)) {
            (*security_failed)++;
            printf("    in case %s\n", name);
        }
    }
    rt_buf_dispose(&actual_presentation);
    rt_buf_dispose(&expected_presentation);

    /* plain text */
    yyjson_val *plain = yyjson_obj_get(case_value, "plain_text");
    bool plain_error = yyjson_obj_get(plain, "error") != NULL;
    rt_buf actual_plain;
    rt_buf_init(&actual_plain);
    rc = rt_to_plain_text(resolver, (const unsigned char *)body, body_len, &actual_plain);
    if (plain_error) {
        record_bool(&tallies[1], name, rc != RT_OK, "error", "ok");
    } else {
        yyjson_val *ok = yyjson_obj_get(plain, "ok");
        const char *expected = ok != NULL && yyjson_is_str(ok) ? yyjson_get_str(ok) : "";
        const char *actual = rc == RT_OK ? hex_or_null(&actual_plain) : "<error>";
        snprintf(label, sizeof label, "[plain_text] %s", name);
        record_mismatch(&tallies[1], label, expected, actual);
    }
    rt_buf_dispose(&actual_plain);

    /* editable */
    yyjson_val *editable = yyjson_obj_get(case_value, "editable");
    bool editable_error = yyjson_obj_get(editable, "error") != NULL;
    const char *editable_message = NULL;
    {
        yyjson_val *message = yyjson_obj_get(editable, "message");
        editable_message = message != NULL ? yyjson_get_str(message) : NULL;
    }
    bool missing_in_editor =
        editable_message != NULL && strstr(editable_message, "MissingAttachable") != NULL;
    rt_buf actual_editable;
    rt_buf_init(&actual_editable);
    bool found = false;
    rc = rt_editable_value(resolver, host, (const unsigned char *)body, body_len, &found,
                           &actual_editable);
    if (editable_error) {
        /* Rails raises; a missing attachable deliberately leaves the editor
         * instead (corpus.rs's missing_in_editor case). */
        bool divergence = missing_in_editor && rc == RT_OK;
        record_bool(&tallies[2], name, rc != RT_OK || divergence, "error", "ok");
    } else {
        yyjson_val *ok = yyjson_obj_get(editable, "ok");
        const char *expected = ok != NULL && yyjson_is_str(ok) ? yyjson_get_str(ok) : NULL;
        const char *actual = NULL;
        if (rc == RT_OK) actual = found ? hex_or_null(&actual_editable) : NULL;
        else actual = "<error>";
        snprintf(label, sizeof label, "[editable] %s", name);
        if (expected == NULL || actual == NULL) {
            record_bool(&tallies[2], name, expected == NULL && actual == NULL,
                        expected != NULL ? "ok" : "null", actual != NULL ? "ok" : "null");
        } else {
            record_mismatch(&tallies[2], label, expected, actual);
        }
    }
    rt_buf_dispose(&actual_editable);

    /* mentioned users */
    yyjson_val *mentioned = yyjson_obj_get(case_value, "mentioned");
    bool mentioned_error = yyjson_obj_get(mentioned, "error") != NULL;
    cf_int64_vector ids = {0};
    rc = rt_mentioned_users(resolver, (const unsigned char *)body, body_len, &ids);
    if (mentioned_error) {
        record_bool(&tallies[3], name, rc != RT_OK, "error", "ok");
    } else {
        yyjson_val *ok = yyjson_obj_get(mentioned, "ok");
        rt_buf expected_ids;
        rt_buf_init(&expected_ids);
        size_t count = ok != NULL ? yyjson_arr_size(ok) : 0;
        rt_buf_putc(&expected_ids, '[');
        for (size_t i = 0; i < count; i++) {
            if (i != 0) rt_buf_puts(&expected_ids, ", ");
            rt_buf_printf(&expected_ids, "%lld",
                          (long long)yyjson_get_sint(yyjson_arr_get(ok, i)));
        }
        rt_buf_putc(&expected_ids, ']');
        rt_buf actual_ids;
        rt_buf_init(&actual_ids);
        if (rc == RT_OK) {
            rt_buf_putc(&actual_ids, '[');
            for (size_t i = 0; i < ids.len; i++) {
                if (i != 0) rt_buf_puts(&actual_ids, ", ");
                rt_buf_printf(&actual_ids, "%lld", (long long)ids.items[i]);
            }
            rt_buf_putc(&actual_ids, ']');
        } else {
            rt_buf_puts(&actual_ids, "<error>");
        }
        snprintf(label, sizeof label, "[mentioned] %s", name);
        record_mismatch(&tallies[3], label, hex_or_null(&expected_ids), hex_or_null(&actual_ids));
        rt_buf_dispose(&expected_ids);
        rt_buf_dispose(&actual_ids);
    }
    cf_int64_vector_dispose(&ids);

}

/* ---- web_url vectors ----------------------------------------------------- */

static void replay_web_urls(corpus_world *world, yyjson_val *urls, tally *t) {
    size_t count = yyjson_arr_size(urls);
    for (size_t i = 0; i < count; i++) {
        yyjson_val *vector = yyjson_arr_get(urls, i);
        const char *value = yyjson_get_str(yyjson_obj_get(vector, "value"));
        const char *host = yyjson_get_str(yyjson_obj_get(vector, "host"));
        yyjson_val *result = yyjson_obj_get(vector, "result");
        bool expected_error = yyjson_obj_get(result, "error") != NULL;
        char *found_value = NULL;
        bool found = false;
        rt_status rc = rt_web_url((const unsigned char *)value, strlen(value), host, &found_value,
                                  &found);
        if (expected_error) {
            record_bool(t, value, rc != RT_OK, "error", "ok");
        } else {
            yyjson_val *ok = yyjson_obj_get(result, "ok");
            const char *expected = ok != NULL && yyjson_is_str(ok) ? yyjson_get_str(ok) : NULL;
            const char *actual = NULL;
            if (rc == RT_OK) actual = found ? found_value : NULL;
            else actual = "<error>";
            char label[512];
            snprintf(label, sizeof label, "[web_url] %s", value);
            if (expected == NULL || actual == NULL) {
                record_bool(t, label, expected == NULL && actual == NULL,
                            expected != NULL ? expected : "null", actual != NULL ? actual : "null");
            } else {
                record_mismatch(t, label, expected, actual);
            }
        }
        free(found_value);
        (void)world;
    }
}

/* ---- tests --------------------------------------------------------------- */

static yyjson_doc *corpus_doc;

CF_TEST(corpus_matches_the_rails_pipeline) {
    const char *path = getenv("RICHTEXT_CORPUS");
    if (path == NULL) path = CORPUS_DEFAULT_PATH;
    size_t len = 0;
    char *bytes = read_file(path, &len);
    CF_REQUIRE(bytes != NULL);
    corpus_doc = yyjson_read(bytes, len, YYJSON_READ_NOFLAG);
    CF_REQUIRE(corpus_doc != NULL);
    yyjson_val *root = yyjson_doc_get_root(corpus_doc);
    CF_REQUIRE(yyjson_is_obj(root));
    yyjson_val *users = yyjson_obj_get(root, "users");
    CF_REQUIRE(users != NULL);

    cf_test_clock_set_fixed_us(CORPUS_NOW_US);
    corpus_world world;
    world_open(&world, users);

    tally tallies[4];
    memset(tallies, 0, sizeof tallies);
    size_t security_checked = 0, security_failed = 0;

    yyjson_val *cases = yyjson_obj_get(root, "cases");
    size_t case_count = yyjson_arr_size(cases);
    for (size_t i = 0; i < case_count; i++) {
        replay_case(&world, yyjson_arr_get(cases, i), tallies, &security_checked, &security_failed);
    }
    tally web_urls;
    memset(&web_urls, 0, sizeof web_urls);
    yyjson_val *urls = yyjson_obj_get(root, "web_urls");
    if (urls != NULL) replay_web_urls(&world, urls, &web_urls);

    const char *names[5] = {"presentation", "plain_text", "editable", "mentioned", "web_url"};
    size_t total_mismatch = 0;
    for (size_t i = 0; i < 4; i++) {
        printf("%s: %zu cases, %zu mismatched\n", names[i], tallies[i].total, tallies[i].mismatched);
        total_mismatch += tallies[i].mismatched;
    }
    printf("%s: %zu cases, %zu mismatched\n", names[4], web_urls.total, web_urls.mismatched);
    total_mismatch += web_urls.mismatched;
    printf("web_url security: %zu outputs checked, %zu violations\n", security_checked,
           security_failed);

    /* The oracle's own outputs must pass the security assertions too. */
    size_t oracle_violations = 0;
    for (size_t i = 0; i < case_count; i++) {
        yyjson_val *case_value = yyjson_arr_get(cases, i);
        yyjson_val *presentation = yyjson_obj_get(case_value, "presentation");
        yyjson_val *ok = yyjson_obj_get(presentation, "ok");
        const char *rails = ok != NULL ? yyjson_get_str(ok) : NULL;
        if (rails == NULL) continue;
        rt_buf diverged;
        rt_buf_init(&diverged);
        diverged_into(rails, &diverged);
        if (!security_ok(diverged.data, diverged.len, false)) oracle_violations++;
        rt_buf_dispose(&diverged);
    }
    printf("oracle security: %zu violations\n", oracle_violations);
    CF_CHECK(oracle_violations == 0);

    CF_CHECK(total_mismatch == 0);
    CF_CHECK(security_failed == 0);

    world_close(&world);
    cf_test_clock_clear();
    yyjson_doc_free(corpus_doc);
    corpus_doc = NULL;
    free(bytes);
}

CF_TEST(security_assertions_catch_planted_defects) {
    struct {
        const char *html;
        const char *what;
    } cases[] = {
        {"<script>alert(1)</script>", "script element"},
        {"<p onclick=\"x()\">p</p>", "event handler"},
        {"<a href=\"java\tscript:alert(1)\">x</a>", "javascript URL"},
        {"<a href=\" JAVASCRIPT:alert(1)\">x</a>", "javascript URL"},
        {"<img src=\"data:text/html,<script>\">", "data URL"},
        {"<span style=\"color: red\">x</span>", "style attribute"},
        {"<svg><a xlink:href=\"javascript:1\">x</a></svg>", "svg"},
        {"<span data-controller=\"x\">x</span>", "data attribute"},
        {"<details>x</details>", "element outside the allowlist"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        rt_buf report;
        rt_buf_init(&report);
        security_violations_into((const unsigned char *)cases[i].html, strlen(cases[i].html), false,
                                 &report);
        CF_CHECK(report.len != 0); /* missed the planted defect otherwise */
        if (report.len == 0) printf("    missed %s\n", cases[i].what);
        rt_buf_dispose(&report);
    }
    const char *clean = "<p><a href=\"https://example.com\">x</a><img src=\"/a.png\"></p>";
    rt_buf report;
    rt_buf_init(&report);
    security_violations_into((const unsigned char *)clean, strlen(clean), false, &report);
    CF_CHECK(report.len == 0);
    rt_buf_dispose(&report);
}

CF_TEST_MAIN()
