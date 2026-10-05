/* H03: the ordered 177-row route table, the finite matcher and the built-in
 * handlers (01-foundation-http.md "H03: exact route table and assets";
 * contracts/routes.json; contracts/route-recognition.json).
 *
 * Ported reference behavior:
 *  - tmp/rust-ref/crates/campfire/src/controllers.rs
 *      normalize_path, compile, path_params, recognize, action_not_found,
 *      missing_controller, health::show, turbo_native::*, mailbox::*
 *  - tmp/rust-ref/crates/kit/src/exceptions.rs (PublicExceptions rendering)
 *  - tmp/rust-ref/crates/kit/src/error.rs (status mapping)
 *  - tmp/rust-ref/crates/kit/src/format.rs (format negotiation, via A00)
 *
 * The table is generated from the artifact by
 * tests/routes/tools/gen_routes.py; rows are in artifact order and a first
 * match wins. The matcher covers exactly the finite grammar the artifact
 * uses: literals, `:name`, `*name` and the optional `(.:format)` group.
 * Percent-decoding happens after recognition; a path parameter that is not
 * valid UTF-8 fails the whole match with CF_INVALID (A00 turns that into
 * 400). HEAD matches GET. Missing verbs/routes return CF_NOT_FOUND.
 */
#include "routes.h"

#include "auth.h" /* cf_check_csrf/cf_auth_halted (conductor POST rows) */
#include "context.h"
#include "http/params.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Development-only binding for rows whose packet has not landed. */
#define CF_ROUTE_DEV_501 cf_action_not_landed_501

/* ---- BEGIN GENERATED ROUTE TABLE (tests/routes/tools/gen_routes.py) ---- */
/*
 * Generated from docs/devel/implementation/contracts/routes.json
 * (reference_commit 64f86353021145b63849fb1cd93adeb08f3b8dbb) by tests/routes/tools/gen_routes.py.
 * 177 rows, artifact order; do not hand-edit rows. Rebinding a row to
 * its packet's action is route registration and belongs to the integrator.
 */
static const cf_route_default cf_defaults_r57[] = { {"user_id", "me"} };
static const cf_route_default cf_defaults_r58[] = { {"user_id", "me"} };
static const cf_route_default cf_defaults_r59[] = { {"user_id", "me"} };
static const cf_route_default cf_defaults_r60[] = { {"user_id", "me"} };
static const cf_route_default cf_defaults_r61[] = { {"user_id", "me"} };
static const cf_route_default cf_defaults_r62[] = { {"user_id", "me"} };
static const cf_route_default cf_defaults_r63[] = { {"user_id", "me"} };
static const cf_route_default cf_defaults_r64[] = { {"user_id", "me"} };
static const cf_route_default cf_defaults_r65[] = { {"user_id", "me"} };
static const cf_route_default cf_defaults_r66[] = { {"user_id", "me"} };
static const cf_route_default cf_defaults_r67[] = { {"user_id", "me"} };
static const cf_route_default cf_defaults_r68[] = { {"user_id", "me"} };
static const cf_route_default cf_defaults_r69[] = { {"user_id", "me"} };
static const cf_route_default cf_defaults_r70[] = { {"user_id", "me"} };
static const cf_route_default cf_defaults_r71[] = { {"user_id", "me"} };
static const cf_route_default cf_defaults_r72[] = { {"user_id", "me"} };
static const cf_route_default cf_defaults_r73[] = { {"user_id", "me"} };
static const cf_route_default cf_defaults_r84[] = { {"format", "json"} };
static const cf_route_default cf_defaults_r85[] = { {"format", "json"} };
static const cf_route_default cf_defaults_r86[] = { {"format", "json"} };
static const cf_route_default cf_defaults_r87[] = { {"format", "json"} };
static const cf_route_default cf_defaults_r88[] = { {"format", "json"} };
static const cf_route_default cf_defaults_r89[] = { {"format", "json"} };
static const cf_route_default cf_defaults_r90[] = { {"format", "json"} };

