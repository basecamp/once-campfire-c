/* src/storage/media.h — task S03: exact media operations
 * (05-storage-integrations.md "S03", acceptance STORE-04).
 *
 * Sources translated (pinned tree, via ./tmp symlink):
 *   crates/storage/src/vips.rs          (thumbnail/autorot/sharpen semantics,
 *       loader page probing, block_untrusted + Openslide block)
 *   crates/storage/src/process.rs       (transform pipeline, VIDEO preview
 *       arguments via content_types.rs, ffmpeg 60 s timeout, kill/reap)
 *   crates/storage/src/analyze.rs       (analyzer selection, ffprobe 30 s
 *       timeout, image orientation rule, video/audio metadata field rules)
 *   crates/storage/src/variation.rs     (resize_to_limit shapes, format
 *       validation via marcel, named app variants below)
 *   crates/storage/src/content_types.rs (VARIABLE list, VIDEO_PREVIEW_ARGUMENTS,
 *       fixed "ffmpeg"/"ffprobe" executable names)
 *
 * Named app variants (reference call sites):
 *   thumb  (messages.rs process_attachment)        resize_to_limit 1200x800, source format
 *   poster (presenters.rs attachment preview)      format webp + resize_to_limit 1200x800
 *   avatar (users/avatars.rs avatar_variant)       resize_to_limit 512x512, format webp
 *   logo   (accounts/logos.rs show)                resize_to_limit 192 or 512, format png
 *   preview (messages.rs process_attachment)       format webp only (ffmpeg poster frame)
 *
 * Differences from the reference (experimental, S03):
 *   The reference calls libvips through FFI (vips.rs). Both builds here use
 *   the pinned-tool subprocesses instead (D-C05): this header owns fixed
 *   argv builders for the `vips` / `ffmpeg` / `ffprobe` CLIs executed
 *   through the S01 boundary (cf_proc_run in storage.h). No shell, no
 *   `sh -c`, no shell pipelines: multi-step operations name explicit
 *   task-owned intermediate files, and every intermediate is cleaned up.
 *   User metadata fills validated scalar arguments only, never switches or
 *   filenames: executable paths and argv[0] stay the S01 fixed constants.
 *
 *   At most CF_MEDIA_MAX_JOBS media jobs run at once, and media work never
 *   holds a write transaction (callers run this off the writer; a failed
 *   transform returns an explicit error so the caller takes the
 *   source-defined missing/error response path, never a bogus thumbnail).
 *
 *   Pinned versions: vips 8.16.1, ffmpeg/ffprobe 7.1.5. Versions alone do
 *   not guarantee equal bytes: live byte comparison runs only when
 *   cf_media_pinned_available() accepts the host tools, otherwise the live
 *   case reports BLOCKED (never a silent skip or PASS).
 *
 * Known CLI-equivalence gap (reported, not approximated): the reference
 * sharpen step convolves with a 3x3 mask whose image carries scale=24 and
 * offset=0 metadata (vips.rs::sharpen_mask). The `vips conv` CLI spelling
 * of that mask metadata is unverified while the pinned `vips` is absent
 * (F00-BLOCKED, no probe here), so cf_media_vips_sharpen_argv() emits the
 * verified operation/precision arguments with a caller-supplied mask path
 * and documents the gap. Likewise the reference `block_untrusted(true)` maps
 * to the VIPS_BLOCK_UNTRUSTED=1 environment (see CF_MEDIA_VIPS_ENV), while
 * the VipsForeignLoadOpenslide block has no CLI equivalent: runners must
 * not process Openslide-backed inputs until the adapter lands.
 */
#ifndef CF_STORAGE_MEDIA_H
#define CF_STORAGE_MEDIA_H

#include "storage/storage.h"

#include <stdint.h>

/* Pinned tool versions (05 S03). Compared verbatim by the live gate. */
#define CF_MEDIA_VIPS_VERSION "8.16.1"
#define CF_MEDIA_FFMPEG_VERSION "7.1.5"
#define CF_MEDIA_FFPROBE_VERSION "7.1.5"

/* Timeouts (05 S03): ffprobe 30 s, ffmpeg poster 60 s, vips operation 60 s. */
#define CF_MEDIA_FFPROBE_TIMEOUT_MS UINT64_C(30000)
#define CF_MEDIA_FFMPEG_TIMEOUT_MS UINT64_C(60000)
#define CF_MEDIA_VIPS_TIMEOUT_MS UINT64_C(60000)

