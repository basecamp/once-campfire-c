/* R02 rich-text focused tests (VIEW-02 beyond the corpus):
 *  - the public cf_ surface over a fake request context (host header, reader);
 *  - mention resolution and attachment signed-ID resolution with DB fixtures
 *    (valid, wrong purpose, expired, tampered, deleted user);
 *  - bot/webhook HTML policy vs ordinary message policy;
 *  - content-attachment nesting cap;
 *  - error fallbacks (rescue-to-empty vs unrenderable vs edit-page raise);
 *  - stored-XSS inertness and the editor/plain-text representations.
 *
 * The corpus test (test_corpus.c) is the differential oracle; this file pins
 * the API shapes and the cases the reference corpus only covers indirectly.
 *
 * Build: see test_corpus.c (same objects and libraries).
 */
#include "cf_test.h"

#include "auth.h"
#include "core/testclock.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "models/types.h"
#include "richtext.h"
#include "richtext/internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RT_TEST_SECRET \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define RT_TEST_NOW_US INT64_C(1767225600000000)

static cf_span span_of(const char *text) {
    cf_span span = {(const unsigned char *)text, strlen(text)};
    return span;
}

/* ---- fixture world -------------------------------------------------------- */

typedef struct {
    char *dir;
    char path[CF_DB_TEST_PATH_CAP];
    cf_db *reader;
    cf_db *writer;
} rt_world;

