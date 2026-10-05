/* tests/storage/test_files.c — S01 disk-service tests (STORE-01 lifecycle
 * parts): key validation/generation, <root>/<key[0:2]>/<key[2:4]>/<key>
 * layout, traversal/symlink escape attempts, upload staging (0600 temp, 16
 * MiB cap, incremental checksum, exclusive publish, rollback cleanup),
 * key-only deletion and legacy-variant cleanup.
 *
 * Build (direct clang; the Makefile does not list src/storage yet):
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -O1 -g -Isrc -Itests
 *         -Ivendor/build/openssl-clang/install/include
 *         tests/storage/test_files.c src/storage/files.c
 *         src/core/{alloc,buffer,clock,random}.c
 *         vendor/build/openssl-clang/install/lib/libcrypto.a -ldl
 *         -o build/s01/test_files
 */
#include "cf_test.h"

#include "storage/storage.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static cf_span S(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* Valid base36 keys of the reference length 28. */
#define KEY1 "abcdefghijklmnopqrstuvwxyz12"
#define KEY2 "0123456789abcdefghijklmnopqr"
#define KEY3 "zzzzzzzzzzzzzzzzzzzzzzzzzzzz"
#define KEY4 "aaaaaaaaaaaaaaaaaaaaaaaaaaaa"

static bool make_root(char *out, size_t cap) {
    snprintf(out, cap, "/tmp/cf-s01-files-XXXXXX");
    return mkdtemp(out) != NULL;
}

static void remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d != NULL) {
            struct dirent *entry;
            while ((entry = readdir(d)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 ||
                    strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                char child[4096];
                snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
                remove_tree(child);
            }
            closedir(d);
        }
        (void)rmdir(path);
    } else {
        (void)unlink(path);
    }
}

static void key_path(const char *root, const char *key, char *out, size_t cap) {
    snprintf(out, cap, "%s/%c%c/%c%c/%s", root, key[0], key[1], key[2], key[3],
             key);
}

static int dir_entries(const char *path) {
    DIR *d = opendir(path);
    if (d == NULL) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        count++;
    }
    closedir(d);
    return count;
}

static int write_file(const char *path, const char *content) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    size_t len = strlen(content);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, content + off, len - off);
        if (n < 0) {
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}

static bool read_path(const char *path, unsigned char *buf, size_t cap,
                      size_t *out_len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, cap);
    close(fd);
    if (n < 0) return false;
    *out_len = (size_t)n;
    return true;
}

static cf_err stage_and_move(cf_storage *s, const char *key,
                             const unsigned char *data, size_t len) {
    cf_storage_upload *u = NULL;
    cf_err rc = cf_storage_upload_begin(s, &u);
    if (rc != CF_OK) return rc;
    rc = cf_storage_upload_write(u, (cf_span){data, len});
    if (rc == CF_OK) rc = cf_storage_upload_move(u, S(key));
    if (rc == CF_OK) rc = cf_storage_upload_commit(u);
    cf_storage_upload_dispose(u);
    return rc;
}

static bool span_is(cf_span span, const char *want) {
    size_t len = strlen(want);
    return span.ptr != NULL && span.len == len && memcmp(span.ptr, want, len) == 0;
}

/* ---- keys --------------------------------------------------------------- */

