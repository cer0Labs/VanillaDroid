#include "bake/EffectBaker.h"

#include "core/Hash.h"

#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vd::bake {
namespace {

constexpr f32 kMaxReach = 80.0f; // yards: culling margin cap (a few spells reach further, they are actors)
constexpr u32 kMaxMeshTriangles = 256; // phase 14: a particle model's triangles (the client's have 6-80)

// BGRA (client) -> RGBA bytes packed little-endian.
u32 bgraToRgba(u32 bgra) {
    const u32 b = bgra & 0xFF;
    const u32 g = (bgra >> 8) & 0xFF;
    const u32 r = (bgra >> 16) & 0xFF;
    const u32 a = (bgra >> 24) & 0xFF;
    return r | (g << 8) | (b << 16) | (a << 24);
}

u32 rgbaOf(const formats::Vec3& rgb, f32 alpha) {
    auto byte = [](f32 v) { return static_cast<u32>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return byte(rgb[0]) | (byte(rgb[1]) << 8) | (byte(rgb[2]) << 16) | (byte(alpha) << 24);
}

template <typename T> bool constantValues(const formats::M2Track<T>& track) {
    return std::all_of(track.values.begin(), track.values.end(),
                       [&](const T& v) { return v == track.values[0]; });
}

// A scalar parameter: in place when it never changes (or has no keys: `fallback`), else a track of the table.
template <typename T>
void scalar(const formats::M2Track<T>& track, f32 scale, f32 fallback, TrackWriter& tracks, u32& trackOut,
            f32& valueOut) {
    trackOut = pack::kEffectNoTrack;
    if (track.times.empty()) {
        valueOut = fallback;
    } else if (constantValues(track)) {
        valueOut = static_cast<f32>(track.values[0]) * scale;
    } else {
        valueOut = static_cast<f32>(track.values[0]) * scale;
        const u32 index = tracks.add(toScalarTrack(track, scale), formats::Vec3{NAN, NAN, NAN});
        trackOut = index == pack::kActorNoTrack ? pack::kEffectNoTrack : index;
    }
}

// Largest absolute value a scalar parameter takes.
template <typename T> f32 largest(const formats::M2Track<T>& track, f32 fallback) {
    if (track.values.empty()) {
        return std::fabs(fallback);
    }
    f32 out = 0.0f;
    for (const T& v : track.values) {
        out = std::max(out, std::fabs(static_cast<f32>(v)));
    }
    return out;
}

glm::mat4 boneLocal(const formats::M2Bone& bone, u32 sequence, u32 time) {
    const formats::Vec3 t =
        formats::sampleM2Track(bone.translation, sequence, time, formats::Vec3{0.0f, 0.0f, 0.0f});
    const formats::Quat r =
        formats::sampleM2Track(bone.rotation, sequence, time, formats::Quat{0.0f, 0.0f, 0.0f, 1.0f});
    const formats::Vec3 s =
        formats::sampleM2Track(bone.scale, sequence, time, formats::Vec3{1.0f, 1.0f, 1.0f});
    const glm::vec3 pivot(bone.pivot[0], bone.pivot[1], bone.pivot[2]);
    glm::mat4 m = glm::translate(glm::mat4(1.0f), pivot + glm::vec3(t[0], t[1], t[2]));
    m = m * glm::mat4_cast(glm::normalize(glm::quat(r[3], r[0], r[1], r[2])));
    m = glm::scale(m, glm::vec3(s[0], s[1], s[2]));
    return glm::translate(m, -pivot);
}

std::array<f32, 12> toRows(const glm::mat4& m) {
    std::array<f32, 12> out{};
    for (u32 row = 0; row < 3; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            out[row * 4 + column] = m[column][row];
        }
    }
    return out;
}

glm::mat4 poseOf(const formats::M2Model& model, u32 bone, u32 sequence, u32 time) {
    glm::mat4 pose(1.0f);
    for (i32 b = static_cast<i32>(bone); b >= 0 && static_cast<usize>(b) < model.bones.size();
         b = model.bones[b].parent) {
        pose = boneLocal(model.bones[b], sequence, time) * pose;
    }
    return pose;
}

// Emitter -> model space of a doodad: its bone where the first sequence starts, then its position.
glm::mat4 emitterTransform(const formats::M2Model& model, u32 bone, const formats::Vec3& position) {
    const u32 time = model.sequences.empty() ? 0 : model.sequences[0].start;
    return glm::translate(poseOf(model, bone, 0, time), glm::vec3(position[0], position[1], position[2]));
}

} // namespace

