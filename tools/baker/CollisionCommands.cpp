#include "BakerCommon.h"

#include "bake/CollisionBaker.h"
#include "bake/ModelBaker.h"
#include "bake/PackWriter.h"
#include "core/Clock.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "formats/Adt.h"
#include "formats/M2.h"
#include "formats/Wmo.h"
#include "jobs/JobSystem.h"
#include "db/DbcView.h"
#include "pack/CollisionFormat.h"
#include "pack/ObjectCollision.h"
#include "pack/PackReader.h"
#include "physics/CharacterController.h"
#include "physics/CollisionWorld.h"

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace vd::baker {
namespace {

constexpr u32 kTileCount = pack::kMapTiles * pack::kMapTiles;

// An ADT/WDT placement, before its model is loaded.
struct Placement {
    std::string model; // canonical path
    std::array<f32, 12> transform;
    u16 doodadSet = 0;
};

// A model placed in the world: its model-space triangles and where they go.
struct Instance {
    const bake::TriangleSoup* solid = nullptr;
    const bake::TriangleSoup* liquid = nullptr;
    std::array<f32, 12> transform;
    std::array<f32, 3> min;
    std::array<f32, 3> max;
};

Result<bake::WmoCollision> loadWmo(const formats::MpqArchiveSet& mpq, const std::string& path) {
    Result<std::vector<u8>> bytes = mpq.read(path);
    if (!bytes) {
        return bytes.error();
    }
    Result<formats::WmoRoot> root = formats::parseWmoRoot(bytes.value());
    if (!root) {
        return root.error();
    }
    std::vector<formats::WmoGroup> groups;
    for (u32 g = 0; g < root.value().groups.size(); ++g) {
        Result<std::vector<u8>> groupBytes = mpq.read(formats::wmoGroupPath(path, g));
        if (!groupBytes) {
            return groupBytes.error();
        }
        Result<formats::WmoGroup> group = formats::parseWmoGroup(groupBytes.value());
        if (!group) {
            return Error{"group " + std::to_string(g) + ": " + group.error().message};
        }
        groups.push_back(std::move(group.value()));
    }
    return bake::wmoCollision(root.value(), groups);
}

Result<bake::TriangleSoup> loadM2(const formats::MpqArchiveSet& mpq, const std::string& path) {
    Result<std::vector<u8>> bytes = mpq.read(path);
    if (!bytes) {
        return bytes.error();
    }
    Result<formats::M2Model> model = formats::parseM2(bytes.value());
    if (!model) {
        return model.error();
    }
    return bake::m2Collision(model.value());
}

// Tiles whose square the world box [min, max] overlaps (clamped to the grid).
void forEachTile(const std::array<f32, 3>& min, const std::array<f32, 3>& max, const auto& f) {
    const f64 size = pack::kTileSize;
    const i32 ty0 = std::max(0, static_cast<i32>(std::floor(32.0 - max[0] / size)));
    const i32 ty1 = std::min(63, static_cast<i32>(std::floor(32.0 - min[0] / size)));
    const i32 tx0 = std::max(0, static_cast<i32>(std::floor(32.0 - max[1] / size)));
    const i32 tx1 = std::min(63, static_cast<i32>(std::floor(32.0 - min[1] / size)));
    for (i32 ty = ty0; ty <= ty1; ++ty) {
        for (i32 tx = tx0; tx <= tx1; ++tx) {
            f(static_cast<u32>(ty) * pack::kMapTiles + static_cast<u32>(tx));
        }
    }
}

int bakeMapCollision(const formats::MpqArchiveSet& mpq, const MapInfo& map, const std::filesystem::path& outDir,
                     JobSystem& jobs) {
    const Clock::time_point start = Clock::now();
    std::vector<u32> adtTiles;
    for (u32 ty = 0; ty < pack::kMapTiles; ++ty) {
        for (u32 tx = 0; tx < pack::kMapTiles; ++tx) {
            if (map.wdt.hasTile(tx, ty)) {
                adtTiles.push_back(ty * pack::kMapTiles + tx);
            }
        }
    }

    // Pass 1: every placement of the map (a building on several tiles is listed by each: same uniqueId).
    std::mutex mutex;
    std::vector<std::string> failures;
    std::map<u32, Placement> wmoPlacements;
    std::map<u32, Placement> doodadPlacements;
    jobs.parallelFor(static_cast<u32>(adtTiles.size()), 4, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            const std::string path =
                formats::adtPath(map.directory, adtTiles[i] % pack::kMapTiles, adtTiles[i] / pack::kMapTiles);
            Result<std::vector<u8>> bytes = mpq.read(path);
            Result<formats::Adt> adt = bytes ? formats::parseAdt(bytes.value()) : Result<formats::Adt>(bytes.error());
            std::lock_guard<std::mutex> lock(mutex);
            if (!adt) {
                failures.push_back(path + ": " + adt.error().message);
                continue;
            }
            for (const formats::AdtDoodadPlacement& d : adt.value().doodads) {
                if (d.nameId < adt.value().models.size()) {
                    doodadPlacements.emplace(
                        d.uniqueId, Placement{normalizeAssetPath(formats::m2Path(adt.value().models[d.nameId])),
                                              bake::placementTransform(d.position, d.rotation, d.scale / 1024.0f)});
                }
            }
            for (const formats::AdtWmoPlacement& w : adt.value().wmoPlacements) {
                if (w.nameId < adt.value().wmos.size()) {
                    wmoPlacements.emplace(w.uniqueId, Placement{normalizeAssetPath(adt.value().wmos[w.nameId]),
                                                                bake::placementTransform(w.position, w.rotation, 1.0f),
                                                                w.doodadSet});
                }
            }
        }
    });
    if (map.wdt.hasWmoPlacement) {
        const formats::WdtWmoPlacement& p = map.wdt.wmoPlacement;
        wmoPlacements.emplace(p.uniqueId, Placement{normalizeAssetPath(map.wdt.wmoName),
                                                    bake::placementTransform(p.position, p.rotation, 1.0f), p.doodadSet});
    }
    for (const std::string& failure : failures) {
        VD_LOG_ERROR("FAILED %s", failure.c_str());
    }
    if (!failures.empty()) {
        return 1;
    }

    // Models: the WMOs, then every M2 placed on the terrain or in a WMO doodad set.
    std::set<std::string> wmoPathSet;
    for (const auto& [id, p] : wmoPlacements) {
        wmoPathSet.insert(p.model);
    }
    const std::vector<std::string> wmoPaths(wmoPathSet.begin(), wmoPathSet.end());
    std::vector<bake::WmoCollision> wmoModels(wmoPaths.size());
    std::vector<bool> wmoLoaded(wmoPaths.size(), false);
    std::atomic<u32> missing{0};
    jobs.parallelFor(static_cast<u32>(wmoPaths.size()), 1, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            Result<bake::WmoCollision> wmo = loadWmo(mpq, wmoPaths[i]);
            if (!wmo) {
                std::lock_guard<std::mutex> lock(mutex);
                failures.push_back(wmoPaths[i] + ": " + wmo.error().message);
                continue;
            }
            wmoModels[i] = std::move(wmo.value());
            wmoLoaded[i] = true;
        }
    });
    std::map<std::string, u32> wmoIndex;
    for (u32 i = 0; i < wmoPaths.size(); ++i) {
        wmoIndex.emplace(wmoPaths[i], i);
    }
    std::set<std::string> m2PathSet;
    for (const auto& [id, p] : doodadPlacements) {
        m2PathSet.insert(p.model);
    }
    for (const bake::WmoCollision& wmo : wmoModels) {
        for (const bake::WmoCollisionDoodad& d : wmo.doodads) {
            if (!d.model.empty()) {
                m2PathSet.insert(d.model);
            }
        }
    }
    const std::vector<std::string> m2Paths(m2PathSet.begin(), m2PathSet.end());
    std::vector<bake::TriangleSoup> m2Models(m2Paths.size());
    jobs.parallelFor(static_cast<u32>(m2Paths.size()), 8, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            if (!mpq.contains(m2Paths[i])) {
                ++missing;
                continue;
            }
            Result<bake::TriangleSoup> m2 = loadM2(mpq, m2Paths[i]);
            if (!m2) {
                std::lock_guard<std::mutex> lock(mutex);
                failures.push_back(m2Paths[i] + ": " + m2.error().message);
                continue;
            }
            m2Models[i] = std::move(m2.value());
        }
    });
    for (const std::string& failure : failures) {
        VD_LOG_ERROR("FAILED %s", failure.c_str());
    }
    if (!failures.empty()) {
        return 1;
    }
    std::map<std::string, u32> m2Index;
    for (u32 i = 0; i < m2Paths.size(); ++i) {
        m2Index.emplace(m2Paths[i], i);
    }

    // Instances with something to collide with, and the tiles each one reaches.
    std::vector<Instance> instances;
    auto addInstance = [&](const bake::TriangleSoup* solid, const bake::TriangleSoup* liquid,
                           const std::array<f32, 12>& transform) {
        Instance instance{solid && !solid->empty() ? solid : nullptr, liquid && !liquid->empty() ? liquid : nullptr,
                          transform, {}, {}};
        if (!instance.solid && !instance.liquid) {
            return;
        }
        instance.min = {1e30f, 1e30f, 1e30f};
        instance.max = {-1e30f, -1e30f, -1e30f};
        for (const bake::TriangleSoup* soup : {instance.solid, instance.liquid}) {
            std::array<f32, 3> min{}, max{};
            if (soup && bake::transformedBounds(*soup, transform, min, max)) {
                for (u32 a = 0; a < 3; ++a) {
                    instance.min[a] = std::min(instance.min[a], min[a]);
                    instance.max[a] = std::max(instance.max[a], max[a]);
                }
            }
        }
        instances.push_back(instance);
    };
    u32 wmoDoodads = 0;
    for (const auto& [id, p] : wmoPlacements) {
        const auto it = wmoIndex.find(p.model);
        if (it == wmoIndex.end() || !wmoLoaded[it->second]) {
            continue;
        }
        const bake::WmoCollision& wmo = wmoModels[it->second];
        addInstance(&wmo.solid, &wmo.liquid, p.transform);
        // The default set, plus the placement's own (as the runtime shows them).
        const u16 sets[2] = {0, p.doodadSet};
        for (u32 s = 0; s < (p.doodadSet == 0 ? 1u : 2u); ++s) {
            if (sets[s] >= wmo.sets.size()) {
                continue;
            }
            const formats::WmoDoodadSet& range = wmo.sets[sets[s]];
            for (u32 d = range.first; d < range.first + range.count && d < wmo.doodads.size(); ++d) {
                const auto m2 = m2Index.find(wmo.doodads[d].model);
                if (m2 != m2Index.end()) {
                    addInstance(&m2Models[m2->second], nullptr,
                                bake::multiplyTransforms(p.transform, wmo.doodads[d].transform));
                    ++wmoDoodads;
                }
            }
        }
    }
    for (const auto& [id, p] : doodadPlacements) {
        const auto it = m2Index.find(p.model);
        if (it != m2Index.end()) {
            addInstance(&m2Models[it->second], nullptr, p.transform);
        }
    }
    std::vector<std::vector<u32>> byTile(kTileCount);
    for (u32 i = 0; i < instances.size(); ++i) {
        forEachTile(instances[i].min, instances[i].max, [&](u32 tile) { byTile[tile].push_back(i); });
    }
    std::vector<u32> tiles;
    for (u32 tile = 0; tile < kTileCount; ++tile) {
        const u32 tx = tile % pack::kMapTiles, ty = tile / pack::kMapTiles;
        if (map.wdt.hasTile(tx, ty) || !byTile[tile].empty()) {
            tiles.push_back(tile);
        }
    }

    // Pass 2: one collision tile each.
    const std::filesystem::path outPath = outDir / pack::collisionPackFileName(map.directory);
    bake::PackWriter writer;
    if (Result<void> created = writer.create(outPath, kBakerVersion); !created) {
        VD_LOG_ERROR("%s", created.error().message.c_str());
        return 1;
    }
    std::atomic<u64> triangles{0}, liquidTriangles{0};
    std::atomic<u32> written{0}, maxTriangles{0}, maxDepth{0};
    u32 biggestTile = 0;
    jobs.parallelFor(static_cast<u32>(tiles.size()), 1, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            const u32 tx = tiles[i] % pack::kMapTiles, ty = tiles[i] / pack::kMapTiles;
            const std::string path = pack::collisionTilePath(map.directory, tx, ty);
            bake::CollisionTileInput input;
            input.tx = tx;
            input.ty = ty;
            Result<formats::Adt> adt = Error{"no ADT"};
            if (map.wdt.hasTile(tx, ty)) {
                Result<std::vector<u8>> bytes = mpq.read(formats::adtPath(map.directory, tx, ty));
                adt = bytes ? formats::parseAdt(bytes.value()) : Result<formats::Adt>(bytes.error());
                if (!adt) {
                    std::lock_guard<std::mutex> lock(mutex);
                    failures.push_back(path + ": " + adt.error().message);
                    continue;
                }
                input.adt = &adt.value();
            }
            const std::array<f32, 2> xyMax{pack::tileCornerX(ty), pack::tileCornerY(tx)};
            const std::array<f32, 2> xyMin{xyMax[0] - pack::kTileSize, xyMax[1] - pack::kTileSize};
            for (u32 index : byTile[tiles[i]]) {
                const Instance& instance = instances[index];
                if (instance.solid) {
                    bake::appendTransformed(*instance.solid, instance.transform, xyMin, xyMax, input.solid);
                }
                if (instance.liquid) {
                    bake::appendTransformed(*instance.liquid, instance.transform, xyMin, xyMax, input.liquid);
                }
            }
            if (!input.adt && input.solid.empty() && input.liquid.empty()) {
                continue;
            }
            bake::CollisionTileStats stats;
            const std::vector<u8> blob = bake::bakeCollisionTile(input, &stats);
            if (Result<void> added = writer.add(path, pack::AssetType::CollisionTile, blob, true); !added) {
                std::lock_guard<std::mutex> lock(mutex);
                failures.push_back(path + ": " + added.error().message);
                continue;
            }
            triangles += stats.triangles;
            liquidTriangles += stats.liquidTriangles;
            ++written;
            std::lock_guard<std::mutex> lock(mutex);
            if (stats.triangles > maxTriangles) {
                maxTriangles = stats.triangles;
                biggestTile = tiles[i];
            }
            maxDepth = std::max(maxDepth.load(), stats.depth);
        }
    });
    for (const std::string& failure : failures) {
        VD_LOG_ERROR("FAILED %s", failure.c_str());
    }
    if (Result<void> finished = writer.finish(); !finished) {
        VD_LOG_ERROR("%s", finished.error().message.c_str());
        return 1;
    }
    VD_LOG_INFO("%s collision: %u tiles | %zu WMO + %zu doodads placed (%u in WMO sets), %zu instances collide, %u M2 "
                "missing | %.1f M triangles (most %u in tile %u_%u, BVH depth %u), %.2f M liquid | %.1f MB | %.1f s -> "
                "%s",
                map.directory.c_str(), written.load(), wmoPlacements.size(), doodadPlacements.size(), wmoDoodads,
                instances.size(), missing.load(), static_cast<f64>(triangles.load()) / 1e6, maxTriangles.load(),
                biggestTile % pack::kMapTiles, biggestTile / pack::kMapTiles, maxDepth.load(),
                static_cast<f64>(liquidTriangles.load()) / 1e6, static_cast<f64>(writer.bytesWritten()) / 1e6,
                secondsBetween(start, Clock::now()), outPath.string().c_str());
    return failures.empty() ? 0 : 1;
}

} // namespace

