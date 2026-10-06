/* src/storage/marcel.c — Marcel identification port; see marcel.h for the
 * contract and translated sources. The generated tables live in
 * marcel_tables.h (data only). */
#include "storage/marcel.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "storage/marcel_tables.h"

/* ------------------------------------------------------------- spans */

static bool span_eq_bytes(const char *a, size_t alen, const char *b, size_t blen) {
    return alen == blen && (alen == 0 || memcmp(a, b, alen) == 0);
}

/* Rust `str::to_lowercase` for the ASCII content-type/filename data these
 * tables carry; non-ASCII bytes pass through unchanged (documented
 * approximation, as in the rest of this port's ASCII metadata paths). */
static char *lower_ascii_copy(cf_span text) {
    char *out = malloc(text.len + 1);
    if (out == NULL) return NULL;
    for (size_t i = 0; i < text.len; i++) {
        unsigned char c = text.ptr[i];
        out[i] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }
    out[text.len] = '\0';
    return out;
}

static int key_compare(const void *key, const void *element) {
    /* bsearch comparator over NUL-terminated string arrays: byte order, like
     * Rust's `str` Ord for the sorted table keys. */
    const char *name = key;
    const char *other = *(const char *const *)element;
    return strcmp(name, other);
}

/* `Marcel::Magic.by_extension`: case-insensitive, with or without the dot. */
static const char *by_extension(cf_span extension) {
    char *lower = lower_ascii_copy(extension);
    if (lower == NULL) return NULL;
    const char *name = lower;
    if (name[0] == '.') name++;
    const char *found = NULL;
    if (name[0] != '\0') {
        const void *hit =
            bsearch(name, marcel_extension_keys, MARCEL_EXTENSIONS_COUNT,
                    sizeof marcel_extension_keys[0], key_compare);
        if (hit != NULL) {
            size_t index =
                (size_t)((const char *const *)hit - marcel_extension_keys);
            found = marcel_extension_types[index];
        }
    }
    free(lower);
    return found;
}

/* `File.basename(path)`: last component, ignoring trailing slashes. */
static cf_span basename_of(cf_span path) {
    size_t end = path.len;
    while (end > 0 && path.ptr[end - 1] == '/') end--;
    if (end == 0) {
        if (path.len == 0) return (cf_span){path.ptr, 0};
        return (cf_span){path.ptr + path.len - 1, 1}; /* "/" */
    }
    size_t start = end;
    while (start > 0 && path.ptr[start - 1] != '/') start--;
    return (cf_span){path.ptr + start, end - start};
}

cf_span cf_marcel_extname(cf_span path) {
    cf_span base = basename_of(path);
    size_t start = 0;
    while (start < base.len && base.ptr[start] == '.') start++;
    cf_span trimmed = {base.ptr + start, base.len - start};
    for (size_t i = trimmed.len; i > 0; i--) {
        if (trimmed.ptr[i - 1] == '.') {
            return (cf_span){trimmed.ptr + i - 1, trimmed.len - (i - 1)};
        }
    }
    return (cf_span){trimmed.ptr + trimmed.len, 0};
}

/* `Marcel::Magic.by_path`: the extension per Ruby's `File.extname`. */
static const char *by_path(cf_span path) {
    cf_span ext = cf_marcel_extname(path);
    return by_extension(ext);
}

/* ------------------------------------------------------------- magic */

/* `IO#read(length)` after skipping `offset` bytes: NULL at EOF, otherwise up
 * to `length` bytes (an empty request always succeeds). */
static bool read_at(cf_span data, size_t offset, size_t length, cf_span *out) {
    if (length == 0) {
        *out = (cf_span){data.ptr + (offset < data.len ? offset : data.len), 0};
        return true;
    }
    if (offset >= data.len) return false;
    size_t avail = data.len - offset;
    *out = (cf_span){data.ptr + offset, length < avail ? length : avail};
    return true;
}

