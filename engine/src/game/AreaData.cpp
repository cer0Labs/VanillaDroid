#include "game/AreaData.h"

#include "core/Hash.h"
#include "core/Log.h"
#include "pack/PackReader.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace vd::game {
namespace {

// AreaTable.dbc (AreaTableEntryfmt "niiiixxxxxissssssssxixxxi"): 0 id, 1 map, 2 parent zone, 3 AreaBit, 4
// flags, 10 exploration level, 11 name (enUS), 20 faction group (team).
namespace area {
constexpr u32 kMap = 1, kParent = 2, kExploreBit = 3, kFlags = 4, kLevel = 10, kName = 11, kTeam = 20;
constexpr u32 kFieldCount = 25;
} // namespace area
// AreaTrigger.dbc (AreaTriggerEntryfmt "niffffffff"): id, map, x, y, z, radius, box x, y, z, orientation.
constexpr u32 kTriggerFieldCount = 10;
// Map.dbc (42 fields): 1 directory, 2 instance type, 4 name (enUS), 19 AreaTable id, 38 LoadingScreens id.
namespace map {
constexpr u32 kDirectory = 1, kInstanceType = 2, kName = 4, kAreaId = 19, kLoadingScreen = 38;
// Phase 17 (queue): 13 MinLevel, 14 MaxLevel, 15 MaxPlayers, 20 / 29 MapDescription0 / 1 (enUS), 39 the
// levels of a bracket, 40 a group may join (WoW.exe 0x4AA6C0, 0x4AB0B0, 0x4A9F60, 0x4AC380).
constexpr u32 kMinLevel = 13, kMaxLevel = 14, kMaxPlayers = 15;
constexpr u32 kDescriptionHorde = 20, kDescriptionAlliance = 29;
constexpr u32 kBracketLevels = 39, kGroupJoin = 40;
constexpr u32 kFieldCount = 42;
} // namespace map
// LoadingScreens.dbc: id, name, texture path.
constexpr u32 kLoadingScreenPath = 2;
// GameTips.dbc (10 fields): id, the text (enUS; 2..9 the other locales and their flags).
constexpr u32 kGameTipText = 1;

std::optional<db::DbcView> dbc(const pack::PackReader& db, const char* name, u32 minFields) {
    const std::optional<pack::PackAsset> asset = db.find(std::string("DBFilesClient\\") + name + ".dbc");
    if (!asset) {
        VD_LOG_WARN("%s.dbc missing", name);
        return std::nullopt;
    }
    Result<db::DbcView> parsed = db::DbcView::parse(asset->data, asset->size);
    if (!parsed) {
        VD_LOG_WARN("%s.dbc: %s", name, parsed.error().message.c_str());
        return std::nullopt;
    }
    if (parsed.value().fieldCount() < minFields) {
        VD_LOG_WARN("%s.dbc has %u fields, expected %u", name, parsed.value().fieldCount(), minFields);
        return std::nullopt;
    }
    return parsed.value();
}

} // namespace

bool AreaTriggerInfo::contains(f32 px, f32 py, f32 pz, f32 delta) const {
    if (radius > 0.0f) {
        const f32 dx = px - x, dy = py - y, dz = pz - z;
        return dx * dx + dy * dy + dz * dz <= (radius + delta) * (radius + delta);
    }
    // The point turned into the box's frame (the server rotates by 2 pi - orientation).
    const f64 rotation = 2.0 * 3.14159265358979323846 - static_cast<f64>(boxOrientation);
    const f64 s = std::sin(rotation), c = std::cos(rotation);
    const f64 ox = px - x, oy = py - y;
    const f32 dx = static_cast<f32>(ox * c - oy * s);
    const f32 dy = static_cast<f32>(oy * c + ox * s);
    const f32 dz = pz - z;
    return std::abs(dx) <= boxX * 0.5f + delta && std::abs(dy) <= boxY * 0.5f + delta &&
           std::abs(dz) <= boxZ * 0.5f + delta;
}

