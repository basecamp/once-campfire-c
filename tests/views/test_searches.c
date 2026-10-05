/* A-searches views: the Rust runner's two searches cases
 * (tmp/rust-ref/crates/views/tests/searches_views.rs: `index_with_results`
 * over golden/b/searches_index and `index_without_query` over
 * golden/b/searches_index_empty) plus the sanitizer and search_path vectors
 * pinned in searches.rs / integrations/search.rs.
 *
 * The fixture's `input` is deserialized into cf_view_searches_index_model the
 * way the runner's serde deserializes IndexView, and the render goes through
 * the fixture context (cf_golden_b_ctx) and the shared DOM comparator.  The
 * comparator's two masks are the only ones (CSRF tokens and the copy-link
 * URL); nothing search-specific is masked.
 */
#include "cf_test.h"
#include "support/golden_b.h"
#include "views.h"

#include "presenters/searches.h"

#include <stdlib.h>
#include <string.h>

static cf_span lit(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static bool contains(cf_span haystack, const char *needle) {
    size_t len = strlen(needle);
    if (len == 0) return true;
    if (haystack.len < len) return false;
    for (size_t i = 0; i + len <= haystack.len; i++) {
        if (memcmp(haystack.ptr + i, needle, len) == 0) return true;
    }
    return false;
}

/* Owned copy of a fixture string (the model disposer frees). */
static cf_str own(const char *text) {
    cf_str out = {0};
    if (text == NULL) return out;
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (copy == NULL) return out;
    memcpy(copy, text, len + 1);
    out.ptr = copy;
    out.len = len;
    return out;
}

/* Build IndexView from the fixture input exactly as serde does. */
static void searches_index_from_input(yyjson_val *input,
                                      cf_view_searches_index_model *out) {
    memset(out, 0, sizeof *out);
    yyjson_val *query = yyjson_obj_get(input, "query");
    if (yyjson_is_str(query)) {
        out->has_query = true;
        out->query = own(yyjson_get_str(query));
    }
    yyjson_val *q = yyjson_obj_get(input, "q");
    if (yyjson_is_str(q)) {
        out->has_q = true;
        out->q = own(yyjson_get_str(q));
    }
    cf_golden_b_message_items(yyjson_obj_get(input, "messages"),
                              &out->messages);
    yyjson_val *recents = yyjson_obj_get(input, "recent_searches");
    size_t count = yyjson_arr_size(recents);
    if (count != 0) {
        out->recent_searches.items =
            calloc(count, sizeof *out->recent_searches.items);
        CF_REQUIRE(out->recent_searches.items != NULL);
        out->recent_searches.cap = count;
        yyjson_val *item;
        yyjson_arr_iter iter = yyjson_arr_iter_with(recents);
        while ((item = yyjson_arr_iter_next(&iter)) != NULL) {
            const char *text = yyjson_get_str(item);
            CF_REQUIRE(text != NULL);
            out->recent_searches.items[out->recent_searches.len++] = own(text);
        }
    }
    out->return_to_room_id =
        yyjson_get_sint(yyjson_obj_get(input, "return_to_room_id"));
}

/* Render golden/b/<name> with its own input/context and assert_dom, the
 * runner's searches_views.rs cases. */
static void searches_index_matches(const char *name) {
    yyjson_doc *doc = cf_golden_b_load(name);
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_golden_b_ctx(&ctx, doc);
    cf_view_searches_index_model index;
    searches_index_from_input(yyjson_obj_get(yyjson_doc_get_root(doc), "input"),
                              &index);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_searches_index(&ctx, &index, &out) == CF_OK);
    cf_golden_b_expect(doc, name, 1, out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_searches_index_model_dispose(&index);
    yyjson_doc_free(doc);
}

CF_TEST(searches_index_with_results_matches_the_golden) {
    searches_index_matches("searches_index");
}

CF_TEST(searches_index_without_query_matches_the_golden) {
    searches_index_matches("searches_index_empty");
}

/* The frame render carries head + content only (blocks = ["head", "content"]):
 * the results area, no nav, no sidebar, no footer, no page title. */
CF_TEST(searches_index_frame_has_the_results_only) {
    yyjson_doc *doc = cf_golden_b_load("searches_index");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_golden_b_ctx(&ctx, doc);
    cf_view_searches_index_model index;
    searches_index_from_input(yyjson_obj_get(yyjson_doc_get_root(doc), "input"),
                              &index);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_searches_index_frame(&ctx, &index, &out) == CF_OK);
    CF_CHECK(contains((cf_span){out.ptr, out.len},
                      "data-search-results-target=\"messages\""));
    CF_CHECK(contains((cf_span){out.ptr, out.len}, "data-message-id=\"309456473\""));
    CF_CHECK(!contains((cf_span){out.ptr, out.len}, "<nav id=\"nav\""));
    CF_CHECK(!contains((cf_span){out.ptr, out.len}, "<aside id=\"sidebar\""));
    CF_CHECK(!contains((cf_span){out.ptr, out.len}, "searches__query"));
    cf_builder_dispose(&out);
    cf_view_searches_index_model_dispose(&index);
    yyjson_doc_free(doc);
}

