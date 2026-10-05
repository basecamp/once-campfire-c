/* A02 test support: golden facts (see support/facts.h). */
#include "support/facts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static yyjson_doc *g_facts;

/* Process-lifetime fixtures, released at exit so leak checkers see a clean
 * process (the pointers stay valid for every case). */
static void facts_cleanup(void) {
    yyjson_doc_free(g_facts);
    g_facts = NULL;
}


static const char *golden_dir(void) {
    const char *dir = getenv("CF_GOLDEN_DIR");
    return dir != NULL && dir[0] != '\0'
               ? dir
               : "tests/fixtures/crates/views/tests/golden";
}

int cf_facts_load(void) {
    if (g_facts != NULL) return 1;
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/a/facts.json", golden_dir());
    if (n < 0 || (size_t)n >= sizeof path) return 0;
    g_facts = yyjson_read_file(path, 0, NULL, NULL);
    if (g_facts != NULL) atexit(facts_cleanup);
    if (g_facts == NULL) {
        fprintf(stderr, "facts: cannot parse %s; run the fixture copy\n", path);
        return 0;
    }
    return 1;
}

yyjson_val *cf_facts_case(const char *name) {
    if (!cf_facts_load()) return NULL;
    yyjson_val *root = yyjson_doc_get_root(g_facts);
    yyjson_val *cases = yyjson_obj_get(root, "cases");
    if (cases == NULL) return NULL;
    yyjson_val *value = yyjson_obj_get(cases, name);
    return yyjson_is_obj(value) ? value : NULL;
}

const char *cf_facts_str(yyjson_val *object, const char *key) {
    if (object == NULL) return "";
    yyjson_val *value = yyjson_obj_get(object, key);
    if (value == NULL || !yyjson_is_str(value)) return "";
    return yyjson_get_str(value);
}

int64_t cf_facts_i64(yyjson_val *object, const char *key, int64_t fallback) {
    if (object == NULL) return fallback;
    yyjson_val *value = yyjson_obj_get(object, key);
    if (value == NULL || !yyjson_is_int(value)) return fallback;
    return yyjson_get_sint(value);
}

bool cf_facts_bool(yyjson_val *object, const char *key) {
    if (object == NULL) return false;
    yyjson_val *value = yyjson_obj_get(object, key);
    return yyjson_is_true(value);
}

yyjson_val *cf_facts_user(yyjson_val *case_obj, const char *name) {
    if (case_obj == NULL) return NULL;
    yyjson_val *users = yyjson_obj_get(case_obj, "users");
    if (users == NULL) return NULL;
    yyjson_val *user = yyjson_obj_get(users, name);
    return yyjson_is_obj(user) ? user : NULL;
}

static yyjson_val *user_by_email(yyjson_val *case_obj, const char *email) {
    if (case_obj == NULL || email == NULL || email[0] == '\0') return NULL;
    yyjson_val *users = yyjson_obj_get(case_obj, "users");
    if (users == NULL) return NULL;
    yyjson_val *key, *value;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(users, &iter);
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        value = yyjson_obj_iter_get_val(key);
        if (yyjson_is_obj(value) &&
            strcmp(cf_facts_str(value, "email_address"), email) == 0) {
            return value;
        }
    }
    return NULL;
}

/* The Rust runner's ViewContext asset closure: facts["assets"][logical]. */
static cf_err facts_asset_path(void *user, cf_span logical, cf_builder *out) {
    (void)user;
    if (out == NULL) return CF_INVALID;
    if (logical.len != 0 && logical.ptr == NULL) return CF_INVALID;
    char name[512];
    if (logical.len >= sizeof name) return CF_NOT_FOUND;
    memcpy(name, logical.ptr, logical.len);
    name[logical.len] = '\0';
    yyjson_val *root = yyjson_doc_get_root(g_facts);
    yyjson_val *assets = yyjson_obj_get(root, "assets");
    yyjson_val *url = assets != NULL ? yyjson_obj_get(assets, name) : NULL;
    if (url == NULL || !yyjson_is_str(url)) return CF_NOT_FOUND;
    const char *text = yyjson_get_str(url);
    return cf_builder_append(
        out, (cf_span){(const unsigned char *)text, strlen(text)});
}

static cf_span span_of_str(const char *text) {
    return (cf_span){(const unsigned char *)text, text == NULL ? 0
                                                               : strlen(text)};
}

/* The layout model's text fields are owned cf_str; this test double borrows
 * the parsed document's strings (which live for the process). */
static cf_str str_of_borrowed(const char *text) {
    return (cf_str){(char *)text, text == NULL ? 0 : strlen(text)};
}

