#include "fx/EffectSet.h"

#include "core/Hash.h"
#include "pack/ActorFormat.h"
#include "pack/ModelFormat.h"

#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace vd::fx {
namespace {

static_assert(pack::kEffectNoTrack == pack::kActorNoTrack, "effect tracks are actor tracks");

constexpr u32 kMinCapacity = 4;
constexpr u32 kMaxCapacity = 512; // per emitter, whatever its rate (a few spells ask for thousands)
constexpr u32 kMaxMeshCapacity = 64; // phase 14: particles that are models (each is tens of triangles)

template <typename T> bool sectionFits(u32 offset, u32 count, usize size) {
    return count == 0 || (offset % alignof(T) == 0 && u64{offset} + u64{count} * sizeof(T) <= size);
}

// Largest value a scalar track (or its in-place value) takes.
f32 largestValue(const anim::TrackTable& table, u32 track, f32 value) {
    if (track == pack::kEffectNoTrack) {
        return value;
    }
    const pack::ActorTrack& t = table.tracks[track];
    f32 out = -1e30f;
    auto scan = [&](u32 first, u32 count) {
        for (u32 k = first; k < first + count; ++k) {
            out = std::max(out, table.vec3Keys[k].value[0]);
        }
    };
    if (t.kind == static_cast<u8>(pack::ActorTrackKind::Sequence)) {
        for (u32 s = 0; s < table.sequenceCount; ++s) {
            const pack::ActorRange& r = table.ranges[s * table.slotCount + t.slot];
            scan(r.first, r.count);
        }
    } else {
        scan(t.firstKey, t.keyCount);
    }
    return out > -1e30f ? out : value;
}

} // namespace

UvTransform uvTransform(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale) {
    // T(translation) * C * R * S * C^-1, C = translate(0.5, 0.5): rotation and scale about the texture's
    // center.
    glm::mat4 m = glm::translate(glm::mat4(1.0f), translation + glm::vec3(0.5f, 0.5f, 0.0f));
    m = m * glm::mat4_cast(rotation);
    m = glm::scale(m, scale);
    m = glm::translate(m, glm::vec3(-0.5f, -0.5f, 0.0f));
    UvTransform out;
    out.rows[0] = m[0][0];
    out.rows[1] = m[1][0];
    out.rows[2] = m[3][0];
    out.rows[3] = 0.0f;
    out.rows[4] = m[0][1];
    out.rows[5] = m[1][1];
    out.rows[6] = m[3][1];
    out.rows[7] = 0.0f;
    return out;
}

u64 effectModelHash(std::string_view clientPath) {
    while (!clientPath.empty() && (clientPath.front() == '\\' || clientPath.front() == '/')) {
        clientPath.remove_prefix(1);
    }
    const usize dot = clientPath.find_last_of('.');
    std::string path(clientPath.substr(0, dot));
    if (path.empty()) {
        return 0;
    }
    path += ".m2";
    return assetPathHash(normalizeAssetPath(path));
}

Result<void> EffectSet::bindModel(const u8* blob, usize size) {
    *this = EffectSet{};
    pack::ModelHeader h;
    if (size < sizeof(h)) {
        return Error{"Model blob too small"};
    }
    std::memcpy(&h, blob, sizeof(h));
    if (h.version != pack::kModelFormatVersion) {
        return Error{"Model format version mismatch (re-bake models)"};
    }
    if (h.effectsOffset == 0) {
        return {};
    }
    if (!m_tracks.bind(blob, size, h.trackCount, h.tracksOffset, h.rangeCount, h.rangesOffset, h.vec3KeyCount,
                       h.vec3KeysOffset, h.quatKeyCount, h.quatKeysOffset, h.sequenceCount,
                       h.sequenceSlotCount, h.globalSequenceCount, h.globalSequencesOffset)) {
        return Error{"Corrupt model effect tracks"};
    }
    m_loopMs = h.sequenceCount > 0 ? std::max(1u, h.sequenceDuration) : 0;
    return bindSections(blob, size, h.effectsOffset, 0);
}

Result<void> EffectSet::bindActor(const u8* blob, usize size, const anim::TrackTable& tracks, u32 boneCount) {
    *this = EffectSet{};
    pack::ActorModelHeader h;
    if (size < sizeof(h)) {
        return Error{"Actor blob too small"};
    }
    std::memcpy(&h, blob, sizeof(h));
    if (h.version != pack::kActorFormatVersion) {
        return Error{"Actor format version mismatch (re-bake actors)"};
    }
    if (h.effectsOffset == 0) {
        return {};
    }
    m_tracks = tracks;
    m_actor = true;
    return bindSections(blob, size, h.effectsOffset, boneCount);
}

