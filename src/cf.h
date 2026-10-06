/* Campfire C shared contract (installed by F01 from
 * docs/devel/implementation/contracts/api.h; integrator owns subsequent
 * changes). All mutable object internals are private unless shown here. */
#ifndef CF_H
#define CF_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CF_OK = 0, CF_NOMEM, CF_LIMIT, CF_INVALID, CF_NOT_FOUND,
    CF_FORBIDDEN, CF_BUSY, CF_IO, CF_DB, CF_INTERNAL
} cf_err;
const char *cf_err_name(cf_err); /* stable short name for sanitized logs */
typedef struct { const unsigned char *ptr; size_t len; } cf_span;
typedef struct { unsigned char *ptr; size_t len, cap; } cf_builder;
typedef struct cf_buf cf_buf;
typedef struct cf_params cf_params;
typedef struct cf_headers cf_headers;
typedef struct cf_app cf_app;
typedef struct cf_db cf_db;
typedef struct cf_tx cf_tx;
typedef struct cf_cache cf_cache;
typedef struct cf_config cf_config;
typedef struct cf_cable cf_cable;
typedef struct cf_jobs cf_jobs;
typedef struct cf_storage cf_storage;
typedef struct cf_crypto cf_crypto;

typedef struct { uint32_t loop, slot; uint64_t generation; } cf_conn_id;
typedef struct { bool present; int64_t value; } cf_optional_i64;
typedef enum { CF_GET, CF_HEAD, CF_POST, CF_PUT, CF_PATCH, CF_DELETE,
               CF_OPTIONS, CF_OTHER } cf_method;
typedef struct { cf_span name, value; } cf_header;
typedef struct {
    cf_conn_id connection;
    uint64_t sequence;
    cf_method method, original_method;
    cf_span raw_method, target, path, query, body, peer_ip;
    cf_header headers[100];
    size_t header_count;
    bool tls, close_after;
    cf_buf *storage; /* owns backing for every span above */
    cf_params *params; /* owned merged body/query/path tree */
} cf_request;
typedef enum { CF_BODY_NONE, CF_BODY_BUFFER, CF_BODY_FILE } cf_body_kind;
typedef struct {
    unsigned status;
    cf_headers *headers; /* owned; duplicate Set-Cookie permitted */
    cf_body_kind body_kind;
    cf_buf *body; /* one owned reference when BUFFER */
    int file_fd; /* owned when FILE; -1 otherwise */
    uint64_t file_offset, file_length;
    bool close_after;
} cf_response;
typedef struct {
    uint32_t id; /* routes.json id, stable within this specification */
    cf_params *path_params; /* owned by context */
} cf_route_match;
typedef enum { CF_AUTH_NONE, CF_AUTH_SESSION, CF_AUTH_BOT } cf_auth_kind;
typedef struct {
    cf_auth_kind kind;
    int64_t user_id, session_id;
    int role, status;
    bool activity_due;
} cf_identity;
typedef struct cf_ctx {
    cf_app *app;               /* borrowed until all workers join */
    cf_db *reader;             /* borrowed, worker-owned */
    const cf_request *request; /* borrowed, immutable */
    cf_response *response;    /* borrowed output, initially empty */
    cf_route_match route;     /* owned path parameters */
    cf_identity identity;
    void *private_state;      /* A00 alone owns cookie/view context internals */
} cf_ctx;
typedef cf_err (*cf_action_fn)(cf_ctx *);

/* Non-owning spans never extend their backing object's lifetime. */
cf_err cf_builder_append(cf_builder *, cf_span);
void cf_builder_dispose(cf_builder *);
cf_err cf_buf_copy(cf_span, cf_buf **out);
cf_err cf_builder_freeze(cf_builder *, cf_buf **out); /* consumes builder on OK */
cf_buf *cf_buf_retain(cf_buf *);
void cf_buf_release(cf_buf *);
cf_span cf_buf_span(const cf_buf *);
void cf_request_destroy(cf_request *);
void cf_response_init(cf_response *);
void cf_response_dispose(cf_response *);
cf_err cf_response_header(cf_response *, cf_span name, cf_span value);
cf_err cf_response_body(cf_response *, cf_buf *); /* retain; caller retains ownership */
cf_err cf_response_file(cf_response *, int fd, uint64_t offset, uint64_t len);
/* response_file takes fd only on OK. Sets kind; replaces/disposes prior body. */

/* CF_NOT_FOUND means absent, distinct from null / empty / wrong type. */
typedef enum { CF_PARAM_NULL, CF_PARAM_STRING, CF_PARAM_NUMBER,
               CF_PARAM_BOOL, CF_PARAM_ARRAY, CF_PARAM_OBJECT,
               CF_PARAM_UPLOAD } cf_param_kind;
