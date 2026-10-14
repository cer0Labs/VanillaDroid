#include "world/Terrain.h"

#include "core/Clock.h"
#include "core/Log.h"
#include "core/Profile.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <thread>

namespace vd {
namespace {

constexpr u32 kMaxUploadsPerFrame = 2;
constexpr f32 kUnloadHysteresis = pack::kTileSize * 0.5f; // no load/unload ping-pong at the edge

u32 tileIndex(u32 tx, u32 ty) {
    return ty * pack::kMapTiles + tx;
}

// 2D distance from (x, y) to the tile's square.
f64 distanceToTile(f64 x, f64 y, u32 tx, u32 ty) {
    const f64 maxX = pack::tileCornerX(ty);
    const f64 maxY = pack::tileCornerY(tx);
    const f64 minX = maxX - pack::kTileSize;
    const f64 minY = maxY - pack::kTileSize;
    const f64 dx = std::max({0.0, minX - x, x - maxX});
    const f64 dy = std::max({0.0, minY - y, y - maxY});
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

Terrain::~Terrain() {
    m_streamer.stop();
}

void Terrain::shutdown(Renderer& renderer) {
    closeMap(renderer);
    for (StagingBlock& block : m_staging) {
        renderer.destroyStagingBlock(block);
    }
    if (m_mapStaging.valid()) {
        renderer.destroyStagingBlock(m_mapStaging);
    }
}

bool Terrain::tileAt(f64 x, f64 y, u32& tx, u32& ty) {
    const f64 fx = 32.0 - y / pack::kTileSize;
    const f64 fy = 32.0 - x / pack::kTileSize;
    if (fx < 0.0 || fy < 0.0 || fx >= pack::kMapTiles || fy >= pack::kMapTiles) {
        return false;
    }
    tx = static_cast<u32>(fx);
    ty = static_cast<u32>(fy);
    return true;
}

Result<void> Terrain::openMap(const std::filesystem::path& packsDir, std::string_view directory, Renderer& renderer) {
    if (m_step != OpenStep::Closed && directory == m_directory) {
        m_queued.clear(); // back to the map being opened before another was asked for
        return {};
    }
    m_packsDir = packsDir;
    if (m_loader.busy()) {
        // The loader still works for the map asked for before (and may be reading its pack): this one starts as
        // soon as it is free (update()).
        m_queued.assign(directory);
        return {};
    }
    m_queued.clear();
    VD_PROFILE_ZONE("Terrain::openMap");
    const Clock::time_point start = Clock::now();
    beginClose(renderer);
    m_directory.assign(directory);
    startOpening(renderer);
    m_openWorstMs = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
    return {};
}

void Terrain::startOpening(Renderer& renderer) {
    m_step = OpenStep::Preparing;
    m_openStart = Clock::now();
    m_loader.prepare(m_packsDir, m_directory, renderer.nativeAstc(), m_residentLiquids);
}

f32 Terrain::openProgress() const {
    switch (m_step) {
    case OpenStep::Closed: return 0.0f;
    case OpenStep::Preparing: return 0.1f;
    case OpenStep::Filling: return 0.2f + 0.8f * m_loader.fillProgress();
    case OpenStep::Open: return 1.0f;
    }
    return 0.0f;
}

void Terrain::beginClose(Renderer& renderer) {
    m_streamer.stop(); // the IO thread reads from the pack
    for (u32 slot = 0; slot < m_loaded.size(); ++slot) {
        if (m_loaded[slot].used && !m_loaded[slot].closing) {
            m_loaded[slot].closing = true;
            m_unloadQueue.push_back(slot);
        }
    }
    if (m_mapObjectsShown) {
        m_unloadQueue.push_back(kMapObjectsSlot);
    }
    m_mapObjects.reset();
    m_mapObjectsShown = false;
    if (m_step != OpenStep::Closed) {
        renderer.clearTerrainMap(); // retired: freed once the frames in flight are done
    }
    // Tiles decompressed but not uploaded are dropped; the slots whose copy the GPU still has to make stay busy until
    // that frame completes (the next map's streamer starts no sooner).
    m_waitingUpload.clear();
    for (const PendingSlot& pending : m_pendingSlots) {
        m_slotsBusyUntil = std::max(m_slotsBusyUntil, pending.frame);
    }
    m_pendingSlots.clear();
    if (m_mapStaging.valid()) {
        renderer.retireStagingBlock(m_mapStaging);
    }
    m_pack.close();
    m_horizonSlots = nullptr;
    m_horizon.clear();
    m_header = nullptr;
    m_tileInfo = nullptr;
    m_directory.clear();
    m_state.fill(TileState::Absent);
    m_stats = {};
    m_step = OpenStep::Closed;
}

void Terrain::unloadClosing(Renderer& renderer, bool all) {
    if (m_unloadQueue.empty()) {
        return;
    }
    VD_PROFILE_ZONE("Terrain::unloadClosing");
    // The objects of a tile go with it (WorldObjects): ~1.5 ms a tile of a city on the desktop. A few per frame, the
    // first one always.
    constexpr f64 kBudgetMs = 2.0;
    const Clock::time_point start = Clock::now();
    while (!m_unloadQueue.empty() && (all || secondsBetween(start, Clock::now()) * 1000.0 < kBudgetMs)) {
        const u32 slot = m_unloadQueue.back();
        m_unloadQueue.pop_back();
        if (slot == kMapObjectsSlot) {
            if (m_listener) {
                m_listener->onTileUnloaded(pack::kMapObjectsTile);
            }
            continue;
        }
        LoadedTile& tile = m_loaded[slot];
        if (m_listener) {
            m_listener->onTileUnloaded(tileIndex(tile.tx, tile.ty));
        }
        renderer.destroyTerrainTile(tile.handle);
        tile.handle = {};
        tile.used = false;
        tile.closing = false;
    }
}

void Terrain::advanceOpening(Renderer& renderer) {
    if (!m_queued.empty() && m_loader.busy() && m_loader.done()) {
        // A step for a map nobody waits for any more: dropped (a prepared pack goes with the next prepare(), a
        // filled block with the map's close).
        m_loader.finish();
    }
    if (!m_queued.empty() && !m_loader.busy()) {
        const std::string next = std::move(m_queued);
        m_queued.clear();
        (void)openMap(m_packsDir, next, renderer); // never fails: errors come when the loader has looked
        return;
    }
    if (!m_loader.busy() || !m_loader.done()) {
        return;
    }
    VD_PROFILE_ZONE("Terrain::advanceOpening");
    const Clock::time_point start = Clock::now();
    TerrainMapLoader::Map& map = m_loader.map();
    if (m_step == OpenStep::Preparing) {
        if (!m_loader.succeeded()) {
            VD_LOG_ERROR("Map %s", map.error.c_str());
            m_loader.finish();
            m_step = OpenStep::Closed; // nothing to draw: the map asked for has no world pack
            return;
        }
        m_pack = std::move(map.pack); // its mapping (and every pointer into it) stays where it was
        m_header = map.header;
        m_tileInfo = map.tiles;
        m_horizon = std::move(map.horizon);
        m_mapObjects = map.objects;
        for (u32 i = 0; i < m_state.size(); ++i) {
            m_state[i] = (m_tileInfo[i].flags & pack::kMapTileExists) ? TileState::Unloaded : TileState::Absent;
        }
        // The shared indices and the horizon through the upload ring (~1 MB), the texture arrays by the loader.
        std::span<const u32> horizonWords;
        std::span<const u16> horizonIndices;
        if (!m_horizon.empty()) {
            pack::HorizonHeader h{};
            std::memcpy(&h, m_horizon.data(), sizeof(h));
            horizonWords = {reinterpret_cast<const u32*>(m_horizon.data() + h.heightsOffset),
                            u64{h.tileCount} * pack::kHorizonStride / 2};
            horizonIndices = {reinterpret_cast<const u16*>(m_horizon.data() + h.indicesOffset), h.indexCount};
            m_horizonSlots = reinterpret_cast<const u16*>(m_horizon.data() + h.slotsOffset);
        }
        if (Result<void> set = renderer.setTerrainGeometry({map.sharedIndices, m_header->sharedIndexCount},
                                                           {&m_header->lod[0].first, 2 * pack::kLodLevels},
                                                           horizonWords, horizonIndices);
            !set) {
            VD_LOG_ERROR("Map %s: %s", m_directory.c_str(), set.error().message.c_str());
            m_horizonSlots = nullptr;
        }
        Result<StagingBlock> staging = renderer.createStagingBlock(map.stagingBytes);
        m_loader.finish();
        if (!staging) {
            VD_LOG_ERROR("Map %s: %s", m_directory.c_str(), staging.error().message.c_str());
            m_step = OpenStep::Open; // tiles without textures: nothing drawn, but the map stands
            return;
        }
        m_mapStaging = staging.value();
        m_step = OpenStep::Filling;
        m_loader.fill(m_mapStaging.data);
        VD_LOG_INFO("Terrain map %s: %u tiles, %u textures, prepared in %.1f ms on vd-map", m_directory.c_str(),
                    m_header->tileCount, m_header->textureCount, static_cast<f64>(map.prepareMs));
    } else if (m_step == OpenStep::Filling) {
        if (m_loader.succeeded()) {
            if (Result<void> set = renderer.setTerrainTextures(m_mapStaging, map.terrainOffset, map.terrain,
                                                               map.liquidOffset, map.newLiquids ? &map.liquids : nullptr);
                set) {
                m_residentLiquids = map.newLiquids ? map.liquidKey : m_residentLiquids;
            } else {
                VD_LOG_ERROR("Map %s: %s", m_directory.c_str(), set.error().message.c_str());
            }
        } else {
            VD_LOG_ERROR("Map %s", map.error.c_str());
        }
        renderer.retireStagingBlock(m_mapStaging); // the copies above are recorded into the next frame
        m_step = OpenStep::Open;
        m_openWorstMs = std::max(m_openWorstMs, static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0));
        VD_LOG_INFO("Terrain map %s ready in %.0f ms (vd-map: prepare %.1f ms, textures %.1f ms%s; game thread: "
                    "at most %.2f ms a frame)",
                    m_directory.c_str(), secondsBetween(m_openStart, Clock::now()) * 1000.0,
                    static_cast<f64>(map.prepareMs), static_cast<f64>(map.fillMs),
                    map.newLiquids ? ", liquid frames too" : "", static_cast<f64>(m_openWorstMs));
        m_loader.finish();
        return;
    }
    m_openWorstMs = std::max(m_openWorstMs, static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0));
}

void Terrain::closeMap(Renderer& renderer) {
    while (m_loader.busy() && !m_loader.done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // shutdown only: the loader may read the pack
    }
    if (m_loader.busy()) {
        m_loader.finish();
    }
    m_queued.clear();
    beginClose(renderer);
    unloadClosing(renderer, true);
}

std::optional<pack::PackAsset> Terrain::findTile(u32 tx, u32 ty) const {
    char path[256];
    std::snprintf(path, sizeof(path), "world/maps/%s/%s_%u_%u.adt", m_directory.c_str(), m_directory.c_str(), tx, ty);
    std::optional<pack::PackAsset> asset = m_pack.find(std::string_view(path));
    if (asset && asset->type != pack::AssetType::TerrainTile) {
        return std::nullopt;
    }
    return asset;
}

Terrain::Upload Terrain::uploadTile(const TerrainStreamer::Done& done, Renderer& renderer) {
    VD_PROFILE_ZONE("Terrain::uploadTile");
    const Clock::time_point start = Clock::now();
    const u32 index = tileIndex(done.tx, done.ty);
    const u8* blob = m_streamer.data(done.slot);
    const usize size = m_streamer.size(done.slot);
    // The blob already sits in GPU staging memory: this only creates resources and records copies.
    Result<TerrainTileHandle> handle = renderer.createTerrainTile(m_staging[done.slot], size);
    if (!handle) {
        VD_LOG_ERROR("Terrain tile %u_%u: %s", done.tx, done.ty, handle.error().message.c_str());
        m_state[index] = TileState::Absent; // broken: never retried
        return Upload::Failed;
    }
    if (!handle.value().valid()) {
        return Upload::Retry; // upload ring full this frame (only on the RGBA8 alpha fallback)
    }

    u32 slot = 0;
    while (slot < m_loaded.size() && m_loaded[slot].used) {
        ++slot;
    }
    if (slot == m_loaded.size()) { // cannot happen: requests are capped by free slots
        renderer.destroyTerrainTile(handle.value());
        m_state[index] = TileState::Unloaded;
        return Upload::Done;
    }
    pack::TerrainTileHeader header;
    std::memcpy(&header, blob, sizeof(header));
    LoadedTile& tile = m_loaded[slot];
    tile.tx = done.tx;
    tile.ty = done.ty;
    tile.handle = handle.value();
    tile.minZ = header.minZ;
    tile.maxZ = header.maxZ;
    tile.used = true;
    const u8* vertices = blob + header.verticesOffset;
    for (u32 v = 0; v < pack::kTileVertices; ++v) {
        std::memcpy(&tile.heights[v], vertices + v * sizeof(pack::TerrainVertex), sizeof(f32));
    }
    for (u32 c = 0; c < pack::kTileChunks; ++c) {
        std::memcpy(&tile.areas[c], blob + header.chunksOffset + c * sizeof(pack::TerrainChunk) +
                                        offsetof(pack::TerrainChunk, areaId),
                    sizeof(u32));
    }
    m_state[index] = TileState::Loaded;
    m_slotOf[index] = static_cast<u8>(slot);
    if (m_listener) {
        m_listener->onTileLoaded(index, blob, size);
    }

    ++m_stats.tilesStreamed;
    m_stats.lastIoMs = done.ioMs;
    m_stats.lastUploadMs = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
    m_stats.worstUploadMs = std::max(m_stats.worstUploadMs, m_stats.lastUploadMs);
    return Upload::Done;
}

void Terrain::releaseFinishedSlots(const Renderer& renderer) {
    usize kept = 0;
    for (const PendingSlot& pending : m_pendingSlots) {
        if (renderer.completedFrames() >= pending.frame) {
            m_streamer.release(pending.slot);
        } else {
            m_pendingSlots[kept++] = pending;
        }
    }
    m_pendingSlots.resize(kept);
}

void Terrain::unloadTile(u32 slot, Renderer& renderer) {
    LoadedTile& tile = m_loaded[slot];
    if (m_listener) {
        m_listener->onTileUnloaded(tileIndex(tile.tx, tile.ty));
    }
    renderer.destroyTerrainTile(tile.handle);
    m_state[tileIndex(tile.tx, tile.ty)] = TileState::Unloaded;
    tile.handle = {};
    tile.used = false;
}

void Terrain::updateOpening(Renderer& renderer) {
    unloadClosing(renderer, false);
    advanceOpening(renderer);
}

void Terrain::update(const glm::dvec3& camera, f32 viewDistance, Renderer& renderer) {
    VD_PROFILE_ZONE("Terrain::update");
    // Phase 18 (world): the map before leaves a few tiles a frame, the new one opens off this thread; its tiles
    // stream once the old ones are gone (the listener keys objects by tile) and the staging slots are free.
    updateOpening(renderer);
    if (m_header == nullptr || !m_unloadQueue.empty()) {
        return;
    }
    if (!m_streamer.running()) {
        if (renderer.completedFrames() < m_slotsBusyUntil) {
            return;
        }
        for (StagingBlock& block : m_staging) {
            if (!block.valid()) {
                Result<StagingBlock> created = renderer.createStagingBlock(kStagingBytes);
                if (!created) {
                    VD_LOG_ERROR("Terrain staging: %s", created.error().message.c_str());
                    return;
                }
                block = created.value();
            }
        }
        if (m_loaded.size() != kMaxLoadedTiles) {
            m_loaded.resize(kMaxLoadedTiles);
            for (LoadedTile& tile : m_loaded) {
                tile.heights.resize(pack::kTileVertices);
            }
            m_waitingUpload.reserve(TerrainStreamer::kSlots);
            m_pendingSlots.reserve(TerrainStreamer::kSlots);
            m_drawDistances.reserve(kMaxLoadedTiles);
        }
        std::array<u8*, TerrainStreamer::kSlots> buffers{};
        for (u32 slot = 0; slot < buffers.size(); ++slot) {
            buffers[slot] = m_staging[slot].data;
        }
        m_streamer.start(buffers, kStagingBytes);
    }

    // Phase 12: the map's global WMO, once, for as long as the map is open.
    if (m_mapObjects && !m_mapObjectsShown && m_listener) {
        m_listener->onTileLoaded(pack::kMapObjectsTile, m_mapObjects->data, m_mapObjects->size);
        m_mapObjectsShown = true;
    }

    // 1. Tiles decompressed by the IO thread -> GPU, within this frame's budget.
    releaseFinishedSlots(renderer);
    TerrainStreamer::Done done;
    while (m_waitingUpload.size() < TerrainStreamer::kSlots && m_streamer.poll(done)) {
        m_waitingUpload.push_back(done);
    }
    u32 uploads = 0;
    while (!m_waitingUpload.empty() && uploads < kMaxUploadsPerFrame) {
        const TerrainStreamer::Done& next = m_waitingUpload.front();
        const u32 index = tileIndex(next.tx, next.ty);
        const Upload result = next.ok ? uploadTile(next, renderer) : Upload::Failed;
        if (result == Upload::Retry) {
            break; // ring full: keep it for the next frame
        }
        if (!next.ok) {
            m_state[index] = TileState::Absent;
        }
        if (result == Upload::Done) {
            // The copy is recorded into the next submitted frame; the slot stays busy until it completes.
            m_pendingSlots.push_back({next.slot, renderer.frameNumber() + 1});
        } else {
            m_streamer.release(next.slot);
        }
        m_waitingUpload.erase(m_waitingUpload.begin());
        ++uploads;
    }

    // 2. Unload what fell out of range, then the furthest tiles while over the memory budget.
    u32 loaded = 0;
    for (u32 slot = 0; slot < m_loaded.size(); ++slot) {
        LoadedTile& tile = m_loaded[slot];
        if (!tile.used || tile.closing) {
            continue;
        }
        if (distanceToTile(camera.x, camera.y, tile.tx, tile.ty) > viewDistance + kUnloadHysteresis) {
            unloadTile(slot, renderer);
        } else {
            ++loaded;
        }
    }
    const u64 textureBytes = renderer.terrainGpuBytes() > loaded * kTileGpuEstimate
                                 ? renderer.terrainGpuBytes() - loaded * kTileGpuEstimate
                                 : 0;
    while (loaded > 0 && textureBytes + loaded * kTileGpuEstimate > m_gpuBudget) {
        u32 furthest = 0;
        f64 furthestDistance = -1.0;
        for (u32 slot = 0; slot < m_loaded.size(); ++slot) {
            if (m_loaded[slot].used) {
                const f64 d = distanceToTile(camera.x, camera.y, m_loaded[slot].tx, m_loaded[slot].ty);
                if (d > furthestDistance) {
                    furthestDistance = d;
                    furthest = slot;
                }
            }
        }
        unloadTile(furthest, renderer);
        --loaded;
    }
    const u32 affordable = m_gpuBudget > textureBytes
                               ? static_cast<u32>((m_gpuBudget - textureBytes) / kTileGpuEstimate)
                               : 0;
    const u32 maxTiles = std::min(kMaxLoadedTiles, affordable);

    // 3. Request the nearest missing tiles.
    u32 cx = 0;
    u32 cy = 0;
    const bool inside = tileAt(camera.x, camera.y, cx, cy);
    const i32 radius = static_cast<i32>(std::ceil(viewDistance / pack::kTileSize)) + 1;
    while (inside && m_streamer.canRequest() && loaded + m_streamer.inFlight() < maxTiles) {
        f64 bestDistance = viewDistance;
        i32 bestX = -1;
        i32 bestY = -1;
        for (i32 ty = std::max(0, static_cast<i32>(cy) - radius);
             ty <= std::min<i32>(pack::kMapTiles - 1, static_cast<i32>(cy) + radius); ++ty) {
            for (i32 tx = std::max(0, static_cast<i32>(cx) - radius);
                 tx <= std::min<i32>(pack::kMapTiles - 1, static_cast<i32>(cx) + radius); ++tx) {
                if (m_state[tileIndex(tx, ty)] != TileState::Unloaded) {
                    continue;
                }
                const f64 distance = distanceToTile(camera.x, camera.y, tx, ty);
                if (distance <= bestDistance) {
                    bestDistance = distance;
                    bestX = tx;
                    bestY = ty;
                }
            }
        }
        if (bestX < 0) {
            break;
        }
        const u32 index = tileIndex(bestX, bestY);
        const std::optional<pack::PackAsset> asset = findTile(bestX, bestY);
        if (!asset) {
            m_state[index] = TileState::Absent;
            continue;
        }
        if (!m_streamer.request(static_cast<u16>(bestX), static_cast<u16>(bestY), *asset)) {
            break;
        }
        m_state[index] = TileState::Requested;
    }

    m_stats.loadedTiles = loaded;
    m_stats.inFlight = m_streamer.inFlight() - static_cast<u32>(m_waitingUpload.size() + m_pendingSlots.size());
    m_stats.waitingUpload = static_cast<u32>(m_waitingUpload.size());
}

void Terrain::collectDraws(const glm::dvec3& camera, const Frustum& frustum, f32 viewDistance,
                           std::vector<TerrainDraw>& out) {
    out.clear();
    m_drawDistances.clear();
    m_drawn.fill(false);
    for (const LoadedTile& tile : m_loaded) {
        if (!tile.used || tile.closing) {
            continue;
        }
        const f32 distance = static_cast<f32>(distanceToTile(camera.x, camera.y, tile.tx, tile.ty));
        if (distance > viewDistance) {
            continue;
        }
        const glm::vec3 corner(static_cast<f32>(pack::tileCornerX(tile.ty) - camera.x),
                               static_cast<f32>(pack::tileCornerY(tile.tx) - camera.y),
                               static_cast<f32>(-camera.z));
        const glm::vec3 min(corner.x - pack::kTileSize, corner.y - pack::kTileSize, tile.minZ + corner.z);
        const glm::vec3 max(corner.x, corner.y, tile.maxZ + corner.z);
        if (!frustum.intersects(min, max)) {
            continue;
        }
        u32 lod = 0;
        while (lod + 1 < pack::kLodLevels && distance > m_lodDistances[lod]) {
            ++lod;
        }
        // Front to back (early depth rejection): insertion into the small sorted list.
        usize at = out.size();
        while (at > 0 && m_drawDistances[at - 1] > distance) {
            --at;
        }
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), TerrainDraw{tile.handle, corner, lod});
        m_drawn[tileIndex(tile.tx, tile.ty)] = true;
        m_drawDistances.insert(m_drawDistances.begin() + static_cast<std::ptrdiff_t>(at), distance);
    }
    m_stats.visibleTiles = static_cast<u32>(out.size());
}