CF_TEST(key_validation) {
    CF_CHECK(cf_storage_key_valid(S(KEY1)));
    CF_CHECK(!cf_storage_key_valid(S("")));
    CF_CHECK(!cf_storage_key_valid(S("short")));
    CF_CHECK(!cf_storage_key_valid(S("ABCDEFGHIJKLMNOPQRSTUVWXYZ12")));
    CF_CHECK(!cf_storage_key_valid(S("abcdefghijklmnopqrstuvwxyz/2")));
    CF_CHECK(!cf_storage_key_valid(S("abcdefghijklmnopqrstuvwxyz.2")));
    CF_CHECK(!cf_storage_key_valid(S("..")));
    CF_CHECK(!cf_storage_key_valid(S("uploads/../../etc/passwd")));
    CF_CHECK(!cf_storage_key_valid(S("/rails/active_storage/disk/x")));
    CF_CHECK(!cf_storage_key_valid(S("moon.jpg")));
    static const unsigned char with_nul[CF_STORAGE_KEY_LENGTH] = {
        'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
        'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '\0'};
    CF_CHECK(!cf_storage_key_valid((cf_span){with_nul, sizeof with_nul}));
}

CF_TEST(key_generation_base36) {
    char seen[8][CF_STORAGE_KEY_LENGTH + 1];
    for (size_t i = 0; i < 8; i++) {
        cf_builder b = {0};
        CF_REQUIRE(cf_storage_key_generate(&b) == CF_OK);
        CF_REQUIRE(b.len == CF_STORAGE_KEY_LENGTH);
        CF_CHECK(cf_storage_key_valid((cf_span){b.ptr, b.len}));
        memcpy(seen[i], b.ptr, CF_STORAGE_KEY_LENGTH);
        seen[i][CF_STORAGE_KEY_LENGTH] = '\0';
        for (size_t j = 0; j < i; j++) CF_CHECK(strcmp(seen[i], seen[j]) != 0);
        cf_builder_dispose(&b);
    }
    CF_CHECK(cf_storage_key_generate(NULL) == CF_INVALID);
}

/* ---- root and layout ---------------------------------------------------- */

CF_TEST(root_creation_and_open_rejections) {
    char base[64];
    CF_REQUIRE(make_root(base, sizeof base));
    char nested[512];
    snprintf(nested, sizeof nested, "%s/deep/files", base);
    cf_storage *s = NULL;
    CF_CHECK(cf_storage_open("", &s) == CF_INVALID);
    CF_CHECK(s == NULL);
    CF_CHECK(cf_storage_open(NULL, &s) == CF_INVALID);
    CF_REQUIRE(cf_storage_open(nested, &s) == CF_OK);
    struct stat st;
    CF_CHECK(stat(nested, &st) == 0 && S_ISDIR(st.st_mode));
    char tmpdir[512];
    snprintf(tmpdir, sizeof tmpdir, "%s/tmp", nested);
    CF_CHECK(stat(tmpdir, &st) == 0 && S_ISDIR(st.st_mode));
    cf_storage_close(s);
    cf_storage_close(NULL);

    /* STORAGE_PATH itself pointing at a symlink is refused (O_NOFOLLOW). */
    char real[512], link[512];
    snprintf(real, sizeof real, "%s/real", base);
    snprintf(link, sizeof link, "%s/link", base);
    CF_REQUIRE(mkdir(real, 0700) == 0);
    CF_REQUIRE(symlink(real, link) == 0);
    s = (cf_storage *)0x1;
    CF_CHECK(cf_storage_open(link, &s) == CF_IO);
    CF_CHECK(s == NULL);

    /* A trailing slash must not defeat O_NOFOLLOW (the kernel follows the
     * final symlink for "link/" even with O_NOFOLLOW), so the same path with
     * one is refused too. */
    char link_slash[520];
    snprintf(link_slash, sizeof link_slash, "%s/", link);
    s = (cf_storage *)0x1;
    CF_CHECK(cf_storage_open(link_slash, &s) == CF_IO);
    CF_CHECK(s == NULL);

    /* A real directory with a trailing slash still opens, is created when
     * missing, and serves reads. */
    char nested2[512], nested2_slash[520];
    snprintf(nested2, sizeof nested2, "%s/deep2/files", base);
    snprintf(nested2_slash, sizeof nested2_slash, "%s/", nested2);
    s = (cf_storage *)0x1;
    CF_REQUIRE(cf_storage_open(nested2_slash, &s) == CF_OK);
    CF_CHECK(s != NULL);
    CF_CHECK(stat(nested2, &st) == 0 && S_ISDIR(st.st_mode));
    const unsigned char payload[] = {'o', 'k'};
    CF_CHECK(stage_and_move(s, KEY2, payload, sizeof payload) == CF_OK);
    int fd = -1;
    uint64_t size = 0;
    CF_CHECK(cf_storage_open_read(s, S(KEY2), &fd, &size) == CF_OK);
    CF_CHECK(size == sizeof payload);
    if (fd >= 0) close(fd);
    cf_storage_close(s);

    remove_tree(base);
}

CF_TEST(layout_and_checksum_known_answers) {
    char root[64];
    CF_REQUIRE(make_root(root, sizeof root));
    cf_storage *s = NULL;
    CF_REQUIRE(cf_storage_open(root, &s) == CF_OK);

    const unsigned char abc[] = {'a', 'b', 'c'};
    CF_REQUIRE(stage_and_move(s, KEY1, abc, sizeof abc) == CF_OK);

    char path[512];
    key_path(root, KEY1, path, sizeof path);
    struct stat st;
    CF_REQUIRE(stat(path, &st) == 0 && S_ISREG(st.st_mode));
    CF_CHECK(st.st_size == 3);

    int fd = -1;
    uint64_t size = 0;
    CF_REQUIRE(cf_storage_open_read(s, S(KEY1), &fd, &size) == CF_OK);
    CF_CHECK(size == 3);
    unsigned char buf[8];
    memset(buf, 0, sizeof buf);
    CF_CHECK(read(fd, buf, sizeof buf) == 3);
    CF_CHECK(memcmp(buf, "abc", 3) == 0);
    close(fd);

    cf_builder sum = {0};
    CF_REQUIRE(cf_storage_checksum(s, S(KEY1), &sum) == CF_OK);
    CF_CHECK(span_is((cf_span){sum.ptr, sum.len}, "kAFQmDzST7DWlj99KOF/cg=="));
    cf_builder_dispose(&sum);

    /* Empty file checksum is the key.rs vector for OpenSSL base64(MD5("")). */
    CF_REQUIRE(stage_and_move(s, KEY2, (const unsigned char *)"", 0) == CF_OK);
    cf_builder empty = {0};
    CF_REQUIRE(cf_storage_checksum(s, S(KEY2), &empty) == CF_OK);
    CF_CHECK(span_is((cf_span){empty.ptr, empty.len}, "1B2M2Y8AsgTpgAmY7PhCfg=="));
    cf_builder_dispose(&empty);

    CF_CHECK(cf_storage_checksum(s, S(KEY3), &empty) == CF_NOT_FOUND);
    CF_CHECK(cf_storage_open_read(s, S(KEY3), &fd, &size) == CF_NOT_FOUND);
    CF_CHECK(fd == -1 && size == 0);
    cf_storage_close(s);
    remove_tree(root);
}

CF_TEST(generated_key_lands_in_reference_layout) {
    char root[64];
    CF_REQUIRE(make_root(root, sizeof root));
    cf_storage *s = NULL;
    CF_REQUIRE(cf_storage_open(root, &s) == CF_OK);
    cf_builder key = {0};
    CF_REQUIRE(cf_storage_key_generate(&key) == CF_OK);
    char key_text[CF_STORAGE_KEY_LENGTH + 1];
    memcpy(key_text, key.ptr, CF_STORAGE_KEY_LENGTH);
    key_text[CF_STORAGE_KEY_LENGTH] = '\0';
    const unsigned char payload[] = "generated";
    CF_REQUIRE(stage_and_move(s, key_text, payload, sizeof payload - 1) == CF_OK);
    char path[512];
    key_path(root, key_text, path, sizeof path);
    struct stat st;
    CF_CHECK(stat(path, &st) == 0 && st.st_size == (off_t)(sizeof payload - 1));
    cf_builder_dispose(&key);
    cf_storage_close(s);
    remove_tree(root);
}

/* ---- traversal and symlinks --------------------------------------------- */

CF_TEST(invalid_keys_cannot_reach_filesystem) {
    char root[64], outside[64];
    CF_REQUIRE(make_root(root, sizeof root));
    CF_REQUIRE(make_root(outside, sizeof outside));
    char secret[512];
    snprintf(secret, sizeof secret, "%s/secret", outside);
    CF_REQUIRE(write_file(secret, "outside-bytes") == 0);

    cf_storage *s = NULL;
    CF_REQUIRE(cf_storage_open(root, &s) == CF_OK);
    cf_storage_upload *u = NULL;
    CF_REQUIRE(cf_storage_upload_begin(s, &u) == CF_OK);
    CF_REQUIRE(cf_storage_upload_write(u, S("x")) == CF_OK);

    const char *bad_keys[] = {
        "../secret", "/etc/passwd", "a/b", "moon.jpg", "..",
        "uploads/../../outside/secret",
    };
    for (size_t i = 0; i < sizeof bad_keys / sizeof bad_keys[0]; i++) {
        int fd = -1;
        uint64_t size = 0;
        cf_builder sum = {0};
        CF_CHECK(cf_storage_open_read(s, S(bad_keys[i]), &fd, &size) == CF_INVALID);
        CF_CHECK(cf_storage_checksum(s, S(bad_keys[i]), &sum) == CF_INVALID);
        CF_CHECK(cf_storage_delete(s, S(bad_keys[i])) == CF_INVALID);
        CF_CHECK(cf_storage_delete_variants(s, S(bad_keys[i])) == CF_INVALID);
        CF_CHECK(cf_storage_upload_move(u, S(bad_keys[i])) == CF_INVALID);
        cf_builder_dispose(&sum);
    }
    cf_storage_upload_dispose(u);

    unsigned char buf[32];
    size_t len = 0;
    CF_REQUIRE(read_path(secret, buf, sizeof buf, &len));
    CF_CHECK(span_is((cf_span){buf, len}, "outside-bytes"));

    /* Root/tmp must be empty again: the failed moves left no staging file. */
    char tmpdir[512];
    snprintf(tmpdir, sizeof tmpdir, "%s/tmp", root);
    CF_CHECK(dir_entries(tmpdir) == 0);

    cf_storage_close(s);
    remove_tree(root);
    remove_tree(outside);
}

CF_TEST(symlink_final_component_never_followed) {
    char root[64], outside[64];
    CF_REQUIRE(make_root(root, sizeof root));
    CF_REQUIRE(make_root(outside, sizeof outside));
    char secret[512];
    snprintf(secret, sizeof secret, "%s/secret", outside);
    CF_REQUIRE(write_file(secret, "outside-bytes") == 0);

    cf_storage *s = NULL;
    CF_REQUIRE(cf_storage_open(root, &s) == CF_OK);
    char d1[512], dir[512], linkpath[512];
    snprintf(d1, sizeof d1, "%s/%c%c", root, KEY3[0], KEY3[1]);
    snprintf(dir, sizeof dir, "%s/%c%c", d1, KEY3[2], KEY3[3]);
    CF_REQUIRE(mkdir(d1, 0700) == 0 || errno == EEXIST);
    CF_REQUIRE(mkdir(dir, 0700) == 0 || errno == EEXIST);
    key_path(root, KEY3, linkpath, sizeof linkpath);
    CF_REQUIRE(symlink(secret, linkpath) == 0);

    int fd = -1;
    uint64_t size = 0;
    cf_builder sum = {0};
    CF_CHECK(cf_storage_open_read(s, S(KEY3), &fd, &size) == CF_IO);
    CF_CHECK(cf_storage_checksum(s, S(KEY3), &sum) == CF_IO);
    cf_builder_dispose(&sum);
    /* Deleting the key removes the link itself, never the target. */
    CF_CHECK(cf_storage_delete(s, S(KEY3)) == CF_OK);

    unsigned char buf[32];
    size_t len = 0;
    CF_REQUIRE(read_path(secret, buf, sizeof buf, &len));
    CF_CHECK(span_is((cf_span){buf, len}, "outside-bytes"));
    CF_CHECK(lstat(linkpath, &(struct stat){0}) != 0);

    cf_storage_close(s);
    remove_tree(root);
    remove_tree(outside);
}

CF_TEST(symlink_directory_component_never_followed) {
    char root[64], outside[64];
    CF_REQUIRE(make_root(root, sizeof root));
    CF_REQUIRE(make_root(outside, sizeof outside));
    /* outside/<d2>/<key> holds the file a symlinked folder would expose. */
    char sub[512], secret[512];
    snprintf(sub, sizeof sub, "%s/%c%c", outside, KEY4[2], KEY4[3]);
    CF_REQUIRE(mkdir(sub, 0700) == 0);
    snprintf(secret, sizeof secret, "%s/%s", sub, KEY4);
    CF_REQUIRE(write_file(secret, "outside-bytes") == 0);

    cf_storage *s = NULL;
    CF_REQUIRE(cf_storage_open(root, &s) == CF_OK);
    char first[512];
    snprintf(first, sizeof first, "%s/%c%c", root, KEY4[0], KEY4[1]);
    CF_REQUIRE(symlink(outside, first) == 0);

    int fd = -1;
    uint64_t size = 0;
    cf_builder sum = {0};
    CF_CHECK(cf_storage_open_read(s, S(KEY4), &fd, &size) == CF_IO);
    CF_CHECK(cf_storage_checksum(s, S(KEY4), &sum) == CF_IO);
    CF_CHECK(cf_storage_delete(s, S(KEY4)) == CF_IO);
    cf_builder_dispose(&sum);

    unsigned char buf[32];
    size_t len = 0;
    CF_REQUIRE(read_path(secret, buf, sizeof buf, &len));
    CF_CHECK(span_is((cf_span){buf, len}, "outside-bytes"));

    cf_storage_close(s);
    remove_tree(root);
    remove_tree(outside);
}

/* ---- upload staging ----------------------------------------------------- */

CF_TEST(upload_temp_is_unique_0600_and_cleaned) {
    char root[64];
    CF_REQUIRE(make_root(root, sizeof root));
    cf_storage *s = NULL;
    CF_REQUIRE(cf_storage_open(root, &s) == CF_OK);
    char tmpdir[512];
    snprintf(tmpdir, sizeof tmpdir, "%s/tmp", root);

    cf_storage_upload *a = NULL, *b = NULL;
    CF_REQUIRE(cf_storage_upload_begin(s, &a) == CF_OK);
    CF_REQUIRE(cf_storage_upload_begin(s, &b) == CF_OK);
    CF_CHECK(dir_entries(tmpdir) == 2);

    /* Both staging files are regular 0600 files inside the storage fs. */
    DIR *d = opendir(tmpdir);
    CF_REQUIRE(d != NULL);
    int seen = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, ".up-", 4) != 0) continue;
        struct stat st;
        CF_REQUIRE(fstatat(dirfd(d), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0);
        CF_CHECK(S_ISREG(st.st_mode));
        CF_CHECK((st.st_mode & (mode_t)07777) == 0600);
        seen++;
    }
    closedir(d);
    CF_CHECK(seen == 2);

    /* A rolled-back upload removes its staging file and nothing else. */
    CF_REQUIRE(cf_storage_upload_write(a, S("partial-bytes")) == CF_OK);
    cf_storage_upload_rollback(a);
    cf_storage_upload_rollback(a); /* repeatable */
    cf_storage_upload_dispose(a);
    CF_CHECK(dir_entries(tmpdir) == 1);
    cf_storage_upload_dispose(b);
    CF_CHECK(dir_entries(tmpdir) == 0);

    /* Interrupted upload (dispose without move) leaves no owned file. */
    cf_storage_upload *c = NULL;
    CF_REQUIRE(cf_storage_upload_begin(s, &c) == CF_OK);
    CF_REQUIRE(cf_storage_upload_write(c, S("abc")) == CF_OK);
    CF_CHECK(cf_storage_upload_size(c) == 3);
    cf_storage_upload_dispose(c);
    CF_CHECK(dir_entries(tmpdir) == 0);
    CF_CHECK(cf_storage_upload_begin(s, NULL) == CF_INVALID);
    cf_storage_upload_dispose(NULL);

    cf_storage_close(s);
    remove_tree(root);
}

