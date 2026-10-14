#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"

#include <span>
#include <string_view>

namespace vd {

// Phase 22 (pairing, ROADMAP 22.4): the pages of the first-run screen (app/FirstRunScreens), drawn with
// app.vdpk alone: our font (ui::FontId::App) on plain shapes, no client art (a phone without data has no
// ui.vdpk). Pure views: what a page shows comes in, what the player touched goes out. Touch first: buttons a
// finger tall or more, two columns on landscape screens (the explanation left, the buttons right, under the
// right thumb: both of the Fold5's screens and 20:9 phones), one column on a tall area (the buttons at the
// bottom); the explanation scrolls when it does not fit (the cover screen, the keyboard up), and so do the
// buttons on the rare page with more than fit. Everything stays inside the safe area the caller gives (the
// keyboard's height taken out while typing).

struct FirstRunButton {
    std::string_view label;
    std::string_view note; // a smaller second line (left out when the button is short)
    bool primary = false;
    bool enabled = true;
};

struct FirstRunField {
    std::string_view label;
    std::string_view text;
    std::string_view placeholder; // shown dimmed while the field is empty
    bool focused = false;         // the keyboard types into it: a caret blinks
};

struct FirstRunPageView {
    std::string_view title;   // next to the app's name; empty on the home page
    std::string_view version; // in the corner (the home page)
    // The explanation's column, from the top: the fields, the progress bar, the body, the check box, the
    // status line, the second body.
    std::span<const FirstRunField> fields;
    bool progress = false;
    bool indeterminate = false; // a bar that moves, for a wait of unknown length
    f32 fraction = 0.0f;
    std::string_view progressTop;   // over the bar (the file)
    std::string_view progressLeft;  // under it, left (what is done)
    std::string_view progressRight; // under it, right (speed, time left)
    bool busy = false;              // three dots that pulse (talking to the PC)
    std::string_view body;
    bool hasCheck = false;
    std::string_view checkLabel;
    bool checked = false;
    std::string_view status;
    bool statusError = false;
    std::string_view body2; // a second text, smaller (the ADB guide's steps)
    std::span<const FirstRunButton> buttons;
};

struct FirstRunInput {
    i32 button = -1; // the button tapped this frame
    i32 field = -1;  // the field tapped this frame
    bool toggled = false;
};

FirstRunInput drawFirstRunPage(DialogContext& c, const FirstRunPageView& view);

} // namespace vd
