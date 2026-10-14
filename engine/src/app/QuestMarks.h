#pragma once

#include "core/Types.h"
#include "game/Entities.h"

#include <unordered_map>
#include <vector>

namespace vd {

class Actors;
class GameSession;
class Units;
namespace game {
class Gameplay;
}

// Ours from WoW.exe (docs/PROTOCOL.md §2.24): the marks over the NPCs, the client's own Interface\Buttons\TalkToMe*.m2
// (actors.vdpk) on the unit's name point (PlayerName 18; PlayerNameMounted 29 on a rider), one per unit (0x6074C0):
// - a quest giver's SMSG_QUESTGIVER_STATUS (0x607440, table 0x80C454): grey "!" a quest not yet, yellow "!" one to take,
//   grey "?" one not done, yellow "?" one to turn in, light blue "?" a reputation reward; none while it is hostile or
//   dead (0x607380, its NPC flags cleared);
// - a flight master whose path the player has not learned (SMSG_TAXINODE_STATUS, 0x5ECDD0): the green "!".
// The model keeps its size whatever the unit's (its point's scale undone, 0x607570: Actors' rescale) and rises over the
// unit's name while that shows (StandHigh, 0x6076C0).
class QuestMarks {
public:
    // Whether the unit's name shows (last frame's): the mark plays StandHigh over it.
    using NameShown = bool (*)(const void* context, u64 guid);
    // Once per frame after Units::update().
    void update(GameSession& session, const Units& units, Actors& actors, NameShown nameShown, const void* context);
    // The mark `unit` shows to `me` (an index into WoW.exe's table at 0xC4D9D8; 0: none).
    static u8 markOf(const game::Gameplay& play, const game::Entity& me, const game::Entity& unit);
    // How high over the unit's name point the mark starts while it rises over the name (StandHigh: the models' bob
    // of 0.155 yd plus 0.517): the HUD's name keeps under it.
    static constexpr f32 kRaisedBottom = 0.155f + 0.517f;
    void clear(Actors& actors);
    u32 count() const { return static_cast<u32>(m_marks.size()); }

private:
    struct Mark {
        u32 actor = ~0u;
        u32 parent = ~0u;
        u32 attachment = 0;
        u8 model = 0; // index into WoW.exe's table (0xC4D9D8)
        u16 animation = 0xFFFF;
        u32 seen = 0; // frame
    };
    std::unordered_map<u64, Mark> m_marks;
    std::vector<u64> m_gone; // scratch
    u32 m_frame = 0;
};

} // namespace vd
