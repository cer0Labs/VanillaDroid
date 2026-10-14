#include "world/GroundEffects.h"

#include "core/Clock.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "pack/TerrainFormat.h"
#include "world/Terrain.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

// AssetStreamer tags: the kind in the top byte; tiles carry the map generation and the tile index.
constexpr u64 kTagModels = 1ull << 56;
constexpr u64 kTagTexture = 2ull << 56;
constexpr u64 kTagTile = 3ull << 56;
constexpr u64 kTagKindMask = 0xFFull << 56;

constexpr i32 kMapChunks = static_cast<i32>(pack::kMapTiles * 16); // chunks per side of a map
// Placement reach over the fade's end: a doodad seen in a corner of the screen at the fade's depth is up to ~1.5 times
// that far away (wide screens, the cover display of the Fold5).
constexpr f32 kReachFactor = 1.5f;
constexpr f32 kPlaceBudgetMs = 0.5f; // chunk placements a frame stop past this (after the first)
constexpr u64 kTileKeepFrames = 120; // a tile record unused this long goes when room is needed

constexpr i8 kLit = 127;
constexpr i8 kShaded = 96; // 0xC0 / 0xFF of the client's vertex color, as snorm8

i8 snorm8(f32 v) {
    return static_cast<i8>(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f));
}

u32 envU32(const char* name, u32 fallback) {
    const char* value = VD_DEV_ENV(name);
    return value && *value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : fallback;
}

} // namespace

GroundEffects::~GroundEffects() {
    m_streamer.stop();
}

u32 GroundEffects::frillDensityForLevel(u32 level) {
    switch (level) {
    case 0: return 0;
    case 1: return grass::kDefaultFrillDensity / 2;
    case 2: return grass::kDefaultFrillDensity;
    default: return grass::kDefaultFrillDensity * 2;
    }
}

Result<void> GroundEffects::open(const std::filesystem::path& packsDir, const pack::PackReader& textures,
                                 Renderer& renderer) {
    if (m_open) {
        return {};
    }
    if (const char* grassOff = VD_DEV_ENV("VD_GRASS"); grassOff && std::strcmp(grassOff, "0") == 0) {
        m_enabled = false;
    }
    m_densityOverride = std::min(envU32("VD_GRASS_DENSITY", 0), grass::kMaxFrillDensity);
    m_trace = envU32("VD_TRACE_GRASS", 0) != 0;
    if (const char* distance = VD_DEV_ENV("VD_GRASS_DISTANCE"); distance && *distance) {
        m_distanceOverride = std::clamp(static_cast<f32>(std::atof(distance)), 5.0f, 300.0f);
    }
    VD_TRY(m_pack.open(packsDir / pack::kGrassPackFileName));
    Result<StagingBlock> staging = renderer.createStagingBlock(kStagingBytes);
    if (!staging) {
        m_pack.close();
        return staging.error();
    }
    m_staging = staging.value();
    m_textures = &textures;
    m_streamer.start(m_staging, "vd-grass");
    m_streamerStarted = true;

    m_chunks.assign(kMaxChunks, ChunkSlot{});
    m_runs.assign(static_cast<usize>(kMaxChunks) * kMaxRuns, Run{});
    clearChunks();
    m_candidates.reserve(1024);
    m_visible.reserve(kMaxChunks);
    m_touched.reserve(1024);
    m_open = true;
    VD_LOG_INFO("Grass: %s (%u assets)%s", (packsDir / pack::kGrassPackFileName).string().c_str(),
                m_pack.entryCount(), m_enabled ? "" : ", off (VD_GRASS=0)");
    return {};
}

