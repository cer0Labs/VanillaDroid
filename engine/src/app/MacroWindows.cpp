#include "app/MacroWindows.h"

#include "app/DialogKit.h"
#include "app/UiResources.h"
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

// MacroFrameButtonTemplate: the icon on UI-EmptySlot-Disabled (64 x 64 around the 36-unit button, one unit down),
// ButtonHilight-Square while held, CheckButtonHilight when checked. MacroFrameSelectedMacroBackground: UI-EmptySlot.
constexpr u64 kEmptySlotDisabled = assetPathHash("Interface\\Buttons\\UI-EmptySlot-Disabled.blp");
constexpr u64 kEmptySlot = assetPathHash("Interface\\Buttons\\UI-EmptySlot.blp");
constexpr u64 kButtonHighlight = assetPathHash("Interface\\Buttons\\ButtonHilight-Square.blp");
constexpr u64 kCheckHighlight = assetPathHash("Interface\\Buttons\\CheckButtonHilight.blp");
constexpr u64 kWindowTab = assetPathHash("Interface\\HelpFrame\\HelpFrameTab-Active.blp");
constexpr u64 kWindowTabInactive = assetPathHash("Interface\\HelpFrame\\HelpFrameTab-Inactive.blp");
constexpr u64 kListboxHighlight = assetPathHash("Interface\\Buttons\\UI-Listbox-Highlight.blp");
constexpr u32 kMacroColumns = 6;     // MacroButton1..6 per row
constexpr u32 kIconColumns = 5;      // NUM_ICONS_PER_ROW
constexpr f32 kButtonGapX = 13.0f;   // the buttons' gaps, in units of their 36
constexpr f32 kButtonGapY = 10.0f;
constexpr f32 kIconGapX = 10.0f;     // MacroPopupButton's
constexpr f32 kIconGapY = 8.0f;

// A macro button (MacroFrameButtonTemplate) with `iconHash` (0: empty, disabled). `r` is the 36-unit button.
ui::WidgetState macroButton(DialogContext& c, Gui& g, WidgetId id, const Rect& r, u64 iconHash,
                            std::string_view name, bool checked, bool enabled, u64 background) {
    const f32 k = r.w / 36.0f;
    g.draw.sprite({r.centerX() - 32.0f * k, r.centerY() - 32.0f * k + k, 64.0f * k, 64.0f * k}, background);
    ui::WidgetState state;
    if (enabled) {
        state = ui::hit(g, id, r);
    }
    if (iconHash != 0) {
        if (const std::optional<u32> icon = c.resources.icon(iconHash)) {
            g.draw.icon(r, *icon);
        }
    }
    if (!name.empty()) { // GameFontHighlightSmallOutline, its bottom 2 units over the button's
        TextStyle style = ui::gameFont(std::max(g.px(9.0f), 10.0f * k * 0.95f), ui::kWhite);
        style.outline = std::max(1.0f, k);
        style.shadow = 0.0f;
        style.align = Align::Center;
        g.draw.pushClip(r);
        g.draw.text(r.centerX(), r.bottom() - 2.0f * k - style.size * 0.2f, name, style);
        g.draw.popClip();
    }
    if (checked) {
        g.draw.spriteAdditive(r, kCheckHighlight);
    }
    if (enabled && ui::held(state, r)) {
        g.draw.spriteAdditive(r, kButtonHighlight, rgba(255, 255, 255, 160));
    }
    return state;
}

// A tab of the frame (TabButtonTemplate: HelpFrameTab-Inactive, the selected one HelpFrameTab-Active, 3 units lower).
bool frameTab(Gui& g, WidgetId id, const Rect& r, std::string_view text, bool active, bool enabled) {
    const ui::WidgetState state = enabled ? ui::hit(g, id, r) : ui::WidgetState{};
    // 16-unit ends of the 64 x 32 art (TexCoords 0..0.25, 0.75..1) on a 32-unit-high tab.
    const f32 cap = std::min(r.w * 0.3f, r.h * 0.5f);
    ui::threeSlice(g.draw, active ? Rect{r.x, r.y + r.h * 3.0f / 32.0f, r.w, r.h} : r, active ? kWindowTab : kWindowTabInactive,
                   {0.0f, 0.0f, 1.0f, 1.0f}, 0.25f, cap);
    if (enabled && ui::held(state, r) && !active) {
        g.draw.spriteAdditive(r.inset(cap * 0.3f, r.h * 0.15f), kListboxHighlight, rgba(255, 255, 255, 120));
    }
    TextStyle style = ui::gameFont(std::min(g.px(14.0f), r.h * 0.45f),
                                   !enabled ? ui::kDisabledText : active ? ui::kWhite : ui::kGold);
    style.align = Align::Center;
    ui::label(g, {r.x + cap * 0.4f, r.y, r.w - cap * 0.8f, r.h}, text, style);
    return enabled && ui::tapped(state);
}

