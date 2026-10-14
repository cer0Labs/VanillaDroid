#pragma once

#include "core/Types.h"
#include "ui/DrawList.h"

#include <span>

namespace vd {

// Phase 16 (pullouts): the geometry of Blizzard_RaidUI.xml's pullout frames in interface units, their
// colours, and where one may stay on a touch screen. Pure functions (no drawing, no game state; unit tested):
// app/RaidPulloutWindows draws with them, app/RaidPullouts keeps the frames.

// RaidPulloutFrameTemplate is 72 wide; RAID_PULLOUT_BUTTON_HEIGHT (a 25-unit button and the 8 to the next);
// the frame's height n * RAID_PULLOUT_BUTTON_HEIGHT + 14 (RaidPullout_Update); its name over the top edge
// (HitRectInsets: top -13); the auras hang right of the frame up to 97 (RaidBuffButtonTemplate); four of them
// (MAX_PARTY_DEBUFFS).
constexpr f32 kRaidPulloutWidth = 72.0f;
constexpr f32 kRaidPulloutButtonHeight = 33.0f;
constexpr f32 kRaidPulloutPadding = 14.0f;
constexpr f32 kRaidPulloutTitle = 13.0f;
constexpr f32 kRaidPulloutReach = 97.0f;
constexpr u32 kRaidPulloutAuras = 4;
constexpr u32 kMaxRaidPulloutMembers = 40; // MAX_RAID_MEMBERS
// RaidPullout_GetFrame never shows two frames with the same filter: the 8 subgroups and the 9 classes at
// most.
constexpr u32 kMaxRaidPullouts = 17;

// Touch: the frames at the party frames' size (app/SocialWindows: 1.6 x the client's units) while they fit
// the screen's height, down to the client's own size for a long class list (beyond that the bottom rows go
// off the screen).
constexpr f32 kRaidPulloutScale = 1.6f;
f32 raidPulloutUnit(f32 baseUnit, u32 members, f32 screenHeight);

// RAID_CLASS_COLORS: game::raidClassColor (game/RaidRoster.h).
// ManaBarColor (UnitFrame.lua): mana, rage, focus, energy, happiness.
u32 raidPowerColor(u8 powerType);
// RED_FONT_COLOR (dead), GRAY_FONT_COLOR (offline: the name and both bars at 0.5), the health bar's green
// (HealthBar_OnValueChanged without `smooth`), GameFontNormalSmall's NORMAL_FONT_COLOR (the title).
constexpr u32 kRaidDeadColor = ui::rgba(255, 26, 26);
constexpr u32 kRaidOfflineColor = ui::rgba(128, 128, 128);
constexpr u32 kRaidHealthColor = ui::rgba(0, 255, 0);
constexpr u32 kRaidTitleColor = ui::rgba(255, 209, 0);

// Where the parts of a pullout go, in logical pixels: the frame's top left at (x, y), `unit` logical pixels
// per interface unit, `members` buttons.
struct RaidPulloutGeometry {
    f32 x = 0.0f, y = 0.0f;
    f32 unit = 1.0f;
    u32 members = 0;

    // The backdrop, 72 x (n * 33 + 14) (MenuBackdrop: setAllPoints); the 13 units over it where its name
    // hangs (BOTTOM to the frame's TOP).
    ui::Rect frame() const;
    ui::Rect title() const;
    // RaidPulloutButtonTemplate i: 57 x 25, its TOP 1 right and 10 under the frame's, 8 apart; the button
    // grown into the gaps (4 over it, 4 under it): what a tap on it hits.
    ui::Rect button(u32 i) const;
    ui::Rect row(u32 i) const;
    // Its parts (RaidPulloutButtonTemplate): UI-RaidFrame-HealthBar's frame (64 x 16 at its bottom left), the
    // name (52 x 12 at (2, 3) above its top), the health bar (51 x 4 at the bottom left's (3, 10)) and the
    // mana bar 3 under.
    ui::Rect barFrame(u32 i) const;
    ui::Rect name(u32 i) const;
    ui::Rect health(u32 i) const;
    ui::Rect mana(u32 i) const;
    // Its aura a (0..3), RaidBuffButtonTemplate's 12 x 12 icons: Debuff1 6 right of the button's top right
    // and 1 down, Debuff2 right of it, Debuff3 and 4 under them, 1 apart.
    ui::Rect aura(u32 i, u32 a) const;
    // What takes the finger: the title, the frame and the auras' column.
    ui::Rect hit() const;
};

// Touch placement of a pullout's hit rectangle (ValidateFramePosition(frame, 25) after a drag, and
// clampedToScreen): kept inside `safe` (top and left first when it is larger), then off the rectangles of
// `avoid` (the stick's pad, the action buttons, the unit frames...: 1.12 draws the pullouts under them, on
// the BACKGROUND strata, and its rule keeps them off the main action bar): the nearest place against their
// edges that covers none of them and stays inside `safe` (the one covering least when there is none). Returns
// the rectangle moved.
ui::Rect raidPulloutPlace(const ui::Rect& hit, const ui::Rect& safe, std::span<const ui::Rect> avoid);

// Development (app/RaidPulloutGallery, VD_RAID_DEMO): the top left of the n-th of several cells `w` x `h`
// laid out in `space`, side by side from its top left, row after row, cascading over the first ones once it
// is full.
ui::Rect raidPulloutDemoCell(u32 n, const ui::Rect& space, f32 w, f32 h);

} // namespace vd
