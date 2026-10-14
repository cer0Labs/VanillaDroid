#pragma once

#include "core/Types.h"
#include "pack/ActorFormat.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

namespace vd::anim {

// Zero-copy view over a baked track table (pack/ActorFormat.h encoding: ActorTrack, per-sequence ActorRange,
// vec3 and quaternion keys, global sequence durations): the actors' bones and colors, and the effects of
// actors and doodads (pack/ParticleFormat.h). The blob must outlive the view. Sampling is read-only and
// thread-safe.
struct TrackTable {
    const pack::ActorTrack* tracks = nullptr;
    const pack::ActorRange* ranges = nullptr; // sequenceCount * slotCount
    const pack::ActorVec3Key* vec3Keys = nullptr;
    const pack::ActorQuatKey* quatKeys = nullptr;
    const u32* globalSequences = nullptr; // durations, ms
    u32 trackCount = 0;
    u32 sequenceCount = 0;
    u32 slotCount = 0;
    u32 vec3KeyCount = 0;
    u32 quatKeyCount = 0;
    u32 globalSequenceCount = 0;

    // Points the table into `blob` (offsets relative to it). Fails when a section does not fit or is
    // misaligned, or the range table has the wrong size. The tracks themselves are checked with trackValid().
    bool bind(const u8* blob, usize size, u32 trackCount, u32 tracksOffset, u32 rangeCount, u32 rangesOffset,
              u32 vec3KeyCount, u32 vec3KeysOffset, u32 quatKeyCount, u32 quatKeysOffset, u32 sequenceCount,
              u32 slotCount, u32 globalSequenceCount, u32 globalSequencesOffset);
    // Every index a sample of `track` follows is in range (pack::kActorNoTrack is valid: the fallback).
    bool trackValid(u32 track, bool quaternion) const;

    // Channel values. `local` is the time inside the sequence (ms from its start); tracks tied to a global
    // sequence use `globalMs` instead. pack::kActorNoTrack or a sequence out of range gives the fallback
    // (identity).
    glm::vec3 sampleVec3(u32 track, u32 sequence, u32 local, u32 globalMs, const glm::vec3& fallback) const;
    glm::quat sampleQuat(u32 track, u32 sequence, u32 local, u32 globalMs) const;
    f32 sampleScalar(u32 track, u32 sequence, u32 local, u32 globalMs, f32 fallback) const {
        return sampleVec3(track, sequence, local, globalMs, glm::vec3(fallback)).x;
    }

private:
    struct KeySpan {
        u32 first; // absolute index into the vec3 or quaternion keys
        u32 count;
        u32 time; // in the keys' own time
    };
    KeySpan keySpan(const pack::ActorTrack& track, u32 sequence, u32 local, u32 globalMs) const;
};

} // namespace vd::anim
