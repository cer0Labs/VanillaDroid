#pragma once

#include "core/Types.h"

#include <glm/vec2.hpp>

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

// One WorldMapArea.dbc row: a zone map (or a continent's, area 0) and the world rectangle it shows. The map
// art is Interface\WorldMap\<directory>\<directory>1..12.blp, 4 x 3 tiles of 256 x 256 pixels of which the
// client shows the top-left 1002 x 668 (WorldMapDetailFrame).
struct WorldMapArea {
    u32 id = 0;
    u32 map = 0;  // Map.dbc
    u32 area = 0; // AreaTable zone (0: the whole continent)
    std::string_view directory;
    // World bounds: left / right are world Y (west / east edges), top / bottom world X (north / south).
    f32 left = 0.0f, right = 0.0f, top = 0.0f, bottom = 0.0f;

    bool continent() const { return area == 0; }
    // A world point on this map: 0..1 from the west edge to the east one, 0..1 from north to south.
    glm::vec2 toMap(f64 x, f64 y) const;
    // The world point (x, y) at map coordinates `uv`.
    glm::dvec2 toWorld(glm::vec2 uv) const;
};

// One WorldMapOverlay.dbc row: art that uncovers part of a zone map once the player explored one of its
// areas. Interface\WorldMap\<directory>\<texture><n>.blp, cut in 256-pixel pieces, at (offsetX, offsetY) of
// the 1002 x 668 detail map.
struct WorldMapOverlay {
    u32 id = 0;
    u32 mapArea = 0;            // WorldMapArea.dbc
    std::array<u32, 4> areas{}; // AreaTable ids that uncover it (0: unused)
    std::string_view texture;   // "GOLDSHIRE"
    u32 width = 0, height = 0;  // pixels of the detail map
    u32 offsetX = 0, offsetY = 0;
};

// One WorldMapContinent.dbc row: the taxi map's world bounds of a continent, and (phase 15) where it lies on the World
// map (Interface\WorldMap\World: both continents, the map above them).
struct WorldMapContinent {
    u32 id = 0;
    u32 map = 0;
    // Phase 15: its ADT tiles (WDT columns left..right, rows top..bottom) and its place on the World map.
    u32 left = 0, right = 0, top = 0, bottom = 0;
    f32 offsetX = 0.0f, offsetY = 0.0f, scale = 1.0f;
    // World rectangle the taxi map (Interface\TaxiFrame\TAXIMAP<map>.blp) shows: x from taxiMinX (south) to
    // taxiMaxX (north), y from taxiMinY (east) to taxiMaxY (west).
    f32 taxiMinX = 0.0f, taxiMinY = 0.0f, taxiMaxX = 0.0f, taxiMaxY = 0.0f;

    // A world point on the taxi map: 0..1 west to east, 0..1 north to south.
    glm::vec2 toTaxiMap(f64 x, f64 y) const;
    // Phase 15: a world point of this continent on the World map (0..1 west to east, north to south). WoW.exe
    // (GetPlayerMapPosition at the world's zoom): u = offsetX * 16 / 1002 + 0.5 - y * scale / 33400, v = offsetY *
    // 16 / 668 + 0.5 - x * scale / 22266.7 — the World map is 33400 x 22266.7 yards seen at the continents' scale.
    glm::vec2 toWorldMap(f64 x, f64 y) const;
    glm::dvec2 fromWorldMap(glm::vec2 uv) const;
    // A point of the World map falls on this continent's ADT tiles (where a tap opens its map).
    bool onWorldMap(glm::vec2 uv) const;
};

// The world map's tables (phase 8): zone and continent maps, their exploration overlays and the taxi maps'
// bounds. Indexes only; names are views into the mapped db pack.
class WorldMapData {
public:
    static constexpr f32 kMapWidth = 1002.0f; // pixels of the detail map (WorldMapDetailFrame)
    static constexpr f32 kMapHeight = 668.0f;
    static constexpr u32 kTileColumns = 4;
    static constexpr u32 kTileRows = 3;
    static constexpr f32 kTileSize = 256.0f;

    bool init(const pack::PackReader& db);
    bool ready() const { return !m_areas.empty(); }

    std::span<const WorldMapArea> areas() const { return m_areas; }
    const WorldMapArea* mapArea(u32 id) const;
    // The zone map of an AreaTable zone (its id, not a subzone's), nullptr if it has none.
    const WorldMapArea* zoneMap(u32 zoneId) const;
    // The continent map of Map.dbc `map` (area 0), nullptr for maps without one (instances).
    const WorldMapArea* continentMap(u32 map) const;
    const WorldMapContinent* continent(u32 map) const;
    // Phase 15: the World map (zoom 0: both continents) — its id among the map areas the screens show, its art
    // directory, the continent under a point of it (nullptr: the sea).
    static constexpr u32 kWorldMap = 0xFFFFFFFFu;
    static constexpr std::string_view kWorldDirectory = "World";
    const WorldMapContinent* continentAt(glm::vec2 worldMapUv) const;
    std::span<const WorldMapOverlay> overlays(u32 mapAreaId) const;
    // Rectangle of a zone map on its continent's map, in the continent map's 0..1 coordinates (min, max).
    bool zoneRect(const WorldMapArea& zone, glm::vec2& min, glm::vec2& max) const;
    // The zone map under `uv` of a continent map (the smallest whose rectangle holds it), nullptr if none.
    const WorldMapArea* zoneAt(u32 map, glm::vec2 uv) const;

    // Texture of tile `index` (1..12, row-major) of a map: Interface\WorldMap\<dir>\<dir><index>.blp.
    static u64 tileTexture(std::string_view directory, u32 index);
    // Texture of piece `index` (1.., row-major) of an overlay.
    static u64 overlayTexture(std::string_view directory, std::string_view texture, u32 index);

private:
    std::vector<WorldMapArea> m_areas;
    std::vector<WorldMapOverlay> m_overlays; // sorted by map area
    std::vector<WorldMapContinent> m_continents;
};

} // namespace vd::game
