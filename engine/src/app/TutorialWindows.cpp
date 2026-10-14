#include "app/TutorialWindows.h"

#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>

namespace vd {
namespace {

using ui::Gui;
using ui::Rect;

constexpr u64 kAlert = assetPathHash("Interface\\TutorialFrame\\TutorialFrameAlert.blp");
constexpr u64 kButtonUp = assetPathHash("Interface\\Buttons\\UI-DialogBox-Button-Up.blp");
constexpr u64 kButtonDown = assetPathHash("Interface\\Buttons\\UI-DialogBox-Button-Down.blp");
constexpr u64 kButtonHighlight = assetPathHash("Interface\\Buttons\\UI-DialogBox-Button-Highlight.blp");
constexpr u64 kCheckUp = assetPathHash("Interface\\Buttons\\UI-CheckBox-Up.blp");
constexpr u64 kCheckDown = assetPathHash("Interface\\Buttons\\UI-CheckBox-Down.blp");
constexpr u64 kCheckChecked = assetPathHash("Interface\\Buttons\\UI-CheckBox-Check.blp");

// The frame's units in dp: as the StaticPopups (app/PopupWindows), its 12-unit text at the dialogs' 15 dp; the
// buttons then are 42 x 52 dp, a finger's size.
constexpr f32 kUnitDp = 1.25f;
// TutorialFrame.xml's sizes in units.
constexpr f32 kAlertW = 34.0f, kAlertH = 42.0f, kAlertStep = 36.0f;
constexpr f32 kFrameW = 230.0f, kTextW = 210.0f, kFrameExtra = 62.0f;
constexpr f32 kFrameAboveAlerts = 45.0f; // TutorialFrame BOTTOM 0, 100 over TutorialFrameParent BOTTOM 0, 55
constexpr f32 kCenteredBelow = 90.0f;    // BOTTOM to UIParent's CENTER, 0, -90

// TutorialFrameOkayButton: DialogButtonNormalTexture / Pushed / Highlight (UI-DialogBox-Button-*, texels
// 0..128 x 0..20 of 128 x 32), GameFontNormalSmall.
bool dialogButton(Gui& g, ui::WidgetId id, const Rect& r, std::string_view text) {
    Rect target = r;
    const f32 minimum = g.px(ui::kTouchDp);
    if (target.h < minimum) {
        target.y -= (minimum - target.h) * 0.5f;
        target.h = minimum;
    }
    const ui::WidgetState state = ui::hit(g, id, target);
    const bool down = ui::held(state, target);
    constexpr f32 kCapU = 8.0f / 128.0f;
    const f32 cap = std::min(r.h * 8.0f / 20.0f, r.w * 0.5f);
    ui::threeSlice(g.draw, r, down ? kButtonDown : kButtonUp, {0.0f, 0.0f, 1.0f, 0.625f}, kCapU, cap);
    if (down) {
        ui::threeSlice(g.draw, r, kButtonHighlight, {0.0f, 0.0f, 1.0f, 0.625f}, kCapU, cap,
                       ui::withAlpha(ui::kWhite, 0.5f));
    }
    ui::TextStyle style = ui::gameFont(r.h * 0.48f, down ? ui::kHighlight : ui::kGold);
    style.align = ui::Align::Center;
    g.draw.text(r.centerX(), ui::centeredBaseline(g.draw, style, r.centerY()), text, style);
    return ui::tapped(state);
}

} // namespace

f32 tutorialAlertsBottom(const DialogContext& c, f32 castBarTop) {
    return std::min(castBarTop, c.safeArea.bottom());
}

namespace {

// Left of `avoid` when `r` reaches into it (and shares its rows); never past the safe area's left edge.
f32 clearOf(const DialogContext& c, const Rect& r, const Rect& avoid) {
    const f32 margin = c.ui.px(8.0f);
    if (avoid.w <= 0.0f || r.right() + margin <= avoid.x || r.x >= avoid.right() || r.bottom() <= avoid.y ||
        r.y >= avoid.bottom()) {
        return 0.0f;
    }
    return std::max(c.safeArea.x + margin - r.x, avoid.x - margin - r.right());
}

} // namespace

i32 tutorialAlerts(DialogContext& c, std::span<const TutorialAlertView> alerts, f32 centerX, f32 bottom,
                   const ui::Rect& avoid) {
    VD_PROFILE_ZONE("TutorialAlerts::draw");
    Gui g{c.ui, c.draw};
    const f32 u = g.px(kUnitDp);
    if (!alerts.empty()) {
        const f32 last = static_cast<f32>(alerts.size() - 1);
        const Rect row{centerX - kAlertW * 0.5f * u, bottom - kAlertH * u, (last * kAlertStep + kAlertW) * u,
                       kAlertH * u};
        centerX += clearOf(c, row, avoid);
    }
    i32 tapped = -1, held = -1;
    Rect heldRect;
    for (u32 i = 0; i < alerts.size(); ++i) {
        const Rect r{centerX + (static_cast<f32>(i) * kAlertStep - kAlertW * 0.5f) * u, bottom - kAlertH * u,
                     kAlertW * u, kAlertH * u};
        const ui::WidgetState state = ui::hit(g, ui::widgetId("tutorial.alert", i), r);
        // NormalTexture: the left half; HighlightTexture (23 x 42, CENTER -1, 0): the right half, while touched or
        // pulsing (its ADD blend here by its own alpha: the art has one).
        g.draw.sprite(r, kAlert, ui::kWhite, {0.0f, 0.0f, 0.53125f, 0.6875f});
        if (alerts[i].highlight || ui::held(state, r)) {
            const Rect light{r.centerX() - u - 23.0f * u * 0.5f, r.y, 23.0f * u, r.h};
            g.draw.spriteAdditive(light, kAlert, ui::kWhite, {0.578125f, 0.0f, 0.9375f, 0.6875f});
        }
        if (ui::tapped(state)) {
            tapped = static_cast<i32>(i);
        }
        if (state.touch && state.touch->longPressed && !state.touch->released) {
            held = static_cast<i32>(i);
            heldRect = r;
        }
    }
    if (held >= 0 && !alerts[static_cast<u32>(held)].title.empty()) {
        // OnEnter: GameTooltip ANCHOR_LEFT with the title (here above the button, clear of the finger).
        const ui::TextStyle style = ui::gameFont(14.0f * u, ui::kWhite);
        const std::string_view title = alerts[static_cast<u32>(held)].title;
        const f32 pad = g.px(8.0f);
        const f32 w = g.draw.measure(title, style.font, style.size) + 2.0f * pad;
        const f32 h = style.size + 2.0f * pad;
        const Rect panel{std::clamp(heldRect.x - w, c.safeArea.x, c.safeArea.right() - w), heldRect.y - h - g.px(6.0f),
                         w, h};
        ui::tooltipPanel(g, panel);
        g.draw.text(panel.x + pad, ui::centeredBaseline(g.draw, style, panel.centerY()), title, style);
    }
    return tapped;
}

TutorialResult tutorialFrame(DialogContext& c, const TutorialFrameView& view, f32 centerX, f32 alertsBottom,
                             const ui::Rect& avoid) {
    VD_PROFILE_ZONE("TutorialFrame::draw");
    TutorialResult result;
    if (!view.shown) {
        return result;
    }
    Gui g{c.ui, c.draw};
    const f32 u = g.px(kUnitDp);
    // TutorialFrameTitle (GameFontHighlight in 1, 0.82, 0) at 10, -9; TutorialFrameText (GameFontNormal in white) 2
    // under it; both 210 wide. The frame's height: the text's + 62 (TutorialFrame_Update).
    const ui::TextStyle titleStyle = ui::gameFont(12.0f * u, ui::rgba(255, 209, 0));
    const ui::TextStyle textStyle = ui::gameFont(12.0f * u, ui::kWhite);
    const f32 titleH = g.draw.measureWrapped(kTextW * u, view.title, titleStyle.font, titleStyle.size);
    const f32 textH = g.draw.measureWrapped(kTextW * u, view.text, textStyle.font, textStyle.size);
    const f32 w = kFrameW * u, h = textH + kFrameExtra * u;
    // Centred (the welcome) it never comes lower than over the alerts: a short wide screen would put it on them.
    f32 bottom = alertsBottom - kFrameAboveAlerts * u;
    if (view.centered) {
        bottom = std::min(bottom, c.safeArea.centerY() + kCenteredBelow * u);
    }
    bottom = std::clamp(bottom, c.safeArea.y + h, c.safeArea.bottom()); // clampedToScreen
    Rect frame{std::clamp(centerX - w * 0.5f, c.safeArea.x, c.safeArea.right() - w), bottom - h, w, h};
    frame.x += clearOf(c, frame, avoid);
    // enableMouse: the frame takes the fingers that land on it.
    ui::hit(g, ui::widgetId("tutorial.frame"), frame);
    // Backdrop: TutorialFrameBackground (black at 60 %) inside UI-Tooltip-Border's 16-unit edges.
    ui::tooltipPanel(g, frame, ui::rgba(0, 0, 0, 153));
    g.draw.wrappedText({frame.x + 10.0f * u, frame.y + 9.0f * u, kTextW * u, titleH + 1.0f}, view.title, titleStyle);
    g.draw.wrappedText({frame.x + 10.0f * u, frame.y + 9.0f * u + titleH + 2.0f * u, kTextW * u, textH + 1.0f},
                       view.text, textStyle);
    // TutorialFrameCheckButton (24 x 24 at BOTTOMLEFT 5, 5) and its label (GameFontNormalSmall, at its right).
    const Rect box{frame.x + 5.0f * u, frame.bottom() - 29.0f * u, 24.0f * u, 24.0f * u};
    const ui::TextStyle small = ui::gameFont(10.0f * u);
    const std::string_view label = text::get("ENABLE_TUTORIAL_TEXT");
    const Rect row{box.x, box.y, box.w + g.draw.measure(label, small.font, small.size) + 4.0f * u, box.h};
    const ui::WidgetState check = ui::hit(g, ui::widgetId("tutorial.check"), row.inset(-g.px(4.0f)));
    g.draw.sprite(box, ui::held(check, row.inset(-g.px(4.0f))) ? kCheckDown : kCheckUp);
    if (view.displayTips) {
        g.draw.sprite(box, kCheckChecked);
    }
    g.draw.text(box.right(), ui::centeredBaseline(g.draw, small, box.centerY()), label, small);
    if (ui::tapped(check)) {
        result.toggledTips = true;
    }
    // TutorialFrameOkayButton: 76 x 21 at BOTTOMRIGHT -7, 7.
    const Rect okay{frame.right() - 83.0f * u, frame.bottom() - 28.0f * u, 76.0f * u, 21.0f * u};
    result.okay = dialogButton(g, ui::widgetId("tutorial.okay"), okay, text::get("OKAY"));
    return result;
}

} // namespace vd
