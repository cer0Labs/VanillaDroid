#include "app/BattlefieldWindows.h"

#include "app/DialogKit.h"
#include "app/UiResources.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

using ui::Align;
using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::TextStyle;
using ui::widgetId;
using namespace kit;

constexpr u64 kPortrait = assetPathHash("Interface\\BattlefieldFrame\\UI-Battlefield-Icon.blp");
constexpr u64 kRowHighlight = assetPathHash("Interface\\Buttons\\UI-Listbox-Highlight2.blp"); // ADD, baked
constexpr u64 kTrackingBorder = assetPathHash("Interface\\Minimap\\MiniMap-TrackingBorder.blp");
constexpr u64 kComboPoint = assetPathHash("Interface\\ComboFrame\\ComboPoint.blp");
constexpr u32 kWhiteText = rgba(255, 255, 255);   // HIGHLIGHT_FONT_COLOR
constexpr u32 kHighlightTint = rgba(255, 209, 0); // the template's OnLoad: SetVertexColor(1.0, 0.82, 0)
constexpr u32 kBlackText = rgba(0, 0, 0);         // GameFontBlack

// The list (BattlefieldZone1..12 in BattlefieldListScrollFrame): every row, scrolled by a finger here instead
// of the FauxScrollFrame's arrows.
void zoneList(DialogContext& c, Gui& g, const Metrics& m, const Rect& area, const BattlefieldFrameView& v,
              BattlefieldFrameResult& result) {
    ui::tooltipPanel(g, area, rgba(0, 0, 0, 150));
    const ui::ScrollView list = ui::beginScroll(g, widgetId("battlefield.list"), area.inset(g.px(5.0f)));
    const f32 rowH = std::round(m.row * 0.85f);
    f32 y = list.top;
    for (u32 i = 0; i < v.rows.size(); ++i) {
        const BattlefieldRowView& row = v.rows[i];
        const Rect r{list.viewport.x, y, list.viewport.w, rowH};
        y += rowH;
        if (r.bottom() < list.viewport.y || r.y > list.viewport.bottom()) {
            continue; // scrolled out: nothing drawn, nothing to hit
        }
        const ui::WidgetState state = ui::hit(g, widgetId("battlefield.row", i), r);
        const bool selected = static_cast<i32>(i) == v.selected;
        const bool down = ui::held(state, r);
        if (selected || down) {
            g.draw.spriteAdditive(r, kRowHighlight, ui::withAlpha(kHighlightTint, selected ? 1.0f : 0.6f));
        }
        // ButtonText at LEFT + (5, 1): GameFontNormal, GameFontHighlight while highlighted; $parentStatus at
        // RIGHT - 10 in GameFontNormal.
        TextStyle status = ui::gameFont(m.text);
        status.align = Align::Right;
        const f32 statusW =
            row.status.empty() ? 0.0f : g.draw.measure(row.status, status.font, status.size) + g.px(8.0f);
        if (statusW > 0.0f) {
            ui::label(g, {r.right() - statusW - g.px(8.0f), r.y, statusW, r.h}, row.status, status);
        }
        const f32 x = r.x + g.px(6.0f);
        ui::label(g, {x, r.y, r.right() - x - statusW - g.px(10.0f), r.h}, row.text,
                  ui::gameFont(m.text, selected || down ? kWhiteText : ui::kGold));
        if (state.longPress) {
            result.rowTip = static_cast<i32>(i);
            result.tipAnchor = r;
        } else if (ui::tapped(state)) {
            result.select = static_cast<i32>(i);
        }
    }
    ui::endScroll(g, list, y - list.top);
    (void)c;
}

} // namespace

