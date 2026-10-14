#include "app/OptionsWindows.h"

#include "app/DialogKit.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <array>
#include <cmath>

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

// UIOptionsCheckButtonTemplate (OptionsCheckButtonTemplate: UI-CheckBox-*), the drop-down's arrow and the tabs' art
// (UIOptionsFrameTab1's: HelpFrameTab-Inactive, the selected one OptionsFrameTab-Active).
constexpr u64 kCheckUp = assetPathHash("Interface\\Buttons\\UI-CheckBox-Up.blp");
constexpr u64 kCheckDown = assetPathHash("Interface\\Buttons\\UI-CheckBox-Down.blp");
constexpr u64 kCheckHighlight = assetPathHash("Interface\\Buttons\\UI-CheckBox-Highlight.blp");
constexpr u64 kCheckMark = assetPathHash("Interface\\Buttons\\UI-CheckBox-Check.blp");
constexpr u64 kCheckMarkDisabled = assetPathHash("Interface\\Buttons\\UI-CheckBox-Check-Disabled.blp");
constexpr u64 kDropDownArrow = assetPathHash("Interface\\ChatFrame\\UI-ChatIcon-ScrollDown-Up.blp");
constexpr u64 kWindowTab = assetPathHash("Interface\\OptionsFrame\\OptionsFrameTab-Active.blp");
constexpr u64 kWindowTabInactive = assetPathHash("Interface\\HelpFrame\\HelpFrameTab-Inactive.blp");
constexpr u64 kListboxHighlight = assetPathHash("Interface\\Buttons\\UI-Listbox-Highlight.blp");
constexpr usize kMaxSections = 16;

struct Sizes {
    f32 header, check, slider, choice, button, pad, gap, indent;
};

f32 rowHeight(const Sizes& s, const OptionRow& row) {
    switch (row.kind) {
    case OptionRow::Kind::Section: return row.label.empty() ? s.pad : s.header; // untitled: a box without a title
    case OptionRow::Kind::Check: return s.check;
    case OptionRow::Kind::Slider: return s.slider;
    case OptionRow::Kind::Choice: return s.choice;
    case OptionRow::Kind::Button: return s.button;
    }
    return s.check;
}

// A widget of the window: its name's hash, a kind and an index.
WidgetId rowId(u64 base, u64 kind, u64 index) {
    return ((base * 1099511628211ull + kind) ^ (index * 0x9E3779B97F4A7C15ull)) | 1;
}

bool windowTab(Gui& g, WidgetId id, const Rect& r, std::string_view text, bool active) {
    const ui::WidgetState state = ui::hit(g, id, r);
    // 16-unit ends of the 64 x 32 art (TexCoords 0..0.25, 0.75..1) on a 42-unit-high tab.
    const f32 cap = std::min(r.w * 0.3f, r.h * 16.0f / 42.0f);
    ui::threeSlice(g.draw, r, active ? kWindowTab : kWindowTabInactive, {0.0f, 0.0f, 1.0f, 1.0f}, 0.25f, cap);
    if (ui::held(state, r) && !active) {
        g.draw.spriteAdditive(r.inset(cap * 0.3f, r.h * 0.15f), kListboxHighlight, rgba(255, 255, 255, 120));
    }
    TextStyle style = ui::gameFont(std::min(g.px(14.0f), r.h * 0.45f), active ? ui::kWhite : ui::kGold);
    style.align = Align::Center;
    ui::label(g, {r.x + cap * 0.4f, r.y, r.w - cap * 0.8f, r.h}, text, style);
    return ui::tapped(state);
}

} // namespace

