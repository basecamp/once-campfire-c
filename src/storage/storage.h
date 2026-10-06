/* src/storage/storage.h — task S01: storage paths, upload staging and the
 * safe subprocess boundary (05-storage-integrations.md "S01").
 *
 * Sources translated:
 *   tmp/rust-ref/crates/storage/src/disk.rs   (folder_for, upload/delete/
 *       delete_prefixed/open semantics, <root>/<key[0:2]>/<key[2:4]>/<key>)
 *   tmp/rust-ref/crates/storage/src/key.rs    (KEY_LENGTH 28 base36 keys,
 *       base64(MD5) whole-content checksum)
 *   tmp/rust-ref/crates/storage/src/process.rs (bounded child with kill/reap
 *       and drained pipes; this port replaces the missing libvips call with a
 *       fixed executable enum and deletes the shell entirely)
 *
 * This is the module's cross-module surface (consumed by S02/S03/J02/H02).
 * No function here accepts a filename, URL or record field as a path: only a
 * validated storage key reaches the filesystem, every path is resolved with
 * openat(2) relative to the owned root directory FD with O_NOFOLLOW on every
 * component, and input filenames remain display metadata.
 *
 * Error mapping used throughout:
 *   CF_INVALID     NULL/empty arguments, malformed key, wrong object state.
 *   CF_NOT_FOUND   key/executable absent (reads; missing deletes are CF_OK).
 *   CF_LIMIT       upload byte cap, captured-output cap, time limit, depth cap.
 *   CF_BUSY        final key already exists; nothing was modified (move only).
 *   CF_IO          any other syscall/pipe/exec failure.
 *
 * Deletion contract (STORE-03 is enforced by S02's transaction): the caller
 * rechecks references before calling a delete function; these functions only
 * ever remove a path derived from a validated storage key. A missing file is
 * success; a permission/I/O failure is returned as CF_IO for the job to
 * record as failed. */
#ifndef CF_STORAGE_STORAGE_H
#define CF_STORAGE_STORAGE_H

#include "cf.h"

#include <sys/types.h> /* pid_t */

/* A reference-generated key: SecureRandom.base36(28) (key.rs::KEY_LENGTH).
 * Exactly this alphabet and length; anything else is rejected. */
#define CF_STORAGE_KEY_LENGTH 28

/* The request body cap shared with H02's multipart parsing (01: "16 MiB
 * decoded request body, uploads included"). Enforced while receiving bytes,
 * never by buffering. */
#define CF_STORAGE_UPLOAD_MAX_BYTES UINT64_C(16777216)
/* Bounded imported webhook replies; browser uploads retain their16MiB cap. */
#define CF_STORAGE_IMPORT_MAX_BYTES UINT64_C(104857600)

/* ---- disk service ------------------------------------------------------- */

/* Open (creating, 0700, if absent) a disk storage service rooted at
 * root_path, and keep an owned O_DIRECTORY|O_NOFOLLOW FD for every later
 * operation. root_path is configuration (STORAGE_PATH), never request data.
 * A staging directory <root>/tmp (0700) is created inside the same
 * filesystem; a crash can leave files there (no recovery service in this
 * experiment). */
cf_err cf_storage_open(const char *root_path, cf_storage **out);
void cf_storage_close(cf_storage *s); /* NULL-safe; resets *state */

/* True when key is exactly CF_STORAGE_KEY_LENGTH [0-9a-z] bytes. Rejects
 * empty, longer, uppercase, '/', '.', '..' and embedded NUL. */
bool cf_storage_key_valid(cf_span key);

/* Append one reference key (base36, uniform over the alphabet) to out. */
cf_err cf_storage_key_generate(cf_builder *out);

/* Open the blob file for reading relative to the root FD (every component
 * O_NOFOLLOW). On CF_OK *out_fd owns a regular file positioned at 0 and
 * *out_size is its length; the caller closes the FD (cf_response_file takes
 * ownership when used for a response body). CF_NOT_FOUND for an absent key
 * or a non-regular file. */
cf_err cf_storage_open_read(cf_storage *s, cf_span key, int *out_fd,
                            uint64_t *out_size);

/* base64(MD5) of the whole file, streamed (key.rs::checksum_file). */
cf_err cf_storage_checksum(cf_storage *s, cf_span key, cf_builder *out);

/* ---- upload staging ----------------------------------------------------- */

/* Staged bytes are a unique 0600 regular file under <root>/tmp, created
 * O_EXCL|O_NOFOLLOW. Move publishes them at their final key without ever
 * overwriting an existing key (hard link, same filesystem); the final file
 * exists before the DB write and is deleted on rollback, so an uncommitted
 * upload leaves no final file. The checksum is incremental and taken from
 * the same received bytes before the DB write. */
typedef struct cf_storage_upload cf_storage_upload;

cf_err cf_storage_upload_begin(cf_storage *s, cf_storage_upload **out);
/* Explicit bounded imported-file policy; byte_limit must be1..100MiB. */
cf_err cf_storage_upload_begin_bounded(cf_storage *s, uint64_t byte_limit, cf_storage_upload **out);
/* Append received bytes; CF_LIMIT before anything is written when the
 * 16 MiB cap would be exceeded. */
cf_err cf_storage_upload_write(cf_storage_upload *u, cf_span bytes);
uint64_t cf_storage_upload_size(const cf_storage_upload *u);
/* base64(MD5) of the bytes received so far; callable repeatedly. */
cf_err cf_storage_upload_checksum(const cf_storage_upload *u, cf_builder *out);

