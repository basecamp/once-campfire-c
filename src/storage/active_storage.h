/* src/storage/active_storage.h — task S02: Active Storage signed tokens and
 * signed-representation route logic as pure helpers (05-storage-integrations.md
 * "S02").
 *
 * Sources translated (read-only; owned by other tasks):
 *   tmp/rust-ref/crates/campfire/src/active_storage.rs (nine route handlers,
 *       set_blob/set_representation, disk PUT, direct uploads, purge)
 *   tmp/rust-ref/crates/storage/src/{paths,disk,variation,file_server,
 *       disposition,content_types,filename,blob,storage}.rs
 *   tmp/rust-ref/crates/db/src/models/active_storage.rs (Blob, Attachment rows)
 *   tmp/rust-ref/crates/campfire/src/controllers/presenters/attachments.rs
 *       (allowed record/name combinations, assignment states)
 *   tmp/rust-ref/crates/ruby/src/rack.rs (byte-range parsing, exact vectors)
 *   tmp/rust-ref/crates/rails_compat/src/{content_disposition.rs,json.rs}
 *       (disposition format, ActiveSupport JSON string escaping)
 *
 * Every function here is callable from future actions; nothing here binds a
 * route, touches the database, or sends a response. Database rows (blob
 * lookup, variant-record insert, attachment/purge selection) stay with the
 * actions and the D01 models; concurrency against the writer is expressed
 * through caller-supplied callbacks so these helpers stay pure and unit
 * testable. Media completion (transforms, previews, analysis) is S03: the
 * representation entry point below fails loudly with CF_INTERNAL until S03
 * lands; it never approximates a transform.
 *
 * Signing uses the A01 application verifier (cf_auth_app_verifier_*, name
 * "ActiveStorage"): SHA1, strict Base64, JSON-fallback serializer, exactly
 * the rails_compat current format. Purposes and expiry match the reference:
 *   blob_id    no expiry (rails_blob_path ids never expire)
 *   variation  no expiry
 *   blob_key   now + CF_ACTIVE_SERVICE_URLS_EXPIRE_IN_SECS (5 minutes)
 *   blob_token now + CF_ACTIVE_SERVICE_URLS_EXPIRE_IN_SECS (5 minutes)
 * A signature made for one purpose never verifies for another; payloads are
 * never trusted before verification (decoding without verifying is refused
 * by construction: every verify helper verifies first, then parses).
 *
 * Error mapping:
 *   CF_OK        success (found=true) or clean rejection (found=false for
 *                bad signature/purpose/expiry/malformed token).
 *   CF_INVALID   malformed caller arguments or a verified payload whose shape
 *                is wrong for its purpose (e.g. a variation key whose JSON is
 *                not an object); for route tokens these still surface as
 *                found=false, never as trust.
 *   CF_INTERNAL  a verified value that cannot be served without S03 media
 *                work, or a JSON shape error where the reference raises
 *                internally (bad variation shape under a good signature).
 *   CF_LIMIT     an upload or range set beyond the fixed caps.
 *   CF_NOMEM/CF_IO pass through from allocation, storage and OpenSSL calls.
 * Out pointers start empty and stay empty on failure (00-contracts.md). */
#ifndef CF_STORAGE_ACTIVE_STORAGE_H
#define CF_STORAGE_ACTIVE_STORAGE_H

#include "cf.h"

#include "models/types.h"

/* `ActiveStorage.service_urls_expire_in` (active_storage.rs). */
#define CF_ACTIVE_SERVICE_URLS_EXPIRE_IN_SECS INT64_C(300)
/* Disk `show` Cache-Control (active_storage.rb initializer). */
#define CF_ACTIVE_DISK_CACHE_MAX_AGE_SECS 3600
/* Byte cap shared with S01/H02 direct uploads (16 MiB decoded body). */
#define CF_ACTIVE_UPLOAD_MAX_BYTES UINT64_C(16777216)
/* Multipart boundary the disk file server uses (file_server.rs). */
#define CF_ACTIVE_DISK_MULTIPART_BOUNDARY "AaB03x"
/* 416 body the disk file server sends (file_server.rs). */
#define CF_ACTIVE_UNSATISFIABLE_MESSAGE "Byte range unsatisfiable\n"

