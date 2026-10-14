#pragma once

#include "core/Types.h"
#include "db/DbcView.h"

#include <array>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

// AreaTable.dbc flags (vMaNGOS DBCEnums.h AreaFlags) and teams (AreaTeams).
constexpr u32 kAreaFlagEnemiesPvp = 0x010; // AREA_FLAG_UNK3: enemies are flagged for PvP
constexpr u32 kAreaFlagRest = 0x020;       // AREA_FLAG_SLAVE_CAPITAL2: resting allowed
constexpr u32 kAreaFlagArena = 0x080;      // free-for-all PvP (Gurubashi Arena)
constexpr u32 kAreaFlagCapital = 0x100;
constexpr u32 kAreaFlagSanctuary = 0x800; // AREA_FLAG_SANCTUARY of later clients: no 1.12 area has it
constexpr u32 kAreaTeamAlliance = 2;      // AREATEAM_ALLY
constexpr u32 kAreaTeamHorde = 4;         // AREATEAM_HORDE

// How a zone looks to the player (the client's GetZonePVPInfo): the color of the zone text.
enum class ZonePvp : u8 {
    Normal,    // no faction (a test map, an instance)
    Friendly,  // the player's faction's territory
    Hostile,   // the other faction's
    Contested, // neither
    Sanctuary,
    Arena, // free-for-all PvP
};

// One AreaTable.dbc row (1.12.1: 25 fields).
struct AreaInfo {
    u32 id = 0;
    u32 map = 0;
    u32 parent = 0;        // its zone (0: it is a zone)
    u32 exploreBit = 0;    // AreaBit: bit of PLAYER_EXPLORED_ZONES_1.. (0..2047)
    u32 flags = 0;         // kAreaFlag*
    i32 level = 0;         // exploration level (0: discovering it gives no experience)
    u32 team = 0;          // kAreaTeam* (0: neither)
    std::string_view name; // enUS
};

// One AreaTrigger.dbc row: a sphere (radius > 0) or a box turned about z, as vMaNGOS tests them
// (ObjectMgr.cpp IsPointInAreaTriggerZone).
struct AreaTriggerInfo {
    u32 id = 0;
    u32 map = 0;
    f32 x = 0.0f, y = 0.0f, z = 0.0f;
    f32 radius = 0.0f;
    f32 boxX = 0.0f, boxY = 0.0f, boxZ = 0.0f; // the box's full extents
    f32 boxOrientation = 0.0f;                 // radians, counter-clockwise

    // `delta` grows the shape (the server accepts 5 yards more than the data).
    bool contains(f32 px, f32 py, f32 pz, f32 delta = 0.0f) const;
};

// One Map.dbc row (42 fields): what the loading screen and the zone text need.
struct MapInfo {
    u32 id = 0;
    std::string_view directory; // "Azeroth": the world pack and minimap names
    std::string_view name;      // enUS ("Eastern Kingdoms", "Deadmines")
    u32 instanceType = 0;       // 0 none, 1 party, 2 raid, 3 battleground
    u32 areaId = 0;             // the AreaTable zone of an instance (0: none)
    u64 loadingScreen = 0;      // assetPathHash of its LoadingScreens.dbc texture, 0: none
    // Phase 17 (queue): what GetBattlefieldInfo (WoW.exe 0x4AB0B0) reads of a battleground's row: 13
    // MinLevel, 14 MaxLevel, 15 MaxPlayers (per side: JoinBattlefield's group check), 20 / 29 the
    // descriptions (enUS) for the Horde and the Alliance, 39 the levels of a bracket (SMSG_BATTLEFIELD_LIST's
    // bracket range), 40 whether a group may join it (CanJoinBattlefieldAsGroup: not Alterac Valley).
    u32 minLevel = 0, maxLevel = 0, maxPlayers = 0;
    std::string_view descriptionHorde, descriptionAlliance;
    u32 bracketLevels = 0;
    bool groupJoin = false;
};

// Static client tables about where the player is (phase 8): AreaTable, AreaTrigger, Map and LoadingScreens (and
// the loading screens' GameTips).
// The DBCs stay in the mapped db pack (names are views into it); this only indexes them. Field numbers are
// those of the 1.12.1 files (vMaNGOS DBCfmt.h; checked against the baked db.vdpk in tests).
class AreaData {
public:
    bool init(const pack::PackReader& db);
    // The tables from elsewhere (tests, tools): indexes them as init() does.
    void set(std::vector<AreaInfo> areas, std::vector<AreaTriggerInfo> triggers, std::vector<MapInfo> maps);
    bool ready() const { return !m_areas.empty(); }

    const AreaInfo* area(u32 id) const;
    // The zone an area belongs to: its parent, or itself for a zone (0 when unknown).
    u32 zoneOf(u32 areaId) const;
    // The zone of a map without terrain areas (an instance): Map.dbc's area, else the map's first zone; 0 if
    // none.
    u32 mapZone(u32 map) const;
    // The area of `map` whose exploration bit is `bit` (as the server's AreaEntry::GetByAreaFlagAndMap).
    const AreaInfo* areaByExploreBit(u32 bit, u32 map) const;
    // PvP status of standing in `areaId` for a player of `playerTeam` (kAreaTeamAlliance / kAreaTeamHorde):
    // the subzone's team, else its zone's.
    ZonePvp pvp(u32 areaId, u32 playerTeam) const;
    // Whether the player discovered `areaId`: its bit in PLAYER_EXPLORED_ZONES (64 words).
    bool explored(std::span<const u32> exploredZones, u32 areaId) const;

    // AreaTrigger.dbc rows of a map (sorted by id).
    std::span<const AreaTriggerInfo> triggers(u32 map) const;
    const AreaTriggerInfo* trigger(u32 id) const;

    const MapInfo* map(u32 id) const;
    const MapInfo* mapByDirectory(std::string_view directory) const;

    // Phase 20 (retouches): GameTips.dbc's lines (enUS, in row order, without their trailing line breaks).
    void setGameTips(std::vector<std::string_view> tips) { m_tips = std::move(tips); }
    std::span<const std::string_view> gameTips() const { return m_tips; }
    // A loading screen's tip as WoW.exe picks it (0x46B5F0): the row `next` (the gameTip CVar; out of range: the
    // first), and `next` becomes the row after it, for the next loading screen. Empty without tips.
    std::string_view gameTip(u32& next) const;

private:
    std::vector<AreaInfo> m_areas;
    std::unordered_map<u32, u32> m_areaIndex; // id -> m_areas
    std::vector<AreaTriggerInfo> m_triggers;  // by map, then id
    std::vector<MapInfo> m_maps;
    std::vector<std::string_view> m_tips;
};

} // namespace vd::game
