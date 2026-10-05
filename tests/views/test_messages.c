/* A02 messages family: VIEW-03 goldens for messages#index/show/edit, the
 * message entry partials, boosts and the create/destroy turbo streams
 * (golden/b), plus the view-level cases the fixture comparison cannot
 * express.
 */
#include "cf_test.h"
#include "support/golden.h"
#include "support/golden_b.h"
#include "views.h"

#include <stdlib.h>
#include <string.h>

static cf_span lit(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* Render golden/b/<name> with a message model built from its input. */
static void message_matches(const char *name, int dom,
                            cf_err (*render)(const cf_view_ctx *,
                                             const cf_view_message *,
                                             cf_builder *)) {
    yyjson_doc *doc = cf_golden_b_load(name);
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_golden_b_ctx(&ctx, doc);
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input");
    cf_view_message message;
    cf_golden_b_message(input, &message);
    cf_builder out = {0};
    CF_REQUIRE(render(&ctx, &message, &out) == CF_OK);
    cf_golden_b_expect(doc, name, dom, out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_message_dispose(&message);
    yyjson_doc_free(doc);
}

/* Render golden/b/<name> with an edit model built from its input. */
static void edit_matches(const char *name) {
    yyjson_doc *doc = cf_golden_b_load(name);
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_golden_b_ctx(&ctx, doc);
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input");
    cf_view_message_edit_model edit;
    cf_golden_b_edit(input, &edit);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_message_edit(&ctx, &edit, &out) == CF_OK);
    cf_golden_b_expect(doc, name, 1, out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_message_edit_model_dispose(&edit);
    yyjson_doc_free(doc);
}

CF_TEST(messages_show_text_matches_golden) {
    message_matches("messages_show_text", 1, cf_view_message_show);
}

CF_TEST(messages_show_image_matches_golden) {
    message_matches("messages_show_image", 1, cf_view_message_show);
}

CF_TEST(messages_edit_text_matches_golden) {
    edit_matches("messages_edit_text");
}

CF_TEST(messages_edit_attachment_matches_golden) {
    edit_matches("messages_edit_attachment");
}

CF_TEST(messages_index_matches_golden) {
    yyjson_doc *doc = cf_golden_b_load("messages_index");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_golden_b_ctx(&ctx, doc);
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input");
    cf_view_message_item_vector items;
    cf_golden_b_message_items(cf_golden_b_input_at(input, "messages"), &items);
    CF_REQUIRE(items.len == 12);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_message_index(&ctx, items.items, items.len, &out) ==
               CF_OK);
    cf_golden_b_expect(doc, "messages_index", 0, out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_message_item_vector_dispose(&items);
    yyjson_doc_free(doc);
}

CF_TEST(messages_create_stream_matches_golden) {
    yyjson_doc *doc = cf_golden_b_load("messages_create");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_golden_b_ctx(&ctx, doc);
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input");
    cf_view_message_item item;
    cf_golden_b_message_item(cf_golden_b_input_at(input, "message"), &item);
    const char *kind =
        yyjson_get_str(cf_golden_b_input_at(input, "room_kind"));
    cf_room_type room_kind = kind != NULL && strcmp(kind, "closed") == 0
                                 ? CF_ROOM_CLOSED
                                 : (kind != NULL && strcmp(kind, "direct") == 0
                                        ? CF_ROOM_DIRECT
                                        : CF_ROOM_OPEN);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_message_create_stream(&ctx, &item, room_kind, &out) ==
               CF_OK);
    CF_CHECK(cf_builder_contains(
        &out, "<turbo-stream action=\"append\" "
              "target=\"messages_rooms_closed_654632876\"><template>"));
    cf_golden_b_expect(doc, "messages_create", 1, out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_message_item_dispose(&item);
    yyjson_doc_free(doc);
}

CF_TEST(messages_destroy_stream_matches_golden) {
    yyjson_doc *doc = cf_golden_b_load("messages_destroy");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_golden_b_ctx(&ctx, doc);
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input");
    cf_view_message message;
    cf_golden_b_message(input, &message);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_message_destroy_stream(&message, &out) == CF_OK);
    cf_golden_b_expect(doc, "messages_destroy", 1, out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_message_dispose(&message);
    yyjson_doc_free(doc);
}

CF_TEST(messages_room_not_found_matches_golden) {
    yyjson_doc *doc = cf_golden_b_load("messages_room_not_found");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_golden_b_ctx(&ctx, doc);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_message_room_not_found(&ctx, &out) == CF_OK);
    cf_golden_b_expect(doc, "messages_room_not_found", 1, out.ptr, out.len);
    cf_builder_dispose(&out);
    yyjson_doc_free(doc);
}

CF_TEST(messages_boosts_index_matches_golden) {
    message_matches("messages_boosts_index", 1, cf_view_boosts_index_frame);
}

CF_TEST(messages_boosts_new_matches_golden) {
    yyjson_doc *doc = cf_golden_b_load("messages_boosts_new");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_golden_b_ctx(&ctx, doc);
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input");
    cf_view_message message;
    cf_golden_b_message(cf_golden_b_input_at(input, "message"), &message);
    cf_view_user user;
    cf_golden_b_user(cf_golden_b_input_at(input, "user"), &user);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_new_boost(&ctx, &message, &user, &out) == CF_OK);
    cf_golden_b_expect(doc, "messages_boosts_new", 1, out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_user_dispose(&user);
    cf_view_message_dispose(&message);
    yyjson_doc_free(doc);
}

/* The presentation partial alone is what messages#update broadcasts. */
CF_TEST(messages_presentation_partial_is_the_presentation_div) {
    yyjson_doc *doc = cf_golden_b_load("messages_show_text");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_golden_b_ctx(&ctx, doc);
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input");
    cf_view_message message;
    cf_golden_b_message(input, &message);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_message_presentation(&ctx, &message, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(
        &out, "<div id=\"presentation_message_rich\" dir=\"auto\" "
              "data-reply-target=\"body\" data-messages-target=\"body\">"));
    CF_CHECK(cf_builder_contains(&out, "lexxy-content"));
    CF_CHECK(cf_builder_contains(
        &out, "data-copy-to-clipboard-url-value=") == 0);
    cf_builder_dispose(&out);
    cf_view_message_dispose(&message);
    yyjson_doc_free(doc);
}

/* A boost fragment goes out as the boost row, frames and all. */
CF_TEST(messages_boost_partial_is_the_boost_div) {
    yyjson_doc *doc = cf_golden_b_load("messages_boosts_index");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_golden_b_ctx(&ctx, doc);
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input");
    cf_view_message message;
    cf_golden_b_message(input, &message);
    CF_REQUIRE(message.boosts.len == 2);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_boost_partial(&ctx, &message.boosts.items[0], &out) ==
               CF_OK);
    CF_CHECK(cf_builder_contains(
        &out, "<div id=\"boost_329428236\""));
    CF_CHECK(cf_builder_contains(
        &out, "data-boost-delete-booster-id-value=\"712064548\""));
    CF_CHECK(cf_builder_contains(&out, "aria-label=\"Kevin boosted 🚀\""));
    CF_CHECK(cf_builder_contains(
        &out, "<form class=\"button_to\" method=\"post\" "
              "action=\"/messages/933434490/boosts/329428236\">"));
    cf_builder_dispose(&out);
    cf_view_message_dispose(&message);
    yyjson_doc_free(doc);
}

