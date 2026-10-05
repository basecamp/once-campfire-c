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

/* A-users-avatars (route 53; destroy 54 stays on dev-501 until S02). */
cf_err cf_action_users_avatars_show(cf_ctx *ctx);

/* A-searches (routes 145, 146, 147). */
cf_err cf_action_searches_clear(cf_ctx *ctx);
cf_err cf_action_searches_index(cf_ctx *ctx);
cf_err cf_action_searches_create(cf_ctx *ctx);

#endif /* CF_ACTIONS_H */
