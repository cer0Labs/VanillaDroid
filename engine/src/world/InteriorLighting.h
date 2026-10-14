#pragma once

#include "core/Types.h"
#include "pack/ModelFormat.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <span>

namespace vd {

// Phase 11 (11.4, 11.5): the light of interiors and of the night, as pure functions over the baked tables
// (pack::ModelLightingHeader), testable without Vulkan. WorldObjects and Actors apply them; the shaders mirror the
// last ones (shaders/interior.glsl, terrain.vert).

// The light a doodad or an actor carries to the GPU (ModelInstanceData::light, ActorInstanceData::light): an
// ambient and one directional light that replace the frame's outdoor light. Both words 0: lit like the outdoors.
struct InstanceLight {
    glm::vec3 ambient{0.0f};
    glm::vec3 direct{0.0f};
    glm::vec3 direction{0.0f, 0.0f, 1.0f}; // world, unit, towards the light
};
// word 0: ambient RGB8 | octahedral direction x << 24; word 1: direct RGB8 | octahedral direction y << 24.
std::array<u32, 2> packInstanceLight(const InstanceLight& light);
InstanceLight unpackInstanceLight(const std::array<u32, 2>& packed);

// Inside a WMO's true interior (a group lit by its vertex colors, not like the outdoors) 1.12 lights what stands there
// with the light baked at that spot instead of the sun: the doodad's own color (MODD, "used in place of the global
// diffuse") or, for actors, the vertex colors around them, over the building's ambient (MOHD). Part of it comes from
// everywhere (kInteriorFill), the rest from the strongest lamp nearby (MOLT) or from above.
constexpr f32 kInteriorFill = 0.5f;
InstanceLight interiorLight(const glm::vec3& baked, const glm::vec3& wmoAmbient, const glm::vec3& towardsLight);

// The share of a MOLT / M2 light that reaches `distance` yards: 1 up to attenStart, 0 from attenEnd, linear between;
// an unattenuated light (attenEnd <= 0) reaches everywhere.
f32 lightAttenuation(const pack::ModelLight& light, f32 distance);
// How much a light weighs at `distance` (its intensity, the brightness of its color and the attenuation).
f32 lightStrength(const pack::ModelLight& light, f32 distance);

// The light baked into a true interior's vertex colors at a model-space point (the grid of its group; points outside
// take the nearest cell). `cells` are the WMO's cells; the group's average when it has no grid.
glm::vec3 gridLight(const pack::ModelGroupLighting& group, std::span<const u32> cells, const glm::vec3& point);

// MFOG: how much a fog weighs at a model-space point (WoW.exe 0x69e1c0): 1 inside its inner radius, 0 past its outer
// radius, linear between.
f32 fogWeight(const pack::ModelFog& fog, const glm::vec3& point);

// The fog of a building for a camera in one of its interior groups at a model-space point, as 1.12 builds it (WoW.exe
// 0x69de20): the building's first fog, then the fogs the camera's group lists (`groupFogs`, 0 = none) that reach the
// point and are not flagged infinite, each blended over it by its weight, the farthest first (0x69efd0: end, start
// scale and color). `underwater`: their underwater halves. The start scale is left as the file has it (the underwater
// fogs' -1 starts the fog at the camera).
struct InteriorFog {
    glm::vec3 color{0.0f};
    f32 end = 0.0f;
    f32 startScale = 0.0f;
};
InteriorFog interiorFog(std::span<const pack::ModelFog> fogs, std::span<const u8, 4> groupFogs, const glm::vec3& point,
                        bool underwater);

// Night glow of the WMO materials flagged SIDN (MOMT 0x10: windows, lamp glass): their emissive color times this.
// 1.12 keeps no table for it; it follows the night of the client's day (sunset 18:00, sunrise 06:00 in dnc.db's sun
// path): off from 07:00 to 18:00, on from 20:00 to 05:00, faded between. `timeOfDay` in half-minutes (0..2880).
f32 nightGlowFactor(f32 timeOfDay);

// An M2 light's intensity at `ms` (its diffuse intensity keys over its loop, or the constant one).
f32 lightIntensity(const pack::ModelLight& light, std::span<const pack::ModelLightKey> keys, u32 ms);

// MCLY texture animation (TerrainChunk::layerFlags): bits 0-2 the direction in 45-degree steps, 3-5 the speed, 0x40
// on. The layer's UVs (texture repeats) scroll by this at `seconds`; shaders/terrain.vert computes the same.
constexpr u8 kTerrainLayerAnimated = 0x40;
glm::vec2 terrainLayerOffset(u8 flags, f64 seconds);

// Packs RGB (0..1) into the low 24 bits of a word.
u32 packRgb8(const glm::vec3& rgb);
glm::vec3 unpackRgb8(u32 rgba);

// What WorldObjects answers to Actors: the light of something standing at a world position. Returns false when it
// stands outdoors (or in a building lit like the outdoors).
class InstanceLightSource {
public:
    virtual bool lightAt(const glm::dvec3& position, std::array<u32, 2>& packed) = 0;

protected:
    ~InstanceLightSource() = default;
};

} // namespace vd