/* The fragment arm of a MessageItem goes out byte for byte. */
CF_TEST(messages_item_fragment_arm_goes_out_verbatim) {
    cf_view_message_item item = {0};
    item.is_fragment = true;
    item.fragment_html = (cf_str){(char *)"<p>cached</p>", 13};
    item.fragment_client_message_id = (cf_str){(char *)"x", 1};
    item.fragment_room_id = 1;
    cf_view_ctx ctx = {0};
    cf_builder out = {0};
    CF_REQUIRE(cf_view_message_item_partial(&ctx, &item, &out) == CF_OK);
    CF_CHECK(cf_builder_equals(&out, "<p>cached</p>"));
    cf_builder_dispose(&out);
}

/* A failing render leaves the caller's builder untouched (A02 conventions). */
CF_TEST(messages_render_failure_keeps_the_builder) {
    yyjson_doc *doc = cf_golden_b_load("messages_show_text");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_golden_b_ctx(&ctx, doc);
    ctx.asset_path = NULL; /* any image_tag render now fails CF_INVALID */
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input");
    cf_view_message message;
    cf_golden_b_message(input, &message);
    cf_builder out = {0};
    CF_REQUIRE(cf_builder_append(&out, lit("keep")) == CF_OK);
    CF_CHECK(cf_view_message_show(&ctx, &message, &out) == CF_INVALID);
    CF_CHECK(cf_builder_equals(&out, "keep"));
    cf_builder_dispose(&out);
    cf_view_message_dispose(&message);
    yyjson_doc_free(doc);
}

/* `MessagesHelper#message_tag`'s rescue arm: the whole message is replaced by
 * messages/_unrenderable.html, included at the parent template's own
 * indentation ("\n  ") and with no trailing newline (askama 0.14 renders the
 * include that way). */
CF_TEST(messages_unrenderable_partial_renders_the_reference_bytes) {
    cf_view_message message = {0};
    message.id = 7;
    message.room_id = 1;
    message.content_kind = CF_VIEW_CONTENT_UNRENDERABLE;
    cf_view_ctx ctx = {0};
    cf_builder out = {0};
    CF_REQUIRE(cf_view_message_partial(&ctx, &message, &out) == CF_OK);
    CF_CHECK(cf_builder_equals(
        &out,
        "\n  <div class=\"message message--formatted message--failed center\">\n"
        "  <div class=\"message__body\">\n"
        "    <div class=\"message__body-content txt-align-center\">\n"
        "      Failed to load message content\n"
        "    </div>\n"
        "  </div>\n"
        "</div>"));
    cf_builder_dispose(&out);
    cf_view_message_dispose(&message);
}

/* The boosts pages also render through turbo-rails' frame layout. */
CF_TEST(messages_boosts_frames_carry_the_content_only) {
    yyjson_doc *doc = cf_golden_b_load("messages_boosts_new");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_golden_b_ctx(&ctx, doc);
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input");
    cf_view_message message;
    cf_golden_b_message(cf_golden_b_input_at(input, "message"), &message);
    cf_view_user user;
    cf_golden_b_user(cf_golden_b_input_at(input, "user"), &user);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_new_boost_frame(&ctx, &message, &user, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "id=\"new_boost_message_emoji\""));
    CF_CHECK(cf_builder_contains(&out, "<nav id=\"nav\"") == 0);
    cf_builder_dispose(&out);
    CF_REQUIRE(cf_view_boosts_index_frame(&ctx, &message, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "id=\"boosting_message_emoji\""));
    CF_CHECK(cf_builder_contains(&out, "<aside id=\"sidebar\"") == 0);
    cf_builder_dispose(&out);
    cf_view_user_dispose(&user);
    cf_view_message_dispose(&message);
    yyjson_doc_free(doc);
}
