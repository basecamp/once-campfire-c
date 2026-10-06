/* src/storage/marcel.h — Marcel 1.1.0 content-type identification
 * (tmp/rust-ref/crates/storage/src/marcel.rs and its generated tables.rs),
 * the identity half of S02's `Blob.build_after_unfurling` (05-storage S02:
 * "Blob metadata and variant records preserve the schema types").
 *
 * Sources translated:
 *   tmp/rust-ref/crates/storage/src/marcel.rs  (identify, by_magic,
 *       by_path/by_extension, extensions, is_child, magic_prefix_len,
 *       for_declared_type, most_specific_type)
 *   tmp/rust-ref/crates/storage/src/tables.rs  (dumped from the reference
 *       image's marcel bundle; regenerated into marcel_tables.h by
 *       tests/fixtures/tools/marcel_tables.py)
 *   tmp/rust-ref/crates/storage/src/filename.rs (extname for by_path)
 *
 * The lookup semantics are the pin's exactly: byte-literal magic matching
 * (the pinned Rust port compares the table values literally, so regex-shaped
 * entries never match), first-hit magic order, declared types downcased,
 * stripped at the first delimiter and ignored when binary, and
 * most_specific_type's unique-candidate walk with the parent graph from
 * TYPE_PARENTS. Nothing here runs a media tool: identification reads only
 * the leading bytes the caller supplies.
 *
 * Error mapping: CF_INVALID for NULL arguments; CF_NOMEM/allocation errors
 * pass through cf_builder; the identified text is appended to `out` (the
 * caller owns/disposes the builder).
 */
#ifndef CF_STORAGE_MARCEL_H
#define CF_STORAGE_MARCEL_H

#include "cf.h"

/* `Marcel::MimeType::BINARY`. */
#define CF_MARCEL_BINARY "application/octet-stream"

/* `Marcel::MimeType.for(io, name:, declared_type:)` (`identify`): the content
 * type for `data`'s leading bytes, the sanitized filename `name` (may be
 * empty) and the part's declared Content-Type (`has_declared` false for
 * none). The type text is appended to out. */
cf_err cf_marcel_identify(cf_span data, cf_span name, cf_span declared_type,
                          bool has_declared, cf_builder *out);

/* `Marcel::MimeType.for(extension:)` (`by_extension`): case-insensitive,
 * with or without a leading dot. found=false leaves out untouched. */
cf_err cf_marcel_for_extension(cf_span extension, bool *found,
                               cf_builder *out);

/* `Marcel::Magic.new(type).extensions.first()`: the first registered
 * extension for a content type, or NULL when the table has none (borrowed
 * literal). */
const char *cf_marcel_first_extension(cf_span content_type);

/* `marcel::magic_prefix_len()`: identifying this many leading bytes gives
 * the same answer as identifying all of them (65555 for the pinned table). */
size_t cf_marcel_magic_prefix_len(void);

/* `File.extname(path)` on Unix (filename.rs::extname), borrowed from path. */
cf_span cf_marcel_extname(cf_span path);

#endif /* CF_STORAGE_MARCEL_H */
