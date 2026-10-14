#pragma once

#include "bake/BakeUtil.h"
#include "bake/TrackWriter.h"
#include "core/Types.h"
#include "formats/M2.h"
#include "pack/ParticleFormat.h"

#include <array>
#include <string>
#include <vector>

namespace vd::bake {

// The effects of an M2 (pack/ParticleFormat.h): particle emitters, ribbons, texture coordinate animations.
// Their animated values become tracks of `tracks`, the model's own table.
struct BakedEffects {
    std::vector<pack::ParticleEmitter> emitters;
    std::vector<pack::RibbonEmitter> ribbons;
    std::vector<pack::UvAnimation> uvAnimations;
    std::vector<std::array<f32, 3>> splinePoints;
    std::vector<pack::ParticleMesh> meshes; // phase 14 (spellfx): what model-emitting emitters emit
    std::vector<pack::ParticleMeshVertex> meshVertices;
    f32 reach = 0.0f;

    bool empty() const { return emitters.empty() && ribbons.empty() && uvAnimations.empty(); }
    // Appends the sections and their header; returns the header's offset (0 when there is nothing).
    u32 write(BlobWriter& writer) const;
};

// Phase 14 (spellfx): the models some emitters emit (M2 geometry_model, "Spells\ConeofCold_Geo.mdx"), parsed
// by the baker that has the client's archives. Thread-safe.
class ParticleModelSource {
public:
    virtual ~ParticleModelSource() = default;
    // The model at that client path (any extension), nullptr when it cannot be read.
    virtual const formats::M2Model* particleModel(const std::string& path) const = 0;
};

// `staticPose`: a doodad, drawn in the pose its first sequence starts with; the emitters' bone poses are
// baked into their transforms. Otherwise (actors) emitters keep their bone and move with the sampled
// skeleton. `models` (phase 14): where emitters that emit models find them (nullptr: those are left out).
BakedEffects bakeEffects(const formats::M2Model& model, TrackWriter& tracks, bool staticPose,
                         const ParticleModelSource* models = nullptr);
// Phase 14 (spellfx): the mesh a model's particles are drawn as (its first view's triangles with the texture
// and blend of its first batch), appended to `effects`; false when it has none.
bool bakeParticleMesh(const formats::M2Model& geometry, const formats::M2ParticleEmitter& emitter,
                      BakedEffects& effects, pack::ParticleMesh& out);

// Model-space matrix of a bone at `time` of `sequence` (its parents included), 3x4 by rows.
std::array<f32, 12> m2BonePose(const formats::M2Model& model, u32 bone, u32 sequence, u32 time);

// Emitters baked per model: what the bakers report.
struct EffectCounts {
    u32 emitters = 0;
    u32 ribbons = 0;
    u32 uvAnimations = 0;
    u32 skippedEmitters = 0; // emitting models (recursion; geometry without its model), not supported
    u32 meshEmitters = 0;    // phase 14: emitting models (geometry)
};
EffectCounts countEffects(const BakedEffects& effects, const formats::M2Model& model);

} // namespace vd::bake