Result<void> EffectSet::bindSections(const u8* blob, usize size, u32 effectsOffset, u32 boneCount) {
    if (!sectionFits<pack::EffectsHeader>(effectsOffset, 1, size)) {
        return Error{"Corrupt effects header"};
    }
    const auto* h = reinterpret_cast<const pack::EffectsHeader*>(blob + effectsOffset);
    if (!sectionFits<pack::ParticleEmitter>(h->emittersOffset, h->emitterCount, size) ||
        !sectionFits<pack::RibbonEmitter>(h->ribbonsOffset, h->ribbonCount, size) ||
        !sectionFits<pack::UvAnimation>(h->uvAnimationsOffset, h->uvAnimationCount, size) ||
        !sectionFits<f32>(h->splinePointsOffset, h->splinePointCount * 3, size)) {
        return Error{"Corrupt effects sections"};
    }
    const std::span<const pack::ParticleEmitter> emitters(
        reinterpret_cast<const pack::ParticleEmitter*>(blob + h->emittersOffset), h->emitterCount);
    const std::span<const pack::RibbonEmitter> ribbons(
        reinterpret_cast<const pack::RibbonEmitter*>(blob + h->ribbonsOffset), h->ribbonCount);
    const std::span<const pack::UvAnimation> uvAnimations(
        reinterpret_cast<const pack::UvAnimation*>(blob + h->uvAnimationsOffset), h->uvAnimationCount);
    auto scalarOk = [&](u32 track) {
        return track == pack::kEffectNoTrack || m_tracks.trackValid(track, false);
    };
    // Phase 14 (spellfx): the meshes of emitters of models, and the corners their triangles use.
    std::span<const pack::ParticleMesh> meshes;
    std::span<const pack::ParticleMeshVertex> meshVertices;
    if (h->meshCount > 0) {
        if (!sectionFits<pack::ParticleMesh>(h->meshesOffset, h->meshCount, size)) {
            return Error{"Corrupt particle meshes"};
        }
        meshes = {reinterpret_cast<const pack::ParticleMesh*>(blob + h->meshesOffset), h->meshCount};
        u64 corners = 0;
        for (const pack::ParticleMesh& m : meshes) {
            corners = std::max(corners, u64{m.firstVertex} + u64{m.triangleCount} * 3);
        }
        if (corners > 0xFFFFFFFFull ||
            !sectionFits<pack::ParticleMeshVertex>(h->meshVerticesOffset, static_cast<u32>(corners), size)) {
            return Error{"Corrupt particle mesh vertices"};
        }
        meshVertices = {reinterpret_cast<const pack::ParticleMeshVertex*>(blob + h->meshVerticesOffset),
                        corners};
    }
    for (const pack::ParticleEmitter& e : emitters) {
        if ((m_actor && e.bone >= boneCount) || e.rows == 0 || e.columns == 0 ||
            u64{e.firstSplinePoint} + e.splinePointCount > h->splinePointCount || e.mesh > meshes.size()) {
            return Error{"Corrupt particle emitter"};
        }
        for (u32 track : e.tracks) {
            if (!scalarOk(track)) {
                return Error{"Corrupt particle emitter track"};
            }
        }
    }
    for (const pack::RibbonEmitter& r : ribbons) {
        if (m_actor && r.bone >= boneCount) {
            return Error{"Corrupt ribbon emitter"};
        }
        for (u32 track : r.tracks) {
            if (!scalarOk(track)) {
                return Error{"Corrupt ribbon emitter track"};
            }
        }
    }
    for (const pack::UvAnimation& uv : uvAnimations) {
        if (!scalarOk(uv.tracks[0]) ||
            !(uv.tracks[1] == pack::kEffectNoTrack || m_tracks.trackValid(uv.tracks[1], true)) ||
            !scalarOk(uv.tracks[2])) {
            return Error{"Corrupt UV animation"};
        }
    }
    m_header = h;
    m_emitters = emitters;
    m_ribbons = ribbons;
    m_uvAnimations = uvAnimations;
    m_splinePoints = {reinterpret_cast<const glm::vec3*>(blob + h->splinePointsOffset), h->splinePointCount};
    m_meshes = meshes;
    m_meshVertices = meshVertices;
    m_reach = h->reach;
    m_capacities.resize(emitters.size());
    for (usize i = 0; i < emitters.size(); ++i) {
        const pack::ParticleEmitter& e = emitters[i];
        const f32 rate = std::max(
            0.0f, largestValue(m_tracks, e.tracks[pack::kEmitterRate], e.values[pack::kEmitterRate]));
        const f32 life = std::max(
            0.0f, largestValue(m_tracks, e.tracks[pack::kEmitterLifespan], e.values[pack::kEmitterLifespan]));
        const f32 alive = std::ceil(rate * life * 1.25f) + 2.0f;
        const u32 most = e.mesh != 0 ? kMaxMeshCapacity : kMaxCapacity;
        m_capacities[i] =
            static_cast<u32>(std::clamp(alive, static_cast<f32>(kMinCapacity), static_cast<f32>(most)));
    }
    return {};
}

void EffectSet::computeUvTransforms(u32 sequence, u32 localMs, u32 globalMs, UvTransform* out) const {
    for (usize i = 0; i < m_uvAnimations.size(); ++i) {
        const pack::UvAnimation& uv = m_uvAnimations[i];
        const glm::vec3 t = m_tracks.sampleVec3(uv.tracks[0], sequence, localMs, globalMs, glm::vec3(0.0f));
        const glm::quat r = m_tracks.sampleQuat(uv.tracks[1], sequence, localMs, globalMs);
        const glm::vec3 s = m_tracks.sampleVec3(uv.tracks[2], sequence, localMs, globalMs, glm::vec3(1.0f));
        out[i] = uvTransform(t, r, s);
    }
}

} // namespace vd::fx
