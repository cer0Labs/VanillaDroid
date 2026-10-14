#include "bake/GrassBaker.h"

#include "bake/BakeUtil.h"
#include "core/Hash.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace vd::bake {
namespace {

// A cell that can grow: not switched off, not in a hole, its layer has an effect.
bool cellGrows(const pack::GrassChunk& record, u32 column, u32 row) {
    if ((record.noEffect[row] >> column) & 1u) {
        return false;
    }
    if ((record.holes >> ((column >> 1) + 4 * (row >> 1))) & 1u) {
        return false;
    }
    return record.effects[(record.cellLayers[row] >> (column * 2)) & 3u] != pack::kNoGrassEffect;
}

} // namespace

GroundEffectTables readGroundEffectTables(const db::DbcView& groundEffectTexture,
                                          const db::DbcView& groundEffectDoodad) {
    GroundEffectTables tables;
    if (groundEffectTexture.fieldCount() > kGroundEffectDensityField) {
        for (u32 r = 0; r < groundEffectTexture.recordCount(); ++r) {
            GroundEffectRow row;
            row.id = groundEffectTexture.getU32(r, 0);
            for (u32 k = 0; k < 4; ++k) {
                row.doodads[k] = groundEffectTexture.getU32(r, kGroundEffectDoodadField + k);
            }
            row.density = groundEffectTexture.getU32(r, kGroundEffectDensityField);
            tables.effects[row.id] = row;
        }
    }
    if (groundEffectDoodad.fieldCount() > kGroundEffectDoodadNameField) {
        tables.doodadRows = groundEffectDoodad.recordCount();
        for (u32 r = 0; r < groundEffectDoodad.recordCount(); ++r) {
            tables.doodadNames[groundEffectDoodad.getU32(r, kGroundEffectDoodadTagField)] =
                std::string(groundEffectDoodad.getString(r, kGroundEffectDoodadNameField));
        }
    }
    return tables;
}

std::string groundEffectModelPath(std::string_view name) {
    std::string path = "World\\NoDXT\\Detail\\";
    path += name;
    const usize dot = path.find_last_of('.');
    if (dot != std::string::npos && dot > path.find_last_of('\\')) {
        path.resize(dot);
    }
    return path + ".m2";
}

Result<GrassModelGeometry> convertGrassModel(const formats::M2Model& model) {
    if (model.triangles.empty() || model.textures.empty()) {
        return Error{"no triangles or no texture"};
    }
    GrassModelGeometry out;
    out.texture = model.textures.front().path;
    if (out.texture.empty()) {
        return Error{"its texture is not a file"};
    }
    out.clamp = (model.textures.front().flags & 3u) == 0;
    // The vertices the triangles use, in first-use order.
    std::vector<u32> remap(model.vertices.size(), std::numeric_limits<u32>::max());
    out.height = 0.0f;
    for (const u32 index : model.triangles) {
        if (index >= model.vertices.size()) {
            return Error{"triangle index out of range"};
        }
        if (remap[index] == std::numeric_limits<u32>::max()) {
            remap[index] = static_cast<u32>(out.vertices.size());
            const formats::M2Vertex& v = model.vertices[index];
            out.vertices.push_back({{v.position[0], v.position[1], v.position[2]}, {v.uv[0], v.uv[1]}});
            out.height = std::max(out.height, v.position[2]);
        }
        if (remap[index] > 0xFFFF) {
            return Error{"too many vertices"};
        }
        out.indices.push_back(static_cast<u16>(remap[index]));
    }
    return out;
}

std::map<u32, u16> GrassTables::setModels(std::vector<std::pair<u32, GrassModelGeometry>> loaded) {
    std::sort(loaded.begin(), loaded.end(), [](const auto& a, const auto& b) {
        const u64 ta = assetPathHash(a.second.texture);
        const u64 tb = assetPathHash(b.second.texture);
        return ta != tb ? ta < tb : a.first < b.first;
    });
    std::map<u32, u16> modelOfTag;
    for (auto& [tag, geometry] : loaded) {
        pack::GrassModel model{};
        model.texture = assetPathHash(geometry.texture);
        model.firstIndex = static_cast<u32>(m_indices.size());
        model.indexCount = static_cast<u32>(geometry.indices.size());
        model.firstVertex = static_cast<u32>(m_vertices.size());
        model.vertexCount = static_cast<u32>(geometry.vertices.size());
        model.height = geometry.height;
        model.flags = geometry.clamp ? pack::kGrassModelClamp : 0;
        modelOfTag[tag] = static_cast<u16>(m_models.size());
        m_models.push_back(model);
        m_modelPaths.push_back(geometry.texture);
        m_vertices.insert(m_vertices.end(), geometry.vertices.begin(), geometry.vertices.end());
        m_indices.insert(m_indices.end(), geometry.indices.begin(), geometry.indices.end());
    }
    return modelOfTag;
}

