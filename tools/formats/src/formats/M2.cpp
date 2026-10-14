#include "formats/M2.h"

#include "formats/ChunkReader.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace vd::formats {
namespace {

struct M2Array {
    u32 count;
    u32 offset;
};

// Version 256 header (1.12). Differs from later versions: playableAnimationLookup after the sequence
// lookups, one more array (texture flipbooks) after the transparency weights, views embedded in the file.
struct M2Header {
    char magic[4];
    u32 version;
    M2Array name;
    u32 globalFlags;
    M2Array globalSequences;
    M2Array sequences;
    M2Array sequenceLookups;
    M2Array playableAnimationLookup;
    M2Array bones;
    M2Array keyBoneLookup;
    M2Array vertices;
    M2Array views;
    M2Array colors;
    M2Array textures;
    M2Array transparency;
    M2Array textureFlipbooks;
    M2Array textureTransforms;
    M2Array replaceableTextureLookup;
    M2Array materials;
    M2Array boneLookup;
    M2Array textureLookup;
    M2Array textureUnitLookup;
    M2Array transparencyLookup;
    M2Array textureTransformLookup;
    f32 boundsMin[3];
    f32 boundsMax[3];
    f32 boundsRadius;
    f32 collisionMin[3];
    f32 collisionMax[3];
    f32 collisionRadius;
    M2Array collisionIndices;
    M2Array collisionPositions;
    M2Array collisionNormals;
    M2Array attachments;
    M2Array attachmentLookup;
    M2Array events;
    M2Array lights;
    M2Array cameras;
    M2Array cameraLookup;
    M2Array ribbonEmitters;
    M2Array particleEmitters;
};
static_assert(sizeof(M2Header) == 324, "M2 v256 header layout");

struct M2SkinProfile {
    M2Array vertices; // u16: view vertex -> model vertex
    M2Array indices;  // u16: triangles, into the view's vertices
    M2Array bones;    // u8[4] per view vertex
    M2Array submeshes;
    M2Array batches;
    u32 boneCountMax;
};
static_assert(sizeof(M2SkinProfile) == 44, "M2 v256 skin profile layout");

struct M2Submesh {
    u16 id;
    u16 level; // high 16 bits of indexStart for views with more than 65535 indices
    u16 vertexStart;
    u16 vertexCount;
    u16 indexStart;
    u16 indexCount;
    u16 boneCount;
    u16 boneComboIndex;
    u16 boneInfluences;
    u16 centerBoneIndex;
    f32 center[3];
};
static_assert(sizeof(M2Submesh) == 32, "M2 v256 submesh layout");

struct M2BatchRecord {
    u8 flags;
    i8 priorityPlane;
    u16 shaderId;
    u16 submesh;
    u16 geosetIndex;
    i16 colorIndex;
    u16 materialIndex;
    u16 materialLayer;
    u16 textureCount;
    u16 textureComboIndex;
    u16 textureCoordComboIndex;
    u16 textureWeightComboIndex;
    u16 textureTransformComboIndex;
};
static_assert(sizeof(M2BatchRecord) == 24, "M2 batch layout");

struct M2TextureRecord {
    u32 type;
    u32 flags;
    M2Array name;
};
static_assert(sizeof(M2TextureRecord) == 16, "M2 texture layout");

struct M2TrackHeader {
    u16 interpolation;
    i16 globalSequence;
    M2Array ranges; // pairs of u32
    M2Array times;  // u32
    M2Array values;
};
static_assert(sizeof(M2TrackHeader) == 28, "M2 v256 track layout");

// M2CompBone before 2.0: no bone name CRC, float quaternions.
struct M2BoneRecord {
    i32 keyBoneId;
    u32 flags;
    i16 parent;
    u16 submesh;
    M2TrackHeader translation;
    M2TrackHeader rotation;
    M2TrackHeader scale;
    f32 pivot[3];
};
static_assert(sizeof(M2BoneRecord) == 108, "M2 v256 bone layout");

// Vanilla sequences carry start and end on the track timeline (later versions: a duration).
struct M2SequenceRecord {
    u16 animationId;
    u16 variation;
    u32 start;
    u32 end;
    f32 moveSpeed;
    u32 flags;
    i16 frequency;
    u16 padding;
    u32 replayMin;
    u32 replayMax;
    u32 blendTime;
    f32 boundsMin[3];
    f32 boundsMax[3];
    f32 boundsRadius;
    i16 nextVariation;
    u16 aliasNext;
};
static_assert(sizeof(M2SequenceRecord) == 68, "M2 v256 sequence layout");

struct M2AttachmentRecord {
    u32 id;
    u16 bone;
    u16 unknown;
    f32 position[3];
    M2TrackHeader animateAttached;
};
static_assert(sizeof(M2AttachmentRecord) == 48, "M2 v256 attachment layout");

// Phase 14: M2Event before 2.0: the bone as a u32 and a timestamp-only track (M2TrackBase: no values).
struct M2EventRecord {
    u32 identifier;
    u32 data;
    u32 bone;
    f32 position[3];
    u16 interpolation;
    i16 globalSequence;
    M2Array ranges; // pairs of u32
    M2Array times;  // u32
};
static_assert(sizeof(M2EventRecord) == 44, "M2 v256 event layout");

struct M2ColorRecord {
    M2TrackHeader color; // C3Vector
    M2TrackHeader alpha; // fixed16
};
static_assert(sizeof(M2ColorRecord) == 56, "M2 color layout");

struct M2TextureTransformRecord {
    M2TrackHeader translation; // C3Vector
    M2TrackHeader rotation;    // C4Quaternion
    M2TrackHeader scale;       // C3Vector
};
static_assert(sizeof(M2TextureTransformRecord) == 84, "M2 texture transform layout");

// M2ParticleOld before 2.0: 16-bit blend and emitter type, and the particle's color, opacity, size and texture cell
// over its life as three fixed points ("fake animation blocks" came later).
struct M2ParticleRecord {
    i32 id;
    u32 flags;
    f32 position[3];
    u16 bone;
    u16 texture;
    M2Array geometryModel;  // char
    M2Array recursionModel; // char
    u16 blend;
    u16 emitterType;
    u8 particleType;
    u8 headOrTail;
    i16 tileRotation;
    u16 rows;
    u16 columns;
    // speed, speed variation, vertical range, horizontal range, gravity, lifespan, emission rate, area length,
    // area width, z source: float tracks.
    M2TrackHeader tracks[10];
    f32 midPoint;
    u32 colors[3]; // BGRA
    f32 scales[3];
    u16 headLifeCells[3];
    u16 headDecayCells[3];
    i16 tailLifeCells[2];
    i16 tailDecayCells[2];
    f32 tailLength;
    f32 twinkleSpeed;
    f32 twinklePercent;
    f32 twinkleScale[2];
    f32 burstMultiplier;
    f32 drag;
    f32 spin;
    f32 tumble[6];
    f32 wind[3];
    f32 windTime;
    f32 follow[4]; // speed 1, scale 1, speed 2, scale 2
    M2Array splinePoints; // C3Vector
    M2TrackHeader enabled; // u8
};
static_assert(sizeof(M2ParticleRecord) == 504, "M2 v256 particle emitter layout");
static_assert(offsetof(M2ParticleRecord, midPoint) == 0x14C, "M2 v256 particle emitter layout");

struct M2RibbonRecord {
    i32 id;
    u32 bone;
    f32 position[3];
    M2Array textures;  // u16
    M2Array materials; // u16
    M2TrackHeader color;
    M2TrackHeader alpha;
    M2TrackHeader above;
    M2TrackHeader below;
    f32 edgesPerSecond;
    f32 edgeLifetime;
    f32 gravity;
    u16 rows;
    u16 columns;
    M2TrackHeader textureSlot;
    M2TrackHeader visibility;
};
static_assert(sizeof(M2RibbonRecord) == 220, "M2 v256 ribbon emitter layout");

// M2Camera before 2.0: a float field of view (later versions animate it), tracks of spline keys.
struct M2CameraRecord {
    i32 type; // 0 portrait, 1 character info, -1 flyby
    f32 fov;
    f32 farClip;
    f32 nearClip;
    M2TrackHeader position; // M2SplineKey<C3Vector>
    f32 positionBase[3];
    M2TrackHeader target; // M2SplineKey<C3Vector>
    f32 targetBase[3];
    M2TrackHeader roll; // M2SplineKey<float>
};
static_assert(sizeof(M2CameraRecord) == 124, "M2 v256 camera layout");

// Keys of the camera tracks: the value and its in and out tangents (hermite / bezier).
struct M2SplineVec3 {
    Vec3 value;
    Vec3 inTangent;
    Vec3 outTangent;
};
static_assert(sizeof(M2SplineVec3) == 36, "M2 spline key layout");
struct M2SplineFloat {
    f32 value;
    f32 inTangent;
    f32 outTangent;
};
static_assert(sizeof(M2SplineFloat) == 12, "M2 spline key layout");

// Phase 11: M2Light of 1.12.
struct M2LightRecord {
    u16 type;
    i16 bone;
    f32 position[3];
    M2TrackHeader ambientColor;
    M2TrackHeader ambientIntensity;
    M2TrackHeader diffuseColor;
    M2TrackHeader diffuseIntensity;
    M2TrackHeader attenuationStart;
    M2TrackHeader attenuationEnd;
    M2TrackHeader visibility;
};
static_assert(sizeof(M2LightRecord) == 212, "M2 v256 light layout");

template <typename T>
Result<std::vector<T>> copyArray(std::span<const u8> file, const M2Array& array) {
    if (u64{array.offset} + u64{array.count} * sizeof(T) > file.size()) {
        return Error{"M2 array out of bounds"};
    }
    std::vector<T> out(array.count);
    if (array.count) {
        std::memcpy(out.data(), file.data() + array.offset, array.count * sizeof(T));
    }
    return out;
}

template <typename T>
Result<M2Track<T>> readTrack(std::span<const u8> file, const M2TrackHeader& h) {
    M2Track<T> track;
    track.interpolation = h.interpolation;
    track.globalSequence = h.globalSequence;
    Result<std::vector<std::array<u32, 2>>> ranges = copyArray<std::array<u32, 2>>(file, h.ranges);
    Result<std::vector<u32>> times = copyArray<u32>(file, h.times);
    Result<std::vector<T>> values = copyArray<T>(file, h.values);
    if (!ranges || !times || !values) {
        return Error{"M2 track out of bounds"};
    }
    if (values.value().size() < times.value().size()) {
        return Error{"M2 track with fewer values than timestamps"};
    }
    track.ranges = std::move(ranges.value());
    track.times = std::move(times.value());
    track.values = std::move(values.value());
    track.values.resize(track.times.size()); // hermite/bezier tangents (never seen in 1.12 data) are dropped
    return track;
}

// First key of a fixed16 opacity track (1 when it has none).
Result<f32> staticOpacity(std::span<const u8> file, const M2TrackHeader& h) {
    Result<M2Track<i16>> track = readTrack<i16>(file, h);
    if (!track) {
        return track.error();
    }
    return track.value().values.empty() ? 1.0f : std::clamp(track.value().values[0] / 32767.0f, 0.0f, 1.0f);
}

std::string arrayString(std::span<const u8> file, const M2Array& array) {
    if (array.count == 0 || u64{array.offset} + array.count > file.size()) {
        return {};
    }
    return stringAt(file.subspan(array.offset, array.count), 0);
}

// Texture transforms, particle and ribbon emitters. Indices into the model's bones, textures and materials are
// checked here, so the bakers can follow them.
Result<void> parseEffects(std::span<const u8> data, const M2Header& h, M2Model& model) {
    Result<std::vector<M2TextureTransformRecord>> transforms =
        copyArray<M2TextureTransformRecord>(data, h.textureTransforms);
    if (!transforms) {
        return Error{"M2 texture transforms out of bounds"};
    }
    for (const M2TextureTransformRecord& r : transforms.value()) {
        Result<M2Track<Vec3>> translation = readTrack<Vec3>(data, r.translation);
        Result<M2Track<Quat>> rotation = readTrack<Quat>(data, r.rotation);
        Result<M2Track<Vec3>> scale = readTrack<Vec3>(data, r.scale);
        if (!translation || !rotation || !scale) {
            return Error{"M2 texture transform track out of bounds"};
        }
        model.textureTransforms.push_back(
            {std::move(translation.value()), std::move(rotation.value()), std::move(scale.value())});
    }

    Result<std::vector<M2ParticleRecord>> particles = copyArray<M2ParticleRecord>(data, h.particleEmitters);
    if (!particles) {
        return Error{"M2 particle emitters out of bounds"};
    }
    for (usize i = 0; i < particles.value().size(); ++i) {
        const M2ParticleRecord& r = particles.value()[i];
        const std::string where = "M2 particle emitter " + std::to_string(i);
        if (r.bone >= model.bones.size() || r.texture >= model.textures.size()) {
            return Error{where + ": bone or texture out of range"};
        }
        M2ParticleEmitter& out = model.particles.emplace_back();
        out.id = r.id;
        out.flags = r.flags;
        std::memcpy(out.position.data(), r.position, sizeof(r.position));
        out.bone = r.bone;
        out.texture = r.texture;
        out.geometryModel = arrayString(data, r.geometryModel);
        out.recursionModel = arrayString(data, r.recursionModel);
        out.blend = r.blend;
        out.emitterType = r.emitterType;
        out.particleType = r.particleType;
        out.headOrTail = r.headOrTail;
        out.tileRotation = r.tileRotation;
        out.rows = r.rows;
        out.columns = r.columns;
        M2Track<f32>* tracks[10] = {&out.speed,     &out.speedVariation, &out.verticalRange, &out.horizontalRange,
                                    &out.gravity,   &out.lifespan,       &out.emissionRate,  &out.areaLength,
                                    &out.areaWidth, &out.zSource};
        for (u32 t = 0; t < 10; ++t) {
            Result<M2Track<f32>> track = readTrack<f32>(data, r.tracks[t]);
            if (!track) {
                return Error{where + ": " + track.error().message};
            }
            *tracks[t] = std::move(track.value());
        }
        Result<M2Track<u8>> enabled = readTrack<u8>(data, r.enabled);
        Result<std::vector<Vec3>> spline = copyArray<Vec3>(data, r.splinePoints);
        if (!enabled || !spline) {
            return Error{where + ": enabled track or spline out of bounds"};
        }
        out.enabled = std::move(enabled.value());
        out.splinePoints = std::move(spline.value());
        out.midPoint = r.midPoint;
        std::memcpy(out.colors.data(), r.colors, sizeof(r.colors));
        std::memcpy(out.scales.data(), r.scales, sizeof(r.scales));
        std::memcpy(out.headLifeCells.data(), r.headLifeCells, sizeof(r.headLifeCells));
        std::memcpy(out.headDecayCells.data(), r.headDecayCells, sizeof(r.headDecayCells));
        std::memcpy(out.tailLifeCells.data(), r.tailLifeCells, sizeof(r.tailLifeCells));
        std::memcpy(out.tailDecayCells.data(), r.tailDecayCells, sizeof(r.tailDecayCells));
        out.tailLength = r.tailLength;
        out.twinkleSpeed = r.twinkleSpeed;
        out.twinklePercent = r.twinklePercent;
        out.twinkleMin = r.twinkleScale[0];
        out.twinkleMax = r.twinkleScale[1];
        out.burstMultiplier = r.burstMultiplier;
        out.drag = r.drag;
        out.spin = r.spin;
        std::memcpy(out.tumbleMin.data(), r.tumble, 12);
        std::memcpy(out.tumbleMax.data(), r.tumble + 3, 12);
        std::memcpy(out.wind.data(), r.wind, sizeof(r.wind));
        out.windTime = r.windTime;
        out.followSpeed1 = r.follow[0];
        out.followScale1 = r.follow[1];
        out.followSpeed2 = r.follow[2];
        out.followScale2 = r.follow[3];
    }

    Result<std::vector<M2RibbonRecord>> ribbons = copyArray<M2RibbonRecord>(data, h.ribbonEmitters);
    if (!ribbons) {
        return Error{"M2 ribbon emitters out of bounds"};
    }
    for (usize i = 0; i < ribbons.value().size(); ++i) {
        const M2RibbonRecord& r = ribbons.value()[i];
        const std::string where = "M2 ribbon emitter " + std::to_string(i);
        M2RibbonEmitter& out = model.ribbons.emplace_back();
        out.id = r.id;
        out.bone = r.bone;
        std::memcpy(out.position.data(), r.position, sizeof(r.position));
        Result<std::vector<u16>> textures = copyArray<u16>(data, r.textures);
        Result<std::vector<u16>> materials = copyArray<u16>(data, r.materials);
        Result<M2Track<Vec3>> color = readTrack<Vec3>(data, r.color);
        Result<M2Track<i16>> alpha = readTrack<i16>(data, r.alpha);
        Result<M2Track<f32>> above = readTrack<f32>(data, r.above);
        Result<M2Track<f32>> below = readTrack<f32>(data, r.below);
        Result<M2Track<u16>> slot = readTrack<u16>(data, r.textureSlot);
        Result<M2Track<u8>> visibility = readTrack<u8>(data, r.visibility);
        if (!textures || !materials || !color || !alpha || !above || !below || !slot || !visibility) {
            return Error{where + ": array or track out of bounds"};
        }
        out.textures = std::move(textures.value());
        out.materials = std::move(materials.value());
        if (r.bone >= model.bones.size()) {
            return Error{where + ": bone out of range"};
        }
        for (u16 t : out.textures) {
            if (t >= model.textures.size()) {
                return Error{where + ": texture out of range"};
            }
        }
        for (u16 m : out.materials) {
            if (m >= model.materials.size()) {
                return Error{where + ": material out of range"};
            }
        }
        out.color = std::move(color.value());
        out.alpha = std::move(alpha.value());
        out.above = std::move(above.value());
        out.below = std::move(below.value());
        out.edgesPerSecond = r.edgesPerSecond;
        out.edgeLifetime = r.edgeLifetime;
        out.gravity = r.gravity;
        out.rows = r.rows;
        out.columns = r.columns;
        out.textureSlot = std::move(slot.value());
        out.visibility = std::move(visibility.value());
    }
    model.textureFlipbookCount = h.textureFlipbooks.count;
    return {};
}

// A spline track's values without their tangents.
template <typename Spline, typename T>
M2Track<T> splineValues(M2Track<Spline>&& spline) {
    M2Track<T> out;
    out.interpolation = spline.interpolation;
    out.globalSequence = spline.globalSequence;
    out.ranges = std::move(spline.ranges);
    out.times = std::move(spline.times);
    out.values.reserve(spline.values.size());
    for (const Spline& key : spline.values) {
        out.values.push_back(key.value);
    }
    return out;
}

Result<void> parseCameras(std::span<const u8> data, const M2Header& h, M2Model& model) {
    Result<std::vector<M2CameraRecord>> cameras = copyArray<M2CameraRecord>(data, h.cameras);
    Result<std::vector<i16>> lookup = copyArray<i16>(data, h.cameraLookup);
    if (!cameras || !lookup) {
        return Error{"M2 cameras out of bounds"};
    }
    for (const M2CameraRecord& r : cameras.value()) {
        Result<M2Track<M2SplineVec3>> position = readTrack<M2SplineVec3>(data, r.position);
        Result<M2Track<M2SplineVec3>> target = readTrack<M2SplineVec3>(data, r.target);
        Result<M2Track<M2SplineFloat>> roll = readTrack<M2SplineFloat>(data, r.roll);
        if (!position || !target || !roll) {
            return Error{"M2 camera track out of bounds"};
        }
        M2Camera& out = model.cameras.emplace_back();
        out.type = r.type;
        out.fov = r.fov;
        out.farClip = r.farClip;
        out.nearClip = r.nearClip;
        std::memcpy(out.positionBase.data(), r.positionBase, sizeof(r.positionBase));
        std::memcpy(out.targetBase.data(), r.targetBase, sizeof(r.targetBase));
        for (const M2SplineVec3& key : position.value().values) {
            out.positionTangents.push_back({key.inTangent, key.outTangent});
        }
        for (const M2SplineVec3& key : target.value().values) {
            out.targetTangents.push_back({key.inTangent, key.outTangent});
        }
        for (const M2SplineFloat& key : roll.value().values) {
            out.rollTangents.push_back({key.inTangent, key.outTangent});
        }
        out.position = splineValues<M2SplineVec3, Vec3>(std::move(position.value()));
        out.target = splineValues<M2SplineVec3, Vec3>(std::move(target.value()));
        out.roll = splineValues<M2SplineFloat, f32>(std::move(roll.value()));
    }
    model.cameraLookup = std::move(lookup.value());
    return {};
}

Result<void> parseLights(std::span<const u8> data, const M2Header& h, M2Model& model) {
    Result<std::vector<M2LightRecord>> lights = copyArray<M2LightRecord>(data, h.lights);
    if (!lights) {
        return Error{"M2 lights out of bounds"};
    }
    for (const M2LightRecord& r : lights.value()) {
        M2Light out;
        out.type = r.type;
        out.bone = r.bone >= 0 && static_cast<usize>(r.bone) < model.bones.size() ? r.bone : static_cast<i16>(-1);
        std::memcpy(out.position.data(), r.position, sizeof(r.position));
        Result<M2Track<Vec3>> ambientColor = readTrack<Vec3>(data, r.ambientColor);
        Result<M2Track<f32>> ambientIntensity = readTrack<f32>(data, r.ambientIntensity);
        Result<M2Track<Vec3>> diffuseColor = readTrack<Vec3>(data, r.diffuseColor);
        Result<M2Track<f32>> diffuseIntensity = readTrack<f32>(data, r.diffuseIntensity);
        Result<M2Track<f32>> attenuationStart = readTrack<f32>(data, r.attenuationStart);
        Result<M2Track<f32>> attenuationEnd = readTrack<f32>(data, r.attenuationEnd);
        Result<M2Track<u8>> visibility = readTrack<u8>(data, r.visibility);
        if (!ambientColor || !ambientIntensity || !diffuseColor || !diffuseIntensity || !attenuationStart ||
            !attenuationEnd || !visibility) {
            return Error{"M2 light track out of bounds"};
        }
        out.ambientColor = std::move(ambientColor.value());
        out.ambientIntensity = std::move(ambientIntensity.value());
        out.diffuseColor = std::move(diffuseColor.value());
        out.diffuseIntensity = std::move(diffuseIntensity.value());
        out.attenuationStart = std::move(attenuationStart.value());
        out.attenuationEnd = std::move(attenuationEnd.value());
        out.visibility = std::move(visibility.value());
        model.lights.push_back(std::move(out));
    }
    return {};
}

Result<void> parseEvents(std::span<const u8> data, const M2Header& h, M2Model& model) {
    Result<std::vector<M2EventRecord>> events = copyArray<M2EventRecord>(data, h.events);
    if (!events) {
        return Error{"M2 events out of bounds"};
    }
    for (const M2EventRecord& r : events.value()) {
        Result<std::vector<std::array<u32, 2>>> ranges = copyArray<std::array<u32, 2>>(data, r.ranges);
        Result<std::vector<u32>> times = copyArray<u32>(data, r.times);
        if (!ranges || !times) {
            return Error{"M2 event times out of bounds"};
        }
        M2Event out;
        out.id = r.identifier;
        out.data = r.data;
        out.bone = r.bone < model.bones.size() ? static_cast<u16>(r.bone) : 0;
        std::memcpy(out.position.data(), r.position, sizeof(r.position));
        out.globalSequence = r.globalSequence;
        out.ranges = std::move(ranges.value());
        out.times = std::move(times.value());
        model.events.push_back(std::move(out));
    }
    return {};
}

} // namespace

