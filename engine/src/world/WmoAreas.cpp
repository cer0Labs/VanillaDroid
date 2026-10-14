#include "world/WmoAreas.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "db/DbcView.h"
#include "pack/WorldSoundFormat.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace vd {
namespace {

// WMOAreaTable.dbc (5875).
constexpr u32 kWmoId = 1, kNameSet = 2, kGroupId = 3, kAmbience = 6, kZoneMusic = 7, kIntro = 8, kFlags = 9,
              kAreaId = 10, kName = 11, kFieldCount = 20;
// The vmap query starts a yard over the feet (TerrainInfo::GetAreaInfo: z += 1) and gives up on the WMO when the
// terrain is above the floor found and below two yards over that point.
constexpr f32 kRayStart = 1.0f;
constexpr f32 kTerrainAbove = 2.0f;
// A floor this close above the ray's start still counts (the cell's heights are rounded to its grid).
constexpr f32 kFloorSlack = 0.25f;

const pack::WmoAreasHeader& header(const u8* data) {
    return *reinterpret_cast<const pack::WmoAreasHeader*>(data);
}

template <typename T>
const T* section(const u8* data, u32 offset) {
    return reinterpret_cast<const T*>(data + offset);
}

bool inBox(const f32 (&min)[3], const f32 (&max)[3], const glm::vec3& p) {
    return p.x >= min[0] && p.x <= max[0] && p.y >= min[1] && p.y <= max[1] && p.z >= min[2] && p.z <= max[2];
}

} // namespace

WmoAreas::~WmoAreas() {
    shutdown();
}

u64 WmoAreas::rowKey(i32 wmoId, i32 nameSet, i32 groupId) {
    return (static_cast<u64>(static_cast<u32>(wmoId)) << 40) ^ (static_cast<u64>(static_cast<u16>(nameSet)) << 32) ^
           static_cast<u32>(groupId);
}

bool WmoAreas::init(const pack::PackReader& db, const std::filesystem::path& packsDir) {
    const std::optional<pack::PackAsset> asset = db.find("DBFilesClient\\WMOAreaTable.dbc");
    Result<db::DbcView> view =
        asset ? db::DbcView::parse(asset->data, asset->size) : Result<db::DbcView>(Error{"none"});
    if (!view || view.value().fieldCount() < kFieldCount) {
        VD_LOG_WARN("WMOAreaTable.dbc missing or unexpected: no interior areas");
        return false;
    }
    const db::DbcView& t = view.value();
    usize nameBytes = 0;
    for (u32 r = 0; r < t.recordCount(); ++r) {
        nameBytes += t.getString(r, kName).size();
    }
    m_names.assign(nameBytes, '\0'); // sized once: the rows' names point into it
    m_rows.resize(t.recordCount());
    m_rowIndex.reserve(t.recordCount());
    usize at = 0;
    for (u32 r = 0; r < t.recordCount(); ++r) {
        WmoAreaRow& row = m_rows[r];
        row.id = t.getU32(r, 0);
        row.wmoId = t.getI32(r, kWmoId);
        row.nameSet = t.getI32(r, kNameSet);
        row.groupId = t.getI32(r, kGroupId);
        row.ambience = t.getU32(r, kAmbience);
        row.zoneMusic = t.getU32(r, kZoneMusic);
        row.intro = t.getU32(r, kIntro);
        row.flags = t.getU32(r, kFlags);
        row.areaId = t.getU32(r, kAreaId);
        const std::string_view name = t.getString(r, kName);
        std::memcpy(m_names.data() + at, name.data(), name.size());
        row.name = std::string_view(m_names.data() + at, name.size());
        at += name.size();
        m_rowIndex.emplace(rowKey(row.wmoId, row.nameSet, row.groupId), r);
    }
    const std::filesystem::path packPath = packsDir / pack::kWorldSoundPack;
    if (Result<void> opened = m_pack.open(packPath); opened) {
        m_packOpen = true;
    } else {
        VD_LOG_INFO("No %s (vd_baker bake --only worldsound): no interior areas", pack::kWorldSoundPack);
    }
    return true;
}

void WmoAreas::shutdown() {
    if (m_started) {
        m_reader.stop();
        m_started = false;
    }
    m_data.reset();
    m_size = 0;
    m_map.clear();
    if (m_packOpen) {
        m_pack.close();
        m_packOpen = false;
    }
}

