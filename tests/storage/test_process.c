/* tests/storage/test_process.c — S01 subprocess-boundary tests (STORE-04
 * lifecycle parts): fixed executable enum, direct argv (no shell), input and
 * output FD routing, concurrent pipe draining, stdout cap, retained/truncated
 * stderr, and timeout kill + reap with no surviving child.
 *
 * The pinned libvips 8.16.1 is absent on this host and stays F00-BLOCKED
 * (vendor/DEPS.json: no probe, not installed); the vips case asserts the
 * defined CF_NOT_FOUND behavior of the enum path, and the test checks ffmpeg
 * and ffprobe, which are installed, with their real binary names. The
 * installed ffmpeg/ffprobe are 9.0.2, NOT the pinned 7.1.5 artifacts
 * (DEPS.json media entries are BLOCKED); this suite therefore claims the S01
 * interface and lifecycle only, never media byte parity.
 *
 * Build (direct clang; the Makefile does not list src/storage yet):
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -O1 -g -Isrc -Itests
 *         tests/storage/test_process.c src/storage/process.c
 *         src/core/{alloc,buffer,clock}.c -o build/s01/test_process
 */
#include "cf_test.h"

#include "storage/storage.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static bool span_has(cf_span span, const char *needle) {
    size_t len = strlen(needle);
    if (span.ptr == NULL || len > span.len) return false;
    return memmem(span.ptr, span.len, needle, len) != NULL;
}

static bool binary_present(const char *path) {
    return access(path, X_OK) == 0;
}

static void temp_path(char *out, size_t cap, const char *tag) {
    snprintf(out, cap, "/tmp/cf-s01-%s-%d", tag, (int)getpid());
    (void)unlink(out);
}

static void base_opts(cf_proc_opts *o, cf_proc_exe exe, char *const *args) {
    memset(o, 0, sizeof *o);
    o->exe = exe;
    o->args = args;
    o->stdin_fd = -1;
    o->stdout_fd = -1;
    o->stderr_fd = -1;
    o->stdout_limit = 1 << 20;
    o->timeout_ms = 15000;
}

CF_TEST(executable_table_is_fixed) {
    CF_CHECK(strcmp(cf_proc_executable_path(CF_PROC_FFMPEG), "/usr/bin/ffmpeg") == 0);
    CF_CHECK(strcmp(cf_proc_executable_path(CF_PROC_FFPROBE), "/usr/bin/ffprobe") == 0);
    CF_CHECK(strcmp(cf_proc_executable_path(CF_PROC_VIPS), "/usr/bin/vips") == 0);
    CF_CHECK(strcmp(cf_proc_executable_name(CF_PROC_FFMPEG), "ffmpeg") == 0);
    CF_CHECK(strcmp(cf_proc_executable_name(CF_PROC_FFPROBE), "ffprobe") == 0);
    CF_CHECK(strcmp(cf_proc_executable_name(CF_PROC_VIPS), "vips") == 0);
    CF_CHECK(cf_proc_executable_path((cf_proc_exe)99) == NULL);
    CF_CHECK(cf_proc_executable_name((cf_proc_exe)99) == NULL);
}

CF_TEST(ffmpeg_smoke) {
    const char *path = cf_proc_executable_path(CF_PROC_FFMPEG);
    CF_REQUIRE(binary_present(path)); /* ffmpeg must be installed for this run */
    static char *const args[] = {"-version", NULL};
    cf_proc_opts o;
    base_opts(&o, CF_PROC_FFMPEG, args);
    cf_proc_result r;
    CF_REQUIRE(cf_proc_run(&o, &r) == CF_OK);
    CF_CHECK(r.exited_ok && r.exit_code == 0);
    CF_CHECK(!r.timed_out && !r.output_limit && r.signal == 0);
    CF_CHECK(r.pid > 0);
    CF_REQUIRE(r.out != NULL);
    CF_CHECK(span_has(cf_buf_span(r.out), "ffmpeg version"));
    cf_proc_result_dispose(&r);
}