std::array<f32, 12> m2BonePose(const formats::M2Model& model, u32 bone, u32 sequence, u32 time) {
    return toRows(poseOf(model, bone, sequence, time));
}

bool bakeParticleMesh(const formats::M2Model& geometry, const formats::M2ParticleEmitter& emitter,
                      BakedEffects& effects, pack::ParticleMesh& out) {
    out = pack::ParticleMesh{};
    const formats::M2Batch* first = nullptr;
    for (const formats::M2Batch& b : geometry.batches) {
        if (b.indexCount >= 3) {
            first = &b;
            break;
        }
    }
    if (!first) {
        return false;
    }
    if (first->texture >= 0 && static_cast<usize>(first->texture) < geometry.textures.size() &&
        !geometry.textures[first->texture].path.empty()) {
        out.texture = assetPathHash(normalizeAssetPath(geometry.textures[first->texture].path));
    }
    out.blend =
        static_cast<u8>(first->material < geometry.materials.size()
                            ? toModelBlend(static_cast<u32>(geometry.materials[first->material].blend))
                            : pack::ModelBlend::Opaque);
    out.firstVertex = static_cast<u32>(effects.meshVertices.size());
    // Every batch's triangles (the parts of one model share its texture in the client's: Skull's eyes too).
    for (const formats::M2Batch& b : geometry.batches) {
        for (u32 i = 0; i + 2 < b.indexCount && out.triangleCount < kMaxMeshTriangles; i += 3) {
            for (u32 corner = 0; corner < 3; ++corner) {
                const u32 index = b.firstIndex + i + corner;
                const formats::M2Vertex& v = geometry.vertices[geometry.triangles[index]];
                pack::ParticleMeshVertex vertex{};
                std::memcpy(vertex.position, v.position.data(), sizeof(vertex.position));
                std::memcpy(vertex.uv, v.uv.data(), sizeof(vertex.uv));
                effects.meshVertices.push_back(vertex);
            }
            ++out.triangleCount;
        }
    }
    for (u32 i = 0; i < 3; ++i) {
        out.tumbleMin[i] = emitter.tumbleMin[i];
        out.tumbleMax[i] = emitter.tumbleMax[i];
    }
    return out.triangleCount > 0;
}

