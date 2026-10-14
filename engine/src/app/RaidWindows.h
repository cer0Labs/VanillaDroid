#pragma once

#include "app/DialogKit.h"
#include "app/Dialogs.h"
#include "app/FriendsFrameLayout.h"
#include "core/Types.h"
#include "ui/Context.h"
#include "ui/DrawList.h"

#include <span>
#include <string_view>

namespace vd {

// Phase 16: the social window's raid tab (FriendsFrame's fourth tab: RaidFrame.xml with Blizzard_RaidUI's
// RaidGroupFrame). A pure view like app/SocialWindows: what the tab shows in, what the player did out;
// app/RaidScreens fills it from game::Social (with game/RaidRoster's rules) and acts. Interface words by key
// (core/Text: the client's RAID_DESCRIPTION, CONVERT_TO_RAID, ADD_RAID_MEMBER, READY_CHECK, RAID_INFO, GROUP,
// EMPTY); names as they come.
//
// Layout (retouches, friends frame: on FriendsFrame's RaidFrame art, app/FriendsFrameLayout): the buttons in the band
// under the title where RaidFrame.xml / Blizzard_RaidUI.xml put them (RaidFrameConvertToRaidButton /
// RaidFrameAddMemberButton at (72, -37) / (71, -37), Ready Check and Raid Info after them), then, in a raid, the eight
// RaidGroup boxes of five slots (UI-RaidFrame-GroupOutline) two by two from (16, -70) as RaidGroup1..8 are anchored,
// each member's RaidGroupButtonTemplate in its slot (UI-RaidFrame-GroupButton: rank token, name, level, class at the
// template's offsets); out of a raid RAID_DESCRIPTION at (29, -85). The slots are a finger high at least (taller than
// the original's 14 units where the frame is small): then the boxes scroll.
//
// Touch: a tap is the left click (RaidGroupButton_OnClick: target the member). A long press is the right
// click (the RAID menu), except for the raid's leader, whose long press lifts the member: moved, it is
// dragged (RaidGroupButton_OnDragStart / OnDragStop, the slot under the finger lit as RaidGroupFrame_OnUpdate
// does; near the box's edges the list scrolls), released where it was, the menu opens. A plain drag scrolls
// the list, so every drag of the tab starts with a long press, as the bags' items do. A group's label
// (RaidGroupNLabel) and a member's class text (RaidGroupButtonNClass) lifted the same way and dropped out of
// the window ask for a pullout (RaidPullout_GenerateGroupFrame / _GenerateClassFrame); taps on them do
// nothing (they have no OnClick in 1.12).

constexpr u32 kRaidTabSlots = 40; // NUM_RAID_GROUPS x MEMBERS_PER_RAID_GROUP

struct RaidMemberButtonView {
    u64 key = 0;                // the member's GUID: its button's identity across frames
    std::string_view name;      // UNKNOWN when nameless
    std::string_view level;     // "" for level 0
    std::string_view className; // "" while unknown
    std::string_view rank;      // RAID_LEADER_TOKEN, RAID_ASSISTANT_TOKEN or ""
    u32 color = 0;   // name, level and class: RAID_CLASS_COLORS, red dead, gray offline (0: the font's gold)
    u8 subgroup = 0; // 0..7
};

struct RaidTabView {
    bool inRaid = false; // GetNumRaidMembers() > 0
    // Out of a raid: "Convert To Raid" (RaidFrame_Update's rule) and RAID_DESCRIPTION.
    bool convertEnabled = false;
    bool raidInfoEnabled = false; // RaidFrameRaidInfoButton (GetNumSavedInstances() > 0)
    // In a raid.
    bool readyCheck = false; // RaidFrameReadyCheckButton shown (RaidFrameReadyCheckButton_Update: the leader)
    bool addMemberEnabled = false; // RaidFrameAddMemberButton enabled (the leader or an assistant)
    bool canMove = false;          // IsRaidLeader(): members can be dragged to other groups
    std::span<const RaidMemberButtonView> members; // GetRaidRosterInfo order (raid1 first)
    std::span<const i8> slots;                     // kRaidTabSlots: group * 5 + place -> member, -1 empty
};

// A drag of the tab that lasts several frames (the caller keeps it; reset() when the tab is not drawn).
struct RaidTabDrag {
    enum class Kind : u8 { None, Member, Group, Class };
    Kind kind = Kind::None;
    bool moving = false; // the finger moved after the long press (else the button is only lifted)
    u64 key = 0;         // Member / Class: the member's GUID
    u8 group = 0;        // Group: its subgroup 0..7
    i32 touch = -1;
    u64 touchDownMs = 0;
    f32 x = 0.0f, y = 0.0f;
    i32 targetSlot = -1; // TARGET_RAID_SLOT: the slot under the finger (Member)
    ui::Rect from;       // Member: its slot when lifted (the menu hangs there)
    u64 lastMs = 0;      // the list's scrolling while dragging...
    u64 edgeSinceMs = 0; // ...once the finger stays at its edge (0: not there)
    void reset() { *this = RaidTabDrag{}; }
};

struct RaidTabResult {
    i32 tapped = -1;                      // a member's button tapped (the left click)
    i32 menu = -1;                        // a member's right click (the RAID menu)...
    ui::Rect menuRect;                    // ...at its button
    i32 dropMember = -1;                  // a member's button dropped on a slot...
    i32 dropSlot = -1;                    // ...this one (group * 5 + place)
    i32 pulloutGroup = -1;                // a group's label (0..7) dropped out of the window...
    i32 pulloutMember = -1;               // ...or a member's class text...
    f32 pulloutX = 0.0f, pulloutY = 0.0f; // ...here
    bool convert = false, addMember = false, readyCheck = false, raidInfo = false;
};

// The tab on the friends frame `l` (at its sizes `s`); `window`: the whole window (a label or class text dropped
// outside it asks for a pullout).
RaidTabResult raidTab(DialogContext& c, const FriendsFrameLayout& l, const kit::QuestArtSizes& s,
                      const ui::Rect& window, const RaidTabView& v, RaidTabDrag& drag);

} // namespace vd