void GroundEffects::close(Renderer& renderer) {
    if (!m_open) {
        return;
    }
    m_streamer.stop();
    m_streamerStarted = false;
    for (TextureEntry& entry : m_textureEntries) {
        if (entry.handle.valid()) {
            renderer.destroyTexture(entry.handle);
        }
    }
    m_textureEntries.clear();
    clearTiles();
    m_chunks.clear();
    m_runs.clear();
    m_chunkInstances.clear();
    m_chunkCapacity = 0;
    m_capacityDensity = 0;
    if (m_staging.valid()) {
        renderer.destroyStagingBlock(m_staging);
    }
    m_staging = {};
    m_pack.close();
    m_models.clear();
    m_effects.clear();
    m_modelsRequested = false;
    m_modelsReady = false;
    m_open = false;
}

void GroundEffects::setMap(std::string_view directory) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    if (directory.size() == m_map.size() &&
        std::equal(directory.begin(), directory.end(), m_map.begin(), [&](char a, char b) { return lower(a) == b; })) {
        return;
    }
    m_map.assign(directory);
    std::transform(m_map.begin(), m_map.end(), m_map.begin(), lower);
    ++m_mapGeneration;
    clearTiles();
    clearChunks();
}

void GroundEffects::clearTiles() {
    m_tileState.fill(TileState::Unknown);
    for (TileRecord& tile : m_tiles) {
        tile = TileRecord{};
    }
}

void GroundEffects::clearChunks() {
    for (ChunkSlot& chunk : m_chunks) {
        chunk = ChunkSlot{};
    }
    m_chunkGrid.fill(kNoSlot);
}

u32 GroundEffects::gridIndex(i32 key) {
    const u32 column = static_cast<u32>(key % kMapChunks);
    const u32 row = static_cast<u32>(key / kMapChunks);
    return (row % kGridSide) * kGridSide + column % kGridSide;
}

u32 GroundEffects::findChunk(i32 key) const {
    const u16 slot = m_chunkGrid[gridIndex(key)];
    return slot != kNoSlot && slot < m_chunks.size() && m_chunks[slot].key == key ? slot : kNoSlot;
}

f32 GroundEffects::fadeEnd() const {
    const f32 end = m_distanceOverride > 0.0f ? m_distanceOverride : grass::kFadeEnd * distanceScale;
    return std::max(end, 5.0f);
}

f32 GroundEffects::fadeStart() const {
    return fadeEnd() * (grass::kFadeStart / grass::kFadeEnd);
}

void GroundEffects::setFrillDensity(u32 frillDensity) {
    if (m_densityOverride > 0) {
        frillDensity = m_densityOverride;
    }
    frillDensity = std::min(frillDensity, grass::kMaxFrillDensity);
    if (frillDensity == m_frillDensity) {
        return;
    }
    m_frillDensity = frillDensity;
    clearChunks(); // the generator draws other cells: every chunk is placed again
}

void GroundEffects::update(Renderer& renderer) {
    if (!m_open) {
        return;
    }
    VD_PROFILE_ZONE("GroundEffects::update");
    if (!m_modelsRequested) {
        m_modelsRequested = true;
        const std::optional<pack::PackAsset> asset = m_pack.find(assetPathHash(pack::kGrassModelsAsset));
        if (!asset || asset->type != pack::AssetType::GrassModels || !m_streamer.request(kTagModels, *asset)) {
            VD_LOG_WARN("Grass: no models in grass.vdpk (re-bake grass)");
        }
    }
    AssetStreamer::Done done;
    u32 textures = 0;
    while (textures < kMaxTexturesPerFrame && m_streamer.poll(done)) {
        const u64 kind = done.tag & kTagKindMask;
        u64 frame = 0; // nothing uploaded from its staging bytes
        if (kind == kTagModels && done.ok) {
            onModels(done, renderer);
            frame = renderer.frameNumber() + 1;
        } else if (kind == kTagTexture) {
            ++textures;
            const u32 index = static_cast<u32>(done.tag & 0xFFFFFFFFu);
            if (done.ok && index < m_textureEntries.size()) {
                Result<TextureHandle> texture =
                    renderer.createTexture(m_staging, done.stagingOffset, done.data, static_cast<usize>(done.size));
                if (texture) {
                    m_textureEntries[index].handle = texture.value();
                    frame = renderer.frameNumber() + 1;
                } else {
                    VD_LOG_WARN("Grass texture: %s", texture.error().message.c_str());
                }
            }
        } else if (kind == kTagTile) {
            const u32 generation = static_cast<u32>((done.tag >> 16) & 0xFFFFFFFFu);
            const u32 tile = static_cast<u32>(done.tag & 0xFFFFu);
            if (generation == m_mapGeneration && tile < m_tileState.size()) {
                if (done.ok) {
                    onTile(tile, done);
                    m_streamer.release(done, 0, true); // the record keeps the heap copy
                    continue;
                }
                m_tileState[tile] = TileState::Absent;
            }
        }
        m_streamer.release(done, frame);
    }
    m_streamer.update(renderer.completedFrames());
    m_stats.pending = m_streamer.pending();
}

