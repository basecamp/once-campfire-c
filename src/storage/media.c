/* src/storage/media.c — S03 exact media operations. See media.h. */
#include "storage/media.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>

/* Maximum task-owned path accepted by the argv builders. */
#define CF_MEDIA_PATH_MAX 4096

/* ---- analyzer selection -------------------------------------------------- */

static bool span_prefix(cf_span s, const char *prefix) {
    size_t n = strlen(prefix);
    return s.len >= n && s.ptr != NULL && memcmp(s.ptr, prefix, n) == 0;
}

cf_media_analyzer cf_media_analyzer_for_content_type(cf_span content_type) {
    if (content_type.ptr == NULL) return CF_MEDIA_ANALYZER_NULL;
    if (span_prefix(content_type, "image")) return CF_MEDIA_ANALYZER_IMAGE;
    if (span_prefix(content_type, "video")) return CF_MEDIA_ANALYZER_VIDEO;
    if (span_prefix(content_type, "audio")) return CF_MEDIA_ANALYZER_AUDIO;
    return CF_MEDIA_ANALYZER_NULL;
}

static bool span_contains(cf_span haystack, const char *needle) {
    size_t n = strlen(needle);
    if (haystack.ptr == NULL || n == 0 || n > haystack.len) return false;
    for (size_t i = 0; i + n <= haystack.len; i++) {
        if (memcmp(haystack.ptr + i, needle, n) == 0) return true;
    }
    return false;
}

bool cf_media_exif_swaps_dimensions(cf_span orientation) {
    static const char *const rotated[] = {
        "Right-top", "Left-bottom", "Top-right", "Bottom-left"
    };
    for (size_t i = 0; i < sizeof rotated / sizeof rotated[0]; i++) {
        if (span_contains(orientation, rotated[i])) return true;
    }
    return false;
}

bool cf_media_angle_swaps_dimensions(int64_t angle) {
    return angle == 90 || angle == 270 || angle == -90 || angle == -270;
}

/* ---- variation validation ------------------------------------------------- */

cf_err cf_media_resize_validate(bool has_width, int64_t width, bool has_height,
                                int64_t height, cf_media_resize *out) {
    if (out == NULL) return CF_INVALID;
    if (!has_width && !has_height) return CF_INVALID;
    if (has_width && (width < 1 || width > INT32_MAX)) return CF_INVALID;
    if (has_height && (height < 1 || height > INT32_MAX)) return CF_INVALID;
    out->has_width = has_width;
    out->width = has_width ? (int32_t)width : 0;
    out->has_height = has_height;
    out->height = has_height ? (int32_t)height : 0;
    return CF_OK;
}

/* VARIABLE extensions with their marcel content types (content_types.rs plus
 * tables.rs TYPE_EXTS). bmp/ico/psd are deliberately absent. */
static const struct {
    const char *ext;
    const char *content_type;
} cf_media_formats[] = {
    {"avif", "image/avif"},
    {"gif", "image/gif"},
    {"heic", "image/heic"},
    {"heif", "image/heif"},
    {"jpeg", "image/jpeg"},
    {"jpg", "image/jpeg"},
    {"png", "image/png"},
    {"tif", "image/tiff"},
    {"tiff", "image/tiff"},
    {"webp", "image/webp"},
};

