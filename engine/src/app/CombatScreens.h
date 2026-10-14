#pragma once

#include "app/CombatWindows.h"
#include "app/Dialogs.h"
#include "app/Hud.h"
#include "app/InventoryWindows.h"
#include "app/SpellBookFlash.h" // phase 18 (extras)
#include "app/UiSound.h"
#include "core/Types.h"
#include "game/AbilityData.h"
#include "game/ActionBar.h"
#include "game/CombatText.h"
#include "game/GameData.h"
#include "game/ItemData.h"
#include "game/ItemTooltip.h"
#include "game/PetData.h"
#include "game/Progress.h"
#include "game/SpellBookTabs.h"
#include "game/Spells.h"
#include "game/UnitInfo.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vd {

class GameSession;
class MacroScreens;
class Portraits;
class PopupScreens;
class UiResources;

// The player's windows of this module (the micro menu opens them; phase 15: Skills and Reputation are the character
// window's tabs, app/CharacterScreens').
enum class CombatWindow : u8 { None, Spellbook, Talents, Skills, Reputation };

// The game's side of the phase-8 ability windows and HUD pieces (the view-model glue, like GameScreens): the
// action bar's pages and edit mode, the spell book, the trainer, talents, the pet frame and bar, combo
// points, the target's cast bar and target, skills, reputation and the combat log's lines (the chat's Combat Log
// tab shows them: app/SocialScreens). Reads game::Gameplay (and
// its game::Abilities), fills the views of app/CombatWindows and applies what the player did. GameScreens
// owns it and calls it at fixed points of its frame. Main thread; storage reserved once (no allocation per
// frame once warmed up).
class CombatScreens {
public:
    // Parity pass (audio): where the windows' own sounds go (the spellbook's page turn).
    void setUiSoundSink(UiSoundSink sink) { m_uiSound = sink; }
    // Where the messages of this module go (GameScreens' message lines).
    using MessageSink = void (*)(void* context, u32 color, bool big, const char* text);

    void init(const game::GameData* data, const game::AbilityData* abilities, UiResources* resources,
              MessageSink sink, void* sinkContext);
    // Phase 13 (combat): the enchantments' names in the combat log (SpellItemEnchantment.dbc).
    void setItemData(const game::ItemData* items) { m_items = items; }
    // Phase 15 (pets): the pet's page title (ChrClasses.dbc's petNameToken: PET_TYPE_PET / PET_TYPE_DEMON).
    void setPetData(const game::PetData* pets) { m_petData = pets; }
    void open(CombatWindow window);
    // Parity pass (panels): several may be open at once (the spell book beside the talents).
    void close(CombatWindow window);
    bool isOpen(CombatWindow window) const { return (m_windows & windowBit(window)) != 0; }

    // --- Before the HUD draws ---
    // The action bar page (12 buttons, ActionButton.lua's six pages and the form's bonus bar), its page button,
    // the placing highlight; the bottom left bar (when hud.showExtraBar) and the stance bar.
    void prepareActionBar(GameSession& session, HudFrame& hud, u32 nowMs);

