#pragma once

#include "core/Types.h"

namespace vd::pack {

// Retouches (ui models): the models the 1.12 interface draws in its Model frames, baked into ui.vdpk for the 2D UI
// (ui/UiModels) instead of rendering a 3D scene per button:
//   "ui/models"  UiModelsHeader, then the arrays it lists (8-byte aligned): UiModelInfo[models] in UiModelId order,
//                their sequences, bones, triangles' vertices, batches, particle emitters, the animation tracks with
//                their per-sequence key ranges and keys, the 1-D textures' texels and the global sequences.
//
// What the client does with them (WoW.exe 1.12.1, CSimpleModel 0x76D240 / 0x7AD7F0 / 0x76D1A0): a Model frame
// without a camera renders through an orthographic projection of its own rectangle (the viewport, which clips),
// the model's origin on the frame's bottom-left corner, X right and Y up, one model unit = 1280 interface units of
// the frame times the model's scale (the XML `scale` of a Model is SetModelScale, not the frame's scale). The
// models are flat (Z only orders them) and their batches are drawn in file order.
//
// Tracks keep the M2 timeline of every sequence (UiModelSequence::start..end, ms) with a [first, last] key range
// per sequence, or loop a global sequence; interpolation 0 holds a key, 1 is linear (quaternions normalized).

constexpr const char* kUiModelsAsset = "ui/models";
constexpr u32 kUiModelsVersion = 1;
constexpr u32 kUiModelNoTrack = ~0u;

enum class UiModelId : u32 {
    Cooldown = 0,    // Interface\Cooldown\UI-Cooldown-Indicator.m2 (CooldownFrameTemplate)
    AutoCast = 1,    // Interface\Buttons\UI-AutoCastButton.m2 (the pet's autocast sparkles)
    MinimapPing = 2, // Interface\MiniMap\Ping\MinimapPing.m2
    Count = 3,
};

struct UiModelArray {
    u32 count;
    u32 offset; // bytes from the blob's start
};

struct UiModelsHeader {
    u32 version; // kUiModelsVersion
    u32 reserved;
    UiModelArray models;          // UiModelInfo
    UiModelArray sequences;       // UiModelSequence
    UiModelArray bones;           // UiModelBone
    UiModelArray vertices;        // UiModelVertex: triangle lists
    UiModelArray batches;         // UiModelBatch
    UiModelArray emitters;        // UiModelEmitter
    UiModelArray tracks;          // UiModelTrack
    UiModelArray ranges;          // UiModelRange
    UiModelArray keys;            // UiModelKey
    UiModelArray texels;          // u32 RGBA8 (red in the low byte)
    UiModelArray globalSequences; // u32 durations, ms
};
static_assert(sizeof(UiModelsHeader) == 96, "UiModelsHeader layout");

struct UiModelInfo {
    u64 path; // assetPathHash of the client's model
    u16 firstSequence, sequenceCount;
    u16 firstBone, boneCount;
    u16 firstBatch, batchCount;
    u16 firstEmitter, emitterCount;
    u16 firstGlobalSequence, globalSequenceCount;
    u32 reserved;
};
static_assert(sizeof(UiModelInfo) == 32, "UiModelInfo layout");

struct UiModelSequence {
    u32 start; // ms on the tracks' timeline
    u32 end;
    u16 animationId; // AnimationData.dbc (0 Stand, 1 Death, 127 Birth)
    u16 flags;       // M2Sequence::flags
};
static_assert(sizeof(UiModelSequence) == 12, "UiModelSequence layout");

// M = parent * T(pivot) * T(translation) * R(rotation) * S(scale) * T(-pivot).
struct UiModelBone {
    f32 pivot[3];
    i16 parent; // -1: none (parents come first)
    u16 flags;  // M2 bone flags
    u32 translation, rotation, scale; // tracks (vec3, quaternion x y z w, vec3) or kUiModelNoTrack
};
static_assert(sizeof(UiModelBone) == 28, "UiModelBone layout");

struct UiModelVertex {
    f32 position[3]; // model space
    f32 uv[2];
    u32 bone; // the one bone it follows (the interface's models have no blending)
};
static_assert(sizeof(UiModelVertex) == 24, "UiModelVertex layout");

constexpr u32 kUiTextureWrapU = 0x1; // M2Texture::flags
constexpr u32 kUiTextureWrapV = 0x2;

// One draw of the model: triangles [firstVertex, firstVertex + vertexCount) with one texture. A texture whose rows
// are all the same (cooldown.blp) is a 1-D texture of texelCount texels along u (texture 0): the UI draws it as
// geometry, which lets its texture coordinate animation clamp and wrap as the client's sampler does. Others are a
// sprite of the UI atlas (the texture's assetPathHash).
struct UiModelBatch {
    u64 texture; // atlas sprite, 0: the 1-D texture
    u32 firstVertex, vertexCount;
    u32 firstTexel, texelCount;
    u8 blend;    // formats::M2Blend: 2 alpha, 4 additive
    u8 wrap;     // kUiTextureWrap*
    u16 reserved;
    u32 color, alpha, weight;        // tracks: color rgb (vec3), its opacity, the texture weight (opacity)
    u32 uvTranslation, uvRotation;   // texture coordinate animation (vec3, quaternion), or kUiModelNoTrack
};
static_assert(sizeof(UiModelBatch) == 48, "UiModelBatch layout");

// A particle emitter as the client's (its constant parameters; the interface's are not animated). Particles are
// camera-facing quads of half size `scales`, born `rate` times a second at the emitter (moved by its bone at that
// moment) and left behind (not in the emitter's space), moving at `speed` along their direction, their color,
// opacity and size through three points of their life (birth, midPoint, death).
struct UiModelEmitter {
    u64 texture; // atlas sprite
    u32 bone;
    u32 flags; // M2 particle flags
    f32 position[3];
    u8 blend;   // formats::M2Blend
    u8 shape;   // 1 plane, 2 sphere, 3 spline (its points all at the emitter in the interface's data)
    u8 rows, columns;
    f32 rate, lifespan, speed, speedVariation, verticalRange, horizontalRange, gravity, areaLength, areaWidth;
    f32 midPoint;
    u32 colors[3]; // RGBA8 (red in the low byte): birth, midPoint, death
    f32 scales[3];
    f32 spin;          // radians per second
    f32 twinklePercent; // twinkle (as fx: only when percent < 1 or the scales differ)
    f32 twinkleMin, twinkleMax;
};
static_assert(sizeof(UiModelEmitter) == 112, "UiModelEmitter layout");

struct UiModelTrack {
    u32 firstKey;
    u16 keyCount;
    u8 interpolation;     // 0 hold, 1 linear
    u8 components;        // 1 scalar, 3 vec3, 4 quaternion
    u32 firstRange;       // UiModelRange per sequence of the model
    u16 rangeCount;
    i16 globalSequence;   // index into the model's global sequences, -1: per sequence
};
static_assert(sizeof(UiModelTrack) == 16, "UiModelTrack layout");

struct UiModelRange {
    u16 first, last; // key indices of the track, inclusive
};
static_assert(sizeof(UiModelRange) == 4, "UiModelRange layout");

struct UiModelKey {
    u32 time; // ms on the timeline (of the global sequence for its tracks)
    f32 value[4];
};
static_assert(sizeof(UiModelKey) == 20, "UiModelKey layout");

} // namespace vd::pack
