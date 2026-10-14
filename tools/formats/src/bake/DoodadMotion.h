#pragma once

#include "core/Types.h"
#include "formats/M2.h"

#include <array>
#include <vector>

namespace vd::bake {

// Phase 14 (14.1): how a placed M2 doodad moves. 1.12 animates every doodad (its doodadAnim CVar sets a world
// flag that nothing reads) over the model's Stand sequence, and turns billboard bones to the camera. The
// baker keeps the static path for the doodads that never visibly move and gives the others a skeleton
// (pack::ModelSkeletonHeader).

// Less than this over the loop (yards, about a millimetre) is not visible motion.
constexpr f32 kDoodadMotionThreshold = 0.001f;

// M2CompBone flags that turn a bone to the camera: spherical billboard, and the ones locked about the bone's
// X, Y or Z axis (cylindrical). The 1.12 data uses the spherical one and the one about Z (vd_baker
// scan-doodad-anim).
constexpr u32 kM2BoneBillboardMask = 0x8 | 0x10 | 0x20 | 0x40;

struct DoodadMotion {
    i32 loopSequence = -1; // the sequence a placed doodad loops (doodadLoopSequence), -1: none
    u32 loopMs = 0;        // its duration
    u32 variations = 0;    // of the loop (doodadLoopVariations); motion is measured over all of them
    u32 sequences = 0;
    u32 bones = 0;
    u32 animatedBones = 0;       // whose own tracks change over the loop (or over a global sequence)
    u32 globalSequenceBones = 0; // of those, driven by a global sequence
    u32 billboardBones = 0;      // flagged billboard, with vertices of their own
    u32 boneFlags = 0;           // OR of every bone's flags
    u32 vertices = 0;
    u32 movingVertices = 0;    // move more than kDoodadMotionThreshold over the loop (billboards aside)
    u32 billboardVertices = 0; // weighted to a billboard bone
    u32 blendedVertices = 0;   // weighted to more than one bone
    u32 movingEffects = 0;     // particle emitters and ribbons on a bone that moves over the loop
    f32 maxMotion = 0.0f;      // yards: the largest displacement of a vertex over the loop
    f32 restOffset = 0.0f;     // yards: the largest distance between the bind pose and the loop's first frame
    bool phased = false;       // a moving bone runs on the loop sequence (not only on global sequences)
    // Every pose of the loop and the bind pose (billboard vertices as the sphere they turn in), model space.
    std::array<f32, 3> boundsMin{0.0f, 0.0f, 0.0f};
    std::array<f32, 3> boundsMax{0.0f, 0.0f, 0.0f};
    bool animated() const { return movingVertices > 0; }
    // Its vertices need the skeleton: they move, turn to the camera, or stand in a pose other than the bind pose.
    bool skinnedMesh() const {
        return animated() || billboardVertices > 0 || restOffset > kDoodadMotionThreshold;
    }
    // It needs a skeleton at all (also for the particle emitters that ride a moving bone).
    bool skinned() const { return skinnedMesh() || movingEffects > 0; }
};

// The sequence 1.12 plays on a doodad: Stand (AnimationData 0), its first variation; the first sequence if it
// has none. -1 for a model without sequences.
i32 doodadLoopSequence(const formats::M2Model& model);
// The loop's variations: that sequence, then the others of the same animation (in the model's order). When
// one ends 1.12 picks the next by their frequencies. Empty without sequences.
std::vector<u32> doodadLoopVariations(const formats::M2Model& model);

// Model-space matrices (3x4 by rows) of every bone at `loopMs` into the loop sequence and `globalMs` on the
// global sequences, parents first. Billboards are not turned (that needs the camera). The reference the
// runtime's sampling of a baked doodad skeleton is checked against.
void doodadPose(const formats::M2Model& model, i32 sequence, u32 loopMs, u32 globalMs,
                std::vector<std::array<f32, 12>>& out);

DoodadMotion analyzeDoodadMotion(const formats::M2Model& model);

// Per bone, the billboard bone that turns it to the camera: itself when flagged, else its nearest flagged
// ancestor (the client's billboard matrix is its children's parent); -1 for none.
std::vector<i32> doodadBillboardBones(const formats::M2Model& model);

} // namespace vd::bake
