#include "BakerCommon.h"

#include "audio/SoundAsset.h"
#include "bake/PackWriter.h"
#include "bake/SoundBaker.h"
#include "core/Clock.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "formats/Adt.h"
#include "formats/Dbc.h"
#include "formats/Wav.h"
#include "jobs/JobSystem.h"
#include "pack/SoundFormat.h"

#include <algorithm>
#include <cstring>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace vd::baker {
namespace {

// SoundEntries.dbc (5875): 3..12 File[10], 23 DirectoryBase (checked against the client in tests/audio).
constexpr u32 kSoundFileField = 3;
constexpr u32 kSoundFileCount = 10;
constexpr u32 kSoundDirectoryField = 23;
// Sounds the client plays by file name, outside SoundEntries: the login screen's music (GlueXML).
constexpr const char* kExtraSounds[] = {"Sound\\Music\\GlueScreenMusic\\wow_main_theme.mp3"};

bool endsWith(const std::string& canonical, const char* suffix) {
    const usize n = std::char_traits<char>::length(suffix);
    return canonical.size() >= n && canonical.compare(canonical.size() - n, n, suffix) == 0;
}

} // namespace

// Every file SoundEntries names (directory + file), original spelling, deduplicated by canonical path. `entries`:
// how many references there are in total.
std::vector<std::string> soundPaths(const formats::MpqArchiveSet& mpq, u32& references) {
    std::map<std::string, std::string> byCanonical;
    references = 0;
    Result<std::vector<u8>> bytes = mpq.read("DBFilesClient\\SoundEntries.dbc");
    if (!bytes) {
        VD_LOG_ERROR("SoundEntries.dbc: %s", bytes.error().message.c_str());
        return {};
    }
    Result<formats::DbcFile> table = formats::DbcFile::parse(std::move(bytes.value()));
    if (!table || table.value()->fieldCount() <= kSoundDirectoryField) {
        VD_LOG_ERROR("SoundEntries.dbc: unexpected layout");
        return {};
    }
    const db::DbcView& view = table.value().view();
    for (u32 r = 0; r < view.recordCount(); ++r) {
        const std::string directory(view.getString(r, kSoundDirectoryField));
        for (u32 f = 0; f < kSoundFileCount; ++f) {
            const std::string_view file = view.getString(r, kSoundFileField + f);
            if (file.empty()) {
                continue;
            }
            const std::string path = audio::joinSoundPath(directory, file);
            byCanonical.emplace(normalizeAssetPath(path), path);
            ++references;
        }
    }
    for (const char* extra : kExtraSounds) {
        byCanonical.emplace(normalizeAssetPath(extra), extra);
    }
    std::vector<std::string> paths;
    paths.reserve(byCanonical.size());
    for (auto& [canonical, path] : byCanonical) {
        paths.push_back(path);
    }
    return paths;
}

