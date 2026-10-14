#include "BakerCommon.h"

#include "bake/PackWriter.h"
#include "bake/TerrainBaker.h"
#include "bake/TextureBaker.h"
#include "core/Clock.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "formats/Adt.h"
#include "formats/Blp.h"
#include "formats/Dbc.h"
#include "jobs/JobSystem.h"
#include "pack/TerrainFormat.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace vd::baker {

std::vector<MapInfo> listMaps(const formats::MpqArchiveSet& mpq) {
    std::vector<MapInfo> maps;
    for (const std::string& path : mpq.list("World\\Maps\\*.wdt")) {
        // World\Maps\<dir>\<dir>.wdt
        const usize first = path.find('\\', path.find('\\') + 1);
        const usize second = path.find('\\', first + 1);
        if (first == std::string::npos || second == std::string::npos) {
            continue;
        }
        Result<std::vector<u8>> bytes = mpq.read(path);
        if (!bytes) {
            VD_LOG_WARN("%s: %s", path.c_str(), bytes.error().message.c_str());
            continue;
        }
        Result<formats::Wdt> wdt = formats::parseWdt(bytes.value());
        if (!wdt) {
            VD_LOG_WARN("%s: %s", path.c_str(), wdt.error().message.c_str());
            continue;
        }
        maps.push_back({path.substr(first + 1, second - first - 1), std::move(wdt.value())});
    }
    std::sort(maps.begin(), maps.end(), [](const MapInfo& a, const MapInfo& b) { return a.directory < b.directory; });
    return maps;
}

