#pragma once

#include "core/Types.h"
#include "pack/AsyncReader.h"
#include "pack/PackReader.h"

#include <glm/vec3.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vd {

// One WMOAreaTable.dbc row (1.12.1, 20 columns: vMaNGOS' DBCStructure.h names 0-3, 9 and 10; the sound columns 4-8
// are the client's m_SoundProviderPref, m_SoundProviderPrefUnderwater, m_AmbienceID, m_ZoneMusic, m_IntroSound;
// 11-18 the name by locale, 11 enUS). Checked against the client's rows in tests/world/test_wmo_areas.cpp.
struct WmoAreaRow {
    u32 id = 0;
    i32 wmoId = 0;         // MOHD
    i32 nameSet = 0;       // the placement's MODF
    i32 groupId = 0;       // MOGP; -1: the whole building
    u32 ambience = 0;      // SoundAmbience id
    u32 zoneMusic = 0;     // ZoneMusic id
    u32 intro = 0;         // ZoneIntroMusicTable id
    u32 flags = 0;         // 0 in every 1.12 row
    u32 areaId = 0;        // AreaTable id (0: the terrain's)
    std::string_view name; // enUS; empty: the building's (its -1 row), else the area's
};

// The room of a building a point is in, and what WMOAreaTable says of it: the group's row, else the building's (its
// group -1 row) for each column (a tavern's rooms share the root row's music, ambience and name; a cellar has its
// own ambience).
struct WmoAreaHit {
    u32 uniqueId = 0; // the placement
    i32 wmoId = -1;   // -1: not in a building with rows
    i32 nameSet = 0;
    i32 groupId = 0;
    u32 groupIndex = 0;
    u32 groupFlags = 0;                // MOGP
    f32 floorZ = 0.0f;                 // world height of the floor found under the point
    const WmoAreaRow* group = nullptr; // (wmoId, nameSet, groupId), may be null
    const WmoAreaRow* root = nullptr;  // (wmoId, nameSet, -1), may be null
    u32 areaId = 0;
    u32 ambience = 0;
    u32 zoneMusic = 0;
    u32 intro = 0;
    std::string_view name;
    bool valid() const { return wmoId >= 0 && (group || root); }
    // Changes when the place changes (another row or building).
    u64 key() const { return valid() ? (static_cast<u64>(uniqueId) << 32) | (group ? group->id : root->id) : 0; }
};

// Phase 14 (sound): which room of which building a point is in (pack/WorldSoundFormat.h "wmoareas/<map>", read on
// its own IO thread when the map changes), and its WMOAreaTable row. The music and ambience of inns and of the halls of
// Ironforge, and the names of Stormwind's districts and of the inns on the minimap, come from here. Works with the
// audio off (the names). Game thread only.
class WmoAreas {
public:
    ~WmoAreas();

    // WMOAreaTable.dbc from db.vdpk (the rows are copied: `db` may close), worldsound.vdpk from `packsDir` (missing:
    // nothing is ever inside a building).
    bool init(const pack::PackReader& db, const std::filesystem::path& packsDir);
    void shutdown();
    bool rowsReady() const { return !m_rows.empty(); }

    // The map to look in (Map.dbc directory, any case; empty: none). The same map again costs a comparison.
    void setMap(std::string_view map);
    // Takes the map's table once it has been read.
    void update();
    bool ready() const { return m_data != nullptr; }

    // Where `feet` (world) is, for a world without the WMOs' BSP (inGroup() otherwise): the vmap rule (a ray down from a yard over the feet, inside a group's box, the nearest
    // floor wins) unless the terrain (`terrainHeight` under the feet, if known) lies between that floor and the
    // feet. Not in a building with rows: an invalid hit. Asked every frame: the answer is kept while `feet` stays
    // within a quarter of a yard and for a few calls.
    const WmoAreaHit& at(const glm::dvec3& feet, std::optional<f32> terrainHeight);
    // The same without the cache (tests, tools).
    WmoAreaHit locate(const glm::dvec3& feet, std::optional<f32> terrainHeight) const;
    // The room a character is in when the world knows its building and group (WorldObjects::standingAt(): 1.12's rule
    // over the WMO's BSP, sharper than the floors baked here): the placement `uniqueId`'s row for its group
    // `groupIndex` (the WMO's order) of `groupCount` (a building whose group count differs from the baked one is not
    // trusted). Not in a building with rows: an invalid hit. Asked every frame: kept while the place stays the same.
    const WmoAreaHit& inGroup(u32 uniqueId, u32 groupIndex, u32 groupCount);

    const WmoAreaRow* row(i32 wmoId, i32 nameSet, i32 groupId) const;
    // Fills the resolved columns of `hit` from its rows.
    void resolve(WmoAreaHit& hit) const;

    // Tests and tools: a map's table from a blob (as the IO thread would hand it over); checks it like the reader.
    bool load(std::unique_ptr<u8[]> data, u32 size);
    static bool validate(const u8* data, u32 size);
    u32 placementCount() const;

private:
    static u64 rowKey(i32 wmoId, i32 nameSet, i32 groupId);

    std::vector<WmoAreaRow> m_rows;
    std::unordered_map<u64, u32> m_rowIndex;
    std::vector<char> m_names; // the rows' names (string_views point here)
    pack::PackReader m_pack;
    bool m_packOpen = false;
    pack::AsyncReader m_reader;
    bool m_started = false;
    std::string m_map;
    u64 m_request = 0;
    std::unique_ptr<u8[]> m_data;
    u32 m_size = 0;
    // at()'s last answer.
    WmoAreaHit m_hit;
    glm::dvec3 m_hitPoint{1e30};
    u32 m_hitAge = 0;
    // inGroup()'s last answer, and the place it was for (stale: a new table since).
    WmoAreaHit m_groupHit;
    u64 m_groupKey = 0;
    bool m_groupStale = true;
};

} // namespace vd
