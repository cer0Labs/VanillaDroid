#include "app/PetWindows.h"

#include "app/DialogKit.h"
#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"

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
using ui::widgetId;
using namespace kit;

constexpr u64 kHappiness = assetPathHash("Interface\\PetPaperDollFrame\\UI-PetHappiness.blp");
constexpr u64 kResistanceIcons =
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-ResistanceIcons.blp");
constexpr u64 kGroupIndicator =
    assetPathHash("Interface\\CharacterFrame\\UI-CharacterFrame-GroupIndicator.blp");
constexpr u64 kStatusBar = assetPathHash("Interface\\TargetingFrame\\UI-StatusBar.blp");
constexpr u64 kExpBarFrame = assetPathHash("Interface\\MainMenuBar\\UI-MainMenuBar-Dwarf.blp");
constexpr u64 kCheckedHighlight = assetPathHash("Interface\\Buttons\\CheckButtonHilight.blp");

constexpr u32 kXpColor = rgba(148, 0, 140); // PetPaperDollFrameExpBar's BarColor (0.58, 0.0, 0.55)
constexpr u32 kRedSlot =
    rgba(255, 26, 26); // PetStable_Update: SetVertexColor(1.0, 0.1, 0.1) on unbought slots
constexpr u32 kRedText = rgba(255, 32, 32);

// PetMagicResFrame1..5's texture coordinates in UI-Character-ResistanceIcons (arcane, fire, nature, frost,
// shadow).
constexpr f32 kResistanceRows[5][2] = {{0.2265625f, 0.33984375f},
                                       {0.0f, 0.11328125f},
                                       {0.11328125f, 0.2265625f},
                                       {0.33984375f, 0.453125f},
                                       {0.453125f, 0.56640625f}};

// The diet's face (PetStablePetInfo / PetPaperDollPetInfo: UI-PetHappiness' first face, 24 x 23) under the
// model's rotate-left button (7 units in), sized as the buttons are (ModelFrame: 35 of the frame's 233
// units).
Rect dietFace(const Gui& g, const Rect& model) {
    const f32 button = std::clamp(model.w * 35.0f / 233.0f, g.px(30.0f), g.px(52.0f));
    const f32 k = button / 35.0f;
    return {model.x + 7.0f * k, model.y + button, 24.0f * k, 23.0f * k};
}

// A small tooltip: a first line (white) and an optional second (gold, wrapped), beside `anchor`, kept on
// screen.
void smallTooltip(DialogContext& c, Gui& g, const Rect& anchor, std::string_view first,
                  std::string_view second) {
    if (first.empty()) {
        return;
    }
    const Metrics m = metrics(c);
    const f32 pad = g.px(8.0f);
    const f32 w = std::min(g.px(280.0f), c.safeArea.w - g.px(16.0f));
    const TextStyle top = ui::gameFont(m.text, ui::kWhite);
    const TextStyle sub = ui::gameFont(m.small, ui::kGold);
    const f32 h1 = g.draw.measureWrapped(w - 2.0f * pad, first, top.font, top.size);
    const f32 h2 = second.empty() ? 0.0f : g.draw.measureWrapped(w - 2.0f * pad, second, sub.font, sub.size);
    const f32 h = h1 + h2 + 2.0f * pad + (second.empty() ? 0.0f : g.px(2.0f));
    f32 x = anchor.right() + g.px(6.0f);
    if (x + w > c.safeArea.right() - g.px(4.0f)) {
        x = anchor.x - w - g.px(6.0f);
    }
    x = std::clamp(x, c.safeArea.x + g.px(4.0f), c.safeArea.right() - w - g.px(4.0f));
    const f32 y = std::clamp(anchor.y, c.safeArea.y + g.px(4.0f), c.safeArea.bottom() - h - g.px(4.0f));
    const Rect panel{x, y, w, h};
    ui::tooltipPanel(g, panel);
    f32 ty = panel.y + pad;
    ty += g.draw.wrappedText({panel.x + pad, ty, w - 2.0f * pad, 1e6f}, first, top);
    if (!second.empty()) {
        g.draw.wrappedText({panel.x + pad, ty + g.px(2.0f), w - 2.0f * pad, 1e6f}, second, sub);
    }
}