bool mapSelected(const std::string& directory, const std::string& filter) {
    if (filter.empty() || filter == "all") {
        return true;
    }
    const std::string name = normalizeAssetPath(directory);
    usize start = 0;
    while (start <= filter.size()) {
        const usize end = std::min(filter.find(',', start), filter.size());
        if (normalizeAssetPath(filter.substr(start, end - start)) == name) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

namespace {

struct TileRef {
    u32 map;
    u32 tx;
    u32 ty;
};

// Mean agreement between the stored MCNR normals and normals derived from MCVT heights, under the
// documented axis convention (column -> -Y, row -> -X) and the transposed one. Only the documented
// one should be close to 1 on sloped terrain.
struct NormalCheck {
    f64 documented = 0.0;
    f64 transposed = 0.0;
    u64 samples = 0;
};

void checkNormals(const formats::AdtChunk& chunk, NormalCheck& out) {
    using formats::AdtChunk;
    constexpr f32 u = formats::kUnitSize;
    auto h = [&](u32 column, u32 row) { return chunk.heights[AdtChunk::outerVertex(column, row)]; };
    for (u32 row = 1; row < 8; ++row) {
        for (u32 column = 1; column < 8; ++column) {
            const auto& n = chunk.normals[AdtChunk::outerVertex(column, row)];
            const f32 len = std::sqrt(f32(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]));
            if (len == 0.0f) {
                continue;
            }
            const f32 dRow = (h(column, row + 1) - h(column, row - 1)) / (2.0f * u);
            const f32 dColumn = (h(column + 1, row) - h(column - 1, row)) / (2.0f * u);
            // Documented: X decreases with row, Y decreases with column -> dh/dX = -dRow, dh/dY = -dColumn.
            const f32 a[3] = {dRow, dColumn, 1.0f};
            const f32 b[3] = {dColumn, dRow, 1.0f};
            const f32 la = std::sqrt(a[0] * a[0] + a[1] * a[1] + 1.0f);
            const f32 lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + 1.0f);
            out.documented += (a[0] * n[0] + a[1] * n[1] + a[2] * n[2]) / (la * len);
            out.transposed += (b[0] * n[0] + b[1] * n[1] + b[2] * n[2]) / (lb * len);
            ++out.samples;
        }
    }
}


// How a terrain texture's alpha is baked. Phase 11: with "Terrain Highlights" 1.12 draws the `<name>_s.blp` twin of a
// terrain texture (the same colors, a specular mask in its alpha); the ones without a twin shine nowhere.
enum class TerrainAlpha : u32 { Keep = 0, SpecularTwin = 1, NoSpecular = 2 };

u64 terrainTextureCacheKey(const std::vector<u8>& blpBytes, TerrainAlpha alpha) {
    constexpr u32 kTerrainTag = 0x54455252u; // "TERR": distinct from the texture pack's cache keys
    const u32 settings[] = {kTerrainTag, static_cast<u32>(pack::TextureFormat::Astc6x6), bake::kTextureEncoderVersion,
                            pack::kTerrainTextureSize, static_cast<u32>(alpha)};
    return fnv1a64(settings, sizeof(settings), fnv1a64(blpBytes.data(), blpBytes.size()));
}

// The specular twin of a terrain texture ("Tileset\Elwynn\ElwynnGrassBase.blp" -> "..._s.blp").
std::string specularTwin(const std::string& path) {
    const usize dot = path.rfind('.');
    return dot == std::string::npos ? path + "_s" : path.substr(0, dot) + "_s" + path.substr(dot);
}

// Every terrain texture of the map, resampled to kTerrainTextureSize and encoded as one 2D array. `specular`: the
// textures of the terrain (their specular twins when they have one), not the liquids' frames.
Result<std::vector<u8>> bakeTerrainTextures(const formats::MpqArchiveSet& mpq, const std::vector<std::string>& paths,
                                            const BakeCache& cache, JobSystem& jobs, u32& cacheHits,
                                            bool specular = false) {
    std::vector<std::vector<u8>> layers(paths.size());
    std::vector<std::string> errors(paths.size());
    std::atomic<u32> hits{0};
    jobs.parallelFor(static_cast<u32>(paths.size()), 1, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            TerrainAlpha alpha = TerrainAlpha::Keep;
            Result<std::vector<u8>> bytes = Error{"not read"};
            if (specular) {
                const std::string twin = specularTwin(paths[i]);
                if (mpq.contains(twin)) {
                    bytes = mpq.read(twin);
                    alpha = TerrainAlpha::SpecularTwin;
                }
            }
            if (!bytes) {
                bytes = mpq.read(paths[i]);
                alpha = specular ? TerrainAlpha::NoSpecular : TerrainAlpha::Keep;
            }
            if (!bytes) {
                errors[i] = paths[i] + ": " + bytes.error().message;
                continue;
            }
            const u64 key = terrainTextureCacheKey(bytes.value(), alpha);
            if (cache.load(key, layers[i])) {
                ++hits;
                continue;
            }
            Result<formats::BlpImage> image = formats::decodeBlp(bytes.value());
            if (!image) {
                errors[i] = paths[i] + ": " + image.error().message;
                continue;
            }
            formats::BlpImage square = bake::resampleSquare(image.value(), pack::kTerrainTextureSize);
            if (alpha == TerrainAlpha::NoSpecular) {
                for (std::vector<u8>& mip : square.mips) { // no twin: no highlights on it
                    for (usize t = 3; t < mip.size(); t += 4) {
                        mip[t] = 0;
                    }
                }
                square.alphaDepth = 8;
            }
            Result<std::vector<u8>> blob = bake::bakeTexture(square, pack::TextureFormat::Astc6x6);
            if (!blob) {
                errors[i] = paths[i] + ": " + blob.error().message;
                continue;
            }
            layers[i] = std::move(blob.value());
            cache.store(key, layers[i]);
        }
    });
    cacheHits = hits.load();
    for (const std::string& error : errors) {
        if (!error.empty()) {
            return Error{error};
        }
    }
    return bake::buildTextureArray(layers);
}

