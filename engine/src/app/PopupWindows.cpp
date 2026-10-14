#include "app/PopupWindows.h"

#include "app/DialogKit.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "ui/Widgets.h"

#include <algorithm>

namespace vd {
namespace {

using ui::Align;
using ui::Gui;
using ui::Rect;
using ui::TextStyle;
using ui::widgetId;

// StaticPopupButtonTemplate: texels 0..128 x 0..20 of 128 x 32 (the highlight is added on top).
constexpr u64 kButtonUp = assetPathHash("Interface\\Buttons\\UI-DialogBox-Button-Up.blp");
constexpr u64 kButtonDown = assetPathHash("Interface\\Buttons\\UI-DialogBox-Button-Down.blp");
constexpr u64 kButtonDisabled = assetPathHash("Interface\\Buttons\\UI-DialogBox-Button-Disabled.blp");
constexpr u64 kButtonHighlight = assetPathHash("Interface\\Buttons\\UI-DialogBox-Button-Highlight.blp");
constexpr u64 kAlertIcon = assetPathHash("Interface\\DialogFrame\\DialogAlertIcon.blp");

// The popup's FrameXML units in dp: its 12-unit GameFontHighlight at the dialogs' 15 dp text (DialogKit's
// Square body text), so a 320-unit popup is 400 dp wide.
constexpr f32 kUnitDp = 1.25f;
constexpr f32 kTemplateHeight = 72.0f;     // StaticPopupTemplate's size before a first show
constexpr f32 kTopShare = 128.0f / 768.0f; // StaticPopup1: 128 units below the top of the 768-unit UIParent

bool dialogButton(Gui& g, ui::WidgetId id, const Rect& r, std::string_view text, bool enabled) {
    Rect target = r;
    const f32 minimum = g.px(ui::kTouchDp);
    if (target.h < minimum) {
        target.y -= (minimum - target.h) * 0.5f;
        target.h = minimum;
    }
    const ui::WidgetState state = ui::hit(g, id, target);
    const bool down = enabled && ui::held(state, target);
    constexpr ui::TexCoords kUv{0.0f, 0.0f, 1.0f, 0.625f};
    constexpr f32 kCapU = 8.0f / 128.0f;
    const f32 cap = std::min(r.h * 8.0f / 20.0f, r.w * 0.5f);
    ui::threeSlice(g.draw, r, !enabled ? kButtonDisabled : down ? kButtonDown : kButtonUp, kUv, kCapU, cap);
    if (down) { // HighlightTexture alphaMode="ADD"
        g.draw.spriteAdditive({r.x, r.y, cap, r.h}, kButtonHighlight, ui::kWhite,
                              {0.0f, 0.0f, kCapU, 0.625f});
        g.draw.spriteAdditive({r.x + cap, r.y, r.w - 2.0f * cap, r.h}, kButtonHighlight, ui::kWhite,
                              {kCapU, 0.0f, 1.0f - kCapU, 0.625f});
        g.draw.spriteAdditive({r.right() - cap, r.y, cap, r.h}, kButtonHighlight, ui::kWhite,
                              {1.0f - kCapU, 0.0f, 1.0f, 0.625f});
    }
    // NormalFont GameFontNormal, HighlightFont GameFontHighlight, DisabledFont GameFontDisable.
    TextStyle style = ui::gameFont(ui::buttonFontSize(g, r.h), !enabled ? ui::kDisabledText
                                                                       : down   ? ui::kHighlight
                                                                                : ui::kGold);
    style.align = Align::Center;
    const f32 push = down ? std::max(1.0f, g.px(1.0f)) : 0.0f;
    ui::label(g, {r.x + cap * 0.5f + push, r.y + push, r.w - cap, r.h}, text, style);
    return enabled && ui::tapped(state);
}

// StaticPopup_Show's button widths: 120 units, or the text plus 20.
f32 buttonWidth(const Gui& g, f32 u, std::string_view text, f32 height) {
    const f32 size = ui::buttonFontSize(g, height);
    return std::max(120.0f * u, g.draw.measure(text, ui::FontId::Friz, size) + 20.0f * u + height * 0.4f);
}

} // namespace

PopupTaps popupStack(DialogContext& c, std::span<const PopupView> slots, std::span<f32> heights) {
    VD_PROFILE_ZONE("Popups::draw");
    PopupTaps taps;
    Gui g{c.ui, c.draw};
    const kit::Metrics m = kit::metrics(c);
    const f32 u = g.px(kUnitDp);
    const Rect& safe = c.safeArea;
    const f32 margin = g.px(8.0f);
    TextStyle textStyle = ui::gameFont(m.text, ui::kWhite); // GameFontHighlight
    textStyle.align = Align::Center;
    const f32 buttonH = m.button;
    f32 y = safe.y + safe.h * kTopShare;
    for (u32 i = 0; i < slots.size() && i < heights.size(); ++i) {
        const PopupView& v = slots[i];
        if (v.text.empty()) { // hidden: the next one still hangs from this frame's place
            y += heights[i] > 0.0f ? heights[i] : kTemplateHeight * u;
            continue;
        }
        const bool two = !v.button2.empty();
        const f32 w1 = v.button1.empty() ? 0.0f : buttonWidth(g, u, v.button1, buttonH);
        const f32 w2 = two ? buttonWidth(g, u, v.button2, buttonH) : 0.0f;
        const f32 iconSize = 64.0f * u;
        // StaticPopup_Resize: 16 + text + 8 + button + 16 (the money frame adds 16, the edit box 8 + 32).
        const f32 width = std::min((v.alert ? 420.0f : 320.0f) * u, safe.w - 2.0f * margin);
        const f32 textW = std::min(290.0f * u, width - 30.0f * u);
        const f32 textH = g.draw.measureWrapped(textW, v.text, textStyle.font, textStyle.size);
        const f32 editH = v.editBox ? std::max(32.0f * u, g.px(ui::kTouchDp)) : 0.0f;
        const f32 h = 16.0f * u + textH + 8.0f * u + (v.money ? 16.0f * u : 0.0f) +
                      (v.editBox ? editH + 8.0f * u : 0.0f) + (v.button1.empty() ? 0.0f : buttonH) + 16.0f * u;
        Rect panel{safe.centerX() - width * 0.5f, y, width, h};
        Rect textRect{panel.centerX() - textW * 0.5f, panel.y + 16.0f * u, textW, textH};
        f32 moneyY = textRect.bottom() + 5.0f * u + 7.0f * u;
        const f32 below = textRect.bottom() + 8.0f * u + (v.money ? 16.0f * u : 0.0f);
        Rect field{panel.centerX() - 65.0f * u, below, 130.0f * u, editH}; // the edit box
        const f32 buttonsY = below + (v.editBox ? editH + 8.0f * u : 0.0f);
        Rect buttons{panel.x, buttonsY, panel.w, buttonH}; // where the buttons go (their row)
        heights[i] = panel.h;
        // Never below the screen (four tall ones on the short screen): the last ones overlap the ones above,
        // their buttons stay reachable.
        const f32 lowest = safe.bottom() - margin - panel.h;
        if (panel.y > lowest) {
            const f32 shift = std::max(safe.y, lowest) - panel.y;
            panel.y += shift;
            textRect.y += shift;
            field.y += shift;
            buttons.y += shift;
            moneyY += shift;
        }
        // The frame takes the fingers that land on it (enableMouse), nothing else: no blocker.
        ui::hit(g, widgetId("popup.frame", i), panel);
        ui::dialogPanel(g, panel);
        if (v.alert) { // AlertIcon: 64 x 64 at the left, 12 units in
            g.draw.sprite({panel.x + 12.0f * u, panel.centerY() - iconSize * 0.5f, iconSize, iconSize},
                          kAlertIcon);
        }
        g.draw.wrappedText({textRect.x, textRect.y, textRect.w, textRect.h + 1.0f}, v.text, textStyle);
        if (v.money) {
            ui::money(g, textRect.centerX(), moneyY, v.copper, m.small, Align::Center);
        }
        if (v.editBox && ui::textField(g, widgetId("popup.edit", i), field, v.editText, {}, v.editing)) {
            taps.editSlot = static_cast<i32>(i);
        }
        if (!v.button1.empty() && two) {
            // Button 1's right edge 6 units left of the centre, button 2 13 units right of it.
            const f32 cx = buttons.centerX();
            if (dialogButton(g, widgetId("popup.button1", i), {cx - 6.0f * u - w1, buttons.y, w1, buttonH}, v.button1,
                             v.button1Enabled)) {
                taps.slot = static_cast<i32>(i);
                taps.button = 1;
            }
            if (dialogButton(g, widgetId("popup.button2", i), {cx + 7.0f * u, buttons.y, w2, buttonH}, v.button2,
                             true)) {
                taps.slot = static_cast<i32>(i);
                taps.button = 2;
            }
        } else if (!v.button1.empty() &&
                   dialogButton(g, widgetId("popup.button1", i), {buttons.centerX() - w1 * 0.5f, buttons.y, w1, buttonH},
                                v.button1, v.button1Enabled)) {
            taps.slot = static_cast<i32>(i);
            taps.button = 1;
        }
        y = panel.bottom();
    }
    return taps;
}

} // namespace vd
