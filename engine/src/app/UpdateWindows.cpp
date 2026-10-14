#include "app/UpdateWindows.h"

#include "app/GlueWindows.h"
#include "core/Hash.h"
#include "core/Profile.h"

#include <algorithm>

namespace vd {
namespace {

using ui::Align;
using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::TextStyle;
using ui::widgetId;
using ui::WidgetState;

// The art of app/GlueWindows' GlueDialog: UI-DialogBox inside its border, GluePanelButton (texels 0..148 x
// 0..48 of 256 x 64; the highlight 0..160 x 0..44, added), GlueFontNormal's gold, GlueFontHighlight's white.
constexpr u64 kDialogBackground = assetPathHash("Interface\\DialogFrame\\UI-DialogBox-Background.blp");
constexpr u64 kDialogBorder = assetPathHash("Interface\\DialogFrame\\UI-DialogBox-Border.blp");
constexpr u64 kButtonUp = assetPathHash("Interface\\Glues\\Common\\Glue-Panel-Button-Up.blp");
constexpr u64 kButtonDown = assetPathHash("Interface\\Glues\\Common\\Glue-Panel-Button-Down.blp");
constexpr u64 kButtonHighlight = assetPathHash("Interface\\Glues\\Common\\Glue-Panel-Button-Highlight.blp");
constexpr ui::TexCoords kButtonUv{0.0f, 0.0f, 0.578125f, 0.75f};
constexpr ui::TexCoords kButtonHighlightUv{0.0f, 0.0f, 0.625f, 0.6875f};
// CastingBarFrame: UI-StatusBar in its casting colour under UI-CastingBar-Border (256 x 64, 28 units above a
// 195 x 13 bar), the spark at the fill's edge.
constexpr u64 kStatusBar = assetPathHash("Interface\\TargetingFrame\\UI-StatusBar.blp");
constexpr u64 kCastBorder = assetPathHash("Interface\\CastingBar\\UI-CastingBar-Border.blp");
constexpr u64 kCastSpark = assetPathHash("Interface\\CastingBar\\UI-CastingBar-Spark.blp");
constexpr u32 kGlueGold = rgba(255, 199, 0);
constexpr u32 kGlueWhite = rgba(255, 255, 255);
constexpr u32 kCastColor = rgba(255, 178, 0);

TextStyle glueFont(const GlueLayout& L, f32 size, u32 color) {
    TextStyle style;
    style.font = ui::FontId::Friz;
    style.size = L.u(size);
    style.color = color;
    style.outline = std::max(0.75f, L.u(0.9f));
    style.shadow = std::max(1.0f, L.u(size >= 18.0f ? 2.0f : 1.0f));
    style.align = Align::Center;
    return style;
}

// A finger's size at least, centred on the art.
Rect touchRect(const Gui& g, const Rect& r) {
    const f32 min = g.px(ui::kTouchDp);
    Rect out = r;
    if (out.w < min) {
        out.x -= (min - out.w) * 0.5f;
        out.w = min;
    }
    if (out.h < min) {
        out.y -= (min - out.h) * 0.5f;
        out.h = min;
    }
    return out;
}

// GlueDialogButtonTemplate: the panel button's art, its text 2 units up, white while held.
bool dialogButton(Gui& g, const GlueLayout& L, ui::WidgetId id, const Rect& r, std::string_view text) {
    const Rect target = touchRect(g, r);
    const WidgetState state = ui::hit(g, id, target);
    const bool down = ui::held(state, target);
    g.draw.sprite(r, down ? kButtonDown : kButtonUp, ui::kWhite, kButtonUv);
    if (down) {
        g.draw.spriteAdditive(r, kButtonHighlight, ui::kWhite, kButtonHighlightUv);
    }
    ui::label(g, {r.x, r.y - L.u(2.0f), r.w, r.h}, text, glueFont(L, 15.0f, down ? kGlueWhite : kGlueGold));
    return ui::tapped(state);
}

} // namespace

UpdateDialogResult updateDialog(DialogContext& c, const UpdateDialogView& v) {
    VD_PROFILE_ZONE("UpdateWindows::dialog");
    Gui g{c.ui, c.draw};
    const GlueLayout L = glueLayout(c);
    UpdateDialogResult result;
    // As GlueDialog nothing under it reacts; unlike it, the screen dims (the login's fields showed through
    // the text).
    ui::modalBlocker(g, widgetId(v.name, 100), 0.5f);
    const Rect area = L.keyboard ? L.visible : L.screen;
    u32 buttons = 0;
    for (const std::string_view b : v.buttons) {
        buttons += b.empty() ? 0u : 1u;
    }
    // GlueDialog's measures (512 wide, its text 440, 16 + text + 8 + button + 16 high), 600 wide for three
    // buttons.
    const f32 width = std::min(L.u(buttons > 2 ? 600.0f : 512.0f), area.w - L.u(8.0f));
    const f32 textW = std::min(L.u(buttons > 2 ? 520.0f : 440.0f), width - L.u(40.0f));
    const TextStyle large = glueFont(L, 18.0f, kGlueGold);
    const TextStyle small = glueFont(L, 12.0f, kGlueWhite);
    const f32 textH = g.draw.measureWrapped(textW, v.text, large.font, large.size);
    const f32 detailH =
        v.detail.empty() ? 0.0f : g.draw.measureWrapped(textW, v.detail, small.font, small.size);
    const f32 barH =
        v.progress ? L.u(13.0f + 28.0f + 16.0f) : 0.0f; // the border's top over the bar, the text under
    const f32 buttonsH = buttons > 0 ? L.u(40.0f + 16.0f) : L.u(8.0f);
    const f32 height = std::min(area.h, L.u(16.0f) + textH + (detailH > 0.0f ? L.u(10.0f) + detailH : 0.0f) +
                                            L.u(8.0f) + barH + buttonsH);
    const Rect panel{area.centerX() - width * 0.5f, area.centerY() - height * 0.5f, width, height};
    // The dialogs' backdrop: UI-DialogBox-Background inside UI-DialogBox-Border (insets 11/12/12/11, edge
    // 32), over black (the background is see-through: the login's fields showed through the text).
    const Rect inside{panel.x + L.u(11.0f), panel.y + L.u(12.0f), panel.w - L.u(23.0f), panel.h - L.u(23.0f)};
    g.draw.solid(inside, rgba(0, 0, 0, 200));
    g.draw.sprite(inside, kDialogBackground);
    g.draw.backdropBorder(panel, kDialogBorder, L.u(32.0f));
    g.ui.blocker(widgetId(v.name, 101), panel);

    f32 top = panel.y + L.u(16.0f);
    g.draw.wrappedText({panel.centerX() - textW * 0.5f, top, textW, textH + 1.0f}, v.text, large);
    top += textH;
    if (detailH > 0.0f) {
        top += L.u(10.0f);
        g.draw.wrappedText({panel.centerX() - textW * 0.5f, top, textW, detailH + 1.0f}, v.detail, small);
        top += detailH;
    }
    top += L.u(8.0f);
    if (v.progress) {
        const f32 fill = std::clamp(v.fraction, 0.0f, 1.0f);
        const Rect bar{panel.centerX() - L.u(195.0f) * 0.5f, top + L.u(28.0f) - L.u(14.0f), L.u(195.0f),
                       L.u(13.0f)};
        g.draw.solid(bar, rgba(0, 0, 0, 128));
        g.draw.sprite({bar.x, bar.y, bar.w * fill, bar.h}, kStatusBar, kCastColor, {0.0f, 0.0f, fill, 1.0f});
        g.draw.sprite({bar.centerX() - L.u(128.0f), bar.y - L.u(28.0f), L.u(256.0f), L.u(64.0f)},
                      kCastBorder);
        if (fill > 0.0f && fill < 1.0f) {
            g.draw.spriteAdditive(
                {bar.x + bar.w * fill - L.u(16.0f), bar.centerY() - L.u(18.0f), L.u(32.0f), L.u(32.0f)},
                kCastSpark);
        }
        if (!v.progressText.empty()) {
            ui::label(g, {panel.x, bar.bottom() + L.u(6.0f), panel.w, L.u(16.0f)}, v.progressText,
                      glueFont(L, 12.0f, kGlueWhite));
        }
    }
    if (buttons > 0) {
        // In a row at the bottom, 13 units apart (GlueDialog's two: Button1 6 units left of the middle).
        const f32 gap = L.u(13.0f);
        const f32 w = std::min(L.u(buttons > 2 ? 170.0f : 200.0f),
                               (panel.w - L.u(40.0f) - gap * static_cast<f32>(buttons - 1)) /
                                   static_cast<f32>(buttons));
        const f32 h = L.u(40.0f);
        const f32 rowW = w * static_cast<f32>(buttons) + gap * static_cast<f32>(buttons - 1);
        f32 x = panel.centerX() - rowW * 0.5f;
        const f32 y = panel.bottom() - L.u(16.0f) - h;
        for (u32 i = 0; i < v.buttons.size(); ++i) {
            if (v.buttons[i].empty()) {
                continue;
            }
            if (dialogButton(g, L, widgetId(v.name, i + 1), {x, y, w, h}, v.buttons[i])) {
                result.button = static_cast<i32>(i);
            }
            x += w + gap;
        }
    }
    return result;
}

} // namespace vd
