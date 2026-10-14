#pragma once

#include "core/Types.h"

namespace vd::pack {

// Phase 18: the glue screens' scenes (Interface\Glues\Models\UI_*.m2: AccountLogin's Dark Portal, the races' places of
// CharacterSelect and CharacterCreate), in cinematics.vdpk under their original paths (".m2"); the models themselves
// are actors (actors.vdpk). The GlueXML shows each with SetSequence(0) and SetCamera(0): its first camera over its
// first sequence, and the model's own lights. Model space, Z up.
//
// Layout: GlueSceneHeader, its lights, then the camera (a whole pack::CinematicCameraHeader blob with its keys, its
// offsets relative to its own start), 16-byte aligned.

constexpr u32 kGlueFormatVersion = 1;

// A light of the model (M2Light) as it stands at the first frame of the first sequence.
struct GlueLight {
    u32 type;        // 0 directional, 1 point (formats::kM2Light*)
    f32 position[3]; // model space at the first frame (its bone's pivot moved by the bone)
    f32 direction[3]; // directional: towards the light, model space (its bone's rotation of +Z)
    f32 ambient[3];  // color x intensity
    f32 diffuse[3];  // color x intensity
    f32 attenuationStart; // point: yards
    f32 attenuationEnd;
    u32 visible; // the visibility track's first value
};
static_assert(sizeof(GlueLight) == 64, "GlueLight layout");

// GlueSceneHeader::flags.
constexpr u32 kGlueHasStand = 1u << 0; // the model has attachment 0: where the character stands

struct GlueSceneHeader {
    u32 version; // kGlueFormatVersion
    u32 lightCount;
    u32 lightsOffset;
    u32 cameraOffset; // pack::CinematicCameraHeader blob (0: the model has no camera)
    u32 cameraSize;
    u32 sequenceMs; // the first sequence's length (its loop)
    f32 boundsRadius;
    u32 flags; // kGlue*
    // The race scenes' attachment 0 at the first frame (model space): the character's feet in CharacterSelect and
    // CharacterCreate, a few yards in front of the camera; standFacing: its bone's +X about Z (radians).
    f32 stand[3];
    f32 standFacing;
};
static_assert(sizeof(GlueSceneHeader) == 48, "GlueSceneHeader layout");

} // namespace vd::pack
