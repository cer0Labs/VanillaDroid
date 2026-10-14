#include "app/TouchEditWindows.h"

#include "app/DialogKit.h"
#include "app/TouchAssists.h" // phase 20 (assists)
#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
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

// ActionButtonTemplate's art (as app/HudActionBar draws the 1.12 bars): the button, the empty one, the flash, the
// checked and pressed highlights, the equipped item's border.
constexpr u64 kQuickslot = assetPathHash("Interface\\Buttons\\UI-Quickslot2.blp");
constexpr u64 kQuickslotEmpty = assetPathHash("Interface\\Buttons\\UI-Quickslot.blp");
constexpr u64 kQuickslotRed = assetPathHash("Interface\\Buttons\\UI-QuickslotRed.blp");
constexpr u64 kQuickslotDown = assetPathHash("Interface\\Buttons\\UI-Quickslot-Depress.blp");
constexpr u64 kCheckHighlight = assetPathHash("Interface\\Buttons\\CheckButtonHilight.blp");
constexpr u64 kButtonHighlight = assetPathHash("Interface\\Buttons\\ButtonHilight-Square.blp");
constexpr u64 kActionBorder = assetPathHash("Interface\\Buttons\\UI-ActionButton-Border.blp");
constexpr u64 kWindowTab = assetPathHash("Interface\\HelpFrame\\HelpFrameTab-Active.blp");
constexpr u64 kWindowTabInactive = assetPathHash("Interface\\HelpFrame\\HelpFrameTab-Inactive.blp");
constexpr u64 kListboxHighlight = assetPathHash("Interface\\Buttons\\UI-Listbox-Highlight.blp");
constexpr u64 kCheck = assetPathHash("Interface\\Buttons\\UI-CheckBox-Check.blp");
constexpr u64 kMenuArrow = assetPathHash("Interface\\ChatFrame\\ChatFrameExpandArrow.blp");
constexpr u32 kNoManaTint = rgba(128, 128, 255); // ActionButton_UpdateUsable's
constexpr u32 kUnusableTint = rgba(102, 102, 102);
constexpr u32 kOutOfRangeTint = rgba(204, 26, 26);
constexpr u64 kFlashPeriodMs = 400; // ATTACK_BUTTON_FLASH_TIME
constexpr u32 kDrawerPages = 6;
constexpr u32 kDrawerButtons = 12;

// A copy of `c` whose windows are panels in `place` (no blocker: the cluster stays usable beside them).
DialogContext panelContext(const DialogContext& c, const Rect& place, PanelAlign align) {
    DialogContext pc = c;
    pc.panel = place;
    pc.panelAlign = align;
    return pc;
}

// A tab (TabButtonTemplate's art, as the macro window's): the selected one lower and white.
bool tab(Gui& g, WidgetId id, const Rect& r, std::string_view text, bool active) {
    const ui::WidgetState state = ui::hit(g, id, r);
    const f32 cap = std::min(r.w * 0.3f, r.h * 0.5f);
    ui::threeSlice(g.draw, active ? Rect{r.x, r.y + r.h * 3.0f / 32.0f, r.w, r.h} : r,
                   active ? kWindowTab : kWindowTabInactive, {0.0f, 0.0f, 1.0f, 1.0f}, 0.25f, cap);
    if (ui::held(state, r) && !active) {
        g.draw.spriteAdditive(r.inset(cap * 0.3f, r.h * 0.15f), kListboxHighlight, rgba(255, 255, 255, 120));
    }
    TextStyle style = ui::gameFont(std::min(g.px(13.0f), r.h * 0.42f), active ? ui::kWhite : ui::kGold);
    style.align = Align::Center;
    ui::label(g, {r.x + cap * 0.3f, r.y, r.w - cap * 0.6f, r.h}, text, style);
    return ui::tapped(state);
}

// A row of tabs filling `r`; returns the one tapped (-1 none).
i32 tabRow(Gui& g, std::string_view name, const Rect& r, std::span<const std::string_view> names, u32 active) {
    i32 tapped = -1;
    if (names.empty()) {
        return tapped;
    }
    const f32 w = r.w / static_cast<f32>(names.size());
    for (u32 i = 0; i < names.size(); ++i) {
        if (tab(g, widgetId(name, i), {r.x + w * static_cast<f32>(i), r.y, w, r.h}, names[i], i == active)) {
            tapped = static_cast<i32>(i);
        }
    }
    return tapped;
}

