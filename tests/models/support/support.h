/* tests/models/support/support.h — test-only support doubles for the model
 * unit tests.
 *
 * These translation units complete the link of each tests/models test binary
 * for the not-yet-landed service boundaries and mirror the pinned Rust test
 * support:
 *
 *  - richtext.c: the `BasicRichText` stand-in
 *    (tmp/rust-ref/crates/db/src/testing.rs) for `cf_richtext_to_plain_text`,
 *    plus the `Mentions` test support of
 *    tmp/rust-ref/crates/db/src/tests/push_test.rs for
 *    `cf_richtext_mentioned_user_ids` (the test states the mentionees; the
 *    reference's BasicRichText reports no mentions) and the `Tx::rich_text`
 *    accessor `cf_tx_rich_text`.
 *  - password.c: `cf_password_verify` on bcrypt via the pinned libxcrypt
 *    static artifact (vendor/DEPS.json libxcrypt entry), matching
 *    tmp/rust-ref/crates/rails_compat/src/password.rs `verify` (only the first
 *    72 password bytes count; a malformed digest verifies false).
 *
 * These are test-support doubles, not production code: they live only under
 * tests/, are never added to src/ or the application library list, and the
 * production linking of these symbols stays with A01 (cf_password_verify) and
 * R02 (rich-text pipeline + transaction accessor) in their own phases.  Every
 * tests/models test binary is linked together with this directory.
 *
 * The rich-text double accepts any `const cf_richtext *` (the concrete R02
 * type is not part of the frozen D01 headers) and reads its behavior from the
 * test-controlled state below, exactly as the Rust `Mentions` test struct
 * carries its own list.
 */
#ifndef CF_TESTS_MODELS_SUPPORT_H
#define CF_TESTS_MODELS_SUPPORT_H

#include <stddef.h>
#include <stdint.h>

/* State the mentionees `cf_richtext_mentioned_user_ids` reports, in order.
 * Mirrors push_test.rs's `Mentions(Vec<i64>)`. Passing NULL (or len 0) clears
 * the list; the default is empty. The list is copied (up to 16 ids, the
 * tests' maximum). */
void cf_test_richtext_set_mentions(const int64_t *ids, size_t len);

#endif /* CF_TESTS_MODELS_SUPPORT_H */
