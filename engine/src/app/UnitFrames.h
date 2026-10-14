#pragma once

#include "core/Types.h"
#include "game/UnitText.h"
#include "render/Renderer.h"
#include "ui/Widgets.h"

#include <span>
#include <string_view>

namespace vd {

// Parity pass: the player frame, the target frame and the experience bar as the 1.12 FrameXML draws them
// (PlayerFrame.xml/.lua, TargetFrame.xml/.lua, MainMenuBar.xml's MainMenuExpBar and ExhaustionTick), as pure
// views: app/UnitScreens fills them from the game, app/Hud draws them where the HUD lays them out,
// app/UnitsGallery with sample data. Positions and sizes are the client's interface units (ui::Gui::unit).
// Touch adaptations: a tap is the left click, a long press the right click; the text the client shows on
// hover shows on a tap.

// PlayerFrame_UpdateStatus: what the portrait's corner shows.
enum class PlayerStatus : u8 {
    None,
    Resting,   // IsResting: "zzz" with its glow, the frame's gold glow pulsing
    Attacking, // PLAYER_ENTER_COMBAT (auto attack on): the crossed swords with their glow and red backdrop
    InCombat,  // PLAYER_REGEN_DISABLED (on a hate list): the swords only
};

struct PlayerFrameView {
    std::string_view name;
    u32 level = 0;
    u32 health = 1, maxHealth = 1;
    u32 power = 0, maxPower = 0;
    u8 powerType = 0;        // 0 mana, 1 rage (kept in tenths), 2 focus, 3 energy, 4 happiness
    u8 race = 1, gender = 0; // the stand-in portrait (TemporaryPortrait-<gender>-<race>)
    TextureHandle portrait;  // the live one (invalid: the stand-in)
    PlayerStatus status = PlayerStatus::None;
    // PlayerStatusTexture: the frame's glow, gold resting, red in combat, pulsing. PlayerFrame_UpdateStatus
    // leaves it on when the fight goes on after the auto attack stopped (InCombat after Attacking), off
    // otherwise.
    bool statusGlow = false;
    bool leader = false;       // UI-Group-LeaderIcon (IsPartyLeader)
    bool masterLooter = false; // UI-Group-MasterLooter (GetLootMethod's lootMaster == 0, in a group)
    game::PvpBadge pvp = game::PvpBadge::None;
    // Phase 15 (options): the bars' numbers always (STATUS_BAR_TEXT, textLockable); else only while a finger is on the
    // frame (TextStatusBar's lockShow: the client's mouse-over).
    bool barText = true;
};

struct TargetFrameView {
    std::string_view name;
    i32 level = 0;          // -1: the skull (UnitLevel's "??")
    u32 levelColor = 0;     // TargetFrame_CheckLevel (game::levelColor)
    u32 nameBackground = 0; // TargetFrame_CheckFaction (game::targetNameBackground)
    bool tapped = false;    // tapped by someone else: the portrait grayed too
    u32 health = 1, maxHealth = 1;
    u32 power = 0, maxPower = 0;
    u8 powerType = 0;
    bool dead = false;
    game::Classification classification = game::Classification::Normal;
    TextureHandle portrait;
    u64 portraitFallback = 0; // its stand-in art (TemporaryPortrait-*)
    game::PvpBadge pvp = game::PvpBadge::None;
    bool leader = false; // UnitIsPartyLeader("target")
    u8 raidTarget = 0;   // phase 13: GetRaidTargetIndex("target") (TargetRaidTargetIcon), 0 none
};

// What happened on a frame this frame.
struct UnitFrameResult {
    bool tapped = false;      // the left click
    bool held = false;        // a long press: the right click (reported once)
    bool badgeTapped = false; // the player's PvP badge (PlayerPVPIconHitArea)
};

// `area` is the frame's 232 x 100 units. `nowMs` drives the status glow's pulse (PlayerFrame_OnUpdate).
UnitFrameResult playerFrame(ui::Gui& g, const ui::Rect& area, const PlayerFrameView& v, u64 nowMs);
// Without a character (offline, development): the frame and its stand-in name only.
void playerFramePlaceholder(ui::Gui& g, const ui::Rect& area, std::string_view name);
UnitFrameResult targetFrame(ui::Gui& g, const ui::Rect& area, const TargetFrameView& v);
// Where the player frame's PvP badge takes taps (PlayerPVPIconHitArea: 39 x 37 units over the badge).
ui::Rect playerBadgeRect(const ui::Gui& g, const ui::Rect& area);

struct ExperienceBarView {
    u32 xp = 0, next = 0;  // next 0: the maximum level (no bar)
    u32 rested = 0;        // GetXPExhaustion: the rested bonus left
    u8 restState = 0;      // game::kRestStateRested: blue; otherwise purple
    bool showText = false; // tapped lately: "xp / next" on the bar, and the rest state's tooltip by its tick
};
// MainMenuExpBar along `area` (the client's is 13 units tall), with ExhaustionLevelFillBar, the bar's
// segmented frame and the ExhaustionTick. Returns true when tapped.
bool experienceBar(ui::Gui& g, const ui::Rect& area, const ExperienceBarView& v);
// Where the ExhaustionTick sits on the bar `area` (w 0: hidden, the rested bonus reaches past the level).
ui::Rect exhaustionTickRect(const ui::Gui& g, const ui::Rect& area, const ExperienceBarView& v);

} // namespace vd