// An icon of the icon cache inside `r` (nothing while it loads).
void icon(DialogContext& c, const Rect& r, u64 iconHash, u32 tint = ui::kWhite) {
    if (iconHash == 0) {
        return;
    }
    if (const std::optional<u32> slot = c.resources.icon(iconHash)) {
        c.draw.icon(r, *slot, tint);
    }
}

// One button of the 1.12 bars (ActionButtonTemplate, as app/HudActionBar's): its states as the bars show them.
ui::WidgetState actionButton(DialogContext& c, Gui& g, WidgetId id, const Rect& r, const HudActionSlot& slot) {
    const ui::WidgetState state = ui::hit(g, id, r);
    const bool unusable = !slot.usable && !slot.noMana;
    const f32 pad = r.w * (64.0f - 36.0f) / 36.0f * 0.5f;
    const bool blue = slot.noMana && slot.inRange && slot.iconHash != 0;
    g.draw.sprite({r.x - pad, r.y - pad, r.w + 2.0f * pad, r.h + 2.0f * pad},
                  slot.iconHash != 0 ? kQuickslot : kQuickslotEmpty, blue ? kNoManaTint : ui::kWhite);
    u32 tint = ui::kWhite;
    if (unusable) {
        tint = kUnusableTint;
    } else if (!slot.inRange) {
        tint = kOutOfRangeTint;
    } else if (slot.noMana) {
        tint = kNoManaTint;
    }
    icon(c, r, slot.iconHash, tint);
    if ((slot.flashing || slot.active) && (g.ui.nowMs() / kFlashPeriodMs) % 2 == 0) {
        g.draw.sprite(r, kQuickslotRed);
    }
    { // retouches (ui models): the Cooldown model (ActionButtonTemplate: 36 x 36, one unit lower; no seconds)
        const f32 unit = r.w / 36.0f;
        const f32 share = slot.cooldownTotal > 0.0f ? std::clamp(slot.cooldownLeft / slot.cooldownTotal, 0.0f, 1.0f)
                                                    : 0.0f;
        g.draw.models().cooldown(g.draw, id, {r.x, r.y + unit, r.w, r.h}, unit, share, slot.cooldownLeft,
                                 g.ui.nowMs());
    }
    if (slot.equipped) {
        const f32 grow = r.w * (62.0f - 36.0f) / 36.0f * 0.5f;
        g.draw.spriteAdditive({r.x - grow, r.y - grow, r.w + 2.0f * grow, r.h + 2.0f * grow}, kActionBorder,
                              rgba(0, 255, 0, 89));
    }
    if (slot.checked || slot.active) {
        g.draw.spriteAdditive(r, kCheckHighlight);
    }
    if (slot.highlight) {
        const f32 pulse = 0.5f + 0.5f * std::sin(static_cast<f32>(g.ui.nowMs() % 800) * 0.00785f);
        const f32 grow = r.w * 0.3f;
        g.draw.spriteAdditive({r.x - grow, r.y - grow, r.w + 2.0f * grow, r.h + 2.0f * grow}, kActionBorder,
                              ui::withAlpha(rgba(120, 255, 120), 0.4f + 0.6f * pulse));
    }
    if (slot.count > 0) {
        char count[8];
        std::snprintf(count, sizeof(count), "%u", slot.count);
        TextStyle style = ui::numberFont(r.h * 0.3f);
        style.align = Align::Right;
        g.draw.text(r.right() - r.w * 0.06f, r.bottom() - r.h * 0.06f, count, style);
    }
    if (!slot.label.empty()) {
        TextStyle style = ui::numberFont(r.h * 0.27f);
        style.align = Align::Center;
        g.draw.pushClip(r);
        g.draw.text(r.centerX(), r.bottom() - r.h * 0.08f, slot.label, style);
        g.draw.popClip();
    }
    if (ui::held(state, r)) {
        g.draw.sprite(r, kQuickslotDown);
        g.draw.spriteAdditive(r, kButtonHighlight, rgba(255, 255, 255, 140));
    }
    return state;
}

