/* tests/storage/test_media.c — S03 exact media operations (STORE-04).
 *
 * Deterministic argv-builder vectors for every transform class, scalar
 * validation rejections, timeout/kill/reap + intermediate cleanup (local
 * fork/sleep probe inside this file only), cap enforcement, checksum and
 * failure-path error propagation — all without executing the pinned media
 * tools. The one live case (live_pinned_tool_bytes) requires
 * CF_MEDIA_LIVE=1 AND pinned vips 8.16.1 + ffmpeg/ffprobe 7.1.5; otherwise
 * it fails with its BLOCKED reason (never a silent skip or PASS).
 *
 * Targeted build (the Makefile does not list src/storage yet):
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra \
 *         -Werror -pthread -O1 -g -Isrc -Itests \
 *         -Ivendor/build/openssl-clang/install/include \
 *         tests/storage/test_media.c src/storage/media.c src/storage/process.c \
 *         src/core/alloc.c src/core/buffer.c src/core/clock.c \
 *         vendor/build/openssl-clang/install/lib/libcrypto.a -ldl \
 *         -o build/s03/test_media
 */
#include "cf_test.h"

#include "storage/media.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static cf_span span(const char *s) {
    return (cf_span){(const unsigned char *)s, strlen(s)};
}

static void check_argv_eq(cf_media_argv *built, const char *const *want, int wantc) {
    CF_REQUIRE(built->argc == wantc);
    for (int i = 0; i < wantc; i++) {
        CF_REQUIRE(built->argv[i] != NULL);
        CF_CHECK(strcmp(built->argv[i], want[i]) == 0);
    }
    CF_REQUIRE(built->argv[wantc] == NULL);
}

CF_TEST(media_analyzer_selection) {
    CF_CHECK(cf_media_analyzer_for_content_type(span("image/png")) == CF_MEDIA_ANALYZER_IMAGE);
    CF_CHECK(cf_media_analyzer_for_content_type(span("image/svg+xml")) == CF_MEDIA_ANALYZER_IMAGE);
    CF_CHECK(cf_media_analyzer_for_content_type(span("video/mp4")) == CF_MEDIA_ANALYZER_VIDEO);
    CF_CHECK(cf_media_analyzer_for_content_type(span("audio/mpeg")) == CF_MEDIA_ANALYZER_AUDIO);
    CF_CHECK(cf_media_analyzer_for_content_type(span("application/pdf")) == CF_MEDIA_ANALYZER_NULL);
    CF_CHECK(cf_media_analyzer_for_content_type(span("")) == CF_MEDIA_ANALYZER_NULL);
    CF_CHECK(cf_media_analyzer_for_content_type((cf_span){NULL, 0}) == CF_MEDIA_ANALYZER_NULL);
    /* Prefix order matches analyze.rs: "image" wins wherever it leads. */
    CF_CHECK(cf_media_analyzer_for_content_type(span("images")) == CF_MEDIA_ANALYZER_IMAGE);
}

CF_TEST(media_exif_orientation_rule) {
    CF_CHECK(cf_media_exif_swaps_dimensions(span("Right-top (Mirror horizontal and rotate 270 CW)")));
    CF_CHECK(cf_media_exif_swaps_dimensions(span("Left-bottom (Mirror horizontal and rotate 90 CW)")));
    CF_CHECK(cf_media_exif_swaps_dimensions(span("Top-right (Mirror horizontal)")));
    CF_CHECK(cf_media_exif_swaps_dimensions(span("Bottom-left (Mirror vertical)")));
    CF_CHECK(!cf_media_exif_swaps_dimensions(span("Top-left")));
    CF_CHECK(!cf_media_exif_swaps_dimensions(span("Bottom-right")));
    CF_CHECK(!cf_media_exif_swaps_dimensions(span("")));
}

