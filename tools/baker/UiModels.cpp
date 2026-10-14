// Retouches (ui models): the models the 1.12 interface shows in its Model frames ("ui/models" of ui.vdpk,
// pack/UiModelFormat.h): the cooldown's sweep and flash, the pet's autocast sparkles and the minimap's ping. Their
// geometry, animation tracks and particle emitters as the M2 has them; the textures of their batches go to the UI
// atlas (kUiTextures / kAdditiveTextures of UiCommands.cpp), except the ones a texture coordinate animation moves:
// those must be the same in every row (cooldown.blp is a vertical edge) and are kept as their first row, which the UI
// draws as geometry with the client's clamping (ui/UiModels).

#include "BakerCommon.h"

#include "core/Hash.h"
#include "core/Log.h"
#include "formats/Blp.h"
#include "formats/M2.h"
#include "pack/UiFormat.h"
#include "pack/UiModelFormat.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace vd::baker {
namespace {

// In pack::UiModelId order.
constexpr const char* kModels[] = {
    "Interface\\Cooldown\\UI-Cooldown-Indicator.m2", // CooldownFrameTemplate (Cooldown.xml)
    "Interface\\Buttons\\UI-AutoCastButton.m2",      // PetActionButtonTemplate / SpellButtonTemplate's AutoCast
    "Interface\\MiniMap\\Ping\\MinimapPing.m2",      // Minimap.xml's MiniMapPing
};
static_assert(std::size(kModels) == static_cast<usize>(pack::UiModelId::Count));

struct Blob {
    std::vector<pack::UiModelInfo> models;
    std::vector<pack::UiModelSequence> sequences;
    std::vector<pack::UiModelBone> bones;
    std::vector<pack::UiModelVertex> vertices;
    std::vector<pack::UiModelBatch> batches;
    std::vector<pack::UiModelEmitter> emitters;
    std::vector<pack::UiModelTrack> tracks;
    std::vector<pack::UiModelRange> ranges;
    std::vector<pack::UiModelKey> keys;
    std::vector<u32> texels;
    std::vector<u32> globalSequences;