    // --- After the HUD drew (GameScreens::drawGame) ---
    // The action bars' taps, long presses (the button's tooltip, or the editor for an empty one) and pages; the
    // stance bar's forms. Returns the unit a spell or attack was aimed at (the player turns to it), 0 none.
    u64 actOnActionBar(GameSession& session, const HudOutput& out);
    // Events, the combat log, the trainer's reach: once per frame.
    void update(GameSession& session, u32 nowMs);
    // Not modal, over the HUD: the pet frame and bar, combo points, the target's cast bar and target, the
    // placing hint.
    void drawHud(GameSession& session, Hud& hud, DialogContext& c, u32 nowMs);
    // Bottom of the pet frame drawn this frame (logical pixels), 0 without a pet: the party frames go under
    // it.
    f32 petFrameBottom() const { return m_petFrameBottom; }
    // Phase 13 (social): the pet frame was held this frame (its menu: app/UnitMenuScreens). Read once.
    bool takePetFrameHold() { return std::exchange(m_petFrameHeld, false); }
    // Parity (auras): the pet frame drawn this frame (empty without a pet): its buffs go on it.
    const ui::Rect& petFrameRect() const { return m_petFrameRect; }
    // Parity: where the pet and target-of-target frames get their portraits (null: the stand-in art).
    void setPortraits(Portraits* portraits) { m_portraits = portraits; }
    // Parity (popups): where USE_BIND and ABANDON_PET are asked (null: not asked).
    void setPopups(PopupScreens* popups) { m_popups = popups; }
    // Phase 15 (options): the macros the action buttons may hold (app/MacroScreens), a button picked up elsewhere for
    // the next tap on an action button (PickupMacro), and LOCK_ACTIONBAR.
    void setMacros(MacroScreens* macros) { m_macros = macros; }
    void placeOnBar(game::ActionButton button) {
        m_placing = button;
        m_tipKind = TipKind::None;
    }
    void setLockActionBars(bool locked) { m_lockActionBars = locked; }
    // SHOW_TARGET_OF_TARGET and its state as they stand now (OptionsScreens::targetOfTargetShown).
    void setTargetOfTargetShown(bool shown) { m_targetOfTargetShown = shown; }
    // The extra bars the HUD drew (Hud::drawnMultiBars, bits 0..3): their pages leave the main bar's paging.
    void setDrawnMultiBars(u8 bars) {
        m_hiddenPages = 0;
        for (u32 b = 0; b < kMultiBars; ++b) {
            m_hiddenPages |= (bars & (1u << b)) != 0 ? static_cast<u8>(1u << (kMultiBarPages[b] - 1)) : 0;
        }
    }
    // Phase 20 (app/TouchBarScreens): the touch cluster's actions are on no server slot. The state of `button` as an
    // action button would show it this frame (after prepareActionBar; cache `cacheIndex` < kTouchViews keeps its
    // Spell.dbc lookup), and using it as a tap on such a button does (a macro runs, an item is used, Attack toggles,
    // Auto Shot and Shoot start or stop, a spell is cast at spellTarget()). Returns the unit aimed at (0: none).
    static constexpr u32 kTouchViews = 140; // phase 20: the drawer's 72 from 40 on, the reactive fan's 8 from 112
    static constexpr u32 kTouchPetViews = 120; // phase 20 (pets): the pet button, its fan and panel: 20 from 120
    HudActionSlot touchView(GameSession& session, game::ActionButton button, u32 cacheIndex, u32 nowMs);
    u64 useButton(GameSession& session, game::ActionButton button);
    // The pet bar's slot `slot` as a button of the cluster (section 6.8: Spell Lock, Seduction...), and using it on the
    // target. Phase 20 (pets): `slot` is a PetAction's id (game/TouchPet.h: a slot, or a pet spell, command or reaction
    // whatever its slot), its view with the pet's cooldown in seconds, its power and the range from the pet.
    HudActionSlot petActionView(GameSession& session, u32 slot, u32 nowMs);
    void usePetAction(GameSession& session, u32 slot);
    // The stance bar's forms (the touch cluster's class bar): their views this frame (after prepareActionBar), and a
    // tap on one (CastShapeshiftForm).
    std::span<const HudActionSlot> forms() const { return {m_stances.data(), m_formCount}; }
    void useForm(GameSession& session, u32 index);
    // phase 20 (smart): public API here
    // Where the windows' sounds go (the touch cluster's reactive ability plays igBonusBarOpen through it).
    const UiSoundSink& uiSoundSink() const { return m_uiSound; }
    // phase 20 (edit): public API here
    // The spell (or macro, item) being put on a bar from the spell book (empty: none); stopPlacing when it went to a
    // slot of the touch cluster instead (TouchEditScreens).
    const game::ActionButton& placing() const { return m_placing; }
    void stopPlacing() { m_placing = {}; }
    // The macros the touch layout's picker lists.
    MacroScreens* macros() const { return m_macros; }
    // The drawer (the six pages of the 1.12 bars): a tap on server slot `slot` (the spell being placed goes there, else
    // it is used), a long press (its tooltip with "Edit", or the editor when empty) whose tooltip goes over the touch
    // windows (drawTouchTooltip).
    u64 useActionSlot(GameSession& session, u32 slot) { return useSlot(session, slot); }
    void holdActionSlot(GameSession& session, u32 slot, const ui::Rect& anchor);
    // A touch cluster button held: `button`'s tooltip (GameTooltip:SetAction) with one action under it (`action`; empty:
    // none); takeTouchTooltipAction() is true once it was tapped. Drawn by drawTouchTooltip, after the touch windows.
    void showTouchTooltip(game::ActionButton button, const ui::Rect& anchor, std::string_view action);
    bool takeTouchTooltipAction() { return std::exchange(m_touchTipAction, false); }
    void drawTouchTooltip(GameSession& session, DialogContext& c);
    // phase 20 (classes): public API here
    // Phase 20 (assists): the unit whose pet frame or target-of-target frame is under (x, y) as last drawn (0: none);
    // `frame` its rectangle. A helpful spell dragged from the touch cluster lands on it.
    u64 unitAt(f32 x, f32 y, ui::Rect& frame) const;
    // phase 20 (pets): public API here
    // The 1.12 pet bar (PetActionBarFrame) under the touch cluster: an option, off by default (the classic bars: on).
    void setShowPetBar(bool shown) { m_showPetBar = shown; }
    // What the touch cluster's pet button reuses: the pet's portrait, the pet tables.
    Portraits* portraits() const { return m_portraits; }
    const game::PetData* petData() const { return m_petData; }
    // The pet's menu as the pet frame's (Dismiss; a hunter's: Abandon, with ABANDON_PET).
    void askPetMenu() { m_modal = Modal::PetMenu; }
    // A modal step of this module is up (the action editor and its pickers). Parity pass: the windows are panels
    // (drawPanel / drawTrainer, placed by app/UiPanels), the talent reset offer a popup (app/PopupScreens).
    bool hasWindow(const GameSession& session) const;
    // Phase 18 (extras): Escape closes that step (the action editor, its pickers, a trainer's question). True when one
    // was up.
    bool escapeWindow() { return std::exchange(m_modal, Modal::None) != Modal::None; }
    void drawWindow(GameSession& session, DialogContext& c, u32 nowMs);
    // Parity pass (panels): the spell book or the talents in the place of `c` (c.panel); the trainer's window (open
    // while the server keeps it: trainerOpen).
    void drawPanel(GameSession& session, DialogContext& c, u32 nowMs, CombatWindow window);
    bool trainerOpen(const GameSession& session) const;
    void drawTrainer(GameSession& session, DialogContext& c, u32 nowMs);
    // Parity: a line the client writes itself for the combat log (experience, reputation, money:
    // game/Notices), and the log's names of units and spells.
    void logLine(std::string_view text, u32 color) { log(text, color, true); }
    game::CombatNames logNames(GameSession& session) { return names(session); }
    // Parity: the log as the chat's Combat Log tab (ChatFrame2) reads it: its lines oldest first, with when each
    // came in and whether it is about the player or the pet (the tab's filter); every line ever logged (a tab
    // scrolled back keeps its place while new ones come in).
    struct LogEntry {
        std::string_view text;
        u32 color = 0;
        u32 timeMs = 0;
        bool mine = false;
    };
    u32 logSize() const { return m_logCount; }
    LogEntry logEntry(u32 index) const;
    u32 logTotal() const { return m_logTotal; }
    bool logOnlyMine() const { return m_logOnlyMine; }
    void setLogOnlyMine(bool onlyMine) { m_logOnlyMine = onlyMine; }
    void clearLog() { m_logCount = 0; }

private:
    enum class Modal : u8 { None, ActionEdit, ItemPicker, PetMenu, AbandonPet, PickSpell, ConfirmProfession };
    struct LogLine {
        std::array<char, 200> text{};
        u32 color = 0;
        u32 timeMs = 0;
        bool mine = false;
    };
    // What the combat log's name callbacks reach.
    struct NameSource {
        GameSession* session = nullptr;
        const game::AbilityData* data = nullptr;
        const game::ItemData* items = nullptr; // phase 13
    };
    struct Tooltip {
        u32 spellId = 0, nextSpellId = 0, level = 0;
        u32 mods = 0; // phase 13 (spells): the spell modifiers' version it was built with
        std::array<char, 48> rank{}, cost{}, range{}, cast{}, cooldown{};
        std::array<char, 256> requirement{};
        std::string description, next;
    };