bool AreaData::init(const pack::PackReader& db) {
    std::vector<AreaInfo> areas;
    std::vector<AreaTriggerInfo> triggers;
    std::vector<MapInfo> maps;
    bool ok = true;
    if (const std::optional<db::DbcView> table = dbc(db, "AreaTable", area::kFieldCount)) {
        areas.reserve(table->recordCount());
        for (u32 r = 0; r < table->recordCount(); ++r) {
            AreaInfo a;
            a.id = table->getU32(r, 0);
            a.map = table->getU32(r, area::kMap);
            a.parent = table->getU32(r, area::kParent);
            a.exploreBit = table->getU32(r, area::kExploreBit);
            a.flags = table->getU32(r, area::kFlags);
            a.level = table->getI32(r, area::kLevel);
            a.team = table->getU32(r, area::kTeam);
            a.name = table->getString(r, area::kName);
            areas.push_back(a);
        }
    } else {
        ok = false;
    }
    if (const std::optional<db::DbcView> table = dbc(db, "AreaTrigger", kTriggerFieldCount)) {
        triggers.reserve(table->recordCount());
        for (u32 r = 0; r < table->recordCount(); ++r) {
            AreaTriggerInfo t;
            t.id = table->getU32(r, 0);
            t.map = table->getU32(r, 1);
            t.x = table->getF32(r, 2);
            t.y = table->getF32(r, 3);
            t.z = table->getF32(r, 4);
            t.radius = table->getF32(r, 5);
            t.boxX = table->getF32(r, 6);
            t.boxY = table->getF32(r, 7);
            t.boxZ = table->getF32(r, 8);
            t.boxOrientation = table->getF32(r, 9);
            triggers.push_back(t);
        }
    } else {
        ok = false;
    }
    const std::optional<db::DbcView> screens = dbc(db, "LoadingScreens", kLoadingScreenPath + 1);
    if (const std::optional<db::DbcView> table = dbc(db, "Map", map::kFieldCount)) {
        maps.reserve(table->recordCount());
        for (u32 r = 0; r < table->recordCount(); ++r) {
            MapInfo m;
            m.id = table->getU32(r, 0);
            m.directory = table->getString(r, map::kDirectory);
            m.instanceType = table->getU32(r, map::kInstanceType);
            m.name = table->getString(r, map::kName);
            m.areaId = table->getU32(r, map::kAreaId);
            m.minLevel = table->getU32(r, map::kMinLevel); // phase 17 (queue): the battlegrounds' frame
            m.maxLevel = table->getU32(r, map::kMaxLevel);
            m.maxPlayers = table->getU32(r, map::kMaxPlayers);
            m.descriptionHorde = table->getString(r, map::kDescriptionHorde);
            m.descriptionAlliance = table->getString(r, map::kDescriptionAlliance);
            m.bracketLevels = table->getU32(r, map::kBracketLevels);
            m.groupJoin = table->getU32(r, map::kGroupJoin) != 0;
            const u32 screen = table->getU32(r, map::kLoadingScreen);
            if (screens && screen != 0) {
                const u32 record = screens->findById(screen);
                if (record < screens->recordCount()) {
                    m.loadingScreen = assetPathHash(screens->getString(record, kLoadingScreenPath));
                }
            }
            maps.push_back(m);
        }
    } else {
        ok = false;
    }
    set(std::move(areas), std::move(triggers), std::move(maps));
    // Phase 20 (retouches): the loading screens' tips. WoW.exe trims the line breaks that end most of them.
    m_tips.clear();
    if (const std::optional<db::DbcView> table = dbc(db, "GameTips", kGameTipText + 1)) {
        m_tips.reserve(table->recordCount());
        for (u32 r = 0; r < table->recordCount(); ++r) {
            std::string_view tip = table->getString(r, kGameTipText);
            while (!tip.empty() && (tip.back() == '\r' || tip.back() == '\n')) {
                tip.remove_suffix(1);
            }
            m_tips.push_back(tip);
        }
    }
    VD_LOG_INFO("Areas: %zu areas, %zu area triggers, %zu maps, %zu tips", m_areas.size(), m_triggers.size(),
                m_maps.size(), m_tips.size());
    return ok;
}

void AreaData::set(std::vector<AreaInfo> areas, std::vector<AreaTriggerInfo> triggers,
                   std::vector<MapInfo> maps) {
    m_areas = std::move(areas);
    m_triggers = std::move(triggers);
    m_maps = std::move(maps);
    m_areaIndex.clear();
    m_areaIndex.reserve(m_areas.size());
    for (u32 i = 0; i < m_areas.size(); ++i) {
        m_areaIndex.emplace(m_areas[i].id, i);
    }
    std::sort(m_triggers.begin(), m_triggers.end(), [](const AreaTriggerInfo& a, const AreaTriggerInfo& b) {
        return a.map != b.map ? a.map < b.map : a.id < b.id;
    });
}