/* Publish the staged bytes at key (created exclusively; CF_BUSY when the key
 * exists). After CF_OK the upload owns the final file until commit/rollback
 * and rejects further writes. */
cf_err cf_storage_upload_move(cf_storage_upload *u, cf_span key);
/* Keep the final file; releases staging state. CF_INVALID unless moved. */
cf_err cf_storage_upload_commit(cf_storage_upload *u);
/* Delete the final file this upload moved into place (and any staging file);
 * deletes nothing else. Safe to call repeatedly and before/without a move. */
void cf_storage_upload_rollback(cf_storage_upload *u);
/* Destructor: rolls back unless committed, then frees. NULL-safe. */
void cf_storage_upload_dispose(cf_storage_upload *u);

/* ---- deletion ----------------------------------------------------------- */

/* Delete the file at key. Missing parent/file is success (purge parity);
 * permission/I/O failure is CF_IO. A symlink at the key is unlinked itself,
 * never followed. */
cf_err cf_storage_delete(cf_storage *s, cf_span key);

/* Delete the contents of the legacy untracked-variant directory
 * <root>/<key[0:2]>/<key[2:4]>/variants/<key>/ (disk.rs::delete_prefixed
 * for "variants/<key>/"), leaving the empty directory. Missing is success;
 * symlinks are removed, never followed; recursion is depth-bounded. */
cf_err cf_storage_delete_variants(cf_storage *s, cf_span key);

/* ---- subprocess boundary ------------------------------------------------ */

/* Fixed executable allowlist. Absolute compile-time paths and argv[0] are
 * chosen by enum, never by request values or a runtime PATH search. External
 * source-pinned tools/adapter are prepared by vendor/media/build.sh; media
 * parity is checked separately from the subprocess lifecycle interface. */
#ifndef CF_PROC_VIPS_PATH
#define CF_PROC_VIPS_PATH "/usr/bin/vips"
#endif
#ifndef CF_PROC_FFMPEG_PATH
#define CF_PROC_FFMPEG_PATH "/usr/bin/ffmpeg"
#endif
#ifndef CF_PROC_FFPROBE_PATH
#define CF_PROC_FFPROBE_PATH "/usr/bin/ffprobe"
#endif
#ifndef CF_PROC_VIPS_ADAPTER_PATH
#define CF_PROC_VIPS_ADAPTER_PATH "/usr/local/bin/cf-vips"
#endif

typedef enum {
    CF_PROC_VIPS = 0,
    CF_PROC_FFMPEG,
    CF_PROC_FFPROBE,
    CF_PROC_VIPS_ADAPTER
} cf_proc_exe;

/* Fixed executable path/argv[0] for diagnostics; NULL for an unknown enum. */
const char *cf_proc_executable_path(cf_proc_exe exe);
const char *cf_proc_executable_name(cf_proc_exe exe);

/* Stderr retained at most (kept prefix; the rest is drained and dropped). */
#define CF_PROC_STDERR_RETAIN ((size_t)64 * 1024)
#define CF_PROC_MAX_ARGS 256

typedef struct {
    cf_proc_exe exe;
    char *const *args; /* NULL-terminated arguments AFTER argv[0]; may be NULL */
    int stdin_fd;      /* -1: /dev/null; otherwise an FD >= 3 */
    int stdout_fd;     /* -1: capture stdout (bounded by stdout_limit) */
    size_t stdout_limit; /* capture cap when stdout_fd == -1 */
    int stderr_fd;     /* -1: capture stderr (<= CF_PROC_STDERR_RETAIN) */
    uint64_t timeout_ms; /* required, > 0 */
} cf_proc_opts;

typedef struct {
    pid_t pid;        /* reaped child; -1 when nothing was spawned */
    bool timed_out;   /* killed by the time limit */
    bool output_limit;/* killed because stdout exceeded stdout_limit */
    bool err_truncated; /* stderr exceeded the retained prefix */
    bool exited_ok;   /* exited normally with status 0 */
    int exit_code;    /* normal exit status, or -1 */
    int signal;       /* terminating signal, or 0 */
    cf_buf *out;      /* captured stdout (owned; NULL for an explicit FD) */
    cf_buf *err;      /* retained stderr (owned; NULL for an explicit FD) */
} cf_proc_result;

/* Spawn and wait. The child runs in its own process group; unrelated FDs are
 * closed in it, argv is passed to execve directly (never sh -c), stdout and
 * stderr are drained concurrently so neither pipe can deadlock, and on
 * timeout/output-limit the whole process group is SIGKILLed and reaped
 * (never a zombie).
 *
 * Returns CF_OK for a normally spawned and waited child even when it exited
 * nonzero (inspect exit_code/signal); CF_NOT_FOUND when the fixed executable
 * is absent/unexecutable; CF_LIMIT when killed by timeout or the stdout cap
 * (timed_out/output_limit say which); CF_NOMEM/CF_IO otherwise. out is
 * initialized empty; on CF_LIMIT/CF_IO after a spawn it may hold diagnostics
 * (out->out/out->err). Dispose unconditionally. */
cf_err cf_proc_run(const cf_proc_opts *opts, cf_proc_result *out);
void cf_proc_result_dispose(cf_proc_result *r); /* NULL/empty-safe */

#endif /* CF_STORAGE_STORAGE_H */
