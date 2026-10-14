#pragma once

#include "app/Dialogs.h"
#include "app/HudActionSlot.h"
#include "core/Types.h"
#include "ui/DrawList.h"

#include <span>
#include <string_view>

namespace vd {

// Phase 20 (pets, docs/MOBILE_CONTROLS.md §6.8): the pet button's windows as pure views (like the other
// *Windows): the pet's panel (the pet button held) and Feed Pet's list of foods. app/TouchPetScreens fills
// them and applies what the player did. Both open beside the pet button, toward the screen's middle, as round
// buttons like the cluster's; a tap outside closes them.

// --- The pet's panel: its commands, its reactions, its spells (a hold: autocast), the owner's pet spells ---

struct TouchPetPanelButton {
    HudActionSlot slot; // icon and states (the command or reaction in use and an autocast on are `checked`)
    std::string_view label; // under it when it fits (empty: none)
    u8 group = 0;           // the buttons of one group stay together on a row (a new group starts a new row
                            // when it does not fit in what is left of the one before)
    bool autocastable = false; // a hold toggles its autocast
};
struct TouchPetPanelView {
    std::string_view title;    // the pet's name
    std::string_view subtitle; // its level and family, how happy it is
    u32 happiness = 0;         // UI-PetHappiness' face: 1 unhappy, 2 content, 3 happy (0: none)
    std::span<const TouchPetPanelButton> buttons;
    ui::Rect anchor;       // the pet button
    f32 buttonSize = 0.0f; // a button's diameter (the pet button's)
    bool mirrored = false; // the cluster on the left: the panel opens to the right of the button
};
struct TouchPetPanelResult {
    i32 tapped = -1;
    i32 held = -1;       // held still (an autocastable spell: its autocast)
    bool closed = false; // a tap outside
    ui::Rect panel;
};
TouchPetPanelResult touchPetPanel(DialogContext& c, const TouchPetPanelView& v);

// --- Feed Pet: the foods carried, those the pet ate before first -------------------------------------------

struct TouchPetFoodRow {
    u64 icon = 0;
    std::string_view name;
    u32 count = 0;
    u32 quality = 1;
    bool accepted = false; // the pet's family ate it before: a mark before the name
};
struct TouchPetFoodView {
    std::string_view title; // Feed Pet
    std::string_view empty; // shown without rows
    std::span<const TouchPetFoodRow> rows;
    ui::Rect anchor;
    bool mirrored = false;
};
struct TouchPetFoodResult {
    i32 tapped = -1;
    bool closed = false;
    ui::Rect panel;
};
TouchPetFoodResult touchPetFood(DialogContext& c, const TouchPetFoodView& v);

} // namespace vd
