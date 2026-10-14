// Phase 14: worldsound.vdpk (pack/WorldSoundFormat.h) — the doodads' sound events, the buildings' rooms
// (WMOAreaTable: interior music, ambience and names) and the terrain's sound emitters — and their censuses.
#include "BakerCommon.h"

#include "bake/CollisionBaker.h"
#include "bake/ModelBaker.h"
#include "bake/PackWriter.h"
#include "bake/WorldSoundBaker.h"
#include "core/Clock.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "formats/AdtSound.h"
#include "formats/Dbc.h"
#include "formats/M2.h"
#include "formats/Wmo.h"
#include "jobs/JobSystem.h"
#include "pack/WorldSoundFormat.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace vd::baker {
namespace {

// SoundEntries.dbc (5875): 2 name, 25 flags (0x200 loop). WMOAreaTable.dbc: 1 WMOID, 6 ambience, 7 music, 8 intro,
// 10 area, 11 name.
constexpr u32 kSoundName = 2, kSoundFlags = 25;
constexpr u32 kSoundLoopFlag = 0x200;
constexpr u32 kWmoAreaWmoId = 1, kWmoAreaAmbience = 6, kWmoAreaMusic = 7, kWmoAreaIntro = 8, kWmoAreaArea = 10,
              kWmoAreaName = 11;          // world/WmoAreas.cpp
[[maybe_unused]] constexpr u32 kWorldSoundBakeVersion = 1; // bump when the floors or events bake differently

struct SoundRows {
    std::unordered_map<u32, u32> flags; // id -> flags
    std::unordered_map<u32, std::string> names;
};

SoundRows soundRows(const formats::MpqArchiveSet& mpq) {
    SoundRows rows;
    Result<std::vector<u8>> bytes = mpq.read("DBFilesClient\\SoundEntries.dbc");
    if (!bytes) {
        return rows;
    }
    Result<formats::DbcFile> table = formats::DbcFile::parse(std::move(bytes.value()));
    if (!table || table.value()->fieldCount() <= kSoundFlags) {
        return rows;
    }
    for (u32 r = 0; r < table.value()->recordCount(); ++r) {
        const u32 id = table.value()->getU32(r, 0);
        rows.flags[id] = table.value()->getU32(r, kSoundFlags);
        rows.names[id] = std::string(table.value()->getString(r, kSoundName));
    }
    return rows;
}

// The WMOs whose rows say something (a sound, an area or a name): the others leave a point outdoors anyway.
std::set<i32> wmoIdsWithRows(const formats::MpqArchiveSet& mpq) {
    std::set<i32> ids;
    Result<std::vector<u8>> bytes = mpq.read("DBFilesClient\\WMOAreaTable.dbc");
    if (!bytes) {
        return ids;
    }
    Result<formats::DbcFile> table = formats::DbcFile::parse(std::move(bytes.value()));
    if (!table || table.value()->fieldCount() <= kWmoAreaName) {
        return ids;
    }
    const db::DbcView& t = table.value().view();
    for (u32 r = 0; r < t.recordCount(); ++r) {
        if (t.getU32(r, kWmoAreaAmbience) != 0 || t.getU32(r, kWmoAreaMusic) != 0 || t.getU32(r, kWmoAreaIntro) != 0 ||
            t.getU32(r, kWmoAreaArea) != 0 || !t.getString(r, kWmoAreaName).empty()) {
            ids.insert(t.getI32(r, kWmoAreaWmoId));
        }
    }
    return ids;
}

bake::SoundRowLookup rowLookup(const SoundRows& rows) {
    return [&rows](u32 sound) {
        const auto it = rows.flags.find(sound);
        return bake::SoundRowInfo{it != rows.flags.end(), it != rows.flags.end() && (it->second & kSoundLoopFlag)};
    };
}

// What a WMO's doodad sets sound like: per doodad of the root (MODD), its model's sounds (null: none) and its placement
// in the WMO; the sets (MODS) index them.
struct WmoDoodadSounds {
    std::vector<formats::WmoDoodadSet> sets;
    std::vector<const bake::ModelSounds*> sounds;
    std::vector<std::array<f32, 12>> transforms;
    bool any = false;
};

// A WMO's root and groups, parsed.
struct ParsedWmo {
    bool ok = false;
    formats::WmoRoot root;
    std::vector<formats::WmoGroup> groups;
};

ParsedWmo parseWmo(const formats::MpqArchiveSet& mpq, const std::string& path, bool withGroups) {
    ParsedWmo out;
    Result<std::vector<u8>> bytes = mpq.read(path);
    Result<formats::WmoRoot> root =
        bytes ? formats::parseWmoRoot(bytes.value()) : Result<formats::WmoRoot>(bytes.error());
    if (!root) {
        return out;
    }
    out.root = std::move(root.value());
    if (withGroups) {
        for (u32 g = 0; g < out.root.groups.size(); ++g) {
            Result<std::vector<u8>> groupBytes = mpq.read(formats::wmoGroupPath(path, g));
            Result<formats::WmoGroup> group =
                groupBytes ? formats::parseWmoGroup(groupBytes.value()) : Result<formats::WmoGroup>(groupBytes.error());
            if (!group) {
                return out;
            }
            out.groups.push_back(std::move(group.value()));
        }
    }
    out.ok = true;
    return out;
}

} // namespace

