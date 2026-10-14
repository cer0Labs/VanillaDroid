#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "pack/GrassFormat.h"
#include "pack/PackReader.h"
#include "pack/TerrainFormat.h"
#include "render/Renderer.h"
#include "world/AssetStreamer.h"
#include "world/Camera.h"
#include "world/GrassPlacement.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vd {

class Terrain;

struct GrassStats {
    u32 cachedChunks = 0;  // placed and kept
    u32 visibleChunks = 0; // drawn this frame
    u32 instances = 0;     // drawn this frame
    u32 draws = 0;
    u32 generated = 0;     // chunks placed this frame
    u32 tiles = 0;         // tile records in memory
    u32 pending = 0;       // reads in flight
    u32 frillDensity = 0;  // in use
    f32 collectMs = 0.0f;  // this frame, placing included
};

// Phase 14 (grass): the terrain's detail doodads, the 1.12 client's ground effects (grass, flowers, small bushes,
// pebbles) from grass.vdpk (pack/GrassFormat.h). The records of the tiles near the camera are read on an IO thread
// ("vd-grass"), the doodads of each chunk placed by WoW.exe's own generator (world/GrassPlacement.h) when the chunk
// comes near (a few chunks a frame, ~10 us each), kept in a fixed pool, and drawn as one instanced draw per model
// (they share a handful of textures). Like the client, chunks whose box reaches nearer than 70 yards of view depth
// carry them, and the shader fades them out from 52.5 to 70 yards of depth. Game thread only; nothing is allocated
// per frame once the pack is open.
//
// Development switches: VD_GRASS=0 (none), VD_GRASS_DENSITY=<frillDensity 1..256> (the CVar; 16 in 1.12),
// VD_GRASS_DISTANCE=<yards> (where the fade ends; it starts at 3/4 of that), VD_TRACE_GRASS=1 (a "GRASS" line in the
// log every 300 frames: the most doodads, draws and triangles of a frame, the CPU it took, the chunks placed).
class GroundEffects {
public:
    static constexpr u32 kMaxChunks = 160;      // placed chunks kept (a 105-yard circle holds ~50)
    static constexpr u32 kMaxRuns = 16;         // models per chunk: 4 layers x 4 slots at most
    static constexpr u32 kMaxTiles = 16;        // tile records kept (each 10-140 KB)
    static constexpr u32 kMaxPlacedPerFrame = 6;
    // Textures created per frame: they all arrive together after the models (a few dozen; on the desktop each one is
    // decoded from ASTC on the CPU), the rest wait in the streamer for the next frames.
    static constexpr u32 kMaxTexturesPerFrame = 2;
    static constexpr u64 kStagingBytes = 4ull << 20;
    static constexpr u32 kMaxInstances = 16384; // drawn per frame (the renderer's ring)

    ~GroundEffects();

    // Opens <packsDir>/grass.vdpk (missing: logged once, nothing drawn) and asks for the models and their textures.
    // `textures` must stay open while this is.
    Result<void> open(const std::filesystem::path& packsDir, const pack::PackReader& textures, Renderer& renderer);
    void close(Renderer& renderer);
    bool isOpen() const { return m_open; }

    // The terrain's map (Map.dbc directory): another one forgets every tile and chunk.
    void setMap(std::string_view directory);
    // Takes what the IO thread read: the models (uploaded once), their textures, tile records.
    void update(Renderer& renderer);
    // Places the chunks coming near and lists the visible doodads by model, camera-relative. `forward`: the camera's
    // unit view direction (depth). `draws` and `instances` are cleared; reserve them once (kMaxInstances).
    void collect(const glm::dvec3& camera, const glm::vec3& forward, const Frustum& frustum, const Terrain& terrain,
                 std::vector<GrassDraw>& draws, std::vector<GrassInstance>& instances);

    // The player's "Ground Clutter" (0 off, 1 half, 2 the 1.12 density, 3 double) as the client's frillDensity.
    static u32 frillDensityForLevel(u32 level);
    // 0: none. Changing it places every chunk again (the client's generator draws other cells).
    void setFrillDensity(u32 frillDensity);
    // The dynamic quality's share of the distances (0.6..1): the fade comes nearer.
    f32 distanceScale = 1.0f;
    f32 fadeStart() const;
    f32 fadeEnd() const;
    bool enabled() const { return m_enabled; }

