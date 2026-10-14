#pragma once

#include "core/Types.h"
#include "formats/M2.h"
#include "pack/ActorFormat.h"

#include <array>
#include <utility>
#include <vector>

namespace vd::bake {

// Baked tracks with their keys regrouped by sequence (pack::ActorTrackKind, see ActorFormat.h): the actors'
// bones and material colors, and the effects (particle parameters, ribbons, texture animations) of actors and
// doodads.
class TrackWriter {
public:
    using Value4 = std::array<f32, 4>;

    // `sequences`: the sequences the tracks are cut into (all of an actor's; the loop of a doodad). `modelSequences`
    // (phase 14): the index of each in the model, whose key ranges the tracks use; empty when they are all of them.
    explicit TrackWriter(const std::vector<formats::M2Sequence>& sequences, std::vector<u32> modelSequences = {})
        : m_sequences(sequences), m_modelSequences(std::move(modelSequences)) {}

    // Returns the track index, or kActorNoTrack for a channel that never moves from `identity`.
    u32 add(const formats::M2Track<formats::Vec3>& track, const formats::Vec3& identity);
    u32 add(const formats::M2Track<formats::Quat>& track, const formats::Quat& identity);

    // Lays the per-sequence keys out sequence by sequence, with their range table. Call once, after every
    // add().
    void finish();

    u32 slotCount() const { return static_cast<u32>(m_pending.size()); }

    std::vector<pack::ActorTrack> m_tracks;
    std::vector<pack::ActorRange> m_ranges;
    std::vector<pack::ActorVec3Key> m_vec3Keys;
    std::vector<pack::ActorQuatKey> m_quatKeys;

private:
    struct LocalKey {
        u32 time;
        Value4 value;
    };
    struct Pending {
        bool quaternion = false;
        std::vector<std::vector<LocalKey>> perSequence;
    };

    template <typename T> u32 addTrack(const formats::M2Track<T>& track, const T& identity);
    template <typename T>
    void localKeys(const formats::M2Track<T>& track, u32 s, u8 interpolation,
                   std::vector<LocalKey>& out) const;
    u32 appendKey(u32 time, const Value4& v, bool quaternion);

    const std::vector<formats::M2Sequence>& m_sequences;
    std::vector<u32> m_modelSequences;
    std::vector<Pending> m_pending;
};

// A scalar M2 track as the vec3 track the baked encoding stores (value in x), scaled by `scale`.
template <typename T>
formats::M2Track<formats::Vec3> toScalarTrack(const formats::M2Track<T>& track, f32 scale = 1.0f) {
    formats::M2Track<formats::Vec3> out;
    out.interpolation = track.interpolation;
    out.globalSequence = track.globalSequence;
    out.ranges = track.ranges;
    out.times = track.times;
    for (const T& v : track.values) {
        out.values.push_back({static_cast<f32>(v) * scale, 0.0f, 0.0f});
    }
    return out;
}

} // namespace vd::bake