static void rt_world_seed_user(cf_db *db, int64_t id, const char *name, const char *bio,
                               const char *updated_at) {
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(
                   handle,
                   "INSERT INTO users (id, name, bio, created_at, updated_at) VALUES (?,?,?,?,?)",
                   -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    if (bio != NULL) sqlite3_bind_text(stmt, 3, bio, -1, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, 3);
    sqlite3_bind_text(stmt, 4, updated_at, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, updated_at, -1, SQLITE_STATIC);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
}

static void rt_world_open(rt_world *world) {
    memset(world, 0, sizeof *world);
    world->dir = cf_db_test_dir();
    CF_REQUIRE(world->dir != NULL);
    cf_db_test_path(world->path, sizeof world->path, world->dir);
    CF_REQUIRE(cf_db_open(world->path, false, &world->writer) == CF_OK);
    rt_world_seed_user(world->writer, 1, "David", "Founder", "2024-01-02 03:04:05");
    rt_world_seed_user(world->writer, 2, "Jason", NULL, "2024-01-02 03:04:05");
    CF_REQUIRE(cf_db_open(world->path, true, &world->reader) == CF_OK);
    cf_richtext_configure(span_of(RT_TEST_SECRET));
}

static void rt_world_close(rt_world *world) {
    if (world->reader != NULL) cf_db_close(world->reader);
    if (world->writer != NULL) cf_db_close(world->writer);
    world->reader = NULL;
    world->writer = NULL;
    if (world->dir != NULL) {
        cf_db_test_cleanup(world->dir);
        free(world->dir);
        world->dir = NULL;
    }
}

static cf_ctx rt_ctx(cf_db *reader, cf_request *request, const char *host) {
    memset(request, 0, sizeof *request);
    request->headers[0].name = span_of("Host");
    request->headers[0].value = span_of(host);
    request->header_count = 1;
    cf_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.reader = reader;
    ctx.request = request;
    return ctx;
}

static bool str_contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

static bool buf_contains(const cf_str *value, const char *needle) {
    if (value->ptr == NULL) return false;
    return strstr(value->ptr, needle) != NULL;
}

/* Parses the rendered output and reports whether any element carries an
 * event-handler attribute (a live on* attribute, not escaped text). */
static bool has_event_handler_attr(const cf_safe_html *html) {
    if (html->bytes == NULL) return false;
    cf_span span = cf_buf_span(html->bytes);
    rt_dom dom;
    rt_dom_init(&dom);
    rt_node root = RT_NODE_NONE;
    bool has = false;
    if (rt_dom_parse_fragment(&dom, span.ptr, span.len, &root) == RT_OK) {
        rt_node_vec all = rt_dom_descendants(&dom, root);
        for (size_t i = 0; i < all.len && !has; i++) {
            rt_dom_node *node = &dom.nodes[all.items[i]];
            if (node->kind != RT_NODE_KIND_ELEMENT) continue;
            for (size_t a = 0; a < node->attrs_len; a++) {
                const char *name = node->attrs[a].name;
                if ((name[0] == 'o' || name[0] == 'O') && (name[1] == 'n' || name[1] == 'N')) {
                    has = true;
                }
            }
        }
        rt_node_vec_dispose(&all);
    }
    rt_dom_dispose(&dom);
    return has;
}

static bool html_contains(const cf_safe_html *html, const char *needle) {
    if (html->bytes == NULL) return false;
    cf_span span = cf_buf_span(html->bytes);
    if (span.len == 0) return false;
    char *copy = malloc(span.len + 1);
    CF_REQUIRE(copy != NULL);
    memcpy(copy, span.ptr, span.len);
    copy[span.len] = '\0';
    bool found = str_contains(copy, needle);
    free(copy);
    return found;
}

/* ---- signed ids for the fixture users ------------------------------------ */

static char *sgid_for(const char *gid_uri) {
    cf_str out = {0};
    CF_REQUIRE(cf_auth_sgid_generate_attachable(span_of(RT_TEST_SECRET), span_of(gid_uri), &out) ==
               CF_OK);
    return out.ptr;
}

static char *mention_body(cf_ctx *ctx, const char *sgid, const char *content_type) {
    (void)ctx;
    size_t need = strlen(sgid) + strlen(content_type) + 128;
    char *body = malloc(need);
    CF_REQUIRE(body != NULL);
    snprintf(body, need,
             "<div>Hey <action-text-attachment sgid=\"%s\" content-type=\"%s\"></action-text-attachment></div>",
             sgid, content_type);
    return body;
}

/* ---- public surface ------------------------------------------------------- */

CF_TEST(public_render_body_editable_plain_and_mentions) {
    cf_test_clock_set_fixed_us(RT_TEST_NOW_US);
    rt_world world;
    rt_world_open(&world);
    cf_request request;
    cf_ctx ctx = rt_ctx(world.reader, &request, "once.campfire.test:3000");

    /* Ordinary message: layout, filters and auto_link, with the host taken
     * from the Host header (the port is stripped). */
    const char *body = "<p>see http://example.com/a?b=1&amp;c=2</p>";
    cf_safe_html rendered = {0};
    CF_REQUIRE(cf_richtext_render(&ctx, span_of(body), &rendered) == CF_OK);
    CF_REQUIRE(rendered.bytes != NULL);
    CF_CHECK(html_contains(&rendered, "<div class=\"lexxy-content\">"));
    CF_CHECK(html_contains(&rendered, "<a target=\"_blank\" href=\"http://example.com/a?b=1&amp;c=2\">"));
    cf_safe_html_dispose(&rendered);

    /* The x.com unfurl host check sees the request host as Current.request_host. */
    char *self_sgid = sgid_for("gid://campfire/User/1");
    char *self_body = mention_body(&ctx, self_sgid, "application/vnd.campfire.mention");
    cf_safe_html mention = {0};
    CF_REQUIRE(cf_richtext_render(&ctx, (cf_span){(const unsigned char *)self_body, strlen(self_body)},
                                  &mention) == CF_OK);
    CF_CHECK(html_contains(&mention, "class=\"mention\""));
    CF_CHECK(html_contains(&mention, "href=\"/users/1\""));
    CF_CHECK(html_contains(&mention, "David"));
    cf_safe_html_dispose(&mention);
    free(self_body);
    free(self_sgid);

    /* Bot/webhook HTML: same layout, but no auto_link and the action_text
     * allowlist keeps style (the message policy's auto_link drops it). */
    const char *styled =
        "<div>www.example.com <span style=\"color: #f00; position: fixed\">x</span></div>";
    cf_safe_html bot = {0};
    CF_REQUIRE(cf_richtext_body_html(&ctx, span_of(styled), &bot) == CF_OK);
    CF_CHECK(html_contains(&bot, "<span style=\"color: #f00;\">x</span>"));
    CF_CHECK(!html_contains(&bot, "<a target=\"_blank\""));
    cf_safe_html_dispose(&bot);
    cf_safe_html message = {0};
    CF_REQUIRE(cf_richtext_render(&ctx, span_of(styled), &message) == CF_OK);
    CF_CHECK(html_contains(&message, "www.example.com"));
    CF_CHECK(!html_contains(&message, "position: fixed"));
    CF_CHECK(!html_contains(&message, "style="));
    cf_safe_html_dispose(&message);

    /* Editor value and plain text. */
    const char *editor_body = "<p>Hello <b>bold</b></p>";
    bool found = false;
    cf_str editable = {0};
    CF_REQUIRE(cf_richtext_editable(&ctx, span_of(editor_body), &found, &editable) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(buf_contains(&editable, "<p>Hello <b>bold</b></p>"));
    cf_str_dispose(&editable);
    cf_str plain = {0};
    CF_REQUIRE(cf_richtext_to_plain_text(world.reader, cf_tx_rich_text(NULL), span_of(editor_body),
                                         &plain) == CF_OK);
    CF_CHECK(buf_contains(&plain, "Hello bold"));
    cf_str_dispose(&plain);
    cf_str blank_plain = {0};
    CF_REQUIRE(cf_richtext_to_plain_text(world.reader, cf_tx_rich_text(NULL), span_of("   "),
                                         &blank_plain) == CF_OK);
    CF_CHECK(blank_plain.len == 0);
    cf_str_dispose(&blank_plain);

    /* A blank body has no editor value. */
    found = true;
    cf_str blank_editor = {0};
    CF_REQUIRE(cf_richtext_editable(&ctx, span_of("  "), &found, &blank_editor) == CF_OK);
    CF_CHECK(!found);
    cf_str_dispose(&blank_editor);

    /* The singleton is returned regardless of the transaction argument. */
    CF_CHECK(cf_tx_rich_text(NULL) != NULL);
    cf_tx *fake = (cf_tx *)&world;
    CF_CHECK(cf_tx_rich_text(fake) == cf_tx_rich_text(NULL));

    rt_world_close(&world);
    cf_test_clock_clear();
}

/* ---- mention and signed-id resolution ------------------------------------ */

CF_TEST(mention_resolution_with_db_fixtures) {
    cf_test_clock_set_fixed_us(RT_TEST_NOW_US);
    rt_world world;
    rt_world_open(&world);
    cf_request request;
    cf_ctx ctx = rt_ctx(world.reader, &request, "once.campfire.test");

    char *david = sgid_for("gid://campfire/User/1");
    char *body = mention_body(&ctx, david, "application/vnd.campfire.mention");

    cf_int64_vector ids = {0};
    CF_REQUIRE(cf_richtext_mentioned_user_ids(world.reader, cf_tx_rich_text(NULL),
                                              span_of(body), &ids) == CF_OK);
    CF_REQUIRE(ids.len == 1);
    CF_CHECK(ids.items[0] == 1);
    cf_int64_vector_dispose(&ids);

    cf_str plain = {0};
    CF_REQUIRE(cf_richtext_to_plain_text(world.reader, cf_tx_rich_text(NULL), span_of(body),
                                         &plain) == CF_OK);
    CF_CHECK(buf_contains(&plain, "@David"));
    cf_str_dispose(&plain);

    /* Wrong purpose: not a mention, but the unverified-GID fallback still
     * renders the user (Campfire's expired-SGID path). */
    cf_str wrong = {0};
    CF_REQUIRE(cf_auth_sgid_generate(span_of(RT_TEST_SECRET), span_of("gid://campfire/User/1"),
                                     span_of("something else"), false, 0, &wrong) == CF_OK);
    char *wrong_body = mention_body(&ctx, wrong.ptr, "application/vnd.campfire.mention");
    cf_int64_vector wrong_ids = {0};
    CF_REQUIRE(cf_richtext_mentioned_user_ids(world.reader, cf_tx_rich_text(NULL),
                                              span_of(wrong_body), &wrong_ids) == CF_OK);
    CF_CHECK(wrong_ids.len == 0);
    cf_int64_vector_dispose(&wrong_ids);
    cf_safe_html wrong_html = {0};
    CF_REQUIRE(cf_richtext_render(&ctx, span_of(wrong_body), &wrong_html) == CF_OK);
    CF_CHECK(html_contains(&wrong_html, "David"));
    cf_safe_html_dispose(&wrong_html);
    free(wrong_body);
    cf_str_dispose(&wrong);

    /* Expired: same fallback, still no mention. */
    cf_str expired = {0};
    CF_REQUIRE(cf_auth_sgid_generate(span_of(RT_TEST_SECRET), span_of("gid://campfire/User/2"),
                                     span_of("attachable"), true,
                                     RT_TEST_NOW_US - INT64_C(3600000000), &expired) == CF_OK);
    char *expired_body = mention_body(&ctx, expired.ptr, "application/vnd.campfire.mention");
    cf_int64_vector expired_ids = {0};
    CF_REQUIRE(cf_richtext_mentioned_user_ids(world.reader, cf_tx_rich_text(NULL),
                                              span_of(expired_body), &expired_ids) == CF_OK);
    CF_CHECK(expired_ids.len == 0);
    cf_int64_vector_dispose(&expired_ids);
    free(expired_body);
    cf_str_dispose(&expired);

    /* A deleted user's mention renders the reference placeholder. */
    char *deleted = sgid_for("gid://campfire/User/42");
    char *deleted_body = mention_body(&ctx, deleted, "application/vnd.campfire.mention");
    cf_safe_html deleted_html = {0};
    CF_REQUIRE(cf_richtext_render(&ctx, span_of(deleted_body), &deleted_html) == CF_OK);
    CF_CHECK(html_contains(&deleted_html, "☒"));
    cf_safe_html_dispose(&deleted_html);
    cf_int64_vector deleted_ids = {0};
    CF_REQUIRE(cf_richtext_mentioned_user_ids(world.reader, cf_tx_rich_text(NULL),
                                              span_of(deleted_body), &deleted_ids) == CF_OK);
    CF_CHECK(deleted_ids.len == 0);
    cf_int64_vector_dispose(&deleted_ids);
    free(deleted_body);
    free(deleted);

    /* Two mentions of the same user: once, in document order. */
    size_t doubled_len = strlen(body) * 2 + 64;
    char *doubled = malloc(doubled_len);
    CF_REQUIRE(doubled != NULL);
    snprintf(doubled, doubled_len, "%s%s", body, body);
    cf_int64_vector doubled_ids = {0};
    CF_REQUIRE(cf_richtext_mentioned_user_ids(world.reader, cf_tx_rich_text(NULL),
                                              span_of(doubled), &doubled_ids) == CF_OK);
    CF_REQUIRE(doubled_ids.len == 1);
    CF_CHECK(doubled_ids.items[0] == 1);
    cf_int64_vector_dispose(&doubled_ids);
    free(doubled);

    free(body);
    free(david);
    rt_world_close(&world);
    cf_test_clock_clear();
}

/* ---- nesting cap ---------------------------------------------------------- */

/* The content attribute escapes & and " exactly as test_helper.rb does. */
static char *escape_attr(const char *text) {
    size_t len = strlen(text);
    char *out = malloc(len * 6 + 1);
    CF_REQUIRE(out != NULL);
    size_t at = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '&') {
            memcpy(out + at, "&amp;", 5);
            at += 5;
        } else if (text[i] == '"') {
            memcpy(out + at, "&quot;", 6);
            at += 6;
        } else {
            out[at++] = text[i];
        }
    }
    out[at] = '\0';
    return out;
}

