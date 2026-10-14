#include "app/MicroMenu.h"

#include "app/InventoryWindows.h"
#include "app/MinimapView.h"
#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cstdio>

namespace vd {
namespace {

using ui::Rect;
using ui::rgba;
using ui::widgetId;

struct ButtonArt {
    u64 up, down;
    text::Key name, tip;
};
constexpr ButtonArt kArt[kMicroButtons] = {
    {assetPathHash("Interface\\Buttons\\UI-MicroButtonCharacter-Up.blp"),
     assetPathHash("Interface\\Buttons\\UI-MicroButtonCharacter-Down.blp"), "CHARACTER_BUTTON",
     "NEWBIE_TOOLTIP_CHARACTER"},
    {assetPathHash("Interface\\Buttons\\UI-MicroButton-Spellbook-Up.blp"),
     assetPathHash("Interface\\Buttons\\UI-MicroButton-Spellbook-Down.blp"), "SPELLBOOK_ABILITIES_BUTTON",
     "NEWBIE_TOOLTIP_SPELLBOOK"},
    {assetPathHash("Interface\\Buttons\\UI-MicroButton-Talents-Up.blp"),
     assetPathHash("Interface\\Buttons\\UI-MicroButton-Talents-Down.blp"), "TALENTS_BUTTON",
     "NEWBIE_TOOLTIP_TALENTS"},
    {assetPathHash("Interface\\Buttons\\UI-MicroButton-Quest-Up.blp"),
     assetPathHash("Interface\\Buttons\\UI-MicroButton-Quest-Down.blp"), "QUESTLOG_BUTTON",
     "NEWBIE_TOOLTIP_QUESTLOG"},
    {assetPathHash("Interface\\Buttons\\UI-MicroButton-Socials-Up.blp"),
     assetPathHash("Interface\\Buttons\\UI-MicroButton-Socials-Down.blp"), "SOCIAL_BUTTON",
     "NEWBIE_TOOLTIP_SOCIAL"},
    {assetPathHash("Interface\\Buttons\\UI-MicroButton-World-Up.blp"),
     assetPathHash("Interface\\Buttons\\UI-MicroButton-World-Down.blp"), "WORLDMAP_BUTTON",
     "NEWBIE_TOOLTIP_WORLDMAP"},
    {assetPathHash("Interface\\Buttons\\UI-MicroButton-MainMenu-Up.blp"),
     assetPathHash("Interface\\Buttons\\UI-MicroButton-MainMenu-Down.blp"), "MAINMENU_BUTTON",
     "NEWBIE_TOOLTIP_MAINMENU"},
    {assetPathHash("Interface\\Buttons\\UI-MicroButton-Help-Up.blp"), // phase 15 (help)
     assetPathHash("Interface\\Buttons\\UI-MicroButton-Help-Down.blp"), "HELP_BUTTON",
     "NEWBIE_TOOLTIP_HELP"},
};
constexpr u64 kHighlight = assetPathHash("Interface\\Buttons\\UI-MicroButton-Hilight.blp");
constexpr u64 kQuickslot = assetPathHash("Interface\\Buttons\\UI-Quickslot2.blp");
constexpr u64 kQuickslotDown = assetPathHash("Interface\\Buttons\\UI-Quickslot-Depress.blp");
constexpr u64 kCheckHighlight = assetPathHash("Interface\\Buttons\\CheckButtonHilight.blp");
constexpr u64 kBagSlotArt = assetPathHash("Interface\\PaperDoll\\UI-PaperDoll-Slot-Bag.blp");
constexpr u64 kKeyRing = assetPathHash("Interface\\Buttons\\UI-Button-KeyRing.blp");
constexpr u64 kKeyRingDown = assetPathHash("Interface\\Buttons\\UI-Button-KeyRing-Down.blp");
constexpr u64 kKeyRingHighlight = assetPathHash("Interface\\Buttons\\UI-Button-KeyRing-Highlight.blp");
constexpr u64 kGearIcon =
    assetPathHash("Interface\\Icons\\INV_Misc_Gear_01.blp"); // the folded menu (textures.vdpk)
constexpr f32 kToggleDegrees = 215.0f; // its place on the minimap's rim (the backpack's is 180)

// MainMenuBarMicroButton: 29 x 58 units showing the 32 x 64 art, its hit rectangle the lower 40
// (HitRectInsets top 18); each anchored 3 units into the previous one. The bag bar: 37-unit item buttons 5
// apart, the 18 x 39 keyring.
constexpr f32 kButtonW = 29.0f, kButtonH = 58.0f, kButtonStep = 26.0f, kButtonHitTop = 18.0f;
constexpr f32 kVisibleTop = 58.0f * 23.0f / 64.0f; // the art's first opaque row (23 of 64)
constexpr f32 kMicroUnits = static_cast<f32>(kMicroButtons - 1) * kButtonStep + kButtonW;
constexpr f32 kBagSize = 37.0f, kBagGap = 5.0f, kKeyRingW = 18.0f, kKeyRingH = 39.0f;
constexpr f32 kBagUnits = 5.0f * kBagSize + 4.0f * kBagGap + kBagGap + kKeyRingW;
constexpr f32 kRowUnits = kButtonH - kVisibleTop; // the visible height (the bag buttons are as tall)
constexpr u64 kPulseMs = 1000; // SetButtonPulse(button, 60, 1): the highlight flips every second

bool fingerOn(const ui::WidgetState& state) {
    return state.touch && !state.touch->released;
}

// A bag button: the bag's icon (or the empty bag slot), UI-Quickslot2 around it, checked while the bags are
// open.
ui::WidgetState bagButton(ui::Gui& g, UiResources& resources, ui::WidgetId id, const Rect& r, u64 iconHash,
                          bool checked) {
    const ui::WidgetState state = ui::hit(g, id, r);
    const bool down = ui::held(state, r);
    const f32 scale = r.w / 37.0f;
    if (const std::optional<u32> icon = iconHash ? resources.icon(iconHash) : std::nullopt) {
        g.draw.icon(r, *icon, down ? rgba(170, 170, 170) : ui::kWhite, {0.0f, 0.0f, 1.0f, 1.0f});
    } else {
        g.draw.sprite(r, kBagSlotArt);
    }
    g.draw.sprite(
        {r.centerX() - 32.0f * scale, r.centerY() - 32.0f * scale + scale, 64.0f * scale, 64.0f * scale},
        kQuickslot);
    if (down) {
        g.draw.sprite(r, kQuickslotDown);
    }
    if (checked) {
        g.draw.spriteAdditive(r, kCheckHighlight);
    }
    return state;
}

// GameTooltip_AddNewbieTip: the name in white, the newbie tip in gold under it; under `anchor`,
// right-aligned.
void newbieTip(ui::Gui& g, const DialogContext& c, const Rect& anchor, std::string_view name,
               std::string_view tip, std::string_view extra) {
    const f32 pad = g.px(8.0f);
    const f32 w = std::min(g.px(260.0f), c.safeArea.w - g.px(12.0f));
    ui::TextStyle title = ui::gameFont(g.px(14.0f), ui::kWhite);
    ui::TextStyle body = ui::gameFont(g.px(12.5f), ui::kGold);
    const f32 textW = w - 2.0f * pad;
    const f32 titleH = g.draw.measureWrapped(textW, name, title.font, title.size);
    const f32 extraH =
        extra.empty() ? 0.0f : g.draw.measureWrapped(textW, extra, body.font, body.size) + g.px(3.0f);
    const f32 tipH = g.draw.measureWrapped(textW, tip, body.font, body.size);
    const f32 h = titleH + extraH + tipH + 2.0f * pad + g.px(4.0f);
    f32 x = std::clamp(anchor.right() - w, c.safeArea.x + g.px(4.0f), c.safeArea.right() - w - g.px(4.0f));
    f32 y = anchor.bottom() + g.px(6.0f);
    if (y + h > c.safeArea.bottom()) {
        y = std::max(c.safeArea.y, anchor.y - g.px(6.0f) - h);
    }
    const Rect panel{x, y, w, h};
    ui::tooltipPanel(g, panel);
    f32 at = y + pad;
    at += g.draw.wrappedText({x + pad, at, textW, titleH + 1.0f}, name, title) + g.px(4.0f);
    if (!extra.empty()) {
        at += g.draw.wrappedText({x + pad, at, textW, extraH + 1.0f}, extra,
                                 ui::gameFont(body.size, ui::kWhite)) +
              g.px(3.0f);
    }
    g.draw.wrappedText({x + pad, at, textW, tipH + 1.0f}, tip, body);
}

// The bag bar (MainMenuBarBagButtons) with its bottom right at (`right`, `bottom`), `bs` pixels per unit: the
// backpack, the bags left of it, the keyring last; the keyring's tooltip under `tipAnchor`.
void bagBar(ui::Gui& g, DialogContext& c, f32 right, f32 bottom, f32 bs, const MicroMenuView& v, const Rect& tipAnchor,
            MicroMenuResult& result) {
    // Phase 15 (items): each button checked while its own ContainerFrame is open.
    bool perBag = false;
    for (const bool open : v.bagOpen) {
        perBag = perBag || open;
    }
    const auto checked = [&](u32 i) { return perBag ? v.bagOpen[i] : v.bagsOpen; };
    Rect r{right - kBagSize * bs, bottom - kBagSize * bs, kBagSize * bs, kBagSize * bs};
    for (u32 i = 0; i < 5; ++i) {
        result.bagRects[i] = r;
        const ui::WidgetState bag = bagButton(g, c.resources, widgetId("micro.bag", i), r, v.bags[i], checked(i));
        if (ui::tapped(bag)) {
            result.bag = static_cast<i32>(i);
        }
        // Phase 15 (items): held, the backpack (or a bag slot with a bag) opens every bag: their Shift + click.
        if (bag.longPress && (i == 0 || v.bags[i] != 0)) {
            result.bag = static_cast<i32>(i);
            result.bagShift = true;
        }
        if (i == 0) { // the free slots on the backpack, red when none (ours: no hover for its tooltip)
            char count[12];
            std::snprintf(count, sizeof(count), "%u", v.freeSlots);
            ui::TextStyle style =
                ui::numberFont(std::max(g.px(11.0f), r.h * 0.36f), v.freeSlots == 0 ? ui::kRed : ui::kWhite);
            style.align = ui::Align::Right;
            g.draw.text(r.right() - r.w * 0.06f, r.bottom() - r.h * 0.06f, count, style);
        }
        r.x -= (kBagSize + kBagGap) * bs;
    }
    if (v.keyring) {
        const Rect key{r.right() - kKeyRingW * bs, bottom - kKeyRingH * bs, kKeyRingW * bs, kKeyRingH * bs};
        result.bagRects[5] = key;
        const ui::WidgetState ring = ui::hit(g, widgetId("micro.keyring"), key);
        const ui::TexCoords uv{0.0f, 0.0f, 0.5625f, 0.609375f};
        g.draw.sprite(key, ui::held(ring, key) || checked(5) ? kKeyRingDown : kKeyRing, ui::kWhite, uv);
        if (ui::held(ring, key)) {
            g.draw.spriteAdditive(key, kKeyRingHighlight, ui::kWhite, uv);
        }
        if (ui::tapped(ring)) {
            result.bag = 5;
        }
        if (ring.touch && ring.touch->longPressed && fingerOn(ring)) {
            newbieTip(g, c, tipAnchor, text::get("KEYRING"), {}, {});
        }
    }
}

} // namespace

MicroMenuLayout microMenuLayout(const DialogContext& c) {
    const Rect& safe = c.safeArea;
    const f32 dp = c.ui.dp();
    const f32 unit = c.ui.px(ui::kUnitDp);
    const MinimapLayout map = minimapLayout(safe, unit, dp);
    MicroMenuLayout l;
    // The gear on the minimap's rim (round like the battlefield's button); the buttons unfold left of it, level with
    // it, at the original's size for the HUD's unit (smaller when the screen is too narrow for them).
    l.toggle = minimapRimButtonRect(map, kToggleDegrees);
    const f32 gap = 10.0f * dp;
    const f32 right = l.toggle.x - gap, left = safe.x + gap;
    l.scale = std::max(0.0f, std::min(unit, (right - left) / kMicroUnits));
    const f32 width = kMicroUnits * l.scale, height = kRowUnits * l.scale;
    l.strip = {right - width, l.toggle.centerY() - height * 0.5f, width, height};
    // The bag bar unfolded from the rim's backpack: under the minimap cluster, at the right edge, half again the HUD's
    // unit (a finger's width per bag).
    l.bagScale = unit * 1.5f;
    const f32 bagsWidth = kBagUnits * l.bagScale, bagsHeight = kKeyRingH * l.bagScale;
    l.bags = {safe.right() - 4.0f * dp - bagsWidth, map.top + 178.0f * map.s + 4.0f * dp, bagsWidth, bagsHeight};
    l.backpack = backpackButtonRect(c);
    return l;
}

MicroMenuResult microMenu(DialogContext& c, const MicroMenuLayout& l, const MicroMenuView& v,
                          MicroMenuState& state) {
    VD_PROFILE_ZONE("MicroMenu::draw");
    ui::Gui g{c.ui, c.draw};
    MicroMenuResult result;
    const u64 nowMs = c.ui.nowMs();
    const bool talents = v.level >= 10; // UpdateTalentButton
    const bool pulse = talents && v.talentPoints > 0 && !v.pushed[static_cast<u32>(MicroButton::Talents)] &&
                       (nowMs / kPulseMs) % 2 == 0;
    // Each folded row opens for one tap; a finger anywhere else folds it.
    for (const ui::Touch& t : c.ui.touches()) {
        if (!t.began) {
            continue;
        }
        if (!l.strip.contains(t.startX, t.startY) && !l.toggle.contains(t.startX, t.startY)) {
            state.unfolded = false;
        }
        if (!l.bags.contains(t.startX, t.startY) && !l.backpack.contains(t.startX, t.startY)) {
            state.bagsUnfolded = false;
        }
    }
    // The gear: lit while open, and with the talents' pulse.
    const MinimapLayout map = minimapLayout(c.safeArea, c.ui.px(ui::kUnitDp), c.ui.dp());
    if (minimapRimButton(c.ui, c.draw, widgetId("micro.toggle"), map, kToggleDegrees, c.resources.icon(kGearIcon),
                         state.unfolded || pulse)
            .clicked) {
        state.unfolded = !state.unfolded;
    }
    if (state.bagsUnfolded) {
        ui::tooltipPanel(g, l.bags.inset(-g.px(4.0f)), rgba(0, 0, 0, 170));
        bagBar(g, c, l.bags.right(), l.bags.bottom(), l.bagScale, v, l.bags, result);
        if (result.bag >= 0) {
            state.bagsUnfolded = false;
        }
    }
    if (!state.unfolded) {
        return result;
    }
    // The micro buttons right-aligned to the gear (without the talents button below level 10 the quest log takes its
    // place). Phase 20 (retouches): without the performance bar (MainMenuBarPerformanceBarFrame) that ended their row
    // since phase 18.
    const f32 s = l.scale;
    const u32 shown = talents ? kMicroButtons : kMicroButtons - 1;
    const f32 rowWidth = (static_cast<f32>(shown - 1) * kButtonStep + kButtonW) * s;
    const Rect row{l.strip.right() - rowWidth, l.strip.y, rowWidth, l.strip.h};
    ui::tooltipPanel(g, row.inset(-g.px(4.0f)), rgba(0, 0, 0, 170));
    f32 x = row.x;
    const f32 artTop = row.bottom() - kButtonH * s;
    i32 tip = -1;
    for (u32 i = 0; i < kMicroButtons; ++i) {
        if (i == static_cast<u32>(MicroButton::Talents) && !talents) {
            continue;
        }
        const Rect art{x, artTop, kButtonW * s, kButtonH * s};
        // The hit rectangle: the lower 40 units, as wide as the step (the next button's overlap is its own).
        const Rect hit{x, artTop + kButtonHitTop * s, kButtonStep * s,
                       (kButtonH - kButtonHitTop) * s + g.px(4.0f)};
        const ui::WidgetState button = ui::hit(g, widgetId("micro.button", i), hit);
        const bool down = ui::held(button, hit);
        const bool pushed = v.pushed[i] || down;
        g.draw.sprite(art, pushed ? kArt[i].down : kArt[i].up);
        if (i == static_cast<u32>(MicroButton::Character)) {
            // MicroButtonPortrait: 18 x 25 units, 28 down from the top; dimmed and shifted while pushed.
            const Rect face{x + (kButtonW - 18.0f) * 0.5f * s, artTop + 28.0f * s, 18.0f * s, 25.0f * s};
            const ui::TexCoords uv = pushed ? ui::TexCoords{0.2666f, 0.0f, 0.8666f, 0.8333f}
                                            : ui::TexCoords{0.2f, 0.0666f, 0.8f, 0.9f};
            const u32 tint = pushed ? ui::withAlpha(ui::kWhite, 0.5f) : ui::kWhite;
            if (v.portrait.valid()) {
                g.draw.texture(face, v.portrait, tint, uv);
            } else if (v.portraitArt != 0) {
                g.draw.sprite(face, v.portraitArt, tint, uv);
            }
        }
        if (down || (i == static_cast<u32>(MicroButton::Talents) && pulse)) {
            g.draw.spriteAdditive(art, kHighlight);
        }
        if (ui::tapped(button)) {
            result.button = static_cast<i32>(i);
        }
        if (button.touch && button.touch->longPressed && fingerOn(button)) {
            tip = static_cast<i32>(i);
        }
        x += kButtonStep * s;
    }
    if (tip >= 0) {
        const u32 i = static_cast<u32>(tip);
        const text::Key name = i == static_cast<u32>(MicroButton::Spellbook) && !v.spells
                                   ? text::Key("ABILITYBOOK_BUTTON")
                                   : kArt[i].name;
        char points[48] = {};
        if (i == static_cast<u32>(MicroButton::Talents) && v.talentPoints > 0) {
            const std::string_view label = text::get("CHARACTER_POINTS1_COLON");
            std::snprintf(points, sizeof(points), "%.*s %u", static_cast<int>(label.size()), label.data(),
                          v.talentPoints);
        }
        newbieTip(g, c, l.strip, text::get(name), v.newbieTips ? text::get(kArt[i].tip) : std::string_view{}, points);
    }
    if (result.button >= 0) {
        state.unfolded = false; // the window opens and the row folds away
    }
    return result;
}

} // namespace vd