static const char *format_lookup(cf_span format) {
    if (format.ptr == NULL || format.len == 0 ||
        format.len > 8) { /* longest valid name is 4; margin, still exact */
        return NULL;
    }
    char lower[9];
    for (size_t i = 0; i < format.len; i++) {
        unsigned char c = format.ptr[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return NULL;
        lower[i] = (char)c;
    }
    lower[format.len] = '\0';
    for (size_t i = 0; i < sizeof cf_media_formats / sizeof cf_media_formats[0]; i++) {
        if (strcmp(lower, cf_media_formats[i].ext) == 0) {
            return cf_media_formats[i].content_type;
        }
    }
    return NULL;
}

bool cf_media_format_valid(cf_span format) {
    return format_lookup(format) != NULL;
}

const char *cf_media_content_type_for_format(cf_span format) {
    return format_lookup(format);
}

bool cf_media_loader_allowed(cf_span content_type) {
    if (content_type.ptr == NULL || content_type.len == 0) return false;
    static const char *const variable[] = {
        "image/png", "image/gif", "image/jpeg", "image/tiff",
        "image/webp", "image/avif", "image/heic", "image/heif"
    };
    for (size_t i = 0; i < sizeof variable / sizeof variable[0]; i++) {
        size_t n = strlen(variable[i]);
        if (content_type.len == n && memcmp(content_type.ptr, variable[i], n) == 0) {
            return true;
        }
    }
    return false;
}

cf_err cf_media_preset_describe(cf_media_preset preset, cf_media_resize *resize_out,
                                const char **format_out) {
    if (resize_out == NULL || format_out == NULL) return CF_INVALID;
    int64_t size = 0;
    switch (preset) {
    case CF_MEDIA_THUMB:
        resize_out->has_width = true;
        resize_out->width = 1200;
        resize_out->has_height = true;
        resize_out->height = 800;
        *format_out = "png"; /* variation.rs::format :png fallback */
        return CF_OK;
    case CF_MEDIA_POSTER:
        resize_out->has_width = true;
        resize_out->width = 1200;
        resize_out->has_height = true;
        resize_out->height = 800;
        *format_out = "webp";
        return CF_OK;
    case CF_MEDIA_AVATAR:
        size = 512;
        *format_out = "webp";
        break;
    case CF_MEDIA_LOGO_SMALL:
        size = 192;
        *format_out = "png";
        break;
    case CF_MEDIA_LOGO_LARGE:
        size = 512;
        *format_out = "png";
        break;
    default:
        return CF_INVALID;
    }
    resize_out->has_width = true;
    resize_out->width = (int32_t)size;
    resize_out->has_height = true;
    resize_out->height = (int32_t)size;
    return CF_OK;
}

/* ---- argv builders ---------------------------------------------------------
 * Switches are string literals; only decimal scalars and task-owned paths
 * vary, so user metadata can never become a switch or a filename. */

static cf_err media_path_copy(const char *path, char *dst, size_t dst_cap,
                              size_t *len_out) {
    if (path == NULL || path[0] == '\0') return CF_INVALID;
    size_t n = strlen(path);
    if (n >= dst_cap || n > CF_MEDIA_PATH_MAX) return CF_INVALID;
    memcpy(dst, path, n + 1);
    *len_out = n;
    return CF_OK;
}

typedef struct {
    cf_media_argv *out;
    size_t used;
    cf_err err;
} media_argwriter;

static void media_put(media_argwriter *w, const char *s) {
    if (w->err != CF_OK) return;
    if (w->out->argc >= CF_MEDIA_ARGV_MAX) {
        w->err = CF_LIMIT;
        return;
    }
    size_t n = strlen(s);
    if (w->used + n + 1 > sizeof w->out->buf) {
        w->err = CF_LIMIT;
        return;
    }
    char *dst = w->out->buf + w->used;
    memcpy(dst, s, n + 1);
    w->used += n + 1;
    w->out->argv[w->out->argc++] = dst;
}

static void media_put_path(media_argwriter *w, const char *path) {
    if (w->err != CF_OK) return;
    if (w->out->argc >= CF_MEDIA_ARGV_MAX) {
        w->err = CF_LIMIT;
        return;
    }
    size_t n = 0;
    if (w->used >= sizeof w->out->buf) {
        w->err = CF_LIMIT;
        return;
    }
    w->err = media_path_copy(path, w->out->buf + w->used,
                             sizeof w->out->buf - w->used, &n);
    if (w->err != CF_OK) return;
    w->out->argv[w->out->argc++] = w->out->buf + w->used;
    w->used += n + 1;
}

static void media_put_dim(media_argwriter *w, int32_t dim) {
    char tmp[16];
    snprintf(tmp, sizeof tmp, "%d", (int)dim);
    media_put(w, tmp);
}

static cf_err media_finish(media_argwriter *w) {
    if (w->err != CF_OK) return w->err;
    w->out->argv[w->out->argc] = NULL;
    return CF_OK;
}

static void media_begin(cf_media_argv *out, media_argwriter *w) {
    out->argc = 0;
    w->out = out;
    w->used = 0;
    w->err = CF_OK;
}

cf_err cf_media_ffprobe_argv(cf_media_argv *out, const char *input_path) {
    if (out == NULL) return CF_INVALID;
    media_argwriter w;
    media_begin(out, &w);
    media_put(&w, "-print_format");
    media_put(&w, "json");
    media_put(&w, "-show_streams");
    media_put(&w, "-show_format");
    media_put(&w, "-v");
    media_put(&w, "error");
    media_put_path(&w, input_path);
    return media_finish(&w);
}

/* content_types.rs::VIDEO_PREVIEW_ARGUMENTS, already shell-split there. */
static const char *const cf_media_preview_filter =
    "select=eq(n\\,0)+eq(key\\,1)+gt(scene\\,0.015),loop=loop=-1:size=2,trim=start_frame=1";

cf_err cf_media_poster_argv(cf_media_argv *out, const char *input_path) {
    if (out == NULL) return CF_INVALID;
    media_argwriter w;
    media_begin(out, &w);
    media_put(&w, "-i");
    media_put_path(&w, input_path);
    media_put(&w, "-vf");
    media_put(&w, cf_media_preview_filter);
    media_put(&w, "-frames:v");
    media_put(&w, "1");
    media_put(&w, "-f");
    media_put(&w, "image2");
    media_put(&w, "-");
    return media_finish(&w);
}

cf_err cf_media_vips_thumbnail_argv(cf_media_argv *out, const char *input_path,
                                    const char *output_path,
                                    const cf_media_resize *resize) {
    if (out == NULL || resize == NULL) return CF_INVALID;
    if (!resize->has_width && !resize->has_height) return CF_INVALID;
    if (resize->has_width && (resize->width < 1 || resize->width > INT32_MAX)) {
        return CF_INVALID;
    }
    if (resize->has_height && (resize->height < 1 || resize->height > INT32_MAX)) {
        return CF_INVALID;
    }
    int32_t width = resize->has_width ? resize->width : CF_MEDIA_MAX_DIM;
    int32_t height = resize->has_height ? resize->height : CF_MEDIA_MAX_DIM;
    media_argwriter w;
    media_begin(out, &w);
    media_put(&w, "thumbnail");
    media_put_path(&w, input_path);
    media_put_path(&w, output_path);
    media_put_dim(&w, width);
    media_put(&w, "--height");
    media_put_dim(&w, height);
    media_put(&w, "--size");
    media_put(&w, "down");
    media_put(&w, "--no-rotate");
    return media_finish(&w);
}

cf_err cf_media_vips_sharpen_argv(cf_media_argv *out, const char *input_path,
                                  const char *output_path, const char *mask_path) {
    if (out == NULL) return CF_INVALID;
    media_argwriter w;
    media_begin(out, &w);
    media_put(&w, "conv");
    media_put_path(&w, input_path);
    media_put_path(&w, output_path);
    media_put_path(&w, mask_path);
    media_put(&w, "--precision");
    media_put(&w, "integer");
    return media_finish(&w);
}

cf_err cf_media_proc_opts(cf_proc_opts *opts, cf_proc_exe exe,
                          cf_media_argv *argv, size_t stdout_limit,
                          uint64_t timeout_ms) {
    if (opts == NULL || argv == NULL || argv->argc <= 0) return CF_INVALID;
    if (timeout_ms == 0 || stdout_limit == 0) return CF_INVALID;
    if (cf_proc_executable_path(exe) == NULL) return CF_INVALID;
    opts->exe = exe;
    opts->args = argv->argv;
    opts->stdin_fd = -1;
    opts->stdout_fd = -1;
    opts->stdout_limit = stdout_limit;
    opts->stderr_fd = -1;
    opts->timeout_ms = timeout_ms;
    return CF_OK;
}

cf_err cf_media_check_result(const cf_proc_result *res) {
    if (res == NULL) return CF_INVALID;
    if (res->pid < 0) return CF_IO; /* nothing was spawned */
    if (res->timed_out || res->output_limit) return CF_LIMIT;
    if (res->exited_ok) return CF_OK;
    return CF_IO;
}

/* ---- media job slots ------------------------------------------------------ */

static pthread_mutex_t cf_media_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cf_media_slot_free = PTHREAD_COND_INITIALIZER;
static int cf_media_slots_used = 0;

cf_err cf_media_slots_try_acquire(void) {
    cf_err rc = CF_BUSY;
    pthread_mutex_lock(&cf_media_slot_lock);
    if (cf_media_slots_used < CF_MEDIA_MAX_JOBS) {
        cf_media_slots_used++;
        rc = CF_OK;
    }
    pthread_mutex_unlock(&cf_media_slot_lock);
    return rc;
}

cf_err cf_media_slots_acquire(void) {
    pthread_mutex_lock(&cf_media_slot_lock);
    while (cf_media_slots_used >= CF_MEDIA_MAX_JOBS) {
        pthread_cond_wait(&cf_media_slot_free, &cf_media_slot_lock);
    }
    cf_media_slots_used++;
    pthread_mutex_unlock(&cf_media_slot_lock);
    return CF_OK;
}

void cf_media_slots_release(void) {
    pthread_mutex_lock(&cf_media_slot_lock);
    if (cf_media_slots_used > 0) cf_media_slots_used--;
    pthread_cond_signal(&cf_media_slot_free);
    pthread_mutex_unlock(&cf_media_slot_lock);
}

int cf_media_slots_held(void) {
    int n;
    pthread_mutex_lock(&cf_media_slot_lock);
    n = cf_media_slots_used;
    pthread_mutex_unlock(&cf_media_slot_lock);
    return n;
}

/* ---- task-owned intermediates --------------------------------------------- */

cf_err cf_media_temp_create(const char *dir, const char *suffix, char *path_out,
                            size_t path_cap, int *fd_out) {
    if (dir == NULL || dir[0] == '\0' || path_out == NULL || path_cap == 0 ||
        fd_out == NULL) {
        return CF_INVALID;
    }
    size_t dn = strlen(dir);
    size_t sn = suffix != NULL ? strlen(suffix) : 0;
    if (sn > 32) return CF_INVALID;
    for (size_t i = 0; i < sn; i++) {
        char c = suffix[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.';
        if (!ok) return CF_INVALID;
    }
    /* "<dir>/cf-media-XXXXXX<suffix>" */
    if (dn + 18 + sn + 1 > path_cap) return CF_LIMIT;
    char tmpl[CF_MEDIA_PATH_MAX + 64];
    if (dn + 18 + sn + 1 > sizeof tmpl) return CF_LIMIT;
    memcpy(tmpl, dir, dn);
    memcpy(tmpl + dn, "/cf-media-XXXXXX", 17); /* 16 chars + NUL */
    memcpy(tmpl + dn + 16, suffix != NULL ? suffix : "", sn + 1);
    int fd = mkstemps(tmpl, (int)sn);
    if (fd < 0) return CF_IO;
    size_t n = strlen(tmpl);
    if (n + 1 > path_cap) {
        close(fd);
        unlink(tmpl);
        return CF_LIMIT;
    }
    memcpy(path_out, tmpl, n + 1);
    *fd_out = fd;
    return CF_OK;
}

cf_err cf_media_temp_cleanup(const char *path) {
    if (path == NULL || path[0] == '\0') return CF_INVALID;
    if (unlink(path) != 0) {
        return errno == ENOENT ? CF_OK : CF_IO;
    }
    return CF_OK;
}

/* ---- checksums ------------------------------------------------------------- */

cf_err cf_media_checksum_file(const char *path, cf_builder *out) {
    if (path == NULL || path[0] == '\0' || out == NULL) return CF_INVALID;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return CF_IO;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx == NULL) {
        close(fd);
        return CF_NOMEM;
    }
    cf_err rc = CF_IO;
    unsigned char buf[1 << 16];
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_DigestInit_ex(ctx, EVP_md5(), NULL) != 1) goto done;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            if (EVP_DigestUpdate(ctx, buf, (size_t)n) != 1) goto done;
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            goto done;
        }
    }
    if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1 || digest_len != 16) {
        goto done;
    }
    {
        unsigned char b64[25];
        int n = EVP_EncodeBlock(b64, digest, 16);
        if (n <= 0) goto done;
        rc = cf_builder_append(out, (cf_span){b64, (size_t)n});
    }
