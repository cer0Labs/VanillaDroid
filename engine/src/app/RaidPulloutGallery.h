#pragma once

#include "app/Dialogs.h"
#include "app/RaidPulloutLayout.h"
#include "app/UnitMenuWindows.h"
#include "core/Types.h"

#include <array>

namespace vd {

// Development (phase 16, pullouts): the raid's pullout frames with app/RaidPulloutSamples' raid of 40, for
// UiGallery:
// - "pullouts": Group 1, the player's Group 3, Group 8 (far away, one offline), the priests with their buffs,
// and the
//   party frames left showing the player's own subgroup (in a raid "party1".."party4" are the others of Group
//   3);
// - "pullouts2": every group and two classes at once (the stress case), one with its background hidden and
// the menu of
//   another open.
// The frames can be dragged (dropped off the stick's pad and the action cluster, both shaded as the game
// would place them), a long press opens a frame's menu, taps go to the log ("UI gallery: ...").
class RaidPulloutGallery {
public:
    enum class Page : u8 { Pullouts, All, Count };
    void draw(DialogContext& c, Page page, u64 nowMs);

private:
    struct Frame {
        bool shown = true;
        bool byClass = false;
        u8 filter = 0;
        bool showBuffs = false;
        f32 unit = 1.0f;
        bool background = true;
        bool placed = false; // its first place set
        bool place = false;  // dropped: placed again off the HUD's controls
        f32 x = 0.0f, y = 0.0f;
        bool dragging = false;
        f32 dragX = 0.0f, dragY = 0.0f;
    };
    Page m_page = Page::Count;
    std::array<Frame, kMaxRaidPullouts> m_frames{};
    u32 m_frameCount = 0;
    i32 m_menuFrame = -1;
    std::array<DropDownItemView, 4> m_menuItems{};
};

} // namespace vd