/* ---- signed blob ids (paths.rs) ---------------------------------------- */

/* `blob.signed_id` (purpose "blob_id"): data is the decimal id text, no
 * expiry unless has_expiry. Byte-exact with the reference for the same
 * inputs (AUTH_VEC_APP_GEN vectors). */
cf_err cf_active_blob_sign(cf_span secret_key_base, int64_t blob_id,
                           bool has_expiry, int64_t expires_us, cf_str *out);
/* `Blob.find_signed!` verification half: found=false for a bad signature,
 * wrong purpose, expiry or malformed input; the id only on CF_OK+found.
 * Nothing is decoded before the signature verifies. */
cf_err cf_active_blob_verify(cf_span secret_key_base, cf_span signed_id,
                             int64_t now_us, int64_t *out_id, bool *found);

/* ---- disk download keys and upload tokens (disk.rs) ---------------------- */

typedef struct {
    cf_str key;           /* owned storage key */
    cf_str disposition;   /* owned full Content-Disposition value */
    cf_optional_str content_type; /* absent == JSON null */
    cf_str service_name;  /* owned */
} cf_active_disk_key;

void cf_active_disk_key_dispose(cf_active_disk_key *key);

typedef struct {
    cf_str key;
    cf_optional_str content_type;
    int64_t content_length;
    cf_str checksum;      /* base64(MD5) the PUT body must match */
    cf_str service_name;
} cf_active_disk_token;

void cf_active_disk_token_dispose(cf_active_disk_token *token);

/* `service.url(...)` payload (purpose "blob_key", key order
 * key/disposition/content_type/service_name). disposition is the complete
 * `content_disposition_with` value built by cf_active_content_disposition;
 * pass has_content_type=false for JSON null. */
cf_err cf_active_disk_key_sign(cf_span secret_key_base, cf_span key,
                               cf_span disposition, cf_span content_type,
                               bool has_content_type, cf_span service_name,
                               bool has_expiry, int64_t expires_us,
                               cf_str *out);
cf_err cf_active_disk_key_verify(cf_span secret_key_base, cf_span encoded_key,
                                 int64_t now_us, cf_active_disk_key *out,
                                 bool *found);
/* `url_for_direct_upload` payload (purpose "blob_token", always expiring,
 * key order key/content_type/content_length/checksum/service_name). */
cf_err cf_active_disk_token_sign(cf_span secret_key_base, cf_span key,
                                 cf_span content_type, bool has_content_type,
                                 int64_t content_length, cf_span checksum,
                                 cf_span service_name, int64_t expires_us,
                                 cf_str *out);
cf_err cf_active_disk_token_verify(cf_span secret_key_base,
                                    cf_span encoded_token, int64_t now_us,
                                    cf_active_disk_token *out, bool *found);

/* `DiskController#update` content check: declared content type (compared
 * case-insensitively after stripping parameters) and exact content length
 * must both equal the token's. has_length=false never matches. */
bool cf_active_disk_token_acceptable(const cf_active_disk_token *token,
                                     cf_span req_content_type,
                                     int64_t req_length, bool has_length);

/* PUT bytes into the disk service with checksum enforcement
 * (DiskService::upload with checksum): stages through the S01 upload
 * boundary, compares the staged checksum with the token's, and publishes at
 * key only on a match. Mismatch deletes the staged bytes and returns
 * CF_INVALID (the action answers 422); CF_BUSY when the key already exists
 * (S01 never overwrites); missing/invalid key is CF_INVALID. */
cf_err cf_active_disk_upload(cf_storage *storage, cf_span key,
                             cf_span contents, cf_span checksum);

/* ---- filenames, dispositions, escaping, content types -------------------- */

/* `Filename#sanitized`: lossy UTF-8, strip NUL/ASCII whitespace at both
 * ends, then map U+202E and "%$|:;/<>?*\"\t\r\n\\" each to "-". */
cf_err cf_active_filename_sanitize(cf_span raw, cf_builder *out);
/* `content_disposition_with` + `ContentDisposition.format`: anything but
 * "attachment" serves inline; output is byte-exact with the reference
 * (storage.json filenames vectors, incl. transliteration). */
