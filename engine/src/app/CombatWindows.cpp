#include "app/CombatWindows.h"

#include "app/DialogKit.h"
#include "app/TalentBranches.h" // retouches (windows)
#include "app/UiResources.h"
#include "app/UnitTooltip.h" // retouches (windows)
#include "app/WindowArt.h"   // retouches (windows)
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iterator>

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

constexpr u64 kQuickslot = assetPathHash("Interface\\Buttons\\UI-Quickslot2.blp");
constexpr u64 kActionBorder = assetPathHash("Interface\\Buttons\\UI-ActionButton-Border.blp");
constexpr u64 kCheckedHighlight = assetPathHash("Interface\\Buttons\\CheckButtonHilight.blp");
constexpr u64 kAutocastOverlay = assetPathHash("Interface\\Buttons\\UI-AutoCastableOverlay.blp");
constexpr u64 kSlotHighlight = assetPathHash("Interface\\Buttons\\ButtonHilight-Square.blp");
constexpr u64 kListboxHighlight = assetPathHash("Interface\\Buttons\\UI-Listbox-Highlight.blp");
constexpr u64 kTabFlash = assetPathHash("Interface\\Buttons\\CheckButtonGlow.blp"); // phase 18 (extras)
constexpr u64 kStatusBar = assetPathHash("Interface\\TargetingFrame\\UI-StatusBar.blp");
constexpr u64 kSmallFrame = assetPathHash("Interface\\TargetingFrame\\UI-SmallTargetingFrame.blp");
constexpr u64 kHappiness = assetPathHash("Interface\\PetPaperDollFrame\\UI-PetHappiness.blp");
constexpr u64 kMonsterPortrait = assetPathHash("Interface\\CharacterFrame\\TemporaryPortrait-Monster.blp");

constexpr u32 kWhiteText = ui::kWhite;
constexpr u32 kGreenText = rgba(26, 255, 26);
constexpr u32 kRedText = rgba(255, 32, 32);
constexpr u32 kGrayText = rgba(150, 150, 150);
constexpr u32 kTooltipGold = ui::kGold;
// Power bar colors (PowerBarColor): mana, rage, focus, energy, happiness.
constexpr u32 kPowerColors[] = {rgba(0, 0, 255), rgba(255, 0, 0), rgba(255, 128, 64), rgba(255, 255, 0),
                                rgba(0, 255, 255)};

// An icon of the icon cache inside a quick slot frame (a spell or item button that is not a widget).
void iconButton(Gui& g, UiResources& resources, const Rect& r, u64 iconHash, u32 tint = ui::kWhite) {
    if (iconHash != 0) {
        ui::iconOrPlaceholder(g.draw, r, resources.icon(iconHash), tint); // retouches (windows)
    }
    const f32 scale = r.w / 37.0f;
    g.draw.sprite({r.centerX() - 32.0f * scale, r.centerY() - 31.0f * scale, 64.0f * scale, 64.0f * scale},
                  kQuickslot);
}

// A labelled line of two halves (the tooltip's "cost | range" rows).
void twoColumns(Gui& g, const Rect& r, std::string_view left, std::string_view right,
                const TextStyle& style) {
    if (!left.empty()) {
        ui::label(g, {r.x, r.y, r.w * 0.6f, r.h}, left, style);
    }
    if (!right.empty()) {
        TextStyle s = style;
        s.align = Align::Right;
        ui::label(g, {r.x + r.w * 0.4f, r.y, r.w * 0.6f, r.h}, right, s);
    }
}

// Retouches (windows): SpellBookSkillLineTabTemplate: the skill line's icon (32 units: `icon`) in
// SpellBook-SkillLineTab (64 units from 3 left of and 11 above it: its notch joins the book on the left),
// CheckButtonHilight (ADD) on the selected one, ButtonHilight-Square (ADD) while pressed. The whole `hitRect`
// is its target. Returns its state (held: the client's hover, which shows its name).
constexpr u64 kSkillLineTab = assetPathHash("Interface\\SpellBook\\SpellBook-SkillLineTab.blp");
ui::WidgetState skillLineTab(Gui& g, UiResources& resources, WidgetId id, const Rect& icon,
                             const Rect& hitRect, u64 iconHash, bool selected) {
    const ui::WidgetState state = ui::hit(g, id, hitRect);
    const f32 s = icon.w / 32.0f;
    g.draw.sprite({icon.x - 3.0f * s, icon.y - 11.0f * s, 64.0f * s, 64.0f * s}, kSkillLineTab);
    if (iconHash != 0) {
        ui::iconOrPlaceholder(g.draw, icon, resources.icon(iconHash)); // its NormalTexture: the whole icon
    }
    if (selected) {
        g.draw.spriteAdditive(icon, kCheckedHighlight);
    }
    if (ui::held(state, hitRect)) {
        g.draw.spriteAdditive(icon, kSlotHighlight);
    }
    return state;
}

// Retouches (windows): SpellBookFrameTabButtonTemplate (the spell book's and the pet's books under it): 128 x
// 64 units of UI-SpellBook-Tab-Unselected whose hit rectangle (HitRectInsets 15, 14, 13, 15: 99 x 36) is `r`;
// the selected one (disabled) UI-SpellBook-Tab1-Selected with GameFontHighlightSmall;
// UI-SpellbookPanel-Tab-Highlight (ADD) while pressed. The text 3 units above the art's middle. Returns true
// when tapped.
constexpr u64 kBookTab = assetPathHash("Interface\\SpellBook\\UI-SpellBook-Tab-Unselected.blp");
constexpr u64 kBookTabSelected = assetPathHash("Interface\\SpellBook\\UI-SpellBook-Tab1-Selected.blp");
constexpr u64 kBookTabHighlight = assetPathHash("Interface\\SpellBook\\UI-SpellbookPanel-Tab-Highlight.blp");
bool bookTab(Gui& g, WidgetId id, const Rect& r, std::string_view text, bool selected, f32 textSize) {
    const ui::WidgetState state = ui::hit(g, id, r);
    const f32 sx = r.w / 99.0f, sy = r.h / 36.0f;
    const Rect art{r.x - 15.0f * sx, r.y - 13.0f * sy, 128.0f * sx, 64.0f * sy};
    g.draw.sprite(art, selected ? kBookTabSelected : kBookTab);
    if (!selected && ui::held(state, r)) {
        g.draw.spriteAdditive(art, kBookTabHighlight);
    }
    TextStyle style = ui::gameFont(textSize, selected ? ui::kHighlight : ui::kGold);
    style.align = Align::Center;
    ui::label(g, {r.x + 6.0f * sx, art.y + 29.0f * sy - r.h * 0.5f, r.w - 12.0f * sx, r.h}, text, style);
    return ui::tapped(state) && !selected;
}

} // namespace

// --- Spell tooltip -----------------------------------------------------------------------------------------