// Animated liquid frames, in LiquidKind order (river, ocean, magma, slime): the same for every map, so (phase 12)
// one pack holds them instead of each world pack.
int bakeLiquids(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outDir, JobSystem& jobs) {
    std::vector<std::string> liquidFrames;
    for (const char* pattern : {"XTextures/river/lake_a.%u.blp", "XTextures/ocean/ocean_h.%u.blp",
                                "XTextures/lava/lava.%u.blp", "XTextures/slime/slime.%u.blp"}) {
        for (u32 frame = 1; frame <= pack::kLiquidFrames; ++frame) {
            char path[64];
            std::snprintf(path, sizeof(path), pattern, frame);
            liquidFrames.push_back(path);
        }
    }
    u32 cacheHits = 0;
    const BakeCache cache(outDir / ".cache" / "terrain_textures");
    Result<std::vector<u8>> liquidArray = bakeTerrainTextures(mpq, liquidFrames, cache, jobs, cacheHits);
    if (!liquidArray) {
        VD_LOG_ERROR("Liquid textures: %s", liquidArray.error().message.c_str());
        return 1;
    }
    const std::filesystem::path outPath = outDir / pack::kLiquidPackFileName;
    bake::PackWriter writer;
    Result<void> written = writer.create(outPath, kBakerVersion);
    if (written) {
        written = writer.add(pack::kLiquidTexturesAsset, pack::AssetType::Texture, liquidArray.value());
    }
    if (written) {
        written = writer.finish();
    }
    if (!written) {
        VD_LOG_ERROR("%s", written.error().message.c_str());
        return 1;
    }
    VD_LOG_INFO("Liquids: %zu frames (%u from cache) | %.1f MB -> %s", liquidFrames.size(), cacheHits,
                static_cast<double>(writer.bytesWritten()) / 1e6, outPath.string().c_str());
    return 0;
}