// A small text line (wrapped) from `y`; returns the height used.
f32 note(Gui& g, f32 x, f32 y, f32 w, std::string_view text, f32 size, u32 color) {
    if (text.empty()) {
        return 0.0f;
    }
    TextStyle style = ui::gameFont(size, color);
    return g.draw.wrappedText({x, y, w, 1e6f}, text, style);
}

} // namespace

// --- The template list ----------------------------------------------------------------------------------------

TouchTemplateListResult touchTemplateList(DialogContext& c, const TouchTemplateListView& v) {
    VD_PROFILE_ZONE("TouchEditWindows::templateList");
    Gui g{c.ui, c.draw};
    TouchTemplateListResult result;
    // A tap anywhere else closes it (the world is not touched by it).
    result.closed = ui::modalBlocker(g, widgetId("touchlist.outside"), 0.0f);
    const Metrics m = metrics(panelContext(c, v.area, PanelAlign::TopLeft));
    const f32 gap = g.px(6.0f);
    const f32 row = m.row;
    const u32 rows = static_cast<u32>(v.rows.size());
    const bool menuOpen = v.menuRow >= 0 && static_cast<u32>(v.menuRow) < rows;
    // Its height: the header, the rows (and a row's actions), four buttons and the note.
    const f32 header = g.px(46.0f);
    const f32 noteH =
        v.note.empty() ? 0.0f : g.draw.measureWrapped(g.px(260.0f), v.note, ui::FontId::Friz, m.small) + gap;
    f32 height = header + static_cast<f32>(rows) * row + (menuOpen ? m.button + gap : 0.0f) + gap * 2.0f +
                 4.0f * (m.button + gap) + noteH + g.px(24.0f);
    const f32 width = std::min(g.px(300.0f), v.area.w);
    height = std::min(height, v.area.h);
    // Beside the templates' button, on the side of the area, its bottom at the button's.
    f32 x = v.anchor.w > 0.0f ? (v.anchor.x > v.area.centerX() ? v.anchor.x - width - gap : v.anchor.right() + gap)
                              : v.area.x;
    x = std::clamp(x, v.area.x, std::max(v.area.x, v.area.right() - width));
    f32 y = v.anchor.w > 0.0f ? v.anchor.bottom() - height : v.area.y;
    y = std::clamp(y, v.area.y, std::max(v.area.y, v.area.bottom() - height));
    const Rect place{x, y, width, height};
    DialogContext pc = panelContext(c, place, PanelAlign::TopLeft);
    const Dialog d = beginDialog(pc, g, "touchlist", width / c.ui.dp(), height / c.ui.dp(),
                                 text::get("VD_TOUCH_EDIT_TEMPLATES"), true, false);
    result.panel = d.panel;
    result.closed = result.closed || d.closed;
    const ui::ScrollView view = ui::beginScroll(g, widgetId("touchlist.scroll"), d.body);
    f32 cy = view.top;
    const Rect& body = d.body;
    for (u32 i = 0; i < rows; ++i) {
        const TouchTemplateRowView& r = v.rows[i];
        const Rect line{body.x, cy, body.w, row};
        const f32 menuW = row * 1.3f;
        if (static_cast<i32>(i) == v.renaming) {
            // Its name being typed: the field and its OK.
            const f32 okW = g.px(70.0f);
            const Rect field{line.x, line.y + row * 0.1f, line.w - okW - gap, row * 0.8f};
            if (ui::textField(g, widgetId("touchlist.name", i), field, v.renameText, {}, true)) {
                result.renameField = true;
            }
            if (ui::button(g, widgetId("touchlist.nameok", i), {field.right() + gap, field.y, okW, field.h},
                           text::get("OKAY"), !v.renameText.empty())) {
                result.renameDone = true;
            }
        } else {
            // The active one checked (UI-CheckBox-Check) and white; a menu button for its actions at the right.
            const f32 mark = row * 0.6f;
            if (r.active) {
                g.draw.sprite({line.x, line.y + (row - mark) * 0.5f, mark, mark}, kCheck);
            }
            ui::RowStyle style;
            style.text = ui::gameFont(m.text, r.active ? ui::kWhite : ui::kGold);
            style.selected = r.active;
            style.highlight = ui::RowHighlight::Listbox;
            if (ui::listRow(g, widgetId("touchlist.row", i),
                            {line.x + mark, line.y, line.w - menuW - gap - mark, line.h}, r.name, style)) {
                result.select = static_cast<i32>(i);
            }
            const Rect menu{line.right() - menuW, line.y + row * 0.1f, menuW, row * 0.8f};
            if (ui::button(g, widgetId("touchlist.menu", i), menu, {})) {
                result.menu = static_cast<i32>(i);
            }
            const f32 arrow = menu.h * 0.5f;
            g.draw.sprite({menu.centerX() - arrow * 0.5f, menu.centerY() - arrow * 0.5f, arrow, arrow}, kMenuArrow);
        }
        cy += row;
        if (static_cast<i32>(i) == v.menuRow) {
            // Its actions under it.
            const f32 w = (body.w - 2.0f * gap) / 3.0f;
            if (ui::button(g, widgetId("touchlist.rename", i), {body.x, cy, w, m.button},
                           text::get("VD_TOUCH_EDIT_RENAME"))) {
                result.rename = static_cast<i32>(i);
            }
            if (ui::button(g, widgetId("touchlist.dup", i), {body.x + w + gap, cy, w, m.button},
                           text::get("VD_TOUCH_EDIT_DUPLICATE"), v.canAdd)) {
                result.duplicate = static_cast<i32>(i);
            }
            if (ui::button(g, widgetId("touchlist.delete", i), {body.x + 2.0f * (w + gap), cy, w, m.button},
                           text::get("DELETE"), v.canDelete)) {
                result.remove = static_cast<i32>(i);
            }
            cy += m.button + gap;
        }
    }
    cy += gap;
    ui::horizontalBreak(g, body.x, cy, body.w);
    cy += gap;
    const auto wideButton = [&](std::string_view id, std::string_view label, bool enabled) {
        const bool tapped = ui::button(g, widgetId(id), {body.x, cy, body.w, m.button}, label, enabled);
        cy += m.button + gap;
        return tapped;
    };
    result.edit = wideButton("touchlist.edit", text::get("VD_TOUCH_EDIT_EDIT"), v.canEdit);
    result.add = wideButton("touchlist.add", text::get("VD_TOUCH_EDIT_NEW"), v.canAdd);
    result.restore = wideButton("touchlist.restore", text::get("VD_TOUCH_EDIT_RESTORE"), v.canRestore);
    result.copyLayout = wideButton("touchlist.copy", text::get("VD_TOUCH_EDIT_COPY_LAYOUT"), rows > 1);
    cy += note(g, body.x, cy, body.w, v.note, m.small, rgba(255, 90, 90));
    ui::endScroll(g, view, cy - view.top + gap);
    return result;
}