int bakeCollision(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outDir, const std::string& mapFilter) {
    int result = 0;
    u32 baked = 0;
    JobSystem jobs;
    jobs.init();
    for (const MapInfo& map : listMaps(mpq)) {
        if ((map.wdt.tileCount == 0 && !map.wdt.hasWmoPlacement) || !mapSelected(map.directory, mapFilter)) {
            continue;
        }
        result |= bakeMapCollision(mpq, map, outDir, jobs);
        ++baked;
    }
    jobs.shutdown();
    if (baked == 0) {
        VD_LOG_ERROR("No map matches '%s'", mapFilter.c_str());
        return 1;
    }
    return result;
}

// Phase 8: the collision of every game object model (GameObjectDisplayInfo.dbc) in its own space, for what moves
// (transports): collision_objects.vdpk (pack/ObjectCollision.h).
int bakeObjectCollision(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outDir) {
    const Clock::time_point start = Clock::now();
    Result<std::vector<u8>> dbc = mpq.read("DBFilesClient\\GameObjectDisplayInfo.dbc");
    if (!dbc) {
        VD_LOG_ERROR("GameObjectDisplayInfo.dbc: %s", dbc.error().message.c_str());
        return 1;
    }
    Result<db::DbcView> displays = db::DbcView::parse(dbc.value().data(), dbc.value().size());
    if (!displays) {
        VD_LOG_ERROR("GameObjectDisplayInfo.dbc: %s", displays.error().message.c_str());
        return 1;
    }
    std::set<std::string> models;
    for (u32 r = 0; r < displays.value().recordCount(); ++r) {
        const std::string_view model = displays.value().getString(r, 1);
        if (!model.empty()) {
            models.insert(pack::objectModelPath(model));
        }
    }
    const std::vector<std::string> paths(models.begin(), models.end());
    const std::filesystem::path outPath = outDir / pack::kObjectCollisionPack;
    bake::PackWriter writer;
    if (Result<void> created = writer.create(outPath, kBakerVersion); !created) {
        VD_LOG_ERROR("%s", created.error().message.c_str());
        return 1;
    }
    JobSystem jobs;
    jobs.init();
    std::mutex mutex;
    std::vector<std::string> failures;
    std::atomic<u32> written{0}, missing{0}, empty{0};
    std::atomic<u64> triangles{0};
    jobs.parallelFor(static_cast<u32>(paths.size()), 4, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            const std::string& path = paths[i];
            if (!mpq.contains(path)) {
                ++missing;
                continue;
            }
            bake::CollisionTileInput input; // model space: tile 0_0, no terrain
            const bool wmo = path.size() > 4 && path.compare(path.size() - 4, 4, ".wmo") == 0;
            if (wmo) {
                Result<bake::WmoCollision> model = loadWmo(mpq, path);
                if (!model) {
                    std::lock_guard<std::mutex> lock(mutex);
                    failures.push_back(path + ": " + model.error().message);
                    continue;
                }
                const std::array<f32, 12> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
                const std::array<f32, 2> everywhere[2] = {{-1e9f, -1e9f}, {1e9f, 1e9f}};
                bake::appendTransformed(model.value().solid, identity, everywhere[0], everywhere[1], input.solid);
                bake::appendTransformed(model.value().liquid, identity, everywhere[0], everywhere[1], input.liquid);
                // The default doodad set (crates, cannons on the decks).
                if (!model.value().sets.empty()) {
                    const formats::WmoDoodadSet& set = model.value().sets[0];
                    for (u32 d = set.first; d < set.first + set.count && d < model.value().doodads.size(); ++d) {
                        const bake::WmoCollisionDoodad& doodad = model.value().doodads[d];
                        if (doodad.model.empty() || !mpq.contains(doodad.model)) {
                            continue;
                        }
                        if (Result<bake::TriangleSoup> m2 = loadM2(mpq, doodad.model)) {
                            bake::appendTransformed(m2.value(), doodad.transform, everywhere[0], everywhere[1],
                                                    input.solid);
                        }
                    }
                }
            } else {
                Result<bake::TriangleSoup> model = loadM2(mpq, path);
                if (!model) {
                    std::lock_guard<std::mutex> lock(mutex);
                    failures.push_back(path + ": " + model.error().message);
                    continue;
                }
                input.solid = std::move(model.value());
            }
            if (input.solid.empty() && input.liquid.empty()) {
                ++empty;
                continue;
            }
            bake::CollisionTileStats stats;
            const std::vector<u8> blob = bake::bakeCollisionTile(input, &stats);
            if (Result<void> added = writer.add(path + ".col", pack::AssetType::CollisionTile, blob, true); !added) {
                std::lock_guard<std::mutex> lock(mutex);
                failures.push_back(path + ": " + added.error().message);
                continue;
            }
            triangles += stats.triangles;
            ++written;
        }
    });
    jobs.shutdown();
    for (const std::string& failure : failures) {
        VD_LOG_ERROR("FAILED %s", failure.c_str());
    }
    if (Result<void> finished = writer.finish(); !finished) {
        VD_LOG_ERROR("%s", finished.error().message.c_str());
        return 1;
    }
    VD_LOG_INFO("Object collision: %u of %zu models (%u without collision, %u missing), %.2f M triangles, %.1f MB, "
                "%.1f s -> %s",
                written.load(), paths.size(), empty.load(), missing.load(),
                static_cast<f64>(triangles.load()) / 1e6, static_cast<f64>(writer.bytesWritten()) / 1e6,
                secondsBetween(start, Clock::now()), outPath.string().c_str());
    return failures.empty() ? 0 : 1;
}