cf_err cf_active_content_disposition(cf_span disposition, cf_span sanitized,
                                     cf_builder *out);
/* Journey escaping for the `*filename`/token URL segments. */
cf_err cf_active_escape_path(cf_span text, cf_builder *out);
cf_err cf_active_escape_segment(cf_span text, cf_builder *out);

/* `content_types.rs` effective lists (pure data; byte identification stays
 * with S03 analysis). */
bool cf_active_content_type_variable(cf_span content_type);
bool cf_active_content_type_web_image(cf_span content_type);
bool cf_active_content_type_allowed_inline(cf_span content_type);
bool cf_active_content_type_serve_as_binary(cf_span content_type);
/* `content_type_for_serving`: binary types serve as application/octet-stream.
 * Returns either that literal or the caller's span (borrowed). */
cf_span cf_active_content_type_for_serving(cf_span content_type);
/* `forced_disposition_for_serving`: "attachment" for binary or non-inline
 * types, NULL otherwise (borrowed literal or NULL). */
const char *cf_active_forced_disposition(cf_span content_type);

/* ---- variations (variation.rs) ------------------------------------------ */

typedef enum {
    CF_ACTIVE_V_NIL = 0,
    CF_ACTIVE_V_BOOL,
    CF_ACTIVE_V_INT,
    CF_ACTIVE_V_STR, /* decoded/JSON string */
    CF_ACTIVE_V_SYM, /* Ruby symbol: digests differently from STR */
    CF_ACTIVE_V_ARR,
    CF_ACTIVE_V_HASH
} cf_active_vkind;

typedef struct cf_active_vval cf_active_vval;

typedef struct {
    char *key; /* owned NUL-terminated, len excludes NUL */
    size_t key_len;
    cf_active_vval *value; /* owned */
} cf_active_ventry;

struct cf_active_vval {
    cf_active_vkind kind;
    bool boolean;   /* V_BOOL */
    int64_t integer; /* V_INT */
    char *bytes;    /* V_STR/V_SYM owned bytes, NUL-terminated */
    size_t len;     /* byte length of bytes */
    cf_active_vval **items; /* V_ARR owned elements */
    size_t count, cap;
    cf_active_ventry *entries; /* V_HASH insertion-ordered */
    size_t hcount, hcap;
};

cf_err cf_active_vnil(cf_active_vval **out);
cf_err cf_active_vbool(bool value, cf_active_vval **out);
cf_err cf_active_vint(int64_t value, cf_active_vval **out);
cf_err cf_active_vstr(cf_span text, cf_active_vval **out);
cf_err cf_active_vsym(cf_span name, cf_active_vval **out);
cf_err cf_active_varr(cf_active_vval **out);
cf_err cf_active_varr_push(cf_active_vval *arr, cf_active_vval *item);
cf_err cf_active_vhash(cf_active_vval **out);
cf_err cf_active_vhash_set(cf_active_vval *hash, cf_span key,
                           cf_active_vval *value);
void cf_active_vval_dispose(cf_active_vval *value); /* NULL-safe; frees */
bool cf_active_vval_equal(const cf_active_vval *a, const cf_active_vval *b);

/* Ordered transformation entries of one variation (the V_HASH entries of a
 * variation object). Empty means "no transformations" (preview image as-is).
 * Helpers borrow/steal plainly: build entries with the constructors above,
 * then dispose the container (not the stolen values) with
 * cf_active_ventries_dispose. */
typedef struct {
    cf_active_ventry *items;
    size_t len, cap;
} cf_active_ventries;

void cf_active_ventries_dispose(cf_active_ventries *entries);
bool cf_active_ventries_empty(const cf_active_ventries *entries);
cf_err cf_active_ventries_push(cf_active_ventries *entries, cf_span key,
                               cf_active_vval *value); /* steals value */

/* `default_to(defaults)`: defaults first, overridden in place by the
 * variation's own keys (variation_for needs this before digesting). */
cf_err cf_active_variation_default(const cf_active_ventries *defaults,
                                   const cf_active_ventries *variation,
                                   cf_active_ventries *out);
/* `digest`: base64(SHA1(Marshal.dump(hash))), byte-exact incl. the
 * symbol/string split and bignum integers (storage.json variations). */
