/* D01 model family "sound" tests.
 *
 * Reference oracles (pinned; hashes in contracts/reference-files.json):
 *  - tmp/rust-ref/crates/db/src/models/sound.rs: BUILTIN order and values;
 *    its inline test builtin_sounds_match_reference checks sound.rb.
 *  - tmp/rails-ref/app/models/sound.rb: the same 56 entries, INDEX.keys.sort.
 * The expected table below was generated from sound.rs and cross-checked
 * entry-for-entry against sound.rb before this test was written.
 *
 * Build (plain; add -fsanitize=address,undefined -fno-sanitize-recover=all
 * -fno-omit-frame-pointer -g for the sanitize run):
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -Isrc -Itests src/models/sound.c
 *         tests/models/sound_test.c -o build/d01-sound/plain/test_sound
 */
#include "models/sound.h"

#include "cf_test.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Expected builtin table, transcribed from the pinned sound.rs. */
typedef struct {
    const char *name;
    const char *text;  /* NULL for image entries */
    const char *image; /* NULL for text entries */
    uint32_t width;
    uint32_t height;
} expected_sound;

static const expected_sound expected_builtin[] = {
    { "56k", NULL, "56k.webp", 79, 33 },
    { "bell", "🔔", NULL, 0, 0 },
    { "bezos", "😆💭", NULL, 0, 0 },
    { "bueller", "anyone?", NULL, 0, 0 },
    { "butts", "👐 🚬", NULL, 0, 0 },
    { "clowntown", NULL, "clowntown.webp", 210, 150 },
    { "cottoneyejoe", "🎶🙉🎶 ", NULL, 0, 0 },
    { "crickets", "hears crickets chirping", NULL, 0, 0 },
    { "curb", NULL, "curb.webp", 150, 101 },
    { "dadgummit", "dad gummit!! 🎣", NULL, 0, 0 },
    { "dangerzone", NULL, "dangerzone.webp", 157, 32 },
    { "danielsan", "🎆 🏆 🎆", NULL, 0, 0 },
    { "deeper", NULL, "top.webp", 188, 80 },
    { "ballmer", "developers!", NULL, 0, 0 },
    { "donotwant", NULL, "donotwant.webp", 150, 150 },
    { "drama", NULL, "drama.webp", 300, 16 },
    { "flawless", "#flawless", NULL, 0, 0 },
    { "glados", "🤖💢", NULL, 0, 0 },
    { "gogogo", "Go, go, go!", NULL, 0, 0 },
    { "greatjob", NULL, "greatjob.webp", 79, 16 },
    { "greyjoy", "😖🎺", NULL, 0, 0 },
    { "guarantee", "guarantees it 👌", NULL, 0, 0 },
    { "heygirl", "✨💁✨", NULL, 0, 0 },
    { "honk", "HONK", NULL, 0, 0 },
    { "horn", "🐶 ✂️ 🐱", NULL, 0, 0 },
    { "horror", "💀 💀 💀 💀 💀 💀 💀", NULL, 0, 0 },
    { "inconceivable", "doesn't think it means what you think it means…", NULL, 0, 0 },
    { "letitgo", "❄️👩❄️⛄️❄️", NULL, 0, 0 },
    { "live", "is DOING IT LIVE", NULL, 0, 0 },
    { "loggins", NULL, "loggins.webp", 200, 151 },
    { "makeitso", "make it so 👉", NULL, 0, 0 },
    { "noooo", "👸💀😒", NULL, 0, 0 },
    { "nyan", NULL, "nyan.webp", 36, 15 },
    { "ohmy", "raises an eyebrow 😏", NULL, 0, 0 },
    { "ohyeah", "isn't playing by the rules", NULL, 0, 0 },
    { "pushit", NULL, "pushit.webp", 104, 15 },
    { "rimshot", "plays a rimshot", NULL, 0, 0 },
    { "rollout", "is rolling out 🚗", NULL, 0, 0 },
    { "rumble", NULL, "rumble.webp", 220, 150 },
    { "sax", "🌇🎷🎶", NULL, 0, 0 },
    { "secret", "found a secret area 🔑", NULL, 0, 0 },
    { "sexyback", "🔞", NULL, 0, 0 },
    { "story", "and now you know…", NULL, 0, 0 },
    { "tada", "plays a fanfare 🎏", NULL, 0, 0 },
    { "tmyk", "✨ ⭐️ The More You Know ✨ ⭐️", NULL, 0, 0 },
    { "totes", "😁👍", NULL, 0, 0 },
    { "trololo", "трололо", NULL, 0, 0 },
    { "trombone", "plays a sad trombone", NULL, 0, 0 },
    { "unix", "knows this 💻", NULL, 0, 0 },
    { "vuvuzela", "======<() ~ ♪ ~♫", NULL, 0, 0 },
    { "what", NULL, "what.webp", 100, 131 },
    { "whoomp", "👏‼️😎", NULL, 0, 0 },
    { "wups", "wups!", NULL, 0, 0 },
    { "yay", NULL, "yay.webp", 103, 50 },
    { "yeah", NULL, "yeah.webp", 104, 15 },
    { "yodel", "📣🗻🙉", NULL, 0, 0 },
};

