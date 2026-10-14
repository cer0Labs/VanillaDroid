#include "app/ModelFrame.h"

#include "core/Hash.h"
#include "core/Profile.h"
#include "render/Renderer.h"

#include <algorithm>
#include <numbers>

namespace vd {
namespace {

using ui::Rect;

constexpr u64 kLeftUp = assetPathHash("Interface\\Buttons\\UI-RotationLeft-Button-Up.blp");
constexpr u64 kLeftDown = assetPathHash("Interface\\Buttons\\UI-RotationLeft-Button-Down.blp");
constexpr u64 kRightUp = assetPathHash("Interface\\Buttons\\UI-RotationRight-Button-Up.blp");
constexpr u64 kRightDown = assetPathHash("Interface\\Buttons\\UI-RotationRight-Button-Down.blp");
constexpr u64 kHighlight = assetPathHash("Interface\\Buttons\\ButtonHilight-Round.blp");

// UIParent.lua: ROTATIONS_PER_SECOND.
constexpr f32 kRotationsPerSecond = 0.5f;
constexpr f32 kTwoPi = 2.0f * std::numbers::pi_v<f32>;
// CharacterModelFrame's width (PaperDollFrame.xml) and its rotate buttons' size, in the client's units.
constexpr f32 kFrameWidth = 233.0f;
constexpr f32 kButtonSize = 35.0f;
// Touch: a drag across the whole frame turns the model this many times.
constexpr f32 kTurnsPerFrameWidth = 0.75f;

f32 wrap(f32 facing) {
    facing = std::fmod(facing, kTwoPi);
    return facing < 0.0f ? facing + kTwoPi : facing;
}

} // namespace

bool modelFrame(DialogContext& c, ui::Gui& g, ui::WidgetId id, const Rect& area, TextureHandle image,
                ModelFrameState& state, f32 keep, bool buttons) {
    VD_PROFILE_ZONE("ModelFrame::draw");
    const u64 now = c.ui.nowMs();
    const f32 dt = state.lastMs != 0 && now > state.lastMs ? std::min(static_cast<f32>(now - state.lastMs) * 0.001f, 0.1f)
                                                           : 0.0f;
    state.lastMs = now;

    // The image keeps its aspect, centered, standing on the frame's floor; in a narrow area its sides go first, down to
    // `keep` of its width.
    const f32 imageAspect = static_cast<f32>(kModelViewWidth) / static_cast<f32>(kModelViewHeight);
    const f32 share = area.w < area.h * imageAspect && area.h > 0.0f
                          ? std::clamp(area.w / (area.h * imageAspect), std::min(keep, 1.0f), 1.0f)
                          : 1.0f;
    const f32 aspect = imageAspect * share;
    Rect picture = area;
    if (area.w > area.h * aspect) {
        picture.w = area.h * aspect;
        picture.x = area.x + (area.w - picture.w) * 0.5f;
    } else {
        picture.h = area.w / aspect;
        picture.y = area.bottom() - picture.h;
    }
    if (image.valid()) {
        g.draw.texture(picture, image, ui::kWhite, {(1.0f - share) * 0.5f, 0.0f, (1.0f + share) * 0.5f, 1.0f});
    }

    // Touch adaptation: a finger dragged across the model turns it (the buttons, declared after, stay on top).
    const ui::WidgetState drag = ui::hit(g, id, area);
    if (drag.pressed && drag.touch && drag.touch->moved && area.w > 1.0f) {
        state.facing -= drag.touch->dx / area.w * kTurnsPerFrameWidth * kTwoPi; // its front follows the finger
    }

    if (!buttons) {
        state.facing = wrap(state.facing);
        return false;
    }
    // ModelFrames.lua's buttons at the frame's top left, sized with the frame (35 of its 233 units).
    const f32 size = std::clamp(area.w * kButtonSize / kFrameWidth, g.px(30.0f), g.px(52.0f));
    const Rect left{area.x, area.y, size, size};
    const Rect right{area.x + size, area.y, size, size};
    const ui::WidgetState l = ui::hit(g, ui::widgetId("modelFrame.left", id), left);
    const ui::WidgetState r = ui::hit(g, ui::widgetId("modelFrame.right", id), right);
    const bool leftDown = ui::held(l, left), rightDown = ui::held(r, right);
    g.draw.sprite(left, leftDown ? kLeftDown : kLeftUp);
    g.draw.sprite(right, rightDown ? kRightDown : kRightUp);
    if (leftDown) {
        g.draw.spriteAdditive(left, kHighlight);
    }
    if (rightDown) {
        g.draw.spriteAdditive(right, kHighlight);
    }
    // Model_OnUpdate: the left button held increases the rotation, the right one decreases it.
    const f32 step = dt * kTwoPi * kRotationsPerSecond;
    state.facing += (leftDown ? step : 0.0f) - (rightDown ? step : 0.0f);
    state.facing = wrap(state.facing);
    const bool began = (l.touch && l.touch->began) || (r.touch && r.touch->began);
    return began;
}

} // namespace vd