f32 spellTooltip(DialogContext& c, Gui& g, const Rect& r, const SpellTooltipView& v) {
    const Metrics m = metrics(c);
    f32 y = r.y;
    if (v.name.empty()) {
        return 0.0f;
    }
    // Icon, name and rank (GameTooltip: the name white, the rank gray at the right).
    const f32 icon = m.item;
    f32 textX = r.x;
    if (v.iconHash != 0) {
        iconButton(g, c.resources, {r.x + g.px(2.0f), y + g.px(2.0f), icon, icon}, v.iconHash);
        textX += icon + g.px(10.0f);
    }
    const f32 lineH = m.text * 1.35f;
    TextStyle title = ui::gameFont(m.heading, kWhiteText);
    ui::label(g, {textX, y, r.right() - textX, lineH * 1.1f}, v.name, title);
    if (!v.rank.empty()) {
        ui::label(g, {textX, y + lineH * 1.1f, r.right() - textX, lineH}, v.rank,
                  ui::gameFont(m.small, kGrayText));
    }
    y += std::max(icon + g.px(6.0f), lineH * 2.2f);
    const TextStyle white = ui::gameFont(m.text, kWhiteText);
    if (!v.cost.empty() || !v.range.empty()) {
        twoColumns(g, {r.x, y, r.w, lineH}, v.cost, v.range, white);
        y += lineH;
    }
    if (!v.castTime.empty() || !v.cooldown.empty()) {
        twoColumns(g, {r.x, y, r.w, lineH}, v.castTime, v.cooldown, white);
        y += lineH;
    }
    if (!v.requirement.empty()) {
        y += g.draw.wrappedText({r.x, y, r.w, 1e6f}, v.requirement, ui::gameFont(m.text, kRedText));
    }
    if (!v.description.empty()) {
        y += g.px(4.0f);
        y += g.draw.wrappedText({r.x, y, r.w, 1e6f}, v.description, ui::gameFont(m.text, kTooltipGold));
    }
    if (!v.nextTitle.empty()) {
        y += g.px(10.0f);
        ui::label(g, {r.x, y, r.w, lineH}, v.nextTitle, white);
        y += lineH;
        y += g.draw.wrappedText({r.x, y, r.w, 1e6f}, v.nextDescription, ui::gameFont(m.text, kTooltipGold));
    }
    return y - r.y;
}

u32 spellTooltipLines(const SpellTooltipView& v, std::span<TooltipLineView> out) {
    u32 count = 0;
    const auto add = [&](const TooltipLineView& line) {
        if (count < out.size()) {
            out[count++] = line;
        }
    };
    if (v.name.empty()) {
        return 0;
    }
    add({v.name, v.rank, kWhiteText, kGrayText});
    if (!v.cost.empty() || !v.range.empty()) {
        add({v.cost, v.range});
    }
    if (!v.castTime.empty() || !v.cooldown.empty()) {
        add({v.castTime, v.cooldown});
    }
    if (!v.requirement.empty()) {
        add({v.requirement, {}, kRedText, kWhiteText, true});
    }
    if (!v.description.empty()) {
        add({v.description, {}, kTooltipGold, kWhiteText, true});
    }
    return count;
}

namespace {

// A dark tooltip panel with a scrolling spell tooltip inside.
void tooltipScroll(DialogContext& c, Gui& g, const Rect& r, WidgetId id, const SpellTooltipView& v,
                   std::string_view empty) {
    ui::tooltipPanel(g, r);
    const ui::ScrollView view = ui::beginScroll(g, id, r.inset(g.px(8.0f)));
    f32 used = 0.0f;
    if (!v.name.empty()) {
        used = spellTooltip(
            c, g, {view.viewport.x + g.px(2.0f), view.top + g.px(2.0f), view.viewport.w - g.px(8.0f), 0.0f},
            v);
    } else if (!empty.empty()) {
        const Metrics m = metrics(c);
        TextStyle style = ui::gameFont(m.text, ui::kDisabledText);
        used =
            g.draw.wrappedText({view.viewport.x, view.top + g.px(6.0f), view.viewport.w, 1e6f}, empty, style);
    }
    ui::endScroll(g, view, used + g.px(8.0f));
}

} // namespace

// --- Spell book --------------------------------------------------------------------------------------------

