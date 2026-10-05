/* src/views/view_assets.c — asset paths and tags for the views.
 *
 * Reference: crates/assets/src/{helpers,tags}.rs over the build-time embedded
 * manifest and importmap.  The C port loads the pinned build outputs from the
 * static root (tests/fixtures/assets is the pinned tree; a production root
 * carries the same files):
 *
 *   <root>/public/assets/.manifest.json  Propshaft manifest
 *                                        (logical path -> digested_path)
 *   <root>/importmap-tags.html           javascript_importmap_tags output
 *
 * cf_views_assets_configure loads both once at boot; renderers only read the
 * loaded memory (A02: a render performs no filesystem work).
 */
#include "views/internal.h"

#include "core/alloc.h"
#include "routes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yyjson.h"

struct asset_entry {
    char *logical;
    char *url; /* "/assets/<digested>" */
};

static struct {
    bool loaded;
    struct asset_entry *entries;
    size_t count, cap;
    size_t *stylesheets; /* indices into entries, sorted by logical path */
    size_t stylesheet_count;
    char *importmap_tags;
    char *stylesheet_tags;
    char *preload_links;
} g_assets;

static void assets_clear(void) {
    for (size_t i = 0; i < g_assets.count; i++) {
        free(g_assets.entries[i].logical);
        free(g_assets.entries[i].url);
    }
    free(g_assets.entries);
    free(g_assets.stylesheets);
    free(g_assets.importmap_tags);
    free(g_assets.stylesheet_tags);
    free(g_assets.preload_links);
    memset(&g_assets, 0, sizeof g_assets);
}

void cf_views_assets_reset(void) { assets_clear(); }

static char *join_path(const char *root, const char *suffix) {
    size_t root_len = strlen(root);
    size_t suffix_len = strlen(suffix);
    char *path = malloc(root_len + suffix_len + 1);
    if (path == NULL) return NULL;
    memcpy(path, root, root_len);
    memcpy(path + root_len, suffix, suffix_len + 1);
    return path;
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(buf);
        return NULL;
    }
    buf[got] = '\0';
    if (out_len != NULL) *out_len = got;
    return buf;
}

static int stylesheet_cmp(const void *a, const void *b) {
    size_t left = *(const size_t *)a;
    size_t right = *(const size_t *)b;
    return strcmp(g_assets.entries[left].logical,
                  g_assets.entries[right].logical);
}

static bool has_suffix(const char *text, const char *suffix) {
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    return text_len >= suffix_len &&
           strcmp(text + text_len - suffix_len, suffix) == 0;
}

/* Build `append_preload_links("", links)` (tags.rs): the 1,000-byte cap is
 * checked against the header-so-far length before the comma. */
static char *build_preload_links(void) {
    char *header = malloc(1);
    if (header == NULL) return NULL;
    header[0] = '\0';
    size_t len = 0;
    for (size_t i = 0; i < g_assets.stylesheet_count; i++) {
        const char *url = g_assets.entries[g_assets.stylesheets[i]].url;
        size_t url_len = strlen(url);
        /* "<" + url + ">; rel=preload; as=style; nopush" */
        size_t link_len = 1 + url_len + strlen(">; rel=preload; as=style; nopush");
        if (len + link_len > 1000) continue;
        char *grown = realloc(header, len + link_len + 2);
        if (grown == NULL) {
            free(header);
            return NULL;
        }
        header = grown;
        if (len != 0) header[len++] = ',';
        header[len++] = '<';
        memcpy(header + len, url, url_len);
        len += url_len;
        memcpy(header + len, ">; rel=preload; as=style; nopush",
               strlen(">; rel=preload; as=style; nopush"));
        len += strlen(">; rel=preload; as=style; nopush");
        header[len] = '\0';
    }
    return header;
}

