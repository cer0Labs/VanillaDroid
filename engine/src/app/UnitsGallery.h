#pragma once

#include "app/Dialogs.h"
#include "app/SocialWindows.h"
#include "core/Types.h"
#include "game/Entities.h"
#include "game/Reactions.h"
#include "game/UnitText.h"

#include <array>
#include <vector>

namespace vd {

// Development (parity, VD_UI_GALLERY=units / unittips / unitmenu): the unit frames, the unit tooltip and the
// experience bar with Elwynn sample units, as app/UnitScreens fills them from the game's rules
// (game/UnitInfo, game/UnitText): the player frame resting (leader, master looter, Alliance badge) and
// fighting (red glow pulsing), target frames for every colour, classification and level rule (an elite, a
// friendly guard, a world boss's skull, a rare, a rare elite ten levels above, a mob tapped by someone else,
// players of the other side flagged and not, a flagged friend, a corpse), the experience bar rested with its
// tick, text and rest tooltip; the tooltips of those units; the player frame's menu.
class UnitsGallery {
public:
    // Phase 13 (social): Menu is a party member's menu (UnitPopup's "PARTY") with the raid target icons open,
    // MenuSelf the player's own in a group led by someone else, MenuCreature a creature's raid target icons.
    enum class Page : u8 { Frames, Tooltips, Menu, MenuSelf, MenuCreature };
    void draw(DialogContext& c, Page page, u64 nowMs);

private:
    void build();

    bool m_built = false;
    game::Reactions m_reactions;
    std::vector<game::Entity> m_units; // [0] the player
    std::array<game::UnitTooltip, 6> m_tooltips{};
};

} // namespace vd