#define EXPECTED_COUNT (sizeof(expected_builtin) / sizeof(expected_builtin[0]))

/* Expected Sound::names order (sorted byte-wise); cross-checked with the
 * Ruby INDEX.keys.sort oracle. */
static const char *const expected_sorted_names[] = {
    "56k",
    "ballmer",
    "bell",
    "bezos",
    "bueller",
    "butts",
    "clowntown",
    "cottoneyejoe",
    "crickets",
    "curb",
    "dadgummit",
    "dangerzone",
    "danielsan",
    "deeper",
    "donotwant",
    "drama",
    "flawless",
    "glados",
    "gogogo",
    "greatjob",
    "greyjoy",
    "guarantee",
    "heygirl",
    "honk",
    "horn",
    "horror",
    "inconceivable",
    "letitgo",
    "live",
    "loggins",
    "makeitso",
    "noooo",
    "nyan",
    "ohmy",
    "ohyeah",
    "pushit",
    "rimshot",
    "rollout",
    "rumble",
    "sax",
    "secret",
    "sexyback",
    "story",
    "tada",
    "tmyk",
    "totes",
    "trololo",
    "trombone",
    "unix",
    "vuvuzela",
    "what",
    "whoomp",
    "wups",
    "yay",
    "yeah",
    "yodel",
};

#define EXPECTED_SORTED_COUNT (sizeof(expected_sorted_names) / sizeof(expected_sorted_names[0]))

static cf_str str_of(const char *text) {
    return (cf_str){(char *)text, strlen(text)};
}

/* The frozen contract releases owned text with cf_str_dispose (types.h),
 * which another D01 subpacket provides; tests free the malloc'd bytes
 * directly so this binary links without that translation unit. */
static void dispose_owned(cf_str *value) {
    free(value->ptr);
    value->ptr = NULL;
    value->len = 0;
}

static int name_order(const cf_str *left, const cf_str *right) {
    size_t common = left->len < right->len ? left->len : right->len;
    int cmp = memcmp(left->ptr, right->ptr, common);
    if (cmp != 0) return cmp;
    if (left->len < right->len) return -1;
    if (left->len > right->len) return 1;
    return 0;
}