static bool matches_any(cf_span data, const struct marcel_match *matches,
                        size_t count, size_t depth) {
    if (depth > 64) return false; /* the pinned tables are acyclic */
    for (size_t i = 0; i < count; i++) {
        const struct marcel_match *m = &matches[i];
        if (m->value == NULL) continue; /* `let Some(value) else return false` */
        cf_span window;
        bool hit;
        if (m->range_end != SIZE_MAX) {
            size_t length = m->range_end - m->offset + m->value_len;
            if (!read_at(data, m->offset, length, &window)) continue;
            hit = m->value_len == 0;
            for (size_t at = 0; !hit && at + m->value_len <= window.len; at++) {
                if (memcmp(window.ptr + at, m->value, m->value_len) == 0) hit = true;
            }
        } else {
            if (!read_at(data, m->offset, m->value_len, &window)) continue;
            hit = window.len == m->value_len &&
                  (m->value_len == 0 ||
                   memcmp(window.ptr, m->value, m->value_len) == 0);
        }
        if (hit &&
            (m->children_len == 0 ||
             matches_any(data, m->children, m->children_len, depth + 1))) {
            return true;
        }
    }
    return false;
}

/* `Marcel::Magic.by_magic`: the first table row whose matches hit (lowercased
 * table type, like the pinned port). Returns a borrowed table string. */
static const char *by_magic(cf_span data) {
    for (size_t i = 0; i < MARCEL_MAGIC_COUNT; i++) {
        if (matches_any(data, marcel_magic_matches[i],
                        marcel_magic_match_counts[i], 0)) {
            return marcel_magic_types[i];
        }
    }
    return NULL;
}

size_t cf_marcel_magic_prefix_len(void) {
    return MARCEL_MAGIC_PREFIX_LEN;
}

/* -------------------------------------------------- declared types */

/* Declared types are downcased, stripped of parameters (the first of
 * [';', ',', ' ', '\t', '\n', '\r', '\v', '\f']) and ignored when binary.
 * Returns an owned lowercase copy or NULL when unusable. */
static char *for_declared_type(cf_span declared) {
    char *lower = lower_ascii_copy(declared);
    if (lower == NULL) return NULL;
    size_t end = 0;
    while (lower[end] != '\0' && lower[end] != ';' && lower[end] != ',' &&
           lower[end] != ' ' && lower[end] != '\t' && lower[end] != '\n' &&
           lower[end] != '\r' && lower[end] != '\v' && lower[end] != '\f') {
        end++;
    }
    lower[end] = '\0';
    bool has_slash = false;
    for (size_t i = 0; i < end; i++) {
        if (lower[i] == '/') has_slash = true;
    }
    if (!has_slash || strcmp(lower, CF_MARCEL_BINARY) == 0) {
        free(lower);
        return NULL;
    }
    return lower;
}

/* ------------------------------------------------- parent graph */

static const char *const *parents_of(cf_span content_type, size_t *count) {
    char *lower = lower_ascii_copy(content_type);
    if (lower == NULL) {
        *count = 0;
        return NULL;
    }
    const void *hit =
        bsearch(lower, marcel_type_parents_keys, MARCEL_TYPE_PARENTS_COUNT,
                sizeof marcel_type_parents_keys[0], key_compare);
    free(lower);
    if (hit == NULL) {
        *count = 0;
        return NULL;
    }
    size_t index =
        (size_t)((const char *const *)hit - marcel_type_parents_keys);
    *count = marcel_type_parents_lens[index];
    return marcel_type_parents_data[index];
}

/* `Marcel::Magic.child?`: equal or a transitive child through TYPE_PARENTS.
 * `span` need not be NUL-terminated; tables are. */
static bool is_child_span(cf_span child, const char *parent, size_t depth) {
    if (depth > 64) return false;
    if (strlen(parent) == child.len &&
        memcmp(parent, child.ptr, child.len) == 0) {
        return true;
    }
    size_t count = 0;
    const char *const *parents = parents_of(child, &count);
    for (size_t i = 0; i < count; i++) {
        if (is_child_span((cf_span){(const unsigned char *)parents[i],
                                    strlen(parents[i])},
                          parent, depth + 1)) {
            return true;
        }
    }
    return false;
}

