#include "world/TerrainMapLoader.h"

#include "core/Clock.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "platform/Thread.h"
#include "render/TextureStaging.h"

#include <algorithm>
#include <cstring>

namespace vd {

TerrainMapLoader::TerrainMapLoader() {
    m_thread = std::thread([this] { run(); });
}

TerrainMapLoader::~TerrainMapLoader() {
    m_stop = true; // also stops a fill() between two layers
    m_wake.release();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void TerrainMapLoader::prepare(const std::filesystem::path& packsDir, std::string_view directory, bool nativeAstc,
                               u64 residentLiquids) {
    if (busy()) {
        return;
    }
    m_map = Map{};
    m_map.directory.assign(directory);
    m_packsDir = packsDir;
    m_nativeAstc = nativeAstc;
    m_residentLiquids = residentLiquids;
    m_ok = false;
    m_job = Job::Prepare;
    m_wake.release();
}

void TerrainMapLoader::fill(u8* staging) {
    if (busy()) {
        return;
    }
    m_staging = staging;
    m_units = 0;
    m_ok = false;
    m_job = Job::Fill;
    m_wake.release();
}

void TerrainMapLoader::finish() {
    m_finished.store(false, std::memory_order_relaxed);
    m_job = Job::None;
}

f32 TerrainMapLoader::fillProgress() const {
    return std::min(1.0f, static_cast<f32>(m_units.load(std::memory_order_relaxed)) / static_cast<f32>(m_totalUnits));
}

void TerrainMapLoader::run() {
    setCurrentThreadName("vd-map");
    for (;;) {
        m_wake.acquire();
        if (m_stop.load()) {
            return;
        }
        const Clock::time_point start = Clock::now();
        if (m_job == Job::Prepare) {
            m_ok = doPrepare();
            m_map.prepareMs = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
        } else if (m_job == Job::Fill) {
            m_ok = doFill();
            m_map.fillMs = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
        }
        m_finished.store(true, std::memory_order_release);
    }
}

bool TerrainMapLoader::doPrepare() {
    VD_PROFILE_ZONE("TerrainMapLoader::prepare");
    Map& map = m_map;
    const std::string& dir = map.directory;
    auto fail = [&](const std::string& reason) {
        map.error = dir + ": " + reason;
        map.pack.close();
        return false;
    };
    if (Result<void> opened = map.pack.open(m_packsDir / pack::worldPackFileName(dir)); !opened) {
        return fail(opened.error().message);
    }
    const std::optional<pack::PackAsset> wdt = map.pack.find("world/maps/" + dir + "/" + dir + ".wdt");
    const std::optional<pack::PackAsset> textures = map.pack.find("world/maps/" + dir + "/terraintextures");
    std::optional<pack::PackAsset> liquids = map.pack.find(pack::kLiquidTexturesAsset);
    if (!liquids) { // phase 12: shared by every map
        if (m_liquidPack.entryCount() == 0) {
            if (Result<void> opened = m_liquidPack.open(m_packsDir / pack::kLiquidPackFileName); !opened) {
                return fail(opened.error().message);
            }
        }
        liquids = m_liquidPack.find(pack::kLiquidTexturesAsset);
    }
    if (!wdt || wdt->type != pack::AssetType::TerrainMap || wdt->compression != pack::Compression::None ||
        wdt->size < sizeof(pack::TerrainMapHeader)) {
        return fail("no terrain map in the pack");
    }
    if (!textures || textures->type != pack::AssetType::Texture || textures->compression != pack::Compression::None ||
        textures->size < sizeof(pack::TextureHeader) || !liquids || liquids->type != pack::AssetType::Texture ||
        liquids->compression != pack::Compression::None || liquids->size < sizeof(pack::TextureHeader)) {
        return fail("no terrain or liquid textures in the pack (re-bake)");
    }
    const auto* header = reinterpret_cast<const pack::TerrainMapHeader*>(wdt->data);
    if (header->version != pack::kTerrainFormatVersion ||
        header->tilesOffset + sizeof(pack::TerrainMapTile) * pack::kMapTiles * pack::kMapTiles > wdt->size ||
        header->sharedIndicesOffset + u64{header->sharedIndexCount} * 2 > wdt->size) {
        return fail("terrain map version " + std::to_string(header->version) + " or layout not supported (re-bake)");
    }
    map.header = header;
    map.tiles = reinterpret_cast<const pack::TerrainMapTile*>(wdt->data + header->tilesOffset);
    map.sharedIndices = reinterpret_cast<const u16*>(wdt->data + header->sharedIndicesOffset);

    // Distant horizon (optional: a pack without it still works).
    if (const std::optional<pack::PackAsset> horizon = map.pack.find("world/maps/" + dir + "/horizon");
        horizon && horizon->type == pack::AssetType::TerrainHorizon) {
        map.horizon.resize(horizon->rawSize);
        pack::HorizonHeader h{};
        bool valid = pack::PackReader::read(*horizon, map.horizon.data(), map.horizon.size()) &&
                     map.horizon.size() >= sizeof(h);
        if (valid) {
            std::memcpy(&h, map.horizon.data(), sizeof(h));
            const u64 heightsEnd = h.heightsOffset + u64{h.tileCount} * pack::kHorizonStride * 2;
            valid = h.version == pack::kTerrainFormatVersion && heightsEnd <= map.horizon.size() &&
                    h.slotsOffset + pack::kMapTiles * pack::kMapTiles * 2 <= map.horizon.size() &&
                    h.indicesOffset + u64{h.indexCount} * 2 <= map.horizon.size();
        }
        if (!valid) {
            VD_LOG_WARN("Horizon of %s: corrupt, left out", dir.c_str());
            map.horizon.clear();
        }
    }
    if (const std::optional<pack::PackAsset> objects = map.pack.find("world/maps/" + dir + "/objects");
        objects && objects->type == pack::AssetType::TerrainObjects &&
        objects->compression == pack::Compression::None && objects->size >= sizeof(pack::TerrainTileHeader)) {
        map.objects = objects; // read in place from the mapped pack
    }

    // The textures as the GPU will take them, one after the other in the staging block.
    m_terrainSource = *textures;
    m_liquidSource = *liquids;
    pack::TextureHeader source;
    std::memcpy(&source, textures->data, sizeof(source));
    u64 bytes = 0;
    map.terrain = stagedTextureLayout(source, m_nativeAstc, bytes);
    map.terrainOffset = 0;
    map.stagingBytes = (bytes + 63) & ~u64{63};
    m_totalUnits = std::max<u32>(1, source.layers) * source.mipCount;
    map.liquidKey = fnv1a64(liquids->data, liquids->size);
    map.newLiquids = map.liquidKey != m_residentLiquids;
    if (map.newLiquids) {
        std::memcpy(&source, liquids->data, sizeof(source));
        map.liquids = stagedTextureLayout(source, m_nativeAstc, bytes);
        map.liquidOffset = map.stagingBytes;
        map.stagingBytes += (bytes + 63) & ~u64{63};
        m_totalUnits += std::max<u32>(1, source.layers) * source.mipCount;
    }
    return true;
}

bool TerrainMapLoader::doFill() {
    VD_PROFILE_ZONE("TerrainMapLoader::fill");
    Map& map = m_map;
    if (!m_staging || !map.header) {
        return false;
    }
    if (!stageTexture(m_terrainSource.data, m_terrainSource.size, map.terrain, m_staging + map.terrainOffset, &m_stop,
                      &m_units)) {
        map.error = map.directory + ": terrain textures could not be staged";
        return false;
    }
    if (map.newLiquids && !stageTexture(m_liquidSource.data, m_liquidSource.size, map.liquids,
                                        m_staging + map.liquidOffset, &m_stop, &m_units)) {
        map.error = map.directory + ": liquid frames could not be staged";
        return false;
    }
    return true;
}

} // namespace vd