static char *nested_content_attachments(size_t levels) {
    char *body = strdup("");
    CF_REQUIRE(body != NULL);
    for (size_t level = levels; level >= 1; level--) {
        size_t inner_need = strlen(body) + 64;
        char *inner = malloc(inner_need);
        CF_REQUIRE(inner != NULL);
        snprintf(inner, inner_need, "<p>level %zu</p>%s", level, body);
        char *escaped = escape_attr(inner);
        free(inner);
        free(body);
        size_t need = strlen(escaped) + 160;
        body = malloc(need);
        CF_REQUIRE(body != NULL);
        snprintf(body, need,
                 "<action-text-attachment content-type=\"text/html\" content=\"%s\"></action-text-attachment>",
                 escaped);
        free(escaped);
    }
    return body;
}

CF_TEST(content_attachment_nesting_stops_at_eight) {
    rt_world world;
    rt_world_open(&world);
    cf_request request;
    cf_ctx ctx = rt_ctx(world.reader, &request, "once.campfire.test");
    char *body = nested_content_attachments(12);
    cf_safe_html html = {0};
    CF_REQUIRE(cf_richtext_render(&ctx, span_of(body), &html) == CF_OK);
    CF_CHECK(html_contains(&html, "level 1"));
    CF_CHECK(html_contains(&html, "level 8"));
    CF_CHECK(!html_contains(&html, "level 9"));
    CF_CHECK(!html_contains(&html, "level 12"));
    cf_safe_html_dispose(&html);
    free(body);
    rt_world_close(&world);
}