const M2Camera* M2Model::camera(i32 type) const {
    if (type >= 0 && static_cast<usize>(type) < cameraLookup.size()) {
        const i16 index = cameraLookup[static_cast<usize>(type)];
        if (index >= 0 && static_cast<usize>(index) < cameras.size() &&
            cameras[static_cast<usize>(index)].type == type) {
            return &cameras[static_cast<usize>(index)];
        }
    }
    for (const M2Camera& c : cameras) {
        if (c.type == type) {
            return &c;
        }
    }
    return nullptr;
}

Result<M2Model> parseM2(std::span<const u8> data) {
    if (data.size() < sizeof(M2Header)) {
        return Error{"M2 too small"};
    }
    const M2Header h = load<M2Header>(data.data());
    if (std::memcmp(h.magic, "MD20", 4) != 0) {
        return Error{"Not an M2 (MD20)"};
    }
    if (h.version < 256 || h.version > 263) {
        return Error{"M2 version " + std::to_string(h.version) + " is not 1.12's"};
    }
    if (h.views.count == 0) {
        return Error{"M2 without views"};
    }

    M2Model model;
    model.globalFlags = h.globalFlags;
    model.sequenceCount = h.sequences.count;
    std::memcpy(model.boundsMin.data(), h.boundsMin, sizeof(h.boundsMin));
    std::memcpy(model.boundsMax.data(), h.boundsMax, sizeof(h.boundsMax));
    model.boundsRadius = h.boundsRadius;
    if (h.name.count > 0 && u64{h.name.offset} + h.name.count <= data.size()) {
        model.name = stringAt(data.subspan(h.name.offset, h.name.count), 0);
    }

    Result<std::vector<M2Vertex>> vertices = copyArray<M2Vertex>(data, h.vertices);
    if (!vertices) {
        return vertices.error();
    }
    model.vertices = std::move(vertices.value());

    Result<std::vector<u16>> collisionIndices = copyArray<u16>(data, h.collisionIndices);
    Result<std::vector<Vec3>> collisionPositions = copyArray<Vec3>(data, h.collisionPositions);
    if (!collisionIndices || !collisionPositions) {
        return Error{"M2 collision arrays out of bounds"};
    }
    model.collisionPositions = std::move(collisionPositions.value());
    model.collisionIndices = std::move(collisionIndices.value());
    model.collisionIndices.resize(model.collisionIndices.size() / 3 * 3);
    for (u16 index : model.collisionIndices) {
        if (index >= model.collisionPositions.size()) {
            return Error{"M2 collision index out of range"};
        }
    }

    Result<std::vector<M2TextureRecord>> textures = copyArray<M2TextureRecord>(data, h.textures);
    if (!textures) {
        return textures.error();
    }
    for (const M2TextureRecord& t : textures.value()) {
        M2Texture& out = model.textures.emplace_back();
        out.type = t.type;
        out.flags = t.flags;
        if (t.name.count > 0 && u64{t.name.offset} + t.name.count <= data.size()) {
            out.path = stringAt(data.subspan(t.name.offset, t.name.count), 0);
        }
    }
    Result<std::vector<std::array<u16, 2>>> materials = copyArray<std::array<u16, 2>>(data, h.materials);
    Result<std::vector<u16>> textureLookup = copyArray<u16>(data, h.textureLookup);
    if (!materials || !textureLookup) {
        return Error{"M2 materials out of bounds"};
    }
    for (const std::array<u16, 2>& m : materials.value()) {
        model.materials.push_back({m[0], static_cast<M2Blend>(m[1])});
    }

    // View 0 = the most detailed level.
    Result<std::vector<M2SkinProfile>> views = copyArray<M2SkinProfile>(data, h.views);
    if (!views) {
        return views.error();
    }
    const M2SkinProfile& view = views.value()[0];
    Result<std::vector<u16>> viewVertices = copyArray<u16>(data, view.vertices);
    Result<std::vector<u16>> viewIndices = copyArray<u16>(data, view.indices);
    Result<std::vector<M2Submesh>> submeshes = copyArray<M2Submesh>(data, view.submeshes);
    Result<std::vector<M2BatchRecord>> batches = copyArray<M2BatchRecord>(data, view.batches);
    if (!viewVertices || !viewIndices || !submeshes || !batches) {
        return Error{"M2 view out of bounds"};
    }
    model.triangles.reserve(viewIndices.value().size());
    for (u16 index : viewIndices.value()) {
        if (index >= viewVertices.value().size() || viewVertices.value()[index] >= model.vertices.size()) {
            return Error{"M2 view index out of range"};
        }
        model.triangles.push_back(viewVertices.value()[index]);
    }
    // Static opacity of each batch (ghosts, glass): its color's alpha times its transparency weight.
    Result<std::vector<M2ColorRecord>> colors = copyArray<M2ColorRecord>(data, h.colors);
    Result<std::vector<M2TrackHeader>> transparency = copyArray<M2TrackHeader>(data, h.transparency);
    Result<std::vector<u16>> transparencyLookup = copyArray<u16>(data, h.transparencyLookup);
    if (!colors || !transparency || !transparencyLookup) {
        return Error{"M2 colors out of bounds"};
    }
    // Texture transforms per batch (unreadable lookups leave the batches unanimated, see parseEffects).
    Result<std::vector<u16>> transformLookup = copyArray<u16>(data, h.textureTransformLookup);
    for (const M2BatchRecord& b : batches.value()) {
        if (b.submesh >= submeshes.value().size()) {
            return Error{"M2 batch submesh out of range"};
        }
        const M2Submesh& s = submeshes.value()[b.submesh];
        M2Batch& out = model.batches.emplace_back();
        out.firstIndex = (u32{s.level} << 16) | s.indexStart;
        out.indexCount = s.indexCount;
        out.geosetId = s.id;
        out.priorityPlane = b.priorityPlane;
        out.flags = b.flags;
        out.material = b.materialIndex;
        if (b.textureComboIndex < textureLookup.value().size() &&
            textureLookup.value()[b.textureComboIndex] < model.textures.size()) {
            out.texture = textureLookup.value()[b.textureComboIndex];
        }
        if (u64{out.firstIndex} + out.indexCount > model.triangles.size() || out.material >= model.materials.size()) {
            return Error{"M2 batch out of range"};
        }
        if (b.colorIndex >= 0 && static_cast<u32>(b.colorIndex) < colors.value().size()) {
            Result<f32> alpha = staticOpacity(data, colors.value()[b.colorIndex].alpha);
            if (!alpha) {
                return alpha.error();
            }
            out.alpha *= alpha.value();
            out.color = b.colorIndex;
        }
        if (b.textureWeightComboIndex < transparencyLookup.value().size() &&
            transparencyLookup.value()[b.textureWeightComboIndex] < transparency.value().size()) {
            const u16 index = transparencyLookup.value()[b.textureWeightComboIndex];
            Result<f32> weight = staticOpacity(data, transparency.value()[index]);
            if (!weight) {
                return weight.error();
            }
            out.alpha *= weight.value();
            out.transparency = static_cast<i16>(index);
        }
        if (transformLookup && b.textureTransformComboIndex < transformLookup.value().size() &&
            transformLookup.value()[b.textureTransformComboIndex] < h.textureTransforms.count) {
            out.textureTransform = static_cast<i16>(transformLookup.value()[b.textureTransformComboIndex]);
        }
    }
    for (const M2ColorRecord& r : colors.value()) {
        Result<M2Track<Vec3>> color = readTrack<Vec3>(data, r.color);
        Result<M2Track<i16>> alpha = readTrack<i16>(data, r.alpha);
        if (!color || !alpha) {
            return Error{"M2 color track out of bounds"};
        }
        model.colors.push_back({std::move(color.value()), std::move(alpha.value())});
    }
    for (const M2TrackHeader& r : transparency.value()) {
        Result<M2Track<i16>> weight = readTrack<i16>(data, r);
        if (!weight) {
            return weight.error();
        }
        model.transparency.push_back(std::move(weight.value()));
    }

    // Skeleton and animations.
    Result<std::vector<u32>> globalSequences = copyArray<u32>(data, h.globalSequences);
    Result<std::vector<M2SequenceRecord>> sequences = copyArray<M2SequenceRecord>(data, h.sequences);
    Result<std::vector<M2BoneRecord>> bones = copyArray<M2BoneRecord>(data, h.bones);
    Result<std::vector<M2AttachmentRecord>> attachments = copyArray<M2AttachmentRecord>(data, h.attachments);
    if (!globalSequences || !sequences || !bones || !attachments) {
        return Error{"M2 skeleton out of bounds"};
    }
    model.globalSequences = std::move(globalSequences.value());
    for (const M2SequenceRecord& r : sequences.value()) {
        M2Sequence& out = model.sequences.emplace_back();
        out.animationId = r.animationId;
        out.variation = r.variation;
        out.start = r.start;
        out.end = std::max(r.start, r.end);
        out.moveSpeed = r.moveSpeed;
        out.flags = r.flags;
        out.frequency = r.frequency;
        out.replayMin = r.replayMin;
        out.replayMax = r.replayMax;
        out.blendTime = r.blendTime;
        std::memcpy(out.boundsMin.data(), r.boundsMin, sizeof(r.boundsMin));
        std::memcpy(out.boundsMax.data(), r.boundsMax, sizeof(r.boundsMax));
        out.boundsRadius = r.boundsRadius;
        out.nextVariation = r.nextVariation;
        out.aliasNext = r.aliasNext;
    }
    for (usize i = 0; i < bones.value().size(); ++i) {
        const M2BoneRecord& r = bones.value()[i];
        if (r.parent >= static_cast<i32>(i) || r.parent < -1) {
            return Error{"M2 bone " + std::to_string(i) + " has parent " + std::to_string(r.parent)};
        }
        M2Bone& out = model.bones.emplace_back();
        out.keyBoneId = r.keyBoneId;
        out.flags = r.flags;
        out.parent = r.parent;
        out.submesh = r.submesh;
        std::memcpy(out.pivot.data(), r.pivot, sizeof(r.pivot));
        Result<M2Track<Vec3>> translation = readTrack<Vec3>(data, r.translation);
        Result<M2Track<Quat>> rotation = readTrack<Quat>(data, r.rotation);
        Result<M2Track<Vec3>> scale = readTrack<Vec3>(data, r.scale);
        if (!translation || !rotation || !scale) {
            return Error{"M2 bone " + std::to_string(i) + " track out of bounds"};
        }
        out.translation = std::move(translation.value());
        out.rotation = std::move(rotation.value());
        out.scale = std::move(scale.value());
    }
    for (const M2Vertex& v : model.vertices) {
        for (u32 k = 0; k < 4; ++k) {
            if (v.boneWeights[k] != 0 && v.boneIndices[k] >= model.bones.size()) {
                return Error{"M2 vertex bone out of range"};
            }
        }
    }
    for (const M2AttachmentRecord& r : attachments.value()) {
        if (r.bone >= model.bones.size()) {
            continue;
        }
        M2Attachment& out = model.attachments.emplace_back();
        out.id = r.id;
        out.bone = r.bone;
        std::memcpy(out.position.data(), r.position, sizeof(r.position));
    }
    if (Result<void> effects = parseEffects(data, h, model); !effects) {
        model.effectsError = effects.error().message;
        model.textureTransforms.clear();
        model.particles.clear();
        model.ribbons.clear();
        for (M2Batch& b : model.batches) {
            b.textureTransform = -1;
        }
    }
    if (Result<void> cameras = parseCameras(data, h, model); !cameras) {
        model.cameras.clear(); // unreadable: the model is framed without them (the portrait's fallback)
        model.cameraLookup.clear();
    }
    if (Result<void> lights = parseLights(data, h, model); !lights) {
        model.lights.clear(); // unreadable: drawn without its own light
    }
    if (Result<void> events = parseEvents(data, h, model); !events) {
        model.events.clear(); // unreadable: silent
    }
    return model;
}