// MacroFrameText inside MacroFrameTextBackground (the tooltip backdrop): the commands wrapped, a caret after the
// last letter while editing. Returns true when tapped (MacroFrameTextButton).
bool bodyBox(Gui& g, const Rect& r, std::string_view body, bool editing) {
    ui::tooltipPanel(g, r);
    const Rect inner = r.inset(g.px(10.0f), g.px(8.0f));
    const ui::ScrollView view = ui::beginScroll(g, widgetId("macro.body.scroll"), inner, editing);
    const TextStyle style = ui::gameFont(g.px(14.0f), ui::kWhite); // GameFontHighlightSmall, a little larger
    const f32 used = body.empty() ? 0.0f : g.draw.wrappedText({inner.x, view.top, inner.w, 1e6f}, body, style);
    if (editing && (g.ui.nowMs() / 500) % 2 == 0) {
        // The caret after the last letter: where the last byte was laid out (a new line starts at the left).
        Rect at{inner.x, view.top, 0.0f, style.size};
        if (!body.empty()) {
            std::array<Rect, 8> spans{};
            const u32 n = g.draw.wrappedSpan(inner.w, body, style.font, style.size, body.size() - 1, body.size(), spans);
            if (n > 0) {
                at = spans[n - 1];
                at.x += inner.x;
                at.y += view.top;
                if (body.back() == '\n') {
                    at = {inner.x, at.y + g.draw.lineHeight(style.font, style.size), 0.0f, style.size};
                } else {
                    at.x += at.w;
                }
            }
        }
        g.draw.solid({at.x + g.px(1.0f), at.y + style.size * 0.1f, std::max(1.0f, g.px(1.5f)), style.size * 0.9f},
                     ui::kWhite);
    }
    ui::endScroll(g, view, used + style.size);
    const ui::WidgetState state = ui::hit(g, widgetId("macro.body"), r);
    return ui::tapped(state);
}

} // namespace