/* ---- error fallbacks ------------------------------------------------------ */

CF_TEST(error_fallbacks_match_the_reference) {
    rt_world world;
    rt_world_open(&world);
    cf_request request;
    cf_ctx ctx = rt_ctx(world.reader, &request, "once.campfire.test");

    /* Unrenderable: the rescue's logging raises again (invalid UTF-8 in a
     * JSON parse error). */
    const char *unrenderable = "<p>Before <action-text-attachment sgid=\"nope\"></action-text-attachment> after</p>";
    cf_safe_html out = {0};
    CF_CHECK(cf_richtext_render(&ctx, span_of(unrenderable), &out) == CF_INVALID);
    CF_CHECK(out.bytes == NULL);

    /* A plain raise (invalid base64) rescues to an empty rendering. */
    const char *raised = "<p><action-text-attachment sgid=\"!!!\" content-type=\"application/vnd.campfire.mention\"></action-text-attachment></p>";
    CF_CHECK(cf_richtext_render(&ctx, span_of(raised), &out) == CF_OK);
    CF_CHECK(out.bytes == NULL);
    cf_str text = {0};
    CF_CHECK(cf_richtext_to_plain_text(world.reader, cf_tx_rich_text(NULL), span_of(raised),
                                       &text) == CF_OK);
    CF_CHECK(text.len == 0);
    cf_str_dispose(&text);
    cf_int64_vector ids = {0};
    CF_CHECK(cf_richtext_mentioned_user_ids(world.reader, cf_tx_rich_text(NULL), span_of(raised),
                                            &ids) == CF_OK);
    CF_CHECK(ids.len == 0);
    cf_int64_vector_dispose(&ids);

    /* Parse limits are an error for plain text and mentions, not a partial
     * rendering. */
    char *deep = malloc(401 * 3 + 1);
    CF_REQUIRE(deep != NULL);
    for (size_t i = 0; i < 401; i++) memcpy(deep + i * 3, "<b>", 3);
    deep[401 * 3] = '\0';
    cf_str deep_text = {0};
    CF_CHECK(cf_richtext_to_plain_text(world.reader, cf_tx_rich_text(NULL), span_of(deep),
                                       &deep_text) == CF_OK);
    CF_CHECK(deep_text.len == 0);
    cf_str_dispose(&deep_text);
    free(deep);

    /* body_html swallows the reference errors to an empty body. */
    cf_safe_html body = {0};
    CF_CHECK(cf_richtext_body_html(&ctx, span_of(unrenderable), &body) == CF_OK);
    CF_CHECK(body.bytes == NULL || cf_buf_span(body.bytes).len == 0);
    cf_safe_html_dispose(&body);

    /* The edit page raises for a remote image (no attachable_content_type). */
    const char *remote = "<action-text-attachment content-type=\"image/png\" url=\"https://example.com/p.png\"></action-text-attachment>";
    bool found = false;
    cf_str editable = {0};
    CF_CHECK(cf_richtext_editable(&ctx, span_of(remote), &found, &editable) == CF_INVALID);
    CF_CHECK(!found);

    rt_world_close(&world);
}

