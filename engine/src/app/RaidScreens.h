#pragma once

#include "app/Dialogs.h"
#include "app/RaidWindows.h"
#include "app/SocialWindows.h"
#include "core/Types.h"
#include "game/RaidRoster.h"
#include "game/Social.h"

#include <array>
#include <span>

namespace vd {

class GameSession;
class PopupScreens;
class RaidPullouts;
class UnitMenuScreens;

// Phase 16: the raid tab's glue (app/RaidWindows' view): the roster from game::Social::raidMembers() laid out
// by game/RaidRoster as RaidGroupFrame_Update does, every frame while the tab shows (it follows the server's
// group lists live), and what the player did: RaidGroupButton_OnClick (TargetUnit: Gameplay::setTarget), the
// RAID menu (app/UnitMenuScreens),
// RaidGroupButton_OnDragStop (SetRaidSubgroup / SwapRaidSubgroup, the leader only; the button snaps back
// until the server's next list moves it), the pullouts (app/RaidPullouts), ConvertToRaid, DoReadyCheck, Add
// Member (InviteByName of a cooperative target, else the ADD_RAIDMEMBER popup) and Raid Info. SocialScreens
// owns it and draws it on the social window's FriendsFrame (its RaidFrame art). Main thread; fixed storage for 40 members (no
// allocation per frame).
class RaidScreens {
public:
    // Who the tab hands its actions to (any may be null: that action does nothing).
    struct Links {
        UnitMenuScreens* menus = nullptr;
        PopupScreens* popups = nullptr;
        RaidPullouts* pullouts = nullptr;
    };
    // What the social window does after the tab: Raid Info opens RaidInfoFrame (its modal,
    // app/SocialScreens).
    enum class Action : u8 { None, RaidInfo };

    // The tab's page on FriendsFrame's RaidFrame art (SocialScreens draws the frame and its tabs).
    Action drawTab(GameSession& session, DialogContext& c, const FriendsFrameShell& f, const Links& links);
    // The tab is not drawn (another tab): a drag in progress ends (RaidGroupFrame_OnHide). A tab not drawn
    // for a moment (the window closed) counts as hidden too: shown again, it asks for the saved instances
    // (RaidFrame's OnShow: RequestRaidInfo).
    void hidden() {
        m_drag.reset();
        m_lastDrawMs = 0;
    }

    // RaidGroupFrame_Update's texts and colours of each member's button (the gallery's sample raid uses it
    // too): UNKNOWN without a name, the level ("" at 0: offline members have none), the class's name, the
    // rank token, RAID_CLASS_COLORS / RED_FONT_COLOR when dead / GRAY_FONT_COLOR offline. `levels` keeps the
    // level texts.
    static void buttonViews(std::span<const game::PartyMemberView> roster,
                            std::span<RaidMemberButtonView> out, std::span<std::array<char, 4>> levels);

private:
    void fill(GameSession& session, RaidTabView& view);
    void addMember(GameSession& session, PopupScreens* popups);

    std::array<game::PartyMemberView, game::kMaxRaidMembers> m_roster{};
    u32 m_count = 0;
    game::RaidSlots m_slots;
    std::array<RaidMemberButtonView, game::kMaxRaidMembers> m_views{};
    std::array<std::array<char, 4>, game::kMaxRaidMembers> m_levels{};
    RaidTabDrag m_drag;
    u64 m_lastDrawMs = 0; // 0: hidden
};

} // namespace vd
