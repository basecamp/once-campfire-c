/* src/views/translations.c — TranslationsHelper (helpers/translations.rs and
 * helpers/translations_table.rs).
 *
 * The table is carried per key; only the keys the foundation families render
 * are translated here.  A key that has not landed yet returns CF_NOT_FOUND
 * (the reference panics on an unknown key; a missing family is a caller bug,
 * not a rendered page).
 */
#include "views/internal.h"

#include <string.h>

struct translation {
    const char *language;
    const char *text;
};

struct translation_set {
    const char *key;
    size_t count;
    const struct translation *items;
};

#define KEY(name) static const struct translation name##_items[]
#define SET(name, key) \
    { key, sizeof name##_items / sizeof name##_items[0], name##_items }

KEY(email_address) = {
    {"\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8", "Enter your email address"},
    {"\xF0\x9F\x87\xAA\xF0\x9F\x87\xB8", "Introduce tu correo electrónico"},
    {"\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7", "Entrez votre adresse courriel"},
    {"\xF0\x9F\x87\xAE\xF0\x9F\x87\xB3", "अपना ईमेल पता दर्ज करें"},
    {"\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA", "Geben Sie Ihre E-Mail-Adresse ein"},
    {"\xF0\x9F\x87\xA7\xF0\x9F\x87\xB7", "Insira seu endereço de email"},
    {"\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5", "メールアドレスを入力してください"},
};

KEY(password) = {
    {"\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8", "Enter your password"},
    {"\xF0\x9F\x87\xAA\xF0\x9F\x87\xB8", "Introduce tu contraseña"},
    {"\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7", "Saisissez votre mot de passe"},
    {"\xF0\x9F\x87\xAE\xF0\x9F\x87\xB3", "अपना पासवर्ड दर्ज करें"},
    {"\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA", "Geben Sie Ihr Passwort ein"},
    {"\xF0\x9F\x87\xA7\xF0\x9F\x87\xB7", "Insira sua senha"},
    {"\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5", "パスワードを入力してください"},
};

KEY(user_name) = {
    {"\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8", "Enter your name"},
    {"\xF0\x9F\x87\xAA\xF0\x9F\x87\xB8", "Introduce tu nombre"},
    {"\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7", "Entrez votre nom"},
    {"\xF0\x9F\x87\xAE\xF0\x9F\x87\xB3", "अपना नाम दर्ज करें"},
    {"\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA", "Geben Sie Ihren Namen ein"},
    {"\xF0\x9F\x87\xA7\xF0\x9F\x87\xB7", "Insira seu nome"},
    {"\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5", "お名前を入力してください"},
};

KEY(incompatible_browser) = {
    {"\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8",
     "Upgrade to a supported web browser. Campfire requires a modern web "
     "browser. Please use one of the browsers listed below and make sure "
     "auto-updates are enabled."},
    {"\xF0\x9F\x87\xAA\xF0\x9F\x87\xB8",
     "Actualiza a un navegador web compatible. Campfire requiere un navegador "
     "web moderno. Utiliza uno de los navegadores listados a continuación y "
     "asegúrate de que las actualizaciones automáticas estén habilitadas."},
    {"\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7",
     "Mettez à jour vers un navigateur web pris en charge. Campfire nécessite "
     "un navigateur web moderne. Veuillez utiliser l'un des navigateurs "
     "répertoriés ci-dessous et assurez-vous que les mises à jour automatiques "
     "sont activées."},
    {"\xF0\x9F\x87\xAE\xF0\x9F\x87\xB3",
     "समर्थित वेब ब्राउज़र में अपग्रेड करें। Campfire को एक आधुनिक वेब ब्राउज़र "
     "की आवश्यकता है। कृपया नीचे सूचीबद्ध ब्राउज़रों में से कोई एक का उपयोग "
     "करें और सुनिश्चित करें कि स्वचालित अपडेट्स सक्षम हैं।"},
    {"\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA",
     "Aktualisieren Sie auf einen unterstützten Webbrowser. Campfire erfordert "
     "einen modernen Webbrowser. Verwenden Sie bitte einen der unten "
     "aufgeführten Browser und stellen Sie sicher, dass automatische Updates "
     "aktiviert sind."},
    {"\xF0\x9F\x87\xA7\xF0\x9F\x87\xB7",
     "Atualize para um navegador compatível. O Campfire requer um navegador "
     "moderno. Por favor, use um dos navegadores listados abaixo e "
     "certifique-se de que as atualizações automáticas estão ativadas."},
    {"\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5",
     "サポートされたウェブブラウザーにアップグレードしてください。Campfireは"
     "モダンなウェブブラウザーが必要です。下記のブラウザーのいずれかを使用し、"
     "自動更新が有効になっていることを確認してください。"},
};

KEY(invite_message) = {
    {"\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8",
     "Welcome to Campfire. To invite some people to chat with you, share the "
     "join link below."},
    {"\xF0\x9F\x87\xAA\xF0\x9F\x87\xB8",
     "Bienvenido a Campfire. Para invitar a algunas personas a chatear "
     "contigo, comparte el enlace de unión que se encuentra a continuación."},
    {"\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7",
     "Bienvenue sur Campfire. Pour inviter des personnes à discuter avec "
     "vous, partagez le lien pour rejoindre ci-dessous."},
    {"\xF0\x9F\x87\xAE\xF0\x9F\x87\xB3",
     "Campfire में आपका स्वागत है। अधिक लोगों को चैट के लिए आमंत्रित करने के "
     "लिए, नीचे जुड़ने का लिंक साझा करें।"},
    {"\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA",
     "Willkommen bei Campfire. Um einige Personen zum Chatten einzuladen, "
     "teilen Sie den unten stehenden Beitrittslink."},
    {"\xF0\x9F\x87\xA7\xF0\x9F\x87\xB7",
     "Boas vindas ao Campfire. Para convidar pessoas para conversarem com "
     "você, compartilhe o link de convite abaixo."},
    {"\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5",
     "Campfireへようこそ。他の人をチャットに招待するには、下記の参加リンクを"
     "共有してください。"},
};

static const struct translation_set SETS[] = {
    SET(email_address, "email_address"),
    SET(password, "password"),
    SET(user_name, "user_name"),
    SET(invite_message, "invite_message"),
    SET(incompatible_browser, "incompatible_browser_messsage"),
};

static const struct translation_set *set_for(const char *key) {
    for (size_t i = 0; i < sizeof SETS / sizeof SETS[0]; i++) {
        if (strcmp(SETS[i].key, key) == 0) return &SETS[i];
    }
    return NULL;
}

/* translations_for(key): the <dl class="language-list"> with no separators
 * between dt/dd pairs. */
static cf_err translations_for(const char *key, cf_builder *out) {
    const struct translation_set *set = set_for(key);
    if (set == NULL) return CF_NOT_FOUND;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "language-list"));
    CF_VIEW_TRY(cf_view_open_start(out, "dl", &attrs));
    for (size_t i = 0; i < set->count; i++) {
        cf_view_attrs dt;
        cf_view_attrs_init(&dt);
        CF_VIEW_TRY(cf_view_content_text(
            out, "dt", &dt,
            (cf_span){(const unsigned char *)set->items[i].language,
                      strlen(set->items[i].language)}));
        cf_view_attrs dd;
        cf_view_attrs_init(&dd);
        CF_VIEW_TRY(cf_view_attr_cstr(&dd, "class", "margin-none"));
        CF_VIEW_TRY(cf_view_content_text(
            out, "dd", &dd,
            (cf_span){(const unsigned char *)set->items[i].text,
                      strlen(set->items[i].text)}));
    }
    CF_VIEW_TRY(cf_view_close_tag(out, "dl"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_translation_button(const cf_view_ctx *ctx, const char *key,
                                  cf_builder *out) {
    if (ctx == NULL || key == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    /* All local builders at function scope so the fail path frees them. */
    cf_builder inner = {0}, summary = {0}, menu = {0}, list = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    /* summary */
    CF_VIEW_TRY(cf_view_str(&summary,
                            "<summary class=\"btn\" tabindex=\"-1\">"));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "size", "20"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "class", "color-icon"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("globe.svg"), &img,
                                      &summary));
        cf_view_attrs sr;
        cf_view_attrs_init(&sr);
        CF_VIEW_TRY(cf_view_attr_cstr(&sr, "class", "for-screen-reader"));
        CF_VIEW_TRY(cf_view_content_text(&summary, "span", &sr,
                                         cf_span_of_lit("Translate")));
    }
    CF_VIEW_TRY(cf_view_str(&summary, "</summary>"));
    CF_VIEW_TRY(cf_view_raw(&inner, cf_view_span_of(&summary)));
    cf_builder_dispose(&summary);

    /* menu */
    CF_VIEW_TRY(translations_for(key, &list));
    {
        cf_view_attrs div;
        cf_view_attrs_init(&div);
        CF_VIEW_TRY(cf_view_attr_cstr(&div, "class",
                                      "language-list-menu shadow"));
        CF_VIEW_TRY(cf_view_attr_cstr(&div, "data-popup-target", "menu"));
        CF_VIEW_TRY(cf_view_content(&menu, "div", &div,
                                    cf_view_span_of(&list)));
    }
    CF_VIEW_TRY(cf_view_raw(&inner, cf_view_span_of(&menu)));
    cf_builder_dispose(&menu);
    cf_builder_dispose(&list);

    cf_view_attrs details;
    cf_view_attrs_init(&details);
    CF_VIEW_TRY(cf_view_attr_cstr(&details, "class", "position-relative"));
    CF_VIEW_TRY(cf_view_attr_cstr(&details, "data-controller", "popup"));
    CF_VIEW_TRY(cf_view_attr_cstr(
        &details, "data-action",
        "keydown.esc->popup#close toggle->popup#toggle "
        "click@document->popup#closeOnClickOutside"));
    CF_VIEW_TRY(cf_view_attr_cstr(&details, "data-popup-orientation-top-class",
                                  "popup-orientation-top"));
    CF_VIEW_TRY(cf_view_content(out, "details", &details,
                                cf_view_span_of(&inner)));
    cf_builder_dispose(&inner);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&inner);
    cf_builder_dispose(&summary);
    cf_builder_dispose(&menu);
    cf_builder_dispose(&list);
    return cf_view_fail(&guard, rc);
}