// A stable slot (PetStableSlotTemplate: the icon in UI-Quickslot2, UI-EmptySlot behind, CheckButtonHilight
// when checked). Returns its state.
ui::WidgetState stableSlot(DialogContext& c, Gui& g, ui::WidgetId id, const Rect& r,
                           const StableSlotView& s) {
    ui::SlotView slot;
    slot.icon = s.iconHash ? c.resources.icon(s.iconHash) : std::nullopt;
    slot.empty = s.iconHash == 0;
    slot.usable = s.enabled;
    if (!s.purchased) {
        g.draw.solid(r, ui::withAlpha(kRedSlot, 0.35f)); // the red background of a slot not bought
    }
    const ui::WidgetState state = ui::itemSlot(g, id, r, slot);
    if (!s.purchased) {
        g.draw.solid(r, ui::withAlpha(kRedSlot, 0.3f));
    }
    if (s.checked || s.picked) {
        g.draw.spriteAdditive(r, kCheckedHighlight, s.picked ? rgba(255, 255, 160) : ui::kWhite);
    }
    return state;
}

} // namespace

// --- PetStableFrame ----------------------------------------------------------------------------------------

StableResult stableWindow(DialogContext& c, const StableView& v, ModelFrameState& model) {
    VD_PROFILE_ZONE("PetWindows::stable");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    StableResult result;
    const Dialog d = beginDialog(c, g, "stable", wide ? 760.0f : 520.0f, wide ? 420.0f : 680.0f,
                                 text::get("STABLES"), true, false);
    result.closed = d.closed;
    const f32 gap = g.px(8.0f);

    // Wide: the model at the left, the rest at the right; square: the texts, the model, the slots, the
    // purchase.
    Rect info, modelRect, slotsRect;
    const f32 slot = std::min(m.item * 1.1f, g.px(56.0f));
    const f32 textsH = m.text * 1.3f + m.small * 1.4f + gap;
    const f32 slotsH = m.small * 1.6f + slot + gap;
    const f32 buyH = v.showPurchase ? m.small * 1.5f + m.button + gap : 0.0f;
    const f32 moneyH = m.small * 1.8f;
    if (wide) {
        const f32 leftW = std::round(d.body.w * 0.48f);
        modelRect = {d.body.x, d.body.y, leftW, d.body.h};
        info = {modelRect.right() + gap, d.body.y, d.body.right() - modelRect.right() - gap, d.body.h};
        slotsRect = {info.x, info.y + textsH + gap, info.w, slotsH};
    } else {
        info = {d.body.x, d.body.y, d.body.w, d.body.h};
        modelRect = {d.body.x, d.body.y + textsH, d.body.w,
                     std::max(g.px(80.0f), d.body.h - textsH - slotsH - buyH - moneyH - gap)};
        slotsRect = {d.body.x, modelRect.bottom() + gap, d.body.w, slotsH};
    }

    // PetStableLevelText and PetStableLoyaltyText.
    TextStyle level = ui::gameFont(m.text, ui::kGold);
    level.align = Align::Center;
    ui::label(g, {info.x, info.y, info.w, m.text * 1.3f}, v.levelText, level);
    TextStyle loyalty = ui::gameFont(m.small, ui::kGold);
    loyalty.align = Align::Center;
    ui::label(g, {info.x, info.y + m.text * 1.3f, info.w, m.small * 1.4f}, v.loyaltyText, loyalty);

    // PetStableModel and its diet face (PetStablePetInfo).
    g.draw.solid(modelRect, rgba(0, 0, 0, 120));
    bool dietTip = false;
    Rect face;
    if (v.showModel) {
        result.rotated = modelFrame(c, g, widgetId("stable.model"), modelRect, v.model, model);
        face = dietFace(g, modelRect);
        g.draw.sprite(face, kHappiness, ui::kWhite, {0.0f, 0.0f, 0.1875f, 0.359375f});
        const ui::WidgetState s = ui::hit(g, widgetId("stable.diet"), face.inset(-g.px(4.0f)));
        dietTip = s.pressed && !v.dietText.empty(); // the hover: while the finger is on it
    }

    // The slots: "Current Pet:" over the current pet, "Stabled Pets:" over the two stable slots.
    const TextStyle labelStyle = ui::gameFont(m.small, ui::kGold);
    const f32 rowY = slotsRect.y + m.small * 1.6f;
    const f32 step = slot + g.px(14.0f);
    const f32 groupGap = g.px(36.0f);
    const f32 rowW = slot + groupGap + 2.0f * slot + (step - slot);
    const f32 x0 = slotsRect.x + (slotsRect.w - rowW) * 0.5f;
    std::array<Rect, 3> cells{};
    cells[0] = {x0, rowY, slot, slot};
    cells[1] = {x0 + slot + groupGap, rowY, slot, slot};
    cells[2] = {cells[1].x + step, rowY, slot, slot};
    TextStyle centered = labelStyle;
    centered.align = Align::Center;
    ui::label(g, {cells[0].centerX() - g.px(70.0f), slotsRect.y, g.px(140.0f), m.small * 1.4f},
              text::get("CURRENT_PET"), centered);
    ui::label(
        g,
        {cells[1].x - g.px(30.0f), slotsRect.y, cells[2].right() - cells[1].x + g.px(60.0f), m.small * 1.4f},
        text::get("STABLED_PETS"), centered);
    const StableSlotView* views[3] = {&v.current, &v.stabled[0], &v.stabled[1]};
    i32 tipSlot = -1;
    for (u32 i = 0; i < 3; ++i) {
        const StableSlotView& s = *views[i];
        const ui::WidgetState state = stableSlot(c, g, widgetId("stable.slot", i), cells[i], s);
        if (!s.enabled || (!s.purchased && i > 0)) {
            continue; // button:Disable()
        }
        if (ui::tapped(state)) {
            result.click = static_cast<i32>(i);
        }
        if (state.longPress) { // the hover's tooltip, and the pet picked up (PickupStablePet)
            result.pickup = static_cast<i32>(i);
        }
        if (state.pressed && state.touch && state.touch->moved && result.pickup < 0 && !s.picked &&
            s.iconHash != 0) {
            result.pickup = static_cast<i32>(i); // OnDragStart
        }
        if (state.released && state.touch && state.touch->moved) {
            for (u32 j = 0; j < 3; ++j) { // OnReceiveDrag on the slot under the finger
                if (j != i && cells[j].contains(state.touch->x, state.touch->y)) {
                    result.drop = static_cast<i32>(j);
                }
            }
        }
        if (s.picked) {
            tipSlot = static_cast<i32>(i);
        }
        // The dragged pet under the finger.
        if (state.pressed && state.touch && state.touch->moved && s.iconHash != 0) {
            if (const std::optional<u32> icon = c.resources.icon(s.iconHash)) {
                g.draw.icon({state.touch->x - slot * 0.5f, state.touch->y - slot * 0.5f, slot, slot}, *icon,
                            ui::withAlpha(ui::kWhite, 0.8f));
            }
        }
    }

    // The purchase (hidden once both slots are bought): STABLE_SLOT_TEXT, "Cost:" and the price (red when the
    // money does not cover it), Purchase.
    f32 y = slotsRect.bottom() + gap;
    if (v.showPurchase) {
        TextStyle ask = ui::gameFont(m.small, ui::kWhite);
        ask.align = Align::Center;
        ui::label(g, {info.x, y, info.w, m.small * 1.4f}, text::get("STABLE_SLOT_TEXT"), ask);
        y += m.small * 1.5f;
        const std::string_view costLabel = text::get("COSTS_LABEL");
        const TextStyle costStyle = ui::gameFont(m.text, ui::kGold);
        const f32 labelW = g.draw.measure(costLabel, costStyle.font, costStyle.size);
        const f32 buttonW = std::min(buttonWidth(g, text::get("PURCHASE"), m.button), info.w * 0.4f);
        const f32 moneyW = ui::moneyWidth(g.draw, v.nextSlotCost, m.text);
        const f32 rowWidth = labelW + g.px(6.0f) + moneyW + gap * 2.0f + buttonW;
        const f32 rx = info.x + std::max(0.0f, (info.w - rowWidth) * 0.5f);
        ui::label(g, {rx, y, labelW + g.px(2.0f), m.button}, costLabel, costStyle);
        // SetMoneyFrameColor: white when affordable, red otherwise (the coins' numbers). Retouches (windows):
        // the numbers themselves, no box.
        ui::money(g, rx + labelW + g.px(6.0f), y + m.button * 0.5f, v.nextSlotCost, m.text, Align::Left, 1.0f,
                  v.canPurchase ? ui::kWhite : kRedText);
        result.purchase =
            ui::button(g, widgetId("stable.purchase"), {rx + rowWidth - buttonW, y, buttonW, m.button},
                       text::get("PURCHASE"), v.canPurchase);
    }
    // PetStableMoneyFrame: the player's money, bottom right.
    ui::money(g, d.body.right() - g.px(6.0f), d.body.bottom() - moneyH * 0.5f, v.money, m.text, Align::Right);

    // Tooltips over everything: the picked slot's (name, "Level 12 Wolf"), the diet's.
    if (tipSlot >= 0) {
        const StableSlotView& s = *views[tipSlot];
        smallTooltip(c, g, cells[static_cast<u32>(tipSlot)], s.tooltip, s.tooltipSub);
    } else if (dietTip) {
        smallTooltip(c, g, face, v.dietText, {});
    }
    return result;
}