CF_TEST(upload_incremental_checksum) {
    char root[64];
    CF_REQUIRE(make_root(root, sizeof root));
    cf_storage *s = NULL;
    CF_REQUIRE(cf_storage_open(root, &s) == CF_OK);
    cf_storage_upload *u = NULL;
    CF_REQUIRE(cf_storage_upload_begin(s, &u) == CF_OK);
    /* Computed over received bytes, not a whole-memory buffer. */
    CF_REQUIRE(cf_storage_upload_write(u, S("a")) == CF_OK);
    CF_REQUIRE(cf_storage_upload_write(u, S("b")) == CF_OK);
    CF_REQUIRE(cf_storage_upload_write(u, S("")) == CF_OK);
    CF_REQUIRE(cf_storage_upload_write(u, S("c")) == CF_OK);
    CF_CHECK(cf_storage_upload_size(u) == 3);
    cf_builder sum = {0};
    CF_REQUIRE(cf_storage_upload_checksum(u, &sum) == CF_OK);
    CF_CHECK(span_is((cf_span){sum.ptr, sum.len}, "kAFQmDzST7DWlj99KOF/cg=="));
    cf_builder_dispose(&sum);
    /* Repeatable: the incremental context is copied, not consumed. */
    cf_builder again = {0};
    CF_REQUIRE(cf_storage_upload_checksum(u, &again) == CF_OK);
    CF_CHECK(span_is((cf_span){again.ptr, again.len}, "kAFQmDzST7DWlj99KOF/cg=="));
    cf_builder_dispose(&again);
    /* A later write updates the incremental checksum. */
    CF_REQUIRE(cf_storage_upload_write(u, S("d")) == CF_OK);
    cf_builder abcd = {0};
    CF_REQUIRE(cf_storage_upload_checksum(u, &abcd) == CF_OK);
    CF_CHECK(span_is((cf_span){abcd.ptr, abcd.len}, "4vxxTEcn7pOV8yTNLn8zHw=="));
    cf_builder_dispose(&abcd);
    CF_REQUIRE(cf_storage_upload_move(u, S(KEY1)) == CF_OK);
    CF_CHECK(cf_storage_upload_move(u, S(KEY1)) == CF_INVALID); /* no re-move */
    /* After the move the writable staging FD is gone. */
    CF_CHECK(cf_storage_upload_write(u, S("x")) == CF_INVALID);
    CF_REQUIRE(cf_storage_upload_commit(u) == CF_OK);
    /* Commit keeps the final file; rollback after commit must not delete it. */
    cf_storage_upload_rollback(u);
    cf_storage_upload_dispose(u);
    char path[512];
    key_path(root, KEY1, path, sizeof path);
    CF_CHECK(access(path, F_OK) == 0);
    cf_storage_close(s);
    remove_tree(root);
}

