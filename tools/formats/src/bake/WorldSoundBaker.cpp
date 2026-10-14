#include "bake/WorldSoundBaker.h"

#include "audio/ModelSoundRules.h"
#include "bake/BakeUtil.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace vd::bake {
namespace {

// MOPY flags, and the triangles vMaNGOS' vmap extractor keeps (bake/CollisionBaker.cpp's rule).
constexpr u8 kMopyDetail = 0x04;
constexpr u8 kMopyCollision = 0x08;
constexpr u8 kMopyRender = 0x20;

// A triangle steeper than this (|normal.z| below it) is a wall: a ray down never stops on it.
constexpr f32 kWallNormalZ = 0.05f;
// Spacing of the points sampled over a triangle to find the cells it covers.
constexpr f32 kSampleStep = 0.5f;

using Vec3 = std::array<f32, 3>;

u8 floorUnits(f32 heightAboveBox, f32 step) {
    const f32 units = std::round(heightAboveBox / step);
    return static_cast<u8>(std::clamp(units, 0.0f, static_cast<f32>(pack::kWmoNoFloor - 1)));
}

// Records the triangle (a, b, c) into the cells of `group` it covers, by its height at points over it.
void rasterizeTriangle(const Vec3& a, const Vec3& b, const Vec3& c, WmoAreaGroupData& group) {
    const pack::WmoAreaGroup& g = group.group;
    const Vec3 ab{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const Vec3 ac{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const Vec3 n{ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]};
    const f32 length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (length <= 1e-6f || std::fabs(n[2]) < kWallNormalZ * length) {
        return; // degenerate, or a wall
    }
    const auto record = [&](f32 x, f32 y, f32 z) {
        const i32 cx = static_cast<i32>(std::floor((x - g.boundsMin[0]) / pack::kWmoFloorCell));
        const i32 cy = static_cast<i32>(std::floor((y - g.boundsMin[1]) / pack::kWmoFloorCell));
        if (cx < 0 || cy < 0 || cx >= g.cellsX || cy >= g.cellsY) {
            return;
        }
        const u8 h = floorUnits(z - g.boundsMin[2], g.floorStep);
        u8* cell = &group.cells[(static_cast<usize>(cy) * g.cellsX + static_cast<usize>(cx)) * 2];
        if (cell[0] == pack::kWmoNoFloor) {
            cell[0] = cell[1] = h;
        } else {
            cell[0] = std::min(cell[0], h);
            cell[1] = std::max(cell[1], h);
        }
    };
    for (const Vec3* v : {&a, &b, &c}) {
        record((*v)[0], (*v)[1], (*v)[2]);
    }
    // Points over the triangle: a grid over its box, kept when inside (barycentric), at the plane's height.
    const f32 minX = std::min({a[0], b[0], c[0]}), maxX = std::max({a[0], b[0], c[0]});
    const f32 minY = std::min({a[1], b[1], c[1]}), maxY = std::max({a[1], b[1], c[1]});
    const f32 det = ab[0] * ac[1] - ab[1] * ac[0]; // the triangle's XY area * 2 (not 0: not a wall)
    for (f32 y = std::floor(minY / kSampleStep) * kSampleStep; y <= maxY; y += kSampleStep) {
        for (f32 x = std::floor(minX / kSampleStep) * kSampleStep; x <= maxX; x += kSampleStep) {
            const f32 px = x - a[0], py = y - a[1];
            const f32 u = (px * ac[1] - py * ac[0]) / det;
            const f32 v = (ab[0] * py - ab[1] * px) / det;
            if (u < -1e-4f || v < -1e-4f || u + v > 1.0001f) {
                continue;
            }
            record(x, y, a[2] + u * ab[2] + v * ac[2]);
        }
    }
}

Vec3 transformPoint(const std::array<f32, 12>& m, const Vec3& p) {
    return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
            m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
}

} // namespace

// --- Doodad sound events -----------------------------------------------------------------------------------------

ModelSounds modelSounds(const formats::M2Model& model, u64 hash, const SoundRowLookup& rows) {
    ModelSounds out;
    out.model = hash;
    // The Stand animation a doodad loops: the first sequence of animation 0.
    i32 stand = -1;
    for (u32 s = 0; s < model.sequences.size(); ++s) {
        if (model.sequences[s].animationId == 0) {
            stand = static_cast<i32>(s);
            break;
        }
    }
    const formats::M2Sequence* sequence = stand >= 0 ? &model.sequences[static_cast<u32>(stand)] : nullptr;
    out.cycleMs = sequence && sequence->end > sequence->start ? sequence->end - sequence->start : 0;
    for (const formats::M2Event& e : model.events) {
        const audio::ModelEventKind kind = audio::classifyModelEvent(e.id);
        if (kind != audio::ModelEventKind::DoodadLoop && kind != audio::ModelEventKind::DoodadOneShot &&
            kind != audio::ModelEventKind::Sound) {
            continue;
        }
        const SoundRowInfo row = rows(e.data);
        if (!row.exists) {
            continue; // a few name rows the client does not have (the banners' 15721)
        }
        pack::ModelSoundEvent baked{};
        baked.sound = e.data;
        for (u32 a = 0; a < 3; ++a) {
            baked.position[a] = e.position[a];
        }
        // Its times in the Stand sequence: those inside the sequence's window of the track timeline (the 1.12
        // event ranges are unreliable, as the actor bake found).
        std::vector<u32> times;
        if (sequence && e.globalSequence < 0) {
            for (const u32 t : e.times) {
                if (t >= sequence->start && t <= sequence->end) {
                    times.push_back(t - sequence->start);
                }
            }
        }
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
        if (row.loop || (kind == audio::ModelEventKind::DoodadLoop && times.empty())) {
            baked.kind = static_cast<u16>(pack::kModelSoundLoop);
            out.events.push_back(baked);
            continue;
        }
        if (out.cycleMs == 0) {
            continue; // a one-shot of an animation the doodad never plays
        }
        baked.kind = static_cast<u16>(pack::kModelSoundTimed);
        for (const u32 t : times) {
            baked.timeMs = std::min(t, out.cycleMs - 1);
            out.events.push_back(baked);
        }
    }
    return out;
}

void placeDoodadSounds(const ModelSounds& model, const std::array<f32, 12>& transform,
                       std::vector<pack::DoodadSound>& out) {
    for (const pack::ModelSoundEvent& e : model.events) {
        const Vec3 world = transformPoint(transform, {e.position[0], e.position[1], e.position[2]});
        pack::DoodadSound record{};
        for (u32 a = 0; a < 3; ++a) {
            record.position[a] = world[a];
        }
        record.sound = e.sound;
        record.cell = pack::doodadSoundCell(world[0], world[1]);
        record.kind = e.kind;
        record.timeMs = e.timeMs;
        record.cycleMs = model.cycleMs;
        out.push_back(record);
    }
}

std::vector<u8> bakeDoodadSounds(std::vector<pack::DoodadSound> records) {
    std::stable_sort(records.begin(), records.end(),
                     [](const pack::DoodadSound& a, const pack::DoodadSound& b) { return a.cell < b.cell; });
    pack::DoodadSoundsHeader header{};
    header.version = pack::kWorldSoundVersion;
    header.count = static_cast<u32>(records.size());
    std::vector<u8> blob(sizeof(header) + records.size() * sizeof(pack::DoodadSound));
    std::memcpy(blob.data(), &header, sizeof(header));
    if (!records.empty()) {
        std::memcpy(blob.data() + sizeof(header), records.data(), records.size() * sizeof(pack::DoodadSound));
    }
    return blob;
}

// --- WMO areas ---------------------------------------------------------------------------------------------------

WmoAreaModelData wmoAreaModel(const formats::WmoRoot& root, const std::vector<formats::WmoGroup>& groups, u64 hash) {
    WmoAreaModelData out;
    out.model = hash;
    out.wmoId = static_cast<i32>(root.areaTableId);
    out.groups.resize(groups.size());
    for (usize gi = 0; gi < groups.size(); ++gi) {
        const formats::WmoGroup& source = groups[gi];
        WmoAreaGroupData& data = out.groups[gi];
        pack::WmoAreaGroup& g = data.group;
        for (u32 a = 0; a < 3; ++a) {
            g.boundsMin[a] = std::min(source.boundsMin[a], source.boundsMax[a]);
            g.boundsMax[a] = std::max(source.boundsMin[a], source.boundsMax[a]);
        }
        g.groupId = static_cast<i32>(source.groupId);
        g.flags = source.flags;
        const auto cellsAlong = [](f32 extent) {
            return static_cast<u16>(std::clamp(std::ceil(extent / pack::kWmoFloorCell), 1.0f, 65535.0f));
        };
        g.floorStep = std::max(pack::kWmoFloorStep, (g.boundsMax[2] - g.boundsMin[2]) / 254.0f);
        g.cellsX = cellsAlong(g.boundsMax[0] - g.boundsMin[0]);
        g.cellsY = cellsAlong(g.boundsMax[1] - g.boundsMin[1]);
        data.cells.assign(static_cast<usize>(g.cellsX) * g.cellsY * 2, pack::kWmoNoFloor);
        const usize triangles = std::min(source.indices.size() / 3, source.triangleFlags.size());
        for (usize t = 0; t < triangles; ++t) {
            const u8 flags = source.triangleFlags[t];
            const bool renderFace = (flags & kMopyRender) && !(flags & kMopyDetail);
            if (!(flags & kMopyCollision) && !renderFace) {
                continue;
            }
            const u16 i0 = source.indices[t * 3], i1 = source.indices[t * 3 + 1], i2 = source.indices[t * 3 + 2];
            if (i0 >= source.positions.size() || i1 >= source.positions.size() || i2 >= source.positions.size()) {
                continue;
            }
            rasterizeTriangle(source.positions[i0], source.positions[i1], source.positions[i2], data);
        }
    }
    return out;
}

std::vector<u8> bakeWmoAreas(const std::vector<WmoAreaPlacementData>& placements,
                             const std::vector<WmoAreaModelData>& models) {
    std::vector<pack::WmoAreaModel> modelRecords;
    std::vector<pack::WmoAreaGroup> groups;
    std::vector<u8> cells;
    for (const WmoAreaModelData& m : models) {
        pack::WmoAreaModel r{};
        r.model = m.model;
        r.wmoId = m.wmoId;
        r.firstGroup = static_cast<u32>(groups.size());
        r.groupCount = static_cast<u32>(m.groups.size());
        modelRecords.push_back(r);
        for (const WmoAreaGroupData& g : m.groups) {
            pack::WmoAreaGroup record = g.group;
            record.firstCell = static_cast<u32>(cells.size() / 2);
            groups.push_back(record);
            cells.insert(cells.end(), g.cells.begin(), g.cells.end());
        }
    }
    std::vector<pack::WmoAreaPlacement> placementRecords;
    for (const WmoAreaPlacementData& p : placements) {
        if (p.model >= models.size()) {
            continue;
        }
        pack::WmoAreaPlacement r{};
        std::memcpy(r.transform, p.transform.data(), sizeof(r.transform));
        r.uniqueId = p.uniqueId;
        r.nameSet = p.nameSet;
        r.model = static_cast<u16>(p.model);
        // The world box of its groups' boxes.
        Vec3 min{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max()};
        Vec3 max{-min[0], -min[1], -min[2]};
        for (const WmoAreaGroupData& g : models[p.model].groups) {
            for (u32 corner = 0; corner < 8; ++corner) {
                const Vec3 local{corner & 1 ? g.group.boundsMax[0] : g.group.boundsMin[0],
                                 corner & 2 ? g.group.boundsMax[1] : g.group.boundsMin[1],
                                 corner & 4 ? g.group.boundsMax[2] : g.group.boundsMin[2]};
                const Vec3 world = transformPoint(p.transform, local);
                for (u32 a = 0; a < 3; ++a) {
                    min[a] = std::min(min[a], world[a]);
                    max[a] = std::max(max[a], world[a]);
                }
            }
        }
        if (models[p.model].groups.empty()) {
            continue;
        }
        for (u32 a = 0; a < 3; ++a) {
            r.boundsMin[a] = min[a];
            r.boundsMax[a] = max[a];
        }
        placementRecords.push_back(r);
    }
    BlobWriter writer(sizeof(pack::WmoAreasHeader));
    pack::WmoAreasHeader header{};
    header.version = pack::kWorldSoundVersion;
    header.placementCount = static_cast<u32>(placementRecords.size());
    header.modelCount = static_cast<u32>(modelRecords.size());
    header.groupCount = static_cast<u32>(groups.size());
    header.cellCount = static_cast<u32>(cells.size() / 2);
    header.placementsOffset = writer.append(placementRecords);
    header.modelsOffset = writer.append(modelRecords);
    header.groupsOffset = writer.append(groups);
    header.cellsOffset = writer.append(cells);
    return writer.finish(header);
}

bool wmoFloorAt(const WmoAreaGroupData& group, f32 x, f32 y, f32& lowest, f32& highest) {
    const pack::WmoAreaGroup& g = group.group;
    const i32 cx = static_cast<i32>(std::floor((x - g.boundsMin[0]) / pack::kWmoFloorCell));
    const i32 cy = static_cast<i32>(std::floor((y - g.boundsMin[1]) / pack::kWmoFloorCell));
    if (cx < 0 || cy < 0 || cx >= g.cellsX || cy >= g.cellsY) {
        return false;
    }
    const u8* cell = &group.cells[(static_cast<usize>(cy) * g.cellsX + static_cast<usize>(cx)) * 2];
    if (cell[0] == pack::kWmoNoFloor) {
        return false;
    }
    lowest = g.boundsMin[2] + static_cast<f32>(cell[0]) * g.floorStep;
    highest = g.boundsMin[2] + static_cast<f32>(cell[1]) * g.floorStep;
    return true;
}

// --- Terrain emitters --------------------------------------------------------------------------------------------

std::vector<u8> bakeSoundEmitters(const std::vector<formats::AdtSoundEmitter>& emitters) {
    std::vector<pack::SoundEmitter> records;
    for (const formats::AdtSoundEmitter& e : emitters) {
        pack::SoundEmitter r{};
        for (u32 a = 0; a < 3; ++a) {
            r.position[a] = e.position[a];
        }
        r.sound = e.sound;
        r.minDistance = e.minDistance;
        r.maxDistance = e.maxDistance;
        r.cutoffDistance = e.cutoffDistance;
        r.startMinute = e.startMinute;
        r.endMinute = e.endMinute;
        r.mode = e.mode;
        r.loopCountMin = e.loopCountMin;
        r.loopCountMax = e.loopCountMax;
        r.silenceMinMs = e.silenceMinMs;
        r.silenceMaxMs = e.silenceMaxMs;
        r.instancesMin = e.instancesMin;
        r.instancesMax = e.instancesMax;
        r.gapMinMs = e.gapMinMs;
        r.gapMaxMs = e.gapMaxMs;
        r.id = e.id;
        records.push_back(r);
    }
    pack::SoundEmittersHeader header{};
    header.version = pack::kWorldSoundVersion;
    header.count = static_cast<u32>(records.size());
    std::vector<u8> blob(sizeof(header) + records.size() * sizeof(pack::SoundEmitter));
    std::memcpy(blob.data(), &header, sizeof(header));
    if (!records.empty()) {
        std::memcpy(blob.data() + sizeof(header), records.data(), records.size() * sizeof(pack::SoundEmitter));
    }
    return blob;
}

} // namespace vd::bake
