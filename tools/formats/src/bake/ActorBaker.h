#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "formats/M2.h"
#include "pack/ActorFormat.h"

#include <glm/mat4x4.hpp>

#include <vector>

namespace vd::bake {

class ParticleModelSource;

// Baked animated model blob (pack/ActorFormat.h): skinned vertices, parts per (material, geoset), skeleton
// with compact tracks, sequences and attachments. Models without bones get a single identity root bone.
// `particleModels` (phase 14): where emitters that emit models find them (nullptr: those emitters are left
// out).
Result<std::vector<u8>> bakeActor(const formats::M2Model& model,
                                  const ParticleModelSource* particleModels = nullptr);

// ActorTextureSlot of an M2 texture type.
pack::ActorTextureSlot actorTextureSlot(u32 m2TextureType);

// The model's portrait framing (pack::ActorPortrait): its portrait camera at the first frame, or, without
// one, a camera in front of its head (the Head key bone and the helm attachment) or of the top of
// `boundsMin..boundsMax` (the vertices' bounds), flagged kActorPortraitComputed.
pack::ActorPortrait actorPortrait(const formats::M2Model& model, const f32 boundsMin[3],
                                  const f32 boundsMax[3]);

// Model-space bone matrices at the first frame of the first sequence (Stand): what the portrait's fallback frames;
// phase 18: where the glue scenes' lights stand.
std::vector<glm::mat4> standPose(const formats::M2Model& model);

} // namespace vd::bake