int bakeMap(const formats::MpqArchiveSet& mpq, const MapInfo& map, const std::filesystem::path& outDir,
            JobSystem& jobs) {
    const Clock::time_point start = Clock::now();
    std::vector<TileRef> tiles;
    for (u32 ty = 0; ty < formats::kMapTiles; ++ty) {
        for (u32 tx = 0; tx < formats::kMapTiles; ++tx) {
            if (map.wdt.hasTile(tx, ty)) {
                tiles.push_back({0, tx, ty});
            }
        }
    }

    // Pass 1: the map's terrain textures, sorted, so layer indices do not depend on thread timing.
    std::mutex mutex;
    std::vector<std::string> failures;
    std::set<std::string> textureSet;
    jobs.parallelFor(static_cast<u32>(tiles.size()), 4, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            const std::string path = formats::adtPath(map.directory, tiles[i].tx, tiles[i].ty);
            Result<std::vector<u8>> bytes = mpq.read(path);
            Result<formats::Adt> adt = bytes ? formats::parseAdt(bytes.value()) : Result<formats::Adt>(bytes.error());
            std::lock_guard<std::mutex> lock(mutex);
            if (!adt) {
                failures.push_back(path + ": " + adt.error().message);
                continue;
            }
            for (const std::string& texture : adt.value().textures) {
                textureSet.insert(normalizeAssetPath(texture));
            }
        }
    });
    for (const std::string& failure : failures) {
        VD_LOG_ERROR("FAILED %s", failure.c_str());
    }
    if (!failures.empty()) {
        return 1;
    }
    if (textureSet.empty()) { // phase 12: a map without ADTs (one WMO) still needs an array to bind
        textureSet.insert(normalizeAssetPath("Tileset\\Generic\\Grey.blp"));
    }
    const std::vector<std::string> textures(textureSet.begin(), textureSet.end());
    if (textures.size() >= pack::kNoTerrainLayer) {
        VD_LOG_ERROR("%s uses %zu terrain textures (max %u)", map.directory.c_str(), textures.size(),
                     pack::kNoTerrainLayer - 1u);
        return 1;
    }
    bake::TerrainTextureIndex textureIndex;
    for (u32 i = 0; i < textures.size(); ++i) {
        textureIndex.emplace(textures[i], i);
    }

    const std::filesystem::path outPath = outDir / pack::worldPackFileName(map.directory);
    bake::PackWriter writer;
    if (Result<void> created = writer.create(outPath, kBakerVersion); !created) {
        VD_LOG_ERROR("%s", created.error().message.c_str());
        return 1;
    }

    const std::string mapPrefix = "world/maps/" + normalizeAssetPath(map.directory) + "/";
    u32 cacheHits = 0;
    const BakeCache cache(outDir / ".cache" / "terrain_textures");
    Result<std::vector<u8>> textureArray = bakeTerrainTextures(mpq, textures, cache, jobs, cacheHits, true);
    if (!textureArray) {
        VD_LOG_ERROR("%s: %s", map.directory.c_str(), textureArray.error().message.c_str());
        return 1;
    }
    {
        // Phase 11: tells the runtime this array's alpha is the specular mask (older packs: no highlights).
        pack::TextureHeader arrayHeader;
        std::memcpy(&arrayHeader, textureArray.value().data(), sizeof(arrayHeader));
        arrayHeader.flags |= pack::kTextureSpecularMask;
        std::memcpy(textureArray.value().data(), &arrayHeader, sizeof(arrayHeader));
    }
    if (Result<void> added = writer.add(mapPrefix + "terraintextures", pack::AssetType::Texture, textureArray.value());
        !added) {
        VD_LOG_ERROR("%s", added.error().message.c_str());
        return 1;
    }

    // Pass 2: tiles.
    std::array<pack::TerrainMapTile, pack::kMapTiles * pack::kMapTiles> summaries{};
    u32 tilesWithHoles = 0;
    jobs.parallelFor(static_cast<u32>(tiles.size()), 1, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            const TileRef& tile = tiles[i];
            const std::string path = formats::adtPath(map.directory, tile.tx, tile.ty);
            const Result<void> baked = [&]() -> Result<void> {
                Result<std::vector<u8>> bytes = mpq.read(path);
                if (!bytes) {
                    return bytes.error();
                }
                Result<formats::Adt> adt = formats::parseAdt(bytes.value());
                if (!adt) {
                    return adt.error();
                }
                Result<bake::BakedTerrainTile> result =
                    bake::bakeTerrainTile(adt.value(), tile.tx, tile.ty, textureIndex);
                if (!result) {
                    return result.error();
                }
                VD_TRY(writer.add(path, pack::AssetType::TerrainTile, result.value().blob, true));
                std::lock_guard<std::mutex> lock(mutex);
                summaries[tile.ty * pack::kMapTiles + tile.tx] = result.value().summary;
                tilesWithHoles += (result.value().summary.flags & pack::kMapTileHasHoles) ? 1u : 0u;
                return {};
            }();
            if (!baked) {
                std::lock_guard<std::mutex> lock(mutex);
                failures.push_back(path + ": " + baked.error().message);
            }
        }
    });
    for (const std::string& failure : failures) {
        VD_LOG_ERROR("FAILED %s", failure.c_str());
    }
    if (!failures.empty()) {
        return 1;
    }

    // The horizon: required with terrain; a map made of one WMO may come without a WDL (nothing to see outside).
    Result<std::vector<u8>> wdlBytes = mpq.read(formats::wdlPath(map.directory));
    Result<formats::Wdl> wdl = wdlBytes ? formats::parseWdl(wdlBytes.value()) : Result<formats::Wdl>(wdlBytes.error());
    if (!wdl && !tiles.empty()) {
        VD_LOG_ERROR("%s: %s", formats::wdlPath(map.directory).c_str(), wdl.error().message.c_str());
        return 1;
    }
    if (wdl) {
        if (Result<void> added = writer.add(mapPrefix + "horizon", pack::AssetType::TerrainHorizon,
                                            bake::bakeTerrainHorizon(wdl.value()), true);
            !added) {
            VD_LOG_ERROR("%s", added.error().message.c_str());
            return 1;
        }
    }

    // Phase 12: the WDT's own WMO (the dungeon itself on maps without ADTs).
    const std::vector<u8> objects = bake::bakeMapObjects(map.wdt);
    if (!objects.empty()) {
        if (Result<void> added = writer.add(mapPrefix + "objects", pack::AssetType::TerrainObjects, objects);
            !added) {
            VD_LOG_ERROR("%s", added.error().message.c_str());
            return 1;
        }
    }

    const std::vector<u8> mapBlob = bake::bakeTerrainMap(summaries, static_cast<u32>(textures.size()));
    if (Result<void> added = writer.add(formats::wdtPath(map.directory), pack::AssetType::TerrainMap, mapBlob);
        !added) {
        VD_LOG_ERROR("%s", added.error().message.c_str());
        return 1;
    }
    if (Result<void> finished = writer.finish(); !finished) {
        VD_LOG_ERROR("%s", finished.error().message.c_str());
        return 1;
    }
    VD_LOG_INFO("%s: %zu tiles (%u with holes), %s%zu textures (%u from cache) | %.1f MB raw -> %.1f MB | %.1f s -> %s",
                map.directory.c_str(), tiles.size(), tilesWithHoles, objects.empty() ? "" : "global WMO, ",
                textures.size(), cacheHits,
                static_cast<double>(writer.rawBytes()) / 1e6, static_cast<double>(writer.bytesWritten()) / 1e6,
                secondsBetween(start, Clock::now()), outPath.string().c_str());
    return 0;
}

} // namespace