    void reset();
    void message(u32 color, bool big, std::string_view line);
    // VD_TRACE_GAMEPLAY: what the player asked for through these screens, in the log.
    void trace(const char* format, ...) const;
    void log(std::string_view what, u32 color, bool mine);
    void readEvents(GameSession& session);
    void readCombat(GameSession& session);
    void watchAuras(GameSession& session);
    game::CombatNames names(GameSession& session);

    // The main bar's slot of button `button` (0..11) on the page shown, with the form's bonus bar.
    u32 mainSlot(const game::Entity& me, u32 button) const;
    // A button's spell, looked up when the button changes (Spell.dbc reads stay out of the frame loop).
    struct SlotSpell {
        u32 packed = 0xFFFFFFFFu; // the button (ActionButton::packed) it was looked up for
        bool hasInfo = false;
        bool hasDetails = false;
        game::SpellInfo info;
        game::SpellDetails details;
        bool hasNeeds = false; // phase 20 (classes): its reagents, totems, target type (Spell.dbc)
        game::SpellRequirements needs;
    };
    // What every button of a frame shares: the player, the target and whether a helpful spell lands on it.
    struct BarContext {
        const game::Entity* me = nullptr;
        const game::Entity* target = nullptr;
        u64 targetGuid = 0;
        bool targetFriendly = false;
        u32 comboPoints = 0;
        const game::SpellNeedsState* needs = nullptr; // phase 20 (classes): worn items, carried items, target type
    };
    const SlotSpell& slotSpell(SlotSpell& cache, game::ActionButton button) const;
    HudActionSlot actionSlot(GameSession& session, const BarContext& bar, u32 slot, SlotSpell& cache, u32 nowMs);
    // A spell's or an item's button (phase 15: a macro's spell too).
    HudActionSlot buttonView(GameSession& session, const BarContext& bar, game::ActionButton button, SlotSpell& cache,
                             u32 nowMs);
    // The stance bar's buttons (rebuilt from the known spells when they change).
    void prepareStances(GameSession& session, const game::Entity& me, HudFrame& hud, u32 nowMs);
    // A tap on an action slot: places the spell being placed, uses an item, toggles the auto attack or an
    // auto-repeat spell, casts. Returns the unit aimed at.
    u64 useSlot(GameSession& session, u32 slot);
    // A long press on an action slot: its tooltip, or the editor when empty.
    void holdSlot(GameSession& session, u32 slot, u32 page, const ui::Rect& anchor);
    // The tooltip of the held button (GameTooltip:SetAction / SetShapeshift) with its "Edit" action.
    void actionTooltip(GameSession& session, DialogContext& c);
    // The tooltip of `spellId` (and `nextSpellId` as its next rank): rebuilt when they change.
    const SpellTooltipView& tooltip(GameSession& session, Tooltip& t, u32 spellId, u32 nextSpellId,
                                    SpellTooltipView& view);