BattlefieldFrameResult battlefieldFrame(DialogContext& c, const BattlefieldFrameView& v) {
    VD_PROFILE_ZONE("BattlefieldWindows::frame");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    BattlefieldFrameResult result;
    const Dialog d =
        beginDialog(c, g, "battlefield", wide ? 720.0f : 440.0f, wide ? 420.0f : 640.0f, v.title, true, true);
    result.closed = d.closed;
    const f32 gap = g.px(6.0f);

    // BattlefieldFramePortrait (UI-Battlefield-Icon) and BATTLEFIELD_NAME beside it, over the list.
    const f32 portrait = std::round(std::min(m.row * 1.1f, g.px(48.0f)));
    g.draw.sprite({d.body.x, d.body.y, portrait, portrait}, kPortrait);
    ui::label(g, {d.body.x + portrait + gap * 2.0f, d.body.y, d.body.w - portrait - gap * 2.0f, portrait},
              text::get("BATTLEFIELD_NAME"), ui::gameFont(m.heading, kWhiteText));
    const f32 top = d.body.y + portrait + gap;
    Rect listRect, textRect;
    if (wide) {
        const f32 listW = std::round(d.body.w * 0.55f);
        listRect = {d.body.x, top, listW, d.body.bottom() - top};
        textRect = {listRect.right() + gap, d.body.y, d.body.right() - listRect.right() - gap, d.body.h};
    } else {
        const f32 rest = d.body.bottom() - top;
        listRect = {d.body.x, top, d.body.w, std::round(rest * 0.6f)};
        textRect = {d.body.x, listRect.bottom() + gap, d.body.w, d.body.bottom() - listRect.bottom() - gap};
    }
    zoneList(c, g, m, listRect, v, result);
    // BattlefieldFrameZoneDescription: GameFontBlack on the frame's parchment.
    const ParchmentScroll p = beginParchment(g, widgetId("battlefield.text"), textRect);
    Flow flow = parchmentFlow(g, p);
    flow.text(v.description, ui::questFont(m.text, kBlackText), g.px(4.0f));
    endParchment(g, p, flow);

    // CancelButton at the right, JoinButton left of it, GroupJoinButton left of that (when
    // CanJoinBattlefieldAsGroup).
    const Rect& f = d.footer;
    f32 cancelW = buttonWidth(g, text::get("CANCEL"), f.h);
    f32 joinW = buttonWidth(g, text::get("BATTLEFIELD_JOIN"), f.h);
    f32 groupW = v.groupJoinShown ? buttonWidth(g, text::get("BATTLEFIELD_GROUP_JOIN"), f.h) : 0.0f;
    const f32 gaps = v.groupJoinShown ? gap * 2.0f : gap;
    const f32 wanted = cancelW + joinW + groupW;
    if (wanted + gaps > f.w) {
        const f32 k = std::max(0.0f, f.w - gaps) / wanted;
        cancelW *= k;
        joinW *= k;
        groupW *= k;
    }
    const Rect cancel{f.right() - cancelW, f.y, cancelW, f.h};
    const Rect join{cancel.x - gap - joinW, f.y, joinW, f.h};
    result.closed =
        ui::button(g, widgetId("battlefield.cancel"), cancel, text::get("CANCEL")) || result.closed;
    result.join = ui::button(g, widgetId("battlefield.join"), join, text::get("BATTLEFIELD_JOIN"));
    if (v.groupJoinShown) {
        const Rect group{join.x - gap - groupW, f.y, groupW, f.h};
        // Its tooltip (OnEnter) also shows while disabled: a long press on it (the same widget: a long press
        // is no click).
        const ui::WidgetState held = ui::hit(g, widgetId("battlefield.group"), group);
        result.joinAsGroup = ui::button(g, widgetId("battlefield.group"), group,
                                        text::get("BATTLEFIELD_GROUP_JOIN"), v.groupJoinEnabled);
        if (held.longPress) {
            result.groupTip = true;
            result.tipAnchor = group;
        }
    }
    return result;
}

void battlefieldTooltip(DialogContext& c, const Rect& anchor, std::string_view title, std::string_view text) {
    Gui g{c.ui, c.draw};
    const f32 pad = g.px(9.0f);
    const f32 w = std::min(g.px(280.0f), c.safeArea.w - g.px(12.0f));
    const TextStyle head = ui::gameFont(g.px(14.0f), kWhiteText);
    const TextStyle body = ui::gameFont(g.px(12.5f), ui::kGold);
    const f32 textW = w - 2.0f * pad;
    const f32 headH = g.draw.measureWrapped(textW, title, head.font, head.size);
    const f32 bodyH = text.empty() ? 0.0f : g.draw.measureWrapped(textW, text, body.font, body.size);
    const f32 h = headH + bodyH + 2.0f * pad + (text.empty() ? 0.0f : g.px(4.0f));
    // GameTooltip_SetDefaultAnchor puts it at the screen's bottom right; here beside what was held.
    f32 x = anchor.right() + g.px(6.0f);
    if (x + w > c.safeArea.right()) {
        x = anchor.x - g.px(6.0f) - w;
    }
    x = std::clamp(x, c.safeArea.x, std::max(c.safeArea.x, c.safeArea.right() - w));
    const f32 y = std::clamp(anchor.y, c.safeArea.y, std::max(c.safeArea.y, c.safeArea.bottom() - h));
    ui::tooltipPanel(g, {x, y, w, h});
    f32 at = y + pad;
    at += g.draw.wrappedText({x + pad, at, textW, headH + 1.0f}, title, head) + g.px(4.0f);
    if (!text.empty()) {
        g.draw.wrappedText({x + pad, at, textW, bodyH + 1.0f}, text, body);
    }
}