    const GrassStats& stats() const { return m_stats; }

private:
    enum class TileState : u8 { Unknown, Requested, Loaded, Absent };
    struct TileRecord {
        u16 tx = 0;
        u16 ty = 0;
        std::unique_ptr<u8[]> data;
        u32 size = 0;
        const pack::GrassChunk* chunks = nullptr;
        const u8* shadows = nullptr;
        u32 shadowCount = 0;
        u64 lastUsed = 0;
    };
    struct Run {
        u16 model = 0;
        u16 count = 0;
        u32 first = 0; // into m_chunkInstances
    };
    struct ChunkSlot {
        i32 key = -1; // global chunk (row * 1024 + column), -1 = free
        u32 runCount = 0;
        u32 instanceCount = 0;
        f64 cornerX = 0.0; // north-west corner (world X, Y)
        f64 cornerY = 0.0;
        f32 minZ = 0.0f;
        f32 maxZ = 0.0f;
        u64 lastUsed = 0;
    };
    struct TextureEntry {
        u64 hash = 0;
        TextureHandle handle;
        bool requested = false;
    };
    struct Candidate {
        f32 distance;
        i32 key;
    };

    void onModels(const AssetStreamer::Done& done, Renderer& renderer);
    void onTile(u32 tile, AssetStreamer::Done& done);
    TileRecord* tileRecord(u32 tx, u32 ty);
    void requestTile(u32 tx, u32 ty);
    void clearChunks();
    void clearTiles();
    static u32 gridIndex(i32 key);
    u32 findChunk(i32 key) const; // kNoSlot when not placed
    u32 freeChunkSlot();          // a free (or freed) slot, kNoSlot when every one is in reach
    bool placeChunk(i32 key, const TileRecord& tile, const f32* tileHeights, u32 slot);
    bool chunkVisible(const ChunkSlot& chunk, const glm::dvec3& camera, const glm::vec3& forward,
                      const Frustum& frustum, f32 fadeEnd) const;
    void gather(const glm::dvec3& camera, const glm::vec3& forward, f32 cutDepth, std::vector<GrassDraw>& draws,
                std::vector<GrassInstance>& instances);

    bool m_open = false;
    bool m_enabled = true;
    u32 m_frillDensity = grass::kDefaultFrillDensity;
    u32 m_densityOverride = 0; // VD_GRASS_DENSITY
    f32 m_distanceOverride = 0.0f; // VD_GRASS_DISTANCE

    pack::PackReader m_pack;
    const pack::PackReader* m_textures = nullptr;
    StagingBlock m_staging;
    AssetStreamer m_streamer;
    bool m_streamerStarted = false;

    // The models and the ground effect table (grass/models).
    bool m_modelsRequested = false;
    bool m_modelsReady = false;
    std::vector<pack::GrassModel> m_models;
    std::vector<pack::GrassEffect> m_effects;
    std::vector<u32> m_modelTexture; // per model: index into m_textureEntries
    std::vector<TextureEntry> m_textureEntries;
    u32 m_maxDensity = 20;

    // The map's tiles.
    std::string m_map; // lowercase
    u32 m_mapGeneration = 1; // in the tile reads' tags: a read for another map is dropped
    std::array<TileState, pack::kMapTiles * pack::kMapTiles> m_tileState{};
    std::array<u8, pack::kMapTiles * pack::kMapTiles> m_tileSlot{};
    std::array<TileRecord, kMaxTiles> m_tiles{};

    // The placed chunks: kMaxChunks slots of m_chunkCapacity instances each, positions relative to their corner.
    std::vector<ChunkSlot> m_chunks;
    // Which slot holds a chunk, by its global indices modulo kGridSide (the reach spans ~11 chunks: two chunks in
    // reach never share a cell; a far one sharing it is simply forgotten).
    static constexpr u32 kGridSide = 64;
    static constexpr u16 kNoSlot = 0xFFFF;
    std::array<u16, kGridSide * kGridSide> m_chunkGrid{};
    std::vector<Run> m_runs; // kMaxRuns per slot
    std::vector<GrassInstance> m_chunkInstances;
    u32 m_chunkCapacity = 0;
    u32 m_capacityDensity = 0; // the frillDensity m_chunkCapacity was sized for

    // Per-frame scratch, reserved at open().
    std::vector<grass::PlacedGrass> m_placed;
    std::vector<Candidate> m_candidates;
    std::vector<u32> m_visible;     // chunk slots drawn this frame
    struct ModelRange {
        u32 first = 0; // its instances in the frame's array
        u32 end = 0;
        u32 cursor = 0;
    };
    std::vector<u32> m_modelBound; // per model: instances of the visible chunks (zero between frames)
    std::vector<ModelRange> m_modelRanges;
    std::vector<u16> m_touched;     // models with instances this frame
    u64 m_frame = 0;
    GrassStats m_stats;
    // VD_TRACE_GRASS: what the last 300 frames drew at most.
    struct Trace {
        u32 frames = 0;
        u32 instances = 0;
        u32 draws = 0;
        u32 triangles = 0;
        u32 chunks = 0;
        u32 placed = 0;
        f64 collectMs = 0.0;
        f32 collectMaxMs = 0.0f;
    };
    bool m_trace = false;
    Trace m_traced;
    void trace(std::span<const GrassDraw> draws);
};

} // namespace vd