SpellbookResult spellbookWindow(DialogContext& c, const SpellbookView& v) {
    VD_PROFILE_ZONE("CombatWindows::spellbook");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    SpellbookResult result;
    const std::string_view title =
        v.picking ? text::get("VD_COMBAT_CHOOSE_SPELL_TITLE") : !v.title.empty() ? v.title : text::get("SPELLBOOK");
    const Dialog d = beginDialog(c, g, "spellbook", wide ? 860.0f : 580.0f, wide ? 440.0f : 720.0f, title);
    const f32 gap = g.px(6.0f);

    // Phase 15 (pets): SpellBookFrameTabButton1..2 along the bottom (the spell book, the pet's page) while
    // the pet has spells. Retouches (windows): in their art's proportions (99 x 36 units, 108 apart), wider
    // for a long name.
    Rect body = d.body;
    if (v.books.size() >= 2) {
        const f32 booksH = std::min(m.row * 0.85f, g.px(44.0f));
        body.h -= booksH + gap;
        const f32 textSize = std::min(m.small, booksH * 0.42f);
        f32 x = body.x + g.px(4.0f);
        for (u32 i = 0; i < v.books.size(); ++i) {
            const f32 w =
                std::max(booksH * 99.0f / 36.0f,
                         g.draw.measure(v.books[i], ui::FontId::Friz, textSize) + booksH * 24.0f / 36.0f);
            if (bookTab(g, widgetId("spellbook.book", i), {x, body.bottom() + gap, w, booksH}, v.books[i],
                        i == v.book, textSize)) {
                result.book = static_cast<i32>(i);
            }
            x += w * 108.0f / 99.0f;
        }
    }

    // The spells, the selected one beside them (wide) or under them (square). Retouches (windows): the skill
    // lines' tabs (SpellBookSkillLineTab1..8) in a column right of the spells, where they stand on the book's
    // edge, 49 units apart (their 32-unit icons 17 apart), as large as the column's height lets them be.
    // Phase 15 (pets): the pet's page has none (SpellBookFrame_Update hides them for BOOKTYPE_PET).
    const u32 tabCount = v.petBook ? 0u : static_cast<u32>(v.tabs.size());
    Rect spells, tipRect;
    if (wide) {
        const f32 tipW = std::min(g.px(290.0f), std::round(body.w * 0.4f));
        tipRect = {body.right() - tipW, body.y, tipW, body.h};
        spells = {body.x, body.y, tipRect.x - gap - body.x, body.h};
    } else {
        const f32 tipH = std::round(body.h * (v.petBook ? 0.36f : 0.34f));
        tipRect = {body.x, body.bottom() - tipH, body.w, tipH};
        spells = {body.x, body.y, body.w, tipRect.y - gap - body.y};
    }
    const f32 tabIcon = tabCount > 0 ? std::clamp(spells.h / (static_cast<f32>(tabCount) * 49.0f / 32.0f),
                                                  g.px(20.0f), g.px(36.0f))
                                     : 0.0f;
    const f32 tabUnit = tabIcon / 32.0f;
    const f32 tabsW = tabCount > 0 ? 40.0f * tabUnit : 0.0f; // the art reaches 39 units right of its notch
    const Rect gridRect{spells.x, spells.y, spells.w - tabsW, spells.h};
    i32 heldTab = -1;
    Rect heldRect;
    for (u32 i = 0; i < tabCount; ++i) {
        const f32 top = spells.y + static_cast<f32>(i) * 49.0f * tabUnit;
        const Rect icon{gridRect.right() + 3.0f * tabUnit, top + 11.0f * tabUnit, tabIcon, tabIcon};
        const Rect hitRect{gridRect.right(), top, tabsW, 49.0f * tabUnit};
        const ui::WidgetState state = skillLineTab(g, c.resources, widgetId("spellbook.tab", i), icon,
                                                   hitRect, v.tabs[i].iconHash, i == v.tab);
        if (ui::tapped(state)) {
            result.tab = static_cast<i32>(i);
        }
        if (ui::held(state, hitRect)) {
            heldTab = static_cast<i32>(i);
            heldRect = hitRect;
        }
        if (v.tabs[i].flash > 0.0f) {
            // Phase 18 (extras): SpellBookSkillLineTab<n>Flash, CheckButtonGlow (64 x 64 over the 32 x 32
            // tab).
            g.draw.spriteAdditive(
                {icon.centerX() - tabIcon, icon.centerY() - tabIcon, tabIcon * 2.0f, tabIcon * 2.0f},
                kTabFlash, ui::withAlpha(ui::kWhite, v.tabs[i].flash));
        }
    }

    // The spells of the tab: a grid of icon + name + rank cells.
    ui::tooltipPanel(g, gridRect, rgba(0, 0, 0, 150));
    const ui::ScrollView grid = ui::beginScroll(g, widgetId("spellbook.grid"), gridRect.inset(g.px(5.0f)));
    const f32 cellMinW = g.px(wide ? 160.0f : 170.0f);
    const u32 columns = std::max(1u, static_cast<u32>(grid.viewport.w / cellMinW));
    const f32 cellW = grid.viewport.w / static_cast<f32>(columns);
    const f32 cellH = m.item + g.px(12.0f);
    f32 y = grid.top;
    for (usize i = 0; i < v.entries.size(); ++i) {
        const SpellbookEntryView& e = v.entries[i];
        const u32 column = static_cast<u32>(i % columns);
        const Rect cell{grid.viewport.x + static_cast<f32>(column) * cellW, y, cellW, cellH};
        const bool selected = static_cast<i32>(i) == v.selected;
        const ui::WidgetState state = ui::hit(g, widgetId("spellbook.cell", i), cell);
        const bool down = ui::held(state, cell);
        if (selected || down) {
            g.draw.sprite(cell.inset(g.px(1.0f)), kListboxHighlight,
                          rgba(255, 255, 255, selected ? 230 : 140));
        }
        ui::SlotView slot;
        slot.icon = e.iconHash ? c.resources.icon(e.iconHash) : std::nullopt;
        slot.cooldown = e.cooldown;
        slot.usable = e.usable;
        const Rect icon{cell.x + g.px(6.0f), cell.y + g.px(6.0f), m.item, m.item};
        ui::itemSlot(g, widgetId("spellbook.icon", i), icon, slot);
        // Phase 15 (pets): SpellButton_UpdateButton's AutoCastable texture (60 x 60 over the 36-unit button) and,
        // retouches (ui models), its AutoCast model (SpellBookFrame.xml: 36 x 36 at (1, 1), model scale 1.22).
        if (e.autocastable) {
            g.draw.sprite(icon.inset(-m.item * 12.0f / 36.0f), kAutocastOverlay);
        }
        if (e.autocast) {
            const f32 unit = m.item / 36.0f;
            g.draw.models().autocast(g.draw, widgetId("spellbook.autocast", i),
                                     {icon.x + unit, icon.y - unit, icon.w, icon.h}, unit, 1.22f, g.ui.nowMs());
        }
        const f32 textX = icon.right() + g.px(8.0f);
        const f32 textW = cell.right() - textX - g.px(4.0f);
        TextStyle name = ui::gameFont(m.text, e.passive ? rgba(200, 200, 200) : ui::kGold);
        ui::label(g, {textX, icon.y, textW, m.item * 0.55f}, e.name, name);
        const std::string_view rank = e.passive ? text::get("SPELL_PASSIVE") : e.rank;
        ui::label(g, {textX, icon.y + m.item * 0.5f, textW, m.item * 0.5f}, rank,
                  ui::gameFont(m.small, kGrayText));
        if (ui::tapped(state)) {
            result.select = static_cast<i32>(i);
        }
        if (state.longPress && v.petBook) { // phase 15 (pets): the right click toggles the autocast
            result.autocast = static_cast<i32>(i);
        }
        if (column + 1 == columns || i + 1 == v.entries.size()) {
            y += cellH;
        }
    }
    if (v.entries.empty()) {
        TextStyle empty = ui::gameFont(m.text, ui::kDisabledText);
        empty.align = Align::Center;
        ui::label(g, {grid.viewport.x, grid.viewport.y, grid.viewport.w, m.row},
                  text::get("VD_COMBAT_BOOK_EMPTY"), empty);
    }
    ui::endScroll(g, grid, y - grid.top + g.px(4.0f));

    // The selected spell.
    tooltipScroll(c, g, tipRect, widgetId("spellbook.tooltip"), v.tooltip, text::get("VD_COMBAT_BOOK_HINT"));

    // Footer: the ranks toggle (not on the pet's page), then place / cast (or choose while picking).
    bool allRanks = v.allRanks;
    const f32 checkW = std::min(g.px(220.0f), d.footer.w * 0.4f);
    if (!v.petBook && ui::checkbox(g, widgetId("spellbook.ranks"),
                                   {d.footer.x + g.px(6.0f), d.footer.y, checkW, d.footer.h},
                                   text::get("VD_COMBAT_ALL_RANKS"), allRanks)) {
        result.toggleRanks = true;
    }
    const bool hasSelection = v.selected >= 0 && v.selected < static_cast<i32>(v.entries.size());
    const bool active = hasSelection && !v.entries[static_cast<usize>(v.selected)].passive;
    const std::string_view placeText = text::get(v.picking ? "VD_COMBAT_CHOOSE" : "VD_COMBAT_PLACE_ON_BAR");
    const f32 placeW = std::min(buttonWidth(g, placeText, d.footer.h), d.footer.w * 0.32f);
    result.place = ui::button(g, widgetId("spellbook.place"),
                              {d.footer.right() - placeW, d.footer.y, placeW, d.footer.h}, placeText, active);
    if (!v.picking) {
        const f32 castW =
            std::min(buttonWidth(g, text::get("VD_COMBAT_CAST"), d.footer.h), d.footer.w * 0.24f);
        result.cast = ui::button(g, widgetId("spellbook.cast"),
                                 {d.footer.right() - placeW - gap - castW, d.footer.y, castW, d.footer.h},
                                 text::get("VD_COMBAT_CAST"),
                                 active && v.entries[static_cast<usize>(v.selected)].usable);
    }
    if (heldTab >= 0) {
        // Retouches (windows): the skill line's tooltip (GameTooltip:SetText(this.tooltip): its name) while
        // held, as the client shows it on hover, right of the tab.
        const game::TooltipLine line{v.tabs[static_cast<usize>(heldTab)].name};
        HudTooltipView tip;
        tip.lines = {&line, 1};
        const HudTooltipSize size = measureHudTooltip(g, tip, std::min(g.px(300.0f), c.safeArea.w * 0.6f));
        hudTooltip(g, widgetId("spellbook.tabTip"), besideTooltipAnchor(g, c.safeArea, heldRect, true, size),
                   tip);
    }
    result.closed = d.closed;
    return result;
}

// --- Trainer -----------------------------------------------------------------------------------------------

