#include "bake/ModelBaker.h"

#include "bake/BakeUtil.h"
#include "bake/DoodadMotion.h"
#include "bake/EffectBaker.h"
#include "bake/Portable.h"
#include "bake/TrackWriter.h"
#include "core/Hash.h"
#include "pack/SkyFormat.h"
#include "pack/TerrainFormat.h"

#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vector_relational.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <tuple>

namespace vd::bake {
namespace {

using pack::ModelBlend;

ModelBlend toBlend(u32 blend) {
    return toModelBlend(blend);
}

u64 textureHash(const std::string& path) {
    return path.empty() ? 0 : assetPathHash(path);
}

// BGRA (client) -> RGBA bytes packed little-endian.
u32 bgraToRgba(u32 bgra) {
    const u32 b = bgra & 0xFF;
    const u32 g = (bgra >> 8) & 0xFF;
    const u32 r = (bgra >> 16) & 0xFF;
    const u32 a = (bgra >> 24) & 0xFF;
    return r | (g << 8) | (b << 16) | (a << 24);
}

struct MaterialKey {
    u64 texture;
    u8 blend;
    u16 flags;
    u32 color;
    u8 uvAnimation = 0;
    auto tie() const { return std::tie(texture, blend, flags, color, uvAnimation); }
    bool operator<(const MaterialKey& o) const { return tie() < o.tie(); }
};

// Material table deduplicated by value.
class Materials {
public:
    u16 add(const MaterialKey& key) {
        const auto [it, inserted] = m_index.emplace(key, static_cast<u16>(m_materials.size()));
        if (inserted) {
            m_materials.push_back({key.texture, key.blend, key.uvAnimation, key.flags, key.color});
        }
        return it->second;
    }
    const std::vector<pack::ModelMaterial>& list() const { return m_materials; }
    ModelBlend blendOf(u16 material) const { return static_cast<ModelBlend>(m_materials[material].blend); }

private:
    std::map<MaterialKey, u16> m_index;
    std::vector<pack::ModelMaterial> m_materials;
};

// Triangles of one group collected per material, then emitted as parts in blend-class order.
void emitParts(std::map<u16, std::vector<u16>>& byMaterial, const Materials& materials, u32 vertexOffset, u16 group,
               std::vector<u16>& indices, std::vector<pack::ModelPart>& parts) {
    std::vector<u16> order;
    for (const auto& [material, list] : byMaterial) {
        if (!list.empty()) {
            order.push_back(material);
        }
    }
    std::stable_sort(order.begin(), order.end(), [&](u16 a, u16 b) {
        return static_cast<u8>(materials.blendOf(a)) < static_cast<u8>(materials.blendOf(b));
    });
    for (u16 material : order) {
        const std::vector<u16>& list = byMaterial[material];
        parts.push_back({static_cast<u32>(indices.size()), static_cast<u32>(list.size()), vertexOffset, material, group});
        indices.insert(indices.end(), list.begin(), list.end());
    }
}

// 10 bits -> every third bit of a 30-bit Morton code.
u32 spreadBits(u32 v) {
    v &= 0x3FF;
    v = (v | (v << 16)) & 0x030000FF;
    v = (v | (v << 8)) & 0x0300F00F;
    v = (v | (v << 4)) & 0x030C30C3;
    v = (v | (v << 2)) & 0x09249249;
    return v;
}

// The effects of a doodad and the track table they and its skeleton use (cut to the sequence doodads loop).
void writeEffects(const formats::M2Model& model, i32 loop, const BakedEffects& effects, const TrackWriter& tracks,
                  bool skeleton, BlobWriter& writer, pack::ModelHeader& header) {
    header.effectsOffset = effects.write(writer);
    if (header.effectsOffset == 0 && !skeleton) {
        return;
    }
    header.trackCount = static_cast<u32>(tracks.m_tracks.size());
    header.tracksOffset = writer.append(tracks.m_tracks);
    header.rangeCount = static_cast<u32>(tracks.m_ranges.size());
    header.rangesOffset = writer.append(tracks.m_ranges);
    header.vec3KeyCount = static_cast<u32>(tracks.m_vec3Keys.size());
    header.vec3KeysOffset = writer.append(tracks.m_vec3Keys);
    header.quatKeyCount = static_cast<u32>(tracks.m_quatKeys.size());
    header.quatKeysOffset = writer.append(tracks.m_quatKeys);
    header.sequenceCount = loop >= 0 ? static_cast<u32>(doodadLoopVariations(model).size()) : 0;
    header.sequenceSlotCount = tracks.slotCount();
    if (loop >= 0) {
        const formats::M2Sequence& s = model.sequences[static_cast<usize>(loop)];
        header.sequenceDuration = s.end > s.start ? s.end - s.start : 0;
    }
    header.globalSequenceCount = static_cast<u32>(model.globalSequences.size());
    header.globalSequencesOffset = writer.append(model.globalSequences);
}

// Phase 14: the skeleton of a doodad that moves or turns to the camera (pack::ModelSkeletonHeader). Its bones' tracks go
// into `tracks` (before finish()); the section is written by write().
struct SkeletonSection {
    std::vector<pack::ActorBone> bones;
    std::vector<pack::ModelSkinVertex> skin;
    std::vector<pack::ModelLoopVariation> variations;
    pack::ModelSkeletonHeader header{};

