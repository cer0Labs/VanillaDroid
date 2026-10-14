#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"
#include "game/ItemTooltip.h"

#include <array>

namespace vd {

class GameSession;
class Hud;
struct HudOutput;

namespace game {
class GameData;
}

// Phase 16 (alerts): the tooltip of a game object held still in the world (HudOutput::objectHold: the touch
// screen's hover over it), at the GameTooltip's default anchor as the unit tooltip: its name (white), then
// what its type adds (WoW.exe's GameObject_C tooltip method, vtable slot 0x84; the base one adds nothing). A
// meeting stone's (0x5F6B10) adds its zone (AreaTable name of data2) and MEETINGSTONE_LEVEL with
// data0..data1, both in AddLine's default colour (NORMAL_FONT_COLOR, 0xC0D3E8). The other types' lines
// (locks, required skills) are not here yet. Kept until tapped or the object goes. Owned by GameScreens. Main
// thread; fixed storage.
class ObjectTooltip {
public:
    void init(const game::GameData* data) { m_data = data; }
    void actOnHud(GameSession& session, const HudOutput& out);
    void draw(GameSession& session, DialogContext& c, const Hud& hud);
    void reset() { m_guid = 0; }

private:
    u32 fill(GameSession& session); // the lines of m_guid's object; 0 when it has none to show

    const game::GameData* m_data = nullptr;
    u64 m_guid = 0;
    std::array<game::TooltipLine, 3> m_lines{};
    std::array<char, 64> m_level{};
};

} // namespace vd
