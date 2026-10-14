#include "BakerCommon.h"

#include "bake/DoodadMotion.h"
#include "bake/ModelBaker.h"
#include "core/Clock.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "formats/Adt.h"
#include "formats/M2.h"
#include "formats/Wmo.h"
#include "jobs/JobSystem.h"

#include <algorithm>
#include <array>
#include <cfloat>
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

// Phase 14: the places the doodad census counts instances in (the cities to compare with 1.12 side by side). A city
// WMO's own doodads and everything placed inside its world box belong to it; the others are circles.
struct Region {
    const char* name;
    const char* map;  // map directory, lowercase
    const char* wmo;  // normalized path of the city's WMO, or nullptr: the circle below
    f32 x, y, radius; // world, yards
};
constexpr Region kRegions[] = {
    {"Stormwind", "azeroth", "world/wmo/azeroth/buildings/stormwind/stormwind.wmo", 0.0f, 0.0f, 0.0f},
    {"Goldshire", "azeroth", nullptr, -9465.0f, 45.0f, 200.0f},
    {"Ironforge", "azeroth", "world/wmo/khazmodan/cities/ironforge/ironforge.wmo", 0.0f, 0.0f, 0.0f},
    {"Orgrimmar", "kalimdor", nullptr, 1630.0f, -4400.0f, 450.0f},
};
constexpr usize kRegionCount = std::size(kRegions);

struct Box {
    std::array<f32, 3> min{FLT_MAX, FLT_MAX, FLT_MAX};
    std::array<f32, 3> max{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    bool valid() const { return min[0] <= max[0]; }
    bool contains(const std::array<f32, 3>& p, f32 margin) const {
        return valid() && p[0] >= min[0] - margin && p[0] <= max[0] + margin && p[1] >= min[1] - margin &&
               p[1] <= max[1] + margin;
    }
};

struct Placement {
    u32 map = 0;
    std::array<f32, 3> position{}; // world
    i32 cityWmo = -1;              // the region whose city WMO lists it
};

struct WmoPlaced {
    std::string path; // normalized
    u32 map = 0;
    std::array<f32, 12> transform{};
    u16 doodadSet = 0;
    Box bounds; // world
};

std::array<f32, 3> apply(const std::array<f32, 12>& m, const std::array<f32, 3>& p) {
    return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
            m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
}

Box worldBox(const f32 (&a)[3], const f32 (&b)[3]) {
    const std::array<f32, 3> p = bake::placementToWorld({a[0], a[1], a[2]});
    const std::array<f32, 3> q = bake::placementToWorld({b[0], b[1], b[2]});
    Box box;
    for (u32 i = 0; i < 3; ++i) {
        box.min[i] = std::min(p[i], q[i]);
        box.max[i] = std::max(p[i], q[i]);
    }
    return box;
}

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

} // namespace

