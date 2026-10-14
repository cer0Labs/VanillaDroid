#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"

#include <span>
#include <string_view>

namespace vd {

// Phase 18 (p3): TutorialFrame.xml as pure views. The alert buttons (TutorialFrameAlertButton1..10: 34 x 42 units of
// TutorialFrameAlert, the first centred on TutorialFrameParent at the bottom of the screen, each next one 36 units
// right of the one before, the highlight pulsing every half second for ten seconds: SetButtonPulse(button, 10, 0.5))
// and TutorialFrame (230 units wide, as tall as its text + 62: the title in gold, the text in white, the
// "Display Tips" check button at the bottom left and Okay at the bottom right, over the tooltip backdrop; 45 units
// above the buttons, or under the screen's centre for the welcome after the intro: TutorialFrame_CheckIntro).
// Touch: a tap on a button is its click; a long press shows its tooltip (the tutorial's title: its OnEnter). The
// original's TutorialFrameParent sits 55 units above the bottom edge, over the main bar; here the bottom centre is
// the cast bar's: the buttons go right above it (`bottom`).

struct TutorialAlertView {
    std::string_view title; // the button's tooltip (TUTORIAL_TITLE<n>)
    bool highlight = false; // the pulse is on now
};
struct TutorialFrameView {
    bool shown = false;
    std::string_view title; // TUTORIAL_TITLE<n>
    std::string_view text;  // TUTORIAL<n> (ours for the ones about a mouse or keys)
    bool displayTips = true; // TutorialFrameCheckButton
    bool centered = false;   // the welcome after the intro (TutorialFrame_CheckIntro)
};
struct TutorialResult {
    i32 alertTapped = -1;      // the button tapped: TutorialFrame shows its tutorial
    bool okay = false;         // TutorialFrameOkayButton (the frame hides)
    bool toggledTips = false;  // the check button changed
};

// The alert buttons, the first one's bottom centre at (centerX, bottom). Returns the button tapped. `avoid`: the touch
// HUD's action buttons (empty: none): a row that would reach them moves left as a whole (ours: the original's bottom
// centre has nothing beside it).
i32 tutorialAlerts(DialogContext& c, std::span<const TutorialAlertView> alerts, f32 centerX, f32 bottom,
                   const ui::Rect& avoid = {});
// TutorialFrame over everything; `alertsBottom` and `avoid` as above (the frame moves left of the buttons too).
TutorialResult tutorialFrame(DialogContext& c, const TutorialFrameView& view, f32 centerX, f32 alertsBottom,
                             const ui::Rect& avoid = {});
// Where the alerts go: right above the cast bar (`castBarTop`: the HUD's, app/Hud castBarRect).
f32 tutorialAlertsBottom(const DialogContext& c, f32 castBarTop);

} // namespace vd
