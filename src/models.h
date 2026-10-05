/* src/models.h — umbrella for the frozen model headers (D01; declarations only).
 *
 * Includes every module header in src/models/ plus the shared vocabulary of
 * src/models/types.h. D01 owns this file; each module header names its pinned
 * Rust source and the schema.sql tables it maps. See
 * docs/devel/evidence/D01-headers.md for the per-symbol coverage record.
 */
#ifndef CF_MODELS_H
#define CF_MODELS_H

#include "models/types.h"
#include "models/account.h"
#include "models/active_storage.h"
#include "models/ban.h"
#include "models/boost.h"
#include "models/first_run.h"
#include "models/membership.h"
#include "models/message.h"
#include "models/push_subscription.h"
#include "models/rich_text_record.h"
#include "models/room.h"
#include "models/search.h"
#include "models/session.h"
#include "models/sound.h"
#include "models/user.h"
#include "models/webhook.h"

#endif /* CF_MODELS_H */
