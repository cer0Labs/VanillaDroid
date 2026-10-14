#include "formats/Wmo.h"

#include "formats/ChunkReader.h"

#include <cstdio>
#include <cstring>

namespace vd::formats {
namespace {

constexpr u32 kWmoVersion = 17;
constexpr u32 kMogpHeaderSize = 68;

struct MohdRecord {
    u32 materialCount;
    u32 groupCount;
    u32 portalCount;
    u32 lightCount;
    u32 doodadNameCount;
    u32 doodadCount;
    u32 doodadSetCount;
    u32 ambientColor;
    u32 areaTableId;
    f32 boundsMin[3];
    f32 boundsMax[3];
    u16 flags;
    u16 lodCount;
};
static_assert(sizeof(MohdRecord) == 64, "MOHD layout");

struct MomtRecord {
    u32 flags;
    u32 shader;
    u32 blendMode;
    u32 texture1;
    u32 emissiveColor;
    u32 frameEmissiveColor;
    u32 texture2;
    u32 diffuseColor;
    u32 groundType;
    u32 texture3;
    u32 color2;
    u32 flags2;
    u32 runtime[4];
};
static_assert(sizeof(MomtRecord) == 64, "MOMT layout");

struct MogiRecord {
    u32 flags;
    f32 boundsMin[3];
    f32 boundsMax[3];
    i32 nameOffset;
};
static_assert(sizeof(MogiRecord) == 32, "MOGI layout");

struct MoptRecord {
    u16 firstVertex;
    u16 vertexCount;
    f32 plane[4];
};
static_assert(sizeof(MoptRecord) == 20, "MOPT layout");

struct MoprRecord {
    u16 portal;
    u16 group;
    i16 side;
    u16 padding;
};
static_assert(sizeof(MoprRecord) == 8, "MOPR layout");

struct ModsRecord {
    char name[20];
    u32 first;
    u32 count;
    u32 padding;
};
static_assert(sizeof(ModsRecord) == 32, "MODS layout");

struct ModdRecord {
    u32 nameAndFlags; // name offset: low 24 bits, flags: high 8
    f32 position[3];
    f32 rotation[4]; // x, y, z, w
    f32 scale;
    u32 color;
};
static_assert(sizeof(ModdRecord) == 40, "MODD layout");

// Phase 11: lights and fogs (v17).
struct MoltRecord {
    u8 type;
    u8 useAttenuation;
    u8 padding[2];
    u32 color;
    f32 position[3];
    f32 intensity;
    f32 rotation[4]; // spot lights (none in 1.12's data)
    f32 attenStart;
    f32 attenEnd;
};
static_assert(sizeof(MoltRecord) == 48, "MOLT layout");

struct MfogRecord {
    u32 flags;
    f32 position[3];
    f32 innerRadius;
    f32 outerRadius;
    f32 end;
    f32 startScale;
    u32 color;
    f32 underwaterEnd;
    f32 underwaterStartScale;
    u32 underwaterColor;
};
static_assert(sizeof(MfogRecord) == 48, "MFOG layout");

struct MogpHeader {
    u32 nameOffset;
    u32 descriptiveNameOffset;
    u32 flags;
    f32 boundsMin[3];
    f32 boundsMax[3];
    u16 portalStart;
    u16 portalCount;
    u16 transBatchCount;
    u16 interiorBatchCount;
    u16 exteriorBatchCount;
    u16 padding;
    u8 fogIds[4];
    u32 liquid;
    u32 groupId;
    u32 flags2;
    u32 unused;
};
static_assert(sizeof(MogpHeader) == kMogpHeaderSize, "MOGP header layout");

struct MobaRecord {
    i16 boxMin[3];
    i16 boxMax[3];
    u32 firstIndex;
    u16 indexCount;
    u16 minVertex;
    u16 maxVertex;
    u8 flags;
    u8 material;
};
static_assert(sizeof(MobaRecord) == 24, "MOBA layout");

Result<void> checkVersion(const std::vector<Chunk>& chunks) {
    if (chunks.empty() || chunks[0].id != chunkId("MVER") || chunks[0].data.size() < 4 ||
        load<u32>(chunks[0].data.data()) != kWmoVersion) {
        return Error{"Not a version 17 WMO"};
    }
    return {};
}

template <usize N>
std::array<f32, N> toArray(const f32 (&values)[N]) {
    std::array<f32, N> out{};
    for (usize i = 0; i < N; ++i) {
        out[i] = values[i];
    }
    return out;
}

} // namespace

Result<WmoRoot> parseWmoRoot(std::span<const u8> data) {
    Result<std::vector<Chunk>> chunks = readChunks(data);
    if (!chunks) {
        return chunks.error();
    }
    VD_TRY(checkVersion(chunks.value()));

    WmoRoot root;
    std::span<const u8> motx, modn, mopv;
    std::vector<MoptRecord> portals;
    std::vector<std::array<u32, 2>> textureOffsets; // per material: texture1, texture2 (into MOTX)
    std::vector<u32> modelOffsets;                  // per doodad (into MODN)
    bool hasHeader = false;
    for (const Chunk& c : chunks.value()) {
        switch (c.id) {
        case chunkId("MOHD"): {
            if (c.data.size() < sizeof(MohdRecord)) {
                return Error{"MOHD too small"};
            }
            const MohdRecord h = load<MohdRecord>(c.data.data());
            root.flags = h.flags;
            root.ambientColor = h.ambientColor;
            root.areaTableId = h.areaTableId;
            root.boundsMin = toArray(h.boundsMin);
            root.boundsMax = toArray(h.boundsMax);
            hasHeader = true;
            break;
        }
        case chunkId("MOTX"): motx = c.data; break;
        case chunkId("MOMT"):
            for (const MomtRecord& m : loadArray<MomtRecord>(c.data)) {
                WmoMaterial& out = root.materials.emplace_back();
                out.flags = m.flags;
                out.shader = m.shader;
                out.blendMode = m.blendMode;
                textureOffsets.push_back({m.texture1, m.texture2});
                out.emissiveColor = m.emissiveColor;
                out.diffuseColor = m.diffuseColor;
            }
            break;
        case chunkId("MOGI"):
            for (const MogiRecord& g : loadArray<MogiRecord>(c.data)) {
                root.groups.push_back({g.flags, toArray(g.boundsMin), toArray(g.boundsMax)});
            }
            break;
        case chunkId("MOSB"): root.skybox = stringAt(c.data, 0); break;
        case chunkId("MOPV"): mopv = c.data; break;
        case chunkId("MOPT"): portals = loadArray<MoptRecord>(c.data); break;
        case chunkId("MOPR"):
            for (const MoprRecord& r : loadArray<MoprRecord>(c.data)) {
                root.portalRefs.push_back({r.portal, r.group, r.side});
            }
            break;
        case chunkId("MODS"):
            for (const ModsRecord& s : loadArray<ModsRecord>(c.data)) {
                root.doodadSets.push_back({std::string(s.name, ::strnlen(s.name, sizeof(s.name))), s.first, s.count});
            }
            break;
        case chunkId("MODN"): modn = c.data; break;
        case chunkId("MODD"):
            for (const ModdRecord& d : loadArray<ModdRecord>(c.data)) {
                WmoDoodad& out = root.doodads.emplace_back();
                modelOffsets.push_back(d.nameAndFlags & 0xFFFFFF);
                out.flags = static_cast<u8>(d.nameAndFlags >> 24);
                out.position = toArray(d.position);
                out.rotation = toArray(d.rotation);
                out.scale = d.scale;
                out.color = d.color;
            }
            break;
        case chunkId("MOLT"):
            for (const MoltRecord& l : loadArray<MoltRecord>(c.data)) {
                WmoLight& out = root.lights.emplace_back();
                out.type = static_cast<WmoLightType>(l.type);
                out.attenuated = l.useAttenuation != 0;
                out.color = l.color;
                out.position = toArray(l.position);
                out.intensity = l.intensity;
                out.attenStart = l.attenStart;
                out.attenEnd = l.attenEnd;
            }
            break;
        case chunkId("MFOG"):
            for (const MfogRecord& f : loadArray<MfogRecord>(c.data)) {
                root.fogs.push_back({f.flags, toArray(f.position), f.innerRadius, f.outerRadius, f.end, f.startScale,
                                     f.color, f.underwaterEnd, f.underwaterStartScale, f.underwaterColor});
            }
            break;
        default: break;
        }
    }
    if (!hasHeader) {
        return Error{"WMO root without MOHD"};
    }

    // String offsets -> names (chunk order is not guaranteed, so resolve after the scan).
    for (usize i = 0; i < root.materials.size(); ++i) {
        root.materials[i].texture1 = stringAt(motx, textureOffsets[i][0]);
        root.materials[i].texture2 = stringAt(motx, textureOffsets[i][1]);
    }
    for (usize i = 0; i < root.doodads.size(); ++i) {
        root.doodads[i].model = stringAt(modn, modelOffsets[i]);
    }
    const std::vector<std::array<f32, 3>> portalVertices = loadArray<std::array<f32, 3>>(mopv);
    for (const MoptRecord& p : portals) {
        if (p.firstVertex + u32{p.vertexCount} > portalVertices.size()) {
            return Error{"Portal vertices out of range"};
        }
        WmoPortal& out = root.portals.emplace_back();
        out.vertices.assign(portalVertices.begin() + p.firstVertex,
                            portalVertices.begin() + p.firstVertex + p.vertexCount);
        out.plane = toArray(p.plane);
    }
    for (const WmoPortalRef& r : root.portalRefs) {
        if (r.portal >= root.portals.size() || r.group >= root.groups.size()) {
            return Error{"Portal reference out of range"};
        }
    }
    return root;
}

namespace {

// 30 bytes, packed: vertex and tile counts, corner, material.
constexpr usize kMliqHeaderSize = 30;

Result<void> parseLiquid(std::span<const u8> data, WmoLiquid& liquid) {
    if (data.size() < kMliqHeaderSize) {
        return Error{"WMO MLIQ too small"};
    }
    const i32 vertsX = load<i32>(data.data());
    const i32 vertsY = load<i32>(data.data() + 4);
    const i32 tilesX = load<i32>(data.data() + 8);
    const i32 tilesY = load<i32>(data.data() + 12);
    if (vertsX <= 0 || vertsY <= 0 || tilesX <= 0 || tilesY <= 0 || tilesX + 1 != vertsX || tilesY + 1 != vertsY ||
        vertsX > 1024 || vertsY > 1024) {
        return Error{"WMO MLIQ grid"};
    }
    const usize vertexCount = static_cast<usize>(vertsX) * static_cast<usize>(vertsY);
    const usize tileCount = static_cast<usize>(tilesX) * static_cast<usize>(tilesY);
    if (data.size() < kMliqHeaderSize + vertexCount * 8 + tileCount) {
        return Error{"WMO MLIQ truncated"};
    }
    liquid.vertsX = static_cast<u32>(vertsX);
    liquid.vertsY = static_cast<u32>(vertsY);
    liquid.tilesX = static_cast<u32>(tilesX);
    liquid.tilesY = static_cast<u32>(tilesY);
    for (u32 i = 0; i < 3; ++i) {
        liquid.corner[i] = load<f32>(data.data() + 16 + i * 4);
    }
    // Each vertex: 4 bytes of flow (water) or texture coordinates (magma), then the height.
    liquid.heights.resize(vertexCount);
    for (usize v = 0; v < vertexCount; ++v) {
        liquid.heights[v] = load<f32>(data.data() + kMliqHeaderSize + v * 8 + 4);
    }
    const u8* tiles = data.data() + kMliqHeaderSize + vertexCount * 8;
    liquid.tiles.assign(tiles, tiles + tileCount);
    return {};
}

} // namespace

Result<WmoGroup> parseWmoGroup(std::span<const u8> data) {
    Result<std::vector<Chunk>> chunks = readChunks(data);
    if (!chunks) {
        return chunks.error();
    }
    VD_TRY(checkVersion(chunks.value()));
    const Chunk* mogp = nullptr;
    for (const Chunk& c : chunks.value()) {
        if (c.id == chunkId("MOGP")) {
            mogp = &c;
        }
    }
    if (!mogp || mogp->data.size() < kMogpHeaderSize) {
        return Error{"WMO group without MOGP"};
    }

    WmoGroup group;
    const MogpHeader h = load<MogpHeader>(mogp->data.data());
    group.flags = h.flags;
    group.boundsMin = toArray(h.boundsMin);
    group.boundsMax = toArray(h.boundsMax);
    group.portalStart = h.portalStart;
    group.portalCount = h.portalCount;
    group.transBatchCount = h.transBatchCount;
    group.interiorBatchCount = h.interiorBatchCount;
    group.exteriorBatchCount = h.exteriorBatchCount;
    group.groupId = h.groupId;
    group.liquidType = h.liquid;
    group.fogIds = {h.fogIds[0], h.fogIds[1], h.fogIds[2], h.fogIds[3]};

    Result<std::vector<Chunk>> subchunks = readChunks(mogp->data.subspan(kMogpHeaderSize));
    if (!subchunks) {
        return subchunks.error();
    }
    for (const Chunk& c : subchunks.value()) {
        switch (c.id) {
        case chunkId("MOPY"):
            for (usize i = 0; i + 1 < c.data.size(); i += 2) {
                group.triangleFlags.push_back(c.data[i]);
                group.triangleMaterials.push_back(c.data[i + 1]);
            }
            break;
        case chunkId("MOVI"): group.indices = loadArray<u16>(c.data); break;
        case chunkId("MOVT"): group.positions = loadArray<std::array<f32, 3>>(c.data); break;
        case chunkId("MONR"): group.normals = loadArray<std::array<f32, 3>>(c.data); break;
        case chunkId("MOTV"):
            if (group.uvs.empty()) { // a second MOTV (later clients) would be a second UV set
                group.uvs = loadArray<std::array<f32, 2>>(c.data);
            }
            break;
        case chunkId("MOCV"): group.colors = loadArray<u32>(c.data); break;
        case chunkId("MOBA"):
            for (const MobaRecord& b : loadArray<MobaRecord>(c.data)) {
                group.batches.push_back({b.firstIndex, b.indexCount, b.minVertex, b.maxVertex, b.flags, b.material});
            }
            break;
        case chunkId("MODR"): group.doodadRefs = loadArray<u16>(c.data); break;
        case chunkId("MOLR"): group.lightRefs = loadArray<u16>(c.data); break;
        case chunkId("MOBN"): // phase 18 (world)
            for (usize i = 0; i + 16 <= c.data.size(); i += 16) {
                WmoBspNode node;
                std::memcpy(&node.flags, c.data.data() + i, 2);
                std::memcpy(&node.negChild, c.data.data() + i + 2, 2);
                std::memcpy(&node.posChild, c.data.data() + i + 4, 2);
                std::memcpy(&node.faceCount, c.data.data() + i + 6, 2);
                std::memcpy(&node.firstFace, c.data.data() + i + 8, 4);
                std::memcpy(&node.planeDist, c.data.data() + i + 12, 4);
                group.bspNodes.push_back(node);
            }
            break;
        case chunkId("MOBR"): group.bspFaces = loadArray<u16>(c.data); break;
        case chunkId("MLIQ"): VD_TRY(parseLiquid(c.data, group.liquid)); break;
        default: break;
        }
    }

    const usize vertexCount = group.positions.size();
    if (!group.colors.empty() && group.colors.size() + 1 == vertexCount) {
        group.colors.push_back(group.colors.back()); // truncated last MOCV entry
    }
    if (group.normals.size() != vertexCount || group.uvs.size() != vertexCount ||
        (!group.colors.empty() && group.colors.size() != vertexCount)) {
        return Error{"WMO group vertex streams differ in length"};
    }
    if (group.indices.size() % 3 != 0 || group.triangleMaterials.size() != group.indices.size() / 3) {
        return Error{"WMO group MOPY/MOVI mismatch"};
    }
    for (u16 index : group.indices) {
        if (index >= vertexCount) {
            return Error{"WMO group index out of range"};
        }
    }
    for (const WmoBatch& b : group.batches) {
        if (u64{b.firstIndex} + b.indexCount > group.indices.size()) {
            return Error{"WMO batch out of range"};
        }
    }
    return group;
}

bool wmoGroupNeverDrawn(u32 groupFlags) {
    return (groupFlags & kWmoGroupUnreachable) != 0 && (groupFlags & (kWmoGroupExterior | kWmoGroupAlwaysDraw)) == 0;
}

WmoLiquidKind wmoLiquidKind(const WmoRoot& root, const WmoGroup& group) {
    const WmoLiquid& liquid = group.liquid;
    if (liquid.tilesX == 0) {
        return WmoLiquidKind::None;
    }
    u32 entry = 0;
    if (root.flags & kWmoRootLiquidTypeIsDbcId) {
        entry = group.liquidType;
    } else if (group.liquidType != 15) {
        entry = group.liquidType + 1;
    }
    if (entry == 0) { // from the first tile that has liquid
        for (u8 tile : liquid.tiles) {
            if ((tile & 0x0F) != kWmoLiquidTileEmpty) {
                entry = (tile & 0x0Fu) + 1;
                break;
            }
        }
    }
    if (entry == 0 || entry >= 21) {
        return entry == 0 ? WmoLiquidKind::None : WmoLiquidKind::Water; // DBC ids beyond 1.12's legacy set
    }
    switch ((entry - 1) & 3) {
    case 0: return (group.flags & kWmoGroupOcean) ? WmoLiquidKind::Ocean : WmoLiquidKind::Water;
    case 1: return WmoLiquidKind::Ocean;
    case 2: return WmoLiquidKind::Magma;
    default: return WmoLiquidKind::Slime;
    }
}

std::string wmoGroupPath(std::string_view rootPath, u32 group) {
    std::string base(rootPath);
    const usize dot = base.rfind('.');
    if (dot != std::string::npos) {
        base.resize(dot);
    }
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "_%03u.wmo", group);
    return base + suffix;
}

std::string m2Path(std::string_view reference) {
    std::string path(reference);
    const usize dot = path.rfind('.');
    if (dot != std::string::npos) {
        path.resize(dot);
    }
    return path + ".m2";
}

} // namespace vd::formats