TrainerResult trainerWindow(DialogContext& c, const TrainerView& v) {
    VD_PROFILE_ZONE("CombatWindows::trainer");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    TrainerResult result;
    const Dialog d = beginDialog(c, g, "trainer", wide ? 820.0f : 580.0f, wide ? 440.0f : 720.0f,
                                 v.npcName.empty() ? text::get("VD_COMBAT_TRAINER") : v.npcName);
    const f32 gap = g.px(6.0f);
    Rect listRect, detailRect;
    if (wide) {
        listRect = {d.body.x, d.body.y, d.body.w * 0.52f, d.body.h};
        detailRect = {listRect.right() + gap, d.body.y, d.body.right() - listRect.right() - gap, d.body.h};
    } else {
        listRect = {d.body.x, d.body.y, d.body.w, std::round(d.body.h * 0.55f)};
        detailRect = {d.body.x, listRect.bottom() + gap, d.body.w, d.body.bottom() - listRect.bottom() - gap};
    }

    // Filters (TrainerFrame's "available / unavailable / used" dropdown) over the list.
    const f32 filterH = g.px(40.0f);
    const f32 filterW = (listRect.w - gap * 2.0f) / 3.0f;
    bool available = v.showAvailable, unavailable = v.showUnavailable, known = v.showKnown;
    result.toggleAvailable =
        ui::checkbox(g, widgetId("trainer.filter", 0),
                     {listRect.x + g.px(6.0f), listRect.y, filterW - g.px(6.0f), filterH},
                     text::get("AVAILABLE"), available);
    result.toggleUnavailable =
        ui::checkbox(g, widgetId("trainer.filter", 1),
                     {listRect.x + filterW + gap + g.px(6.0f), listRect.y, filterW - g.px(6.0f), filterH},
                     text::get("UNAVAILABLE"), unavailable);
    result.toggleKnown = ui::checkbox(
        g, widgetId("trainer.filter", 2),
        {listRect.x + 2.0f * (filterW + gap) + g.px(6.0f), listRect.y, filterW - g.px(6.0f), filterH},
        text::get("USED"), known);
    const Rect rows{listRect.x, listRect.y + filterH + g.px(4.0f), listRect.w,
                    listRect.h - filterH - g.px(4.0f)};
    ui::tooltipPanel(g, rows, rgba(0, 0, 0, 150));
    const ui::ScrollView list = ui::beginScroll(g, widgetId("trainer.list"), rows.inset(g.px(5.0f)));
    f32 y = list.top;
    const f32 rowH = std::max(m.row, m.item * 0.8f + g.px(8.0f));
    for (usize i = 0; i < v.services.size(); ++i) {
        const TrainerServiceView& s = v.services[i];
        const Rect row{list.viewport.x, y, list.viewport.w, rowH};
        const bool selected = static_cast<i32>(i) == v.selected;
        const ui::WidgetState state = ui::hit(g, widgetId("trainer.row", i), row);
        if (selected || ui::held(state, row)) {
            g.draw.sprite(row, kListboxHighlight, rgba(255, 255, 255, selected ? 230 : 140));
        }
        const f32 icon = rowH - g.px(10.0f);
        iconButton(g, c.resources, {row.x + g.px(6.0f), row.centerY() - icon * 0.5f, icon, icon}, s.iconHash,
                   s.state == TrainerServiceState::Known ? rgba(140, 140, 140) : ui::kWhite);
        const u32 color = s.state == TrainerServiceState::Available     ? kGreenText
                          : s.state == TrainerServiceState::Unavailable ? kRedText
                                                                        : kGrayText;
        const f32 textX = row.x + icon + g.px(16.0f);
        const f32 moneyW = s.cost > 0 && s.state != TrainerServiceState::Known
                               ? ui::moneyWidth(g.draw, s.cost, m.small) + g.px(10.0f)
                               : 0.0f;
        ui::label(g, {textX, row.y + g.px(2.0f), row.right() - textX - moneyW, rowH * 0.55f}, s.name,
                  ui::gameFont(m.text, color));
        ui::label(g, {textX, row.y + rowH * 0.5f, row.right() - textX - moneyW, rowH * 0.45f}, s.rank,
                  ui::gameFont(m.small, kGrayText));
        if (moneyW >
            0.0f) { // retouches (windows): red when it cannot be paid (ClassTrainer_SetSelection's colour)
            ui::money(g, row.right() - g.px(6.0f), row.centerY(), s.cost, m.small, Align::Right, 1.0f,
                      s.cost <= v.money ? ui::kWhite : ui::kRed);
        }
        if (ui::tapped(state)) {
            result.select = static_cast<i32>(i);
        }
        y += rowH;
    }
    if (v.services.empty()) {
        TextStyle empty = ui::gameFont(m.text, ui::kDisabledText);
        empty.align = Align::Center;
        ui::label(g, {list.viewport.x, list.viewport.y, list.viewport.w, rowH},
                  text::get("VD_COMBAT_TRAINER_EMPTY"), empty);
    }
    ui::endScroll(g, list, y - list.top);

    // The selected service (or the greeting).
    if (v.tooltip.name.empty()) {
        const ParchmentScroll words = beginParchment(g, widgetId("trainer.greeting"), detailRect);
        Flow flow = parchmentFlow(g, words);
        flow.text(v.greeting, ui::questFont(m.text), g.px(8.0f));
        endParchment(g, words, flow);
    } else {
        tooltipScroll(c, g, detailRect, widgetId("trainer.detail"), v.tooltip, {});
    }

    // Footer: the player's money, "Train".
    const std::string_view moneyText = text::get("VD_COMBAT_YOUR_MONEY");
    const TextStyle moneyLabel = ui::gameFont(m.small, ui::kWhite);
    const f32 labelW = g.draw.measure(moneyText, moneyLabel.font, moneyLabel.size);
    ui::label(g, {d.footer.x + g.px(6.0f), d.footer.y, labelW + g.px(4.0f), d.footer.h}, moneyText,
              moneyLabel);
    ui::money(g, d.footer.x + g.px(14.0f) + labelW, d.footer.centerY(), v.money, m.text, Align::Left);
    const f32 learnW = std::min(buttonWidth(g, text::get("TRAIN"), d.footer.h), d.footer.w * 0.4f);
    result.learn =
        ui::button(g, widgetId("trainer.learn"), {d.footer.right() - learnW, d.footer.y, learnW, d.footer.h},
                   text::get("TRAIN"), v.canLearn);
    result.closed = d.closed;
    return result;
}

// --- Talents -----------------------------------------------------------------------------------------------