CF_TEST(media_angle_swap_rule) {
    CF_CHECK(cf_media_angle_swaps_dimensions(90));
    CF_CHECK(cf_media_angle_swaps_dimensions(270));
    CF_CHECK(cf_media_angle_swaps_dimensions(-90));
    CF_CHECK(cf_media_angle_swaps_dimensions(-270));
    CF_CHECK(!cf_media_angle_swaps_dimensions(0));
    CF_CHECK(!cf_media_angle_swaps_dimensions(180));
    CF_CHECK(!cf_media_angle_swaps_dimensions(-180));
    CF_CHECK(!cf_media_angle_swaps_dimensions(45));
}

CF_TEST(media_resize_validation) {
    cf_media_resize r;
    CF_CHECK(cf_media_resize_validate(true, 512, true, 512, &r) == CF_OK);
    CF_CHECK(r.has_width && r.width == 512 && r.has_height && r.height == 512);
    CF_CHECK(cf_media_resize_validate(true, 1200, true, 800, &r) == CF_OK);
    CF_CHECK(cf_media_resize_validate(false, 0, true, 800, &r) == CF_OK);
    CF_CHECK(!r.has_width && r.has_height && r.height == 800);
    CF_CHECK(cf_media_resize_validate(true, 1, false, 0, &r) == CF_OK);
    CF_CHECK(cf_media_resize_validate(true, CF_MEDIA_MAX_DIM, true, CF_MEDIA_MAX_DIM, &r) == CF_OK);
    CF_CHECK(cf_media_resize_validate(true, INT64_C(2147483647), true, 100, &r) == CF_OK);
    /* Rejections: blank pair, zero/negative, over MAX_COORD, over i32. */
    CF_CHECK(cf_media_resize_validate(false, 0, false, 0, &r) == CF_INVALID);
    CF_CHECK(cf_media_resize_validate(true, 0, true, 100, &r) == CF_INVALID);
    CF_CHECK(cf_media_resize_validate(true, -5, true, 100, &r) == CF_INVALID);
    CF_CHECK(cf_media_resize_validate(true, 100, true, -1, &r) == CF_INVALID);
    CF_CHECK(cf_media_resize_validate(true, CF_MEDIA_MAX_DIM + 1, true, 100, &r) == CF_OK);
    CF_CHECK(cf_media_resize_validate(true, INT64_C(2147483647) + 1, true, 100, &r) == CF_INVALID);
    CF_CHECK(cf_media_resize_validate(true, 512, true, 512, NULL) == CF_INVALID);
}

CF_TEST(media_format_validation) {
    const char *valid[] = {"png", "PNG", "jpg", "Jpeg", "gif", "webp", "WEBP",
                           "avif", "heic", "heif", "tiff", "tif"};
    for (size_t i = 0; i < sizeof valid / sizeof valid[0]; i++) {
        CF_CHECK(cf_media_format_valid(span(valid[i])));
        CF_REQUIRE(cf_media_content_type_for_format(span(valid[i])) != NULL);
    }
    /* bmp/ico/psd stay unloadable (vips.rb removes them from variable). */
    const char *blocked[] = {"bmp", "ico", "psd", "svg", "pdf", "", "png ",
                            "we bp", "-webp", "png;rm", "../png", "jpeg2000"};
    for (size_t i = 0; i < sizeof blocked / sizeof blocked[0]; i++) {
        CF_CHECK(!cf_media_format_valid(span(blocked[i])));
        CF_CHECK(cf_media_content_type_for_format(span(blocked[i])) == NULL);
    }
    CF_CHECK(!cf_media_format_valid((cf_span){NULL, 0}));
}

CF_TEST(media_format_content_types) {
    CF_CHECK(strcmp(cf_media_content_type_for_format(span("png")), "image/png") == 0);
    CF_CHECK(strcmp(cf_media_content_type_for_format(span("jpg")), "image/jpeg") == 0);
    CF_CHECK(strcmp(cf_media_content_type_for_format(span("jpeg")), "image/jpeg") == 0);
    CF_CHECK(strcmp(cf_media_content_type_for_format(span("gif")), "image/gif") == 0);
    CF_CHECK(strcmp(cf_media_content_type_for_format(span("webp")), "image/webp") == 0);
    CF_CHECK(strcmp(cf_media_content_type_for_format(span("avif")), "image/avif") == 0);
    CF_CHECK(strcmp(cf_media_content_type_for_format(span("heic")), "image/heic") == 0);
    CF_CHECK(strcmp(cf_media_content_type_for_format(span("heif")), "image/heif") == 0);
    CF_CHECK(strcmp(cf_media_content_type_for_format(span("tiff")), "image/tiff") == 0);
}

