/* src/richtext.h — R02 rich text: module surface and the D02/R02 boundary.
 *
 * Reference: tmp/rust-ref/crates/richtext/src (content, dom, sanitizer,
 * filters, autolink, plain_text, attachables, uri) and the app-side wiring in
 * crates/campfire/src/{rich_text.rs,controllers/presenters/rich_text.rs}.
 *
 * The pipeline is one immutable, stateless, process-wide instance
 * (`cf_tx_rich_text` returns it; the transaction is only the D02 seam and is
 * not consulted). It carries the borrowed SECRET_KEY_BASE used to verify
 * attachment SGIDs; cf_richtext_configure sets it once at startup from the
 * loaded config, mirroring cf_writer_start(app, config). Until it is
 * configured, signed attachments resolve as unverified only (the reference's
 * expired-SGID fallback still finds a User by GID; mentions require a
 * verified SGID).
 *
 * The three cf_message_* boundary functions keep the exact signatures
 * message.c declares locally until the integrator includes this header:
 *
 *   cf_richtext_to_plain_text(cf_db *, const cf_richtext *, cf_span, cf_str *)
 *   cf_richtext_mentioned_user_ids(cf_db *, const cf_richtext *, cf_span,
 *                                  cf_int64_vector *)
 *   cf_tx_rich_text(cf_tx *)
 */
#ifndef CF_RICHTEXT_H
#define CF_RICHTEXT_H

#include "cf.h"
#include "models/types.h"

/* Concrete definition of the D01 opaque pointer type. */
struct cf_richtext {
    cf_span secret_key_base; /* borrowed from the loaded config; never logged */
};

/* Set the singleton's borrowed key material. Call once at startup after
 * cf_config_load and before serving; a second call replaces the span. Passing
 * an empty span leaves signed attachments unverified (tests, and production
 * before wiring). */
void cf_richtext_configure(cf_span secret_key_base);

/* Rust Tx::rich_text: the immutable pipeline the transaction carries. It is
 * the process singleton; `tx` is accepted and ignored (NULL is fine). */
const cf_richtext *cf_tx_rich_text(cf_tx *tx);

/* The sanitized presentation of a message body (`message_presentation` with
 * its rescue, i.e. MessagesHelper#message_presentation):
 *   CF_OK and out->bytes != NULL: the rendered HTML;
 *   CF_OK and out->bytes == NULL: the rescue-to-empty branch (Html(""));
 *   CF_INVALID: the reference's unrenderable-message path
 *               (messages/_unrenderable.html.erb).
 * Resource failures return CF_NOMEM/CF_INTERNAL. `ctx` supplies the reader
 * used for record lookups and the request Host the opengraph checks use. */
cf_err cf_richtext_render(cf_ctx *ctx, cf_span input, cf_safe_html *out);

/* `Presenter::body_html`: the HTML body bots and webhooks get
 * (ActionText::Content#to_s with the lexxy-content layout, no message filters
 * and no auto_link). Errors render as an empty body, like the reference's
 * unwrap_or_default. */
cf_err cf_richtext_body_html(cf_ctx *ctx, cf_span input, cf_safe_html *out);

/* `editable_body` as the editor's `value`: found=false for a blank body,
 * found=true with *out set otherwise. CF_INVALID is the reference's raise
 * (an attachable without attachable_content_type, or a missing asset). */
cf_err cf_richtext_editable(cf_ctx *ctx, cf_span input, bool *found, cf_str *out);

/* Rust RichText::to_plain_text (AppRichText): raises are swallowed to an
 * empty string, exactly as the production wiring logs and continues. */
cf_err cf_richtext_to_plain_text(cf_db *db, const cf_richtext *rich_text,
                                 cf_span input, cf_str *out);

/* Rust RichText::mentioned_user_ids: attached users with a verified SGID,
 * document order, unique. Raises are swallowed to an empty list. */
cf_err cf_richtext_mentioned_user_ids(cf_db *db, const cf_richtext *rich_text,
                                      cf_span input, cf_int64_vector *out);

/* `Webhook#without_recipient_mentions`: the plain body with the bot's own
 * "@Name" removed and Unicode whitespace trimmed. */
cf_err cf_richtext_without_recipient_mentions(cf_span plain_text,
                                              cf_span recipient_name, cf_str *out);

#endif /* CF_RICHTEXT_H */
