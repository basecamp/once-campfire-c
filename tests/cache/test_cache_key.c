/* tests/cache/test_cache_key.c — K01c cache-key encoding, weak ETag and
 * admission rules (06-cache-performance.md "K01: complete-body cache" field
 * list 1-7, "Representation and HTTP behavior").
 *
 * Byte-precise: the expected keys are complete hex strings derived by hand
 * from the documented layout (src/cache_key.h), not from the implementation.
 * Absence vs present-but-empty is asserted for every optional field, values
 * carry non-ASCII bytes and raw query text, and every field is mutated one at
 * a time to prove it participates in the key. */
#include "cf_test.h"

#include "cache_key.h"

#include <stdint.h>
#include <string.h>

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static cf_span SPN(const unsigned char *bytes, size_t len) {
    return (cf_span){bytes, len};
}

/* Compare the builder against an independently derived hex string. */
static bool key_is_hex(const cf_builder *builder, const char *expected) {
    size_t expected_len = strlen(expected) / 2;
    if (builder->len != expected_len) return false;
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < expected_len; i++) {
        char hi = digits[(builder->ptr[i] >> 4) & 0xf];
        char lo = digits[builder->ptr[i] & 0xf];
        if (expected[2 * i] != hi || expected[2 * i + 1] != lo) return false;
    }
    return true;
}

/* The worked example's fields (all present, gzip, non-empty values). */
static cf_cache_key_fields example_fields(void) {
    return (cf_cache_key_fields){
        .route_id = 101,
        .data_version = 7,
        .has_user_id = true,
        .user_id = 127326141,
        .target = SP("/rooms/654632876?page=2"),
        .format_symbol = "html",
        .encoding = CF_ENC_GZIP,
        .has_turbo_frame = true,
        .turbo_frame = SP("rooms_654632876"),
        .has_last_room = true,
        .last_room_id = 654632876,
        .has_user_agent = true,
        .user_agent = SP("Mozilla/5.0 (X11; Linux x86_64)"),
        .public_origin = SP("http://campfire.test"),
    };
}

CF_TEST(cache_key_worked_example_bytes) {
    cf_builder key = {0};
    cf_cache_key_fields fields = example_fields();
    CF_REQUIRE(cf_cache_key_build(&fields, &key) == CF_OK);
    /* 1 + 4 + 8 + 1 + 8 + (4+25) + (4+4) + 1 + 1+(4+15) + 1+8 + 1+(4+29) +
     * (4+20) = 147 bytes. */
    CF_CHECK(key.len == 147);
    CF_CHECK(key_is_hex(
        &key,
        "0165000000070000000000000001bdd796070000000017000000"
        "2f726f6f6d732f3635343633323837363f706167653d32"
        "0400000068746d6c01"
        "010f000000726f6f6d735f363534363332383736"
        "01ace7042700000000"
        "011f0000004d6f7a696c6c612f352e3020285831313b204c696e7578207838365f363429"
        "14000000687474703a2f2f63616d70666972652e74657374"));
    cf_builder_dispose(&key);
}

/* Every optional field: absent marker (one 0x00 byte) vs present-but-empty
 * (0x01 then a zero u32 length). The hex vectors are built from one base with
 * exactly one field flipped per case, so a mismatch names the field. */
static cf_cache_key_fields absence_base(void) {
    return (cf_cache_key_fields){
        .route_id = 96,
        .data_version = 1,
        .has_user_id = true,
        .user_id = 1,
        .target = SP("/rooms/1/@/2"),
        .format_symbol = "html",
        .encoding = CF_ENC_IDENTITY,
        .has_turbo_frame = true,
        .turbo_frame = SP(""),
        .has_last_room = true,
        .last_room_id = 0,
        .has_user_agent = true,
        .user_agent = SP(""),
        .public_origin = SP("http://campfire.test"),
    };
}

static void check_key_hex(cf_cache_key_fields fields, const char *expected) {
    cf_builder key = {0};
    CF_REQUIRE(cf_cache_key_build(&fields, &key) == CF_OK);
    CF_CHECK(key_is_hex(&key, expected));
    cf_builder_dispose(&key);
}