/* searches.rs `query`: `params[:q]&.gsub(/[^[:word:]]/, " ")`. */
CF_TEST(searches_sanitize_query_replaces_non_word_characters) {
    cf_str out = {0};
    CF_REQUIRE(cf_searches_sanitize_query(lit("hello, world!"), &out) == CF_OK);
    CF_CHECK(out.len == 13 && memcmp(out.ptr, "hello  world ", 13) == 0);
    cf_str_dispose(&out);

    CF_REQUIRE(cf_searches_sanitize_query(lit("caf\xC3\xA9_1 \xE6\x97\xA5\xE6\x9C\xAC"),
                                          &out) == CF_OK);
    CF_CHECK(out.len == 14 &&
             memcmp(out.ptr, "caf\xC3\xA9_1 \xE6\x97\xA5\xE6\x9C\xAC", 14) == 0);
    cf_str_dispose(&out);

    CF_REQUIRE(cf_searches_sanitize_query(lit("\"quoted\" AND-x"), &out) ==
               CF_OK);
    CF_CHECK(out.len == 14 && memcmp(out.ptr, " quoted  AND x", 14) == 0);
    cf_str_dispose(&out);

    CF_REQUIRE(cf_searches_sanitize_query(lit(""), &out) == CF_OK);
    CF_CHECK(out.len == 0);
    cf_str_dispose(&out);
}

CF_TEST(searches_sanitize_query_rejects_invalid_utf8) {
    cf_str out = {0};
    CF_CHECK(cf_searches_sanitize_query(
                  (cf_span){(const unsigned char *)"a\xFF b", 4}, &out) ==
              CF_INTERNAL);
    CF_CHECK(out.ptr == NULL);
}

/* `is_present`: blank sanitized queries don't search. */
CF_TEST(searches_query_present_is_whitespace_aware) {
    CF_CHECK(!cf_searches_query_present(lit("")));
    CF_CHECK(!cf_searches_query_present(lit("   ")));
    CF_CHECK(!cf_searches_query_present(lit(" \t\n")));
    CF_CHECK(!cf_searches_query_present(lit("\xC2\xA0"))); /* NBSP */
    CF_CHECK(cf_searches_query_present(lit("a")));
    CF_CHECK(cf_searches_query_present(lit("  a  ")));
}

/* `searches::search_path`: CGI.escape's output. */
CF_TEST(searches_search_path_escapes_like_cgi_escape) {
    cf_builder out = {0};
    CF_REQUIRE(cf_view_searches_search_path(lit("pizza & \"pie\" *~"), &out) ==
               CF_OK);
    const char *want = "/searches?q=pizza+%26+%22pie%22+%2A~";
    CF_CHECK(out.len == strlen(want) &&
             memcmp(out.ptr, want, out.len) == 0);
    cf_builder_dispose(&out);

    /* The Rust action test's redirect target. */
    CF_REQUIRE(cf_view_searches_search_path(lit("hello  world"), &out) ==
               CF_OK);
    const char *want2 = "/searches?q=hello++world";
    CF_CHECK(out.len == strlen(want2) &&
             memcmp(out.ptr, want2, out.len) == 0);
    cf_builder_dispose(&out);
}

/* CF_TEST_MAIN lives in tests/views/support/test_main.c (the views bucket). */