cf_err cf_active_variation_digest(const cf_active_ventries *variation,
                                  cf_str *out);
/* `Variation#key` (purpose "variation", never expiring): data is the
 * transformations-as-JSON text (symbols as strings, insertion order). */
cf_err cf_active_variation_sign(cf_span secret_key_base,
                                const cf_active_ventries *variation,
                                cf_str *out);
/* `Variation.decode`: signature failure (or malformed token) is found=false;
 * a verified key whose JSON is not a transformations object, or carries a
 * float, is CF_INTERNAL (the reference raises internally, not 404). Decoded
 * string values stay strings (never symbols), exactly like the reference. */
cf_err cf_active_variation_verify(cf_span secret_key_base, cf_span key,
                                  int64_t now_us, cf_active_ventries *out,
                                  bool *found);

/* ---- byte ranges (ruby_compat rack.rs, exact vectors) -------------------- */

typedef struct {
    uint64_t start, end; /* inclusive, within size */
} cf_active_range;

typedef enum {
    CF_ACTIVE_RANGE_FULL = 0, /* no usable Range header: serve 200 */
    CF_ACTIVE_RANGE_PART,     /* satisfiable ranges for 206 */
    CF_ACTIVE_RANGE_UNSAT,     /* 416 */
    CF_ACTIVE_RANGE_INVALID   /* malformed: disk serves 200, proxy 416 */
} cf_active_range_outcome;

/* `Rack::Utils.get_byte_ranges`: has_header=false (or empty/whitespace-only
 * header) is FULL; size==0 is FULL; >=100 commas is INVALID; unsatisfiable or
 * over-broad sets are UNSAT. Ranges come back in header order in a malloc'd
 * array the caller releases with cf_active_ranges_dispose (NULL-safe). */
cf_err cf_active_range_parse(cf_span header, bool has_header, uint64_t size,
                             cf_active_range **out, size_t *out_n,
                             cf_active_range_outcome *outcome);
void cf_active_ranges_dispose(cf_active_range *ranges);

/* ActiveRecord-style integer cast for `byte_size` (integer.rs): leading
 * SPACE, optional sign, then digits (underscores between digits, 0d prefix);
 * ok=false unless it starts like a number and fits in an i64. */
bool cf_active_integer_cast(cf_span text, int64_t *out);

/* ---- serve planning (send_blob_*, file_server.rs) ------------------------ */

typedef struct {
    int status; /* 200, 206, 304, 404 or 416 (OPTIONS answers 200) */
    cf_str content_type;        /* owned; "" when none */
    cf_str content_disposition; /* owned; "" when none */
    cf_str content_range;       /* owned; 206 single-range only */
    cf_str last_modified;       /* owned; disk only */
    cf_str cache_control;       /* owned; disk only */
    cf_str allow;               /* owned; OPTIONS only */
    cf_str boundary;            /* owned; multipart 206 only */
    uint64_t content_length;
    bool has_body;      /* false for HEAD/304/404/416-from-proxy/OPTIONS */
    bool accept_ranges; /* blobs proxy 200/206 only */
    bool has_single;    /* single file slice serves the body */
    cf_active_range single;
    cf_active_range *ranges; /* multipart slices, malloc'd */
    size_t n_ranges;
} cf_active_serve;

void cf_active_serve_dispose(cf_active_serve *plan); /* NULL-safe */

/* `Blobs::ProxyController#show` planning: file_exists=false is 404 (the
 * reference expires and answers not_found); a present non-blank Range goes
 * down the byte-range path (malformed there is 416); otherwise 200 with
 * Accept-Ranges. disposition_param is the caller's `disposition` query value
 * (NULL/empty = none); boundary_hex16 is 32 caller randomness hex chars for
 * multipart responses (NULL = fixed fallback, tests only). */
cf_err cf_active_proxy_serve(bool file_exists, bool has_range,
                             cf_span range_header, uint64_t file_size,
                             cf_span content_type, cf_span sanitized_filename,
                             cf_span disposition_param, bool has_disposition,
                             cf_span boundary_hex16, bool has_boundary,
                             cf_active_serve *plan);