    // A track of the model (its keys, the key range of each sequence), or kUiModelNoTrack when it has no keys.
    template <typename T, typename Convert>
    u32 track(const formats::M2Track<T>& t, u8 components, Convert convert) {
        if (t.times.empty()) {
            return pack::kUiModelNoTrack;
        }
        pack::UiModelTrack out{};
        out.firstKey = static_cast<u32>(keys.size());
        out.keyCount = static_cast<u16>(t.times.size());
        out.interpolation = static_cast<u8>(t.interpolation);
        out.components = components;
        out.firstRange = static_cast<u32>(ranges.size());
        out.rangeCount = static_cast<u16>(t.ranges.size());
        out.globalSequence = t.globalSequence;
        for (usize i = 0; i < t.times.size(); ++i) {
            pack::UiModelKey key{};
            key.time = t.times[i];
            convert(t.values[i], key.value);
            keys.push_back(key);
        }
        for (const std::array<u32, 2>& r : t.ranges) {
            ranges.push_back({static_cast<u16>(r[0]), static_cast<u16>(r[1])});
        }
        tracks.push_back(out);
        return static_cast<u32>(tracks.size() - 1);
    }
    u32 vec3(const formats::M2Track<formats::Vec3>& t) {
        return track(t, 3, [](const formats::Vec3& v, f32* out) { std::memcpy(out, v.data(), 12); });
    }
    u32 quat(const formats::M2Track<formats::Quat>& t) {
        return track(t, 4, [](const formats::Quat& q, f32* out) { std::memcpy(out, q.data(), 16); });
    }
    u32 fixed16(const formats::M2Track<i16>& t) {
        return track(t, 1, [](i16 v, f32* out) { out[0] = static_cast<f32>(v) / 32767.0f; });
    }
};

// A fixed16 opacity track that is 0 at every key (ping6.blp's batches): never seen.
bool alwaysZero(const formats::M2Track<i16>& t) {
    return !t.values.empty() && std::all_of(t.values.begin(), t.values.end(), [](i16 v) { return v == 0; });
}

// The particle emitters' parameters are constant in the interface's models: the first key (or the default).
f32 constant(const formats::M2Track<f32>& t, f32 fallback, const char* model, const char* name, int& failures) {
    if (t.values.empty()) {
        return fallback;
    }
    if (std::any_of(t.values.begin(), t.values.end(), [&](f32 v) { return v != t.values[0]; })) {
        VD_LOG_ERROR("ui models: %s: the emitter's %s is animated (unsupported)", model, name);
        ++failures;
    }
    return t.values[0];
}

// M2ParticleOld colors (BGRA read as a little-endian u32) -> RGBA8 with red in the low byte.
u32 bgraToRgba(u32 v) {
    return ((v >> 16) & 0xFFu) | (v & 0xFF00u) | ((v & 0xFFu) << 16) | (v & 0xFF000000u);
}

bool bakeModel(const formats::MpqArchiveSet& mpq, const char* path, Blob& blob, int& failures) {
    Result<std::vector<u8>> bytes = mpq.read(path);
    if (!bytes) {
        VD_LOG_ERROR("ui models: %s: %s", path, bytes.error().message.c_str());
        return false;
    }
    Result<formats::M2Model> parsed = formats::parseM2(bytes.value());
    if (!parsed) {
        VD_LOG_ERROR("ui models: %s: %s", path, parsed.error().message.c_str());
        return false;
    }
    const formats::M2Model& m = parsed.value();
    pack::UiModelInfo info{};
    info.path = assetPathHash(path);
    info.firstSequence = static_cast<u16>(blob.sequences.size());
    info.sequenceCount = static_cast<u16>(m.sequences.size());
    info.firstBone = static_cast<u16>(blob.bones.size());
    info.boneCount = static_cast<u16>(m.bones.size());
    info.firstBatch = static_cast<u16>(blob.batches.size());
    info.firstEmitter = static_cast<u16>(blob.emitters.size());
    info.firstGlobalSequence = static_cast<u16>(blob.globalSequences.size());
    info.globalSequenceCount = static_cast<u16>(m.globalSequences.size());
    blob.globalSequences.insert(blob.globalSequences.end(), m.globalSequences.begin(), m.globalSequences.end());
    for (const formats::M2Sequence& s : m.sequences) {
        blob.sequences.push_back({s.start, s.end, s.animationId, static_cast<u16>(s.flags)});
    }
    for (const formats::M2Bone& b : m.bones) {
        if (b.flags & (formats::kM2BoneSphericalBillboard | formats::kM2BoneCylindricalZBillboard)) {
            VD_LOG_ERROR("ui models: %s: a billboarded bone (unsupported)", path);
            ++failures;
        }
        pack::UiModelBone bone{};
        std::memcpy(bone.pivot, b.pivot.data(), sizeof(bone.pivot));
        bone.parent = b.parent;
        bone.flags = static_cast<u16>(b.flags);
        bone.translation = blob.vec3(b.translation);
        bone.rotation = blob.quat(b.rotation);
        bone.scale = blob.vec3(b.scale);
        blob.bones.push_back(bone);
    }

    for (const formats::M2Batch& b : m.batches) {
        if (b.transparency >= 0 && alwaysZero(m.transparency[static_cast<usize>(b.transparency)])) {
            continue; // never shown
        }
        if (b.texture < 0) {
            VD_LOG_ERROR("ui models: %s: a batch without a texture", path);
            ++failures;
            continue;
        }
        const formats::M2Texture& texture = m.textures[static_cast<usize>(b.texture)];
        pack::UiModelBatch out{};
        out.blend = static_cast<u8>(m.materials[b.material].blend);
        out.wrap = static_cast<u8>(texture.flags & (pack::kUiTextureWrapU | pack::kUiTextureWrapV));
        out.color = out.alpha = out.weight = out.uvTranslation = out.uvRotation = pack::kUiModelNoTrack;
        if (b.color >= 0) {
            out.color = blob.vec3(m.colors[static_cast<usize>(b.color)].color);
            out.alpha = blob.fixed16(m.colors[static_cast<usize>(b.color)].alpha);
        }
        if (b.transparency >= 0) {
            out.weight = blob.fixed16(m.transparency[static_cast<usize>(b.transparency)]);
        }
        if (b.textureTransform >= 0) {
            const formats::M2TextureTransform& uv = m.textureTransforms[static_cast<usize>(b.textureTransform)];
            if (uv.scale.animated()) {
                VD_LOG_ERROR("ui models: %s: a scaled texture coordinate animation (unsupported)", path);
                ++failures;
            }
            if (texture.flags & pack::kUiTextureWrapU) {
                VD_LOG_ERROR("ui models: %s: an animated texture wrapping along u (unsupported)", path);
                ++failures;
            }
            out.uvTranslation = blob.vec3(uv.translation);
            out.uvRotation = blob.quat(uv.rotation);
            // Moved texture coordinates: the texture as one row (it must not change down its height).
            Result<std::vector<u8>> blp = mpq.read(texture.path);
            Result<formats::BlpImage> image =
                blp ? formats::decodeBlp(blp.value()) : Result<formats::BlpImage>(blp.error());
            if (!image || image.value().mips.empty()) {
                VD_LOG_ERROR("ui models: %s: %s unreadable", path, texture.path.c_str());
                ++failures;
                continue;
            }
            const formats::BlpImage& img = image.value();
            const std::vector<u8>& pixels = img.mips[0];
            const usize row = usize{img.width} * 4;
            for (u32 y = 1; y < img.height; ++y) {
                if (std::memcmp(pixels.data(), pixels.data() + y * row, row) != 0) {
                    VD_LOG_ERROR("ui models: %s: %s changes down its height (only 1-D animated textures)", path,
                                 texture.path.c_str());
                    ++failures;
                    break;
                }
            }
            out.firstTexel = static_cast<u32>(blob.texels.size());
            out.texelCount = img.width;
            for (u32 x = 0; x < img.width; ++x) {
                const u8* p = pixels.data() + x * 4;
                blob.texels.push_back(u32{p[0]} | (u32{p[1]} << 8) | (u32{p[2]} << 16) | (u32{p[3]} << 24));
            }
        } else {
            out.texture = assetPathHash(texture.path);
        }
        // The triangles, their vertices one by one (a few quads).
        out.firstVertex = static_cast<u32>(blob.vertices.size());
        const u32 first = b.firstIndex; // level 0: these models have a few dozen indices
        for (u32 i = first; i < first + b.indexCount; ++i) {
            const formats::M2Vertex& v = m.vertices[m.triangles[i]];
            pack::UiModelVertex out2{};
            std::memcpy(out2.position, v.position.data(), sizeof(out2.position));
            out2.uv[0] = v.uv[0];
            out2.uv[1] = v.uv[1];
            out2.bone = v.boneIndices[0];
            if (v.boneWeights[0] != 255) {
                VD_LOG_ERROR("ui models: %s: a vertex on several bones (unsupported)", path);
                ++failures;
            }
            blob.vertices.push_back(out2);
        }
        out.vertexCount = b.indexCount;
        blob.batches.push_back(out);
    }
    info.batchCount = static_cast<u16>(blob.batches.size() - info.firstBatch);

    for (const formats::M2ParticleEmitter& e : m.particles) {
        pack::UiModelEmitter out{};
        out.texture = e.texture < m.textures.size() ? assetPathHash(m.textures[e.texture].path) : 0;
        out.bone = e.bone;
        out.flags = e.flags;
        std::memcpy(out.position, e.position.data(), sizeof(out.position));
        out.blend = static_cast<u8>(e.blend);
        out.shape = static_cast<u8>(e.emitterType);
        out.rows = static_cast<u8>(std::max<u16>(e.rows, 1));
        out.columns = static_cast<u8>(std::max<u16>(e.columns, 1));
        out.speed = constant(e.speed, 0.0f, path, "speed", failures);
        out.speedVariation = constant(e.speedVariation, 0.0f, path, "speed variation", failures);
        out.verticalRange = constant(e.verticalRange, 0.0f, path, "vertical range", failures);
        out.horizontalRange = constant(e.horizontalRange, 0.0f, path, "horizontal range", failures);
        out.gravity = constant(e.gravity, 0.0f, path, "gravity", failures);
        out.lifespan = constant(e.lifespan, 1.0f, path, "lifespan", failures);
        out.rate = constant(e.emissionRate, 0.0f, path, "rate", failures);
        out.areaLength = constant(e.areaLength, 0.0f, path, "area length", failures);
        out.areaWidth = constant(e.areaWidth, 0.0f, path, "area width", failures);
        out.midPoint = e.midPoint;
        for (u32 i = 0; i < 3; ++i) {
            out.colors[i] = bgraToRgba(e.colors[i]);
            out.scales[i] = e.scales[i];
        }
        out.spin = e.spin;
        out.twinklePercent = e.twinklePercent;
        out.twinkleMin = e.twinkleMin;
        out.twinkleMax = e.twinkleMax;
        if (e.emitterType == formats::kM2EmitterSpline &&
            std::any_of(e.splinePoints.begin(), e.splinePoints.end(),
                        [](const formats::Vec3& p) { return p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f; })) {
            VD_LOG_ERROR("ui models: %s: a spline emitter with a shape (unsupported)", path);
            ++failures;
        }
        blob.emitters.push_back(out);
    }
    info.emitterCount = static_cast<u16>(blob.emitters.size() - info.firstEmitter);
    blob.models.push_back(info);
    VD_LOG_INFO("ui models: %s: %zu sequences, %zu bones, %u batches, %u emitters", path, m.sequences.size(),
                m.bones.size(), info.batchCount, info.emitterCount);
    return true;
}

template <typename T> void append(std::vector<u8>& out, pack::UiModelArray& array, const std::vector<T>& items) {
    out.resize((out.size() + 7) & ~usize{7}); // 8-aligned: the u64 paths read in place
    array.count = static_cast<u32>(items.size());
    array.offset = static_cast<u32>(out.size());
    const usize bytes = items.size() * sizeof(T);
    out.resize(out.size() + bytes);
    if (bytes > 0) {
        std::memcpy(out.data() + array.offset, items.data(), bytes);
    }
}

} // namespace

