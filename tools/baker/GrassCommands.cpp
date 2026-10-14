#include "BakerCommon.h"

#include "bake/GrassBaker.h"
#include "bake/PackWriter.h"
#include "core/Clock.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "formats/Adt.h"
#include "formats/Dbc.h"
#include "formats/M2.h"
#include "jobs/JobSystem.h"
#include "pack/GrassFormat.h"
#include "world/GrassPlacement.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// Phase 14 (grass): the terrain's detail doodads (GroundEffectTexture / GroundEffectDoodad) -> grass.vdpk, and a
// census of how the 1.12 maps use them.

namespace vd::baker {
namespace {

struct LoadedTables {
    bake::GroundEffectTables rows;
    bake::GrassTables tables;
    u64 hash = kFnvOffset; // of both DBCs: the bake cache key of every tile
    bool ok = false;
};

LoadedTables loadTables(const formats::MpqArchiveSet& mpq) {
    LoadedTables out;
    Result<std::vector<u8>> textureBytes = mpq.read("DBFilesClient\\GroundEffectTexture.dbc");
    Result<std::vector<u8>> doodadBytes = mpq.read("DBFilesClient\\GroundEffectDoodad.dbc");
    if (!textureBytes || !doodadBytes) {
        VD_LOG_ERROR("GroundEffectTexture.dbc / GroundEffectDoodad.dbc unreadable");
        return out;
    }
    out.hash = fnv1a64(textureBytes.value().data(), textureBytes.value().size(), out.hash);
    out.hash = fnv1a64(doodadBytes.value().data(), doodadBytes.value().size(), out.hash);
    Result<formats::DbcFile> textures = formats::DbcFile::parse(std::move(textureBytes.value()));
    Result<formats::DbcFile> doodads = formats::DbcFile::parse(std::move(doodadBytes.value()));
    if (!textures || !doodads) {
        VD_LOG_ERROR("GroundEffect tables: unexpected layout");
        return out;
    }
    out.rows = bake::readGroundEffectTables(textures.value().view(), doodads.value().view());
    out.tables.build(out.rows, [&](u32 tag) -> Result<bake::GrassModelGeometry> {
        const std::string path = bake::groundEffectModelPath(out.rows.doodadNames.at(tag));
        Result<std::vector<u8>> bytes = mpq.read(path);
        if (!bytes) {
            return Error{path + ": " + bytes.error().message};
        }
        out.hash = fnv1a64(bytes.value().data(), bytes.value().size(), out.hash);
        Result<formats::M2Model> model = formats::parseM2(bytes.value());
        if (!model) {
            return Error{path + ": " + model.error().message};
        }
        return bake::convertGrassModel(model.value());
    });
    out.ok = true;
    return out;
}

// A chunk's heights as the placement reads them: absolute (the ADT stores them over the chunk's base).
void absoluteHeights(const formats::AdtChunk& chunk, f32 (&out)[pack::kChunkVertices]) {
    for (u32 v = 0; v < pack::kChunkVertices; ++v) {
        out[v] = chunk.position[2] + chunk.heights[v];
    }
}

} // namespace

int commandScanGrass(const Options& options) {
    formats::MpqArchiveSet mpq;
    if (!openArchives(options, mpq)) {
        return 1;
    }
    const Clock::time_point start = Clock::now();
    LoadedTables loaded = loadTables(mpq);
    if (!loaded.ok) {
        return 1;
    }
    const bake::GrassTables& tables = loaded.tables;
    u32 rowsWithDoodads = 0;
    for (const auto& [id, row] : loaded.rows.effects) {
        rowsWithDoodads += std::any_of(row.doodads.begin(), row.doodads.end(),
                                       [](u32 tag) { return tag != bake::kNoGroundEffectDoodad; })
                               ? 1
                               : 0;
    }
    std::printf("GroundEffectTexture: %zu rows, %u with doodads; GroundEffectDoodad: %u rows | %zu models (%u "
                "missing or unreadable), %zu effects (%u broken)\n",
                loaded.rows.effects.size(), rowsWithDoodads, loaded.rows.doodadRows, tables.models().size(),
                tables.missingModels(), tables.effects().size(), tables.brokenEffects());
    std::map<std::string, u32> textures;
    u32 maxIndices = 0, maxVertices = 0;
    for (u32 i = 0; i < tables.models().size(); ++i) {
        ++textures[tables.modelPaths()[i]];
        maxIndices = std::max(maxIndices, tables.models()[i].indexCount);
        maxVertices = std::max(maxVertices, tables.models()[i].vertexCount);
    }
    std::printf("%zu textures; largest model %u vertices, %u indices\n", textures.size(), maxVertices, maxIndices);
    u32 maxDensity = 0;
    for (const pack::GrassEffect& e : tables.effects()) {
        maxDensity = std::max<u32>(maxDensity, e.density);
    }

    const u32 frillDensity = static_cast<u32>(std::stoul(option(options, "density", "16")));
    JobSystem jobs;
    struct MapCensus {
        u32 tiles = 0, grassTiles = 0, chunks = 0, growing = 0, brokenChunks = 0, shadowed = 0;
        u64 doodads = 0, triangles = 0, dropped = 0, shaded = 0;
        u32 maxPerChunk = 0;
        u64 bytes = 0;
        u32 effectIdsHigh = 0;  // MCLY effectId past 16 bits
        u32 layerPastCount = 0; // cells whose layer index is past the chunk's layers
    };
    u32 failures = 0;
    for (const MapInfo& map : listMaps(mpq)) {
        if (map.wdt.tileCount == 0 || !mapSelected(map.directory, option(options, "maps"))) {
            continue;
        }
        std::vector<std::pair<u32, u32>> tiles;
        for (u32 ty = 0; ty < 64; ++ty) {
            for (u32 tx = 0; tx < 64; ++tx) {
                if (map.wdt.hasTile(tx, ty)) {
                    tiles.emplace_back(tx, ty);
                }
            }
        }
        MapCensus census;
        std::mutex lock;
        std::atomic<u32> failed{0};
        jobs.parallelFor(static_cast<u32>(tiles.size()), 4, [&](u32 begin, u32 end) {
            MapCensus local;
            std::vector<grass::PlacedGrass> placed(grass::maxPerChunk(frillDensity, maxDensity));
            for (u32 i = begin; i < end; ++i) {
                const auto [tx, ty] = tiles[i];
                Result<std::vector<u8>> bytes = mpq.read(formats::adtPath(map.directory, tx, ty));
                Result<formats::Adt> adt = bytes ? formats::parseAdt(bytes.value()) : Result<formats::Adt>(bytes.error());
                if (!adt) {
                    ++failed;
                    continue;
                }
                ++local.tiles;
                u32 grows = 0;
                const std::vector<u8> blob = tables.tileBlob(adt.value(), tx, ty, &grows);
                local.bytes += blob.size();
                local.grassTiles += blob.empty() ? 0 : 1;
                for (u32 c = 0; c < adt.value().chunks.size(); ++c) {
                    const formats::AdtChunk& chunk = adt.value().chunks[c];
                    ++local.chunks;
                    for (u32 k = 0; k < std::min<u32>(chunk.layerCount, 4); ++k) {
                        local.effectIdsHigh += chunk.layers[k].effectId > 0xFFFF ? 1 : 0;
                    }
                    for (u32 row = 0; row < 8; ++row) {
                        for (u32 column = 0; column < 8; ++column) {
                            local.layerPastCount += ((chunk.cellLayers[row] >> (column * 2)) & 3u) >= chunk.layerCount;
                        }
                    }
                    const pack::GrassChunk record = tables.chunkRecord(chunk, chunk.hasShadow ? 0 : pack::kNoGrassShadow);
                    if ((record.flags & pack::kGrassChunkGrows) == 0) {
                        bool named = false;
                        for (u32 k = 0; k < 4; ++k) {
                            named |= record.effects[k] != pack::kNoGrassEffect &&
                                     (tables.effects()[record.effects[k]].flags & pack::kGrassEffectBroken) != 0;
                        }
                        local.brokenChunks += named ? 1 : 0;
                        continue;
                    }
                    ++local.growing;
                    local.shadowed += chunk.hasShadow ? 1 : 0;
                    f32 heights[pack::kChunkVertices];
                    absoluteHeights(chunk, heights);
                    grass::ChunkInput input;
                    input.chunk = &record;
                    input.shadowBits = chunk.hasShadow ? chunk.shadowBits.data() : nullptr;
                    input.heights = heights;
                    input.seed = grass::chunkSeed(tx, ty, c % 16, c / 16);
                    grass::PlacementStats stats;
                    const u32 count =
                        grass::placeChunk(input, tables.effects(), tables.models(), frillDensity, placed, &stats);
                    local.doodads += count;
                    local.dropped += stats.dropped;
                    local.maxPerChunk = std::max(local.maxPerChunk, count);
                    for (u32 p = 0; p < count; ++p) {
                        local.triangles += tables.models()[placed[p].model].indexCount / 3;
                        local.shaded += placed[p].shaded;
                    }
                }
            }
            std::lock_guard<std::mutex> guard(lock);
            census.tiles += local.tiles;
            census.grassTiles += local.grassTiles;
            census.chunks += local.chunks;
            census.growing += local.growing;
            census.brokenChunks += local.brokenChunks;
            census.shadowed += local.shadowed;
            census.doodads += local.doodads;
            census.triangles += local.triangles;
            census.dropped += local.dropped;
            census.shaded += local.shaded;
            census.maxPerChunk = std::max(census.maxPerChunk, local.maxPerChunk);
            census.bytes += local.bytes;
            census.effectIdsHigh += local.effectIdsHigh;
            census.layerPastCount += local.layerPastCount;
        });
        failures += failed.load();
        if (census.growing == 0) {
            continue;
        }
        std::printf("%-22s %4u tiles (%3u with grass) | %6u of %6u chunks grow, %4u never (missing doodad), %5u "
                    "shadowed | frillDensity %u: %.1f doodads/chunk (max %u), %.1f tris/chunk, %.1f%% shaded, %llu "
                    "dropped | %.1f MB raw | %u effectIds > 16 bits, %u cells past the layers\n",
                    map.directory.c_str(), census.tiles, census.grassTiles, census.growing, census.chunks,
                    census.brokenChunks, census.shadowed, frillDensity,
                    static_cast<f64>(census.doodads) / census.growing, census.maxPerChunk,
                    static_cast<f64>(census.triangles) / census.growing,
                    census.doodads ? 100.0 * static_cast<f64>(census.shaded) / static_cast<f64>(census.doodads) : 0.0,
                    static_cast<unsigned long long>(census.dropped), static_cast<f64>(census.bytes) / 1e6,
                    census.effectIdsHigh, census.layerPastCount);
    }
    VD_LOG_INFO("scan-grass: %u ADTs unreadable, %.1f s", failures, secondsBetween(start, Clock::now()));
    return failures == 0 ? 0 : 1;
}

int bakeGrass(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outPath) {
    const Clock::time_point start = Clock::now();
    LoadedTables loaded = loadTables(mpq);
    if (!loaded.ok) {
        return 1;
    }
    const bake::GrassTables& tables = loaded.tables;
    bake::PackWriter writer;
    if (Result<void> created = writer.create(outPath, kBakerVersion); !created) {
        VD_LOG_ERROR("%s", created.error().message.c_str());
        return 1;
    }
    const std::vector<u8> models = tables.modelsBlob();
    u32 failures = 0;
    if (Result<void> added = writer.add(pack::kGrassModelsAsset, pack::AssetType::GrassModels, models, true);
        !added) {
        VD_LOG_ERROR("%s", added.error().message.c_str());
        ++failures;
    }
    const BakeCache cache(outPath.parent_path() / ".cache" / "grass");
    JobSystem jobs;
    u32 tileCount = 0;
    u64 growing = 0;
    for (const MapInfo& map : listMaps(mpq)) {
        if (map.wdt.tileCount == 0) {
            continue;
        }
        struct Tile {
            u32 tx = 0, ty = 0;
            std::vector<u8> blob;
        };
        std::vector<Tile> tiles;
        for (u32 ty = 0; ty < 64; ++ty) {
            for (u32 tx = 0; tx < 64; ++tx) {
                if (map.wdt.hasTile(tx, ty)) {
                    tiles.push_back({tx, ty, {}});
                }
            }
        }
        std::atomic<u32> failed{0};
        std::atomic<u64> grows{0};
        jobs.parallelFor(static_cast<u32>(tiles.size()), 4, [&](u32 begin, u32 end) {
            for (u32 i = begin; i < end; ++i) {
                Tile& tile = tiles[i];
                Result<std::vector<u8>> bytes = mpq.read(formats::adtPath(map.directory, tile.tx, tile.ty));
                if (!bytes) {
                    ++failed;
                    continue;
                }
                const u32 settings[] = {bake::kGrassBakeVersion, pack::kGrassFormatVersion, tile.tx, tile.ty};
                const u64 key = fnv1a64(settings, sizeof(settings),
                                        fnv1a64(bytes.value().data(), bytes.value().size(), loaded.hash));
                std::vector<u8> cached;
                if (cache.load(key, cached)) {
                    // One byte for "nothing grows here", else the blob.
                    if (cached.size() > 1) {
                        tile.blob = std::move(cached);
                        grows += 1;
                    }
                    continue;
                }
                Result<formats::Adt> adt = formats::parseAdt(bytes.value());
                if (!adt) {
                    ++failed;
                    continue;
                }
                u32 chunks = 0;
                tile.blob = tables.tileBlob(adt.value(), tile.tx, tile.ty, &chunks);
                grows += tile.blob.empty() ? 0 : 1;
                cache.store(key, tile.blob.empty() ? std::vector<u8>{0} : tile.blob);
            }
        });
        const std::string mapLower = normalizeAssetPath(map.directory);
        for (const Tile& tile : tiles) {
            if (tile.blob.empty()) {
                continue;
            }
            char path[128];
            pack::grassTilePath(path, sizeof(path), mapLower.c_str(), tile.tx, tile.ty);
            if (Result<void> added = writer.add(path, pack::AssetType::GrassTile, tile.blob, true); !added) {
                VD_LOG_ERROR("%s", added.error().message.c_str());
                ++failures;
            }
            ++tileCount;
        }
        failures += failed.load();
        growing += grows.load();
    }
    if (Result<void> finished = writer.finish(); !finished) {
        VD_LOG_ERROR("%s", finished.error().message.c_str());
        return 1;
    }
    VD_LOG_INFO("grass: %zu models, %zu effects (%u broken), %u tiles with grass, %u failures | %.1f MB raw -> %.1f MB "
                "| %.1f s -> %s",
                tables.models().size(), tables.effects().size(), tables.brokenEffects(), tileCount, failures,
                static_cast<f64>(writer.rawBytes()) / 1e6, static_cast<f64>(writer.bytesWritten()) / 1e6,
                secondsBetween(start, Clock::now()), outPath.string().c_str());
    (void)growing;
    return failures == 0 ? 0 : 1;
}

} // namespace vd::baker