/* `DiskController#show` planning (file_server.rs): OPTIONS, conditional GET
 * on an exact If-Modified-Since match, HEAD, single and multipart ranges
 * with the fixed boundary, and 416s. content_type_opt empty = absent
 * (defaults to application/octet-stream); same for disposition_value. */
cf_err cf_active_disk_serve(cf_span method, bool has_range,
                            cf_span range_header, bool has_ims,
                            cf_span if_modified_since, bool file_exists,
                            uint64_t file_size, cf_span mtime_httpdate,
                            cf_span content_type_opt,
                            cf_span disposition_value, cf_active_serve *plan);

/* Multipart framing bytes between file slices (exact reference layouts):
 * proxy style uses capitalized header names, disk style lowercase with the
 * fixed text/plain part type. */
cf_err cf_active_proxy_part_heading(cf_span boundary, cf_span content_type,
                                    uint64_t start, uint64_t end,
                                    uint64_t size, cf_builder *out);
cf_err cf_active_proxy_part_trailer(cf_span boundary, cf_builder *out);
cf_err cf_active_disk_part_heading(uint64_t start, uint64_t end,
                                   uint64_t size, cf_builder *out);
cf_err cf_active_disk_part_trailer(cf_builder *out);

/* `Time#httpdate` for Last-Modified comparisons ("Sun, 06 Nov 1994
 * 08:49:37 GMT", C-locale fixed names, no locale dependence). */
cf_err cf_active_httpdate(int64_t unix_seconds, char out[30]);

/* ---- redirect / direct-upload route helpers ------------------------------ */

/* `blob.url(disposition:)` disk path (the redirect Location path; the action
 * prefixes protocol+host): signs {key, disposition, content_type,
 * service_name} for blob_key expiring at expires_us, then appends the
 * escaped sanitized filename. disposition_kind is "attachment" or anything
 * else (inline); has_content_type=false sends JSON null. */
cf_err cf_active_service_url_path(cf_span secret_key_base, cf_span key,
                                  int64_t expires_us,
                                  cf_span sanitized_filename,
                                  cf_span content_type, bool has_content_type,
                                  cf_span disposition_kind,
                                  cf_span service_name, cf_builder *out);
/* `rails_blob_path`-style redirect path for a signed id (kept for the
 * redirect/proxy route helpers; the disk Location above is what blob_url
 * uses). Appends ?disposition= when has_disposition. */
cf_err cf_active_blob_redirect_path(cf_span secret_key_base, int64_t blob_id,
                                    cf_span sanitized_filename,
                                    cf_span disposition, bool has_disposition,
                                    cf_builder *out);

/* Direct-upload parameter check (direct_uploads_create): filename and
 * checksum must be present and non-empty (else UNPROCESSABLE), byte_size
 * must cast to an integer (else UNPROCESSABLE) within 0..=16MiB (else
 * TOO_LARGE). */
typedef enum {
    CF_ACTIVE_UPLOAD_OK = 0,
    CF_ACTIVE_UPLOAD_UNPROCESSABLE = 422,
    CF_ACTIVE_UPLOAD_TOO_LARGE = 413
} cf_active_upload_decision;

cf_active_upload_decision cf_active_direct_upload_check(cf_span byte_size_text,
                                                        bool has_byte_size,
                                                        bool has_filename,
                                                        bool has_checksum,
                                                        int64_t *out_size);
/* `blob.as_json(...).merge(direct_upload: ...)` body (exact key order and
 * ActiveSupport string escaping). metadata_json is the blob metadata object
 * text, inserted verbatim. created_at_iso is the json_time rendering of the
 * stored timestamp. Nulls for absent content_type/checksum/upload type. */
cf_err cf_active_direct_upload_json(int64_t id, cf_span key,
                                    cf_span filename_raw,
                                    cf_span content_type,
                                    bool has_content_type,
                                    cf_span metadata_json,
                                    cf_span service_name, int64_t byte_size,
                                    cf_span checksum, bool has_checksum,
                                    cf_span created_at_iso, cf_span signed_id,
                                    cf_span url,
                                    cf_span upload_content_type,
                                    bool has_upload_content_type,
                                    cf_builder *out);