CF_TEST(cache_key_absence_is_distinct_from_empty) {
    cf_cache_key_fields fields = absence_base();
    fields.has_turbo_frame = false;
    check_key_hex(
        fields,
        "016000000001000000000000000101000000000000000c0000002f726f6f6d"
        "732f312f402f320400000068746d6c000001000000000000000001000000001"
        "4000000687474703a2f2f63616d70666972652e74657374");
    check_key_hex(
        absence_base(),
        "016000000001000000000000000101000000000000000c0000002f726f6f6d"
        "732f312f402f320400000068746d6c000100000000010000000000000000010"
        "000000014000000687474703a2f2f63616d70666972652e74657374");

    fields = absence_base();
    fields.has_user_agent = false;
    check_key_hex(
        fields,
        "016000000001000000000000000101000000000000000c0000002f726f6f6d"
        "732f312f402f320400000068746d6c000100000000010000000000000000001"
        "4000000687474703a2f2f63616d70666972652e74657374");
    check_key_hex(
        absence_base(),
        "016000000001000000000000000101000000000000000c0000002f726f6f6d"
        "732f312f402f320400000068746d6c000100000000010000000000000000010"
        "000000014000000687474703a2f2f63616d70666972652e74657374");

    fields = absence_base();
    fields.has_last_room = false;
    check_key_hex(
        fields,
        "016000000001000000000000000101000000000000000c0000002f726f6f6d"
        "732f312f402f320400000068746d6c000100000000000100000000140000006"
        "87474703a2f2f63616d70666972652e74657374");
    check_key_hex(
        absence_base(),
        "016000000001000000000000000101000000000000000c0000002f726f6f6d"
        "732f312f402f320400000068746d6c000100000000010000000000000000010"
        "000000014000000687474703a2f2f63616d70666972652e74657374");

    /* Present-but-empty costs the marker plus a zero u32 length (4 more). */
    cf_builder absent = {0}, empty = {0};
    cf_cache_key_fields empty_fields = absence_base();
    fields = absence_base();
    fields.has_turbo_frame = false;
    CF_REQUIRE(cf_cache_key_build(&fields, &absent) == CF_OK);
    CF_REQUIRE(cf_cache_key_build(&empty_fields, &empty) == CF_OK);
    CF_CHECK(empty.len == absent.len + 4);
    cf_builder_dispose(&absent);
    cf_builder_dispose(&empty);
}

/* Non-ASCII User-Agent bytes and a raw query string are copied verbatim; the
 * u32 length prefixes keep the layout unambiguous. */
CF_TEST(cache_key_non_ascii_and_raw_query) {
    cf_builder key = {0};
    cf_cache_key_fields fields = {
        .route_id = 76,
        .data_version = 9,
        .has_user_id = true,
        .user_id = 2,
        .target = SP("/rooms/1/messages?before=42"),
        .format_symbol = "html",
        .encoding = CF_ENC_IDENTITY,
        .has_turbo_frame = false,
        .has_last_room = false,
        .has_user_agent = true,
        .user_agent = SP("caf\xc3\xa9-browser/1.0"),
        .public_origin = SP("http://campfire.test"),
    };
    CF_REQUIRE(cf_cache_key_build(&fields, &key) == CF_OK);
    CF_CHECK(key_is_hex(
        &key,
        "014c00000009000000000000000102000000000000001b000000"
        "2f726f6f6d732f312f6d657373616765733f6265666f72653d3432"
        "0400000068746d6c00"
        "00"
        "00"
        "0111000000636166c3a92d62726f777365722f312e30"
        "14000000687474703a2f2f63616d70666972652e74657374"));
    cf_builder_dispose(&key);
}

/* A delimiter-free target cannot collide with the following length prefix
 * (the encoding is lengths+bytes, not concatenation). */
CF_TEST(cache_key_lengths_disambiguate_neighbours) {
    cf_builder a = {0}, b = {0};
    cf_cache_key_fields fields = {
        .route_id = 1,
        .data_version = 1,
        .has_user_id = true,
        .user_id = 1,
        .target = SP("/a"),
        .format_symbol = "bc",
        .encoding = CF_ENC_IDENTITY,
        .public_origin = SP("o"),
    };
    CF_REQUIRE(cf_cache_key_build(&fields, &a) == CF_OK);
    fields.target = SP("/ab");
    fields.format_symbol = "c";
    CF_REQUIRE(cf_cache_key_build(&fields, &b) == CF_OK);
    CF_CHECK(a.len == b.len);
    CF_CHECK(memcmp(a.ptr, b.ptr, a.len) != 0);
    cf_builder_dispose(&a);
    cf_builder_dispose(&b);
}