void Terrain::collectHorizon(const glm::dvec3& camera, const Frustum& frustum, f32 distance,
                             std::vector<HorizonDraw>& out) {
    out.clear();
    u32 cx = 0;
    u32 cy = 0;
    if (!m_horizonSlots || !tileAt(camera.x, camera.y, cx, cy)) {
        return;
    }
    const i32 radius = static_cast<i32>(std::ceil(distance / pack::kTileSize)) + 1;
    for (i32 ty = std::max(0, static_cast<i32>(cy) - radius);
         ty <= std::min<i32>(pack::kMapTiles - 1, static_cast<i32>(cy) + radius); ++ty) {
        for (i32 tx = std::max(0, static_cast<i32>(cx) - radius);
             tx <= std::min<i32>(pack::kMapTiles - 1, static_cast<i32>(cx) + radius); ++tx) {
            const u32 index = tileIndex(tx, ty);
            if (m_drawn[index] || m_horizonSlots[index] == pack::kNoHorizon ||
                distanceToTile(camera.x, camera.y, tx, ty) > distance || out.size() == out.capacity()) {
                continue;
            }
            const glm::vec3 corner(static_cast<f32>(pack::tileCornerX(ty) - camera.x),
                                   static_cast<f32>(pack::tileCornerY(tx) - camera.y),
                                   static_cast<f32>(-camera.z));
            // Height bounds from the map table (WDL heights stay within the ADT ones, roughly).
            const pack::TerrainMapTile& info = m_tileInfo[index];
            const f32 minZ = (info.flags & pack::kMapTileExists) ? info.minZ : -600.0f;
            const f32 maxZ = (info.flags & pack::kMapTileExists) ? info.maxZ : 2000.0f;
            const glm::vec3 min(corner.x - pack::kTileSize, corner.y - pack::kTileSize, minZ + corner.z - 50.0f);
            const glm::vec3 max(corner.x, corner.y, maxZ + corner.z + 50.0f);
            if (frustum.intersects(min, max)) {
                out.push_back({corner, m_horizonSlots[index]});
            }
        }
    }
}