BakedEffects bakeEffects(const formats::M2Model& model, TrackWriter& tracks, bool staticPose,
                         const ParticleModelSource* models) {
    BakedEffects out;
    const std::array<f32, 12> identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

    for (const formats::M2TextureTransform& t : model.textureTransforms) {
        pack::UvAnimation uv{};
        uv.tracks[0] = tracks.add(t.translation, formats::Vec3{0.0f, 0.0f, 0.0f});
        uv.tracks[1] = tracks.add(t.rotation, formats::Quat{0.0f, 0.0f, 0.0f, 1.0f});
        uv.tracks[2] = tracks.add(t.scale, formats::Vec3{1.0f, 1.0f, 1.0f});
        for (u32& track : uv.tracks) {
            track = track == pack::kActorNoTrack ? pack::kEffectNoTrack : track;
        }
        out.uvAnimations.push_back(uv);
    }

    for (const formats::M2ParticleEmitter& e : model.particles) {
        if (!e.recursionModel.empty()) {
            continue; // emitters of another model's emitters: not supported
        }
        // Phase 14 (spellfx): an emitter of models draws each particle as that model.
        pack::ParticleMesh mesh{};
        if (!e.geometryModel.empty()) {
            const formats::M2Model* geometry = models ? models->particleModel(e.geometryModel) : nullptr;
            if (!geometry || !bakeParticleMesh(*geometry, e, out, mesh)) {
                continue;
            }
            out.meshes.push_back(mesh);
        }
        pack::ParticleEmitter p{};
        const formats::M2Texture& texture = model.textures[e.texture];
        p.texture = texture.path.empty() ? 0 : assetPathHash(normalizeAssetPath(texture.path));
        p.flags = e.flags;
        p.bone = e.bone;
        p.blend = static_cast<u8>(toModelBlend(e.blend));
        if (!e.geometryModel.empty()) {
            // Its particles take the model's texture and blend.
            p.texture = mesh.texture;
            p.blend = mesh.blend;
            p.mesh = static_cast<u32>(out.meshes.size()); // index + 1
        }
        p.shape = static_cast<u8>(std::clamp<u16>(e.emitterType, 1, 3));
        p.style = static_cast<u8>(std::min<u8>(e.particleType, 2));
        p.rows = static_cast<u8>(std::clamp<u16>(e.rows, 1, 255));
        p.columns = static_cast<u8>(std::clamp<u16>(e.columns, 1, 255));
        std::memcpy(p.position, e.position.data(), sizeof(p.position));
        const std::array<f32, 12> transform =
            staticPose ? toRows(emitterTransform(model, e.bone, e.position)) : identity;
        std::memcpy(p.transform, transform.data(), sizeof(p.transform));
        const formats::M2Track<f32>* params[] = {
            &e.speed,    &e.speedVariation, &e.verticalRange, &e.horizontalRange, &e.gravity,
            &e.lifespan, &e.emissionRate,   &e.areaLength,    &e.areaWidth,       &e.zSource};
        for (u32 i = 0; i < 10; ++i) {
            scalar(*params[i], 1.0f, 0.0f, tracks, p.tracks[i], p.values[i]);
        }
        scalar(e.enabled, 1.0f, 1.0f, tracks, p.tracks[pack::kEmitterEnabled],
               p.values[pack::kEmitterEnabled]);
        p.midPoint = std::clamp(e.midPoint, 0.0f, 1.0f);
        for (u32 i = 0; i < 3; ++i) {
            p.colors[i] = bgraToRgba(e.colors[i]);
            p.scales[i] = e.scales[i];
        }
        for (u32 i = 0; i < 3; ++i) {
            p.headCells[i] = e.headLifeCells[i];
            p.headCells[3 + i] = e.headDecayCells[i];
        }
        p.tailCells[0] = e.tailLifeCells[0];
        p.tailCells[1] = e.tailLifeCells[1];
        p.tailCells[2] = e.tailDecayCells[0];
        p.tailCells[3] = e.tailDecayCells[1];
        p.tailLength = e.tailLength;
        p.twinkleSpeed = e.twinkleSpeed;
        p.twinklePercent = e.twinklePercent;
        p.twinkleMin = e.twinkleMin;
        p.twinkleMax = e.twinkleMax;
        p.burstMultiplier = e.burstMultiplier;
        p.drag = e.drag;
        p.spin = e.spin;
        std::memcpy(p.wind, e.wind.data(), sizeof(p.wind));
        p.windTime = e.windTime;
        p.followSpeed1 = e.followSpeed1;
        p.followScale1 = e.followScale1;
        p.followSpeed2 = e.followSpeed2;
        p.followScale2 = e.followScale2;
        p.firstSplinePoint = static_cast<u32>(out.splinePoints.size());
        p.splinePointCount = static_cast<u32>(e.splinePoints.size());
        out.splinePoints.insert(out.splinePoints.end(), e.splinePoints.begin(), e.splinePoints.end());

        // How far its particles can get from the emitter: the spawn area, the travel of the fastest one over
        // the longest life (with gravity) and the largest quad.
        const f32 life = std::min(largest(e.lifespan, 0.0f), 30.0f);
        const f32 speed = largest(e.speed, 0.0f) * (1.0f + largest(e.speedVariation, 0.0f));
        const f32 area = std::max(largest(e.areaLength, 0.0f), largest(e.areaWidth, 0.0f));
        const f32 size = std::max({std::fabs(e.scales[0]), std::fabs(e.scales[1]), std::fabs(e.scales[2])});
        const f32 travel = speed * life + 0.5f * largest(e.gravity, 0.0f) * life * life;
        p.reach =
            std::min(kMaxReach, area + travel + size * std::max(1.0f, e.twinkleMax) + speed * e.tailLength);
        const f32 origin = staticPose
                               ? std::sqrt(transform[3] * transform[3] + transform[7] * transform[7] +
                                           transform[11] * transform[11])
                               : std::sqrt(e.position[0] * e.position[0] + e.position[1] * e.position[1] +
                                           e.position[2] * e.position[2]);
        out.reach = std::max(out.reach, origin + p.reach);
        out.emitters.push_back(p);
    }

    for (const formats::M2RibbonEmitter& r : model.ribbons) {
        if (r.textures.empty() || r.materials.empty()) {
            continue;
        }
        pack::RibbonEmitter ribbon{};
        const formats::M2Texture& texture = model.textures[r.textures[0]];
        ribbon.texture = texture.path.empty() ? 0 : assetPathHash(normalizeAssetPath(texture.path));
        ribbon.bone = static_cast<u16>(r.bone);
        ribbon.blend = static_cast<u8>(toModelBlend(static_cast<u32>(model.materials[r.materials[0]].blend)));
        const formats::Vec3 white{1.0f, 1.0f, 1.0f};
        ribbon.color = rgbaOf(r.color.values.empty() ? white : r.color.values[0], 1.0f);
        std::memcpy(ribbon.position, r.position.data(), sizeof(ribbon.position));
        const std::array<f32, 12> transform =
            staticPose ? toRows(emitterTransform(model, r.bone, r.position)) : identity;
        std::memcpy(ribbon.transform, transform.data(), sizeof(ribbon.transform));
        scalar(r.above, 1.0f, 0.0f, tracks, ribbon.tracks[pack::kRibbonAbove],
               ribbon.values[pack::kRibbonAbove]);
        scalar(r.below, 1.0f, 0.0f, tracks, ribbon.tracks[pack::kRibbonBelow],
               ribbon.values[pack::kRibbonBelow]);
        scalar(r.alpha, 1.0f / 32767.0f, 1.0f, tracks, ribbon.tracks[pack::kRibbonAlpha],
               ribbon.values[pack::kRibbonAlpha]);
        scalar(r.visibility, 1.0f, 1.0f, tracks, ribbon.tracks[pack::kRibbonVisibility],
               ribbon.values[pack::kRibbonVisibility]);
        ribbon.edgesPerSecond = std::max(r.edgesPerSecond, 1.0f);
        ribbon.edgeLifetime = std::clamp(r.edgeLifetime, 0.05f, 20.0f);
        ribbon.gravity = r.gravity;
        out.ribbons.push_back(ribbon);
        const f32 height = std::max(largest(r.above, 0.0f), largest(r.below, 0.0f));
        out.reach =
            std::max(out.reach, std::sqrt(r.position[0] * r.position[0] + r.position[1] * r.position[1] +
                                          r.position[2] * r.position[2]) +
                                    height + std::fabs(r.gravity) * ribbon.edgeLifetime);
    }
    out.reach = std::min(out.reach, kMaxReach * 2.0f);
    return out;
}

