#include "app/UnitTooltip.h"

#include "core/Profile.h"

#include <algorithm>

namespace vd {
namespace {

using ui::Rect;

// GameTooltipHeaderText (14) and GameTooltipText (12) at touch sizes; lines 2 units apart
// (GameTooltipTemplate), the text 10 units in from the border.
constexpr f32 kHeaderDp = 15.0f;
constexpr f32 kTextDp = 13.0f;
constexpr f32 kPadDp = 9.0f;
constexpr f32 kLineGapDp = 2.0f;
constexpr f32 kButtonDp = 36.0f;

ui::TextStyle lineStyle(const ui::Gui& g, usize index, u32 color) {
    ui::TextStyle style;
    style.font = ui::FontId::Friz;
    style.size = g.px(index == 0 ? kHeaderDp : kTextDp);
    style.color = color;
    return style;
}

f32 lineHeight(const ui::Gui& g, const game::TooltipLine& line, usize index, f32 innerWidth) {
    const ui::TextStyle style = lineStyle(g, index, line.leftColor);
    const f32 h = line.wrap ? g.draw.measureWrapped(innerWidth, line.left, style.font, style.size)
                            : g.draw.lineHeight(style.font, style.size);
    return h + g.px(kLineGapDp);
}

} // namespace

HudTooltipSize measureHudTooltip(const ui::Gui& g, const HudTooltipView& v, f32 maxWidth) {
    const f32 pad = g.px(kPadDp);
    const f32 maxInner = std::max(1.0f, maxWidth - 2.0f * pad);
    f32 inner = 0.0f;
    for (usize i = 0; i < v.lines.size(); ++i) {
        const game::TooltipLine& line = v.lines[i];
        const ui::TextStyle style = lineStyle(g, i, line.leftColor);
        f32 w = g.draw.measure(line.left, style.font, style.size);
        if (!line.right.empty()) {
            w += g.px(16.0f) + g.draw.measure(line.right, style.font, style.size);
        }
        inner = std::max(inner, std::min(w, maxInner));
    }
    if (!v.actions.empty()) {
        inner = std::max(inner, std::min(maxInner, g.px(120.0f) * static_cast<f32>(v.actions.size())));
    }
    HudTooltipSize size;
    size.w = inner + 2.0f * pad;
    size.h = 2.0f * pad - g.px(kLineGapDp);
    for (usize i = 0; i < v.lines.size(); ++i) {
        size.h += lineHeight(g, v.lines[i], i, inner);
    }
    if (!v.actions.empty()) {
        size.h += g.px(kButtonDp) + g.px(8.0f);
    }
    return size;
}

HudTooltipResult hudTooltip(ui::Gui& g, ui::WidgetId id, const Rect& r, const HudTooltipView& v) {
    VD_PROFILE_ZONE("UnitTooltip::draw");
    HudTooltipResult result;
    if (v.interactive) {
        result.closed = ui::tapped(ui::hit(g, id, r));
    }
    ui::tooltipPanel(g, r);
    const f32 pad = g.px(kPadDp);
    const f32 inner = r.w - 2.0f * pad;
    f32 y = r.y + pad;
    for (usize i = 0; i < v.lines.size(); ++i) {
        const game::TooltipLine& line = v.lines[i];
        ui::TextStyle style = lineStyle(g, i, line.leftColor);
        if (line.wrap) {
            g.draw.wrappedText({r.x + pad, y, inner, 1e6f}, line.left, style);
        } else {
            const f32 h = g.draw.lineHeight(style.font, style.size);
            const f32 baseline = ui::centeredBaseline(g.draw, style, y + h * 0.5f);
            g.draw.text(r.x + pad, baseline, line.left, style);
            if (!line.right.empty()) {
                ui::TextStyle right = lineStyle(g, i, line.rightColor);
                right.align = ui::Align::Right;
                g.draw.text(r.right() - pad, baseline, line.right, right);
            }
        }
        y += lineHeight(g, line, i, inner);
    }
    if (!v.actions.empty()) {
        const f32 gap = g.px(6.0f);
        const f32 bh = g.px(kButtonDp);
        const f32 count = static_cast<f32>(v.actions.size());
        const f32 bw = (inner - gap * (count - 1.0f)) / count;
        for (usize i = 0; i < v.actions.size(); ++i) {
            const Rect button{r.x + pad + static_cast<f32>(i) * (bw + gap), r.bottom() - pad - bh, bw, bh};
            if (ui::button(g, ui::widgetId("hudTooltip.action", id + i), button, v.actions[i])) {
                result.action = static_cast<i32>(i);
                result.closed = false;
            }
        }
    }
    return result;
}

Rect defaultTooltipAnchor(const ui::Gui& g, const Rect& safe, const Rect& buttons, f32 topLimit,
                          HudTooltipSize size) {
    const f32 gap = g.px(8.0f);
    const f32 w = std::min(size.w, safe.w - 2.0f * gap);
    const f32 h = std::min(size.h, safe.h - 2.0f * gap);
    // Above the action buttons, right-aligned with the screen (the client's corner).
    const f32 aboveY = (buttons.h > 0.0f ? buttons.y : safe.bottom()) - gap - h;
    if (aboveY >= topLimit) {
        return {safe.right() - gap - w, aboveY, w, h};
    }
    // Beside them, on their left, level with their bottom (clear of the experience bar under them).
    const f32 right = buttons.w > 0.0f ? buttons.x - gap : safe.right() - gap;
    const f32 bottom =
        buttons.h > 0.0f ? std::min(buttons.bottom(), safe.bottom() - gap) : safe.bottom() - gap;
    return {std::max(safe.x + gap, right - w), std::max(safe.y + gap, bottom - h), w, h};
}

Rect besideTooltipAnchor(const ui::Gui& g, const Rect& safe, const Rect& owner, bool right,
                         HudTooltipSize size) {
    const f32 gap = g.px(6.0f);
    const f32 w = std::min(size.w, safe.w - 2.0f * gap);
    const f32 h = std::min(size.h, safe.h - 2.0f * gap);
    f32 x = right ? owner.right() + gap : owner.x - gap - w;
    x = std::clamp(x, safe.x + gap, std::max(safe.x + gap, safe.right() - gap - w));
    // Top-aligned with the owner; above it when it sits in the lower half (the rest tick on the bottom edge).
    f32 y = owner.centerY() > safe.centerY() ? owner.y - gap - h : owner.y;
    y = std::clamp(y, safe.y + gap, std::max(safe.y + gap, safe.bottom() - gap - h));
    return {x, y, w, h};
}

} // namespace vd
