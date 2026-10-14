#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "db/DbcView.h"
#include "formats/Adt.h"
#include "formats/M2.h"
#include "pack/GrassFormat.h"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace vd::bake {

// Phase 14 (grass): the 1.12 ground effects (the terrain's detail doodads) into grass.vdpk (pack/GrassFormat.h).
// Bump when the baked blobs change for the same input (the bake cache keys on it).
constexpr u32 kGrassBakeVersion = 1;

// GroundEffectTexture.dbc (1.12.1, 7 columns, checked against the client in tests): 0 ID, 1..4 DoodadId (the second
// column of GroundEffectDoodad, its "tag", not its ID: WoW.exe indexes its doodad table by it; -1 = none),
// 5 Density (0 = 8), 6 TerrainType (the footsteps).
// GroundEffectDoodad.dbc (3 columns): 0 ID, 1 the tag, 2 the model's name under World\NoDXT\Detail\ (".mdl"/".mdx",
// shipped as .m2).
constexpr u32 kGroundEffectDoodadField = 1;
constexpr u32 kGroundEffectDensityField = 5;
constexpr u32 kGroundEffectDoodadTagField = 1;
constexpr u32 kGroundEffectDoodadNameField = 2;
constexpr u32 kNoGroundEffectDoodad = 0xFFFFFFFFu;

struct GroundEffectRow {
    u32 id = 0;
    std::array<u32, 4> doodads{}; // tags, kNoGroundEffectDoodad = none
    u32 density = 0;              // as in the DBC (0 = 8)
};

struct GroundEffectTables {
    std::map<u32, GroundEffectRow> effects; // by ID
    std::map<u32, std::string> doodadNames; // by tag: the DBC's name ("ElwGra01.mdl")
    u32 doodadRows = 0; // WoW.exe's doodad table has this many slots: tags at or past it read outside it
};

GroundEffectTables readGroundEffectTables(const db::DbcView& groundEffectTexture,
                                          const db::DbcView& groundEffectDoodad);

// "ElwGra01.mdl" -> "World\NoDXT\Detail\ElwGra01.m2" (the client's CDetailDoodadData path).
std::string groundEffectModelPath(std::string_view name);

// A detail doodad M2 as the client draws it: its view-0 triangles over the vertices they use (bind pose, the models
// have one static bone), with its first texture.
struct GrassModelGeometry {
    std::vector<pack::GrassVertex> vertices;
    std::vector<u16> indices;
    std::string texture; // client path
    bool clamp = false;  // the texture does not wrap
    f32 height = 0.0f;   // top of the vertices
};
Result<GrassModelGeometry> convertGrassModel(const formats::M2Model& model);

// The ground effect table and the models it uses, ready to index from the tiles: every GroundEffectTexture row with
// at least one doodad, in ID order, and every model they name, sorted by texture. Built once, then shared (read only)
// by the tile bakes.
class GrassTables {
public:
    // `loadModel(tag)`: the converted model of a doodad tag, or an error when the client could not load it either
    // (no such tag, no file, unreadable): effects naming it are marked broken.
    template <typename LoadModel>
    void build(const GroundEffectTables& tables, LoadModel&& loadModel);

    // The effect index of an MCLY effectId (WoW.exe reads its low 16 bits), kNoGrassEffect when the row has no
    // doodads or does not exist.
    u16 effectIndex(u32 effectId) const;
    const std::vector<pack::GrassEffect>& effects() const { return m_effects; }
    const std::vector<pack::GrassModel>& models() const { return m_models; }
    const std::vector<std::string>& modelPaths() const { return m_modelPaths; }
    u32 brokenEffects() const { return m_broken; }
    u32 missingModels() const { return m_missingModels; }

    // grass/models: GrassModelsHeader, models, effects, vertices, indices.
    std::vector<u8> modelsBlob() const;

    // A tile's GrassChunk records and shadows; empty when nothing grows on it. `grows`: chunks that grow grass.
    std::vector<u8> tileBlob(const formats::Adt& adt, u32 tx, u32 ty, u32* grows = nullptr) const;
    // One chunk's record (exposed for tests and the census). `shadowSlot`: its index in the tile's shadows if it
    // carries one.
    pack::GrassChunk chunkRecord(const formats::AdtChunk& chunk, u16 shadowSlot) const;

private:
    // Sorts the models by texture (then tag), packs their geometry; returns each tag's model index.
    std::map<u32, u16> setModels(std::vector<std::pair<u32, GrassModelGeometry>> loaded);

    std::vector<pack::GrassEffect> m_effects;
    std::map<u32, u16> m_effectIndex; // GroundEffectTexture ID -> index
    std::vector<pack::GrassModel> m_models;
    std::vector<std::string> m_modelPaths;
    std::vector<pack::GrassVertex> m_vertices;
    std::vector<u16> m_indices;
    u32 m_broken = 0;
    u32 m_missingModels = 0;
};

template <typename LoadModel>
void GrassTables::build(const GroundEffectTables& tables, LoadModel&& loadModel) {
    // Every tag the rows name, loaded once.
    std::map<u32, bool> tags; // tag -> loaded
    std::vector<std::pair<u32, GrassModelGeometry>> loaded;
    for (const auto& [id, row] : tables.effects) {
        for (const u32 tag : row.doodads) {
            if (tag == kNoGroundEffectDoodad || tags.count(tag)) {
                continue;
            }
            Result<GrassModelGeometry> model = tag < tables.doodadRows && tables.doodadNames.count(tag)
                                                   ? loadModel(tag)
                                                   : Result<GrassModelGeometry>(Error{"no such doodad"});
            tags[tag] = model.ok();
            if (model) {
                loaded.emplace_back(tag, std::move(model.value()));
            } else {
                ++m_missingModels;
            }
        }
    }
    // The effects, with their tags turned into model indices (in the sorted order setModels() gave them).
    const std::map<u32, u16> modelOfTag = setModels(std::move(loaded));
    for (const auto& [id, row] : tables.effects) {
        bool any = false;
        pack::GrassEffect effect{};
        effect.id = id;
        effect.density = static_cast<u16>(row.density == 0 ? 8 : std::min<u32>(row.density, 0xFFFF));
        for (u32 k = 0; k < 4; ++k) {
            const u32 tag = row.doodads[k];
            effect.models[k] = pack::kNoGrassModel;
            if (tag == kNoGroundEffectDoodad) {
                continue;
            }
            any = true;
            if (const auto it = modelOfTag.find(tag); it != modelOfTag.end()) {
                effect.models[k] = it->second;
            } else {
                effect.flags |= pack::kGrassEffectBroken;
            }
        }
        if (!any) {
            continue; // a row without doodads grows nothing (most of the table: terrain types for the footsteps)
        }
        m_broken += (effect.flags & pack::kGrassEffectBroken) != 0 ? 1 : 0;
        m_effectIndex[id] = static_cast<u16>(m_effects.size());
        m_effects.push_back(effect);
    }
}

} // namespace vd::bake