CF_TEST(upload_move_is_exclusive_and_rollback_owns_only_new_file) {
    char root[64];
    CF_REQUIRE(make_root(root, sizeof root));
    cf_storage *s = NULL;
    CF_REQUIRE(cf_storage_open(root, &s) == CF_OK);
    char path[512], tmpdir[512];
    key_path(root, KEY1, path, sizeof path);
    snprintf(tmpdir, sizeof tmpdir, "%s/tmp", root);

    const unsigned char first[] = "first-bytes";
    CF_REQUIRE(stage_and_move(s, KEY1, first, sizeof first - 1) == CF_OK);

    /* A second upload must never overwrite the existing key. */
    cf_storage_upload *u = NULL;
    CF_REQUIRE(cf_storage_upload_begin(s, &u) == CF_OK);
    CF_REQUIRE(cf_storage_upload_write(u, S("second-bytes")) == CF_OK);
    CF_CHECK(cf_storage_upload_move(u, S(KEY1)) == CF_BUSY);
    cf_storage_upload_dispose(u);
    unsigned char buf[32];
    size_t len = 0;
    CF_REQUIRE(read_path(path, buf, sizeof buf, &len));
    CF_CHECK(span_is((cf_span){buf, len}, "first-bytes"));
    CF_CHECK(dir_entries(tmpdir) == 0);

    /* Rollback deletes only its own final file; the original survives. */
    const unsigned char other[] = "other";
    CF_REQUIRE(stage_and_move(s, KEY2, other, sizeof other - 1) == CF_OK);
    cf_storage_upload *v = NULL;
    CF_REQUIRE(cf_storage_upload_begin(s, &v) == CF_OK);
    CF_REQUIRE(cf_storage_upload_write(v, S("review-me")) == CF_OK);
    CF_REQUIRE(cf_storage_upload_move(v, S(KEY3)) == CF_OK);
    cf_storage_upload_rollback(v);
    cf_storage_upload_dispose(v);
    char path2[512], path3[512];
    key_path(root, KEY2, path2, sizeof path2);
    key_path(root, KEY3, path3, sizeof path3);
    CF_CHECK(access(path2, F_OK) == 0);
    CF_CHECK(access(path3, F_OK) != 0);

    /* The key freed by rollback can be taken again. */
    CF_REQUIRE(stage_and_move(s, KEY3, other, sizeof other - 1) == CF_OK);
    CF_CHECK(access(path3, F_OK) == 0);

    cf_storage_close(s);
    remove_tree(root);
}