namespace {

// --model <path>: one model's bones, batches and what animates them.
int dumpModel(const formats::MpqArchiveSet& mpq, const std::string& path) {
    Result<std::vector<u8>> bytes = mpq.read(path);
    Result<formats::M2Model> parsed =
        bytes ? formats::parseM2(bytes.value()) : Result<formats::M2Model>(bytes.error());
    if (!parsed) {
        std::printf("%s: %s\n", path.c_str(), parsed.error().message.c_str());
        return 1;
    }
    const formats::M2Model& m = parsed.value();
    const bake::DoodadMotion motion = bake::analyzeDoodadMotion(m);
    std::printf("%s: %zu vertices, %zu bones, loop sequence %d (%u ms), global sequences", path.c_str(),
                m.vertices.size(), m.bones.size(), motion.loopSequence, motion.loopMs);
    for (const u32 g : m.globalSequences) {
        std::printf(" %u", g);
    }
    std::printf(" | radius %.2f, file radius %.2f\n",
                0.5f * std::sqrt(std::pow(m.boundsMax[0] - m.boundsMin[0], 2.0f) +
                                 std::pow(m.boundsMax[1] - m.boundsMin[1], 2.0f) +
                                 std::pow(m.boundsMax[2] - m.boundsMin[2], 2.0f)),
                m.boundsRadius);
    for (usize s = 0; s < m.sequences.size(); ++s) {
        std::printf("  sequence %zu: animation %u.%u [%u, %u] frequency %d\n", s, m.sequences[s].animationId,
                    m.sequences[s].variation, m.sequences[s].start, m.sequences[s].end,
                    m.sequences[s].frequency);
    }
    for (usize b = 0; b < m.bones.size(); ++b) {
        const formats::M2Bone& bone = m.bones[b];
        std::printf("  bone %zu: parent %d flags 0x%x pivot (%.2f %.2f %.2f) keys t%zu%s r%zu%s s%zu%s\n", b,
                    bone.parent, bone.flags, bone.pivot[0], bone.pivot[1], bone.pivot[2],
                    bone.translation.times.size(), bone.translation.globalSequence >= 0 ? "g" : "",
                    bone.rotation.times.size(), bone.rotation.globalSequence >= 0 ? "g" : "",
                    bone.scale.times.size(), bone.scale.globalSequence >= 0 ? "g" : "");
        if (std::getenv("VD_SCAN_VERBOSE")) {
            // The key ranges of each sequence (the rotation's: the windmills turn).
            for (usize r = 0; r < bone.rotation.ranges.size(); ++r) {
                const auto& range = bone.rotation.ranges[r];
                std::printf("    rotation range %zu: keys %u..%u", r, range[0], range[1]);
                for (u32 k = range[0]; k <= range[1] && k < bone.rotation.times.size() && k < range[0] + 4;
                     ++k) {
                    std::printf(" | %u ms (%.2f %.2f %.2f %.2f)", bone.rotation.times[k],
                                bone.rotation.values[k][0], bone.rotation.values[k][1],
                                bone.rotation.values[k][2], bone.rotation.values[k][3]);
                }
                std::printf("\n");
            }
        }
    }
    for (usize i = 0; i < m.batches.size(); ++i) {
        const formats::M2Batch& b = m.batches[i];
        std::set<u32> bones;
        for (u32 k = b.firstIndex; k < b.firstIndex + b.indexCount && k < m.triangles.size(); ++k) {
            const formats::M2Vertex& v = m.vertices[m.triangles[k]];
            for (u32 w = 0; w < 4; ++w) {
                if (v.boneWeights[w] > 0) {
                    bones.insert(v.boneIndices[w]);
                }
            }
        }
        std::string list;
        for (const u32 bone : bones) {
            list += std::to_string(bone) + " ";
        }
        const std::string texture =
            b.texture >= 0 ? m.textures[static_cast<usize>(b.texture)].path : std::string("-");
        std::printf(
            "  batch %zu: %u triangles, blend %u, flags 0x%x, alpha %.2f, color %d, transparency %d, uv %d, "
            "texture %s, bones %s\n",
            i, b.indexCount / 3, static_cast<u32>(m.materials[b.material].blend),
            m.materials[b.material].flags, b.alpha, b.color, b.transparency, b.textureTransform,
            texture.c_str(), list.c_str());
    }
    for (usize i = 0; i < m.colors.size(); ++i) {
        std::printf("  color %zu: %zu rgb keys%s, %zu alpha keys%s, first alpha %d\n", i,
                    m.colors[i].color.times.size(), m.colors[i].color.globalSequence >= 0 ? " (global)" : "",
                    m.colors[i].alpha.times.size(), m.colors[i].alpha.globalSequence >= 0 ? " (global)" : "",
                    m.colors[i].alpha.values.empty() ? -1 : m.colors[i].alpha.values[0]);
    }
    for (usize i = 0; i < m.transparency.size(); ++i) {
        std::printf("  transparency %zu: %zu keys%s, values", i, m.transparency[i].times.size(),
                    m.transparency[i].globalSequence >= 0 ? " (global)" : "");
        for (usize k = 0; k < m.transparency[i].values.size() && k < 12; ++k) {
            std::printf(" %d", m.transparency[i].values[k]);
        }
        std::printf("\n");
    }
    for (const formats::M2ParticleEmitter& e : m.particles) {
        std::printf("  emitter on bone %u at (%.2f %.2f %.2f)\n", e.bone, e.position[0], e.position[1],
                    e.position[2]);
    }
    for (const formats::M2Light& l : m.lights) {
        std::printf("  light type %u on bone %d at (%.2f %.2f %.2f)\n", l.type, l.bone, l.position[0],
                    l.position[1], l.position[2]);
    }
    std::printf(
        "  motion: animated bones %u (%u global), moving vertices %u (max %.3f yd), billboard bones %u "
        "(%u vertices), moving effects %u, phased %d\n",
        motion.animatedBones, motion.globalSequenceBones, motion.movingVertices, motion.maxMotion,
        motion.billboardBones, motion.billboardVertices, motion.movingEffects, motion.phased ? 1 : 0);
    return 0;
}

} // namespace

