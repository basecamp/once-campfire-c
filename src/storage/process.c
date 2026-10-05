/* src/storage/process.c — S01 subprocess boundary. See storage.h.
 *
 * The reference (tmp/rust-ref/crates/storage/src/process.rs) runs the pinned
 * media tools with std::process::Command and kills a child that overruns its
 * timeout while draining both pipes. This port keeps those semantics and adds
 * the S01 restrictions: the executable is chosen by an enum whose path and
 * argv[0] are compile-time constants (never a request value, never a shell),
 * the child is its own process group so a timeout kills the whole group, all
 * unrelated FDs are closed before execve, stderr is retained only up to
 * CF_PROC_STDERR_RETAIN while still being drained, and every child is reaped
 * (WNOHANG + blocking wait on the kill paths). There is no `sh -c` path and
 * no string is ever re-interpreted by a shell.
 *
 * vips 8.16.1 / ffmpeg 7.1.5 are the pinned versions (vendor/DEPS.json); the
 * helper does not inspect versions (S03 owns media parity), and all three
 * entries remain F00-BLOCKED because the media probe/provenance is missing. */

#include "storage/storage.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* The only three executables this process may spawn. Absolute, fixed at
 * compile time: no getenv, no PATH search, no caller-supplied path. */
#define CF_PROC_VIPS_PATH "/usr/bin/vips"
#define CF_PROC_FFMPEG_PATH "/usr/bin/ffmpeg"
#define CF_PROC_FFPROBE_PATH "/usr/bin/ffprobe"
#define CF_PROC_VIPS_NAME "vips"
#define CF_PROC_FFMPEG_NAME "ffmpeg"
#define CF_PROC_FFPROBE_NAME "ffprobe"

const char *cf_proc_executable_path(cf_proc_exe exe) {
    switch (exe) {
    case CF_PROC_VIPS: return CF_PROC_VIPS_PATH;
    case CF_PROC_FFMPEG: return CF_PROC_FFMPEG_PATH;
    case CF_PROC_FFPROBE: return CF_PROC_FFPROBE_PATH;
    }
    return NULL;
}

const char *cf_proc_executable_name(cf_proc_exe exe) {
    switch (exe) {
    case CF_PROC_VIPS: return CF_PROC_VIPS_NAME;
    case CF_PROC_FFMPEG: return CF_PROC_FFMPEG_NAME;
    case CF_PROC_FFPROBE: return CF_PROC_FFPROBE_NAME;
    }
    return NULL;
}

static void proc_close(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/* Child-side close of every FD except `keep` (the exec-error pipe). Prefers
 * close_range(2) and falls back to a bounded loop if the kernel/libc lack
 * it; only async-signal-safe syscalls run between fork and exec.
 *
 * Fil-C 0.685 aborts a child on the raw close_range syscall ("unsupported
 * syscall: 436"), so the pizfix build always uses the close loop. */
static void proc_child_close_others(int keep) {
#if defined(SYS_close_range) && !defined(__FILC__)
    bool ranges_ok = true;
    if (keep > 3 &&
        syscall(SYS_close_range, 3u, (unsigned)(keep - 1), 0u) != 0) {
        ranges_ok = false;
    }
    if (ranges_ok && keep < INT_MAX &&
        syscall(SYS_close_range, (unsigned)(keep + 1), ~0u, 0u) != 0) {
        ranges_ok = false;
    }
    if (ranges_ok) return;
#endif
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > (1 << 20)) maxfd = 1 << 20;
    for (int fd = 3; fd < maxfd; fd++) {
        if (fd != keep) (void)close(fd);
    }
}

/* Runs between fork and execve: fixes the child's group/signal state and
 * standard descriptors. Returns only on failure (execve failed), writing the
 * errno to the exec-error pipe before _exit. */
static void proc_child_exec(const char *path, char *const *argv,
                            const cf_proc_opts *o, int nullfd, bool cap_out,
                            int outpipe_w, bool cap_err, int errpipe_w,
                            int execpipe_w) {
    int saved_errno = 0;
    (void)setpgid(0, 0); /* own process group: the timeout kills the group */
    (void)signal(SIGPIPE, SIG_DFL);
    sigset_t empty;
    sigemptyset(&empty);
    (void)sigprocmask(SIG_SETMASK, &empty, NULL);

    int in = o->stdin_fd >= 0 ? o->stdin_fd : nullfd;
    if (dup2(in, STDIN_FILENO) < 0) {
        saved_errno = errno;
        goto fail;
    }
    int out = cap_out ? outpipe_w : o->stdout_fd;
    if (dup2(out, STDOUT_FILENO) < 0) {
        saved_errno = errno;
        goto fail;
    }
    int err = cap_err ? errpipe_w : o->stderr_fd;
    if (dup2(err, STDERR_FILENO) < 0) {
        saved_errno = errno;
        goto fail;
    }
    proc_child_close_others(execpipe_w);
    execve(path, argv, environ);
    saved_errno = errno;
fail:
    if (saved_errno != 0) {
        ssize_t written = write(execpipe_w, &saved_errno, sizeof saved_errno);
        (void)written;
    }
    _exit(127);
}

