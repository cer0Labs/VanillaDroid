#pragma once

#include "core/Hash.h"
#include "core/Types.h"

#include <string>
#include <string_view>

namespace vd::pack {

// Baked sky models (shared by the baker and the runtime), in models.vdpk under "sky/" + the M2's canonical
// path (e.g. "sky/environments/stars/stars.m2"), asset type SkyModel: the stars WoW.exe draws at night and
// the skyboxes of LightSkybox.dbc and of the WMOs (MOSB). They are drawn around the camera, behind
// everything, so they keep only what that needs: positions, texture coordinates and each M2 batch's static
// opacity in its vertices (the stars' layers differ in brightness), the batches merged by texture and blend,
// and the rotation of the root bone over its loop (DeathClouds turns once every 133 s). Model space, Z up.

constexpr u32 kSkyModelFormatVersion = 1;

struct SkyVertex {
    f32 position[3];
    f32 uv[2];
    u8 color[4]; // RGBA: white, alpha = the batch's transparency weight times its color's alpha (the first
                 // keys)
};
static_assert(sizeof(SkyVertex) == 24, "SkyVertex layout");

constexpr u8 kSkyPartClamp = 1u << 0; // clamp texture coordinates (else repeat)

// The batches with one texture and blend mode: indices [firstIndex, firstIndex + indexCount), in the order of
// the M2's first batch of each.
struct SkyPart {
    u32 firstIndex;
    u32 indexCount;
    u64 texture; // assetPathHash in textures.vdpk, 0 = none
    u8 blend;    // pack::ModelBlend
    u8 flags;    // kSkyPart*
    u16 reserved[3];
};
static_assert(sizeof(SkyPart) == 24, "SkyPart layout");

// A key of the root bone's rotation: quaternion (x, y, z, w) at `time` ms of the loop, linear in between.
struct SkyRotationKey {
    u32 time;
    f32 rotation[4];
};
static_assert(sizeof(SkyRotationKey) == 20, "SkyRotationKey layout");

struct SkyModelHeader {
    u32 version; // kSkyModelFormatVersion
    u32 vertexCount;
    u32 verticesOffset; // SkyVertex
    u32 indexCount;
    u32 indicesOffset; // u16 triangle list
    u32 partCount;
    u32 partsOffset; // SkyPart
    u32 rotationKeyCount;
    u32 rotationKeysOffset; // SkyRotationKey, sorted by time
    u32 rotationLoop;       // ms the rotation keys loop over, 0 = no rotation
    f32 radius;             // farthest vertex from the origin
    u32 reserved[5];
};
static_assert(sizeof(SkyModelHeader) == 64, "SkyModelHeader layout");

// Where a sky model lives in models.vdpk: "sky/" + the canonical path of the M2, from a reference as
// LightSkybox.dbc and the WMOs' MOSB chunks spell it ("Environments\Stars\DeathClouds.mdx": ".mdx" and ".mdl"
// are the M2's old names). Empty when the reference is not a model file (AZ_SunkenTemple's MOSB holds a
// title).
inline std::string skyModelPath(std::string_view reference) {
    const usize dot = reference.rfind('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    const std::string extension = normalizeAssetPath(reference.substr(dot));
    if (extension != ".mdx" && extension != ".mdl" && extension != ".m2") {
        return {};
    }
    return "sky/" + normalizeAssetPath(reference.substr(0, dot)) + ".m2";
}

} // namespace vd::pack