// --- The edit mode's panel ------------------------------------------------------------------------------------

TouchEditPanelResult touchEditPanel(DialogContext& c, const TouchEditPanelView& v) {
    VD_PROFILE_ZONE("TouchEditWindows::panel");
    Gui g{c.ui, c.draw};
    TouchEditPanelResult result;
    DialogContext pc = panelContext(c, v.area, PanelAlign::TopLeft);
    const Metrics m = metrics(pc);
    const f32 gap = g.px(6.0f);
    const Dialog d = beginDialog(pc, g, "touchedit", std::min(380.0f, v.area.w / c.ui.dp()), v.area.h / c.ui.dp(),
                                 v.templateName, false, true);
    result.panel = d.panel;
    const Rect& body = d.body;
    // The profile edited, then the two modes as tabs.
    f32 y = body.y;
    {
        TextStyle style = ui::gameFont(m.small, rgba(200, 200, 200));
        ui::label(g, {body.x, y, body.w, m.small * 1.4f},
                  text::get(v.wide ? "VD_TOUCH_EDIT_PROFILE_WIDE" : "VD_TOUCH_EDIT_PROFILE_SQUARE"), style);
        y += m.small * 1.4f + gap * 0.5f;
    }
    const std::string_view modes[2] = {text::get("VD_TOUCH_EDIT_ACTIONS"), text::get("VD_TOUCH_EDIT_LAYOUT")};
    if (const i32 t = tabRow(g, "touchedit.mode", {body.x, y, body.w, m.button}, modes, v.layout ? 1u : 0u); t >= 0) {
        result.mode = t;
    }
    y += m.button + gap;
    const Rect scrollArea{body.x, y, body.w, body.bottom() - y};
    const ui::ScrollView view = ui::beginScroll(g, widgetId("touchedit.scroll"), scrollArea);
    f32 cy = view.top;
    if (v.layout) {
        // The selected place, its size; the cluster's scale; the defaults.
        TextStyle title = ui::gameFont(m.text, v.selected.empty() ? rgba(200, 200, 200) : ui::kWhite);
        ui::label(g, {body.x, cy, body.w, m.row * 0.7f},
                  v.selected.empty() ? text::get("VD_TOUCH_EDIT_PICK_PLACE") : v.selected, title);
        cy += m.row * 0.7f;
        char line[64];
        text::format(line, "VD_TOUCH_EDIT_SIZE", static_cast<int>(v.size + 0.5f));
        ui::label(g, {body.x, cy, body.w, m.small * 1.3f}, line, ui::gameFont(m.small));
        cy += m.small * 1.3f;
        f32 size = v.size;
        if (!v.selected.empty() &&
            ui::slider(g, widgetId("touchedit.size"), {body.x, cy, body.w, m.row * 0.8f}, size, 70.0f, 150.0f, 5.0f)) {
            result.size = size;
        }
        cy += m.row * 0.8f + gap * 0.5f;
        if (v.tooSmall) {
            cy += note(g, body.x, cy, body.w, text::get("VD_TOUCH_EDIT_TOO_SMALL"), m.small, rgba(255, 90, 90)) +
                  gap * 0.5f;
        }
        text::format(line, "VD_TOUCH_EDIT_SCALE", static_cast<int>(v.scale + 0.5f));
        ui::label(g, {body.x, cy, body.w, m.small * 1.3f}, line, ui::gameFont(m.small));
        cy += m.small * 1.3f;
        f32 scale = v.scale;
        if (ui::slider(g, widgetId("touchedit.scale"), {body.x, cy, body.w, m.row * 0.8f}, scale, 70.0f, 130.0f,
                       5.0f)) {
            result.scale = scale;
        }
        cy += m.row * 0.8f + gap;
        if (ui::button(g, widgetId("touchedit.reset"), {body.x, cy, body.w, m.button}, text::get("RESET"))) {
            result.reset = true;
        }
        cy += m.button + gap;
        // Phase 20 (assists): where the template is switched to, the party frames beside the cluster.
        TouchAssistRows assist;
        cy += touchAssistRows(g, {body.x, cy, body.w, 0.0f}, m.button, v.site, v.healerFrames, assist);
        result.site = assist.site;
        result.toggleHealer = assist.toggleHealer;
    } else {
        // The layers: a tap edits that one; the base's are drawn translucent with a chain where a layer inherits.
        for (u32 i = 0; i < v.layers.size(); ++i) {
            const TouchLayerView& layer = v.layers[i];
            ui::RowStyle style;
            style.text = ui::gameFont(m.text, !layer.enabled ? ui::kDisabledText
                                              : i == v.layer ? ui::kWhite
                                                             : ui::kGold);
            style.selected = i == v.layer;
            style.highlight = ui::RowHighlight::Listbox;
            const Rect r{body.x, cy, body.w, m.row * 0.85f};
            if (ui::listRow(g, widgetId("touchedit.layer", i), r, layer.name, style)) {
                result.layer = static_cast<i32>(i);
            }
            if (layer.own && i != 0) { // a layer with actions of its own: a gold dot at the right
                g.draw.ring(r.right() - r.h * 0.3f, r.centerY(), r.h * 0.12f, 0.0f, ui::kGold);
            }
            cy += r.h;
        }
        cy += gap;
        if (v.layer > 0 && v.layer < v.layers.size()) {
            bool enabled = v.layers[v.layer].enabled;
            if (ui::checkbox(g, widgetId("touchedit.uselayer"), {body.x, cy, body.w, m.row},
                             text::get("VD_TOUCH_EDIT_USE_LAYER"), enabled)) {
                result.toggleLayer = true;
            }
            cy += m.row + gap;
        }
    }
    cy += note(g, body.x, cy, body.w, v.hint, m.small, rgba(200, 200, 200)) + gap;
    ui::endScroll(g, view, cy - view.top);
    bool cancel = false, done = false;
    footerButtons(g, d.footer, "touchedit.footer", text::get("CANCEL"), true, cancel, text::get("VD_TOUCH_EDIT_DONE"),
                  done);
    result.cancel = cancel;
    result.done = done;
    return result;
}