namespace {

// Interface\TalentFrame\<background>-<part>.blp
u64 talentBackground(std::string_view background, std::string_view part) {
    u64 hash = kFnvOffset;
    const auto mix = [&](std::string_view s) {
        for (const char ch : s) {
            hash ^= static_cast<u8>(normalizeAssetPathChar(ch));
            hash *= kFnvPrime;
        }
    };
    mix("Interface\\TalentFrame\\");
    mix(background);
    mix("-");
    mix(part);
    mix(".blp");
    return hash;
}

// Retouches (windows): TalentFrame.xml's art. TalentButtonTemplate: UI-EmptySlot-White behind the icon tinted
// by TalentFrame_Update, TalentFrame-RankBorder on its bottom right corner; the lines (TalentBranchTemplate)
// and arrows (TalentArrowTemplate) between the talents.
constexpr u64 kTalentBranches = assetPathHash("Interface\\TalentFrame\\UI-TalentBranches.blp");
constexpr u64 kTalentArrows = assetPathHash("Interface\\TalentFrame\\UI-TalentArrows.blp");
constexpr u64 kTalentRankBorder = assetPathHash("Interface\\TalentFrame\\TalentFrame-RankBorder.blp");
constexpr u64 kTalentSlot = assetPathHash("Interface\\Buttons\\UI-EmptySlot-White.blp");
constexpr u32 kTalentGreen =
    rgba(26, 255, 26);                         // (0.1, 1.0, 0.1): it can grow (GREEN_FONT_COLOR for the rank)
constexpr u32 kTalentGold = rgba(255, 209, 0); // (1.0, 0.82, 0): full (NORMAL_FONT_COLOR)
constexpr u32 kTalentGrey = rgba(128, 128, 128); // (0.5, 0.5, 0.5): out of reach (GRAY_FONT_COLOR)

// A piece of UI-TalentBranches (TALENT_BRANCH_TEXTURECOORDS: its u, the lit row on top, the grey one under
// it).
ui::TexCoords branchPiece(f32 u0, f32 u1, i8 value) {
    return value > 0 ? ui::TexCoords{u0, 0.0f, u1, 0.5f} : ui::TexCoords{u0, 0.5f, u1, 1.0f};
}

// The straight lines, any length: the middle of the "down" piece (vertical) or of the "right" piece
// (horizontal) stretched along the line, `piece` (its 32 units) across it.
void branchVertical(ui::DrawList& draw, f32 cx, f32 y0, f32 y1, f32 piece, i8 value) {
    if (value != 0 && y1 > y0) {
        const f32 v0 = value > 0 ? 0.1f : 0.6f;
        draw.sprite({cx - piece * 0.5f, y0, piece, y1 - y0}, kTalentBranches, ui::kWhite,
                    {0.0f, v0, 0.125f, v0 + 0.3f});
    }
}
void branchHorizontal(ui::DrawList& draw, f32 x0, f32 x1, f32 cy, f32 piece, i8 value) {
    if (value != 0 && x1 > x0) {
        draw.sprite({x0, cy - piece * 0.5f, x1 - x0, piece}, kTalentBranches, ui::kWhite,
                    branchPiece(0.2734375f, 0.3671875f, value));
    }
}

// TalentFrame_Update's pieces of one cell, stretched to the window's cells (the client's are 63 units apart,
// its pieces 32): straight lines from the middle of a talent's cell (under its button) or from the joint
// piece of an empty one out to the cell's edges, where the next cell's go on. An arrow's line reaches its
// talent (the client's "down" piece reaches under the next button; ours stops at the cell's edge).
void talentCellBranches(ui::DrawList& draw, const TalentBranchCell& cell, f32 cx, f32 cy, f32 halfW,
                        f32 halfH, f32 piece) {
    i8 up = cell.up, down = cell.down, left = cell.left, right = cell.right;
    f32 inner = 0.0f; // where the straight lines start, from the middle
    if (cell.talent) {
        up = up != 0 ? up : cell.topArrow;
        left = left != 0 ? left : cell.leftArrow;
        right = right != 0 ? right : cell.rightArrow;
    } else {
        const TalentJoint joint = talentJoint(cell);
        ui::TexCoords uv{};
        bool keepUp = false, keepDown = false, keepLeft = false, keepRight = false; // what the joint draws
        switch (joint) {
        case TalentJoint::None: return;
        case TalentJoint::TeeUp:
            uv = branchPiece(0.7734375f, 0.8984375f, cell.up);
            keepUp = keepLeft = keepRight = true;
            break;
        case TalentJoint::TeeDown:
            uv = branchPiece(0.64453125f, 0.76953125f, cell.down);
            keepDown = keepLeft = keepRight = true;
            break;
        case TalentJoint::TopRight:
            uv = branchPiece(0.515625f, 0.640625f, cell.left);
            keepLeft = keepDown = true;
            break;
        case TalentJoint::BottomRight:
            uv = branchPiece(0.38671875f, 0.51171875f, cell.left);
            keepLeft = keepUp = true;
            break;
        case TalentJoint::TopLeft:
            uv = branchPiece(0.640625f, 0.515625f, cell.right);
            keepRight = keepDown = true;
            break;
        case TalentJoint::BottomLeft:
            uv = branchPiece(0.51171875f, 0.38671875f, cell.right);
            keepRight = keepUp = true;
            break;
        case TalentJoint::Across: keepLeft = keepRight = true; break;
        case TalentJoint::Through: keepUp = keepDown = true; break;
        }
        up = keepUp ? up : 0;
        down = keepDown ? down : 0;
        left = keepLeft ? left : 0;
        right = keepRight ? right : 0;
        if (joint != TalentJoint::Across && joint != TalentJoint::Through) {
            draw.sprite({cx - piece * 0.5f, cy - piece * 0.5f, piece, piece}, kTalentBranches, ui::kWhite,
                        uv);
            inner = piece * 0.5f;
        }
    }
    branchVertical(draw, cx, cy - halfH, cy - inner, piece, up);
    branchVertical(draw, cx, cy + inner, cy + halfH, piece, down);
    branchHorizontal(draw, cx - halfW, cx - inner, cy, piece, left);
    branchHorizontal(draw, cx + inner, cx + halfW, cy, piece, right);
}

// TalentFrame_SetArrowTexture's arrows into a talent (TALENT_ARROW_TEXTURECOORDS), from its button's top left
// (bx, by) as the client offsets them; `s`: pixels per unit. They lie over the buttons
// (TalentFrameArrowFrame).
void talentArrows(ui::DrawList& draw, const TalentBranchCell& cell, f32 bx, f32 by, f32 s) {
    const f32 size = 32.0f * s;
    if (cell.topArrow != 0) {
        draw.sprite({bx + 2.0f * s, by - 19.0f * s, size, size}, kTalentArrows, ui::kWhite,
                    cell.topArrow > 0 ? ui::TexCoords{0.0f, 0.0f, 0.5f, 0.5f}
                                      : ui::TexCoords{0.0f, 0.5f, 0.5f, 1.0f});
    }
    if (cell.rightArrow != 0) { // from the right: the arrow mirrored, pointing left
        draw.sprite({bx + 23.0f * s, by + 2.0f * s, size, size}, kTalentArrows, ui::kWhite,
                    cell.rightArrow > 0 ? ui::TexCoords{1.0f, 0.0f, 0.5f, 0.5f}
                                        : ui::TexCoords{1.0f, 0.5f, 0.5f, 1.0f});
    }
    if (cell.leftArrow != 0) {
        draw.sprite({bx - 19.0f * s, by + 2.0f * s, size, size}, kTalentArrows, ui::kWhite,
                    cell.leftArrow > 0 ? ui::TexCoords{0.5f, 0.0f, 1.0f, 0.5f}
                                       : ui::TexCoords{0.5f, 0.5f, 1.0f, 1.0f});
    }
}

} // namespace

