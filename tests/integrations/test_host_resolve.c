/* tests/integrations/test_host_resolve.c — packet V-G resolver tests.
 *
 * Matrix over the pinned surfguard decision (guard.rs `classifies_...` and
 * `resolves_hosts_like_the_private_network_guard` cases): blocked forms
 * (private/decimal/hex/octal/loopback/link-local/mapped/NAT64/reserved),
 * public forms, malformed hosts (no DNS), the v4-before-v6 selection, the
 * resolution-failure arm, and the cf_push_resolve_fn adapter.
 *
 * No test performs real DNS: every name lookup goes through the injectable
 * seam with fixed answers (the C shape of the reference FakeResolver), and
 * numeric hosts never reach the seam at all (call counts prove it). The one
 * NULL-resolver case uses a numeric host only.
 *
 * Build (integrator wiring):
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -Isrc -Itests
 *         tests/integrations/test_host_resolve.c
 *         src/integrations/host_resolve.c
 *         src/models/types.c src/core/{alloc,buffer,clock,error,random}.c
 *         -o build/host-resolve/plain/test_host_resolve
 */
#include "cf_test.h"

#include "integrations/host_resolve.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- fixed-answer seam (FakeResolver shape) ------------------------------ */

#define FAKE_MAX_ENTRIES 8
#define FAKE_MAX_ANSWERS 8

typedef struct {
    const char *host;
    const char *answers[FAKE_MAX_ANSWERS];
    size_t answer_count;
    cf_err fail_rc; /* CF_OK answers, otherwise the lookup fails this way */
} fake_entry;

typedef struct {
    fake_entry entries[FAKE_MAX_ENTRIES];
    size_t entry_count;
    size_t lookups;
    char last_host[256];
} fake_resolver;

static cf_err fake_lookup(void *arg, const char *host, cf_host_addr *out,
                          size_t cap, size_t *out_len) {
    fake_resolver *fake = arg;
    fake->lookups++;
    size_t n = strlen(host);
    if (n >= sizeof fake->last_host) n = sizeof fake->last_host - 1;
    memcpy(fake->last_host, host, n);
    fake->last_host[n] = '\0';
    for (size_t i = 0; i < fake->entry_count; i++) {
        if (strcmp(fake->entries[i].host, host) != 0) continue;
        if (fake->entries[i].fail_rc != CF_OK) return fake->entries[i].fail_rc;
        if (fake->entries[i].answer_count > cap) return CF_NOT_FOUND;
        for (size_t k = 0; k < fake->entries[i].answer_count; k++) {
            if (!cf_host_addr_parse(
                    (cf_span){(const unsigned char *)
                                  fake->entries[i].answers[k],
                              strlen(fake->entries[i].answers[k])},
                    &out[k])) {
                return CF_IO;
            }
        }
        *out_len = fake->entries[i].answer_count;
        return CF_OK;
    }
    return CF_NOT_FOUND; /* unknown host: unresolvable */
}

static cf_host_resolver fake_as_resolver(fake_resolver *fake) {
    return (cf_host_resolver){fake_lookup, fake};
}

static void fake_add(fake_resolver *fake, const char *host,
                     const char *answers[], size_t count) {
    CF_REQUIRE(fake->entry_count < FAKE_MAX_ENTRIES);
    CF_REQUIRE(count <= FAKE_MAX_ANSWERS);
    fake_entry *entry = &fake->entries[fake->entry_count++];
    entry->host = host;
    entry->answer_count = count;
    entry->fail_rc = CF_OK;
    for (size_t i = 0; i < count; i++) entry->answers[i] = answers[i];
}

/* --- matrix --------------------------------------------------------------- */

CF_TEST(blocked_addresses_stay_blocked) {
    static const char *blocked[] = {
        "0.0.0.0", "10.1.2.3", "100.64.0.1", "127.0.0.1", "168.63.129.16",
        "169.254.169.254", "172.16.0.0", "172.31.255.255", "192.0.0.8",
        "192.0.2.1", "192.88.99.1", "192.168.1.1", "198.18.0.1",
        "198.51.100.1", "203.0.113.1", "224.0.0.1", "240.0.0.1",
        "255.255.255.255", "::", "::1", "::ffff:192.168.1.1", "::8.8.8.8",
        "64:ff9b::a00:1", "64:ff9b:1::1", "::ffff:0:a00:1", "fc00::1",
        "fd00::1", "fe80::1", "fec0::1", "ff02::1", "2001::1", "2001:db8::1",
        "2002::1", "3fff::1", "5f00::1", "100::1", "2001:2::1", "4000::1",
        "2001:10::1",
    };
    for (size_t i = 0; i < sizeof blocked / sizeof blocked[0]; i++) {
        cf_host_addr addr;
        memset(&addr, 0, sizeof addr);
        size_t len = strlen(blocked[i]);
        CF_REQUIRE(cf_host_addr_parse(
            (cf_span){(const unsigned char *)blocked[i], len}, &addr));
        CF_CHECK(cf_host_addr_blocked(addr));
    }
}

