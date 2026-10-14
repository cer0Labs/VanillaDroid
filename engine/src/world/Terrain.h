#pragma once

#include "core/Clock.h"
#include "core/Result.h"
#include "core/Types.h"
#include "pack/PackReader.h"
#include "pack/TerrainFormat.h"
#include "render/Renderer.h"
#include "world/Camera.h"
#include "world/TerrainMapLoader.h"
#include "world/TerrainStreamer.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vd {

struct TerrainStats {
    u32 loadedTiles = 0;
    u32 visibleTiles = 0;
    u32 inFlight = 0;      // being read/decompressed on the IO thread
    u32 waitingUpload = 0; // decompressed, waiting for room in the upload ring
    u32 tilesStreamed = 0; // since the map was opened
    f32 lastIoMs = 0.0f;     // IO thread: read + LZ4 of the last tile
    f32 lastUploadMs = 0.0f; // game thread: GPU resources + staging copy of the last tile
    f32 worstUploadMs = 0.0f; // since resetWorst()
};

// Told when tiles come and go, with the raw tile blob (placements are read from it).
class TerrainListener {
public:
    // tile = ty * 64 + tx, or pack::kMapObjectsTile for the map's global WMO (phase 12: shown while the map is
    // open; the blob has only placements). `blob` is valid only during the call.
    virtual void onTileLoaded(u32 tile, const u8* blob, usize size) = 0;
    virtual void onTileUnloaded(u32 tile) = 0;

protected:
    ~TerrainListener() = default;
};

// Terrain of one map: streams tiles around the camera from world_<map>.vdpk and produces the
// terrain draw list. Game thread only (the IO work lives in TerrainStreamer).
class Terrain {
public:
    static constexpr u32 kMaxLoadedTiles = 64;
    static constexpr u64 kStagingBytes = pack::kMaxTerrainTileBytes;
    // Rough GPU cost of a loaded tile (buffer + 1024x1024 16-bit alpha), for budgeting requests.
    static constexpr u64 kTileGpuEstimate = 2800ull << 10;

    ~Terrain();
    // Closes the map and frees the staging memory. Call before destroying the renderer.
    void shutdown(Renderer& renderer);

    // `directory` as in Map.dbc ("Azeroth", "Kalimdor"). Phase 18 (world): returns at once. The map before goes over
    // the next frames (its tiles a few per frame); the new one's pack, tables and texture arrays are read on the
    // map loader's thread (world/TerrainMapLoader) and handed to the GPU without waiting, then its tiles stream as
    // before. isOpen() from now on (the map is this one), ready() once its textures are on their way to the GPU.
    Result<void> openMap(const std::filesystem::path& packsDir, std::string_view directory, Renderer& renderer);
    // At once (shutdown): waits for the map loader.
    void closeMap(Renderer& renderer);
    bool isOpen() const { return m_step != OpenStep::Closed; }
    bool ready() const { return m_step == OpenStep::Open; }
    // How far the opening went, 0..1 (1 once ready(); the tiles come after).
    f32 openProgress() const;
    const std::string& mapDirectory() const { return m_directory; }

    // Loads/unloads tiles for the camera position. Uploads at most a couple of tiles per frame.
    void update(const glm::dvec3& camera, f32 viewDistance, Renderer& renderer);
    // update() without the tiles: the map before still leaves and the new one still opens. For the glue screens,
    // whose scene draws no terrain but needs an open map (the renderer draws a scene only once its textures are set).
    void updateOpening(Renderer& renderer);
    // Loaded tiles inside the view distance and the frustum, nearest first. `out` is cleared; reserve
    // it once (kMaxLoadedTiles) and no allocation happens here.
    void collectDraws(const glm::dvec3& camera, const Frustum& frustum, f32 viewDistance,
                      std::vector<TerrainDraw>& out);
    // Distant horizon tiles (WDL) up to `distance` that collectDraws() did not draw as terrain this frame.
    void collectHorizon(const glm::dvec3& camera, const Frustum& frustum, f32 distance, std::vector<HorizonDraw>& out);
    // Height of the terrain surface at world (x, y), if that tile is loaded.
    std::optional<f32> heightAt(f64 x, f64 y) const;
    // AreaTable.dbc id of the terrain chunk at world (x, y) (a zone or one of its subzones), 0 when that tile is
    // not loaded or the chunk has none. Inside WMOs the client also uses WMOAreaTable; this is the terrain's.
    u32 areaAt(f64 x, f64 y) const;
    // Phase 14 (grass): the kTileVertices absolute heights of a loaded tile (chunk by chunk, MCVT order), null when it
    // is not loaded. Valid until the next update().
    const f32* tileHeights(u32 tx, u32 ty) const;

