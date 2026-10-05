/* Controller action declarations (integrator-owned).
 *
 * One declaration per landed packet action. src/routes.c binds these symbols
 * in its table; a packet's row moves from cf_action_not_landed_501 to its real
 * symbol here when the packet verifies. Rows whose packet has not landed stay
 * on the development 501 (01-foundation-http.md H03).
 */
#ifndef CF_ACTIONS_H
#define CF_ACTIONS_H

#include "cf.h"

/* A-welcome (route 1). */
cf_err cf_action_welcome_show(cf_ctx *ctx);

/* A-first_runs (routes 4, 8). */
cf_err cf_action_first_runs_show(cf_ctx *ctx);
cf_err cf_action_first_runs_create(cf_ctx *ctx);

/* A-sessions (routes 12, 17, 18). */
cf_err cf_action_sessions_new(cf_ctx *ctx);
cf_err cf_action_sessions_destroy(cf_ctx *ctx);
cf_err cf_action_sessions_create(cf_ctx *ctx);

/* A-rooms (routes 96, 97, 101, 104). */
cf_err cf_action_rooms_show(cf_ctx *ctx);
cf_err cf_action_rooms_index(cf_ctx *ctx);
cf_err cf_action_rooms_destroy(cf_ctx *ctx);

/* A-messages (routes 76-83, 137, 138, 140-144). */
cf_err cf_action_messages_index(cf_ctx *ctx);
cf_err cf_action_messages_create(cf_ctx *ctx);
cf_err cf_action_messages_edit(cf_ctx *ctx);
cf_err cf_action_messages_show(cf_ctx *ctx);
cf_err cf_action_messages_update(cf_ctx *ctx);
cf_err cf_action_messages_destroy(cf_ctx *ctx);

/* A-users-bans (routes 55, 56). */
cf_err cf_action_users_bans_destroy(cf_ctx *ctx);
cf_err cf_action_users_bans_create(cf_ctx *ctx);

/* A-users-sidebars (route 57). */
cf_err cf_action_users_sidebars_show(cf_ctx *ctx);

/* A-users-avatars show (route 53); destroy (54) declared below. */
cf_err cf_action_users_avatars_show(cf_ctx *ctx);

/* A-searches (routes 145, 146, 147). */
cf_err cf_action_searches_clear(cf_ctx *ctx);
cf_err cf_action_searches_index(cf_ctx *ctx);
cf_err cf_action_searches_create(cf_ctx *ctx);

/* A-pwa (routes 149, 150). */
cf_err cf_action_pwa_manifest(cf_ctx *ctx);
cf_err cf_action_pwa_service_worker(cf_ctx *ctx);

/* A-qr_code (route 52). */
cf_err cf_action_qr_code_show(cf_ctx *ctx);

/* A-sessions-transfers (routes 9, 10, 11). */
cf_err cf_action_sessions_transfers_show(cf_ctx *ctx);
cf_err cf_action_sessions_transfers_update(cf_ctx *ctx);

/* A-users (routes 50, 51, 74). */
cf_err cf_action_users_new(cf_ctx *ctx);
cf_err cf_action_users_create(cf_ctx *ctx);
cf_err cf_action_users_show(cf_ctx *ctx);

/* A-autocompletable-users (route 75). */
cf_err cf_action_autocompletable_users_index(cf_ctx *ctx);

/* A-accounts (routes 44, 46, 47). */
cf_err cf_action_accounts_edit(cf_ctx *ctx);
cf_err cf_action_accounts_update(cf_ctx *ctx);

/* A-accounts-users (routes 19, 24, 25, 26). */
cf_err cf_action_accounts_users_index(cf_ctx *ctx);
cf_err cf_action_accounts_users_update(cf_ctx *ctx);
cf_err cf_action_accounts_users_destroy(cf_ctx *ctx);

/* A-accounts-bots (routes 29, 30, 31, 32, 34, 35, 36). */
cf_err cf_action_accounts_bots_index(cf_ctx *ctx);
cf_err cf_action_accounts_bots_create(cf_ctx *ctx);
cf_err cf_action_accounts_bots_new(cf_ctx *ctx);
cf_err cf_action_accounts_bots_edit(cf_ctx *ctx);
cf_err cf_action_accounts_bots_update(cf_ctx *ctx);
cf_err cf_action_accounts_bots_destroy(cf_ctx *ctx);

/* A-accounts-bots-keys (routes 27, 28). */
cf_err cf_action_accounts_bots_keys_update(cf_ctx *ctx);

/* A-accounts-join_codes (route 37). */
cf_err cf_action_accounts_join_codes_create(cf_ctx *ctx);