CF_TEST(upload_cap_enforced_while_receiving) {
    char root[64];
    CF_REQUIRE(make_root(root, sizeof root));
    cf_storage *s = NULL;
    CF_REQUIRE(cf_storage_open(root, &s) == CF_OK);
    cf_storage_upload *u = NULL;
    CF_REQUIRE(cf_storage_upload_begin(s, &u) == CF_OK);

    uint64_t total = CF_STORAGE_UPLOAD_MAX_BYTES;
    static unsigned char chunk[1 << 20];
    memset(chunk, 0x5a, sizeof chunk);
    uint64_t written = 0;
    while (total - written > sizeof chunk) {
        CF_REQUIRE(cf_storage_upload_write(u, (cf_span){chunk, sizeof chunk}) == CF_OK);
        written += sizeof chunk;
    }
    size_t tail = (size_t)(total - written);
    CF_REQUIRE(cf_storage_upload_write(u, (cf_span){chunk, tail}) == CF_OK);
    CF_CHECK(cf_storage_upload_size(u) == total);
    /* One byte over the decoded cap is refused before it is written. */
    CF_CHECK(cf_storage_upload_write(u, S("x")) == CF_LIMIT);
    CF_CHECK(cf_storage_upload_size(u) == total);

    CF_REQUIRE(cf_storage_upload_move(u, S(KEY1)) == CF_OK);
    CF_REQUIRE(cf_storage_upload_commit(u) == CF_OK);
    cf_storage_upload_dispose(u);
    char path[512];
    key_path(root, KEY1, path, sizeof path);
    struct stat st;
    CF_REQUIRE(stat(path, &st) == 0);
    CF_CHECK((uint64_t)st.st_size == CF_STORAGE_UPLOAD_MAX_BYTES);
    CF_CHECK(cf_storage_delete(s, S(KEY1)) == CF_OK);

    cf_storage_close(s);
    remove_tree(root);
}

