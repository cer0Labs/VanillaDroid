#pragma once

#include "app/Dialogs.h"
#include "app/ModelFrame.h"
#include "core/Types.h"
#include "game/CombatFeedback.h"
#include "render/Renderer.h"
#include "ui/Widgets.h"

#include <array>
#include <span>
#include <string_view>

namespace vd {

// Phase 15 (pets): the windows and HUD pieces of the hunter's and the warlock's pet that 1.12 has and the
// client lacked (ROADMAP F15.5, 15.2b, 15.11), drawn with app/DialogKit over the client's art. Pure views
// like app/Dialogs: a view-model in, what the player did out; no game state, no network (app/PetScreens fills
// them, app/PetGallery with sample data). The interface's words come from core/Text (the client's keys:
// STABLES, CURRENT_PET, PET_DIET_TEMPLATE, TRAINING_POINTS...; ours: VD_PETS_*). Touch adaptations: a tap is
// the left click (ClickStablePet, a stat's tooltip), a long press the hover (a slot's tooltip) and, on the
// stable's slots, PickupStablePet (then a tap elsewhere drops it); a finger dragged from a slot to another is
// the drag and drop.

// --- PetStableFrame (PetStable.xml / .lua) -----------------------------------------------------------------

struct StableSlotView {
    u64 iconHash = 0;            // the pet's family icon (CreatureFamily.dbc); 0: an empty slot
    bool purchased = true;       // a stable slot not bought yet: the background tinted red and disabled
    bool enabled = true;         // PetStableCurrentPet:Disable() while a warlock's pet is out
    bool checked = false;        // the selected pet (CheckButtonHilight)
    bool picked = false;         // on the cursor (PickupStablePet): being moved
    std::string_view tooltip;    // the pet's name, or EMPTY_STABLE_SLOT
    std::string_view tooltipSub; // "Level 12 Wolf"
};
struct StableView {
    std::string_view levelText;   // PetStableLevelText: "Fang Level 12 Wolf"
    std::string_view loyaltyText; // PetStableLoyaltyText: the loyalty level's name
    std::string_view dietText;    // PetStablePetInfo's tooltip (PET_DIET_TEMPLATE); empty: no pet shown
    bool showModel = false;       // PetStableModel shown (a pet selected)
    TextureHandle model;          // its image (app/ModelViews; invalid while it loads)
    StableSlotView current;       // PetStableCurrentPet (index 0)
    std::array<StableSlotView, 2> stabled{}; // PetStableStabledPet1..2 (indexes 1, 2)
    bool showPurchase =
        true; // fewer slots bought than NUM_PET_STABLE_SLOTS: STABLE_SLOT_TEXT, the cost, Purchase
    bool canPurchase = false; // the money covers GetNextStableSlotCost (the cost white, else red)
    u32 nextSlotCost = 0;
    u32 money = 0; // PetStableMoneyFrame: the player's
};
struct StableResult {
    i32 click = -1;        // ClickStablePet(index): a tap (it also drops a pet on the cursor)
    i32 pickup = -1;       // PickupStablePet(index): a long press, or a drag starting on it
    i32 drop = -1;         // a drag released over this slot (after `pickup` of the slot it started on)
    bool purchase = false; // BuyStableSlot
    bool rotated = false;  // a rotate button went down (igInventoryRotateCharacter)
    bool closed = false;
};
// The window (a panel in c.panel when the panels place it): the selected pet's name, level, family and
// loyalty, its model with the diet's face, the current pet's and the two stabled pets' slots, the slot's
// purchase, the money.
StableResult stableWindow(DialogContext& c, const StableView& v, ModelFrameState& model);

// --- PetPaperDollFrame (PetPaperDollFrame.xml / .lua): the character window's Pet tab ----------------------

// A line of PetAttributesFrame (AttributeFrameTemplate / StatFrameTemplate): label, value and the tooltip a
// tap shows.
struct PetStatView {
    std::string_view label;
    std::string_view value;
    u32 color = 0xFFFFFFFF;      // the value's (green buffed, red debuffed)
    std::string_view tooltip;    // the first line (HIGHLIGHT)
    std::string_view tooltipSub; // the next (NORMAL_FONT_COLOR, wrapped); empty: none
};
// PetMagicResFrame1..5: Arcane, Fire, Nature, Frost, Shadow (UI-Character-ResistanceIcons).
struct PetResistanceView {
    std::string_view value; // with its color
    u32 color = 0xFFFFFFFF;
    std::string_view tooltip; // RESISTANCE<n>_NAME, with the buffs' formula when there are some
};
struct PetPaperDollView {
    std::string_view name;        // PetNameText (the window's title shows it instead of the character's)
    std::string_view levelText;   // PetLevelText: "Level 12 Wolf"
    std::string_view loyaltyText; // PetLoyaltyText: empty for a warlock's pet (GetPetLoyalty: hunters only)
    TextureHandle model;
    bool hunter = false;                 // HasPetUI's canGainXP: the diet's face and the training points show
    std::string_view dietText;           // PetPaperDollPetInfo's tooltip (PET_DIET_TEMPLATE)
    std::string_view trainingText;       // PetTrainingPointText: total - spent (may be negative)
    std::array<PetStatView, 5> stats{};  // Strength .. Spirit
    std::array<PetStatView, 5> combat{}; // Attack, Power (attack power), Damage, Defense, Armor
    std::array<PetResistanceView, 5> resistances{};
    u32 xp = 0, nextXp = 0; // GetPetExperience (0 / 0: an empty bar)
};
struct PetPaperDollResult {
    bool rotated = false;
};
// The page inside `body` (InventoryScreens' character window body); `state` keeps the model's facing and the
// tooltip a tap opened between frames.
struct PetPaperDollState {
    ModelFrameState model;
    i32 tip = -1; // the stat (0..4 attributes, 5..9 attack..armor, 10..14 resistances, 15 diet, 16 xp) whose
                  // tooltip shows
};
PetPaperDollResult petPaperDoll(DialogContext& c, const ui::Rect& body, const PetPaperDollView& v,
                                PetPaperDollState& state);

// --- The unit frames' pieces (PlayerFrame.xml, PetFrame.xml, CombatFeedback.lua) ---------------------------

// PlayerFrameGroupIndicator over the player frame `playerFrame` (its 232 x 100 units): "Group N" (GROUP and
// the subgroup), the indicator's art at 0.3 alpha and its text at 0.7, as PlayerFrame_UpdateGroupIndicator
// shows it in a raid.
void groupIndicator(ui::Gui& g, const ui::Rect& playerFrame, u32 subgroup);
// A hit indicator (PlayerHitIndicator, PetHitIndicator: NumberFontNormalHuge) centered on (x, y): the number,
// or the word of `word` (CombatFeedbackText: MISS, DODGE...), at `unit` logical pixels per interface unit.
void hitIndicator(ui::Gui& g, f32 x, f32 y, f32 unit, const game::FeedbackView& v);
// The word CombatFeedbackText gives an event (the client's MISS, RESIST, DODGE, PARRY, BLOCK, EVADE, IMMUNE,
// DEFLECT, ABSORB, REFLECT, INTERRUPT).
std::string_view feedbackWord(game::UnitCombatEvent event);

} // namespace vd