CF_TEST(media_loader_restrictions) {
    const char *allowed[] = {"image/png", "image/gif", "image/jpeg", "image/tiff",
                             "image/webp", "image/avif", "image/heic", "image/heif"};
    for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++) {
        CF_CHECK(cf_media_loader_allowed(span(allowed[i])));
    }
    /* Blocked loaders are rejected even though the tool could read them. */
    const char *denied[] = {"image/bmp", "image/vnd.microsoft.icon",
                            "image/vnd.adobe.photoshop", "image/svg+xml",
                            "application/pdf", "video/mp4", "", "image/png "};
    for (size_t i = 0; i < sizeof denied / sizeof denied[0]; i++) {
        CF_CHECK(!cf_media_loader_allowed(span(denied[i])));
    }
    CF_CHECK(!cf_media_loader_allowed((cf_span){NULL, 0}));
}

CF_TEST(media_preset_vectors) {
    cf_media_resize r;
    const char *format = NULL;
    CF_REQUIRE(cf_media_preset_describe(CF_MEDIA_THUMB, &r, &format) == CF_OK);
    CF_CHECK(r.width == 1200 && r.height == 800 && strcmp(format, "png") == 0);
    CF_REQUIRE(cf_media_preset_describe(CF_MEDIA_POSTER, &r, &format) == CF_OK);
    CF_CHECK(r.width == 1200 && r.height == 800 && strcmp(format, "webp") == 0);
    CF_REQUIRE(cf_media_preset_describe(CF_MEDIA_AVATAR, &r, &format) == CF_OK);
    CF_CHECK(r.width == 512 && r.height == 512 && strcmp(format, "webp") == 0);
    CF_REQUIRE(cf_media_preset_describe(CF_MEDIA_LOGO_SMALL, &r, &format) == CF_OK);
    CF_CHECK(r.width == 192 && r.height == 192 && strcmp(format, "png") == 0);
    CF_REQUIRE(cf_media_preset_describe(CF_MEDIA_LOGO_LARGE, &r, &format) == CF_OK);
    CF_CHECK(r.width == 512 && r.height == 512 && strcmp(format, "png") == 0);
    CF_CHECK(cf_media_preset_describe(CF_MEDIA_LOGO_LARGE, NULL, &format) == CF_INVALID);
    CF_CHECK(cf_media_preset_describe(CF_MEDIA_LOGO_LARGE, &r, NULL) == CF_INVALID);
    CF_CHECK(cf_media_preset_describe((cf_media_preset)99, &r, &format) == CF_INVALID);
}

CF_TEST(media_ffprobe_argv) {
    cf_media_argv built;
    CF_REQUIRE(cf_media_ffprobe_argv(&built, "/t/in.mp4") == CF_OK);
    const char *const want[] = {"-print_format", "json", "-show_streams",
                                "-show_format", "-v", "error", "/t/in.mp4"};
    check_argv_eq(&built, want, 7);
    CF_CHECK(cf_media_ffprobe_argv(&built, NULL) == CF_INVALID);
    CF_CHECK(cf_media_ffprobe_argv(&built, "") == CF_INVALID);
    CF_CHECK(cf_media_ffprobe_argv(NULL, "/t/in.mp4") == CF_INVALID);
}

CF_TEST(media_poster_argv) {
    cf_media_argv built;
    CF_REQUIRE(cf_media_poster_argv(&built, "/t/in.mp4") == CF_OK);
    const char *const want[] = {
        "-i", "/t/in.mp4", "-vf",
        "select=eq(n\\,0)+eq(key\\,1)+gt(scene\\,0.015),loop=loop=-1:size=2,trim=start_frame=1",
        "-frames:v", "1", "-f", "image2", "-"};
    check_argv_eq(&built, want, 9);
    CF_CHECK(cf_media_poster_argv(&built, NULL) == CF_INVALID);
    CF_CHECK(cf_media_poster_argv(&built, "") == CF_INVALID);
}

