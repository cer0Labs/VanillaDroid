#include "app/WorldLabels.h"

#include <algorithm>
#include <cstdio>

namespace vd {
namespace {

using ui::Rect;
using ui::rgba;
using ui::withAlpha;

// WoW.exe's name plates (the four paths together in its strings): the border, the target's glow, the fill and the
// skull of a level too high to show.
constexpr u64 kBarFill = assetPathHash("Interface\\TargetingFrame\\UI-TargetingFrame-BarFill.blp");
constexpr u64 kNameplateGlow = assetPathHash("Interface\\Tooltips\\Nameplate-Glow.blp");
constexpr u64 kSkull = assetPathHash("Interface\\TargetingFrame\\UI-TargetingFrame-Skull.blp");

// Where the text's baseline goes for a line whose bottom is `bottom`.
f32 baselineAbove(const ui::DrawList& draw, const ui::TextStyle& style, f32 bottom) {
    return bottom - (draw.lineHeight(style.font, style.size) - draw.ascent(style.font, style.size));
}

} // namespace

f32 drawUnitLabel(ui::DrawList& draw, f32 dp, const UnitLabelView& v) {
    ui::TextStyle style;
    style.size = 11.0f * dp;
    style.outline = std::max(1.0f, dp);
    style.align = ui::Align::Center;
    style.color = v.color;
    if (v.plate) {
        // Nameplate-Border (128 x 32 texels): the bar's inside is texels 5..106 x 20..27, the level's box is
        // centred on (116, 23.5), the art starts at row 15; the name sits in the empty rows above.
        const f32 w = 100.0f * dp;
        const f32 texel = w / 128.0f;
        const Rect plate{v.x - w * 0.5f, v.y - 32.0f * texel, w, 32.0f * texel};
        const Rect bar{plate.x + 5.0f * texel, plate.y + 20.0f * texel, 101.0f * texel, 7.0f * texel};
        draw.sprite({bar.x, bar.y, bar.w * std::clamp(v.health, 0.0f, 1.0f), bar.h}, kBarFill, v.barColor,
                    {0.0f, 0.0f, std::clamp(v.health, 0.0f, 1.0f), 1.0f});
        draw.sprite(plate, kNameplateBorder);
        if (v.targeted) {
            draw.sprite(plate, kNameplateGlow); // additive (no alpha in the client: baked with its brightness)
        }
        if (v.level > 0) {
            char level[8];
            std::snprintf(level, sizeof(level), "%u", v.level);
            ui::TextStyle levelStyle = style;
            levelStyle.size = 8.5f * dp;
            levelStyle.color = v.levelColor;
            draw.text(plate.x + 116.0f * texel, plate.y + 23.5f * texel + levelStyle.size * 0.33f, level,
                      levelStyle);
        } else {
            const f32 size = 12.0f * texel; // the skull fills the level box (14 texels high in Nameplate-Border)
            draw.sprite({plate.x + 116.0f * texel - size * 0.5f, plate.y + 23.5f * texel - size * 0.5f, size, size},
                        kSkull);
        }
        style.color = v.targeted ? ui::rgba(255, 255, 0) : ui::kWhite;
        const f32 nameBottom = plate.y + 15.0f * texel;
        draw.text(v.x, baselineAbove(draw, style, nameBottom), v.name, style);
        return nameBottom - draw.lineHeight(style.font, style.size);
    }
    f32 bottom = v.y;
    if (!v.title.empty()) {
        char title[72];
        std::snprintf(title, sizeof(title), "<%.*s>", static_cast<int>(std::min<usize>(v.title.size(), 64)),
                      v.title.data());
        ui::TextStyle titleStyle = style;
        titleStyle.size = 9.5f * dp;
        draw.text(v.x, baselineAbove(draw, titleStyle, bottom), title, titleStyle);
        bottom -= draw.lineHeight(titleStyle.font, titleStyle.size) * 0.95f;
    }
    draw.text(v.x, baselineAbove(draw, style, bottom), v.name, style);
    return bottom - draw.lineHeight(style.font, style.size);
}

f32 unitLabelHeight(const ui::DrawList& draw, f32 dp, const UnitLabelView& v) {
    // drawUnitLabel()'s layout, measured only.
    const ui::FontId font = ui::TextStyle{}.font;
    const f32 size = 11.0f * dp;
    if (v.plate) {
        return 17.0f * (100.0f * dp / 128.0f) + draw.lineHeight(font, size);
    }
    f32 height = draw.lineHeight(font, size);
    height += v.title.empty() ? 0.0f : draw.lineHeight(font, 9.5f * dp) * 0.95f;
    return height;
}

ChatBubbleLayout chatBubbleLayout(const ui::DrawList& draw, f32 unit, f32 tipX, f32 tipY,
                                  std::string_view text, const ui::Rect& bounds) {
    // The backdrop's cells (32 texels) and the tail (32 x 32, its tip at texel (22, 21)) at half a unit per
    // texel; the text inset by 10 units, wrapped at 300.
    const f32 size = kChatBubbleTextUnits * unit;
    const f32 edge = 16.0f * unit;
    const f32 pad = 10.0f * unit;
    const f32 maxWidth = 300.0f * unit;
    const f32 textW = std::min(draw.measure(text, ui::FontId::Friz, size), maxWidth) + 1.0f;
    const f32 textH = draw.measureWrapped(textW, text, ui::FontId::Friz, size);
    const f32 w = std::max(textW + 2.0f * pad, 2.0f * edge + 4.0f * unit);
    const f32 h = std::max(textH + 1.6f * pad, 2.0f * edge);
    ChatBubbleLayout l;
    const f32 tail = 16.0f * unit;
    l.tail = {tipX - tail * (22.0f / 32.0f), tipY - tail * (21.0f / 32.0f), tail, tail};
    // The tail's top rows cover the bubble's bottom border where they meet.
    l.bubble = {l.tail.centerX() - w * 0.5f, l.tail.y + 3.0f * unit - h, w, h};
    const f32 lowest = std::max(bounds.x, l.tail.right() + edge * 0.5f - w);
    const f32 highest = std::min(bounds.right() - w, l.tail.x - edge * 0.5f);
    if (lowest <= highest) {
        l.bubble.x = std::clamp(l.bubble.x, lowest, highest);
    }
    l.text = {l.bubble.x + (w - textW) * 0.5f, l.bubble.y + (h - textH) * 0.5f, textW, textH};
    return l;
}

void drawChatBubble(ui::DrawList& draw, f32 unit, const ChatBubbleLayout& l, std::string_view text, u32 color,
                    f32 alpha) {
    const u32 tint = withAlpha(ui::kWhite, alpha);
    const f32 edge = std::min({16.0f * unit, l.bubble.w * 0.5f, l.bubble.h * 0.5f});
    draw.sprite(l.bubble.inset(edge), kChatBubbleBackground, tint);
    // The border: ChatBubble-Backdrop's 8 cells like any backdrop (left, right, top, bottom, the corners),
    // drawn here piece by piece so it fades with the text.
    if (const std::optional<ui::Sprite> border = draw.assets()->sprite(kChatBubbleBackdrop)) {
        const auto cell = [](u32 i) { return ui::TexCoords{i / 8.0f, 0.0f, (i + 1) / 8.0f, 1.0f}; };
        const f32 x0 = l.bubble.x, y0 = l.bubble.y, x1 = l.bubble.right(), y1 = l.bubble.bottom();
        draw.sprite({x0, y0 + edge, edge, y1 - y0 - 2.0f * edge}, *border, tint, cell(0));
        draw.sprite({x1 - edge, y0 + edge, edge, y1 - y0 - 2.0f * edge}, *border, tint, cell(1));
        // Top and bottom: the strips laid along x (their length runs across the texture's v).
        const f32 top[8] = {x0 + edge, y0, x0 + edge, y0 + edge, x1 - edge, y0 + edge, x1 - edge, y0};
        draw.spriteQuad(top, *border, tint, cell(2));
        const f32 bottom[8] = {x0 + edge, y1 - edge, x0 + edge, y1, x1 - edge, y1, x1 - edge, y1 - edge};
        draw.spriteQuad(bottom, *border, tint, cell(3));
        draw.sprite({x0, y0, edge, edge}, *border, tint, cell(4));
        draw.sprite({x1 - edge, y0, edge, edge}, *border, tint, cell(5));
        draw.sprite({x0, y1 - edge, edge, edge}, *border, tint, cell(6));
        draw.sprite({x1 - edge, y1 - edge, edge, edge}, *border, tint, cell(7));
    }
    draw.sprite(l.tail, kChatBubbleTail, tint);
    ui::TextStyle style;
    style.size = kChatBubbleTextUnits * unit;
    style.color = withAlpha(color, alpha);
    style.align = ui::Align::Center;
    draw.wrappedText(l.text, text, style);
}

} // namespace vd