f32 sampleM2Track(const M2Track<f32>& track, u32 sequence, u32 time, f32 fallback) {
    if (track.times.empty()) {
        return fallback;
    }
    u32 first = 0;
    u32 last = static_cast<u32>(track.times.size()) - 1;
    if (track.globalSequence < 0 && sequence < track.ranges.size()) {
        first = std::min(track.ranges[sequence][0], last);
        last = std::clamp(track.ranges[sequence][1], first, last);
    }
    if (time <= track.times[first] || first == last) {
        return track.values[first];
    }
    if (time >= track.times[last]) {
        return track.values[last];
    }
    u32 k = first;
    while (k + 1 < last && track.times[k + 1] <= time) {
        ++k;
    }
    const f32 a = track.values[k];
    if (track.interpolation == 0) {
        return a;
    }
    const f32 b = track.values[k + 1];
    const u32 span = track.times[k + 1] - track.times[k];
    const f32 t = span ? static_cast<f32>(time - track.times[k]) / static_cast<f32>(span) : 0.0f;
    return a + (b - a) * t;
}

Vec3 sampleM2Track(const M2Track<Vec3>& track, u32 sequence, u32 time, const Vec3& fallback) {
    if (track.times.empty()) {
        return fallback;
    }
    u32 first = 0;
    u32 last = static_cast<u32>(track.times.size()) - 1;
    if (track.globalSequence < 0 && sequence < track.ranges.size()) {
        first = std::min(track.ranges[sequence][0], last);
        last = std::clamp(track.ranges[sequence][1], first, last);
    }
    if (time <= track.times[first] || first == last) {
        return track.values[first];
    }
    if (time >= track.times[last]) {
        return track.values[last];
    }
    u32 k = first;
    while (k + 1 < last && track.times[k + 1] <= time) {
        ++k;
    }
    const Vec3& a = track.values[k];
    if (track.interpolation == 0) {
        return a;
    }
    const Vec3& b = track.values[k + 1];
    const u32 span = track.times[k + 1] - track.times[k];
    const f32 t = span ? static_cast<f32>(time - track.times[k]) / static_cast<f32>(span) : 0.0f;
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

Quat sampleM2Track(const M2Track<Quat>& track, u32 sequence, u32 time, const Quat& fallback) {
    if (track.times.empty()) {
        return fallback;
    }
    u32 first = 0;
    u32 last = static_cast<u32>(track.times.size()) - 1;
    if (track.globalSequence < 0 && sequence < track.ranges.size()) {
        first = std::min(track.ranges[sequence][0], last);
        last = std::clamp(track.ranges[sequence][1], first, last);
    }
    if (time <= track.times[first] || first == last) {
        return track.values[first];
    }
    if (time >= track.times[last]) {
        return track.values[last];
    }
    u32 k = first;
    while (k + 1 < last && track.times[k + 1] <= time) {
        ++k;
    }
    const Quat& a = track.values[k];
    if (track.interpolation == 0) {
        return a;
    }
    Quat b = track.values[k + 1];
    if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0.0f) {
        for (f32& c : b) {
            c = -c;
        }
    }
    const u32 span = track.times[k + 1] - track.times[k];
    const f32 t = span ? static_cast<f32>(time - track.times[k]) / static_cast<f32>(span) : 0.0f;
    Quat out;
    f32 length = 0.0f;
    for (u32 i = 0; i < 4; ++i) {
        out[i] = a[i] + (b[i] - a[i]) * t;
        length += out[i] * out[i];
    }
    length = length > 1e-12f ? 1.0f / std::sqrt(length) : 1.0f;
    for (f32& c : out) {
        c *= length;
    }
    return out;
}

} // namespace vd::formats