u32 Terrain::areaAt(f64 x, f64 y) const {
    u32 tx = 0;
    u32 ty = 0;
    if (!isOpen() || !tileAt(x, y, tx, ty) || m_state[tileIndex(tx, ty)] != TileState::Loaded) {
        return 0;
    }
    const LoadedTile& tile = m_loaded[m_slotOf[tileIndex(tx, ty)]];
    // Chunks are 16 x 16 per tile, rows along t (from the tile's X corner), columns along s (from its Y corner).
    const f64 s = (pack::tileCornerY(tx) - y) / pack::kUnitSize;
    const f64 t = (pack::tileCornerX(ty) - x) / pack::kUnitSize;
    const u32 column = std::min<u32>(static_cast<u32>(std::max(s, 0.0)), 127) / 8;
    const u32 row = std::min<u32>(static_cast<u32>(std::max(t, 0.0)), 127) / 8;
    return tile.areas[row * 16 + column];
}

const f32* Terrain::tileHeights(u32 tx, u32 ty) const {
    if (!isOpen() || tx >= pack::kMapTiles || ty >= pack::kMapTiles || m_state[tileIndex(tx, ty)] != TileState::Loaded) {
        return nullptr;
    }
    const LoadedTile& tile = m_loaded[m_slotOf[tileIndex(tx, ty)]];
    return tile.heights.size() == pack::kTileVertices ? tile.heights.data() : nullptr;
}