    void spellbook(GameSession& session, DialogContext& c);
    void petSpellbook(GameSession& session, DialogContext& c, std::string_view title); // phase 15 (pets)
    void drawPetPlacing(Hud& hud, DialogContext& c);                                 // phase 15 (pets)
    void trainer(GameSession& session, DialogContext& c);
    void talents(GameSession& session, DialogContext& c);
    static u8 windowBit(CombatWindow window) { return static_cast<u8>(1u << static_cast<u32>(window)); }
    void actionEdit(GameSession& session, DialogContext& c);
    void pickItem(GameSession& session, DialogContext& c);
    void petHud(GameSession& session, Hud& hud, DialogContext& c);
    void targetHud(GameSession& session, Hud& hud, DialogContext& c, u32 nowMs);

    const game::GameData* m_data = nullptr;
    const game::AbilityData* m_abilities = nullptr;
    const game::ItemData* m_items = nullptr; // phase 13 (combat)
    const game::PetData* m_petData = nullptr; // phase 15 (pets)
    UiResources* m_resources = nullptr;
    MessageSink m_sink = nullptr;
    void* m_sinkContext = nullptr;
    u32 m_nowMs = 0;
    f32 m_petFrameBottom = 0.0f;
    bool m_petFrameHeld = false; // phase 13 (social)
    ui::Rect m_petFrameRect;
    ui::Rect m_totRect; // phase 20 (assists): the target of target's frame drawn this frame, and whose
    u64 m_totGuid = 0;
    u64 m_petFrameGuid = 0;
    bool m_trace = false;
    u64 m_player = 0;
    u32 m_eventCursor = 0;
    u32 m_combatCursor = 0;