void GroundEffects::onModels(const AssetStreamer::Done& done, Renderer& renderer) {
    pack::GrassModelsHeader header;
    if (!done.data || done.size < sizeof(header)) {
        return;
    }
    std::memcpy(&header, done.data, sizeof(header));
    if (header.version != pack::kGrassFormatVersion ||
        header.modelsOffset + u64{header.modelCount} * sizeof(pack::GrassModel) > done.size ||
        header.effectsOffset + u64{header.effectCount} * sizeof(pack::GrassEffect) > done.size) {
        VD_LOG_WARN("Grass: models of another version (re-bake grass)");
        return;
    }
    if (Result<void> set = renderer.setGrassModels(m_staging, done.stagingOffset, done.data,
                                                   static_cast<usize>(done.size));
        !set) {
        VD_LOG_WARN("Grass: %s", set.error().message.c_str());
        return;
    }
    m_models.resize(header.modelCount);
    std::memcpy(m_models.data(), done.data + header.modelsOffset, header.modelCount * sizeof(pack::GrassModel));
    m_effects.resize(header.effectCount);
    std::memcpy(m_effects.data(), done.data + header.effectsOffset, header.effectCount * sizeof(pack::GrassEffect));
    m_maxDensity = 1;
    for (pack::GrassEffect& effect : m_effects) {
        for (u16& model : effect.models) {
            model = model < m_models.size() ? model : pack::kNoGrassModel;
        }
        m_maxDensity = std::max<u32>(m_maxDensity, effect.density);
    }
    // Their textures (a few dozen small ones): all of them, now.
    m_modelTexture.assign(m_models.size(), 0);
    for (u32 i = 0; i < m_models.size(); ++i) {
        u32 entry = 0;
        while (entry < m_textureEntries.size() && m_textureEntries[entry].hash != m_models[i].texture) {
            ++entry;
        }
        if (entry == m_textureEntries.size()) {
            m_textureEntries.push_back({m_models[i].texture, {}, false});
        }
        m_modelTexture[i] = entry;
    }
    for (u32 i = 0; i < m_textureEntries.size(); ++i) {
        TextureEntry& entry = m_textureEntries[i];
        const std::optional<pack::PackAsset> asset = m_textures ? m_textures->find(entry.hash) : std::nullopt;
        entry.requested = asset && m_streamer.request(kTagTexture | i, *asset);
    }
    m_modelBound.assign(m_models.size(), 0);
    m_modelRanges.assign(m_models.size(), ModelRange{});
    m_modelsReady = true;
    VD_LOG_INFO("Grass: %zu models, %zu ground effects, %zu textures", m_models.size(), m_effects.size(),
                m_textureEntries.size());
}

