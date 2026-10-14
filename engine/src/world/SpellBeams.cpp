#include "world/SpellBeams.h"

#include "core/Profile.h"
#include "pack/ModelFormat.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace vd {
namespace {

u32 nextRandom(u32& state) {
    // xorshift32: never 0 once seeded with a nonzero value.
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// [-1, 1), like the client's (a random mantissa in [1, 2) moved by its sign bit, 0x7AF7F0).
f32 signedUnit(u32& state) {
    return static_cast<f32>(nextRandom(state) >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

u16 unorm16(f32 v) {
    return static_cast<u16>(std::clamp(v, 0.0f, 1.0f) * 65535.0f + 0.5f);
}

ParticleVertex vertex(const glm::vec3& p, f32 u, f32 v, u32 color) {
    return {{p.x, p.y, p.z}, {unorm16(u), unorm16(v)}, color};
}

// A quad of the strip between joints a and b: (a+, a-, b+, b-), the particle renderer's corner order.
void writeStripQuad(ParticleVertex* dst, const glm::vec3& aTop, const glm::vec3& aBottom,
                    const glm::vec3& bTop, const glm::vec3& bBottom, f32 ua, f32 ub, f32 va0, f32 va1,
                    f32 vb0, f32 vb1, u32 color) {
    dst[0] = vertex(aTop, ua, va0, color);
    dst[1] = vertex(aBottom, ua, va1, color);
    dst[2] = vertex(bTop, ub, vb0, color);
    dst[3] = vertex(bBottom, ub, vb1, color);
}

// The screen-space normal of a segment (the client's (-dy, dx, 0) in view space), unit, or zero when the
// segment points at the camera.
glm::vec3 screenNormal(const glm::vec3& d, const glm::vec3& forward) {
    const glm::vec3 n = glm::cross(forward, d);
    const f32 length = glm::length(n);
    return length > 1e-4f ? n / length : glm::vec3(0.0f);
}

constexpr u32 kWhite = 0xFFFFFFFFu;

} // namespace

u32 beamJoints(const game::ChainEffect& effect, const glm::vec3& from, const glm::vec3& to, u32 seed,
               std::span<glm::vec3> out) {
    if (out.size() < 2) {
        return 0;
    }
    const glm::vec3 d = to - from;
    const f32 length = glm::length(d);
    const u32 wanted = static_cast<u32>(std::max(0.0f, length / effect.segmentLength + 2.0f));
    const u32 segments = std::clamp(wanted, 1u, static_cast<u32>(out.size()) - 1);
    const f32 amplitude = length * effect.noise;
    u32 random = seed | 1u;
    out[0] = from;
    for (u32 i = 1; i < segments; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(segments);
        // The joint of this frame (0.75) blended with a second random one (0.25), as 0x7AFC08 does.
        glm::vec3 a, b;
        for (u32 c = 0; c < 3; ++c) {
            a[c] = signedUnit(random);
        }
        for (u32 c = 0; c < 3; ++c) {
            b[c] = signedUnit(random);
        }
        out[i] = from + d * t + (a * 0.75f + b * 0.25f) * amplitude;
    }
    out[segments] = to;
    return segments + 1;
}

f32 beamScroll(const game::ChainEffect& effect, f32 age) {
    if (effect.scroll == 0.0f) {
        return 0.0f;
    }
    // fmod keeps the sign of the age (positive): the offset runs over (-1, 0] or [0, 1) by the scroll's sign.
    return -(std::fmod(std::max(0.0f, age), effect.scroll) / effect.scroll);
}

u32 writeBeam(const BeamInstance& beam, const glm::dvec3& camera, const glm::vec3& cameraForward,
              std::span<ParticleVertex> out) {
    if (!beam.effect || out.size() < 4) {
        return 0;
    }
    const game::ChainEffect& effect = *beam.effect;
    std::array<glm::vec3, kMaxBeamJoints> joints;
    const glm::vec3 from(beam.from - camera);
    const glm::vec3 to(beam.to - camera);
    const u32 count = beamJoints(effect, from, to, beam.seed, joints);
    if (count < 2) {
        return 0;
    }
    // Each joint widened by half of each neighbouring segment's normal times the width (0x7B0150); the two
    // ends stay on their point (0x7B04B9).
    std::array<glm::vec3, kMaxBeamJoints> side;
    side.fill(glm::vec3(0.0f));
    for (u32 i = 0; i + 1 < count; ++i) {
        const glm::vec3 n = screenNormal(joints[i + 1] - joints[i], cameraForward) * (effect.width * 0.5f);
        side[i] += n;
        side[i + 1] += n;
    }
    side[0] = glm::vec3(0.0f);
    side[count - 1] = glm::vec3(0.0f);

    const f32 offset = beamScroll(effect, beam.age);
    const u32 maxQuads = static_cast<u32>(std::min<usize>(out.size() / 4, kMaxBeamQuads));
    const f32 step = 1.0f / static_cast<f32>(count - 1);
    u32 written = 0;
    // v across the strip; the pointed ends sample the middle of the texture (v 0.5, 0x7AFB5D).
    const auto vTop = [&](u32 i) { return i == 0 || i + 1 == count ? 0.5f : 0.0f; };
    const auto vBottom = [&](u32 i) { return i == 0 || i + 1 == count ? 0.5f : 1.0f; };
    for (u32 i = 0; i + 1 < count && written < maxQuads; ++i) {
        const glm::vec3 aTop = joints[i] + side[i], aBottom = joints[i] - side[i];
        const glm::vec3 bTop = joints[i + 1] + side[i + 1], bBottom = joints[i + 1] - side[i + 1];
        // The texture coordinate along the beam, scrolled: it wraps once somewhere along the strip.
        const f32 sa = static_cast<f32>(i) * step + offset;
        const f32 sb = static_cast<f32>(i + 1) * step + offset;
        const f32 base = std::floor(sa);
        const f32 cross = base + 1.0f;
        if (sb > cross + 1e-6f && written + 2 <= maxQuads) {
            // Split where it wraps: [sa, cross] ends at u 1, [cross, sb] starts at u 0.
            const f32 f = (cross - sa) / (sb - sa);
            const glm::vec3 mTop = aTop + (bTop - aTop) * f, mBottom = aBottom + (bBottom - aBottom) * f;
            const f32 mv0 = vTop(i) + (vTop(i + 1) - vTop(i)) * f;
            const f32 mv1 = vBottom(i) + (vBottom(i + 1) - vBottom(i)) * f;
            writeStripQuad(out.data() + written * 4, aTop, aBottom, mTop, mBottom, sa - base, 1.0f, vTop(i),
                           vBottom(i), mv0, mv1, kWhite);
            ++written;
            writeStripQuad(out.data() + written * 4, mTop, mBottom, bTop, bBottom, 0.0f, sb - cross, mv0, mv1,
                           vTop(i + 1), vBottom(i + 1), kWhite);
            ++written;
            continue;
        }
        const f32 ua = sa - base;
        const f32 ub = std::min(sb - base, 1.0f);
        writeStripQuad(out.data() + written * 4, aTop, aBottom, bTop, bBottom, ua, ub, vTop(i), vBottom(i),
                       vTop(i + 1), vBottom(i + 1), kWhite);
        ++written;
    }
    return written;
}

u32 writeLine(const glm::dvec3& from, const glm::dvec3& to, f32 sag, const glm::dvec3& camera,
              const glm::vec3& cameraForward, f32 pixelsPerYard, f32 pixels, std::span<ParticleVertex> out) {
    const u32 maxQuads = static_cast<u32>(std::min<usize>(out.size() / 4, kFishingLinePieces));
    if (maxQuads == 0) {
        return 0;
    }
    const glm::vec3 a(from - camera);
    const glm::vec3 d(to - from);
    std::array<glm::vec3, kFishingLinePieces + 1> points;
    for (u32 i = 0; i <= kFishingLinePieces; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(kFishingLinePieces);
        points[i] = a + d * t;
        points[i].z += std::sin(t * 3.14159265f) * -sag; // 0x61F991: sin(pi t) * -0.5
    }
    const f32 yardsPerPixel = 1.0f / std::max(pixelsPerYard, 1.0f);
    const auto halfWidth = [&](const glm::vec3& p) { return 0.5f * pixels * glm::length(p) * yardsPerPixel; };
    u32 written = 0;
    for (u32 i = 0; i < kFishingLinePieces && written < maxQuads; ++i) {
        const glm::vec3 n = screenNormal(points[i + 1] - points[i], cameraForward);
        const glm::vec3 wa = n * halfWidth(points[i]), wb = n * halfWidth(points[i + 1]);
        writeStripQuad(out.data() + written * 4, points[i] + wa, points[i] - wa, points[i + 1] + wb,
                       points[i + 1] - wb, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, kWhite);
        ++written;
    }
    return written;
}

void BeamWriter::init() {
    m_scratch.assign(u64{kMaxQuads} * 4, ParticleVertex{});
    m_runs.clear();
    m_runs.reserve(256);
}

void BeamWriter::begin(const glm::dvec3& camera, const glm::vec3& cameraForward, f32 pixelsPerYard,
                       f32 viewportHeight) {
    m_camera = camera;
    m_forward = cameraForward;
    m_pixelsPerYard = pixelsPerYard;
    // The client's one-pixel line on its 768 lines, as thick on this screen (at least a pixel).
    m_linePixels = std::max(1.0f, viewportHeight / 768.0f);
    m_quads = 0;
    m_beams = 0;
    m_runs.clear();
}

void BeamWriter::addRun(TextureHandle texture, u8 blend, u8 flags, u32 quads) {
    if (quads == 0) {
        return;
    }
    if (!m_runs.empty() && m_runs.back().texture.index == texture.index && m_runs.back().blend == blend &&
        m_runs.back().flags == flags && m_runs.back().firstQuad + m_runs.back().quadCount == m_quads) {
        m_runs.back().quadCount += quads;
    } else if (m_runs.size() < m_runs.capacity()) {
        m_runs.push_back({texture, blend, flags, m_quads, quads});
    } else {
        return; // no room for another draw: not drawn
    }
    m_quads += quads;
}

void BeamWriter::addBeams(std::span<const BeamInstance> beams, TextureLookup texture, void* context) {
    if (m_scratch.empty()) {
        return;
    }
    VD_PROFILE_ZONE("BeamWriter::addBeams");
    for (const BeamInstance& beam : beams) {
        if (!beam.effect || m_quads + kMaxBeamQuads > kMaxQuads) {
            continue;
        }
        const TextureHandle handle = texture ? texture(context, beam.effect->texture) : TextureHandle{};
        if (!handle.valid()) {
            continue; // its texture is still loading
        }
        const u32 quads = writeBeam(beam, m_camera, m_forward,
                                    std::span(m_scratch).subspan(u64{m_quads} * 4, u64{kMaxBeamQuads} * 4));
        addRun(handle, static_cast<u8>(pack::ModelBlend::Add), 0, quads);
        m_beams += quads > 0 ? 1 : 0;
    }
}

void BeamWriter::addLine(const glm::dvec3& from, const glm::dvec3& to) {
    if (m_scratch.empty() || m_quads + kFishingLinePieces > kMaxQuads) {
        return;
    }
    const u32 quads = writeLine(from, to, kFishingLineSag, m_camera, m_forward, m_pixelsPerYard, m_linePixels,
                                std::span(m_scratch).subspan(u64{m_quads} * 4, u64{kFishingLinePieces} * 4));
    addRun(TextureHandle{}, static_cast<u8>(pack::ModelBlend::Alpha), kParticleDrawLit, quads);
}

u32 BeamWriter::finish(std::span<ParticleVertex> vertices, u32 firstQuad, std::vector<ParticleDraw>& draws) {
    if (m_quads == 0 || vertices.size() < u64{firstQuad} * 4) {
        return firstQuad;
    }
    const u32 room = static_cast<u32>(vertices.size() / 4) - firstQuad;
    const u32 quads = std::min(m_quads, room);
    std::memcpy(vertices.data() + u64{firstQuad} * 4, m_scratch.data(),
                u64{quads} * 4 * sizeof(ParticleVertex));
    for (const Run& run : m_runs) {
        if (run.firstQuad >= quads) {
            break;
        }
        ParticleDraw draw;
        draw.texture = run.texture;
        draw.blend = run.blend;
        draw.flags = run.flags;
        draw.firstQuad = firstQuad + run.firstQuad;
        draw.quadCount = std::min(run.quadCount, quads - run.firstQuad);
        draws.push_back(draw);
    }
    return firstQuad + quads;
}

} // namespace vd