    // Windows.
    u8 m_windows = 0; // parity pass: the open ones (a bit per CombatWindow)
    Modal m_modal = Modal::None;

    // Action bar (parity pass: 12 buttons a page, ActionButton.lua's six pages; the bottom left bar; the stance
    // bar; the held button's tooltip).
    static constexpr u32 kPages = game::kActionBarPages;
    std::array<HudActionSlot, game::kActionBarButtons> m_actions{};
    // Phase 15 (options): the four MultiActionBars and their pages (BOTTOMLEFT_ACTIONBAR_PAGE 6, BOTTOMRIGHT 5,
    // RIGHT 3, LEFT 4); the pages the bars drawn last frame show are not the main bar's (VIEWABLE_ACTION_BAR_PAGES).
    static constexpr u32 kMultiBars = 4;
    static constexpr u32 kMultiBarPages[kMultiBars] = {6, 5, 3, 4};
    std::array<std::array<HudActionSlot, game::kActionBarButtons>, kMultiBars> m_extraActions{};
    u8 m_hiddenPages = 0; // bit p for page p + 1
    std::array<HudActionSlot, game::kMaxShapeshiftForms> m_stances{};
    std::array<game::ShapeshiftForm, game::kMaxShapeshiftForms> m_forms{};
    u32 m_formCount = 0;
    u32 m_formsVersion = 0;
    std::array<SlotSpell, (1 + kMultiBars) * game::kActionBarButtons + game::kMaxShapeshiftForms> m_slotSpells{}; // main, extra bars, forms
    std::array<SlotSpell, kTouchViews> m_touchSpells{}; // phase 20: the touch cluster's buttons
    BarContext m_bar; // phase 20: this frame's (prepareActionBar), for touchView
    // Phase 20 (classes, docs/MOBILE_CONTROLS.md §10.3): what the buttons' spells need besides power, state and form
    // (game::spellNeedMissing): the worn items (read again when the inventory or a query answer changes), the carried
    // ones (counted when the bags change) and the target's creature type.
    void prepareNeeds(GameSession& session, BarContext& bar);
    std::array<game::WornItem, game::kWornSlots> m_worn{};
    u32 m_wornInventory = 0;
    u32 m_wornQueries = 0;
    game::CarriedCounts m_carried;
    const game::Inventory* m_carriedInventory = nullptr;
    game::SpellNeedsState m_needs;
    u32 m_page = 0;
    u32 m_pages = kPages;
    game::ActionButton m_placing; // being put on the bar (empty: none)
    u32 m_editSlot = 0;           // the action button being edited
    u32 m_editPage = 1;           // its page as the editor's title says it (1-based)
    enum class TipKind : u8 { None, Action, Form, Button }; // phase 20 (edit): Button, a touch cluster's
    TipKind m_tipKind = TipKind::None;
    game::ActionButton m_tipButton; // phase 20 (edit): TipKind::Button's
    std::string_view m_tipAction;   // ...its action ("Edit", "Fixed Rank")
    bool m_touchTipAction = false;  // ...tapped
    bool m_tipOverTouch = false;    // drawn by drawTouchTooltip (over the touch windows), not drawHud
    u32 m_tipSlot = 0;  // the action slot (0..119), or the form's index on the stance bar
    u32 m_tipPage = 1;
    ui::Rect m_tipAnchor;
    u64 m_tipKey = 0;   // what the lines were built for
    Tooltip m_actionTip;
    game::ItemTooltip m_itemTip;
    std::array<TooltipLineView, game::ItemTooltip::kMaxLines + 2> m_tipLines{};
    u32 m_tipLineCount = 0;
    std::array<char, 64> m_tipCooldown{};
    std::vector<game::InventoryItem> m_usableItems;
    std::vector<ItemView> m_itemViews;

