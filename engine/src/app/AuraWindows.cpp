#include "app/AuraWindows.h"

#include "app/DialogKit.h"
#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>

namespace vd {
namespace {

using ui::Align;
using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::TextStyle;
using ui::widgetId;

constexpr u64 kDebuffOverlays = assetPathHash("Interface\\Buttons\\UI-Debuff-Overlays.blp");
constexpr u64 kTempEnchantBorder = assetPathHash("Interface\\Buttons\\UI-TempEnchant-Border.blp");
constexpr u64 kButtonHighlight = assetPathHash("Interface\\Buttons\\ButtonHilight-Square.blp");
// UI-Debuff-Overlays: the debuff frame (BuffButtonHarmful's $parentBorder TexCoords).
constexpr ui::TexCoords kDebuffBorderUv{0.296875f, 0.0f, 0.5703125f, 0.515625f};

Rect centered(const Rect& r, f32 w, f32 h) {
    return {r.centerX() - w * 0.5f, r.centerY() - h * 0.5f, w, h};
}

} // namespace

AuraButtonsResult auraButtons(ui::Context& ui, ui::DrawList& draw, UiResources& resources,
                              std::span<const HudAura> auras, std::span<const AuraButtonPlace> places,
                              f32 unit, u64 nowMs, AuraButtonStyle style, std::string_view idName,
                              bool bordered) {
    VD_PROFILE_ZONE("AuraWindows::buttons");
    AuraButtonsResult result;
    const bool player = style == AuraButtonStyle::Player || style == AuraButtonStyle::PlayerNoDuration;
    for (const AuraButtonPlace& p : places) {
        if (p.aura >= auras.size()) {
            continue;
        }
        const HudAura& a = auras[p.aura];
        const Rect& r = p.icon;
        const ui::WidgetState state = ui.widget(widgetId(idName, p.aura), p.hit);
        result.bottom = std::max(result.bottom, p.hit.bottom());
        if (state.clicked) {
            result.tapped = static_cast<i32>(p.aura);
            result.rect = r;
        }
        if (state.longPress) {
            result.held = static_cast<i32>(p.aura);
            result.rect = r;
        }
        // BuffButton_OnUpdate: the whole button (icon, border, count) flashes; the duration under it does
        // not.
        const f32 alpha = player ? auraButtonAlpha(a.remaining, nowMs) : 1.0f;
        // Retouches (windows): the question mark while its icon loads.
        ui::iconOrPlaceholder(draw, r, resources.icon(a.iconHash), ui::withAlpha(ui::kWhite, alpha));
        if (a.enchant) { // BuffButtonTempEnchant: UI-TempEnchant-Border, 32 x 32 over 30
            draw.sprite(centered(r, r.w * 32.0f / 30.0f, r.h * 32.0f / 30.0f), kTempEnchantBorder,
                        ui::withAlpha(ui::kWhite, alpha));
        } else if (a.harmful || bordered) {
            // BuffButtonHarmful 33 x 32 over 30; TargetDebuffButtonTemplate and PartyBuffButtonTemplate 2
            // units wider.
            const Rect border = player ? centered(r, r.w * 33.0f / 30.0f, r.h * 32.0f / 30.0f)
                                       : centered(r, r.w + 2.0f * unit, r.h + 2.0f * unit);
            draw.sprite(border, kDebuffOverlays, ui::withAlpha(debuffTypeColor(a.dispel), alpha),
                        kDebuffBorderUv);
        }
        if (state.pressed) { // touch feedback (the client has none: a mouse hovers)
            draw.spriteAdditive(r, kButtonHighlight, rgba(255, 255, 255, 140));
        }
        // Applications: BuffButton's NumberFontNormal (14) at BOTTOMRIGHT (-2, 2); the target's debuffs'
        // NumberFontNormalSmall (12) at (-1, 0); the small frames show none.
        if (a.stacks > 1 && style != AuraButtonStyle::Small && (player || a.harmful)) {
            char stacks[8];
            std::snprintf(stacks, sizeof(stacks), "%u", a.stacks);
            TextStyle count =
                ui::numberFont((player ? 14.0f : 12.0f) * unit, ui::withAlpha(ui::kWhite, alpha));
            count.outline = std::max(1.0f, unit);
            count.align = Align::Right;
            draw.text(r.right() - (player ? 2.0f : 1.0f) * unit, r.bottom() - (player ? 2.0f : 0.0f) * unit,
                      stacks, count);
        }
        // BuffButtonNDuration (GameFontNormalSmall) under the button, white in its last minute.
        if (style == AuraButtonStyle::Player && a.remaining > 0.0f) {
            char left[24];
            TextStyle duration = ui::gameFont(10.0f * 1.25f * unit, auraDurationColor(a.remaining));
            duration.align = Align::Center;
            duration.outline = std::max(1.0f, unit);
            draw.text(r.centerX(), r.bottom() + draw.ascent(duration.font, duration.size) + unit,
                      secondsToTimeAbbrev(left, a.remaining), duration);
        }
    }
    return result;
}

AuraTooltipResult auraTooltip(DialogContext& c, const AuraTooltipView& v) {
    VD_PROFILE_ZONE("AuraWindows::tooltip");
    AuraTooltipResult result;
    if (v.name.empty()) {
        return result;
    }
    Gui g{c.ui, c.draw};
    const kit::Metrics m = kit::metrics(c);
    const Rect& safe = c.safeArea;
    const f32 pad = g.px(10.0f), gap = g.px(4.0f);
    const f32 w = std::min(g.px(c.profile == ScreenProfile::Wide ? 270.0f : 290.0f), safe.w - g.px(16.0f));
    const f32 textW = w - 2.0f * pad;
    // GameTooltip's first line (GameTooltipHeaderText, white), the aura's text and its time left
    // (NORMAL_FONT_COLOR).
    const TextStyle title = ui::gameFont(m.heading, ui::kWhite);
    const TextStyle body = ui::gameFont(m.text, ui::kGold);
    const f32 titleH = g.draw.lineHeight(title.font, title.size);
    const f32 lineH = g.draw.lineHeight(body.font, body.size);
    const f32 descriptionH =
        v.description.empty() ? 0.0f : g.draw.measureWrapped(textW, v.description, body.font, body.size);
    const f32 h = 2.0f * pad + titleH + (descriptionH > 0.0f ? gap + descriptionH : 0.0f) +
                  (v.timeLeft.empty() ? 0.0f : gap + lineH);

    const Rect& a = v.anchor;
    const f32 u = g.unit(1.0f);
    f32 x = a.right(), y = a.y - h;              // Right: its bottom left at the button's top right
    if (v.at == AuraTooltipAnchor::BottomLeft) { // its top right at the button's bottom left
        x = a.x - w;
        y = a.bottom();
    } else if (v.at ==
               AuraTooltipAnchor::BottomRight) { // its top left at the button's bottom right + (15, -25)
        x = a.right() + 15.0f * u;
        y = a.bottom() + 25.0f * u;
    }
    const f32 margin = g.px(8.0f);
    x = std::clamp(x, safe.x + margin, std::max(safe.x + margin, safe.right() - margin - w));
    y = std::clamp(y, safe.y + margin, std::max(safe.y + margin, safe.bottom() - margin - h));
    const Rect panel{x, y, w, h};
    result.panel = panel;
    result.tapped = ui::tapped(ui::hit(g, widgetId("aura.tooltip"), panel));
    ui::tooltipPanel(g, panel);

    f32 ty = panel.y + pad;
    f32 typeW = 0.0f;
    if (!v.type.empty()) { // the dispel type at the right of the name
        TextStyle type = ui::gameFont(m.text, ui::kWhite);
        type.align = Align::Right;
        ui::label(g, {panel.x + pad, ty, textW, titleH}, v.type, type);
        typeW = g.draw.measure(v.type, type.font, type.size) + g.px(8.0f);
    }
    ui::label(g, {panel.x + pad, ty, textW - typeW, titleH}, v.name, title);
    ty += titleH;
    if (descriptionH > 0.0f) {
        ty += gap;
        ty += g.draw.wrappedText({panel.x + pad, ty, textW, 1e6f}, v.description, body);
    }
    if (!v.timeLeft.empty()) {
        ty += gap;
        ui::label(g, {panel.x + pad, ty, textW, lineH}, v.timeLeft, body);
    }
    return result;
}

AuraListResult auraList(DialogContext& c, const AuraListView& v, u64 nowMs) {
    VD_PROFILE_ZONE("AuraWindows::list");
    AuraListResult result;
    // MAX_PARTY_TOOLTIP_BUFFS (16) helpful, then MAX_PARTY_TOOLTIP_DEBUFFS (8) harmful ones.
    std::array<AuraButtonPlace, 24> places{};
    u32 buffs = 0, debuffs = 0;
    for (const HudAura& a : v.auras) {
        buffs += !a.harmful && buffs < 16 ? 1u : 0u;
        debuffs += a.harmful && debuffs < 8 ? 1u : 0u;
    }
    const u32 rows = (buffs + 7) / 8 + (debuffs + 7) / 8;
    const u32 columns = std::min(8u, std::max(buffs, debuffs));
    if (rows == 0 || columns == 0) {
        return result;
    }
    Gui g{c.ui, c.draw};
    const f32 k = v.unit;
    Rect panel{v.anchor.x + 47.0f * k, v.anchor.y + 30.0f * k,
               (static_cast<f32>(columns) * 17.0f + 15.0f) * k, (static_cast<f32>(rows) * 17.0f + 15.0f) * k};
    const Rect& safe = c.safeArea;
    panel.x = std::clamp(panel.x, safe.x, std::max(safe.x, safe.right() - panel.w));
    panel.y = std::clamp(panel.y, safe.y, std::max(safe.y, safe.bottom() - panel.h));
    result.panel = panel;
    ui::hit(g, widgetId("aura.list"), panel);
    ui::tooltipPanel(g, panel);
    // $parentBuff1 at TOPLEFT + (9, -9), 17 apart, 8 a row; the debuffs' rows under the buffs'.
    const u32 buffRows = (buffs + 7) / 8;
    u32 count = 0, b = 0, d = 0;
    for (u32 i = 0; i < v.auras.size() && count < places.size(); ++i) {
        const HudAura& a = v.auras[i];
        u32 index = 0, row = 0;
        if (a.harmful) {
            if (d == 8) {
                continue;
            }
            index = d % 8;
            row = buffRows + d / 8;
            ++d;
        } else {
            if (b == 16) {
                continue;
            }
            index = b % 8;
            row = b / 8;
            ++b;
        }
        AuraButtonPlace& p = places[count++];
        p.aura = i;
        p.icon = {panel.x + (9.0f + static_cast<f32>(index) * 17.0f) * k,
                  panel.y + (9.0f + static_cast<f32>(row) * 17.0f) * k, 15.0f * k, 15.0f * k};
        p.hit = p.icon.inset(-1.0f * k);
    }
    const AuraButtonsResult r = auraButtons(c.ui, c.draw, c.resources, v.auras, {places.data(), count}, k,
                                            nowMs, AuraButtonStyle::Small, "aura.list.icon");
    result.tapped = r.tapped >= 0 ? r.tapped : r.held;
    result.rect = r.rect;
    return result;
}

} // namespace vd