CF_TEST(public_addresses_stay_public) {
    static const char *public[] = {
        "8.8.8.8",
        "1.1.1.1",
        "93.184.216.34",
        "142.250.185.206",
        "172.32.0.1",
        "100.128.0.1",
        "192.0.1.1",
        "2606:2800:220:1:248:1893:25c8:1946",
        "2a00:1450:4001:82a::200e",
        "2001:3::1",
        "2001:4:112::1",
        "64:ff9b::808:808",
        "::ffff:0:808:808",
        "2c0f:ffff::1",
    };
    for (size_t i = 0; i < sizeof public / sizeof public[0]; i++) {
        cf_host_addr addr;
        memset(&addr, 0, sizeof addr);
        size_t len = strlen(public[i]);
        CF_REQUIRE(cf_host_addr_parse(
            (cf_span){(const unsigned char *)public[i], len}, &addr));
        CF_CHECK(!cf_host_addr_blocked(addr));
    }
}

CF_TEST(numeric_forms_never_reach_dns) {
    static const struct {
        const char *host;
        cf_err want;
        const char *want_text; /* when want == CF_OK */
    } cases[] = {
        {"127.0.0.1", CF_FORBIDDEN, NULL},
        {"127.1", CF_FORBIDDEN, NULL},
        {"0x7f.1", CF_FORBIDDEN, NULL},
        {"2130706433", CF_FORBIDDEN, NULL},
        {"0177.0.0.01", CF_FORBIDDEN, NULL},
        {"10.0.258", CF_FORBIDDEN, NULL},
        {"09.1.1.1", CF_FORBIDDEN, NULL},
        {"256.1.1.1", CF_FORBIDDEN, NULL},
        {"1.2.3.4.", CF_FORBIDDEN, NULL},
        {"01.2.3.4.", CF_FORBIDDEN, NULL},
        {"8.8.8.8", CF_OK, "8.8.8.8"},
        {"::1", CF_FORBIDDEN, NULL},
        {"[::1]", CF_FORBIDDEN, NULL},
        {"[fd00::1]", CF_FORBIDDEN, NULL},
        {"[2606:2800:220:1:248:1893:25c8:1946]", CF_OK,
         "2606:2800:220:1:248:1893:25c8:1946"},
        {"::ffff:8.8.8.8", CF_FORBIDDEN, NULL}, /* mapped: always blocked */
        {"::ffff:0:808:808", CF_OK, "::ffff:0:808:808"}, /* translatable: inner checked */
        {"::ffff:192.168.1.1", CF_FORBIDDEN, NULL},
    };
    fake_resolver fake;
    memset(&fake, 0, sizeof fake);
    cf_host_resolver resolver = fake_as_resolver(&fake);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char text[64];
        memset(text, 0, sizeof text);
        cf_err rc =
            cf_host_resolve_public(&resolver, cases[i].host, text,
                                   sizeof text);
        CF_CHECK(rc == cases[i].want);
        if (cases[i].want == CF_OK) {
            CF_CHECK(strcmp(text, cases[i].want_text) == 0);
        }
    }
    /* None of the numeric hosts consulted DNS. */
    CF_CHECK(fake.lookups == 0);
}

CF_TEST(malformed_hosts_are_violations_without_dns) {
    static const char *malformed[] = {
        "",        "a..b",          "host%eth0", "under_score.example",
        "-lead.example", "[v1.x]", "ex\xc3\xa4mple.com",
    };
    fake_resolver fake;
    memset(&fake, 0, sizeof fake);
    cf_host_resolver resolver = fake_as_resolver(&fake);
    for (size_t i = 0; i < sizeof malformed / sizeof malformed[0]; i++) {
        char text[64];
        CF_CHECK(cf_host_resolve_public(&resolver, malformed[i], text,
                                        sizeof text) == CF_FORBIDDEN);
    }
    CF_CHECK(fake.lookups == 0);
}

