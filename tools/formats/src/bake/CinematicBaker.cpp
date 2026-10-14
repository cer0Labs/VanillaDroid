#include "bake/CinematicBaker.h"

#include "bake/BakeUtil.h"
#include "pack/CinematicFormat.h"

#include <algorithm>
#include <cstring>

namespace vd::bake {
namespace {

// The keys of `track` inside [start, end] of the timeline, times from `start`, appended to `keys`.
template <typename T, typename Tangents, typename Copy>
pack::CinematicTrack writeTrack(const formats::M2Track<T>& track, const std::vector<Tangents>& tangents, u32 start,
                                u32 end, std::vector<pack::CinematicKey>& keys, Copy copy) {
    pack::CinematicTrack out{};
    out.interpolation = track.interpolation;
    out.firstKey = static_cast<u32>(keys.size());
    for (usize k = 0; k < track.times.size(); ++k) {
        const u32 time = track.times[k];
        if (track.globalSequence < 0 && (time < start || time > end)) {
            continue;
        }
        pack::CinematicKey key{};
        key.time = track.globalSequence < 0 ? time - start : time;
        copy(track.values[k], key.value);
        if (k < tangents.size()) {
            copy(tangents[k][0], key.in);
            copy(tangents[k][1], key.out);
        } else {
            copy(track.values[k], key.in);
            copy(track.values[k], key.out);
        }
        keys.push_back(key);
    }
    out.keyCount = static_cast<u32>(keys.size()) - out.firstKey;
    return out;
}

void copyVec3(const formats::Vec3& v, f32 (&out)[3]) {
    std::memcpy(out, v.data(), sizeof(out));
}

void copyScalar(f32 v, f32 (&out)[3]) {
    out[0] = v;
    out[1] = 0.0f;
    out[2] = 0.0f;
}

} // namespace

Result<std::vector<u8>> bakeCinematicCamera(const formats::M2Model& model) {
    if (model.cameras.empty()) {
        return Error{"M2 without a camera"};
    }
    const formats::M2Camera& camera = model.cameras[0];
    const u32 start = model.sequences.empty() ? 0 : model.sequences[0].start;
    const u32 end = model.sequences.empty() ? 0 : model.sequences[0].end;

    pack::CinematicCameraHeader header{};
    header.version = pack::kCinematicFormatVersion;
    header.durationMs = end - start;
    header.fov = camera.fov;
    header.nearClip = camera.nearClip;
    header.farClip = camera.farClip;
    std::memcpy(header.positionBase, camera.positionBase.data(), sizeof(header.positionBase));
    std::memcpy(header.targetBase, camera.targetBase.data(), sizeof(header.targetBase));
    std::vector<pack::CinematicKey> keys;
    header.position = writeTrack(camera.position, camera.positionTangents, start, end, keys, copyVec3);
    header.target = writeTrack(camera.target, camera.targetTangents, start, end, keys, copyVec3);
    header.roll = writeTrack(camera.roll, camera.rollTangents, start, end, keys, copyScalar);
    if (header.durationMs == 0) {
        for (const pack::CinematicKey& key : keys) {
            header.durationMs = std::max(header.durationMs, key.time);
        }
    }

    BlobWriter writer(sizeof(header));
    header.keyCount = static_cast<u32>(keys.size());
    header.keysOffset = writer.append(keys);
    return writer.finish(header);
}

} // namespace vd::bake