void GroundEffects::onTile(u32 tile, AssetStreamer::Done& done) {
    pack::GrassTileHeader header{};
    bool valid = done.data && done.size >= sizeof(header);
    if (valid) {
        std::memcpy(&header, done.data, sizeof(header));
        valid = header.version == pack::kGrassFormatVersion &&
                header.chunksOffset + u64{pack::kTileChunks} * sizeof(pack::GrassChunk) <= done.size &&
                header.shadowsOffset + u64{header.shadowCount} * pack::kGrassShadowBytes <= done.size;
    }
    if (!valid) {
        delete[] done.data;
        m_tileState[tile] = TileState::Absent;
        return;
    }
    // A free record, or the one unused longest.
    u32 slot = 0;
    for (u32 i = 0; i < kMaxTiles; ++i) {
        if (!m_tiles[i].data) {
            slot = i;
            break;
        }
        if (m_tiles[i].lastUsed < m_tiles[slot].lastUsed) {
            slot = i;
        }
    }
    TileRecord& record = m_tiles[slot];
    if (record.data) {
        m_tileState[record.ty * pack::kMapTiles + record.tx] = TileState::Unknown; // read again if it comes back
    }
    record.tx = static_cast<u16>(tile % pack::kMapTiles);
    record.ty = static_cast<u16>(tile / pack::kMapTiles);
    record.data.reset(done.data);
    record.size = static_cast<u32>(done.size);
    record.chunks = reinterpret_cast<const pack::GrassChunk*>(record.data.get() + header.chunksOffset);
    record.shadows = record.data.get() + header.shadowsOffset;
    record.shadowCount = header.shadowCount;
    record.lastUsed = m_frame;
    m_tileState[tile] = TileState::Loaded;
    m_tileSlot[tile] = static_cast<u8>(slot);
}

GroundEffects::TileRecord* GroundEffects::tileRecord(u32 tx, u32 ty) {
    const u32 tile = ty * pack::kMapTiles + tx;
    if (m_tileState[tile] != TileState::Loaded) {
        return nullptr;
    }
    TileRecord& record = m_tiles[m_tileSlot[tile]];
    record.lastUsed = m_frame;
    return &record;
}

void GroundEffects::requestTile(u32 tx, u32 ty) {
    const u32 tile = ty * pack::kMapTiles + tx;
    if (m_tileState[tile] != TileState::Unknown) {
        return;
    }
    // Room for it: a record unused for a while must be there (the ones in reach are used every frame).
    bool room = false;
    for (const TileRecord& record : m_tiles) {
        room |= !record.data || record.lastUsed + kTileKeepFrames < m_frame;
    }
    if (!room) {
        return;
    }
    char path[128];
    pack::grassTilePath(path, sizeof(path), m_map.c_str(), tx, ty);
    const std::optional<pack::PackAsset> asset = m_pack.find(assetPathHash(path));
    if (!asset || asset->type != pack::AssetType::GrassTile) {
        m_tileState[tile] = TileState::Absent; // nothing grows on it
        return;
    }
    const u64 tag = kTagTile | (static_cast<u64>(m_mapGeneration) << 16) | tile;
    if (m_streamer.request(tag, *asset)) {
        m_tileState[tile] = TileState::Requested;
    }
}

u32 GroundEffects::freeChunkSlot() {
    u32 best = 0;
    for (u32 i = 0; i < m_chunks.size(); ++i) {
        if (m_chunks[i].key < 0) {
            return i;
        }
        if (m_chunks[i].lastUsed < m_chunks[best].lastUsed) {
            best = i;
        }
    }
    if (m_chunks[best].lastUsed >= m_frame) {
        return kNoSlot; // never one in reach this frame
    }
    if (m_chunkGrid[gridIndex(m_chunks[best].key)] == best) {
        m_chunkGrid[gridIndex(m_chunks[best].key)] = kNoSlot;
    }
    m_chunks[best] = ChunkSlot{};
    return best;
}