    u32 write(BlobWriter& writer) {
        if (bones.empty()) {
            return 0;
        }
        header.boneCount = static_cast<u32>(bones.size());
        header.bonesOffset = writer.append(bones);
        header.skinOffset = writer.append(skin);
        header.variationCount = static_cast<u32>(variations.size());
        header.variationsOffset = writer.append(variations);
        return writer.append(std::vector<pack::ModelSkeletonHeader>{header});
    }
};

SkeletonSection doodadSkeleton(const formats::M2Model& model, const DoodadMotion& motion, bool withVertices,
                               TrackWriter& tracks) {
    SkeletonSection out;
    if (!motion.skinned() || model.bones.empty() || model.bones.size() > 256) {
        return out;
    }
    for (const formats::M2Bone& bone : model.bones) {
        pack::ActorBone b{};
        b.keyBone = bone.keyBoneId;
        // M2 0x8: spherical billboard; 0x40: cylindrical about Z (the only two in the 1.12 data, scan-doodad-anim).
        b.flags = (bone.flags & 0x8u) ? pack::kModelBoneBillboard : (bone.flags & 0x70u) ? pack::kModelBoneBillboardZ : 0u;
        b.parent = bone.parent;
        std::memcpy(b.pivot, bone.pivot.data(), sizeof(b.pivot));
        b.tracks[0] = tracks.add(bone.translation, formats::Vec3{0.0f, 0.0f, 0.0f});
        b.tracks[1] = tracks.add(bone.rotation, formats::Quat{0.0f, 0.0f, 0.0f, 1.0f});
        b.tracks[2] = tracks.add(bone.scale, formats::Vec3{1.0f, 1.0f, 1.0f});
        out.bones.push_back(b);
    }
    if (withVertices && motion.skinnedMesh()) { // a mesh that never moves stays static (its effects' bones aside)
        const std::vector<i32> billboard = doodadBillboardBones(model);
        out.skin.reserve(model.vertices.size());
        for (const formats::M2Vertex& v : model.vertices) {
            pack::ModelSkinVertex s{};
            u32 sum = 0, dominant = 0;
            for (u32 k = 0; k < 4; ++k) {
                const bool valid = v.boneIndices[k] < model.bones.size();
                s.bones[k] = valid ? v.boneIndices[k] : 0;
                s.weights[k] = valid ? v.boneWeights[k] : 0;
                sum += s.weights[k];
                dominant = s.weights[k] > s.weights[dominant] ? k : dominant;
            }
            if (sum == 0) { // unweighted: rigid on bone 0
                s = pack::ModelSkinVertex{{0, 0, 0, 0}, {255, 0, 0, 0}};
            }
            if (sum > 0 && billboard[s.bones[dominant]] >= 0) {
                // Turned to the camera about its billboard bone (all weights zero: see pack::ModelSkinVertex).
                s = pack::ModelSkinVertex{{static_cast<u8>(billboard[s.bones[dominant]]), 0, 0, 0}, {0, 0, 0, 0}};
            }
            out.skin.push_back(s);
        }
    }
    out.header.flags = (motion.animated() ? pack::kModelSkeletonAnimated : 0u) |
                       (motion.phased ? pack::kModelSkeletonPhased : 0u);
    out.header.loopMs = motion.loopMs;
    for (const u32 s : doodadLoopVariations(model)) {
        const formats::M2Sequence& sequence = model.sequences[s];
        out.variations.push_back({sequence.end > sequence.start ? sequence.end - sequence.start : 0,
                                  static_cast<u16>(std::max<i16>(sequence.frequency, 0)), 0});
    }
    return out;
}

// The highest opacity a batch's track ever gives (fixed16), 1 without a track or keys.
f32 maxOpacity(const formats::M2Track<i16>* track) {
    if (!track || track->values.empty()) {
        return 1.0f;
    }
    f32 highest = 0.0f;
    for (const i16 v : track->values) {
        highest = std::max(highest, std::clamp(static_cast<f32>(v) / 32767.0f, 0.0f, 1.0f));
    }
    return highest;
}

// A batch the original client never shows: its color's alpha times its transparency weight is 0 in every key.
// Orgrimmar's bonfire smoke and embers carry a 60-yard mesh of these around their emitters (drawn, it covered the
// Valley of Strength with flat untextured blocks). Batches that only fade for a while stay.
bool invisibleBatch(const formats::M2Model& model, const formats::M2Batch& batch) {
    const formats::M2Track<i16>* alpha = batch.color >= 0 && static_cast<usize>(batch.color) < model.colors.size()
                                             ? &model.colors[batch.color].alpha
                                             : nullptr;
    const formats::M2Track<i16>* weight =
        batch.transparency >= 0 && static_cast<usize>(batch.transparency) < model.transparency.size()
            ? &model.transparency[batch.transparency]
            : nullptr;
    return maxOpacity(alpha) * maxOpacity(weight) <= 0.002f;
}

// What the client multiplies a batch's texture by, RGBA8 (pack::kModelMaterialTinted): its color's RGB and its opacity
// (its color's alpha times its transparency weight) at their first key; a batch that only shows now and then (0 at
// first) keeps its brightest. White without either. The lantern's light beam in the Darkmoon Faire's Ticket Master
// tent is 5 % of a yellowish white, added: drawn white it was an opaque white wall.
u32 batchTint(const formats::M2Model& model, const formats::M2Batch& batch, f32& opacity) {
    const formats::M2Color* color =
        batch.color >= 0 && static_cast<usize>(batch.color) < model.colors.size() ? &model.colors[batch.color] : nullptr;
    const formats::M2Track<i16>* weight =
        batch.transparency >= 0 && static_cast<usize>(batch.transparency) < model.transparency.size()
            ? &model.transparency[batch.transparency]
            : nullptr;
    formats::Vec3 rgb{1.0f, 1.0f, 1.0f};
    if (color && !color->color.values.empty()) {
        rgb = color->color.values[0];
    }
    opacity = std::clamp(batch.alpha, 0.0f, 1.0f);
    if (opacity <= 0.002f) {
        opacity = maxOpacity(color ? &color->alpha : nullptr) * maxOpacity(weight);
    }
    const auto byte = [](f32 v) { return static_cast<u32>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return byte(rgb[0]) | (byte(rgb[1]) << 8) | (byte(rgb[2]) << 16) | (byte(opacity) << 24);
}

// Phase 11 (v6): a WMO vertex's color (pack::ModelVertex::color, see ModelFormat.h). The batches of a group come in
// three classes (MOGP's counts; their vertices never mix): interior ones are lit by the vertex colors with 1.12's
// overbright (MapObjOverbright: rgb * (1 + 4 * alpha)), transition ones (at doors) blend from that light to the
// outdoor one by the vertex alpha, exterior ones (and every batch of a group without vertex colors) take the outdoor
// light, day and night.
enum class BatchLight : u8 { Exterior, Interior, Transition };

u32 wmoVertexColor(u32 bgra, BatchLight light) {
    if (light == BatchLight::Exterior) {
        return 0xFF808080u; // all outdoor light (the color is unused)
    }
    const f32 alpha = static_cast<f32>(bgra >> 24) / 255.0f;
    const f32 boost = light == BatchLight::Interior ? 1.0f + 4.0f * alpha : 1.0f;
    u32 out = light == BatchLight::Interior ? 0u : (bgra & 0xFF000000u);
    for (u32 c = 0; c < 3; ++c) {
        const f32 value = static_cast<f32>((bgra >> (16 - 8 * c)) & 0xFF) * boost * 0.5f; // halved: the shader doubles
        out |= static_cast<u32>(std::min(value + 0.5f, 255.0f)) << (8 * c);
    }
    return out;
}

// --- Phase 11: lighting sections (pack::ModelLightingHeader) ---------------------------------------------------------

struct LightingSection {
    std::vector<pack::ModelLight> lights;
    std::vector<pack::ModelLightKey> keys;
    std::vector<u16> lightRefs;
    std::vector<pack::ModelFog> fogs;
    std::vector<pack::ModelGroupLighting> groups;
    std::vector<u32> cells;
    std::vector<u16> doodadGroups;

    // The sections, then their header; returns the header's offset (0 when there is nothing to write).
    u32 write(BlobWriter& writer) const {
        if (lights.empty() && fogs.empty() && groups.empty() && doodadGroups.empty()) {
            return 0;
        }
        pack::ModelLightingHeader h{};
        h.lightCount = static_cast<u32>(lights.size());
        h.lightsOffset = writer.append(lights);
        h.lightKeyCount = static_cast<u32>(keys.size());
        h.lightKeysOffset = writer.append(keys);
        h.lightRefCount = static_cast<u32>(lightRefs.size());
        h.lightRefsOffset = writer.append(lightRefs);
        h.fogCount = static_cast<u32>(fogs.size());
        h.fogsOffset = writer.append(fogs);
        h.groupCount = static_cast<u32>(groups.size());
        h.groupsOffset = writer.append(groups);
        h.cellCount = static_cast<u32>(cells.size());
        h.cellsOffset = writer.append(cells);
        h.doodadCount = static_cast<u32>(doodadGroups.size());
        h.doodadGroupsOffset = writer.append(doodadGroups);
        return writer.append(std::vector<pack::ModelLightingHeader>{h});
    }
};

// A group lit by its own vertex colors and not like the outdoors (the inside of a house, not a city street): what
// stands in it takes the interior's light.
bool trueInterior(u32 groupFlags) {
    return (groupFlags & formats::kWmoGroupInterior) != 0 && (groupFlags & formats::kWmoGroupExteriorLit) == 0;
}

u32 packRgba(const glm::vec3& rgb) {
    const glm::uvec3 c(glm::clamp(rgb, 0.0f, 1.0f) * 255.0f + 0.5f);
    return c.x | (c.y << 8) | (c.z << 16) | (255u << 24);
}

// The light baked into a true interior group's vertex colors, averaged over cells of ~kLightCellSize yards (bigger
// in big groups: at most kLightGridCells per side horizontally, half that vertically). Cells without vertices take
// their neighbors' light. 1.12 lights an actor inside by the vertex colors around it (the floor under its feet).
constexpr f32 kLightCellSize = 3.0f;
constexpr u32 kLightGridCells = 24;

void bakeLightGrid(const formats::WmoGroup& group, pack::ModelGroupLighting& out, std::vector<u32>& cells) {
    if (group.colors.empty() || group.positions.empty()) {
        return;
    }
    glm::vec3 min(FLT_MAX), max(-FLT_MAX);
    glm::dvec3 sum(0.0);
    for (usize v = 0; v < group.positions.size(); ++v) {
        const glm::vec3 p(group.positions[v][0], group.positions[v][1], group.positions[v][2]);
        min = glm::min(min, p);
        max = glm::max(max, p);
        const u32 bgra = group.colors[v];
        sum += glm::dvec3((bgra >> 16) & 0xFF, (bgra >> 8) & 0xFF, bgra & 0xFF);
    }
    out.average = packRgba(glm::vec3(sum / static_cast<f64>(group.positions.size())) / 255.0f);
    const glm::vec3 extent = glm::max(max - min, glm::vec3(0.01f));
    const f32 cell = std::max({kLightCellSize, extent.x / kLightGridCells, extent.y / kLightGridCells,
                               extent.z / (kLightGridCells / 2)});
    const glm::uvec3 size = glm::clamp(glm::uvec3(glm::ceil(extent / cell)), glm::uvec3(1u), glm::uvec3(255u));
    std::vector<glm::vec4> accumulated(static_cast<usize>(size.x) * size.y * size.z, glm::vec4(0.0f));
    auto cellOf = [&](const glm::vec3& p) {
        const glm::uvec3 c = glm::min(glm::uvec3(glm::max((p - min) / cell, glm::vec3(0.0f))), size - 1u);
        return (static_cast<usize>(c.z) * size.y + c.y) * size.x + c.x;
    };
    for (usize v = 0; v < group.positions.size(); ++v) {
        const u32 bgra = group.colors[v];
        accumulated[cellOf(glm::vec3(group.positions[v][0], group.positions[v][1], group.positions[v][2]))] +=
            glm::vec4((bgra >> 16) & 0xFF, (bgra >> 8) & 0xFF, bgra & 0xFF, 1.0f);
    }
    // Empty cells: the mean of their filled neighbors, wave after wave.
    for (bool changed = true; changed;) {
        changed = false;
        std::vector<glm::vec4> next = accumulated;
        for (u32 z = 0; z < size.z; ++z) {
            for (u32 y = 0; y < size.y; ++y) {
                for (u32 x = 0; x < size.x; ++x) {
                    const usize i = (static_cast<usize>(z) * size.y + y) * size.x + x;
                    if (accumulated[i].w > 0.0f) {
                        continue;
                    }
                    glm::vec4 around(0.0f);
                    const i32 offsets[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                    for (const auto& o : offsets) {
                        const glm::ivec3 n(static_cast<i32>(x) + o[0], static_cast<i32>(y) + o[1],
                                           static_cast<i32>(z) + o[2]);
                        if (glm::any(glm::lessThan(n, glm::ivec3(0))) || glm::any(glm::greaterThanEqual(n, glm::ivec3(size)))) {
                            continue;
                        }
                        const glm::vec4& c = accumulated[(static_cast<usize>(n.z) * size.y + n.y) * size.x + n.x];
                        if (c.w > 0.0f) {
                            around += glm::vec4(glm::vec3(c) / c.w, 1.0f);
                        }
                    }
                    if (around.w > 0.0f) {
                        next[i] = around;
                        changed = true;
                    }
                }
            }
        }
        accumulated = std::move(next);
    }
    out.firstCell = static_cast<u32>(cells.size());
    out.gridMin[0] = min.x;
    out.gridMin[1] = min.y;
    out.gridMin[2] = min.z;
    out.cellSize = cell;
    out.gridSize[0] = static_cast<u8>(size.x);
    out.gridSize[1] = static_cast<u8>(size.y);
    out.gridSize[2] = static_cast<u8>(size.z);
    for (const glm::vec4& c : accumulated) {
        cells.push_back(packRgba(glm::vec3(c) / std::max(c.w, 1.0f) / 255.0f));
    }
}

LightingSection wmoLighting(const formats::WmoRoot& root, const std::vector<formats::WmoGroup>& groups) {
    LightingSection out;
    for (const formats::WmoLight& l : root.lights) {
        pack::ModelLight light{};
        std::memcpy(light.position, l.position.data(), sizeof(light.position));
        light.color = bgraToRgba(l.color) | 0xFF000000u;
        light.intensity = l.intensity;
        light.attenStart = l.attenuated ? l.attenStart : 0.0f;
        light.attenEnd = l.attenuated ? std::max(l.attenEnd, l.attenStart + 0.01f) : 0.0f;
        light.type = static_cast<u8>(l.type);
        out.lights.push_back(light);
    }
    for (const formats::WmoFog& f : root.fogs) {
        pack::ModelFog fog{};
        std::memcpy(fog.position, f.position.data(), sizeof(fog.position));
        fog.innerRadius = f.innerRadius;
        fog.outerRadius = std::max(f.outerRadius, f.innerRadius);
        fog.end = f.end;
        fog.startScale = f.startScale;
        fog.color = bgraToRgba(f.color) | 0xFF000000u;
        fog.underwaterEnd = f.underwaterEnd;
        fog.underwaterStartScale = f.underwaterStartScale;
        fog.underwaterColor = bgraToRgba(f.underwaterColor) | 0xFF000000u;
        fog.flags = (f.flags & formats::kWmoFogInfinite) ? pack::kModelFogInfinite : 0u;
        out.fogs.push_back(fog);
    }
    out.doodadGroups.assign(root.doodads.size(), pack::kModelNoGroup);
    for (u32 g = 0; g < groups.size(); ++g) {
        const formats::WmoGroup& group = groups[g];
        pack::ModelGroupLighting lighting{};
        lighting.firstLightRef = static_cast<u32>(out.lightRefs.size());
        for (u16 ref : group.lightRefs) {
            if (ref < root.lights.size()) {
                out.lightRefs.push_back(ref);
            }
        }
        lighting.lightRefCount = static_cast<u16>(std::min<usize>(out.lightRefs.size() - lighting.firstLightRef, 0xFFFF));
        for (u32 i = 0; i < 4; ++i) {
            lighting.fogs[i] = group.fogIds[i] < root.fogs.size() ? group.fogIds[i] : 0;
        }
        if (trueInterior(group.flags)) {
            bakeLightGrid(group, lighting, out.cells);
        }
        out.groups.push_back(lighting);
        // A doodad listed by several groups (a doorway) belongs to the interior one.
        for (u16 d : group.doodadRefs) {
            if (d < out.doodadGroups.size() &&
                (out.doodadGroups[d] == pack::kModelNoGroup ||
                 (!trueInterior(groups[out.doodadGroups[d]].flags) && trueInterior(group.flags)))) {
                out.doodadGroups[d] = static_cast<u16>(g);
            }
        }
    }
    return out;
}

// An M2's own lights at the bind pose, their diffuse intensity keyed over the sequence doodads loop (`sequence`).
constexpr usize kMaxLightKeys = 32;

// --- Phase 18 (world): the groups' BSP trees (pack::ModelBspHeader) ---------------------------------------------------

struct BspSection {
    std::vector<pack::ModelBspGroup> groups;
    std::vector<pack::ModelBspNode> nodes;
    std::vector<u16> refs;
    std::vector<std::array<u16, 3>> triangles;
    std::vector<std::array<f32, 3>> vertices;
    std::vector<std::array<u32, 2>> doodadRanges;
    std::vector<u16> doodadGroups;

    u32 write(BlobWriter& writer) const {
        if (nodes.empty()) {
            return 0;
        }
        pack::ModelBspHeader h{};
        h.groupCount = static_cast<u32>(groups.size());
        h.groupsOffset = writer.append(groups);
        h.nodeCount = static_cast<u32>(nodes.size());
        h.nodesOffset = writer.append(nodes);
        h.refCount = static_cast<u32>(refs.size());
        h.refsOffset = writer.append(refs);
        h.triangleCount = static_cast<u32>(triangles.size());
        h.trianglesOffset = writer.append(triangles);
        h.vertexCount = static_cast<u32>(vertices.size());
        h.verticesOffset = writer.append(vertices);
        h.doodadCount = static_cast<u32>(doodadRanges.size());
        h.doodadRangesOffset = writer.append(doodadRanges);
        h.doodadGroupCount = static_cast<u32>(doodadGroups.size());
        h.doodadGroupsOffset = writer.append(doodadGroups);
        return writer.append(std::vector<pack::ModelBspHeader>{h});
    }
};

// What 1.12 walks to find the group under the camera (WoW.exe 0x6b92b0): the group's nodes as they are, and its leaves'
// faces without those its query skips (MOPY flag 0x04, the detail faces: 0x6b88e0 masks MOPY with the query's 0x04 |
// 0x80, 0x80 being its own visited mark) or can never meet (the vertical ones), renumbered into the faces some leaf
// keeps and their vertices. A group whose
// tree does not hold together keeps none (the runtime then knows nothing under it, as for a group without MOBN).
BspSection wmoBsp(const std::vector<formats::WmoGroup>& groups, usize doodadCount) {
    BspSection out;
    // Every group whose MODR lists a doodad, per doodad.
    std::vector<std::vector<u16>> listing(doodadCount);
    for (u32 g = 0; g < groups.size(); ++g) {
        for (const u16 ref : groups[g].doodadRefs) {
            if (ref < doodadCount && (listing[ref].empty() || listing[ref].back() != g)) {
                listing[ref].push_back(static_cast<u16>(g));
            }
        }
    }
    for (const std::vector<u16>& list : listing) {
        out.doodadRanges.push_back({static_cast<u32>(out.doodadGroups.size()), static_cast<u32>(list.size())});
        out.doodadGroups.insert(out.doodadGroups.end(), list.begin(), list.end());
    }
    for (const formats::WmoGroup& group : groups) {
        pack::ModelBspGroup entry{};
        entry.firstNode = static_cast<u32>(out.nodes.size());
        entry.firstRef = static_cast<u32>(out.refs.size());
        entry.firstTriangle = static_cast<u32>(out.triangles.size());
        entry.firstVertex = static_cast<u32>(out.vertices.size());
        const usize faces = group.indices.size() / 3;
        bool valid = !group.bspNodes.empty() && group.bspNodes.size() < pack::kModelBspNoChild;
        for (const formats::WmoBspNode& node : group.bspNodes) {
            const bool leaf = (node.flags & pack::kModelBspLeaf) != 0;
            valid = valid && (leaf ? u64{node.firstFace} + node.faceCount <= group.bspFaces.size()
                                   : (node.flags & pack::kModelBspAxisMask) < 3 &&
                                         (node.negChild == pack::kModelBspNoChild ||
                                          node.negChild < group.bspNodes.size()) &&
                                         (node.posChild == pack::kModelBspNoChild ||
                                          node.posChild < group.bspNodes.size()));
        }
        for (const u16 face : group.bspFaces) {
            valid = valid && face < faces;
        }
        if (!valid) {
            out.groups.push_back(entry);
            continue;
        }
        std::vector<u32> triangleOf(faces, ~0u);
        std::vector<u32> vertexOf(group.positions.size(), ~0u);
        for (const formats::WmoBspNode& node : group.bspNodes) {
            pack::ModelBspNode baked{node.flags, node.negChild, node.posChild, 0, 0, node.planeDist};
            if (node.flags & pack::kModelBspLeaf) {
                baked.firstFace = static_cast<u32>(out.refs.size()) - entry.firstRef;
                for (u32 i = node.firstFace; i < node.firstFace + node.faceCount; ++i) {
                    const u16 face = group.bspFaces[i];
                    if (face < group.triangleFlags.size() && (group.triangleFlags[face] & formats::kWmoTriangleNoCollision)) {
                        continue; // a detail face: the camera's query passes through it
                    }
                    {
                        // A vertical face (a wall): the vertical segment of the query never crosses it (the client's
                        // test finds nothing there either), so it is left out (a third of a city's faces).
                        const glm::vec3 a(group.positions[group.indices[usize{face} * 3]][0],
                                          group.positions[group.indices[usize{face} * 3]][1],
                                          group.positions[group.indices[usize{face} * 3]][2]);
                        const glm::vec3 b(group.positions[group.indices[usize{face} * 3 + 1]][0],
                                          group.positions[group.indices[usize{face} * 3 + 1]][1],
                                          group.positions[group.indices[usize{face} * 3 + 1]][2]);
                        const glm::vec3 c(group.positions[group.indices[usize{face} * 3 + 2]][0],
                                          group.positions[group.indices[usize{face} * 3 + 2]][1],
                                          group.positions[group.indices[usize{face} * 3 + 2]][2]);
                        const glm::vec3 normal = glm::cross(b - a, c - a);
                        if (std::abs(normal.z) <= 1e-4f * glm::length(normal)) {
                            continue;
                        }
                    }
                    if (triangleOf[face] == ~0u) {
                        triangleOf[face] = static_cast<u32>(out.triangles.size()) - entry.firstTriangle;
                        std::array<u16, 3> triangle{};
                        for (u32 corner = 0; corner < 3; ++corner) {
                            const u16 v = group.indices[usize{face} * 3 + corner];
                            if (vertexOf[v] == ~0u) {
                                vertexOf[v] = static_cast<u32>(out.vertices.size()) - entry.firstVertex;
                                out.vertices.push_back(group.positions[v]);
                            }
                            triangle[corner] = static_cast<u16>(vertexOf[v]);
                        }
                        out.triangles.push_back(triangle);
                    }
                    out.refs.push_back(static_cast<u16>(triangleOf[face]));
                }
                baked.faceCount = static_cast<u16>(out.refs.size() - entry.firstRef - baked.firstFace);
            }
            out.nodes.push_back(baked);
        }
        entry.nodeCount = static_cast<u32>(out.nodes.size()) - entry.firstNode;
        entry.refCount = static_cast<u32>(out.refs.size()) - entry.firstRef;
        entry.triangleCount = static_cast<u32>(out.triangles.size()) - entry.firstTriangle;
        entry.vertexCount = static_cast<u32>(out.vertices.size()) - entry.firstVertex;
        out.groups.push_back(entry);
    }
    return out;
}

LightingSection m2Lighting(const formats::M2Model& model, i32 sequence) {
    LightingSection out;
    const formats::M2Sequence* loop = sequence >= 0 ? &model.sequences[static_cast<usize>(sequence)] : nullptr;
    const u32 index = sequence >= 0 ? static_cast<u32>(sequence) : 0u;
    for (const formats::M2Light& l : model.lights) {
        pack::ModelLight light{};
        std::memcpy(light.position, l.position.data(), sizeof(light.position));
        const u32 start = loop ? loop->start : 0;
        const formats::Vec3 color =
            formats::sampleM2Track(l.diffuseColor, index, start, formats::Vec3{1.0f, 1.0f, 1.0f});
        light.color = packRgba(glm::vec3(color[0], color[1], color[2]));
        light.intensity = formats::sampleM2Track(l.diffuseIntensity, index, start, 1.0f);
        light.attenStart = formats::sampleM2Track(l.attenuationStart, index, start, 0.0f);
        light.attenEnd =
            std::max(formats::sampleM2Track(l.attenuationEnd, index, start, 0.0f), light.attenStart + 0.01f);
        light.type = l.type == formats::kM2LightDirectional ? pack::kModelLightDirectional : pack::kModelLightOmni;
        light.bone = l.bone >= 0 && l.bone < 0xFF && static_cast<usize>(l.bone) < model.bones.size()
                         ? static_cast<u8>(l.bone)
                         : u8{0xFF}; // phase 14
        if (!l.visibility.values.empty() && l.visibility.values[0] == 0) {
            continue; // switched off
        }
        // Flicker: the keys of the loop (a global sequence, or the first sequence's range of the timeline).
        const formats::M2Track<f32>& track = l.diffuseIntensity;
        if (!track.times.empty()) {
            u32 first = 0, last = static_cast<u32>(track.times.size()) - 1, offset = 0, length = 0;
            if (track.globalSequence >= 0 && static_cast<usize>(track.globalSequence) < model.globalSequences.size()) {
                length = model.globalSequences[static_cast<usize>(track.globalSequence)];
            } else if (loop && index < track.ranges.size()) {
                first = std::min(track.ranges[index][0], last);
                last = std::clamp(track.ranges[index][1], first, last);
                offset = loop->start;
                length = loop->end - loop->start;
            }
            light.firstKey = static_cast<u32>(out.keys.size());
            for (u32 k = first; k <= last && out.keys.size() - light.firstKey < kMaxLightKeys; ++k) {
                if (track.times[k] < offset || track.times[k] - offset > length) {
                    continue;
                }
                out.keys.push_back({track.times[k] - offset, track.values[k]});
            }
            light.keyCount = static_cast<u16>(out.keys.size() - light.firstKey);
            light.loopMs = light.keyCount > 1 ? length : 0;
            if (light.keyCount < 2) {
                out.keys.resize(light.firstKey); // constant: the intensity above is enough
                light.keyCount = 0;
                light.firstKey = 0;
            }
        }
        out.lights.push_back(light);
    }
    return out;
}

f32 radiusOf(const f32 (&min)[3], const f32 (&max)[3]) {
    f32 radius = 0.0f;
    for (u32 corner = 0; corner < 8; ++corner) {
        const glm::vec3 p((corner & 1) ? max[0] : min[0], (corner & 2) ? max[1] : min[1], (corner & 4) ? max[2] : min[2]);
        radius = std::max(radius, glm::length(p));
    }
    return radius;
}

} // namespace

Result<std::vector<u8>> bakeM2(const formats::M2Model& model, u32* invisibleBatches) {
    if (model.vertices.size() > 0xFFFF) {
        return Error{"M2 with more than 65535 vertices"};
    }
    std::vector<bool> shown(model.batches.size(), true);
    bool anyShown = false;
    for (usize i = 0; i < model.batches.size(); ++i) {
        shown[i] = !invisibleBatch(model, model.batches[i]);
        anyShown = anyShown || shown[i];
        if (!shown[i] && invisibleBatches) {
            ++*invisibleBatches;
        }
    }
    pack::ModelHeader header{};
    header.version = pack::kModelFormatVersion;
    header.kind = static_cast<u32>(pack::ModelKind::M2);

    std::vector<pack::ModelVertex> vertices;
    vertices.reserve(model.vertices.size());
    f32 min[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    f32 max[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const formats::M2Vertex& v : model.vertices) {
        if (!anyShown) {
            break; // nothing of its mesh is ever seen: baked as its effects alone
        }
        pack::ModelVertex out{};
        std::memcpy(out.position, v.position.data(), sizeof(out.position));
        for (u32 i = 0; i < 3; ++i) {
            out.normal[i] = toSnorm8(v.normal[i]);
            min[i] = std::min(min[i], v.position[i]);
            max[i] = std::max(max[i], v.position[i]);
        }
        std::memcpy(out.uv, v.uv.data(), sizeof(out.uv));
        std::memset(out.color, 255, sizeof(out.color));
        vertices.push_back(out);
    }
    // Effects (particles, ribbons, texture animations) and, phase 14, the skeleton of a doodad that moves or turns to
    // the camera, animated over the sequences doodads loop (Stand's variations; the effects use the first).
    const i32 loop = doodadLoopSequence(model);
    const std::vector<u32> loopIndex = doodadLoopVariations(model);
    std::vector<formats::M2Sequence> loopSequence;
    for (const u32 s : loopIndex) {
        loopSequence.push_back(model.sequences[s]);
    }
    TrackWriter tracks(loopSequence, loopIndex);
    const BakedEffects effects = bakeEffects(model, tracks, true);
    const DoodadMotion motion = analyzeDoodadMotion(model);
    SkeletonSection skeleton = doodadSkeleton(model, motion, !vertices.empty(), tracks);
    tracks.finish();
    header.fileRadius = model.boundsRadius;
    if (vertices.empty()) {
        // Emitter-only models (smoke, embers, fireflies), or whose meshes are all invisible: nothing but their effects.
        header.groupCount = 0;
        header.radius = effects.reach;
        BlobWriter empty(sizeof(header));
        writeEffects(model, loop, effects, tracks, !skeleton.bones.empty(), empty, header);
        header.lightingOffset = m2Lighting(model, loop).write(empty);
        header.skeletonOffset = skeleton.write(empty);
        return empty.finish(header);
    }

    Materials materials;
    std::map<u16, std::vector<u16>> byMaterial;
    for (usize b = 0; b < model.batches.size(); ++b) {
        const formats::M2Batch& batch = model.batches[b];
        if (!shown[b]) {
            continue;
        }
        const formats::M2Material& m = model.materials[batch.material];
        const formats::M2Texture* texture = batch.texture >= 0 ? &model.textures[batch.texture] : nullptr;
        MaterialKey key{};
        // Replaceable textures (type != 0: skins, capes...) have no file: the runtime uses a placeholder.
        key.texture = texture && texture->type == 0 ? textureHash(normalizeAssetPath(texture->path)) : 0;
        key.blend = static_cast<u8>(toBlend(static_cast<u32>(m.blend)));
        key.flags = static_cast<u16>(((m.flags & formats::kM2MaterialUnlit) ? pack::kModelMaterialUnlit : 0) |
                                     ((m.flags & formats::kM2MaterialUnfogged) ? pack::kModelMaterialUnfogged : 0) |
                                     ((m.flags & formats::kM2MaterialTwoSided) ? pack::kModelMaterialTwoSided : 0) |
                                     (texture && (texture->flags & 3u) == 0 ? pack::kModelMaterialClamp : 0));
        if (batch.textureTransform >= 0 && static_cast<usize>(batch.textureTransform) < effects.uvAnimations.size() &&
            batch.textureTransform < 255) {
            key.uvAnimation = static_cast<u8>(batch.textureTransform + 1);
        }
        f32 opacity = 1.0f;
        if (const u32 tint = batchTint(model, batch, opacity); tint != 0xFFFFFFFFu) {
            key.flags |= pack::kModelMaterialTinted;
            key.color = tint;
            const auto blend = static_cast<pack::ModelBlend>(key.blend);
            if (opacity < 0.999f && (blend == pack::ModelBlend::Opaque || blend == pack::ModelBlend::AlphaKey)) {
                key.blend = static_cast<u8>(pack::ModelBlend::Alpha); // a see-through batch of an opaque material
            }
        }
        std::vector<u16>& list = byMaterial[materials.add(key)];
        for (u32 i = 0; i < batch.indexCount; ++i) {
            list.push_back(static_cast<u16>(model.triangles[batch.firstIndex + i]));
        }
    }
    std::vector<u16> indices;
    std::vector<pack::ModelPart> parts;
    emitParts(byMaterial, materials, 0, 0, indices, parts);

    pack::ModelGroup group{};
    std::memcpy(group.boundsMin, min, sizeof(min));
    std::memcpy(group.boundsMax, max, sizeof(max));
    group.flags = pack::kModelGroupExterior;

    if (!skeleton.bones.empty()) { // phase 14: every pose of its loop
        std::memcpy(min, motion.boundsMin.data(), sizeof(min));
        std::memcpy(max, motion.boundsMax.data(), sizeof(max));
    }
    std::memcpy(header.boundsMin, min, sizeof(min));
    std::memcpy(header.boundsMax, max, sizeof(max));
    header.radius = radiusOf(min, max);
    BlobWriter writer(sizeof(header));
    header.vertexCount = static_cast<u32>(vertices.size());
    header.verticesOffset = writer.append(vertices);
    header.indexCount = static_cast<u32>(indices.size());
    header.indicesOffset = writer.append(indices);
    header.materialCount = static_cast<u32>(materials.list().size());
    header.materialsOffset = writer.append(materials.list());
    header.partCount = static_cast<u32>(parts.size());
    header.partsOffset = writer.append(parts);
    header.groupCount = 1;
    header.groupsOffset = writer.append(std::vector<pack::ModelGroup>{group});
    writeEffects(model, loop, effects, tracks, !skeleton.bones.empty(), writer, header);
    header.lightingOffset = m2Lighting(model, loop).write(writer);
    header.skeletonOffset = skeleton.write(writer);
    return writer.finish(header);
}

Result<std::vector<u8>> bakeWmo(const formats::WmoRoot& root, const std::vector<formats::WmoGroup>& groups) {
    if (groups.size() != root.groups.size()) {
        return Error{"WMO group count mismatch"};
    }
    pack::ModelHeader header{};
    header.version = pack::kModelFormatVersion;
    header.kind = static_cast<u32>(pack::ModelKind::Wmo);
    header.ambientColor = bgraToRgba(root.ambientColor);

    Materials materials;
    // Phase 11: per MOMT material, its key; the light of each batch (MOBA's classes) is added to it below.
    std::vector<MaterialKey> materialKeys(root.materials.size());
    for (usize i = 0; i < root.materials.size(); ++i) {
        const formats::WmoMaterial& m = root.materials[i];
        MaterialKey& key = materialKeys[i];
        key.texture = textureHash(normalizeAssetPath(m.texture1));
        key.blend = static_cast<u8>(toBlend(m.blendMode));
        key.flags = static_cast<u16>(
            ((m.flags & formats::kWmoMaterialUnlit) ? pack::kModelMaterialUnlit : 0) |
            ((m.flags & formats::kWmoMaterialUnfogged) ? pack::kModelMaterialUnfogged : 0) |
            ((m.flags & formats::kWmoMaterialTwoSided) ? pack::kModelMaterialTwoSided : 0) |
            ((m.flags & (formats::kWmoMaterialClampS | formats::kWmoMaterialClampT)) ? pack::kModelMaterialClamp : 0) |
            ((m.flags & formats::kWmoMaterialNightGlow) ? pack::kModelMaterialNightGlow : 0) |
            ((m.flags & formats::kWmoMaterialExteriorLight) ? pack::kModelMaterialExteriorLight : 0));
        key.color = (m.flags & formats::kWmoMaterialNightGlow) ? bgraToRgba(m.emissiveColor) : 0;
    }

    // Layout (see ModelFormat.h): groups in Morton order of their centers.
    std::vector<u32> layout(groups.size());
    std::iota(layout.begin(), layout.end(), 0u);
    {
        const glm::vec3 rootMin(root.boundsMin[0], root.boundsMin[1], root.boundsMin[2]);
        const glm::vec3 extent =
            glm::max(glm::vec3(root.boundsMax[0], root.boundsMax[1], root.boundsMax[2]) - rootMin, glm::vec3(1.0f));
        std::vector<u32> codes(groups.size());
        for (u32 g = 0; g < groups.size(); ++g) {
            const glm::vec3 center =
                (glm::vec3(groups[g].boundsMin[0], groups[g].boundsMin[1], groups[g].boundsMin[2]) +
                 glm::vec3(groups[g].boundsMax[0], groups[g].boundsMax[1], groups[g].boundsMax[2])) *
                0.5f;
            const glm::uvec3 q(glm::clamp((center - rootMin) / extent, 0.0f, 1.0f) * 1023.0f);
            codes[g] = spreadBits(q.x) | (spreadBits(q.y) << 1) | (spreadBits(q.z) << 2);
        }
        std::stable_sort(layout.begin(), layout.end(), [&](u32 a, u32 b) { return codes[a] < codes[b]; });
    }

    struct GroupPart {
        u8 blend;
        u16 material;
        u32 order; // position of the group in the layout
        u16 group;
        std::vector<u32> indices;
    };
    std::vector<pack::ModelVertex> vertices;
    std::vector<GroupPart> groupParts;
    for (u32 order = 0; order < layout.size(); ++order) {
        const u32 g = layout[order];
        const formats::WmoGroup& group = groups[g];
        const formats::WmoLiquidKind liquidKind = formats::wmoLiquidKind(root, group);
        const u32 base = static_cast<u32>(vertices.size());
        const bool colored = !group.colors.empty();
        std::vector<BatchLight> vertexLight(group.positions.size(), BatchLight::Exterior);
        for (usize b = 0; colored && b < group.batches.size(); ++b) {
            const formats::WmoBatch& batch = group.batches[b];
            const BatchLight light = b < group.transBatchCount ? BatchLight::Transition
                                     : b < u32{group.transBatchCount} + group.interiorBatchCount ? BatchLight::Interior
                                                                                                   : BatchLight::Exterior;
            for (u32 i = batch.firstIndex; i < batch.firstIndex + batch.indexCount; ++i) {
                vertexLight[group.indices[i]] = light;
            }
        }
        for (usize v = 0; v < group.positions.size(); ++v) {
            pack::ModelVertex out{};
            std::memcpy(out.position, group.positions[v].data(), sizeof(out.position));
            for (u32 i = 0; i < 3; ++i) {
                out.normal[i] = toSnorm8(group.normals[v][i]);
            }
            std::memcpy(out.uv, group.uvs[v].data(), sizeof(out.uv));
            const u32 rgba = colored ? wmoVertexColor(group.colors[v], vertexLight[v]) : 0xFF808080u;
            std::memcpy(out.color, &rgba, sizeof(out.color));
            vertices.push_back(out);
        }
        std::map<u16, std::vector<u32>> byMaterial;
        for (const formats::WmoBatch& batch : group.batches) {
            if (batch.material >= materialKeys.size()) {
                return Error{"WMO batch material out of range"};
            }
            std::vector<u32>& list = byMaterial[materials.add(materialKeys[batch.material])];
            for (u32 i = batch.firstIndex; i < batch.firstIndex + batch.indexCount; ++i) {
                list.push_back(base + group.indices[i]);
            }
        }
        if (liquidKind != formats::WmoLiquidKind::None) {
            // The liquid's own vertices follow the group's.
            const formats::WmoLiquid& liquid = group.liquid;
            const u8 kind = static_cast<u8>(liquidKind);
            const u32 liquidBase = static_cast<u32>(vertices.size());
            for (u32 y = 0; y < liquid.vertsY; ++y) {
                for (u32 x = 0; x < liquid.vertsX; ++x) {
                    pack::ModelVertex out{};
                    out.position[0] = liquid.corner[0] + static_cast<f32>(x) * formats::kWmoLiquidTileSize;
                    out.position[1] = liquid.corner[1] + static_cast<f32>(y) * formats::kWmoLiquidTileSize;
                    out.position[2] = liquid.heights[y * liquid.vertsX + x];
                    out.normal[2] = 127;
                    out.uv[0] = static_cast<f32>(x) * 0.5f; // like the terrain: repeat every 2 tiles
                    out.uv[1] = static_cast<f32>(y) * 0.5f;
                    out.color[0] = kind;
                    out.color[1] = 128; // WMOs store no depth: halfway between shallow and deep
                    out.color[3] = 255;
                    vertices.push_back(out);
                }
            }
            MaterialKey key{};
            key.blend = static_cast<u8>(liquidKind == formats::WmoLiquidKind::Magma ||
                                                liquidKind == formats::WmoLiquidKind::Slime
                                            ? ModelBlend::Opaque
                                            : ModelBlend::Alpha);
            key.flags = pack::kModelMaterialLiquid;
            key.color = kind;
            std::vector<u32>& list = byMaterial[materials.add(key)];
            for (u32 y = 0; y < liquid.tilesY; ++y) {
                for (u32 x = 0; x < liquid.tilesX; ++x) {
                    if ((liquid.tiles[y * liquid.tilesX + x] & 0x0F) == formats::kWmoLiquidTileEmpty) {
                        continue;
                    }
                    const u32 v00 = liquidBase + y * liquid.vertsX + x;
                    const u32 v10 = v00 + 1;
                    const u32 v01 = v00 + liquid.vertsX;
                    const u32 v11 = v01 + 1;
                    list.insert(list.end(), {v00, v01, v10, v10, v01, v11});
                }
            }
        }
        for (auto& [material, list] : byMaterial) {
            if (!list.empty()) {
                groupParts.push_back({static_cast<u8>(materials.blendOf(material)), material, order,
                                      static_cast<u16>(g), std::move(list)});
            }
        }
    }
    std::stable_sort(groupParts.begin(), groupParts.end(), [](const GroupPart& a, const GroupPart& b) {
        return std::tie(a.blend, a.material, a.order) < std::tie(b.blend, b.material, b.order);
    });
    std::vector<u32> indices;
    std::vector<pack::ModelPart> parts;
    for (const GroupPart& p : groupParts) {
        parts.push_back({static_cast<u32>(indices.size()), static_cast<u32>(p.indices.size()), 0, p.material, p.group});
        indices.insert(indices.end(), p.indices.begin(), p.indices.end());
    }

    std::vector<pack::ModelGroup> outGroups;
    for (const formats::WmoGroup& group : groups) {
        pack::ModelGroup out{};
        std::memcpy(out.boundsMin, group.boundsMin.data(), sizeof(out.boundsMin));
        std::memcpy(out.boundsMax, group.boundsMax.data(), sizeof(out.boundsMax));
        out.flags = ((group.flags & formats::kWmoGroupInterior) ? pack::kModelGroupInterior : 0) |
                    ((group.flags & formats::kWmoGroupExterior) ? pack::kModelGroupExterior : 0) |
                    ((group.flags & formats::kWmoGroupExteriorLit) ? pack::kModelGroupExteriorLit : 0) |
                    ((group.flags & formats::kWmoGroupUnreachable) ? pack::kModelGroupUnreachable : 0) |
                    ((group.flags & formats::kWmoGroupAlwaysDraw) ? pack::kModelGroupAlwaysDraw : 0);
        out.firstPortalRef = group.portalStart;
        out.portalRefCount = group.portalCount;
        outGroups.push_back(out);
    }

    std::vector<pack::ModelPortal> portals;
    std::vector<std::array<f32, 3>> portalVertices;
    for (const formats::WmoPortal& p : root.portals) {
        pack::ModelPortal out{};
        std::memcpy(out.plane, p.plane.data(), sizeof(out.plane));
        out.firstVertex = static_cast<u32>(portalVertices.size());
        out.vertexCount = static_cast<u32>(p.vertices.size());
        portalVertices.insert(portalVertices.end(), p.vertices.begin(), p.vertices.end());
        portals.push_back(out);
    }
    std::vector<pack::ModelPortalRef> portalRefs;
    for (const formats::WmoPortalRef& r : root.portalRefs) {
        portalRefs.push_back({r.portal, r.group, r.side, 0});
    }
    std::vector<pack::ModelDoodadSet> sets;
    for (const formats::WmoDoodadSet& s : root.doodadSets) {
        pack::ModelDoodadSet out{};
        std::memcpy(out.name, s.name.data(), std::min<usize>(s.name.size(), sizeof(out.name) - 1));
        out.first = s.first;
        out.count = s.count;
        sets.push_back(out);
    }
    // Doodads only groups the client never draws refer to are never drawn either (the client draws a group's doodads
    // with the group): baked without a model.
    std::vector<u8> doodadShown(root.doodads.size(), 0);
    for (const formats::WmoGroup& group : groups) {
        for (const u16 ref : group.doodadRefs) {
            if (ref < doodadShown.size()) {
                doodadShown[ref] |= formats::wmoGroupNeverDrawn(group.flags) ? 1 : 2;
            }
        }
    }
    std::vector<pack::ModelDoodad> doodads;
    for (usize i = 0; i < root.doodads.size(); ++i) {
        const formats::WmoDoodad& d = root.doodads[i];
        pack::ModelDoodad out{};
        out.model = d.model.empty() || doodadShown[i] == 1
                        ? 0
                        : assetPathHash(normalizeAssetPath(formats::m2Path(d.model)));
        std::memcpy(out.position, d.position.data(), sizeof(out.position));
        std::memcpy(out.rotation, d.rotation.data(), sizeof(out.rotation));
        out.scale = d.scale;
        out.color = bgraToRgba(d.color);
        doodads.push_back(out);
    }

    f32 min[3], max[3];
    std::memcpy(min, root.boundsMin.data(), sizeof(min));
    std::memcpy(max, root.boundsMax.data(), sizeof(max));
    std::memcpy(header.boundsMin, min, sizeof(min));
    std::memcpy(header.boundsMax, max, sizeof(max));
    header.radius = radiusOf(min, max);
    BlobWriter writer(sizeof(header));
    header.vertexCount = static_cast<u32>(vertices.size());
    header.verticesOffset = writer.append(vertices);
    header.indexCount = static_cast<u32>(indices.size());
    if (vertices.size() > 0x10000) {
        header.flags |= pack::kModelIndices32;
        header.indicesOffset = writer.append(indices);
    } else {
        std::vector<u16> narrow(indices.size());
        for (usize i = 0; i < indices.size(); ++i) {
            narrow[i] = static_cast<u16>(indices[i]);
        }
        header.indicesOffset = writer.append(narrow);
    }
    header.materialCount = static_cast<u32>(materials.list().size());
    header.materialsOffset = writer.append(materials.list());
    header.partCount = static_cast<u32>(parts.size());
    header.partsOffset = writer.append(parts);
    header.groupCount = static_cast<u32>(outGroups.size());
    header.groupsOffset = writer.append(outGroups);
    header.portalCount = static_cast<u32>(portals.size());
    header.portalsOffset = writer.append(portals);
    header.portalVertexCount = static_cast<u32>(portalVertices.size());
    header.portalVerticesOffset = writer.append(portalVertices);
    header.portalRefCount = static_cast<u32>(portalRefs.size());
    header.portalRefsOffset = writer.append(portalRefs);
    header.doodadSetCount = static_cast<u32>(sets.size());
    header.doodadSetsOffset = writer.append(sets);
    header.doodadCount = static_cast<u32>(doodads.size());
    header.doodadsOffset = writer.append(doodads);
    header.lightingOffset = wmoLighting(root, groups).write(writer);
    header.bspOffset = wmoBsp(groups, root.doodads.size()).write(writer); // phase 18 (world)
    // Phase 11: the skybox its MOSB names, shown while the camera is inside it (pack/SkyFormat.h).
    if (const std::string skybox = pack::skyModelPath(root.skybox); !skybox.empty()) {
        const u64 hash = assetPathHash(skybox);
        header.reserved2[0] = static_cast<u32>(hash);
        header.reserved2[1] = static_cast<u32>(hash >> 32);
    }
    return writer.finish(header);
}

std::array<f32, 3> placementToWorld(const std::array<f32, 3>& p) {
    return {pack::kMapHalfSize - p[2], pack::kMapHalfSize - p[0], p[1]};
}

std::array<f32, 12> placementTransform(const std::array<f32, 3>& position, const std::array<f32, 3>& rotationDegrees,
                                       f32 scale) {
    // In the placement axes (y' up) the client builds T(pos) * Ry(ry - 90) * Rz(-rx) * Rx(rz) * S, applied to
    // model vertices converted with (x, y, z) -> (x, z, -y). World axes: X = h - z', Y = h - x', Z = y'.
    glm::mat4 m(1.0f);
    // Phase 22: glm::rotate with the sines and cosines every platform gets alike (bake/Portable.h).
    m = portableRotate(m, glm::radians(rotationDegrees[1] - 90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    m = portableRotate(m, glm::radians(-rotationDegrees[0]), glm::vec3(0.0f, 0.0f, 1.0f));
    m = portableRotate(m, glm::radians(rotationDegrees[2]), glm::vec3(1.0f, 0.0f, 0.0f));
    m = glm::scale(m, glm::vec3(scale));
    glm::mat4 fromModel(0.0f); // (x, y, z) -> (x, z, -y)
    fromModel[0][0] = 1.0f;
    fromModel[1][2] = -1.0f;
    fromModel[2][1] = 1.0f;
    fromModel[3][3] = 1.0f;
    glm::mat4 toWorld(0.0f); // (x', y', z') -> (-z', -x', y')
    toWorld[0][1] = -1.0f;
    toWorld[1][2] = 1.0f;
    toWorld[2][0] = -1.0f;
    toWorld[3][3] = 1.0f;
    const glm::mat4 linear = toWorld * m * fromModel;
    const std::array<f32, 3> t = placementToWorld(position);
    std::array<f32, 12> out{};
    for (u32 row = 0; row < 3; ++row) {
        for (u32 column = 0; column < 3; ++column) {
            out[row * 4 + column] = linear[column][row];
        }
        out[row * 4 + 3] = t[row];
    }
    return out;
}

std::array<f32, 12> placementTransform(const f32 (&position)[3], const f32 (&rotationDegrees)[3], f32 scale) {
    return placementTransform(std::array<f32, 3>{position[0], position[1], position[2]},
                              std::array<f32, 3>{rotationDegrees[0], rotationDegrees[1], rotationDegrees[2]}, scale);
}

} // namespace vd::bake
