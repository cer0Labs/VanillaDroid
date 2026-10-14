#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "pack/AsyncReader.h"
#include "pack/CollisionFormat.h"
#include "pack/PackReader.h"
#include "physics/Collision.h"

#include <glm/vec3.hpp>

#include <array>
#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace vd::physics {

// A liquid surface found over a point.
struct LiquidSample {
    f32 level = 0.0f; // world Z of the surface
    u8 kind = 0;      // pack::LiquidKind
};

struct CollisionStats {
    u32 residentTiles = 0;
    u32 loadingTiles = 0;
    u64 residentBytes = 0;
    u32 tilesLoaded = 0; // since the map was opened
};

// What a map collides with (phase 6): the collision tiles of collision_<map>.vdpk around a focus point, loaded
// on an IO thread and used in place, and the queries the character controller and the camera need. Terrain
// triangles are rebuilt from the tile's height grid only where a query looks. Game thread only.
class CollisionWorld {
public:
    CollisionWorld();
    ~CollisionWorld();
    CollisionWorld(const CollisionWorld&) = delete;
    CollisionWorld& operator=(const CollisionWorld&) = delete;

    Result<void> openMap(const std::filesystem::path& packsDir, std::string_view directory);
    // Phase 18 (world): the same work (the pack and its tile index) on a thread of its own: returns at once. isOpen()
    // from now on; ready() and the tiles once update() has taken the index (a frame or two later).
    void beginOpenMap(const std::filesystem::path& packsDir, std::string_view directory);
    void closeMap();
    bool isOpen() const { return m_open; }
    const std::string& mapDirectory() const { return m_directory; }

    // Keeps the tiles within `radius` yards of `focus` loaded (requests go to the IO thread, finished loads are
    // taken here) and drops the ones well outside.
    void update(const glm::dvec3& focus, f32 radius = 96.0f);
    // Every tile overlapping the square of `radius` around (x, y) is loaded, or has no collision data.
    bool ready(f64 x, f64 y, f32 radius = 0.0f) const;
    // Tests and tools: a tile blob (CollisionTileHeader) added directly, without a pack or streaming.
    Result<void> addTile(std::vector<u8> blob);

    // Terrain and object triangles overlapping the world box [min, max] into `out` (relative to its origin,
    // which the caller sets with reset()). Loaded tiles only.
    void gather(const glm::dvec3& min, const glm::dvec3& max, TriangleSet& out) const;
    // Highest surface crossed by the vertical segment from `top` down to `bottom` at (x, y).
    std::optional<f64> groundHeight(f64 x, f64 y, f64 top, f64 bottom) const;
    // Phase 18 (world): the highest terrain surface (its holes left out, no objects) crossed by the same segment: where
    // 1.12's camera finds the ground before a building's floor (WoW.exe 0x6821f0).
    std::optional<f64> terrainHeight(f64 x, f64 y, f64 top, f64 bottom) const;
    // Highest liquid surface over (x, y) between `bottom` and `top`.
    std::optional<LiquidSample> liquidAt(f64 x, f64 y, f64 top, f64 bottom) const;

    const CollisionStats& stats() const { return m_stats; }

    // Tile containing world (x, y); false outside the map grid.
    static bool tileAt(f64 x, f64 y, i32& tx, i32& ty);

    // --- Phase 8: collision that moves (transports) ---
    // A model-space collision blob (pack/ObjectCollision.h, validated like a tile) placed by a position and a
    // rotation about Z, moved every frame by its owner. Its triangles come out of gather() (and so out of the
    // character's and the camera's queries) in world space, flagged kTriangleMoving. They outlive map changes.
    static constexpr u32 kNoObject = ~0u;
    u32 addObject(std::vector<u8> blob); // kNoObject when invalid
    void placeObject(u32 object, const glm::dvec3& position, f32 orientation);
    void removeObject(u32 object);
    u32 objectCount() const { return m_objectCount; }
    // The moving object whose surface is the highest the vertical segment from `top` down to `bottom` at (x, y)
    // crosses (kNoObject: none), and that surface's height.
    u32 objectBelow(f64 x, f64 y, f64 top, f64 bottom, f64* height = nullptr) const;

private:
    struct Tile {
        u8* data = nullptr; // owned
        u32 size = 0;
        const pack::CollisionTileHeader* header = nullptr;
        const u16* heights = nullptr;
        const f32* heightBases = nullptr;
        const u16* holes = nullptr;
        const f32* vertices = nullptr;
        const u32* indices = nullptr;
        const u8* flags = nullptr;
        const pack::CollisionNode* nodes = nullptr;
        const f32* liquidVertices = nullptr;
        const u32* liquidIndices = nullptr;
        const u8* liquidKinds = nullptr;
        const pack::CollisionNode* liquidNodes = nullptr;
    };
    enum class State : u8 { Absent, Unloaded, Requested, Loaded };

    static bool bind(Tile& tile, u8* data, u32 size);
    void unload(u32 index);
    void gatherTerrain(const Tile& tile, u32 tx, u32 ty, const glm::dvec3& min, const glm::dvec3& max,
                       TriangleSet& out) const;
    void gatherObjects(const Tile& tile, const glm::dvec3& min, const glm::dvec3& max, TriangleSet& out) const;

    struct MovingObject {
        Tile tile;
        bool alive = false;
        bool placed = false;
        glm::dvec3 position{0.0};
        f32 cos = 1.0f, sin = 0.0f;
        glm::dvec3 worldMin{0.0}, worldMax{0.0}; // its bounds where it is now
    };
    void gatherObject(const MovingObject& object, const glm::dvec3& min, const glm::dvec3& max,
                      TriangleSet& out) const;

    // Phase 18 (world): the index of a map's tiles (openMap() on the game thread, beginOpenMap() on m_indexer).
    static Result<u32> indexMap(const std::filesystem::path& packsDir, std::string_view directory,
                                pack::PackReader& pack, std::vector<pack::PackAsset>& assets);
    void takeIndex(u32 tiles);
    std::thread m_indexer;
    std::atomic<bool> m_indexed{false};
    bool m_indexing = false;
    pack::PackReader m_indexPack;                // the indexer's until taken
    std::vector<pack::PackAsset> m_indexAssets;  // idem
    Result<u32> m_indexResult{0u};

    pack::PackReader m_pack;
    pack::AsyncReader m_reader;
    std::vector<MovingObject> m_objects; // phase 8; slots reused (alive = false: free)
    u32 m_objectCount = 0;
    bool m_open = false;
    std::string m_directory;
    std::array<State, pack::kMapTiles * pack::kMapTiles> m_state{};
    std::vector<pack::PackAsset> m_assets; // per tile (Absent: none)
    std::vector<Tile> m_tiles;             // per tile, bound while Loaded
    std::vector<u32> m_loaded;             // indices of the loaded tiles
    mutable TriangleSet m_scratch{1024};   // groundHeight()
    CollisionStats m_stats;
};

} // namespace vd::physics
