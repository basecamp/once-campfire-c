/* Test-only H03 route-table double (A00's tests; never linked into the
 * application). H03 owns the real ordered 177-row table and matcher; this
 * double implements the same cf_route_match_request/cf_route_match_dispose
 * and the proposed cf_route_action seam with a tiny finite grammar:
 *
 *   - literal segments,
 *   - `:name` captures one nonempty segment,
 *   - a terminal `*name` captures the remaining path,
 *   - an optional trailing `(.:format)` captures the extension as `format`.
 *
 * Matching is in registration order (first match wins), HEAD falls back to a
 * GET entry, duplicate and trailing slashes are normalized away, and path
 * captures are percent-decoded (a malformed escape or bad UTF-8 fails the
 * match attempt with CF_INVALID, as H03 specifies for bad path parameters).
 * `+` stays literal in path segments (it is form-decoded only in the query).
 */
#ifndef CF_TEST_ROUTE_DOUBLE_H
#define CF_TEST_ROUTE_DOUBLE_H

#include "cf.h"

#define CF_TEST_ROUTES_MAX 32

#define CF_TEST_SPAN(lit) \
    ((cf_span){(const unsigned char *)(lit), sizeof(lit) - 1})

/* Register one route row. Returns CF_LIMIT when full, CF_INVALID on bad
 * arguments. `action` may be NULL (cf_route_action then reports no action and
 * dispatch answers 404). */
cf_err cf_test_routes_add(const char *method, const char *pattern,
                          uint32_t id, cf_action_fn action);
void cf_test_routes_reset(void);

/* Number of successful matches (ordered-match observation). */
size_t cf_test_routes_matches(void);

/* Reset the ordered-match counter only. */
void cf_test_routes_reset_matches(void);

#endif /* CF_TEST_ROUTE_DOUBLE_H */
