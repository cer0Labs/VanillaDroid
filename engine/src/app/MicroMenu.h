#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"
#include "ui/DrawList.h"

#include <array>

namespace vd {

// Parity pass (micro menu): MainMenuBarMicroButtons and MainMenuBarBagButtons, the right end of the
// original's main bar: one tap per window with the client's art (UI-MicroButton-*), the pushed look while its
// window is open, the talents button from level 10 and pulsing while talent points wait (SetButtonPulse: the
// highlight on and off each second), the backpack, the four bag slots and the keyring.
//
// The original's main bar is the bottom edge; here the action buttons and the movement stick take it. Phase 20: on
// every screen the buttons fold into a gear on the minimap's rim, and a tap on it unfolds them in a row left of it, at
// the original's size for the HUD's unit and without the performance bar; a tap on one opens its window and folds the
// row again (a finger anywhere else too). It goes over the windows while it is open. The bag bar unfolds the same way
// from a long press on the backpack button on the minimap's rim (app/InventoryWindows), whose tap opens every bag: each
// bag on its own, the keyring, a bag dropped on its slot. No mouse-over: a long press on a button shows its tooltip
// (the name and the newbie tip). A pure view: what to show in, what was tapped out (app/GameScreens applies it).

// Phase 15 (help): HelpMicroButton, the last of the 1.12 bar (ToggleHelpFrame).
enum class MicroButton : u8 { Character, Spellbook, Talents, QuestLog, Social, WorldMap, MainMenu, Help, Count };
constexpr u32 kMicroButtons = static_cast<u32>(MicroButton::Count);

struct MicroMenuView {
    u32 level = 0;                            // the talents button shows from level 10 (UpdateTalentButton)
    u32 talentPoints = 0;                     // unspent: the talents button pulses
    std::array<bool, kMicroButtons> pushed{}; // the button's window is open (UpdateMicroButtons)
    TextureHandle portrait; // CharacterMicroButton's MicroButtonPortrait (invalid: portraitArt)
    u64 portraitArt = 0;
    bool spells = true; // its tooltip: SPELLBOOK_ABILITIES_BUTTON with spells, ABILITYBOOK_BUTTON without
    bool newbieTips = true; // phase 15 (options): SHOW_NEWBIE_TIPS off: the tooltips show only the button's name
    // The bag bar (where it fits): the backpack's and the bag slots' icons (textures.vdpk; 0: an empty slot),
    // the keyring (shown once it holds a key: SHOW_KEYRING), the backpack's free slots, the bags' checked
    // highlight.
    std::array<u64, 5> bags{};
    bool keyring = false;
    u32 freeSlots = 0;
    bool bagsOpen = false;
    // Phase 15 (items): each button's ContainerFrame open (the backpack, the four bags, the keyring): its checked
    // highlight (ContainerFrame_OnShow's SetChecked). All false: bagsOpen checks them all.
    std::array<bool, 6> bagOpen{};
};

struct MicroMenuResult {
    i32 button = -1; // MicroButton tapped
    i32 bag = -1;    // the bag bar: 0 the backpack, 1..4 the bag slots, 5 the keyring
    // Phase 15 (items): the backpack or a bag held instead of tapped: their Shift + click (OpenAllBags).
    bool bagShift = false;
    // Phase 15 (items): where the bag bar's buttons are this frame (0 the backpack, 1..4, 5 the keyring; empty when
    // not drawn): a carried item dropped on one goes into that bag (BagSlotButton's OnReceiveDrag).
    std::array<ui::Rect, 6> bagRects{};
};

// Kept between frames.
struct MicroMenuState {
    bool unfolded = false;     // the buttons' row unfolded from the gear
    bool bagsUnfolded = false; // the bag bar unfolded from the rim's backpack (a long press on it)
};

// Where it goes on the screen of `c`.
struct MicroMenuLayout {
    ui::Rect toggle;   // the gear on the minimap's rim
    ui::Rect strip;    // where the buttons unfold, left of it
    ui::Rect bags;     // where the bag bar unfolds from the rim's backpack (under the minimap)
    ui::Rect backpack; // the rim's backpack (a finger there does not fold the bag bar)
    f32 scale = 1.0f;    // logical pixels per unit of the original (the buttons' 29 x 58)
    f32 bagScale = 1.0f; // the unfolded bag bar's
};
MicroMenuLayout microMenuLayout(const DialogContext& c);

// Draws the gear and what is unfolded (call it after the panel windows: the rows go over them).
MicroMenuResult microMenu(DialogContext& c, const MicroMenuLayout& layout, const MicroMenuView& v,
                          MicroMenuState& state);

} // namespace vd