// The "ui/models" blob; `count` the models baked. Empty when one of them failed. The textures of their batches and
// emitters must be in the atlas (`sprites`).
std::vector<u8> bakeUiModels(const formats::MpqArchiveSet& mpq, std::span<const pack::UiSprite> sprites, usize& count) {
    Blob blob;
    int failures = 0;
    for (const char* path : kModels) {
        if (!bakeModel(mpq, path, blob, failures)) {
            ++failures;
        }
    }
    const auto inAtlas = [&](u64 hash) {
        return std::any_of(sprites.begin(), sprites.end(), [&](const pack::UiSprite& s) { return s.pathHash == hash; });
    };
    for (const pack::UiModelBatch& b : blob.batches) {
        if (b.texture != 0 && !inAtlas(b.texture)) {
            VD_LOG_ERROR("ui models: a batch's texture is not in the atlas (kUiTextures)");
            ++failures;
        }
    }
    for (const pack::UiModelEmitter& e : blob.emitters) {
        if (!inAtlas(e.texture)) {
            VD_LOG_ERROR("ui models: a particle texture is not in the atlas (kUiTextures)");
            ++failures;
        }
    }
    count = blob.models.size();
    if (failures > 0 || blob.models.size() != std::size(kModels)) {
        VD_LOG_ERROR("ui models: %d failed", failures);
        count = 0;
        return {};
    }
    std::vector<u8> out(sizeof(pack::UiModelsHeader));
    pack::UiModelsHeader header{};
    header.version = pack::kUiModelsVersion;
    append(out, header.models, blob.models);
    append(out, header.sequences, blob.sequences);
    append(out, header.bones, blob.bones);
    append(out, header.vertices, blob.vertices);
    append(out, header.batches, blob.batches);
    append(out, header.emitters, blob.emitters);
    append(out, header.tracks, blob.tracks);
    append(out, header.ranges, blob.ranges);
    append(out, header.keys, blob.keys);
    append(out, header.texels, blob.texels);
    append(out, header.globalSequences, blob.globalSequences);
    std::memcpy(out.data(), &header, sizeof(header));
    return out;
}

} // namespace vd::baker