MacroFrameResult macroFrame(DialogContext& c, const MacroFrameView& v) {
    VD_PROFILE_ZONE("MacroWindows::frame");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    MacroFrameResult result;
    const Dialog d = beginDialog(c, g, "macro", wide ? 760.0f : 520.0f, wide ? 440.0f : 700.0f,
                                 text::get("CREATE_MACROS"));
    const bool enabled = !v.popup;
    const f32 gap = g.px(8.0f);
    // The original's column needs a tall place; a place wider than that (a square-profile panel under the micro menu
    // and the bag row) gets the two columns too.
    const bool columns = wide || d.body.h < d.body.w * 0.85f;

    // Wide: the tabs and the buttons at the left, the selected macro and its commands at the right. Square: the
    // original's column.
    const Rect left = columns ? Rect{d.body.x, d.body.y, std::round(d.body.w * 0.5f), d.body.h} : d.body;
    // The tabs (GENERAL_MACROS, CHARACTER_SPECIFIC_MACROS).
    const f32 tabH = g.px(34.0f);
    const f32 tabW = std::min((left.w - gap) * 0.5f, g.px(220.0f));
    const std::string_view tabs[2] = {text::get("GENERAL_MACROS"), v.characterTab};
    for (u32 i = 0; i < 2; ++i) {
        if (frameTab(g, widgetId("macro.tab", i), {left.x + static_cast<f32>(i) * (tabW + gap), left.y, tabW, tabH},
                     tabs[i], v.tab == i, enabled)) {
            result.tab = static_cast<i32>(i);
        }
    }
    // The 18 buttons: three rows of six, 13 units apart across, 10 down.
    const f32 gridTop = left.y + tabH + gap * 1.5f;
    const f32 fitW = (left.w - g.px(8.0f)) / (kMacroColumns + (kMacroColumns - 1) * kButtonGapX / 36.0f);
    const f32 fitH = columns ? (left.bottom() - gridTop) / (3.0f + 2.0f * kButtonGapY / 36.0f + 0.2f) : 1e6f;
    const f32 button = std::floor(std::min({fitW, fitH, g.px(56.0f)}));
    const f32 stepX = button * (1.0f + kButtonGapX / 36.0f);
    const f32 stepY = button * (1.0f + kButtonGapY / 36.0f);
    const f32 gridW = button + stepX * static_cast<f32>(kMacroColumns - 1);
    const f32 gridX = left.x + (left.w - gridW) * 0.5f;
    for (u32 i = 0; i < kMacroColumns * 3; ++i) {
        const Rect r{gridX + static_cast<f32>(i % kMacroColumns) * stepX, gridTop + static_cast<f32>(i / kMacroColumns) * stepY,
                     button, button};
        const bool filled = i < v.macros.size();
        const MacroSlotView slot = filled ? v.macros[i] : MacroSlotView{};
        const ui::WidgetState state = macroButton(c, g, widgetId("macro.button", i), r, slot.iconHash, slot.name,
                                                  filled && static_cast<i32>(i) == v.selected, filled && enabled,
                                                  kEmptySlotDisabled);
        if (state.longPress) {
            result.pickup = static_cast<i32>(i); // OnDragStart: PickupMacro
        } else if (ui::tapped(state)) {
            result.select = static_cast<i32>(i);
        }
    }
    const f32 gridBottom = gridTop + stepY * 2.0f + button;

    // The selected macro: its button on UI-EmptySlot, its name, Change Name/Icon; then the commands.
    const Rect right = columns ? Rect{left.right() + gap * 2.0f, d.body.y, d.body.right() - left.right() - gap * 2.0f,
                                   d.body.h}
                            : Rect{d.body.x, gridBottom + gap, d.body.w, d.body.bottom() - gridBottom - gap};
    if (!columns) {
        ui::horizontalBreak(g, right.x, right.y, right.w);
    }
    const bool hasSelection = v.selected >= 0 && static_cast<u32>(v.selected) < v.macros.size();
    f32 y = right.y + (columns ? 0.0f : gap);
    const f32 big = std::min(g.px(52.0f), button * 1.05f);
    if (hasSelection) {
        const MacroSlotView& selected = v.macros[static_cast<u32>(v.selected)];
        const Rect r{right.x + g.px(10.0f), y + g.px(10.0f), big, big};
        const ui::WidgetState state =
            macroButton(c, g, widgetId("macro.selected"), r, selected.iconHash, {}, false, enabled, kEmptySlot);
        if (ui::tapped(state) || state.longPress) {
            result.pickup = v.selected; // MacroFrameSelectedMacroButton: PickupMacro(MacroFrame.selectedMacro)
        }
        const f32 textX = r.right() + g.px(14.0f);
        TextStyle nameStyle = ui::gameFont(g.px(18.0f)); // GameFontNormalLarge
        ui::label(g, {textX, r.y - g.px(2.0f), right.right() - textX, g.px(24.0f)}, selected.name, nameStyle);
        const std::string_view change = text::get("CHANGE_MACRO_NAME_ICON");
        const f32 changeW = std::min(right.right() - textX, buttonWidth(g, change, m.button));
        result.change = ui::button(g, widgetId("macro.change"), {textX, r.y + g.px(24.0f), changeW, m.button}, change,
                                   enabled);
        y = std::max(r.bottom(), r.y + g.px(24.0f) + m.button) + gap;
        TextStyle label = ui::gameFont(g.px(13.0f), ui::kWhite); // GameFontHighlightSmall
        ui::label(g, {right.x + g.px(10.0f), y, right.w, g.px(20.0f)}, text::get("ENTER_MACRO_LABEL"), label);
        y += g.px(22.0f);
        // MacroFrameCharLimitText under the box.
        const f32 limitH = g.px(20.0f);
        const Rect box{right.x, y, right.w, std::max(g.px(60.0f), right.bottom() - y - limitH - g.px(4.0f))};
        if (bodyBox(g, box, v.body, v.editing) && enabled) {
            result.bodyTapped = true;
        }
        char limit[64];
        u32 letters = 0;
        for (const char ch : v.body) {
            letters += (static_cast<u8>(ch) & 0xC0u) != 0x80u ? 1u : 0u; // GetNumLetters counts characters
        }
        label.align = Align::Center;
        ui::label(g, {box.x, box.bottom() + g.px(2.0f), box.w, limitH},
                  text::format(limit, "MACROFRAME_CHAR_LIMIT", static_cast<int>(letters)), label);
    }

    // Delete at the left; New and Exit at the right (MacroFrame_Update enables them).
    const Rect f = d.footer;
    const std::string_view deleteText = text::get("DELETE"), newText = text::get("NEW"), exitText = text::get("EXIT");
    const f32 deleteW = buttonWidth(g, deleteText, f.h), newW = buttonWidth(g, newText, f.h);
    const f32 exitW = buttonWidth(g, exitText, f.h);
    result.remove = ui::button(g, widgetId("macro.delete"), {f.x, f.y, deleteW, f.h}, deleteText, enabled && hasSelection);
    result.create = ui::button(g, widgetId("macro.new"), {f.right() - exitW - gap - newW, f.y, newW, f.h}, newText,
                               enabled && v.macros.size() < kMacroColumns * 3);
    const bool exit = ui::button(g, widgetId("macro.exit"), {f.right() - exitW, f.y, exitW, f.h}, exitText);
    result.closed = d.closed || exit;
    return result;
}