// --- PetPaperDollFrame -------------------------------------------------------------------------------------

namespace {

// AttributeFrameTemplate / StatFrameTemplate: the label at the left, the value at the right, on a dark well.
// Returns true when tapped (its tooltip toggles).
bool statLine(DialogContext& c, Gui& g, ui::WidgetId id, const Rect& r, const PetStatView& s, bool open) {
    const Metrics m = metrics(c);
    const ui::WidgetState state = ui::hit(g, id, r);
    if (open || ui::held(state, r)) {
        g.draw.solid(r, rgba(255, 255, 255, 30));
    }
    ui::label(g, {r.x + g.px(6.0f), r.y, r.w * 0.6f, r.h}, s.label, ui::gameFont(m.small, ui::kGold));
    TextStyle value = ui::gameFont(m.small, s.color);
    value.align = Align::Right;
    ui::label(g, {r.x + r.w * 0.35f, r.y, r.w * 0.65f - g.px(6.0f), r.h}, s.value, value);
    return ui::tapped(state);
}

} // namespace

PetPaperDollResult petPaperDoll(DialogContext& c, const Rect& body, const PetPaperDollView& v,
                                PetPaperDollState& state) {
    VD_PROFILE_ZONE("PetWindows::paperDoll");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    PetPaperDollResult result;
    const f32 gap = g.px(8.0f);
    const bool wide = body.w >= body.h * 1.25f;

    // PetNameText (the window keeps the character's name in its title), PetLevelText, PetLoyaltyText.
    const f32 headH = m.text * 1.3f + m.small * 1.3f + (v.loyaltyText.empty() ? 0.0f : m.small * 1.3f);
    const Rect head =
        wide ? Rect{body.x, body.y, std::round(body.w * 0.42f), headH} : Rect{body.x, body.y, body.w, headH};
    TextStyle name = ui::gameFont(m.text, ui::kWhite);
    name.align = Align::Center;
    ui::label(g, {head.x, head.y, head.w, m.text * 1.3f}, v.name, name);
    TextStyle small = ui::gameFont(m.small, ui::kGold);
    small.align = Align::Center;
    ui::label(g, {head.x, head.y + m.text * 1.3f, head.w, m.small * 1.3f}, v.levelText, small);
    if (!v.loyaltyText.empty()) {
        ui::label(g, {head.x, head.y + m.text * 1.3f + m.small * 1.3f, head.w, m.small * 1.3f}, v.loyaltyText,
                  small);
    }

    // The bottom: training points (hunters) and the experience bar (PetPaperDollFrameExpBar, 319 x 11).
    const f32 barH = std::max(g.px(12.0f), m.small * 0.9f);
    const f32 trainH = v.hunter ? m.small * 1.5f : 0.0f;
    const f32 statRow = m.small * 1.45f;
    const f32 statsH = 5.0f * statRow + g.px(8.0f);
    Rect model, resist, stats, train, bar;
    const f32 resistW = std::min(g.px(44.0f), body.w * 0.12f);
    if (wide) {
        model = {body.x, head.bottom() + g.px(2.0f), head.w, body.bottom() - head.bottom() - g.px(2.0f)};
        const Rect right{model.right() + gap, body.y, body.right() - model.right() - gap, body.h};
        bar = {right.x + g.px(4.0f), right.bottom() - barH - g.px(4.0f), right.w - g.px(8.0f), barH};
        train = {right.x, bar.y - gap - trainH, right.w, trainH};
        stats = {right.x, train.y - g.px(4.0f) - statsH, right.w, statsH};
        resist = {right.x, right.y, right.w, std::max(g.px(40.0f), stats.y - gap - right.y)};
    } else {
        bar = {body.x + g.px(6.0f), body.bottom() - barH - g.px(4.0f), body.w - g.px(12.0f), barH};
        train = {body.x, bar.y - gap - trainH, body.w, trainH};
        stats = {body.x, train.y - g.px(4.0f) - statsH, body.w, statsH};
        model = {body.x, head.bottom() + g.px(2.0f), body.w - resistW - gap, stats.y - gap - head.bottom()};
        resist = {model.right() + gap, model.y, resistW, model.h};
    }

    // PetModelFrame, and the diet's face for a hunter's pet (PetPaperDollPetInfo).
    g.draw.solid(model, rgba(0, 0, 0, 90));
    result.rotated = modelFrame(c, g, widgetId("petPaperDoll.model"), model, v.model, state.model);
    std::array<Rect, 17> anchors{};
    if (v.hunter) {
        const Rect face = dietFace(g, model);
        g.draw.sprite(face, kHappiness, ui::kWhite, {0.0f, 0.0f, 0.1875f, 0.359375f});
        anchors[15] = face;
        if (ui::tapped(ui::hit(g, widgetId("petPaperDoll.diet"), face.inset(-g.px(4.0f))))) {
            state.tip = state.tip == 15 ? -1 : 15;
        }
    }

    // PetResistanceFrame: the five icons with their values (a column beside the model; a row in the wide
    // layout).
    {
        const bool row = wide;
        const f32 cell = row ? std::min(resist.w / 5.0f - g.px(4.0f), g.px(40.0f))
                             : std::min(resist.w, std::min(g.px(40.0f), resist.h / 5.0f - g.px(4.0f)));
        const f32 iconH = cell * 29.0f / 32.0f;
        for (u32 i = 0; i < 5; ++i) {
            const Rect r =
                row ? Rect{resist.x + static_cast<f32>(i) * (cell + g.px(4.0f)), resist.y, cell, iconH}
                    : Rect{resist.x + (resist.w - cell) * 0.5f,
                           resist.y + static_cast<f32>(i) * (iconH + g.px(4.0f)), cell, iconH};
            g.draw.sprite(r, kResistanceIcons, ui::kWhite,
                          {0.0f, kResistanceRows[i][0], 1.0f, kResistanceRows[i][1]});
            TextStyle value = ui::numberFont(std::max(g.px(11.0f), iconH * 0.42f), v.resistances[i].color);
            value.align = Align::Center;
            value.outline = std::max(1.0f, g.px(1.0f));
            g.draw.text(r.centerX(), r.bottom() - g.px(3.0f), v.resistances[i].value, value);
            anchors[10 + i] = r;
            if (ui::tapped(ui::hit(g, widgetId("petPaperDoll.resist", i), r))) {
                state.tip = state.tip == static_cast<i32>(10 + i) ? -1 : static_cast<i32>(10 + i);
            }
        }
    }

    // PetAttributesFrame: the five attributes at the left, attack, power, damage, defense and armor at the
    // right.
    g.draw.solid(stats, rgba(0, 0, 0, 110));
    const f32 colW = (stats.w - gap) * 0.5f;
    for (u32 i = 0; i < 5; ++i) {
        const Rect left{stats.x + g.px(2.0f), stats.y + g.px(4.0f) + static_cast<f32>(i) * statRow,
                        colW - g.px(2.0f), statRow};
        const Rect right{stats.x + colW + gap, left.y, colW - g.px(2.0f), statRow};
        anchors[i] = left;
        anchors[5 + i] = right;
        if (statLine(c, g, widgetId("petPaperDoll.stat", i), left, v.stats[i],
                     state.tip == static_cast<i32>(i))) {
            state.tip = state.tip == static_cast<i32>(i) ? -1 : static_cast<i32>(i);
        }
        if (statLine(c, g, widgetId("petPaperDoll.stat", 5 + i), right, v.combat[i],
                     state.tip == static_cast<i32>(5 + i))) {
            state.tip = state.tip == static_cast<i32>(5 + i) ? -1 : static_cast<i32>(5 + i);
        }
    }

    // PetTrainingPointLabel / Text.
    if (v.hunter) {
        const std::string_view label = text::get("TRAINING_POINTS");
        const TextStyle labelStyle = ui::gameFont(m.small, ui::kGold);
        const f32 labelW = g.draw.measure(label, labelStyle.font, labelStyle.size);
        const f32 x = train.right() - g.px(70.0f) - labelW;
        ui::label(g, {x, train.y, labelW + g.px(4.0f), train.h}, label, labelStyle);
        ui::label(g, {x + labelW + g.px(8.0f), train.y, g.px(60.0f), train.h}, v.trainingText,
                  ui::gameFont(m.small, ui::kWhite));
    }

    // The experience bar: purple, the segmented frame over it, "XP value / max" while the tooltip is asked.
    g.draw.solid(bar, rgba(0, 0, 0, 140));
    if (v.nextXp > 0) {
        const f32 fill = std::clamp(static_cast<f32>(v.xp) / static_cast<f32>(v.nextXp), 0.0f, 1.0f);
        g.draw.sprite({bar.x, bar.y, bar.w * fill, bar.h}, kStatusBar, kXpColor, {0.0f, 0.0f, fill, 1.0f});
    }
    const f32 frameH = bar.h * 13.0f / 11.0f;
    for (u32 i = 0; i < 2; ++i) {
        g.draw.sprite({bar.x + bar.w * 0.5f * static_cast<f32>(i), bar.y - (frameH - bar.h) * 0.5f,
                       bar.w * 0.5f, frameH},
                      kExpBarFrame, ui::kWhite, {0.203125f, 0.2890625f, 0.8046875f, 0.33984375f});
    }
    anchors[16] = bar;
    if (ui::tapped(ui::hit(g, widgetId("petPaperDoll.xp"), bar.inset(0.0f, -g.px(6.0f))))) {
        state.tip = state.tip == 16 ? -1 : 16;
    }
    if (state.tip == 16) { // SetTextStatusBarTextPrefix(XP): "XP 120 / 1400"
        char line[64];
        const std::string_view prefix = text::get("XP");
        std::snprintf(line, sizeof(line), "%.*s %u / %u", static_cast<int>(prefix.size()), prefix.data(),
                      v.xp, v.nextXp);
        TextStyle style = ui::numberFont(std::max(bar.h * 1.05f, g.px(12.0f)), ui::kWhite);
        style.align = Align::Center;
        g.draw.text(bar.centerX(), ui::centeredBaseline(g.draw, style, bar.centerY()), line, style);
    }

    // The tooltip a tap opened (the hover of the stat frames' OnEnter).
    if (state.tip >= 0 && state.tip < 16) {
        const u32 t = static_cast<u32>(state.tip);
        const Rect& anchor = anchors[t];
        if (t < 5) {
            smallTooltip(c, g, anchor, v.stats[t].tooltip, v.stats[t].tooltipSub);
        } else if (t < 10) {
            smallTooltip(c, g, anchor, v.combat[t - 5].tooltip, v.combat[t - 5].tooltipSub);
        } else if (t < 15) {
            smallTooltip(c, g, anchor, v.resistances[t - 10].tooltip, {});
        } else if (!v.dietText.empty()) {
            smallTooltip(c, g, anchor, v.dietText, {});
        }
    }
    return result;
}