TalentResult talentWindow(DialogContext& c, const TalentView& v) {
    VD_PROFILE_ZONE("CombatWindows::talents");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    TalentResult result;
    const Dialog d =
        beginDialog(c, g, "talents", wide ? 860.0f : 580.0f, wide ? 450.0f : 740.0f, text::get("TALENTS"));
    const f32 gap = g.px(6.0f);

    // The trees' tabs on top (TalentTabTemplate: CharacterFrameTabButtonTemplate, the tree's name alone), the
    // tree, and the selected talent beside it (wide) or under it (square).
    const f32 tabH = m.row * 0.9f;
    const u32 treeCount = static_cast<u32>(std::min<usize>(v.trees.size(), 5)); // MAX_TALENT_TABS
    Rect tabsRect, treeRect, tipRect;
    if (wide) {
        const f32 tipW = std::round(d.body.w * 0.42f);
        tabsRect = {d.body.x, d.body.y, d.body.w - tipW - gap, tabH};
        tipRect = {d.body.right() - tipW, d.body.y, tipW, d.body.h};
        treeRect = {d.body.x, tabsRect.bottom() + gap, tipRect.x - gap - d.body.x,
                    d.body.bottom() - tabsRect.bottom() - gap};
    } else {
        const f32 tipH = std::round(d.body.h * 0.27f);
        tabsRect = {d.body.x, d.body.y, d.body.w, tabH};
        tipRect = {d.body.x, d.body.bottom() - tipH, d.body.w, tipH};
        treeRect = {d.body.x, tabsRect.bottom() + gap, d.body.w, tipRect.y - gap - tabsRect.bottom() - gap};
    }
    std::array<std::string_view, 5> names{};
    for (u32 i = 0; i < treeCount; ++i) {
        names[i] = v.trees[i].name;
    }
    const i32 tab = art::characterTabBar(g, "talents.tabs", tabsRect, {names.data(), treeCount}, v.tree,
                                         m.small, g.px(70.0f));
    if (tab >= 0) {
        result.tree = tab;
    }

    if (v.tree < treeCount) {
        const TalentTreeView& tree = v.trees[v.tree];
        // The tree's art: four quarters of a 320 x 384 picture (256 + 64 wide, 256 + 128 tall) of which the
        // client's scroll frame shows the top left 296 x 332.
        g.draw.pushClip(treeRect);
        g.draw.solid(treeRect, rgba(0, 0, 0, 200));
        if (!tree.background.empty()) {
            const f32 sx = treeRect.w / 296.0f, sy = treeRect.h / 332.0f;
            g.draw.sprite({treeRect.x, treeRect.y, 256.0f * sx, 256.0f * sy},
                          talentBackground(tree.background, "TopLeft"));
            g.draw.sprite({treeRect.x + 256.0f * sx, treeRect.y, 64.0f * sx, 256.0f * sy},
                          talentBackground(tree.background, "TopRight"));
            g.draw.sprite({treeRect.x, treeRect.y + 256.0f * sy, 256.0f * sx, 128.0f * sy},
                          talentBackground(tree.background, "BottomLeft"));
            g.draw.sprite({treeRect.x + 256.0f * sx, treeRect.y + 256.0f * sy, 64.0f * sx, 128.0f * sy},
                          talentBackground(tree.background, "BottomRight"));
        }
        g.draw.popClip();

        // The grid: 4 columns, as many rows as the tree has (7 in 1.12), scrolled when short.
        u32 rows = 1;
        std::array<TalentNode, 32> nodes{}; // MAX_NUM_TALENTS is 20
        const u32 nodeCount = static_cast<u32>(std::min<usize>(tree.talents.size(), nodes.size()));
        for (u32 i = 0; i < nodeCount; ++i) {
            const TalentButtonView& t = tree.talents[i];
            rows = std::max(rows, t.row + 1);
            // TalentFrame_Update: its requirements met, or points in it (a talent with no point while the
            // player has none to spend is forced grey); its line from the prerequisite shares its state.
            nodes[i] = {t.row, t.column, t.prerequisite, t.available || t.rank > 0};
        }
        TalentBranchGrid branches;
        layoutTalentBranches({nodes.data(), nodeCount}, branches);
        const ui::ScrollView view = ui::beginScroll(g, widgetId("talents.tree"), treeRect.inset(g.px(4.0f)));
        const f32 cellW = view.viewport.w / 4.0f;
        const f32 button =
            std::clamp(std::min(cellW * 0.62f, view.viewport.h / static_cast<f32>(rows) * 0.72f), g.px(34.0f),
                       g.px(52.0f));
        const f32 cellH = std::max(button + g.px(16.0f), view.viewport.h / static_cast<f32>(rows));
        const f32 s = button / 37.0f; // the client's units: ItemButtonTemplate's 37-unit icon
        const auto centerX = [&](u32 column) {
            return view.viewport.x + (static_cast<f32>(column) + 0.5f) * cellW;
        };
        const auto centerY = [&](u32 row) { return view.top + (static_cast<f32>(row) + 0.5f) * cellH; };
        // The lines first (the scroll child's textures), under the buttons.
        for (u32 row = 0; row < std::min(rows, kTalentTiers); ++row) {
            for (u32 column = 0; column < kTalentColumns; ++column) {
                talentCellBranches(g.draw, branches.at(row, column), centerX(column), centerY(row),
                                   cellW * 0.5f, cellH * 0.5f, 32.0f * s);
            }
        }
        for (u32 i = 0; i < nodeCount; ++i) {
            const TalentButtonView& t = tree.talents[i];
            const f32 x = centerX(t.column), y = centerY(t.row);
            const Rect r{x - button * 0.5f, y - button * 0.5f, button, button};
            const bool lit = nodes[i].lit;
            const bool maxed = t.rank >= t.maxRank;
            // The slot behind it: green while it can grow, gold when full, grey out of reach.
            const u32 tint = !lit ? kTalentGrey : maxed ? kTalentGold : kTalentGreen;
            g.draw.sprite({x - 32.0f * s, y - 32.0f * s, 64.0f * s, 64.0f * s}, kTalentSlot, tint);
            ui::SlotView slot;
            slot.icon = t.iconHash ? c.resources.icon(t.iconHash) : std::nullopt;
            slot.selected = static_cast<i32>(i) == v.selected; // the one in the tooltip: hovered, in 1.12
            slot.desaturated = !lit; // SetItemButtonDesaturated(button, 1, 0.65, 0.65, 0.65)
            const ui::WidgetState state = ui::itemSlot(g, widgetId("talents.button", i), r, slot);
            if (lit || t.rank > 0) {
                // The rank alone (TalentFrameTalent<n>Rank: GameFontNormalSmall) on the RankBorder plate,
                // centered on the button's bottom right corner; grey with the plate out of reach, hidden
                // there without points.
                const Rect plate{r.right() - 16.0f * s, r.bottom() - 16.0f * s, 32.0f * s, 32.0f * s};
                g.draw.sprite(plate, kTalentRankBorder, lit ? ui::kWhite : kTalentGrey);
                char rank[12];
                std::snprintf(rank, sizeof(rank), "%u", t.rank);
                TextStyle style = ui::gameFont(10.0f * s * 1.2f, tint);
                style.align = Align::Center;
                g.draw.text(plate.centerX(), ui::centeredBaseline(g.draw, style, plate.centerY()), rank,
                            style);
            }
            if (state.clicked) {
                result.select = static_cast<i32>(i);
            }
        }
        // The arrows over the buttons (TalentFrameArrowFrame).
        for (u32 i = 0; i < nodeCount; ++i) {
            const TalentButtonView& t = tree.talents[i];
            if (t.row < kTalentTiers && t.column < kTalentColumns) {
                talentArrows(g.draw, branches.at(t.row, t.column), centerX(t.column) - button * 0.5f,
                             centerY(t.row) - button * 0.5f, s);
            }
        }
        ui::endScroll(g, view, static_cast<f32>(rows) * cellH);
    }

    // The selected talent.
    tooltipScroll(c, g, tipRect, widgetId("talents.tooltip"), v.tooltip,
                  v.notice.empty() ? text::get("VD_COMBAT_TALENT_HINT") : v.notice);

    // Footer: TalentFrameSpentPoints ("Points spent in Fire Talents: 6") and TalentFrameTalentPoints ("Talent
    // Points: 2"): their labels in GameFontNormalSmall, the numbers in GameFontHighlightSmall; then "Learn".
    const f32 learnW = std::min(buttonWidth(g, text::get("VD_COMBAT_LEARN"), d.footer.h), d.footer.w * 0.4f);
    const TextStyle label = ui::gameFont(m.small);
    const TextStyle number = ui::gameFont(m.small, ui::kHighlight);
    const f32 baseline = ui::centeredBaseline(g.draw, label, d.footer.centerY());
    f32 x = d.footer.x + g.px(6.0f);
    char buffer[96];
    if (v.tree < treeCount) {
        x += g.draw.text(x, baseline, text::format(buffer, "MASTERY_POINTS_SPENT", v.trees[v.tree].name),
                         label) +
             g.px(4.0f);
        std::snprintf(buffer, sizeof(buffer), "%u", v.trees[v.tree].points);
        x += g.draw.text(x, baseline, buffer, number) + g.px(18.0f);
    }
    x += g.draw.text(x, baseline, text::get("CHARACTER_POINTS1_COLON"), label) + g.px(4.0f);
    std::snprintf(buffer, sizeof(buffer), "%u", v.freePoints);
    g.draw.text(x, baseline, buffer, number);
    result.learn =
        ui::button(g, widgetId("talents.learn"), {d.footer.right() - learnW, d.footer.y, learnW, d.footer.h},
                   text::get("VD_COMBAT_LEARN"), v.canLearn);
    result.closed = d.closed;
    return result;
}

