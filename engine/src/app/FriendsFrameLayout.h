#pragma once

#include "core/Types.h"
#include "ui/DrawList.h"

#include <array>

namespace vd {

// Friends frame: FriendsFrame with its 1.12 art, at its proportions, and where FriendsFrame.xml, RaidFrame.xml and
// Blizzard_RaidUI.xml put the parts of its pages on it: the 384 x 512 picture (its top and bottom halves change with
// the page: FriendsFrame_Update), its portrait, title, close button, each page's lists with their scroll bars, texts
// and buttons; and FriendsFrameTab1..4 turned to stand along its right edge (not 1.12's) instead of hanging under it.
// Pure geometry (unit tested): app/SocialWindows, app/GuildWindows and app/RaidWindows draw it. Rectangles in logical
// pixels.

// What the art shows of its 384 x 512 units: the portrait's ring from 2 across, the border to 350 and 440 down.
constexpr f32 kFriendsFrameLeft = 2.0f;
constexpr f32 kFriendsFrameRight = 350.0f;
constexpr f32 kFriendsFrameHeight = 440.0f;
// The turned tabs' column: an unselected tab's top 4 units over the border (FriendsFrameTab1 hangs from 433 under the
// border's 437), as wide as CharacterFrameTabButtonTemplate is high.
constexpr f32 kFriendsTabsLeft = 345.0f;
constexpr f32 kFriendsTabsWidth = 32.0f;
// The whole: from the ring to the tabs' outer side.
constexpr f32 kFriendsLayoutWidth = kFriendsTabsLeft + kFriendsTabsWidth - kFriendsFrameLeft;

// A page's list (a FauxScrollFrameTemplate and its buttons).
struct FriendsListLayout {
    ui::Rect view;    // where the rows show (they scroll in it)
    f32 rowX = 0.0f;  // the buttons' left...
    f32 rowW = 0.0f;  // ...width...
    f32 rowTop = 0.0f; // ...and the first one's top
    ui::Rect bar;     // the scroll frame's UIPanelScrollBarTemplate: 16 wide, 6 right of it, its buttons within
    // Its track (UI-Character-ScrollBar): the top part 31 wide from 2 left of the scroll frame's right and 5 above it
    // (TexCoords 0..0.484375 across, 0..trackV down), the bottom part 31 x 106 down to 2 under it (0.515625..1,
    // 0..0.4140625).
    std::array<ui::Rect, 2> track{};
    f32 trackV = 1.0f;
};

// WhoFrame.
struct WhoFrameLayout {
    ui::Rect headers; // WhoFrameColumnHeader1..4 (WhoFrameColumnHeaderTemplate, 24 high) from (20, -70)
    FriendsListLayout list; // WhoFrameButton1..17 (298 x 16 from (15, -95)), WhoListScrollFrame 296 x 287
    ui::Rect totals;  // WhoFrameTotals (GameFontNormalSmall, 298 x 16 at BOTTOM (-10, 127))
    ui::Rect edit;    // WhoFrameEditBox (296 x 32 at BOTTOM (-20, 100)), in the art's slot
    ui::Rect refresh; // WhoFrameWhoButton (85 x 22) left of...
    ui::Rect addFriend; // WhoFrameAddFriendButton (120 x 22) left of...
    ui::Rect invite;  // WhoFrameGroupInviteButton (120 x 22 at BOTTOMRIGHT (-40, 82))
};

// GuildFrame.
struct GuildFrameLayout {
    ui::Rect headers;       // GuildFrameColumnHeader1..4 / GuildFrameGuildStatusColumnHeader1..4 from (20, -70)
    FriendsListLayout list; // GuildFrameButton1..13 (298 x 16 from (15, -95)), GuildListScrollFrame 296 x 237
    f32 totalsCenterX = 0.0f; // GuildFrameTotals (GameFontNormalSmall) at BOTTOM (-122, 181)...
    ui::Rect totals;          // ...its line
    ui::Rect toggle;   // GuildFrameGuildListToggleButton (32 x 32 at LEFT (245, -67)), its text right-aligned 32 left
                       // of its right
    ui::Rect motdLabel; // GuildFrameNotesLabel (225 x 11 at (23, -344))
    ui::Rect motd;      // GuildFrameNotesText (315 x 45, 3 under it): GuildMOTDEditButton covers it
    ui::Rect information; // GuildFrameGuildInformationButton (123 x 22) left of...
    ui::Rect addMember;   // GuildFrameAddMemberButton (98 x 22), 2 left of...
    ui::Rect control;     // GuildFrameControlButton (104 x 22 at BOTTOMRIGHT (-39, 82))
};

// RaidFrame and Blizzard_RaidUI's RaidGroupFrame.
struct RaidFrameLayout {
    ui::Rect convert;     // RaidFrameConvertToRaidButton (115 x 21 at (72, -37)), out of a raid
    ui::Rect infoAfterConvert; // RaidFrameRaidInfoButton (90 x 21) 65 right of it
    ui::Rect addMember;   // RaidFrameAddMemberButton (90 x 21 at (71, -37)), in a raid
    ui::Rect readyCheck;  // RaidFrameReadyCheckButton (90 x 21) 2 right of it
    ui::Rect infoAfterReady; // RaidFrameRaidInfoButton 2 right of Ready Check (RaidGroupFrame_Update)
    ui::Rect description; // RaidFrameRaidDescription (GameFontNormal, 300 wide at (29, -85)) down to the frame's floor
    // RaidGroup1..8 (RaidGroupTemplate, 162 x 80 from (16, -70)): two across 3 apart, each 14 under the one above
    // (their labels, RaidGroupNLabel, hang in that gap); the area they and their labels take, down to the floor.
    ui::Rect groups;
    f32 groupW = 0.0f, groupH = 0.0f, columnStep = 0.0f, rowStep = 0.0f, labelH = 0.0f;
};

struct FriendsFrameLayout {
    f32 k = 1.0f;          // logical pixels per unit of the art
    ui::Rect art;          // the 384 x 512 picture: TopLeft 256 x 256 at its top left, TopRight 128 x 256 right of it,
                           // BotLeft and BotRight under them
    ui::Rect frame;        // what of it shows
    ui::Rect portrait;     // FriendsFrameScrollIcon: 60 x 60 at (7, -6), under the ring
    ui::Rect title;        // FriendsFrameTitleText (GameFontNormal) at TOP (0, -18)
    ui::Rect close;        // FriendsFrameCloseButton (UIPanelCloseButton, 32 x 32) at TOPRIGHT (-30, -8)
    ui::Rect tabs;         // the turned tabs' column, from the lists' top down to the border's bottom
    // FriendsListFrame and IgnoreListFrame.
    ui::Rect toggleTabs;   // FriendsFrameToggleTab1 / 2 (TabButtonTemplate, 32 high) from TOPLEFT (70, -39)
    FriendsListLayout friends; // FriendsFrameFriendButton1..10 (298 x 31 from (23, -76)), the scroll frame 296 x 304
    FriendsListLayout ignore;  // FriendsFrameIgnoreButton1..20 (298 x 16 from (23, -80)), 296 x 332
    // The buttons (UIPanelButtonTemplate, 131 x 21).
    ui::Rect addFriend;    // FriendsFrameAddFriendButton: BOTTOMLEFT (17, 107)
    ui::Rect removeFriend; // 5 under it
    ui::Rect sendMessage;  // 66 right of Add Friend
    ui::Rect groupInvite;  // 5 under it
    ui::Rect ignorePlayer; // FriendsFrameIgnorePlayerButton: BOTTOMLEFT (17, 81)
    ui::Rect stopIgnore;   // 62 right of it
    WhoFrameLayout who;
    GuildFrameLayout guild;
    RaidFrameLayout raid;
};

// The frame at `k` logical pixels per unit, or less when `place` is smaller than it with its tabs; at the place's top
// left, or centered in it.
FriendsFrameLayout friendsFrameLayout(const ui::Rect& place, f32 k, bool center);

// Where a frame that 1.12 hangs at FriendsFrame's right (GuildMemberDetailFrame, GuildInfoFrame,
// GuildControlPopupFrame) goes: `top` units under the frame's top, right of the turned tabs, `w` x `h` logical pixels
// (at most the frame's height under `top`); where that does not fit `bounds` (the safe area), as far right as it fits,
// over the frame.
ui::Rect friendsSideFrame(const FriendsFrameLayout& l, const ui::Rect& bounds, f32 top, f32 w, f32 h);

} // namespace vd
