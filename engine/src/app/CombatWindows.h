#pragma once

#include "app/Dialogs.h"
#include "app/InventoryWindows.h"
#include "core/Types.h"
#include "ui/Context.h"
#include "ui/DrawList.h"
#include "ui/Widgets.h"

#include <array>
#include <span>
#include <string_view>

namespace vd {

// The phase-8 windows of the character's abilities (spell book, trainer, talents, skills, reputation, combat
// log, action slot editor) and the pet's HUD pieces, drawn with app/DialogKit and ui/Widgets over the
// client's art. Pure views like app/Dialogs: a view-model in, what the player did out; no game state, no
// network. Spell and unit names are client / server data; the interface's words come from core/Text (the
// client's keys: SPELLBOOK, TRAIN, AT_WAR...; ours: VD_COMBAT_*).

// --- Spell tooltip (any spell: book, trainer, talents, action buttons) -------------------------------------

struct SpellTooltipView {
    u64 iconHash = 0;
    std::string_view name;
    std::string_view rank;        // "Rank 2", "Passive"...
    std::string_view cost;        // "30 Mana" (empty: none)
    std::string_view range;       // "30 yd range"
    std::string_view castTime;    // "1.5 sec cast"
    std::string_view cooldown;    // "8 sec cooldown"
    std::string_view requirement; // red ("Requires: Level 12"), empty when met
    std::string_view description; // resolved
    std::string_view nextTitle;   // talents: "Next rank:" (empty: no second part)
    std::string_view nextDescription;
};
// The tooltip's lines inside `r` (from its top, wrapped to its width). Returns the height used.
f32 spellTooltip(DialogContext& c, ui::Gui& g, const ui::Rect& r, const SpellTooltipView& v);
// Parity pass: the same as GameTooltip:SetSpell's lines (the name and its rank in gray at the right, cost and range,
// cast time and cooldown, the requirement in red, the description in gold) for the item tooltip's view
// (app/InventoryWindows::itemTooltip), which shows a held action button's tooltip beside it with its "Edit"
// action. Returns how many lines were written.
u32 spellTooltipLines(const SpellTooltipView& v, std::span<TooltipLineView> out);

// --- Spell book --------------------------------------------------------------------------------------------

struct SpellbookTabView {
    std::string_view name;
    u64 iconHash = 0;
    f32 flash = 0.0f; // phase 18 (extras): its new-spell glow now (app/SpellBookFlash), 0 none
};
struct SpellbookEntryView {
    u64 iconHash = 0;
    std::string_view name;
    std::string_view rank;
    bool passive = false;
    f32 cooldown = 0.0f; // remaining share, 0..1
    bool usable = true;
    // Phase 15 (pets): the pet's page (GetSpellAutocast): UI-AutoCastableOverlay, and the autocast's sparkles when on.
    bool autocastable = false;
    bool autocast = false;
};
struct SpellbookView {
    std::span<const SpellbookTabView> tabs;
    u32 tab = 0;
    std::span<const SpellbookEntryView> entries; // of the tab shown
    i32 selected = -1;                           // its tooltip and buttons
    SpellTooltipView tooltip;
    bool allRanks = false;
    bool picking = false; // choosing a spell for an action button: "Choose" instead of the bar / cast buttons
    // Phase 15 (pets): SpellBookFrameTabButton1..2 at the bottom ("Spellbook", "Pet" / "Demon") while the pet has
    // spells (HasPetSpells); `petBook`: the pet's page shows (no skill line tabs, no rank toggle; a long press on a
    // spell toggles its autocast).
    std::span<const std::string_view> books;
    u32 book = 0;
    bool petBook = false;
    std::string_view title; // empty: SPELLBOOK (the pet's page: PET_TYPE_PET / PET_TYPE_DEMON)
};
struct SpellbookResult {
    i32 tab = -1;       // tab tapped
    i32 select = -1;    // entry tapped
    bool place = false; // "Put on Bar" (or "Choose" while picking)
    bool cast = false;  // "Cast"
    bool toggleRanks = false;
    bool closed = false;
    i32 book = -1;     // phase 15 (pets): a book tab tapped
    i32 autocast = -1; // phase 15 (pets): an entry held long (the right click: ToggleSpellAutocast)
};
SpellbookResult spellbookWindow(DialogContext& c, const SpellbookView& v);

// --- Trainer -----------------------------------------------------------------------------------------------

enum class TrainerServiceState : u8 { Available, Unavailable, Known };
struct TrainerServiceView {
    u64 iconHash = 0;
    std::string_view name;
    std::string_view rank;
    u32 cost = 0;
    TrainerServiceState state = TrainerServiceState::Available;
};
struct TrainerView {
    std::string_view npcName;
    std::string_view greeting;
    std::span<const TrainerServiceView> services; // already filtered
    i32 selected = -1;
    SpellTooltipView tooltip; // of the selected service, with its requirements
    u32 money = 0;            // the player's
    bool showAvailable = true, showUnavailable = true, showKnown = false;
    bool canLearn = false; // the selected one is available and affordable
};
struct TrainerResult {
    i32 select = -1;
    bool learn = false;
    bool toggleAvailable = false, toggleUnavailable = false, toggleKnown = false;
    bool closed = false;
};
TrainerResult trainerWindow(DialogContext& c, const TrainerView& v);

// --- Talents -----------------------------------------------------------------------------------------------

struct TalentButtonView {
    u64 iconHash = 0;
    u32 row = 0, column = 0;
    u32 rank = 0, maxRank = 1;
    bool available = false; // a point could go there now (tree points, prerequisite, free points)
    i32 prerequisite = -1;  // index of the talent it needs (an arrow from it), -1 none
    bool prerequisiteMet = true;
};
struct TalentTreeView {
    std::string_view name;
    std::string_view
        background; // TalentTab background ("MageFire"): Interface\TalentFrame\<it>-TopLeft.blp...
    u32 points = 0;
    std::span<const TalentButtonView> talents;
};
struct TalentView {
    std::span<const TalentTreeView> trees;
    u32 tree = 0;
    i32 selected = -1;
    SpellTooltipView tooltip; // this rank (or the first) and the next
    u32 freePoints = 0;
    bool canLearn = false;
    std::string_view notice; // "Talents can be learned from level 10..." (empty: none)
};
struct TalentResult {
    i32 tree = -1;
    i32 select = -1;
    bool learn = false;
    bool closed = false;
};
TalentResult talentWindow(DialogContext& c, const TalentView& v);

// --- Pet (HUD) ---------------------------------------------------------------------------------------------

struct PetFrameView {
    std::string_view name;
    u32 level = 0;
    f32 health = 0.0f; // 0..1
    f32 power = 0.0f;
    u32 powerType = 0;     // 0 mana, 2 focus...
    i32 happiness = -1;    // hunters: 1 unhappy, 2 content, 3 happy (-1: none)
    bool selected = false; // the player's target
    TextureHandle portrait; // parity: the pet's portrait (app/Portraits); invalid: TemporaryPortrait-Monster
};
struct PetFrameResult {
    bool tapped = false; // target the pet
    bool held = false;   // a long press (dismiss / abandon)
};
// The small frame (UI-SmallTargetingFrame) at `area`.
PetFrameResult petFrame(ui::Gui& g, const ui::Rect& area, const PetFrameView& v);

struct PetSlotView {
    u64 iconHash = 0;          // 0: empty
    bool active = false;       // the current command / reaction
    bool autocast = false;     // on
    bool autocastable = false; // a spell that can be autocast (long press toggles it)
    f32 cooldown = 0.0f;       // remaining share 0..1
    bool usable = true;
    bool target = false;       // phase 15 (pets): a pet spell being placed may go here (lit)
};
struct PetBarResult {
    i32 tapped = -1;
    i32 held = -1; // long press (autocast)
    // Phase 15 (pets): a finger dragged from one button to another (PickupPetAction twice: they change places).
    i32 dragFrom = -1;
    i32 dragTo = -1;
};
// The 10 buttons of the pet bar in a row at `area` (its height is the button size).
PetBarResult petBar(ui::Gui& g, UiResources& resources, const ui::Rect& area,
                    std::span<const PetSlotView> slots);

// --- Action slot editor ------------------------------------------------------------------------------------

struct ActionEditView {
    u64 iconHash = 0; // what the slot holds (0: empty)
    std::string_view name;
    u32 page = 0;             // 1-based, for the title
    u32 position = 0;         // 1..12
    SpellTooltipView tooltip; // a spell's: what the button does (a long press is the touch screen's hover)
};
struct ActionEditResult {
    bool pickSpell = false;
    bool pickItem = false;
    bool clear = false;
    bool closed = false;
};
ActionEditResult actionEditDialog(DialogContext& c, const ActionEditView& v);
// A list of items to put on a slot. Returns the one tapped (-1 none); `closed` when dismissed.
i32 itemPicker(DialogContext& c, std::span<const ItemView> items, bool& closed);

// --- Combat log --------------------------------------------------------------------------------------------

struct CombatLogLineView {
    std::string_view text;
    u32 color = ui::kWhite;
};
struct CombatLogResult {
    bool closed = false;
    bool toggleOnlyMine = false;
    bool clear = false;
};
CombatLogResult combatLogWindow(DialogContext& c, std::span<const CombatLogLineView> lines, bool onlyMine);

} // namespace vd
