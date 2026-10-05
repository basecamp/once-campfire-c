/* src/auth/platform.c — ApplicationPlatform and the allow_browser check.
 * Reference: tmp/rust-ref/crates/campfire/src/concerns/platform.rs and
 * concerns.rs::allow_browser/render_incompatible_browser (chrome step 7).
 *
 * `to_view`: chrome?/firefox?/safari?/edge? read the gem's browser, windows?
 * reads operating_system, and the gem raising a nil browser answers false for
 * the four (Ruby would 500).  operating_system follows try_operating_system:
 * first the six named platform needles, then the gem's os normalized (with
 * "Linux" mapped to itself).  See platform.h for lifetimes and the buffers.
 */
#include "platform.h"

#include "user_agent.h"

#include <stdlib.h>
#include <string.h>

static cf_span platform_lit(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static cf_span platform_empty(void) {
    static const char EMPTY[] = "";
    return (cf_span){(const unsigned char *)EMPTY, 0};
}

static bool platform_contains(cf_span hay, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0) return true;
    if (hay.len < n) return false;
    for (size_t i = 0; i + n <= hay.len; i++) {
        if (memcmp(hay.ptr + i, needle, n) == 0) return true;
    }
    return false;
}

/* Copy a synthesized string into the platform's own buffer.  Non-borrowed
 * query results always fit (the passing scratch is this same size, and an
 * overflowing synthesis falls back to the borrowed raw text); when one does
 * not, the borrowed span is kept rather than a truncated value. */
static cf_span platform_store(char *buffer, size_t cap, cf_span text,
                              bool borrowed) {
    if (borrowed || text.len == 0) return text;
    if (text.len >= cap) return text;
    memcpy(buffer, text.ptr, text.len);
    return (cf_span){(const unsigned char *)buffer, text.len};
}

/* BrowserBlocker#blocked? over the already-parsed facts. */
static bool platform_blocked_compute(cf_span user_agent, cf_span browser,
                                     bool browser_present,
                                     const cf_ua_version_result *version,
                                     bool bot) {
    if (!cf_ua_present(user_agent)) return false;
    if (version->raised || !version->found) return false;
    if (!cf_ua_present(version->version.text)) return false;
    if (!browser_present) return false; /* nil browser raises in Rails */

    const char *minimum = NULL;
    bool always_blocked = false;
    if (cf_ua_fold_eq(browser, "safari")) {
        minimum = "17.2";
    } else if (cf_ua_fold_eq(browser, "chrome")) {
        minimum = "120";
    } else if (cf_ua_fold_eq(browser, "firefox")) {
        minimum = "121";
    } else if (cf_ua_fold_eq(browser, "opera")) {
        minimum = "104";
    } else if (cf_ua_fold_eq(browser, "internet explorer")) {
        always_blocked = true; /* ie: false */
    } else {
        return false; /* not version-guarded */
    }
    bool below = always_blocked ||
                 cf_ua_version_lt_lit(version->version, minimum);
    return below && !bot;
}

cf_err cf_platform_parse(cf_span user_agent, cf_platform *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    out->browser = platform_empty();
    out->operating_system = platform_empty();
    out->browser_version = platform_empty();

    cf_ua_agent agent;
    cf_err rc = cf_ua_parse(user_agent, &agent);
    if (rc != CF_OK) {
        cf_ua_dispose(&agent);
        return rc;
    }

    /* The same size as the buffers the results are copied into: a synthesis
     * that cannot fit the scratch falls back to borrowed raw text, so a
     * non-borrowed result always fits platform_store. */
    char scratch_bytes[CF_AUTH_PLATFORM_TEXT_MAX];
    cf_ua_buf scratch = {scratch_bytes, 0, sizeof scratch_bytes, false};

    /* ApplicationPlatform predicates over the raw string. */
    out->ios = platform_contains(user_agent, "iPhone") ||
               platform_contains(user_agent, "iPad");
    out->android = platform_contains(user_agent, "Android");
    out->mac = platform_contains(user_agent, "Macintosh");
    out->apple_messages =
        cf_ua_fold_contains(user_agent, "facebookexternalhit") &&
        cf_ua_fold_contains(user_agent, "twitterbot");
    out->mobile = out->ios || out->android;
    out->desktop = !out->mobile;

    cf_span browser = {NULL, 0};
    bool browser_present = cf_ua_browser(&agent, &browser) == CF_UA_SOME;
    if (browser_present) {
        out->browser = browser;
        out->chrome = platform_contains(browser, "Chrome");
        out->firefox = platform_contains(browser, "Firefox");
        out->safari = platform_contains(browser, "Safari");
        out->edge = platform_contains(browser, "Edg");
    }

    cf_ua_version_result version = cf_ua_version_of(&agent, &scratch);
    if (version.found && version.version.text.ptr != NULL) {
        out->browser_version = platform_store(
            out->version_text, sizeof out->version_text, version.version.text,
            version.borrowed);
    }

    cf_span platform = {NULL, 0};
    cf_ua_result platform_result = cf_ua_platform(&agent, &platform);

    cf_span os = {NULL, 0};
    bool os_borrowed = true;
    cf_ua_result os_result =
        cf_ua_os(&agent, &scratch, &os, &os_borrowed);

    /* try_operating_system: a raising try_platform makes it raise; a nil
     * platform is ""; otherwise the six named needles, then try_os with
     * "Linux" mapped to itself. */
    bool operating_system_raised = platform_result == CF_UA_RAISED;
    if (!operating_system_raised) {
        cf_span p = platform_result == CF_UA_SOME ? platform : platform_empty();
        const char *named = NULL;
        if (platform_contains(p, "Android")) {
            named = "Android";
        } else if (platform_contains(p, "iPad")) {
            named = "iPad";
        } else if (platform_contains(p, "iPhone")) {
            named = "iPhone";
        } else if (platform_contains(p, "Macintosh")) {
            named = "macOS";
        } else if (platform_contains(p, "Windows")) {
            named = "Windows";
        } else if (platform_contains(p, "CrOS")) {
            named = "ChromeOS";
        }
        if (named != NULL) {
            out->operating_system = platform_lit(named);
        } else if (os_result == CF_UA_SOME) {
            if (platform_contains(os, "Linux")) {
                out->operating_system = platform_lit("Linux");
            } else {
                out->operating_system = platform_store(
                    out->os_text, sizeof out->os_text, os, os_borrowed);
            }
        }
    }
    out->windows = out->operating_system.len == 7 &&
                   memcmp(out->operating_system.ptr, "Windows", 7) == 0;

    out->bot = cf_ua_is_bot(&agent);
    out->blocked = platform_blocked_compute(user_agent, browser,
                                            browser_present, &version,
                                            out->bot);
    cf_ua_dispose(&agent);
    return CF_OK;
}

bool cf_platform_blocked(const cf_platform *p) {
    return p != NULL && p->blocked;
}
