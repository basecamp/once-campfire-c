#ifndef CF_TEST_AVATAR_H
#define CF_TEST_AVATAR_H
#include "storage/storage.h"
#include "db/db_testutil.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AVATAR_BYTES "<svg xmlns=\"http://www.w3.org/2000/svg\"/>"
#define AVATAR_PART "Content-Disposition: form-data; name=\"user[avatar]\"; filename=\"avatar.svg\"\r\nContent-Type: image/svg+xml\r\n\r\n" AVATAR_BYTES "\r\n--avatar--\r\n"
static inline void avatar_test_root(cf_config *config, char *root) {
    CF_REQUIRE(mkdtemp(root) != NULL);
    free(config->storage_path);
    config->storage_path = strdup(root);
    CF_REQUIRE(config->storage_path != NULL);
}
static inline size_t avatar_test_file_count(const char *root) {
    DIR *dir = opendir(root);
    if (dir == NULL) return 0;
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name,".")==0 || strcmp(entry->d_name,"..")==0) continue;
        char path[4096];
        int n = snprintf(path,sizeof path,"%s/%s",root,entry->d_name);
        CF_REQUIRE(n > 0 && (size_t)n < sizeof path);
        struct stat st;
        if (lstat(path,&st) != 0) continue;
        count += S_ISDIR(st.st_mode) ? avatar_test_file_count(path) : 1;
    }
    closedir(dir);
    return count;
}
static inline void avatar_test_remove_tree(const char *root) {
    DIR *dir = opendir(root);
    if (dir == NULL) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name,".")==0 || strcmp(entry->d_name,"..")==0) continue;
        char path[4096];
        int n = snprintf(path,sizeof path,"%s/%s",root,entry->d_name);
        CF_REQUIRE(n > 0 && (size_t)n < sizeof path);
        struct stat st;
        if (lstat(path,&st) != 0) continue;
        if (S_ISDIR(st.st_mode)) avatar_test_remove_tree(path);
        else CF_CHECK(unlink(path)==0);
    }
    closedir(dir);
    CF_CHECK(rmdir(root)==0);
}
static inline void avatar_test_persisted(cf_db *db, const char *root) {
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db),
        "SELECT b.key,b.filename,b.byte_size,b.content_type FROM active_storage_attachments a JOIN active_storage_blobs b ON b.id=a.blob_id WHERE a.record_type='User' AND a.name='avatar'",-1,&stmt,NULL)==SQLITE_OK);
    CF_REQUIRE(sqlite3_step(stmt)==SQLITE_ROW);
    CF_CHECK(strcmp((const char *)sqlite3_column_text(stmt,1),"avatar.svg")==0);
    CF_CHECK(sqlite3_column_int64(stmt,2)==(int64_t)strlen(AVATAR_BYTES));
    CF_CHECK(strcmp((const char *)sqlite3_column_text(stmt,3),"image/svg+xml")==0);
    cf_storage *storage = NULL; int fd = -1; uint64_t size = 0;
    CF_REQUIRE(cf_storage_open(root,&storage)==CF_OK);
    cf_span key = {sqlite3_column_text(stmt,0),(size_t)sqlite3_column_bytes(stmt,0)};
    CF_REQUIRE(cf_storage_open_read(storage,key,&fd,&size)==CF_OK);
    char bytes[64]={0};
    CF_CHECK(read(fd,bytes,sizeof bytes)==(ssize_t)strlen(AVATAR_BYTES));
    CF_CHECK(size==strlen(AVATAR_BYTES));
    CF_CHECK(strcmp(bytes,AVATAR_BYTES)==0);
    close(fd); cf_storage_close(storage);
    CF_CHECK(sqlite3_step(stmt)==SQLITE_DONE);
    sqlite3_finalize(stmt);
}
#endif