/* Every field participates: mutating one field changes the key bytes. */
CF_TEST(cache_key_every_field_changes_the_key) {
    cf_cache_key_fields base = example_fields();
    cf_builder reference = {0};
    CF_REQUIRE(cf_cache_key_build(&base, &reference) == CF_OK);

#define KEY_DIFFERS(label, mutated)                                          \
    do {                                                                     \
        cf_cache_key_fields fields_ = (mutated);                             \
        cf_builder other_ = {0};                                             \
        CF_REQUIRE(cf_cache_key_build(&fields_, &other_) == CF_OK);          \
        CF_CHECK(other_.len != reference.len ||                              \
                 memcmp(other_.ptr, reference.ptr, other_.len) != 0);        \
        cf_builder_dispose(&other_);                                         \
    } while (0)

    {
        cf_cache_key_fields mutated = base;
        mutated.route_id = 96;
        KEY_DIFFERS("route", mutated);
    }
    {
        cf_cache_key_fields mutated = base;
        mutated.data_version = 8;
        KEY_DIFFERS("version", mutated);
    }
    {
        cf_cache_key_fields mutated = base;
        mutated.user_id = 2;
        KEY_DIFFERS("user", mutated);
    }
    {
        cf_cache_key_fields mutated = base;
        mutated.has_user_id = false;
        KEY_DIFFERS("user absence", mutated);
    }
    {
        cf_cache_key_fields mutated = base;
        mutated.target = SP("/rooms/654632876?page=3");
        KEY_DIFFERS("query", mutated);
    }
    {
        cf_cache_key_fields mutated = base;
        mutated.format_symbol = "json";
        KEY_DIFFERS("format", mutated);
    }
    {
        cf_cache_key_fields mutated = base;
        mutated.encoding = CF_ENC_IDENTITY;
        KEY_DIFFERS("encoding", mutated);
    }
    {
        cf_cache_key_fields mutated = base;
        mutated.has_turbo_frame = false;
        KEY_DIFFERS("turbo absence", mutated);
    }
    {
        cf_cache_key_fields mutated = base;
        mutated.turbo_frame = SP("rooms_other");
        KEY_DIFFERS("turbo value", mutated);
    }
    {
        cf_cache_key_fields mutated = base;
        mutated.has_last_room = false;
        KEY_DIFFERS("last_room absence", mutated);
    }
    {
        cf_cache_key_fields mutated = base;
        mutated.last_room_id = 1;
        KEY_DIFFERS("last_room value", mutated);
    }
    {
        cf_cache_key_fields mutated = base;
        mutated.has_user_agent = false;
        KEY_DIFFERS("ua absence", mutated);
    }
    {
        cf_cache_key_fields mutated = base;
        mutated.user_agent = SP("Mozilla/5.0 (X11; Linux x86_64) other");
        KEY_DIFFERS("ua value", mutated);
    }
    {
        cf_cache_key_fields mutated = base;
        mutated.public_origin = SP("https://campfire.test");
        KEY_DIFFERS("origin", mutated);
    }
    cf_builder_dispose(&reference);
}

CF_TEST(cache_key_rejects_invalid_fields) {
    cf_builder key = {0};
    cf_cache_key_fields fields = example_fields();
    CF_CHECK(cf_cache_key_build(NULL, &key) == CF_INVALID);
    CF_CHECK(cf_cache_key_build(&fields, NULL) == CF_INVALID);
    fields.format_symbol = NULL;
    CF_CHECK(cf_cache_key_build(&fields, &key) == CF_INVALID);
    fields = example_fields();
    fields.encoding = CF_ENC_UNACCEPTABLE;
    CF_CHECK(cf_cache_key_build(&fields, &key) == CF_INVALID);
    fields = example_fields();
    fields.target = SPN(NULL, 3);
    CF_CHECK(cf_cache_key_build(&fields, &key) == CF_INVALID);
    cf_builder_dispose(&key);
}

CF_TEST(cache_etag_sha256_vectors) {
    char etag[69];
    CF_REQUIRE(cf_cache_etag(SP(""), etag) == CF_OK);
    CF_CHECK(strcmp(etag,
                    "W/\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca4"
                    "95991b7852b855\"") == 0);
    CF_CHECK(strlen(etag) == 68);
    CF_REQUIRE(cf_cache_etag(SP("hello campfire\n"), etag) == CF_OK);
    CF_CHECK(strcmp(etag,
                    "W/\"21cab22bfce9385aae835afd359a2f11ba5cf30158acf24f4769"
                    "3b650b35fd1f\"") == 0);
    /* lowercase only */
    for (size_t i = 3; i < 67; i++) {
        CF_CHECK(!(etag[i] >= 'A' && etag[i] <= 'F'));
    }
    CF_CHECK(cf_cache_etag(SPN(NULL, 2), etag) == CF_INVALID);
    CF_CHECK(cf_cache_etag(SP("x"), NULL) == CF_INVALID);
}

