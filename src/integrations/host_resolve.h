/* src/integrations/host_resolve.h — shared private-network host resolver
 * (packet V-G).
 *
 * Port of the DECISION in tmp/rust-ref/crates/campfire/src/integrations/
 * net/guard.rs (RestrictedHTTP::PrivateNetworkGuard + the surfguard default
 * policy it calls): a host goes in and a public address to pin comes out.
 * This is not a new SSRF policy — the blocked-address tables, the numeric
 * forms (decimal/octal/hex inet_aton, bracketed literals) and the
 * violation/unresolvable split are the pinned ones. I01's translation unit
 * is not linkable yet, so this file is standalone: the only DNS entry point
 * is getaddrinfo, hidden behind an injectable lookup seam (the C shape of
 * the reference's `Resolver` trait) that tests substitute with fixed
 * answers.
 *
 * Intended call sites (swap list for the integrator; this packet does not
 * edit the callers):
 *  - src/actions/users/push_subscriptions.c R3: local guard tables ~:173-370
 *    (byte-identical port of unfurl.c kDisV4/kDisV6/kIanaV6) plus
 *    push_default_resolve at :374-421 and the weak
 *    cf_users_push_resolve_host seam at :423-431. Swap: delete the tables
 *    and helpers, replace the push_default_resolve body with
 *    `return cf_host_resolve_fn(NULL, host);`, keep push_resolve and the
 *    weak seam untouched.
 *  - src/actions/users/push_subscriptions/test_notifications.c R2: same
 *    shape (tables ~:125-320, test_notifications_default_resolve at
 *    :337-370, weak cf_users_push_test_resolve_host seam at :381-390).
 *    Same swap with the same one-liner.
 *
 * One documented delta versus those local copies: exotic numeric hosts the
 * guard parses itself (0x7f.1, 2130706433, 0177.0.0.01) resolve-and-block
 * here instead of failing getaddrinfo. The outcome is the same (the model
 * reports "resolves to a private or invalid IP address"); this file follows
 * guard.rs exactly rather than the copies' approximation.
 *
 * No thread-local state (00-contracts.md): the resolver struct is
 * caller-owned and every call is synchronous. getaddrinfo itself is
 * thread-safe.
 */
#ifndef CF_INTEGRATIONS_HOST_RESOLVE_H
#define CF_INTEGRATIONS_HOST_RESOLVE_H

#include "cf.h"
#include "models/types.h"

#include <stddef.h>
#include <stdint.h>

/* One resolved address: AF_INET (first 4 bytes) or AF_INET6 (all 16). */
typedef struct {
    int family; /* AF_INET or AF_INET6 */
    unsigned char bytes[16]; /* network order */
} cf_host_addr;

/* Build a cf_host_addr from four IPv4 octets (test doubles, seams). */
cf_host_addr cf_host_addr_v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);

/* Parse dotted-quad or IPv6 text (brackets optional) into an address.
 * False when the text is not a full-length literal. */
bool cf_host_addr_parse(cf_span text, cf_host_addr *out);

/* The pinned surfguard default-policy decision: true when the address must
 * never be dialed (private, loopback, link-local, reserved, unallocated). */
bool cf_host_addr_blocked(cf_host_addr addr);

/* Lookup seam (the reference's `Resolver::lookup`): fill `out` with at most
 * `cap` answers for `host` and set `*out_len`. CF_OK on answers (even zero),
 * CF_NOT_FOUND/CF_IO when the lookup fails. A NULL seam means the system
 * resolver (getaddrinfo, which reads /etc/hosts then DNS like the
 * reference's SystemResolver). */
typedef cf_err (*cf_host_lookup_fn)(void *arg, const char *host,
                                    cf_host_addr *out, size_t cap,
                                    size_t *out_len);

typedef struct {
    cf_host_lookup_fn lookup; /* NULL selects getaddrinfo */
    void *arg;
} cf_host_resolver;

/* Resolve `host` to its first public address, written as NUL-terminated
 * inet_ntop text into `out` (which must hold at least 46 bytes,
 * INET6_ADDRSTRLEN). IPv4 answers come before IPv6 ones, in lookup order
 * within each family; blocked answers are dropped.
 * CF_OK: public address written. CF_FORBIDDEN: blocked or malformed host
 * (the reference's Violation arm). CF_NOT_FOUND: lookup failure, empty or
 * over-long answer list (the Unresolvable arm). CF_INVALID: NULL host/out.
 * CF_LIMIT: out_cap too small. Numeric hosts never reach DNS. */
cf_err cf_host_resolve_public(const cf_host_resolver *resolver,
                              const char *host, char *out, size_t out_cap);

/* cf_push_resolve_fn-compatible adapter (models/push_subscription.h): `arg`
 * is a `const cf_host_resolver *`, NULL selecting the system resolver.
 * Returns owned NUL-terminated address text with present=true for a public
 * host, and present=false for a private or unresolvable one. The model
 * disposes the returned value (malloc/free).
 *
 * No cf_push_exchange_fn shape is needed here: that typedef
 * (integrations/push.h:237) is I01's HTTP-exchange contract for delivery
 * (test_notifications.c R1, I01-owned), not the resolver. The resolver
 * shape specified in-file is cf_push_resolve_fn, which both requesters
 * already implement as `static cf_optional_str xxx_resolve(void *arg,
 * cf_str host)` over a weak seam. This adapter has exactly that signature,
 * so each action keeps its seam dispatch and swaps only its default body
 * for `cf_host_resolve_fn(NULL, host)`. */
cf_optional_str cf_host_resolve_fn(void *arg, cf_str host);

#endif /* CF_INTEGRATIONS_HOST_RESOLVE_H */