cf_err cf_views_assets_configure(const char *static_root) {
    assets_clear();
    const char *root = static_root;
    if (root == NULL || root[0] == '\0') root = cf_static_root();

    char *manifest_path = join_path(root, "/public/assets/.manifest.json");
    char *importmap_path = join_path(root, "/importmap-tags.html");
    if (manifest_path == NULL || importmap_path == NULL) {
        free(manifest_path);
        free(importmap_path);
        return CF_NOMEM;
    }

    yyjson_doc *doc = yyjson_read_file(manifest_path, 0, NULL, NULL);
    if (doc == NULL) {
        free(manifest_path);
        free(importmap_path);
        return CF_IO;
    }
    yyjson_val *root_obj = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root_obj)) {
        yyjson_doc_free(doc);
        free(manifest_path);
        free(importmap_path);
        return CF_INVALID;
    }

    size_t capacity = yyjson_obj_size(root_obj);
    g_assets.entries = calloc(capacity != 0 ? capacity : 1,
                              sizeof *g_assets.entries);
    g_assets.stylesheets = calloc(capacity != 0 ? capacity : 1,
                                  sizeof *g_assets.stylesheets);
    if (g_assets.entries == NULL || g_assets.stylesheets == NULL) {
        yyjson_doc_free(doc);
        free(manifest_path);
        free(importmap_path);
        assets_clear();
        return CF_NOMEM;
    }

    yyjson_val *key, *value;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(root_obj, &iter);
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        value = yyjson_obj_iter_get_val(key);
        if (!yyjson_is_obj(value)) continue;
        yyjson_val *digested = yyjson_obj_get(value, "digested_path");
        if (digested == NULL || !yyjson_is_str(digested)) continue;
        const char *logical = yyjson_get_str(key);
        const char *digested_path = yyjson_get_str(digested);
        if (logical == NULL || digested_path == NULL) continue;
        struct asset_entry *entry = &g_assets.entries[g_assets.count];
        entry->logical = strdup(logical);
        const char *prefix = "/assets/";
        size_t url_len = strlen(prefix) + strlen(digested_path);
        entry->url = malloc(url_len + 1);
        if (entry->logical == NULL || entry->url == NULL) {
            yyjson_doc_free(doc);
            free(manifest_path);
            free(importmap_path);
            assets_clear();
            return CF_NOMEM;
        }
        memcpy(entry->url, prefix, strlen(prefix));
        memcpy(entry->url + strlen(prefix), digested_path,
               strlen(digested_path) + 1);
        g_assets.count++;
    }
    yyjson_doc_free(doc);

    /* `all_stylesheets_paths`: every CSS logical path, sorted. */
    for (size_t i = 0; i < g_assets.count; i++) {
        if (has_suffix(g_assets.entries[i].logical, ".css")) {
            g_assets.stylesheets[g_assets.stylesheet_count++] = i;
        }
    }
    if (g_assets.stylesheet_count > 1) {
        qsort(g_assets.stylesheets, g_assets.stylesheet_count,
              sizeof *g_assets.stylesheets, stylesheet_cmp);
    }

    g_assets.importmap_tags = read_file(importmap_path, NULL);
    free(manifest_path);
    free(importmap_path);
    if (g_assets.importmap_tags == NULL) {
        assets_clear();
        return CF_IO;
    }
    g_assets.loaded = true;
    g_assets.preload_links = build_preload_links();
    if (g_assets.preload_links == NULL) {
        assets_clear();
        return CF_NOMEM;
    }
    {
        cf_builder tags = {0};
        if (cf_views_stylesheet_tags(&tags) != CF_OK) {
            cf_builder_dispose(&tags);
            assets_clear();
            return CF_NOMEM;
        }
        g_assets.stylesheet_tags =
            malloc(tags.len + 1);
        if (g_assets.stylesheet_tags == NULL) {
            cf_builder_dispose(&tags);
            assets_clear();
            return CF_NOMEM;
        }
        memcpy(g_assets.stylesheet_tags, tags.ptr, tags.len);
        g_assets.stylesheet_tags[tags.len] = '\0';
        cf_builder_dispose(&tags);
    }
    return CF_OK;
}

/* ActionView::Helpers::AssetUrlHelper::URI_REGEXP / views asset_path. */
static bool is_uri(const char *source) {
    if (strncmp(source, "//", 2) == 0 || strncmp(source, "cid:", 4) == 0 ||
        strncmp(source, "data:", 5) == 0) {
        return true;
    }
    const char *scheme = strstr(source, "://");
    if (scheme == NULL || scheme == source) return false;
    for (const char *p = source; p < scheme; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || c == '-')) return false;
    }
    return true;
}