static const cf_route cf_route_table[177] = {
    {1, CF_GET, "/", "welcome#show", "cf_action_welcome_show", "A-welcome", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {2, CF_GET, "/first_run/new(.:format)", "first_runs#new", "cf_action_first_runs_new", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {3, CF_GET, "/first_run/edit(.:format)", "first_runs#edit", "cf_action_first_runs_edit", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {4, CF_GET, "/first_run(.:format)", "first_runs#show", "cf_action_first_runs_show", "A-first_runs", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {5, CF_PATCH, "/first_run(.:format)", "first_runs#update", "cf_action_first_runs_update", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {6, CF_PUT, "/first_run(.:format)", "first_runs#update", "cf_action_first_runs_update", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {7, CF_DELETE, "/first_run(.:format)", "first_runs#destroy", "cf_action_first_runs_destroy", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {8, CF_POST, "/first_run(.:format)", "first_runs#create", "cf_action_first_runs_create", "A-first_runs", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {9, CF_GET, "/session/transfers/:id(.:format)", "sessions/transfers#show", "cf_action_sessions_transfers_show", "A-sessions-transfers", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {10, CF_PATCH, "/session/transfers/:id(.:format)", "sessions/transfers#update", "cf_action_sessions_transfers_update", "A-sessions-transfers", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {11, CF_PUT, "/session/transfers/:id(.:format)", "sessions/transfers#update", "cf_action_sessions_transfers_update", "A-sessions-transfers", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {12, CF_GET, "/session/new(.:format)", "sessions#new", "cf_action_sessions_new", "A-sessions", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {13, CF_GET, "/session/edit(.:format)", "sessions#edit", "cf_action_sessions_edit", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {14, CF_GET, "/session(.:format)", "sessions#show", "cf_action_sessions_show", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {15, CF_PATCH, "/session(.:format)", "sessions#update", "cf_action_sessions_update", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {16, CF_PUT, "/session(.:format)", "sessions#update", "cf_action_sessions_update", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {17, CF_DELETE, "/session(.:format)", "sessions#destroy", "cf_action_sessions_destroy", "A-sessions", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {18, CF_POST, "/session(.:format)", "sessions#create", "cf_action_sessions_create", "A-sessions", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {19, CF_GET, "/account/users(.:format)", "accounts/users#index", "cf_action_accounts_users_index", "A-accounts-users", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {20, CF_POST, "/account/users(.:format)", "accounts/users#create", "cf_action_accounts_users_create", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {21, CF_GET, "/account/users/new(.:format)", "accounts/users#new", "cf_action_accounts_users_new", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {22, CF_GET, "/account/users/:id/edit(.:format)", "accounts/users#edit", "cf_action_accounts_users_edit", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {23, CF_GET, "/account/users/:id(.:format)", "accounts/users#show", "cf_action_accounts_users_show", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {24, CF_PATCH, "/account/users/:id(.:format)", "accounts/users#update", "cf_action_accounts_users_update", "A-accounts-users", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {25, CF_PUT, "/account/users/:id(.:format)", "accounts/users#update", "cf_action_accounts_users_update", "A-accounts-users", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {26, CF_DELETE, "/account/users/:id(.:format)", "accounts/users#destroy", "cf_action_accounts_users_destroy", "A-accounts-users", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {27, CF_PATCH, "/account/bots/:bot_id/key(.:format)", "accounts/bots/keys#update", "cf_action_accounts_bots_keys_update", "A-accounts-bots-keys", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {28, CF_PUT, "/account/bots/:bot_id/key(.:format)", "accounts/bots/keys#update", "cf_action_accounts_bots_keys_update", "A-accounts-bots-keys", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {29, CF_GET, "/account/bots(.:format)", "accounts/bots#index", "cf_action_accounts_bots_index", "A-accounts-bots", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {30, CF_POST, "/account/bots(.:format)", "accounts/bots#create", "cf_action_accounts_bots_create", "A-accounts-bots", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {31, CF_GET, "/account/bots/new(.:format)", "accounts/bots#new", "cf_action_accounts_bots_new", "A-accounts-bots", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {32, CF_GET, "/account/bots/:id/edit(.:format)", "accounts/bots#edit", "cf_action_accounts_bots_edit", "A-accounts-bots", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {33, CF_GET, "/account/bots/:id(.:format)", "accounts/bots#show", "cf_action_accounts_bots_show", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {34, CF_PATCH, "/account/bots/:id(.:format)", "accounts/bots#update", "cf_action_accounts_bots_update", "A-accounts-bots", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {35, CF_PUT, "/account/bots/:id(.:format)", "accounts/bots#update", "cf_action_accounts_bots_update", "A-accounts-bots", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {36, CF_DELETE, "/account/bots/:id(.:format)", "accounts/bots#destroy", "cf_action_accounts_bots_destroy", "A-accounts-bots", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {37, CF_POST, "/account/join_code(.:format)", "accounts/join_codes#create", "cf_action_accounts_join_codes_create", "A-accounts-join_codes", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {38, CF_GET, "/account/logo(.:format)", "accounts/logos#show", "cf_action_accounts_logos_show", "A-accounts-logos", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {39, CF_DELETE, "/account/logo(.:format)", "accounts/logos#destroy", "cf_action_accounts_logos_destroy", "A-accounts-logos", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {40, CF_GET, "/account/custom_styles/edit(.:format)", "accounts/custom_styles#edit", "cf_action_accounts_custom_styles_edit", "A-accounts-custom_styles", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {41, CF_PATCH, "/account/custom_styles(.:format)", "accounts/custom_styles#update", "cf_action_accounts_custom_styles_update", "A-accounts-custom_styles", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {42, CF_PUT, "/account/custom_styles(.:format)", "accounts/custom_styles#update", "cf_action_accounts_custom_styles_update", "A-accounts-custom_styles", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {43, CF_GET, "/account/new(.:format)", "accounts#new", "cf_action_accounts_new", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {44, CF_GET, "/account/edit(.:format)", "accounts#edit", "cf_action_accounts_edit", "A-accounts", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {45, CF_GET, "/account(.:format)", "accounts#show", "cf_action_accounts_show", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {46, CF_PATCH, "/account(.:format)", "accounts#update", "cf_action_accounts_update", "A-accounts", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {47, CF_PUT, "/account(.:format)", "accounts#update", "cf_action_accounts_update", "A-accounts", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {48, CF_DELETE, "/account(.:format)", "accounts#destroy", "cf_action_accounts_destroy", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {49, CF_POST, "/account(.:format)", "accounts#create", "cf_action_accounts_create", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {50, CF_GET, "/join/:join_code(.:format)", "users#new", "cf_action_users_new", "A-users", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {51, CF_POST, "/join/:join_code(.:format)", "users#create", "cf_action_users_create", "A-users", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {52, CF_GET, "/qr_code/:id(.:format)", "qr_code#show", "cf_action_qr_code_show", "A-qr_code", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {53, CF_GET, "/users/:user_id/avatar(.:format)", "users/avatars#show", "cf_action_users_avatars_show", "A-users-avatars", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {54, CF_DELETE, "/users/:user_id/avatar(.:format)", "users/avatars#destroy", "cf_action_users_avatars_destroy", "A-users-avatars", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {55, CF_DELETE, "/users/:user_id/ban(.:format)", "users/bans#destroy", "cf_action_users_bans_destroy", "A-users-bans", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {56, CF_POST, "/users/:user_id/ban(.:format)", "users/bans#create", "cf_action_users_bans_create", "A-users-bans", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {57, CF_GET, "/users/:user_id/sidebar(.:format)", "users/sidebars#show", "cf_action_users_sidebars_show", "A-users-sidebars", CF_ROUTE_IMPLEMENT,
     cf_defaults_r57, 1, CF_ROUTE_DEV_501},
    {58, CF_GET, "/users/:user_id/profile/new(.:format)", "users/profiles#new", "cf_action_users_profiles_new", "H03", CF_ROUTE_REFERENCE_ERROR,
     cf_defaults_r58, 1, cf_action_reference_action_not_found},
    {59, CF_GET, "/users/:user_id/profile/edit(.:format)", "users/profiles#edit", "cf_action_users_profiles_edit", "H03", CF_ROUTE_REFERENCE_ERROR,
     cf_defaults_r59, 1, cf_action_reference_action_not_found},
    {60, CF_GET, "/users/:user_id/profile(.:format)", "users/profiles#show", "cf_action_users_profiles_show", "A-users-profiles", CF_ROUTE_IMPLEMENT,
     cf_defaults_r60, 1, CF_ROUTE_DEV_501},
    {61, CF_PATCH, "/users/:user_id/profile(.:format)", "users/profiles#update", "cf_action_users_profiles_update", "A-users-profiles", CF_ROUTE_IMPLEMENT,
     cf_defaults_r61, 1, CF_ROUTE_DEV_501},
    {62, CF_PUT, "/users/:user_id/profile(.:format)", "users/profiles#update", "cf_action_users_profiles_update", "A-users-profiles", CF_ROUTE_IMPLEMENT,
     cf_defaults_r62, 1, CF_ROUTE_DEV_501},
    {63, CF_DELETE, "/users/:user_id/profile(.:format)", "users/profiles#destroy", "cf_action_users_profiles_destroy", "H03", CF_ROUTE_REFERENCE_ERROR,
     cf_defaults_r63, 1, cf_action_reference_action_not_found},
    {64, CF_POST, "/users/:user_id/profile(.:format)", "users/profiles#create", "cf_action_users_profiles_create", "H03", CF_ROUTE_REFERENCE_ERROR,
     cf_defaults_r64, 1, cf_action_reference_action_not_found},
    {65, CF_POST, "/users/:user_id/push_subscriptions/:push_subscription_id/test_notifications(.:format)", "users/push_subscriptions/test_notifications#create", "cf_action_users_push_subscriptions_test_notifications_create", "A-users-push_subscriptions-test_notifications", CF_ROUTE_IMPLEMENT,
     cf_defaults_r65, 1, CF_ROUTE_DEV_501},
    {66, CF_GET, "/users/:user_id/push_subscriptions(.:format)", "users/push_subscriptions#index", "cf_action_users_push_subscriptions_index", "A-users-push_subscriptions", CF_ROUTE_IMPLEMENT,
     cf_defaults_r66, 1, CF_ROUTE_DEV_501},
    {67, CF_POST, "/users/:user_id/push_subscriptions(.:format)", "users/push_subscriptions#create", "cf_action_users_push_subscriptions_create", "A-users-push_subscriptions", CF_ROUTE_IMPLEMENT,
     cf_defaults_r67, 1, CF_ROUTE_DEV_501},
    {68, CF_GET, "/users/:user_id/push_subscriptions/new(.:format)", "users/push_subscriptions#new", "cf_action_users_push_subscriptions_new", "H03", CF_ROUTE_REFERENCE_ERROR,
     cf_defaults_r68, 1, cf_action_reference_action_not_found},
    {69, CF_GET, "/users/:user_id/push_subscriptions/:id/edit(.:format)", "users/push_subscriptions#edit", "cf_action_users_push_subscriptions_edit", "H03", CF_ROUTE_REFERENCE_ERROR,
     cf_defaults_r69, 1, cf_action_reference_action_not_found},
    {70, CF_GET, "/users/:user_id/push_subscriptions/:id(.:format)", "users/push_subscriptions#show", "cf_action_users_push_subscriptions_show", "H03", CF_ROUTE_REFERENCE_ERROR,
     cf_defaults_r70, 1, cf_action_reference_action_not_found},
    {71, CF_PATCH, "/users/:user_id/push_subscriptions/:id(.:format)", "users/push_subscriptions#update", "cf_action_users_push_subscriptions_update", "H03", CF_ROUTE_REFERENCE_ERROR,
     cf_defaults_r71, 1, cf_action_reference_action_not_found},
    {72, CF_PUT, "/users/:user_id/push_subscriptions/:id(.:format)", "users/push_subscriptions#update", "cf_action_users_push_subscriptions_update", "H03", CF_ROUTE_REFERENCE_ERROR,
     cf_defaults_r72, 1, cf_action_reference_action_not_found},
    {73, CF_DELETE, "/users/:user_id/push_subscriptions/:id(.:format)", "users/push_subscriptions#destroy", "cf_action_users_push_subscriptions_destroy", "A-users-push_subscriptions", CF_ROUTE_IMPLEMENT,
     cf_defaults_r73, 1, CF_ROUTE_DEV_501},
    {74, CF_GET, "/users/:id(.:format)", "users#show", "cf_action_users_show", "A-users", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {75, CF_GET, "/autocompletable/users(.:format)", "autocompletable/users#index", "cf_action_autocompletable_users_index", "A-autocompletable-users", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {76, CF_GET, "/rooms/:room_id/messages(.:format)", "messages#index", "cf_action_messages_index", "A-messages", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {77, CF_POST, "/rooms/:room_id/messages(.:format)", "messages#create", "cf_action_messages_create", "A-messages", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {78, CF_GET, "/rooms/:room_id/messages/new(.:format)", "messages#new", "cf_action_messages_new", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {79, CF_GET, "/rooms/:room_id/messages/:id/edit(.:format)", "messages#edit", "cf_action_messages_edit", "A-messages", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {80, CF_GET, "/rooms/:room_id/messages/:id(.:format)", "messages#show", "cf_action_messages_show", "A-messages", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {81, CF_PATCH, "/rooms/:room_id/messages/:id(.:format)", "messages#update", "cf_action_messages_update", "A-messages", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {82, CF_PUT, "/rooms/:room_id/messages/:id(.:format)", "messages#update", "cf_action_messages_update", "A-messages", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {83, CF_DELETE, "/rooms/:room_id/messages/:id(.:format)", "messages#destroy", "cf_action_messages_destroy", "A-messages", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {84, CF_POST, "/rooms/:room_id/:bot_key/messages/:message_id/boosts(.:format)", "messages/boosts/by_bots#create", "cf_action_messages_boosts_by_bots_create", "A-messages-boosts-by_bots", CF_ROUTE_IMPLEMENT,
     cf_defaults_r84, 1, CF_ROUTE_DEV_501},
    {85, CF_DELETE, "/rooms/:room_id/:bot_key/messages/:message_id/boosts/:id(.:format)", "messages/boosts/by_bots#destroy", "cf_action_messages_boosts_by_bots_destroy", "A-messages-boosts-by_bots", CF_ROUTE_IMPLEMENT,
     cf_defaults_r85, 1, CF_ROUTE_DEV_501},
    {86, CF_GET, "/rooms/:room_id/:bot_key/messages(.:format)", "messages/by_bots#index", "cf_action_messages_by_bots_index", "A-messages-by_bots", CF_ROUTE_IMPLEMENT,
     cf_defaults_r86, 1, CF_ROUTE_DEV_501},
    {87, CF_POST, "/rooms/:room_id/:bot_key/messages(.:format)", "messages/by_bots#create", "cf_action_messages_by_bots_create", "A-messages-by_bots", CF_ROUTE_IMPLEMENT,
     cf_defaults_r87, 1, CF_ROUTE_DEV_501},
    {88, CF_PATCH, "/rooms/:room_id/:bot_key/messages/:id(.:format)", "messages/by_bots#update", "cf_action_messages_by_bots_update", "A-messages-by_bots", CF_ROUTE_IMPLEMENT,
     cf_defaults_r88, 1, CF_ROUTE_DEV_501},
    {89, CF_PUT, "/rooms/:room_id/:bot_key/messages/:id(.:format)", "messages/by_bots#update", "cf_action_messages_by_bots_update", "A-messages-by_bots", CF_ROUTE_IMPLEMENT,
     cf_defaults_r89, 1, CF_ROUTE_DEV_501},
    {90, CF_DELETE, "/rooms/:room_id/:bot_key/messages/:id(.:format)", "messages/by_bots#destroy", "cf_action_messages_by_bots_destroy", "A-messages-by_bots", CF_ROUTE_IMPLEMENT,
     cf_defaults_r90, 1, CF_ROUTE_DEV_501},
    {91, CF_GET, "/rooms/:room_id/refresh(.:format)", "rooms/refreshes#show", "cf_action_rooms_refreshes_show", "A-rooms-refreshes", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {92, CF_GET, "/rooms/:room_id/settings(.:format)", "rooms/settings#show", "cf_action_rooms_settings_show", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_missing_controller},
    {93, CF_GET, "/rooms/:room_id/involvement(.:format)", "rooms/involvements#show", "cf_action_rooms_involvements_show", "A-rooms-involvements", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {94, CF_PATCH, "/rooms/:room_id/involvement(.:format)", "rooms/involvements#update", "cf_action_rooms_involvements_update", "A-rooms-involvements", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {95, CF_PUT, "/rooms/:room_id/involvement(.:format)", "rooms/involvements#update", "cf_action_rooms_involvements_update", "A-rooms-involvements", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {96, CF_GET, "/rooms/:room_id/@:message_id(.:format)", "rooms#show", "cf_action_rooms_show", "A-rooms", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {97, CF_GET, "/rooms(.:format)", "rooms#index", "cf_action_rooms_index", "A-rooms", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {98, CF_POST, "/rooms(.:format)", "rooms#create", "cf_action_rooms_create", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {99, CF_GET, "/rooms/new(.:format)", "rooms#new", "cf_action_rooms_new", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {100, CF_GET, "/rooms/:id/edit(.:format)", "rooms#edit", "cf_action_rooms_edit", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {101, CF_GET, "/rooms/:id(.:format)", "rooms#show", "cf_action_rooms_show", "A-rooms", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {102, CF_PATCH, "/rooms/:id(.:format)", "rooms#update", "cf_action_rooms_update", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {103, CF_PUT, "/rooms/:id(.:format)", "rooms#update", "cf_action_rooms_update", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {104, CF_DELETE, "/rooms/:id(.:format)", "rooms#destroy", "cf_action_rooms_destroy", "A-rooms", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {105, CF_GET, "/rooms/opens(.:format)", "rooms/opens#index", "cf_action_rooms_opens_index", "A-rooms-opens", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {106, CF_POST, "/rooms/opens(.:format)", "rooms/opens#create", "cf_action_rooms_opens_create", "A-rooms-opens", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {107, CF_GET, "/rooms/opens/new(.:format)", "rooms/opens#new", "cf_action_rooms_opens_new", "A-rooms-opens", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {108, CF_GET, "/rooms/opens/:id/edit(.:format)", "rooms/opens#edit", "cf_action_rooms_opens_edit", "A-rooms-opens", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {109, CF_GET, "/rooms/opens/:id(.:format)", "rooms/opens#show", "cf_action_rooms_opens_show", "A-rooms-opens", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {110, CF_PATCH, "/rooms/opens/:id(.:format)", "rooms/opens#update", "cf_action_rooms_opens_update", "A-rooms-opens", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {111, CF_PUT, "/rooms/opens/:id(.:format)", "rooms/opens#update", "cf_action_rooms_opens_update", "A-rooms-opens", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {112, CF_DELETE, "/rooms/opens/:id(.:format)", "rooms/opens#destroy", "cf_action_rooms_opens_destroy", "A-rooms-opens", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {113, CF_GET, "/rooms/closeds(.:format)", "rooms/closeds#index", "cf_action_rooms_closeds_index", "A-rooms-closeds", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {114, CF_POST, "/rooms/closeds(.:format)", "rooms/closeds#create", "cf_action_rooms_closeds_create", "A-rooms-closeds", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {115, CF_GET, "/rooms/closeds/new(.:format)", "rooms/closeds#new", "cf_action_rooms_closeds_new", "A-rooms-closeds", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {116, CF_GET, "/rooms/closeds/:id/edit(.:format)", "rooms/closeds#edit", "cf_action_rooms_closeds_edit", "A-rooms-closeds", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {117, CF_GET, "/rooms/closeds/:id(.:format)", "rooms/closeds#show", "cf_action_rooms_closeds_show", "A-rooms-closeds", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {118, CF_PATCH, "/rooms/closeds/:id(.:format)", "rooms/closeds#update", "cf_action_rooms_closeds_update", "A-rooms-closeds", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {119, CF_PUT, "/rooms/closeds/:id(.:format)", "rooms/closeds#update", "cf_action_rooms_closeds_update", "A-rooms-closeds", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {120, CF_DELETE, "/rooms/closeds/:id(.:format)", "rooms/closeds#destroy", "cf_action_rooms_closeds_destroy", "A-rooms-closeds", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {121, CF_GET, "/rooms/directs(.:format)", "rooms/directs#index", "cf_action_rooms_directs_index", "A-rooms-directs", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {122, CF_POST, "/rooms/directs(.:format)", "rooms/directs#create", "cf_action_rooms_directs_create", "A-rooms-directs", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {123, CF_GET, "/rooms/directs/new(.:format)", "rooms/directs#new", "cf_action_rooms_directs_new", "A-rooms-directs", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {124, CF_GET, "/rooms/directs/:id/edit(.:format)", "rooms/directs#edit", "cf_action_rooms_directs_edit", "A-rooms-directs", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {125, CF_GET, "/rooms/directs/:id(.:format)", "rooms/directs#show", "cf_action_rooms_directs_show", "A-rooms-directs", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {126, CF_PATCH, "/rooms/directs/:id(.:format)", "rooms/directs#update", "cf_action_rooms_directs_update", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {127, CF_PUT, "/rooms/directs/:id(.:format)", "rooms/directs#update", "cf_action_rooms_directs_update", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {128, CF_DELETE, "/rooms/directs/:id(.:format)", "rooms/directs#destroy", "cf_action_rooms_directs_destroy", "A-rooms-directs", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {129, CF_GET, "/messages/:message_id/boosts(.:format)", "messages/boosts#index", "cf_action_messages_boosts_index", "A-messages-boosts", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {130, CF_POST, "/messages/:message_id/boosts(.:format)", "messages/boosts#create", "cf_action_messages_boosts_create", "A-messages-boosts", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {131, CF_GET, "/messages/:message_id/boosts/new(.:format)", "messages/boosts#new", "cf_action_messages_boosts_new", "A-messages-boosts", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {132, CF_GET, "/messages/:message_id/boosts/:id/edit(.:format)", "messages/boosts#edit", "cf_action_messages_boosts_edit", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {133, CF_GET, "/messages/:message_id/boosts/:id(.:format)", "messages/boosts#show", "cf_action_messages_boosts_show", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {134, CF_PATCH, "/messages/:message_id/boosts/:id(.:format)", "messages/boosts#update", "cf_action_messages_boosts_update", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {135, CF_PUT, "/messages/:message_id/boosts/:id(.:format)", "messages/boosts#update", "cf_action_messages_boosts_update", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {136, CF_DELETE, "/messages/:message_id/boosts/:id(.:format)", "messages/boosts#destroy", "cf_action_messages_boosts_destroy", "A-messages-boosts", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {137, CF_GET, "/messages(.:format)", "messages#index", "cf_action_messages_index", "A-messages", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {138, CF_POST, "/messages(.:format)", "messages#create", "cf_action_messages_create", "A-messages", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {139, CF_GET, "/messages/new(.:format)", "messages#new", "cf_action_messages_new", "H03", CF_ROUTE_REFERENCE_ERROR,
     NULL, 0, cf_action_reference_action_not_found},
    {140, CF_GET, "/messages/:id/edit(.:format)", "messages#edit", "cf_action_messages_edit", "A-messages", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {141, CF_GET, "/messages/:id(.:format)", "messages#show", "cf_action_messages_show", "A-messages", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {142, CF_PATCH, "/messages/:id(.:format)", "messages#update", "cf_action_messages_update", "A-messages", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {143, CF_PUT, "/messages/:id(.:format)", "messages#update", "cf_action_messages_update", "A-messages", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {144, CF_DELETE, "/messages/:id(.:format)", "messages#destroy", "cf_action_messages_destroy", "A-messages", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {145, CF_DELETE, "/searches/clear(.:format)", "searches#clear", "cf_action_searches_clear", "A-searches", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {146, CF_GET, "/searches(.:format)", "searches#index", "cf_action_searches_index", "A-searches", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {147, CF_POST, "/searches(.:format)", "searches#create", "cf_action_searches_create", "A-searches", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {148, CF_POST, "/unfurl_link(.:format)", "unfurl_links#create", "cf_action_unfurl_links_create", "A-unfurl_links", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {149, CF_GET, "/webmanifest(.:format)", "pwa#manifest", "cf_action_pwa_manifest", "A-pwa", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {150, CF_GET, "/service-worker(.:format)", "pwa#service_worker", "cf_action_pwa_service_worker", "A-pwa", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {151, CF_GET, "/up(.:format)", "rails/health#show", "cf_action_rails_health_show", "H03", CF_ROUTE_IMPLEMENT,
     NULL, 0, cf_action_health_show},
    {152, CF_GET, "/recede_historical_location(.:format)", "turbo/native/navigation#recede", "cf_action_turbo_native_navigation_recede", "H03", CF_ROUTE_IMPLEMENT,
     NULL, 0, cf_action_turbo_native_recede},
    {153, CF_GET, "/resume_historical_location(.:format)", "turbo/native/navigation#resume", "cf_action_turbo_native_navigation_resume", "H03", CF_ROUTE_IMPLEMENT,
     NULL, 0, cf_action_turbo_native_resume},
    {154, CF_GET, "/refresh_historical_location(.:format)", "turbo/native/navigation#refresh", "cf_action_turbo_native_navigation_refresh", "H03", CF_ROUTE_IMPLEMENT,
     NULL, 0, cf_action_turbo_native_refresh},
    {155, CF_POST, "/rails/action_mailbox/postmark/inbound_emails(.:format)", "action_mailbox/ingresses/postmark/inbound_emails#create", "cf_action_action_mailbox_ingresses_postmark_inbound_emails_create", "H03", CF_ROUTE_IMPLEMENT,
     NULL, 0, cf_action_mailbox_ingress_not_configured},
    {156, CF_POST, "/rails/action_mailbox/relay/inbound_emails(.:format)", "action_mailbox/ingresses/relay/inbound_emails#create", "cf_action_action_mailbox_ingresses_relay_inbound_emails_create", "H03", CF_ROUTE_IMPLEMENT,
     NULL, 0, cf_action_mailbox_ingress_not_configured},
    {157, CF_POST, "/rails/action_mailbox/sendgrid/inbound_emails(.:format)", "action_mailbox/ingresses/sendgrid/inbound_emails#create", "cf_action_action_mailbox_ingresses_sendgrid_inbound_emails_create", "H03", CF_ROUTE_IMPLEMENT,
     NULL, 0, cf_action_mailbox_ingress_not_configured},
    {158, CF_GET, "/rails/action_mailbox/mandrill/inbound_emails(.:format)", "action_mailbox/ingresses/mandrill/inbound_emails#health_check", "cf_action_action_mailbox_ingresses_mandrill_inbound_emails_health_check", "H03", CF_ROUTE_IMPLEMENT,
     NULL, 0, cf_action_mailbox_ingress_not_configured},
    {159, CF_POST, "/rails/action_mailbox/mandrill/inbound_emails(.:format)", "action_mailbox/ingresses/mandrill/inbound_emails#create", "cf_action_action_mailbox_ingresses_mandrill_inbound_emails_create", "H03", CF_ROUTE_IMPLEMENT,
     NULL, 0, cf_action_mailbox_ingress_not_configured},
    {160, CF_POST, "/rails/action_mailbox/mailgun/inbound_emails/mime(.:format)", "action_mailbox/ingresses/mailgun/inbound_emails#create", "cf_action_action_mailbox_ingresses_mailgun_inbound_emails_create", "H03", CF_ROUTE_IMPLEMENT,
     NULL, 0, cf_action_mailbox_ingress_not_configured},
    {161, CF_GET, "/rails/conductor/action_mailbox/inbound_emails(.:format)", "rails/conductor/action_mailbox/inbound_emails#index", "cf_action_rails_conductor_action_mailbox_inbound_emails_index", "H03", CF_ROUTE_IMPLEMENT,
     NULL, 0, cf_action_mailbox_conductor_get},
    {162, CF_POST, "/rails/conductor/action_mailbox/inbound_emails(.:format)", "rails/conductor/action_mailbox/inbound_emails#create", "cf_action_rails_conductor_action_mailbox_inbound_emails_create", "H03", CF_ROUTE_IMPLEMENT,
     NULL, 0, cf_action_mailbox_conductor_post},
    {163, CF_GET, "/rails/conductor/action_mailbox/inbound_emails/new(.:format)", "rails/conductor/action_mailbox/inbound_emails#new", "cf_action_rails_conductor_action_mailbox_inbound_emails_new", "H03", CF_ROUTE_IMPLEMENT,
     NULL, 0, cf_action_mailbox_conductor_get},
    {164, CF_GET, "/rails/conductor/action_mailbox/inbound_emails/:id(.:format)", "rails/conductor/action_mailbox/inbound_emails#show", "cf_action_rails_conductor_action_mailbox_inbound_emails_show", "H03", CF_ROUTE_IMPLEMENT,
     NULL, 0, cf_action_mailbox_conductor_get},
    {165, CF_GET, "/rails/conductor/action_mailbox/inbound_emails/sources/new(.:format)", "rails/conductor/action_mailbox/inbound_emails/sources#new", "cf_action_rails_conductor_action_mailbox_inbound_emails_sources_new", "H03", CF_ROUTE_IMPLEMENT,
     NULL, 0, cf_action_mailbox_conductor_get},
    {166, CF_POST, "/rails/conductor/action_mailbox/inbound_emails/sources(.:format)", "rails/conductor/action_mailbox/inbound_emails/sources#create", "cf_action_rails_conductor_action_mailbox_inbound_emails_sources_create", "H03", CF_ROUTE_IMPLEMENT,
     NULL, 0, cf_action_mailbox_conductor_post},
    {167, CF_POST, "/rails/conductor/action_mailbox/:inbound_email_id/reroute(.:format)", "rails/conductor/action_mailbox/reroutes#create", "cf_action_rails_conductor_action_mailbox_reroutes_create", "H03", CF_ROUTE_IMPLEMENT,
     NULL, 0, cf_action_mailbox_conductor_post},
    {168, CF_POST, "/rails/conductor/action_mailbox/:inbound_email_id/incinerate(.:format)", "rails/conductor/action_mailbox/incinerates#create", "cf_action_rails_conductor_action_mailbox_incinerates_create", "H03", CF_ROUTE_IMPLEMENT,
     NULL, 0, cf_action_mailbox_conductor_post},
    {169, CF_GET, "/rails/active_storage/blobs/redirect/:signed_id/*filename(.:format)", "active_storage/blobs/redirect#show", "cf_action_active_storage_blobs_redirect_show", "S02", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {170, CF_GET, "/rails/active_storage/blobs/proxy/:signed_id/*filename(.:format)", "active_storage/blobs/proxy#show", "cf_action_active_storage_blobs_proxy_show", "S02", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {171, CF_GET, "/rails/active_storage/blobs/:signed_id/*filename(.:format)", "active_storage/blobs/redirect#show", "cf_action_active_storage_blobs_redirect_show", "S02", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {172, CF_GET, "/rails/active_storage/representations/redirect/:signed_blob_id/:variation_key/*filename(.:format)", "active_storage/representations/redirect#show", "cf_action_active_storage_representations_redirect_show", "S02", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {173, CF_GET, "/rails/active_storage/representations/proxy/:signed_blob_id/:variation_key/*filename(.:format)", "active_storage/representations/proxy#show", "cf_action_active_storage_representations_proxy_show", "S02", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {174, CF_GET, "/rails/active_storage/representations/:signed_blob_id/:variation_key/*filename(.:format)", "active_storage/representations/redirect#show", "cf_action_active_storage_representations_redirect_show", "S02", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {175, CF_GET, "/rails/active_storage/disk/:encoded_key/*filename(.:format)", "active_storage/disk#show", "cf_action_active_storage_disk_show", "S02", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {176, CF_PUT, "/rails/active_storage/disk/:encoded_token(.:format)", "active_storage/disk#update", "cf_action_active_storage_disk_update", "S02", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
    {177, CF_POST, "/rails/active_storage/direct_uploads(.:format)", "active_storage/direct_uploads#create", "cf_action_active_storage_direct_uploads_create", "S02", CF_ROUTE_IMPLEMENT,
     NULL, 0, CF_ROUTE_DEV_501},
};
/* ---- END GENERATED ROUTE TABLE ---- */

/* ------------------------------------------------------------- utilities */

static cf_span cf_span_lit(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static int cf_hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Strict UTF-8 (same rules String::from_utf8 enforces: no overlongs, no
 * surrogates, at most U+10FFFF). */
static bool cf_utf8_valid(const unsigned char *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = p[i];
        size_t need;
        uint32_t cp;
        if (c < 0x80) {
            i++;
            continue;
        } else if (c >= 0xc2 && c <= 0xdf) {
            need = 1;
            cp = c & 0x1f;
        } else if (c >= 0xe0 && c <= 0xef) {
            need = 2;
            cp = c & 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            need = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + need >= n) return false;
        for (size_t k = 1; k <= need; k++) {
            unsigned char cc = p[i + k];
            if ((cc & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3f);
        }
        if (need == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) {
            return false;
        }
        if (need == 3 && (cp < 0x10000 || cp > 0x10ffff)) return false;
        i += need + 1;
    }
    return true;
}

/* percent_encoding::percent_decode_str: valid %XX becomes a byte, anything
 * else (including a malformed escape) is kept literally. */
static cf_err cf_percent_decode(cf_span in, unsigned char **out,
                                size_t *out_len) {
    unsigned char *buf = malloc(in.len == 0 ? 1 : in.len);
    if (buf == NULL) return CF_NOMEM;
    size_t n = 0;
    for (size_t i = 0; i < in.len;) {
        if (in.ptr[i] == '%' && i + 2 < in.len) {
            int hi = cf_hex_value(in.ptr[i + 1]);
            int lo = cf_hex_value(in.ptr[i + 2]);
            if (hi >= 0 && lo >= 0) {
                buf[n++] = (unsigned char)((hi << 4) | lo);
                i += 3;
                continue;
            }
        }
        buf[n++] = in.ptr[i];
        i++;
    }
    *out = buf;
    *out_len = n;
    return CF_OK;
}

/* ---- normalize_path ---------------------------------------------------- */

/* `Journey::Router::Utils.normalize_path`: one leading slash, repeated
 * slashes squeezed, trailing slashes dropped, percent-escapes upcased. */
static cf_err cf_normalize_path(cf_span path, char **out, size_t *out_len) {
    char *buf = malloc(path.len + 2);
    if (buf == NULL) return CF_NOMEM;
    size_t n = 0;
    buf[n++] = '/';
    for (size_t i = 0; i < path.len; i++) {
        unsigned char c = path.ptr[i];
        if (c == '/' && n > 0 && buf[n - 1] == '/') continue;
        buf[n++] = (char)c;
    }
    while (n > 1 && buf[n - 1] == '/') n--;
    for (size_t i = 0; i + 2 < n;) {
        if (buf[i] == '%' && cf_hex_value((unsigned char)buf[i + 1]) >= 0 &&
            cf_hex_value((unsigned char)buf[i + 2]) >= 0) {
            for (size_t k = 1; k <= 2; k++) {
                if (buf[i + k] >= 'a' && buf[i + k] <= 'f') {
                    buf[i + k] = (char)(buf[i + k] - ('a' - 'A'));
                }
            }
            i += 3;
        } else {
            i++;
        }
    }
    buf[n] = '\0';
    *out = buf;
    *out_len = n;
    return CF_OK;
}

/* ---- finite pattern matcher -------------------------------------------- */

#define CF_ROUTE_MAX_ELEMENTS 32
#define CF_ROUTE_MAX_CAPTURES 8

enum {
    CF_EL_LIT = 0,
    CF_EL_PARAM,      /* :name   -> ([^/.?]+), greedy, nonempty */
    CF_EL_GLOB,       /* *name   -> (.+?), non-greedy, nonempty */
    CF_EL_OPT_FORMAT  /* (.:name)-> (?:\\.([^/.?]+))?, greedy-optional */
};

struct cf_el {
    uint8_t op;
    uint8_t cap;                  /* capture index for PARAM/GLOB/OPT_FORMAT */
    const unsigned char *name;    /* capture name (PARAM/GLOB/OPT_FORMAT) */
    size_t name_len;
    const unsigned char *lit;     /* literal text (LIT) */
    size_t lit_len;
};

struct cf_caps {
    bool set[CF_ROUTE_MAX_CAPTURES];
    const unsigned char *ptr[CF_ROUTE_MAX_CAPTURES];
    size_t len[CF_ROUTE_MAX_CAPTURES];
};

/* Compile the artifact's finite grammar into elements; SIZE_MAX when the
 * pattern is outside it (the generator validates every artifact row, so this
 * is a loud internal failure, never a silent mis-route). */
static size_t cf_pattern_parse(const char *pattern, struct cf_el *els,
                               size_t cap) {
    const unsigned char *p = (const unsigned char *)pattern;
    size_t n = 0, ncap = 0;
    while (*p != '\0') {
        if (*p == '(') {
            if (p[1] != '.' || p[2] != ':') return SIZE_MAX;
            const unsigned char *q = p + 3;
            size_t name_len = 0;
            while ((q[name_len] >= 'A' && q[name_len] <= 'Z') ||
                   (q[name_len] >= 'a' && q[name_len] <= 'z') ||
                   (q[name_len] >= '0' && q[name_len] <= '9') ||
                   q[name_len] == '_') {
                name_len++;
            }
            if (name_len == 0 || q[name_len] != ')') return SIZE_MAX;
            if (n >= cap || ncap >= CF_ROUTE_MAX_CAPTURES) return SIZE_MAX;
            els[n].op = CF_EL_OPT_FORMAT;
            els[n].cap = (uint8_t)ncap++;
            els[n].name = q;
            els[n].name_len = name_len;
            els[n].lit = NULL;
            els[n].lit_len = 0;
            n++;
            p = q + name_len + 1;
            continue;
        }
        if (*p == ')' ) return SIZE_MAX;
        if (*p == ':' || *p == '*') {
            const unsigned char *q = p + 1;
            size_t name_len = 0;
            while ((q[name_len] >= 'A' && q[name_len] <= 'Z') ||
                   (q[name_len] >= 'a' && q[name_len] <= 'z') ||
                   (q[name_len] >= '0' && q[name_len] <= '9') ||
                   q[name_len] == '_') {
                name_len++;
            }
            if (name_len == 0) return SIZE_MAX;
            if (n >= cap || ncap >= CF_ROUTE_MAX_CAPTURES) return SIZE_MAX;
            els[n].op = *p == ':' ? CF_EL_PARAM : CF_EL_GLOB;
            els[n].cap = (uint8_t)ncap++;
            els[n].name = q;
            els[n].name_len = name_len;
            els[n].lit = NULL;
            els[n].lit_len = 0;
            n++;
            p = q + name_len;
            continue;
        }
        const unsigned char *start = p;
        while (*p != '\0' && *p != '(' && *p != ')' && *p != ':' &&
               *p != '*') {
            p++;
        }
        if (n >= cap) return SIZE_MAX;
        els[n].op = CF_EL_LIT;
        els[n].cap = 0;
        els[n].name = NULL;
        els[n].name_len = 0;
        els[n].lit = start;
        els[n].lit_len = (size_t)(p - start);
        n++;
    }
    return n;
}

static bool cf_capture_stop(unsigned char c) {
    return c == '/' || c == '.' || c == '?';
}

/* Anchored match equivalent to compile()'s regex, with the same greedy
 * `([^/.?]+)`, non-greedy `(.+?)` and greedy-optional `(?:\.([^/.?]+))?`. */
static bool cf_match_els(const struct cf_el *els, size_t n, size_t i,
                         const unsigned char *s, const unsigned char *end,
                         struct cf_caps *caps) {
    if (i == n) return s == end;
    const struct cf_el *el = &els[i];
    switch (el->op) {
    case CF_EL_LIT:
        if ((size_t)(end - s) < el->lit_len) return false;
        if (el->lit_len != 0 && memcmp(s, el->lit, el->lit_len) != 0) {
            return false;
        }
        return cf_match_els(els, n, i + 1, s + el->lit_len, end, caps);
    case CF_EL_PARAM: {
        size_t max = 0;
        while (s + max < end && !cf_capture_stop(s[max])) max++;
        for (size_t len = max; len >= 1; len--) {
            caps->set[el->cap] = true;
            caps->ptr[el->cap] = s;
            caps->len[el->cap] = len;
            if (cf_match_els(els, n, i + 1, s + len, end, caps)) return true;
        }
        return false;
    }
    case CF_EL_GLOB: {
        for (size_t len = 1; s + len <= end; len++) {
            caps->set[el->cap] = true;
            caps->ptr[el->cap] = s;
            caps->len[el->cap] = len;
            if (cf_match_els(els, n, i + 1, s + len, end, caps)) return true;
        }
        return false;
    }
    case CF_EL_OPT_FORMAT: {
        if (s < end && *s == '.') {
            const unsigned char *q = s + 1;
            size_t max = 0;
            while (q + max < end && !cf_capture_stop(q[max])) max++;
            for (size_t len = max; len >= 1; len--) {
                caps->set[el->cap] = true;
                caps->ptr[el->cap] = q;
                caps->len[el->cap] = len;
                if (cf_match_els(els, n, i + 1, q + len, end, caps)) {
                    return true;
                }
            }
        }
        /* The group is absent: clear any capture left by a failed attempt
         * above, or a caller's successful continuation would report a stale
         * `format`. */
        caps->set[el->cap] = false;
        return cf_match_els(els, n, i + 1, s, end, caps);
    }
    default:
        return false;
    }
}

/* ---- path parameters --------------------------------------------------- */

/* form-urlencode one decoded value so cf_params_parse (H02, form rules)
 * returns the exact decoded bytes: '+', '&', '=' and '%' are escaped too. */
static cf_err cf_form_encode(cf_builder *b, cf_span raw) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < raw.len; i++) {
        unsigned char c = raw.ptr[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            cf_err rc = cf_builder_append(b, (cf_span){&c, 1});
            if (rc != CF_OK) return rc;
        } else {
            unsigned char esc[3] = {'%', (unsigned char)hex[c >> 4],
                                    (unsigned char)hex[c & 0x0f]};
            cf_err rc = cf_builder_append(b, (cf_span){esc, 3});
            if (rc != CF_OK) return rc;
        }
    }
    return CF_OK;
}

static cf_err cf_params_append_pair(cf_builder *b, cf_span name,
                                    cf_span value, bool *first) {
    if (!*first) {
        cf_err rc = cf_builder_append(b, (cf_span){(const unsigned char *)"&",
                                                   1});
        if (rc != CF_OK) return rc;
    }
    *first = false;
    cf_err rc = cf_form_encode(b, name);
    if (rc == CF_OK) {
        rc = cf_builder_append(b, (cf_span){(const unsigned char *)"=", 1});
    }
    if (rc == CF_OK) rc = cf_form_encode(b, value);
    return rc;
}

/* defaults, then captures (percent-decoded, UTF-8 checked), then
 * controller/action, exactly like path_params(). The tree is built through
 * cf_params_parse of the encoded query so H02's form decoder owns the value
 * bytes. Bad UTF-8 in a capture is CF_INVALID (400). */
static cf_err cf_build_path_params(const cf_route *row, const struct cf_el *els,
                                   size_t n, const struct cf_caps *caps,
                                   cf_params **out) {
    cf_builder b;
    memset(&b, 0, sizeof b);
    bool first = true;
    cf_err rc = CF_OK;

    for (size_t i = 0; i < row->default_count && rc == CF_OK; i++) {
        rc = cf_params_append_pair(&b, cf_span_lit(row->defaults[i].name),
                                   cf_span_lit(row->defaults[i].value), &first);
    }
    for (size_t i = 0; i < n && rc == CF_OK; i++) {
        if (els[i].op == CF_EL_LIT) continue;
        if (!caps->set[els[i].cap]) continue;
        unsigned char *decoded = NULL;
        size_t decoded_len = 0;
        rc = cf_percent_decode(
            (cf_span){caps->ptr[els[i].cap], caps->len[els[i].cap]}, &decoded,
            &decoded_len);
        if (rc != CF_OK) break;
        if (!cf_utf8_valid(decoded, decoded_len)) {
            free(decoded);
            rc = CF_INVALID; /* BadRequest("Invalid path parameters") */
            break;
        }
        rc = cf_params_append_pair(
            &b, (cf_span){els[i].name, els[i].name_len},
            (cf_span){decoded, decoded_len}, &first);
        free(decoded);
    }
    if (rc == CF_OK) {
        const char *hash = strchr(row->endpoint, '#');
        cf_span controller = cf_span_lit(row->endpoint);
        cf_span action = {NULL, 0};
        if (hash != NULL) {
            controller.len = (size_t)(hash - row->endpoint);
            action = cf_span_lit(hash + 1);
        }
        rc = cf_params_append_pair(&b, cf_span_lit("controller"), controller,
                                   &first);
        if (rc == CF_OK) {
            rc = cf_params_append_pair(&b, cf_span_lit("action"), action,
                                       &first);
        }
    }
    if (rc != CF_OK) {
        cf_builder_dispose(&b);
        return rc;
    }
    cf_request synthetic;
    memset(&synthetic, 0, sizeof synthetic);
    synthetic.query = (cf_span){b.ptr, b.len};
    rc = cf_params_parse(&synthetic, out);
    cf_builder_dispose(&b);
    return rc;
}

/* ---- public matcher API ------------------------------------------------ */

cf_err cf_route_match_request(const cf_request *request, cf_route_match *out) {
    if (out != NULL) {
        out->id = 0;
        out->path_params = NULL;
    }
    if (request == NULL || out == NULL) return CF_INVALID;

    char *path = NULL;
    size_t path_len = 0;
    cf_err rc = cf_normalize_path(request->path, &path, &path_len);
    if (rc != CF_OK) return rc;

    cf_method verb = request->method == CF_HEAD ? CF_GET : request->method;
    size_t count = 0;
    const cf_route *table = cf_routes(&count);
    cf_err result = CF_NOT_FOUND;
    struct cf_el els[CF_ROUTE_MAX_ELEMENTS];
    struct cf_caps caps;

    for (size_t r = 0; r < count; r++) {
        const cf_route *row = &table[r];
        if (row->method != verb) continue;
        size_t n = cf_pattern_parse(row->pattern, els, CF_ROUTE_MAX_ELEMENTS);
        if (n == SIZE_MAX) { /* unreachable: generator-validated grammar */
            result = CF_INTERNAL;
            break;
        }
        memset(&caps, 0, sizeof caps);
        if (!cf_match_els(els, n, 0, (const unsigned char *)path,
                          (const unsigned char *)path + path_len, &caps)) {
            continue;
        }
        cf_params *params = NULL;
        rc = cf_build_path_params(row, els, n, &caps, &params);
        if (rc != CF_OK) {
            result = rc; /* bad UTF-8 capture fails the match immediately */
            break;
        }
        out->id = row->id;
        out->path_params = params;
        result = CF_OK;
        break;
    }
    free(path);
    return result;
}

void cf_route_match_dispose(cf_route_match *match) {
    if (match == NULL) return;
    cf_params_destroy(match->path_params);
    match->path_params = NULL;
    match->id = 0;
}

cf_action_fn cf_route_action(uint32_t route_id) {
    const cf_route *row = cf_route_by_id(route_id);
    return row != NULL ? row->action : NULL;
}

const cf_route *cf_routes(size_t *count) {
    if (count != NULL) *count = 177;
    return cf_route_table;
}

const cf_route *cf_route_by_id(uint32_t id) {
    for (size_t i = 0; i < 177; i++) {
        if (cf_route_table[i].id == id) return &cf_route_table[i];
    }
    return NULL;
}

/* ---- response helpers -------------------------------------------------- */

static cf_err cf_response_bytes(cf_response *resp, const char *content_type,
                                const unsigned char *bytes, size_t len) {
    cf_err rc = cf_response_header(resp, cf_span_lit("Content-Type"),
                                   cf_span_lit(content_type));
    if (rc != CF_OK) return rc;
    cf_buf *buf = NULL;
    rc = cf_buf_copy((cf_span){bytes, len}, &buf);
    if (rc != CF_OK) return rc;
    rc = cf_response_body(resp, buf);
    cf_buf_release(buf);
    return rc;
}

/* ---- PublicExceptions rendering --------------------------------------- */

static const char *cf_reference_reason(unsigned status) {
    switch (status) {
    case 404:
        return "Not Found";
    case 500:
        return "Internal Server Error";
    default:
        return "";
    }
}

/* exceptions.rs::render: JSON/XML/YAML requests get the {status, error}
 * hash; everything else gets public/<status>.html (or an empty body when
 * that file is absent, PublicExceptions' pass_response). HEAD is empty in
 * the request's format, the wildcard included. Charset is uppercase UTF-8. */
static cf_err cf_reference_error_response(cf_ctx *ctx, unsigned status) {
    cf_response *resp = ctx->response;
    const cf_format *fmt = NULL;
    const cf_format **formats = NULL;
    size_t format_count = 0;
    cf_err rc = cf_ctx_formats(ctx, &formats, &format_count);
    if (rc == CF_INVALID) {
        format_count = 0; /* formats().unwrap_or_default() */
    } else if (rc != CF_OK) {
        return rc;
    }
    if (format_count > 0) fmt = formats[0];
    const char *symbol = fmt != NULL ? fmt->symbol : "html";
    const char *string = fmt != NULL ? fmt->string : "text/html";
    char content_type[128];
    resp->status = status;

    if (ctx->request->method == CF_HEAD) {
        snprintf(content_type, sizeof content_type, "%s; charset=UTF-8",
                 string);
        return cf_response_header(resp, cf_span_lit("Content-Type"),
                                  cf_span_lit(content_type));
    }

    const char *reason = cf_reference_reason(status);
    if (strcmp(symbol, "json") == 0) {
        snprintf(content_type, sizeof content_type,
                 "application/json; charset=UTF-8");
        char body[64];
        int n = snprintf(body, sizeof body,
                         "{\"status\":%u,\"error\":\"%s\"}", status, reason);
        return cf_response_bytes(resp, content_type,
                                 (const unsigned char *)body, (size_t)n);
    }
    if (strcmp(symbol, "xml") == 0) {
        char body[256];
        int n = snprintf(body, sizeof body,
                         "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                         "<hash>\n"
                         "  <status type=\"integer\">%u</status>\n"
                         "  <error>%s</error>\n"
                         "</hash>\n",
                         status, reason);
        return cf_response_bytes(resp, "application/xml; charset=UTF-8",
                                 (const unsigned char *)body, (size_t)n);
    }
    if (strcmp(symbol, "yaml") == 0) {
        char body[128];
        int n = snprintf(body, sizeof body,
                         "---\n:status: %u\n:error: %s\n", status, reason);
        snprintf(content_type, sizeof content_type, "%s; charset=UTF-8",
                 string);
        return cf_response_bytes(resp, content_type,
                                 (const unsigned char *)body, (size_t)n);
    }

    /* text/html page (or the empty pass_response when it is missing). */
    rc = cf_response_header(resp, cf_span_lit("Content-Type"),
                            cf_span_lit("text/html; charset=UTF-8"));
    if (rc != CF_OK) return rc;
    char path[CF_STATIC_ROOT_MAX + 64];
    int written = snprintf(path, sizeof path, "%s/public/%u.html",
                           cf_static_root(), status);
    if (written < 0 || (size_t)written >= sizeof path) return CF_OK;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return CF_OK; /* no page configured: empty body */
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(fd);
        return CF_OK;
    }
    rc = cf_response_file(resp, fd, 0, (uint64_t)st.st_size);
    if (rc != CF_OK) close(fd);
    return rc;
}

cf_err cf_action_reference_action_not_found(cf_ctx *ctx) {
    /* AbstractController::ActionNotFound: Err(Error::NotFound). */
    return cf_reference_error_response(ctx, 404);
}

cf_err cf_action_reference_missing_controller(cf_ctx *ctx) {
    /* rooms/settings: the controller constant fails to load (Error::internal
     * -> 500). The log stays sanitized to the route id. */
    fprintf(stderr, "campfire: dispatch: uninitialized constant (route=%u)\n",
            ctx->route.id);
    return cf_reference_error_response(ctx, 500);
}

/* ---- small built-ins --------------------------------------------------- */

#define CF_HEALTH_HTML \
    "<!DOCTYPE html><html><body style=\"background-color: green\"></body></html>"

cf_err cf_action_health_show(cf_ctx *ctx) {
    const cf_format *offered[2] = {&cf_format_html, &cf_format_json};
    const cf_format *chosen = NULL;
    cf_err rc = cf_ctx_respond_to(ctx, offered, 2, &chosen);
    if (rc != CF_OK) return rc;
    ctx->response->status = 200;
    if (chosen != NULL && strcmp(chosen->symbol, "json") == 0) {
        int64_t us = cf_now_us(ctx->app);
        time_t secs = (time_t)(us / INT64_C(1000000));
        struct tm tm;
        char timestamp[32] = "1970-01-01T00:00:00Z";
        if (gmtime_r(&secs, &tm) != NULL) {
            (void)strftime(timestamp, sizeof timestamp, "%Y-%m-%dT%H:%M:%SZ",
                           &tm);
        }
        char body[96];
        int n = snprintf(body, sizeof body,
                         "{\"status\":\"up\",\"timestamp\":\"%s\"}",
                         timestamp);
        return cf_response_bytes(ctx->response,
                                 "application/json; charset=utf-8",
                                 (const unsigned char *)body, (size_t)n);
    }
    return cf_response_bytes(ctx->response, "text/html; charset=utf-8",
                             (const unsigned char *)CF_HEALTH_HTML,
                             strlen(CF_HEALTH_HTML));
}

static cf_err cf_turbo_native_html(cf_ctx *ctx, const char *text) {
    ctx->response->status = 200;
    return cf_response_bytes(ctx->response, "text/html; charset=utf-8",
                             (const unsigned char *)text, strlen(text));
}

cf_err cf_action_turbo_native_recede(cf_ctx *ctx) {
    return cf_turbo_native_html(ctx, "Going back\xE2\x80\xA6");
}

cf_err cf_action_turbo_native_resume(cf_ctx *ctx) {
    return cf_turbo_native_html(ctx, "Staying put\xE2\x80\xA6");
}

cf_err cf_action_turbo_native_refresh(cf_ctx *ctx) {
    return cf_turbo_native_html(ctx, "Refreshing\xE2\x80\xA6");
}

/* `head status`: no body; the rendered format's bare content type. */
static cf_err cf_head_status(cf_ctx *ctx, unsigned status) {
    ctx->response->status = status;
    const cf_format *fmt = cf_ctx_rendered_format(ctx);
    return cf_response_header(ctx->response, cf_span_lit("Content-Type"),
                              cf_span_lit(fmt->string));
}

cf_err cf_action_mailbox_ingress_not_configured(cf_ctx *ctx) {
    /* ActionMailbox::BaseController#ensure_configured: no ingress
     * configured -> head 404 for every ingress route. */
    return cf_head_status(ctx, 404);
}

cf_err cf_action_mailbox_conductor_get(cf_ctx *ctx) {
    /* Rails::Conductor::BaseController: verify_authenticity_token is a no-op
     * for safe methods, then head 403. */
    return cf_head_status(ctx, 403);
}

cf_err cf_action_mailbox_conductor_post(cf_ctx *ctx) {
    /* Unsafe methods run A01's verify_authenticity_token first: CF_OK with a
     * halted response is the 422 rejection (the halt convention in auth.h);
     * a cleared check falls through to the same head 403 as the GET rows. */
    cf_err rc = cf_check_csrf(ctx, false);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;
    return cf_head_status(ctx, 403);
}

cf_err cf_action_not_landed_501(cf_ctx *ctx) {
    const cf_route *row = cf_route_by_id(ctx->route.id);
    char body[256];
    int n = snprintf(body, sizeof body,
                     "501 Not Implemented: route %u %s (%s)\n",
                     ctx->route.id, row != NULL ? row->c_symbol : "?",
                     row != NULL ? row->pattern : "?");
    if (n < 0) n = 0;
    ctx->response->status = 501;
    return cf_response_bytes(ctx->response, "text/plain; charset=utf-8",
                             (const unsigned char *)body, (size_t)n);
}