int bakeWorld(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outDir, const std::string& mapFilter) {
    int result = 0;
    u32 baked = 0;
    JobSystem jobs;
    jobs.init();
    result |= bakeLiquids(mpq, outDir, jobs);
    for (const MapInfo& map : listMaps(mpq)) {
        // Phase 12: also the maps made of one WMO (most dungeons); a WDT with neither is an empty test map.
        if ((map.wdt.tileCount == 0 && !map.wdt.hasWmoPlacement) || !mapSelected(map.directory, mapFilter)) {
            continue;
        }
        result |= bakeMap(mpq, map, outDir, jobs);
        ++baked;
    }
    jobs.shutdown();
    if (baked == 0) {
        VD_LOG_ERROR("No map matches '%s'", mapFilter.c_str());
        return 1;
    }
    return result;
}

int commandScanTerrain(const Options& options) {
    formats::MpqArchiveSet mpq;
    if (!openArchives(options, mpq)) {
        return 1;
    }
    const Clock::time_point start = Clock::now();
    const std::vector<MapInfo> maps = listMaps(mpq);
    std::vector<TileRef> tiles;
    for (u32 m = 0; m < maps.size(); ++m) {
        for (u32 ty = 0; ty < formats::kMapTiles; ++ty) {
            for (u32 tx = 0; tx < formats::kMapTiles; ++tx) {
                if (maps[m].wdt.hasTile(tx, ty)) {
                    tiles.push_back({m, tx, ty});
                }
            }
        }
    }

    std::mutex mutex;
    std::vector<std::string> failures;
    std::vector<std::set<std::string>> texturesByMap(maps.size());
    std::array<u64, 5> layerHistogram{};
    std::array<u64, 4> liquidsByType{};
    u64 chunksWithHoles = 0, tilesWithHoles = 0, layersWithoutAlpha = 0, animatedLayers = 0, overbrightLayers = 0;
    u64 chunksWithShadow = 0, multiLiquidChunks = 0, doodads = 0, wmos = 0;
    u32 maxTexturesPerTile = 0;
    f32 minHeight = 1e9f, maxHeight = -1e9f;
    NormalCheck normals;

    JobSystem jobs;
    jobs.init();
    jobs.parallelFor(static_cast<u32>(tiles.size()), 4, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            const TileRef& tile = tiles[i];
            const std::string path = formats::adtPath(maps[tile.map].directory, tile.tx, tile.ty);
            Result<std::vector<u8>> bytes = mpq.read(path);
            Result<formats::Adt> adt = bytes ? formats::parseAdt(bytes.value()) : Result<formats::Adt>(bytes.error());
            std::lock_guard<std::mutex> lock(mutex);
            if (!adt) {
                failures.push_back(path + ": " + adt.error().message);
                continue;
            }
            const formats::Adt& a = adt.value();
            texturesByMap[tile.map].insert(a.textures.begin(), a.textures.end());
            maxTexturesPerTile = std::max(maxTexturesPerTile, static_cast<u32>(a.textures.size()));
            doodads += a.doodads.size();
            wmos += a.wmoPlacements.size();
            bool holes = false;
            for (const formats::AdtChunk& chunk : a.chunks) {
                ++layerHistogram[chunk.layerCount];
                holes |= chunk.holes != 0;
                chunksWithHoles += chunk.holes != 0;
                chunksWithShadow += chunk.hasShadow;
                multiLiquidChunks += chunk.liquids.size() > 1;
                for (const formats::AdtLiquid& liquid : chunk.liquids) {
                    ++liquidsByType[static_cast<u32>(liquid.type)];
                }
                for (u32 l = 0; l < chunk.layerCount; ++l) {
                    const u32 flags = chunk.layers[l].flags;
                    layersWithoutAlpha += l > 0 && !(flags & formats::kLayerUseAlpha);
                    animatedLayers += (flags & 0x40) != 0;
                    overbrightLayers += (flags & formats::kLayerOverbright) != 0;
                }
                for (f32 height : chunk.heights) {
                    minHeight = std::min(minHeight, chunk.position[2] + height);
                    maxHeight = std::max(maxHeight, chunk.position[2] + height);
                }
                checkNormals(chunk, normals);
            }
            tilesWithHoles += holes;
        }
    });
    jobs.shutdown();

    // Terrain textures: sizes (a texture array needs one size) and missing files.
    std::set<std::string> allTextures;
    for (u32 m = 0; m < maps.size(); ++m) {
        if (!texturesByMap[m].empty()) {
            std::printf("%-24s %4u tiles %4zu textures\n", maps[m].directory.c_str(), maps[m].wdt.tileCount,
                        texturesByMap[m].size());
        }
        for (const std::string& t : texturesByMap[m]) {
            allTextures.insert(normalizeAssetPath(t));
        }
    }
    std::map<std::string, u32> sizes;
    u32 missingTextures = 0;
    for (const std::string& texture : allTextures) {
        Result<std::vector<u8>> bytes = mpq.read(texture);
        if (!bytes || bytes.value().size() < 20) {
            ++missingTextures;
            std::printf("MISSING %s\n", texture.c_str());
            continue;
        }
        u32 wh[2];
        std::memcpy(wh, bytes.value().data() + 12, sizeof(wh));
        ++sizes[std::to_string(wh[0]) + "x" + std::to_string(wh[1])];
    }
    for (const auto& [size, count] : sizes) {
        std::printf("texture size %-9s %u\n", size.c_str(), count);
    }
    for (const std::string& failure : failures) {
        std::printf("FAILED %s\n", failure.c_str());
    }
    std::printf("layers per chunk: 0:%llu 1:%llu 2:%llu 3:%llu 4:%llu\n", (unsigned long long)layerHistogram[0],
                (unsigned long long)layerHistogram[1], (unsigned long long)layerHistogram[2],
                (unsigned long long)layerHistogram[3], (unsigned long long)layerHistogram[4]);
    std::printf("holes: %llu chunks in %llu tiles | shadow chunks %llu | layers w/o alpha %llu, animated %llu, "
                "overbright %llu\n",
                (unsigned long long)chunksWithHoles, (unsigned long long)tilesWithHoles,
                (unsigned long long)chunksWithShadow, (unsigned long long)layersWithoutAlpha,
                (unsigned long long)animatedLayers, (unsigned long long)overbrightLayers);
    std::printf("liquids: river %llu ocean %llu magma %llu slime %llu (chunks with >1: %llu)\n",
                (unsigned long long)liquidsByType[0], (unsigned long long)liquidsByType[1],
                (unsigned long long)liquidsByType[2], (unsigned long long)liquidsByType[3],
                (unsigned long long)multiLiquidChunks);
    std::printf("heights %.1f .. %.1f | max textures per tile %u | doodads %llu, wmos %llu\n", minHeight, maxHeight,
                maxTexturesPerTile, (unsigned long long)doodads, (unsigned long long)wmos);
    std::printf("normals vs heights: documented axes %.4f, transposed %.4f (%llu samples)\n",
                normals.documented / std::max<u64>(1, normals.samples),
                normals.transposed / std::max<u64>(1, normals.samples), (unsigned long long)normals.samples);
    VD_LOG_INFO("%zu tiles in %zu maps, %zu failed, %zu unique terrain textures (%u missing), %.1f s", tiles.size(),
                maps.size(), failures.size(), allTextures.size(), missingTextures,
                secondsBetween(start, Clock::now()));
    return failures.empty() ? 0 : 1;
}


