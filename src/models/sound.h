/* src/models/sound.h — frozen model header (D01; declarations only).
 *
 * Source: tmp/rust-ref/crates/db/src/models/sound.rs
 * Tables: none. Sound is a compile-time builtin table from sound.rb; its
 * fields are borrowed static strings, so there is no record dispose and
 * lookups return interior pointers into the table.
 */
#ifndef CF_MODELS_SOUND_H
#define CF_MODELS_SOUND_H

#include "types.h"

/* A builtin image: name is the asset file, width/height its intrinsic size. */
typedef struct cf_sound_image {
    const char *name; /* static, borrowed */
    uint32_t width;
    uint32_t height;
} cf_sound_image;

/* A builtin sound. text and image are NULL when the reference entry has none;
 * all pointed-to data is static and never disposed. */
typedef struct cf_sound {
    const char *name;            /* static, borrowed */
    const char *text;            /* static, borrowed; NULL = no text */
    const cf_sound_image *image; /* static, borrowed; NULL = no image */
} cf_sound;

/* Owned vector of copied builtin names (Sound::names is sorted). */
typedef struct cf_sound_name_vector {
    cf_str *items;
    size_t len, cap;
} cf_sound_name_vector;

void cf_sound_name_vector_dispose(cf_sound_name_vector *vector);

/* Rust: SoundImage::asset_path — "sounds/<name>". */
cf_err cf_sound_image_asset_path(const cf_sound_image *image, cf_str *out);
/* Rust: Sound::asset_path — "<name>.mp3". */
cf_err cf_sound_asset_path(const cf_sound *sound, cf_str *out);
/* Rust: Sound::find_by_name — out borrows the static table; false leaves it
 * NULL. */
bool cf_sound_find_by_name(cf_str name, const cf_sound **out);
/* Rust: Sound::names — copied names, sorted. */
cf_err cf_sound_names(cf_sound_name_vector *out);

/* Every model-functions.json symbol for this module is prototyped above. */

#endif /* CF_MODELS_SOUND_H */
