/* Accept-Encoding negotiation for response bodies (06-cache-performance.md
 * "Representation and HTTP behavior"; K01a).
 *
 * The port serves exactly two representations of a cacheable body: identity
 * and gzip. The selected coding is a cache-key field, both representations
 * share one weak ETag, and `Vary` includes Accept-Encoding (K01c owns the
 * header/key/304 wiring and the 406 response).
 *
 * The oracle is the pinned reference's app-level middleware, which is
 * `Rack::Deflater` as ported in tmp/rust-ref crates/kit/src/deflater.rs
 * (pin 64f86353021145b63849fb1cd93adeb08f3b8dbb):
 *
 *   - deflater.rs:45 reads only the FIRST `Accept-Encoding` header line,
 *     through `headers.get(..)` + `HeaderValue::to_str`, and falls back to ""
 *     when it is absent or unreadable; a later header line is not consulted.
 *   - deflater.rs:52 selects with `select_best_encoding(["gzip","identity"],
 *     parse_accept_encoding(..))`.
 *   - deflater.rs:94-102 answers 406 (text/plain body) when selection is None;
 *     a gzip selection sets `Content-Encoding: gzip` and removes
 *     Content-Length (deflater.rs:79-80); identity leaves the body alone.
 *
 * Exact rules (all mirrored from those functions, which the reference's tests
 * at deflater.rs:280-309 pin):
 *
 *  1. The input is the raw value of the first Accept-Encoding line. Absent,
 *     empty and unreadable all select identity: to_str (http 1.5.0) accepts
 *     only HTAB and visible ASCII (0x20..=0x7E), deflater.rs:45 passes None
 *     through as "", and parse of "" yields no tokens.
 *  2. Tokens: split the value on ',', trim HTAB/space around each part, drop
 *     empty parts, then keep at most the first 16 (deflater.rs:140-141,
 *     162). Each part splits once on ';': the coding name is the part before
 *     it (trimmed again, case preserved - deflater.rs:145,155 does NOT
 *     lowercase, so "GZIP" is a different token from "gzip"); the quality
 *     comes from the remainder after the first ';'.
 *  3. Quality is 1.0 unless that remainder (trimmed) begins with exactly
 *     "q=" (lowercase) and is followed by a non-empty run of [0-9.]; the run
 *     is converted like Ruby's String#to_f (deflater.rs:148-154). So
 *     "gzip;q=", "gzip;q=abc" and "gzip;Q=0.5" are all 1.0, "q=.5" is 0.5,
 *     and "q=."/"q=0..5" are 0.0.
 *  4. The available codings are gzip (preference 0) and identity
 *     (preference 1); an unknown token's preference is 2
 *     (deflater.rs:166,171).
 *  5. The first "*" token expands to the available codings not explicitly
 *     listed anywhere in the 16 tokens (exact, case-sensitive name match)
 *     with the wildcard's quality; later "*" tokens are ignored
 *     (deflater.rs:167-174).
 *  6. Candidates (explicit tokens plus wildcard expansions) sort by quality
 *     descending, then preference ascending (deflater.rs:178-179); an
 *     implicit identity candidate is appended when none exists
 *     (deflater.rs:181-182).
 *  7. Any candidate whose name appears in a q=0 entry is removed, all
 *     occurrences of that name (deflater.rs:184-188). Duplicates therefore
 *     behave as reject-if-any-zero.
 *  8. The first remaining gzip or identity candidate wins; none means
 *     unacceptable (deflater.rs:189 -> 406 at deflater.rs:94-102).
 *
 * Derived consequences pinned by tests: "br" selects identity; "gzip;q=0"
 * selects identity; "identity;q=0", "gzip;q=0, identity;q=0" and "*;q=0"
 * are unacceptable; "gzip;q=0.5" selects gzip even though identity is the
 * server default; equal qualities select gzip; "gzip;q=0, *;q=1" selects
 * identity because the wildcard never covers an explicitly listed coding.
 *
 * Divergence disclosure (spec vs pin; K01a evidence has the full note): the
 * pinned reference runs a SECOND, front-layer negotiation
 * (crates/kit/src/front/compression.rs:199-213) that prefers zstd at equal
 * quality and matches coding names case-insensitively
 * (compression.rs:219-220). The port has no front layer and 06 fixes the
 * representations to identity/gzip, so this module mirrors the app-level
 * Rack::Deflater semantics above. Consequences for a deployed-reference
 * comparison: "zstd"-only clients get identity here (the reference's front
 * would send zstd), and "GZIP" gets identity here (the front would compress,
 * though Rack::Deflater itself also selects identity for it).
 *
 * The functions are pure: no allocation, no header mutation, no 406 response
 * (the caller maps CF_ENC_UNACCEPTABLE to its 406 path).
 */
#ifndef CF_ENCODING_H
#define CF_ENCODING_H

#include "cf.h"

typedef enum {
    CF_ENC_IDENTITY = 0,   /* serve/store the identity representation */
    CF_ENC_GZIP,           /* serve/store the gzip representation */
    CF_ENC_UNACCEPTABLE,   /* both forbidden: caller returns 406 */
} cf_encoding;

/* Selects between identity and gzip from the FIRST Accept-Encoding line's
 * raw value. An absent header is (cf_span){NULL, 0}. The to_str readability
 * gate is applied here: a value with any byte outside HTAB/0x20..0x7E is
 * treated exactly like an absent header (identity), so callers cannot
 * accidentally parse bytes the reference would refuse to read. */
cf_encoding cf_encoding_select(cf_span first_accept_encoding_value);

/* Canonical Content-Encoding token for a selected coding: "gzip",
 * "identity", or NULL for CF_ENC_UNACCEPTABLE. */
const char *cf_encoding_name(cf_encoding encoding);

#endif /* CF_ENCODING_H */
