#include "app/TouchPetWindows.h"

#include "app/TouchRoundIcon.h" // retouches (ui models)
#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace vd {
namespace {

using ui::Align;
using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::TextStyle;
using ui::withAlpha;

constexpr u32 kBackground = rgba(12, 12, 28, 255);
constexpr u64 kHappiness = assetPathHash("Interface\\PetPaperDollFrame\\UI-PetHappiness.blp");
constexpr u64 kHighlight = assetPathHash("Interface\\Buttons\\UI-Listbox-Highlight.blp");
constexpr u64 kCheck = assetPathHash("Interface\\Buttons\\UI-CheckBox-Check.blp");
constexpr u32 kMaxColumns = 6;
constexpr u32 kMaxButtons = 32;

// A round button as the cluster draws it (Hud::touchButton; retouches (ui models): app/TouchRoundIcon, the icon
// without a rim and the 1.12 button's states on it).
void roundButton(DialogContext& c, f32 cx, f32 cy, f32 r, const HudActionSlot& slot, bool pressed) {
    drawRoundIcon(c.draw, c.resources, cx, cy, pressed ? r * kRoundIconPressed : r, slot, RoundIconState{},
                  c.ui.nowMs());
}

// Where a window `w` x `h` opens beside the pet button: toward the screen's middle, centered on it in height,
// inside the safe area.
Rect besideAnchor(const DialogContext& c, const Rect& anchor, f32 w, f32 h, bool mirrored, f32 gap) {
    Rect r{mirrored ? anchor.right() + gap : anchor.x - gap - w, anchor.centerY() - h * 0.5f, w, h};
    const Rect& safe = c.safeArea;
    r.x = std::clamp(r.x, safe.x, std::max(safe.x, safe.right() - w));
    r.y = std::clamp(r.y, safe.y, std::max(safe.y, safe.bottom() - h));
    return r;
}

void panelBackground(DialogContext& c, Gui& g, const Rect& r) {
    // Opaque: the cluster's buttons may be under it (the tooltip's own background lets them show through).
    c.draw.solid(r.inset(g.unit(4.0f)), kBackground);
    ui::tooltipPanel(g, r);
}

} // namespace