CF_TEST(asset_path_image_and_sound) {
    const cf_sound *deeper = NULL;
    CF_REQUIRE(cf_sound_find_by_name(CF_STR_LIT("deeper"), &deeper));
    CF_REQUIRE(deeper->image != NULL);
    CF_CHECK(strcmp(deeper->image->name, "top.webp") == 0);
    CF_CHECK(deeper->image->width == 188 && deeper->image->height == 80);
    CF_CHECK(deeper->text == NULL);

    cf_str path = {0};
    CF_REQUIRE(cf_sound_image_asset_path(deeper->image, &path) == CF_OK);
    CF_CHECK(path.ptr != NULL && path.len == strlen("sounds/top.webp"));
    CF_CHECK(path.ptr != NULL && strcmp(path.ptr, "sounds/top.webp") == 0);
    CF_CHECK(path.ptr != NULL && path.ptr[path.len] == '\0');
    dispose_owned(&path);

    const cf_sound *bell = NULL;
    CF_REQUIRE(cf_sound_find_by_name(CF_STR_LIT("bell"), &bell));
    /* U+1F514, F0 9F 94 94: the text bytes are compared explicitly. */
    CF_CHECK(bell->text != NULL && strcmp(bell->text, "\xf0\x9f\x94\x94") == 0);
    CF_CHECK(bell->image == NULL);

    CF_REQUIRE(cf_sound_asset_path(bell, &path) == CF_OK);
    CF_CHECK(path.ptr != NULL && path.len == strlen("bell.mp3"));
    CF_CHECK(path.ptr != NULL && strcmp(path.ptr, "bell.mp3") == 0);
    dispose_owned(&path);

    /* 56k is both a sound name and an image file name with the same text. */
    const cf_sound *k56 = NULL;
    CF_REQUIRE(cf_sound_find_by_name(CF_STR_LIT("56k"), &k56));
    CF_REQUIRE(k56->image != NULL);
    CF_REQUIRE(cf_sound_image_asset_path(k56->image, &path) == CF_OK);
    CF_CHECK(path.ptr != NULL && strcmp(path.ptr, "sounds/56k.webp") == 0);
    dispose_owned(&path);
}

CF_TEST(asset_path_failure_paths) {
    cf_str out = {NULL, 0};
    CF_CHECK(cf_sound_image_asset_path(NULL, &out) == CF_INVALID);
    CF_CHECK(out.ptr == NULL && out.len == 0);
    CF_CHECK(cf_sound_image_asset_path(NULL, NULL) == CF_INVALID);

    cf_sound_image nameless = {NULL, 0, 0};
    CF_CHECK(cf_sound_image_asset_path(&nameless, &out) == CF_INVALID);
    CF_CHECK(out.ptr == NULL && out.len == 0);

    CF_CHECK(cf_sound_asset_path(NULL, &out) == CF_INVALID);
    CF_CHECK(out.ptr == NULL && out.len == 0);
    CF_CHECK(cf_sound_asset_path(NULL, NULL) == CF_INVALID);

    cf_sound unnamed = {NULL, NULL, NULL};
    CF_CHECK(cf_sound_asset_path(&unnamed, &out) == CF_INVALID);
    CF_CHECK(out.ptr == NULL && out.len == 0);
}

CF_TEST(find_by_name_all_builtins) {
    CF_REQUIRE(EXPECTED_COUNT == 56);
    for (size_t i = 0; i < EXPECTED_COUNT; i++) {
        const expected_sound *want = &expected_builtin[i];
        const cf_sound *got = NULL;
        CF_REQUIRE(cf_sound_find_by_name(str_of(want->name), &got));
        CF_REQUIRE(got != NULL);
        CF_CHECK(strcmp(got->name, want->name) == 0);
        if (want->text != NULL) {
            CF_CHECK(got->text != NULL && strcmp(got->text, want->text) == 0);
            CF_CHECK(got->image == NULL);
        } else {
            CF_CHECK(got->text == NULL);
            CF_REQUIRE(got->image != NULL);
            CF_CHECK(strcmp(got->image->name, want->image) == 0);
            CF_CHECK(got->image->width == want->width);
            CF_CHECK(got->image->height == want->height);
        }
        /* The returned record is a stable interior pointer into the table. */
        const cf_sound *again = NULL;
        CF_REQUIRE(cf_sound_find_by_name(str_of(want->name), &again));
        CF_CHECK(again == got);
    }
}