CF_TEST(dns_answers_filter_and_prefer_v4) {
    fake_resolver fake;
    memset(&fake, 0, sizeof fake);
    const char *www[] = {"93.184.216.34"};
    fake_add(&fake, "www.example.com", www, 1);
    /* Blocked private answer first, public v6 before public v4: the first
     * public IPv4 address still wins. */
    const char *mixed[] = {"10.0.0.1", "2606:2800:220:1:248:1893:25c8:1946",
                           "::1", "93.184.216.39"};
    fake_add(&fake, "mixed.example", mixed, 4);
    const char *only_private[] = {"192.168.1.10"};
    fake_add(&fake, "private.example", only_private, 1);
    cf_host_resolver resolver = fake_as_resolver(&fake);

    char text[64];
    CF_REQUIRE(cf_host_resolve_public(&resolver, "www.example.com", text,
                                      sizeof text) == CF_OK);
    CF_CHECK(strcmp(text, "93.184.216.34") == 0);

    memset(text, 0, sizeof text);
    CF_REQUIRE(cf_host_resolve_public(&resolver, "mixed.example", text,
                                      sizeof text) == CF_OK);
    CF_CHECK(strcmp(text, "93.184.216.39") == 0);

    CF_CHECK(cf_host_resolve_public(&resolver, "private.example", text,
                                    sizeof text) == CF_FORBIDDEN);
    /* Unknown host: the seam fails, the Unresolvable arm reports NOT_FOUND. */
    CF_CHECK(cf_host_resolve_public(&resolver, "nowhere.example", text,
                                    sizeof text) == CF_NOT_FOUND);
    CF_CHECK(fake.lookups == 4);
    CF_CHECK(strcmp(fake.last_host, "nowhere.example") == 0);
}

CF_TEST(resolution_failure_arm) {
    fake_resolver fake;
    memset(&fake, 0, sizeof fake);
    fake_entry *entry = &fake.entries[fake.entry_count++];
    entry->host = "flaky.example";
    entry->answer_count = 0;
    entry->fail_rc = CF_IO; /* transport failure, not just unknown */
    fake_entry *empty = &fake.entries[fake.entry_count++];
    empty->host = "empty.example";
    empty->answer_count = 0;
    empty->fail_rc = CF_OK; /* answered, but with nothing */
    cf_host_resolver resolver = fake_as_resolver(&fake);

    char text[64];
    CF_CHECK(cf_host_resolve_public(&resolver, "flaky.example", text,
                                    sizeof text) == CF_NOT_FOUND);
    CF_CHECK(cf_host_resolve_public(&resolver, "empty.example", text,
                                    sizeof text) == CF_NOT_FOUND);
}

CF_TEST(resolve_fn_adapter_matches_model_contract) {
    fake_resolver fake;
    memset(&fake, 0, sizeof fake);
    const char *www[] = {"93.184.216.34"};
    fake_add(&fake, "www.example.com", www, 1);
    const char *only_private[] = {"192.168.1.10"};
    fake_add(&fake, "private.example", only_private, 1);
    cf_host_resolver resolver = fake_as_resolver(&fake);

    cf_str host = {(char *)"www.example.com", strlen("www.example.com")};
    cf_optional_str got = cf_host_resolve_fn(&resolver, host);
    CF_CHECK(got.present);
    CF_REQUIRE(got.value.ptr != NULL);
    CF_CHECK(got.value.len == strlen("93.184.216.34"));
    CF_CHECK(memcmp(got.value.ptr, "93.184.216.34", got.value.len) == 0);
    CF_CHECK(got.value.ptr[got.value.len] == '\0');
    cf_optional_str_dispose(&got);

    cf_str blocked = {(char *)"private.example",
                      strlen("private.example")};
    cf_optional_str denied = cf_host_resolve_fn(&resolver, blocked);
    CF_CHECK(!denied.present);
    cf_optional_str_dispose(&denied);

    /* NULL resolver means the system one; numeric hosts need no network. */
    cf_str numeric = {(char *)"8.8.8.8", strlen("8.8.8.8")};
    cf_optional_str direct = cf_host_resolve_fn(NULL, numeric);
    CF_CHECK(direct.present);
    cf_optional_str_dispose(&direct);

    cf_str empty = {NULL, 0};
    CF_CHECK(!cf_host_resolve_fn(&resolver, empty).present);
}

CF_TEST_MAIN()
