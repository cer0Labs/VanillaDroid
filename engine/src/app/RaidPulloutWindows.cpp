#include "app/RaidPulloutWindows.h"

#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <optional>

namespace vd {
namespace {

using ui::Align;
using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::TextStyle;
using ui::widgetId;

constexpr u64 kHealthBarFrame = assetPathHash("Interface\\RaidFrame\\UI-RaidFrame-HealthBar.blp");
constexpr u64 kStatusBar = assetPathHash("Interface\\TargetingFrame\\UI-StatusBar.blp");
constexpr u64 kDebuffOverlays = assetPathHash("Interface\\Buttons\\UI-Debuff-Overlays.blp");
constexpr u64 kTooltipBackground = assetPathHash("Interface\\Tooltips\\UI-Tooltip-Background.blp");
constexpr u64 kTooltipBorder = assetPathHash("Interface\\Tooltips\\UI-Tooltip-Border.blp");
// MenuBackdrop: TOOLTIP_DEFAULT_BACKGROUND_COLOR (0.09, 0.09, 0.19), the border at (0.5, 0.5, 0.5), the whole
// at 0.7.
constexpr f32 kBackdropAlpha = 0.7f;
constexpr u32 kBackdropColor = rgba(23, 23, 48);
constexpr u32 kBackdropBorder = rgba(128, 128, 128);

// A status bar filled to `value` (0..1) with UI-StatusBar in `color` (the part left empty shows what is under
// it).
void statusBar(Gui& g, const Rect& r, f32 value, u32 color) {
    const f32 v = std::clamp(value, 0.0f, 1.0f);
    if (v > 0.0f) {
        g.draw.sprite({r.x, r.y, r.w * v, r.h}, kStatusBar, color, {0.0f, 0.0f, v, 1.0f});
    }
}

} // namespace

RaidPulloutResult raidPullout(DialogContext& c, u32 index, const RaidPulloutView& v) {
    VD_PROFILE_ZONE("RaidPulloutWindows::pullout");
    Gui g{c.ui, c.draw};
    RaidPulloutGeometry at = v.at;
    at.members = static_cast<u32>(v.members.size());
    const f32 k = at.unit;
    RaidPulloutResult out;

    // One widget for all of it (title, frame, auras): taps are resolved by where they land.
    const Rect hit = at.hit();
    const ui::WidgetState state = ui::hit(g, widgetId("raid.pullout", index), hit);
    if (const ui::Touch* t = state.touch) {
        if (t->moved && (state.pressed || state.released)) {
            out.moving = true;
            out.fingerDx = t->x - t->startX;
            out.fingerDy = t->y - t->startY;
            out.dropped = state.released;
        }
        if (state.longPress) {
            out.held = true;
        }
        if (ui::tapped(state) && !t->moved) { // a drag released over the frame is no tap
            for (u32 i = 0; i < at.members && out.tapped < 0 && out.aura < 0; ++i) {
                for (u32 a = 0; a < v.members[i].auraCount && a < kRaidPulloutAuras; ++a) {
                    // The 2 x 2 grid's cells grown into their 1-unit gaps: one strip a finger can hit.
                    const Rect icon = at.aura(i, a);
                    if (icon.inset(-1.0f * k).contains(t->x, t->y)) {
                        out.auraMember = static_cast<i32>(i);
                        out.aura = static_cast<i32>(a);
                        out.auraRect = icon;
                    }
                }
                if (out.aura < 0 && at.row(i).contains(t->x, t->y)) {
                    out.tapped = static_cast<i32>(i);
                }
            }
        }
    }

    // MenuBackdrop: the tooltip's backdrop (background inset 5, edge 16), at 70%.
    const Rect frame = at.frame();
    if (v.background) {
        g.draw.sprite(frame.inset(5.0f * k), kTooltipBackground,
                      ui::withAlpha(kBackdropColor, kBackdropAlpha));
        g.draw.backdropBorder(frame, kTooltipBorder, std::min(16.0f * k, frame.h * 0.5f),
                              ui::withAlpha(kBackdropBorder, kBackdropAlpha));
    }
    // $parentName: GameFontNormalSmall, its bottom on the frame's top, centered.
    TextStyle titleStyle = ui::gameFont(10.0f * k, kRaidTitleColor);
    titleStyle.shadow = std::max(1.0f, g.px(1.0f));
    titleStyle.align = Align::Center;
    ui::label(g, at.title(), v.title, titleStyle);

    for (u32 i = 0; i < at.members; ++i) {
        const RaidPulloutMemberView& m = v.members[i];
        // The frame's art (ARTWORK), the bars over it (child StatusBars), the name.
        g.draw.sprite(at.barFrame(i), kHealthBarFrame, ui::kWhite, {0.0f, 0.0f, 1.0f, 0.5f});
        statusBar(g, at.health(i), m.health, m.healthColor);
        statusBar(g, at.mana(i), m.power, m.powerColor);
        TextStyle nameStyle = ui::gameFont(10.0f * k, m.nameColor);
        nameStyle.shadow = std::max(1.0f, g.px(1.0f));
        nameStyle.align = Align::Center;
        ui::label(g, at.name(i), m.name, nameStyle);
        for (u32 a = 0; a < m.auraCount && a < kRaidPulloutAuras; ++a) {
            const Rect icon = at.aura(i, a);
            if (const std::optional<u32> slot = c.resources.icon(m.auras[a].iconHash)) {
                g.draw.icon(icon, *slot, ui::kWhite, {0.0f, 0.0f, 1.0f, 1.0f});
            } else {
                g.draw.solid(icon, rgba(0, 0, 0, 120));
            }
            // The border: 13 units centered on the 12 of the icon (RaidBuffButtonTemplate's OnLoad).
            g.draw.sprite(icon.inset(-0.5f * k), kDebuffOverlays, m.auras[a].border,
                          {0.296875f, 0.0f, 0.5703125f, 0.515625f});
        }
    }
    return out;
}

} // namespace vd