CF_TEST(find_by_name_missing) {
    const cf_sound *out = (const cf_sound *)1;
    CF_CHECK(!cf_sound_find_by_name(CF_STR_LIT("nosuchsound"), &out));
    CF_CHECK(out == NULL);

    out = (const cf_sound *)1;
    CF_CHECK(!cf_sound_find_by_name(CF_STR_LIT(""), &out));
    CF_CHECK(out == NULL);

    out = (const cf_sound *)1;
    CF_CHECK(!cf_sound_find_by_name(CF_STR_LIT("BELL"), &out)); /* case-sensitive */
    CF_CHECK(out == NULL);

    out = (const cf_sound *)1;
    CF_CHECK(!cf_sound_find_by_name(CF_STR_LIT("bell "), &out));
    CF_CHECK(out == NULL);

    /* A borrowed slice without NUL termination matches by length and bytes. */
    cf_str prefix = {(char *)"bell", 4};
    out = (const cf_sound *)1;
    CF_CHECK(cf_sound_find_by_name(prefix, &out));
    CF_CHECK(out != NULL && strcmp(out->name, "bell") == 0);

    CF_CHECK(!cf_sound_find_by_name(CF_STR_LIT("bell"), NULL));
    CF_CHECK(!cf_sound_find_by_name((cf_str){NULL, 3}, &out));
    CF_CHECK(out == NULL);
}

CF_TEST(names_sorted_and_copied) {
    cf_sound_name_vector names = {0};
    CF_REQUIRE(cf_sound_names(&names) == CF_OK);
    CF_REQUIRE(names.items != NULL);
    CF_CHECK(names.len == EXPECTED_SORTED_COUNT);
    CF_CHECK(names.len == EXPECTED_COUNT);
    CF_CHECK(names.cap >= names.len);

    for (size_t i = 0; i < names.len; i++) {
        CF_REQUIRE(names.items[i].ptr != NULL);
        CF_CHECK(names.items[i].ptr[names.items[i].len] == '\0');
        if (i > 0) {
            CF_CHECK(name_order(&names.items[i - 1], &names.items[i]) < 0);
        }
    }
    for (size_t i = 0; i < EXPECTED_SORTED_COUNT; i++) {
        size_t got = names.len;
        for (size_t j = 0; j < names.len; j++) {
            if (strcmp(names.items[j].ptr, expected_sorted_names[i]) == 0) {
                got = j;
                break;
            }
        }
        CF_REQUIRE(got < names.len);
        /* Owner copies, not table aliases: the bytes must live elsewhere. */
        const cf_sound *record = NULL;
        CF_REQUIRE(cf_sound_find_by_name(str_of(expected_sorted_names[i]), &record));
        CF_CHECK((const char *)names.items[got].ptr != record->name);
        CF_CHECK(strcmp(names.items[got].ptr, record->name) == 0);
        CF_CHECK(names.items[got].len == strlen(expected_sorted_names[i]));
    }

    cf_sound_name_vector_dispose(&names);
    CF_CHECK(names.items == NULL && names.len == 0 && names.cap == 0);
    cf_sound_name_vector_dispose(&names); /* zero state is a no-op */
}

CF_TEST(names_failure_and_dispose_edges) {
    CF_CHECK(cf_sound_names(NULL) == CF_INVALID);

    cf_sound_name_vector empty = {0};
    cf_sound_name_vector_dispose(&empty);
    CF_CHECK(empty.items == NULL && empty.len == 0 && empty.cap == 0);
    cf_sound_name_vector_dispose(NULL);

    /* A partially filled vector frees exactly its owned elements. */
    cf_sound_name_vector partial = {0};
    partial.items = malloc(2 * sizeof *partial.items);
    CF_REQUIRE(partial.items != NULL);
    partial.items[0].ptr = malloc(2);
    CF_REQUIRE(partial.items[0].ptr != NULL);
    memcpy(partial.items[0].ptr, "a", 2);
    partial.items[0].len = 1;
    partial.len = 1;
    partial.cap = 2;
    cf_sound_name_vector_dispose(&partial);
    CF_CHECK(partial.items == NULL && partial.len == 0 && partial.cap == 0);
}

CF_TEST_MAIN()
