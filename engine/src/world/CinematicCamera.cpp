#include "world/CinematicCamera.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vd {
namespace {

glm::vec3 vec(const f32 (&v)[3]) {
    return {v[0], v[1], v[2]};
}

bool trackFits(const pack::CinematicTrack& track, u32 keyCount) {
    return u64{track.firstKey} + track.keyCount <= keyCount;
}

} // namespace

Result<void> CinematicCamera::bind(const u8* blob, usize size) {
    m_header = nullptr;
    m_keys = nullptr;
    if (size < sizeof(pack::CinematicCameraHeader) || reinterpret_cast<uintptr_t>(blob) % 4 != 0) {
        return Error{"Cinematic camera blob too small or misaligned"};
    }
    const auto* h = reinterpret_cast<const pack::CinematicCameraHeader*>(blob);
    if (h->version != pack::kCinematicFormatVersion) {
        return Error{"Cinematic camera format version mismatch (re-bake cinematics)"};
    }
    if ((h->keyCount != 0 && (h->keysOffset % 4 != 0 ||
                              u64{h->keysOffset} + u64{h->keyCount} * sizeof(pack::CinematicKey) > size)) ||
        !trackFits(h->position, h->keyCount) || !trackFits(h->target, h->keyCount) || !trackFits(h->roll, h->keyCount)) {
        return Error{"Corrupt cinematic camera"};
    }
    m_header = h;
    m_keys = reinterpret_cast<const pack::CinematicKey*>(blob + h->keysOffset);
    return {};
}

glm::vec3 CinematicCamera::sampleTrack(const pack::CinematicTrack& track, u32 ms) const {
    if (track.keyCount == 0) {
        return glm::vec3(0.0f);
    }
    const pack::CinematicKey* keys = m_keys + track.firstKey;
    const u32 count = track.keyCount;
    if (ms <= keys[0].time || count == 1) {
        return vec(keys[0].value);
    }
    if (ms >= keys[count - 1].time) {
        return vec(keys[count - 1].value);
    }
    u32 k = 0;
    while (k + 2 < count && keys[k + 1].time <= ms) {
        ++k;
    }
    const pack::CinematicKey& a = keys[k];
    const pack::CinematicKey& b = keys[k + 1];
    const u32 span = b.time - a.time;
    const f32 t = span ? static_cast<f32>(ms - a.time) / static_cast<f32>(span) : 0.0f;
    const glm::vec3 p0 = vec(a.value);
    const glm::vec3 p3 = vec(b.value);
    switch (static_cast<pack::CinematicInterpolation>(track.interpolation)) {
    case pack::CinematicInterpolation::Step:
        return p0;
    case pack::CinematicInterpolation::Linear:
        return p0 + (p3 - p0) * t;
    case pack::CinematicInterpolation::Bezier: {
        // The handles are points: a's out-handle and b's in-handle.
        const glm::vec3 p1 = vec(a.out);
        const glm::vec3 p2 = vec(b.in);
        const f32 u = 1.0f - t;
        return u * u * u * p0 + 3.0f * u * u * t * p1 + 3.0f * u * t * t * p2 + t * t * t * p3;
    }
    case pack::CinematicInterpolation::Hermite: {
        const f32 t2 = t * t;
        const f32 t3 = t2 * t;
        return (2.0f * t3 - 3.0f * t2 + 1.0f) * p0 + (t3 - 2.0f * t2 + t) * vec(a.out) +
               (-2.0f * t3 + 3.0f * t2) * p3 + (t3 - t2) * vec(b.in);
    }
    }
    return p0 + (p3 - p0) * t;
}

CinematicView CinematicCamera::sample(u32 ms, const glm::dvec3& origin, f32 facing) const {
    CinematicView view;
    if (!m_header) {
        view.eye = origin;
        view.target = origin + glm::dvec3(1.0, 0.0, 0.0);
        return view;
    }
    const glm::vec3 eye = vec(m_header->positionBase) + sampleTrack(m_header->position, ms);
    const glm::vec3 target = vec(m_header->targetBase) + sampleTrack(m_header->target, ms);
    const f64 c = std::cos(static_cast<f64>(facing));
    const f64 s = std::sin(static_cast<f64>(facing));
    const auto place = [&](const glm::vec3& p) {
        return origin + glm::dvec3(c * p.x - s * p.y, s * p.x + c * p.y, static_cast<f64>(p.z));
    };
    view.eye = place(eye);
    view.target = place(target);
    // Rolls are authored around a whole turn (6.28 = level): keep the part that tilts.
    view.roll = std::remainder(sampleTrack(m_header->roll, ms).x, 6.2831853f);
    view.fovDiagonal = m_header->fov > 0.01f ? m_header->fov : view.fovDiagonal;
    view.nearClip = m_header->nearClip > 0.0f ? m_header->nearClip : view.nearClip;
    view.farClip = m_header->farClip > view.nearClip ? m_header->farClip : view.farClip;
    return view;
}

f32 verticalFov(f32 diagonal, f32 aspect) {
    const f32 d = std::clamp(diagonal, 0.05f, 3.0f);
    return 2.0f * std::atan(std::tan(d * 0.5f) / std::sqrt(1.0f + aspect * aspect));
}

} // namespace vd
