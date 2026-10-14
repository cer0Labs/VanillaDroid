#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"
#include "pack/UiFormat.h"

#include <span>
#include <string_view>

namespace vd {

// Phase 15 (options): Blizzard_MacroUI's two frames as pure views like app/Dialogs (a view-model in, what the player
// did out; no game state): the MacroFrame (UIPanelWindows "left") and the MacroPopupFrame that names a macro and
// chooses its icon. Laid out as their XML: the two tabs (GENERAL_MACROS, CHARACTER_SPECIFIC_MACROS), the 18 macro
// buttons in three rows of six (MacroButtonTemplate: the icon on UI-EmptySlot-Disabled, its name under it in a small
// outlined font, CheckButtonHilight on the selected one), a bar, the selected macro's button, name and "Change
// Name/Icon", "Enter Macro Commands:", the 255-letter edit box with "%d/255 Characters Used" under it, and Delete /
// New / Exit; the popup's name box (16 letters) over the icon grid (five a row, MacroPopupButtonTemplate) with
// Okay / Cancel. Wide places put the body beside the buttons.
//
// Touch: a tap is the click. The original picks a macro up for the action bars by clicking the selected macro's
// button (PickupMacro) or dragging one out of the list (OnDragStart): here a tap on the selected button or a long press
// on a list button reports `pickup`, and the glue lights the action buttons for the next tap (the way the spell book
// places spells, app/CombatScreens). A tap on the edit box gives it the keyboard (TextKind::MultiLine: Enter types a
// line break). The popup is a modal panel over the frame (the original hangs it at the frame's right).

struct MacroSlotView {
    u64 iconHash = 0; // its icon in textures.vdpk
    std::string_view name;
};

struct MacroFrameView {
    u32 tab = 0;                          // 0 the account's ("General Macros"), 1 the character's
    std::string_view characterTab;        // CHARACTER_SPECIFIC_MACROS with the character's name
    std::span<const MacroSlotView> macros; // the tab's, sorted by name (at most MAX_MACROS)
    i32 selected = -1;                    // into macros
    std::string_view body;                // the selected macro's commands (the edit box's text while it is edited)
    bool editing = false;                 // the edit box has the keyboard
    bool popup = false;                   // the popup is up: the tabs and the buttons wait (MacroPopupFrame_OnShow)
};
struct MacroFrameResult {
    i32 tab = -1;          // a tab tapped
    i32 select = -1;       // a macro button tapped (MacroButton_OnClick)
    i32 pickup = -1;       // a macro to put on an action button (the selected one's button, a long press)
    bool bodyTapped = false; // MacroFrameTextButton: the edit box takes the keyboard
    bool create = false;   // MacroNewButton
    bool change = false;   // MacroEditButton ("Change Name/Icon")
    bool remove = false;   // MacroDeleteButton
    bool closed = false;   // Exit or the close button
};
MacroFrameResult macroFrame(DialogContext& c, const MacroFrameView& v);

struct MacroPopupView {
    std::string_view name;
    bool nameEditing = false;
    std::span<const pack::UiMacroIcon> icons; // GetMacroIconInfo's order
    i32 selectedIcon = -1;
    bool okay = false; // MacroPopupOkayButton_Update: a name, and an icon (or editing)
};
struct MacroPopupResult {
    bool nameTapped = false;
    i32 icon = -1; // an icon tapped (MacroPopupButton_OnClick)
    bool okay = false;
    bool cancel = false;
};
MacroPopupResult macroPopup(DialogContext& c, const MacroPopupView& v);

} // namespace vd
