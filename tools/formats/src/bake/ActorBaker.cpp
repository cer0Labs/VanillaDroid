#include "bake/ActorBaker.h"

#include "bake/BakeUtil.h"
#include "bake/EffectBaker.h"
#include "bake/Portable.h"
#include "bake/TrackWriter.h"
#include "core/Hash.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>
#include <type_traits>

namespace vd::bake {
namespace {

using pack::ModelBlend;

struct MaterialKey {
    u64 texture;
    u8 blend;
    u8 slot;
    u16 flags;
    u32 color;
    u16 colorTrack;
    u16 alphaTrack;
    u16 transparencyTrack;
    u16 uvAnimation;
    auto tie() const {
        return std::tie(texture, blend, slot, flags, color, colorTrack, alphaTrack, transparencyTrack, uvAnimation);
    }
    bool operator<(const MaterialKey& o) const { return tie() < o.tie(); }
};

bool isTransparent(ModelBlend blend) {
    return blend != ModelBlend::Opaque && blend != ModelBlend::AlphaKey;
}

// fixed16 opacity -> a vec3 track holding the 0..1 value in x.
formats::M2Track<formats::Vec3> toOpacityTrack(const formats::M2Track<i16>& track) {
    formats::M2Track<formats::Vec3> out;
    out.interpolation = track.interpolation;
    out.globalSequence = track.globalSequence;
    out.ranges = track.ranges;
    out.times = track.times;
    for (i16 v : track.values) {
        out.values.push_back({std::clamp(v / 32767.0f, 0.0f, 1.0f), 0.0f, 0.0f});
    }
    return out;
}

f32 minimumOpacity(const formats::M2Track<i16>* track) {
    f32 minimum = 1.0f;
    if (track) {
        for (i16 v : track->values) {
            minimum = std::min(minimum, std::clamp(v / 32767.0f, 0.0f, 1.0f));
        }
    }
    return minimum;
}

// Portraits without a camera in the model: the M2 key bone of the head and the helm's attachment point.
constexpr i32 kHeadKeyBone =
    6; // 0 ArmL, 1 ArmR, 2 ShoulderL, 3 ShoulderR, 4 SpineLow, 5 Waist, 6 Head, 7 Jaw
constexpr u32 kHelmAttachment = 11;
// The original portrait cameras' usual lens and clip planes (most creatures: 0.95 rad, 0.222 .. 27.78 yd).
constexpr f32 kPortraitFov = 0.95f;
constexpr f32 kPortraitNear = 0.222f;
constexpr f32 kPortraitFar = 27.78f;

bool finite3(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}


// Phase 14: the model's events (pack::ActorEventsHeader), 0 when it has none. An event's times are on the tracks'
// timeline: those inside a sequence's [start, end] become local times of that sequence. Events looping over a
// global sequence (none in the 1.12 data: scan-events) or with no times at all are kept as always on.
u32 writeEvents(const formats::M2Model& model, BlobWriter& writer) {
    if (model.events.empty()) {
        return 0;
    }
    const u32 sequenceCount = static_cast<u32>(model.sequences.size());
    std::vector<pack::ActorEvent> events;
    std::vector<pack::ActorRange> ranges;
    std::vector<u32> times;
    for (const formats::M2Event& e : model.events) {
        pack::ActorEvent out{};
        out.id = e.id;
        out.data = e.data;
        out.bone = e.bone;
        std::memcpy(out.position, e.position.data(), sizeof(out.position));
        out.firstRange = static_cast<u32>(ranges.size());
        const bool timed = !e.times.empty() && e.globalSequence < 0;
        out.flags = timed ? pack::kActorEventTimed : 0;
        for (u32 s = 0; s < sequenceCount; ++s) {
            pack::ActorRange range{static_cast<u32>(times.size()), 0};
            if (timed) {
                const formats::M2Sequence& sequence = model.sequences[s];
                std::vector<u32> local;
                for (const u32 t : e.times) {
                    if (t >= sequence.start && t <= sequence.end) {
                        local.push_back(t - sequence.start);
                    }
                }
                std::sort(local.begin(), local.end());
                local.erase(std::unique(local.begin(), local.end()), local.end());
                times.insert(times.end(), local.begin(), local.end());
                range.count = static_cast<u32>(local.size());
            }
            ranges.push_back(range);
        }
        events.push_back(out);
    }
    pack::ActorEventsHeader header{};
    header.eventCount = static_cast<u32>(events.size());
    header.eventsOffset = writer.append(events);
    header.rangeCount = static_cast<u32>(ranges.size());
    header.rangesOffset = writer.append(ranges);
    header.timeCount = static_cast<u32>(times.size());
    header.timesOffset = writer.append(times);
    return writer.append(std::vector<pack::ActorEventsHeader>{header});
}

} // namespace

std::vector<glm::mat4> standPose(const formats::M2Model& model) {
    std::vector<glm::mat4> pose(model.bones.size(), glm::mat4(1.0f));
    const u32 start = model.sequences.empty() ? 0 : model.sequences[0].start;
    for (usize b = 0; b < model.bones.size(); ++b) {
        const formats::M2Bone& bone = model.bones[b];
        const auto timeOf = [&](i16 globalSequence) { return globalSequence >= 0 ? 0u : start; };
        const formats::Vec3 t = formats::sampleM2Track(
            bone.translation, 0, timeOf(bone.translation.globalSequence), formats::Vec3{0.0f, 0.0f, 0.0f});
        const formats::Quat r = formats::sampleM2Track(bone.rotation, 0, timeOf(bone.rotation.globalSequence),
                                                       formats::Quat{0.0f, 0.0f, 0.0f, 1.0f});
        const formats::Vec3 s = formats::sampleM2Track(bone.scale, 0, timeOf(bone.scale.globalSequence),
                                                       formats::Vec3{1.0f, 1.0f, 1.0f});
        const glm::vec3 pivot(bone.pivot[0], bone.pivot[1], bone.pivot[2]);
        glm::mat4 local = glm::translate(glm::mat4(1.0f), pivot + glm::vec3(t[0], t[1], t[2]));
        local *= glm::mat4_cast(glm::normalize(glm::quat(r[3], r[0], r[1], r[2])));
        local = glm::scale(local, glm::vec3(s[0], s[1], s[2]));
        local = glm::translate(local, -pivot);
        pose[b] = bone.parent >= 0 && static_cast<usize>(bone.parent) < b
                      ? pose[static_cast<usize>(bone.parent)] * local
                      : local;
    }
    return pose;
}

pack::ActorPortrait actorPortrait(const formats::M2Model& model, const f32 boundsMin[3],
                                  const f32 boundsMax[3]) {
    pack::ActorPortrait out{};
    // The model's own camera, as it stands at the first frame of the first sequence (Stand).
    if (const formats::M2Camera* camera = model.camera(formats::kM2CameraPortrait)) {
        const u32 start = model.sequences.empty() ? 0 : model.sequences[0].start;
        const formats::Vec3 p =
            formats::sampleM2Track(camera->position, 0, start, formats::Vec3{0.0f, 0.0f, 0.0f});
        const formats::Vec3 t =
            formats::sampleM2Track(camera->target, 0, start, formats::Vec3{0.0f, 0.0f, 0.0f});
        const glm::vec3 position =
            glm::vec3(camera->positionBase[0], camera->positionBase[1], camera->positionBase[2]) +
            glm::vec3(p[0], p[1], p[2]);
        const glm::vec3 target =
            glm::vec3(camera->targetBase[0], camera->targetBase[1], camera->targetBase[2]) +
            glm::vec3(t[0], t[1], t[2]);
        if (finite3(position) && finite3(target) && glm::length(position - target) > 0.01f &&
            camera->fov > 0.05f && camera->fov < 3.0f) {
            std::memcpy(out.position, &position[0], sizeof(out.position));
            std::memcpy(out.target, &target[0], sizeof(out.target));
            out.fov = camera->fov;
            out.nearClip = camera->nearClip > 0.0f ? camera->nearClip : kPortraitNear;
            out.farClip = camera->farClip > out.nearClip ? camera->farClip : kPortraitFar;
            out.roll = camera->roll.values.empty() ? 0.0f : camera->roll.values[0];
            out.roll = std::isfinite(out.roll) ? out.roll : 0.0f;
            return out;
        }
    }

    // None: a camera in front of the head — the vertices skinned to the Head key bone and the bones under it
    // (jaw, ears, hair), posed as the model stands —, else in front of the helm point, else of the top
    // quarter of the model's front half (a quadruped's head). Seen from its front left and a little above,
    // like most creatures' own cameras.
    out.flags = pack::kActorPortraitComputed;
    const glm::vec3 min(boundsMin[0], boundsMin[1], boundsMin[2]);
    const glm::vec3 max(boundsMax[0], boundsMax[1], boundsMax[2]);
    const glm::vec3 size = glm::max(max - min, glm::vec3(0.1f));
    const std::vector<glm::mat4> pose = standPose(model);
    std::vector<u8> inHead(model.bones.size(), 0);
    bool hasHead = false;
    for (usize b = 0; b < model.bones.size(); ++b) { // parents come before their children
        const i16 parent = model.bones[b].parent;
        inHead[b] =
            model.bones[b].keyBoneId == kHeadKeyBone || (parent >= 0 && inHead[static_cast<usize>(parent)]);
        hasHead = hasHead || inHead[b];
    }
    glm::vec3 headMin(FLT_MAX), headMax(-FLT_MAX);
    u32 headVertices = 0;
    for (const formats::M2Vertex& v : model.vertices) {
        if (!hasHead) {
            break;
        }
        u32 strongest = 0;
        for (u32 k = 1; k < 4; ++k) {
            strongest = v.boneWeights[k] > v.boneWeights[strongest] ? k : strongest;
        }
        const u8 bone = v.boneIndices[strongest];
        if (v.boneWeights[strongest] == 0 || bone >= inHead.size() || !inHead[bone]) {
            continue;
        }
        glm::vec3 p(0.0f);
        for (u32 k = 0; k < 4; ++k) {
            if (v.boneWeights[k] > 0 && v.boneIndices[k] < pose.size()) {
                p += glm::vec3(pose[v.boneIndices[k]] *
                               glm::vec4(v.position[0], v.position[1], v.position[2], 1.0f)) *
                     (static_cast<f32>(v.boneWeights[k]) / 255.0f);
            }
        }
        headMin = glm::min(headMin, p);
        headMax = glm::max(headMax, p);
        ++headVertices;
    }
    const formats::M2Attachment* helm = nullptr;
    for (const formats::M2Attachment& a : model.attachments) {
        if (a.id == kHelmAttachment && a.bone < pose.size()) {
            helm = &a;
            break;
        }
    }
    glm::vec3 center;
    f32 half = 0.0f; // half the side of the square seen around the center
    if (headVertices >= 8) {
        const glm::vec3 extent = headMax - headMin;
        center = (headMin + headMax) * 0.5f;
        half = std::max(0.15f, extent.z * 0.55f); // the artists frame a head in 0.4 .. 0.8 of its height
    } else if (helm) {
        center = glm::vec3(pose[helm->bone] *
                           glm::vec4(helm->position[0], helm->position[1], helm->position[2], 1.0f));
        half = std::max(0.15f, size.z * 0.12f);
    } else {
        center = glm::vec3(min.x + size.x * 0.75f, (min.y + max.y) * 0.5f, min.z + size.z * 0.75f);
        half = std::max(0.15f, std::max(size.z, size.x * 0.5f) * 0.25f);
    }
    const glm::vec3 direction = glm::normalize(glm::vec3(0.8f, 0.55f, 0.15f));
    const f32 distance = half * std::sqrt(2.0f) / portableTan(kPortraitFov * 0.5f); // phase 22: bake/Portable.h
    const glm::vec3 position = center + direction * distance;
    std::memcpy(out.position, &position[0], sizeof(out.position));
    std::memcpy(out.target, &center[0], sizeof(out.target));
    out.fov = kPortraitFov;
    out.nearClip = std::min(kPortraitNear, distance * 0.5f);
    out.farClip = std::max(kPortraitFar, distance * 4.0f);
    return out;
}

pack::ActorTextureSlot actorTextureSlot(u32 type) {
    switch (type) {
    case formats::kM2TextureFile: return pack::ActorTextureSlot::File;
    case formats::kM2TextureBody: return pack::ActorTextureSlot::Body;
    case formats::kM2TextureObjectSkin: return pack::ActorTextureSlot::Cape;
    case formats::kM2TextureHair: return pack::ActorTextureSlot::Hair;
    case formats::kM2TextureSkinExtra: return pack::ActorTextureSlot::SkinExtra;
    case formats::kM2TextureMonster1: return pack::ActorTextureSlot::Monster1;
    case formats::kM2TextureMonster2: return pack::ActorTextureSlot::Monster2;
    case formats::kM2TextureMonster3: return pack::ActorTextureSlot::Monster3;
    default: return pack::ActorTextureSlot::None;
    }
}

Result<std::vector<u8>> bakeActor(const formats::M2Model& model, const ParticleModelSource* particleModels) {
    if (model.vertices.size() > 0xFFFF) {
        return Error{"M2 with more than 65535 vertices"};
    }
    if (model.bones.size() > 256) {
        return Error{"M2 with more than 256 bones"};
    }
    pack::ActorModelHeader header{};
    header.version = pack::kActorFormatVersion;

    std::vector<pack::ActorVertex> vertices;
    vertices.reserve(model.vertices.size());
    f32 min[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    f32 max[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const formats::M2Vertex& v : model.vertices) {
        pack::ActorVertex out{};
        std::memcpy(out.position, v.position.data(), sizeof(out.position));
        for (u32 i = 0; i < 3; ++i) {
            out.normal[i] = toSnorm8(v.normal[i]);
            min[i] = std::min(min[i], v.position[i]);
            max[i] = std::max(max[i], v.position[i]);
        }
        std::memcpy(out.uv, v.uv.data(), sizeof(out.uv));
        u32 sum = 0;
        for (u32 i = 0; i < 4; ++i) {
            out.weights[i] = v.boneWeights[i];
            out.bones[i] = model.bones.empty() ? 0 : v.boneIndices[i];
            sum += v.boneWeights[i];
        }
        if (sum == 0 || model.bones.empty()) { // unweighted: rigid on bone 0
            std::memset(out.weights, 0, sizeof(out.weights));
            std::memset(out.bones, 0, sizeof(out.bones));
            out.weights[0] = 255;
        }
        vertices.push_back(out);
    }

    const u32 sequenceCount = static_cast<u32>(model.sequences.size());
    TrackWriter tracks(model.sequences);
    // Effects first: materials refer to their texture animations.
    const BakedEffects effects = bakeEffects(model, tracks, false, particleModels);
    // Animated batch colors: one baked track per M2 color / transparency, shared by the batches using it.
    std::map<i32, u16> colorTracks, alphaTracks, transparencyTracks;
    auto trackOf = [&](std::map<i32, u16>& cache, i32 key, auto&& make) -> u16 {
        const auto [it, inserted] = cache.emplace(key, pack::kActorNoColorTrack);
        if (inserted) {
            const u32 index = make();
            it->second = index < pack::kActorNoColorTrack ? static_cast<u16>(index) : pack::kActorNoColorTrack;
        }
        return it->second;
    };

    // Materials (deduplicated) and one part per (material, geoset).
    std::map<MaterialKey, u16> materialIndex;
    std::vector<pack::ActorMaterial> materials;
    struct PartKey {
        u8 blend;
        u16 material;
        u16 geoset;
        u32 order; // first batch: the draw order of blended parts
        std::vector<u16> indices;
    };
    std::vector<PartKey> parts;
    for (u32 b = 0; b < model.batches.size(); ++b) {
        const formats::M2Batch& batch = model.batches[b];
        if (batch.indexCount == 0) {
            continue;
        }
        const formats::M2Material& m = model.materials[batch.material];
        const formats::M2Texture* texture = batch.texture >= 0 ? &model.textures[batch.texture] : nullptr;
        MaterialKey key{};
        key.slot = static_cast<u8>(texture ? actorTextureSlot(texture->type) : pack::ActorTextureSlot::None);
        if (texture && texture->type == formats::kM2TextureFile && !texture->path.empty()) {
            key.texture = assetPathHash(normalizeAssetPath(texture->path));
        }
        ModelBlend blend = toModelBlend(static_cast<u32>(m.blend));
        const f32 alpha = std::clamp(batch.alpha, 0.0f, 1.0f);
        const formats::M2Color* color =
            batch.color >= 0 && static_cast<usize>(batch.color) < model.colors.size() ? &model.colors[batch.color]
                                                                                        : nullptr;
        const formats::M2Track<i16>* weight =
            batch.transparency >= 0 && static_cast<usize>(batch.transparency) < model.transparency.size()
                ? &model.transparency[batch.transparency]
                : nullptr;
        key.colorTrack = key.alphaTrack = key.transparencyTrack = pack::kActorNoColorTrack;
        if (batch.textureTransform >= 0 && static_cast<usize>(batch.textureTransform) < effects.uvAnimations.size()) {
            key.uvAnimation = static_cast<u16>(batch.textureTransform + 1);
        }
        if (color) {
            key.colorTrack = trackOf(colorTracks, batch.color,
                                     [&] { return tracks.add(color->color, formats::Vec3{1.0f, 1.0f, 1.0f}); });
            key.alphaTrack = trackOf(alphaTracks, batch.color, [&] {
                return tracks.add(toOpacityTrack(color->alpha), formats::Vec3{1.0f, 0.0f, 0.0f});
            });
        }
        if (weight) {
            key.transparencyTrack = trackOf(transparencyTracks, batch.transparency, [&] {
                return tracks.add(toOpacityTrack(*weight), formats::Vec3{1.0f, 0.0f, 0.0f});
            });
        }
        const f32 minimum = std::min({alpha, minimumOpacity(color ? &color->alpha : nullptr), minimumOpacity(weight)});
        if (minimum < 0.999f && !isTransparent(blend)) {
            blend = ModelBlend::Alpha; // a see-through or fading batch of an opaque material (ghosts, glows)
        }
        key.blend = static_cast<u8>(blend);
        key.flags = static_cast<u16>(((m.flags & formats::kM2MaterialUnlit) ? pack::kModelMaterialUnlit : 0) |
                                     ((m.flags & formats::kM2MaterialUnfogged) ? pack::kModelMaterialUnfogged : 0) |
                                     ((m.flags & formats::kM2MaterialTwoSided) ? pack::kModelMaterialTwoSided : 0) |
                                     (texture && (texture->flags & 3u) == 0 ? pack::kModelMaterialClamp : 0));
        key.color = 0x00FFFFFFu | (static_cast<u32>(std::lround(alpha * 255.0f)) << 24);
        const auto [it, inserted] = materialIndex.emplace(key, static_cast<u16>(materials.size()));
        if (inserted) {
            materials.push_back({key.texture, key.blend, key.slot, key.flags, key.color, key.colorTrack, key.alphaTrack,
                                 key.transparencyTrack, key.uvAnimation});
        }
        const u16 material = it->second;
        auto part = std::find_if(parts.begin(), parts.end(), [&](const PartKey& p) {
            return p.material == material && p.geoset == batch.geosetId;
        });
        if (part == parts.end()) {
            parts.push_back({key.blend, material, batch.geosetId, b, {}});
            part = parts.end() - 1;
        }
        for (u32 i = 0; i < batch.indexCount; ++i) {
            part->indices.push_back(static_cast<u16>(model.triangles[batch.firstIndex + i]));
        }
    }
    std::stable_sort(parts.begin(), parts.end(), [](const PartKey& a, const PartKey& b) {
        const bool at = isTransparent(static_cast<ModelBlend>(a.blend));
        const bool bt = isTransparent(static_cast<ModelBlend>(b.blend));
        if (at != bt) {
            return !at;
        }
        if (at) {
            return a.order < b.order; // blended: the model's own order
        }
        return std::tie(a.blend, a.material, a.geoset) < std::tie(b.blend, b.material, b.geoset);
    });
    std::vector<u16> geosets;
    for (const PartKey& p : parts) {
        geosets.push_back(p.geoset);
    }
    std::sort(geosets.begin(), geosets.end());
    geosets.erase(std::unique(geosets.begin(), geosets.end()), geosets.end());
    std::vector<u16> indices;
    std::vector<pack::ActorPart> outParts;
    for (const PartKey& p : parts) {
        pack::ActorPart out{};
        out.firstIndex = static_cast<u32>(indices.size());
        out.indexCount = static_cast<u32>(p.indices.size());
        out.material = p.material;
        out.geoset = p.geoset;
        out.geosetIndex =
            static_cast<u16>(std::lower_bound(geosets.begin(), geosets.end(), p.geoset) - geosets.begin());
        indices.insert(indices.end(), p.indices.begin(), p.indices.end());
        outParts.push_back(out);
    }

    // Skeleton.
    std::vector<pack::ActorBone> bones;
    for (const formats::M2Bone& bone : model.bones) {
        pack::ActorBone out{};
        out.keyBone = bone.keyBoneId;
        out.flags = (bone.flags & formats::kM2BoneSphericalBillboard)      ? pack::kActorBoneBillboard
                    : (bone.flags & formats::kM2BoneCylindricalZBillboard) ? pack::kActorBoneBillboardZ
                                                                            : 0;
        out.parent = bone.parent;
        std::memcpy(out.pivot, bone.pivot.data(), sizeof(out.pivot));
        out.tracks[0] = tracks.add(bone.translation, formats::Vec3{0.0f, 0.0f, 0.0f});
        out.tracks[1] = tracks.add(bone.rotation, formats::Quat{0.0f, 0.0f, 0.0f, 1.0f});
        out.tracks[2] = tracks.add(bone.scale, formats::Vec3{1.0f, 1.0f, 1.0f});
        bones.push_back(out);
    }
    if (bones.empty()) {
        pack::ActorBone root{};
        root.keyBone = -1;
        root.parent = -1;
        root.tracks[0] = root.tracks[1] = root.tracks[2] = pack::kActorNoTrack;
        bones.push_back(root);
    }
    std::vector<pack::ActorSequence> sequences;
    f32 sequenceRadius = 0.0f;
    for (const formats::M2Sequence& s : model.sequences) {
        pack::ActorSequence out{};
        out.animation = s.animationId;
        out.variation = s.variation;
        out.start = s.start;
        out.end = s.end;
        out.moveSpeed = s.moveSpeed;
        out.flags = s.flags;
        out.frequency = s.frequency;
        out.nextVariation = s.nextVariation >= 0 && static_cast<u32>(s.nextVariation) < sequenceCount ? s.nextVariation
                                                                                                        : i16{-1};
        out.blendTime = s.blendTime;
        out.boundsRadius = s.boundsRadius;
        sequenceRadius = std::max(sequenceRadius, s.boundsRadius);
        sequences.push_back(out);
    }
    std::vector<pack::ActorAttachment> attachments;
    for (const formats::M2Attachment& a : model.attachments) {
        pack::ActorAttachment out{};
        out.id = a.id;
        out.bone = a.bone;
        std::memcpy(out.position, a.position.data(), sizeof(out.position));
        attachments.push_back(out);
    }

    if (vertices.empty()) {
        std::fill(std::begin(min), std::end(min), 0.0f);
        std::fill(std::begin(max), std::end(max), 0.0f);
    }
    f32 radius = 0.0f;
    for (u32 corner = 0; corner < 8; ++corner) {
        const glm::vec3 p((corner & 1) ? max[0] : min[0], (corner & 2) ? max[1] : min[1], (corner & 4) ? max[2] : min[2]);
        radius = std::max(radius, glm::length(p));
    }
    std::memcpy(header.boundsMin, min, sizeof(min));
    std::memcpy(header.boundsMax, max, sizeof(max));
    // Animations reach further than the bind pose (a dragon's wings, a kodo's tail).
    header.radius = std::max({radius, model.boundsRadius, sequenceRadius});
    // The selection circle: by the Stand animation's box (its first sequence), or the file's.
    const auto stand = std::find_if(model.sequences.begin(), model.sequences.end(),
                                    [](const formats::M2Sequence& s) { return s.animationId == 0; });
    header.selectionHalfSize =
        stand != model.sequences.end()
            ? pack::actorSelectionHalfSize(stand->boundsMin.data(), stand->boundsMax.data())
            : pack::actorSelectionHalfSize(model.boundsMin.data(), model.boundsMax.data());

    BlobWriter writer(sizeof(header));
    header.vertexCount = static_cast<u32>(vertices.size());
    header.verticesOffset = writer.append(vertices);
    header.indexCount = static_cast<u32>(indices.size());
    header.indicesOffset = writer.append(indices);
    header.materialCount = static_cast<u32>(materials.size());
    header.materialsOffset = writer.append(materials);
    header.partCount = static_cast<u32>(outParts.size());
    header.partsOffset = writer.append(outParts);
    header.geosetCount = static_cast<u32>(geosets.size());
    header.geosetsOffset = writer.append(geosets);
    header.boneCount = static_cast<u32>(bones.size());
    header.bonesOffset = writer.append(bones);
    tracks.finish();
    header.sequenceSlotCount = tracks.slotCount();
    header.trackCount = static_cast<u32>(tracks.m_tracks.size());
    header.tracksOffset = writer.append(tracks.m_tracks);
    header.rangeCount = static_cast<u32>(tracks.m_ranges.size());
    header.rangesOffset = writer.append(tracks.m_ranges);
    header.vec3KeyCount = static_cast<u32>(tracks.m_vec3Keys.size());
    header.vec3KeysOffset = writer.append(tracks.m_vec3Keys);
    header.quatKeyCount = static_cast<u32>(tracks.m_quatKeys.size());
    header.quatKeysOffset = writer.append(tracks.m_quatKeys);
    header.sequenceCount = sequenceCount;
    header.sequencesOffset = writer.append(sequences);
    header.globalSequenceCount = static_cast<u32>(model.globalSequences.size());
    header.globalSequencesOffset = writer.append(model.globalSequences);
    header.attachmentCount = static_cast<u32>(attachments.size());
    header.attachmentsOffset = writer.append(attachments);
    header.effectsOffset = effects.write(writer);
    if (!vertices.empty()) {
        header.portraitOffset =
            writer.append(std::vector<pack::ActorPortrait>{actorPortrait(model, min, max)});
    }
    header.eventsOffset = writeEvents(model, writer);
    return writer.finish(header);
}

} // namespace vd::bake