bool GroundEffects::placeChunk(i32 key, const TileRecord& tile, const f32* tileHeights, u32 slot) {
    VD_PROFILE_ZONE("GroundEffects::placeChunk");
    const u32 column = static_cast<u32>(key % kMapChunks);
    const u32 row = static_cast<u32>(key / kMapChunks);
    const u32 index = (row % 16) * 16 + column % 16;
    const pack::GrassChunk& record = tile.chunks[index];
    grass::ChunkInput input;
    input.chunk = &record;
    input.shadowBits = record.shadow != pack::kNoGrassShadow && record.shadow < tile.shadowCount
                           ? tile.shadows + static_cast<usize>(record.shadow) * pack::kGrassShadowBytes
                           : nullptr;
    input.heights = tileHeights + index * pack::kChunkVertices;
    input.seed = grass::chunkSeed(column / 16, row / 16, column % 16, row % 16);
    const u32 count = grass::placeChunk(input, m_effects, m_models, m_frillDensity, m_placed);

    ChunkSlot& chunk = m_chunks[slot];
    chunk = ChunkSlot{};
    chunk.key = key;
    m_chunkGrid[gridIndex(key)] = static_cast<u16>(slot); // a far chunk of the same grid cell is forgotten
    chunk.cornerX = (static_cast<f64>(kMapChunks / 2) - row) * pack::kChunkSize;
    chunk.cornerY = (static_cast<f64>(kMapChunks / 2) - column) * pack::kChunkSize;
    chunk.lastUsed = m_frame;
    // Grouped by model (at most kMaxRuns of them: four layers of four slots), in the client's order within each.
    Run* runs = &m_runs[static_cast<usize>(slot) * kMaxRuns];
    u32 runCount = 0;
    for (u32 i = 0; i < count; ++i) {
        u32 r = 0;
        while (r < runCount && runs[r].model != m_placed[i].model) {
            ++r;
        }
        if (r == runCount) {
            if (runCount == kMaxRuns) {
                continue;
            }
            runs[runCount++] = {m_placed[i].model, 0, 0};
        }
        ++runs[r].count;
    }
    u32 first = slot * m_chunkCapacity;
    for (u32 r = 0; r < runCount; ++r) {
        runs[r].first = first;
        first += runs[r].count;
        runs[r].count = 0; // the write cursor below
    }
    f32 minZ = 1e9f;
    f32 maxZ = -1e9f;
    for (u32 i = 0; i < count; ++i) {
        const grass::PlacedGrass& p = m_placed[i];
        u32 r = 0;
        while (r < runCount && runs[r].model != p.model) {
            ++r;
        }
        if (r == runCount) {
            continue;
        }
        GrassInstance& out = m_chunkInstances[runs[r].first + runs[r].count++];
        out.position[0] = p.position[0];
        out.position[1] = p.position[1];
        out.position[2] = p.position[2];
        out.rotation[0] = p.cosine * p.scale;
        out.rotation[1] = p.sine * p.scale;
        out.normal[0] = snorm8(p.normal[0]);
        out.normal[1] = snorm8(p.normal[1]);
        out.normal[2] = snorm8(p.normal[2]);
        out.normal[3] = p.shaded ? kShaded : kLit;
        minZ = std::min(minZ, p.position[2]);
        maxZ = std::max(maxZ, p.position[2] + m_models[p.model].height * p.scale);
    }
    chunk.runCount = runCount;
    chunk.instanceCount = count;
    chunk.minZ = count > 0 ? minZ : 0.0f;
    chunk.maxZ = count > 0 ? maxZ : 0.0f;
    ++m_stats.generated;
    return true;
}