/* `message_tag`'s plain-text evaluation: the crate-level to_plain_text the
 * presenter calls before rendering. An unloggable raise fails the page
 * (CF_INTERNAL -> HTTP 500); every other raise renders
 * messages/_unrenderable (CF_RICHTEXT_PLAIN_UNRENDERABLE); a success yields
 * the text the sound arm then scans. */
CF_TEST(plain_text_outcome_distinguishes_fail_from_unrenderable) {
    rt_world world;
    rt_world_open(&world);

    /* Invalid UTF-8 in the JSON parse error: logging it raises again, so the
     * whole page fails (the reference error is an unrescued ArgumentError). */
    const char *fail = "<p>Before <action-text-attachment sgid=\"nope\"></action-text-attachment> after</p>";
    cf_str text = {0};
    cf_richtext_plain_outcome outcome = CF_RICHTEXT_PLAIN_TEXT;
    CF_CHECK(cf_richtext_to_plain_text_outcome(world.reader, cf_tx_rich_text(NULL),
                                               span_of(fail), &text, &outcome) == CF_INTERNAL);
    CF_CHECK(text.ptr == NULL);

    /* Any loggable raise is rescued by message_tag: the unrenderable
     * partial, never the message body. */
    const char *invalid_base64 = "<p><action-text-attachment sgid=\"!!!\" content-type=\"application/vnd.campfire.mention\"></action-text-attachment></p>";
    CF_CHECK(cf_richtext_to_plain_text_outcome(world.reader, cf_tx_rich_text(NULL),
                                               span_of(invalid_base64), &text, &outcome) == CF_OK);
    CF_CHECK(outcome == CF_RICHTEXT_PLAIN_UNRENDERABLE);
    CF_CHECK(text.ptr == NULL);

    /* A JSON::ParserError whose message is valid UTF-8 ("hello" decodes to
     * valid UTF-8, valid base64, invalid JSON) is loggable: unrenderable. */
    const char *valid_utf8 = "<p><action-text-attachment sgid=\"aGVsbG8=\"></action-text-attachment></p>";
    CF_CHECK(cf_richtext_to_plain_text_outcome(world.reader, cf_tx_rich_text(NULL),
                                               span_of(valid_utf8), &text, &outcome) == CF_OK);
    CF_CHECK(outcome == CF_RICHTEXT_PLAIN_UNRENDERABLE);
    CF_CHECK(text.ptr == NULL);

    /* Parse limits raise Nokogiri errors: loggable, so unrenderable. */
    char *deep = malloc(401 * 3 + 1);
    CF_REQUIRE(deep != NULL);
    for (size_t i = 0; i < 401; i++) memcpy(deep + i * 3, "<b>", 3);
    deep[401 * 3] = '\0';
    CF_CHECK(cf_richtext_to_plain_text_outcome(world.reader, cf_tx_rich_text(NULL), span_of(deep),
                                               &text, &outcome) == CF_OK);
    CF_CHECK(outcome == CF_RICHTEXT_PLAIN_UNRENDERABLE);
    CF_CHECK(text.ptr == NULL);
    free(deep);

    /* Success: the text, and an empty body is a successful empty string. */
    const char *ok = "<p>Hello <b>there</b></p>";
    CF_CHECK(cf_richtext_to_plain_text_outcome(world.reader, cf_tx_rich_text(NULL), span_of(ok),
                                               &text, &outcome) == CF_OK);
    CF_CHECK(outcome == CF_RICHTEXT_PLAIN_TEXT);
    CF_CHECK(text.ptr != NULL && strcmp(text.ptr, "Hello there") == 0);
    cf_str_dispose(&text);
    CF_CHECK(cf_richtext_to_plain_text_outcome(world.reader, cf_tx_rich_text(NULL), span_of(""),
                                               &text, &outcome) == CF_OK);
    CF_CHECK(outcome == CF_RICHTEXT_PLAIN_TEXT);
    CF_CHECK(text.ptr != NULL && text.len == 0);
    cf_str_dispose(&text);
    CF_CHECK(cf_richtext_to_plain_text_outcome(world.reader, cf_tx_rich_text(NULL),
                                               (cf_span){NULL, 0}, &text, &outcome) == CF_OK);
    CF_CHECK(outcome == CF_RICHTEXT_PLAIN_TEXT);
    CF_CHECK(text.ptr != NULL && text.len == 0);
    cf_str_dispose(&text);

    rt_world_close(&world);
}

/* ---- stored XSS inertness -------------------------------------------------- */