/* Stored `created_at` ("YYYY-MM-DD HH:MM:SS[.ffffff]", UTC) as
 * ActiveSupport::JSON encodes times (milliseconds ISO 8601); garbage passes
 * through unchanged like the reference. */
cf_err cf_active_json_time(cf_span db_time, cf_builder *out);

/* ---- attachments (presenters/attachments.rs) ----------------------------- */

/* The four assignment states plus invalid. absent=unchanged; permitted
 * null/""=delete; upload=staged file next; verified blob_id string=existing
 * blob; anything else=invalid ("Could not find or build blob"). */
typedef enum {
    CF_ACTIVE_ATTACH_UNCHANGED = 0,
    CF_ACTIVE_ATTACH_DELETE,
    CF_ACTIVE_ATTACH_UPLOAD,
    CF_ACTIVE_ATTACH_SIGNED,
    CF_ACTIVE_ATTACH_INVALID
} cf_active_attach_kind;

typedef enum {
    CF_ACTIVE_SHAPE_ABSENT = 0, /* key not given */
    CF_ACTIVE_SHAPE_NULL,       /* permitted nil */
    CF_ACTIVE_SHAPE_EMPTY,     /* permitted "" */
    CF_ACTIVE_SHAPE_UPLOAD,    /* multipart file */
    CF_ACTIVE_SHAPE_STRING     /* plain string: signed blob id or invalid */
} cf_active_attach_input;

/* Classifies one permitted parameter value. STRING inputs verify (purpose
 * blob_id, never decoded first): verified ids report SIGNED with out_blob_id,
 * everything else reports INVALID. Pure otherwise: no I/O, no allocation. */
cf_err cf_active_attach_classify(cf_span secret_key_base,
                                 cf_active_attach_input input,
                                 cf_span string_value, int64_t now_us,
                                 cf_active_attach_kind *out_kind,
                                 int64_t *out_blob_id);
/* Allowed (record_type, name) combinations from the reference: (User,avatar),
 * (Account,logo), (Message,attachment), (ActiveStorage::VariantRecord,image)
 * and (ActiveStorage::Blob,preview_image). Everything else is rejected so a
 * future action cannot attach a blob to an unexpected record. */
bool cf_active_attach_allowed(cf_span record_type, cf_span name);

/* ---- upload staging (storage.rs `Staged` / unfurl) ------------------------- */

/* `DiskService` name the app configures (Campfire's single "local" service). */
#define CF_ACTIVE_SERVICE_NAME "local"

struct cf_storage_upload; /* S01 staging handle, owned below */

/* A new blob whose file is already published in the disk service but whose
 * row is not saved yet: `Staged` from storage.rs plus the `NewBlob` fields
 * `Blob.build_after_unfurling(identify: true)` computes. Dispose rolls the
 * file back unless committed (the staged-blob drop rule); a failed DB write
 * therefore leaves no final file. All cf_str fields are owned. */
typedef struct {
    cf_str key;                   /* reference storage key */
    cf_str filename;              /* raw normalized upload filename */
    cf_optional_str content_type; /* marcel identification, always present */
    cf_str metadata;              /* `{"identified":true}` */
    cf_str service_name;          /* owned copy of CF_ACTIVE_SERVICE_NAME */
    int64_t byte_size;
    cf_str checksum;              /* base64(MD5), present */
    struct cf_storage_upload *upload; /* owned; final file until commit */
} cf_active_staged;

/* Keep the published file (the blob row committed). CF_INVALID when the
 * staged value is NULL or was already disposed; CF_OK is idempotent. */
cf_err cf_active_staged_commit(cf_active_staged *staged);
/* Roll back (delete the final file) unless committed, then free. NULL-safe. */
void cf_active_staged_dispose(cf_active_staged *staged);

/* `Storage::stage_file(source, filename, declared_type)` with `identify:
 * true`: stream `src_fd` (offset 0..EOF, read-only, caller-owned) through
 * S01's staging into `storage` under a fresh key, identify the content type
 * with Marcel over the leading bytes and the sanitized filename, compute the
 * incremental base64(MD5) and publish at the key. `storage` must outlive the
 * staged value (S01 keeps it borrowed). Maps the S01 cap to CF_LIMIT
 * (nothing kept) and staged-file failures to their S01 codes; NULL/negative
 * arguments are CF_INVALID. */