u32 BakedEffects::write(BlobWriter& writer) const {
    if (empty()) {
        return 0;
    }
    pack::EffectsHeader header{};
    header.emitterCount = static_cast<u32>(emitters.size());
    header.emittersOffset = writer.append(emitters);
    header.ribbonCount = static_cast<u32>(ribbons.size());
    header.ribbonsOffset = writer.append(ribbons);
    header.uvAnimationCount = static_cast<u32>(uvAnimations.size());
    header.uvAnimationsOffset = writer.append(uvAnimations);
    header.splinePointCount = static_cast<u32>(splinePoints.size());
    header.splinePointsOffset = writer.append(splinePoints);
    header.reach = reach;
    if (!meshes.empty()) { // phase 14 (spellfx)
        header.meshCount = static_cast<u32>(meshes.size());
        header.meshesOffset = writer.append(meshes);
        header.meshVerticesOffset = writer.append(meshVertices);
    }
    return writer.append(std::vector<pack::EffectsHeader>{header});
}

EffectCounts countEffects(const BakedEffects& effects, const formats::M2Model& model) {
    EffectCounts counts;
    counts.emitters = static_cast<u32>(effects.emitters.size());
    counts.ribbons = static_cast<u32>(effects.ribbons.size());
    counts.uvAnimations = static_cast<u32>(effects.uvAnimations.size());
    counts.skippedEmitters = static_cast<u32>(model.particles.size()) - counts.emitters;
    counts.meshEmitters = static_cast<u32>(effects.meshes.size());
    return counts;
}

} // namespace vd::bake