CF_TEST(stored_xss_stays_inert) {
    rt_world world;
    rt_world_open(&world);
    cf_request request;
    cf_ctx ctx = rt_ctx(world.reader, &request, "once.campfire.test");
    const char *attacks[] = {
        "<script>alert(1)</script>",
        "<img src=x onerror=alert(1)>",
        "<a href=\"javascript:alert(1)\">x</a>",
        "<svg onload=alert(1)>",
        "<math><mtext><table><mglyph><style><img src=x onerror=alert(1)></style></mglyph></table></mtext></math>",
        "<p title=\"x> http://evil.test/ <img src=x onerror=alert(1)>\">hi</p>",
        "<p><a name=\"body\" href=\"/x\">x</a><span name=\"cookie\">y</span></p>",
        "<iframe src=\"javascript:alert(1)\"></iframe>",
    };
    for (size_t i = 0; i < sizeof attacks / sizeof attacks[0]; i++) {
        cf_safe_html html = {0};
        CF_REQUIRE(cf_richtext_render(&ctx, span_of(attacks[i]), &html) == CF_OK);
        CF_CHECK(!html_contains(&html, "<script"));
        CF_CHECK(!has_event_handler_attr(&html));
        CF_CHECK(!html_contains(&html, "<iframe"));
        CF_CHECK(!html_contains(&html, "name=\""));
        CF_CHECK(!html_contains(&html, "<img src=x"));
        cf_safe_html_dispose(&html);
    }
    rt_world_close(&world);
}

/* D1 repair: body_html re-renders an attachment in the namespace of its
 * context element (dom.rs context_for carries the full QualName). A foreign
 * (SVG/MathML) <select> must not be re-parsed as an HTML select: the HTML
 * "in select" insertion mode drops the rendered mention span, the reference's
 * foreign context keeps it. */
CF_TEST(body_html_keeps_attachments_inside_foreign_elements) {
    cf_test_clock_set_fixed_us(RT_TEST_NOW_US);
    rt_world world;
    rt_world_open(&world);
    cf_request request;
    cf_ctx ctx = rt_ctx(world.reader, &request, "once.campfire.test");
    char *david = sgid_for("gid://campfire/User/1");
    const char *wrappers[] = {"math", "svg"};
    for (size_t i = 0; i < sizeof wrappers / sizeof wrappers[0]; i++) {
        size_t need = strlen(david) + 256;
        char *body = malloc(need);
        CF_REQUIRE(body != NULL);
        snprintf(body, need,
                 "<%s><select><action-text-attachment sgid=\"%s\" "
                 "content-type=\"application/vnd.campfire.mention\">"
                 "</action-text-attachment></select></%s>",
                 wrappers[i], david, wrappers[i]);
        cf_safe_html html = {0};
        CF_REQUIRE(cf_richtext_body_html(&ctx, span_of(body), &html) == CF_OK);
        CF_REQUIRE(html.bytes != NULL);
        CF_CHECK(html_contains(&html, "class=\"mention\""));
        CF_CHECK(html_contains(&html, "David"));
        cf_safe_html_dispose(&html);
        free(body);
    }
    free(david);
    rt_world_close(&world);
    cf_test_clock_clear();
}

/* Residual R02 divergence: the pinned Rust reference's TreeSink never reports
 * a MathML annotation-xml as an HTML integration point (markup5ever's default
 * is false), so encoding="text/html" (ASCII case-insensitive, like
 * "application/xhtml+xml") keeps its children in foreign content. Gumbo
 * applies the HTML5 rule, so rt_dom renames that attribute aside for the parse
 * and restores it in the arena. SVG annotation-xml stays foreign in both
 * engines, and svg foreignObject -- a real HTML integration point in both --
 * keeps the HTML5 behavior (the attachment is dropped by the HTML "in select"
 * mode). */
