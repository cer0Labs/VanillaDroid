#include "BakerCommon.h"

#include "bake/ModelBaker.h"
#include "bake/PackWriter.h"
#include "bake/SkyBaker.h"
#include "core/Clock.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "formats/Adt.h"
#include "formats/Dbc.h"
#include "formats/M2.h"
#include "formats/Wmo.h"
#include "jobs/JobSystem.h"
#include "pack/SkyFormat.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace vd::baker {
namespace {

bool isWmoGroupFile(const std::string& path) {
    // Group files end in _NNN.wmo.
    if (path.size() < 9) {
        return false;
    }
    const usize dot = path.size() - 4;
    return path[dot - 4] == '_' && std::isdigit(static_cast<u8>(path[dot - 1])) &&
           std::isdigit(static_cast<u8>(path[dot - 2])) && std::isdigit(static_cast<u8>(path[dot - 3]));
}

Result<std::vector<u8>> bakeWmoFile(const formats::MpqArchiveSet& mpq, const std::string& path,
                                    std::vector<std::string>& doodadModels, std::string& skybox) {
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
    for (const formats::WmoDoodad& d : root.value().doodads) {
        doodadModels.push_back(normalizeAssetPath(formats::m2Path(d.model)));
    }
    skybox = root.value().skybox;
    return bake::bakeWmo(root.value(), groups);
}

// Phase 11: the sky models (pack/SkyFormat.h) -- the stars WoW.exe draws at night (its
// "Environments\Stars\stars.mdl"), the skyboxes of LightSkybox.dbc and those the WMOs name (MOSB) -- under
// "sky/<m2 path>".
u32 bakeSkyModels(const formats::MpqArchiveSet& mpq, std::set<std::string> references,
                  bake::PackWriter& writer, std::vector<std::string>& failures) {
    references.insert("Environments\\Stars\\Stars.m2");
    if (Result<std::vector<u8>> bytes = mpq.read("DBFilesClient\\LightSkybox.dbc"); bytes) {
        if (Result<formats::DbcFile> table = formats::DbcFile::parse(std::move(bytes.value())); table) {
            for (u32 r = 0; r < table.value()->recordCount(); ++r) {
                references.insert(std::string(table.value()->getString(r, 1)));
            }
        }
    }
    std::set<std::string> baked;
    for (const std::string& reference : references) {
        const std::string path = pack::skyModelPath(reference);
        const std::string m2 = path.empty() ? std::string() : path.substr(4); // without "sky/"
        if (path.empty() || !mpq.contains(m2) || !baked.insert(path).second) {
            continue; // not a model (AZ_SunkenTemple's MOSB is a title), or one the MPQs lack
        }
        Result<std::vector<u8>> bytes = mpq.read(m2);
        Result<formats::M2Model> model =
            bytes ? formats::parseM2(bytes.value()) : Result<formats::M2Model>(bytes.error());
        Result<std::vector<u8>> blob =
            model ? bake::bakeSkyModel(model.value()) : Result<std::vector<u8>>(model.error());
        Result<void> added = blob ? writer.add(path, pack::AssetType::SkyModel, blob.value())
                                  : Result<void>(blob.error());
        if (!added) {
            failures.push_back(path + ": " + added.error().message);
            baked.erase(path);
        }
    }
    return static_cast<u32>(baked.size());
}

} // namespace