bool WmoAreas::validate(const u8* data, u32 size) {
    if (!data || size < sizeof(pack::WmoAreasHeader)) {
        return false;
    }
    const pack::WmoAreasHeader& h = header(data);
    const auto fits = [&](u32 offset, u64 count, u64 stride) {
        return count == 0 || (offset % 4 == 0 && u64{offset} + count * stride <= size);
    };
    if (h.version != pack::kWorldSoundVersion ||
        !fits(h.placementsOffset, h.placementCount, sizeof(pack::WmoAreaPlacement)) ||
        !fits(h.modelsOffset, h.modelCount, sizeof(pack::WmoAreaModel)) ||
        !fits(h.groupsOffset, h.groupCount, sizeof(pack::WmoAreaGroup)) || !fits(h.cellsOffset, h.cellCount, 2)) {
        return false;
    }
    const auto* placements = section<pack::WmoAreaPlacement>(data, h.placementsOffset);
    for (u32 i = 0; i < h.placementCount; ++i) {
        if (placements[i].model >= h.modelCount) {
            return false;
        }
    }
    const auto* models = section<pack::WmoAreaModel>(data, h.modelsOffset);
    for (u32 i = 0; i < h.modelCount; ++i) {
        if (u64{models[i].firstGroup} + models[i].groupCount > h.groupCount) {
            return false;
        }
    }
    const auto* groups = section<pack::WmoAreaGroup>(data, h.groupsOffset);
    for (u32 i = 0; i < h.groupCount; ++i) {
        if (u64{groups[i].firstCell} + u64{groups[i].cellsX} * groups[i].cellsY > h.cellCount) {
            return false;
        }
    }
    return true;
}

bool WmoAreas::load(std::unique_ptr<u8[]> data, u32 size) {
    if (!validate(data.get(), size)) {
        return false;
    }
    m_data = std::move(data);
    m_size = size;
    m_hitPoint = glm::dvec3(1e30);
    m_groupStale = true;
    return true;
}

u32 WmoAreas::placementCount() const {
    return m_data ? header(m_data.get()).placementCount : 0;
}

void WmoAreas::setMap(std::string_view map) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    if (map.size() == m_map.size() &&
        std::equal(map.begin(), map.end(), m_map.begin(), [&](char a, char b) { return lower(a) == b; })) {
        return;
    }
    m_map.assign(map);
    std::transform(m_map.begin(), m_map.end(), m_map.begin(), lower);
    m_data.reset();
    m_size = 0;
    m_hit = {};
    m_hitPoint = glm::dvec3(1e30);
    m_groupStale = true;
    ++m_request; // a read still in flight for the last map is dropped when it arrives
    if (m_map.empty() || !m_packOpen) {
        return;
    }
    const std::optional<pack::PackAsset> asset = m_pack.find(pack::wmoAreasPath(m_map));
    if (!asset || asset->type != pack::AssetType::WmoAreas) {
        return; // a map without buildings that have rows
    }
    if (!m_started) {
        m_reader.start("vd-wmoareas");
        m_started = true;
    }
    if (!m_reader.request(m_request, *asset, &WmoAreas::validate)) {
        VD_LOG_WARN("WMO areas of %s: the reader is busy", m_map.c_str());
    }
}

void WmoAreas::update() {
    if (!m_started) {
        return;
    }
    pack::AsyncReader::Done done;
    while (m_reader.poll(done)) {
        if (done.tag == m_request && done.data) {
            m_data.reset(done.data);
            m_size = done.size;
            m_hitPoint = glm::dvec3(1e30);
            m_groupStale = true;
        } else {
            delete[] done.data;
        }
    }
}

const WmoAreaRow* WmoAreas::row(i32 wmoId, i32 nameSet, i32 groupId) const {
    const auto it = m_rowIndex.find(rowKey(wmoId, nameSet, groupId));
    if (it == m_rowIndex.end()) {
        return nullptr;
    }
    const WmoAreaRow& r = m_rows[it->second];
    return r.wmoId == wmoId && r.nameSet == nameSet && r.groupId == groupId ? &r : nullptr;
}

void WmoAreas::resolve(WmoAreaHit& hit) const {
    hit.group = row(hit.wmoId, hit.nameSet, hit.groupId);
    hit.root = row(hit.wmoId, hit.nameSet, -1);
    const auto pick = [&](u32 WmoAreaRow::*column) {
        return hit.group && hit.group->*column != 0 ? hit.group->*column : (hit.root ? hit.root->*column : 0u);
    };
    hit.areaId = pick(&WmoAreaRow::areaId);
    hit.ambience = pick(&WmoAreaRow::ambience);
    hit.zoneMusic = pick(&WmoAreaRow::zoneMusic);
    hit.intro = pick(&WmoAreaRow::intro);
    hit.name = hit.group && !hit.group->name.empty() ? hit.group->name : hit.root ? hit.root->name : std::string_view{};
}

