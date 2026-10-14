#pragma once

#include "core/Types.h"
#include "game/Appearance.h"

namespace vd {

namespace game {
struct Entity;
}
class GameSession;

// Phase 15 (guild): a player's look from its update fields, as app/Units builds it for the world (race,
// gender, customization, the displays of what it wears — asked to the server while unknown — and its guild's
// tabard design). Returns false while an item's template has not come yet (the caller tries again later). For
// the windows that show a player with changes (the tabard designer's preview; the lead unifies it with the
// dressing room's).
bool playerCharacterLook(GameSession& session, const game::Entity& player, game::CharacterLook& out);

} // namespace vd