done:
    EVP_MD_CTX_free(ctx);
    close(fd);
    return rc;
}

/* ---- live-tool gate --------------------------------------------------------- */

static cf_err media_version_probe(cf_proc_exe exe, const char *flag,
                                 char *text, size_t cap) {
    char *const args[] = {(char *)flag, NULL};
    cf_proc_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.exe = exe;
    opts.args = args;
    opts.stdin_fd = -1;
    opts.stdout_fd = -1;
    opts.stdout_limit = 8192;
    opts.stderr_fd = -1;
    opts.timeout_ms = 10000;
    cf_proc_result res;
    memset(&res, 0, sizeof res);
    cf_err rc = cf_proc_run(&opts, &res);
    if (rc == CF_NOT_FOUND) {
        cf_proc_result_dispose(&res);
        return CF_NOT_FOUND;
    }
    if (rc == CF_LIMIT) {
        cf_proc_result_dispose(&res);
        return CF_LIMIT;
    }
    if (rc != CF_OK || !res.exited_ok || res.out == NULL) {
        cf_proc_result_dispose(&res);
        return CF_IO;
    }
    cf_span span = cf_buf_span(res.out);
    size_t n = span.len < cap - 1 ? span.len : cap - 1;
    if (cap > 0) {
        if (n > 0) memcpy(text, span.ptr, n);
        text[n] = '\0';
    }
    cf_proc_result_dispose(&res);
    return CF_OK;
}