// --- Unit frame pieces -------------------------------------------------------------------------------------

void groupIndicator(Gui& g, const Rect& playerFrame, u32 subgroup) {
    if (subgroup == 0) {
        return;
    }
    // PlayerFrameGroupIndicator: BOTTOMLEFT at the frame's TOPLEFT + (97, -20), 16 high, its width the text's
    // + 40; the left and right caps 24 wide, the middle stretched; the text 20 in (GameFontHighlightSmall).
    const f32 s = playerFrame.w / 232.0f;
    char line[48];
    const std::string_view group = text::get("GROUP");
    std::snprintf(line, sizeof(line), "%.*s %u", static_cast<int>(group.size()), group.data(), subgroup);
    TextStyle style = ui::gameFont(10.0f * s * 1.2f, ui::withAlpha(ui::kWhite, 0.7f));
    const f32 textW = g.draw.measure(line, style.font, style.size);
    const f32 w = textW + 40.0f * s;
    const Rect r{playerFrame.x + 97.0f * s, playerFrame.y + 4.0f * s, w, 16.0f * s};
    const u32 art = ui::withAlpha(ui::kWhite, 0.3f);
    g.draw.sprite({r.x, r.y, 24.0f * s, r.h}, kGroupIndicator, art, {0.0f, 0.0f, 0.1875f, 1.0f});
    g.draw.sprite({r.right() - 24.0f * s, r.y, 24.0f * s, r.h}, kGroupIndicator, art,
                  {0.53125f, 0.0f, 0.71875f, 1.0f});
    if (r.w > 48.0f * s) {
        g.draw.sprite({r.x + 24.0f * s, r.y, r.w - 48.0f * s, r.h}, kGroupIndicator, art,
                      {0.1875f, 0.0f, 0.53125f, 1.0f});
    }
    style.shadow = std::max(1.0f, s);
    g.draw.text(r.x + 20.0f * s, ui::centeredBaseline(g.draw, style, r.centerY() + 2.0f * s), line, style);
}