/* Stdout caps (05 S03): metadata 1 MiB; media output starts at the 16 MiB
 * upload-sized experimental limit (CF_STORAGE_UPLOAD_MAX_BYTES). */
#define CF_MEDIA_METADATA_MAX_BYTES ((size_t)1048576)
#define CF_MEDIA_OUTPUT_MAX_BYTES ((size_t)16777216)

/* At most four media jobs at once (active_storage.rs::MAX_MEDIA_JOBS). */
#define CF_MEDIA_MAX_JOBS 4

/* resize_to_limit coordinate bound (vips.rs::MAX_COORD). */
#define CF_MEDIA_MAX_DIM 10000000

/* libvips honors this environment as block_untrusted(true); runners must
 * set it for every spawned vips child. There is no CLI spelling for the
 * VipsForeignLoadOpenslide block (see the gap note above). */
#define CF_MEDIA_VIPS_ENV "VIPS_BLOCK_UNTRUSTED=1"

/* Analyzer selection (analyze.rs::Analyzer::for_content_type): prefix match
 * on the content type, image before video before audio, else null. */
typedef enum {
    CF_MEDIA_ANALYZER_IMAGE = 0,
    CF_MEDIA_ANALYZER_VIDEO,
    CF_MEDIA_ANALYZER_AUDIO,
    CF_MEDIA_ANALYZER_NULL
} cf_media_analyzer;

cf_media_analyzer cf_media_analyzer_for_content_type(cf_span content_type);

/* EXIF orientation rule (analyze.rs::image_metadata): dimensions swap for
 * orientations whose libvips string contains one of Right-top, Left-bottom,
 * Top-right or Bottom-left. */
bool cf_media_exif_swaps_dimensions(cf_span orientation);

/* Video rotation rule (analyze.rs::video_metadata): dimensions swap for
 * angles of exactly +-90 or +-270 degrees. */
bool cf_media_angle_swaps_dimensions(int64_t angle);

/* A validated resize_to_limit pair (process.rs::operations): at least one
 * side present, each present side a positive i32 (the reference checks
 * i32::try_from: i32::MAX passes, i32::MAX+1 fails). Width/height arrive
 * as validated scalars; anything else is CF_INVALID, never a switch. Absent
 * sides widen to CF_MEDIA_MAX_DIM at argv build time, exactly as the
 * reference widens them to MAX_COORD. */
typedef struct {
    bool has_width;
    int32_t width;
    bool has_height;
    int32_t height;
} cf_media_resize;

cf_err cf_media_resize_validate(bool has_width, int64_t width, bool has_height,
                                int64_t height, cf_media_resize *out);

/* Variant output formats (variation.rs::format validated against the marcel
 * extension table, then restricted to the reference VARIABLE content types
 * by config/initializers/vips.rb: bmp, ico and psd stay unloadable even
 * though the installed tool could read them). ASCII case-insensitive;
 * anything outside this table is CF_INVALID, never passed through. */
bool cf_media_format_valid(cf_span format);
/* marcel::for_extension for a valid format (tables.rs TYPE_EXTS); NULL for
 * an invalid format. */
const char *cf_media_content_type_for_format(cf_span format);
/* content_types.rs::is_variable: only these content types may reach a vips
 * transform. In particular image/bmp, image/vnd.microsoft.icon and
 * image/vnd.adobe.photoshop are rejected here. */
bool cf_media_loader_allowed(cf_span content_type);

/* The five named app transforms (call sites listed above). resize_out is
 * always filled; format_out points at a static lowercase name ("png" for
 * the thumb default, matching variation.rs::format's :png fallback). */
typedef enum {
    CF_MEDIA_THUMB = 0, /* 1200x800, source format */
    CF_MEDIA_POSTER,    /* webp + 1200x800 (video poster variant) */
    CF_MEDIA_AVATAR,    /* 512x512 webp */
    CF_MEDIA_LOGO_SMALL,/* 192x192 png */
    CF_MEDIA_LOGO_LARGE /* 512x512 png */
} cf_media_preset;

cf_err cf_media_preset_describe(cf_media_preset preset, cf_media_resize *resize_out,
                                const char **format_out);

