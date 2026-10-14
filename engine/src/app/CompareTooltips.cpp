#include "app/CompareTooltips.h"

#include "app/DialogKit.h"
#include "core/Profile.h"
#include "ui/Widgets.h"

#include <algorithm>

namespace vd {
namespace {

// The item tooltip's sizes (app/InventoryWindows): its name size for the name line, the small size for the
// rest.
struct Sizes {
    f32 pad, name, small;
};
Sizes sizes(const DialogContext& c) {
    const kit::Metrics m = kit::metrics(c);
    return {c.ui.px(10.0f), m.text * 1.12f, m.small * 1.08f};
}

f32 lineSize(const Sizes& s, usize i) {
    return i == 1 ? s.name : s.small; // ShoppingTooltipTemplate: TextLeft2 in GameFontNormal
}

} // namespace

f32 compareTooltipHeight(const DialogContext& c, std::span<const TooltipLineView> lines, f32 width) {
    const Sizes s = sizes(c);
    const f32 textW = width - 2.0f * s.pad;
    f32 h = 0.0f;
    for (usize i = 0; i < lines.size(); ++i) {
        const f32 size = lineSize(s, i);
        const f32 lh = lines[i].wrap ? c.draw.measureWrapped(textW, lines[i].left, ui::FontId::Friz, size)
                                     : c.draw.lineHeight(ui::FontId::Friz, size);
        h += lh + (lines[i].gapBefore ? size * 0.6f : 0.0f) + size * 0.12f;
    }
    return std::min(c.safeArea.h - c.ui.px(16.0f), h + 2.0f * s.pad);
}

void compareTooltip(DialogContext& c, u32 index, const ui::Rect& r, std::span<const TooltipLineView> lines) {
    VD_PROFILE_ZONE("CompareTooltips::draw");
    ui::Gui g{c.ui, c.draw};
    const Sizes s = sizes(c);
    ui::hit(g, ui::widgetId("tooltip.compare", index), r);
    ui::tooltipPanel(g, r);
    g.draw.pushClip(r);
    const f32 textW = r.w - 2.0f * s.pad;
    const f32 x = r.x + s.pad;
    f32 y = r.y + s.pad;
    for (usize i = 0; i < lines.size(); ++i) {
        const TooltipLineView& line = lines[i];
        const f32 size = lineSize(s, i);
        if (line.gapBefore) {
            y += size * 0.6f;
        }
        const ui::TextStyle style = ui::gameFont(size, line.leftColor);
        if (line.wrap) {
            y += g.draw.wrappedText({x, y, textW, 1e6f}, line.left, style) + size * 0.12f;
            continue;
        }
        const f32 lh = g.draw.lineHeight(ui::FontId::Friz, size);
        const f32 baseline = ui::centeredBaseline(g.draw, style, y + lh * 0.5f);
        const f32 leftW = g.draw.text(x, baseline, line.left, style);
        if (line.money > 0) {
            ui::money(g, x + leftW + g.px(6.0f), y + lh * 0.5f, line.money, size, ui::Align::Left);
        }
        if (!line.right.empty()) {
            ui::TextStyle right = ui::gameFont(size, line.rightColor);
            right.align = ui::Align::Right;
            g.draw.text(x + textW, baseline, line.right, right);
        }
        y += lh + size * 0.12f;
    }
    g.draw.popClip();
}

} // namespace vd
