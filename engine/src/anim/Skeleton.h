#pragma once

#include "anim/BoneMatrix.h"
#include "anim/TrackTable.h"
#include "core/Result.h"
#include "core/Types.h"
#include "pack/ActorFormat.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <span>

namespace vd::anim {

constexpr u32 kMaxBones = 256; // a vertex addresses its bones with a u8

// Zero-copy view over the skeleton and animation sections of a baked actor model (pack/ActorFormat.h). The
// blob must outlive the view. Sampling is read-only and thread-safe (jobs sample many actors at once).
class Skeleton {
public:
    Result<void> bind(const u8* blob, usize size);
    bool valid() const { return m_header != nullptr; }

    u32 boneCount() const { return m_header->boneCount; }
    u32 sequenceCount() const { return m_header->sequenceCount; }
    const pack::ActorModelHeader& header() const { return *m_header; }
    const pack::ActorBone& bone(u32 index) const { return m_bones[index]; }
    const pack::ActorSequence& sequence(u32 index) const { return m_sequences[index]; }
    std::span<const pack::ActorAttachment> attachments() const { return {m_attachments, m_header->attachmentCount}; }
    std::span<const pack::ActorMaterial> materials() const { return {m_materials, m_header->materialCount}; }
    // Attachment point by id (1 right hand, 11 helm...), nullptr if the model has none.
    const pack::ActorAttachment* attachment(u32 id) const;
    // Phase 14: the model's events (empty when it has none), and the local times (ms, ascending) at which event `e`
    // fires in `sequence` (empty: none there, or an always-on event).
    std::span<const pack::ActorEvent> events() const { return {m_events, m_eventCount}; }
    std::span<const u32> eventTimes(u32 e, u32 sequence) const {
        const pack::ActorRange& range = m_eventRanges[m_events[e].firstRange + sequence];
        return {m_eventTimes + range.first, range.count};
    }
    bool hasTimedEvents() const { return m_timedEvents; }

    // First sequence playing `animationId` (variation 0), -1 if the model lacks it.
    i32 findSequence(u16 animationId) const;
    // Duration of a sequence in ms (at least 1).
    u32 duration(u32 sequence) const;

    // Channel values. `local` is the time inside the sequence (ms from its start); tracks tied to a global
    // sequence use `globalMs` instead.
    glm::vec3 sampleVec3(u32 track, u32 sequence, u32 local, u32 globalMs, const glm::vec3& fallback) const {
        return m_table.sampleVec3(track, sequence, local, globalMs, fallback);
    }
    glm::quat sampleQuat(u32 track, u32 sequence, u32 local, u32 globalMs) const {
        return m_table.sampleQuat(track, sequence, local, globalMs);
    }
    // The model's track table (bones, material colors, effects).
    const TrackTable& tracks() const { return m_table; }

    // Phase 9: the upper body is the bone with the SpineLow key bone and everything under it (chest, arms, head;
    // the legs hang from the Waist bone, both children of the Root). Models without that key bone (most creatures)
    // have none and play their actions on the whole body.
    static constexpr i32 kSpineLowKeyBone = 4;
    bool hasUpperBody() const { return m_hasUpperBody; }
    bool upperBody(u32 bone) const { return (m_upperBody[bone >> 6] >> (bone & 63)) & 1u; }

private:
    const pack::ActorModelHeader* m_header = nullptr;
    const pack::ActorBone* m_bones = nullptr;
    TrackTable m_table;
    const pack::ActorSequence* m_sequences = nullptr;
    const pack::ActorAttachment* m_attachments = nullptr;
    const pack::ActorMaterial* m_materials = nullptr;
    const pack::ActorEvent* m_events = nullptr; // phase 14
    u32 m_eventCount = 0;
    const pack::ActorRange* m_eventRanges = nullptr;
    const u32* m_eventTimes = nullptr;
    bool m_timedEvents = false;
    std::array<u64, kMaxBones / 64> m_upperBody{};
    bool m_hasUpperBody = false;
};

// What a skeleton is playing: the current sequence and, while a transition lasts, the previous one blended out.
struct AnimLayer {
    i32 sequence = -1; // -1: bind pose
    u32 time = 0;      // ms from the sequence start
};
struct AnimPose {
    AnimLayer current;
    AnimLayer previous;
    f32 blend = 1.0f; // weight of `current` (1: no transition)
};

// Phase 9: an action played over the base pose (a swing, a wound, a cast), with its own sequences and
// transitions, on every bone or only on the upper body (Skeleton::upperBody) while the legs keep the base pose.
struct ActionPose {
    AnimPose pose;
    f32 weight = 0.0f; // 0: not shown, 1: replaces the base on its bones
    bool upperBody = false;
};

// Model-space skinning matrices of every bone for `pose` into out[0 .. boneCount). `cameraInModel`: the camera
// direction in the model's space (unit), for bones that always face it; zero vector to skip billboarding.
// `action` (may be null) is blended over the pose on the bones it plays on.
void computeBones(const Skeleton& skeleton, const AnimPose& pose, u32 globalMs, const glm::vec3& cameraInModel,
                  BoneMatrix* out, const ActionPose* action = nullptr);

// Animated color multiplier (RGBA) of every material into out[0 .. materialCount).
void computeMaterialColors(const Skeleton& skeleton, const AnimPose& pose, u32 globalMs, glm::vec4* out);

// out = a * b for 3x4 affine matrices.
BoneMatrix multiply(const BoneMatrix& a, const BoneMatrix& b);
glm::vec3 transformPoint(const BoneMatrix& m, const glm::vec3& p);

} // namespace vd::anim