// Every WMO of the client and every M2 that the world places (ADT MMDX, WMO doodad sets) -> models.vdpk.
int bakeModels(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outPath) {
    const Clock::time_point start = Clock::now();
    bake::PackWriter writer;
    if (Result<void> created = writer.create(outPath, kBakerVersion); !created) {
        VD_LOG_ERROR("%s", created.error().message.c_str());
        return 1;
    }
    std::vector<std::string> wmos;
    for (const std::string& path : mpq.list("*.wmo")) {
        if (!isWmoGroupFile(path)) {
            wmos.push_back(path);
        }
    }
    const std::vector<std::string> adts = mpq.list("World\\Maps\\*.adt");

    std::mutex mutex;
    std::set<std::string> m2s;
    std::set<std::string> skyboxes; // MOSB references
    std::vector<std::string> failures;
    JobSystem jobs;
    jobs.init();
    // Doodads placed on terrain.
    jobs.parallelFor(static_cast<u32>(adts.size()), 4, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            Result<std::vector<u8>> bytes = mpq.read(adts[i]);
            Result<formats::Adt> adt = bytes ? formats::parseAdt(bytes.value()) : Result<formats::Adt>(bytes.error());
            if (!adt) {
                continue; // orphan or broken ADT: scan-terrain reports the ones maps use
            }
            std::lock_guard<std::mutex> lock(mutex);
            for (const std::string& model : adt.value().models) {
                m2s.insert(normalizeAssetPath(formats::m2Path(model)));
            }
        }
    });
    // Objects the server spawns (chests, quest objects, doors, mailboxes...): GameObjectDisplayInfo.dbc field 1 is
    // the model (".mdx" for M2; WMOs are all baked above).
    if (Result<std::vector<u8>> bytes = mpq.read("DBFilesClient\\GameObjectDisplayInfo.dbc"); bytes) {
        if (Result<formats::DbcFile> table = formats::DbcFile::parse(std::move(bytes.value())); table) {
            for (u32 r = 0; r < table.value()->recordCount(); ++r) {
                const std::string path(table.value()->getString(r, 1));
                const usize dot = path.find_last_of('.');
                if (path.size() > 4 && dot != std::string::npos && path.compare(dot, std::string::npos, ".wmo") != 0 &&
                    path.compare(dot, std::string::npos, ".WMO") != 0) {
                    m2s.insert(normalizeAssetPath(formats::m2Path(path)));
                }
            }
        }
    }
    // WMOs (and their doodads).
    u64 wmoBytes = 0;
    jobs.parallelFor(static_cast<u32>(wmos.size()), 1, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            std::vector<std::string> doodads;
            std::string skybox;
            Result<std::vector<u8>> blob = bakeWmoFile(mpq, wmos[i], doodads, skybox);
            Result<void> added = blob ? writer.add(wmos[i], pack::AssetType::Model, blob.value(), true)
                                      : Result<void>(blob.error());
            std::lock_guard<std::mutex> lock(mutex);
            if (!added) {
                failures.push_back(wmos[i] + ": " + added.error().message);
                continue;
            }
            wmoBytes += blob.value().size();
            m2s.insert(doodads.begin(), doodads.end());
            if (!skybox.empty()) {
                skyboxes.insert(skybox);
            }
        }
    });
    // M2 doodads.
    const std::vector<std::string> models(m2s.begin(), m2s.end());
    u32 missing = 0;
    u64 m2Bytes = 0;
    std::atomic<u32> invisibleBatches{0};
    jobs.parallelFor(static_cast<u32>(models.size()), 8, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            if (!mpq.contains(models[i])) {
                std::lock_guard<std::mutex> lock(mutex);
                ++missing;
                continue;
            }
            Result<std::vector<u8>> bytes = mpq.read(models[i]);
            Result<formats::M2Model> m2 = bytes ? formats::parseM2(bytes.value()) : Result<formats::M2Model>(bytes.error());
            u32 invisible = 0;
            Result<std::vector<u8>> blob =
                m2 ? bake::bakeM2(m2.value(), &invisible) : Result<std::vector<u8>>(m2.error());
            Result<void> added = blob ? writer.add(models[i], pack::AssetType::Model, blob.value(), true)
                                      : Result<void>(blob.error());
            invisibleBatches += invisible;
            std::lock_guard<std::mutex> lock(mutex);
            if (!added) {
                failures.push_back(models[i] + ": " + added.error().message);
            } else {
                m2Bytes += blob.value().size();
            }
        }
    });
    jobs.shutdown();
    const u32 skyModels = bakeSkyModels(mpq, skyboxes, writer, failures);
    for (const std::string& failure : failures) {
        VD_LOG_ERROR("FAILED %s", failure.c_str());
    }
    if (Result<void> finished = writer.finish(); !finished) {
        VD_LOG_ERROR("%s", finished.error().message.c_str());
        return 1;
    }
    VD_LOG_INFO("models: %zu WMO (%.1f MB) + %zu M2 (%.1f MB, %u referenced but missing, %u invisible batches left "
                "out) + %u sky models, %zu failed | %.1f MB in pack | %.1f s -> %s",
                wmos.size(), static_cast<double>(wmoBytes) / 1e6, models.size() - missing,
                static_cast<double>(m2Bytes) / 1e6, missing, invisibleBatches.load(), skyModels, failures.size(),
                static_cast<double>(writer.bytesWritten()) / 1e6, secondsBetween(start, Clock::now()),
                outPath.string().c_str());
    return failures.empty() ? 0 : 1;
}

