#pragma once

#include "app/CharacterFrameLayout.h"
#include "app/DialogKit.h"
#include "app/Dialogs.h"
#include "app/InventoryWindows.h"
#include "app/ModelFrame.h"
#include "core/Types.h"
#include "game/ItemTooltip.h"
#include "render/Renderer.h"
#include "ui/Widgets.h"

#include <span>
#include <string_view>

namespace vd {

// The touch windows of the character window of phase 15 (CharacterFrame's tabs and Blizzard_InspectUI's
// InspectFrame), pure views like app/Dialogs: PaperDollFrame / InspectPaperDollFrame (the equipment around
// the 3D model, the attributes, melee and ranged blocks and the five resistances, each with 1.12's tooltip),
// HonorFrame / InspectHonorFrame, ReputationFrame with its detail frame, SkillFrame with its detail pane, and
// the ReputationWatchBar over the experience bar. Laid out from the FrameXML's anchors and sizes, scaled into
// the panel place on the square screens; on the wide screens' short panels the slots go two columns a side
// and the numbers beside the model scroll. Touch: a tap or a long press on a stat, a resistance, an honor
// line or a label shows its tooltip (the desktop's hover) until the next tap; a held reputation bar shows its
// numbers (its OnEnter).

// --- The frame
// -------------------------------------------------------------------------------------------------

// Character frame: the window's pages. The paper doll is tall (the original) or wide (not in 1.12) as its place
// suits; the others always take the tall frame with their own 1.12 art:
// ReputationFrame and PetPaperDollFrame the plain frame (UI-Character-General-*), SkillFrame its SkillFrame-Bot*,
// HonorFrame the plain frame with its UI-Character-Honor-* boxes.
enum class CharacterPage : u8 { PaperDoll, Pet, Reputation, Skills, Honor };

// What a page draws on: the frame's places (its at() for the FrameXML's units) and the art's sizes.
struct CharacterPageFrame {
    CharacterFrameLayout l;
    kit::QuestArtSizes s;
};

// --- Paper doll
// -------------------------------------------------------------------------------------------------

struct PaperDollStatView {
    std::string_view label;
    std::string_view value; // with colour codes
    std::span<const game::TooltipLine> tooltip;
};

struct PaperDollView {
    std::string_view title; // the window's title: UnitPVPName("player"), the inspected player's name
    std::string_view level; // "Level 5 Human Warrior"
    std::string_view guild; // GUILD_TITLE_TEMPLATE ("Member of X"); empty: none
    std::span<const std::string_view> tabs;
    u32 tab = 0;
    CharacterPage page = CharacterPage::PaperDoll; // retouches (character frame): the tab's page (its frame and art)
    TextureHandle portrait; // retouches (character frame): CharacterFramePortrait (SetPortraitTexture; invalid: none)
    std::span<const ItemSlotView> equipment; // 19 slots (head .. tabard), then the ammo slot (20)
    TextureHandle model;                     // app/ModelViews' image (invalid: loading)
    ModelFrameState* modelState = nullptr;   // the model frame's rotation (null: no model frame)
    // In game::PaperDollStat order: Strength .. RangedDamage, then the five resistances (empty:
    // InspectPaperDollFrame shows none).
    std::span<const PaperDollStatView> stats;
    bool ammoSlot = true; // hidden for InspectPaperDollFrame and the classes with a relic slot
    i32 statShown = -1;   // the stat whose tooltip shows (the caller keeps it from statTapped)
};

struct PaperDollResult {
    ItemWindowResult items; // the slots' taps and drops (InventoryWindows: zone 0 = equip), close, tabs
    i32 statTapped = -1;    // a stat or resistance tapped or held: show its tooltip (another tap hides it)
    bool tappedElsewhere = false; // a tap began on the window but on no stat: hide the tooltip
    bool rotated = false;         // a rotate button went down (UiSound::InventoryRotate)
    CharacterPageFrame frame;     // retouches (character frame): where the other pages draw
};
// The character window (CharacterFrame) with its tabs; tab 0 draws the paper doll, the others only the frame and the
// tabs (their panel goes in `body`). The 1.12 art, tall as the original or wide (app/CharacterFrameLayout),
// CharacterFrameTab1..5 turned along its right edge.
PaperDollResult paperDollWindow(DialogContext& c, const PaperDollView& v, ItemDrag& drag);

// --- Honor (HonorFrame / InspectHonorFrame)
// ---------------------------------------------------------------------

struct HonorView {
    std::string_view level; // HonorLevelText / HonorGuildText (empty: none)
    std::string_view guild;
    std::string_view rankTitle;  // the rank's name, or NONE
    std::string_view rankNumber; // "(Rank 3)"
    u64 rankIcon = 0;            // Interface\PvPRankBadges\PvPRank<nn> (0: none)
    f32 progress = 0.0f;         // the rank bar
    bool alliance = false;       // its colour: (0.05, 0.15, 0.36), the Horde's (0.63, 0.09, 0.09)
    // The values, as the frame writes them.
    u32 todayHonorable = 0, todayDishonorable = 0;
    u32 yesterdayHonorable = 0, yesterdayHonor = 0;
    u32 thisWeekHonorable = 0, thisWeekHonor = 0;
    u32 lastWeekHonorable = 0, lastWeekHonor = 0, lastWeekStanding = 0;
    u32 lifetimeHonorable = 0, lifetimeDishonorable = 0;
    std::string_view highestRank;
    bool waiting = false; // InspectHonorFrame before MSG_INSPECT_HONOR_STATS answers: the values are blank
};
// Retouches (character frame): HonorFrame's lines where HonorFrame.xml puts them on its boxes (the frame draws the
// art). `shownTip` is the tooltip kept from a previous tap (the caller's; -1 none): the function updates it.
void honorPanel(DialogContext& c, const CharacterPageFrame& f, const HonorView& v, i32& shownTip);

// --- Reputation (ReputationFrame)
// ---------------------------------------------------------------------------------

struct FactionRowView {
    bool header = false;
    bool collapsed = false;
    std::string_view name;
    std::string_view standing; // FACTION_STANDING_LABEL<n>
    u32 standingId = 4;        // 1..8: FACTION_BAR_COLORS
    i32 value = 0, max = 1;    // normalized to the rank: 0..max
    bool atWar = false;        // the crossed swords right of the bar
    bool watched = false;      // the check after the name
    bool selected = false;
};
struct FactionDetailView {
    bool shown = false;
    std::string_view name;
    std::string_view description;
    bool atWar = false;
    bool canToggleAtWar = false;
    bool inactive = false;
    bool watched = false;
};
struct ReputationPanelResult {
    i32 headerTapped = -1; // collapse or expand it
    i32 rowTapped = -1; // ReputationBar_OnClick: select it and show the detail, or hide it when it is shown
    bool closeDetail = false;
    bool toggleAtWar = false;
    bool toggleInactive = false;
    bool toggleWatched = false;
};
// Retouches (character frame): ReputationFrame's list where ReputationFrame.xml puts it (the rows a finger's height
// at least: they scroll), and ReputationDetailFrame at the frame's right. `shownTip`: as honorPanel's.
ReputationPanelResult reputationPanel(DialogContext& c, const CharacterPageFrame& f,
                                      std::span<const FactionRowView> rows, const FactionDetailView& detail,
                                      i32& shownTip);

// --- Skills (SkillFrame)
// ------------------------------------------------------------------------------------------

struct SkillLineView {
    bool header = false;
    bool expanded = true;
    std::string_view name;
    std::string_view rank;    // "52/75", "62 (|cff20ff20+5|r)/77"; empty for proficiencies
    f32 fill = 0.0f;          // the bar: rank / max
    bool proficiency = false; // maximum 1: a full grey bar without numbers
    bool selected = false;
};
struct SkillDetailView {
    bool shown = false;
    std::string_view name;
    std::string_view rank;
    f32 fill = 0.0f;
    bool proficiency = false;
    bool abandonable = false;     // the Unlearn button (CancelButton art) right of the bar
    std::string_view description; // SKILL_DESCRIPTION
};
struct SkillPanelResult {
    i32 headerTapped = -1;
    bool allTapped = false; // SkillFrameCollapseAllButton
    i32 rowTapped = -1;     // SkillBar_OnClick: select it
    bool unlearn = false;   // the Unlearn button: UNLEARN_SKILL asks first
    bool close = false;     // retouches (character frame): SkillFrameCancelButton (CLOSE): the window closes
};
// Retouches (character frame): SkillFrame where SkillFrame.xml puts it: the "All" tab, the list (a finger's height at
// least: it scrolls), the divider, the selected skill's detail and Close.
SkillPanelResult skillPanel(DialogContext& c, const CharacterPageFrame& f, std::span<const SkillLineView> rows,
                            bool allExpanded, const SkillDetailView& detail, i32& shownTip);

// --- ReputationWatchBar
// -------------------------------------------------------------------------------------------

struct ReputationWatchView {
    bool shown = false;
    bool maxLevel =
        false; // at MAX_PLAYER_LEVEL it replaces the experience bar (13 units), else sits on it (8)
    f32 fill = 0.0f;
    u32 standingId = 4;
    std::string_view text; // "<name> <value> / <max>"
    bool showText =
        false; // ReputationWatchStatusBarText shown (hover, the character window at the top level)
};
// Over the experience bar `xpBar` (app/Hud::experienceBarRect). Returns true when tapped (its OnEnter: the
// text).
bool reputationWatchBar(ui::Gui& g, const ui::Rect& xpBar, const ReputationWatchView& v);

} // namespace vd