WmoAreaHit WmoAreas::locate(const glm::dvec3& feet, std::optional<f32> terrainHeight) const {
    WmoAreaHit best;
    if (!m_data) {
        return best;
    }
    const u8* data = m_data.get();
    const pack::WmoAreasHeader& h = header(data);
    const auto* placements = section<pack::WmoAreaPlacement>(data, h.placementsOffset);
    const auto* models = section<pack::WmoAreaModel>(data, h.modelsOffset);
    const auto* groups = section<pack::WmoAreaGroup>(data, h.groupsOffset);
    const auto* cells = section<u8>(data, h.cellsOffset);
    const glm::vec3 start(static_cast<f32>(feet.x), static_cast<f32>(feet.y), static_cast<f32>(feet.z) + kRayStart);
    f32 bestDistance = std::numeric_limits<f32>::max();
    for (u32 i = 0; i < h.placementCount; ++i) {
        const pack::WmoAreaPlacement& p = placements[i];
        if (!inBox(p.boundsMin, p.boundsMax, start)) {
            continue;
        }
        // The point in model space (the rows of the placement's rotation; scale 1).
        const f32* m = p.transform;
        const glm::vec3 d(start.x - m[3], start.y - m[7], start.z - m[11]);
        const glm::vec3 local(m[0] * d.x + m[4] * d.y + m[8] * d.z, m[1] * d.x + m[5] * d.y + m[9] * d.z,
                              m[2] * d.x + m[6] * d.y + m[10] * d.z);
        const pack::WmoAreaModel& model = models[p.model];
        for (u32 g = 0; g < model.groupCount; ++g) {
            const pack::WmoAreaGroup& group = groups[model.firstGroup + g];
            if (!inBox(group.boundsMin, group.boundsMax, local)) {
                continue;
            }
            const i32 cx = static_cast<i32>((local.x - group.boundsMin[0]) / pack::kWmoFloorCell);
            const i32 cy = static_cast<i32>((local.y - group.boundsMin[1]) / pack::kWmoFloorCell);
            if (cx < 0 || cy < 0 || cx >= group.cellsX || cy >= group.cellsY) {
                continue;
            }
            const u8* cell =
                cells + (u64{group.firstCell} + static_cast<u64>(cy) * group.cellsX + static_cast<u64>(cx)) * 2;
            if (cell[0] == pack::kWmoNoFloor) {
                continue; // nothing of this group under the point
            }
            const f32 lowest = group.boundsMin[2] + static_cast<f32>(cell[0]) * group.floorStep;
            const f32 highest = group.boundsMin[2] + static_cast<f32>(cell[1]) * group.floorStep;
            f32 floor = 0.0f;
            if (local.z + kFloorSlack >= highest) {
                floor = highest;
            } else if (local.z + kFloorSlack >= lowest) {
                floor = lowest; // a lower storey of the cell (what lies between is not known: its lowest)
            } else {
                continue; // the group is all above
            }
            const f32 distance = std::max(0.0f, local.z - floor);
            if (distance < bestDistance) {
                bestDistance = distance;
                best.uniqueId = p.uniqueId;
                best.wmoId = model.wmoId;
                best.nameSet = p.nameSet;
                best.groupId = group.groupId;
                best.groupIndex = g;
                best.groupFlags = group.flags;
                best.floorZ = start.z - distance;
            }
        }
    }
    if (best.wmoId < 0) {
        return best;
    }
    // The terrain between the floor and the point: on the terrain, not in the building (a cellar under the ground).
    if (terrainHeight && start.z + kTerrainAbove > *terrainHeight && *terrainHeight > best.floorZ) {
        return {};
    }
    resolve(best);
    if (!best.valid()) {
        return {};
    }
    return best;
}

const WmoAreaHit& WmoAreas::inGroup(u32 uniqueId, u32 groupIndex, u32 groupCount) {
    const u64 key = (static_cast<u64>(uniqueId) << 32) | groupIndex;
    if (!m_groupStale && key == m_groupKey) {
        return m_groupHit;
    }
    m_groupStale = false;
    m_groupKey = key;
    m_groupHit = {};
    if (!m_data) {
        return m_groupHit;
    }
    // Only when the place changes: a walk over the map's placements (a few thousand at most).
    const u8* data = m_data.get();
    const pack::WmoAreasHeader& h = header(data);
    const auto* placements = section<pack::WmoAreaPlacement>(data, h.placementsOffset);
    const auto* models = section<pack::WmoAreaModel>(data, h.modelsOffset);
    const auto* groups = section<pack::WmoAreaGroup>(data, h.groupsOffset);
    for (u32 i = 0; i < h.placementCount; ++i) {
        const pack::WmoAreaPlacement& p = placements[i];
        if (p.uniqueId != uniqueId) {
            continue;
        }
        const pack::WmoAreaModel& model = models[p.model];
        if (model.groupCount != groupCount || groupIndex >= model.groupCount) {
            break;
        }
        const pack::WmoAreaGroup& group = groups[model.firstGroup + groupIndex];
        WmoAreaHit hit;
        hit.uniqueId = p.uniqueId;
        hit.wmoId = model.wmoId;
        hit.nameSet = p.nameSet;
        hit.groupId = group.groupId;
        hit.groupIndex = groupIndex;
        hit.groupFlags = group.flags;
        resolve(hit);
        if (hit.valid()) {
            m_groupHit = hit;
        }
        break;
    }
    return m_groupHit;
}

const WmoAreaHit& WmoAreas::at(const glm::dvec3& feet, std::optional<f32> terrainHeight) {
    const glm::dvec3 moved = feet - m_hitPoint;
    if (glm::dot(moved, moved) < 0.0625 && ++m_hitAge < 16) {
        return m_hit;
    }
    VD_PROFILE_ZONE("WmoAreas::at");
    m_hitPoint = feet;
    m_hitAge = 0;
    m_hit = locate(feet, terrainHeight);
    return m_hit;
}

} // namespace vd
