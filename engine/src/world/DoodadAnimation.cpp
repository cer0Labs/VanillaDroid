#include "world/DoodadAnimation.h"

#include "core/Log.h"
#include "core/Profile.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>

#include <algorithm>
#include <cstring>

namespace vd {
namespace {

template <typename T> bool sectionFits(u32 offset, u32 count, usize size) {
    return count == 0 || (offset % alignof(T) == 0 && u64{offset} + u64{count} * sizeof(T) <= size);
}

constexpr anim::BoneMatrix kIdentity{
    {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}};

anim::BoneMatrix multiply(const anim::BoneMatrix& a, const anim::BoneMatrix& b) {
    anim::BoneMatrix out;
    for (u32 row = 0; row < 3; ++row) {
        const f32* r = a.rows + row * 4;
        for (u32 column = 0; column < 4; ++column) {
            out.rows[row * 4 + column] = r[0] * b.rows[column] + r[1] * b.rows[4 + column] +
                                         r[2] * b.rows[8 + column] + (column == 3 ? r[3] : 0.0f);
        }
    }
    return out;
}

// A well mixed u32 (the variations' picks: the same in every run, different per model, phase and loop).
u32 mix(u32 x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

// Animation LOD (the actors' rule): a new pose every frame near the camera, every 2, 4 and 8 frames further
// away.
u32 sampleInterval(f32 distance) {
    return distance < 30.0f ? 1u : distance < 60.0f ? 2u : distance < 120.0f ? 4u : 8u;
}

} // namespace

bool DoodadSkeleton::bind(const u8* blob, usize size) {
    *this = DoodadSkeleton{};
    pack::ModelHeader h;
    if (size < sizeof(h)) {
        return false;
    }
    std::memcpy(&h, blob, sizeof(h));
    if (h.version != pack::kModelFormatVersion || h.skeletonOffset == 0 ||
        !sectionFits<pack::ModelSkeletonHeader>(h.skeletonOffset, 1, size)) {
        return false;
    }
    const auto* s = reinterpret_cast<const pack::ModelSkeletonHeader*>(blob + h.skeletonOffset);
    if (s->boneCount == 0 || s->boneCount > 256 ||
        !sectionFits<pack::ActorBone>(s->bonesOffset, s->boneCount, size) ||
        (s->skinOffset != 0 && !sectionFits<pack::ModelSkinVertex>(s->skinOffset, h.vertexCount, size))) {
        return false;
    }
    anim::TrackTable table;
    if (!table.bind(blob, size, h.trackCount, h.tracksOffset, h.rangeCount, h.rangesOffset, h.vec3KeyCount,
                    h.vec3KeysOffset, h.quatKeyCount, h.quatKeysOffset, h.sequenceCount, h.sequenceSlotCount,
                    h.globalSequenceCount, h.globalSequencesOffset)) {
        return false;
    }
    if (s->variationCount != h.sequenceCount || s->variationCount == 0 ||
        !sectionFits<pack::ModelLoopVariation>(s->variationsOffset, s->variationCount, size)) {
        return false;
    }
    const auto* bones = reinterpret_cast<const pack::ActorBone*>(blob + s->bonesOffset);
    bool moves = false;
    for (u32 b = 0; b < s->boneCount; ++b) {
        if (bones[b].parent >= static_cast<i32>(b) || !table.trackValid(bones[b].tracks[0], false) ||
            !table.trackValid(bones[b].tracks[1], true) || !table.trackValid(bones[b].tracks[2], false)) {
            return false;
        }
        for (const u32 track : bones[b].tracks) {
            moves = moves || (track != pack::kActorNoTrack &&
                              table.tracks[track].kind != static_cast<u8>(pack::ActorTrackKind::Constant));
        }
    }
    // A model without a loop has no time to sample in it: its pose is the first one.
    m_tracks = table;
    m_bones = bones;
    m_variations = reinterpret_cast<const pack::ModelLoopVariation*>(blob + s->variationsOffset);
    m_variationCount = s->variationCount;
    for (u32 v = 0; v < m_variationCount; ++v) {
        m_frequencySum += m_variations[v].frequency;
    }
    m_boneCount = s->boneCount;
    m_loopMs = std::max(1u, s->loopMs);
    m_flags = s->flags;
    m_moves = moves;
    return true;
}

u32 DoodadSkeleton::nextVariation(u32 random) const {
    if (m_variationCount < 2) {
        return 0;
    }
    if (m_frequencySum == 0) {
        return random % m_variationCount;
    }
    u32 pick = random % m_frequencySum;
    for (u32 v = 0; v < m_variationCount; ++v) {
        if (pick < m_variations[v].frequency) {
            return v;
        }
        pick -= m_variations[v].frequency;
    }
    return 0;
}

void DoodadSkeleton::pose(u32 variation, u32 loopMs, u32 globalMs, anim::BoneMatrix* out) const {
    const u32 sequence = variation < m_variationCount ? variation : 0;
    for (u32 b = 0; b < m_boneCount; ++b) {
        const pack::ActorBone& bone = m_bones[b];
        if (bone.tracks[0] == pack::kActorNoTrack && bone.tracks[1] == pack::kActorNoTrack &&
            bone.tracks[2] == pack::kActorNoTrack) {
            out[b] = bone.parent >= 0 ? out[bone.parent]
                                      : kIdentity; // T(pivot) * T(-pivot): moves with its parent
            continue;
        }
        const glm::vec3 t =
            bone.tracks[0] == pack::kActorNoTrack
                ? glm::vec3(0.0f)
                : m_tracks.sampleVec3(bone.tracks[0], sequence, loopMs, globalMs, glm::vec3(0.0f));
        const glm::quat r = bone.tracks[1] == pack::kActorNoTrack
                                ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
                                : m_tracks.sampleQuat(bone.tracks[1], sequence, loopMs, globalMs);
        const glm::vec3 s =
            bone.tracks[2] == pack::kActorNoTrack
                ? glm::vec3(1.0f)
                : m_tracks.sampleVec3(bone.tracks[2], sequence, loopMs, globalMs, glm::vec3(1.0f));
        // local = T(pivot) * T(t) * R * S * T(-pivot)
        const glm::vec3 pivot(bone.pivot[0], bone.pivot[1], bone.pivot[2]);
        glm::mat3 rs = glm::mat3_cast(r);
        rs[0] *= s.x;
        rs[1] *= s.y;
        rs[2] *= s.z;
        const glm::vec3 translation = pivot + t - rs * pivot;
        // glm is column-major: rs[column][row].
        const anim::BoneMatrix local{{rs[0][0], rs[1][0], rs[2][0], translation.x, rs[0][1], rs[1][1],
                                      rs[2][1], translation.y, rs[0][2], rs[1][2], rs[2][2], translation.z}};
        out[b] = bone.parent >= 0 ? multiply(out[bone.parent], local) : local;
    }
}

anim::BoneMatrix billboardEntry(const pack::ActorBone& bone, const anim::BoneMatrix& m) {
    const f32* p = bone.pivot;
    const f32 posed[3] = {m.rows[0] * p[0] + m.rows[1] * p[1] + m.rows[2] * p[2] + m.rows[3],
                          m.rows[4] * p[0] + m.rows[5] * p[1] + m.rows[6] * p[2] + m.rows[7],
                          m.rows[8] * p[0] + m.rows[9] * p[1] + m.rows[10] * p[2] + m.rows[11]};
    const f32 scale = glm::length(glm::vec3(m.rows[0], m.rows[4], m.rows[8]));
    const f32 kind = (bone.flags & pack::kModelBoneBillboard) ? 1.0f : 2.0f;
    return anim::BoneMatrix{
        {posed[0], posed[1], posed[2], scale, p[0], p[1], p[2], kind, 0.0f, 0.0f, 0.0f, 0.0f}};
}

u32 DoodadAnimator::add(const u8* blob, usize size) {
    DoodadSkeleton skeleton;
    if (!skeleton.bind(blob, size)) {
        return kNone;
    }
    u32 slot = 0;
    if (!m_free.empty()) {
        slot = m_free.back();
        m_free.pop_back();
    } else {
        slot = static_cast<u32>(m_models.size());
        m_models.emplace_back();
    }
    Model& model = m_models[slot];
    model.skeleton = skeleton;
    model.used = true;
    model.poses.assign(usize{kPhases} * skeleton.boneCount(), kIdentity);
    model.encoded.assign(usize{kPhases} * skeleton.boneCount(), kIdentity);
    model.phases = {};
    return slot;
}

void DoodadAnimator::remove(u32 slot) {
    if (slot < m_models.size() && m_models[slot].used) {
        m_models[slot] = Model{};
        m_free.push_back(slot);
    }
}

void DoodadAnimator::clear() {
    m_models.clear();
    m_free.clear();
    m_palette = {};
    m_paletteUsed = 0;
}

void DoodadAnimator::beginFrame(u32 nowMs, std::span<anim::BoneMatrix> palette) {
    ++m_frame;
    m_nowMs = nowMs;
    m_palette = palette;
    m_paletteUsed = 0;
    m_stats = {};
}

u32 DoodadAnimator::touch(u32 slot, u32 originMs, f32 distance) {
    Model& model = m_models[slot];
    const DoodadSkeleton& skeleton = model.skeleton;
    const u32 loop = skeleton.loopMs();
    // The phase the loop started in, in kPhases steps (global sequences alone: one phase for all).
    const u32 index = (skeleton.flags() & pack::kModelSkeletonPhased)
                          ? static_cast<u32>(u64{originMs % loop} * kPhases / loop)
                          : 0u;
    Phase& phase = model.phases[index];
    if (phase.frame != m_frame) {
        phase.lastNearest = phase.frame + 1 == m_frame ? phase.nearest : distance;
        phase.frame = m_frame;
        phase.nearest = distance;
        ++m_stats.poses;
        const u32 interval = sampleInterval(phase.lastNearest);
        if (phase.sampledFrame == 0 || (skeleton.moves() && m_frame - phase.sampledFrame >= interval)) {
            sample(slot, index);
            phase.sampledFrame = m_frame;
        }
    } else {
        phase.nearest = std::min(phase.nearest, distance);
    }
    return slot * kPhases + index;
}

void DoodadAnimator::sample(u32 slot, u32 index) {
    VD_PROFILE_ZONE("DoodadAnimator::sample");
    Model& model = m_models[slot];
    const DoodadSkeleton& skeleton = model.skeleton;
    Phase& phase = model.phases[index];
    const u32 loop = skeleton.loopMs();
    if (!phase.started || m_nowMs - phase.variationStart > kMaxCatchUpMs) {
        // In the first variation, as if it had looped since its phase began (kPhases steps of the loop).
        const u32 start = static_cast<u32>(u64{loop} * index / kPhases);
        phase.variation = 0;
        phase.variationStart = m_nowMs - (m_nowMs % loop + loop - start) % loop;
        phase.started = true;
    }
    // A loop that ended: the next variation, by their weights.
    while (m_nowMs - phase.variationStart >= std::max(1u, skeleton.variationMs(phase.variation))) {
        phase.variationStart += std::max(1u, skeleton.variationMs(phase.variation));
        phase.variation =
            skeleton.nextVariation(mix(phase.variationStart ^ ((slot * kPhases + index) * 0x9E3779B9u)));
    }
    anim::BoneMatrix* pose = model.poses.data() + usize{index} * skeleton.boneCount();
    anim::BoneMatrix* encoded = model.encoded.data() + usize{index} * skeleton.boneCount();
    skeleton.pose(phase.variation, m_nowMs - phase.variationStart, m_nowMs, pose);
    for (u32 b = 0; b < skeleton.boneCount(); ++b) {
        const pack::ActorBone& bone = skeleton.bone(b);
        encoded[b] = (bone.flags & (pack::kModelBoneBillboard | pack::kModelBoneBillboardZ))
                         ? billboardEntry(bone, pose[b])
                         : pose[b];
    }
    ++m_stats.sampled;
}

u32 DoodadAnimator::paletteBase(u32 handle) {
    Model& model = m_models[handle / kPhases];
    Phase& phase = model.phases[handle % kPhases];
    if (phase.paletteFrame == m_frame) {
        return phase.paletteBase;
    }
    phase.paletteFrame = m_frame;
    const u32 bones = model.skeleton.boneCount();
    if (m_paletteUsed + bones > m_palette.size()) {
        if (!m_paletteFull) {
            VD_LOG_WARN("Doodad bone palette full (%zu matrices): some animated doodads stay still",
                        m_palette.size());
            m_paletteFull = true;
        }
        phase.paletteBase = kNone;
        return kNone;
    }
    phase.paletteBase = m_paletteUsed;
    std::memcpy(m_palette.data() + m_paletteUsed, model.encoded.data() + usize{handle % kPhases} * bones,
                bones * sizeof(anim::BoneMatrix));
    m_paletteUsed += bones;
    m_stats.bones += bones;
    return phase.paletteBase;
}

const anim::BoneMatrix* DoodadAnimator::pose(u32 handle) const {
    const Model& model = m_models[handle / kPhases];
    return model.poses.data() + usize{handle % kPhases} * model.skeleton.boneCount();
}

} // namespace vd
