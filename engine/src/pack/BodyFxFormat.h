#pragma once

#include "core/Types.h"

namespace vd::pack {

// Phase 14 (bodyfx): what the units' bodies need that only the models know, baked into bodyfx.vdpk (a small
// pack of its own: re-baking it never touches actors.vdpk). One asset, kBodyBoundsAsset
// (AssetType::BodyBounds):
//
//   BodyBoundsHeader
//   BodyBoundsModel[modelCount], sorted by model hash
//   BodyBoundsBox[boxCount], each model's boxes contiguous and sorted by animation
//
// The boxes are the M2 sequences' own bounding boxes (variation 0 of each animation, model space: +X forward,
// Z up) of every model of CreatureModelData.dbc (characters, creatures, mounts). WoW.exe projects a unit's
// blob shadow over the box of the sequence its model plays (0x6D7920 over CM2Model 0x711A20, which reads the
// sequence's box at +0x24): a running wolf's shadow is long, a standing human's round.
constexpr const char* kBodyFxPack = "bodyfx.vdpk";
constexpr const char* kBodyBoundsAsset = "bodyfx/bounds";
constexpr u32 kBodyBoundsVersion = 1;

struct BodyBoundsHeader {
    u32 version; // kBodyBoundsVersion
    u32 modelCount;
    u32 boxCount;
    u32 reserved;
};
static_assert(sizeof(BodyBoundsHeader) == 16, "BodyBoundsHeader layout");

struct BodyBoundsModel {
    u64 model; // actors.vdpk asset hash of the model (its canonical ".m2" path)
    u32 firstBox;
    u32 boxCount;
};
static_assert(sizeof(BodyBoundsModel) == 16, "BodyBoundsModel layout");

struct BodyBoundsBox {
    u16 animation; // AnimationData.dbc
    u16 reserved;
    f32 min[3];
    f32 max[3];
};
static_assert(sizeof(BodyBoundsBox) == 28, "BodyBoundsBox layout");

} // namespace vd::pack