const AreaInfo* AreaData::area(u32 id) const {
    const auto found = m_areaIndex.find(id);
    return found == m_areaIndex.end() ? nullptr : &m_areas[found->second];
}

u32 AreaData::zoneOf(u32 areaId) const {
    const AreaInfo* a = area(areaId);
    if (!a) {
        return 0;
    }
    return a->parent != 0 ? a->parent : a->id;
}

u32 AreaData::mapZone(u32 mapId) const {
    if (const MapInfo* m = map(mapId); m && m->areaId != 0 && area(m->areaId)) {
        return m->areaId;
    }
    for (const AreaInfo& a : m_areas) {
        if (a.map == mapId && a.parent == 0) {
            return a.id;
        }
    }
    return 0;
}

const AreaInfo* AreaData::areaByExploreBit(u32 bit, u32 map) const {
    // 1.12.1's table repeats some bits: the one of this map wins, else any (vMaNGOS GetByAreaFlagAndMap).
    const AreaInfo* any = nullptr;
    for (const AreaInfo& a : m_areas) {
        if (a.exploreBit == bit && bit != 0) {
            if (a.map == map) {
                return &a;
            }
            any = any ? any : &a;
        }
    }
    return any;
}

ZonePvp AreaData::pvp(u32 areaId, u32 playerTeam) const {
    const AreaInfo* a = area(areaId);
    if (!a) {
        return ZonePvp::Normal;
    }
    const AreaInfo* zone = a->parent ? area(a->parent) : a;
    const u32 flags = a->flags | (zone ? zone->flags : 0);
    if (flags & kAreaFlagSanctuary) {
        return ZonePvp::Sanctuary;
    }
    if (a->flags & kAreaFlagArena) {
        return ZonePvp::Arena;
    }
    const u32 team = a->team != 0 ? a->team : (zone ? zone->team : 0);
    const MapInfo* m = map(a->map);
    if (m && m->instanceType != 0 && team == 0) {
        return ZonePvp::Normal; // dungeons and battlegrounds of nobody
    }
    if (team == 0) {
        return ZonePvp::Contested;
    }
    return team == playerTeam ? ZonePvp::Friendly : ZonePvp::Hostile;
}

bool AreaData::explored(std::span<const u32> exploredZones, u32 areaId) const {
    const AreaInfo* a = area(areaId);
    if (!a) {
        return false;
    }
    const u32 word = a->exploreBit / 32;
    return word < exploredZones.size() && (exploredZones[word] & (1u << (a->exploreBit % 32))) != 0;
}

std::span<const AreaTriggerInfo> AreaData::triggers(u32 map) const {
    const auto first = std::lower_bound(m_triggers.begin(), m_triggers.end(), map,
                                        [](const AreaTriggerInfo& t, u32 m) { return t.map < m; });
    auto last = first;
    while (last != m_triggers.end() && last->map == map) {
        ++last;
    }
    return {m_triggers.data() + (first - m_triggers.begin()), static_cast<usize>(last - first)};
}

const AreaTriggerInfo* AreaData::trigger(u32 id) const {
    for (const AreaTriggerInfo& t : m_triggers) {
        if (t.id == id) {
            return &t;
        }
    }
    return nullptr;
}

const MapInfo* AreaData::map(u32 id) const {
    for (const MapInfo& m : m_maps) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

const MapInfo* AreaData::mapByDirectory(std::string_view directory) const {
    for (const MapInfo& m : m_maps) {
        if (m.directory.size() == directory.size() &&
            std::equal(m.directory.begin(), m.directory.end(), directory.begin(), [](char a, char b) {
                return normalizeAssetPathChar(a) == normalizeAssetPathChar(b);
            })) {
            return &m;
        }
    }
    return nullptr;
}

std::string_view AreaData::gameTip(u32& next) const {
    if (m_tips.empty()) {
        return {};
    }
    const u32 row = next < m_tips.size() ? next : 0;
    next = row + 1;
    return m_tips[row];
}

} // namespace vd::game