std::optional<f32> Terrain::heightAt(f64 x, f64 y) const {
    u32 tx = 0;
    u32 ty = 0;
    if (!isOpen() || !tileAt(x, y, tx, ty) || m_state[tileIndex(tx, ty)] != TileState::Loaded) {
        return std::nullopt;
    }
    const LoadedTile& tile = m_loaded[m_slotOf[tileIndex(tx, ty)]];
    // Tile-local (s, t) in vertex units (pack/TerrainFormat.h).
    const f64 s = (pack::tileCornerY(tx) - y) / pack::kUnitSize;
    const f64 t = (pack::tileCornerX(ty) - x) / pack::kUnitSize;
    const u32 column = std::min<u32>(static_cast<u32>(s), 127);
    const u32 row = std::min<u32>(static_cast<u32>(t), 127);
    const f32 fs = static_cast<f32>(s - column);
    const f32 ft = static_cast<f32>(t - row);
    const u32 base = ((row / 8) * 16 + column / 8) * pack::kChunkVertices;
    const u32 q = column % 8;
    const u32 r = row % 8;
    const f32 tl = tile.heights[base + r * 17 + q];
    const f32 tr = tile.heights[base + r * 17 + q + 1];
    const f32 bl = tile.heights[base + (r + 1) * 17 + q];
    const f32 br = tile.heights[base + (r + 1) * 17 + q + 1];
    const f32 center = tile.heights[base + r * 17 + 9 + q];

    // The quad is a fan of 4 triangles around its center: pick the one under (fs, ft) and
    // interpolate linearly inside it.
    const f32 dx = fs - 0.5f;
    const f32 dy = ft - 0.5f;
    f32 a = 0.0f; // corner values of the triangle (edge a -> b), plus weights along both axes
    f32 b = 0.0f;
    f32 along = 0.0f; // position along the edge, 0..1
    f32 depth = 0.0f; // 0 at the center, 1 on the edge
    if (-dy >= std::abs(dx)) { // top edge: tl -> tr
        a = tl, b = tr, depth = -2.0f * dy;
        along = depth > 0.0f ? 0.5f + dx / depth : 0.5f;
    } else if (dx >= std::abs(dy)) { // right edge: tr -> br
        a = tr, b = br, depth = 2.0f * dx;
        along = depth > 0.0f ? 0.5f + dy / depth : 0.5f;
    } else if (dy >= std::abs(dx)) { // bottom edge: bl -> br
        a = bl, b = br, depth = 2.0f * dy;
        along = depth > 0.0f ? 0.5f + dx / depth : 0.5f;
    } else { // left edge: tl -> bl
        a = tl, b = bl, depth = -2.0f * dx;
        along = depth > 0.0f ? 0.5f + dy / depth : 0.5f;
    }
    const f32 edge = a + (b - a) * along;
    return center + (edge - center) * depth;
}

} // namespace vd