    // Spell book.
    game::SpellBookTabs m_book;
    u32 m_bookType = 0;   // phase 15 (pets): 0 BOOKTYPE_SPELL, 1 BOOKTYPE_PET
    u32 m_placingPet = 0; // phase 15 (pets): a pet spell being put on the pet bar (0: none)
    std::array<std::string_view, 2> m_bookNames{};
    bool m_allRanks = false;
    u32 m_bookTab = 0;
    i32 m_bookSelected = -1;
    std::vector<SpellbookTabView> m_tabViews;
    std::vector<SpellbookEntryView> m_entryViews;
    Tooltip m_bookTip;
    // Phase 18 (extras): the new-spell flash of the tabs (SpellBookFrame.lua), the book as last shown.
    SpellBookFlash m_bookFlash;
    bool m_bookShown = false;
    u32 m_bookTypeShown = 0;
    void learnedSpellInTab(GameSession& session, u32 spellId); // LEARNED_SPELL_IN_TAB
    void followBook(u32 nowMs);                                // SpellBookFrame_OnShow / OnHide

    // Trainer.
    u64 m_trainerSeen = 0;
    i32 m_trainerSelected = -1;
    u32 m_confirmService = 0; // a primary profession's first rank, asked before buying
    bool m_showAvailable = true, m_showUnavailable = true, m_showKnown = false;
    std::vector<TrainerServiceView> m_serviceViews;
    std::vector<u32> m_serviceIndex; // view -> index in the trainer's services
    Tooltip m_trainerTip;

    // Talents.
    u32 m_talentTree = 0;
    i32 m_talentSelected = -1;
    std::array<game::TalentTabInfo, 4> m_trees{};
    std::array<TalentTreeView, 3> m_treeViews{};
    std::vector<TalentButtonView> m_talentViews;
    std::vector<const game::TalentInfo*> m_talentInfos;
    Tooltip m_talentTip;
    std::array<char, 32> m_talentRank{};
    std::array<char, 160> m_talentNotice{};

    // Pet HUD.
    std::array<PetSlotView, 10> m_petSlots{};
    std::array<u32, 10> m_petSlotIndex{}; // shown button -> bar slot
    std::string m_petName;
    Portraits* m_portraits = nullptr;

    // Combat log.
    std::array<LogLine, 256> m_log{};
    u32 m_logNext = 0, m_logCount = 0;
    u32 m_logTotal = 0;
    bool m_logOnlyMine = true;
    game::AuraWatch m_auraWatch;
    NameSource m_nameSource;
    UiSoundSink m_uiSound;
    PopupScreens* m_popups = nullptr; // parity (popups)
    MacroScreens* m_macros = nullptr; // phase 15 (options)
    bool m_lockActionBars = false;
    bool m_targetOfTargetShown = true;
    bool m_showPetBar = true; // phase 20 (pets)
};

} // namespace vd
