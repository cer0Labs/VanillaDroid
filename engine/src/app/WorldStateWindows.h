#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"
#include "ui/DrawList.h"

#include <array>
#include <span>
#include <string_view>

namespace vd {

// Phase 17 (scores): the battleground's and the zone objectives' frames of WorldStateFrame.xml as pure views
// (a view-model in, what the player did out; app/WorldStateScreens fills them from game::WorldStateUi and
// game::BattlefieldScore). Sizes and anchors are the XML's, in units of the original's 768-high interface.

// --- WorldStateAlwaysUpFrame and WorldStateCaptureBar
// --------------------------------------------------------

// One AlwaysUpFrame<n> (WorldStateAlwaysUpTemplate, 45 x 24): the 42-unit icon at LEFT - 6, the text
// (GameFontNormalSmall) at the icon's RIGHT - 12, + 10 up, the dynamic icon (32 units, RIGHT + 40, + 9 up)
// and its additive "Flash" twin flashing (UIFrameFlash 0.5 in, 0.5 out) while the state is 2.
struct AlwaysUpLineView {
    std::string_view text;
    u64 icon = 0;        // assetPathHash of "<icon>.blp" (0: none)
    u64 dynamicIcon = 0; // 0: none
    u64 dynamicFlash = 0;
    bool flashing = false; // state 2: the dynamic icon shows and flashes
    std::string_view tooltip, dynamicTooltip;
};
// One WorldStateCaptureBar<n> (WorldStateCaptureBarTemplate, 173 x 26): CaptureBar_Update's indicator at 25 +
// 124 * (1 - value / 100) units from its left (100: the Alliance's end), its arrow towards where it last
// moved, the side's icon lit beyond 60 / under 40 (the 40-20-40 zones).
struct CaptureBarView {
    f32 value = 50.0f;
    i32 moving = 0; // -1 left (IndicatorLeft), 1 right, 0 still
};
// The touch HUD the frames keep clear of (logical pixels; empty rectangles: none).
struct WorldStateHudAvoid {
    ui::Rect targetFrame; // the target frame with its portrait
    ui::Rect buffs;       // the player's buff rows (app/Hud playerAurasRect)
    ui::Rect cluster;     // MinimapCluster (192 x 192 units at the top right)
    ui::Rect actions;     // the action buttons' cluster
    ui::Rect stance;      // the stance bar beside it
    ui::Rect extraBar;    // the extra action bar (along the top on the wide screens, over the cluster on the
                          // inner one)
    ui::Rect lines;       // the world state lines as drawn (the battle map keeps clear of them)
};
// The two stacks' places this frame (logical pixels), from worldStateHudPlaces().
struct WorldStateHudPlaces {
    ui::Rect lines;          // the lines' block (icons and texts)
    f32 linesCenterX = 0.0f; // AlwaysUpFrame1's TOP point (the frames' 45-unit column)
    f32 linesTop = 0.0f;
    f32 barsRight = 0.0f; // the capture bars' TOPRIGHT (under the minimap cluster)
    f32 barsTop = 0.0f;
    f32 unit = 1.0f; // logical pixels per interface unit (the HUD's)
};
// Where the original puts them (WorldStateAlwaysUpFrame TOP (-5, -15), AlwaysUpFrame1 TOP (-23, -20); the
// capture bars TOPRIGHT at MinimapCluster's BOTTOMRIGHT, UIParent_ManageFramePositions), moved clear of the
// touch HUD: right of the target frame, under the player's buff rows where they would cover them, left of the
// minimap cluster.
WorldStateHudPlaces worldStateHudPlaces(const ui::DrawList& draw, const ui::Rect& safe, f32 unit,
                                        const WorldStateHudAvoid& avoid,
                                        std::span<const AlwaysUpLineView> lines);
// The height the capture bars take (what moves the quest tracker down: UIParent_ManageFramePositions'
// anchorY).
f32 captureBarsHeight(f32 unit, u32 bars);
struct WorldStateHudResult {
    // A line or its dynamic icon held: its tooltip shows (the desktop's mouse over).
    std::string_view tooltip;
    ui::Rect tooltipAnchor;
};
// Not modal: only the lines and the dynamic icons take fingers (a finger held on one shows its tooltip).
WorldStateHudResult worldStateHud(DialogContext& c, const WorldStateHudPlaces& places,
                                  std::span<const AlwaysUpLineView> lines,
                                  std::span<const CaptureBarView> bars, u64 nowMs);
// GameTooltip:SetText(text) (its default colour: gold, WoW.exe 0x531B90) and AddLine(line, 1, 1, 1) beside
// `anchor` (under it, kept on the screen). `titleColor`: AddLine's white for a tooltip of that line alone.
constexpr u32 kTooltipTextColor = ui::rgba(255, 210, 0);
void worldStateTooltip(DialogContext& c, const ui::Rect& anchor, std::string_view title,
                       std::string_view line = {}, u32 titleColor = kTooltipTextColor);

// --- WorldStateScoreFrame
// ----------------------------------------------------------------------------------------

// GetBattlefieldStatInfo(i): a column's header (its text, "Flag\nCaptures"-style from WorldStateUI), the icon
// of its lines ("<icon>0" Horde, "<icon>1" Alliance; none: the number alone) and its tooltip.
struct ScoreColumnView {
    std::string_view text, tooltip;
    bool hasIcon = false;
    std::array<u64, 2> icons{}; // by side: [0] Horde, [1] Alliance
};
// GetBattlefieldScore(i) as WorldStateScoreFrame_Update shows it.
struct ScoreRowView {
    std::string_view name;
    u32 nameColor = 0;                    // Horde (1, 0.1, 0.1), Alliance (0, 0.68, 0.94), the player gold
    i32 faction = -1;                     // the row's highlight (WorldStateFinalScore-Highlight); -1: none
    u64 rankBadge = 0;                    // Interface\PvPRankBadges\PvPRank<nn> (0: rank number <= 0)
    std::string_view rankName, raceClass; // the badge's and the name's tooltips
    u32 killingBlows = 0, honorableKills = 0, deaths = 0, bonusHonor = 0;
    std::array<i32, 7> stats{};
};
struct WorldStateScoreView {
    std::span<const ScoreColumnView> columns; // GetNumBattlefieldStats (0..7)
    std::span<const ScoreRowView> rows;       // GetNumBattlefieldScores of them
    u32 tab = 0;                              // PanelTemplates' selected tab: 0 All, 1 Alliance, 2 Horde
    std::string_view label;                   // WorldStateScoreFrameLabel: STAT_TEMPLATE with the tab's text
    i32 winner = -1;                          // GetBattlefieldWinner: the banner, Leave Battle and the timer
    std::string_view winnerText;              // VICTORY_TEXT<winner> (empty: none for a draw)
    std::string_view playerCount;             // PLAYER_COUNT_ALLIANCE / _HORDE (empty: hidden)
    std::string_view runTime;                 // TIME_ELAPSED and SecondsToTime(GetBattlefieldInstanceRunTime)
    std::string_view timer;                   // SecondsToTimeAbbrev(BATTLEFIELD_SHUTDOWN_TIMER)
};
// What the frame keeps between frames: FauxScrollFrame's offset (in rows), the drag being followed, the
// tooltip held.
struct WorldStateScoreState {
    u32 offset = 0;
    f32 dragRest = 0.0f;
};
struct WorldStateScoreResult {
    bool closed = false; // WorldStateScoreFrameCloseButton
    i32 tab = -1;        // a tab tapped (WorldStateScoreFrameTab_OnClick)
    bool leave = false;  // WorldStateScoreFrameLeaveButton: LeaveBattlefield()
    ui::Rect panel;
};
// WorldStateScoreFrame_Resize's width: 530 + 77 a column, + 37 with the scroll bar (more than 22 lines).
f32 worldStateScoreWidth(u32 columns, bool scrollBar);
// The frame (its art scaled to fit c.panel, or the safe area). Touch: a finger held on a header, a name or a
// rank badge shows its tooltip (the mouse over); the lines scroll by dragging them (FauxScrollFrame's wheel
// and bar).
WorldStateScoreResult worldStateScoreFrame(DialogContext& c, const WorldStateScoreView& v,
                                           WorldStateScoreState& s);

// --- Blizzard_BattlefieldMinimap
// ------------------------------------------------------------------------------

// A dot of the map (map coordinates 0..1, west to east and north to south).
struct BattlefieldMapDot {
    f32 u = 0.0f, v = 0.0f;
    std::string_view name;
};
struct BattlefieldMapFlag {
    f32 u = 0.0f, v = 0.0f;
    u64 texture = 0; // Interface\WorldStateFrame\<token>.blp
};
struct BattlefieldMinimapView {
    std::array<TextureHandle, 12> tiles{}; // Interface\WorldMap\<map>\<map>1..12 (streamed)
    TextureHandle partyIcon;               // Interface\WorldMap\WorldMapPartyIcon
    bool hasPlayer = false;
    f32 playerU = 0.0f, playerV = 0.0f;
    f32 facing = 0.0f;                        // radians, 0 = north, counter-clockwise
    std::span<const BattlefieldMapDot> party; // party or raid members (BattlefieldMinimapParty / Raid)
    std::span<const BattlefieldMapDot> team;  // GetBattlefieldPosition: teammates out of the group
    std::span<const BattlefieldMapFlag> flags;
    bool hasCorpse = false;
    f32 corpseU = 0.0f, corpseV = 0.0f;
    std::string_view playerName;
    // BattlefieldMinimapOptions.
    f32 opacity = 0.7f; // the slider's value: the map's alpha is 1 - opacity
    bool locked = true;
    bool showPlayers = true;
};
// What the frame keeps: where its tab is (the original's BattlefieldMinimapTab BOTTOMLEFT at UIParent's
// BOTTOMRIGHT (-225, BATTLEFIELD_TAB_OFFSET_Y); `placed`: moved by the player), the tab's fade, the menu and
// the opacity frame.
struct BattlefieldMinimapState {
    bool placed = false;
    f32 x = 0.0f, y = 0.0f; // the tab's top left (logical pixels) once placed
    u64 touchedMs = 0;      // the tab shows while the map is touched, and BATTLEFIELD_TAB_SHOW_DELAY... after
    bool menu = false;      // BattlefieldMinimapTabDropDown
    bool opacityFrame = false;
    bool dragging = false;
};
struct BattlefieldMinimapResult {
    bool closed = false;
    bool togglePlayers = false; // BattlefieldMinimap_TogglePlayers
    bool toggleLock = false;    // BattlefieldMinimap_ToggleLock
    bool opacityChanged = false;
    f32 opacity = 0.0f;
    bool moved = false; // the tab was dragged (its place is the player's now)
    ui::Rect map;       // where the map was drawn
    // A finger held on a dot: the names under it (BattlefieldMinimapUnit_OnEnter).
    bool tooltip = false;
};
// The default place (logical pixels) of the map's tab: BOTTOMLEFT at the screen's BOTTOMRIGHT -225 units,
// over the action bars (BATTLEFIELD_TAB_OFFSET_Y). Here the first of these that covers nothing of the touch
// HUD: right-aligned above the action buttons, under the minimap cluster, left of it under the top bars, left
// of the action buttons; the one that covers the least when none fits (the cover screen: the player may move
// it).
ui::Rect battlefieldMinimapDefault(const DialogContext& c, const WorldStateHudAvoid& avoid);
// Not modal: the map, its close button and its tab take fingers. Touch: a finger on the map shows the tab
// (the mouse over: MouseIsOver fades it in); a long press on the map or the tab opens the options menu (the
// tab's right click); dragging the tab moves the map unless it is locked.
BattlefieldMinimapResult battlefieldMinimap(DialogContext& c, const BattlefieldMinimapView& v,
                                            BattlefieldMinimapState& s, const WorldStateHudAvoid& avoid,
                                            u64 nowMs);

} // namespace vd
