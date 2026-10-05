/* K01c cache-key encoding, weak-ETag validator and admission decision
 * (06-cache-performance.md "K01: complete-body cache", "Representation and
 * HTTP behavior").
 *
 * The key is a versioned sequence of lengths and bytes: a one-byte encoding
 * version, fixed-width little-endian integers for the numeric fields, and
 * uint32-length-prefixed byte strings for every byte field. Nothing is
 * delimiter-concatenated, so a value containing the delimiter cannot collide
 * with another field. Optional byte fields carry a one-byte presence marker,
 * so absence is distinct from present-but-empty.
 *
 * Layout (offset from the start of the key):
 *
 *   u8   key version        (CF_CACHE_KEY_VERSION)
 *   u32  route ID           (HEAD requests carry the GET row's ID)
 *   u64  data version       (the version captured for the read snapshot)
 *   u8   has user ID; u64 user ID (two's complement) when present
 *   str  raw origin-form request target, including the raw query
 *   str  negotiated format symbol ("html", "json", ...)
 *   u8   selected encoding  (CF_ENC_IDENTITY / CF_ENC_GZIP)
 *   u8   has Turbo-Frame; str exact value when present
 *   u8   has last_room;   u64 parsed cookie ID when present
 *   u8   has User-Agent;  str exact value when present
 *   str  configured PUBLIC_ORIGIN
 *
 * where `str` is a u32 little-endian length followed by that many raw bytes
 * (an empty present string is a zero length; a NULL span is present with
 * length 0).
 *
 * Field list and order are 06's seven fields; the derived inputs (route ID,
 * format symbol, parsed last_room) are resolved by the caller (context.c),
 * which owns the request/context state. This module is pure: no allocation
 * beyond the caller's cf_builder, no locks, no I/O.
 */
#ifndef CF_CACHE_KEY_H
#define CF_CACHE_KEY_H

#include "cf.h"
#include "encoding.h"

/* Bump when the field list, order or encoding changes. The byte is part of
 * every key, so old-version entries can never match a new-version key. */
#define CF_CACHE_KEY_VERSION 1

typedef struct {
    uint32_t route_id;
    uint64_t data_version;
    bool has_user_id;
    int64_t user_id;
    cf_span target;            /* raw origin-form target incl. raw query */
    const char *format_symbol; /* negotiated format (cf_format.symbol) */
    cf_encoding encoding;      /* CF_ENC_IDENTITY or CF_ENC_GZIP only */
    bool has_turbo_frame;
    cf_span turbo_frame; /* exact first Turbo-Frame value */
    bool has_last_room;
    int64_t last_room_id; /* parsed last_room cookie ID */
    bool has_user_agent;
    cf_span user_agent;  /* exact first User-Agent value */
    cf_span public_origin; /* configured PUBLIC_ORIGIN */
} cf_cache_key_fields;

/* Append the key to *out (the caller initializes and disposes the builder).
 * CF_INVALID for NULL fields or a NULL target pointer with nonzero length;
 * CF_NOMEM/CF_LIMIT from the builder. A key longer than CF_CACHE_KEY_MAX
 * (cf_cache.h) is simply never admitted or matched; the caller does not have
 * to check. */
cf_err cf_cache_key_build(const cf_cache_key_fields *fields, cf_builder *out);

/* `W/"<64 lowercase hex>"` over the identity bytes (SHA-256), 69 bytes
 * including the NUL. Used for identity and gzip alike: the validator names
 * the identity representation both are semantically equal to. CF_INTERNAL if
 * the digest fails. */
cf_err cf_cache_etag(cf_span identity, char out[69]);

/* `request.fresh?(response)` with `strict_freshness` on If-None-Match only
 * (kit ctx.rs:666-675): true when the readable header value lists the exact
 * ETag or `*`, trimmed of SP/HTAB around each comma-separated token. A value
 * the pin's HeaderValue::to_str would refuse (any byte outside HTAB and
 * 0x20..0x7E) reads as absent, so it never matches. 06 specifies If-None-Match
 * handling; Last-Modified/If-Modified-Since are not part of this
 * representation. */
bool cf_cache_if_none_match(cf_span header_value, const char *weak_etag);

/* Admission decision (06: "Only admit 200 GET bodies ..."). Every bypass
 * reason is a documented rule so the live path and the unit tests share one
 * table; the caller applies the cache module's own caps/budget on top. */
typedef enum {
    CF_CACHE_ADMIT = 0,
    CF_CACHE_BYPASS_DISABLED,   /* no enabled cache object */
    CF_CACHE_BYPASS_ROUTE,      /* not one of the admitted handlers */
    CF_CACHE_BYPASS_METHOD,     /* not a GET (HEAD may look up, never puts) */
    CF_CACHE_BYPASS_STATUS,     /* not a 200 */
    CF_CACHE_BYPASS_BODY,       /* no buffered body */
    CF_CACHE_BYPASS_FLASH,      /* a flash-bearing body bypasses caching */
    CF_CACHE_BYPASS_BOT,        /* bot JSON never enters the cache */
    CF_CACHE_BYPASS_ENCODING,   /* response already carries Content-Encoding */
    CF_CACHE_BYPASS_NO_TRANSFORM /* Cache-Control: no-transform */
} cf_cache_decision;

const char *cf_cache_decision_name(cf_cache_decision decision);

typedef struct {
    bool cache_enabled;  /* an enabled cf_cache exists */
    bool route_admitted; /* route ID is one of the admitted handlers */
    bool method_get;     /* the wire method is GET */
    unsigned status;
    bool body_buffer;    /* the response body is a buffered cf_buf */
    bool flash_present;  /* the request's flash map holds an entry */
    bool bot;            /* authenticated as a bot */
    bool action_content_encoding; /* the action set Content-Encoding */
    bool no_transform;   /* the action set Cache-Control: no-transform */
} cf_cache_admit_input;

cf_cache_decision cf_cache_admit_decide(const cf_cache_admit_input *in);

/* Rack::Deflater's `should_deflate?` as the port can observe it: a status
 * that may carry a body (not 1xx/204/304) and a buffered body. The pin's
 * middleware only skips an app-set `Content-Length: 0`; the port's serializer
 * owns Content-Length, so there is nothing else to observe. */
bool cf_cache_representation_eligible(unsigned status, bool has_buffered_body);

#endif /* CF_CACHE_KEY_H */
