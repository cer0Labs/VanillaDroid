#include "game/WorldMapData.h"

#include "core/Hash.h"
#include "core/Log.h"
#include "db/DbcView.h"
#include "pack/PackReader.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>

namespace vd::game {
namespace {

// WorldMapArea.dbc (8 fields): id, map, area, directory, left, right, top, bottom.
constexpr u32 kAreaFieldCount = 8;
// WorldMapOverlay.dbc (17 fields): id, map area, 4 area ids, map point x / y, texture, width, height, offset
// x / y, hit rectangle.
namespace overlay {
constexpr u32 kMapArea = 1, kAreas = 2, kTexture = 8, kWidth = 9, kHeight = 10, kOffsetX = 11, kOffsetY = 12;
constexpr u32 kFieldCount = 17;
} // namespace overlay
// WorldMapContinent.dbc (13 fields): id, map, tile bounds (4), offset (2), scale, taxi min x / y, taxi max x
// / y.
constexpr u32 kContinentFieldCount = 13;

std::optional<db::DbcView> table(const pack::PackReader& db, const char* name, u32 minFields) {
    const std::optional<pack::PackAsset> asset = db.find(std::string("DBFilesClient\\") + name + ".dbc");
    if (!asset) {
        VD_LOG_WARN("%s.dbc missing", name);
        return std::nullopt;
    }
    Result<db::DbcView> parsed = db::DbcView::parse(asset->data, asset->size);
    if (!parsed || parsed.value().fieldCount() < minFields) {
        VD_LOG_WARN("%s.dbc: unexpected layout", name);
        return std::nullopt;
    }
    return parsed.value();
}

// assetPathHash of "Interface\WorldMap\<directory>\<name><index>.blp" without building the string.
u64 worldMapHash(std::string_view directory, std::string_view name, u32 index) {
    char number[12];
    const int digits = std::snprintf(number, sizeof(number), "%u", index);
    u64 hash = kFnvOffset;
    const auto add = [&](std::string_view part) {
        for (const char c : part) {
            hash ^= static_cast<u8>(normalizeAssetPathChar(c));
            hash *= kFnvPrime;
        }
    };
    add("interface/worldmap/");
    add(directory);
    add("/");
    add(name);
    add({number, static_cast<usize>(digits > 0 ? digits : 0)});
    add(".blp");
    return hash;
}

} // namespace

glm::vec2 WorldMapArea::toMap(f64 x, f64 y) const {
    return {static_cast<f32>((static_cast<f64>(left) - y) / (static_cast<f64>(left) - right)),
            static_cast<f32>((static_cast<f64>(top) - x) / (static_cast<f64>(top) - bottom))};
}

glm::dvec2 WorldMapArea::toWorld(glm::vec2 uv) const {
    return {static_cast<f64>(top) - static_cast<f64>(uv.y) * (static_cast<f64>(top) - bottom),
            static_cast<f64>(left) - static_cast<f64>(uv.x) * (static_cast<f64>(left) - right)};
}

glm::vec2 WorldMapContinent::toTaxiMap(f64 x, f64 y) const {
    return {static_cast<f32>((static_cast<f64>(taxiMaxY) - y) / (static_cast<f64>(taxiMaxY) - taxiMinY)),
            static_cast<f32>((static_cast<f64>(taxiMaxX) - x) / (static_cast<f64>(taxiMaxX) - taxiMinX))};
}

// Phase 15: the World map's size in yards at the continents' scale (WoW.exe's 1 / 33400 and 1 / 22266.7) and the
// pixels its offsets are counted in (16 of the 1002 x 668 map).
constexpr f64 kWorldMapYardsWide = 33400.0;
constexpr f64 kWorldMapYardsHigh = 22266.7;
constexpr f64 kWorldMapOffsetU = 16.0 / 1002.0;
constexpr f64 kWorldMapOffsetV = 16.0 / 668.0;
constexpr f64 kAdtSize = 533.33333;

glm::vec2 WorldMapContinent::toWorldMap(f64 x, f64 y) const {
    return {static_cast<f32>(offsetX * kWorldMapOffsetU + 0.5 - y * scale / kWorldMapYardsWide),
            static_cast<f32>(offsetY * kWorldMapOffsetV + 0.5 - x * scale / kWorldMapYardsHigh)};
}

glm::dvec2 WorldMapContinent::fromWorldMap(glm::vec2 uv) const {
    const f64 s = scale != 0.0f ? static_cast<f64>(scale) : 1.0;
    return {(offsetY * kWorldMapOffsetV + 0.5 - uv.y) * kWorldMapYardsHigh / s,
            (offsetX * kWorldMapOffsetU + 0.5 - uv.x) * kWorldMapYardsWide / s};
}

bool WorldMapContinent::onWorldMap(glm::vec2 uv) const {
    const glm::dvec2 p = fromWorldMap(uv);
    const f64 column = 32.0 - p.y / kAdtSize, row = 32.0 - p.x / kAdtSize;
    return column >= left && column < static_cast<f64>(right) + 1.0 && row >= top &&
           row < static_cast<f64>(bottom) + 1.0;
}

bool WorldMapData::init(const pack::PackReader& db) {
    m_areas.clear();
    m_overlays.clear();
    m_continents.clear();
    bool ok = true;
    if (const std::optional<db::DbcView> areas = table(db, "WorldMapArea", kAreaFieldCount)) {
        m_areas.reserve(areas->recordCount());
        for (u32 r = 0; r < areas->recordCount(); ++r) {
            WorldMapArea a;
            a.id = areas->getU32(r, 0);
            a.map = areas->getU32(r, 1);
            a.area = areas->getU32(r, 2);
            a.directory = areas->getString(r, 3);
            a.left = areas->getF32(r, 4);
            a.right = areas->getF32(r, 5);
            a.top = areas->getF32(r, 6);
            a.bottom = areas->getF32(r, 7);
            if (a.left != a.right && a.top != a.bottom) {
                m_areas.push_back(a);
            }
        }
    } else {
        ok = false;
    }
    if (const std::optional<db::DbcView> overlays = table(db, "WorldMapOverlay", overlay::kFieldCount)) {
        m_overlays.reserve(overlays->recordCount());
        for (u32 r = 0; r < overlays->recordCount(); ++r) {
            WorldMapOverlay o;
            o.id = overlays->getU32(r, 0);
            o.mapArea = overlays->getU32(r, overlay::kMapArea);
            for (u32 i = 0; i < 4; ++i) {
                o.areas[i] = overlays->getU32(r, overlay::kAreas + i);
            }
            o.texture = overlays->getString(r, overlay::kTexture);
            o.width = overlays->getU32(r, overlay::kWidth);
            o.height = overlays->getU32(r, overlay::kHeight);
            o.offsetX = overlays->getU32(r, overlay::kOffsetX);
            o.offsetY = overlays->getU32(r, overlay::kOffsetY);
            if (!o.texture.empty() && o.width > 0 && o.height > 0) {
                m_overlays.push_back(o);
            }
        }
        std::stable_sort(
            m_overlays.begin(), m_overlays.end(),
            [](const WorldMapOverlay& a, const WorldMapOverlay& b) { return a.mapArea < b.mapArea; });
    } else {
        ok = false;
    }
    if (const std::optional<db::DbcView> continents = table(db, "WorldMapContinent", kContinentFieldCount)) {
        for (u32 r = 0; r < continents->recordCount(); ++r) {
            WorldMapContinent c;
            c.id = continents->getU32(r, 0);
            c.map = continents->getU32(r, 1);
            c.left = continents->getU32(r, 2); // phase 15: the World map
            c.right = continents->getU32(r, 3);
            c.top = continents->getU32(r, 4);
            c.bottom = continents->getU32(r, 5);
            c.offsetX = continents->getF32(r, 6);
            c.offsetY = continents->getF32(r, 7);
            c.scale = continents->getF32(r, 8);
            c.taxiMinX = continents->getF32(r, 9);
            c.taxiMinY = continents->getF32(r, 10);
            c.taxiMaxX = continents->getF32(r, 11);
            c.taxiMaxY = continents->getF32(r, 12);
            m_continents.push_back(c);
        }
    } else {
        ok = false;
    }
    VD_LOG_INFO("World map: %zu maps, %zu overlays, %zu continents", m_areas.size(), m_overlays.size(),
                m_continents.size());
    return ok;
}

const WorldMapArea* WorldMapData::mapArea(u32 id) const {
    for (const WorldMapArea& a : m_areas) {
        if (a.id == id) {
            return &a;
        }
    }
    return nullptr;
}

const WorldMapArea* WorldMapData::zoneMap(u32 zoneId) const {
    if (zoneId == 0) {
        return nullptr;
    }
    for (const WorldMapArea& a : m_areas) {
        if (a.area == zoneId) {
            return &a;
        }
    }
    return nullptr;
}

const WorldMapArea* WorldMapData::continentMap(u32 map) const {
    for (const WorldMapArea& a : m_areas) {
        if (a.map == map && a.area == 0) {
            return &a;
        }
    }
    return nullptr;
}

const WorldMapContinent* WorldMapData::continent(u32 map) const {
    for (const WorldMapContinent& c : m_continents) {
        if (c.map == map) {
            return &c;
        }
    }
    return nullptr;
}

const WorldMapContinent* WorldMapData::continentAt(glm::vec2 worldMapUv) const {
    for (const WorldMapContinent& c : m_continents) {
        if (c.onWorldMap(worldMapUv)) {
            return &c;
        }
    }
    return nullptr;
}

std::span<const WorldMapOverlay> WorldMapData::overlays(u32 mapAreaId) const {
    const auto first = std::lower_bound(m_overlays.begin(), m_overlays.end(), mapAreaId,
                                        [](const WorldMapOverlay& o, u32 id) { return o.mapArea < id; });
    auto last = first;
    while (last != m_overlays.end() && last->mapArea == mapAreaId) {
        ++last;
    }
    return {m_overlays.data() + (first - m_overlays.begin()), static_cast<usize>(last - first)};
}

bool WorldMapData::zoneRect(const WorldMapArea& zone, glm::vec2& min, glm::vec2& max) const {
    const WorldMapArea* c = continentMap(zone.map);
    if (!c || zone.continent()) {
        return false;
    }
    // The zone's corners (north-west, south-east) on the continent map.
    min = c->toMap(zone.top, zone.left);
    max = c->toMap(zone.bottom, zone.right);
    return true;
}

const WorldMapArea* WorldMapData::zoneAt(u32 map, glm::vec2 uv) const {
    const WorldMapArea* best = nullptr;
    f32 bestArea = 0.0f;
    for (const WorldMapArea& a : m_areas) {
        glm::vec2 min, max;
        if (a.map != map || a.continent() || !zoneRect(a, min, max)) {
            continue;
        }
        if (uv.x < min.x || uv.y < min.y || uv.x > max.x || uv.y > max.y) {
            continue;
        }
        const f32 area = (max.x - min.x) * (max.y - min.y);
        if (!best || area < bestArea) {
            best = &a;
            bestArea = area;
        }
    }
    return best;
}

u64 WorldMapData::tileTexture(std::string_view directory, u32 index) {
    return worldMapHash(directory, directory, index);
}

u64 WorldMapData::overlayTexture(std::string_view directory, std::string_view texture, u32 index) {
    return worldMapHash(directory, texture, index);
}

} // namespace vd::game
