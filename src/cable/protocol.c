/* src/cable/protocol.c — task C01: the Action Cable wire format
 * (tmp/rust-ref/crates/cable/src/protocol.rs; ActionCable::INTERNAL).
 *
 * Frames are Ruby hashes run through ActiveSupport::JSON.encode, so key
 * order is the hash-literal order in Connection::Base and Channel::Base.
 * Identifiers are encoded with cf_json_string (Active Support JSON string
 * escaping: <, > and & become <, >, &). */
#include "cable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Move a built buffer into an owned NUL-terminated cf_str. */
static cf_err protocol_finish(cf_builder *builder, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    *out = (cf_str){0};
    if (builder->len > SIZE_MAX - 1) return CF_LIMIT;
    size_t len = builder->len;
    char *text = malloc(len + 1);
    if (text == NULL) return CF_NOMEM;
    if (len != 0) memcpy(text, builder->ptr, len);
    text[len] = '\0';
    cf_builder_dispose(builder);
    *out = (cf_str){text, len};
    return CF_OK;
}

static cf_err protocol_append_literal(cf_builder *builder, const char *text) {
    return cf_builder_append(builder,
                             (cf_span){(const unsigned char *)text,
                                       strlen(text)});
}

const char *cf_cable_reason_name(cf_cable_disconnect_reason reason) {
    switch (reason) {
    case CF_CABLE_REASON_UNAUTHORIZED:
        return "unauthorized";
    case CF_CABLE_REASON_INVALID_REQUEST:
        return "invalid_request";
    case CF_CABLE_REASON_SERVER_RESTART:
        return "server_restart";
    case CF_CABLE_REASON_REMOTE:
        return "remote";
    case CF_CABLE_REASON_NONE:
    default:
        return "null";
    }
}

cf_err cf_cable_welcome(cf_str *out) {
    if (out == NULL) return CF_INVALID;
    *out = (cf_str){0};
    cf_builder b = {0};
    cf_err rc = protocol_append_literal(&b, "{\"type\":\"welcome\"}");
    if (rc != CF_OK) {
        cf_builder_dispose(&b);
        return rc;
    }
    return protocol_finish(&b, out);
}

cf_err cf_cable_ping(int64_t unix_seconds, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    *out = (cf_str){0};
    cf_builder b = {0};
    char number[32];
    int n = snprintf(number, sizeof number, "%lld", (long long)unix_seconds);
    if (n < 0 || (size_t)n >= sizeof number) {
        cf_builder_dispose(&b);
        return CF_INTERNAL;
    }
    cf_err rc = protocol_append_literal(&b, "{\"type\":\"ping\",\"message\":");
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &b, (cf_span){(const unsigned char *)number, (size_t)n});
    }
    if (rc == CF_OK) rc = protocol_append_literal(&b, "}");
    if (rc != CF_OK) {
        cf_builder_dispose(&b);
        return rc;
    }
    return protocol_finish(&b, out);
}

cf_err cf_cable_disconnect_frame(cf_cable_disconnect_reason reason,
                                 cf_span reconnect_json, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    *out = (cf_str){0};
    cf_builder b = {0};
    cf_err rc = protocol_append_literal(&b, "{\"type\":\"disconnect\",\"reason\":");
    if (rc == CF_OK) {
        if (reason == CF_CABLE_REASON_NONE) {
            rc = protocol_append_literal(&b, "null");
        } else {
            rc = protocol_append_literal(&b, "\"");
            if (rc == CF_OK) {
                rc = protocol_append_literal(&b, cf_cable_reason_name(reason));
            }
            if (rc == CF_OK) rc = protocol_append_literal(&b, "\"");
        }
    }
    if (rc == CF_OK) {
        rc = protocol_append_literal(&b, ",\"reconnect\":");
    }
    if (rc == CF_OK) {
        if (reconnect_json.len != 0) {
            rc = cf_builder_append(&b, reconnect_json);
        } else {
            rc = protocol_append_literal(&b, "true");
        }
    }
    if (rc == CF_OK) rc = protocol_append_literal(&b, "}");
    if (rc != CF_OK) {
        cf_builder_dispose(&b);
        return rc;
    }
    return protocol_finish(&b, out);
}

cf_err cf_cable_confirm_subscription(cf_span identifier, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    *out = (cf_str){0};
    cf_builder b = {0};
    cf_err rc = protocol_append_literal(&b, "{\"identifier\":");
    if (rc == CF_OK) rc = cf_json_string(&b, identifier);
    if (rc == CF_OK) {
        rc = protocol_append_literal(&b, ",\"type\":\"confirm_subscription\"}");
    }
    if (rc != CF_OK) {
        cf_builder_dispose(&b);
        return rc;
    }
    return protocol_finish(&b, out);
}

cf_err cf_cable_reject_subscription(cf_span identifier, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    *out = (cf_str){0};
    cf_builder b = {0};
    cf_err rc = protocol_append_literal(&b, "{\"identifier\":");
    if (rc == CF_OK) rc = cf_json_string(&b, identifier);
    if (rc == CF_OK) {
        rc = protocol_append_literal(&b, ",\"type\":\"reject_subscription\"}");
    }
    if (rc != CF_OK) {
        cf_builder_dispose(&b);
        return rc;
    }
    return protocol_finish(&b, out);
}

cf_err cf_cable_message_frame(cf_span encoded_identifier,
                              cf_span encoded_message, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    *out = (cf_str){0};
    cf_builder b = {0};
    cf_err rc = protocol_append_literal(&b, "{\"identifier\":");
    if (rc == CF_OK) rc = cf_builder_append(&b, encoded_identifier);
    if (rc == CF_OK) rc = protocol_append_literal(&b, ",\"message\":");
    if (rc == CF_OK) rc = cf_builder_append(&b, encoded_message);
    if (rc == CF_OK) rc = protocol_append_literal(&b, "}");
    if (rc != CF_OK) {
        cf_builder_dispose(&b);
        return rc;
    }
    return protocol_finish(&b, out);
}
