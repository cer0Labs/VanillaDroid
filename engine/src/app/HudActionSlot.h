#pragma once

#include "core/Types.h"

#include <string_view>

namespace vd {

// One button of the action bar, as the game describes it this frame.
struct HudActionSlot {
    u64 iconHash = 0;         // its icon texture (0: an empty slot)
    f32 cooldownLeft = 0.0f;  // seconds
    f32 cooldownTotal = 0.0f;
    bool usable = true;       // enough power, right state
    bool inRange = true;
    bool active = false;      // an on/off action that is on (auto attack, Auto Shot)
    u32 count = 0;            // items: how many are left (0: not shown)
    bool highlight = false;   // a target for the spell being placed (glows)
    // Parity pass (ActionButton.lua): short of power only (ActionButton_UpdateUsable: blue), the current action
    // (CheckButtonHilight: casting, queued for the next swing, auto-repeating), the auto attack / auto-repeat
    // flash (UI-QuickslotRed blinking every ATTACK_BUTTON_FLASH_TIME), an equipped item (the green border).
    // `active` draws as checked and flashing (the context button's auto attack).
    bool noMana = false;
    bool checked = false;
    bool flashing = false;
    bool equipped = false;
    // Phase 15 (options): a macro's name at the button's bottom (ActionButton_Update: GetActionText; empty: none).
    std::string_view label;
};

} // namespace vd