CF_TEST(media_vips_thumbnail_argv) {
    cf_media_argv built;
    cf_media_resize r = {.has_width = true, .width = 512, .has_height = true, .height = 512};
    CF_REQUIRE(cf_media_vips_thumbnail_argv(&built, "/t/in.jpg", "/t/out.webp", &r) == CF_OK);
    const char *const want[] = {"thumbnail", "/t/in.jpg", "/t/out.webp", "512",
                                "--height", "512", "--size", "down", "--no-rotate"};
    check_argv_eq(&built, want, 9);
    /* Absent sides widen to MAX_COORD, exactly like the reference. */
    cf_media_resize honly = {.has_width = false, .width = 0, .has_height = true, .height = 800};
    CF_REQUIRE(cf_media_vips_thumbnail_argv(&built, "/t/in.jpg", "/t/o.png", &honly) == CF_OK);
    const char *const wanth[] = {"thumbnail", "/t/in.jpg", "/t/o.png", "10000000",
                                 "--height", "800", "--size", "down", "--no-rotate"};
    check_argv_eq(&built, wanth, 9);
    cf_media_resize bad = {.has_width = false, .width = 0, .has_height = false, .height = 0};
    CF_CHECK(cf_media_vips_thumbnail_argv(&built, "/t/i", "/t/o", &bad) == CF_INVALID);
    cf_media_resize over = {.has_width = true, .width = 0, .has_height = true, .height = 8};
    (void)over;
    cf_media_resize neg = {.has_width = true, .width = -3, .has_height = true, .height = 8};
    CF_CHECK(cf_media_vips_thumbnail_argv(&built, "/t/i", "/t/o", &neg) == CF_INVALID);
    CF_CHECK(cf_media_vips_thumbnail_argv(&built, NULL, "/t/o", &r) == CF_INVALID);
    CF_CHECK(cf_media_vips_thumbnail_argv(&built, "/t/i", NULL, &r) == CF_INVALID);
    CF_CHECK(cf_media_vips_thumbnail_argv(NULL, "/t/i", "/t/o", &r) == CF_INVALID);
    CF_CHECK(cf_media_vips_thumbnail_argv(&built, "/t/i", "/t/o", NULL) == CF_INVALID);
}

CF_TEST(media_vips_sharpen_argv) {
    cf_media_argv built;
    CF_REQUIRE(cf_media_vips_sharpen_argv(&built, "/t/mid.png", "/t/out.webp", "/t/mask.mat") == CF_OK);
    const char *const want[] = {"conv", "/t/mid.png", "/t/out.webp", "/t/mask.mat",
                                "--precision", "integer"};
    check_argv_eq(&built, want, 6);
    CF_CHECK(cf_media_vips_sharpen_argv(&built, NULL, "/t/o", "/t/m") == CF_INVALID);
    CF_CHECK(cf_media_vips_sharpen_argv(&built, "/t/i", "", "/t/m") == CF_INVALID);
    CF_CHECK(cf_media_vips_sharpen_argv(&built, "/t/i", "/t/o", NULL) == CF_INVALID);
}

