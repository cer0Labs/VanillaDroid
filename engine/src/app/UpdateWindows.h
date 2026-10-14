#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"

#include <array>
#include <string_view>

namespace vd {

// Phase 22 (updates): the update notices on the glue screens (app/UpdateScreens fills them): a GlueDialog of
// the 1.12 (UI-DialogBox's backdrop, GlueFontNormalLarge, GluePanelButton art) with a second, smaller text, a
// progress bar (CastingBarFrame's) and up to three buttons. Modal, sized in the glue's units
// (app/GlueWindows' GlueLayout). Pure view: what is shown in, the button tapped out.
struct UpdateDialogView {
    std::string_view text;   // GlueFontNormalLarge, gold
    std::string_view detail; // GlueFontHighlightSmall, white; empty: none
    bool progress = false;   // the bar under the texts
    f32 fraction = 0.0f;     // its fill, 0..1
    std::string_view progressText;
    std::array<std::string_view, 3> buttons{}; // left to right; empty ones are left out
    std::string_view name = "glue.update";     // its widgets' ids
};
struct UpdateDialogResult {
    i32 button = -1; // the one tapped, -1 none
};
UpdateDialogResult updateDialog(DialogContext& c, const UpdateDialogView& v);

} // namespace vd