// Phase 14 (14.1): which placed doodads move, which turn to the camera, and how many of them stand in the
// cities.
int commandScanDoodadAnim(const Options& options) {
    formats::MpqArchiveSet mpq;
    if (!openArchives(options, mpq)) {
        return 1;
    }
    if (const std::string model = option(options, "model"); !model.empty()) {
        return dumpModel(mpq, normalizeAssetPath(model));
    }
    const Clock::time_point start = Clock::now();
    const std::string filter = normalizeAssetPath(option(options, "filter"));
    const usize top = static_cast<usize>(std::max(0, std::atoi(option(options, "top", "60").c_str())));
    const std::string mapFilter = option(options, "maps");

    // Placements: ADT MDDF and the doodad sets of the WMOs the ADTs (and the WMO-only maps' WDTs) place.
    std::vector<MapInfo> maps = listMaps(mpq);
    std::vector<std::string> mapNames;
    for (const MapInfo& m : maps) {
        mapNames.push_back(lower(m.directory));
    }
    std::mutex mutex;
    std::map<std::string, std::vector<Placement>> placements; // by normalized M2 path
    std::map<std::pair<u32, u32>, WmoPlaced> wmos;            // by (map, uniqueId)
    struct TileRef {
        u32 map, tx, ty;
    };
    std::vector<TileRef> tiles;
    for (u32 m = 0; m < maps.size(); ++m) {
        if (!mapSelected(maps[m].directory, mapFilter)) {
            continue;
        }
        for (u32 ty = 0; ty < formats::kMapTiles; ++ty) {
            for (u32 tx = 0; tx < formats::kMapTiles; ++tx) {
                if (maps[m].wdt.hasTile(tx, ty)) {
                    tiles.push_back({m, tx, ty});
                }
            }
        }
        if (maps[m].wdt.hasWmoPlacement && !maps[m].wdt.wmoName.empty()) {
            const formats::WdtWmoPlacement& p = maps[m].wdt.wmoPlacement;
            WmoPlaced& w = wmos[{m, p.uniqueId}];
            w.path = normalizeAssetPath(maps[m].wdt.wmoName);
            w.map = m;
            w.transform = bake::placementTransform(p.position, p.rotation, 1.0f);
            w.doodadSet = p.doodadSet;
            w.bounds = worldBox(p.extentsMin, p.extentsMax);
        }
    }
    JobSystem jobs;
    jobs.init();
    jobs.parallelFor(static_cast<u32>(tiles.size()), 4, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            const TileRef& t = tiles[i];
            Result<std::vector<u8>> bytes = mpq.read(formats::adtPath(maps[t.map].directory, t.tx, t.ty));
            Result<formats::Adt> adt =
                bytes ? formats::parseAdt(bytes.value()) : Result<formats::Adt>(bytes.error());
            if (!adt) {
                continue;
            }
            std::lock_guard<std::mutex> lock(mutex);
            for (const formats::AdtDoodadPlacement& p : adt.value().doodads) {
                if (p.nameId < adt.value().models.size()) {
                    const std::string model =
                        normalizeAssetPath(formats::m2Path(adt.value().models[p.nameId]));
                    placements[model].push_back(
                        {t.map, bake::placementToWorld({p.position[0], p.position[1], p.position[2]})});
                }
            }
            for (const formats::AdtWmoPlacement& p : adt.value().wmoPlacements) {
                if (p.nameId >= adt.value().wmos.size()) {
                    continue;
                }
                WmoPlaced& w = wmos[{t.map, p.uniqueId}];
                w.path = normalizeAssetPath(adt.value().wmos[p.nameId]);
                w.map = t.map;
                w.transform = bake::placementTransform(p.position, p.rotation, 1.0f);
                w.doodadSet = p.doodadSet;
                w.bounds = worldBox(p.extentsMin, p.extentsMax);
            }
        }
    });
    // The regions' boxes, from their city WMO.
    std::array<Box, kRegionCount> regionBoxes{};
    for (const auto& [key, w] : wmos) {
        for (usize r = 0; r < kRegionCount; ++r) {
            if (kRegions[r].wmo && w.path == kRegions[r].wmo && mapNames[w.map] == kRegions[r].map) {
                regionBoxes[r] = w.bounds;
            }
        }
    }
    std::map<std::string, formats::WmoRoot> roots;
    for (const auto& [key, w] : wmos) {
        if (roots.count(w.path) == 0) {
            Result<std::vector<u8>> bytes = mpq.read(w.path);
            Result<formats::WmoRoot> root =
                bytes ? formats::parseWmoRoot(bytes.value()) : Result<formats::WmoRoot>(bytes.error());
            roots[w.path] = root ? std::move(root.value()) : formats::WmoRoot{};
        }
        const formats::WmoRoot& root = roots[w.path];
        i32 city = -1;
        for (usize r = 0; r < kRegionCount; ++r) {
            if (kRegions[r].wmo && w.path == kRegions[r].wmo && mapNames[w.map] == kRegions[r].map) {
                city = static_cast<i32>(r);
            }
        }
        // The default set (0), then the placement's own.
        const u32 sets[2] = {0u, w.doodadSet};
        for (u32 k = 0; k < (w.doodadSet != 0 ? 2u : 1u); ++k) {
            if (sets[k] >= root.doodadSets.size()) {
                continue;
            }
            const formats::WmoDoodadSet& range = root.doodadSets[sets[k]];
            for (u32 d = range.first; d < range.first + range.count && d < root.doodads.size(); ++d) {
                const formats::WmoDoodad& doodad = root.doodads[d];
                placements[normalizeAssetPath(formats::m2Path(doodad.model))].push_back(
                    {w.map, apply(w.transform, doodad.position), city});
            }
        }
    }

    // Every placed model, analysed on the workers.
    std::vector<std::string> models;
    for (const auto& [model, list] : placements) {
        if (matchesFilter(model, filter)) {
            models.push_back(model);
        }
    }
    std::vector<bake::DoodadMotion> motion(models.size());
    std::vector<u8> loaded(models.size(), 0);
    std::vector<std::string> sequenceNotes(models.size());
    jobs.parallelFor(static_cast<u32>(models.size()), 4, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            Result<std::vector<u8>> bytes = mpq.read(models[i]);
            Result<formats::M2Model> m2 =
                bytes ? formats::parseM2(bytes.value()) : Result<formats::M2Model>(bytes.error());
            if (!m2) {
                continue;
            }
            motion[i] = bake::analyzeDoodadMotion(m2.value());
            loaded[i] = 1;
            // Which animations the model has, for the models whose loop is not their first sequence.
            std::string notes;
            for (const formats::M2Sequence& s : m2.value().sequences) {
                notes += std::to_string(s.animationId) + "." + std::to_string(s.variation) + " ";
            }
            sequenceNotes[i] = notes;
        }
    });
    jobs.shutdown();

    // Instances: everywhere and per region.
    auto regionOf = [&](const Placement& p) -> i32 {
        if (p.cityWmo >= 0) {
            return p.cityWmo;
        }
        for (usize r = 0; r < kRegionCount; ++r) {
            if (mapNames[p.map] != kRegions[r].map) {
                continue;
            }
            if (kRegions[r].wmo) {
                if (regionBoxes[r].contains(p.position, 20.0f)) {
                    return static_cast<i32>(r);
                }
            } else {
                const f32 dx = p.position[0] - kRegions[r].x;
                const f32 dy = p.position[1] - kRegions[r].y;
                if (dx * dx + dy * dy <= kRegions[r].radius * kRegions[r].radius) {
                    return static_cast<i32>(r);
                }
            }
        }
        return -1;
    };
    struct Row {
        usize model;
        u64 instances = 0;
        std::array<u64, kRegionCount> regions{};
    };
    std::vector<Row> rows;
    u64 totalInstances = 0, animatedInstances = 0, billboardInstances = 0, skinnedInstances = 0;
    std::array<u64, kRegionCount> regionAll{}, regionSkinned{}, regionAnimated{}, regionBillboard{};
    std::array<std::set<usize>, kRegionCount> regionModels{};
    u32 animatedModels = 0, billboardModels = 0, skinnedModels = 0, globalModels = 0, restPosed = 0;
    u32 notStandFirst = 0, standVariations = 0, failed = 0;
    u64 skinnedVertices = 0, movingVertices = 0, skinnedBones = 0;
    std::map<u32, u32> flagModels; // bone flag bit -> placed models having it
    for (usize i = 0; i < models.size(); ++i) {
        const std::vector<Placement>& list = placements[models[i]];
        totalInstances += list.size();
        if (!loaded[i]) {
            ++failed;
            continue;
        }
        const bake::DoodadMotion& m = motion[i];
        for (u32 bit = 0; bit < 32; ++bit) {
            if (m.boneFlags & (1u << bit)) {
                ++flagModels[1u << bit];
            }
        }
        if (m.loopSequence > 0) {
            ++notStandFirst;
        }
        Row row{i};
        row.instances = list.size();
        for (const Placement& p : list) {
            const i32 r = regionOf(p);
            if (r >= 0) {
                ++row.regions[static_cast<usize>(r)];
                ++regionAll[static_cast<usize>(r)];
                regionSkinned[static_cast<usize>(r)] += m.skinned() ? 1 : 0;
                regionAnimated[static_cast<usize>(r)] += m.animated() ? 1 : 0;
                regionBillboard[static_cast<usize>(r)] += m.billboardVertices > 0 ? 1 : 0;
                if (m.skinned()) {
                    regionModels[static_cast<usize>(r)].insert(i);
                }
            }
        }
        restPosed += !m.animated() && m.restOffset > bake::kDoodadMotionThreshold ? 1 : 0;
        if (!m.skinned()) {
            continue;
        }
        ++skinnedModels;
        skinnedInstances += list.size();
        skinnedVertices += m.vertices;
        movingVertices += m.movingVertices;
        skinnedBones += m.bones;
        if (m.animated()) {
            ++animatedModels;
            animatedInstances += list.size();
            globalModels += m.globalSequenceBones > 0 ? 1 : 0;
        }
        if (m.billboardVertices > 0) {
            ++billboardModels;
            billboardInstances += list.size();
        }
        rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.instances > b.instances; });

    std::printf("%-72s %5s %4s %4s %3s %3s %5s %5s %6s %6s %5s %6s",
                "model (skinned: moves or turns to the camera)", "loop", "seq", "bone", "anm", "gs", "bbBn",
                "verts", "moving", "maxyd", "rest", "inst");
    for (const Region& r : kRegions) {
        std::printf(" %5.5s", r.name);
    }
    std::printf("\n");
    for (usize k = 0; k < rows.size() && k < top; ++k) {
        const bake::DoodadMotion& m = motion[rows[k].model];
        std::printf("%-72s %5u %4u %4u %3u %3u %5u %5u %6u %6.3f %5.2f %6llu", models[rows[k].model].c_str(),
                    m.loopMs, m.sequences, m.bones, m.animatedBones, m.globalSequenceBones, m.billboardBones,
                    m.vertices, m.movingVertices, m.maxMotion, m.restOffset,
                    static_cast<unsigned long long>(rows[k].instances));
        for (usize r = 0; r < kRegionCount; ++r) {
            std::printf(" %5llu", static_cast<unsigned long long>(rows[k].regions[r]));
        }
        std::printf("\n");
        if (std::getenv("VD_SCAN_VERBOSE") && m.sequences > 1) {
            std::printf("    sequences (animation.variation): %s\n", sequenceNotes[rows[k].model].c_str());
        }
    }
    // --positions <text>: where the instances of the models whose path holds it stand (to aim the captures).
    if (const std::string wanted = option(options, "positions"); !wanted.empty()) {
        for (usize i = 0; i < models.size(); ++i) {
            if (models[i].find(wanted) == std::string::npos) {
                continue;
            }
            for (const Placement& p : placements[models[i]]) {
                std::printf("position %s %s %.1f %.1f %.1f\n", maps[p.map].directory.c_str(),
                            models[i].c_str(), p.position[0], p.position[1], p.position[2]);
            }
        }
    }
    for (const auto& [bit, count] : flagModels) {
        std::printf("bone flag 0x%-5x in %u placed models\n", bit, count);
    }
    for (usize i = 0; i < models.size(); ++i) {
        if (loaded[i] && motion[i].sequences > 1 && motion[i].skinned()) {
            u32 stands = 0;
            for (usize p = 0; p + 1 < sequenceNotes[i].size(); ++p) {
                stands +=
                    (p == 0 || sequenceNotes[i][p - 1] == ' ') && sequenceNotes[i].compare(p, 2, "0.") == 0
                        ? 1
                        : 0;
            }
            standVariations += stands > 1 ? 1 : 0;
        }
    }
    std::printf("placed models %zu (%u unreadable), instances %llu\n", models.size(), failed,
                static_cast<unsigned long long>(totalInstances));
    std::printf("skinned models %u (%llu instances): animated %u (%llu instances, %u with global sequences), "
                "billboards "
                "%u (%llu instances)\n",
                skinnedModels, static_cast<unsigned long long>(skinnedInstances), animatedModels,
                static_cast<unsigned long long>(animatedInstances), globalModels, billboardModels,
                static_cast<unsigned long long>(billboardInstances));
    std::printf(
        "skinned vertices %llu (%llu moving), bones %llu | static models posed off their bind pose %u | loop "
        "not the first sequence %u | skinned with several Stand variations %u\n",
        static_cast<unsigned long long>(skinnedVertices), static_cast<unsigned long long>(movingVertices),
        static_cast<unsigned long long>(skinnedBones), restPosed, notStandFirst, standVariations);
    for (usize r = 0; r < kRegionCount; ++r) {
        std::printf("%-10s doodads %6llu | skinned %5llu (%zu models) | animated %5llu | billboards %5llu\n",
                    kRegions[r].name, static_cast<unsigned long long>(regionAll[r]),
                    static_cast<unsigned long long>(regionSkinned[r]), regionModels[r].size(),
                    static_cast<unsigned long long>(regionAnimated[r]),
                    static_cast<unsigned long long>(regionBillboard[r]));
    }
    VD_LOG_INFO("scan-doodad-anim: %zu models, %.1f s", models.size(), secondsBetween(start, Clock::now()));
    return 0;
}

} // namespace vd::baker