CF_TEST(media_proc_opts_limits) {
    cf_media_argv built;
    cf_proc_opts opts;
    CF_REQUIRE(cf_media_ffprobe_argv(&built, "/t/in.mp4") == CF_OK);
    CF_REQUIRE(cf_media_proc_opts(&opts, CF_PROC_FFPROBE, &built,
                                  CF_MEDIA_METADATA_MAX_BYTES,
                                  CF_MEDIA_FFPROBE_TIMEOUT_MS) == CF_OK);
    CF_CHECK(opts.timeout_ms == UINT64_C(30000));
    CF_CHECK(opts.stdout_limit == (size_t)1048576);
    CF_CHECK(opts.stdin_fd == -1 && opts.stdout_fd == -1 && opts.stderr_fd == -1);
    CF_REQUIRE(cf_media_poster_argv(&built, "/t/in.mp4") == CF_OK);
    CF_REQUIRE(cf_media_proc_opts(&opts, CF_PROC_FFMPEG, &built,
                                  CF_MEDIA_OUTPUT_MAX_BYTES,
                                  CF_MEDIA_FFMPEG_TIMEOUT_MS) == CF_OK);
    CF_CHECK(opts.timeout_ms == UINT64_C(60000));
    CF_CHECK(opts.stdout_limit == (size_t)16777216);
    cf_media_resize r = {.has_width = true, .width = 8, .has_height = true, .height = 8};
    CF_REQUIRE(cf_media_vips_thumbnail_argv(&built, "/t/i", "/t/o", &r) == CF_OK);
    CF_REQUIRE(cf_media_proc_opts(&opts, CF_PROC_VIPS, &built,
                                  CF_MEDIA_OUTPUT_MAX_BYTES,
                                  CF_MEDIA_VIPS_TIMEOUT_MS) == CF_OK);
    CF_CHECK(opts.timeout_ms == UINT64_C(60000));
    CF_CHECK(cf_media_proc_opts(NULL, CF_PROC_VIPS, &built, 64, 1000) == CF_INVALID);
    CF_CHECK(cf_media_proc_opts(&opts, CF_PROC_VIPS, NULL, 64, 1000) == CF_INVALID);
    CF_CHECK(cf_media_proc_opts(&opts, CF_PROC_VIPS, &built, 0, 1000) == CF_INVALID);
    CF_CHECK(cf_media_proc_opts(&opts, CF_PROC_VIPS, &built, 64, 0) == CF_INVALID);
    CF_CHECK(cf_media_proc_opts(&opts, (cf_proc_exe)99, &built, 64, 1000) == CF_INVALID);
}

CF_TEST(media_check_result_mapping) {
    cf_proc_result res;
    memset(&res, 0, sizeof res);
    res.pid = -1;
    CF_CHECK(cf_media_check_result(&res) == CF_IO); /* nothing spawned */
    CF_CHECK(cf_media_check_result(NULL) == CF_INVALID);
    memset(&res, 0, sizeof res);
    res.pid = 1234;
    res.exited_ok = true;
    CF_CHECK(cf_media_check_result(&res) == CF_OK);
    res.exited_ok = false;
    res.exit_code = 1;
    CF_CHECK(cf_media_check_result(&res) == CF_IO); /* failed transform: explicit error */
    res.exit_code = -1;
    res.signal = 11;
    CF_CHECK(cf_media_check_result(&res) == CF_IO);
    res.signal = 0;
    res.timed_out = true;
    CF_CHECK(cf_media_check_result(&res) == CF_LIMIT);
    res.timed_out = false;
    res.output_limit = true;
    CF_CHECK(cf_media_check_result(&res) == CF_LIMIT);
}

CF_TEST(media_stdout_cap_enforced) {
    if (access("/usr/bin/ffprobe", X_OK) != 0) {
        CF_REQUIRE(false); /* installed here; absence fails loudly, never skips */
    }
    char *const args[] = {"-version", NULL};
    cf_proc_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.exe = CF_PROC_FFPROBE;
    opts.args = args;
    opts.stdin_fd = -1;
    opts.stdout_fd = -1;
    opts.stdout_limit = 16; /* ffprobe -version banners past 16 bytes */
    opts.stderr_fd = -1;
    opts.timeout_ms = 15000;
    cf_proc_result res;
    memset(&res, 0, sizeof res);
    cf_err rc = cf_proc_run(&opts, &res);
    CF_CHECK(rc == CF_LIMIT);
    CF_CHECK(res.output_limit);
    cf_proc_result_dispose(&res);
}

CF_TEST(media_failure_path_propagates) {
    cf_media_argv built;
    CF_REQUIRE(cf_media_ffprobe_argv(&built, "/nonexistent-cf-media-dir/in.mp4") == CF_OK);
    cf_proc_opts opts;
    CF_REQUIRE(cf_media_proc_opts(&opts, CF_PROC_FFPROBE, &built,
                                  CF_MEDIA_METADATA_MAX_BYTES,
                                  CF_MEDIA_FFPROBE_TIMEOUT_MS) == CF_OK);
    cf_proc_result res;
    memset(&res, 0, sizeof res);
    cf_err rc = cf_proc_run(&opts, &res);
    CF_REQUIRE(rc == CF_OK); /* spawned and reaped; the failure is in the status */
    CF_CHECK(!res.exited_ok);
    CF_CHECK(cf_media_check_result(&res) == CF_IO);
    cf_proc_result_dispose(&res);
}