typedef struct cf_param cf_param;
const cf_param *cf_param_get(const cf_params *, cf_span name);
cf_param_kind cf_param_type(const cf_param *);
cf_err cf_param_string(const cf_param *, cf_span *out);
size_t cf_param_count(const cf_param *); /* array elements / object fields; 0 otherwise */
const cf_param *cf_param_at(const cf_param *, size_t index);
const cf_param *cf_param_field(const cf_param *, cf_span name); /* exact object key */
cf_err cf_param_i64(const cf_param *, cf_optional_i64 *out);
cf_err cf_param_bool(const cf_param *, bool *present, bool *out);
/* H02 multipart upload accessor: one file part as
 * `ActionDispatch::Http::UploadedFile`. Borrowed from the params: the spans
 * live in the params arena and `fd` is closed by cf_params_destroy; the
 * caller must not close it. `fd` is read-only and positioned at 0.
 * CF_NOT_FOUND for a NULL param, CF_INVALID for a non-upload kind. */
typedef struct {
    cf_span filename;     /* Rack-normalized original filename */
    cf_span content_type; /* declared part Content-Type (empty when absent) */
    bool has_content_type;
    int64_t size;         /* decoded byte length */
    int fd;               /* spool file descriptor (params-owned) */
} cf_upload;
cf_err cf_param_upload(const cf_param *, cf_upload *out);
cf_err cf_params_parse(const cf_request *, cf_params **out);
void cf_params_destroy(cf_params *);
cf_err cf_route_match_request(const cf_request *, cf_route_match *out);
void cf_route_match_dispose(cf_route_match *);
cf_err cf_dispatch(cf_ctx *);

/* Called only by request workers; finishes before context is destroyed. */
typedef enum { CF_AUTH_REQUIRED, CF_AUTH_SKIPPED,
               CF_AUTH_REQUIRE_UNAUTHENTICATED } cf_auth_policy;
typedef struct {
    cf_auth_policy authentication;
    bool deny_bots, forgery_protection;
} cf_before;
cf_err cf_before_actions(cf_ctx *, cf_before);
cf_err cf_authenticate(cf_ctx *);
cf_err cf_check_csrf(cf_ctx *, bool bot_exempt);
cf_err cf_authorize_room(cf_db *, int64_t user_id, int64_t room_id);
cf_err cf_finish_cookies(cf_ctx *);

/* Writer call borrows arg until return; never called from loop/writer thread.
 * fn executes inside BEGIN IMMEDIATE. It must not perform external I/O.
 * CF_OK means committed AND required revocations applied. */
typedef cf_err (*cf_write_fn)(cf_tx *, void *arg);
cf_err cf_write(cf_app *, cf_write_fn fn, void *arg);
cf_db *cf_tx_db(cf_tx *); /* borrowed, writer thread only */
uint64_t cf_data_version(const cf_app *);
cf_err cf_read_begin(cf_db *);
cf_err cf_read_end(cf_db *); /* no read transaction may span cf_write */

typedef enum {
    CF_EVENT_DISCONNECT_USER, CF_EVENT_PUSH_MESSAGE,
    CF_EVENT_REMOVE_BANNED_CONTENT, CF_EVENT_DELIVER_WEBHOOK,
    CF_EVENT_PURGE_BLOB
} cf_event_kind;
typedef struct {
    cf_event_kind kind;
    int64_t user_id, room_id, message_id, blob_id;
    bool reconnect;
} cf_event;
cf_err cf_tx_event(cf_tx *, cf_event); /* append; no delivery until commit */

/* Rendering errors preserve out ownership; trusted HTML has its own call. */
cf_err cf_html_text(cf_builder *, cf_span);
cf_err cf_html_attr(cf_builder *, cf_span);
cf_err cf_json_string(cf_builder *, cf_span);
cf_err cf_url_component(cf_builder *, cf_span);
typedef struct { cf_buf *bytes; } cf_safe_html;
cf_err cf_richtext_render(cf_ctx *, cf_span input, cf_safe_html *out);
void cf_safe_html_dispose(cf_safe_html *);
cf_err cf_html_trusted(cf_builder *, const cf_safe_html *);
cf_err cf_gzip(cf_span identity, cf_buf **out);

/* Cache stores only a representation; body ref returned on hit is owned. */
typedef struct {
    cf_buf *body;
    char weak_etag[69]; /* W/" + 64 hex + " + NUL */
    bool gzip;
} cf_cached_body;
cf_err cf_cache_get(cf_cache *, cf_span key, cf_cached_body *out);
cf_err cf_cache_put(cf_cache *, cf_span key, uint64_t expected_version,
                    const cf_cached_body *); /* copies key, retains body on OK */
void cf_cached_body_dispose(cf_cached_body *);

/* Publish copies stream/id metadata and retains payload on successful enqueue. */
cf_err cf_cable_publish(cf_cable *, cf_span stream, cf_buf *payload);
cf_err cf_cable_disconnect_user(cf_cable *, int64_t user_id, bool reconnect);
/* disconnect_user returns only after all loops have applied revocation. */
cf_err cf_job_enqueue(cf_jobs *, cf_event); /* CF_BUSY means not enqueued */

/* Determinism belongs in injected test clocks/RNG, not production shortcuts. */
int64_t cf_now_us(const cf_app *); /* UTC microseconds since epoch */
uint64_t cf_monotonic_ms(void);
cf_err cf_random_bytes(void *out, size_t len);
#endif