// --- The action picker ----------------------------------------------------------------------------------------

TouchPickerResult touchActionPicker(DialogContext& c, const TouchPickerView& v) {
    VD_PROFILE_ZONE("TouchEditWindows::picker");
    Gui g{c.ui, c.draw};
    TouchPickerResult result;
    DialogContext pc = panelContext(c, v.area, PanelAlign::TopLeft);
    const Metrics m = metrics(pc);
    const f32 gap = g.px(6.0f);
    const Dialog d = beginDialog(pc, g, "touchpick", std::min(440.0f, v.area.w / c.ui.dp()), v.area.h / c.ui.dp(),
                                 v.title, true, false);
    result.panel = d.panel;
    result.closed = d.closed;
    const Rect& body = d.body;
    f32 y = body.y;
    if (!v.ranks) {
        const std::string_view tabs[static_cast<u32>(TouchPickerTab::Count)] = {text::get("SPELLS"), text::get("ITEMS"),
                                                                                text::get("MACROS"), text::get("PET"),
                                                                                text::get("VD_TOUCH_EDIT_SPECIAL")};
        if (const i32 t = tabRow(g, "touchpick.tab", {body.x, y, body.w, m.button}, tabs, static_cast<u32>(v.tab));
            t >= 0) {
            result.tab = t;
        }
        y += m.button + gap * 0.5f;
        if (!v.subTabs.empty()) {
            if (const i32 t = tabRow(g, "touchpick.sub", {body.x, y, body.w, m.button * 0.8f}, v.subTabs, v.subTab);
                t >= 0) {
                result.subTab = t;
            }
            y += m.button * 0.8f + gap * 0.5f;
        }
    }
    // The buttons at the bottom (what a slot can be besides an action): one row where four fit, else two; the group
    // being made over them.
    const bool oneRow = body.w >= g.px(4.0f * 112.0f);
    const f32 buttonsH = v.ranks || oneRow ? m.button : 2.0f * m.button + gap;
    const f32 groupH = v.grouping ? m.item + m.small * 1.6f + gap : 0.0f;
    const Rect list{body.x, y, body.w, body.bottom() - y - buttonsH - groupH - gap};
    const ui::ScrollView view = ui::beginScroll(g, widgetId("touchpick.list"), list);
    // The entries: icon and name, in as many columns as fit.
    const f32 cellH = m.row;
    const u32 columns = std::max(1u, static_cast<u32>(list.w / g.px(170.0f)));
    const f32 cellW = list.w / static_cast<f32>(columns);
    for (u32 i = 0; i < v.entries.size(); ++i) {
        const TouchPickerEntry& e = v.entries[i];
        const Rect cell{list.x + cellW * static_cast<f32>(i % columns),
                        view.top + cellH * static_cast<f32>(i / columns), cellW - gap, cellH - g.px(3.0f)};
        if (cell.bottom() < list.y || cell.y > list.bottom()) {
            continue; // scrolled out: not drawn, not touched
        }
        const ui::WidgetState state = ui::hit(g, widgetId("touchpick.entry", i), cell);
        if (e.marked) {
            g.draw.spriteAdditive(cell, kListboxHighlight, rgba(120, 255, 120, 200));
        } else if (ui::held(state, cell)) {
            g.draw.spriteAdditive(cell, kListboxHighlight, rgba(255, 255, 255, 140));
        }
        const Rect ic{cell.x + g.px(2.0f), cell.y + (cell.h - m.item * 0.85f) * 0.5f, m.item * 0.85f, m.item * 0.85f};
        g.draw.sprite(ic.inset(-ic.w * 0.12f), kQuickslot);
        icon(c, ic, e.iconHash);
        const f32 tx = ic.right() + gap;
        const TextStyle nameStyle = ui::gameFont(m.small * 1.05f, e.marked ? ui::kWhite : ui::kGold);
        if (e.detail.empty()) {
            ui::label(g, {tx, cell.y, cell.right() - tx, cell.h}, e.name, nameStyle);
        } else {
            ui::label(g, {tx, cell.y, cell.right() - tx, cell.h * 0.58f}, e.name, nameStyle);
            ui::label(g, {tx, cell.y + cell.h * 0.5f, cell.right() - tx, cell.h * 0.45f}, e.detail,
                      ui::gameFont(m.small * 0.9f, rgba(190, 190, 190)));
        }
        if (state.longPress) {
            result.entryHeld = static_cast<i32>(i);
            result.entryRect = cell;
        } else if (ui::tapped(state)) {
            result.entry = static_cast<i32>(i);
        }
    }
    const f32 rowsUsed = std::ceil(static_cast<f32>(v.entries.size()) / static_cast<f32>(columns));
    if (v.entries.empty() && !v.empty.empty()) {
        note(g, list.x, view.top + gap, list.w, v.empty, m.small, rgba(200, 200, 200));
    }
    ui::endScroll(g, view, rowsUsed * cellH + gap);
    f32 by = list.bottom() + gap;
    if (v.grouping) {
        // The group so far: a tap takes an action out.
        ui::label(g, {body.x, by, body.w, m.small * 1.4f}, text::get("VD_TOUCH_EDIT_GROUP_HINT"),
                  ui::gameFont(m.small, rgba(120, 255, 120)));
        by += m.small * 1.6f;
        const f32 s = m.item;
        for (u32 i = 0; i < v.group.size(); ++i) {
            const Rect r{body.x + (s + gap) * static_cast<f32>(i), by, s * 0.9f, s * 0.9f};
            const ui::WidgetState state = ui::hit(g, widgetId("touchpick.group", i), r);
            g.draw.sprite(r.inset(-r.w * 0.12f), kQuickslot);
            icon(c, r, v.group[i].iconHash);
            if (ui::tapped(state)) {
                result.groupEntry = static_cast<i32>(i);
            }
        }
        by += s + gap;
    }
    // Empty (the base) or Inherit and Hidden (a layer); Fixed rank; New group / Save group.
    if (v.ranks) {
        result.closed = result.closed ||
                        ui::button(g, widgetId("touchpick.back"), {body.x, by, body.w, m.button}, text::get("CANCEL"));
        return result;
    }
    const u32 count = v.layer ? 4u : 3u;
    const u32 perRow = oneRow ? count : 2u;
    u32 at = 0;
    const auto next = [&]() {
        const u32 row = at / perRow, column = at % perRow;
        const u32 inRow = std::min(perRow, count - row * perRow);
        const f32 w = (body.w - gap * static_cast<f32>(inRow - 1)) / static_cast<f32>(inRow);
        ++at;
        return Rect{body.x + (w + gap) * static_cast<f32>(column), by + (m.button + gap) * static_cast<f32>(row), w,
                    m.button};
    };
    if (v.layer) {
        result.inherit = ui::button(g, widgetId("touchpick.inherit"), next(), text::get("VD_TOUCH_EDIT_INHERIT"));
        result.hide = ui::button(g, widgetId("touchpick.hide"), next(), text::get("VD_TOUCH_EDIT_HIDDEN"));
    } else {
        result.empty = ui::button(g, widgetId("touchpick.empty"), next(), text::get("EMPTY"));
    }
    result.rank = ui::button(g, widgetId("touchpick.rank"), next(), text::get("VD_TOUCH_EDIT_FIXED_RANK"), v.canRank);
    result.group = ui::button(g, widgetId("touchpick.newgroup"), next(),
                              text::get(v.grouping ? "VD_TOUCH_EDIT_SAVE_GROUP" : "VD_TOUCH_EDIT_NEW_GROUP"),
                              !v.grouping || !v.group.empty());
    return result;
}