CF_TEST(cache_if_none_match_rules) {
    const char *etag =
        "W/\"21cab22bfce9385aae835afd359a2f11ba5cf30158acf24f47693b650b35fd1f\"";
    CF_CHECK(cf_cache_if_none_match(SP(etag), etag));
    {
        char list[256];
        snprintf(list, sizeof list, "\"other\",   %s , \"third\"", etag);
        CF_CHECK(cf_cache_if_none_match(SP(list), etag));
    }
    CF_CHECK(cf_cache_if_none_match(SP("*"), etag));
    CF_CHECK(cf_cache_if_none_match(SP(" \t* \t"), etag));
    CF_CHECK(!cf_cache_if_none_match(SP("\"other\""), etag));
    /* The weak prefix is part of the exact comparison (is_fresh compares the
     * whole value, kit ctx.rs:666-675). */
    CF_CHECK(!cf_cache_if_none_match(
        SP("21cab22bfce9385aae835afd359a2f11ba5cf30158acf24f47693b650b35fd1f"),
        etag));
    CF_CHECK(!cf_cache_if_none_match(SPN(NULL, 0), etag));
    {
        /* obs-text makes the whole header unreadable (HeaderValue::to_str),
         * even when a later comma-separated token would otherwise match. */
        const unsigned char bad[] = {0x80, '"', 'x', '"'};
        CF_CHECK(!cf_cache_if_none_match(SPN(bad, sizeof bad), etag));
        char list[256];
        snprintf(list, sizeof list, "%s,\x80", etag);
        CF_CHECK(!cf_cache_if_none_match(SP(list), etag));
    }
    {
        const unsigned char bad[] = {0x7f};
        CF_CHECK(!cf_cache_if_none_match(SPN(bad, sizeof bad), etag));
    }
    CF_CHECK(!cf_cache_if_none_match(SP(etag), NULL));
}

CF_TEST(cache_admission_decision_table) {
    cf_cache_admit_input ok = {
        .cache_enabled = true,
        .route_admitted = true,
        .method_get = true,
        .status = 200,
        .body_buffer = true,
    };
    CF_CHECK(cf_cache_admit_decide(&ok) == CF_CACHE_ADMIT);
    CF_CHECK(strcmp(cf_cache_decision_name(CF_CACHE_ADMIT), "admit") == 0);

    cf_cache_admit_input in = ok;
    in.cache_enabled = false;
    CF_CHECK(cf_cache_admit_decide(&in) == CF_CACHE_BYPASS_DISABLED);
    in = ok;
    in.route_admitted = false;
    CF_CHECK(cf_cache_admit_decide(&in) == CF_CACHE_BYPASS_ROUTE);
    in = ok;
    in.method_get = false; /* HEAD looks up, never populates */
    CF_CHECK(cf_cache_admit_decide(&in) == CF_CACHE_BYPASS_METHOD);
    in = ok;
    in.status = 204;
    CF_CHECK(cf_cache_admit_decide(&in) == CF_CACHE_BYPASS_STATUS);
    in.status = 302;
    CF_CHECK(cf_cache_admit_decide(&in) == CF_CACHE_BYPASS_STATUS);
    in = ok;
    in.body_buffer = false;
    CF_CHECK(cf_cache_admit_decide(&in) == CF_CACHE_BYPASS_BODY);
    in = ok;
    in.flash_present = true;
    CF_CHECK(cf_cache_admit_decide(&in) == CF_CACHE_BYPASS_FLASH);
    in = ok;
    in.bot = true;
    CF_CHECK(cf_cache_admit_decide(&in) == CF_CACHE_BYPASS_BOT);
    in = ok;
    in.action_content_encoding = true;
    CF_CHECK(cf_cache_admit_decide(&in) == CF_CACHE_BYPASS_ENCODING);
    in = ok;
    in.no_transform = true;
    CF_CHECK(cf_cache_admit_decide(&in) == CF_CACHE_BYPASS_NO_TRANSFORM);
    CF_CHECK(cf_cache_admit_decide(NULL) == CF_CACHE_BYPASS_DISABLED);
    CF_CHECK(strcmp(cf_cache_decision_name(CF_CACHE_BYPASS_FLASH), "flash") ==
             0);
}

CF_TEST(cache_representation_eligibility) {
    /* Rack::Deflater's should_deflate?: no 1xx/204/304, body present. */
    CF_CHECK(cf_cache_representation_eligible(200, true));
    CF_CHECK(cf_cache_representation_eligible(302, true));
    CF_CHECK(!cf_cache_representation_eligible(200, false));
    CF_CHECK(!cf_cache_representation_eligible(204, true));
    CF_CHECK(!cf_cache_representation_eligible(304, true));
    CF_CHECK(!cf_cache_representation_eligible(100, true));
    CF_CHECK(!cf_cache_representation_eligible(0, true));
}

CF_TEST_MAIN()