namespace {

// --- Ground sounds: the footstep surface of every map's terrain (pack/SoundFormat.h "ground/<map>") ---------------

// GroundEffectTexture.dbc (5875): 6 TerrainType. TerrainType.dbc: 1 name, 4 SoundID (FootstepTerrainLookup's
// terrain). Checked against the client in tests/audio/test_sound_data.cpp.
constexpr u32 kGroundEffectTerrainField = 6;
constexpr u32 kTerrainTypeSoundField = 4;
constexpr u32 kGroundBakeVersion = 2; // 2: "iron" is not metal

// TerrainType ids.
constexpr u32 kTerrainDirt = 0, kTerrainMetal = 1, kTerrainStone = 2, kTerrainSnow = 3, kTerrainWood = 4,
              kTerrainGrass = 5, kTerrainLeaves = 6, kTerrainSand = 7, kTerrainSoggy = 8;

// Most layers carry no ground effect (only grass-spawning ones do): their texture's name says the surface
// ("Tileset\Elwynn\ElwynnCobblestoneBase.blp"). Dirt when nothing matches.
u32 terrainTypeFromName(std::string_view texture) {
    std::string name(texture.substr(texture.find_last_of("\\/") == std::string_view::npos
                                        ? 0
                                        : texture.find_last_of("\\/") + 1));
    std::transform(name.begin(), name.end(), name.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    const auto has = [&](std::initializer_list<const char*> words) {
        for (const char* word : words) {
            if (name.find(word) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    if (has({"snow", "ice", "frost"})) {
        return kTerrainSnow;
    }
    if (has({"sand", "beach", "dune"})) {
        return kTerrainSand;
    }
    if (has({"mud", "swamp", "bog", "marsh", "muck", "slime"})) {
        return kTerrainSoggy;
    }
    if (has({"leaf", "leaves", "needle", "forestfloor"})) {
        return kTerrainLeaves;
    }
    if (has({"grass", "moss", "flower", "fern", "weed", "clover", "lawn", "meadow"})) {
        return kTerrainGrass;
    }
    if (has({"plank", "wood", "bark"})) {
        return kTerrainWood;
    }
    if (has({"metal", "steel"})) { // not "iron": IronForge* textures are snow and rock
        return kTerrainMetal;
    }
    if (has({"rock", "stone", "cobble", "brick", "tile", "slate", "cliff", "marble", "pave", "crystal", "granite",
             "boulder", "pebble", "gravel", "road"})) {
        return name.find("road") != std::string::npos && name.find("stone") == std::string::npos &&
                       name.find("cobble") == std::string::npos
                   ? kTerrainDirt // the dirt roads of the countryside
                   : kTerrainStone;
    }
    return kTerrainDirt;
}

struct GroundTables {
    std::map<u32, u32> effectTerrain; // GroundEffectTexture id -> TerrainType id (non-zero ones)
    std::map<u32, u32> terrainSound;  // TerrainType id -> SoundID
    u64 hash = 0;                     // of both tables (the bake cache)
};

GroundTables groundTables(const formats::MpqArchiveSet& mpq) {
    GroundTables t;
    t.hash = kFnvOffset;
    if (Result<std::vector<u8>> bytes = mpq.read("DBFilesClient\\GroundEffectTexture.dbc"); bytes) {
        t.hash = fnv1a64(bytes.value().data(), bytes.value().size(), t.hash);
        if (Result<formats::DbcFile> table = formats::DbcFile::parse(std::move(bytes.value()));
            table && table.value()->fieldCount() > kGroundEffectTerrainField) {
            for (u32 r = 0; r < table.value()->recordCount(); ++r) {
                if (const u32 terrain = table.value()->getU32(r, kGroundEffectTerrainField); terrain != 0) {
                    t.effectTerrain[table.value()->getU32(r, 0)] = terrain;
                }
            }
        }
    }
    if (Result<std::vector<u8>> bytes = mpq.read("DBFilesClient\\TerrainType.dbc"); bytes) {
        t.hash = fnv1a64(bytes.value().data(), bytes.value().size(), t.hash);
        if (Result<formats::DbcFile> table = formats::DbcFile::parse(std::move(bytes.value()));
            table && table.value()->fieldCount() > kTerrainTypeSoundField) {
            for (u32 r = 0; r < table.value()->recordCount(); ++r) {
                t.terrainSound[table.value()->getU32(r, 0)] = table.value()->getU32(r, kTerrainTypeSoundField);
            }
        }
    }
    return t;
}

// One tile's kGroundTileBytes: per chunk and cell, the SoundID of the layer that shows most (the client's blend:
// layer k over the ones below with its alpha).
std::vector<u8> groundTile(const formats::Adt& adt, const GroundTables& tables) {
    std::vector<u8> out(pack::kGroundTileBytes, 0);
    constexpr u32 kCellTexels = formats::kAlphaSize / pack::kGroundCells;
    for (const formats::AdtChunk& chunk : adt.chunks) {
        const u32 layers = std::min<u32>(chunk.layerCount, 4);
        if (layers == 0 || chunk.indexX >= 16 || chunk.indexY >= 16) {
            continue;
        }
        u8 sounds[4] = {};
        for (u32 k = 0; k < layers; ++k) {
            const formats::AdtLayer& layer = chunk.layers[k];
            u32 terrain = kTerrainDirt;
            if (const auto effect = tables.effectTerrain.find(layer.effectId); effect != tables.effectTerrain.end()) {
                terrain = effect->second;
            } else if (layer.textureId < adt.textures.size()) {
                terrain = terrainTypeFromName(adt.textures[layer.textureId]);
            }
            const auto sound = tables.terrainSound.find(terrain);
            sounds[k] = static_cast<u8>(sound != tables.terrainSound.end() ? sound->second : 1);
        }
        u8* cells = out.data() + (chunk.indexY * 16 + chunk.indexX) * pack::kGroundCells * pack::kGroundCells;
        for (u32 cellRow = 0; cellRow < pack::kGroundCells; ++cellRow) {
            for (u32 cellColumn = 0; cellColumn < pack::kGroundCells; ++cellColumn) {
                f32 weights[4] = {};
                for (u32 r = cellRow * kCellTexels; r < (cellRow + 1) * kCellTexels; ++r) {
                    for (u32 c = cellColumn * kCellTexels; c < (cellColumn + 1) * kCellTexels; ++c) {
                        f32 remaining = 1.0f;
                        for (u32 k = layers - 1; k >= 1; --k) {
                            const f32 alpha = chunk.alpha[k - 1][r * formats::kAlphaSize + c] / 255.0f;
                            weights[k] += remaining * alpha;
                            remaining *= 1.0f - alpha;
                        }
                        weights[0] += remaining;
                    }
                }
                u32 best = 0;
                for (u32 k = 1; k < layers; ++k) {
                    best = weights[k] > weights[best] ? k : best;
                }
                cells[cellRow * pack::kGroundCells + cellColumn] = sounds[best];
            }
        }
    }
    return out;
}

// "ground/<map>" of every map with terrain. Returns the failures.
u32 bakeGroundSounds(const formats::MpqArchiveSet& mpq, bake::PackWriter& writer, const BakeCache& cache,
                     JobSystem& jobs, u32& tileCount, u64& bytes) {
    const GroundTables tables = groundTables(mpq);
    if (tables.terrainSound.empty()) {
        VD_LOG_ERROR("TerrainType.dbc: unreadable, no ground sounds");
        return 1;
    }
    u32 failures = 0;
    for (const MapInfo& map : listMaps(mpq)) {
        if (map.wdt.tileCount == 0) {
            continue;
        }
        struct Tile {
            u32 tx = 0, ty = 0;
            std::vector<u8> cells;
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
        jobs.parallelFor(static_cast<u32>(tiles.size()), 4, [&](u32 begin, u32 end) {
            for (u32 i = begin; i < end; ++i) {
                Tile& tile = tiles[i];
                Result<std::vector<u8>> adtBytes = mpq.read(formats::adtPath(map.directory, tile.tx, tile.ty));
                if (!adtBytes) {
                    ++failed;
                    continue;
                }
                const u32 settings[] = {kGroundBakeVersion, pack::kGroundCells};
                const u64 key = fnv1a64(settings, sizeof(settings),
                                        fnv1a64(adtBytes.value().data(), adtBytes.value().size(), tables.hash));
                if (cache.load(key, tile.cells) && tile.cells.size() == pack::kGroundTileBytes) {
                    continue;
                }
                Result<formats::Adt> adt = formats::parseAdt(adtBytes.value());
                if (!adt) {
                    ++failed;
                    tile.cells.assign(pack::kGroundTileBytes, 0);
                    continue;
                }
                tile.cells = groundTile(adt.value(), tables);
                cache.store(key, tile.cells);
            }
        });
        pack::GroundSoundsHeader header{};
        header.version = pack::kGroundSoundsVersion;
        header.tileCount = static_cast<u32>(tiles.size());
        std::fill(std::begin(header.tiles), std::end(header.tiles), pack::kNoGroundTile);
        std::vector<u8> blob(sizeof(header) + tiles.size() * pack::kGroundTileBytes, 0);
        for (u32 i = 0; i < tiles.size(); ++i) {
            header.tiles[tiles[i].ty * 64 + tiles[i].tx] = static_cast<u16>(i);
            if (tiles[i].cells.size() == pack::kGroundTileBytes) {
                std::memcpy(blob.data() + sizeof(header) + static_cast<usize>(i) * pack::kGroundTileBytes,
                            tiles[i].cells.data(), pack::kGroundTileBytes);
            }
        }
        std::memcpy(blob.data(), &header, sizeof(header));
        if (Result<void> added =
                writer.add("ground/" + normalizeAssetPath(map.directory), pack::AssetType::GroundSounds, blob, true);
            !added) {
            VD_LOG_ERROR("%s", added.error().message.c_str());
            ++failures;
        }
        failures += failed.load();
        tileCount += header.tileCount;
        bytes += blob.size();
    }
    return failures;
}

} // namespace

u64 soundCacheKey(const std::vector<u8>& bytes, bool mp3) {
    const u32 settings[] = {mp3 ? 1u : 0u, bake::kSoundEncoderVersion, bake::kSoundBlockBytesPerChannel,
                            pack::kSoundFormatVersion};
    return fnv1a64(settings, sizeof(settings), fnv1a64(bytes.data(), bytes.size()));
}

int commandScanSounds(const Options& options) {
    formats::MpqArchiveSet mpq;
    if (!openArchives(options, mpq)) {
        return 1;
    }
    const Clock::time_point start = Clock::now();
    const std::vector<std::string> wavs = mpq.list("*.wav");
    const std::vector<std::string> mp3s = mpq.list("*.mp3");
    // Census of the WAV formats (codec, channels, rate, bits) with their sizes and lengths.
    struct Bucket {
        u32 files = 0;
        u64 bytes = 0;
        u64 frames = 0;
    };
    std::map<std::tuple<u16, u16, u32, u16>, Bucket> census;
    u64 wavBytes = 0, pcmBytes = 0, imaBytes = 0, mp3Bytes = 0;
    u32 failures = 0;
    for (const std::string& path : wavs) {
        Result<std::vector<u8>> bytes = mpq.read(path);
        Result<formats::WavInfo> wav = bytes ? formats::parseWav(bytes.value()) : Result<formats::WavInfo>(bytes.error());
        if (!wav) {
            VD_LOG_WARN("%s: %s", path.c_str(), wav.error().message.c_str());
            ++failures;
            continue;
        }
        const formats::WavInfo& w = wav.value();
        Bucket& bucket = census[{w.format, w.channels, w.sampleRate, w.bitsPerSample}];
        ++bucket.files;
        bucket.bytes += bytes.value().size();
        bucket.frames += w.frameCount;
        wavBytes += bytes.value().size();
        pcmBytes += static_cast<u64>(w.frameCount) * w.channels * 2;
        imaBytes += static_cast<u64>(w.frameCount) * w.channels / 2;
    }
    for (const std::string& path : mp3s) {
        if (Result<std::vector<u8>> bytes = mpq.read(path); bytes) {
            mp3Bytes += bytes.value().size();
        }
    }
    std::printf("codec  ch   rate bits  files      MB   minutes\n");
    for (const auto& [key, bucket] : census) {
        const auto [format, channels, rate, bits] = key;
        std::printf("0x%04x %2u %6u %4u %6u %7.1f %8.1f\n", format, channels, rate, bits, bucket.files,
                    static_cast<f64>(bucket.bytes) / 1e6, static_cast<f64>(bucket.frames) / rate / 60.0);
    }
    u32 references = 0;
    const std::vector<std::string> used = soundPaths(mpq, references);
    u32 missing = 0;
    for (const std::string& path : used) {
        if (!mpq.contains(path)) {
            ++missing;
        }
    }
    VD_LOG_INFO("%zu WAV (%.1f MB as shipped, %.1f MB as PCM16, ~%.1f MB as IMA ADPCM), %u unreadable | %zu MP3 "
                "(%.1f MB) | SoundEntries: %u references to %zu files, %u not in the client | %.1f s",
                wavs.size(), static_cast<f64>(wavBytes) / 1e6, static_cast<f64>(pcmBytes) / 1e6,
                static_cast<f64>(imaBytes) / 1e6, failures, mp3s.size(), static_cast<f64>(mp3Bytes) / 1e6, references,
                used.size(), missing, secondsBetween(start, Clock::now()));
    return failures == 0 ? 0 : 1;
}

int commandProbeGround(const Options& options) {
    formats::MpqArchiveSet mpq;
    if (!openArchives(options, mpq)) {
        return 1;
    }
    const std::string map = option(options, "map", "Azeroth");
    f64 x = 0.0, y = 0.0;
    if (std::sscanf(option(options, "pos").c_str(), "%lf,%lf", &x, &y) != 2) {
        VD_LOG_ERROR("probe-ground needs --pos x,y");
        return 1;
    }
    const u32 tx = static_cast<u32>(32.0 - y / pack::kTileSize);
    const u32 ty = static_cast<u32>(32.0 - x / pack::kTileSize);
    Result<std::vector<u8>> bytes = mpq.read(formats::adtPath(map, tx, ty));
    Result<formats::Adt> adt = bytes ? formats::parseAdt(bytes.value()) : Result<formats::Adt>(bytes.error());
    if (!adt) {
        VD_LOG_ERROR("%s", adt.error().message.c_str());
        return 1;
    }
    const GroundTables tables = groundTables(mpq);
    const f64 s = (pack::tileCornerY(tx) - y) / pack::kUnitSize;
    const f64 t = (pack::tileCornerX(ty) - x) / pack::kUnitSize;
    const u32 column = static_cast<u32>(s) / 8, row = static_cast<u32>(t) / 8;
    const formats::AdtChunk& chunk = adt.value().chunks[row * 16 + column];
    std::printf("tile %u_%u chunk row %u column %u (index %u,%u) area %u, %u layers\n", tx, ty, row, column,
                chunk.indexX, chunk.indexY, chunk.areaId, chunk.layerCount);
    for (u32 k = 0; k < chunk.layerCount && k < 4; ++k) {
        const formats::AdtLayer& layer = chunk.layers[k];
        const auto effect = tables.effectTerrain.find(layer.effectId);
        const std::string texture =
            layer.textureId < adt.value().textures.size() ? adt.value().textures[layer.textureId] : "?";
        std::printf("  layer %u: %s effect %u (terrain %d) name -> terrain %u\n", k, texture.c_str(), layer.effectId,
                    effect != tables.effectTerrain.end() ? static_cast<int>(effect->second) : -1,
                    terrainTypeFromName(texture));
    }
    const std::vector<u8> cells = groundTile(adt.value(), tables);
    for (u32 r = 0; r < pack::kGroundCells; ++r) {
        std::printf("  ");
        for (u32 c = 0; c < pack::kGroundCells; ++c) {
            std::printf("%u ", cells[(row * 16 + column) * pack::kGroundCells * pack::kGroundCells + r * pack::kGroundCells + c]);
        }
        std::printf("\n");
    }
    return 0;
}

int bakeSound(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outPath) {
    const Clock::time_point start = Clock::now();
    u32 references = 0;
    const std::vector<std::string> paths = soundPaths(mpq, references);
    if (paths.empty()) {
        return 1;
    }
    bake::PackWriter writer;
    if (Result<void> created = writer.create(outPath, kBakerVersion); !created) {
        VD_LOG_ERROR("%s", created.error().message.c_str());
        return 1;
    }
    const BakeCache cache(outPath.parent_path() / ".cache" / "sound");
    std::atomic<u32> missing{0}, badNames{0}, cacheHits{0}, wavCount{0}, mp3Count{0};
    std::atomic<u64> inputBytes{0}, wavOut{0}, mp3Out{0};
    std::mutex failuresMutex;
    std::vector<std::string> failures;
    std::vector<std::string> missingPaths;

    JobSystem jobs;
    jobs.init();
    jobs.parallelFor(static_cast<u32>(paths.size()), 8, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            const std::string& path = paths[i];
            const std::string canonical = normalizeAssetPath(path);
            const bool mp3 = endsWith(canonical, ".mp3");
            if (!mp3 && !endsWith(canonical, ".wav")) {
                ++badNames; // a broken row ("mHumanFemaleWoundVoxA" + "wav" in two fields): the client fails too
                continue;
            }
            if (!mpq.contains(path)) {
                ++missing;
                std::lock_guard<std::mutex> lock(failuresMutex);
                missingPaths.push_back(path);
                continue;
            }
            const Result<void> added = [&]() -> Result<void> {
                Result<std::vector<u8>> bytes = mpq.read(path);
                if (!bytes) {
                    return bytes.error();
                }
                inputBytes += bytes.value().size();
                const u64 key = soundCacheKey(bytes.value(), mp3);
                std::vector<u8> blob;
                if (cache.load(key, blob)) {
                    ++cacheHits;
                } else {
                    Result<std::vector<u8>> baked = mp3 ? bake::bakeMp3Sound(bytes.value())
                                                        : bake::bakeWavSound(bytes.value());
                    if (!baked) {
                        return baked.error();
                    }
                    blob = std::move(baked.value());
                    cache.store(key, blob);
                }
                (mp3 ? mp3Out : wavOut) += blob.size();
                ++(mp3 ? mp3Count : wavCount);
                return writer.add(canonical, pack::AssetType::Sound, blob);
            }();
            if (!added) {
                std::lock_guard<std::mutex> lock(failuresMutex);
                failures.push_back(path + ": " + added.error().message);
            }
        }
    });
    // The footstep surface of the terrain.
    u32 groundTiles = 0;
    u64 groundBytes = 0;
    const Clock::time_point groundStart = Clock::now();
    const u32 groundFailures = bakeGroundSounds(mpq, writer, BakeCache(outPath.parent_path() / ".cache" / "ground"), jobs,
                                                groundTiles, groundBytes);
    VD_LOG_INFO("ground sounds: %u tiles (%.1f MB before LZ4), %u failed, %.1f s", groundTiles,
                static_cast<f64>(groundBytes) / 1e6, groundFailures, secondsBetween(groundStart, Clock::now()));
    jobs.shutdown();

    if (Result<void> finished = writer.finish(); !finished) {
        VD_LOG_ERROR("%s", finished.error().message.c_str());
        return 1;
    }
    std::sort(missingPaths.begin(), missingPaths.end());
    for (usize i = 0; i < missingPaths.size() && i < 10; ++i) {
        VD_LOG_WARN("Not in the client: %s", missingPaths[i].c_str());
    }
    for (const std::string& failure : failures) {
        VD_LOG_ERROR("FAILED %s", failure.c_str());
    }
    VD_LOG_INFO("sound: %u assets (%u WAV -> IMA ADPCM %.1f MB, %u MP3 %.1f MB; %u from cache), %u named but missing "
                "in the client, %u not sound files, %zu failed | %.1f MB in | %.1f MB pack | %.1f s -> %s",
                writer.entryCount(), wavCount.load(), static_cast<f64>(wavOut.load()) / 1e6, mp3Count.load(),
                static_cast<f64>(mp3Out.load()) / 1e6, cacheHits.load(), missing.load(), badNames.load(), failures.size(),
                static_cast<f64>(inputBytes.load()) / 1e6, static_cast<f64>(writer.bytesWritten()) / 1e6,
                secondsBetween(start, Clock::now()), outPath.string().c_str());
    return failures.empty() && groundFailures == 0 ? 0 : 1;
}

} // namespace vd::baker