// --- The drawer -----------------------------------------------------------------------------------------------

TouchDrawerResult touchDrawer(DialogContext& c, const TouchDrawerView& v) {
    VD_PROFILE_ZONE("TouchEditWindows::drawer");
    Gui g{c.ui, c.draw};
    TouchDrawerResult result;
    DialogContext pc = panelContext(c, v.area, PanelAlign::TopLeft);
    const Metrics m = metrics(pc);
    const f32 gap = g.px(5.0f);
    const Dialog d = beginDialog(pc, g, "touchdrawer", v.area.w / c.ui.dp(), v.area.h / c.ui.dp(),
                                 text::get("ACTIONBAR_LABEL"), true, true);
    result.panel = d.panel;
    result.closed = d.closed;
    const Rect& body = d.body;
    // The six pages as rows of twelve, or two rows of six each, whichever gives bigger buttons without scrolling; when
    // neither reaches a finger's size (kTouchDp) the one with the wider buttons at that size, scrolled.
    const f32 label = g.px(18.0f);
    const f32 least = g.px(ui::kTouchDp);
    struct Grid {
        u32 columns = 0;
        f32 width = 0.0f; // a cell's width
        f32 fit = 0.0f;   // ...and its size with every row in view
    };
    const auto grid = [&](u32 columns) {
        const u32 rows = kDrawerPages * (kDrawerButtons / columns);
        const f32 w = (body.w - label - gap) / static_cast<f32>(columns);
        return Grid{columns, w, std::min(w, body.h / static_cast<f32>(rows))};
    };
    const Grid twelve = grid(kDrawerButtons), six = grid(kDrawerButtons / 2);
    Grid use = twelve.fit >= six.fit ? twelve : six;
    if (use.fit < least) {
        use = twelve.width >= least ? twelve : six;
    }
    const bool split = use.columns != kDrawerButtons;
    const u32 columns = use.columns;
    const f32 cell = std::min(use.width, std::max(use.fit, least));
    const f32 size = cell * 0.86f;
    const ui::ScrollView view = ui::beginScroll(g, widgetId("touchdrawer.scroll"), body);
    f32 y = view.top;
    for (u32 page = 0; page < kDrawerPages; ++page) {
        char number[4];
        std::snprintf(number, sizeof(number), "%u", page + 1);
        TextStyle style = ui::gameFont(m.text, ui::kGold);
        style.align = Align::Center;
        g.draw.text(body.x + label * 0.5f, y + cell * 0.5f + m.text * 0.35f, number, style);
        for (u32 b = 0; b < kDrawerButtons; ++b) {
            const u32 slot = page * kDrawerButtons + b;
            if (slot >= v.slots.size()) {
                break;
            }
            const u32 col = b % columns, rowInPage = b / columns;
            const Rect r{body.x + label + gap + cell * static_cast<f32>(col) + (cell - size) * 0.5f,
                         y + cell * static_cast<f32>(rowInPage) + (cell - size) * 0.5f, size, size};
            if (r.bottom() < body.y || r.y > body.bottom()) {
                continue;
            }
            const ui::WidgetState state = actionButton(c, g, widgetId("touchdrawer.slot", slot), r, v.slots[slot]);
            if (state.longPress) {
                result.held = static_cast<i32>(slot);
                result.heldRect = r;
            } else if (ui::tapped(state)) {
                result.used = static_cast<i32>(slot);
            }
        }
        y += cell * (split ? 2.0f : 1.0f) + (page + 1 < kDrawerPages ? gap : 0.0f);
    }
    ui::endScroll(g, view, y - view.top, false); // dragged with the finger: no bar beside the buttons
    bool keep = v.keepOpen;
    if (ui::checkbox(g, widgetId("touchdrawer.keep"), d.footer, text::get("VD_TOUCH_EDIT_KEEP_OPEN"), keep)) {
        result.keepOpen = true;
    }
    return result;
}

} // namespace vd
