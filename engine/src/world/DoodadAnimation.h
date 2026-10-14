#pragma once

#include "anim/BoneMatrix.h"
#include "anim/TrackTable.h"
#include "core/Types.h"
#include "pack/ActorFormat.h"
#include "pack/ModelFormat.h"

#include <array>
#include <span>
#include <vector>

namespace vd {

// Phase 14 (14.1): zero-copy view over the skeleton of a baked doodad (pack::ModelSkeletonHeader): its bones,
// their tracks in the model's own track table (a sequence per variation of the loop, Stand) and the flags.
// The blob must outlive it.
class DoodadSkeleton {
public:
    // Fails (false) without a skeleton or when a section does not fit.
    bool bind(const u8* blob, usize size);
    bool valid() const { return m_bones != nullptr; }

    u32 boneCount() const { return m_boneCount; }
    u32 loopMs() const { return m_loopMs; } // the first variation's
    u32 variationCount() const { return m_variationCount; }
    u32 variationMs(u32 variation) const { return m_variations[variation].durationMs; }
    // The variation that follows one of them, picked by their weights with `random` (any u32), as 1.12 does
    // when a loop ends.
    u32 nextVariation(u32 random) const;
    u32 flags() const { return m_flags; }
    const pack::ActorBone& bone(u32 index) const { return m_bones[index]; }
    // Some bone track changes with time (else the pose is the same every frame: billboards alone).
    bool moves() const { return m_moves; }

    // Model-space matrices of every bone at `loopMs` into variation `variation` and `globalMs` on the global
    // sequences, parents first, billboards not turned (the vertex shader turns them). out[0 .. boneCount()).
    void pose(u32 variation, u32 loopMs, u32 globalMs, anim::BoneMatrix* out) const;

private:
    anim::TrackTable m_tracks;
    const pack::ActorBone* m_bones = nullptr;
    const pack::ModelLoopVariation* m_variations = nullptr;
    u32 m_variationCount = 0;
    u32 m_frequencySum = 0;
    u32 m_boneCount = 0;
    u32 m_loopMs = 0;
    u32 m_flags = 0;
    bool m_moves = false;
};

// The GPU palette entry of a billboard bone (shaders/doodad.vert): row 0 = the bone's pivot as posed (model
// space) and its scale, row 1 = its bind-pose pivot and the kind (1 spherical, 2 about the model's Z), row 2
// unused.
anim::BoneMatrix billboardEntry(const pack::ActorBone& bone, const anim::BoneMatrix& posed);

// Phase 14 (14.1): the poses of the animated doodads. 1.12 animates every placed M2 (its doodadAnim CVar sets
// a world flag nothing reads) over its Stand sequence, starting the loop when the doodad appears: instances
// that appear together (a tile, a building's doodad set, or every instance waiting for their model to load)
// move in step, the others out of step; global sequences (flickers, wisps, some wheels) run on one clock for
// all. When a loop ends the next of Stand's variations is picked by their weights. Here a model's instances
// share one pose per phase bucket (kPhases per loop, each with its own run of variations), sampled once per
// frame (less often far away: animation LOD like the actors'), and the GPU skins and turns the billboards of
// every instance. Game thread only; nothing allocates per frame (models allocate when they load).
class DoodadAnimator {
public:
    static constexpr u32 kNone = ~0u;
    static constexpr u32 kPhases = 8;
    static constexpr u32 kMaxCatchUpMs = 60000; // a phase unseen longer starts its variations afresh

    // A model's skeleton in `blob` (which must outlive it): its slot, kNone without one.
    u32 add(const u8* blob, usize size);
    void remove(u32 slot);
    void clear();

    // Once per frame, before the requests: the clock and this frame's palette (write-only mapped memory).
    void beginFrame(u32 nowMs, std::span<anim::BoneMatrix> palette);
    // The pose shown this frame by an instance of `slot` that appeared at `originMs` (see above), `distance`
    // yards away: sampled now if due, else the last one. A handle for paletteBase() and pose().
    u32 touch(u32 slot, u32 originMs, f32 distance);
    // The handle's first matrix in this frame's palette (written on first use), kNone when the palette is
    // full.
    u32 paletteBase(u32 handle);
    // The handle's model-space bone matrices (billboards not turned): what the particle emitters on its bones
    // follow.
    const anim::BoneMatrix* pose(u32 handle) const;
    u32 boneCount(u32 slot) const { return m_models[slot].skeleton.boneCount(); }
    u32 paletteUsed() const { return m_paletteUsed; }

    struct Stats {
        u32 poses = 0;   // handles touched this frame
        u32 sampled = 0; // poses sampled this frame
        u32 bones = 0;   // palette matrices written
    };
    const Stats& stats() const { return m_stats; }

private:
    struct Phase {
        u32 frame = 0;        // last frame touched
        u32 sampledFrame = 0; // 0: never
        u32 paletteFrame = 0; // frame of paletteBase
        u32 paletteBase = kNone;
        f32 nearest = 0.0f;     // this frame
        f32 lastNearest = 0.0f; // the previous frame it was touched: decides the sampling rate
        u32 variation = 0;      // playing since variationStart (ms on the animator's clock)
        u32 variationStart = 0;
        bool started = false;
    };
    struct Model {
        DoodadSkeleton skeleton;
        bool used = false;
        std::vector<anim::BoneMatrix> poses;   // kPhases * bones: model space
        std::vector<anim::BoneMatrix> encoded; // the same, billboards as their palette entries
        std::array<Phase, kPhases> phases{};
    };

    void sample(u32 slot, u32 phase);

    std::vector<Model> m_models;
    std::vector<u32> m_free;
    std::span<anim::BoneMatrix> m_palette;
    u32 m_paletteUsed = 0;
    u32 m_nowMs = 0;
    u32 m_frame = 0;
    bool m_paletteFull = false;
    Stats m_stats;
};

} // namespace vd
