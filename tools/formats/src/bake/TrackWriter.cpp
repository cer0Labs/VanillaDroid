#include "bake/TrackWriter.h"

#include "bake/BakeUtil.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace vd::bake {
namespace {

using Value4 = TrackWriter::Value4;

template <typename T> bool allEqual(const std::vector<T>& values) {
    return std::all_of(values.begin(), values.end(), [&](const T& v) { return v == values.front(); });
}

Value4 toValue4(const formats::Vec3& v) {
    return {v[0], v[1], v[2], 0.0f};
}

Value4 toValue4(const formats::Quat& q) {
    return {q[0], q[1], q[2], q[3]};
}

Value4 interpolate(const Value4& a, Value4 b, f32 t, bool quaternion) {
    if (quaternion) {
        if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0.0f) {
            for (f32& c : b) {
                c = -c;
            }
        }
        Value4 out;
        f32 length = 0.0f;
        for (u32 i = 0; i < 4; ++i) {
            out[i] = a[i] + (b[i] - a[i]) * t;
            length += out[i] * out[i];
        }
        length = std::sqrt(length);
        for (f32& c : out) {
            c = length > 1e-6f ? c / length : c;
        }
        return out;
    }
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t, 0.0f};
}

// Value on the original timeline, searching keys [first, last] and holding outside them (the client's rule).
template <typename T>
Value4 sampleAt(const formats::M2Track<T>& track, u32 first, u32 last, u32 time, u8 interpolation) {
    constexpr bool quaternion = std::is_same_v<T, formats::Quat>;
    if (first == last || time <= track.times[first]) {
        return toValue4(track.values[first]);
    }
    if (time >= track.times[last]) {
        return toValue4(track.values[last]);
    }
    u32 k = first;
    while (k + 1 < last && track.times[k + 1] <= time) {
        ++k;
    }
    if (interpolation == 0) {
        return toValue4(track.values[k]);
    }
    const u32 span = track.times[k + 1] - track.times[k];
    const f32 t = span ? static_cast<f32>(time - track.times[k]) / static_cast<f32>(span) : 0.0f;
    return interpolate(toValue4(track.values[k]), toValue4(track.values[k + 1]), t, quaternion);
}

} // namespace

u32 TrackWriter::add(const formats::M2Track<formats::Vec3>& track, const formats::Vec3& identity) {
    return addTrack(track, identity);
}

u32 TrackWriter::add(const formats::M2Track<formats::Quat>& track, const formats::Quat& identity) {
    return addTrack(track, identity);
}

template <typename T> u32 TrackWriter::addTrack(const formats::M2Track<T>& track, const T& identity) {
    constexpr bool quaternion = std::is_same_v<T, formats::Quat>;
    if (track.times.empty() || (allEqual(track.values) && track.values.front() == identity)) {
        return pack::kActorNoTrack;
    }
    pack::ActorTrack out{};
    out.interpolation = static_cast<u8>(std::min<u16>(track.interpolation, 1));
    out.globalSequence = -1;
    const u32 keyCount = static_cast<u32>(track.times.size());
    if (allEqual(track.values) || m_sequences.empty()) {
        out.kind = static_cast<u8>(pack::ActorTrackKind::Constant);
        out.firstKey = appendKey(0, toValue4(track.values.front()), quaternion);
        out.keyCount = 1;
    } else if (track.globalSequence >= 0) {
        out.kind = static_cast<u8>(pack::ActorTrackKind::Global);
        out.globalSequence = track.globalSequence;
        out.firstKey = static_cast<u32>(quaternion ? m_quatKeys.size() : m_vec3Keys.size());
        for (u32 k = 0; k < keyCount; ++k) {
            appendKey(track.times[k], toValue4(track.values[k]), quaternion);
        }
        out.keyCount = keyCount;
    } else {
        out.kind = static_cast<u8>(pack::ActorTrackKind::Sequence);
        out.slot = static_cast<u32>(m_pending.size());
        Pending& pending = m_pending.emplace_back();
        pending.quaternion = quaternion;
        pending.perSequence.resize(m_sequences.size());
        for (u32 s = 0; s < m_sequences.size(); ++s) {
            localKeys(track, s, out.interpolation, pending.perSequence[s]);
        }
    }
    m_tracks.push_back(out);
    return static_cast<u32>(m_tracks.size() - 1);
}

void TrackWriter::finish() {
    m_ranges.clear();
    for (u32 s = 0; s < m_sequences.size(); ++s) {
        for (const Pending& pending : m_pending) {
            const std::vector<LocalKey>& keys = pending.perSequence[s];
            pack::ActorRange range{};
            range.first = static_cast<u32>(pending.quaternion ? m_quatKeys.size() : m_vec3Keys.size());
            range.count = static_cast<u32>(keys.size());
            for (const LocalKey& key : keys) {
                appendKey(key.time, key.value, pending.quaternion);
            }
            m_ranges.push_back(range);
        }
    }
}

// Keys of sequence `s` in its own time: the value at its start, the keys strictly inside, the value at its
// end.
template <typename T>
void TrackWriter::localKeys(const formats::M2Track<T>& track, u32 s, u8 interpolation,
                            std::vector<LocalKey>& out) const {
    const u32 keyCount = static_cast<u32>(track.times.size());
    u32 first = 0;
    u32 last = keyCount - 1;
    const u32 range = s < m_modelSequences.size() ? m_modelSequences[s] : s; // the model's sequence index
    if (range < track.ranges.size()) {
        first = std::min(track.ranges[range][0], keyCount - 1);
        last = std::clamp(track.ranges[range][1], first, keyCount - 1);
    }
    const u32 start = m_sequences[s].start;
    const u32 end = m_sequences[s].end;
    out.push_back({0, sampleAt(track, first, last, start, interpolation)});
    for (u32 k = first; k <= last; ++k) {
        if (track.times[k] > start && track.times[k] < end) {
            out.push_back({track.times[k] - start, toValue4(track.values[k])});
        }
    }
    if (end > start) {
        out.push_back({end - start, sampleAt(track, first, last, end, interpolation)});
    }
    const bool constant = std::all_of(out.begin(), out.end(),
                                      [&](const LocalKey& key) { return key.value == out.front().value; });
    if (constant) {
        out.resize(1);
    }
}

u32 TrackWriter::appendKey(u32 time, const Value4& v, bool quaternion) {
    if (!quaternion) {
        m_vec3Keys.push_back({time, {v[0], v[1], v[2]}});
        return static_cast<u32>(m_vec3Keys.size() - 1);
    }
    f32 length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]);
    if (length < 1e-6f) {
        length = 1.0f;
    }
    pack::ActorQuatKey key{};
    key.time = time;
    for (u32 i = 0; i < 4; ++i) {
        key.value[i] = toSnorm16(v[i] / length);
    }
    m_quatKeys.push_back(key);
    return static_cast<u32>(m_quatKeys.size() - 1);
}

} // namespace vd::bake