/* A-accounts-logos (routes 38, 39). */
cf_err cf_action_accounts_logos_show(cf_ctx *ctx);
cf_err cf_action_accounts_logos_destroy(cf_ctx *ctx);

/* A-accounts-custom_styles (routes 40, 41, 42). */
cf_err cf_action_accounts_custom_styles_edit(cf_ctx *ctx);
cf_err cf_action_accounts_custom_styles_update(cf_ctx *ctx);

/* A-users-avatars destroy (route 54; show/53 above). */
cf_err cf_action_users_avatars_destroy(cf_ctx *ctx);

/* A-users-profiles (routes 60, 61, 62). */
cf_err cf_action_users_profiles_show(cf_ctx *ctx);
cf_err cf_action_users_profiles_update(cf_ctx *ctx);

/* A-users-push_subscriptions (routes 66, 67, 73). */
cf_err cf_action_users_push_subscriptions_index(cf_ctx *ctx);
cf_err cf_action_users_push_subscriptions_create(cf_ctx *ctx);
cf_err cf_action_users_push_subscriptions_destroy(cf_ctx *ctx);

/* A-users-push_subscriptions-test_notifications (route 65). */
cf_err cf_action_users_push_subscriptions_test_notifications_create(cf_ctx *ctx);

/* A-rooms-refreshes (route 91). */
cf_err cf_action_rooms_refreshes_show(cf_ctx *ctx);

/* A-rooms-involvements (routes 93, 94, 95). */
cf_err cf_action_rooms_involvements_show(cf_ctx *ctx);
cf_err cf_action_rooms_involvements_update(cf_ctx *ctx);

/* A-rooms-opens (routes 105-112). */
cf_err cf_action_rooms_opens_index(cf_ctx *ctx);
cf_err cf_action_rooms_opens_create(cf_ctx *ctx);
cf_err cf_action_rooms_opens_new(cf_ctx *ctx);
cf_err cf_action_rooms_opens_edit(cf_ctx *ctx);
cf_err cf_action_rooms_opens_show(cf_ctx *ctx);
cf_err cf_action_rooms_opens_update(cf_ctx *ctx);
cf_err cf_action_rooms_opens_destroy(cf_ctx *ctx);

/* A-rooms-closeds (routes 113-120). */
cf_err cf_action_rooms_closeds_index(cf_ctx *ctx);
cf_err cf_action_rooms_closeds_create(cf_ctx *ctx);
cf_err cf_action_rooms_closeds_new(cf_ctx *ctx);
cf_err cf_action_rooms_closeds_edit(cf_ctx *ctx);
cf_err cf_action_rooms_closeds_show(cf_ctx *ctx);
cf_err cf_action_rooms_closeds_update(cf_ctx *ctx);
cf_err cf_action_rooms_closeds_destroy(cf_ctx *ctx);

/* A-rooms-directs (routes 121, 122, 123, 124, 125, 128). */
cf_err cf_action_rooms_directs_index(cf_ctx *ctx);
cf_err cf_action_rooms_directs_create(cf_ctx *ctx);
cf_err cf_action_rooms_directs_new(cf_ctx *ctx);
cf_err cf_action_rooms_directs_edit(cf_ctx *ctx);
cf_err cf_action_rooms_directs_show(cf_ctx *ctx);
cf_err cf_action_rooms_directs_destroy(cf_ctx *ctx);

/* A-messages-boosts (routes 129, 130, 131, 136). */
cf_err cf_action_messages_boosts_index(cf_ctx *ctx);
cf_err cf_action_messages_boosts_create(cf_ctx *ctx);
cf_err cf_action_messages_boosts_new(cf_ctx *ctx);
cf_err cf_action_messages_boosts_destroy(cf_ctx *ctx);

/* A-messages-boosts-by_bots (routes 84, 85). */
cf_err cf_action_messages_boosts_by_bots_create(cf_ctx *ctx);
cf_err cf_action_messages_boosts_by_bots_destroy(cf_ctx *ctx);

/* A-messages-by_bots (routes 86, 87, 88, 89, 90). */
cf_err cf_action_messages_by_bots_index(cf_ctx *ctx);
cf_err cf_action_messages_by_bots_create(cf_ctx *ctx);
cf_err cf_action_messages_by_bots_update(cf_ctx *ctx);
cf_err cf_action_messages_by_bots_destroy(cf_ctx *ctx);

/* A-unfurl_links (route 148). */
cf_err cf_action_unfurl_links_create(cf_ctx *ctx);

#endif /* CF_ACTIONS_H */