int bakeWorldSound(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outPath) {
    const Clock::time_point start = Clock::now();
    const SoundRows rows = soundRows(mpq);
    const std::set<i32> wmoIds = wmoIdsWithRows(mpq);
    if (rows.flags.empty() || wmoIds.empty()) {
        VD_LOG_ERROR("worldsound: SoundEntries.dbc or WMOAreaTable.dbc unreadable");
        return 1;
    }
    bake::PackWriter writer;
    if (Result<void> created = writer.create(outPath, kBakerVersion); !created) {
        VD_LOG_ERROR("%s", created.error().message.c_str());
        return 1;
    }
    JobSystem jobs;
    jobs.init();
    std::mutex mutex;
    std::vector<std::string> failures;

    // The doodads' sound events: every M2 of the client (only those with sound events are kept), by canonical path.
    const std::vector<std::string> m2s = mpq.list("*.m2");
    std::map<std::string, bake::ModelSounds> modelSounds;
    const bake::SoundRowLookup lookup = rowLookup(rows);
    jobs.parallelFor(static_cast<u32>(m2s.size()), 16, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            Result<std::vector<u8>> bytes = mpq.read(m2s[i]);
            Result<formats::M2Model> model =
                bytes ? formats::parseM2(bytes.value()) : Result<formats::M2Model>(bytes.error());
            if (!model) {
                continue; // a few unreadable client files: scan-models reports them
            }
            bake::ModelSounds sounds = bake::modelSounds(model.value(), assetPathHash(m2s[i]), lookup);
            if (!sounds.events.empty()) {
                std::lock_guard<std::mutex> lock(mutex);
                modelSounds.emplace(normalizeAssetPath(m2s[i]), std::move(sounds));
            }
        }
    });
    const auto soundsOf = [&](std::string_view model) -> const bake::ModelSounds* {
        const auto it = modelSounds.find(normalizeAssetPath(formats::m2Path(model)));
        return it == modelSounds.end() ? nullptr : &it->second;
    };
    std::map<std::string, WmoDoodadSounds> wmoDoodads; // every WMO root seen (maps share buildings)
    u32 doodadSoundsTotal = 0, mapsWithDoodadSounds = 0;

    // Per map: the buildings with WMOAreaTable rows and the terrain's emitters.
    std::map<std::string, i32> wmoIdByPath;                  // every WMO root seen: its MOHD id (-1 unreadable)
    std::map<std::string, bake::WmoAreaModelData> builtWmos; // those with rows: groups and floors (maps share some)
    u32 placementsTotal = 0, emittersTotal = 0, mapsWithAreas = 0;
    u64 areaBytes = 0;
    for (const MapInfo& map : listMaps(mpq)) {
        struct Placement {
            std::string model;
            formats::AdtWmoPlacement p;
        };
        struct DoodadPlacement {
            std::string model;
            formats::AdtDoodadPlacement p;
        };
        std::vector<Placement> placements;
        std::vector<DoodadPlacement> doodads;
        std::vector<formats::AdtSoundEmitter> emitters;
        std::vector<u32> tiles;
        for (u32 ty = 0; ty < 64; ++ty) {
            for (u32 tx = 0; tx < 64; ++tx) {
                if (map.wdt.hasTile(tx, ty)) {
                    tiles.push_back(ty * 64 + tx);
                }
            }
        }
        jobs.parallelFor(static_cast<u32>(tiles.size()), 4, [&](u32 begin, u32 end) {
            for (u32 i = begin; i < end; ++i) {
                const std::string path = formats::adtPath(map.directory, tiles[i] % 64, tiles[i] / 64);
                Result<std::vector<u8>> bytes = mpq.read(path);
                if (!bytes) {
                    std::lock_guard<std::mutex> lock(mutex);
                    failures.push_back(path + ": " + bytes.error().message);
                    continue;
                }
                Result<formats::AdtPlacements> wmos = formats::parseAdtPlacements(bytes.value());
                Result<std::vector<formats::AdtSoundEmitter>> sounds = formats::parseAdtSoundEmitters(bytes.value());
                std::lock_guard<std::mutex> lock(mutex);
                if (!wmos || !sounds) {
                    failures.push_back(path + ": " + (wmos ? sounds.error().message : wmos.error().message));
                    continue;
                }
                for (const formats::AdtWmoPlacement& p : wmos.value().placements) {
                    if (p.nameId < wmos.value().wmos.size()) {
                        placements.push_back({normalizeAssetPath(wmos.value().wmos[p.nameId]), p});
                    }
                }
                for (const formats::AdtDoodadPlacement& p : wmos.value().doodads) {
                    if (p.nameId < wmos.value().models.size() && soundsOf(wmos.value().models[p.nameId])) {
                        doodads.push_back({wmos.value().models[p.nameId], p});
                    }
                }
                emitters.insert(emitters.end(), sounds.value().begin(), sounds.value().end());
            }
        });
        if (map.wdt.hasWmoPlacement) {
            const formats::WdtWmoPlacement& w = map.wdt.wmoPlacement;
            formats::AdtWmoPlacement p;
            p.uniqueId = w.uniqueId;
            std::copy(std::begin(w.position), std::end(w.position), std::begin(p.position));
            std::copy(std::begin(w.rotation), std::end(w.rotation), std::begin(p.rotation));
            p.doodadSet = w.doodadSet;
            p.nameSet = w.nameSet;
            placements.push_back({normalizeAssetPath(map.wdt.wmoName), p});
        }
        if (!emitters.empty()) {
            // Stable (phase 22): equal ids keep the tiles' order on every standard library.
            std::stable_sort(emitters.begin(), emitters.end(),
                             [](const formats::AdtSoundEmitter& a, const formats::AdtSoundEmitter& b) {
                                 return a.id < b.id;
                             });
            if (Result<void> added = writer.add(pack::soundEmittersPath(map.directory), pack::AssetType::SoundEmitters,
                                                bake::bakeSoundEmitters(emitters), true);
                !added) {
                failures.push_back(added.error().message);
            }
            emittersTotal += static_cast<u32>(emitters.size());
        }

        // One placement per building (it is listed by every tile it reaches), in uniqueId order.
        std::stable_sort(placements.begin(), placements.end(), // phase 22: the same first one everywhere
                         [](const Placement& a, const Placement& b) { return a.p.uniqueId < b.p.uniqueId; });
        placements.erase(
            std::unique(placements.begin(), placements.end(),
                        [](const Placement& a, const Placement& b) { return a.p.uniqueId == b.p.uniqueId; }),
            placements.end());
        // The WMOs' ids (their roots), then the groups and floors of those with rows.
        std::vector<std::string> pending;
        for (const Placement& p : placements) {
            if (!wmoIdByPath.contains(p.model)) {
                wmoIdByPath[p.model] = -2; // to read
                pending.push_back(p.model);
            }
        }
        std::vector<i32> pendingIds(pending.size(), -1);
        std::vector<WmoDoodadSounds> pendingDoodads(pending.size());
        jobs.parallelFor(static_cast<u32>(pending.size()), 4, [&](u32 begin, u32 end) {
            for (u32 i = begin; i < end; ++i) {
                const ParsedWmo root = parseWmo(mpq, pending[i], false);
                pendingIds[i] = root.ok ? static_cast<i32>(root.root.areaTableId) : -1;
                // Its doodads that sound (modelSounds is complete and read-only by now).
                WmoDoodadSounds& d = pendingDoodads[i];
                d.sets = root.root.doodadSets;
                for (const formats::WmoDoodad& doodad : root.root.doodads) {
                    const bake::ModelSounds* sounds = doodad.model.empty() ? nullptr : soundsOf(doodad.model);
                    d.sounds.push_back(sounds);
                    d.transforms.push_back(sounds ? bake::doodadTransform(doodad) : std::array<f32, 12>{});
                    d.any = d.any || sounds;
                }
            }
        });
        std::vector<std::string> needFloors;
        for (usize i = 0; i < pending.size(); ++i) {
            wmoDoodads.emplace(pending[i], std::move(pendingDoodads[i]));
            wmoIdByPath[pending[i]] = pendingIds[i];
            if (pendingIds[i] >= 0 && wmoIds.contains(pendingIds[i])) {
                needFloors.push_back(pending[i]);
            }
        }
        std::vector<bake::WmoAreaModelData> built(needFloors.size());
        std::vector<bool> builtOk(needFloors.size(), false);
        jobs.parallelFor(static_cast<u32>(needFloors.size()), 1, [&](u32 begin, u32 end) {
            for (u32 i = begin; i < end; ++i) {
                const ParsedWmo wmo = parseWmo(mpq, needFloors[i], true);
                if (!wmo.ok) {
                    std::lock_guard<std::mutex> lock(mutex);
                    failures.push_back(needFloors[i] + ": unreadable WMO");
                    continue;
                }
                built[i] = bake::wmoAreaModel(wmo.root, wmo.groups, assetPathHash(needFloors[i]));
                builtOk[i] = true;
            }
        });
        for (usize i = 0; i < needFloors.size(); ++i) {
            if (builtOk[i]) {
                builtWmos.emplace(needFloors[i], std::move(built[i]));
            }
        }
        // The doodads' sounds of the map: those placed on the terrain (one per uniqueId: a doodad is listed by every
        // tile it reaches), then those of the buildings' doodad sets (the default one and the placement's, as the
        // runtime shows them).
        std::stable_sort(doodads.begin(), doodads.end(), // phase 22: the same first one everywhere
                         [](const DoodadPlacement& a, const DoodadPlacement& b) {
                             return a.p.uniqueId < b.p.uniqueId;
                         });
        doodads.erase(std::unique(doodads.begin(), doodads.end(),
                                  [](const DoodadPlacement& a, const DoodadPlacement& b) {
                                      return a.p.uniqueId == b.p.uniqueId;
                                  }),
                      doodads.end());
        std::vector<pack::DoodadSound> doodadSounds;
        for (const DoodadPlacement& d : doodads) {
            bake::placeDoodadSounds(*soundsOf(d.model),
                                    bake::placementTransform(d.p.position, d.p.rotation, d.p.scale / 1024.0f),
                                    doodadSounds);
        }
        for (const Placement& p : placements) {
            const auto wmo = wmoDoodads.find(p.model);
            if (wmo == wmoDoodads.end() || !wmo->second.any) {
                continue;
            }
            const WmoDoodadSounds& d = wmo->second;
            const std::array<f32, 12> transform = bake::placementTransform(p.p.position, p.p.rotation, 1.0f);
            const u16 sets[2] = {0, p.p.doodadSet};
            for (u32 s = 0; s < (p.p.doodadSet == 0 ? 1u : 2u); ++s) {
                if (sets[s] >= d.sets.size()) {
                    continue;
                }
                const formats::WmoDoodadSet& range = d.sets[sets[s]];
                for (u32 i = range.first; i < range.first + range.count && i < d.sounds.size(); ++i) {
                    if (d.sounds[i]) {
                        bake::placeDoodadSounds(*d.sounds[i], bake::multiplyTransforms(transform, d.transforms[i]),
                                                doodadSounds);
                    }
                }
            }
        }
        if (!doodadSounds.empty()) {
            doodadSoundsTotal += static_cast<u32>(doodadSounds.size());
            ++mapsWithDoodadSounds;
            if (Result<void> added = writer.add(pack::modelSoundsPath(map.directory), pack::AssetType::ModelSounds,
                                                bake::bakeDoodadSounds(std::move(doodadSounds)), true);
                !added) {
                failures.push_back(added.error().message);
            }
        }

        // The map's table: the placements of WMOs with rows, their models in first-use order.
        std::map<std::string, u32> modelIndex;
        std::vector<bake::WmoAreaModelData> models;
        std::vector<bake::WmoAreaPlacementData> areaPlacements;
        for (const Placement& p : placements) {
            const auto wmo = builtWmos.find(p.model);
            if (wmo == builtWmos.end()) {
                continue;
            }
            auto [index, inserted] = modelIndex.emplace(p.model, static_cast<u32>(models.size()));
            if (inserted) {
                models.push_back(wmo->second);
            }
            bake::WmoAreaPlacementData placement;
            placement.uniqueId = p.p.uniqueId;
            placement.nameSet = p.p.nameSet;
            placement.model = index->second;
            placement.transform = bake::placementTransform(p.p.position, p.p.rotation, 1.0f);
            areaPlacements.push_back(placement);
        }
        if (!areaPlacements.empty()) {
            std::vector<u8> blob = bake::bakeWmoAreas(areaPlacements, models);
            areaBytes += blob.size();
            if (Result<void> added =
                    writer.add(pack::wmoAreasPath(map.directory), pack::AssetType::WmoAreas, blob, true);
                !added) {
                failures.push_back(added.error().message);
            }
            placementsTotal += static_cast<u32>(areaPlacements.size());
            ++mapsWithAreas;
        }
    }
    jobs.shutdown();
    if (Result<void> finished = writer.finish(); !finished) {
        VD_LOG_ERROR("%s", finished.error().message.c_str());
        return 1;
    }
    for (const std::string& failure : failures) {
        VD_LOG_ERROR("FAILED %s", failure.c_str());
    }
    VD_LOG_INFO("worldsound: %zu doodad models with sound events, %u placed in %u maps | %u maps with WMO areas (%u "
                "placements, %.1f MB before LZ4) | %u terrain emitters | %zu failed | %.1f MB pack | %.1f s -> %s",
                modelSounds.size(), doodadSoundsTotal, mapsWithDoodadSounds, mapsWithAreas, placementsTotal,
                static_cast<f64>(areaBytes) / 1e6, emittersTotal, failures.size(),
                static_cast<f64>(writer.bytesWritten()) / 1e6, secondsBetween(start, Clock::now()),
                outPath.string().c_str());
    return failures.empty() ? 0 : 1;
}

