#pragma once

#include "core/Types.h"
#include "game/SpellKitFx.h"
#include "render/Renderer.h"

#include <glm/vec3.hpp>

#include <span>
#include <vector>

namespace vd {

// Phase 14 (spellfx): the beams of the 1.12 client (CLightning, WoW.exe 0x7AF600-0x7B0D00) and the fishing
// line (0x61F780), as camera-facing strips written into the particle renderer's quads (same pass and
// pipelines as the particles: additive beams, an alpha-blended lit line). Pure geometry, tested in
// tests/world.

// A beam to draw this frame (world/SpellVisuals collects them: channels and the jumps of chains).
struct BeamInstance {
    const game::ChainEffect* effect = nullptr;
    glm::dvec3 from{0.0};
    glm::dvec3 to{0.0};
    f32 age = 0.0f; // seconds since it started: where its texture has scrolled to
    u32 seed = 1;   // its joints' jitter (the client draws new joints every frame)
};

// Joints of the longest beam (a 170-yard beam of the shortest segments; longer ones get longer segments).
constexpr u32 kMaxBeamJoints = 64;
// Quads of one beam at most: a segment each, plus one more where the scrolled texture wraps.
constexpr u32 kMaxBeamQuads = kMaxBeamJoints;

// The joints of a beam (0x7AF6D0): int(length / segmentLength + 2) segments from `from` to `to`, the inner
// joints moved by 0.75 * r1 + 0.25 * r2 (r: uniform in a cube of +-length * noise; the client rebuilds the
// joints every frame and blends them 3:1 with another random set at 0x7AFBE7). Returns the joints written (>=
// 2), at most out.size().
u32 beamJoints(const game::ChainEffect& effect, const glm::vec3& from, const glm::vec3& to, u32 seed,
               std::span<glm::vec3> out);
// The texture's offset along the beam at `age` (0x7AF9B0 / 0x7B0544): -(fmod(age, scroll) / scroll); 0 when
// still.
f32 beamScroll(const game::ChainEffect& effect, f32 age);
// Writes a beam (0x7AFCB0): its joints camera-relative, each widened by `width` along the screen-space normal
// of its neighbouring segments (half of each), both ends pointed; the texture once over its length (u),
// across it (v), scrolled. A segment where the scrolled texture wraps is split in two (the particle textures
// clamp). Returns the quads written (<= kMaxBeamQuads, never more than out.size() / 4).
u32 writeBeam(const BeamInstance& beam, const glm::dvec3& camera, const glm::vec3& cameraForward,
              std::span<ParticleVertex> out);

// The fishing line (0x61F780): 64 straight pieces from the pole's tip to the bobber, sagging by sin(pi * t) *
// `sag` yards; the client draws it as a one-pixel line strip, here a strip `pixels` wide on screen (the
// particles' pixelsPerYard). White: the particle shader lights it like the pole's model.
constexpr u32 kFishingLinePieces = 64;
constexpr f32 kFishingLineSag = 0.5f;
u32 writeLine(const glm::dvec3& from, const glm::dvec3& to, f32 sag, const glm::dvec3& camera,
              const glm::vec3& cameraForward, f32 pixelsPerYard, f32 pixels, std::span<ParticleVertex> out);

// Collects this frame's beams and lines into the renderer's particle quads (after what is already there) and
// their draws: beams by texture, additive; lines alpha-blended and lit. Built in cached memory, copied once.
// Game thread; no allocation once its scratch is reserved.
class BeamWriter {
public:
    static constexpr u32 kMaxQuads = 4096;

    void init();
    // `texture` resolves a chain effect's texture hash (invalid: not loaded yet; that beam waits).
    using TextureLookup = TextureHandle (*)(void* context, u64 hash);
    void begin(const glm::dvec3& camera, const glm::vec3& cameraForward, f32 pixelsPerYard,
               f32 viewportHeight);
    void addBeams(std::span<const BeamInstance> beams, TextureLookup texture, void* context);
    void addLine(const glm::dvec3& from, const glm::dvec3& to);
    // Copies the quads to `vertices` from quad `firstQuad` on and appends their draws; returns the new quad
    // count.
    u32 finish(std::span<ParticleVertex> vertices, u32 firstQuad, std::vector<ParticleDraw>& draws);

    u32 beams() const { return m_beams; }
    u32 quads() const { return m_quads; }

private:
    struct Run {
        TextureHandle texture;
        u8 blend = 0;
        u8 flags = 0;
        u32 firstQuad = 0;
        u32 quadCount = 0;
    };
    void addRun(TextureHandle texture, u8 blend, u8 flags, u32 quads);

    std::vector<ParticleVertex> m_scratch;
    std::vector<Run> m_runs;
    glm::dvec3 m_camera{0.0};
    glm::vec3 m_forward{1.0f, 0.0f, 0.0f};
    f32 m_pixelsPerYard = 1000.0f;
    f32 m_linePixels = 1.0f;
    u32 m_quads = 0;
    u32 m_beams = 0;
};

} // namespace vd
