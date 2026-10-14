#include "game/BodyData.h"

#include "core/Hash.h"
#include "core/Log.h"
#include "fx/EffectSet.h"
#include "game/AreaData.h"
#include "pack/PackReader.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

namespace vd::game {
namespace {

// CreatureModelData.dbc (5875, 16 fields; vMaNGOS DBCStructure.h CreatureModelDataEntry): 0 id, 1 flags, 2
// model, 3 size class, 4 scale, 5 blood (UnitBloodLevels), 6 footprint texture (FootprintTextures), 7
// footprint length, 8 width (inches), 9 footprint particle scale (WoW.exe keeps it, never reads it), 10 foley
// material, 11 footstep shake size, 12 death thud shake size, 13 sound data, 14 collision width, 15 collision
// height.
namespace modeldata {
constexpr u32 kFlags = 1, kPath = 2, kBlood = 5, kFootprint = 6, kFootprintLength = 7, kFootprintWidth = 8;
constexpr u32 kCollisionWidth = 14, kCollisionHeight = 15, kFields = 16;
} // namespace modeldata
// FootprintTextures.dbc: 0 id, 1 texture path (no extension).
constexpr u32 kFootprintPath = 1;
// UnitBloodLevels.dbc: 0 id, 1-3 the UnitBlood id at violence levels 0, 1, 2.
// UnitBlood.dbc (10 fields): 0 id, 1-2 spurt in front small / large, 3-4 from behind small / large
// (SpellVisualEffectName ids), 5-9 ground blood textures (WoW.exe 1.12.1 never reads them: 0x60A260, the
// bleeding of a unit under 20 % health, only rolls its timer).
constexpr u32 kBloodFields = 5;
// TerrainType.dbc: 0 id, 1 name, 2 footstep spray when running, 3 when walking (SpellVisualEffectName), 4
// SoundID, 5 flags (1: footprints).
namespace terrain {
constexpr u32 kRunSpray = 2, kWalkSpray = 3, kSound = 4, kFlags = 5, kFields = 6;
constexpr u32 kFootprints = 0x1;
} // namespace terrain
// SpellVisualEffectName.dbc: 0 id, 1 name, 2 model path, 3 area effect size, 4 scale.
constexpr u32 kEffectName = 1, kEffectPath = 2, kEffectScale = 4, kEffectFields = 5;
constexpr std::string_view kHardcodedNames[static_cast<u32>(HardcodedEffect::Count)] = {
    "HARDCODED Footstep Water Run Spray",
    "HARDCODED Footstep Water Walk Spray",
    "HARDCODED Breath Underwater",
    "HARDCODED Breath Cold",
    "HARDCODED Mount Poof",
    "HARDCODED Inebriated Bubbles",
    "HARDCODED Meeting Stone Join", // phase 16 (alerts)
};
// AreaTable.dbc flags.
constexpr u32 kAreaSnow = 0x1;          // AREA_FLAG_SNOW: breath condenses
constexpr u32 kAreaOverridesZone = 0x2; // the area's own snow flag rules, not its zone's

bool load(const pack::PackReader& db, const char* name, u32 minFields, db::DbcView& out) {
    const std::optional<pack::PackAsset> asset = db.find(std::string("DBFilesClient\\") + name + ".dbc");
    if (!asset) {
        VD_LOG_WARN("%s.dbc missing", name);
        return false;
    }
    Result<db::DbcView> parsed = db::DbcView::parse(asset->data, asset->size);
    if (!parsed || parsed.value().fieldCount() < minFields) {
        VD_LOG_WARN("%s.dbc: unexpected layout", name);
        return false;
    }
    out = parsed.value();
    return true;
}

} // namespace

bool BodyData::init(const pack::PackReader& db) {
    m_ready = false;
    db::DbcView effects, models, footprints, bloodLevels, bloods, terrains;
    if (!load(db, "SpellVisualEffectName", kEffectFields, effects) ||
        !load(db, "CreatureModelData", modeldata::kFields, models) ||
        !load(db, "FootprintTextures", 2, footprints) || !load(db, "UnitBloodLevels", 4, bloodLevels) ||
        !load(db, "UnitBlood", kBloodFields, bloods) ||
        !load(db, "TerrainType", terrain::kFields, terrains)) {
        return false;
    }
    m_effects.clear();
    for (u32 r = 0; r < effects.recordCount(); ++r) {
        const std::string_view path = effects.getString(r, kEffectPath);
        const f32 scale = effects.getF32(r, kEffectScale);
        const BodyEffect effect{path.empty() ? 0 : fx::effectModelHash(path), scale > 0.0f ? scale : 1.0f};
        m_effects[effects.getU32(r, 0)] = effect;
        const std::string_view name = effects.getString(r, kEffectName);
        for (u32 h = 0; h < std::size(kHardcodedNames); ++h) {
            if (name == kHardcodedNames[h]) {
                m_hardcoded[h] = effect;
            }
        }
    }
    std::unordered_map<u32, u64> footprintTextures;
    for (u32 r = 0; r < footprints.recordCount(); ++r) {
        const std::string_view path = footprints.getString(r, kFootprintPath);
        if (!path.empty()) {
            footprintTextures[footprints.getU32(r, 0)] = assetPathHash(std::string(path) + ".blp");
        }
    }
    m_models.clear();
    for (u32 r = 0; r < models.recordCount(); ++r) {
        const std::string_view path = models.getString(r, modeldata::kPath);
        if (path.size() <= 4) {
            continue;
        }
        BodyModel m;
        m.flags = models.getU32(r, modeldata::kFlags);
        const i32 blood = models.getI32(r, modeldata::kBlood);
        m.bloodLevel = blood > 0 ? static_cast<u32>(blood) : 0;
        if (const auto texture = footprintTextures.find(models.getU32(r, modeldata::kFootprint));
            texture != footprintTextures.end()) {
            m.footprint = texture->second;
        }
        m.footprintLength = models.getF32(r, modeldata::kFootprintLength) / 36.0f;
        m.footprintWidth = models.getF32(r, modeldata::kFootprintWidth) / 36.0f;
        m.collisionWidth = models.getF32(r, modeldata::kCollisionWidth);
        m.collisionHeight = models.getF32(r, modeldata::kCollisionHeight);
        m_models.emplace(fx::effectModelHash(path), m); // a model several rows use: the first (they agree)
    }
    m_bloodLevels.clear();
    for (u32 r = 0; r < bloodLevels.recordCount(); ++r) {
        m_bloodLevels[bloodLevels.getU32(r, 0)] = {bloodLevels.getU32(r, 1), bloodLevels.getU32(r, 2),
                                                   bloodLevels.getU32(r, 3)};
    }
    m_bloods.clear();
    for (u32 r = 0; r < bloods.recordCount(); ++r) {
        m_bloods[bloods.getU32(r, 0)] = {bloods.getU32(r, 1), bloods.getU32(r, 2), bloods.getU32(r, 3),
                                         bloods.getU32(r, 4)};
    }
    m_runSpray.fill({});
    m_walkSpray.fill({});
    m_footprints.fill(false);
    for (u32 r = 0; r < terrains.recordCount(); ++r) {
        const u32 sound = terrains.getU32(r, terrain::kSound);
        if (sound >= kTerrainSounds) {
            continue;
        }
        // Grass and DustyGrass share a sound: what either has (neither has a spray or prints).
        if (const BodyEffect run = effect(terrains.getU32(r, terrain::kRunSpray)); run.model != 0) {
            m_runSpray[sound] = run;
        }
        if (const BodyEffect walk = effect(terrains.getU32(r, terrain::kWalkSpray)); walk.model != 0) {
            m_walkSpray[sound] = walk;
        }
        m_footprints[sound] =
            m_footprints[sound] || (terrains.getU32(r, terrain::kFlags) & terrain::kFootprints) != 0;
    }
    m_ready = true;
    return true;
}

BodyEffect BodyData::effect(u32 id) const {
    const auto it = id != 0 ? m_effects.find(id) : m_effects.end();
    return it != m_effects.end() ? it->second : BodyEffect{};
}

const BodyModel* BodyData::model(u64 modelHash) const {
    const auto it = m_models.find(modelHash);
    return it != m_models.end() ? &it->second : nullptr;
}

BodyEffect BodyData::bloodSpurt(u32 bloodLevel, bool front, bool large, u32 violence) const {
    const auto level = bloodLevel != 0 ? m_bloodLevels.find(bloodLevel) : m_bloodLevels.end();
    if (level == m_bloodLevels.end() || violence > 2) {
        return {};
    }
    const auto blood = m_bloods.find(level->second[violence]);
    if (blood == m_bloods.end()) {
        return {}; // 0: no blood at this violence level
    }
    return effect(blood->second[(front ? 0u : 2u) + (large ? 1u : 0u)]);
}

BodyEffect BodyData::footstepSpray(u32 terrainSound, bool walking) const {
    if (terrainSound >= kTerrainSounds) {
        return {};
    }
    return walking ? m_walkSpray[terrainSound] : m_runSpray[terrainSound];
}

bool BodyData::keepsFootprints(u32 terrainSound) const {
    return terrainSound < kTerrainSounds && m_footprints[terrainSound];
}

bool BodyData::coldArea(const AreaData& areas, u32 areaId) {
    const AreaInfo* area = areas.area(areaId);
    if (!area) {
        return false;
    }
    u32 flags = area->flags;
    if (!(flags & kAreaOverridesZone)) {
        if (const AreaInfo* zone = area->parent != 0 ? areas.area(area->parent) : nullptr) {
            flags = zone->flags;
        }
    }
    return (flags & kAreaSnow) != 0;
}

bool BodyData::openBounds(const pack::PackReader& bodyfx) {
    m_boundsModels = nullptr;
    m_boundsBoxes = nullptr;
    m_boundsModelCount = m_boundsBoxCount = 0;
    const std::optional<pack::PackAsset> asset = bodyfx.find(pack::kBodyBoundsAsset);
    if (!asset || asset->compression != pack::Compression::None ||
        asset->size < sizeof(pack::BodyBoundsHeader)) {
        return false;
    }
    pack::BodyBoundsHeader header;
    std::memcpy(&header, asset->data, sizeof(header));
    const u64 need = sizeof(header) + u64{header.modelCount} * sizeof(pack::BodyBoundsModel) +
                     u64{header.boxCount} * sizeof(pack::BodyBoundsBox);
    if (header.version != pack::kBodyBoundsVersion || need > asset->size) {
        VD_LOG_WARN("bodyfx.vdpk: version %u, expected %u (re-bake: vd_baker bake --only bodyfx)",
                    header.version, pack::kBodyBoundsVersion);
        return false;
    }
    // The blob is 64-byte aligned in the pack, and every record is made of 4- and 8-byte fields.
    m_boundsModels = reinterpret_cast<const pack::BodyBoundsModel*>(asset->data + sizeof(header));
    m_boundsBoxes = reinterpret_cast<const pack::BodyBoundsBox*>(
        asset->data + sizeof(header) + header.modelCount * sizeof(pack::BodyBoundsModel));
    m_boundsModelCount = header.modelCount;
    m_boundsBoxCount = header.boxCount;
    return true;
}

const pack::BodyBoundsBox* BodyData::bounds(u64 model, u16 animation) const {
    if (!m_boundsModels) {
        return nullptr;
    }
    const pack::BodyBoundsModel* end = m_boundsModels + m_boundsModelCount;
    const pack::BodyBoundsModel* entry = std::lower_bound(
        m_boundsModels, end, model, [](const pack::BodyBoundsModel& m, u64 hash) { return m.model < hash; });
    if (entry == end || entry->model != model || entry->boxCount == 0 ||
        u64{entry->firstBox} + entry->boxCount > m_boundsBoxCount) {
        return nullptr;
    }
    const pack::BodyBoundsBox* first = m_boundsBoxes + entry->firstBox;
    const pack::BodyBoundsBox* last = first + entry->boxCount;
    const auto find = [&](u16 id) -> const pack::BodyBoundsBox* {
        const pack::BodyBoundsBox* box = std::lower_bound(
            first, last, id, [](const pack::BodyBoundsBox& b, u16 a) { return b.animation < a; });
        return box != last && box->animation == id ? box : nullptr;
    };
    if (const pack::BodyBoundsBox* box = find(animation)) {
        return box;
    }
    const pack::BodyBoundsBox* stand = find(0);
    return stand ? stand : first;
}

} // namespace vd::game