OptionsWindowResult optionsWindow(DialogContext& c, const OptionsWindowView& v) {
    VD_PROFILE_ZONE("OptionsWindows::window");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    OptionsWindowResult result;
    const Dialog d = beginFullScreen(c, g, v.name, v.title); // as UIOptionsFrame (SoundOptionsFrame too, unlike 1.12)
    const u64 base = stringHash(v.name);
    Rect body = d.body;

    // The tabs (UIOptionsFrameTab1 / 2) over the sections.
    if (!v.tabs.empty()) {
        // 115 x 42 units, 3 apart; the selected one 3 lower, on the sections' edge.
        const f32 tabH = g.px(36.0f);
        const f32 gap = g.px(3.0f);
        const f32 tabW = std::min((body.w - g.px(12.0f) - gap * static_cast<f32>(v.tabs.size() - 1)) / static_cast<f32>(v.tabs.size()),
                                  g.px(170.0f));
        for (u32 i = 0; i < v.tabs.size(); ++i) {
            const f32 drop = v.tab == i ? g.px(3.0f) : 0.0f;
            if (windowTab(g, rowId(base, 0x7AB, i), {body.x + g.px(12.0f) + static_cast<f32>(i) * (tabW + gap), body.y + drop,
                                                     tabW, tabH}, v.tabs[i], v.tab == i)) {
                result.tab = static_cast<i32>(i);
            }
        }
        body.y += tabH + g.px(6.0f);
        body.h -= tabH + g.px(6.0f);
    }

    const Sizes s{g.px(30.0f), g.px(40.0f), g.px(66.0f), g.px(62.0f), g.px(48.0f), g.px(8.0f), g.px(14.0f), g.px(22.0f)};
    // Sections: each Section row starts one (rows before the first make an untitled one).
    struct Section {
        u32 first = 0, end = 0;
        f32 height = 0.0f;
        u32 column = 0;
        f32 y = 0.0f;
    };
    std::array<Section, kMaxSections> sections{};
    u32 sectionCount = 0;
    for (u32 i = 0; i < v.rows.size(); ++i) {
        if ((v.rows[i].kind == OptionRow::Kind::Section || i == 0) && sectionCount < kMaxSections) {
            sections[sectionCount++] = {i, i, 0.0f, 0, 0.0f};
        }
        Section& cur = sections[sectionCount - 1];
        cur.end = i + 1;
        cur.height += rowHeight(s, v.rows[i]);
    }
    const u32 columns = body.w >= g.px(600.0f) ? 2u : 1u;
    const f32 columnGap = g.px(16.0f);
    const f32 columnW = (body.w - g.px(8.0f) - columnGap * static_cast<f32>(columns - 1)) / static_cast<f32>(columns);
    std::array<f32, 2> columnH{};
    for (u32 i = 0; i < sectionCount; ++i) {
        Section& sec = sections[i];
        sec.height += s.pad * 2.0f;
        sec.column = columns == 2 && columnH[1] < columnH[0] ? 1u : 0u;
        sec.y = columnH[sec.column];
        columnH[sec.column] += sec.height + s.gap;
    }

    const ui::ScrollView view = ui::beginScroll(g, rowId(base, 0x5C0, 0), body);
    Rect tipAnchor{};
    Rect choiceAnchor{};
    for (u32 si = 0; si < sectionCount; ++si) {
        const Section& sec = sections[si];
        const f32 x0 = body.x + g.px(4.0f) + static_cast<f32>(sec.column) * (columnW + columnGap);
        f32 y = view.top + g.px(4.0f) + sec.y;
        const bool titled = v.rows[sec.first].kind == OptionRow::Kind::Section && !v.rows[sec.first].label.empty();
        // OptionFrameBoxTemplate: the backdrop with its grey border, the title over its top left.
        const Rect box{x0, y + (titled ? s.header * 0.6f : 0.0f), columnW, sec.height - (titled ? s.header * 0.6f : 0.0f)};
        optionBox(g, box);
        y += s.pad;
        for (u32 i = sec.first; i < sec.end; ++i) {
            const OptionRow& row = v.rows[i];
            const f32 h = rowHeight(s, row);
            const f32 x = x0 + g.px(12.0f) + (row.indent ? s.indent : 0.0f);
            const f32 w = x0 + columnW - g.px(12.0f) - x;
            const Rect r{x, y, w, h};
            const WidgetId id = rowId(base, 0, i);
            const u32 textColor = row.enabled ? ui::kGold : ui::kDisabledText;
            switch (row.kind) {
            case OptionRow::Kind::Section: {
                TextStyle title = ui::gameFont(g.px(15.0f), ui::kWhite); // $parentTitle: GameFontHighlight
                ui::label(g, {x0 + g.px(10.0f), y - s.pad, columnW, s.header * 0.8f}, row.label, title);
                break;
            }
            case OptionRow::Kind::Check: {
                const ui::WidgetState state = ui::hit(g, id, r);
                const bool down = row.enabled && ui::held(state, r);
                const f32 art = std::min(h * 0.8f, g.px(30.0f)) * 32.0f / 23.0f;
                const Rect check{x - art * 4.0f / 32.0f, r.centerY() - art * 0.5f, art, art};
                g.draw.sprite(check, down ? kCheckDown : kCheckUp, row.enabled ? ui::kWhite : rgba(160, 160, 160));
                if (row.checked) {
                    g.draw.sprite(check, row.enabled ? kCheckMark : kCheckMarkDisabled);
                }
                if (down) {
                    g.draw.spriteAdditive(check, kCheckHighlight);
                }
                const f32 textX = check.x + art * 30.0f / 32.0f;
                ui::label(g, {textX, r.y, r.right() - textX, r.h}, row.label,
                          ui::gameFont(g.px(14.0f), down ? ui::kHighlight : textColor));
                if (state.longPress) {
                    result.held = static_cast<i32>(i);
                } else if (ui::tapped(state) && row.enabled) {
                    result.toggled = static_cast<i32>(i);
                }
                break;
            }
            case OptionRow::Kind::Slider: {
                const f32 labelH = g.px(22.0f);
                const ui::WidgetState state = ui::hit(g, rowId(base, 0x11, i), {r.x, r.y, r.w, labelH});
                ui::label(g, {r.x, r.y, r.w * 0.72f, labelH}, row.label, ui::gameFont(g.px(14.0f), textColor));
                TextStyle shown = ui::gameFont(g.px(13.0f), row.enabled ? ui::kWhite : ui::kDisabledText);
                shown.align = Align::Right;
                ui::label(g, {r.x + r.w * 0.72f, r.y, r.w * 0.28f, labelH}, row.shown, shown);
                f32 value = row.value;
                const bool moved = ui::slider(g, rowId(base, 0x22, i),
                                              {r.x + g.px(4.0f), r.y + labelH, r.w - g.px(8.0f), h - labelH - g.px(6.0f)},
                                              value, row.min, row.max, row.step);
                if (moved && row.enabled) {
                    result.slider = static_cast<i32>(i);
                    result.value = value;
                }
                if (state.longPress) {
                    result.held = static_cast<i32>(i);
                }
                break;
            }
            case OptionRow::Kind::Choice: {
                // The label over the drop-down (GameFontNormalSmall), the box with the choice and its arrow.
                const f32 labelH = g.px(22.0f);
                const ui::WidgetState labelState = ui::hit(g, rowId(base, 0x33, i), {r.x, r.y, r.w, labelH});
                ui::label(g, {r.x, r.y, r.w, labelH}, row.label, ui::gameFont(g.px(13.0f), textColor));
                const Rect field{r.x, r.y + labelH, std::min(r.w, g.px(240.0f)), h - labelH - g.px(6.0f)};
                const ui::WidgetState state = ui::hit(g, rowId(base, 0x44, i), field);
                ui::tooltipPanel(g, field, rgba(0, 0, 0, ui::held(state, field) ? 220 : 170));
                const f32 arrow = std::min(field.h - g.px(8.0f), g.px(24.0f));
                g.draw.sprite({field.right() - arrow - g.px(4.0f), field.centerY() - arrow * 0.5f, arrow, arrow}, kDropDownArrow,
                              row.enabled ? ui::kWhite : rgba(120, 120, 120));
                const std::string_view current = row.choice < row.choices.size() ? row.choices[row.choice] : std::string_view{};
                ui::label(g, {field.x + g.px(8.0f), field.y, field.w - arrow - g.px(16.0f), field.h}, current,
                          ui::gameFont(g.px(13.0f), row.enabled ? ui::kWhite : ui::kDisabledText));
                if (ui::tapped(state) && row.enabled) {
                    result.choiceOpened = static_cast<i32>(i);
                }
                if (labelState.longPress || state.longPress) {
                    result.held = static_cast<i32>(i);
                }
                if (static_cast<i32>(i) == v.openChoice) {
                    choiceAnchor = field;
                }
                break;
            }
            case OptionRow::Kind::Button: {
                const std::string_view label = row.label;
                const f32 bw = std::min(r.w, buttonWidth(g, label, m.button));
                if (ui::button(g, id, {r.x, r.y + (h - m.button) * 0.5f, bw, m.button}, label, row.enabled)) {
                    result.button = static_cast<i32>(i);
                }
                break;
            }
            }
            if (static_cast<i32>(i) == v.tooltipRow) {
                tipAnchor = r;
            }
            y += h;
        }
    }
    ui::endScroll(g, view, std::max(columnH[0], columnH[1]) + g.px(8.0f));

    // An open drop-down's choices under its box (UIDropDownMenu's list), over the rest.
    if (choiceAnchor.w > 0.0f && v.openChoice >= 0 && static_cast<u32>(v.openChoice) < v.rows.size()) {
        const OptionRow& row = v.rows[static_cast<u32>(v.openChoice)];
        const f32 itemH = std::max(m.row, g.px(40.0f));
        Rect list{choiceAnchor.x, choiceAnchor.bottom(), std::max(choiceAnchor.w, g.px(200.0f)),
                  itemH * static_cast<f32>(row.choices.size()) + g.px(12.0f)};
        if (list.bottom() > d.panel.bottom()) {
            list.y = choiceAnchor.y - list.h; // no room under it: above
        }
        ui::tooltipPanel(g, list);
        for (u32 k = 0; k < row.choices.size(); ++k) {
            const Rect item{list.x + g.px(6.0f), list.y + g.px(6.0f) + static_cast<f32>(k) * itemH, list.w - g.px(12.0f), itemH};
            ui::RowStyle style;
            style.selected = k == row.choice;
            if (ui::listRow(g, rowId(base, 0x55, k), item, row.choices[k], style)) {
                result.choice = static_cast<i32>(k);
            }
        }
    }

    // The tooltip of a held row (GameTooltip:SetText(this.tooltipText, nil, nil, nil, nil, 1): wrapped, white).
    if (tipAnchor.w > 0.0f && v.tooltipRow >= 0 && static_cast<u32>(v.tooltipRow) < v.rows.size() &&
        !v.rows[static_cast<u32>(v.tooltipRow)].tooltip.empty()) {
        const OptionRow& row = v.rows[static_cast<u32>(v.tooltipRow)];
        const TextStyle style = ui::gameFont(g.px(14.0f), ui::kWhite);
        const f32 w = std::min(g.px(320.0f), c.safeArea.w - g.px(16.0f));
        const f32 textH = g.draw.measureWrapped(w - g.px(20.0f), row.tooltip, style.font, style.size);
        Rect tip{std::clamp(tipAnchor.x, c.safeArea.x + g.px(8.0f), c.safeArea.right() - w - g.px(8.0f)),
                 tipAnchor.bottom() + g.px(4.0f), w, textH + g.px(20.0f)};
        if (tip.bottom() > c.safeArea.bottom()) {
            tip.y = tipAnchor.y - tip.h - g.px(4.0f);
        }
        ui::tooltipPanel(g, tip);
        g.draw.wrappedText(tip.inset(g.px(10.0f)), row.tooltip, style);
        if (ui::tapped(ui::hit(g, rowId(base, 0x66, 0), tip))) {
            result.tooltipDismissed = true;
        }
    }

    // Defaults at the bottom left; Okay left of Cancel at the bottom right.
    const Rect f = d.footer;
    const std::string_view okay = text::get("OKAY"), cancel = text::get("CANCEL"), defaults = text::get("DEFAULTS");
    const f32 cancelW = buttonWidth(g, cancel, f.h), okayW = buttonWidth(g, okay, f.h);
    result.cancel = ui::button(g, rowId(base, 0x77, 0), {f.right() - cancelW, f.y, cancelW, f.h}, cancel) || d.closed;
    result.okay = ui::button(g, rowId(base, 0x88, 0), {f.right() - cancelW - g.px(8.0f) - okayW, f.y, okayW, f.h}, okay);
    if (v.defaults) {
        result.defaults =
            ui::button(g, rowId(base, 0x99, 0), {f.x, f.y, std::min(buttonWidth(g, defaults, f.h), f.w * 0.35f), f.h}, defaults);
    }
    return result;
}

} // namespace vd