CF_TEST(body_html_ignores_annotation_xml_integration_points) {
    cf_test_clock_set_fixed_us(RT_TEST_NOW_US);
    rt_world world;
    rt_world_open(&world);
    cf_request request;
    cf_ctx ctx = rt_ctx(world.reader, &request, "once.campfire.test");
    char *david = sgid_for("gid://campfire/User/1");
    struct {
        const char *prefix;
        const char *suffix;
        bool keeps_mention;
    } cases[] = {
        {"<math><annotation-xml encoding=\"text/html\"><select>",
         "</select></annotation-xml></math>", true},
        {"<math><annotation-xml EnCoDiNg=\"TeXt/HtMl\"><select>",
         "</select></annotation-xml></math>", true},
        {"<math><annotation-xml encoding=\"application/xhtml+xml\"><select>",
         "</select></annotation-xml></math>", true},
        {"<svg><annotation-xml encoding=\"text/html\"><select>",
         "</select></annotation-xml></svg>", true},
        {"<svg><foreignObject><select>",
         "</select></foreignObject></svg>", false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        size_t need = strlen(cases[i].prefix) + strlen(cases[i].suffix) + strlen(david) + 128;
        char *body = malloc(need);
        CF_REQUIRE(body != NULL);
        snprintf(body, need,
                 "%s<action-text-attachment sgid=\"%s\"></action-text-attachment>%s",
                 cases[i].prefix, david, cases[i].suffix);

        cf_safe_html html = {0};
        CF_REQUIRE(cf_richtext_body_html(&ctx, span_of(body), &html) == CF_OK);
        CF_REQUIRE(html.bytes != NULL);
        CF_CHECK(html_contains(&html, "class=\"mention\"") == cases[i].keeps_mention);
        CF_CHECK(html_contains(&html, "David") == cases[i].keeps_mention);

        cf_int64_vector ids = {0};
        CF_REQUIRE(cf_richtext_mentioned_user_ids(world.reader, cf_tx_rich_text(NULL),
                                                  span_of(body), &ids) == CF_OK);
        CF_CHECK(ids.len == (cases[i].keeps_mention ? 1u : 0u));
        if (ids.len == 1) CF_CHECK(ids.items[0] == 1);
        cf_int64_vector_dispose(&ids);

        cf_safe_html_dispose(&html);
        free(body);
    }
    free(david);
    rt_world_close(&world);
    cf_test_clock_clear();
}

/* Residual R02 divergence (namespace-aware raw text): the scanner used to
 * treat every `<title>` as raw text, including an SVG-namespace `<title>`.
 * `<svg><title>` is an HTML integration point, but the tokenizer is not
 * switched to RCDATA for it, so Gumbo keeps parsing markup inside: a
 * `<math><annotation-xml encoding="text/html">` nested there is real markup
 * and its rename must happen, or Gumbo applies the integration point the
 * reference does not have (see the test above) and drops the mention. HTML
 * `<title>` stays raw text: an encoding-looking string inside it is text,
 * never an attribute to rename (a rename there would leak the internal
 * neutral attribute name into the rendered text). */
CF_TEST(foreign_titles_are_scanned_as_markup) {
    cf_test_clock_set_fixed_us(RT_TEST_NOW_US);
    rt_world world;
    rt_world_open(&world);
    cf_request request;
    cf_ctx ctx = rt_ctx(world.reader, &request, "once.campfire.test");
    char *david = sgid_for("gid://campfire/User/1");

    /* The verified defect shape: the mention inside the foreign-content title
     * is parsed and kept like the Rust reference. The sanitizer drops the SVG
     * title element itself (so the presentation is empty in both engines),
     * but plain text, the editor value and the mentioned ids all retain it. */
    size_t need = strlen(david) + 512;
    char *body = malloc(need);
    CF_REQUIRE(body != NULL);
    snprintf(body, need,
             "<svg><title><math><annotation-xml encoding=\"text/html\"><select>"
             "<action-text-attachment sgid=\"%s\" "
             "content-type=\"application/vnd.campfire.mention\">"
             "</action-text-attachment></select></annotation-xml></math></title></svg>",
             david);
    cf_str plain = {0};
    CF_REQUIRE(cf_richtext_to_plain_text(world.reader, cf_tx_rich_text(NULL), span_of(body),
                                         &plain) == CF_OK);
    CF_CHECK(buf_contains(&plain, "@David"));
    cf_str_dispose(&plain);
    bool found = false;
    cf_str editable = {0};
    CF_REQUIRE(cf_richtext_editable(&ctx, span_of(body), &found, &editable) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(buf_contains(&editable, "content-type=\"application/vnd.campfire.mention\""));
    CF_CHECK(buf_contains(&editable, "David"));
    cf_str_dispose(&editable);
    cf_int64_vector ids = {0};
    CF_REQUIRE(cf_richtext_mentioned_user_ids(world.reader, cf_tx_rich_text(NULL), span_of(body),
                                              &ids) == CF_OK);
    CF_CHECK(ids.len == 1);
    if (ids.len == 1) CF_CHECK(ids.items[0] == 1);
    cf_int64_vector_dispose(&ids);
    free(body);

    /* HTML `<title>` is RCDATA: the same construction inside it stays text,
     * so the mention is never resolved. */
    need = strlen(david) + 512;
    body = malloc(need);
    CF_REQUIRE(body != NULL);
    snprintf(body, need,
             "<title><math><annotation-xml encoding=\"text/html\"><select>"
             "<action-text-attachment sgid=\"%s\" "
             "content-type=\"application/vnd.campfire.mention\">"
             "</action-text-attachment></select></annotation-xml></math></title>",
             david);
    cf_safe_html raw = {0};
    CF_REQUIRE(cf_richtext_body_html(&ctx, span_of(body), &raw) == CF_OK);
    CF_REQUIRE(raw.bytes != NULL);
    CF_CHECK(!html_contains(&raw, "class=\"mention\""));
    cf_safe_html_dispose(&raw);
    cf_int64_vector html_ids = {0};
    CF_REQUIRE(cf_richtext_mentioned_user_ids(world.reader, cf_tx_rich_text(NULL), span_of(body),
                                              &html_ids) == CF_OK);
    CF_CHECK(html_ids.len == 0);
    cf_int64_vector_dispose(&html_ids);
    free(body);

    /* An encoding-looking string in an HTML title is text, not an attribute:
     * the raw-text scan must leave it byte-for-byte alone. */
    cf_str title_text = {0};
    CF_REQUIRE(cf_richtext_to_plain_text(world.reader, cf_tx_rich_text(NULL),
                                         span_of("<title><p encoding=\"x\">HELLO</p></title>"),
                                         &title_text) == CF_OK);
    CF_CHECK(buf_contains(&title_text, "<p encoding=\"x\">HELLO</p>"));
    CF_CHECK(!buf_contains(&title_text, "campfire-rt-enc-ni"));
    cf_str_dispose(&title_text);

    /* SVG-title plain text is text too: nothing is renamed and the text is
     * unchanged. */
    cf_str svg_plain = {0};
    CF_REQUIRE(cf_richtext_to_plain_text(
                   world.reader, cf_tx_rich_text(NULL),
                   span_of("<svg><title>plain encoding=\"x\" text</title></svg>"),
                   &svg_plain) == CF_OK);
    CF_CHECK(buf_contains(&svg_plain, "plain encoding=\"x\" text"));
    CF_CHECK(!buf_contains(&svg_plain, "campfire-rt-enc-ni"));
    cf_str_dispose(&svg_plain);

    free(david);
    rt_world_close(&world);
    cf_test_clock_clear();
}

CF_TEST(opengraph_embeds_use_the_request_host) {
    rt_world world;
    rt_world_open(&world);
    cf_request request;
    cf_ctx ctx = rt_ctx(world.reader, &request, "once.campfire.test");
    const char *body =
        "<action-text-attachment content-type=\"application/vnd.actiontext.opengraph-embed\" "
        "href=\"https://example.com/page\" filename=\"Example\" url=\"https://example.com/i.png\"></action-text-attachment>";
    cf_safe_html other = {0};
    CF_REQUIRE(cf_richtext_render(&ctx, span_of(body), &other) == CF_OK);
    CF_CHECK(html_contains(&other, "https://example.com/page"));
    cf_safe_html_dispose(&other);
    /* The same embed pointing at this Campfire is dropped by web_url. */
    const char *self =
        "<action-text-attachment content-type=\"application/vnd.actiontext.opengraph-embed\" "
        "href=\"https://once.campfire.test/page\" filename=\"Example\" url=\"https://once.campfire.test/i.png\"></action-text-attachment>";
    cf_safe_html local = {0};
    CF_REQUIRE(cf_richtext_render(&ctx, span_of(self), &local) == CF_OK);
    CF_CHECK(!html_contains(&local, "once.campfire.test"));
    CF_CHECK(html_contains(&local, "Example"));
    cf_safe_html_dispose(&local);
    rt_world_close(&world);
}

CF_TEST(unconfigured_pipeline_resolves_no_verified_mentions) {
    cf_test_clock_set_fixed_us(RT_TEST_NOW_US);
    rt_world world;
    rt_world_open(&world);
    cf_request request;
    cf_ctx ctx = rt_ctx(world.reader, &request, "once.campfire.test");
    char *david = sgid_for("gid://campfire/User/1");
    char *body = mention_body(&ctx, david, "application/vnd.campfire.mention");
    cf_span empty = {0};
    cf_richtext_configure(empty); /* no key material: unverified only */
    cf_int64_vector ids = {0};
    CF_REQUIRE(cf_richtext_mentioned_user_ids(world.reader, cf_tx_rich_text(NULL), span_of(body),
                                              &ids) == CF_OK);
    CF_CHECK(ids.len == 0);
    cf_int64_vector_dispose(&ids);
    /* The presentation fallback still finds the user through the GID. */
    cf_safe_html html = {0};
    CF_REQUIRE(cf_richtext_render(&ctx, span_of(body), &html) == CF_OK);
    CF_CHECK(html_contains(&html, "David"));
    cf_safe_html_dispose(&html);
    cf_richtext_configure(span_of(RT_TEST_SECRET));
    free(body);
    free(david);
    rt_world_close(&world);
    cf_test_clock_clear();
}

/* ---- editor representation ------------------------------------------------- */

CF_TEST(editor_representation_restores_mentions) {
    rt_world world;
    rt_world_open(&world);
    cf_request request;
    cf_ctx ctx = rt_ctx(world.reader, &request, "once.campfire.test");
    char *david = sgid_for("gid://campfire/User/1");
    size_t need = strlen(david) + 256;
    char *body = malloc(need);
    CF_REQUIRE(body != NULL);
    /* A mention edited under Trix: content-type was overwritten. */
    snprintf(body, need,
             "<div>Hey <action-text-attachment sgid=\"%s\" content-type=\"application/octet-stream\"></action-text-attachment></div>",
             david);
    bool found = false;
    cf_str editable = {0};
    CF_REQUIRE(cf_richtext_editable(&ctx, span_of(body), &found, &editable) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(buf_contains(&editable, "content-type=\"application/vnd.campfire.mention\""));
    CF_CHECK(buf_contains(&editable, "David"));
    cf_str_dispose(&editable);
    free(body);
    free(david);
    rt_world_close(&world);
}

CF_TEST(webhook_plain_text_drops_recipient_mentions) {
    cf_str out = {0};
    CF_REQUIRE(cf_richtext_without_recipient_mentions(span_of("@David hello"), span_of("David"),
                                                      &out) == CF_OK);
    CF_CHECK(buf_contains(&out, "hello"));
    CF_CHECK(!buf_contains(&out, "@David"));
    cf_str_dispose(&out);
}

CF_TEST_MAIN()
