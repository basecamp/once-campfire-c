/* src/views/ctx.c — the render context (crates/views/src/lib.rs ViewContext):
 * the request state a renderer reads, assembled without touching the
 * database.  Presenters load rows (cf_presenter_layout_load) and pass the
 * owned model here; this function only copies spans and reads the flash and
 * the configured base URL.
 */
#include "views.h"

#include "app.h"
#include "config.h"

#include <string.h>

void cf_view_ctx_init(cf_view_ctx *out, const cf_ctx *ctx,
                      const cf_view_layout_model *layout) {
    if (out == NULL) return;
    memset(out, 0, sizeof *out);
    if (layout != NULL) {
        out->current_user = layout->current_user;
        out->account = layout->account;
        out->platform = layout->platform;
        out->has_custom_styles = layout->has_custom_styles;
        out->custom_styles =
            (cf_span){(const unsigned char *)layout->custom_styles.ptr,
                      layout->custom_styles.len};
        out->has_vapid_public_key = layout->has_vapid_public_key;
        out->vapid_public_key =
            (cf_span){(const unsigned char *)layout->vapid_public_key.ptr,
                      layout->vapid_public_key.len};
        out->app_version = (cf_span){
            (const unsigned char *)layout->app_version.ptr,
            layout->app_version.len};
    }
    if (ctx == NULL) return;
    cf_span flash;
    if (cf_ctx_flash_get(ctx, (cf_span){(const unsigned char *)"notice", 6},
                         &flash) == CF_OK) {
        out->has_flash_notice = true;
        out->flash_notice = flash;
    }
    if (cf_ctx_flash_get(ctx, (cf_span){(const unsigned char *)"alert", 5},
                         &flash) == CF_OK) {
        out->has_flash_alert = true;
        out->flash_alert = flash;
    }
    out->importmap_tags = cf_views_assets_importmap_tags();
    out->stylesheet_tags = cf_views_assets_stylesheet_tags();
    out->asset_path = cf_views_assets_default_path;
    out->asset_path_user = NULL;
    const cf_config *config = cf_app_config(ctx->app);
    if (config != NULL && config->public_origin != NULL) {
        out->base_url = (cf_span){(const unsigned char *)config->public_origin,
                                  strlen(config->public_origin)};
    }
}