static bool is_child(const char *child, const char *parent) {
    return is_child_span((cf_span){(const unsigned char *)child, strlen(child)},
                         parent, 0);
}

const char *cf_marcel_first_extension(cf_span content_type) {
    char *lower = lower_ascii_copy(content_type);
    if (lower == NULL) return NULL;
    const void *hit =
        bsearch(lower, marcel_type_exts_keys, MARCEL_TYPE_EXTS_COUNT,
                sizeof marcel_type_exts_keys[0], key_compare);
    free(lower);
    if (hit == NULL) return NULL;
    size_t index = (size_t)((const char *const *)hit - marcel_type_exts_keys);
    if (marcel_type_exts_lens[index] == 0) return NULL;
    return marcel_type_exts_data[index][0];
}

cf_err cf_marcel_for_extension(cf_span extension, bool *found,
                               cf_builder *out) {
    if (found == NULL || out == NULL) return CF_INVALID;
    *found = false;
    if (extension.len != 0 && extension.ptr == NULL) return CF_INVALID;
    const char *type = by_extension(extension);
    if (type == NULL) return CF_OK;
    *found = true;
    return cf_builder_append(out, (cf_span){(const unsigned char *)type,
                                            strlen(type)});
}

/* ------------------------------------------------------------ identify */

struct candidate {
    const char *text;
    size_t len;
    char *owned; /* freed after use */
};

cf_err cf_marcel_identify(cf_span data, cf_span name, cf_span declared_type,
                          bool has_declared, cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    if ((data.len != 0 && data.ptr == NULL) ||
        (name.len != 0 && name.ptr == NULL)) {
        return CF_INVALID;
    }
    struct candidate candidates[3];
    size_t count = 0;
    const char *magic = by_magic(data);
    if (magic != NULL) {
        candidates[count++] = (struct candidate){magic, strlen(magic), NULL};
    }
    if (has_declared) {
        if (declared_type.len != 0 && declared_type.ptr == NULL) {
            return CF_INVALID;
        }
        char *lower = for_declared_type(declared_type);
        if (lower != NULL) {
            candidates[count++] =
                (struct candidate){lower, strlen(lower), lower};
        }
    }
    const char *filename_type = name.len != 0 ? by_path(name) : NULL;
    if (filename_type != NULL) {
        candidates[count++] =
            (struct candidate){filename_type, strlen(filename_type), NULL};
    }
    /* Unique in first-seen order, then the binary fallback. */
    static const char binary[] = CF_MARCEL_BINARY;
    struct candidate unique[4];
    size_t unique_count = 0;
    for (size_t i = 0; i < count; i++) {
        bool seen = false;
        for (size_t k = 0; k < unique_count; k++) {
            if (span_eq_bytes(candidates[i].text, candidates[i].len,
                              unique[k].text, unique[k].len)) {
                seen = true;
                break;
            }
        }
        if (!seen) unique[unique_count++] = candidates[i];
    }
    bool has_binary = false;
    for (size_t k = 0; k < unique_count; k++) {
        if (span_eq_bytes(unique[k].text, unique[k].len, binary,
                          sizeof binary - 1)) {
            has_binary = true;
            break;
        }
    }
    if (!has_binary) {
        unique[unique_count++] =
            (struct candidate){binary, sizeof binary - 1, NULL};
    }
    /* most_specific_type: later candidates only win when children. */
    size_t pick = 0;
    for (size_t i = 1; i < unique_count; i++) {
        if (is_child(unique[i].text, unique[pick].text)) pick = i;
    }
    cf_err rc = cf_builder_append(
        out, (cf_span){(const unsigned char *)unique[pick].text,
                       unique[pick].len});
    for (size_t i = 0; i < count; i++) free(candidates[i].owned);
    return rc;
}
