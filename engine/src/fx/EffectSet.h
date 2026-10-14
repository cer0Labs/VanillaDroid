#pragma once

#include "anim/TrackTable.h"
#include "core/Result.h"
#include "core/Types.h"
#include "pack/ParticleFormat.h"
#include "render/Renderer.h"

#include <glm/vec3.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace vd::fx {

// Zero-copy view over the effects of a baked model (pack/ParticleFormat.h): its particle emitters, ribbons
// and texture animations, with the track table their animated values use. Bound once when the model loads
// (every index is checked there, so sampling needs no checks); the blob must outlive the view. Read-only and
// thread-safe.
class EffectSet {
public:
    // A doodad (pack::ModelHeader blob). Its effects loop the model's first sequence: loopMs().
    Result<void> bindModel(const u8* blob, usize size);
    // An actor model (pack::ActorModelHeader blob) whose skeleton has `boneCount` bones and track table
    // `tracks`.
    Result<void> bindActor(const u8* blob, usize size, const anim::TrackTable& tracks, u32 boneCount);

    bool valid() const { return m_header != nullptr; }
    bool hasParticles() const { return !m_emitters.empty() || !m_ribbons.empty(); }
    std::span<const pack::ParticleEmitter> emitters() const { return m_emitters; }
    std::span<const pack::RibbonEmitter> ribbons() const { return m_ribbons; }
    std::span<const pack::UvAnimation> uvAnimations() const { return m_uvAnimations; }
    std::span<const glm::vec3> splinePoints() const { return m_splinePoints; }
    // Phase 14 (spellfx): the model an emitter's particles are drawn as (nullptr: quads), and its triangles'
    // corners.
    const pack::ParticleMesh* mesh(u32 emitter) const {
        const u32 m = m_emitters[emitter].mesh;
        return m != 0 ? &m_meshes[m - 1] : nullptr;
    }
    std::span<const pack::ParticleMeshVertex> meshVertices(const pack::ParticleMesh& mesh) const {
        return m_meshVertices.subspan(mesh.firstVertex, u64{mesh.triangleCount} * 3);
    }
    const anim::TrackTable& tracks() const { return m_tracks; }
    f32 reach() const { return m_reach; }
    bool actor() const { return m_actor; }
    u32 loopMs() const { return m_loopMs; } // doodads: duration of the sequence they loop (0: none)
    // Most particles emitter `index` can have alive at once (its highest rate times its longest life).
    u32 capacity(u32 emitter) const { return m_capacities[emitter]; }
    // Textures the effects need (emitters then ribbons), to load with the model.
    u64 emitterTexture(u32 emitter) const { return m_emitters[emitter].texture; }

    // Texture matrices of every UV animation at this time into out[0 .. uvAnimations().size()).
    void computeUvTransforms(u32 sequence, u32 localMs, u32 globalMs, UvTransform* out) const;

private:
    Result<void> bindSections(const u8* blob, usize size, u32 effectsOffset, u32 boneCount);

    const pack::EffectsHeader* m_header = nullptr;
    std::span<const pack::ParticleEmitter> m_emitters;
    std::span<const pack::RibbonEmitter> m_ribbons;
    std::span<const pack::UvAnimation> m_uvAnimations;
    std::span<const glm::vec3> m_splinePoints;
    std::span<const pack::ParticleMesh> m_meshes; // phase 14
    std::span<const pack::ParticleMeshVertex> m_meshVertices;
    anim::TrackTable m_tracks;
    std::vector<u32> m_capacities;
    f32 m_reach = 0.0f;
    u32 m_loopMs = 0;
    bool m_actor = false;
};

// Texture matrix of one UV animation from its translation, rotation and scale (see pack::UvAnimation).
UvTransform uvTransform(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale);

// actors.vdpk asset hash of a model path as the client tables spell it (SpellVisualEffectName.dbc:
// "Spells\Frostbolt_Missile.mdx", sometimes with a leading backslash or ".mdl"): the canonical ".m2" path. 0
// if empty.
u64 effectModelHash(std::string_view clientPath);

} // namespace vd::fx