static void media_reason(char *reason, size_t cap, const char *text) {
    if (cap == 0) return;
    size_t n = strlen(text);
    if (n >= cap) n = cap - 1;
    memcpy(reason, text, n);
    reason[n] = '\0';
}

bool cf_media_pinned_available(char *reason, size_t cap) {
    const char *live = getenv("CF_MEDIA_LIVE");
    if (live == NULL || strcmp(live, "1") != 0) {
        media_reason(reason, cap,
                     "BLOCKED: CF_MEDIA_LIVE!=1; live media bytes need CF_MEDIA_LIVE=1 "
                     "with pinned vips " CF_MEDIA_VIPS_VERSION
                     " + ffmpeg/ffprobe " CF_MEDIA_FFMPEG_VERSION);
        return false;
    }
    char text[512];
    char detail[1024];
    text[0] = '\0';
    if (media_version_probe(CF_PROC_VIPS, "--version", text, sizeof text) != CF_OK ||
        strstr(text, CF_MEDIA_VIPS_VERSION) == NULL) {
        snprintf(detail, sizeof detail,
                 "BLOCKED: vips %s not observed (need %s, F00 media probe BLOCKED)",
                 text[0] != '\0' ? text : "absent", CF_MEDIA_VIPS_VERSION);
        /* Keep only the first line of a chatty version banner. */
        detail[strcspn(detail, "\r\n")] = '\0';
        media_reason(reason, cap, detail);
        return false;
    }
    text[0] = '\0';
    if (media_version_probe(CF_PROC_FFMPEG, "-version", text, sizeof text) != CF_OK ||
        strstr(text, CF_MEDIA_FFMPEG_VERSION) == NULL) {
        snprintf(detail, sizeof detail, "BLOCKED: ffmpeg %s not observed (need %s)",
                 text, CF_MEDIA_FFMPEG_VERSION);
        detail[strcspn(detail, "\r\n")] = '\0';
        media_reason(reason, cap, detail);
        return false;
    }
    text[0] = '\0';
    if (media_version_probe(CF_PROC_FFPROBE, "-version", text, sizeof text) != CF_OK ||
        strstr(text, CF_MEDIA_FFPROBE_VERSION) == NULL) {
        snprintf(detail, sizeof detail, "BLOCKED: ffprobe %s not observed (need %s)",
                 text, CF_MEDIA_FFPROBE_VERSION);
        detail[strcspn(detail, "\r\n")] = '\0';
        media_reason(reason, cap, detail);
        return false;
    }
    media_reason(reason, cap, "pinned media tools observed");
    return true;
}
