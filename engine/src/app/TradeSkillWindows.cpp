#include "app/TradeSkillWindows.h"

#include "app/ChatLinkInsert.h"
#include "app/DialogKit.h"
#include "app/UiResources.h"
#include "app/WindowArt.h" // retouches (windows)
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cstdio>

namespace vd {
namespace {

using ui::Align;
using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::TextStyle;
using ui::WidgetId;
using ui::widgetId;
using namespace kit;

constexpr u64 kListboxHighlight = assetPathHash("Interface\\Buttons\\UI-Listbox-Highlight2.blp");
constexpr u64 kQuickslot = assetPathHash("Interface\\Buttons\\UI-Quickslot2.blp");
constexpr u64 kDropDownArrow = assetPathHash("Interface\\ChatFrame\\UI-ChatIcon-ScrollDown-Up.blp");
constexpr u64 kPlusUp = assetPathHash("Interface\\Buttons\\UI-PlusButton-Up.blp");
constexpr u64 kMinusUp = assetPathHash("Interface\\Buttons\\UI-MinusButton-Up.blp");
constexpr u64 kPlusMinusHighlight = assetPathHash("Interface\\Buttons\\UI-PlusButton-Hilight.blp");
// TradeSkillDecrementButton / IncrementButton: the spell book's page arrows.
constexpr u64 kPrevUp = assetPathHash("Interface\\Buttons\\UI-SpellbookIcon-PrevPage-Up.blp");
constexpr u64 kPrevDown = assetPathHash("Interface\\Buttons\\UI-SpellbookIcon-PrevPage-Down.blp");
constexpr u64 kPrevDisabled = assetPathHash("Interface\\Buttons\\UI-SpellbookIcon-PrevPage-Disabled.blp");
constexpr u64 kNextUp = assetPathHash("Interface\\Buttons\\UI-SpellbookIcon-NextPage-Up.blp");
constexpr u64 kNextDown = assetPathHash("Interface\\Buttons\\UI-SpellbookIcon-NextPage-Down.blp");
constexpr u64 kNextDisabled = assetPathHash("Interface\\Buttons\\UI-SpellbookIcon-NextPage-Disabled.blp");

constexpr u32 kWhiteText = ui::kWhite;             // HIGHLIGHT_FONT_COLOR
constexpr u32 kRedText = rgba(255, 26, 26);        // GameFontRedSmall (RED_FONT_COLOR)
constexpr u32 kHeaderColor = rgba(255, 209, 0);    // TradeSkillTypeColor["header"] (1, 0.82, 0)
constexpr u32 kGrayIconShade = rgba(0, 0, 0, 128); // SetItemButtonTextureVertexColor(0.5, 0.5, 0.5)

// TradeSkillRankFrame / CraftRankFrame: the blue bar (BarColor 0.25, 0.25, 0.75; SetStatusBarColor(0, 0, 1,
// 0.5) over its background's 0.75 blue), the skill line in GameFontNormalSmall and "rank/max" in white after
// it. Retouches (windows): UI-Character-Skills-Bar inside UI-Character-Skills-BarBorder, as the client draws
// it (the background: Color 1, 1, 1, 0.2 times SetVertexColor 0, 0, 0.75, 0.5).
void rankBar(Gui& g, const Metrics& m, const Rect& r, std::string_view name, u32 rank, u32 maxRank) {
    const f32 fill =
        maxRank > 0 ? std::clamp(static_cast<f32>(rank) / static_cast<f32>(maxRank), 0.0f, 1.0f) : 0.0f;
    art::skillsBar(g, r, fill, rgba(0, 0, 255, 128), art::kRankFrameBorder, rgba(0, 0, 191, 26));
    const TextStyle nameStyle = ui::gameFont(std::min(m.small, r.h * 0.8f));
    const f32 nameW = g.draw.measure(name, nameStyle.font, nameStyle.size);
    ui::label(g, {r.x + g.px(6.0f), r.y, nameW + g.px(2.0f), r.h}, name, nameStyle);
    char numbers[32];
    std::snprintf(numbers, sizeof(numbers), "%u/%u", rank, maxRank);
    ui::label(g, {r.x + g.px(6.0f) + nameW + g.px(13.0f), r.y, r.w * 0.5f, r.h}, numbers,
              ui::gameFont(std::min(m.small, r.h * 0.8f), kWhiteText));
}

// A page arrow (the count's - and +) inside the touch target `r`.
bool arrowButton(Gui& g, WidgetId id, const Rect& r, bool next, bool enabled) {
    const ui::WidgetState state = ui::hit(g, id, r);
    const bool down = enabled && ui::held(state, r);
    const f32 size = std::min(r.w, r.h);
    const u64 art = !enabled ? (next ? kNextDisabled : kPrevDisabled)
                    : down   ? (next ? kNextDown : kPrevDown)
                             : (next ? kNextUp : kPrevUp);
    g.draw.sprite({r.centerX() - size * 0.5f, r.centerY() - size * 0.5f, size, size}, art);
    return enabled && ui::tapped(state);
}

// The "All" collapse button (TradeSkillCollapseAllButton): the plus / minus and ALL.
bool collapseAllButton(Gui& g, const Metrics& m, WidgetId id, const Rect& r, bool collapsed, bool enabled) {
    const ui::WidgetState state = ui::hit(g, id, r); // the button and its label: one target
    const f32 size = std::min(r.h - g.px(6.0f), g.px(26.0f));
    const Rect art{r.x + g.px(4.0f), r.centerY() - size * 0.5f, size, size};
    g.draw.sprite(art, collapsed ? kPlusUp : kMinusUp, enabled ? kWhiteText : rgba(128, 128, 128));
    if (enabled && ui::held(state, r)) {
        g.draw.sprite(art, kPlusMinusHighlight);
    }
    TextStyle style = ui::gameFont(m.text, enabled ? kHeaderColor : ui::kDisabledText);
    ui::label(g, {art.right() + g.px(4.0f), r.y, r.right() - art.right() - g.px(4.0f), r.h}, text::get("ALL"),
              style);
    return enabled && ui::tapped(state);
}

// The recipe list (TradeSkillSkillButtonTemplate / CraftButtonTemplate rows in their scroll frame).
i32 recipeList(DialogContext& c, Gui& g, const Metrics& m, std::string_view name, const Rect& area,
               std::span<const RecipeRowView> rows, i32 selected) {
    ui::tooltipPanel(g, area, rgba(0, 0, 0, 150));
    const ui::ScrollView list = ui::beginScroll(g, widgetId(name, 1), area.inset(g.px(5.0f)));
    const f32 rowH = std::round(m.row * 0.85f);
    f32 y = list.top;
    i32 tapped = -1;
    for (u32 i = 0; i < rows.size(); ++i) {
        const RecipeRowView& row = rows[i];
        const Rect r{list.viewport.x, y, list.viewport.w, rowH};
        y += rowH;
        if (r.bottom() < list.viewport.y || r.y > list.viewport.bottom()) {
            continue; // scrolled out: nothing drawn, nothing to hit
        }
        const ui::WidgetState state = ui::hit(g, widgetId(name, 1000 + i), r);
        const bool isSelected = static_cast<i32>(i) == selected && !row.header;
        if (isSelected || (!row.header && ui::held(state, r))) {
            // TradeSkillHighlight: UI-Listbox-Highlight2 tinted by the row's colour.
            g.draw.sprite(r, kListboxHighlight, ui::withAlpha(row.color, isSelected ? 0.9f : 0.5f));
        }
        f32 x = r.x + g.px(4.0f);
        if (row.header) {
            // The header's UI-PlusButton / UI-MinusButton (the whole row is its target).
            const f32 size = std::min(rowH - g.px(6.0f), g.px(24.0f));
            const Rect art{x, r.centerY() - size * 0.5f, size, size};
            g.draw.sprite(art, row.expanded ? kMinusUp : kPlusUp);
            if (ui::held(state, r)) {
                g.draw.sprite(art, kPlusMinusHighlight);
            }
            x += size + g.px(6.0f);
        } else {
            x += g.px(12.0f); // the recipes' " " lead under the headers' button
        }
        const f32 costW = row.cost.empty() ? 0.0f : g.px(64.0f);
        char line[160];
        if (row.available > 0 && !row.header) {
            std::snprintf(line, sizeof(line), "%.*s [%u]", static_cast<int>(row.name.size()), row.name.data(),
                          row.available);
        } else {
            std::snprintf(line, sizeof(line), "%.*s", static_cast<int>(row.name.size()), row.name.data());
        }
        const TextStyle style = ui::gameFont(m.text, isSelected ? kWhiteText : row.color);
        const f32 textW = r.right() - x - costW - g.px(4.0f);
        const f32 lineW = std::min(textW, g.draw.measure(line, style.font, style.size));
        ui::label(g, {x, r.y, textW, r.h}, line, style);
        if (!row.sub.empty() && lineW + g.px(10.0f) < textW) {
            // CraftNSubText: the rank after the name, white while selected.
            ui::label(g, {x + lineW + g.px(10.0f), r.y, textW - lineW - g.px(10.0f), r.h}, row.sub,
                      ui::gameFont(m.small, isSelected ? kWhiteText : row.color));
        }
        if (costW > 0.0f) {
            TextStyle cost = ui::gameFont(m.small, isSelected ? kWhiteText : row.color);
            cost.align = Align::Right;
            ui::label(g, {r.right() - costW - g.px(6.0f), r.y, costW, r.h}, row.cost, cost);
        }
        if (ui::tapped(state)) {
            tapped = static_cast<i32>(i);
        }
    }
    ui::endScroll(g, list, y - list.top);
    (void)c;
    return tapped;
}

// The selected recipe: the product's icon (with how many it makes), its name, the requirements and the
// cooldown or the cost, the description (crafts), and the reagents two by two with "have /need" under each
// icon.
void recipeDetail(DialogContext& c, Gui& g, const Metrics& m, std::string_view name, const Rect& area,
                  const RecipeDetailView& v, bool& productTapped, i32& reagentTapped) {
    ui::tooltipPanel(g, area, rgba(0, 0, 0, 150));
    const ui::ScrollView view = ui::beginScroll(g, widgetId(name, 2), area.inset(g.px(6.0f)));
    const f32 x = view.viewport.x;
    const f32 w = view.viewport.w;
    f32 y = view.top;
    // TradeSkillSkillIcon (37 x 37) and its count (NumberFontNormal at the bottom right).
    const f32 icon = m.item * 1.1f;
    const Rect iconRect{x + g.px(4.0f), y + g.px(4.0f), icon, icon};
    if (v.iconHash != 0) { // retouches (windows): the question mark while it loads
        ui::iconOrPlaceholder(g.draw, iconRect, c.resources.icon(v.iconHash), ui::kWhite,
                              {0.08f, 0.08f, 0.92f, 0.92f});
    }
    const f32 frame = icon / 37.0f;
    g.draw.sprite({iconRect.centerX() - 32.0f * frame, iconRect.centerY() - 31.0f * frame, 64.0f * frame,
                   64.0f * frame},
                  kQuickslot);
    if (!v.made.empty()) {
        TextStyle count = ui::numberFont(m.small, kWhiteText);
        count.align = Align::Right;
        ui::label(g,
                  {iconRect.x, iconRect.bottom() - m.small * 1.2f, iconRect.w - g.px(3.0f), m.small * 1.2f},
                  v.made, count);
    }
    const ui::WidgetState iconState = ui::hit(g, widgetId(name, 3), iconRect);
    if (ui::tapped(iconState)) {
        productTapped = true;
    }
    // SetTradeSkillItem: the product's tooltip on a long press, or on a tap that links nothing (the chat is
    // closed).
    const bool linking = c.chatLink && c.chatLink->active();
    if ((iconState.longPress || (!linking && ui::tapped(iconState))) && v.itemEntry != 0) {
        kit::requestItemTooltip(v.itemEntry, iconRect);
    }
    // TradeSkillSkillName (GameFontNormal), then REQUIRES_LABEL and the tools / focus, the cooldown in red.
    const f32 textX = iconRect.right() + g.px(10.0f);
    const f32 textW = x + w - textX;
    f32 ty = y + g.px(2.0f);
    const TextStyle nameStyle = ui::gameFont(m.heading);
    ty += g.draw.wrappedText({textX, ty, textW, 1e6f}, v.name, nameStyle) + g.px(2.0f);
    const TextStyle small = ui::gameFont(m.small, kWhiteText);
    if (!v.requirements.empty()) {
        const std::string_view label = text::get("REQUIRES_LABEL");
        const f32 labelW = g.draw.measure(label, small.font, small.size);
        ui::label(g, {textX, ty, labelW + g.px(2.0f), m.small * 1.3f}, label, small);
        ty += g.draw.wrappedText({textX + labelW + g.px(4.0f), ty, textW - labelW - g.px(4.0f), 1e6f},
                                 v.requirements, small);
    }
    if (!v.cooldown.empty()) {
        ty += g.draw.wrappedText({textX, ty, textW, 1e6f}, v.cooldown, ui::gameFont(m.small, kRedText));
    }
    if (!v.cost.empty()) {
        ty += g.draw.wrappedText({textX, ty, textW, 1e6f}, v.cost, small);
    }
    y = std::max(iconRect.bottom(), ty) + g.px(10.0f);
    if (!v.description.empty()) {
        y += g.draw.wrappedText({x + g.px(4.0f), y, w - g.px(8.0f), 1e6f}, v.description, small) + g.px(8.0f);
    }
    // SPELL_REAGENTS and the reagents (TradeSkillItemTemplate = QuestItemTemplate), two a row.
    if (!v.reagents.empty()) {
        ui::label(g, {x + g.px(4.0f), y, w, m.small * 1.4f}, text::get("SPELL_REAGENTS"),
                  ui::gameFont(m.small));
        y += m.small * 1.5f;
        const f32 cellH = m.item;
        const f32 gap = g.px(6.0f);
        const u32 columns = w >= g.px(300.0f) ? 2 : 1;
        const f32 cellW = (w - gap * static_cast<f32>(columns - 1)) / static_cast<f32>(columns);
        for (u32 i = 0; i < v.reagents.size(); ++i) {
            const ReagentView& r = v.reagents[i];
            const Rect cell{x + static_cast<f32>(i % columns) * (cellW + gap), y, cellW, cellH};
            ItemView item = r.item;
            item.count = 0;                          // the counts are "have /need", drawn below
            item.quality = r.have >= r.need ? 1 : 0; // GRAY_FONT_COLOR when short (as the poor items' grey)
            if (kit::questItem(c, g, widgetId(name, 100 + i), cell, item, false, true)) {
                reagentTapped = static_cast<i32>(i);
            }
            const Rect iconCell{cell.x, cell.y, cell.h, cell.h};
            if (r.have < r.need) {
                g.draw.solid(iconCell, kGrayIconShade);
            }
            char count[24];
            if (r.have >= 100) {
                std::snprintf(count, sizeof(count), "* /%u", r.need);
            } else {
                std::snprintf(count, sizeof(count), "%u /%u", r.have, r.need);
            }
            TextStyle countStyle = ui::numberFont(std::min(m.small, cell.h * 0.36f), kWhiteText);
            countStyle.align = Align::Right;
            ui::label(g,
                      {iconCell.x - cell.h, iconCell.bottom() - cell.h * 0.38f, cell.h * 2.0f - g.px(2.0f),
                       cell.h * 0.38f},
                      count, countStyle);
            if (i % columns == columns - 1 || i + 1 == v.reagents.size()) {
                y += cellH + g.px(4.0f);
            }
        }
    }
    ui::endScroll(g, view, y - view.top + g.px(4.0f));
}

} // namespace

u32 recipeRowColor(game::RecipeColor color) {
    switch (color) {
    case game::RecipeColor::Optimal: return rgba(255, 128, 64); // (1, 0.5, 0.25)
    case game::RecipeColor::Medium: return rgba(255, 255, 0);
    case game::RecipeColor::Easy: return rgba(64, 191, 64); // (0.25, 0.75, 0.25)
    case game::RecipeColor::Trivial:
    case game::RecipeColor::Used: return rgba(128, 128, 128);
    case game::RecipeColor::None: return rgba(64, 191, 64);
    }
    return kWhiteText;
}

bool dropDownField(DialogContext& c, WidgetId id, const Rect& r, std::string_view label) {
    Gui g{c.ui, c.draw};
    const ui::WidgetState state = ui::hit(g, id, r);
    const bool down = ui::held(state, r);
    ui::tooltipPanel(g, r, rgba(0, 0, 0, down ? 220 : 170));
    const f32 arrow = std::min(r.h - g.px(8.0f), g.px(24.0f));
    g.draw.sprite({r.right() - arrow - g.px(4.0f), r.centerY() - arrow * 0.5f, arrow, arrow}, kDropDownArrow);
    ui::label(g, {r.x + g.px(8.0f), r.y, r.w - arrow - g.px(16.0f), r.h}, label,
              ui::gameFont(c.ui.px(12.5f), kWhiteText));
    return ui::tapped(state);
}

TradeSkillResult tradeSkillWindow(DialogContext& c, const TradeSkillView& v) {
    VD_PROFILE_ZONE("TradeSkillWindows::tradeSkill");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    TradeSkillResult result;
    const Dialog d = beginDialog(c, g, "tradeskill", wide ? 820.0f : 560.0f, wide ? 440.0f : 720.0f, v.title);
    const f32 gap = g.px(6.0f);
    const f32 barH = std::round(m.small * 1.6f);
    const f32 filterH = std::round(m.row * 0.85f);

    // Wide: the filters across the top, then the list at the left and the rank bar over the detail at the
    // right (the rank bar beside the filters when there is room). Square: the rank bar, the filters, the
    // list, then the detail under it (the original's order).
    Rect barRect, filterRect, listRect, detailRect;
    if (wide) {
        const bool roomy = d.body.w >= g.px(700.0f);
        const f32 listW = std::round(d.body.w * 0.52f);
        filterRect = {d.body.x, d.body.y, roomy ? listW : d.body.w, filterH};
        const f32 top = filterRect.bottom() + gap;
        listRect = {d.body.x, top, listW, d.body.bottom() - top};
        const f32 right = listRect.right() + gap;
        if (roomy) {
            barRect = {right, d.body.y + (filterH - barH) * 0.5f, d.body.right() - right, barH};
            detailRect = {right, top, barRect.w, listRect.h};
        } else {
            barRect = {right, top, d.body.right() - right, barH};
            detailRect = {right, barRect.bottom() + gap, barRect.w, d.body.bottom() - barRect.bottom() - gap};
        }
    } else {
        barRect = {d.body.x + g.px(8.0f), d.body.y, d.body.w - g.px(16.0f), barH};
        filterRect = {d.body.x, barRect.bottom() + gap, d.body.w, filterH};
        const f32 rest = d.body.bottom() - filterRect.bottom() - gap;
        listRect = {d.body.x, filterRect.bottom() + gap, d.body.w, std::round(rest * 0.48f)};
        detailRect = {d.body.x, listRect.bottom() + gap, d.body.w, d.body.bottom() - listRect.bottom() - gap};
    }
    rankBar(g, m, barRect, v.rankName, v.rank, v.maxRank);

    // TradeSkillExpandButtonFrame, TradeSkillSubClassDropDown, TradeSkillInvSlotDropDown.
    const f32 allW = std::min(g.px(84.0f), filterRect.w * 0.24f);
    if (collapseAllButton(g, m, widgetId("tradeskill.all"), {filterRect.x, filterRect.y, allW, filterRect.h},
                          v.allCollapsed, v.hasRows)) {
        result.toggleAll = true;
    }
    const f32 dropW = (filterRect.right() - filterRect.x - allW - gap * 2.0f) * 0.5f;
    const Rect subRect{filterRect.x + allW + gap, filterRect.y, dropW, filterRect.h};
    const Rect slotRect{subRect.right() + gap, filterRect.y, dropW, filterRect.h};
    result.subclassMenu =
        dropDownField(c, widgetId("tradeskill.subclass"), subRect, v.subclassText) && v.hasRows;
    result.slotMenu = dropDownField(c, widgetId("tradeskill.slots"), slotRect, v.slotText) && v.hasRows;

    result.select = recipeList(c, g, m, "tradeskill.list", listRect, v.rows, v.selected);
    if (v.hasDetail) {
        recipeDetail(c, g, m, "tradeskill.detail", detailRect, v.detail, result.productTapped,
                     result.reagentTapped);
    } else {
        ui::tooltipPanel(g, detailRect, rgba(0, 0, 0, 150));
    }

    // Footer, left to right as the original: Create All, the count (- box +), Create, Exit. In a narrow place
    // Exit goes first (the close button in the header does the same), then the buttons give up width (not the
    // arrows and box).
    const Rect f = d.footer;
    const f32 arrow = std::min(f.h, g.px(40.0f));
    const f32 boxW = g.px(48.0f);
    const f32 fixed = arrow * 2.0f + boxW + g.px(6.0f) + gap * 4.0f;
    f32 allButtonW = buttonWidth(g, text::get("CREATE_ALL"), f.h);
    f32 createW = buttonWidth(g, text::get("CREATE"), f.h);
    f32 exitW = buttonWidth(g, text::get("EXIT"), f.h);
    f32 room = std::max(0.0f, f.w - fixed);
    if (allButtonW + createW + exitW > room) {
        exitW = 0.0f;
        room += gap;
    }
    const f32 wanted = allButtonW + createW + exitW;
    if (wanted > room) {
        const f32 k = room / wanted;
        allButtonW *= k;
        createW *= k;
        exitW *= k;
    }
    f32 x = f.x;
    result.createAll = ui::button(g, widgetId("tradeskill.createAll"), {x, f.y, allButtonW, f.h},
                                  text::get("CREATE_ALL"), v.canCreate);
    x += allButtonW + gap;
    result.decrement = arrowButton(g, widgetId("tradeskill.dec"), {x, f.y, arrow, f.h}, false, v.hasDetail);
    x += arrow + g.px(3.0f);
    const Rect box{x, f.y + g.px(2.0f), boxW, f.h - g.px(4.0f)};
    result.countTapped = ui::textField(g, widgetId("tradeskill.count"), box, v.countText, {}, v.countEditing);
    x += boxW + g.px(3.0f);
    result.increment = arrowButton(g, widgetId("tradeskill.inc"), {x, f.y, arrow, f.h}, true, v.hasDetail);
    const f32 exitGap = exitW > 0.0f ? gap : 0.0f;
    result.create = ui::button(g, widgetId("tradeskill.create"),
                               {f.right() - exitW - exitGap - createW, f.y, createW, f.h},
                               text::get("CREATE"), v.canCreate);
    const bool exit = exitW > 0.0f && ui::button(g, widgetId("tradeskill.exit"),
                                                 {f.right() - exitW, f.y, exitW, f.h}, text::get("EXIT"));
    result.closed = d.closed || exit;
    return result;
}

CraftResult craftWindow(DialogContext& c, const CraftView& v) {
    VD_PROFILE_ZONE("TradeSkillWindows::craft");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    CraftResult result;
    const Dialog d = beginDialog(c, g, "craft", wide ? 820.0f : 560.0f, wide ? 440.0f : 720.0f, v.title);
    const f32 gap = g.px(6.0f);
    const f32 barH = std::round(m.small * 1.6f);
    const f32 filterH = v.headers ? std::round(m.row * 0.85f) : 0.0f;
    const bool bar = !v.rankName.empty();

    Rect barRect, filterRect, listRect, detailRect;
    if (wide) {
        const f32 listW = std::round(d.body.w * 0.5f);
        filterRect = {d.body.x, d.body.y, listW, filterH};
        const f32 listTop = filterRect.bottom() + (v.headers ? gap : 0.0f);
        listRect = {d.body.x, listTop, listW, d.body.bottom() - listTop};
        const f32 right = listRect.right() + gap;
        barRect = {right, d.body.y, d.body.right() - right, bar ? barH : 0.0f};
        const f32 detailTop = barRect.bottom() + (bar ? gap : 0.0f);
        detailRect = {right, detailTop, barRect.w, d.body.bottom() - detailTop};
    } else {
        barRect = {d.body.x + g.px(8.0f), d.body.y, d.body.w - g.px(16.0f), bar ? barH : 0.0f};
        filterRect = {d.body.x, barRect.bottom() + (bar ? gap : 0.0f), d.body.w, filterH};
        const f32 listTop = filterRect.bottom() + (v.headers || bar ? gap : 0.0f);
        const f32 rest = d.body.bottom() - listTop;
        listRect = {d.body.x, listTop, d.body.w, std::round(rest * 0.48f)};
        detailRect = {d.body.x, listRect.bottom() + gap, d.body.w, d.body.bottom() - listRect.bottom() - gap};
    }
    if (bar) {
        rankBar(g, m, barRect, v.rankName, v.rank, v.maxRank);
    }
    if (v.headers) {
        const f32 allW = std::min(g.px(84.0f), filterRect.w * 0.24f);
        result.toggleAll = collapseAllButton(
            g, m, widgetId("craft.all"), {filterRect.x, filterRect.y, allW, filterH}, v.allCollapsed, true);
    }
    result.select = recipeList(c, g, m, "craft.list", listRect, v.rows, v.selected);
    if (v.hasDetail) {
        recipeDetail(c, g, m, "craft.detail", detailRect, v.detail, result.productTapped,
                     result.reagentTapped);
    } else {
        ui::tooltipPanel(g, detailRect, rgba(0, 0, 0, 150));
    }

    // Footer: CraftFramePointsLabel / Text at the left (Beast Training), the craft button and Exit at the
    // right.
    const Rect f = d.footer;
    if (v.trainingPoints >= 0) {
        const std::string_view label = text::get("TRAINING_POINTS");
        const TextStyle labelStyle = ui::gameFont(m.small);
        const f32 labelW = g.draw.measure(label, labelStyle.font, labelStyle.size);
        ui::label(g, {f.x + g.px(6.0f), f.y, labelW + g.px(2.0f), f.h}, label, labelStyle);
        char points[16];
        std::snprintf(points, sizeof(points), "%d", v.trainingPoints);
        ui::label(g, {f.x + g.px(12.0f) + labelW, f.y, g.px(60.0f), f.h}, points,
                  ui::gameFont(m.small, kWhiteText));
    }
    const f32 exitW = std::min(buttonWidth(g, text::get("EXIT"), f.h), f.w * 0.24f);
    const f32 craftW = std::min(buttonWidth(g, v.button, f.h), f.w * 0.3f);
    result.craft = ui::button(g, widgetId("craft.do"), {f.right() - exitW - gap - craftW, f.y, craftW, f.h},
                              v.button, v.canCraft);
    const bool exit =
        ui::button(g, widgetId("craft.exit"), {f.right() - exitW, f.y, exitW, f.h}, text::get("EXIT"));
    result.closed = d.closed || exit;
    return result;
}

} // namespace vd