TouchPetPanelResult touchPetPanel(DialogContext& c, const TouchPetPanelView& v) {
    VD_PROFILE_ZONE("TouchPetWindows::panel");
    Gui g{c.ui, c.draw};
    TouchPetPanelResult result;
    result.closed = ui::modalBlocker(g, ui::widgetId("touchPet.panel"), 0.0f);
    const f32 d = std::max(v.buttonSize, g.px(40.0f));
    const f32 gap = d * 0.32f;
    const f32 pad = g.px(10.0f);
    TextStyle labelStyle = ui::gameFont(std::max(g.px(10.0f), d * 0.26f), ui::kWhite);
    labelStyle.align = Align::Center;
    const f32 labelH = labelStyle.size * 1.15f;
    const f32 cellW = d + gap, cellH = d + labelH + gap * 0.5f;
    const u32 count = std::min<u32>(static_cast<u32>(v.buttons.size()), kMaxButtons);
    // As many columns as fit beside the pet button (the panel then leaves the cluster in view), at least
    // four; else as many as fit on the screen (over the cluster).
    const f32 beside =
        v.mirrored ? c.safeArea.right() - v.anchor.right() - gap : v.anchor.x - gap - c.safeArea.x;
    const u32 besideColumns = static_cast<u32>(std::max(0.0f, (beside - 2.0f * pad + gap) / cellW));
    const u32 fit = static_cast<u32>(std::max(1.0f, (c.safeArea.w * 0.9f - 2.0f * pad + gap) / cellW));
    const u32 columns = std::min(kMaxColumns, besideColumns >= 4 ? besideColumns : fit);
    // The rows: a group starts a new row when it does not fit in what is left of the one before.
    std::array<u32, kMaxButtons> row{}, column{};
    u32 rows = 0, used = 0, widest = 0;
    for (u32 i = 0; i < count; ++i) {
        const bool groupStart = i == 0 || v.buttons[i].group != v.buttons[i - 1].group;
        if (groupStart) {
            u32 size = 0;
            while (i + size < count && v.buttons[i + size].group == v.buttons[i].group) {
                ++size;
            }
            if (i == 0 || used + std::min(size, columns) > columns) {
                ++rows;
                used = 0;
            }
        } else if (used == columns) {
            ++rows;
            used = 0;
        }
        row[i] = rows - 1;
        column[i] = used++;
        widest = std::max(widest, used);
    }
    const TextStyle titleStyle = ui::gameFont(std::max(g.px(14.0f), d * 0.36f), ui::kGold);
    TextStyle subtitleStyle = ui::gameFont(std::max(g.px(11.0f), d * 0.27f), ui::kWhite);
    const f32 face = v.happiness > 0 ? titleStyle.size * 1.3f : 0.0f;
    const f32 header = titleStyle.size * 1.2f + (v.subtitle.empty() ? 0.0f : subtitleStyle.size * 1.15f);
    f32 textW = std::max(c.draw.measure(v.title, titleStyle.font, titleStyle.size) + face,
                         c.draw.measure(v.subtitle, subtitleStyle.font, subtitleStyle.size));
    const f32 w = std::min(
        c.safeArea.w, std::max(static_cast<f32>(std::max(widest, 1u)) * cellW - gap, textW) + 2.0f * pad);
    const f32 h = std::min(c.safeArea.h, header + static_cast<f32>(rows) * cellH + 2.0f * pad);
    const Rect r = besideAnchor(c, v.anchor, w, h, v.mirrored, gap);
    result.panel = r;
    panelBackground(c, g, r);
    // The header: the pet's name and its mood's face, its level and family under.
    f32 y = r.y + pad;
    ui::label(g, {r.x + pad, y, r.w - 2.0f * pad - face, titleStyle.size * 1.2f}, v.title, titleStyle);
    if (v.happiness > 0) { // PetFrame_SetHappiness' faces: happy, content, unhappy
        const f32 u0 = v.happiness >= 3 ? 0.0f : v.happiness == 2 ? 0.1875f : 0.375f;
        c.draw.sprite({r.right() - pad - face, y, face, face * 23.0f / 24.0f}, kHappiness, ui::kWhite,
                      {u0, 0.0f, u0 + 0.1875f, 0.359375f});
    }
    y += titleStyle.size * 1.2f;
    if (!v.subtitle.empty()) {
        ui::label(g, {r.x + pad, y, r.w - 2.0f * pad, subtitleStyle.size * 1.15f}, v.subtitle, subtitleStyle);
        y += subtitleStyle.size * 1.15f;
    }
    // The buttons, each row's centered.
    std::array<u32, kMaxButtons> rowCount{};
    for (u32 i = 0; i < count; ++i) {
        rowCount[row[i]] = std::max(rowCount[row[i]], column[i] + 1);
    }
    for (u32 i = 0; i < count; ++i) {
        const TouchPetPanelButton& b = v.buttons[i];
        const f32 rowW = static_cast<f32>(rowCount[row[i]]) * cellW - gap;
        const f32 cx = r.centerX() - rowW * 0.5f + d * 0.5f + static_cast<f32>(column[i]) * cellW;
        const f32 cy = y + static_cast<f32>(row[i]) * cellH + d * 0.5f;
        const ui::WidgetState state =
            c.ui.widgetCircle(ui::widgetId("touchPet.button", i), cx, cy, d * 0.5f, gap * 0.4f);
        roundButton(c, cx, cy, d * 0.5f, b.slot, state.pressed);
        if (!b.label.empty()) { // smaller when it does not fit; left out when it still does not
            TextStyle style = labelStyle;
            if (c.draw.measure(b.label, style.font, style.size) > cellW) {
                style.size *= 0.8f;
            }
            if (c.draw.measure(b.label, style.font, style.size) <= cellW) {
                c.draw.text(cx, cy + d * 0.5f + labelStyle.size * 0.95f, b.label, style);
            }
        }
        if (state.longPress) {
            result.held = static_cast<i32>(i);
        } else if (state.clicked) {
            result.tapped = static_cast<i32>(i);
        }
    }
    return result;
}