MacroPopupResult macroPopup(DialogContext& c, const MacroPopupView& v) {
    VD_PROFILE_ZONE("MacroWindows::popup");
    // A modal panel over the frame, wherever the frame is.
    DialogContext modal = c;
    modal.panel = {};
    Gui g{modal.ui, modal.draw};
    const Metrics m = metrics(modal);
    const bool wide = modal.profile == ScreenProfile::Wide;
    MacroPopupResult result;
    const Dialog d = beginDialog(modal, g, "macropopup", wide ? 560.0f : 440.0f, wide ? 420.0f : 600.0f, {}, false);
    const TextStyle label = ui::gameFont(g.px(13.0f), ui::kWhite); // GameFontHighlightSmall
    f32 y = d.body.y;
    ui::label(g, {d.body.x + g.px(6.0f), y, d.body.w, g.px(20.0f)}, text::get("MACRO_POPUP_TEXT"), label);
    y += g.px(22.0f);
    const Rect field{d.body.x + g.px(6.0f), y, std::min(d.body.w - g.px(12.0f), g.px(300.0f)), std::max(m.button, g.px(36.0f))};
    result.nameTapped = ui::textField(g, widgetId("macropopup.name"), field, v.name, {}, v.nameEditing);
    y = field.bottom() + g.px(10.0f);
    ui::label(g, {d.body.x + g.px(6.0f), y, d.body.w, g.px(20.0f)}, text::get("MACRO_POPUP_CHOOSE_ICON"), label);
    y += g.px(24.0f);

    // MacroPopupScrollFrame: the icons five a row (the original shows four rows of the list at a time).
    const Rect list{d.body.x, y, d.body.w, d.body.bottom() - y};
    const ui::ScrollView view = ui::beginScroll(g, widgetId("macropopup.icons"), list);
    const f32 icon = std::floor(std::min(g.px(54.0f), (list.w - g.px(16.0f)) / (kIconColumns + (kIconColumns - 1) * kIconGapX / 36.0f)));
    const f32 stepX = icon * (1.0f + kIconGapX / 36.0f), stepY = icon * (1.0f + kIconGapY / 36.0f);
    const f32 gridX = list.x + (list.w - (icon + stepX * (kIconColumns - 1))) * 0.5f;
    const u32 rows = (static_cast<u32>(v.icons.size()) + kIconColumns - 1) / kIconColumns;
    const f32 pad = icon * 0.25f;
    // Only the rows in sight are drawn (2684 icons in 1.12.1).
    const f32 scrolled = list.y - view.top;
    const u32 firstRow = scrolled > pad ? static_cast<u32>((scrolled - pad) / stepY) : 0u;
    const u32 lastRow = std::min(rows, static_cast<u32>((scrolled + list.h) / stepY) + 2u);
    for (u32 row = firstRow; row < lastRow; ++row) {
        for (u32 col = 0; col < kIconColumns; ++col) {
            const u32 index = row * kIconColumns + col;
            if (index >= v.icons.size()) {
                break;
            }
            const Rect r{gridX + static_cast<f32>(col) * stepX, view.top + pad + static_cast<f32>(row) * stepY, icon, icon};
            const ui::WidgetState state =
                macroButton(modal, g, widgetId("macropopup.icon", index), r, v.icons[index].textureHash, {},
                            static_cast<i32>(index) == v.selectedIcon, true, kEmptySlotDisabled);
            if (ui::tapped(state)) {
                result.icon = static_cast<i32>(index);
            }
        }
    }
    ui::endScroll(g, view, pad * 2.0f + static_cast<f32>(rows) * stepY);

    // Okay left of Cancel at the bottom right.
    const Rect f = d.footer;
    const std::string_view okayText = text::get("OKAY"), cancelText = text::get("CANCEL");
    const f32 okayW = buttonWidth(g, okayText, f.h), cancelW = buttonWidth(g, cancelText, f.h);
    result.cancel = ui::button(g, widgetId("macropopup.cancel"), {f.right() - cancelW, f.y, cancelW, f.h}, cancelText);
    result.okay = ui::button(g, widgetId("macropopup.okay"), {f.right() - cancelW - g.px(8.0f) - okayW, f.y, okayW, f.h},
                             okayText, v.okay);
    result.cancel = result.cancel || d.closed || d.outside;
    return result;
}

} // namespace vd