int commandScanEmitters(const Options& options) {
    formats::MpqArchiveSet mpq;
    if (!openArchives(options, mpq)) {
        return 1;
    }
    const SoundRows rows = soundRows(mpq);
    u32 total = 0, tilesWith = 0, missing = 0;
    for (const MapInfo& map : listMaps(mpq)) {
        for (u32 ty = 0; ty < 64; ++ty) {
            for (u32 tx = 0; tx < 64; ++tx) {
                if (!map.wdt.hasTile(tx, ty)) {
                    continue;
                }
                const std::string path = formats::adtPath(map.directory, tx, ty);
                Result<std::vector<u8>> bytes = mpq.read(path);
                Result<std::vector<formats::AdtSoundEmitter>> emitters =
                    bytes ? formats::parseAdtSoundEmitters(bytes.value())
                          : Result<std::vector<formats::AdtSoundEmitter>>(bytes.error());
                if (!emitters) {
                    VD_LOG_WARN("%s: %s", path.c_str(), emitters.error().message.c_str());
                    continue;
                }
                tilesWith += emitters.value().empty() ? 0 : 1;
                for (const formats::AdtSoundEmitter& e : emitters.value()) {
                    const auto name = rows.names.find(e.sound);
                    missing += name == rows.names.end() ? 1 : 0;
                    std::printf("%s chunk %u,%u | id %u sound %u %s | (%.1f %.1f %.1f) | distances %.1f %.1f %.1f | "
                                "minutes %u-%u mode %u loops %u-%u silence %u-%u ms instances %u-%u gap %u-%u ms\n",
                                path.c_str(), e.chunkX, e.chunkY, e.id, e.sound,
                                name != rows.names.end() ? name->second.c_str() : "(not in SoundEntries)",
                                e.position[0], e.position[1], e.position[2], e.minDistance, e.maxDistance,
                                e.cutoffDistance, e.startMinute, e.endMinute, e.mode, e.loopCountMin, e.loopCountMax,
                                e.silenceMinMs, e.silenceMaxMs, e.instancesMin, e.instancesMax, e.gapMinMs, e.gapMaxMs);
                    ++total;
                }
            }
        }
    }
    VD_LOG_INFO("%u terrain sound emitters in %u tiles, %u naming a sound the client does not have", total, tilesWith,
                missing);
    return 0;
}

} // namespace vd::baker