// --- Pet (HUD) ---------------------------------------------------------------------------------------------

PetFrameResult petFrame(Gui& g, const Rect& area, const PetFrameView& v) {
    // PetFrame (FrameXML/PetFrame.xml): 128 x 64 art, a 37-unit portrait, 69 x 8 bars.
    const f32 s = area.w / 128.0f;
    const ui::WidgetState state = g.ui.widget(widgetId("pet.frame"), area);
    g.draw.solid({area.x + 45.0f * s, area.y + 19.0f * s, 73.0f * s, 20.0f * s}, rgba(0, 0, 0, 140));
    g.draw.portrait({area.x + 7.0f * s, area.y + 6.0f * s, 37.0f * s, 37.0f * s}, v.portrait,
                    kMonsterPortrait);
    const f32 health = std::clamp(v.health, 0.0f, 1.0f), power = std::clamp(v.power, 0.0f, 1.0f);
    g.draw.sprite({area.x + 47.0f * s, area.y + 22.0f * s, 69.0f * s * health, 8.0f * s}, kStatusBar,
                  rgba(0, 255, 0), {0.0f, 0.0f, health, 1.0f});
    g.draw.sprite({area.x + 47.0f * s, area.y + 30.0f * s, 69.0f * s * power, 8.0f * s}, kStatusBar,
                  kPowerColors[std::min<u32>(v.powerType, 4)], {0.0f, 0.0f, power, 1.0f});
    g.draw.sprite(area, kSmallFrame, v.selected ? rgba(255, 240, 160) : ui::kWhite);
    TextStyle name = ui::gameFont(10.0f * s * 1.2f, ui::kGold);
    name.shadow = std::max(1.0f, s);
    ui::label(g, {area.x + 46.0f * s, area.y + 6.0f * s, 76.0f * s, 14.0f * s}, v.name, name);
    if (v.level > 0) {
        char level[8];
        std::snprintf(level, sizeof(level), "%u", v.level);
        TextStyle style = ui::numberFont(9.0f * s * 1.2f);
        style.align = Align::Center;
        g.draw.solid({area.x + 8.0f * s, area.y + 34.0f * s, 14.0f * s, 10.0f * s}, rgba(0, 0, 0, 170));
        g.draw.text(area.x + 15.0f * s, area.y + 43.0f * s, level, style);
    }
    if (v.happiness > 0) { // a hunter pet's mood (PetFrame_SetHappiness): happy, content, unhappy faces
        const f32 u0 = v.happiness >= 3 ? 0.0f : v.happiness == 2 ? 0.1875f : 0.375f;
        g.draw.sprite({area.x + 118.0f * s, area.y + 16.0f * s, 24.0f * s, 23.0f * s}, kHappiness, ui::kWhite,
                      {u0, 0.0f, u0 + 0.1875f, 0.359375f});
    }
    return {state.clicked, state.longPress};
}

PetBarResult petBar(Gui& g, UiResources& resources, const Rect& area, std::span<const PetSlotView> slots) {
    PetBarResult result;
    const u32 count = static_cast<u32>(std::min<usize>(slots.size(), 10));
    if (count == 0) {
        return result;
    }
    const f32 size = area.h;
    const f32 gap =
        count > 1 ? (area.w - size * static_cast<f32>(count)) / static_cast<f32>(count - 1) : 0.0f;
    for (u32 i = 0; i < count; ++i) {
        const PetSlotView& s = slots[i];
        const Rect r{area.x + static_cast<f32>(i) * (size + gap), area.y, size, size};
        ui::WidgetState state = g.ui.widget(widgetId("pet.slot", i), r);
        g.draw.solid(r, rgba(0, 0, 0, 110));
        const u32 tint = !s.usable ? rgba(110, 110, 110) : state.pressed ? rgba(190, 190, 190) : ui::kWhite;
        iconButton(g, resources, r, s.iconHash, tint);
        // Retouches (ui models): PetActionButton_OnLoad's Cooldown (33 x 33 at (-2, -1) of the 30-unit button, frame
        // scale 1, the template's model scale 0.75).
        const f32 unit = size / 30.0f;
        g.draw.models().cooldown(g.draw, widgetId("pet.slot", i),
                                 {r.centerX() - 18.5f * unit, r.centerY() - 15.5f * unit, 33.0f * unit, 33.0f * unit},
                                 unit, s.cooldown, -1.0f, g.ui.nowMs());
        if (s.active) {
            g.draw.spriteAdditive(r, kCheckedHighlight);
        }
        if (s.autocastable) { // PetActionButtonTemplate's AutoCastable: 58 x 58 over the 30-unit button
            g.draw.sprite(r.inset(-14.0f * unit), kAutocastOverlay);
        }
        if (s.autocast) { // retouches (ui models): its AutoCast model (setAllPoints, model scale 1.2)
            g.draw.models().autocast(g.draw, widgetId("pet.autocast", i), r, unit, 1.2f, g.ui.nowMs());
        }
        if (s.target) { // phase 15 (pets): where the spell being placed may go (PET_BAR_SHOWGRID)
            const f32 pulse = 0.5f + 0.5f * std::sin(static_cast<f32>(g.ui.nowMs() % 1000) * 0.00628f);
            g.draw.spriteAdditive(r, kSlotHighlight, ui::withAlpha(rgba(120, 255, 120), 0.4f + 0.5f * pulse));
        }
        if (state.pressed) {
            g.draw.sprite(r, kSlotHighlight);
        }
        if (state.longPress) {
            result.held = static_cast<i32>(i);
        }
        if (state.clicked && (s.iconHash != 0 || s.target)) {
            result.tapped = static_cast<i32>(i);
        }
        // Phase 15 (pets): PetActionButton_OnDragStart / OnReceiveDrag: a finger dragged from this button released on
        // another moves it there (the icon follows the finger meanwhile).
        if (state.touch && state.touch->moved && s.iconHash != 0) {
            if (state.pressed) {
                if (const std::optional<u32> icon = resources.icon(s.iconHash)) {
                    g.draw.icon({state.touch->x - size * 0.5f, state.touch->y - size * 0.5f, size, size}, *icon,
                                ui::withAlpha(ui::kWhite, 0.8f));
                }
            } else if (state.released) {
                for (u32 j = 0; j < count; ++j) {
                    const Rect other{area.x + static_cast<f32>(j) * (size + gap), area.y, size, size};
                    if (j != i && other.inset(-gap * 0.5f).contains(state.touch->x, state.touch->y)) {
                        result.dragFrom = static_cast<i32>(i);
                        result.dragTo = static_cast<i32>(j);
                    }
                }
            }
        }
    }
    return result;
}

