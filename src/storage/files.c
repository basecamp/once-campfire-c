/* src/storage/files.c — S01 disk service: key validation/generation, safe
 * paths, upload staging and key-only deletion. See storage.h for the
 * contract and 05-storage-integrations.md "S01".
 *
 * Layout and behavior follow tmp/rust-ref/crates/storage/src/disk.rs
 * (folder_for: <root>/<key[0:2]>/<key[2:4]>/<key>; delete treats ENOENT as
 * success; delete_prefixed for legacy variants) and key.rs (base36 keys of
 * length 28; base64(MD5) checksums), with the S01 differences the contract
 * requires: every path component is opened O_NOFOLLOW relative to the owned
 * root FD, uploads are staged in a 0600 temp file computed incrementally,
 * the final key is published without overwrite, and rollback removes only
 * the file this upload created.
 *
 * Every syscall error is mapped explicitly (storage.h); no error is
 * swallowed: the one deliberate ENOENT-ignore is deletion of a missing file,
 * which the reference (and STORE-03) define as success. */
#include "storage/storage.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/evp.h>

/* Legacy untracked variants live under this directory below the key folder;
 * a two-character base36 key directory can never collide with the name. */
#define CF_STORAGE_TMP_DIR "tmp"
#define CF_STORAGE_MAX_DEPTH 32

struct cf_storage {
    int root_fd; /* owned O_DIRECTORY|O_NOFOLLOW */
    int tmp_fd;  /* owned <root>/tmp staging directory */
};

struct cf_storage_upload {
    cf_storage *s; /* borrowed */
    int fd;        /* staging file; -1 once moved or rolled back */
    EVP_MD_CTX *md5;
    uint64_t size;
    char tmp_name[40];
    int final_dir; /* owned key-folder FD once moved */
    char key[CF_STORAGE_KEY_LENGTH + 1];
    bool moved;
    bool committed;
};

/* ---- keys --------------------------------------------------------------- */