CF_TEST(media_slots_bounded) {
    CF_REQUIRE(cf_media_slots_held() == 0);
    CF_REQUIRE(cf_media_slots_acquire() == CF_OK);
    CF_REQUIRE(cf_media_slots_acquire() == CF_OK);
    CF_REQUIRE(cf_media_slots_acquire() == CF_OK);
    CF_REQUIRE(cf_media_slots_acquire() == CF_OK);
    CF_CHECK(cf_media_slots_held() == 4);
    CF_CHECK(cf_media_slots_try_acquire() == CF_BUSY); /* fifth job refused */
    cf_media_slots_release();
    CF_CHECK(cf_media_slots_held() == 3);
    CF_CHECK(cf_media_slots_try_acquire() == CF_OK);
    CF_CHECK(cf_media_slots_held() == 4);
    cf_media_slots_release();
    cf_media_slots_release();
    cf_media_slots_release();
    cf_media_slots_release();
    CF_CHECK(cf_media_slots_held() == 0);
}

CF_TEST(media_temp_lifecycle) {
    char path[256];
    int fd = -1;
    CF_REQUIRE(cf_media_temp_create("/tmp/opencode", ".png", path, sizeof path, &fd) == CF_OK);
    CF_REQUIRE(fd >= 0);
    CF_CHECK(access(path, F_OK) == 0);
    const char bytes[] = "intermediate";
    CF_REQUIRE(write(fd, bytes, sizeof bytes - 1) == (ssize_t)(sizeof bytes - 1));
    close(fd);
    CF_REQUIRE(cf_media_temp_cleanup(path) == CF_OK);
    CF_CHECK(access(path, F_OK) != 0 && errno == ENOENT);
    CF_CHECK(cf_media_temp_cleanup(path) == CF_OK); /* double cleanup is safe */
    CF_CHECK(cf_media_temp_cleanup(NULL) == CF_INVALID);
    CF_CHECK(cf_media_temp_cleanup("") == CF_INVALID);
    CF_CHECK(cf_media_temp_create("/tmp/opencode", ".png;rm", path, sizeof path, &fd) == CF_INVALID);
    CF_CHECK(cf_media_temp_create("/nonexistent-cf-media-dir", ".png", path, sizeof path, &fd) == CF_IO);
    CF_CHECK(cf_media_temp_create(NULL, ".png", path, sizeof path, &fd) == CF_INVALID);
}

CF_TEST(media_checksum_vector) {
    char path[256];
    int fd = -1;
    CF_REQUIRE(cf_media_temp_create("/tmp/opencode", ".bin", path, sizeof path, &fd) == CF_OK);
    CF_REQUIRE(write(fd, "abc", 3) == 3);
    close(fd);
    cf_builder out = {0};
    CF_REQUIRE(cf_media_checksum_file(path, &out) == CF_OK);
    /* base64(MD5("abc")) == key.rs::checksum_file for the same bytes. */
    const char *want = "kAFQmDzST7DWlj99KOF/cg==";
    CF_CHECK(out.len == strlen(want) && memcmp(out.ptr, want, out.len) == 0);
    cf_builder_dispose(&out);
    CF_REQUIRE(cf_media_temp_cleanup(path) == CF_OK);
    cf_builder missing = {0};
    CF_CHECK(cf_media_checksum_file(path, &missing) == CF_IO);
    cf_builder_dispose(&missing);
    cf_builder bad = {0};
    CF_CHECK(cf_media_checksum_file(NULL, &bad) == CF_INVALID);
    cf_builder_dispose(&bad);
}