std::string_view feedbackWord(game::UnitCombatEvent event) {
    using E = game::UnitCombatEvent;
    switch (event) {
    case E::Interrupt: return text::get("INTERRUPT");
    case E::Miss: return text::get("MISS");
    case E::Resist: return text::get("RESIST");
    case E::Dodge: return text::get("DODGE");
    case E::Parry: return text::get("PARRY");
    case E::Block: return text::get("BLOCK");
    case E::Evade: return text::get("EVADE");
    case E::Immune: return text::get("IMMUNE");
    case E::Deflect: return text::get("DEFLECT");
    case E::Absorb: return text::get("ABSORB");
    case E::Reflect: return text::get("REFLECT");
    default: return {};
    }
}

void hitIndicator(Gui& g, f32 x, f32 y, f32 unit, const game::FeedbackView& v) {
    if (!v.shown || v.alpha <= 0.0f) {
        return;
    }
    char number[16];
    std::string_view line;
    if (v.word != game::UnitCombatEvent::None) {
        line = feedbackWord(v.word);
    } else {
        std::snprintf(number, sizeof(number), "%d", v.amount);
        line = number;
    }
    const u32 color = v.color == game::FeedbackColor::Yellow  ? rgba(255, 255, 0)
                      : v.color == game::FeedbackColor::Green ? rgba(0, 255, 0)
                                                              : ui::kWhite;
    // NumberFontNormalHuge: a thick black outline.
    TextStyle style = ui::numberFont(v.height * unit, ui::withAlpha(color, v.alpha));
    style.align = Align::Center;
    style.outline = std::max(1.0f, 2.0f * unit);
    g.draw.text(x, ui::centeredBaseline(g.draw, style, y), line, style);
}

} // namespace vd
