/* A02 test support: reads the pinned golden facts
 * (tests/fixtures/crates/views/tests/golden/a/facts.json, written by the
 * reference's reference-tools/views/a/render.rb) and builds the cf_view_ctx
 * the case was rendered with, exactly as the Rust runner's support/facts.rs
 * does.  Borrowed strings point into the parsed document, which lives for
 * the process; a test must not free it.
 */
#ifndef CF_VIEWS_TEST_FACTS_H
#define CF_VIEWS_TEST_FACTS_H

#include "views.h"

#include "yyjson.h"

/* Parse facts.json once (CF_GOLDEN_DIR overrides the directory).  Returns
 * false with a message on stderr when the fixture tree is missing. */
int cf_facts_load(void);

/* One case object, or NULL. */
yyjson_val *cf_facts_case(const char *name);

/* A case field as a C string; "" when absent/null. */
const char *cf_facts_str(yyjson_val *object, const char *key);
int64_t cf_facts_i64(yyjson_val *object, const char *key, int64_t fallback);
bool cf_facts_bool(yyjson_val *object, const char *key);

/* The user object named `name` inside a case's "users". */
yyjson_val *cf_facts_user(yyjson_val *case_obj, const char *name);

/* Fill a render context for `case_name`, with the request facts the Rust
 * runner passes per case: `flash_notice`/`flash_alert` (NULL for absent).
 * Returns 1 on success. */
int cf_facts_view_ctx(cf_view_ctx *out, const char *case_name,
                      const char *flash_notice, const char *flash_alert);

/* Render `case_name`'s account name (convenience for assertions). */
const char *cf_facts_account_name(const char *case_name);

/* One-time test setup: parse facts.json and load the pinned asset manifest.
 * Returns 1 on success; a failure is a missing fixture/prerequisite, never a
 * skipped case. */
int cf_test_views_setup(void);

/* Fill `ctx`'s asset fields from the configured asset module (the default
 * resolver and tags cf_view_ctx_init would set). */
void cf_test_views_assets_ctx(cf_view_ctx *ctx);

#endif /* CF_VIEWS_TEST_FACTS_H */
