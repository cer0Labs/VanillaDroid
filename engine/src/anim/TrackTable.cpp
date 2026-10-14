#include "anim/TrackTable.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace vd::anim {
namespace {

template <typename T> bool sectionFits(u32 offset, u32 count, usize size) {
    return count == 0 || (offset % alignof(T) == 0 && u64{offset} + u64{count} * sizeof(T) <= size);
}

glm::quat toQuat(const pack::ActorQuatKey& key) {
    constexpr f32 kScale = 1.0f / 32767.0f;
    return glm::quat(key.value[3] * kScale, key.value[0] * kScale, key.value[1] * kScale,
                     key.value[2] * kScale);
}

glm::quat nlerp(const glm::quat& a, glm::quat b, f32 t) {
    if (glm::dot(a, b) < 0.0f) {
        b = -b;
    }
    return glm::normalize(a * (1.0f - t) + b * t);
}

} // namespace

bool TrackTable::bind(const u8* blob, usize size, u32 trackCount_, u32 tracksOffset, u32 rangeCount,
                      u32 rangesOffset, u32 vec3KeyCount_, u32 vec3KeysOffset, u32 quatKeyCount_,
                      u32 quatKeysOffset, u32 sequenceCount_, u32 slotCount_, u32 globalSequenceCount_,
                      u32 globalSequencesOffset) {
    *this = TrackTable{};
    if (!sectionFits<pack::ActorTrack>(tracksOffset, trackCount_, size) ||
        !sectionFits<pack::ActorRange>(rangesOffset, rangeCount, size) ||
        !sectionFits<pack::ActorVec3Key>(vec3KeysOffset, vec3KeyCount_, size) ||
        !sectionFits<pack::ActorQuatKey>(quatKeysOffset, quatKeyCount_, size) ||
        !sectionFits<u32>(globalSequencesOffset, globalSequenceCount_, size) ||
        u64{sequenceCount_} * slotCount_ != rangeCount) {
        return false;
    }
    tracks = reinterpret_cast<const pack::ActorTrack*>(blob + tracksOffset);
    ranges = reinterpret_cast<const pack::ActorRange*>(blob + rangesOffset);
    vec3Keys = reinterpret_cast<const pack::ActorVec3Key*>(blob + vec3KeysOffset);
    quatKeys = reinterpret_cast<const pack::ActorQuatKey*>(blob + quatKeysOffset);
    globalSequences = reinterpret_cast<const u32*>(blob + globalSequencesOffset);
    trackCount = trackCount_;
    sequenceCount = sequenceCount_;
    slotCount = slotCount_;
    vec3KeyCount = vec3KeyCount_;
    quatKeyCount = quatKeyCount_;
    globalSequenceCount = globalSequenceCount_;
    return true;
}

bool TrackTable::trackValid(u32 track, bool quaternion) const {
    if (track == pack::kActorNoTrack) {
        return true;
    }
    if (track >= trackCount) {
        return false;
    }
    const pack::ActorTrack& t = tracks[track];
    const u32 keys = quaternion ? quatKeyCount : vec3KeyCount;
    switch (static_cast<pack::ActorTrackKind>(t.kind)) {
    case pack::ActorTrackKind::Constant: return t.keyCount == 1 && t.firstKey < keys;
    case pack::ActorTrackKind::Global:
        return t.keyCount > 0 && u64{t.firstKey} + t.keyCount <= keys && t.globalSequence >= 0 &&
               static_cast<u32>(t.globalSequence) < globalSequenceCount;
    case pack::ActorTrackKind::Sequence:
        if (t.slot >= slotCount) {
            return false;
        }
        for (u32 seq = 0; seq < sequenceCount; ++seq) {
            const pack::ActorRange& r = ranges[u64{seq} * slotCount + t.slot];
            if (r.count == 0 || u64{r.first} + r.count > keys) {
                return false;
            }
        }
        return true;
    default: return false;
    }
}

TrackTable::KeySpan TrackTable::keySpan(const pack::ActorTrack& track, u32 sequence, u32 local,
                                        u32 globalMs) const {
    switch (static_cast<pack::ActorTrackKind>(track.kind)) {
    case pack::ActorTrackKind::Global: {
        const u32 period = globalSequences[track.globalSequence];
        return {track.firstKey, track.keyCount, period ? globalMs % period : 0};
    }
    case pack::ActorTrackKind::Sequence: {
        const pack::ActorRange& r = ranges[sequence * slotCount + track.slot];
        return {r.first, r.count, local};
    }
    default: return {track.firstKey, 1, 0};
    }
}

glm::vec3 TrackTable::sampleVec3(u32 trackIndex, u32 sequence, u32 local, u32 globalMs,
                                 const glm::vec3& fallback) const {
    if (trackIndex == pack::kActorNoTrack) {
        return fallback;
    }
    const pack::ActorTrack& track = tracks[trackIndex];
    if (track.kind == static_cast<u8>(pack::ActorTrackKind::Sequence) && sequence >= sequenceCount) {
        return fallback;
    }
    const KeySpan span = keySpan(track, sequence, local, globalMs);
    const pack::ActorVec3Key* keys = vec3Keys + span.first;
    auto value = [&](u32 k) { return glm::vec3(keys[k].value[0], keys[k].value[1], keys[k].value[2]); };
    const u32 last = span.count - 1;
    if (last == 0 || span.time <= keys[0].time) {
        return value(0);
    }
    if (span.time >= keys[last].time) {
        return value(last);
    }
    // Last key with time <= span.time, in [0, last - 1].
    u32 lo = 0;
    u32 hi = last;
    while (hi - lo > 1) {
        const u32 mid = (lo + hi) / 2;
        if (keys[mid].time <= span.time) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    if (track.interpolation == 0) {
        return value(lo);
    }
    const u32 width = keys[lo + 1].time - keys[lo].time;
    const f32 t = width ? static_cast<f32>(span.time - keys[lo].time) / static_cast<f32>(width) : 0.0f;
    return glm::mix(value(lo), value(lo + 1), t);
}

glm::quat TrackTable::sampleQuat(u32 trackIndex, u32 sequence, u32 local, u32 globalMs) const {
    if (trackIndex == pack::kActorNoTrack) {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    const pack::ActorTrack& track = tracks[trackIndex];
    if (track.kind == static_cast<u8>(pack::ActorTrackKind::Sequence) && sequence >= sequenceCount) {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    const KeySpan span = keySpan(track, sequence, local, globalMs);
    const pack::ActorQuatKey* keys = quatKeys + span.first;
    const u32 last = span.count - 1;
    if (last == 0 || span.time <= keys[0].time) {
        return glm::normalize(toQuat(keys[0]));
    }
    if (span.time >= keys[last].time) {
        return glm::normalize(toQuat(keys[last]));
    }
    u32 lo = 0;
    u32 hi = last;
    while (hi - lo > 1) {
        const u32 mid = (lo + hi) / 2;
        if (keys[mid].time <= span.time) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    if (track.interpolation == 0) {
        return glm::normalize(toQuat(keys[lo]));
    }
    const u32 width = keys[lo + 1].time - keys[lo].time;
    const f32 t = width ? static_cast<f32>(span.time - keys[lo].time) / static_cast<f32>(width) : 0.0f;
    return nlerp(toQuat(keys[lo]), toQuat(keys[lo + 1]), t);
}

} // namespace vd::anim