/* Reads whatever is available now. `truncate`: keep the first `limit` bytes
 * and drop the rest while still draining (stderr). Otherwise a byte beyond
 * `limit` is *hit and CF_LIMIT: the caller kills the group. `eof` reports a
 * clean end so the poll set can drop the FD. */
static cf_err proc_drain(int fd, cf_builder *b, size_t limit, bool truncate,
                         bool *hit, bool *eof) {
    for (;;) {
        unsigned char buf[1 << 16];
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            size_t room = limit > b->len ? limit - b->len : 0;
            size_t take = (size_t)n < room ? (size_t)n : room;
            if (take > 0) {
                cf_err rc = cf_builder_append(b, (cf_span){buf, take});
                if (rc != CF_OK) return rc;
            }
            if (take < (size_t)n) {
                *hit = true;
                if (!truncate) return CF_LIMIT;
            }
        } else if (n == 0) {
            *eof = true;
            return CF_OK;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return CF_OK;
        } else {
            return CF_IO;
        }
    }
}

static void proc_kill_group(pid_t pid) {
    if (pid > 0) (void)kill(-pid, SIGKILL);
}

cf_err cf_proc_run(const cf_proc_opts *o, cf_proc_result *out) {
    if (o == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    out->pid = -1;
    out->exit_code = -1;
    if (o->timeout_ms == 0) return CF_INVALID;
    const char *path = cf_proc_executable_path(o->exe);
    const char *name = cf_proc_executable_name(o->exe);
    if (path == NULL || name == NULL) return CF_INVALID;
    /* Caller FDs must not alias the standard descriptors being installed. */
    if ((o->stdin_fd >= 0 && o->stdin_fd < 3) ||
        (o->stdout_fd >= 0 && o->stdout_fd < 3) ||
        (o->stderr_fd >= 0 && o->stderr_fd < 3)) {
        return CF_INVALID;
    }
    if (access(path, X_OK) != 0) return CF_NOT_FOUND;

    size_t argc = 0;
    while (o->args != NULL && o->args[argc] != NULL) {
        if (argc == CF_PROC_MAX_ARGS) return CF_LIMIT;
        argc++;
    }
    char **argv = malloc((argc + 2) * sizeof *argv);
    if (argv == NULL) return CF_NOMEM;
    argv[0] = (char *)name;
    for (size_t i = 0; i < argc; i++) argv[i + 1] = o->args[i];
    argv[argc + 1] = NULL;

    bool cap_out = o->stdout_fd < 0;
    bool cap_err = o->stderr_fd < 0;
    int nullfd = -1, out_r = -1, out_w = -1, err_r = -1, err_w = -1;
    int exec_r = -1, exec_w = -1;
    cf_err rc = CF_IO;

    if (o->stdin_fd < 0) {
        nullfd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (nullfd < 0) goto before_spawn;
    }
    if (cap_out) {
        int pipefd[2];
        if (pipe2(pipefd, O_CLOEXEC) != 0) goto before_spawn;
        out_r = pipefd[0];
        out_w = pipefd[1];
    }
    if (cap_err) {
        int pipefd[2];
        if (pipe2(pipefd, O_CLOEXEC) != 0) goto before_spawn;
        err_r = pipefd[0];
        err_w = pipefd[1];
    }
    {
        int pipefd[2];
        if (pipe2(pipefd, O_CLOEXEC) != 0) goto before_spawn;
        exec_r = pipefd[0];
        exec_w = pipefd[1];
    }

    pid_t pid = fork();
    if (pid < 0) goto before_spawn;
    if (pid == 0) {
        proc_child_exec(path, argv, o, nullfd, cap_out, out_w, cap_err, err_w,
                        exec_w);
        _exit(127); /* proc_child_exec never returns */
    }

    out->pid = pid;
    proc_close(&nullfd);
    proc_close(&out_w);
    proc_close(&err_w);
    proc_close(&exec_w);
    (void)setpgid(pid, pid); /* race-free with the child's own setpgid */
    if (out_r >= 0) (void)fcntl(out_r, F_SETFL, O_NONBLOCK);
    if (err_r >= 0) (void)fcntl(err_r, F_SETFL, O_NONBLOCK);
    if (exec_r >= 0) (void)fcntl(exec_r, F_SETFL, O_NONBLOCK);

    cf_builder outb = {0}, errb = {0};
    uint64_t start = cf_monotonic_ms();
    uint64_t deadline = start + o->timeout_ms;
    if (deadline < start) deadline = UINT64_MAX; /* saturate, never wrap */
    bool reaped = false, out_eof = false, err_eof = false, kill_group = false;
    int status = 0;
    cf_err drain_rc = CF_OK;

    for (;;) {
        if (out_r >= 0 && !out_eof && drain_rc == CF_OK) {
            rc = proc_drain(out_r, &outb, o->stdout_limit, false,
                            &out->output_limit, &out_eof);
            if (rc == CF_LIMIT) {
                kill_group = true;
            } else if (rc != CF_OK) {
                drain_rc = rc;
                kill_group = true;
            }
        }
        if (err_r >= 0 && !err_eof && drain_rc == CF_OK) {
            rc = proc_drain(err_r, &errb, CF_PROC_STDERR_RETAIN, true,
                            &out->err_truncated, &err_eof);
            if (rc != CF_OK && rc != CF_LIMIT) {
                drain_rc = rc;
                kill_group = true;
            }
        }
        if (kill_group) break;

        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            reaped = true;
            break;
        }
        if (waited < 0 && errno != EINTR) {
            drain_rc = CF_IO;
            kill_group = true;
            break;
        }

        uint64_t now = cf_monotonic_ms();
        if (now >= deadline) {
            out->timed_out = true;
            kill_group = true;
            break;
        }

        struct pollfd pfds[2];
        nfds_t nfds = 0;
        if (out_r >= 0 && !out_eof) {
            pfds[nfds].fd = out_r;
            pfds[nfds].events = POLLIN;
            nfds++;
        }
        if (err_r >= 0 && !err_eof) {
            pfds[nfds].fd = err_r;
            pfds[nfds].events = POLLIN;
            nfds++;
        }
        int wait_ms = (int)(deadline - now > 1000 ? 1000 : deadline - now);
        if (nfds == 0) {
            struct timespec pause = {0, 1000000}; /* child alive, pipes closed */
            (void)nanosleep(&pause, NULL);
        } else if (poll(pfds, nfds, wait_ms) < 0 && errno != EINTR) {
            drain_rc = CF_IO;
            kill_group = true;
            break;
        }
    }

    /* No zombie on any path, including timeout and output-limit kills. */
    if (!reaped) {
        if (kill_group) proc_kill_group(pid);
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) break;
        }
        reaped = true;
    }
    /* Writers are gone (or SIGKILLed): only buffered bytes remain. */
    if (out_r >= 0 && !out_eof && drain_rc == CF_OK) {
        bool hit = false, eof = false;
        (void)proc_drain(out_r, &outb, o->stdout_limit, false, &hit, &eof);
    }
    if (err_r >= 0 && !err_eof && drain_rc == CF_OK) {
        bool hit = false, eof = false;
        (void)proc_drain(err_r, &errb, CF_PROC_STDERR_RETAIN, true, &hit, &eof);
    }
    proc_close(&out_r);
    proc_close(&err_r);

    /* execve failure is reported through its own CLOEXEC pipe. */
    cf_err exec_rc = CF_OK;
    if (exec_r >= 0) {
        int exec_errno = 0;
        ssize_t got = read(exec_r, &exec_errno, sizeof exec_errno);
        proc_close(&exec_r);
        if (got == (ssize_t)sizeof exec_errno) {
            exec_rc = exec_errno == ENOENT ? CF_NOT_FOUND : CF_IO;
        }
    }

    if (WIFEXITED(status)) {
        out->exit_code = WEXITSTATUS(status);
        out->exited_ok = out->exit_code == 0;
    } else if (WIFSIGNALED(status)) {
        out->signal = WTERMSIG(status);
        out->exit_code = -1;
    }

    if (drain_rc != CF_OK) {
        /* Resource failure while collecting: drop partial captures. */
        cf_builder_dispose(&outb);
        cf_builder_dispose(&errb);
    } else {
        if (cap_out) {
            rc = cf_builder_freeze(&outb, &out->out);
            if (rc != CF_OK) drain_rc = rc;
        }
        cf_builder_dispose(&outb);
        if (cap_err) {
            rc = cf_builder_freeze(&errb, &out->err);
            if (rc != CF_OK) drain_rc = rc;
        }
        cf_builder_dispose(&errb);
    }
    free(argv);

    if (exec_rc != CF_OK) return exec_rc;
    if (drain_rc != CF_OK) return drain_rc;
    if (out->timed_out || out->output_limit) return CF_LIMIT;
    return CF_OK;

before_spawn:
    proc_close(&nullfd);
    proc_close(&out_r);
    proc_close(&out_w);
    proc_close(&err_r);
    proc_close(&err_w);
    proc_close(&exec_r);
    proc_close(&exec_w);
    free(argv);
    return rc;
}

void cf_proc_result_dispose(cf_proc_result *r) {
    if (r == NULL) return;
    cf_buf_release(r->out);
    cf_buf_release(r->err);
    memset(r, 0, sizeof *r);
    r->pid = -1;
    r->exit_code = -1;
}