    const TerrainStats& stats() const { return m_stats; }
    // GPU memory the terrain may use (tiles + texture array); tiles beyond it are not loaded, and the
    // furthest ones are dropped if the budget shrinks (low-memory warning).
    static constexpr u64 kDefaultGpuBudget = 320ull << 20;
    void setGpuBudget(u64 bytes) { m_gpuBudget = bytes; }
    void setListener(TerrainListener* listener) { m_listener = listener; }
    // Distance (yards, to the tile's square) beyond which each LOD level switches to the next one.
    std::array<f32, pack::kLodLevels - 1>& lodDistances() { return m_lodDistances; }
    u64 gpuBudget() const { return m_gpuBudget; }
    void resetWorst() { m_stats.worstUploadMs = 0.0f; }

    // Tile containing world (x, y); false outside the map.
    static bool tileAt(f64 x, f64 y, u32& tx, u32& ty);

private:
    enum class TileState : u8 { Absent, Unloaded, Requested, Loaded };

    struct LoadedTile {
        u16 tx = 0;
        u16 ty = 0;
        TerrainTileHandle handle;
        f32 minZ = 0.0f;
        f32 maxZ = 0.0f;
        std::vector<f32> heights; // kTileVertices, for height queries
        std::array<u32, pack::kTileChunks> areas{}; // AreaTable id of each chunk
        bool used = false;
        bool closing = false; // phase 18 (world): the map before's, waiting in m_unloadQueue
    };
    // Phase 18 (world): where opening a map stands.
    enum class OpenStep : u8 { Closed, Preparing, Filling, Open };

    enum class Upload : u8 { Done, Retry, Failed };

    std::optional<pack::PackAsset> findTile(u32 tx, u32 ty) const;
    Upload uploadTile(const TerrainStreamer::Done& done, Renderer& renderer);
    void releaseFinishedSlots(const Renderer& renderer);
    void unloadTile(u32 slot, Renderer& renderer);
    // Phase 18 (world): the map before leaves (its tiles queued for unloading, its GPU data retired), and the steps
    // of the new one as the map loader finishes them.
    void beginClose(Renderer& renderer);
    void unloadClosing(Renderer& renderer, bool all);
    void advanceOpening(Renderer& renderer);
    void startOpening(Renderer& renderer);

    pack::PackReader m_pack;
    std::string m_directory;
    // Phase 18 (world): opening a map off the game thread.
    TerrainMapLoader m_loader;
    OpenStep m_step = OpenStep::Closed;
    std::filesystem::path m_packsDir;
    std::string m_queued;              // a map asked for while the loader still worked for another
    std::vector<u32> m_unloadQueue;    // slots of the map before (kMapObjectsSlot: its global WMO)
    static constexpr u32 kMapObjectsSlot = ~0u;
    StagingBlock m_mapStaging;         // the texture arrays, filled by the loader
    u64 m_residentLiquids = 0;         // TerrainMapLoader::Map::liquidKey of the liquid frames on the GPU
    u64 m_slotsBusyUntil = 0;          // the streamer's staging slots are copied by the GPU until this frame completes
    Clock::time_point m_openStart{};
    f32 m_openWorstMs = 0.0f;          // the longest the game thread spent on the opening in one frame
    const pack::TerrainMapHeader* m_header = nullptr;
    const pack::TerrainMapTile* m_tileInfo = nullptr; // 64 * 64, in the mapped pack
    std::array<TileState, pack::kMapTiles * pack::kMapTiles> m_state{};
    std::array<u8, pack::kMapTiles * pack::kMapTiles> m_slotOf{}; // LoadedTile index when Loaded
    std::vector<LoadedTile> m_loaded;
    std::vector<TerrainStreamer::Done> m_waitingUpload;
    struct PendingSlot {
        u32 slot;
        u64 frame; // the GPU copy is recorded in this frame; the slot is free once it completes
    };
    std::vector<PendingSlot> m_pendingSlots;
    std::array<StagingBlock, TerrainStreamer::kSlots> m_staging{};
    u64 m_gpuBudget = kDefaultGpuBudget;
    TerrainListener* m_listener = nullptr;
    std::array<f32, pack::kLodLevels - 1> m_lodDistances{120.0f, 300.0f, 600.0f};
    std::vector<f32> m_drawDistances; // scratch for sorting draws
    std::array<bool, pack::kMapTiles * pack::kMapTiles> m_drawn{}; // drawn as terrain this frame
    std::vector<u8> m_horizon;                                      // decompressed horizon blob
    const u16* m_horizonSlots = nullptr;
    std::optional<pack::PackAsset> m_mapObjects; // phase 12: the WDT's global WMO, if the map places one
    bool m_mapObjectsShown = false;              // handed to the listener (as tile kMapObjectsTile)
    TerrainStreamer m_streamer;
    TerrainStats m_stats;
};

} // namespace vd