int commandScanModels(const Options& options) {
    formats::MpqArchiveSet mpq;
    if (!openArchives(options, mpq)) {
        return 1;
    }
    const Clock::time_point start = Clock::now();
    std::vector<std::string> roots;
    for (const std::string& path : mpq.list("*.wmo")) {
        if (!isWmoGroupFile(path)) {
            roots.push_back(path);
        }
    }

    std::mutex mutex;
    std::vector<std::string> failures;
    std::map<std::string, u32> blendShader;
    std::set<std::string> doodadModels;
    std::set<std::string> textures;
    std::map<std::string, std::string> skyboxes; // phase 11: WMO -> its MOSB skybox model
    u64 groups = 0, vertices = 0, triangles = 0, batches = 0, doodads = 0, portals = 0;
    u64 groupsWithColors = 0, interiorGroups = 0, exteriorGroups = 0, collisionOnlyTriangles = 0;
    u32 maxGroupVertices = 0, maxGroups = 0;
    std::string largest;

    JobSystem jobs;
    jobs.init();
    jobs.parallelFor(static_cast<u32>(roots.size()), 1, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            const std::string& path = roots[i];
            Result<std::vector<u8>> bytes = mpq.read(path);
            Result<formats::WmoRoot> root =
                bytes ? formats::parseWmoRoot(bytes.value()) : Result<formats::WmoRoot>(bytes.error());
            if (!root) {
                std::lock_guard<std::mutex> lock(mutex);
                failures.push_back(path + ": " + root.error().message);
                continue;
            }
            u64 localVertices = 0, localTriangles = 0, localBatches = 0, localColors = 0, localInterior = 0;
            u64 localExterior = 0, localCollision = 0;
            u32 localMax = 0;
            std::string error;
            for (u32 g = 0; g < root.value().groups.size() && error.empty(); ++g) {
                const std::string groupPath = formats::wmoGroupPath(path, g);
                Result<std::vector<u8>> groupBytes = mpq.read(groupPath);
                Result<formats::WmoGroup> group =
                    groupBytes ? formats::parseWmoGroup(groupBytes.value()) : Result<formats::WmoGroup>(groupBytes.error());
                if (!group) {
                    error = groupPath + ": " + group.error().message;
                    break;
                }
                const formats::WmoGroup& grp = group.value();
                localVertices += grp.positions.size();
                localTriangles += grp.indices.size() / 3;
                localBatches += grp.batches.size();
                localColors += grp.colors.empty() ? 0 : 1;
                localInterior += (grp.flags & formats::kWmoGroupInterior) ? 1 : 0;
                localExterior += (grp.flags & formats::kWmoGroupExterior) ? 1 : 0;
                localMax = std::max(localMax, static_cast<u32>(grp.positions.size()));
                for (u8 material : grp.triangleMaterials) {
                    localCollision += material == formats::kWmoTriangleCollisionOnly ? 1 : 0;
                }
                for (const formats::WmoBatch& b : grp.batches) {
                    if (b.material >= root.value().materials.size()) {
                        error = groupPath + ": batch material out of range";
                    }
                }
            }
            std::lock_guard<std::mutex> lock(mutex);
            if (!error.empty()) {
                failures.push_back(error);
                continue;
            }
            groups += root.value().groups.size();
            vertices += localVertices;
            triangles += localTriangles;
            batches += localBatches;
            groupsWithColors += localColors;
            interiorGroups += localInterior;
            exteriorGroups += localExterior;
            collisionOnlyTriangles += localCollision;
            doodads += root.value().doodads.size();
            portals += root.value().portals.size();
            maxGroupVertices = std::max(maxGroupVertices, localMax);
            if (root.value().groups.size() > maxGroups) {
                maxGroups = static_cast<u32>(root.value().groups.size());
                largest = path;
            }
            for (const formats::WmoMaterial& m : root.value().materials) {
                ++blendShader["shader " + std::to_string(m.shader) + " blend " + std::to_string(m.blendMode)];
                textures.insert(normalizeAssetPath(m.texture1));
            }
            for (const formats::WmoDoodad& d : root.value().doodads) {
                doodadModels.insert(normalizeAssetPath(formats::m2Path(d.model)));
            }
            if (!root.value().skybox.empty()) {
                skyboxes[path] = root.value().skybox;
            }
        }
    });
    jobs.shutdown();

    for (const auto& [wmo, skybox] : skyboxes) {
        std::printf("skybox (MOSB) %-60s %s%s\n", wmo.c_str(), skybox.c_str(),
                    mpq.contains(normalizeAssetPath(formats::m2Path(skybox))) ? "" : " (missing)");
    }
    u32 missingModels = 0, missingTextures = 0;
    for (const std::string& model : doodadModels) {
        missingModels += mpq.contains(model) ? 0 : 1;
    }
    for (const std::string& texture : textures) {
        missingTextures += texture.empty() || mpq.contains(texture) ? 0 : 1;
    }
    for (const auto& [key, count] : blendShader) {
        std::printf("%-22s %6u materials\n", key.c_str(), count);
    }
    for (const std::string& failure : failures) {
        std::printf("FAILED %s\n", failure.c_str());
    }
    std::printf("groups %llu (%llu with vertex colors, %llu interior, %llu exterior) | vertices %llu | triangles %llu "
                "(%llu collision-only) | batches %llu\n",
                (unsigned long long)groups, (unsigned long long)groupsWithColors, (unsigned long long)interiorGroups,
                (unsigned long long)exteriorGroups, (unsigned long long)vertices, (unsigned long long)triangles,
                (unsigned long long)collisionOnlyTriangles, (unsigned long long)batches);
    std::printf("doodads %llu (%zu distinct models, %u missing) | portals %llu | textures %zu (%u missing)\n",
                (unsigned long long)doodads, doodadModels.size(), missingModels, (unsigned long long)portals,
                textures.size(), missingTextures);
    std::printf("max vertices in a group %u | most groups: %u (%s)\n", maxGroupVertices, maxGroups, largest.c_str());
    VD_LOG_INFO("%zu WMO roots, %zu failed, %.1f s", roots.size(), failures.size(), secondsBetween(start, Clock::now()));

    // Every M2 in the client.
    const std::vector<std::string> m2s = mpq.list("*.m2");
    std::vector<std::string> m2Failures;
    std::map<std::string, u32> m2Blends, m2TextureTypes, m2Versions;
    u64 m2Triangles = 0, m2Batches = 0, untextured = 0, animated = 0, twoSided = 0;
    u32 maxVertices = 0;
    std::string biggest;
    jobs.init();
    jobs.parallelFor(static_cast<u32>(m2s.size()), 8, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            Result<std::vector<u8>> bytes = mpq.read(m2s[i]);
            Result<formats::M2Model> model = bytes ? formats::parseM2(bytes.value()) : Result<formats::M2Model>(bytes.error());
            std::lock_guard<std::mutex> lock(mutex);
            if (!model) {
                m2Failures.push_back(m2s[i] + ": " + model.error().message);
                continue;
            }
            const formats::M2Model& m = model.value();
            m2Triangles += m.triangles.size() / 3;
            m2Batches += m.batches.size();
            animated += m.sequenceCount > 1 ? 1 : 0;
            if (m.vertices.size() > maxVertices) {
                maxVertices = static_cast<u32>(m.vertices.size());
                biggest = m2s[i];
            }
            for (const formats::M2Batch& b : m.batches) {
                untextured += b.texture < 0 ? 1 : 0;
                const formats::M2Material& material = m.materials[b.material];
                ++m2Blends["blend " + std::to_string(static_cast<u32>(material.blend))];
                twoSided += (material.flags & formats::kM2MaterialTwoSided) ? 1 : 0;
            }
            for (const formats::M2Texture& t : m.textures) {
                ++m2TextureTypes["texture type " + std::to_string(t.type)];
            }
        }
    });
    jobs.shutdown();
    for (const auto& [key, count] : m2Blends) {
        std::printf("M2 %-18s %6u batches\n", key.c_str(), count);
    }
    for (const auto& [key, count] : m2TextureTypes) {
        std::printf("M2 %-18s %6u\n", key.c_str(), count);
    }
    for (const std::string& failure : m2Failures) {
        std::printf("FAILED %s\n", failure.c_str());
    }
    std::printf("M2: %zu models, %llu triangles, %llu batches (%llu untextured, %llu two-sided), %llu animated | most "
                "vertices %u (%s)\n",
                m2s.size(), (unsigned long long)m2Triangles, (unsigned long long)m2Batches, (unsigned long long)untextured,
                (unsigned long long)twoSided, (unsigned long long)animated, maxVertices, biggest.c_str());
    VD_LOG_INFO("%zu M2 models, %zu failed", m2s.size(), m2Failures.size());
    return failures.empty() && m2Failures.empty() ? 0 : 1;
}

} // namespace vd::baker