// Phase 12: every map of the client -- Map.dbc's id, name and instance type next to what its WDT holds (ADT tiles,
// a global WMO, a WDL) and which world pack the bake makes of it.
int commandScanMaps(const Options& options) {
    formats::MpqArchiveSet mpq;
    if (!openArchives(options, mpq)) {
        return 1;
    }
    struct MapRow {
        u32 id = 0;
        u32 instanceType = 0;
        std::string name;
    };
    std::map<std::string, MapRow> rows; // by directory, case-insensitive
    if (Result<std::vector<u8>> bytes = mpq.read("DBFilesClient\\Map.dbc"); bytes) {
        if (Result<formats::DbcFile> dbc = formats::DbcFile::parse(std::move(bytes.value())); dbc) {
            // Map.dbc (5875, 42 fields): 0 id, 1 directory, 2 instance type, 4 name (enUS).
            for (u32 r = 0; r < dbc.value()->recordCount(); ++r) {
                rows[normalizeAssetPath(std::string(dbc.value()->getString(r, 1)))] = {
                    dbc.value()->getU32(r, 0), dbc.value()->getU32(r, 2), std::string(dbc.value()->getString(r, 4))};
            }
        }
    }
    const char* kinds[] = {"world", "party", "raid", "pvp"};
    u32 terrain = 0, wmoOnly = 0, empty = 0;
    std::printf("%-5s %-24s %-6s %5s  %-3s  %-48s %s\n", "id", "directory", "type", "tiles", "wdl", "global WMO",
                "name");
    for (const MapInfo& map : listMaps(mpq)) {
        const auto row = rows.find(normalizeAssetPath(map.directory));
        const bool known = row != rows.end();
        const bool wdl = mpq.contains(formats::wdlPath(map.directory));
        std::printf("%-5s %-24s %-6s %5u  %-3s  %-48s %s\n", known ? std::to_string(row->second.id).c_str() : "-",
                    map.directory.c_str(), known && row->second.instanceType < 4 ? kinds[row->second.instanceType] : "-",
                    map.wdt.tileCount, wdl ? "yes" : "no", map.wdt.hasWmoPlacement ? map.wdt.wmoName.c_str() : "",
                    known ? row->second.name.c_str() : "(not in Map.dbc)");
        if (map.wdt.hasWmoPlacement) {
            const formats::WdtWmoPlacement& p = map.wdt.wmoPlacement;
            std::printf("      MODF position %.2f %.2f %.2f rotation %.1f %.1f %.1f extents %.1f %.1f %.1f .. %.1f %.1f "
                        "%.1f set %u flags 0x%x\n",
                        p.position[0], p.position[1], p.position[2], p.rotation[0], p.rotation[1], p.rotation[2],
                        p.extentsMin[0], p.extentsMin[1], p.extentsMin[2], p.extentsMax[0], p.extentsMax[1],
                        p.extentsMax[2], p.doodadSet, p.flags);
        }
        terrain += map.wdt.tileCount > 0;
        wmoOnly += map.wdt.tileCount == 0 && map.wdt.hasWmoPlacement;
        empty += map.wdt.tileCount == 0 && !map.wdt.hasWmoPlacement;
    }
    VD_LOG_INFO("%u maps with terrain, %u made of one WMO, %u empty (not baked); %zu rows in Map.dbc", terrain, wmoOnly,
                empty, rows.size());
    return 0;
}

} // namespace vd::baker
