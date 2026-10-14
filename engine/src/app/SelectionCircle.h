#pragma once

#include "core/Hash.h"
#include "core/Types.h"
#include "game/Reactions.h"
#include "render/Renderer.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <optional>
#include <span>

namespace vd {

// The selection circle at the target's feet (parity; the client's engine draws it: ObjectSelectionCircle in
// WoW.exe). Its texture is Textures\UnitSelectTexture.blp: white, the ring and a soft fill in the alpha, the
// ring brighter on one side; the engine tints it by how the unit looks to the player (green, yellow, red) and
// sizes it by the unit's model (pack::actorSelectionHalfSize). Here it is a small grid of quads in the
// renderer's particle pass (depth tested, alpha blended, no pipeline of its own) whose corners sit on the floor
// found by `ground`, so it follows slopes and steps instead of cutting into them, lifted a little against
// z-fighting, with the texture's bright side towards the camera. The floor is sampled again only when the unit
// moves or the size changes. Game thread only; nothing allocates.
class SelectionCircle {
public:
    static constexpr u64 kTexture = assetPathHash("Textures\\UnitSelectTexture.blp");
    static constexpr u32 kCells = 4;               // grid cells per side (the centre is a sample)
    static constexpr u32 kQuads = kCells * kCells; // written per circle
    static constexpr u32 kMaxCells = 8;            // phase 13 (spells): a spell's wide circle (setCells)
    // The ring's radius in the texture (its brightest line, 113 of 128 texels from the centre).
    static constexpr f32 kRingFraction = 0.88f;

    // The highest floor under (x, y) between `top` and `bottom` (yards), nullopt if there is none.
    using GroundQuery = std::optional<f32> (*)(const void* context, f32 x, f32 y, f32 top, f32 bottom);

    // The ring's radius (yards) for the texture's square of half width `halfSize` (WoW.exe's size: the unit's
    // model's, Units::selectionHalfSize).
    static f32 ringOfHalfSize(f32 halfSize) { return halfSize * kRingFraction; }
    // While its model loads: the ring's radius for a unit's UNIT_FIELD_BOUNDINGRADIUS (the server's, already scaled
    // with the unit): about twice its body's radius, never smaller than a small critter's.
    static f32 ringRadius(f32 boundingRadius);
    // The engine's tint: green friendly, yellow neutral, red hostile (RGBA8, red in the low byte).
    static u32 color(game::Reaction reaction);

    // The circle's quads into `out` (camera-relative corners in ParticleVertex order, 4 per quad, texture
    // coordinates over the whole texture); returns how many were written: cells() squared, or 0 when they do
    // not fit or `ringRadius` <= 0. `forward`: the camera's view direction (the bright side faces the camera).
    u32 build(const glm::dvec3& feet, f32 ringRadius, u32 color, const glm::dvec3& camera,
              const glm::vec3& forward, std::span<ParticleVertex> out, GroundQuery ground,
              const void* context);
    // Forgets the sampled floor (a new map: the same spot may lie on another floor).
    void reset() { m_halfSize = 0.0f; }
    // Ours (spell targeting): the texture laid along the world's axes instead of its bright side towards the camera
    // (WoW.exe projects the spell's shadow as a box on the ground, 0x48383B).
    void setWorldAligned(bool aligned) { m_worldAligned = aligned; }
    // Phase 13 (spells): the grid's cells per side (1..kMaxCells; kCells by default): a wide circle hugs the
    // ground with more.
    void setCells(u32 cells) {
        const u32 clamped = cells < 1 ? 1 : (cells > kMaxCells ? kMaxCells : cells);
        if (clamped != m_cells) {
            m_cells = clamped;
            m_halfSize = 0.0f; // sampled again
        }
    }
    u32 cells() const { return m_cells; }

private:
    static constexpr u32 kMaxSide = kMaxCells + 1;
    // Floor under each grid corner (rows of kMaxSide), relative to m_feet.z.
    std::array<f32, kMaxSide * kMaxSide> m_heights{};
    u32 m_cells = kCells;
    glm::dvec3 m_feet{0.0};                     // where it was sampled
    f32 m_halfSize = 0.0f;                      // half the quad's width when sampled (0: nothing sampled)
    glm::vec2 m_toCamera{1.0f, 0.0f};           // last horizontal direction to the camera (kept looking down)
    bool m_worldAligned = false;                // ours (spell targeting)
};

} // namespace vd
