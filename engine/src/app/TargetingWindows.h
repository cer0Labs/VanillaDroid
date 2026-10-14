#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"
#include "ui/Context.h"

#include <string_view>

namespace vd {

// Phase 13 (spells): the touch side of a spell waiting for its target (the desktop changes the cursor and
// waits for a click; a touch screen has no cursor). Pure view like app/Dialogs: a view-model in, what the
// player did out.

// The bar over the bottom middle (above the cast bar) while a spell waits: its icon and name, what to do, and
// its buttons: Cast for a place (the circle's confirm) and Cancel (the desktop's right click / Escape).
struct TargetingBarView {
    u64 iconHash = 0;       // the spell's icon, or the item's that casts it
    std::string_view title; // the spell's name
    std::string_view hint;  // VD_TARGETING_PLACE / _ITEM / _LOCKED
    bool showCast = false;  // a place: the Cast button
    bool inRange = true;    // false: the circle (and the Cast button's text) is red
    // The action buttons' cluster (empty: none): the bar goes right over it, on the side of the thumb that
    // taps them, instead of the bottom middle.
    ui::Rect above;
};
struct TargetingBarResult {
    bool cast = false;
    bool cancel = false;
};
TargetingBarResult targetingBar(DialogContext& c, const TargetingBarView& v);
// Where the bar goes: over the action cluster `above`, right-aligned with it, or else the safe area's bottom
// middle over the cast bar.
ui::Rect targetingBarRect(const DialogContext& c, const ui::Rect& above = {});

} // namespace vd
