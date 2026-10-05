/* src/auth/user_agent.h — module-private port of the useragent gem (0.16.11)
 * surface the application platform needs (task A01 completion).
 *
 * Reference: tmp/rust-ref/crates/campfire/src/concerns/user_agent.rs, itself a
 * port of the gem whose vectors the differential corpus pins
 * (tmp/rust-ref/vectors/campfire_user_agents.json, copied to
 * tests/fixtures/ua/).  Nothing here is public API: the only external surface
 * is src/auth/platform.h.
 *
 * Everything the parse returns borrows the User-Agent span it was given (or a
 * string literal); the agent owns only its product/comment arrays, released by
 * cf_ua_dispose.  Query results that the gem builds by formatting (normalize_os
 * prefixing "OS X"/"iOS"/"ChromeOS", PlayStation comments joined) are written
 * into a caller-provided cf_ua_buf and marked !borrowed.
 */
#ifndef CF_AUTH_USER_AGENT_H
#define CF_AUTH_USER_AGENT_H

#include "cf.h"

/* One parsed product (UserAgent::Product).  `comment_raw` is the text between
 * the parentheses when a (...) group was consumed; `comments` is
 * ruby_split(comment_raw, "; ") with trailing empty fields dropped. */
typedef struct {
    cf_span product;
    cf_span version; /* raw version text (may be empty) */
    cf_span comment_raw;
    bool has_comment;
    cf_span *comments;
    size_t comment_count;
} cf_ua_product;

typedef struct {
    cf_ua_product *products;
    size_t count, cap;
    int kind; /* ua_kind, set by cf_ua_parse after the products exist */
} cf_ua_agent;

/* What a query answered. */
typedef enum {
    CF_UA_ABSENT = 0, /* Ok(None) / nil */
    CF_UA_SOME,       /* Ok(Some(_)) */
    CF_UA_RAISED      /* Err(Raised): the gem raised (the view answers false/"") */
} cf_ua_result;

/* A Version value: text plus the gem's `blank`/`comparable` flags. */
typedef struct {
    cf_span text;
    bool comparable;
    bool blank; /* Version#nil? (the text is empty or all Ruby space) */
} cf_ua_version;

/* A version query: found is Option::is_some, raised is Err, borrowed tells
 * whether version.text points at the UA/literal or at the caller's scratch. */
typedef struct {
    cf_ua_version version;
    bool found;
    bool raised;
    bool borrowed;
} cf_ua_version_result;

/* Caller-owned scratch for the queries that synthesize text.  Start it from a
 * stack array: cf_ua_buf b = {buf, 0, sizeof buf, false}; on overflow the
 * using query falls back to the borrowed raw text (see each function). */
typedef struct {
    char *ptr;
    size_t len, cap;
    bool overflow;
} cf_ua_buf;

/* Parse a User-Agent (blank strings parse as "Mozilla/4.0 (compatible)", like
 * UserAgent.parse).  Never fails on content: CF_NOMEM only. */
cf_err cf_ua_parse(cf_span user_agent, cf_ua_agent *out);
void cf_ua_dispose(cf_ua_agent *agent);

/* Agent#browser (nil -> CF_UA_ABSENT).  Text borrows the UA or a literal. */
cf_ua_result cf_ua_browser(const cf_ua_agent *agent, cf_span *out);

/* Agent#version (nil/Err mapped to {.text = ""}).  `scratch` holds the
 * synthesized "iOS x.y" capture for the WebKit iOS fallback; on overflow the
 * raw captured digits are returned instead. */
cf_ua_version_result cf_ua_version_of(const cf_ua_agent *agent,
                                      cf_ua_buf *scratch);

/* Agent#platform (the gem's platform string, not ApplicationPlatform's
 * operating_system).  May raise (PodcastAddict). */
cf_ua_result cf_ua_platform(const cf_ua_agent *agent, cf_span *out);

/* Agent#os.  May raise (PodcastAddict, Windows Media Player).  `scratch` holds
 * normalize_os output and PlayStation joined comments; `*borrowed` is false
 * when *out points into the scratch. */
cf_ua_result cf_ua_os(const cf_ua_agent *agent, cf_ua_buf *scratch,
                      cf_span *out, bool *borrowed);

/* Agent#bot? */
bool cf_ua_is_bot(const cf_ua_agent *agent);

/* Version comparison (<) with the gem's Version#<=> (not a total order). */
bool cf_ua_version_lt(cf_ua_version a, cf_ua_version b);
bool cf_ua_version_lt_lit(cf_ua_version a, const char *text);

/* `Version#<=>` over two version strings, normalized to -1/0/1 (the corpus
 * `comparisons` differential's entry point; `Version#==` is string equality,
 * which callers can test directly). */
int cf_ua_version_cmp_lit(const char *a, const char *b);

/* ActiveSupport's `str.present?`: false when every character is Unicode
 * whitespace (Ruby's String#blank?); the empty string is not present.  Invalid
 * UTF-8 bytes count as non-whitespace. */
bool cf_ua_present(cf_span text);

/* HeaderValue::to_str -> is_visible_ascii (http 1.5.0): every byte must be
 * HTAB or 0x20..=0x7E.  Obs-text (>= 0x80), valid UTF-8 included, is
 * unreadable, so a header carrying it answers the kit's `None` (H01 already
 * rejects the other control bytes). */
bool cf_ua_header_readable(cf_span text);

/* `haystack.downcase.include?(needle)` / `== literal.downcase` for ASCII
 * needles/literals; exposed for platform.c's apple_messages and the
 * allow_browser browser names. */
bool cf_ua_fold_contains(cf_span haystack, const char *needle);
bool cf_ua_fold_eq(cf_span haystack, const char *literal);

#endif /* CF_AUTH_USER_AGENT_H */