bool cf_storage_key_valid(cf_span key) {
    if (key.ptr == NULL || key.len != CF_STORAGE_KEY_LENGTH) return false;
    for (size_t i = 0; i < key.len; i++) {
        unsigned char c = key.ptr[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'))) return false;
    }
    return true;
}

/* Copies a validated key into a NUL-terminated buffer. */
static cf_err storage_key_copy(cf_span key, char out[CF_STORAGE_KEY_LENGTH + 1]) {
    if (!cf_storage_key_valid(key)) return CF_INVALID;
    memcpy(out, key.ptr, CF_STORAGE_KEY_LENGTH);
    out[CF_STORAGE_KEY_LENGTH] = '\0';
    return CF_OK;
}

cf_err cf_storage_key_generate(cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    /* SecureRandom.base36(28): uniform over 0-9a-z. Bytes >= 252 are
     * rejected so the modulo introduces no bias. */
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    unsigned char key[CF_STORAGE_KEY_LENGTH];
    size_t filled = 0;
    while (filled < sizeof key) {
        unsigned char raw[32];
        cf_err rc = cf_random_bytes(raw, sizeof raw);
        if (rc != CF_OK) return rc;
        for (size_t i = 0; i < sizeof raw && filled < sizeof key; i++) {
            if (raw[i] >= 252) continue;
            key[filled++] = (unsigned char)alphabet[raw[i] % 36];
        }
    }
    return cf_builder_append(out, (cf_span){key, sizeof key});
}

/* ---- opening the root --------------------------------------------------- */

/* mkdir -p semantics for the configured root path (the reference's upload
 * path uses create_dir_all for the same directories). */
static cf_err storage_mkdir_p(const char *path) {
    size_t len = strlen(path);
    if (len == 0 || len > 65535) return CF_INVALID;
    char *buf = malloc(len + 1);
    if (buf == NULL) return CF_NOMEM;
    memcpy(buf, path, len + 1);
    cf_err rc = CF_OK;
    for (size_t i = 1; i <= len; i++) {
        if (buf[i] != '/' && buf[i] != '\0') continue;
        char saved = buf[i];
        buf[i] = '\0';
        if (mkdir(buf, 0700) != 0 && errno != EEXIST) {
            rc = CF_IO;
            break;
        }
        buf[i] = saved;
    }
    free(buf);
    return rc;
}

cf_err cf_storage_open(const char *root_path, cf_storage **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (root_path == NULL || root_path[0] == '\0') return CF_INVALID;
    /* A trailing slash makes the kernel follow a symlinked final component
     * even with O_NOFOLLOW, so resolve the configured root without it: the
     * no-symlink rule must hold for "/path/link/" exactly as for
     * "/path/link".  "/" itself stays "/". */
    size_t root_len = strlen(root_path);
    while (root_len > 1 && root_path[root_len - 1] == '/') root_len--;
    char *normalized = malloc(root_len + 1);
    if (normalized == NULL) return CF_NOMEM;
    memcpy(normalized, root_path, root_len);
    normalized[root_len] = '\0';
    cf_err rc = storage_mkdir_p(normalized);
    if (rc != CF_OK) {
        free(normalized);
        return rc;
    }
    int root = open(normalized, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    free(normalized);
    if (root < 0) return CF_IO;
    cf_storage *s = calloc(1, sizeof *s);
    if (s == NULL) {
        close(root);
        return CF_NOMEM;
    }
    s->root_fd = root;
    s->tmp_fd = -1;
    if (mkdirat(root, CF_STORAGE_TMP_DIR, 0700) != 0 && errno != EEXIST) {
        cf_storage_close(s);
        return CF_IO;
    }
    int tmp = openat(root, CF_STORAGE_TMP_DIR,
                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (tmp < 0) {
        cf_storage_close(s);
        return CF_IO;
    }
    s->tmp_fd = tmp;
    *out = s;
    return CF_OK;
}

void cf_storage_close(cf_storage *s) {
    if (s == NULL) return;
    if (s->root_fd >= 0) close(s->root_fd);
    if (s->tmp_fd >= 0) close(s->tmp_fd);
    s->root_fd = -1;
    s->tmp_fd = -1;
    free(s);
}

/* ---- safe relative resolution ------------------------------------------- */

static int storage_open_dir_at(int dirfd, const char *name) {
    return openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

/* Opens <root>/<key[0:2]>/<key[2:4]> (disk.rs::folder_for) with every
 * component O_NOFOLLOW. With create, both key folders are made 0700 first. */
static cf_err storage_open_parent(cf_storage *s, const char *key, bool create,
                                  int *out_dir) {
    char d1[3] = {key[0], key[1], '\0'};
    char d2[3] = {key[2], key[3], '\0'};
    int first = storage_open_dir_at(s->root_fd, d1);
    if (first < 0) {
        if (!create || errno != ENOENT) {
            return errno == ENOENT ? CF_NOT_FOUND : CF_IO;
        }
        if (mkdirat(s->root_fd, d1, 0700) != 0 && errno != EEXIST) return CF_IO;
        first = storage_open_dir_at(s->root_fd, d1);
        if (first < 0) return errno == ENOENT ? CF_NOT_FOUND : CF_IO;
    }
    int second = storage_open_dir_at(first, d2);
    if (second < 0 && create && errno == ENOENT) {
        if (mkdirat(first, d2, 0700) != 0 && errno != EEXIST) {
            close(first);
            return CF_IO;
        }
        second = storage_open_dir_at(first, d2);
    }
    close(first);
    if (second < 0) return errno == ENOENT ? CF_NOT_FOUND : CF_IO;
    *out_dir = second;
    return CF_OK;
}

/* ---- checksums ---------------------------------------------------------- */

/* base64(MD5) (key.rs::checksum uses STANDARD base64 with padding). */
static cf_err storage_md5_b64(const unsigned char digest[16], cf_builder *out) {
    unsigned char b64[25];
    int n = EVP_EncodeBlock(b64, digest, 16);
    if (n <= 0) return CF_IO;
    return cf_builder_append(out, (cf_span){b64, (size_t)n});
}

static cf_err storage_checksum_fd(int fd, cf_builder *out) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx == NULL) return CF_NOMEM;
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
    rc = storage_md5_b64(digest, out);
done:
    EVP_MD_CTX_free(ctx);
    return rc;
}

cf_err cf_storage_checksum(cf_storage *s, cf_span key, cf_builder *out) {
    if (s == NULL || out == NULL) return CF_INVALID;
    char k[CF_STORAGE_KEY_LENGTH + 1];
    cf_err rc = storage_key_copy(key, k);
    if (rc != CF_OK) return rc;
    int dir = -1;
    rc = storage_open_parent(s, k, false, &dir);
    if (rc != CF_OK) return rc;
    int fd = openat(dir, k, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    int saved_errno = errno;
    close(dir);
    if (fd < 0) return saved_errno == ENOENT ? CF_NOT_FOUND : CF_IO;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return CF_NOT_FOUND;
    }
    rc = storage_checksum_fd(fd, out);
    close(fd);
    return rc;
}

/* ---- reading ------------------------------------------------------------ */

cf_err cf_storage_open_read(cf_storage *s, cf_span key, int *out_fd,
                            uint64_t *out_size) {
    if (s == NULL || out_fd == NULL) return CF_INVALID;
    *out_fd = -1;
    if (out_size != NULL) *out_size = 0;
    char k[CF_STORAGE_KEY_LENGTH + 1];
    cf_err rc = storage_key_copy(key, k);
    if (rc != CF_OK) return rc;
    int dir = -1;
    rc = storage_open_parent(s, k, false, &dir);
    if (rc != CF_OK) return rc;
    int fd = openat(dir, k, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    int saved_errno = errno;
    close(dir);
    if (fd < 0) return saved_errno == ENOENT ? CF_NOT_FOUND : CF_IO;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return CF_NOT_FOUND;
    }
    if (out_size != NULL) *out_size = (uint64_t)st.st_size;
    *out_fd = fd;
    return CF_OK;
}

/* ---- upload staging ----------------------------------------------------- */

/* Unique "<root>/tmp/.up-<24 hex>" created 0600 with O_EXCL|O_NOFOLLOW. */
static cf_err storage_open_temp(cf_storage *s, char *name, size_t name_cap,
                                int *out_fd) {
    static const char hex[] = "0123456789abcdef";
    for (int attempt = 0; attempt < 32; attempt++) {
        unsigned char rnd[12];
        cf_err rc = cf_random_bytes(rnd, sizeof rnd);
        if (rc != CF_OK) return rc;
        size_t pos = 0;
        if (name_cap < 4 + sizeof rnd * 2 + 1) return CF_INTERNAL;
        name[pos++] = '.';
        name[pos++] = 'u';
        name[pos++] = 'p';
        name[pos++] = '-';
        for (size_t i = 0; i < sizeof rnd; i++) {
            name[pos++] = hex[rnd[i] >> 4];
            name[pos++] = hex[rnd[i] & 0x0f];
        }
        name[pos] = '\0';
        int fd = openat(s->tmp_fd, name,
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                        0600);
        if (fd >= 0) {
            *out_fd = fd;
            return CF_OK;
        }
        if (errno != EEXIST) return CF_IO;
    }
    return CF_BUSY; /* 32 random collisions cannot happen in practice */
}

cf_err cf_storage_upload_begin(cf_storage *s, cf_storage_upload **out) {
    if (s == NULL || out == NULL) return CF_INVALID;
    *out = NULL;
    cf_storage_upload *u = calloc(1, sizeof *u);
    if (u == NULL) return CF_NOMEM;
    u->s = s;
    u->fd = -1;
    u->final_dir = -1;
    cf_err rc = storage_open_temp(s, u->tmp_name, sizeof u->tmp_name, &u->fd);
    if (rc != CF_OK) {
        free(u);
        return rc;
    }
    u->md5 = EVP_MD_CTX_new();
    cf_err init_rc = u->md5 == NULL ? CF_NOMEM : CF_IO;
    if (u->md5 == NULL || EVP_DigestInit_ex(u->md5, EVP_md5(), NULL) != 1) {
        if (u->md5 != NULL) EVP_MD_CTX_free(u->md5);
        (void)unlinkat(s->tmp_fd, u->tmp_name, 0);
        close(u->fd);
        free(u);
        return init_rc;
    }
    *out = u;
    return CF_OK;
}

cf_err cf_storage_upload_write(cf_storage_upload *u, cf_span bytes) {
    if (u == NULL || u->fd < 0 || u->moved || u->md5 == NULL) return CF_INVALID;
    if (bytes.len != 0 && bytes.ptr == NULL) return CF_INVALID;
    /* The cap is checked while receiving, before anything is written. */
    if ((uint64_t)bytes.len > CF_STORAGE_UPLOAD_MAX_BYTES - u->size) {
        return CF_LIMIT;
    }
    size_t off = 0;
    while (off < bytes.len) {
        ssize_t n = write(u->fd, bytes.ptr + off, bytes.len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return CF_IO;
        }
        off += (size_t)n;
    }
    if (bytes.len != 0 && EVP_DigestUpdate(u->md5, bytes.ptr, bytes.len) != 1) {
        return CF_IO;
    }
    u->size += (uint64_t)bytes.len;
    return CF_OK;
}

uint64_t cf_storage_upload_size(const cf_storage_upload *u) {
    return u == NULL ? 0 : u->size;
}

cf_err cf_storage_upload_checksum(const cf_storage_upload *u, cf_builder *out) {
    if (u == NULL || u->md5 == NULL || out == NULL) return CF_INVALID;
    EVP_MD_CTX *copy = EVP_MD_CTX_new();
    if (copy == NULL) return CF_NOMEM;
    /* copy_ex only reads the source context (the member is a plain pointer
     * even through the const struct pointer). */
    cf_err rc = CF_IO;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_MD_CTX_copy_ex(copy, u->md5) != 1) goto done;
    if (EVP_DigestFinal_ex(copy, digest, &digest_len) != 1 || digest_len != 16) {
        goto done;
    }
    rc = storage_md5_b64(digest, out);
done:
    EVP_MD_CTX_free(copy);
    return rc;
}

cf_err cf_storage_upload_move(cf_storage_upload *u, cf_span key) {
    if (u == NULL || u->moved || u->fd < 0) return CF_INVALID;
    char k[CF_STORAGE_KEY_LENGTH + 1];
    cf_err rc = storage_key_copy(key, k);
    if (rc != CF_OK) return rc;
    int dir = -1;
    rc = storage_open_parent(u->s, k, true, &dir);
    if (rc != CF_OK) return rc;
    /* Exclusive publish: link fails with EEXIST instead of replacing a key
     * another upload (or a crash-orphaned run) already created. */
    if (linkat(u->s->tmp_fd, u->tmp_name, dir, k, 0) != 0) {
        int saved_errno = errno;
        close(dir);
        return saved_errno == EEXIST ? CF_BUSY : CF_IO;
    }
    (void)unlinkat(u->s->tmp_fd, u->tmp_name, 0); /* retried by commit/dispose */
    memcpy(u->key, k, sizeof u->key);
    u->final_dir = dir;
    u->moved = true;
    close(u->fd);
    u->fd = -1;
    return CF_OK;
}

cf_err cf_storage_upload_commit(cf_storage_upload *u) {
    if (u == NULL || !u->moved || u->committed) return CF_INVALID;
    u->committed = true;
    if (u->final_dir >= 0) {
        close(u->final_dir);
        u->final_dir = -1;
    }
    if (u->s != NULL && u->tmp_name[0] != '\0') {
        (void)unlinkat(u->s->tmp_fd, u->tmp_name, 0);
    }
    return CF_OK;
}

void cf_storage_upload_rollback(cf_storage_upload *u) {
    if (u == NULL) return;
    /* Only the final file this upload created through an exclusive link. */
    if (u->moved && !u->committed && u->final_dir >= 0) {
        (void)unlinkat(u->final_dir, u->key, 0);
    }
    if (u->final_dir >= 0) {
        close(u->final_dir);
        u->final_dir = -1;
    }
    if (u->fd >= 0) {
        close(u->fd);
        u->fd = -1;
    }
    if (u->s != NULL && u->tmp_name[0] != '\0') {
        if (unlinkat(u->s->tmp_fd, u->tmp_name, 0) == 0 || errno == ENOENT) {
            u->tmp_name[0] = '\0';
        }
    }
    if (u->md5 != NULL) {
        EVP_MD_CTX_free(u->md5);
        u->md5 = NULL;
    }
    u->moved = false;
    u->committed = false;
}

void cf_storage_upload_dispose(cf_storage_upload *u) {
    if (u == NULL) return;
    cf_storage_upload_rollback(u);
    free(u);
}

/* ---- deletion ----------------------------------------------------------- */

cf_err cf_storage_delete(cf_storage *s, cf_span key) {
    if (s == NULL) return CF_INVALID;
    char k[CF_STORAGE_KEY_LENGTH + 1];
    cf_err rc = storage_key_copy(key, k);
    if (rc != CF_OK) return rc;
    int dir = -1;
    rc = storage_open_parent(s, k, false, &dir);
    if (rc == CF_NOT_FOUND) return CF_OK; /* missing file is success */
    if (rc != CF_OK) return rc;
    int unlink_rc = unlinkat(dir, k, 0);
    int saved_errno = errno;
    close(dir);
    if (unlink_rc == 0 || saved_errno == ENOENT) return CF_OK;
    return CF_IO;
}

/* Removes every entry of `dirfd` without following symlinks; directories are
 * recursed (depth-bounded) and removed, symlinks are unlinked as links. */
static cf_err storage_remove_children(int dirfd, size_t depth) {
    if (depth > CF_STORAGE_MAX_DEPTH) return CF_LIMIT;
    int scan = dup(dirfd);
    if (scan < 0) return CF_IO;
    DIR *d = fdopendir(scan);
    if (d == NULL) {
        close(scan);
        return CF_IO;
    }
    cf_err rc = CF_OK;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(d);
        if (entry == NULL) {
            if (errno != 0) rc = CF_IO;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        struct stat st;
        if (fstatat(dirfd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            rc = CF_IO;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            int child = storage_open_dir_at(dirfd, entry->d_name);
            if (child < 0) {
                rc = CF_IO;
                break;
            }
            rc = storage_remove_children(child, depth + 1);
            close(child);
            if (rc != CF_OK) break;
            if (unlinkat(dirfd, entry->d_name, AT_REMOVEDIR) != 0) {
                rc = CF_IO;
                break;
            }
        } else if (unlinkat(dirfd, entry->d_name, 0) != 0) {
            rc = CF_IO;
            break;
        }
    }
    closedir(d);
    return rc;
}

cf_err cf_storage_delete_variants(cf_storage *s, cf_span key) {
    if (s == NULL) return CF_INVALID;
    char k[CF_STORAGE_KEY_LENGTH + 1];
    cf_err rc = storage_key_copy(key, k);
    if (rc != CF_OK) return rc;
    char d1[3] = {k[0], k[1], '\0'};
    char d2[3] = {k[2], k[3], '\0'};
    int first = storage_open_dir_at(s->root_fd, d1);
    if (first < 0) return errno == ENOENT ? CF_OK : CF_IO;
    int second = storage_open_dir_at(first, d2);
    close(first);
    if (second < 0) return errno == ENOENT ? CF_OK : CF_IO;
    int variants = storage_open_dir_at(second, "variants");
    close(second);
    if (variants < 0) return errno == ENOENT ? CF_OK : CF_IO;
    int keydir = storage_open_dir_at(variants, k);
    close(variants);
    if (keydir < 0) return errno == ENOENT ? CF_OK : CF_IO;
    rc = storage_remove_children(keydir, 1);
    close(keydir);
    return rc;
}