/* ---- deletion ----------------------------------------------------------- */

CF_TEST(delete_missing_is_success_and_io_failure_is_error) {
    char root[64];
    CF_REQUIRE(make_root(root, sizeof root));
    cf_storage *s = NULL;
    CF_REQUIRE(cf_storage_open(root, &s) == CF_OK);

    CF_CHECK(cf_storage_delete(s, S(KEY1)) == CF_OK); /* nothing to delete */
    const unsigned char bytes[] = "delete-me";
    CF_REQUIRE(stage_and_move(s, KEY1, bytes, sizeof bytes - 1) == CF_OK);
    char path[512];
    key_path(root, KEY1, path, sizeof path);
    CF_REQUIRE(access(path, F_OK) == 0);
    CF_CHECK(cf_storage_delete(s, S(KEY1)) == CF_OK);
    CF_CHECK(access(path, F_OK) != 0);
    CF_CHECK(cf_storage_delete(s, S(KEY1)) == CF_OK); /* repeat */

    /* A damaged tree (key folder is a regular file) is an I/O failure, not a
     * swallowed success; the reference job records it as failed. */
    char broken[512];
    snprintf(broken, sizeof broken, "%s/%c%c", root, KEY2[0], KEY2[1]);
    CF_REQUIRE(write_file(broken, "not-a-directory") == 0);
    CF_CHECK(cf_storage_delete(s, S(KEY2)) == CF_IO);
    CF_CHECK(cf_storage_delete_variants(s, S(KEY2)) == CF_IO);

    cf_storage_close(s);
    remove_tree(root);
}

