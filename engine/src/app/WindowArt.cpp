#include "app/WindowArt.h"

#include "core/Hash.h"
#include "core/Profile.h"

#include <algorithm>
#include <array>
#include <optional>

namespace vd::art {
namespace {

using ui::Align;
using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::TextStyle;

// CharacterFrameTabButtonTemplate
constexpr u64 kTabActive = assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-ActiveTab.blp");
constexpr u64 kTabInactive = assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-InActiveTab.blp");
constexpr u64 kTabHighlight = assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-Tab-Highlight.blp");
// UIRadioButtonTemplate
constexpr u64 kRadio = assetPathHash("Interface\\Buttons\\UI-RadioButton.blp");
// The spell book's page buttons (SpellBookPrevPageButton, BrowsePrevPageButton...)
constexpr u64 kPrevUp = assetPathHash("Interface\\Buttons\\UI-SpellbookIcon-PrevPage-Up.blp");
constexpr u64 kPrevDown = assetPathHash("Interface\\Buttons\\UI-SpellbookIcon-PrevPage-Down.blp");
constexpr u64 kPrevDisabled = assetPathHash("Interface\\Buttons\\UI-SpellbookIcon-PrevPage-Disabled.blp");
constexpr u64 kNextUp = assetPathHash("Interface\\Buttons\\UI-SpellbookIcon-NextPage-Up.blp");
constexpr u64 kNextDown = assetPathHash("Interface\\Buttons\\UI-SpellbookIcon-NextPage-Down.blp");
constexpr u64 kNextDisabled = assetPathHash("Interface\\Buttons\\UI-SpellbookIcon-NextPage-Disabled.blp");
constexpr u64 kMouseHighlight = assetPathHash("Interface\\Buttons\\UI-Common-MouseHilight.blp");
// StatusBars of the skills art
constexpr u64 kSkillsBar = assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-Skills-Bar.blp");
constexpr u64 kSkillsBarBorder =
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-Skills-BarBorder.blp");
// TradeHighlightTemplate
constexpr u64 kTradeHighlight = assetPathHash("Interface\\TradeFrame\\UI-TradeFrame-Highlight.blp");

constexpr u32 kMaxTabs = 8;
constexpr f32 kColumnOverlap = 14.0f; // FriendsFrameTab2..4: LEFT of the one before's RIGHT, -14

// `uv` of an atlas texture over `dst` turned a quarter counter-clockwise: the texture's top edge along dst's left side,
// its left end at dst's bottom.
void spriteTurned(ui::DrawList& draw, const Rect& dst, u64 hash, ui::TexCoords uv, bool additive = false) {
    if (const std::optional<ui::Sprite> s = draw.assets()->sprite(hash)) {
        const f32 corners[8] = {dst.x, dst.bottom(), dst.x, dst.y, dst.right(), dst.y, dst.right(), dst.bottom()};
        draw.spriteQuad(corners, *s, ui::kWhite, uv, additive);
    }
}

// Each tab's length along the column (PanelTemplates_TabResize: the text, the padding, the two 20-unit ends).
u32 columnLengths(const Gui& g, f32 u, std::span<const std::string_view> tabs, f32 textSize, f32 minLength,
                  f32 padding, std::array<f32, kMaxTabs>& lengths, f32& total) {
    const u32 count = static_cast<u32>(std::min<usize>(tabs.size(), kMaxTabs));
    total = 0.0f;
    for (u32 i = 0; i < count; ++i) {
        lengths[i] = std::max(minLength, g.draw.measure(tabs[i], ui::FontId::Friz, textSize) + (padding + 40.0f) * u);
        total += lengths[i] - (i > 0 ? kColumnOverlap * u : 0.0f);
    }
    return count;
}

} // namespace

i32 characterTabBar(Gui& g, std::string_view name, const Rect& r, std::span<const std::string_view> tabs,
                    u32 selected, f32 textSize, f32 minWidth, f32 padding) {
    VD_PROFILE_ZONE("WindowArt::characterTabBar");
    const u32 count = static_cast<u32>(std::min<usize>(tabs.size(), kMaxTabs));
    const f32 u = r.h / 32.0f; // one unit of the tab's 32-unit art
    const f32 overlap = 15.0f * u;
    // PanelTemplates_TabResize: the text, the padding, the two 20-unit ends; then the 15-unit overlaps.
    std::array<f32, kMaxTabs> widths{};
    f32 total = 0.0f;
    for (u32 i = 0; i < count; ++i) {
        widths[i] =
            std::max(minWidth, g.draw.measure(tabs[i], ui::FontId::Friz, textSize) + (padding + 40.0f) * u);
        total += widths[i] - (i > 0 ? overlap : 0.0f);
    }
    const f32 squeeze = total > r.w && total > 0.0f ? r.w / total : 1.0f;
    i32 tapped = -1;
    f32 x = r.x;
    for (u32 i = 0; i < count; ++i) {
        const f32 w = widths[i] * squeeze;
        const bool active = i == selected;
        // The halves of the overlaps belong to each neighbour.
        const f32 hitLeft = i > 0 ? overlap * squeeze * 0.5f : 0.0f;
        const f32 hitRight = i + 1 < count ? overlap * squeeze * 0.5f : 0.0f;
        const Rect hit{x + hitLeft, r.y, w - hitLeft - hitRight, r.h};
        const ui::WidgetState state = ui::hit(g, ui::widgetId(name, 100 + i), hit);
        // Flipped (v 1 -> 0): the tab rises from the pane under it; the selected one reaches 5 units into it.
        const Rect art{x, r.y + (active ? 5.0f * u : 0.0f), w, r.h};
        const f32 cap = std::min(20.0f * u, w * 0.5f);
        const u64 texture = active ? kTabActive : kTabInactive;
        g.draw.sprite({art.x, art.y, cap, art.h}, texture, ui::kWhite, {0.0f, 1.0f, 0.15625f, 0.0f});
        g.draw.sprite({art.x + cap, art.y, art.w - 2.0f * cap, art.h}, texture, ui::kWhite,
                      {0.15625f, 1.0f, 0.84375f, 0.0f});
        g.draw.sprite({art.right() - cap, art.y, cap, art.h}, texture, ui::kWhite,
                      {0.84375f, 1.0f, 1.0f, 0.0f});
        if (!active && ui::held(state, hit)) { // the HighlightTexture, 10 units in from either end
            g.draw.spriteAdditive({x + 10.0f * u, r.y + 2.0f * u, w - 20.0f * u, r.h}, kTabHighlight);
        }
        TextStyle style =
            ui::gameFont(textSize * std::min(1.0f, squeeze * 1.1f), active ? ui::kHighlight : ui::kGold);
        style.align = Align::Center;
        ui::label(g, {x + 10.0f * u, r.y + 3.0f * u, w - 20.0f * u, 29.0f * u}, tabs[i], style);
        if (ui::tapped(state) && !active) { // PanelTemplates_SelectTab disables the selected one
            tapped = static_cast<i32>(i);
        }
        x += w - overlap * squeeze;
    }
    return tapped;
}

i32 characterTabColumn(Gui& g, std::string_view name, const Rect& r, std::span<const std::string_view> tabs,
                       u32 selected, f32 textSize, f32 minLength, u32 disabled, f32 minHitWidth, f32 padding) {
    VD_PROFILE_ZONE("WindowArt::characterTabColumn");
    const f32 u = r.w / 32.0f; // one unit of the tab's 32-unit art, across the column
    std::array<f32, kMaxTabs> lengths{};
    f32 total = 0.0f;
    const u32 count = columnLengths(g, u, tabs, textSize, minLength, padding, lengths, total);
    const f32 squeeze = total > r.h && total > 0.0f ? r.h / total : 1.0f;
    const f32 overlap = kColumnOverlap * u * squeeze;
    i32 tapped = -1;
    f32 y = r.y;
    for (u32 i = 0; i < count; ++i) {
        const f32 len = lengths[i] * squeeze;
        const bool active = i == selected;
        const bool off = (disabled >> i) & 1u;
        // The halves of the overlaps belong to each neighbour; the selected one's reach into the frame is its own.
        const f32 hitTop = i > 0 ? overlap * 0.5f : 0.0f;
        const f32 hitBottom = i + 1 < count ? overlap * 0.5f : 0.0f;
        const f32 hitLeft = r.x - (active ? 5.0f * u : 0.0f);
        const Rect hit{hitLeft, y + hitTop, std::max(r.right(), r.x + minHitWidth) - hitLeft, len - hitTop - hitBottom};
        const ui::WidgetState state = off ? ui::WidgetState{} : ui::hit(g, ui::widgetId(name, 100 + i), hit);
        // Turned, the tab's left end (u 0) is at the bottom: the caps of its 3-slice swap places.
        const Rect art{r.x - (active ? 5.0f * u : 0.0f), y, r.w, len};
        const f32 cap = std::min(20.0f * u, len * 0.5f);
        const u64 texture = active ? kTabActive : kTabInactive;
        spriteTurned(g.draw, {art.x, art.y, art.w, cap}, texture, {0.84375f, 0.0f, 1.0f, 1.0f});
        spriteTurned(g.draw, {art.x, art.y + cap, art.w, art.h - 2.0f * cap}, texture, {0.15625f, 0.0f, 0.84375f, 1.0f});
        spriteTurned(g.draw, {art.x, art.bottom() - cap, art.w, cap}, texture, {0.0f, 0.0f, 0.15625f, 1.0f});
        if (!active && !off && ui::held(state, hit)) { // the HighlightTexture, 10 units in from either end
            spriteTurned(g.draw, {r.x + 2.0f * u, y + 10.0f * u, r.w, len - 20.0f * u}, kTabHighlight, {}, true);
        }
        // GameFontHighlightSmall on the selected one, GameFontNormalSmall on the others, gray when disabled; across
        // an unselected tab's visible 24 units (its art's 2..26: the button's text does not move with the selected
        // art), along it 10 units in from either end.
        const u32 color = off ? ui::kGray : active ? ui::kHighlight : ui::kGold;
        const f32 room = len - 20.0f * u, natural = g.draw.measure(tabs[i], ui::FontId::Friz, textSize);
        const f32 fit = natural > room && natural > 0.0f ? std::max(room, 0.0f) / natural : 1.0f; // squeezed: smaller
        const TextStyle style = ui::gameFont(textSize * fit, color);
        g.draw.textTurned({r.x + 2.0f * u, y + 10.0f * u, 24.0f * u, len - 20.0f * u}, tabs[i], style);
        if (ui::tapped(state) && !active) { // PanelTemplates_SelectTab disables the selected one
            tapped = static_cast<i32>(i);
        }
        y += len - overlap;
    }
    return tapped;
}

i32 radioRow(Gui& g, ui::WidgetId id, const Rect& r, std::span<const std::string_view> labels, u32 selected,
             f32 textSize) {
    const u32 count = static_cast<u32>(labels.size());
    if (count == 0) {
        return -1;
    }
    const f32 radio = std::min(r.h * 0.75f, g.px(24.0f)); // 16 units, enlarged for a finger
    // Each cell as wide as its button and label, the room left shared out; when they do not fit, the labels'
    // text shrinks first (to 3/4), then the cells squeeze.
    const f32 fixed = g.px(2.0f) + radio + g.unit(5.0f) + g.px(4.0f);
    f32 textW = 0.0f;
    for (const std::string_view label : labels) {
        textW += g.draw.measure(label, ui::FontId::Friz, textSize);
    }
    const f32 room = r.w - fixed * static_cast<f32>(count);
    const f32 shrink = textW > room && textW > 0.0f ? std::max(0.75f, room / textW) : 1.0f;
    const TextStyle labelStyle = ui::gameFont(textSize * shrink);
    const f32 natural = fixed * static_cast<f32>(count) + textW * shrink;
    const f32 extra = (r.w - natural) / static_cast<f32>(count);
    const f32 squeeze = natural > r.w && natural > 0.0f ? r.w / natural : 1.0f;
    i32 tapped = -1;
    f32 x = r.x;
    for (u32 i = 0; i < count; ++i) {
        const f32 own = fixed + g.draw.measure(labels[i], labelStyle.font, labelStyle.size);
        const f32 cellW = squeeze < 1.0f ? own * squeeze : own + extra;
        const Rect cell{x, r.y, cellW, r.h};
        x += cellW;
        const ui::WidgetState state = ui::hit(g, id + i * 7919, cell);
        const bool checked = i == selected;
        const Rect art{cell.x + g.px(2.0f), cell.centerY() - radio * 0.5f, radio, radio};
        // UI-RadioButton: the NormalTexture (0..0.25), the CheckedTexture over it (0.25..0.5), the highlight
        // (0.5..0.75, ADD).
        g.draw.sprite(art, kRadio, ui::kWhite, {0.0f, 0.0f, 0.25f, 1.0f});
        if (checked) {
            g.draw.sprite(art, kRadio, ui::kWhite, {0.25f, 0.0f, 0.5f, 1.0f});
        }
        if (ui::held(state, cell)) {
            g.draw.spriteAdditive(art, kRadio, ui::kWhite, {0.5f, 0.0f, 0.75f, 1.0f});
        }
        const f32 textX = art.right() + g.unit(5.0f);
        ui::label(g, {textX, cell.y, cell.right() - textX, cell.h}, labels[i], labelStyle);
        if (ui::tapped(state)) {
            tapped = static_cast<i32>(i);
        }
    }
    return tapped;
}

bool pageButton(Gui& g, ui::WidgetId id, const Rect& r, bool next, bool enabled, std::string_view label,
                f32 textSize) {
    const ui::WidgetState state = ui::hit(g, id, r);
    const bool down = enabled && ui::held(state, r);
    const f32 size = std::min(r.h, r.w);
    const Rect arrow{next ? r.right() - size : r.x, r.centerY() - size * 0.5f, size, size};
    const u64 texture = !enabled ? (next ? kNextDisabled : kPrevDisabled)
                        : down   ? (next ? kNextDown : kPrevDown)
                                 : (next ? kNextUp : kPrevUp);
    g.draw.sprite(arrow, texture);
    if (down) {
        g.draw.spriteAdditive(arrow, kMouseHighlight);
    }
    if (!label.empty()) {
        // GameFontNormal (gold) whatever the button's state, as the FontString in its BACKGROUND layer.
        TextStyle style = ui::gameFont(textSize);
        style.align = next ? Align::Right : Align::Left;
        const Rect text = next ? Rect{r.x, r.y, arrow.x - r.x, r.h}
                               : Rect{arrow.right(), r.y, r.right() - arrow.right(), r.h};
        ui::label(g, text, label, style);
    }
    return enabled && ui::tapped(state);
}

f32 pageButtonWidth(const Gui& g, f32 arrow, std::string_view label, f32 textSize) {
    return arrow + (label.empty() ? 0.0f : g.draw.measure(label, ui::FontId::Friz, textSize) + g.px(2.0f));
}

void skillsBar(Gui& g, const Rect& bar, f32 fill, u32 color, const BarBorder& border, u32 background) {
    if (background != 0) {
        g.draw.solid(bar, background);
    }
    const f32 f = std::clamp(fill, 0.0f, 1.0f);
    g.draw.sprite({bar.x, bar.y, bar.w * f, bar.h}, kSkillsBar, color, {0.0f, 0.0f, f, 1.0f});
    // The border's rounded ends keep their shape when the bar is wider than the client's: 16 of its 256
    // texels at the height's scale (32 texels high), the middle stretched.
    const f32 s = bar.h / border.barH; // the client's units at the bar's height
    const Rect r{bar.x + border.x * s, bar.y + border.y * s, bar.w + (border.w - border.barW) * s,
                 border.h * s};
    ui::threeSlice(g.draw, r, kSkillsBarBorder, {}, 16.0f / 256.0f, std::min(r.h * 0.5f, r.w * 0.25f));
}

void tradeHighlight(Gui& g, const Rect& r) {
    // 161 units of the texture's 256 wide; 16-unit ends of its 256 high, the middle stretched.
    constexpr f32 kRight = 0.62890625f;
    const f32 end = std::min(r.w * 16.0f / 161.0f, r.h * 0.5f);
    g.draw.sprite({r.x, r.y, r.w, end}, kTradeHighlight, ui::kWhite, {0.0f, 0.0f, kRight, 0.0625f});
    g.draw.sprite({r.x, r.y + end, r.w, std::max(0.0f, r.h - 2.0f * end)}, kTradeHighlight, ui::kWhite,
                  {0.0f, 0.0625f, kRight, 0.9375f});
    g.draw.sprite({r.x, r.bottom() - end, r.w, end}, kTradeHighlight, ui::kWhite,
                  {0.0f, 0.9375f, kRight, 1.0f});
}

} // namespace vd::art
