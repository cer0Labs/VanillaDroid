#include "app/InventoryWindows.h"

#include "app/ContainerWindows.h"
#include "app/DialogKit.h"
#include "app/MinimapView.h"
#include "app/UiResources.h"
#include "app/WindowArt.h" // retouches (windows)
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vd {
namespace {

using kit::Dialog;
using kit::Metrics;
using ui::Align;
using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::TextStyle;
using ui::WidgetId;
using ui::widgetId;

constexpr u64 kDurabilityIcons = assetPathHash("Interface\\Durability\\UI-Durability-Icons.blp");
constexpr u64 kListboxHighlight = assetPathHash("Interface\\Buttons\\UI-Listbox-Highlight.blp");
constexpr u64 kItemNameFrame = assetPathHash("Interface\\QuestFrame\\UI-QuestItemNameFrame.blp");
constexpr u64 kSlotHighlight = assetPathHash("Interface\\Buttons\\ButtonHilight-Square.blp");
// Phase 13 (spells): an action button's checked state (CheckButtonHilight), on the items a waiting spell suits.
constexpr u64 kLitHighlight = assetPathHash("Interface\\Buttons\\CheckButtonHilight.blp");
constexpr u64 kBackpackIcon = assetPathHash("Interface\\Buttons\\Button-Backpack-Up.blp"); // textures.vdpk, 64x64
constexpr u32 kWhiteText = rgba(255, 255, 255);
constexpr u32 kSectionText = rgba(255, 209, 0);

// A row of tabs across `r` (the vendor's): retouches (windows): CharacterFrameTabButtonTemplate
// (MerchantFrameTab1..2, app/WindowArt). Returns the one tapped (-1 none).
i32 tabBar(Gui& g, std::string_view name, const Rect& r, std::span<const std::string_view> tabs, u32 selected,
           f32 textSize) {
    return art::characterTabBar(g, name, r, tabs, selected, textSize, g.px(96.0f));
}

// A thin dark well behind a group of cells (the bag frames' backgrounds).
void well(Gui& g, const Rect& r, bool special) {
    g.draw.solid(r, special ? rgba(40, 30, 10, 150) : rgba(0, 0, 0, 110));
}

f32 cellStep(const Gui& g, const Metrics& m) {
    return m.item + g.px(8.0f);
}

} // namespace

// -------------------------------------------------------------------------------------------------------
// Dragging.

void ItemDrag::begin(const ItemSlotView& cell, const ui::Touch& finger) {
    active = true;
    from = cell.ref;
    iconHash = cell.iconHash;
    quality = cell.quality;
    count = cell.count;
    touch = finger.id;
    touchDownMs = finger.downMs;
    x = finger.x;
    y = finger.y;
    released = false;
    taken = false;
}

void ItemDrag::track(std::span<const ui::Touch> touches) {
    if (!active) {
        return;
    }
    for (const ui::Touch& t : touches) {
        if (t.id == touch && t.downMs == touchDownMs) {
            x = t.x;
            y = t.y;
            released = t.released;
            return;
        }
    }
    *this = ItemDrag{}; // the finger is gone without a release (cancelled)
}

void ItemDrag::finish() {
    if (active && released) {
        *this = ItemDrag{};
    }
}

// -------------------------------------------------------------------------------------------------------
// Cells.

void itemCell(DialogContext& c, Gui& g, WidgetId id, const Rect& r, const ItemSlotView& cell, ItemDrag& drag,
              ItemWindowResult& result) {
    ui::SlotView slot;
    slot.empty = cell.empty;
    slot.icon = !cell.empty && cell.iconHash ? c.resources.icon(cell.iconHash) : std::nullopt;
    slot.count = cell.count;
    slot.quality = cell.quality;
    slot.cooldown = cell.cooldown;
    slot.usable = cell.usable && !cell.broken;
    slot.emptyArt = !cell.bagArt; // phase 15 (items)
    const bool carried = drag.active && drag.from == cell.ref && cell.ref.valid();
    slot.selected = carried;
    // Retouches (windows): a locked item (a move on its way, offered, carried: the client locks what is on
    // the cursor) desaturated as ContainerFrame_Update / PaperDollItemSlotButton_UpdateLock do; the cell's
    // own tint. No low durability mark: 1.12 shows it in DurabilityFrame's figure only (broken: red, as
    // `usable`).
    slot.desaturated = cell.dimmed || carried;
    slot.tint = cell.tint;
    const ui::WidgetState state = ui::itemSlot(g, id, r, slot);
    if (cell.empty && cell.emptyArt != 0) {
        g.draw.sprite(r, cell.emptyArt, ui::withAlpha(cell.tint, 230.0f / 255.0f)); // UI-PaperDoll-Slot-*
    }
    if (!cell.empty && cell.lit && !carried) {
        g.draw.spriteAdditive(r, kLitHighlight);
    }
    if (drag.active && !drag.released && cell.ref.valid() && !carried && r.contains(drag.x, drag.y)) {
        g.draw.sprite(r, kSlotHighlight); // where the carried item would go
    }
    if (state.clicked && !drag.active && cell.ref.valid()) {
        result.tapped = cell.ref;
        result.tappedRect = r;
    }
    if (state.longPress && state.touch && !cell.empty && !drag.active && cell.ref.valid()) {
        drag.begin(cell, *state.touch);
    }
    const Rect clip = g.draw.clip();
    if (cell.ref.valid() && drag.droppedIn(r) && (clip.w <= 0.0f || clip.contains(drag.x, drag.y))) {
        result.dropFrom = drag.from;
        result.dropOn = cell.ref;
        drag.taken = true;
    }
}

// -------------------------------------------------------------------------------------------------------
// Bags.

namespace {

// The grid of cells of the sections, from `y`; returns the y below it.
f32 sectionsGrid(DialogContext& c, Gui& g, std::string_view name, f32 x, f32 y, f32 w,
                 std::span<const BagSectionView> sections, std::span<const ItemSlotView> cells, ItemDrag& drag,
                 ItemWindowResult& result) {
    const Metrics m = kit::metrics(c);
    const f32 step = cellStep(g, m);
    const u32 columns = std::max(1u, static_cast<u32>((w + g.px(8.0f)) / step));
    const f32 gridW = static_cast<f32>(columns) * step - g.px(8.0f);
    const f32 left = x + (w - gridW) * 0.5f;
    const TextStyle title = ui::gameFont(m.small, kSectionText);
    for (u32 s = 0; s < sections.size(); ++s) {
        const BagSectionView& section = sections[s];
        if (section.count == 0) {
            continue;
        }
        ui::label(g, {left, y, gridW, m.small * 1.4f}, section.title, title);
        y += m.small * 1.5f;
        const u32 rows = (section.count + columns - 1) / columns;
        const f32 wellW = static_cast<f32>(std::min(section.count, columns)) * step - g.px(8.0f);
        well(g, {left - g.px(3.0f), y - g.px(3.0f), wellW + g.px(6.0f), static_cast<f32>(rows) * step}, section.special);
        for (u32 i = 0; i < section.count && section.first + i < cells.size(); ++i) {
            const Rect r{left + static_cast<f32>(i % columns) * step, y + static_cast<f32>(i / columns) * step, m.item,
                         m.item};
            itemCell(c, g, widgetId(name, 1000 + section.first + i), r, cells[section.first + i], drag, result);
        }
        y += static_cast<f32>(rows) * step + g.px(4.0f);
    }
    return y;
}

void bagsGridZone(DialogContext& c, Gui& g, std::string_view name, const Rect& area, const BagsView& v,
                  ItemDrag& drag, ItemWindowResult& result, i32 zone) {
    const Metrics m = kit::metrics(c);
    const f32 step = cellStep(g, m);
    // The bag bar (the backpack and the 4 bag slots), then money and free slots on its right; smaller cells where
    // they would not fit (parity pass: a narrow panel).
    const f32 barStep =
        v.bagBar.empty() ? step : std::min(step, (area.w - g.px(8.0f)) / static_cast<f32>(v.bagBar.size()));
    const f32 barItem = std::min(m.item, barStep - (step - m.item));
    f32 barH = barItem + g.px(10.0f);
    f32 x = area.x + g.px(4.0f);
    for (u32 i = 0; i < v.bagBar.size(); ++i) {
        itemCell(c, g, widgetId(name, 10 + i), {x, area.y + g.px(4.0f), barItem, barItem}, v.bagBar[i], drag, result);
        x += barStep;
    }
    const f32 info = area.right() - g.px(6.0f);
    // Phase 8: a narrow pane (the mail's, the auction house's) puts them on a line of their own under the
    // bar.
    f32 infoY = area.y;
    if (x + ui::moneyWidth(g.draw, v.money, m.text) + g.px(8.0f) > info) {
        x = area.x + g.px(4.0f);
        infoY = area.y + barH - g.px(4.0f);
        barH += m.text * 1.5f;
    }
    const f32 infoH = infoY == area.y ? barH : m.text * 1.5f;
    ui::money(g, info, infoY + infoH * 0.32f, v.money, m.text, Align::Right);
    char buffer[64];
    const std::string_view slots = text::format(buffer, "VD_INV_FREE_SLOTS", v.freeSlots, v.totalSlots);
    TextStyle style = ui::gameFont(m.small, v.freeSlots == 0 ? ui::kRed : kWhiteText);
    const bool beside = infoY == area.y;
    style.align = beside ? Align::Right : Align::Left;
    const f32 labelY = infoY + (beside ? infoH * 0.55f : 0.0f);
    ui::label(g, {x, labelY, info - x, beside ? infoH * 0.4f : infoH * 0.64f}, slots, style);

    const Rect grid{area.x, area.y + barH, area.w, area.h - barH};
    const ui::ScrollView view = ui::beginScroll(g, widgetId(name, 2), grid);
    const f32 bottom = sectionsGrid(c, g, name, view.viewport.x + g.px(4.0f), view.top + g.px(2.0f),
                                    view.viewport.w - g.px(10.0f), v.sections, v.cells, drag, result);
    ui::endScroll(g, view, bottom - view.top + g.px(4.0f));
    if (drag.droppedIn(area)) {
        result.dropFrom = drag.from;
        result.dropZone = zone;
        drag.taken = true;
    }
}

} // namespace

void bagsGrid(DialogContext& c, Gui& g, std::string_view name, const Rect& area, const BagsView& v, ItemDrag& drag,
              ItemWindowResult& result) {
    bagsGridZone(c, g, name, area, v, drag, result, 0);
}

ItemWindowResult bagsWindow(DialogContext& c, const BagsView& v, ItemDrag& drag) {
    VD_PROFILE_ZONE("InventoryWindows::bags");
    Gui g{c.ui, c.draw};
    const bool wide = c.profile == ScreenProfile::Wide;
    ItemWindowResult result;
    // As tall as its bags (up to the screen): columns as the grid will lay them out.
    const Metrics m = kit::metrics(c);
    const f32 widthDp = wide ? 620.0f : 560.0f;
    const f32 step = cellStep(g, m);
    const f32 gridW = std::min(g.px(widthDp), c.safeArea.w - g.px(12.0f)) - g.px(44.0f);
    const u32 columns = std::max(1u, static_cast<u32>((gridW + g.px(8.0f)) / step));
    f32 contentH = m.item + g.px(16.0f);
    for (const BagSectionView& s : v.sections) {
        contentH += m.small * 1.5f + static_cast<f32>((s.count + columns - 1) / columns) * step + g.px(4.0f);
    }
    const f32 heightDp = std::clamp(contentH / c.ui.dp() + 70.0f, 240.0f, wide ? 420.0f : 680.0f);
    const Dialog d = kit::beginDialog(c, g, "bags", widthDp, heightDp, text::get("VD_INV_BAGS"), true, false);
    bagsGrid(c, g, "bags.grid", d.body, v, drag, result);
    result.closed = d.closed || (d.outside && !drag.active);
    return result;
}

// -------------------------------------------------------------------------------------------------------
// Tooltip.

Rect itemTooltipSize(const DialogContext& c, const TooltipView& v) {
    Gui g{c.ui, c.draw};
    const Metrics m = kit::metrics(c);
    const Rect& safe = c.safeArea;
    const f32 pad = g.px(10.0f);
    const f32 w = std::min(g.px(c.profile == ScreenProfile::Wide ? 320.0f : 330.0f), safe.w - g.px(16.0f));
    const f32 textW = w - 2.0f * pad;
    const f32 nameSize = m.text * 1.12f, size = m.small * 1.08f;
    f32 textH = 0.0f;
    for (usize i = 0; i < v.lines.size(); ++i) {
        const TooltipLineView& line = v.lines[i];
        const f32 s = i == 0 ? nameSize : size;
        const f32 h = line.wrap ? g.draw.measureWrapped(textW, line.left, ui::FontId::Friz, s)
                                : g.draw.lineHeight(ui::FontId::Friz, s);
        textH += h + (line.gapBefore ? s * 0.6f : 0.0f) + s * 0.12f;
    }
    const u32 actionRows = static_cast<u32>((v.actions.size() + 2) / 3);
    const f32 actionsH = actionRows > 0 ? static_cast<f32>(actionRows) * (m.button + g.px(6.0f)) + g.px(4.0f) : 0.0f;
    return {0.0f, 0.0f, w, std::min(safe.h - g.px(16.0f), textH + actionsH + 2.0f * pad)};
}

TooltipResult itemTooltip(DialogContext& c, const TooltipView& v) {
    VD_PROFILE_ZONE("InventoryWindows::tooltip");
    Gui g{c.ui, c.draw};
    const Metrics m = kit::metrics(c);
    TooltipResult result;
    result.closed = ui::modalBlocker(g, widgetId("tooltip.block"), 0.0f);

    const Rect& safe = c.safeArea;
    const f32 pad = g.px(10.0f);
    const f32 w = std::min(g.px(c.profile == ScreenProfile::Wide ? 320.0f : 330.0f), safe.w - g.px(16.0f));
    const f32 textW = w - 2.0f * pad;
    const f32 nameSize = m.text * 1.12f, size = m.small * 1.08f;
    const auto lineSize = [&](usize i) { return i == 0 ? nameSize : size; };
    const auto lineHeight = [&](usize i) {
        const TooltipLineView& line = v.lines[i];
        const f32 s = lineSize(i);
        const f32 h = line.wrap ? g.draw.measureWrapped(textW, line.left, ui::FontId::Friz, s)
                                : g.draw.lineHeight(ui::FontId::Friz, s);
        return h + (line.gapBefore ? s * 0.6f : 0.0f) + s * 0.12f;
    };
    f32 textH = 0.0f;
    for (usize i = 0; i < v.lines.size(); ++i) {
        textH += lineHeight(i);
    }
    // The action row: up to three buttons per row.
    const u32 perRow = 3;
    const u32 actionRows = static_cast<u32>((v.actions.size() + perRow - 1) / perRow);
    const f32 buttonH = m.button;
    const f32 actionsH = actionRows > 0 ? static_cast<f32>(actionRows) * (buttonH + g.px(6.0f)) + g.px(4.0f) : 0.0f;
    const f32 maxH = safe.h - g.px(16.0f);
    const f32 h = std::min(maxH, textH + actionsH + 2.0f * pad);

    // Beside the item: right of it when it fits, else left; clamped to the safe area.
    const Rect& a = v.anchor;
    f32 x = a.right() + g.px(8.0f);
    if (a.w <= 0.0f) {
        x = safe.centerX() - w * 0.5f;
    } else if (x + w > safe.right() - g.px(8.0f)) {
        x = a.x - g.px(8.0f) - w;
    }
    x = std::clamp(x, safe.x + g.px(8.0f), safe.right() - g.px(8.0f) - w);
    f32 y = std::clamp(a.h > 0.0f ? a.y : safe.centerY() - h * 0.5f, safe.y + g.px(8.0f), safe.bottom() - g.px(8.0f) - h);
    if (v.placed.w > 0.0f) { // phase 15 (items): placed with its comparisons
        x = v.placed.x;
        y = v.placed.y;
    }
    const Rect panel{x, y, w, h};
    result.panel = panel;
    ui::hit(g, widgetId("tooltip.panel"), panel);
    ui::tooltipPanel(g, panel);
    // The text, scrolling when it does not fit.
    const Rect textArea{panel.x + pad * 0.5f, panel.y + pad * 0.5f, panel.w - pad, panel.h - actionsH - pad};
    const ui::ScrollView view = ui::beginScroll(g, widgetId("tooltip.text"), textArea);
    f32 ty = view.top + pad * 0.5f;
    const f32 tx = view.viewport.x + pad * 0.5f;
    for (usize i = 0; i < v.lines.size(); ++i) {
        const TooltipLineView& line = v.lines[i];
        const f32 s = lineSize(i);
        if (line.gapBefore) {
            ty += s * 0.6f;
        }
        TextStyle style = ui::gameFont(s, line.leftColor);
        if (line.wrap) {
            ty += g.draw.wrappedText({tx, ty, textW, 1e6f}, line.left, style) + s * 0.12f;
            continue;
        }
        const f32 lh = g.draw.lineHeight(ui::FontId::Friz, s);
        const f32 baseline = ui::centeredBaseline(g.draw, style, ty + lh * 0.5f);
        const f32 leftW = g.draw.text(tx, baseline, line.left, style);
        if (line.money > 0) {
            ui::money(g, tx + leftW + g.px(6.0f), ty + lh * 0.5f, line.money, s, Align::Left);
        }
        if (!line.right.empty()) {
            TextStyle right = ui::gameFont(s, line.rightColor);
            right.align = Align::Right;
            g.draw.text(tx + textW, baseline, line.right, right);
        }
        ty += lh + s * 0.12f;
    }
    ui::endScroll(g, view, ty - view.top + pad * 0.5f);

    // The actions.
    const u32 widest = std::min<u32>(perRow, static_cast<u32>(v.actions.size()));
    const f32 bw = widest > 0 ? (panel.w - pad * 2.0f - g.px(6.0f) * static_cast<f32>(widest - 1)) / static_cast<f32>(widest)
                              : 0.0f;
    for (u32 row = 0; row < actionRows; ++row) {
        const u32 first = row * perRow;
        const u32 count = std::min<u32>(perRow, static_cast<u32>(v.actions.size()) - first);
        const f32 rowW = static_cast<f32>(count) * bw + g.px(6.0f) * static_cast<f32>(count - 1);
        const f32 by = panel.bottom() - pad - static_cast<f32>(actionRows - row) * (buttonH + g.px(6.0f)) + g.px(6.0f);
        for (u32 i = 0; i < count; ++i) {
            const Rect r{panel.centerX() - rowW * 0.5f + static_cast<f32>(i) * (bw + g.px(6.0f)), by, bw, buttonH};
            if (ui::button(g, widgetId("tooltip.action", first + i), r, v.actions[first + i])) {
                result.action = static_cast<i32>(first + i);
            }
        }
    }
    return result;
}

// -------------------------------------------------------------------------------------------------------
// Vendor.

namespace {

// Two panes: side by side on wide screens, stacked on square ones.
void splitPanes(const DialogContext& c, const Gui& g, const Rect& body, f32 firstShare, Rect& first, Rect& second) {
    const f32 gap = g.px(8.0f);
    if (c.profile == ScreenProfile::Wide) {
        const f32 w = (body.w - gap) * firstShare;
        first = {body.x, body.y, w, body.h};
        second = {body.x + w + gap, body.y, body.w - w - gap, body.h};
    } else {
        const f32 h = (body.h - gap) * firstShare;
        first = {body.x, body.y, body.w, h};
        second = {body.x, body.y + h + gap, body.w, body.h - h - gap};
    }
}

void vendorRow(DialogContext& c, Gui& g, u32 index, const Rect& r, const VendorRowView& row, ItemDrag& drag,
               ItemWindowResult& result) {
    const Metrics m = kit::metrics(c);
    const ui::WidgetState state = ui::hit(g, widgetId("vendor.row", index), r);
    if (ui::held(state, r)) {
        g.draw.sprite(r, kListboxHighlight, rgba(255, 255, 255, 150));
    }
    if (ui::tapped(state) && !drag.active) {
        result.tapped = row.cell.ref;
        result.tappedRect = r;
    }
    const Rect icon{r.x + g.px(4.0f), r.centerY() - m.item * 0.5f, m.item, m.item};
    const bool soldOut = row.limited && row.stock == 0;
    // Retouches (windows): MerchantFrame_UpdateMerchantInfo's vertex colours: sold out, the icon and its
    // frame 0.5 grey (dark red when the player could not use it either), not usable (0.9, 0, 0); the name
    // frame 0.5 grey, red (1, 0, 0) for what the player cannot use, (0.5, 0, 0) for that sold out.
    ItemSlotView cell = row.cell;
    const bool usable = cell.usable;
    cell.usable = true;
    cell.dimmed = cell.dimmed && !soldOut;
    cell.tint = soldOut  ? (usable ? rgba(128, 128, 128) : rgba(128, 0, 0))
                : usable ? ui::kWhite
                         : rgba(230, 0, 0);
    const u32 nameFrameTint = usable ? rgba(128, 128, 128) : soldOut ? rgba(128, 0, 0) : rgba(255, 0, 0);
    // UI-QuestItemNameFrame behind the name and price (the merchant's slots), from under the icon's right edge.
    const f32 frameX = icon.right() - icon.h * 0.12f;
    const f32 frameH = icon.h * 1.12f;
    ui::threeSlice(g.draw, {frameX, r.centerY() - frameH * 0.5f, r.right() - frameX, frameH}, kItemNameFrame,
                   {11.0f / 128.0f, 11.0f / 64.0f, 117.0f / 128.0f, 53.0f / 64.0f}, 16.0f / 128.0f,
                   frameH * 16.0f / 42.0f, nameFrameTint);
    itemCell(c, g, widgetId("vendor.cell", index), icon, cell, drag, result);
    const f32 textX = icon.right() + g.px(8.0f);
    const f32 textW = r.right() - textX - g.px(4.0f);
    TextStyle name = ui::gameFont(m.small, soldOut ? ui::kDisabledText : ui::qualityColor(row.cell.quality));
    ui::label(g, {textX, r.y + r.h * 0.08f, textW, r.h * 0.46f}, row.name, name);
    if (row.price > 0) { // retouches (windows): what cannot be paid in red (SetMoneyFrameColor)
        ui::money(g, textX, r.y + r.h * 0.74f, row.price, m.small * 0.95f, Align::Left, 1.0f,
                  row.affordable ? ui::kWhite : ui::kRed);
    }
    char buffer[24];
    std::string_view extra;
    if (row.limited) {
        extra = text::format(buffer, "MERCHANT_STOCK", row.stock);
    } else if (row.buyCount > 1) {
        extra = text::format(buffer, "VD_INV_BUY_COUNT", row.buyCount);
    }
    if (!extra.empty()) {
        TextStyle style = ui::gameFont(m.small, soldOut ? ui::kRed : kWhiteText);
        style.align = Align::Right;
        ui::label(g, {textX, r.y + r.h * 0.5f, textW, r.h * 0.46f}, extra, style);
    }
}

} // namespace

ItemWindowResult vendorWindow(DialogContext& c, const VendorView& v, ItemDrag& drag) {
    VD_PROFILE_ZONE("InventoryWindows::vendor");
    Gui g{c.ui, c.draw};
    const Metrics m = kit::metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    ItemWindowResult result;
    const Dialog d =
        kit::beginDialog(c, g, "vendor", wide ? 920.0f : 680.0f, wide ? 420.0f : 760.0f, v.npcName, true, true);
    result.closed = d.closed;
    Rect list, bags;
    splitPanes(c, g, d.body, wide ? 0.54f : 0.5f, list, bags);

    // The vendor's side: the tabs, then its rows in two columns where they fit.
    const std::string_view tabs[] = {text::get("VD_INV_BUY"), text::get("BUYBACK")};
    const f32 tabH = g.px(30.0f);
    result.tab = tabBar(g, "vendor.tabs", {list.x, list.y, list.w, tabH}, tabs, v.buyback ? 1 : 0, m.text);
    const Rect rows{list.x, list.y + tabH + g.px(6.0f), list.w, list.h - tabH - g.px(6.0f)};
    ui::parchment(g, rows);
    const ui::ScrollView view = ui::beginScroll(g, widgetId("vendor.list"), rows.inset(g.px(3.0f)));
    const u32 columns = view.viewport.w >= g.px(440.0f) ? 2 : 1;
    const f32 rowW = (view.viewport.w - g.px(8.0f)) / static_cast<f32>(columns);
    const f32 rowH = m.item + g.px(12.0f);
    for (u32 i = 0; i < v.rows.size(); ++i) {
        const Rect r{view.viewport.x + g.px(4.0f) + static_cast<f32>(i % columns) * rowW,
                     view.top + g.px(4.0f) + static_cast<f32>(i / columns) * rowH, rowW - g.px(4.0f), rowH - g.px(2.0f)};
        vendorRow(c, g, i, r, v.rows[i], drag, result);
    }
    const u32 rowCount = static_cast<u32>((v.rows.size() + columns - 1) / columns);
    if (v.rows.empty()) {
        TextStyle style = ui::questFont(m.text);
        style.align = Align::Center;
        ui::label(g, {view.viewport.x, view.top + g.px(10.0f), view.viewport.w, rowH},
                  text::get(v.buyback ? "VD_INV_BUYBACK_EMPTY" : "VD_INV_VENDOR_EMPTY"), style);
    }
    ui::endScroll(g, view, static_cast<f32>(rowCount) * rowH + g.px(8.0f));
    if (drag.droppedIn(list)) { // an item of the bags dropped on the vendor: sell it
        result.dropFrom = drag.from;
        result.dropZone = 0;
        drag.taken = true;
    }

    // The player's bags.
    bagsGridZone(c, g, "vendor.bags", bags, v.bags, drag, result, 1);

    // Footer: repair everything and sell the grey items. Retouches (windows): their prices in money frames
    // after their buttons (1.12 shows the repair's in MerchantRepairAllButton's tooltip; a finger has no
    // hover), the repair's red when it cannot be paid (SetMoneyFrameColor).
    const f32 gap = g.px(8.0f);
    const f32 moneySize = m.small;
    if (v.canRepair) {
        const std::string_view label =
            v.repairAllCost > 0 ? text::get("REPAIR_ALL_ITEMS") : text::get("VD_INV_NOTHING_TO_REPAIR");
        const f32 costW =
            v.repairAllCost > 0 ? ui::moneyWidth(g.draw, v.repairAllCost, moneySize) + gap : 0.0f;
        const f32 w =
            std::min(d.footer.w * 0.48f - costW, kit::buttonWidth(g, label, d.footer.h) + d.footer.h);
        const Rect r{d.footer.x, d.footer.y, w, d.footer.h};
        if (ui::button(g, widgetId("vendor.repair"), r, label, v.repairAllCost > 0 && v.repairAllCost <= v.money)) {
            result.button = 0;
        }
        if (v.repairAllCost > 0) {
            ui::money(g, r.right() + gap, r.centerY(), v.repairAllCost, moneySize, Align::Left, 1.0f,
                      v.repairAllCost <= v.money ? ui::kWhite : ui::kRed);
        }
    }
    const std::string_view junk = text::get(v.junkValue > 0 ? "VD_INV_SELL_JUNK" : "VD_INV_NO_JUNK");
    const f32 junkW = v.junkValue > 0 ? ui::moneyWidth(g.draw, v.junkValue, moneySize) + gap : 0.0f;
    const f32 w = std::min(d.footer.w * 0.48f - junkW, kit::buttonWidth(g, junk, d.footer.h));
    const Rect junkRect{d.footer.right() - w, d.footer.y, w, d.footer.h};
    if (ui::button(g, widgetId("vendor.junk"), junkRect, junk, v.junkValue > 0 && !v.buyback)) {
        result.button = 1;
    }
    if (v.junkValue > 0) {
        ui::money(g, junkRect.x - gap, junkRect.centerY(), v.junkValue, moneySize, Align::Right);
    }
    return result;
}

// -------------------------------------------------------------------------------------------------------
// Bank.

ItemWindowResult bankWindow(DialogContext& c, const BankView& v, ItemDrag& drag, ContainerFramesResult* frames) {
    VD_PROFILE_ZONE("InventoryWindows::bank");
    Gui g{c.ui, c.draw};
    const Metrics m = kit::metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    ItemWindowResult result;
    const Dialog d = kit::beginDialog(c, g, "bank", wide ? 920.0f : 680.0f, wide ? 420.0f : 760.0f,
                                      text::get("VD_INV_BANK"), true, false);
    result.closed = d.closed;
    Rect bank, bags;
    splitPanes(c, g, d.body, 0.52f, bank, bags);

    // The bank bag slots, and the button for the next one. Retouches (windows): UpdateBagSlotStatus: a slot
    // not bought yet red (SetItemButtonTextureVertexColor 1, 0.1, 0.1); BankFramePurchaseInfo: COSTS_LABEL
    // and its money frame (red when it cannot be paid), then BANKSLOTPURCHASE.
    const f32 step = cellStep(g, m);
    const f32 barH = m.item + g.px(10.0f);
    f32 x = bank.x + g.px(4.0f);
    for (u32 i = 0; i < v.bankBags.size(); ++i) {
        ItemSlotView cell = v.bankBags[i];
        if (i >= v.bankBagSlots) {
            cell.tint = rgba(255, 26, 26);
        }
        itemCell(c, g, widgetId("bank.bag", i), {x, bank.y + g.px(4.0f), m.item, m.item}, cell, drag, result);
        x += step;
    }
    if (v.nextSlotPrice > 0) {
        const std::string_view label = text::get("BANKSLOTPURCHASE");
        const std::string_view costLabel = text::get("COSTS_LABEL");
        const TextStyle costStyle = ui::gameFont(m.small);
        const f32 room = bank.right() - x - g.px(4.0f);
        const f32 moneyW = ui::moneyWidth(g.draw, v.nextSlotPrice, m.small);
        // A short row narrows the button to its text first, then leaves the label out.
        f32 labelW = g.draw.measure(costLabel, costStyle.font, costStyle.size) + g.px(4.0f);
        const f32 tightW = g.draw.measure(label, ui::FontId::Friz, m.text) + g.px(28.0f);
        f32 bw = std::max(
            tightW, std::min(kit::buttonWidth(g, label, m.button), room - labelW - moneyW - g.px(10.0f)));
        if (labelW + moneyW + g.px(10.0f) + bw > room) {
            labelW = 0.0f;
            bw =
                std::max(tightW, std::min(kit::buttonWidth(g, label, m.button), room - moneyW - g.px(10.0f)));
        }
        const f32 rowW = labelW + moneyW + g.px(10.0f) + bw;
        if (rowW <= room) {
            const f32 by = bank.y + (barH - m.button) * 0.5f;
            f32 rx = bank.right() - rowW;
            if (labelW > 0.0f) {
                ui::label(g, {rx, by, labelW, m.button}, costLabel, costStyle);
                rx += labelW;
            }
            ui::money(g, rx, by + m.button * 0.5f, v.nextSlotPrice, m.small, Align::Left, 1.0f,
                      v.nextSlotPrice <= v.bags.money ? ui::kWhite : rgba(255, 26, 26));
            if (ui::button(g, widgetId("bank.buySlot"), {bank.right() - bw, by, bw, m.button}, label)) {
                result.button = 0;
            }
        }
    }
    const Rect grid{bank.x, bank.y + barH, bank.w, bank.h - barH};
    const ui::ScrollView view = ui::beginScroll(g, widgetId("bank.grid"), grid);
    f32 bottom = sectionsGrid(c, g, "bank.cells", view.viewport.x + g.px(4.0f), view.top + g.px(2.0f),
                              view.viewport.w - g.px(10.0f), v.sections, v.cells, drag, result);
    if (v.bagFrames && v.bagFrameCount > 0 && frames) {
        // Phase 15 (items): the open bank bags' ContainerFrames (UI-Bag-Components-Bank), in rows under the slots.
        const u32 count = std::min<u32>(v.bagFrameCount, 6);
        const f32 left = view.viewport.x + g.px(4.0f), right = view.viewport.right() - g.px(4.0f);
        const f32 s = std::min(m.item / 37.0f, (right - left) / kContainerWidth); // as wide as the pane at most
        const f32 fw = kContainerWidth * s, gap = g.px(4.0f);
        std::array<Rect, 6> rects{};
        f32 fx = left, fy = bottom, rowH = 0.0f;
        for (u32 i = 0; i < count; ++i) {
            const f32 h = containerGeometry(v.bagFrames[i].id, static_cast<u32>(v.bagFrames[i].cells.size())).height * s;
            if (fx > left && fx + fw > right) {
                fx = left;
                fy += rowH + gap;
                rowH = 0.0f;
            }
            rects[i] = {fx, fy, fw, h};
            fx += fw + gap;
            rowH = std::max(rowH, h);
        }
        bottom = fy + rowH;
        containerFrames(c, {v.bagFrames, count}, {rects.data(), count}, s, drag, result, *frames);
    }
    ui::endScroll(g, view, bottom - view.top + g.px(4.0f));
    if (drag.droppedIn(bank)) {
        result.dropFrom = drag.from;
        result.dropZone = 0;
        drag.taken = true;
    }
    bagsGridZone(c, g, "bank.bags", bags, v.bags, drag, result, 1);
    return result;
}

// -------------------------------------------------------------------------------------------------------
// Reading, quantities.

ReadingResult readingWindow(DialogContext& c, const ReadingView& v) {
    VD_PROFILE_ZONE("InventoryWindows::reading");
    Gui g{c.ui, c.draw};
    const Metrics m = kit::metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    ReadingResult result;
    const Dialog d = kit::beginDialog(c, g, "reading", wide ? 560.0f : 540.0f, wide ? 420.0f : 620.0f, v.title, true, true);
    result.closed = d.closed;
    const kit::ParchmentScroll p = kit::beginParchment(g, widgetId("reading.page", v.page), d.body);
    kit::Flow flow = kit::parchmentFlow(g, p);
    if (v.text.empty()) {
        flow.text(v.waiting ? "..." : "", ui::questFont(m.text), 0.0f);
    } else {
        flow.text(v.text, ui::questFont(m.text * 1.05f), g.px(6.0f));
    }
    kit::endParchment(g, p, flow);
    bool previous = false, next = false;
    if (v.pageCount > 1) {
        const f32 bw = std::min(d.footer.w * 0.3f, g.px(150.0f));
        previous = ui::button(g, widgetId("reading.previous"), {d.footer.x, d.footer.y, bw, d.footer.h},
                              text::get("PREV"), v.page > 0);
        next = ui::button(g, widgetId("reading.next"), {d.footer.right() - bw, d.footer.y, bw, d.footer.h},
                          text::get("NEXT"), v.page + 1 < v.pageCount);
        char buffer[64];
        const std::string_view page = text::format(buffer, "VD_INV_PAGE_OF", v.page + 1, v.pageCount);
        TextStyle style = ui::gameFont(m.small, ui::kGold);
        style.align = Align::Center;
        ui::label(g, d.footer, page, style);
    }
    result.turn = previous ? -1 : next ? 1 : 0;
    return result;
}

QuantityResult quantityDialog(DialogContext& c, const QuantityView& v, u32& value) {
    Gui g{c.ui, c.draw};
    const Metrics m = kit::metrics(c);
    QuantityResult result;
    const Dialog d = kit::beginDialog(c, g, "quantity", 420.0f, v.unitPrice > 0 ? 300.0f : 270.0f, v.title, true, true);
    value = std::clamp(value, v.min, std::max(v.min, v.max));
    // The item.
    ItemView item{v.iconHash, v.itemName, 1, v.quality};
    kit::questItem(c, g, widgetId("quantity.item"), {d.body.x, d.body.y + g.px(4.0f), d.body.w, m.item}, item, false, false);
    // - [ slider ] +, and the number.
    const f32 rowY = d.body.y + m.item + g.px(16.0f);
    const f32 button = g.px(40.0f);
    if (ui::plusMinusButton(g, widgetId("quantity.minus"), {d.body.x, rowY, button, button}, false, value > v.min)) {
        --value;
    }
    if (ui::plusMinusButton(g, widgetId("quantity.plus"), {d.body.right() - button, rowY, button, button}, true,
                            value < v.max)) {
        ++value;
    }
    f32 slide = static_cast<f32>(value);
    if (v.max > v.min &&
        ui::slider(g, widgetId("quantity.slider"), {d.body.x + button + g.px(8.0f), rowY, d.body.w - 2.0f * button - g.px(16.0f), button},
                   slide, static_cast<f32>(v.min), static_cast<f32>(v.max), 1.0f)) {
        value = static_cast<u32>(std::lround(slide));
    }
    char text[48];
    std::snprintf(text, sizeof(text), "%u", value);
    TextStyle number = ui::gameFont(m.title, kWhiteText);
    number.align = Align::Center;
    ui::label(g, {d.body.x, rowY + button + g.px(4.0f), d.body.w, m.title * 1.3f}, text, number);
    if (v.unitPrice > 0) {
        ui::money(g, d.body.centerX(), rowY + button + m.title * 1.6f + m.text, v.unitPrice * value, m.text, Align::Center);
    }
    bool cancel = false;
    kit::footerButtons(g, d.footer, "quantity", v.confirm, true, result.confirmed, text::get("CANCEL"), cancel);
    result.cancelled = cancel || d.closed || d.outside;
    return result;
}

// -------------------------------------------------------------------------------------------------------
// Durability figure.

bool durabilityFigure(DialogContext& c, f32 x, f32 y, f32 height, std::span<const u8> parts, bool offhandWeapon) {
    // DurabilityFrame.xml: each part's size, its place (units from the frame's top left, anchors resolved) and its
    // texels of UI-Durability-Icons. The figure spans x -12..78.5, y 0..76.
    struct Part {
        f32 x, y, w, h;
        ui::TexCoords uv;
        bool separate; // weapon, shield, ranged: only drawn when they need repair
    };
    static constexpr Part kParts[11] = {
        {21.0f, 0.0f, 18.0f, 22.0f, {0.0f, 0.0f, 0.140625f, 0.171875f}, false},             // head
        {6.0f, 6.0f, 48.0f, 22.0f, {0.140625f, 0.0f, 0.515625f, 0.171875f}, false},        // shoulders
        {20.0f, 13.0f, 20.0f, 22.0f, {0.515625f, 0.0f, 0.6640625f, 0.171875f}, false},     // chest
        {22.0f, 29.0f, 16.0f, 5.0f, {0.328125f, 0.171875f, 0.46875f, 0.203125f}, false},   // waist
        {15.5f, 32.0f, 29.0f, 20.0f, {0.46875f, 0.171875f, 0.6875f, 0.3203125f}, false},   // legs
        {9.5f, 44.0f, 41.0f, 32.0f, {0.6875f, 0.171875f, 1.0f, 0.4140625f}, false},        // feet
        {8.0f, 21.0f, 44.0f, 22.0f, {0.6640625f, 0.0f, 1.0f, 0.171875f}, false},           // wrists
        {9.0f, 28.0f, 42.0f, 18.0f, {0.0f, 0.171875f, 0.328125f, 0.3046875f}, false},      // hands
        {-12.0f, 15.5f, 20.0f, 45.0f, {0.0f, 0.3203125f, 0.140625f, 0.6640625f}, true},    // weapon
        {52.0f, 6.5f, 25.0f, 31.0f, {0.1875f, 0.3203125f, 0.375f, 0.5546875f}, true},      // shield
        {50.5f, 32.5f, 28.0f, 38.0f, {0.1875f, 0.5546875f, 0.3984375f, 0.84375f}, true},   // ranged
    };
    constexpr Part kOffWeapon{52.0f, 15.5f, 20.0f, 45.0f, {0.0f, 0.3203125f, 0.140625f, 0.6640625f}, true};
    bool any = false;
    for (const u8 p : parts) {
        any = any || p != 0;
    }
    if (!any || parts.size() < 11) {
        return false;
    }
    Gui g{c.ui, c.draw};
    const f32 scale = height / 76.0f;
    const f32 left = x + 12.0f * scale;
    for (u32 i = 0; i < 11; ++i) {
        const Part& part = (i == 9 && offhandWeapon) ? kOffWeapon : kParts[i];
        const u8 status = parts[i];
        if (part.separate && status == 0) {
            continue;
        }
        const u32 color = status == 2 ? rgba(237, 18, 18) : status == 1 ? rgba(255, 209, 46) : rgba(255, 255, 255, 128);
        g.draw.sprite({left + part.x * scale, y + part.y * scale, part.w * scale, part.h * scale}, kDurabilityIcons, color,
                      part.uv);
    }
    const Rect area{x, y, 90.5f * scale, height};
    return ui::tapped(ui::hit(g, widgetId("durability"), area));
}

// On the minimap's rim at 180 degrees (left of the map), a round button like the battlefield's; the menu's (app/
// MicroMenu) is under it at 215.
constexpr f32 kBackpackDegrees = 180.0f;

Rect backpackButtonRect(const DialogContext& c) {
    return minimapRimButtonRect(minimapLayout(c.safeArea, c.ui.px(ui::kUnitDp), c.ui.dp()), kBackpackDegrees);
}

Rect durabilityHudRect(const DialogContext& c) {
    const f32 unit = c.ui.px(ui::kUnitDp);
    const Rect button = backpackButtonRect(c);
    const f32 height = 60.0f * unit;
    const f32 width = height * 90.5f / 76.0f;
    return {button.x - width - c.ui.px(10.0f), button.centerY() - height * 0.45f, width, height};
}

ui::WidgetState backpackButton(DialogContext& c) {
    const MinimapLayout map = minimapLayout(c.safeArea, c.ui.px(ui::kUnitDp), c.ui.dp());
    return minimapRimButton(c.ui, c.draw, widgetId("hud.backpack"), map, kBackpackDegrees,
                            c.resources.icon(kBackpackIcon));
}

} // namespace vd
