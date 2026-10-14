#pragma once

#include "app/Dialogs.h"
#include "app/PopupWindows.h"
#include "app/RaidWindows.h"
#include "core/Types.h"
#include "game/RaidRoster.h"
#include "game/Social.h"

#include <array>
#include <string_view>

namespace vd {

// Development (phase 16, raidframe): the social window's raid tab with a sample raid of 40 (every class, a
// leader, two assistants, one dead, one offline, one whose class is not known yet), offline
// (VD_UI_GALLERY=raid: the player leads; raid2: the player is a plain member; raid3: the player leads 37,
// three groups with an empty slot; raid0: a party, not a raid). What the tab asks for goes to the log ("UI
// gallery: raid ..."); a member moved or swapped changes groups 0.6 s later, as the server's next group list
// would; Add Member shows the ADD_RAIDMEMBER popup.
class RaidGallery {
public:
    // "raid", "raid0", "raid2", "raid3": true when it is one of these pages (the page to draw).
    bool select(std::string_view page);
    void draw(DialogContext& c, u64 nowMs);

private:
    enum class Page : u8 { Leader, NotInRaid, Member, Partial };
    void build();
    void fill(RaidTabView& view);

    Page m_page = Page::Leader;
    bool m_built = false;
    std::array<game::PartyMemberView, game::kMaxRaidMembers> m_roster{};
    u32 m_count = 0;
    game::RaidSlots m_slots;
    std::array<RaidMemberButtonView, game::kMaxRaidMembers> m_views{};
    std::array<std::array<char, 4>, game::kMaxRaidMembers> m_levels{};
    RaidTabDrag m_drag;
    // A move or swap the "server" applies later (member -1: none).
    i32 m_pendingMember = -1, m_pendingOther = -1;
    u8 m_pendingGroup = 0;
    u64 m_pendingAtMs = 0;
    // ADD_RAIDMEMBER's sample.
    bool m_popup = false;
    std::array<PopupView, 4> m_popupViews{};
    std::array<f32, 4> m_popupHeights{};
};

} // namespace vd
