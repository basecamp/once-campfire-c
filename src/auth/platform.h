/* src/auth/platform.h — the User-Agent platform layer (task A01 completion).
 *
 * Reference: tmp/rust-ref/crates/campfire/src/concerns/platform.rs
 * (`ApplicationPlatform` over the ported useragent gem) and the `allow_browser`
 * check in concerns.rs.  Ratified interface
 * (docs/devel/IMPLEMENTATION-ROADMAP.md, 2026-10-05 "A01/A02 completion"):
 *   cf_err cf_platform_parse(cf_span user_agent, cf_platform *out);
 *   bool   cf_platform_blocked(const cf_platform *p);
 *
 * `cf_platform` carries the A02 view facts (the same field names as
 * `cf_view_platform`, which views.h aliases to this type so
 * `cf_ctx_platform(ctx)` can be passed to cf_presenter_layout_load directly)
 * plus the agent's browser-version text and bot flag.
 *
 * Lifetime: the parse is self-contained.  `browser`/`operating_system` and the
 * other texts point into the User-Agent span or at string literals; when the
 * gem synthesizes a string (normalize_os' "OS X ..."/"iOS ..."/"ChromeOS ..."
 * prefixes, PlayStation comments joined) the text is copied into the struct's
 * own `version_text`/`os_text` buffer and the span points there.  The struct
 * therefore needs no disposal, but a *copy* of it must not outlive the
 * original while such a span is in use (the layout presenter copies it for the
 * duration of one request, which is safe: the context outlives its layout).
 */
#ifndef CF_AUTH_PLATFORM_H
#define CF_AUTH_PLATFORM_H

#include "cf.h"

/* Room for the synthesized strings.  A User-Agent that normalizes to more
 * than this (only hostile input; the pinned corpus peaks at 40 bytes) keeps
 * the borrowed raw text instead of a truncated value. */
#define CF_AUTH_PLATFORM_TEXT_MAX 256

typedef struct {
    /* ApplicationPlatform#to_view facts (cf_view_platform). */
    bool ios, android, mac, windows;
    bool chrome, firefox, safari, edge;
    bool mobile, desktop;
    bool apple_messages;
    /* Platform#browser / #operating_system ("Chrome", "macOS").  Borrowed or
     * into the buffers below. */
    cf_span browser;
    cf_span operating_system;
    /* The agent's browser version ("" when nil or raised) and `bot?`. */
    cf_span browser_version;
    bool bot;
    /* Internal: the blocked? answer cf_platform_blocked returns. */
    bool blocked;
    char version_text[CF_AUTH_PLATFORM_TEXT_MAX];
    char os_text[CF_AUTH_PLATFORM_TEXT_MAX];
} cf_platform;

/* `ApplicationPlatform.new(user_agent)` plus the `to_view` mapping (Ruby
 * raises answer false/"") and the allow_browser `blocked?`.  CF_NOMEM only;
 * any byte sequence is accepted without crashing. */
cf_err cf_platform_parse(cf_span user_agent, cf_platform *out);

/* `BrowserBlocker#blocked?` with Campfire's VERSIONS (safari 17.2, chrome 120,
 * firefox 121, opera 104, ie false).  A versioned agent with a nil browser
 * (Rails would raise) is not blocked, like the reference port.  A NULL or
 * never-parsed platform is not blocked. */
bool cf_platform_blocked(const cf_platform *p);

#endif /* CF_AUTH_PLATFORM_H */