// vd_baker check-collision --packs packs --map Azeroth --points spawns.txt: for each "x y z" line (creatures standing
// where the server put them), the ground the runtime finds just under it. A census of how well the baked collision
// agrees with the server's own maps.
int commandCheckCollision(const Options& options) {
    const std::string packs = option(options, "packs", "packs");
    const std::string map = option(options, "map");
    const std::string points = option(options, "points");
    if (map.empty() || points.empty()) {
        VD_LOG_ERROR("check-collision needs --map and --points");
        return 1;
    }
    pack::PackReader reader;
    if (Result<void> opened = reader.open(std::filesystem::path(packs) / pack::collisionPackFileName(map)); !opened) {
        VD_LOG_ERROR("%s", opened.error().message.c_str());
        return 1;
    }
    std::FILE* file = std::fopen(points.c_str(), "r");
    if (!file) {
        VD_LOG_ERROR("Cannot read %s", points.c_str());
        return 1;
    }
    physics::CollisionWorld world;
    std::set<u32> added;
    struct Sample {
        f64 x, y, z, error;
    };
    std::vector<Sample> results;
    u32 noGround = 0, noTile = 0;
    f64 x = 0, y = 0, z = 0;
    while (std::fscanf(file, "%lf %lf %lf", &x, &y, &z) == 3) {
        i32 tx = 0, ty = 0;
        if (!physics::CollisionWorld::tileAt(x, y, tx, ty)) {
            ++noTile;
            continue;
        }
        const u32 tile = static_cast<u32>(ty) * pack::kMapTiles + static_cast<u32>(tx);
        if (added.insert(tile).second) {
            const std::optional<pack::PackAsset> asset =
                reader.find(pack::collisionTilePath(map, static_cast<u32>(tx), static_cast<u32>(ty)));
            if (asset) {
                std::vector<u8> blob(asset->rawSize);
                if (pack::PackReader::read(*asset, blob.data(), blob.size())) {
                    if (Result<void> ok = world.addTile(std::move(blob)); !ok) {
                        VD_LOG_ERROR("Tile %d_%d: %s", tx, ty, ok.error().message.c_str());
                    }
                }
            }
        }
        // The ground a character standing there would find: within a step above, or below.
        const std::optional<f64> ground = world.groundHeight(x, y, z + 1.0, z - 8.0);
        if (!ground) {
            ++noGround;
            results.push_back({x, y, z, 1e9});
            continue;
        }
        results.push_back({x, y, z, *ground - z});
    }
    std::fclose(file);
    u32 within[4] = {};
    constexpr f64 kLimits[4] = {0.1, 0.25, 0.5, 1.0};
    for (const Sample& r : results) {
        for (u32 i = 0; i < 4; ++i) {
            within[i] += std::abs(r.error) <= kLimits[i] ? 1 : 0;
        }
    }
    std::sort(results.begin(), results.end(),
              [](const Sample& a, const Sample& b) { return std::abs(a.error) > std::abs(b.error); });
    const u32 show = static_cast<u32>(std::min<usize>(results.size(), std::stoul(option(options, "show", "15"))));
    for (u32 i = 0; i < show; ++i) {
        if (results[i].error > 1e8) {
            std::printf("  (%.2f, %.2f, %.2f): no ground within 1 yd above / 8 below\n", results[i].x, results[i].y,
                        results[i].z);
        } else {
            std::printf("  (%.2f, %.2f, %.2f): ground %+.2f\n", results[i].x, results[i].y, results[i].z,
                        results[i].error);
        }
    }
    const f64 n = static_cast<f64>(std::max<usize>(1, results.size()));
    std::printf("%zu points, %zu tiles: |ground - z| <= 0.1: %.1f%%, <= 0.25: %.1f%%, <= 0.5: %.1f%%, <= 1: %.1f%% | "
                "no ground %u, outside the map %u\n",
                results.size(), added.size(), 100.0 * within[0] / n, 100.0 * within[1] / n, 100.0 * within[2] / n,
                100.0 * within[3] / n, noGround, noTile);
    return 0;
}

