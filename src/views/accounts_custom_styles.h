#ifndef CF_VIEWS_ACCOUNTS_CUSTOM_STYLES_H
#define CF_VIEWS_ACCOUNTS_CUSTOM_STYLES_H

#include "views.h"

/* accounts::CustomStylesEdit: the current custom CSS, if any. */
typedef struct cf_view_accounts_custom_styles_model {
    bool has_custom_styles;
    cf_str custom_styles; /* owned when has_custom_styles */
} cf_view_accounts_custom_styles_model;

void cf_view_accounts_custom_styles_model_dispose(
    cf_view_accounts_custom_styles_model *model);

/* accounts/custom_styles/edit.html.erb (page and turbo-rails frame). */
cf_err cf_view_accounts_custom_styles_edit(
    const cf_view_ctx *ctx, const cf_view_accounts_custom_styles_model *model,
    cf_builder *out);
cf_err cf_view_accounts_custom_styles_edit_frame(
    const cf_view_ctx *ctx, const cf_view_accounts_custom_styles_model *model,
    cf_builder *out);

#endif
