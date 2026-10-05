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

#endif /* CF_ACTIONS_H */
