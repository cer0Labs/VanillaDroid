#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"

#include <span>
#include <string_view>

namespace vd {

// One StaticPopup as it is drawn (parity pass, popups): a pure view, filled by app/PopupScreens.
struct PopupView {
    std::string_view text;    // empty: the slot is not shown
    std::string_view button1; // empty: no such button
    std::string_view button2;
    bool button1Enabled = true;
    bool alert = false; // showAlert: wider, with DialogAlertIcon at the left
    bool money = false; // hasMoneyFrame: `copper` under the text
    u32 copper = 0;
    bool editBox = false; // hasEditBox: a text field under the text
    std::string_view editText;
    bool editing = false; // the field has the keyboard
};

struct PopupTaps {
    i32 slot = -1;     // a button of this slot was tapped...
    u32 button = 0;    // ...1 or 2
    i32 editSlot = -1; // the edit box of this slot was tapped
};

// The four StaticPopups (StaticPopup.xml) at the top centre of the safe area: StaticPopup1 128 units below
// the top, each next one under the one before, whether that one shows or not (a hidden frame keeps its place
// and the height it last had: `heights`, one per slot, kept by the caller). The template's art:
// UI-DialogBox-Background and -Border, white GameFontHighlight text, UI-DialogBox-Button-* buttons (button 1
// left of the centre, button 2 right of it; one alone centered), DialogAlertIcon for showAlert. Not modal: no
// blocker, the world, the stick and the action bar keep their fingers outside the dialogs. The same on every
// screen (the text over the buttons, as 1.12); on a short one the last of four tall ones overlap the ones above.
// Returns what was tapped this frame.
PopupTaps popupStack(DialogContext& c, std::span<const PopupView> slots, std::span<f32> heights);

} // namespace vd
