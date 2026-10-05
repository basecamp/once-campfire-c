/* src/models/sound.c — D01 model family "sound".
 *
 * Source: tmp/rust-ref/crates/db/src/models/sound.rs (pinned; SHA-256 in
 * docs/devel/implementation/contracts/reference-files.json), which mirrors
 * tmp/rails-ref/app/models/sound.rb.
 *
 * Sound is a compile-time builtin table, not a database table: cf_sound
 * records point into static storage and have no dispose. Sound::names copies
 * the names into an owned vector and sorts them (INDEX.keys.sort). */
#include "models/sound.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Reference BUILTIN order, byte for byte from sound.rs. Each image is a
 * file-scope compound literal (C11 6.5.2.5p5: static storage duration).
 * text and image are mutually exclusive, as in the Rust constructors. */
static const cf_sound cf_sound_builtin[] = {
    { "56k", NULL, &(const cf_sound_image){ "56k.webp", 79, 33 } },
    { "bell", "🔔", NULL },
    { "bezos", "😆💭", NULL },
    { "bueller", "anyone?", NULL },
    { "butts", "👐 🚬", NULL },
    { "clowntown", NULL, &(const cf_sound_image){ "clowntown.webp", 210, 150 } },
    { "cottoneyejoe", "🎶🙉🎶 ", NULL },
    { "crickets", "hears crickets chirping", NULL },
    { "curb", NULL, &(const cf_sound_image){ "curb.webp", 150, 101 } },
    { "dadgummit", "dad gummit!! 🎣", NULL },
    { "dangerzone", NULL, &(const cf_sound_image){ "dangerzone.webp", 157, 32 } },
    { "danielsan", "🎆 🏆 🎆", NULL },
    { "deeper", NULL, &(const cf_sound_image){ "top.webp", 188, 80 } },
    { "ballmer", "developers!", NULL },
    { "donotwant", NULL, &(const cf_sound_image){ "donotwant.webp", 150, 150 } },
    { "drama", NULL, &(const cf_sound_image){ "drama.webp", 300, 16 } },
    { "flawless", "#flawless", NULL },
    { "glados", "🤖💢", NULL },
    { "gogogo", "Go, go, go!", NULL },
    { "greatjob", NULL, &(const cf_sound_image){ "greatjob.webp", 79, 16 } },
    { "greyjoy", "😖🎺", NULL },
    { "guarantee", "guarantees it 👌", NULL },
    { "heygirl", "✨💁✨", NULL },
    { "honk", "HONK", NULL },
    { "horn", "🐶 ✂️ 🐱", NULL },
    { "horror", "💀 💀 💀 💀 💀 💀 💀", NULL },
    { "inconceivable", "doesn't think it means what you think it means…", NULL },
    { "letitgo", "❄️👩❄️⛄️❄️", NULL },
    { "live", "is DOING IT LIVE", NULL },
    { "loggins", NULL, &(const cf_sound_image){ "loggins.webp", 200, 151 } },
    { "makeitso", "make it so 👉", NULL },
    { "noooo", "👸💀😒", NULL },
    { "nyan", NULL, &(const cf_sound_image){ "nyan.webp", 36, 15 } },
    { "ohmy", "raises an eyebrow 😏", NULL },
    { "ohyeah", "isn't playing by the rules", NULL },
    { "pushit", NULL, &(const cf_sound_image){ "pushit.webp", 104, 15 } },
    { "rimshot", "plays a rimshot", NULL },
    { "rollout", "is rolling out 🚗", NULL },
    { "rumble", NULL, &(const cf_sound_image){ "rumble.webp", 220, 150 } },
    { "sax", "🌇🎷🎶", NULL },
    { "secret", "found a secret area 🔑", NULL },
    { "sexyback", "🔞", NULL },
    { "story", "and now you know…", NULL },
    { "tada", "plays a fanfare 🎏", NULL },
    { "tmyk", "✨ ⭐️ The More You Know ✨ ⭐️", NULL },
    { "totes", "😁👍", NULL },
    { "trololo", "трололо", NULL },
    { "trombone", "plays a sad trombone", NULL },
    { "unix", "knows this 💻", NULL },
    { "vuvuzela", "======<() ~ ♪ ~♫", NULL },
    { "what", NULL, &(const cf_sound_image){ "what.webp", 100, 131 } },
    { "whoomp", "👏‼️😎", NULL },
    { "wups", "wups!", NULL },
    { "yay", NULL, &(const cf_sound_image){ "yay.webp", 103, 50 } },
    { "yeah", NULL, &(const cf_sound_image){ "yeah.webp", 104, 15 } },
    { "yodel", "📣🗻🙉", NULL },
};