/* ---- fixed argv builders -------------------------------------------------
 * Each builder fills out with a NULL-terminated argv after argv[0] for
 * cf_proc_run (exe selected by the caller from the S01 enum). Switches are
 * compile-time literals; only validated scalars (decimal dimensions) and
 * task-owned paths (input/output/intermediate/mask) vary. Builders copy
 * every string into out's buffer, so out owns the argv until the next
 * builder call on it. CF_INVALID for any rejected scalar or path. */

#define CF_MEDIA_ARGV_MAX 24
#define CF_MEDIA_ARG_BUF 4096

typedef struct {
    int argc;
    char *argv[CF_MEDIA_ARGV_MAX + 1]; /* NULL-terminated */
    char buf[CF_MEDIA_ARG_BUF];
} cf_media_argv;

/* ffprobe -print_format json -show_streams -show_format -v error <input>
 * (analyze.rs::probe). */
cf_err cf_media_ffprobe_argv(cf_media_argv *out, const char *input_path);

/* ffmpeg -i <input> <VIDEO_PREVIEW_ARGUMENTS...> - (process.rs::video_preview
 * with content_types.rs::VIDEO_PREVIEW_ARGUMENTS, already shell-split). */
cf_err cf_media_poster_argv(cf_media_argv *out, const char *input_path);

/* vips thumbnail <input> <output> <width> --height <height> --size down
 * --no-rotate (vips.rs::resize_to_limit: thumbnail_image(size: :down,
 * no_rotate: true); absent sides widen to CF_MEDIA_MAX_DIM exactly as the
 * reference widens them to MAX_COORD). */
cf_err cf_media_vips_thumbnail_argv(cf_media_argv *out, const char *input_path,
                                    const char *output_path,
                                    const cf_media_resize *resize);

/* vips conv <input> <output> <mask> --precision integer (vips.rs conv with
 * precision: :integer). GAP (reported): the reference mask's scale=24 /
 * offset=0 image metadata has no verified `vips conv` CLI spelling while the
 * pinned vips is F00-BLOCKED, so the mask file is a task-owned intermediate
 * the adapter will define; this builder pins the verified arguments only. */
cf_err cf_media_vips_sharpen_argv(cf_media_argv *out, const char *input_path,
                                  const char *output_path, const char *mask_path);

/* Fill S01 run options from built argv with the S03 timeout/output caps.
 * stdout goes to a bounded capture (callers wanting a file pass an FD via
 * S01 directly); stderr keeps the S01 retained prefix for diagnostics. */
cf_err cf_media_proc_opts(cf_proc_opts *opts, cf_proc_exe exe,
                          cf_media_argv *argv, size_t stdout_limit,
                          uint64_t timeout_ms);

/* Map a finished child to the media contract: CF_OK only when it exited
 * normally with status 0 (real bytes); CF_LIMIT when the S01 boundary
 * killed it by timeout or the stdout cap; CF_IO for any other nonzero
 * exit, signal death or spawn failure. A failed transform is therefore an
 * explicit error return, never bogus output bytes. */
cf_err cf_media_check_result(const cf_proc_result *res);

/* ---- media job slots (at most CF_MEDIA_MAX_JOBS) ------------------------ */
cf_err cf_media_slots_try_acquire(void); /* CF_BUSY when all four are held */
cf_err cf_media_slots_acquire(void);     /* blocks until a slot is free */
void cf_media_slots_release(void);
int cf_media_slots_held(void);

/* ---- task-owned intermediates --------------------------------------------
 * Files for multi-step CLI intermediates (never shell pipelines). Create
 * with mkstemps under dir (<dir>/cf-media-XXXXXX<suffix>, 0600); dispose
 * with unlink (missing is CF_OK so double cleanup is safe). */
cf_err cf_media_temp_create(const char *dir, const char *suffix, char *path_out,
                            size_t path_cap, int *fd_out);
cf_err cf_media_temp_cleanup(const char *path);

/* base64(MD5) of a file's bytes (key.rs::checksum_file), for verifying
 * staged media outputs. */
cf_err cf_media_checksum_file(const char *path, cf_builder *out);

/* Live-tool gate: true only when CF_MEDIA_LIVE=1 in the environment AND
 * the installed vips/ffmpeg/ffprobe report exactly the pinned versions.
 * Otherwise false with a human-readable BLOCKED reason in reason[] (always
 * NUL-terminated when cap > 0). Version drift reports BLOCKED, never PASS. */
bool cf_media_pinned_available(char *reason, size_t cap);

#endif /* CF_STORAGE_MEDIA_H */