TouchPetFoodResult touchPetFood(DialogContext& c, const TouchPetFoodView& v) {
    VD_PROFILE_ZONE("TouchPetWindows::food");
    Gui g{c.ui, c.draw};
    TouchPetFoodResult result;
    result.closed = ui::modalBlocker(g, ui::widgetId("touchPet.food"), 0.0f);
    const f32 rowH = std::max(g.px(38.0f), g.unit(22.0f));
    const f32 icon = rowH * 0.78f;
    const f32 pad = g.px(8.0f);
    const f32 check = rowH * 0.6f;
    const TextStyle titleStyle = ui::gameFont(std::max(g.px(14.0f), rowH * 0.4f), ui::kGold);
    const TextStyle style = ui::gameFont(std::max(g.px(13.0f), rowH * 0.36f), ui::kWhite);
    TextStyle countStyle = ui::numberFont(style.size, ui::kWhite);
    countStyle.align = Align::Right;
    f32 textW = c.draw.measure(v.rows.empty() ? v.empty : std::string_view{}, style.font, style.size);
    for (const TouchPetFoodRow& row : v.rows) {
        textW = std::max(textW, c.draw.measure(row.name, style.font, style.size));
    }
    const f32 countW = c.draw.measure("999", countStyle.font, countStyle.size);
    const f32 titleH = titleStyle.size * 1.4f;
    const u32 shown = std::max<u32>(1, static_cast<u32>(v.rows.size()));
    const f32 width =
        std::min(c.safeArea.w - 2.0f * pad,
                 std::max(check + icon + textW + countW + 5.0f * pad,
                          c.draw.measure(v.title, titleStyle.font, titleStyle.size) + 2.0f * pad));
    const f32 height = std::min(c.safeArea.h, titleH + rowH * static_cast<f32>(shown) + 2.0f * pad);
    const Rect r = besideAnchor(c, v.anchor, width, height, v.mirrored, pad);
    result.panel = r;
    panelBackground(c, g, r);
    ui::label(g, {r.x + pad, r.y + pad, r.w - 2.0f * pad, titleH}, v.title, titleStyle);
    const Rect body{r.x, r.y + pad + titleH, r.w, r.h - 2.0f * pad - titleH};
    if (v.rows.empty()) {
        TextStyle note = style;
        note.color = ui::kDisabledText;
        ui::label(g, {body.x + pad, body.y, body.w - 2.0f * pad, rowH}, v.empty, note);
        return result;
    }
    const ui::ScrollView view = ui::beginScroll(g, ui::widgetId("touchPet.food.scroll"), body);
    for (u32 i = 0; i < v.rows.size(); ++i) {
        const TouchPetFoodRow& row = v.rows[i];
        const Rect line{body.x + pad * 0.5f, view.top + rowH * static_cast<f32>(i), body.w - pad, rowH};
        if (line.bottom() < body.y || line.y > body.bottom()) {
            continue;
        }
        const ui::WidgetState state = ui::hit(g, ui::widgetId("touchPet.food.row", i), line);
        if (ui::held(state, line)) {
            c.draw.spriteAdditive(line, kHighlight, rgba(255, 255, 255, 200));
        }
        f32 x = line.x + pad * 0.5f;
        if (row.accepted) { // the family ate it before
            c.draw.sprite({x, line.centerY() - check * 0.5f, check, check}, kCheck);
        }
        x += check;
        if (const std::optional<u32> slot = row.icon ? c.resources.icon(row.icon) : std::nullopt) {
            c.draw.icon({x, line.centerY() - icon * 0.5f, icon, icon}, *slot);
        }
        x += icon + pad;
        TextStyle nameStyle = style;
        nameStyle.color = ui::qualityColor(row.quality);
        ui::label(g, {x, line.y, line.right() - x - countW - pad, line.h}, row.name, nameStyle);
        if (row.count > 0) {
            char number[12];
            std::snprintf(number, sizeof(number), "%u", row.count);
            c.draw.text(line.right() - pad, ui::centeredBaseline(c.draw, countStyle, line.centerY()), number,
                        countStyle);
        }
        if (ui::tapped(state)) {
            result.tapped = static_cast<i32>(i);
        }
    }
    ui::endScroll(g, view, rowH * static_cast<f32>(v.rows.size()));
    return result;
}

} // namespace vd