// --- Action slot editor ------------------------------------------------------------------------------------

ActionEditResult actionEditDialog(DialogContext& c, const ActionEditView& v) {
    VD_PROFILE_ZONE("CombatWindows::actionEdit");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    ActionEditResult result;
    char title[96];
    text::format(title, "VD_COMBAT_ACTION_TITLE", v.position, v.page);
    const f32 buttonH = std::max(m.button, g.px(42.0f));
    const bool tip = !v.tooltip.name.empty();
    const f32 tipH = tip ? g.px(c.profile == ScreenProfile::Wide ? 130.0f : 190.0f) : 0.0f;
    const f32 heightDp = (m.item + tipH + buttonH * 2.0f + g.px(40.0f)) / c.ui.dp() + 90.0f;
    const Dialog d = beginDialog(c, g, "actionEdit", 480.0f, heightDp, title, true, false);
    f32 top = d.body.y;
    if (tip) {
        // The spell's tooltip (its icon and name included), scrolling when the screen is short.
        const f32 h = std::max(m.item + g.px(16.0f), d.body.h - buttonH * 2.0f - g.px(26.0f));
        tooltipScroll(c, g, {d.body.x, top, d.body.w, h}, widgetId("actionEdit.tooltip"), v.tooltip, {});
        top += h + g.px(16.0f);
    } else {
        // What it holds now.
        const Rect icon{d.body.x + g.px(8.0f), d.body.y + g.px(4.0f), m.item, m.item};
        if (v.iconHash != 0) {
            iconButton(g, c.resources, icon, v.iconHash);
        } else {
            ui::SlotView empty;
            empty.empty = true;
            ui::itemSlot(g, widgetId("actionEdit.icon"), icon, empty);
        }
        ui::label(g,
                  {icon.right() + g.px(10.0f), icon.y, d.body.right() - icon.right() - g.px(10.0f), icon.h},
                  v.iconHash != 0 ? v.name : text::get("EMPTY"), ui::gameFont(m.text, ui::kWhite));
        top = icon.bottom() + g.px(16.0f);
    }
    // Two rows of two buttons.
    const f32 w = (d.body.w - g.px(12.0f)) * 0.5f;
    result.pickSpell = ui::button(g, widgetId("actionEdit.spell"), {d.body.x, top, w, buttonH},
                                  text::get("VD_COMBAT_PICK_SPELL"));
    result.pickItem =
        ui::button(g, widgetId("actionEdit.item"), {d.body.x + w + g.px(12.0f), top, w, buttonH},
                   text::get("VD_COMBAT_PICK_ITEM"));
    result.clear =
        ui::button(g, widgetId("actionEdit.clear"), {d.body.x, top + buttonH + g.px(10.0f), w, buttonH},
                   text::get("VD_COMBAT_CLEAR_SLOT"), v.iconHash != 0);
    const bool cancel = ui::button(g, widgetId("actionEdit.cancel"),
                                   {d.body.x + w + g.px(12.0f), top + buttonH + g.px(10.0f), w, buttonH},
                                   text::get("CANCEL"));
    result.closed = cancel || d.closed || d.outside;
    return result;
}

i32 itemPicker(DialogContext& c, std::span<const ItemView> items, bool& closed) {
    VD_PROFILE_ZONE("CombatWindows::itemPicker");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const f32 rowH = m.item + g.px(8.0f);
    const f32 contentDp = static_cast<f32>(std::max<usize>(items.size(), 1)) * rowH / c.ui.dp();
    const Dialog d = beginDialog(c, g, "itemPicker", 420.0f, contentDp + 120.0f,
                                 text::get("VD_COMBAT_CHOOSE_ITEM_TITLE"), true, false);
    i32 tapped = -1;
    const ui::ScrollView view = ui::beginScroll(g, widgetId("itemPicker.list"), d.body);
    f32 y = view.top + g.px(2.0f);
    for (usize i = 0; i < items.size(); ++i) {
        if (questItem(c, g, widgetId("itemPicker.item", i),
                      {view.viewport.x + g.px(6.0f), y, view.viewport.w - g.px(12.0f), m.item}, items[i],
                      false, true)) {
            tapped = static_cast<i32>(i);
        }
        y += rowH;
    }
    if (items.empty()) {
        TextStyle empty = ui::gameFont(m.text, ui::kDisabledText);
        empty.align = Align::Center;
        ui::label(g, {view.viewport.x, view.viewport.y, view.viewport.w, rowH},
                  text::get("VD_COMBAT_NO_USABLE_ITEMS"), empty);
    }
    ui::endScroll(g, view, y - view.top);
    closed = d.closed || d.outside;
    return tapped;
}

// --- Combat log --------------------------------------------------------------------------------------------

CombatLogResult combatLogWindow(DialogContext& c, std::span<const CombatLogLineView> lines, bool onlyMine) {
    VD_PROFILE_ZONE("CombatWindows::combatLog");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    CombatLogResult result;
    const Dialog d = beginDialog(c, g, "combatLog", wide ? 760.0f : 560.0f, wide ? 420.0f : 700.0f,
                                 text::get("COMBAT_LOG"));
    g.draw.sprite(d.body, assetPathHash("Interface\\ChatFrame\\ChatFrameBackground.blp"), rgba(0, 0, 0, 170));
    const TextStyle base = ui::chatFont(g.px(wide ? 13.5f : 14.5f));
    const Rect box = d.body.inset(g.px(6.0f));
    // Newest at the bottom, sticking to the end while there.
    f32 total = 0.0f;
    for (const CombatLogLineView& line : lines) {
        total += g.draw.measureWrapped(box.w - g.px(6.0f), line.text, base.font, base.size);
    }
    const ui::ScrollView view = ui::beginScroll(g, widgetId("combatLog.lines"), box, true);
    f32 y = view.top + std::max(0.0f, box.h - total);
    TextStyle style = base;
    for (const CombatLogLineView& line : lines) {
        const f32 h = g.draw.measureWrapped(box.w - g.px(6.0f), line.text, style.font, style.size);
        if (y + h >= box.y && y <= box.bottom()) {
            style.color = line.color;
            g.draw.wrappedText({box.x, y, box.w - g.px(6.0f), h + 1.0f}, line.text, style);
        }
        y += h;
    }
    if (lines.empty()) {
        TextStyle empty = ui::gameFont(m.text, ui::kDisabledText);
        empty.align = Align::Center;
        ui::label(g, {box.x, box.y, box.w, m.row}, text::get("VD_COMBAT_LOG_EMPTY"), empty);
    }
    ui::endScroll(g, view, y - view.top);

    bool mine = onlyMine;
    result.toggleOnlyMine = ui::checkbox(
        g, widgetId("combatLog.mine"), {d.footer.x + g.px(6.0f), d.footer.y, d.footer.w * 0.45f, d.footer.h},
        text::get("VD_COMBAT_LOG_ONLY_MINE"), mine);
    const f32 clearW =
        std::min(buttonWidth(g, text::get("VD_COMBAT_LOG_CLEAR"), d.footer.h), d.footer.w * 0.3f);
    result.clear = ui::button(g, widgetId("combatLog.clear"),
                              {d.footer.right() - clearW, d.footer.y, clearW, d.footer.h},
                              text::get("VD_COMBAT_LOG_CLEAR"), !lines.empty());
    result.closed = d.closed;
    return result;
}

} // namespace vd
