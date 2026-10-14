#pragma once

#include "core/Types.h"
#include "pack/PackFormat.h"
#include "pack/PackReader.h"
#include "pack/TerrainFormat.h"

#include <atomic>
#include <filesystem>
#include <optional>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

namespace vd {

// Phase 18 (world): the heavy part of opening a map, on a thread of its own (vd-map), so that the game thread keeps
// drawing the loading screen. Two steps, each started by the game thread and polled by it once per frame:
//   prepare(): opens world_<map>.vdpk (liquids.vdpk once, for the maps without their own liquid frames), checks the
//              map table, decompresses the horizon, finds the map's global WMO, and lays out the textures the GPU needs:
//              the terrain layer array and the liquid frames unless the ones on the GPU are these (liquidKey); RGBA8
//              where the GPU has no ASTC (desktop).
//   fill():    writes those textures into the staging block the game thread allocated (stagingBytes): a copy out of
//              the mapped pack (page faults = flash reads, here), or the software ASTC decode on the desktop.
// The game thread then creates the GPU objects from the block without waiting (Renderer::setTerrainTextures).
//
// Threading: every public function is the game thread's. The loader owns `Map` while a step runs; the game thread
// reads it only after done() and keeps `Map::pack` open until fill() has finished (it reads the textures from there).
class TerrainMapLoader {
public:
    struct Map {
        std::string directory;
        std::string error; // a failed prepare(): why
        pack::PackReader pack;
        const pack::TerrainMapHeader* header = nullptr; // in the mapped pack
        const pack::TerrainMapTile* tiles = nullptr;    // 64 * 64, in the mapped pack
        const u16* sharedIndices = nullptr;             // header->sharedIndexCount, in the mapped pack
        std::vector<u8> horizon;                        // the decompressed horizon blob, empty without one
        std::optional<pack::PackAsset> objects;         // phase 12: the map's global WMO placements
        // What fill() stages: the layer array at terrainOffset, then (newLiquids) the liquid frames at liquidOffset.
        pack::TextureHeader terrain{};
        u64 terrainOffset = 0;
        bool newLiquids = false;
        pack::TextureHeader liquids{};
        u64 liquidOffset = 0;
        u64 liquidKey = 0; // identifies the liquid frames' content (the same in every pack today)
        u64 stagingBytes = 0;
        f32 prepareMs = 0.0f;
        f32 fillMs = 0.0f;
    };

    TerrainMapLoader();
    ~TerrainMapLoader();
    TerrainMapLoader(const TerrainMapLoader&) = delete;
    TerrainMapLoader& operator=(const TerrainMapLoader&) = delete;

    // True while a step runs (its result not taken yet).
    bool busy() const { return m_job != Job::None; }
    bool done() const { return m_finished.load(std::memory_order_acquire); }

    // Step 1. `residentLiquids`: the liquidKey of the liquid frames on the GPU (0: none). `map` is cleared first.
    void prepare(const std::filesystem::path& packsDir, std::string_view directory, bool nativeAstc,
                 u64 residentLiquids);
    // Step 2, after prepare() is done and succeeded: `staging` holds map().stagingBytes.
    void fill(u8* staging);
    // The step's result, once done(): the game thread may read and move from it; finish() hands the loader back.
    Map& map() { return m_map; }
    bool succeeded() const { return m_ok; }
    void finish();
    // fill()'s progress, 0..1 (any time).
    f32 fillProgress() const;

private:
    enum class Job : u8 { None, Prepare, Fill };
    void run();
    bool doPrepare();
    bool doFill();

    std::thread m_thread;
    std::counting_semaphore<4> m_wake{0};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_finished{false};
    Job m_job = Job::None; // the game thread's; the loader reads it after the wake
    bool m_ok = false;
    std::filesystem::path m_packsDir;
    bool m_nativeAstc = true;
    u64 m_residentLiquids = 0;
    u8* m_staging = nullptr;
    Map m_map;
    // The loader's own: liquids.vdpk, opened by the first map without its own liquid frames and kept, and the sources
    // fill() copies from.
    pack::PackReader m_liquidPack;
    pack::PackAsset m_terrainSource;
    pack::PackAsset m_liquidSource;
    std::atomic<u32> m_units{0};
    u32 m_totalUnits = 1;
};

} // namespace vd
