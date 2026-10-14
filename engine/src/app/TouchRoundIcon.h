#pragma once

#include "app/HudActionSlot.h"
#include "core/Types.h"

namespace vd {

namespace ui {
class DrawList;
}
class UiResources;

// The touch cluster's round button: the action's icon in its circle, without a rim or a socket, tinted as 1.12's
// ActionButton does (ActionButton.lua: unusable grey, out of range red, short of power blue), with an equipped item's
// border (UI-ActionButton-Border, green, ADD) 62 units round the 36-unit button as 1.12 places it, in its own circle.
// The rest is ours, clearer under a thumb than 1.12's textures and Cooldown model: the button shrinks while a finger is
// on it (kRoundIconPressed, applied by the caller), a gold ring pulses round the current action, the auto attack /
// auto-repeat blinks red over the icon, and the cooldown is a dark sweep with its seconds (the global cooldown a thin
// ring closing). Shared by the cluster (app/HudTouchBar, its edit mode app/HudTouchEdit) and the pet's button and panel
// (app/TouchPetWindows).
constexpr f32 kRoundIconPressed = 0.92f; // the radius while a finger is on the button

struct RoundIconState {
    bool selected = false; // the edit mode's chosen place: the checked texture (CheckButtonHilight)
    f32 alpha = 1.0f;      // the cluster dimmed (no control), an inherited slot in the edit mode...
};

// The 1.12 textures a round icon draws over itself, by the icon cache (64 x 64 each): asked for once per frame while
// the cluster shows, so that the first press already has its art.
void primeRoundIconTextures(UiResources& resources);

// The action's state over the circle (cx, cy, r): its marks and its cooldown.
void drawRoundIconStates(ui::DrawList& draw, UiResources& resources, f32 cx, f32 cy, f32 r, const HudActionSlot& slot,
                         const RoundIconState& state, u64 nowMs);

// The icon itself, tinted as ActionButton_UpdateUsable / ActionButton_OnUpdate do, then its states.
void drawRoundIcon(ui::DrawList& draw, UiResources& resources, f32 cx, f32 cy, f32 r, const HudActionSlot& slot,
                   const RoundIconState& state, u64 nowMs);

// A 64 x 64 client texture of the icon cache over the circle's bounding square, clipped to the circle (antialiased);
// `additive` for the client's alphaMode="ADD".
void roundTexture(ui::DrawList& draw, UiResources& resources, u64 texture, f32 cx, f32 cy, f32 r, u32 color,
                  bool additive);

} // namespace vd