CF_TEST(media_timeout_kill_reap_and_cleanup) {
    /* Local lifecycle probe (this file only): a real /bin/sleep child is
     * SIGKILLed after a deadline, reaped with no zombie left, and its
     * task-owned intermediate is always unlinked. */
    char path[256];
    int fd = -1;
    CF_REQUIRE(cf_media_temp_create("/tmp/opencode", ".mid", path, sizeof path, &fd) == CF_OK);
    close(fd);
    pid_t pid = fork();
    CF_REQUIRE(pid >= 0);
    if (pid == 0) {
        execl("/bin/sleep", "sleep", "30", (char *)NULL);
        _exit(127);
    }
    struct timespec deadline = {0, 200 * 1000 * 1000};
    nanosleep(&deadline, NULL);
    CF_REQUIRE(kill(pid, SIGKILL) == 0);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int status = 0;
    CF_REQUIRE(waitpid(pid, &status, 0) == pid);
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000 +
                      (end.tv_nsec - start.tv_nsec) / 1000000;
    CF_CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    CF_CHECK(elapsed_ms < 5000);
    CF_CHECK(kill(pid, 0) != 0 && errno == ESRCH); /* reaped: no zombie */
    CF_REQUIRE(cf_media_temp_cleanup(path) == CF_OK);
    CF_CHECK(access(path, F_OK) != 0);
}

CF_TEST(media_gate_reports_blocked) {
    CF_REQUIRE(setenv("CF_MEDIA_LIVE", "0", 1) == 0);
    char reason[512];
    memset(reason, 0, sizeof reason);
    CF_CHECK(!cf_media_pinned_available(reason, sizeof reason));
    CF_CHECK(reason[0] != '\0');
    CF_CHECK(strstr(reason, "BLOCKED") != NULL);
    unsetenv("CF_MEDIA_LIVE");
    memset(reason, 0, sizeof reason);
    CF_CHECK(!cf_media_pinned_available(reason, sizeof reason));
    CF_CHECK(strstr(reason, "BLOCKED") != NULL);
}

/* Live pinned-tool bytes. Requires CF_MEDIA_LIVE=1 AND pinned
 * vips 8.16.1 + ffmpeg/ffprobe 7.1.5; any version drift fails here with
 * its BLOCKED reason instead of silently skipping or passing. */
CF_TEST(live_pinned_tool_bytes) {
    char reason[1024];
    memset(reason, 0, sizeof reason);
    if (!cf_media_pinned_available(reason, sizeof reason)) {
        fprintf(stderr, "BLOCKED live media bytes: %s\n", reason);
        CF_REQUIRE(false);
        return;
    }
    /* Pinned tools present: the ffprobe contract runs against a
     * task-owned input and must exit 0 with captured JSON metadata. */
    char path[256];
    int fd = -1;
    CF_REQUIRE(cf_media_temp_create("/tmp/opencode", ".raw", path, sizeof path, &fd) == CF_OK);
    static const unsigned char tiny_png[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
        0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
        0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xFB, 0x8E, 0xA3, 0x00, 0x00, 0x00,
        0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    CF_REQUIRE(write(fd, tiny_png, sizeof tiny_png) == (ssize_t)sizeof tiny_png);
    close(fd);
    cf_media_argv built;
    CF_REQUIRE(cf_media_ffprobe_argv(&built, path) == CF_OK);
    cf_proc_opts opts;
    CF_REQUIRE(cf_media_proc_opts(&opts, CF_PROC_FFPROBE, &built,
                                  CF_MEDIA_METADATA_MAX_BYTES,
                                  CF_MEDIA_FFPROBE_TIMEOUT_MS) == CF_OK);
    cf_proc_result res;
    memset(&res, 0, sizeof res);
    CF_REQUIRE(cf_proc_run(&opts, &res) == CF_OK);
    CF_CHECK(cf_media_check_result(&res) == CF_OK);
    CF_REQUIRE(res.out != NULL);
    cf_span outspan = cf_buf_span(res.out);
    CF_CHECK(outspan.len > 0);
    cf_proc_result_dispose(&res);
    CF_REQUIRE(cf_media_temp_cleanup(path) == CF_OK);
}

CF_TEST_MAIN()
