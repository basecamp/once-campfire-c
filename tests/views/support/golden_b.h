/* A02 rooms/messages test support: the pinned golden/b comparison.
 *
 * Reference: tests/fixtures/crates/views/tests/messages_support/mod.rs (the
 * pinned copy of crates/views/tests/messages_support/mod.rs) plus the
 * golden/b JSON files written by reference-tools/views/b/run.sh:
 *
 *   - `tokens(html)` is that runner's own tokenizer (attributes in source
 *     order, names/keys lowercased, values entity-decoded, a Rails forgery
 *     token dropped, script/style raw text kept undecoded, text runs
 *     whitespace-collapsed and merged), NOT the parity_a dom.rs comparison;
 *   - the expected token array is masked exactly twice: the CSRF input/meta
 *     tokens are dropped (merging the text around them), and a copy-link
 *     button's absolute `data-copy-to-clipboard-content-value` becomes
 *     `data-copy-to-clipboard-url-value` with the path (Rust's intentional
 *     difference from Rails).  No other mask exists;
 *   - `assert_dom` compares the reference page's regions (title, head, nav,
 *     main minus footer, footer, sidebar); `assert_content` compares the
 *     whole fragment, or the page's main region against the whole render;
 *   - a fixture's input is deserialized into the C view models exactly as the
 *     runner's serde does, so the renderers are driven by reference data.
 *
 * CF_GOLDEN_DIR overrides the fixture directory; CF_GOLDEN_DUMP=<dir> writes
 * the actual render to <dir>/<name>.actual.html for out-of-band inspection.
 */
#ifndef CF_VIEWS_TEST_GOLDEN_B_H
#define CF_VIEWS_TEST_GOLDEN_B_H

#include "views.h"

#include "yyjson.h"

/* Load golden/b/<name>.json.  Returns an owned document (free with
 * yyjson_doc_free) or NULL with a message on stderr. */
yyjson_doc *cf_golden_b_load(const char *name);

/* Build the render context the reference request had: current user, account,
 * platform, base URL, last_room_visited_id and the fixture's asset map. */
void cf_golden_b_ctx(cf_view_ctx *out, yyjson_doc *doc);

/* Model builders from a fixture's "input" (owned; dispose with the matching
 * cf_view_*_dispose). */
void cf_golden_b_user(yyjson_val *object, cf_view_user *out);
void cf_golden_b_room(yyjson_val *object, cf_view_room *out);
void cf_golden_b_boost(yyjson_val *object, cf_view_boost *out);
void cf_golden_b_message(yyjson_val *object, cf_view_message *out);
void cf_golden_b_message_item(yyjson_val *object, cf_view_message_item *out);
void cf_golden_b_message_items(yyjson_val *array,
                               cf_view_message_item_vector *out);
void cf_golden_b_show(yyjson_val *input, cf_view_room_show_model *out);
void cf_golden_b_edit(yyjson_val *input, cf_view_message_edit_model *out);

/* `input[key]`. */
yyjson_val *cf_golden_b_input_at(yyjson_val *input, const char *key);

/* Compare `actual` with the fixture as the Rust runner does: `dom` selects
 * assert_dom (regions for a page) versus assert_content.  On mismatch prints
 * the first differing token and records a CF_CHECK failure. */
void cf_golden_b_expect(yyjson_doc *doc, const char *name, int dom,
                        const unsigned char *actual, size_t actual_len);

#endif /* CF_VIEWS_TEST_GOLDEN_B_H */