#define CF_SOUND_BUILTIN_COUNT (sizeof(cf_sound_builtin) / sizeof(cf_sound_builtin[0]))

/* Allocates "<prefix><suffix>" as owned NUL-terminated text. */
static cf_err cf_sound_concat(const char *prefix, const char *suffix, cf_str *out) {
    size_t prefix_len = strlen(prefix);
    size_t suffix_len = strlen(suffix);
    if (prefix_len > SIZE_MAX - suffix_len - 1) return CF_LIMIT;
    char *text = malloc(prefix_len + suffix_len + 1);
    if (text == NULL) return CF_NOMEM;
    memcpy(text, prefix, prefix_len);
    memcpy(text + prefix_len, suffix, suffix_len);
    text[prefix_len + suffix_len] = '\0';
    out->ptr = text;
    out->len = prefix_len + suffix_len;
    return CF_OK;
}

void cf_sound_name_vector_dispose(cf_sound_name_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len && vector->items != NULL; i++) {
        free(vector->items[i].ptr);
    }
    free(vector->items);
    vector->items = NULL;
    vector->len = 0;
    vector->cap = 0;
}

cf_err cf_sound_image_asset_path(const cf_sound_image *image, cf_str *out) {
    if (out == NULL || image == NULL || image->name == NULL) return CF_INVALID;
    return cf_sound_concat("sounds/", image->name, out);
}

cf_err cf_sound_asset_path(const cf_sound *sound, cf_str *out) {
    if (out == NULL || sound == NULL || sound->name == NULL) return CF_INVALID;
    return cf_sound_concat(sound->name, ".mp3", out);
}

bool cf_sound_find_by_name(cf_str name, const cf_sound **out) {
    if (out == NULL) return false;
    *out = NULL;
    if (name.len != 0 && name.ptr == NULL) return false;
    for (size_t i = 0; i < CF_SOUND_BUILTIN_COUNT; i++) {
        const cf_sound *sound = &cf_sound_builtin[i];
        size_t sound_len = strlen(sound->name);
        if (sound_len == name.len && memcmp(sound->name, name.ptr, name.len) == 0) {
            *out = sound;
            return true;
        }
    }
    return false;
}

/* Rust str ordering: byte-wise lexicographic, shorter prefix first. */
static int cf_sound_name_compare(const void *a, const void *b) {
    const cf_str *left = a;
    const cf_str *right = b;
    size_t common = left->len < right->len ? left->len : right->len;
    int cmp = memcmp(left->ptr, right->ptr, common);
    if (cmp != 0) return cmp;
    if (left->len < right->len) return -1;
    if (left->len > right->len) return 1;
    return 0;
}

cf_err cf_sound_names(cf_sound_name_vector *out) {
    if (out == NULL) return CF_INVALID;
    cf_sound_name_vector built = {0};
    cf_str *items = malloc(CF_SOUND_BUILTIN_COUNT * sizeof *items);
    if (items == NULL) return CF_NOMEM;
    built.items = items;
    built.cap = CF_SOUND_BUILTIN_COUNT;
    for (size_t i = 0; i < CF_SOUND_BUILTIN_COUNT; i++) {
        const char *name = cf_sound_builtin[i].name;
        size_t name_len = strlen(name);
        char *copy = malloc(name_len + 1);
        if (copy == NULL) {
            cf_sound_name_vector_dispose(&built);
            return CF_NOMEM;
        }
        memcpy(copy, name, name_len + 1);
        items[i] = (cf_str){ copy, name_len };
        built.len = i + 1;
    }
    qsort(items, built.len, sizeof *items, cf_sound_name_compare);
    *out = built;
    return CF_OK;
}