bool GroundEffects::chunkVisible(const ChunkSlot& chunk, const glm::dvec3& camera, const glm::vec3& forward,
                                 const Frustum& frustum, f32 fadeEnd) const {
    if (chunk.instanceCount == 0) {
        return false;
    }
    const glm::vec3 min(static_cast<f32>(chunk.cornerX - pack::kChunkSize - camera.x),
                        static_cast<f32>(chunk.cornerY - pack::kChunkSize - camera.y),
                        static_cast<f32>(static_cast<f64>(chunk.minZ) - camera.z));
    const glm::vec3 max(static_cast<f32>(chunk.cornerX - camera.x), static_cast<f32>(chunk.cornerY - camera.y),
                        static_cast<f32>(static_cast<f64>(chunk.maxZ) - camera.z));
    // The box's nearest view depth (the client's test is on the chunk's sphere: the box is tighter).
    const glm::vec3 center = (min + max) * 0.5f;
    const glm::vec3 half = (max - min) * 0.5f;
    const f32 nearest = glm::dot(forward, center) - glm::dot(glm::abs(forward), half);
    return nearest < fadeEnd && frustum.intersects(min, max);
}

void GroundEffects::collect(const glm::dvec3& camera, const glm::vec3& forward, const Frustum& frustum,
                            const Terrain& terrain, std::vector<GrassDraw>& draws,
                            std::vector<GrassInstance>& instances) {
    draws.clear();
    instances.clear();
    m_stats.visibleChunks = 0;
    m_stats.instances = 0;
    m_stats.draws = 0;
    m_stats.generated = 0;
    m_stats.collectMs = 0.0f;
    m_stats.frillDensity = m_frillDensity;
    if (!m_open || !m_enabled || !m_modelsReady || m_frillDensity == 0 || m_map.empty() || !terrain.isOpen()) {
        return;
    }
    VD_PROFILE_ZONE("GroundEffects::collect");
    const Clock::time_point start = Clock::now();
    ++m_frame;
    // Chunk storage for this density (a settings change, not a frame's work).
    if (m_capacityDensity != m_frillDensity) {
        m_capacityDensity = m_frillDensity;
        m_chunkCapacity = grass::maxPerChunk(m_frillDensity, m_maxDensity);
        m_chunkInstances.assign(static_cast<usize>(kMaxChunks) * m_chunkCapacity, GrassInstance{});
        m_placed.resize(m_chunkCapacity);
        clearChunks();
    }
    const f32 fadeEndDepth = fadeEnd();
    const f32 reach = fadeEndDepth * kReachFactor;
    // The chunks around the camera (global indices: column along -Y, row along -X, both from the map's corner).
    const f64 columnAt = static_cast<f64>(kMapChunks / 2) - camera.y / pack::kChunkSize;
    const f64 rowAt = static_cast<f64>(kMapChunks / 2) - camera.x / pack::kChunkSize;
    const i32 span = static_cast<i32>(std::ceil(reach / pack::kChunkSize)) + 1;
    const i32 column0 = std::max(0, static_cast<i32>(std::floor(columnAt)) - span);
    const i32 column1 = std::min(kMapChunks - 1, static_cast<i32>(std::floor(columnAt)) + span);
    const i32 row0 = std::max(0, static_cast<i32>(std::floor(rowAt)) - span);
    const i32 row1 = std::min(kMapChunks - 1, static_cast<i32>(std::floor(rowAt)) + span);
    m_candidates.clear();
    m_visible.clear();
    for (i32 row = row0; row <= row1; ++row) {
        for (i32 column = column0; column <= column1; ++column) {
            const f64 cornerX = (static_cast<f64>(kMapChunks / 2) - row) * pack::kChunkSize;
            const f64 cornerY = (static_cast<f64>(kMapChunks / 2) - column) * pack::kChunkSize;
            const f64 dx = std::max({0.0, cornerX - pack::kChunkSize - camera.x, camera.x - cornerX});
            const f64 dy = std::max({0.0, cornerY - pack::kChunkSize - camera.y, camera.y - cornerY});
            const f32 distance = static_cast<f32>(std::sqrt(dx * dx + dy * dy));
            if (distance > reach) {
                continue;
            }
            const i32 key = row * kMapChunks + column;
            const u32 slot = findChunk(key);
            if (slot != kNoSlot) {
                ChunkSlot& chunk = m_chunks[slot];
                chunk.lastUsed = m_frame;
                if (TileRecord* tile = tileRecord(static_cast<u32>(column) / 16, static_cast<u32>(row) / 16)) {
                    tile->lastUsed = m_frame;
                }
                if (chunkVisible(chunk, camera, forward, frustum, fadeEndDepth)) {
                    m_visible.push_back(slot);
                }
                continue;
            }
            const u32 tx = static_cast<u32>(column) / 16;
            const u32 ty = static_cast<u32>(row) / 16;
            TileRecord* tile = tileRecord(tx, ty);
            if (!tile) {
                requestTile(tx, ty);
                continue;
            }
            const pack::GrassChunk& record = tile->chunks[(row % 16) * 16 + column % 16];
            if ((record.flags & pack::kGrassChunkGrows) == 0 || m_candidates.size() == m_candidates.capacity()) {
                continue;
            }
            m_candidates.push_back({distance, key});
        }
    }
    // The nearest first, a few a frame.
    std::sort(m_candidates.begin(), m_candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
    for (const Candidate& candidate : m_candidates) {
        if (m_stats.generated >= kMaxPlacedPerFrame ||
            (m_stats.generated > 0 && secondsBetween(start, Clock::now()) * 1000.0 > kPlaceBudgetMs)) {
            break;
        }
        const u32 column = static_cast<u32>(candidate.key % kMapChunks);
        const u32 row = static_cast<u32>(candidate.key / kMapChunks);
        const f32* heights = terrain.tileHeights(column / 16, row / 16);
        TileRecord* tile = tileRecord(column / 16, row / 16);
        if (!heights || !tile) {
            continue; // the terrain under it is not loaded yet
        }
        const u32 slot = freeChunkSlot();
        if (slot == kNoSlot) {
            break;
        }
        placeChunk(candidate.key, *tile, heights, slot);
        if (chunkVisible(m_chunks[slot], camera, forward, frustum, fadeEndDepth)) {
            m_visible.push_back(slot);
        }
    }
    // Doodads past where the ramp hides them (alpha under the reference) are not sent; a margin for their size.
    const f32 cutDepth = fadeEndDepth - 0.5f * (fadeEndDepth - fadeStart()) + 2.0f;
    gather(camera, forward, cutDepth, draws, instances);
    u32 cached = 0;
    for (const ChunkSlot& chunk : m_chunks) {
        cached += chunk.key >= 0 ? 1 : 0;
    }
    u32 tiles = 0;
    for (const TileRecord& tile : m_tiles) {
        tiles += tile.data ? 1 : 0;
    }
    m_stats.cachedChunks = cached;
    m_stats.tiles = tiles;
    m_stats.visibleChunks = static_cast<u32>(m_visible.size());
    m_stats.draws = static_cast<u32>(draws.size());
    m_stats.collectMs = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
    if (m_trace) {
        trace(draws);
    }
}

void GroundEffects::trace(std::span<const GrassDraw> draws) {
    u32 triangles = 0;
    for (const GrassDraw& draw : draws) {
        triangles += draw.indexCount / 3 * draw.instanceCount;
    }
    Trace& t = m_traced;
    if (m_stats.collectMs > 1.0f) {
        VD_LOG_INFO("GRASS slow frame: %.2f ms, %u chunks placed, %u doodads, %u tiles reading",
                    static_cast<f64>(m_stats.collectMs), m_stats.generated, m_stats.instances, m_stats.pending);
    }
    ++t.frames;
    t.instances = std::max(t.instances, m_stats.instances);
    t.draws = std::max(t.draws, m_stats.draws);
    t.triangles = std::max(t.triangles, triangles);
    t.chunks = std::max(t.chunks, m_stats.visibleChunks);
    t.placed += m_stats.generated;
    t.collectMs += m_stats.collectMs;
    t.collectMaxMs = std::max(t.collectMaxMs, m_stats.collectMs);
    if (t.frames >= 300) {
        VD_LOG_INFO("GRASS %u frames: max %u doodads, %u draws, %u tris, %u chunks | %u chunks placed | collect "
                    "%.3f ms avg, %.3f max | frillDensity %u, fade %.1f-%.1f yd",
                    t.frames, t.instances, t.draws, t.triangles, t.chunks, t.placed, t.collectMs / t.frames,
                    static_cast<f64>(t.collectMaxMs), m_frillDensity, static_cast<f64>(fadeStart()),
                    static_cast<f64>(fadeEnd()));
        t = Trace{};
    }
}

void GroundEffects::gather(const glm::dvec3& camera, const glm::vec3& forward, f32 cutDepth,
                           std::vector<GrassDraw>& draws, std::vector<GrassInstance>& instances) {
    // 1. How many each model may get (every instance of the visible chunks), and its range of the frame's instances.
    m_touched.clear();
    for (const u32 slot : m_visible) {
        const Run* runs = &m_runs[static_cast<usize>(slot) * kMaxRuns];
        for (u32 r = 0; r < m_chunks[slot].runCount; ++r) {
            if (m_modelBound[runs[r].model] == 0) {
                m_touched.push_back(runs[r].model);
            }
            m_modelBound[runs[r].model] += runs[r].count;
        }
    }
    if (m_touched.empty()) {
        return;
    }
    std::sort(m_touched.begin(), m_touched.end()); // models are sorted by texture: fewer binds
    const u32 capacity = static_cast<u32>(std::min<usize>(instances.capacity(), kMaxInstances));
    u32 total = 0;
    for (const u16 model : m_touched) {
        ModelRange& range = m_modelRanges[model];
        range.first = total;
        total = std::min(total + m_modelBound[model], capacity);
        range.end = total;
        range.cursor = range.first;
        m_modelBound[model] = 0;
    }
    instances.resize(total);
    // 2. The instances, camera-relative, but not those the depth ramp hides.
    for (const u32 slot : m_visible) {
        const ChunkSlot& chunk = m_chunks[slot];
        const glm::vec3 offset(static_cast<f32>(chunk.cornerX - camera.x), static_cast<f32>(chunk.cornerY - camera.y),
                               static_cast<f32>(-camera.z));
        const Run* runs = &m_runs[static_cast<usize>(slot) * kMaxRuns];
        for (u32 r = 0; r < chunk.runCount; ++r) {
            ModelRange& range = m_modelRanges[runs[r].model];
            const GrassInstance* source = &m_chunkInstances[runs[r].first];
            for (u32 i = 0; i < runs[r].count && range.cursor < range.end; ++i) {
                const glm::vec3 p(source[i].position[0] + offset.x, source[i].position[1] + offset.y,
                                  source[i].position[2] + offset.z);
                if (glm::dot(forward, p) > cutDepth) {
                    continue;
                }
                GrassInstance& out = instances[range.cursor++];
                out = source[i];
                out.position[0] = p.x;
                out.position[1] = p.y;
                out.position[2] = p.z;
            }
        }
    }
    // 3. One draw per model with what it got (gaps left by the cut are never drawn).
    for (const u16 model : m_touched) {
        const ModelRange& range = m_modelRanges[model];
        const TextureEntry& texture = m_textureEntries[m_modelTexture[model]];
        if (range.cursor == range.first || !texture.handle.valid()) {
            continue;
        }
        const pack::GrassModel& m = m_models[model];
        GrassDraw draw;
        draw.firstIndex = m.firstIndex;
        draw.indexCount = m.indexCount;
        draw.vertexOffset = m.firstVertex;
        draw.firstInstance = range.first;
        draw.instanceCount = range.cursor - range.first;
        draw.texture = texture.handle;
        draw.clamp = (m.flags & pack::kGrassModelClamp) != 0;
        draws.push_back(draw);
        m_stats.instances += draw.instanceCount;
    }
}

} // namespace vd