CF_TEST(delete_variants_removes_only_the_key_prefix) {
    char root[64], outside[64];
    CF_REQUIRE(make_root(root, sizeof root));
    CF_REQUIRE(make_root(outside, sizeof outside));
    char secret[512];
    snprintf(secret, sizeof secret, "%s/secret", outside);
    CF_REQUIRE(write_file(secret, "outside-bytes") == 0);

    /* root/<d1>/<d2>/variants/<key>/{a.txt, sub/b.txt, link -> secret}. */
    char d1[512], d2[512], variants[512], keydir[512], sub[512], file[512], link[512];
    snprintf(d1, sizeof d1, "%s/%c%c", root, KEY1[0], KEY1[1]);
    snprintf(d2, sizeof d2, "%s/%c%c", d1, KEY1[2], KEY1[3]);
    snprintf(variants, sizeof variants, "%s/variants", d2);
    snprintf(keydir, sizeof keydir, "%s/%s", variants, KEY1);
    snprintf(sub, sizeof sub, "%s/sub", keydir);
    CF_REQUIRE(mkdir(d1, 0700) == 0);
    CF_REQUIRE(mkdir(d2, 0700) == 0);
    CF_REQUIRE(mkdir(variants, 0700) == 0);
    CF_REQUIRE(mkdir(keydir, 0700) == 0);
    CF_REQUIRE(mkdir(sub, 0700) == 0);
    snprintf(file, sizeof file, "%s/a.txt", keydir);
    CF_REQUIRE(write_file(file, "variant") == 0);
    snprintf(file, sizeof file, "%s/b.txt", sub);
    CF_REQUIRE(write_file(file, "nested") == 0);
    snprintf(link, sizeof link, "%s/link", keydir);
    CF_REQUIRE(symlink(secret, link) == 0);

    cf_storage *s = NULL;
    CF_REQUIRE(cf_storage_open(root, &s) == CF_OK);
    CF_CHECK(cf_storage_delete_variants(s, S(KEY1)) == CF_OK);
    CF_CHECK(dir_entries(keydir) == 0);
    CF_CHECK(access(keydir, F_OK) == 0); /* the key directory itself stays */
    CF_CHECK(access(secret, F_OK) == 0); /* the symlink target survives */
    CF_CHECK(cf_storage_delete_variants(s, S(KEY1)) == CF_OK); /* missing */

    /* A key that never had variants is a no-op success. */
    CF_CHECK(cf_storage_delete_variants(s, S(KEY2)) == CF_OK);

    cf_storage_close(s);
    remove_tree(root);
    remove_tree(outside);
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

CF_TEST(no_fd_leak_across_operations) {
    char root[64], link[128];
    CF_REQUIRE(make_root(root, sizeof root));
    int before = fd_count();
    CF_REQUIRE(before > 0);

    /* Failed open (root is a symlink) leaves no FD behind. */
    snprintf(link, sizeof link, "%s/nope-link", root);
    CF_REQUIRE(symlink("/tmp", link) == 0);
    cf_storage *bad = NULL;
    CF_CHECK(cf_storage_open(link, &bad) == CF_IO);

    cf_storage *s = NULL;
    CF_REQUIRE(cf_storage_open(root, &s) == CF_OK);
    CF_REQUIRE(stage_and_move(s, KEY1, (const unsigned char *)"abc", 3) == CF_OK);
    int fd = -1;
    uint64_t size = 0;
    CF_REQUIRE(cf_storage_open_read(s, S(KEY1), &fd, &size) == CF_OK);
    close(fd);
    cf_builder sum = {0};
    CF_REQUIRE(cf_storage_checksum(s, S(KEY1), &sum) == CF_OK);
    cf_builder_dispose(&sum);
    CF_REQUIRE(cf_storage_delete(s, S(KEY1)) == CF_OK);
    CF_CHECK(cf_storage_open_read(s, S(KEY2), &fd, &size) == CF_NOT_FOUND);

    cf_storage_upload *u = NULL;
    CF_REQUIRE(cf_storage_upload_begin(s, &u) == CF_OK);
    CF_REQUIRE(cf_storage_upload_write(u, S("staged")) == CF_OK);
    cf_storage_upload_dispose(u); /* rollback closes the staging FD */
    cf_storage_close(s);

    int after = fd_count();
    CF_CHECK(after == before);
    remove_tree(root);
}

CF_TEST_MAIN()
