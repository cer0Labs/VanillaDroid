#include "app/TouchRoundIcon.h"

#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/DrawList.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

namespace vd {
namespace {

using ui::rgba;
using ui::withAlpha;

// ActionButtonTemplate's art.
constexpr u64 kBorder = assetPathHash("Interface\\Buttons\\UI-ActionButton-Border.blp");  // $parentBorder, ADD
constexpr u64 kChecked = assetPathHash("Interface\\Buttons\\CheckButtonHilight.blp");     // CheckedTexture, ADD
constexpr u64 kFlashPeriodMs = 400;  // ATTACK_BUTTON_FLASH_TIME
constexpr u64 kPulsePeriodMs = 900;  // the current action's gold ring
constexpr f32 kPi = 3.14159265f;
// ActionButton_UpdateUsable: short of power 0.5 0.5 1.0, unusable 0.4 0.4 0.4; out of range: ours, the red the client
// gives the hotkey (ActionButton_OnUpdate), on the icon (the cluster has no hotkeys).
constexpr u32 kNoManaTint = rgba(128, 128, 255);
constexpr u32 kUnusableTint = rgba(102, 102, 102);
constexpr u32 kOutOfRangeTint = rgba(204, 26, 26);
// UI-ActionButton-Border on an equipped item (ActionButton_Update: SetVertexColor(0, 1.0, 0, 0.35)).
constexpr u32 kEquippedColor = rgba(0, 255, 0, 89);
constexpr f32 kGlobalCooldownMax = 1.6f; // seconds: a cooldown this short is the global one (a thin ring)

constexpr u32 iconCircleParams(u32 slot, bool additive) {
    // UiMode::IconCircle with its circle at 0.5 of the texture (bits 16-23: 255), bit 24 additive.
    return (slot & 4095u) | (static_cast<u32>(UiMode::IconCircle) << 12) | (255u << 16) | (additive ? 1u << 24 : 0u);
}

} // namespace

void primeRoundIconTextures(UiResources& resources) {
    for (const u64 texture : {kBorder, kChecked}) {
        resources.icon(texture);
    }
}

void roundTexture(ui::DrawList& draw, UiResources& resources, u64 texture, f32 cx, f32 cy, f32 r, u32 color,
                  bool additive) {
    const std::optional<u32> slot = resources.icon(texture);
    if (!slot || r <= 0.0f) {
        return;
    }
    const u32 p = iconCircleParams(*slot, additive);
    const UiVertex quad[4] = {{cx - r, cy - r, 0.0f, 0.0f, color, p},
                              {cx + r, cy - r, 1.0f, 0.0f, color, p},
                              {cx + r, cy + r, 1.0f, 1.0f, color, p},
                              {cx - r, cy + r, 0.0f, 1.0f, color, p}};
    draw.customQuad(quad);
}

void drawRoundIconStates(ui::DrawList& draw, UiResources& resources, f32 cx, f32 cy, f32 r, const HudActionSlot& slot,
                         const RoundIconState& state, u64 nowMs) {
    const f32 a = state.alpha;
    // The cooldown's dark sweep, over the icon and under the button's textures.
    const bool cooling = slot.cooldownLeft > 0.0f && slot.cooldownTotal > 0.0f;
    const f32 left = cooling ? std::clamp(slot.cooldownLeft / slot.cooldownTotal, 0.0f, 1.0f) : 0.0f;
    const bool global = slot.cooldownTotal <= kGlobalCooldownMax;
    if (cooling && !global) {
        draw.ring(cx, cy, r, 0.0f, withAlpha(rgba(0, 0, 0), 0.65f * a), left);
    }
    // The auto attack and auto-repeat: red over the icon, on and off every 0.4 s (ActionButton_UpdateFlash's rhythm).
    if ((slot.flashing || slot.active) && (nowMs / kFlashPeriodMs) % 2 == 0) {
        draw.ring(cx, cy, r, 0.0f, withAlpha(rgba(255, 30, 30), 0.35f * a), 1.0f, true);
    }
    // $parentBorder (OVERLAY): an equipped item (IsEquippedAction): 62 units round the 36-unit button, its glow beyond
    // the icon's edge (in its own circle).
    if (slot.equipped) {
        roundTexture(draw, resources, kBorder, cx, cy, r * (62.0f / 36.0f), withAlpha(kEquippedColor, a), true);
    }
    // The edit mode's chosen place: the checked texture.
    if (state.selected) {
        roundTexture(draw, resources, kChecked, cx, cy, r, withAlpha(ui::kWhite, a), true);
    }
    // The current action (ActionButton_UpdateState: casting, auto-repeating, the next swing queued, the form's aura): a
    // gold ring pulsing round the button.
    if (slot.checked || slot.active) {
        const f32 pulse = 0.5f + 0.5f * std::sin(static_cast<f32>(nowMs % kPulsePeriodMs) *
                                                 (2.0f * kPi / static_cast<f32>(kPulsePeriodMs)));
        draw.ring(cx, cy, r * 1.08f, 0.82f, withAlpha(rgba(255, 220, 120), (0.45f + 0.4f * pulse) * a), 1.0f, true);
    }
    // Over everything: the global cooldown's thin ring closing, or the cooldown's seconds (minutes from one).
    if (cooling && global) {
        draw.ring(cx, cy, r * 1.04f, 0.82f, withAlpha(rgba(255, 240, 200), 0.85f * a), left);
    } else if (cooling && slot.cooldownLeft >= 1.5f) {
        char seconds[16];
        if (slot.cooldownLeft >= 60.0f) {
            text::format(seconds, "MINUTE_ONELETTER_ABBR", static_cast<int>(slot.cooldownLeft / 60.0f + 0.5f));
        } else {
            std::snprintf(seconds, sizeof(seconds), "%d", static_cast<int>(slot.cooldownLeft + 0.5f));
        }
        ui::TextStyle style;
        style.size = r * 0.7f;
        style.color = withAlpha(rgba(255, 255, 160), a);
        style.outline = std::max(1.0f, r * 0.05f);
        style.align = ui::Align::Center;
        draw.text(cx, cy + style.size * 0.35f, seconds, style);
    }
}

void drawRoundIcon(ui::DrawList& draw, UiResources& resources, f32 cx, f32 cy, f32 r, const HudActionSlot& slot,
                   const RoundIconState& state, u64 nowMs) {
    VD_PROFILE_ZONE("TouchRoundIcon::draw");
    if (slot.iconHash != 0) {
        if (const std::optional<u32> icon = resources.icon(slot.iconHash)) {
            const bool unusable = !slot.usable && !slot.noMana;
            const u32 tint = unusable        ? kUnusableTint
                             : !slot.inRange ? kOutOfRangeTint
                             : slot.noMana   ? kNoManaTint
                                             : ui::kWhite;
            draw.iconCircle({cx - r, cy - r, 2.0f * r, 2.0f * r}, *icon, withAlpha(tint, state.alpha));
        }
    }
    drawRoundIconStates(draw, resources, cx, cy, r, slot, state, nowMs);
}

} // namespace vd