cf_err cf_active_stage_upload(cf_storage *storage, int src_fd,
                              cf_span filename, cf_span declared_type,
                              bool has_declared_type, cf_active_staged *out);

/* Imported integration reply with explicit cap; user upload entry keeps16MiB. */
cf_err cf_active_stage_import(cf_storage *storage, int src_fd,
    cf_span filename, cf_span declared_type, bool has_declared_type,
    uint64_t byte_limit, cf_active_staged *out);

/* `Storage::analyzed_metadata` for an analyzer the port can run without S03:
 * content types outside image/video/audio (Analyzer::Null) merge
 * `"analyzed": true` into the stored metadata object preserving key order
 * (`metadata.merge(extracted.merge(analyzed: true))` with an empty
 * extracted). A content type whose analyzer is Image/Video/Audio returns
 * CF_INTERNAL without producing output: those need S03's pinned tools and
 * are never approximated. `metadata_json` must be a JSON object (the stored
 * column), otherwise CF_INTERNAL like the reference's JSON raise. */
cf_err cf_active_analyze_metadata(cf_span content_type,
                                  cf_span metadata_json, cf_builder *out);

/* `Blob#default_variant_format`: web images keep their format, everything
 * else becomes PNG (marcel tables; `format()` agreement or first registered
 * extension, then "png"). Appends to out. */
cf_err cf_active_default_variant_format(cf_span content_type,
                                        cf_span filename_raw, cf_builder *out);

/* `url_for(blob.representation(variation))` path (paths.rs::
 * representation_redirect_path): /rails/active_storage/representations/
 * redirect/<signed_blob_id>/<signed_variation_key>/<escaped sanitized
 * filename>. The variation entries are signed byte-exactly as given (symbol
 * vs string matters). */
cf_err cf_active_representation_redirect_path(cf_span secret_key_base,
                                              int64_t blob_id,
                                              cf_span filename_raw,
                                              const cf_active_ventries *variation,
                                              cf_builder *out);

/* ---- variant races and purge (STORE-03) ----------------------------------- */

/* `create_or_find_by!` coordination: try_insert attempts the unique
 * (blob_id, digest) insert and reports inserted=true exactly when this
 * caller won. The winner keeps its staged file; the loser drops only its own
 * file and re-reads the winner's record (re-read stays with the action).
 * try_insert receives the base64 digest and the caller's ctx. */
typedef cf_err (*cf_active_variant_insert_fn)(void *ctx, cf_span digest,
                                              bool *inserted);

cf_err cf_active_variant_claim(void *ctx, cf_span digest,
                               cf_active_variant_insert_fn try_insert,
                               bool *won);

/* Purge gate (`Blob#purge` refusal): referenced>0 refuses (the job is a
 * no-op success and emits nothing); 0 proceeds to delete the files. */
bool cf_active_purge_proceed(size_t attachment_refs);
/* File half of purge (`Blob#delete`): the key file, plus the legacy
 * untracked-variants directory for images. Missing files are success;
 * permission/I-O failures return CF_IO for the job to record as failed.
 * Only validated storage keys ever reach the filesystem (never a URL or an
 * original filename). */
cf_err cf_active_purge_files(cf_storage *storage, cf_span key, bool is_image);

/* Process/reuse a representation on a request worker. Expensive file/media
 * work precedes the writer; output blob owns its fields. ctx supplies app and
 * reader, and no writer transaction may be held by the caller. */
struct cf_blob;
cf_err cf_active_processed_representation(cf_ctx *ctx, const struct cf_blob *blob,
                                          const cf_active_ventries *variation,
                                          struct cf_blob *out);

/* ---- S03 boundary (fail loudly, never approximate) ------------------------ */

/* Representation processing (variants, previews, analysis) needs the S03
 * media workers and pinned toolchains. Until they land this returns
 * CF_INTERNAL with the documented reason so a route can never serve an
 * approximated or stub transform. */
cf_err cf_active_representation_process(cf_span content_type,
                                        bool has_transformations);

#endif /* CF_STORAGE_ACTIVE_STORAGE_H */
