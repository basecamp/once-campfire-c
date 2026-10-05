/* src/presenters/searches.h — A-searches presenter surface.
 *
 * Sources: tmp/rust-ref/crates/campfire/src/controllers/searches.rs (`index`'s
 * set_messages / recent searches / last_room_visited block) and
 * tmp/rust-ref/crates/campfire/src/integrations/search.rs (`sanitize_query`),
 * the pinned port of reference/app/controllers/searches_controller.rb.
 *
 * Contract: presenters load rows and assemble the view models; rendering
 * performs no SQL.  The model type lives in views.h with the other view
 * models.
 */
#ifndef CF_PRESENTERS_SEARCHES_H
#define CF_PRESENTERS_SEARCHES_H

#include "views.h"

/* `integrations::search::sanitize_query`:
 * `params[:q]&.gsub(/[^[:word:]]/, " ")` — every code point Onigmo's
 * `[[:word:]]` does not match becomes one ASCII space (the generated Unicode
 * 15.0 range table below is the pinned word_ranges.rs).  `out` is owned.  An
 * invalid UTF-8 byte sequence is Ruby's `ArgumentError: invalid byte sequence
 * in UTF-8` (CF_INTERNAL), the same failure `gsub` raises. */
cf_err cf_searches_sanitize_query(cf_span q, cf_str *out);

/* Rust `is_present`: `!value.chars().all(char::is_whitespace)`.  The sanitized
 * query only ever holds ASCII spaces and word characters, on which this is
 * "some byte is not a space". */
bool cf_searches_query_present(cf_span value);

/* `searches#index`: `set_messages`, `Search::ordered_for_user` and
 * `last_room_visited_in` in one reader trip, then the message items through
 * the messages presenter.  `has_q`/`q` are the raw `params[:q]`
 * (`query_param`); the sanitized query and the searched messages are derived
 * here, exactly as `index` derives them before `present`. */
cf_err cf_presenter_searches_index(cf_ctx *ctx, const cf_user *user, bool has_q,
                                   cf_span q,
                                   cf_view_searches_index_model *out);

#endif /* CF_PRESENTERS_SEARCHES_H */