u16 GrassTables::effectIndex(u32 effectId) const {
    const auto it = m_effectIndex.find(effectId & 0xFFFFu); // WoW.exe reads the field as 16 bits
    return it != m_effectIndex.end() ? it->second : pack::kNoGrassEffect;
}

std::vector<u8> GrassTables::modelsBlob() const {
    BlobWriter writer(sizeof(pack::GrassModelsHeader));
    pack::GrassModelsHeader header{};
    header.version = pack::kGrassFormatVersion;
    header.modelCount = static_cast<u32>(m_models.size());
    header.effectCount = static_cast<u32>(m_effects.size());
    header.vertexCount = static_cast<u32>(m_vertices.size());
    header.indexCount = static_cast<u32>(m_indices.size());
    header.modelsOffset = writer.append(m_models);
    header.effectsOffset = writer.append(m_effects);
    header.verticesOffset = writer.append(m_vertices);
    header.indicesOffset = writer.append(m_indices);
    return writer.finish(header);
}

pack::GrassChunk GrassTables::chunkRecord(const formats::AdtChunk& chunk, u16 shadowSlot) const {
    pack::GrassChunk record{};
    std::fill(std::begin(record.effects), std::end(record.effects), pack::kNoGrassEffect);
    bool broken = false;
    const u32 layers = std::min<u32>(chunk.layerCount, 4);
    for (u32 k = 0; k < layers; ++k) {
        record.effects[k] = effectIndex(chunk.layers[k].effectId);
        if (record.effects[k] != pack::kNoGrassEffect &&
            (m_effects[record.effects[k]].flags & pack::kGrassEffectBroken) != 0) {
            broken = true; // WoW.exe waits for every doodad of every layer: this chunk never gets any
        }
    }
    std::memcpy(record.cellLayers, chunk.cellLayers.data(), sizeof(record.cellLayers));
    std::memcpy(record.noEffect, chunk.noEffectDoodad.data(), sizeof(record.noEffect));
    record.holes = chunk.holes;
    record.shadow = shadowSlot;
    // Some cell must lead to an effect for the chunk to count as growing.
    bool grows = false;
    for (u32 cell = 0; cell < 64 && !grows && !broken; ++cell) {
        grows = cellGrows(record, cell % 8, cell / 8);
    }
    record.flags = grows ? pack::kGrassChunkGrows : 0;
    if (!grows) {
        record.shadow = pack::kNoGrassShadow;
    }
    return record;
}

std::vector<u8> GrassTables::tileBlob(const formats::Adt& adt, u32 tx, u32 ty, u32* growing) const {
    std::vector<pack::GrassChunk> chunks(pack::kTileChunks);
    std::vector<u8> shadows;
    u32 grows = 0;
    for (u32 i = 0; i < pack::kTileChunks && i < adt.chunks.size(); ++i) {
        const formats::AdtChunk& chunk = adt.chunks[i];
        chunks[i] = chunkRecord(chunk, chunk.hasShadow ? 0 : pack::kNoGrassShadow); // its slot is set below
        if ((chunks[i].flags & pack::kGrassChunkGrows) == 0) {
            continue;
        }
        ++grows;
        if (chunks[i].shadow == pack::kNoGrassShadow) {
            continue;
        }
        // Only the bits under cells that can grow are ever read: the others are cleared (they compress away), and a
        // chunk left without any keeps no shadow.
        std::array<u8, pack::kGrassShadowBytes> bits{};
        bool any = false;
        for (u32 row = 0; row < 8; ++row) {
            for (u32 column = 0; column < 8; ++column) {
                if (!cellGrows(chunks[i], column, row)) {
                    continue;
                }
                for (u32 texelRow = row * 8; texelRow < row * 8 + 8; ++texelRow) {
                    bits[texelRow * 8 + column] = chunk.shadowBits[texelRow * 8 + column];
                    any |= bits[texelRow * 8 + column] != 0;
                }
            }
        }
        if (!any) {
            chunks[i].shadow = pack::kNoGrassShadow;
            continue;
        }
        chunks[i].shadow = static_cast<u16>(shadows.size() / pack::kGrassShadowBytes);
        shadows.insert(shadows.end(), bits.begin(), bits.end());
    }
    for (u32 i = static_cast<u32>(adt.chunks.size()); i < pack::kTileChunks; ++i) {
        std::fill(std::begin(chunks[i].effects), std::end(chunks[i].effects), pack::kNoGrassEffect);
        chunks[i].shadow = pack::kNoGrassShadow;
    }
    if (growing) {
        *growing = grows;
    }
    if (grows == 0) {
        return {};
    }
    BlobWriter writer(sizeof(pack::GrassTileHeader));
    pack::GrassTileHeader header{};
    header.version = pack::kGrassFormatVersion;
    header.tileX = static_cast<u16>(tx);
    header.tileY = static_cast<u16>(ty);
    header.chunksOffset = writer.append(chunks);
    header.shadowCount = static_cast<u32>(shadows.size() / pack::kGrassShadowBytes);
    header.shadowsOffset = writer.append(shadows);
    return writer.finish(header);
}

} // namespace vd::bake