static const struct asset_entry *entry_for(const char *logical,
                                           size_t logical_len) {
    for (size_t i = 0; i < g_assets.count; i++) {
        if (strlen(g_assets.entries[i].logical) == logical_len &&
            memcmp(g_assets.entries[i].logical, logical, logical_len) == 0) {
            return &g_assets.entries[i];
        }
    }
    return NULL;
}

cf_err cf_views_asset_path(cf_span logical, cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    if (logical.len != 0 && logical.ptr == NULL) return CF_INVALID;
    if (logical.len == 0) return CF_OK;
    char *source = malloc(logical.len + 1);
    if (source == NULL) return CF_NOMEM;
    memcpy(source, logical.ptr, logical.len);
    source[logical.len] = '\0';

    cf_err rc = CF_OK;
    if (source[0] == '/' || is_uri(source)) {
        rc = cf_view_str(out, source);
    } else if (!g_assets.loaded) {
        rc = CF_INVALID;
    } else {
        /* Tail: `[?#].+` (a trailing lone '?'/'#' stays in the path). */
        size_t tail_at = SIZE_MAX;
        for (size_t i = 0; source[i] != '\0'; i++) {
            if ((source[i] == '?' || source[i] == '#') && source[i + 1] != '\0') {
                tail_at = i;
                break;
            }
        }
        char *tail = NULL;
        size_t tail_len = 0;
        char saved = '\0';
        if (tail_at != SIZE_MAX) {
            /* Keep the tail (including its '?'/'#') after looking the logical
             * path up NUL-terminated. */
            tail = source + tail_at;
            tail_len = logical.len - tail_at;
            saved = source[tail_at];
            source[tail_at] = '\0';
        }
        const struct asset_entry *entry = entry_for(source, strlen(source));
        if (tail != NULL) source[tail_at] = saved;
        if (entry == NULL) {
            rc = CF_NOT_FOUND;
        } else {
            rc = cf_view_str(out, entry->url);
            if (rc == CF_OK && tail != NULL) {
                rc = cf_view_raw(out, (cf_span){(const unsigned char *)tail,
                                                tail_len});
            }
        }
    }
    free(source);
    return rc;
}

cf_err cf_views_stylesheet_tags(cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    if (!g_assets.loaded) return CF_INVALID;
    for (size_t i = 0; i < g_assets.stylesheet_count; i++) {
        const char *url = g_assets.entries[g_assets.stylesheets[i]].url;
        cf_err rc = cf_view_str(out, "<link rel=\"stylesheet\" href=\"");
        if (rc != CF_OK) return rc;
        /* html_escape of a digested path: no escapable characters exist in a
         * manifest URL, but escape through the same helper anyway. */
        rc = cf_view_html_attr(out, (cf_span){(const unsigned char *)url,
                                         strlen(url)});
        if (rc != CF_OK) return rc;
        rc = cf_view_str(out, "\" data-turbo-track=\"reload\" />");
        if (rc != CF_OK) return rc;
        if (i + 1 < g_assets.stylesheet_count) {
            rc = cf_view_str(out, "\n");
            if (rc != CF_OK) return rc;
        }
    }
    return CF_OK;
}

cf_err cf_views_importmap_tags(cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    if (!g_assets.loaded) return CF_INVALID;
    return cf_view_str(out, g_assets.importmap_tags);
}

cf_span cf_views_assets_importmap_tags(void) {
    if (!g_assets.loaded || g_assets.importmap_tags == NULL) {
        return (cf_span){NULL, 0};
    }
    return (cf_span){(const unsigned char *)g_assets.importmap_tags,
                     strlen(g_assets.importmap_tags)};
}

cf_span cf_views_assets_stylesheet_tags(void) {
    if (!g_assets.loaded || g_assets.stylesheet_tags == NULL) {
        return (cf_span){NULL, 0};
    }
    return (cf_span){(const unsigned char *)g_assets.stylesheet_tags,
                     strlen(g_assets.stylesheet_tags)};
}

cf_err cf_views_assets_default_path(void *user, cf_span logical,
                                    cf_builder *out) {
    (void)user;
    return cf_views_asset_path(logical, out);
}

cf_err cf_views_preload_links(cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    if (!g_assets.loaded) return CF_INVALID;
    return cf_view_str(out, g_assets.preload_links);
}
