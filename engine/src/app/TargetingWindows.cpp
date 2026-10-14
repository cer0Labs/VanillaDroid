#include "app/TargetingWindows.h"

#include "app/DialogKit.h"
#include "app/UiResources.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <optional>

namespace vd {
namespace {

using ui::Rect;
using ui::rgba;

constexpr f32 kBarWidthDp = 520.0f;
constexpr f32 kBarHeightDp = 56.0f;
constexpr f32 kButtonHeightDp = 40.0f; // a finger's
constexpr f32 kButtonMinWidthDp = 84.0f;
constexpr f32 kCastBarDp = 70.0f;              // app/Hud: the cast bar's distance from the safe area's bottom
constexpr f32 kAboveCastBarUnits = 34.0f;      // clear of its border (28 units over it) and the spell's name
constexpr u32 kOutOfRange = rgba(255, 26, 26); // the client's red error text

f32 buttonWidth(const ui::Gui& g, std::string_view label) {
    return std::max(g.px(kButtonMinWidthDp),
                    g.draw.measure(label, ui::FontId::Friz, g.px(15.0f)) + g.px(28.0f));
}

} // namespace

Rect targetingBarRect(const DialogContext& c, const Rect& above) {
    const ui::Gui g{c.ui, c.draw};
    const Rect& safe = c.safeArea;
    const f32 w = std::min(g.px(kBarWidthDp), safe.w - g.px(16.0f));
    const f32 h = g.px(kBarHeightDp);
    if (above.w > 0.0f && above.h > 0.0f) {
        const f32 right = std::min(above.right(), safe.right() - g.px(8.0f));
        return {std::max(safe.x + g.px(8.0f), right - w), above.y - g.px(8.0f) - h, w, h};
    }
    return {safe.centerX() - w * 0.5f, safe.bottom() - g.px(kCastBarDp) - g.unit(kAboveCastBarUnits) - h, w,
            h};
}

TargetingBarResult targetingBar(DialogContext& c, const TargetingBarView& v) {
    VD_PROFILE_ZONE("TargetingWindows::bar");
    TargetingBarResult result;
    ui::Gui g{c.ui, c.draw};
    const Rect r = targetingBarRect(c, v.above);
    // The bar takes its fingers (the stick and the camera keep the rest of the screen).
    c.ui.blocker(ui::widgetId("targeting.bar"), r);
    ui::tooltipPanel(g, r);
    const f32 pad = g.px(8.0f);
    const f32 icon = r.h - 2.0f * pad;
    const std::optional<u32> iconTexture = v.iconHash != 0 ? c.resources.icon(v.iconHash) : std::nullopt;
    if (iconTexture) {
        c.draw.icon({r.x + pad, r.y + pad, icon, icon}, *iconTexture);
    }
    // Buttons on the right: Cast (a place), then Cancel.
    const f32 buttonH = std::min(g.px(kButtonHeightDp), r.h - 2.0f * g.px(4.0f));
    const std::string_view cancelText = text::get("CANCEL");
    const std::string_view castText = text::get("VD_TARGETING_CAST");
    const f32 cancelW = buttonWidth(g, cancelText);
    const f32 castW = v.showCast ? buttonWidth(g, castText) : 0.0f;
    f32 right = r.right() - pad;
    const f32 buttonY = r.centerY() - buttonH * 0.5f;
    if (ui::button(g, ui::widgetId("targeting.cancel"), {right - cancelW, buttonY, cancelW, buttonH},
                   cancelText)) {
        result.cancel = true;
    }
    right -= cancelW + pad * 0.5f;
    if (v.showCast) {
        if (ui::button(g, ui::widgetId("targeting.cast"), {right - castW, buttonY, castW, buttonH},
                       castText)) {
            result.cast = true;
        }
        right -= castW + pad * 0.5f;
    }
    // The spell's name over what to do (red: out of range).
    const f32 textX = r.x + pad + (iconTexture ? icon + pad : 0.0f);
    const f32 textW = std::max(0.0f, right - textX - pad * 0.5f);
    ui::label(g, {textX, r.y + pad * 0.5f, textW, r.h * 0.5f - pad * 0.5f}, v.title,
              ui::gameFont(g.px(15.0f)));
    ui::TextStyle hint = ui::gameFont(g.px(12.0f), v.inRange ? ui::kWhite : kOutOfRange);
    const f32 hintW = c.draw.measure(v.hint, hint.font, hint.size);
    if (hintW > textW && hintW > 0.0f) { // a longer translation: a little smaller before it is shortened
        hint.size *= std::max(0.75f, textW / hintW);
    }
    ui::label(g, {textX, r.centerY(), textW, r.h * 0.5f - pad * 0.5f}, v.hint, hint);
    return result;
}

} // namespace vd
