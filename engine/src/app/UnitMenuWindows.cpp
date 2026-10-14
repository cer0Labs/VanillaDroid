#include "app/UnitMenuWindows.h"

#include "core/Profile.h"
#include "ui/Widgets.h"

#include <algorithm>

namespace vd {
namespace {

using ui::Rect;
using ui::rgba;
using ui::TextStyle;

constexpr u64 kCheck = assetPathHash("Interface\\Buttons\\UI-CheckBox-Check.blp");
constexpr u64 kExpandArrow = assetPathHash("Interface\\ChatFrame\\ChatFrameExpandArrow.blp");
constexpr u64 kHighlight = assetPathHash("Interface\\QuestFrame\\UI-QuestTitleHighlight.blp");
constexpr u32 kGrayFont = rgba(128, 128, 128); // GRAY_FONT_COLOR

// UIDropDownMenu's measures in units (UIDROPDOWNMENU_BUTTON_HEIGHT, the check, the icon, the arrow), times `s`
// logical pixels per unit: the client's scale, raised so that a row takes a finger.
struct Metrics {
    f32 s = 1.0f; // vertical: the row
    f32 h = 1.0f; // horizontal and the art: the font's (10 units of text)
    f32 row = 16.0f;
    f32 border = 8.0f;
    f32 font = 10.0f;
};

Metrics metrics(const DialogContext& c, u32 rows) {
    const ui::Gui g{c.ui, c.draw};
    Metrics m;
    m.border = g.px(9.0f);
    m.s = std::max(g.unit(1.0f), g.px(34.0f) / 16.0f);
    // Everything in sight: smaller on a short screen, never below the client's own size.
    const f32 room = c.safeArea.h - 2.0f * m.border - g.px(8.0f);
    if (rows > 0 && 16.0f * m.s * static_cast<f32>(rows) > room) {
        m.s = std::max(g.unit(1.0f), room / (16.0f * static_cast<f32>(rows)));
    }
    m.row = 16.0f * m.s;
    // GameFontHighlightSmall is 10 units in a 16-unit row; a row raised for a finger keeps a readable size, not
    // the whole height (the frames around it stay at the client's scale).
    m.font = std::max(g.unit(10.0f), std::min(10.0f * m.s, m.row * 0.44f));
    m.h = m.font / 10.0f;
    return m;
}

TextStyle itemStyle(const Metrics& m, const DropDownItemView& item) {
    const u32 color = item.title ? ui::kGold : !item.enabled ? kGrayFont : item.color != 0 ? item.color : ui::kWhite;
    return ui::gameFont(m.font, color);
}

// UIDropDownMenu_AddButton's width: the text + 60, + 10 with an arrow or an icon, - 30 not checkable.
f32 itemWidth(const ui::DrawList& draw, const Metrics& m, const DropDownItemView& item) {
    const TextStyle style = itemStyle(m, item);
    f32 w = draw.measure(item.text, style.font, style.size) + 60.0f * m.h;
    if (item.hasArrow) {
        w += 10.0f * m.h;
    }
    if (!item.checkable) {
        w -= 30.0f * m.h;
    }
    if (item.raidIcon != 0) {
        w += 10.0f * m.h;
    }
    return w;
}

Rect listRect(const ui::DrawList& draw, const Metrics& m, std::span<const DropDownItemView> items, f32 x, f32 y) {
    f32 width = 0.0f;
    for (const DropDownItemView& item : items) {
        width = std::max(width, itemWidth(draw, m, item));
    }
    return {x, y, width + 2.0f * m.border, m.row * static_cast<f32>(items.size()) + 2.0f * m.border};
}

// The client's off-screen rule, against the safe area.
Rect keepOnScreen(Rect r, const Rect& safe, f32 flipX, f32 flipY) {
    if (r.bottom() > safe.bottom()) {
        r.y = std::max(safe.y, flipY - r.h);
    }
    if (r.right() > safe.right()) {
        r.x = std::max(safe.x, flipX - r.w);
    }
    r.x = std::max(r.x, safe.x);
    r.y = std::max(r.y, safe.y);
    return r;
}

// One list; returns the row tapped (-1 none) and where each row is (rowTop).
i32 drawList(DialogContext& c, ui::Gui& g, const Metrics& m, std::string_view name, u32 level, const Rect& r,
             std::span<const DropDownItemView> items, i32 openRow, i32& toggled) {
    ui::tooltipPanel(g, r); // DropDownList's MenuBackdrop: UI-Tooltip-Background / -Border
    (void)c;
    i32 picked = -1;
    for (u32 i = 0; i < items.size(); ++i) {
        const DropDownItemView& item = items[i];
        const Rect row{r.x + m.border * 0.5f, r.y + m.border + m.row * static_cast<f32>(i), r.w - m.border, m.row};
        const ui::WidgetState state = ui::hit(g, ui::widgetId(name, level * 100 + i), row);
        const bool active = !item.title && item.enabled;
        if (active && (ui::held(state, row) || static_cast<i32>(i) == openRow)) {
            g.draw.spriteAdditive(row, kHighlight, rgba(255, 255, 255, 200)); // the button's highlight (ADD)
        }
        // The check column (UI-CheckBox-Check, 24 units at the left), the text 27 units in when checkable.
        f32 textX = r.x + m.border;
        if (item.checkable && !item.title) {
            const f32 check = 24.0f * m.h;
            if (item.checked) {
                g.draw.sprite({textX - 4.0f * m.h, row.centerY() - check * 0.5f, check, check}, kCheck);
            }
            textX += 23.0f * m.h;
        }
        TextStyle style = itemStyle(m, item);
        g.draw.text(textX, ui::centeredBaseline(g.draw, style, row.centerY()), item.text, style);
        const f32 right = r.right() - m.border;
        if (item.raidIcon != 0) {
            const f32 icon = 15.0f * m.h;
            drawRaidTargetIcon(g.draw, {right - 5.0f * m.h - icon, row.centerY() - icon * 0.5f, icon, icon},
                               item.raidIcon);
        }
        if (item.hasArrow) {
            const f32 arrow = 16.0f * m.h;
            g.draw.sprite({right - 6.0f * m.h - arrow, row.centerY() - arrow * 0.5f, arrow, arrow}, kExpandArrow);
        }
        if (active && ui::tapped(state)) {
            if (item.hasArrow) {
                toggled = static_cast<i32>(i);
            } else {
                picked = static_cast<i32>(i);
            }
        }
    }
    return picked;
}

} // namespace

ui::TexCoords raidTargetIconCoords(u8 icon) {
    // RAID_TARGET_ICON_DIMENSION 64 of RAID_TARGET_TEXTURE_DIMENSION 256, four columns.
    const u32 index = icon > 0 ? icon - 1u : 0u;
    const f32 left = static_cast<f32>(index % 4) * 0.25f;
    const f32 top = static_cast<f32>(index / 4) * 0.25f;
    return {left, top, left + 0.25f, top + 0.25f};
}

void drawRaidTargetIcon(ui::DrawList& draw, const ui::Rect& r, u8 icon, u32 tint) {
    if (icon >= 1 && icon <= 8) {
        draw.sprite(r, kRaidTargetIconsTexture, tint, raidTargetIconCoords(icon));
    }
}

ui::Rect dropDownMenuRect(const DialogContext& c, const DropDownMenuView& v) {
    const Metrics m = metrics(c, static_cast<u32>(v.level1.size()));
    const Rect r = listRect(c.draw, m, v.level1, v.x, v.y);
    return keepOnScreen(r, c.safeArea, v.x, v.y);
}

DropDownMenuResult dropDownMenu(DialogContext& c, std::string_view name, const DropDownMenuView& v) {
    VD_PROFILE_ZONE("UnitMenuWindows::dropDown");
    ui::Gui g{c.ui, c.draw};
    DropDownMenuResult result;
    const u32 rows = static_cast<u32>(std::max(v.level1.size(), v.level2.size()));
    const Metrics m = metrics(c, rows);
    // Nothing under the lists takes a finger while the menu is open; a tap outside them closes it.
    result.closed = ui::modalBlocker(g, ui::widgetId(name), 0.0f);
    const Rect first = keepOnScreen(listRect(c.draw, m, v.level1, v.x, v.y), c.safeArea, v.x, v.y);
    i32 toggled = -1;
    const i32 picked1 = drawList(c, g, m, name, 1, first, v.level1, v.level2.empty() ? -1 : v.level2Row, toggled);
    if (picked1 >= 0) {
        result.level = 1;
        result.picked = picked1;
    }
    result.toggled = toggled;
    if (!v.level2.empty() && v.level2Row >= 0) {
        // TOPLEFT to the row's TOPRIGHT, 14 units up (its border beside the row); left of level 1 off the right.
        const f32 rowTop = first.y + m.border + m.row * static_cast<f32>(v.level2Row);
        Rect second = listRect(c.draw, m, v.level2, first.right(), rowTop - m.border);
        if (second.right() > c.safeArea.right()) {
            second.x = first.x - second.w;
        }
        second = keepOnScreen(second, c.safeArea, second.x, rowTop + m.row + m.border);
        i32 unused = -1;
        const i32 picked2 = drawList(c, g, m, name, 2, second, v.level2, -1, unused);
        if (picked2 >= 0) {
            result.level = 2;
            result.picked = picked2;
        }
    }
    return result;
}

void autoFollowStatus(DialogContext& c, std::string_view text, f32 alpha) {
    if (text.empty() || alpha <= 0.0f) {
        return;
    }
    ui::Gui g{c.ui, c.draw};
    TextStyle style = ui::gameFont(g.unit(20.0f), ui::withAlpha(ui::kGold, alpha)); // GameFontNormal, 20 high
    style.align = ui::Align::Center;
    style.shadow = std::max(1.0f, g.px(1.0f));
    const Rect screen{0.0f, 0.0f, c.ui.width(), c.ui.height()};
    g.draw.text(screen.centerX(), ui::centeredBaseline(g.draw, style, screen.centerY()), text, style);
}

} // namespace vd
