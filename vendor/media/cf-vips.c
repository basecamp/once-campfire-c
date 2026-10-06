/* Fixed subprocess adapter for the pinned libvips 8.16.1 build. Operations
 * mirror crates/storage/src/vips.rs; the application never links libvips.
 * The parent owns execution deadlines, fd isolation and output byte limits. */
#include <vips/vips.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static int fail(void) {
    fprintf(stderr, "%s", vips_error_buffer());
    vips_error_clear();
    return 1;
}

static void json_string(const char *s) {
    if (!s) { fputs("null", stdout); return; }
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

static int analyze(const char *path) {
    VipsImage *in = vips_image_new_from_file(path, "access", VIPS_ACCESS_SEQUENTIAL, NULL);
    /* Reference ImageAnalyzer deliberately treats unsupported/corrupt images
     * as empty metadata. Initialization/version errors remain failures. */
    if (!in) { vips_error_clear(); puts("{}"); return 0; }
    char *orientation = NULL;
    if (vips_image_get_typeof(in, "exif-ifd0-Orientation") &&
        vips_image_get_as_string(in, "exif-ifd0-Orientation", &orientation))
        vips_error_clear();
    printf("{\"width\":%d,\"height\":%d,\"exif_orientation\":",
           vips_image_get_width(in), vips_image_get_height(in));
    json_string(orientation);
    puts("}");
    g_free(orientation);
    g_object_unref(in);
    return ferror(stdout) ? 1 : 0;
}

static gboolean accepts_page(const char *path) {
    const char *loader = vips_foreign_find_load(path);
    if (!loader) { vips_error_clear(); return FALSE; }
    VipsOperation *op = vips_operation_new(loader);
    if (!op) { vips_error_clear(); return FALSE; }
    const char **names = NULL;
    int *flags = NULL, n = 0;
    gboolean accepts = FALSE;
    if (vips_object_get_args(VIPS_OBJECT(op), &names, &flags, &n) == 0) {
        for (int i = 0; i < n; i++) {
            gboolean required = (flags[i] & VIPS_ARGUMENT_REQUIRED) &&
                                !(flags[i] & VIPS_ARGUMENT_DEPRECATED);
            if (!strcmp(names[i], "page") && !required &&
                (flags[i] & VIPS_ARGUMENT_CONSTRUCT) &&
                (flags[i] & VIPS_ARGUMENT_INPUT)) accepts = TRUE;
        }
    } else vips_error_clear();
    g_object_unref(op);
    return accepts;
}

static int dimension(const char *text, int *out) {
    char *end = NULL;
    errno = 0;
    long n = strtol(text, &end, 10);
    if (errno || !text[0] || *end || n < 0 || n > INT_MAX) return -1;
    *out = (int)n;
    return 0;
}

/* A bounded target caps only encoded output, unlike RLIMIT_FSIZE which also
 * kills valid operations when libvips spills intermediate pixels to disk.
 * File/target savers share codec options; committed byte vectors verify this. */
struct bounded_target { int fd; gint64 position; };

static gint64 target_write(VipsTargetCustom *target, const void *data, gint64 size,
                           struct bounded_target *out) {
    (void)target;
    if (size < 0 || size > 16777216 - out->position) {
        vips_error("cf-vips", "encoded output exceeds 16 MiB");
        errno = EFBIG;
        return -1;
    }
    const unsigned char *bytes = data;
    gint64 done = 0;
    while (done < size) {
        ssize_t n = write(out->fd, bytes + done, (size_t)(size - done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += n;
        out->position += n;
    }
    return done;
}

static gint64 target_read(VipsTargetCustom *target, void *data, gint64 size,
                          struct bounded_target *out) {
    (void)target;
    if (size < 0 || size > 16777216) return -1;
    ssize_t n;
    do { n = read(out->fd, data, (size_t)size); } while (n < 0 && errno == EINTR);
    if (n > 0) out->position += n;
    return n;
}

static gint64 target_seek(VipsTargetCustom *target, gint64 offset, int whence,
                          struct bounded_target *out) {
    (void)target;
    off_t position = lseek(out->fd, (off_t)offset, whence);
    if (position < 0 || position > 16777216) return -1;
    out->position = position;
    return position;
}

static int bounded_save(VipsImage *image, const char *path, const char *suffix) {
    struct bounded_target out = {open(path, O_RDWR | O_TRUNC | O_CREAT, 0600), 0};
    if (out.fd < 0) { vips_error("cf-vips", "cannot open output"); return -1; }
    VipsTargetCustom *target = vips_target_custom_new();
    if (!target) { close(out.fd); return -1; }
    g_signal_connect(target, "write", G_CALLBACK(target_write), &out);
    g_signal_connect(target, "read", G_CALLBACK(target_read), &out);
    g_signal_connect(target, "seek", G_CALLBACK(target_seek), &out);
    int status = vips_image_write_to_target(image, suffix, VIPS_TARGET(target), NULL);
    g_object_unref(target);
    if (close(out.fd)) status = -1;
    return status;
}

static int transform(const char *input, const char *output, const char *format,
                     const char *width_arg, const char *height_arg) {
    int width, height;
    const char *ext = strrchr(output, '.');
    if ((!strcmp(format, "jpg") || !strcmp(format, "jpeg") ||
         !strcmp(format, "png") || !strcmp(format, "webp") || !strcmp(format, "gif") ||
         !strcmp(format, "avif") || !strcmp(format, "heic") || !strcmp(format, "heif") ||
         !strcmp(format, "tif") || !strcmp(format, "tiff")) &&
        ext && !strcmp(ext + 1, format) &&
        !dimension(width_arg, &width) && !dimension(height_arg, &height)) {
        VipsImage *in = accepts_page(input)
            ? vips_image_new_from_file(input, "page", 0, NULL)
            : vips_image_new_from_file(input, NULL);
        VipsImage *rotated = NULL, *thumb = NULL, *mask = NULL, *sharp = NULL;
        int status = -1;
        if (!in || vips_autorot(in, &rotated, NULL)) goto done;
        if (width || height) {
            if (vips_thumbnail_image(rotated, &thumb, width ? width : 10000000,
                "height", height ? height : 10000000,
                "size", VIPS_SIZE_DOWN, "no_rotate", TRUE, NULL)) goto done;
            const double values[9] = {-1,-1,-1,-1,32,-1,-1,-1,-1};
            mask = vips_image_new_matrix_from_array(3, 3, values, 9);
            if (!mask) goto done;
            vips_image_set_double(mask, "scale", 24.0);
            vips_image_set_double(mask, "offset", 0.0);
            if (vips_conv(thumb, &sharp, mask, "precision", VIPS_PRECISION_INTEGER, NULL)) goto done;
        }
        status = bounded_save(sharp ? sharp : rotated, output, ext);
done:
        if (sharp) g_object_unref(sharp);
        if (mask) g_object_unref(mask);
        if (thumb) g_object_unref(thumb);
        if (rotated) g_object_unref(rotated);
        if (in) g_object_unref(in);
        return status ? fail() : 0;
    }
    fputs("invalid format, extension or dimensions\n", stderr);
    return 2;
}

int main(int argc, char **argv) {
    if (VIPS_INIT(argv[0])) return fail();
    if (strcmp(vips_version_string(), "8.16.1")) {
        fputs("cf-vips requires libvips 8.16.1\n", stderr);
        vips_shutdown();
        return 1;
    }
    vips_block_untrusted_set(TRUE);
    vips_operation_block_set("VipsForeignLoadOpenslide", TRUE);
    int status;
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        printf("vips-%s\n", vips_version_string());
        status = 0;
    } else if (argc == 3 && !strcmp(argv[1], "analyze")) status = analyze(argv[2]);
    else if (argc == 7 && !strcmp(argv[1], "transform"))
        status = transform(argv[2], argv[3], argv[4], argv[5], argv[6]);
    else {
        fputs("usage: cf-vips --version | analyze INPUT | transform INPUT OUTPUT FORMAT WIDTH HEIGHT\n", stderr);
        status = 2;
    }
    vips_shutdown();
    return status;
}