int cf_facts_view_ctx(cf_view_ctx *out, const char *case_name,
                      const char *flash_notice, const char *flash_alert) {
    if (out == NULL || !cf_facts_load()) return 0;
    yyjson_val *case_obj = cf_facts_case(case_name);
    if (case_obj == NULL) {
        fprintf(stderr, "facts: no case %s\n", case_name);
        return 0;
    }
    yyjson_val *root = yyjson_doc_get_root(g_facts);
    yyjson_val *account = yyjson_obj_get(case_obj, "account");
    memset(out, 0, sizeof *out);

    /* Current.user, from the case's `as` email (facts.rs user_by_email). */
    const char *as = cf_facts_str(case_obj, "as");
    yyjson_val *me = user_by_email(case_obj, as);
    if (me != NULL) {
        out->current_user.has_user = true;
        out->current_user.id = cf_facts_i64(me, "id", 0);
        out->current_user.name = str_of_borrowed(cf_facts_str(me, "name"));
        out->current_user.administrator =
            strcmp(cf_facts_str(me, "role"), "administrator") == 0;
        out->current_user.bot = strcmp(cf_facts_str(me, "role"), "bot") == 0;
        out->current_user.avatar_url =
            str_of_borrowed(cf_facts_str(me, "avatar_path"));
    }

    /* Current.account: name, fresh_account_logo_path, has_logo. */
    out->account.name =
        str_of_borrowed(cf_facts_str(account, "name"));
    const char *logo = cf_facts_str(account, "logo_path");
    if (logo[0] == '\0') logo = "/account/logo";
    out->account.logo_url = str_of_borrowed(logo);
    out->account.has_logo = cf_facts_bool(account, "has_logo");

    if (flash_notice != NULL) {
        out->has_flash_notice = true;
        out->flash_notice = span_of_str(flash_notice);
    }
    if (flash_alert != NULL) {
        out->has_flash_alert = true;
        out->flash_alert = span_of_str(flash_alert);
    }

    /* Platform facts (facts.rs platform(case["ua"])). */
    yyjson_val *platforms = yyjson_obj_get(root, "platforms");
    yyjson_val *platform =
        platforms != NULL
            ? yyjson_obj_get(platforms, cf_facts_str(case_obj, "ua"))
            : NULL;
    out->platform.apple_messages = cf_facts_bool(platform, "apple_messages");
    out->platform.mobile = cf_facts_bool(platform, "mobile");
    out->platform.desktop = cf_facts_bool(platform, "desktop");

    out->has_vapid_public_key =
        cf_facts_str(root, "vapid_public_key")[0] != '\0';
    out->vapid_public_key = span_of_str(cf_facts_str(root, "vapid_public_key"));
    out->has_custom_styles = cf_facts_str(account, "custom_styles")[0] != '\0';
    out->custom_styles = span_of_str(cf_facts_str(account, "custom_styles"));
    out->app_version = span_of_str(cf_facts_str(root, "app_version"));
    out->base_url = span_of_str(cf_facts_str(root, "base_url"));
    /* The runner's ViewContext: the golden importmap/stylesheet tags and the
     * facts asset map, not the asset build's (which is a different pinned
     * revision; see the evidence). */
    out->importmap_tags = span_of_str(cf_facts_str(root, "importmap_tags"));
    out->stylesheet_tags = span_of_str(cf_facts_str(root, "stylesheet_tags"));
    out->asset_path = facts_asset_path;
    out->asset_path_user = NULL;
    return 1;
}

const char *cf_facts_account_name(const char *case_name) {
    yyjson_val *case_obj = cf_facts_case(case_name);
    return cf_facts_str(yyjson_obj_get(case_obj, "account"), "name");
}

void cf_test_views_assets_ctx(cf_view_ctx *ctx) {
    if (ctx == NULL) return;
    ctx->importmap_tags = cf_views_assets_importmap_tags();
    ctx->stylesheet_tags = cf_views_assets_stylesheet_tags();
    ctx->asset_path = cf_views_assets_default_path;
    ctx->asset_path_user = NULL;
}

int cf_test_views_setup(void) {
    static int ready;
    if (ready) return 1;
    if (!cf_facts_load()) return 0;
    atexit(cf_views_assets_reset);
    if (cf_views_assets_configure(NULL) != CF_OK) {
        fprintf(stderr, "views test: cannot load the pinned asset manifest "
                        "(tests/fixtures/assets)\n");
        return 0;
    }
    ready = 1;
    return 1;
}