// --- MiniMapBattlefieldFrame
// ---------------------------------------------------------------------------------

ui::Rect battlefieldButtonRect(const MinimapLayout& l, BattlefieldButtonPlace p) {
    return l.at(p.x - 4.0f, p.y - 4.0f, 41.0f, 41.0f);
}

BattlefieldButtonResult battlefieldMinimapButton(DialogContext& c, const MinimapLayout& l,
                                                 const BattlefieldButtonView& v) {
    VD_PROFILE_ZONE("BattlefieldWindows::minimapButton");
    Gui g{c.ui, c.draw};
    BattlefieldButtonResult result;
    const BattlefieldButtonPlace p = v.place;
    result.rect = battlefieldButtonRect(l, p);
    const ui::WidgetState state = ui::hit(g, widgetId("minimap.battlefield"), result.rect);
    const f32 alpha = std::clamp(v.alpha, 0.0f, 1.0f);
    const u32 tint = ui::withAlpha(state.pressed ? rgba(190, 190, 190) : ui::kWhite, alpha);
    // MiniMapBattlefieldIcon (BACKGROUND), BattlegroundShine (BORDER, ADD), MiniMapBattlefieldBorder
    // (ARTWORK).
    g.draw.sprite(l.at(p.x - 0.5f, p.y + 0.5f, 32.0f, 32.0f), v.icon, tint);
    if (v.shine > 0.0f) {
        g.draw.spriteAdditive(l.at(p.x + 3.5f, p.y - 2.5f, 32.0f, 32.0f), kComboPoint,
                              ui::withAlpha(ui::kWhite, std::clamp(v.shine, 0.0f, 1.0f) * alpha),
                              {0.5625f, 0.0f, 1.0f, 1.0f});
    }
    g.draw.sprite(l.at(p.x, p.y, 52.0f, 52.0f), kTrackingBorder, ui::withAlpha(ui::kWhite, alpha));
    result.longPress = state.longPress;
    result.tapped = !state.longPress && ui::tapped(state);
    return result;
}

ui::Rect battlefieldMenuAnchor(const MinimapLayout& l, BattlefieldButtonPlace p) {
    return l.at(p.x + 10.0f, p.y + 33.0f + 5.0f, 0.0f, 0.0f); // the button's BOTTOMLEFT + (10, 5 down)
}

void battlefieldButtonTooltip(DialogContext& c, const Rect& anchor, std::string_view text) {
    if (text.empty()) {
        return;
    }
    Gui g{c.ui, c.draw};
    TextStyle style = ui::gameFont(g.px(13.0f), rgba(255, 210, 0));
    const f32 pad = g.px(9.0f);
    // The widest of its lines (the colour codes do not count).
    f32 widest = 0.0f;
    usize start = 0;
    while (start <= text.size()) {
        const usize end = std::min(text.find('\n', start), text.size());
        widest = std::max(widest, g.draw.measure(text.substr(start, end - start), style.font, style.size));
        start = end + 1;
    }
    const f32 textW = std::min(widest + g.px(2.0f), c.safeArea.w - 2.0f * pad);
    const f32 textH = g.draw.measureWrapped(textW, text, style.font, style.size);
    const f32 w = textW + 2.0f * pad, h = textH + 2.0f * pad;
    // ANCHOR_LEFT: its bottom right at the owner's top left.
    const f32 x = std::clamp(anchor.x - w, c.safeArea.x, std::max(c.safeArea.x, c.safeArea.right() - w));
    const f32 y = std::clamp(anchor.y - h, c.safeArea.y, std::max(c.safeArea.y, c.safeArea.bottom() - h));
    ui::tooltipPanel(g, {x, y, w, h});
    g.draw.wrappedText({x + pad, y + pad, textW + 1.0f, textH + 1.0f}, text, style);
}

} // namespace vd
