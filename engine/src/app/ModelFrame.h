#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"
#include "ui/Widgets.h"

namespace vd {

// Phase 15: Model_OnLoad's starting rotation of the client's model frames (radians).
constexpr f32 kModelFrameDefaultFacing = 0.61f;

// What a model frame keeps between frames (its window's Screens object owns it): the model's rotation and the time
// of the last frame it was drawn (the rotate buttons turn by time).
struct ModelFrameState {
    f32 facing = kModelFrameDefaultFacing;
    u64 lastMs = 0;
};

// A model frame of the interface (PlayerModel / DressUpModel / TabardModel): `image` (app/ModelViews::show) fitted in
// `area` keeping its aspect, centered and bottom aligned (the model stands on the frame's floor), with
// ModelFrames.lua's two rotate buttons (UI-RotationLeft / UI-RotationRight, 35 x 35 at the frame's top left as in
// PaperDollFrame.xml): held, they turn the model at ROTATIONS_PER_SECOND (Model_OnUpdate: left increases the
// rotation). Touch adaptation: dragging a finger across the model turns it too. Updates `state.facing`; returns true
// when a rotate button went down this frame (Model_RotateLeft / Right play igInventoryRotateCharacter: the caller
// plays UiSound::InventoryRotate). Nothing is drawn but the buttons while `image` is invalid (loading). Retouches
// (character frame): in an area narrower than the image, its empty sides may be cut down to `keep` of its width (the
// model stays whole in the middle) so that it fills more of the area's height; `buttons` false leaves the rotate
// buttons out (the character frame: the finger turns it).
bool modelFrame(DialogContext& c, ui::Gui& g, ui::WidgetId id, const ui::Rect& area, TextureHandle image,
                ModelFrameState& state, f32 keep = 1.0f, bool buttons = true);

} // namespace vd