CF_TEST(ffprobe_smoke) {
    const char *path = cf_proc_executable_path(CF_PROC_FFPROBE);
    CF_REQUIRE(binary_present(path));
    static char *const args[] = {"-version", NULL};
    cf_proc_opts o;
    base_opts(&o, CF_PROC_FFPROBE, args);
    cf_proc_result r;
    CF_REQUIRE(cf_proc_run(&o, &r) == CF_OK);
    CF_CHECK(r.exited_ok && r.exit_code == 0);
    CF_REQUIRE(r.out != NULL);
    CF_CHECK(span_has(cf_buf_span(r.out), "ffprobe version"));
    cf_proc_result_dispose(&r);
}

/* libvips is absent: the enum path must resolve deterministically without the
 * binary (F00-BLOCKED). If a vips ever appears, the same path must still be
 * the one executed. */
CF_TEST(vips_enum_path_before_f00_media) {
    const char *path = cf_proc_executable_path(CF_PROC_VIPS);
    CF_REQUIRE(path != NULL);
    static char *const args[] = {"--version", NULL};
    cf_proc_opts o;
    base_opts(&o, CF_PROC_VIPS, args);
    cf_proc_result r;
    cf_err rc = cf_proc_run(&o, &r);
    if (binary_present(path)) {
        CF_CHECK(rc == CF_OK);
        CF_CHECK(r.exited_ok);
    } else {
        CF_CHECK(rc == CF_NOT_FOUND);
        CF_CHECK(r.pid == -1);
    }
    cf_proc_result_dispose(&r);
}

/* Arguments go to execve directly: shell metacharacters stay literal. */
CF_TEST(argv_is_never_a_shell) {
    const char *path = cf_proc_executable_path(CF_PROC_FFPROBE);
    CF_REQUIRE(binary_present(path));
    char marker[128], argument[192];
    temp_path(marker, sizeof marker, "pwned");
    snprintf(argument, sizeof argument, "$(touch %s)", marker);
    char *const args[] = {"-v", "error", argument, NULL};
    cf_proc_opts o;
    base_opts(&o, CF_PROC_FFPROBE, args);
    cf_proc_result r;
    CF_REQUIRE(cf_proc_run(&o, &r) == CF_OK);
    CF_CHECK(!r.exited_ok); /* a literal "$(touch ...)" path cannot be opened */
    CF_CHECK(r.exit_code != 0);
    CF_REQUIRE(r.err != NULL);
    CF_CHECK(span_has(cf_buf_span(r.err), "$(touch "));
    CF_CHECK(access(marker, F_OK) != 0); /* nothing was executed by a shell */
    cf_proc_result_dispose(&r);
}

static void check_reaped(const cf_proc_result *r) {
    CF_CHECK(r->pid > 0);
    if (r->pid > 0) {
        errno = 0;
        CF_CHECK(kill(r->pid, 0) == -1 && errno == ESRCH);
    }
}

CF_TEST(timeout_kills_group_and_reaps) {
    const char *path = cf_proc_executable_path(CF_PROC_FFMPEG);
    CF_REQUIRE(binary_present(path));
    /* Infinite rawvideo input: runs until its time limit. */
    static char *const args[] = {
        "-hide_banner", "-loglevel", "error",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "16x16", "-r", "25",
        "-i", "/dev/zero", "-f", "null", "-", NULL};
    cf_proc_opts o;
    base_opts(&o, CF_PROC_FFMPEG, args);
    o.timeout_ms = 300;
    cf_proc_result r;
    uint64_t started = cf_monotonic_ms();
    CF_REQUIRE(cf_proc_run(&o, &r) == CF_LIMIT);
    uint64_t elapsed = cf_monotonic_ms() - started;
    CF_CHECK(r.timed_out && !r.output_limit);
    CF_CHECK(elapsed >= 250 && elapsed < 10000);
    check_reaped(&r);
    CF_CHECK(r.err == NULL || cf_buf_span(r.err).len <= CF_PROC_STDERR_RETAIN);
    cf_proc_result_dispose(&r);
}

/* If stdin were not routed, ffmpeg would see EOF and exit before the limit;
 * a timeout proves the caller's pipe FD was inherited and left empty. */