namespace {

// Loads every collision tile of `map` overlapping the XY box into `world`.
void loadTiles(const pack::PackReader& reader, const std::string& map, f64 minX, f64 maxX, f64 minY, f64 maxY,
               physics::CollisionWorld& world) {
    i32 tx0, ty0, tx1, ty1;
    physics::CollisionWorld::tileAt(maxX, maxY, tx0, ty0);
    physics::CollisionWorld::tileAt(minX, minY, tx1, ty1);
    for (i32 ty = std::max(0, ty0); ty <= std::min(63, ty1); ++ty) {
        for (i32 tx = std::max(0, tx0); tx <= std::min(63, tx1); ++tx) {
            const std::optional<pack::PackAsset> asset =
                reader.find(pack::collisionTilePath(map, static_cast<u32>(tx), static_cast<u32>(ty)));
            if (!asset) {
                continue;
            }
            std::vector<u8> blob(asset->rawSize);
            if (pack::PackReader::read(*asset, blob.data(), blob.size())) {
                (void)world.addTile(std::move(blob));
            }
        }
    }
}

} // namespace

// vd_baker probe-walk --map Azeroth --from x,y,z --to x,y [--seconds 20]: the character controller walks from one
// point towards another over the baked collision, printing its trace, and where it stops, what is in the way.
int commandProbeWalk(const Options& options) {
    const std::string packs = option(options, "packs", "packs");
    const std::string map = option(options, "map");
    f64 x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0;
    if (map.empty() || std::sscanf(option(options, "from").c_str(), "%lf,%lf,%lf", &x0, &y0, &z0) != 3 ||
        std::sscanf(option(options, "to").c_str(), "%lf,%lf", &x1, &y1) != 2) {
        VD_LOG_ERROR("probe-walk needs --map, --from x,y,z and --to x,y");
        return 1;
    }
    pack::PackReader reader;
    if (Result<void> opened = reader.open(std::filesystem::path(packs) / pack::collisionPackFileName(map)); !opened) {
        VD_LOG_ERROR("%s", opened.error().message.c_str());
        return 1;
    }
    physics::CollisionWorld world;
    loadTiles(reader, map, std::min(x0, x1) - 50.0, std::max(x0, x1) + 50.0, std::min(y0, y1) - 50.0,
              std::max(y0, y1) + 50.0, world);
    // --triangles R: instead of walking, every collision triangle within R yards of --from (and 4 yards of z0).
    if (const std::string triangles = option(options, "triangles"); !triangles.empty()) {
        const f64 r = std::atof(triangles.c_str());
        physics::TriangleSet set(8192);
        set.reset(glm::dvec3(x0, y0, z0));
        world.gather(glm::dvec3(x0 - r, y0 - r, z0 - 4.0), glm::dvec3(x0 + r, y0 + r, z0 + 4.0), set);
        for (const physics::Triangle& t : set.triangles()) {
            std::printf("z %+6.2f %+6.2f %+6.2f  normal (%+.2f %+.2f %+.2f)  flags 0x%02X  xy (%+.1f %+.1f) (%+.1f %+.1f) "
                        "(%+.1f %+.1f)\n",
                        t.a.z, t.b.z, t.c.z, t.normal.x, t.normal.y, t.normal.z, t.flags, t.a.x, t.a.y, t.b.x, t.b.y,
                        t.c.x, t.c.y);
        }
        return 0;
    }
    // --grid R: instead of walking, a map of the highest ground under z0 + 2 in the square of R yards around --from,
    // one row per half yard (tenths of a yard over z0; '..' = nothing within 4 yards below).
    if (const std::string grid = option(options, "grid"); !grid.empty()) {
        const f64 half = std::atof(grid.c_str());
        for (f64 x = x0 + half; x >= x0 - half - 1e-6; x -= 0.5) { // north (+X) on top
            std::printf("%9.1f ", x);
            for (f64 y = y0 + half; y >= y0 - half - 1e-6; y -= 0.5) { // west (+Y) on the left
                const std::optional<f64> g = world.groundHeight(x, y, z0 + 2.0, z0 - 4.0);
                if (g) {
                    std::printf("%3d", static_cast<int>(std::lround((*g - z0) * 10.0)));
                } else {
                    std::printf(" ..");
                }
            }
            std::printf("\n");
        }
        std::printf("columns: y from %.1f (left) to %.1f, every 0.5\n", y0 + half, y0 - half);
        return 0;
    }
    // --scan N: instead of walking, the biggest rise between ground samples every 5 cm along 2N + 1 parallel lines
    // 0.5 yd apart (the steps a character meets there, from left to right).
    if (const std::string scan = option(options, "scan"); !scan.empty()) {
        const i32 lines = std::atoi(scan.c_str());
        const glm::dvec2 dir = glm::normalize(glm::dvec2(x1 - x0, y1 - y0));
        const glm::dvec2 left(-dir.y, dir.x);
        const f64 length = glm::length(glm::dvec2(x1 - x0, y1 - y0));
        for (i32 line = lines; line >= -lines; --line) {
            const glm::dvec2 start = glm::dvec2(x0, y0) + left * (0.5 * line);
            f64 z = z0, worst = 0.0;
            glm::dvec2 where = start;
            bool lost = false;
            for (f64 d = 0.0; d <= length; d += 0.05) {
                const glm::dvec2 at = start + dir * d;
                const std::optional<f64> g = world.groundHeight(at.x, at.y, z + 1.5, z - 3.0);
                if (!g) {
                    lost = true;
                    break;
                }
                if (*g - z > worst) {
                    worst = *g - z;
                    where = at;
                }
                z = *g;
            }
            std::printf("%+5.1f yd: biggest rise %.2f at (%.2f, %.2f)%s\n", 0.5 * line, worst, where.x, where.y,
                        lost ? " (ground lost)" : "");
        }
        return 0;
    }
    physics::CharacterController c;
    if (const std::string step = option(options, "step"); !step.empty()) {
        c.stepHeight = static_cast<f32>(std::atof(step.c_str()));
    }
    c.place({x0, y0, z0}, 0.0f);
    const f32 seconds = static_cast<f32>(std::atof(option(options, "seconds", "20").c_str()));
    const u32 frames = static_cast<u32>(seconds * 60.0f);
    glm::dvec3 last = c.position();
    u32 still = 0;
    const bool jumpWhenBlocked = !option(options, "jump").empty();
    bool jumped = false;
    for (u32 frame = 0; frame < frames; ++frame) {
        const glm::dvec3 p = c.position();
        physics::CharacterInput input;
        input.forward = std::hypot(x1 - p.x, y1 - p.y) > 1.0;
        input.facing = static_cast<f32>(std::atan2(y1 - p.y, x1 - p.x));
        // --jump: blocked for half a second, it jumps (once) without letting go of forward.
        if (jumpWhenBlocked && still == 30 && !jumped) {
            input.jump = true;
            jumped = true;
            still = 0;
            std::printf("%6.2f s  JUMP\n", frame / 60.0f);
        }
        c.step(world, input, 1.0f / 60.0f);
        const glm::dvec3 now = c.position();
        still = glm::length(now - last) < 0.002 ? still + 1 : 0;
        last = now;
        if (c.events().fell || c.events().landed) {
            physics::TriangleSet around(4096);
            around.reset(now);
            world.gather(now - glm::dvec3(1.0), now + glm::dvec3(1.0, 1.0, 3.0), around);
            physics::Hit ground;
            if (physics::raycast(around, glm::vec3(0.0f, 0.0f, 0.6f), glm::vec3(0.0f, 0.0f, -1.2f), ground)) {
                std::printf("         ground under the axis %+.3f, normal z %.2f\n", 0.6f - 1.2f * ground.t,
                            ground.normal.z);
            }
        }
        if (frame % 15 == 0 || c.events().fell || c.events().landed) {
            std::printf("%6.2f s  (%.2f, %.2f, %.2f)  %s%s%s\n", frame / 60.0f, now.x, now.y, now.z,
                        c.mode() == physics::MotionMode::Ground ? "ground" : c.mode() == physics::MotionMode::Air ? "air" : "swim",
                        c.events().fell ? " FELL" : "", c.events().landed ? " LANDED" : "");
        }
        if (!input.forward) {
            std::printf("arrived\n");
            return 0;
        }
        if (still == 60) {
            break;
        }
    }
    // Stuck: what is ahead. Ground profile along the way, then the body's first contact.
    const glm::dvec3 p = c.position();
    const glm::dvec2 dir = glm::normalize(glm::dvec2(x1 - p.x, y1 - p.y));
    std::printf("stuck at (%.2f, %.2f, %.2f), heading %.1f deg\nground ahead (every 0.1 yd):", p.x, p.y, p.z,
                std::atan2(dir.y, dir.x) * 57.29578);
    for (u32 i = 0; i <= 20; ++i) {
        const f64 d = 0.1 * i;
        const std::optional<f64> g = world.groundHeight(p.x + dir.x * d, p.y + dir.y * d, p.z + 3.0, p.z - 3.0);
        std::printf(" %+.2f", g ? *g - p.z : 99.0);
    }
    std::printf("\n");
    physics::TriangleSet set(8192);
    set.reset(p);
    world.gather(p - glm::dvec3(4.0), p + glm::dvec3(4.0, 4.0, 6.0), set);
    for (u32 i = 0; i <= 4; ++i) {
        const glm::vec3 from(static_cast<f32>(dir.x * 0.05 * i), static_cast<f32>(dir.y * 0.05 * i), c.stepHeight);
        physics::Hit ground;
        if (physics::raycast(set, from, glm::vec3(0.0f, 0.0f, -1.5f), ground)) {
            std::printf("ground ray +%.2f yd: %+.3f, normal (%.2f, %.2f, %.2f), flags 0x%02X\n", 0.05 * i,
                        from.z - 1.5f * ground.t, ground.normal.x, ground.normal.y, ground.normal.z, ground.flags);
        }
    }
    const glm::vec3 motion(static_cast<f32>(dir.x), static_cast<f32>(dir.y), 0.0f);
    for (f32 z = c.stepHeight + c.radius; z <= c.height - c.radius + 1e-3f; z += c.radius) {
        physics::Hit hit;
        if (physics::sweepSphere(set, glm::vec3(0.0f, 0.0f, z), c.radius, motion, hit)) {
            std::printf("sphere at +%.2f: hit after %.3f yd, normal (%.2f, %.2f, %.2f), point (%.2f, %.2f, %+.2f), flags 0x%02X\n",
                        z, hit.t, hit.normal.x, hit.normal.y, hit.normal.z, p.x + hit.point.x, p.y + hit.point.y,
                        hit.point.z, hit.flags);
        } else {
            std::printf("sphere at +%.2f: free\n", z);
        }
    }
    return 0;
}

} // namespace vd::baker
