#include "app/RaidAlertWindows.h"

#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>

namespace vd {
namespace {

using ui::Gui;
using ui::Rect;

constexpr u64 kFrameArt = assetPathHash("Interface\\RaidFrame\\UI-ReadyCheckFrame.blp");
// The frame's units in dp: as the StaticPopups (app/PopupWindows), its 12-unit text at the dialogs' 15 dp.
constexpr f32 kUnitDp = 1.25f;
constexpr f32 kWidth = 323.0f,
              kHeight = 97.0f; // ReadyCheckFrame's size (and its art's, TexCoords 0..0.6309, 0..0.7578)

} // namespace

Rect readyCheckFrameRect(const DialogContext& c, const ui::Rect& avoid, f32 minTop) {
    const f32 u = c.ui.px(kUnitDp);
    const Rect& safe = c.safeArea;
    const f32 margin = c.ui.px(8.0f);
    // Narrower screens shrink it whole (the cover screen is ~880 dp wide: it fits at full size).
    const f32 scale = std::min(1.0f, (safe.w - 2.0f * margin) / (kWidth * u));
    const f32 w = kWidth * u * scale, h = kHeight * u * scale;
    // CENTER, 0, -10 of UIParent; moved left of the touch HUD's action buttons where they reach it (a short
    // screen), as the UIErrorsFrame keeps clear of the chat.
    Rect r{safe.centerX() - w * 0.5f, safe.centerY() + 10.0f * u * scale - h * 0.5f, w, h};
    if (r.y < minTop) { // a short screen: under the raid warning frames rather than behind them
        r.y = std::max(safe.y, std::min(minTop, safe.bottom() - margin - h));
    }
    if (avoid.w > 0.0f && r.right() > avoid.x && r.x < avoid.right() && r.bottom() > avoid.y &&
        r.y < avoid.bottom()) {
        r.x = std::max(safe.x + margin, avoid.x - margin - w);
    }
    return r;
}

ReadyCheckChoice readyCheckFrame(DialogContext& c, const ReadyCheckView& view, const ui::Rect& avoid,
                                 f32 minTop) {
    VD_PROFILE_ZONE("ReadyCheckFrame::draw");
    Gui g{c.ui, c.draw};
    const Rect f = readyCheckFrameRect(c, avoid, minTop);
    const f32 u = f.w / kWidth; // logical pixels per unit of the frame
    const auto at = [&](f32 x, f32 y, f32 w, f32 h) { return Rect{f.x + x * u, f.y + y * u, w * u, h * u}; };

    // BACKGROUND: ReadyCheckPortrait under the art's hole; ARTWORK: the frame.
    g.draw.portrait(at(7.0f, 6.0f, 50.0f, 50.0f), view.portrait, view.portraitFallback);
    g.draw.sprite(f, kFrameArt, ui::kWhite, {0.0f, 0.0f, 0.630859375f, 0.7578125f});

    // ReadyCheckFrameText: GameFontNormal, its TOP 15 units right of the frame's centre and 24 below its top.
    ui::TextStyle style = ui::gameFont(12.0f * u);
    style.align = ui::Align::Center;
    const f32 textWidth = 250.0f * u; // the art's inner field right of the portrait
    g.draw.wrappedText(
        {f.x + (kWidth * 0.5f + 15.0f) * u - textWidth * 0.5f, f.y + 24.0f * u, textWidth, 40.0f * u},
        view.text, style);

    ReadyCheckChoice choice = ReadyCheckChoice::None;
    const Rect yes = at(55.0f, kHeight - 18.0f - 24.0f, 109.0f, 24.0f);
    if (ui::button(g, ui::widgetId("readycheck.yes"), yes, text::get("YES"))) {
        choice = ReadyCheckChoice::Yes;
    }
    if (ui::button(g, ui::widgetId("readycheck.no"), {yes.right(), yes.y, yes.w, yes.h}, text::get("NO"))) {
        choice = ReadyCheckChoice::No;
    }
    return choice;
}

} // namespace vd