CF_TEST(stdin_fd_is_routed) {
    const char *path = cf_proc_executable_path(CF_PROC_FFMPEG);
    CF_REQUIRE(binary_present(path));
    int pipefd[2];
    CF_REQUIRE(pipe(pipefd) == 0);
    static char *const args[] = {
        "-hide_banner", "-loglevel", "error",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "16x16", "-r", "25",
        "-i", "pipe:0", "-f", "null", "-", NULL};
    cf_proc_opts o;
    base_opts(&o, CF_PROC_FFMPEG, args);
    o.stdin_fd = pipefd[0];
    o.timeout_ms = 300;
    cf_proc_result r;
    CF_REQUIRE(cf_proc_run(&o, &r) == CF_LIMIT);
    CF_CHECK(r.timed_out);
    check_reaped(&r);
    cf_proc_result_dispose(&r);
    close(pipefd[0]);
    close(pipefd[1]);
}

CF_TEST(stdout_limit_kills_and_reaps) {
    const char *path = cf_proc_executable_path(CF_PROC_FFMPEG);
    CF_REQUIRE(binary_present(path));
    static char *const args[] = {
        "-hide_banner", "-loglevel", "error",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "64x64",
        "-i", "/dev/zero", "-frames:v", "1000",
        "-pix_fmt", "rgb24", "-f", "rawvideo", "-", NULL};
    cf_proc_opts o;
    base_opts(&o, CF_PROC_FFMPEG, args);
    o.stdout_limit = 1024;
    cf_proc_result r;
    CF_REQUIRE(cf_proc_run(&o, &r) == CF_LIMIT);
    CF_CHECK(r.output_limit && !r.timed_out);
    check_reaped(&r);
    CF_REQUIRE(r.err != NULL);
    CF_CHECK(cf_buf_span(r.err).len <= CF_PROC_STDERR_RETAIN);
    CF_REQUIRE(r.out != NULL); /* bounded partial capture, never a deadlock */
    CF_CHECK(cf_buf_span(r.out).len <= 1024);
    cf_proc_result_dispose(&r);
}

/* 122880 bytes of stdout is larger than any pipe buffer: the helper must drain
 * while the child runs, and the exact byte count proves no truncation. */
CF_TEST(stdout_drained_without_deadlock) {
    const char *path = cf_proc_executable_path(CF_PROC_FFMPEG);
    CF_REQUIRE(binary_present(path));
    static char *const args[] = {
        "-hide_banner", "-loglevel", "error",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "64x64",
        "-i", "/dev/zero", "-frames:v", "10",
        "-pix_fmt", "rgb24", "-f", "rawvideo", "-", NULL};
    cf_proc_opts o;
    base_opts(&o, CF_PROC_FFMPEG, args);
    o.stdout_limit = 1 << 20;
    cf_proc_result r;
    CF_REQUIRE(cf_proc_run(&o, &r) == CF_OK);
    CF_CHECK(r.exited_ok);
    CF_REQUIRE(r.out != NULL);
    CF_CHECK(cf_buf_span(r.out).len == 122880); /* 10 * 64 * 64 * 3 */
    cf_proc_result_dispose(&r);
}

/* Stderr is drained even past the retained prefix; the prefix is exactly the
 * 64 KiB cap and the truncation flag is set. */
CF_TEST(stderr_retained_prefix_and_truncation) {
    const char *path = cf_proc_executable_path(CF_PROC_FFMPEG);
    CF_REQUIRE(binary_present(path));
    static char *const args[] = {
        "-hide_banner", "-loglevel", "trace",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "64x64",
        "-i", "/dev/zero", "-frames:v", "400", "-f", "null", "-", NULL};
    cf_proc_opts o;
    base_opts(&o, CF_PROC_FFMPEG, args);
    cf_proc_result r;
    CF_REQUIRE(cf_proc_run(&o, &r) == CF_OK);
    CF_CHECK(r.exited_ok);
    CF_REQUIRE(r.err != NULL);
    CF_CHECK(cf_buf_span(r.err).len == CF_PROC_STDERR_RETAIN);
    CF_CHECK(r.err_truncated);
    CF_CHECK(span_has(cf_buf_span(r.err), "Splitting the commandline"));
    cf_proc_result_dispose(&r);
}

