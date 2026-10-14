#include "bake/SkyBaker.h"

#include "bake/BakeUtil.h"
#include "core/Hash.h"
#include "pack/SkyFormat.h"

#include <algorithm>
#include <cmath>

namespace vd::bake {
namespace {

// The root bone's rotation keys over its loop: a global sequence loops all its keys; otherwise the keys of
// the first sequence, from its start.
void bakeRotation(const formats::M2Model& model, std::vector<pack::SkyRotationKey>& keys, u32& loop) {
    loop = 0;
    if (model.bones.empty() || !model.bones[0].rotation.animated()) {
        return;
    }
    const formats::M2Track<formats::Quat>& track = model.bones[0].rotation;
    u32 start = 0;
    usize first = 0;
    usize last = std::min(track.times.size(), track.values.size());
    if (track.globalSequence >= 0 &&
        static_cast<usize>(track.globalSequence) < model.globalSequences.size()) {
        loop = model.globalSequences[static_cast<usize>(track.globalSequence)];
    } else if (!model.sequences.empty()) {
        start = model.sequences[0].start;
        loop = model.sequences[0].end - model.sequences[0].start;
        if (!track.ranges.empty()) {
            first = std::min<usize>(track.ranges[0][0], last);
            last = std::min<usize>(static_cast<usize>(track.ranges[0][1]) + 1, last);
        }
    }
    for (usize k = first; k < last; ++k) {
        if (track.times[k] < start || track.times[k] - start > loop) {
            continue;
        }
        pack::SkyRotationKey key{};
        key.time = track.times[k] - start;
        std::copy(track.values[k].begin(), track.values[k].end(), key.rotation);
        keys.push_back(key);
    }
    if (keys.size() < 2 || loop == 0) {
        keys.clear();
        loop = 0;
    }
}

} // namespace

Result<std::vector<u8>> bakeSkyModel(const formats::M2Model& model) {
    if (model.vertices.empty() || model.vertices.size() > 0xFFFF) {
        return Error{"sky model without vertices or with more than 65535"};
    }
    pack::SkyModelHeader header{};
    header.version = pack::kSkyModelFormatVersion;

    // The batches grouped by what they draw with, in the order of each group's first batch.
    struct Group {
        pack::SkyPart part;
        std::vector<u32> batches;
    };
    std::vector<Group> groups;
    for (u32 b = 0; b < model.batches.size(); ++b) {
        const formats::M2Batch& batch = model.batches[b];
        if (batch.indexCount == 0 || batch.firstIndex + batch.indexCount > model.triangles.size()) {
            continue;
        }
        const formats::M2Texture* texture =
            batch.texture >= 0 && static_cast<usize>(batch.texture) < model.textures.size()
                ? &model.textures[static_cast<usize>(batch.texture)]
                : nullptr;
        const formats::M2Material material =
            batch.material < model.materials.size() ? model.materials[batch.material] : formats::M2Material{};
        pack::SkyPart part{};
        part.texture = texture && texture->type == formats::kM2TextureFile && !texture->path.empty()
                           ? assetPathHash(texture->path)
                           : 0;
        part.blend = static_cast<u8>(toModelBlend(static_cast<u32>(material.blend)));
        part.flags = texture && (texture->flags & 3u) == 0 ? pack::kSkyPartClamp : 0;
        auto same = [&](const Group& g) {
            return g.part.texture == part.texture && g.part.blend == part.blend && g.part.flags == part.flags;
        };
        auto it = std::find_if(groups.begin(), groups.end(), same);
        if (it == groups.end()) {
            groups.push_back({part, {}});
            it = groups.end() - 1;
        }
        it->batches.push_back(b);
    }
    if (groups.empty()) {
        return Error{"sky model without batches"};
    }

    // Each batch gets its own copies of its vertices, carrying its opacity.
    std::vector<pack::SkyVertex> vertices;
    std::vector<u16> indices;
    std::vector<pack::SkyPart> parts;
    std::vector<u32> remap(model.vertices.size());
    f32 radius = 0.0f;
    for (Group& group : groups) {
        group.part.firstIndex = static_cast<u32>(indices.size());
        for (const u32 b : group.batches) {
            const formats::M2Batch& batch = model.batches[b];
            const u8 alpha = static_cast<u8>(std::lround(std::clamp(batch.alpha, 0.0f, 1.0f) * 255.0f));
            std::fill(remap.begin(), remap.end(), ~0u);
            for (u32 i = 0; i < batch.indexCount; ++i) {
                const u32 source = model.triangles[batch.firstIndex + i];
                if (remap[source] == ~0u) {
                    if (vertices.size() >= 0xFFFF) {
                        return Error{"sky model with more than 65535 vertices once split by batch"};
                    }
                    const formats::M2Vertex& v = model.vertices[source];
                    pack::SkyVertex out{};
                    std::copy(v.position.begin(), v.position.end(), out.position);
                    std::copy(v.uv.begin(), v.uv.end(), out.uv);
                    out.color[0] = out.color[1] = out.color[2] = 255;
                    out.color[3] = alpha;
                    radius = std::max(radius, std::sqrt(v.position[0] * v.position[0] +
                                                        v.position[1] * v.position[1] +
                                                        v.position[2] * v.position[2]));
                    remap[source] = static_cast<u32>(vertices.size());
                    vertices.push_back(out);
                }
                indices.push_back(static_cast<u16>(remap[source]));
            }
        }
        group.part.indexCount = static_cast<u32>(indices.size()) - group.part.firstIndex;
        parts.push_back(group.part);
    }

    std::vector<pack::SkyRotationKey> rotation;
    bakeRotation(model, rotation, header.rotationLoop);

    BlobWriter writer(sizeof(header));
    header.vertexCount = static_cast<u32>(vertices.size());
    header.verticesOffset = writer.append(vertices);
    header.indexCount = static_cast<u32>(indices.size());
    header.indicesOffset = writer.append(indices);
    header.partCount = static_cast<u32>(parts.size());
    header.partsOffset = writer.append(parts);
    header.rotationKeyCount = static_cast<u32>(rotation.size());
    header.rotationKeysOffset = writer.append(rotation);
    header.radius = radius;
    return writer.finish(header);
}

} // namespace vd::bake
