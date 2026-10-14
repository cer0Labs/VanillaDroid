#include "anim/Skeleton.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>

#include <algorithm>
#include <cstring>

namespace vd::anim {
namespace {

template <typename T>
bool sectionFits(u32 offset, u32 count, usize size) {
    return count == 0 || (offset % alignof(T) == 0 && u64{offset} + u64{count} * sizeof(T) <= size);
}

glm::quat nlerp(const glm::quat& a, glm::quat b, f32 t) {
    if (glm::dot(a, b) < 0.0f) {
        b = -b;
    }
    return glm::normalize(a * (1.0f - t) + b * t);
}

BoneMatrix fromAffine(const glm::mat3& m, const glm::vec3& t) {
    // glm is column-major: m[column][row].
    return BoneMatrix{{m[0][0], m[1][0], m[2][0], t.x, m[0][1], m[1][1], m[2][1], t.y, m[0][2], m[1][2], m[2][2], t.z}};
}

struct Channels {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

Channels sampleBone(const Skeleton& skeleton, const pack::ActorBone& bone, const AnimLayer& layer, u32 globalMs) {
    Channels c;
    if (layer.sequence < 0) {
        return c;
    }
    const u32 s = static_cast<u32>(layer.sequence);
    if (bone.tracks[0] != pack::kActorNoTrack) {
        c.translation = skeleton.sampleVec3(bone.tracks[0], s, layer.time, globalMs, glm::vec3(0.0f));
    }
    if (bone.tracks[1] != pack::kActorNoTrack) {
        c.rotation = skeleton.sampleQuat(bone.tracks[1], s, layer.time, globalMs);
    }
    if (bone.tracks[2] != pack::kActorNoTrack) {
        c.scale = skeleton.sampleVec3(bone.tracks[2], s, layer.time, globalMs, glm::vec3(1.0f));
    }
    return c;
}

} // namespace

Result<void> Skeleton::bind(const u8* blob, usize size) {
    *this = Skeleton{};
    if (size < sizeof(pack::ActorModelHeader) || reinterpret_cast<uintptr_t>(blob) % 8 != 0) {
        return Error{"Actor blob too small or misaligned"};
    }
    const auto* h = reinterpret_cast<const pack::ActorModelHeader*>(blob);
    if (h->version != pack::kActorFormatVersion) {
        return Error{"Actor format version mismatch (re-bake actors)"};
    }
    if (h->boneCount == 0 || h->boneCount > kMaxBones || !sectionFits<pack::ActorBone>(h->bonesOffset, h->boneCount, size) ||
        !sectionFits<pack::ActorTrack>(h->tracksOffset, h->trackCount, size) ||
        !sectionFits<pack::ActorRange>(h->rangesOffset, h->rangeCount, size) ||
        !sectionFits<pack::ActorVec3Key>(h->vec3KeysOffset, h->vec3KeyCount, size) ||
        !sectionFits<pack::ActorQuatKey>(h->quatKeysOffset, h->quatKeyCount, size) ||
        !sectionFits<pack::ActorSequence>(h->sequencesOffset, h->sequenceCount, size) ||
        !sectionFits<u32>(h->globalSequencesOffset, h->globalSequenceCount, size) ||
        !sectionFits<pack::ActorAttachment>(h->attachmentsOffset, h->attachmentCount, size) ||
        !sectionFits<pack::ActorMaterial>(h->materialsOffset, h->materialCount, size)) {
        return Error{"Corrupt actor skeleton"};
    }
    const auto* bones = reinterpret_cast<const pack::ActorBone*>(blob + h->bonesOffset);
    const auto* materials = reinterpret_cast<const pack::ActorMaterial*>(blob + h->materialsOffset);
    // Every index the samplers follow is checked once here, so sampling needs no bounds checks.
    TrackTable table;
    if (!table.bind(blob, size, h->trackCount, h->tracksOffset, h->rangeCount, h->rangesOffset, h->vec3KeyCount,
                    h->vec3KeysOffset, h->quatKeyCount, h->quatKeysOffset, h->sequenceCount, h->sequenceSlotCount,
                    h->globalSequenceCount, h->globalSequencesOffset)) {
        return Error{"Corrupt actor track table"};
    }
    auto trackOk = [&](u32 track, bool quaternion) { return table.trackValid(track, quaternion); };
    for (u32 b = 0; b < h->boneCount; ++b) {
        if (bones[b].parent >= static_cast<i32>(b) || !trackOk(bones[b].tracks[0], false) ||
            !trackOk(bones[b].tracks[1], true) || !trackOk(bones[b].tracks[2], false)) {
            return Error{"Corrupt actor bone"};
        }
    }
    for (u32 m = 0; m < h->materialCount; ++m) {
        for (u16 track : {materials[m].colorTrack, materials[m].alphaTrack, materials[m].transparencyTrack}) {
            if (track != pack::kActorNoColorTrack && !trackOk(track, false)) {
                return Error{"Corrupt actor material track"};
            }
        }
    }
    const auto* attachments = reinterpret_cast<const pack::ActorAttachment*>(blob + h->attachmentsOffset);
    for (u32 a = 0; a < h->attachmentCount; ++a) {
        if (attachments[a].bone >= h->boneCount) {
            return Error{"Corrupt actor attachment"};
        }
    }
    // Phase 14: the events, every index checked here as well.
    const pack::ActorEvent* events = nullptr;
    const pack::ActorRange* eventRanges = nullptr;
    const u32* eventTimes = nullptr;
    u32 eventCount = 0;
    bool timedEvents = false;
    if (h->eventsOffset != 0) {
        if (!sectionFits<pack::ActorEventsHeader>(h->eventsOffset, 1, size)) {
            return Error{"Corrupt actor events"};
        }
        const auto* e = reinterpret_cast<const pack::ActorEventsHeader*>(blob + h->eventsOffset);
        if (!sectionFits<pack::ActorEvent>(e->eventsOffset, e->eventCount, size) ||
            !sectionFits<pack::ActorRange>(e->rangesOffset, e->rangeCount, size) ||
            !sectionFits<u32>(e->timesOffset, e->timeCount, size) ||
            u64{e->rangeCount} != u64{e->eventCount} * h->sequenceCount) {
            return Error{"Corrupt actor events"};
        }
        events = reinterpret_cast<const pack::ActorEvent*>(blob + e->eventsOffset);
        eventRanges = reinterpret_cast<const pack::ActorRange*>(blob + e->rangesOffset);
        eventTimes = reinterpret_cast<const u32*>(blob + e->timesOffset);
        for (u32 i = 0; i < e->eventCount; ++i) {
            if (events[i].bone >= h->boneCount || u64{events[i].firstRange} + h->sequenceCount > e->rangeCount) {
                return Error{"Corrupt actor event"};
            }
            timedEvents = timedEvents || (events[i].flags & pack::kActorEventTimed) != 0;
        }
        for (u32 r = 0; r < e->rangeCount; ++r) {
            if (u64{eventRanges[r].first} + eventRanges[r].count > e->timeCount) {
                return Error{"Corrupt actor event times"};
            }
        }
        eventCount = e->eventCount;
    }
    m_header = h;
    m_bones = bones;
    m_events = events;
    m_eventCount = eventCount;
    m_eventRanges = eventRanges;
    m_eventTimes = eventTimes;
    m_timedEvents = timedEvents;
    m_table = table;
    m_sequences = reinterpret_cast<const pack::ActorSequence*>(blob + h->sequencesOffset);
    m_attachments = attachments;
    m_materials = materials;
    // The upper body: the SpineLow bone and its descendants (parents come first).
    for (u32 b = 0; b < h->boneCount; ++b) {
        const bool upper = bones[b].keyBone == kSpineLowKeyBone || (bones[b].parent >= 0 && upperBody(bones[b].parent));
        if (upper) {
            m_upperBody[b >> 6] |= 1ull << (b & 63);
            m_hasUpperBody = true;
        }
    }
    return {};
}

const pack::ActorAttachment* Skeleton::attachment(u32 id) const {
    for (u32 a = 0; a < m_header->attachmentCount; ++a) {
        if (m_attachments[a].id == id) {
            return &m_attachments[a];
        }
    }
    return nullptr;
}

i32 Skeleton::findSequence(u16 animationId) const {
    i32 found = -1;
    for (u32 s = 0; s < m_header->sequenceCount; ++s) {
        if (m_sequences[s].animation == animationId) {
            if (m_sequences[s].variation == 0) {
                return static_cast<i32>(s);
            }
            if (found < 0) {
                found = static_cast<i32>(s);
            }
        }
    }
    return found;
}

u32 Skeleton::duration(u32 sequence) const {
    const pack::ActorSequence& s = m_sequences[sequence];
    return std::max<u32>(1, s.end - s.start);
}

BoneMatrix multiply(const BoneMatrix& a, const BoneMatrix& b) {
    BoneMatrix out;
    for (u32 row = 0; row < 3; ++row) {
        const f32* r = a.rows + row * 4;
        for (u32 column = 0; column < 4; ++column) {
            out.rows[row * 4 + column] = r[0] * b.rows[column] + r[1] * b.rows[4 + column] + r[2] * b.rows[8 + column] +
                                         (column == 3 ? r[3] : 0.0f);
        }
    }
    return out;
}

glm::vec3 transformPoint(const BoneMatrix& m, const glm::vec3& p) {
    return {m.rows[0] * p.x + m.rows[1] * p.y + m.rows[2] * p.z + m.rows[3],
            m.rows[4] * p.x + m.rows[5] * p.y + m.rows[6] * p.z + m.rows[7],
            m.rows[8] * p.x + m.rows[9] * p.y + m.rows[10] * p.z + m.rows[11]};
}

void computeBones(const Skeleton& skeleton, const AnimPose& pose, u32 globalMs, const glm::vec3& cameraInModel,
                  BoneMatrix* out, const ActionPose* action) {
    const u32 count = skeleton.boneCount();
    const bool blending = pose.blend < 1.0f && pose.previous.sequence >= 0;
    const bool acting = action && action->weight > 0.0f && action->pose.current.sequence >= 0;
    const bool actionBlending = acting && action->pose.blend < 1.0f && action->pose.previous.sequence >= 0;
    const bool upperOnly = acting && action->upperBody && skeleton.hasUpperBody();
    for (u32 b = 0; b < count; ++b) {
        const pack::ActorBone& bone = skeleton.bone(b);
        if (bone.tracks[0] == pack::kActorNoTrack && bone.tracks[1] == pack::kActorNoTrack &&
            bone.tracks[2] == pack::kActorNoTrack && !(bone.flags & pack::kActorBoneBillboard)) {
            // Never animated: T(pivot) * T(-pivot) is the identity, so it moves exactly like its parent.
            out[b] = bone.parent >= 0 ? out[bone.parent]
                                      : BoneMatrix{{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}};
            continue;
        }
        Channels c = sampleBone(skeleton, bone, pose.current, globalMs);
        if (blending) {
            const Channels p = sampleBone(skeleton, bone, pose.previous, globalMs);
            c.translation = glm::mix(p.translation, c.translation, pose.blend);
            c.rotation = nlerp(p.rotation, c.rotation, pose.blend);
            c.scale = glm::mix(p.scale, c.scale, pose.blend);
        }
        if (acting && (!upperOnly || skeleton.upperBody(b))) {
            Channels a = sampleBone(skeleton, bone, action->pose.current, globalMs);
            if (actionBlending) {
                const Channels p = sampleBone(skeleton, bone, action->pose.previous, globalMs);
                a.translation = glm::mix(p.translation, a.translation, action->pose.blend);
                a.rotation = nlerp(p.rotation, a.rotation, action->pose.blend);
                a.scale = glm::mix(p.scale, a.scale, action->pose.blend);
            }
            c.translation = glm::mix(c.translation, a.translation, action->weight);
            c.rotation = nlerp(c.rotation, a.rotation, action->weight);
            c.scale = glm::mix(c.scale, a.scale, action->weight);
        }
        // local = T(pivot) * T(translation) * R * S * T(-pivot)
        const glm::vec3 pivot(bone.pivot[0], bone.pivot[1], bone.pivot[2]);
        glm::mat3 rs = glm::mat3_cast(c.rotation);
        rs[0] *= c.scale.x;
        rs[1] *= c.scale.y;
        rs[2] *= c.scale.z;
        const glm::vec3 t = pivot + c.translation - rs * pivot;
        BoneMatrix local = fromAffine(rs, t);
        BoneMatrix world = bone.parent >= 0 ? multiply(out[bone.parent], local) : local;
        if ((bone.flags & (pack::kActorBoneBillboard | pack::kActorBoneBillboardZ)) &&
            glm::dot(cameraInModel, cameraInModel) > 0.5f) {
            // Faces the camera: the bone's +X towards it, +Z as close to up as possible, keeping its scale. About Z
            // only: +X towards it seen from above, +Z straight up.
            const glm::vec3 origin = transformPoint(world, pivot);
            const f32 scale = glm::length(glm::vec3(world.rows[0], world.rows[4], world.rows[8]));
            glm::vec3 x = cameraInModel;
            if (bone.flags & pack::kActorBoneBillboardZ) {
                const glm::vec2 flat(x.x, x.y);
                x = glm::dot(flat, flat) > 1e-6f ? glm::vec3(glm::normalize(flat), 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            }
            glm::vec3 z = glm::vec3(0.0f, 0.0f, 1.0f) - x * x.z;
            z = glm::dot(z, z) > 1e-6f ? glm::normalize(z) : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 y = glm::cross(z, x);
            glm::mat3 facing(x * scale, y * scale, z * scale);
            world = fromAffine(facing, origin - facing * pivot);
        }
        out[b] = world;
    }
}

void computeMaterialColors(const Skeleton& skeleton, const AnimPose& pose, u32 globalMs, glm::vec4* out) {
    const std::span<const pack::ActorMaterial> materials = skeleton.materials();
    const i32 sequence = pose.current.sequence;
    for (usize m = 0; m < materials.size(); ++m) {
        const pack::ActorMaterial& material = materials[m];
        if (material.colorTrack == pack::kActorNoColorTrack && material.alphaTrack == pack::kActorNoColorTrack &&
            material.transparencyTrack == pack::kActorNoColorTrack) {
            const u32 c = material.color;
            out[m] = glm::vec4(static_cast<f32>(c & 0xFF), static_cast<f32>((c >> 8) & 0xFF),
                               static_cast<f32>((c >> 16) & 0xFF), static_cast<f32>(c >> 24)) /
                     255.0f;
            continue;
        }
        const u32 s = sequence >= 0 ? static_cast<u32>(sequence) : 0;
        const u32 local = sequence >= 0 ? pose.current.time : 0;
        auto track = [](u16 t) { return t == pack::kActorNoColorTrack ? pack::kActorNoTrack : u32{t}; };
        const glm::vec3 rgb = skeleton.sampleVec3(track(material.colorTrack), s, local, globalMs, glm::vec3(1.0f));
        const f32 alpha = skeleton.sampleVec3(track(material.alphaTrack), s, local, globalMs, glm::vec3(1.0f)).x *
                          skeleton.sampleVec3(track(material.transparencyTrack), s, local, globalMs, glm::vec3(1.0f)).x;
        out[m] = glm::vec4(glm::clamp(rgb, 0.0f, 1.0f), std::clamp(alpha, 0.0f, 1.0f));
    }
}

} // namespace vd::anim