CF_TEST(explicit_output_fds_bypass_capture) {
    const char *path = cf_proc_executable_path(CF_PROC_FFPROBE);
    CF_REQUIRE(binary_present(path));
    char out_path[128], err_path[128];
    temp_path(out_path, sizeof out_path, "stdout");
    temp_path(err_path, sizeof err_path, "stderr");
    int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    int err_fd = open(err_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CF_REQUIRE(out_fd >= 3 && err_fd >= 3);
    static char *const args[] = {"-version", NULL};
    cf_proc_opts o;
    base_opts(&o, CF_PROC_FFPROBE, args);
    o.stdout_fd = out_fd;
    o.stderr_fd = err_fd;
    cf_proc_result r;
    CF_REQUIRE(cf_proc_run(&o, &r) == CF_OK);
    CF_CHECK(r.exited_ok);
    CF_CHECK(r.out == NULL && r.err == NULL); /* capture bypassed */
    cf_proc_result_dispose(&r);
    close(out_fd);
    close(err_fd);

    struct stat st;
    CF_REQUIRE(stat(out_path, &st) == 0);
    CF_CHECK(st.st_size > 0);
    unsigned char buf[64];
    int fd = open(out_path, O_RDONLY | O_CLOEXEC);
    CF_REQUIRE(fd >= 0);
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    CF_CHECK(n > 0 && memmem(buf, (size_t)n, "ffprobe version", 15) != NULL);
    (void)unlink(out_path);
    (void)unlink(err_path);
}

CF_TEST(invalid_options) {
    cf_proc_result r;
    CF_CHECK(cf_proc_run(NULL, &r) == CF_INVALID);
    static char *const args[] = {"-version", NULL};
    cf_proc_opts o;
    base_opts(&o, CF_PROC_FFMPEG, args);
    CF_CHECK(cf_proc_run(&o, NULL) == CF_INVALID);
    o.timeout_ms = 0;
    CF_CHECK(cf_proc_run(&o, &r) == CF_INVALID);
    o.timeout_ms = 1000;
    o.exe = (cf_proc_exe)42;
    CF_CHECK(cf_proc_run(&o, &r) == CF_INVALID);
    o.exe = CF_PROC_FFMPEG;
    o.stdin_fd = 1; /* would alias the installed stdout */
    CF_CHECK(cf_proc_run(&o, &r) == CF_INVALID);
    cf_proc_result_dispose(NULL);
}

/* A caller-owned intermediate output file survives the kill (nothing is
 * still writing it) and can be removed after the failed job. */
CF_TEST(timeout_cleans_caller_intermediate) {
    const char *path = cf_proc_executable_path(CF_PROC_FFMPEG);
    CF_REQUIRE(binary_present(path));
    char out_path[128];
    temp_path(out_path, sizeof out_path, "intermediate");
    int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CF_REQUIRE(out_fd >= 3);
    static char *const args[] = {
        "-hide_banner", "-loglevel", "error",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "16x16", "-r", "25",
        "-i", "/dev/zero", "-f", "null", "-", NULL};
    cf_proc_opts o;
    base_opts(&o, CF_PROC_FFMPEG, args);
    o.stdout_fd = out_fd; /* child holds the file's fd, not our descriptor */
    o.timeout_ms = 300;
    cf_proc_result r;
    CF_REQUIRE(cf_proc_run(&o, &r) == CF_LIMIT);
    CF_CHECK(r.timed_out);
    check_reaped(&r);
    cf_proc_result_dispose(&r);
    close(out_fd);
    CF_CHECK(unlink(out_path) == 0); /* the intermediate is task-owned */
    CF_CHECK(access(out_path, F_OK) != 0);
}

static int fd_count(void) {
    DIR *d = opendir("/proc/self/fd");
    if (d == NULL) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        count++;
    }
    closedir(d);
    return count;
}

CF_TEST(no_fd_leak_after_runs) {
    const char *path = cf_proc_executable_path(CF_PROC_FFMPEG);
    CF_REQUIRE(binary_present(path));
    static char *const args[] = {"-version", NULL};
    int before = fd_count();
    CF_REQUIRE(before > 0);
    for (int i = 0; i < 3; i++) {
        cf_proc_opts o;
        base_opts(&o, CF_PROC_FFMPEG, args);
        cf_proc_result r;
        CF_REQUIRE(cf_proc_run(&o, &r) == CF_OK);
        cf_proc_result_dispose(&r);
    }
    int after = fd_count();
    CF_CHECK(after == before);
}

CF_TEST_MAIN()
