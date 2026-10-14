#include "formats/Adt.h"

#include "formats/ChunkReader.h"

#include <bit>
#include <cstdio>
#include <cstring>

namespace vd::formats {
namespace {

constexpr u32 kMcnkHeaderSize = 128;
constexpr u32 kLiquidInstanceSize = 804;
constexpr u32 kAdtVersion = 18;

// NUL-separated string block (MTEX, MMDX, MWMO), in file order.
std::vector<std::string> splitStrings(std::span<const u8> block) {
    std::vector<std::string> out;
    usize start = 0;
    for (usize i = 0; i < block.size(); ++i) {
        if (block[i] == 0) {
            if (i > start) {
                out.emplace_back(reinterpret_cast<const char*>(block.data() + start), i - start);
            }
            start = i + 1;
        }
    }
    return out;
}

// Strings addressed by an offset table (MMID into MMDX, MWID into MWMO).
Result<std::vector<std::string>> stringsByOffset(std::span<const u8> block, std::span<const u8> offsets) {
    std::vector<std::string> out;
    out.reserve(offsets.size() / 4);
    for (usize i = 0; i + 4 <= offsets.size(); i += 4) {
        const u32 offset = load<u32>(offsets.data() + i);
        if (offset >= block.size()) {
            return Error{"String offset out of bounds"};
        }
        const char* text = reinterpret_cast<const char*>(block.data() + offset);
        out.emplace_back(text, ::strnlen(text, block.size() - offset));
    }
    return out;
}

void readVec3(const u8* at, f32 (&out)[3]) {
    std::memcpy(out, at, sizeof(out));
}

// The client treats the 64x64 maps as 63x63: the last row and column repeat the previous ones.
void fixEdges(std::array<u8, kAlphaSize * kAlphaSize>& map) {
    for (u32 row = 0; row < kAlphaSize; ++row) {
        map[row * kAlphaSize + kAlphaSize - 1] = map[row * kAlphaSize + kAlphaSize - 2];
    }
    std::memcpy(&map[(kAlphaSize - 1) * kAlphaSize], &map[(kAlphaSize - 2) * kAlphaSize], kAlphaSize);
}

struct McnkHeader {
    u32 flags;
    u32 indexX;
    u32 indexY;
    u32 layerCount;
    u32 doodadRefCount;
    u32 ofsHeight;
    u32 ofsNormal;
    u32 ofsLayer;
    u32 ofsRefs;
    u32 ofsAlpha;
    u32 sizeAlpha;
    u32 ofsShadow;
    u32 sizeShadow;
    u32 areaId;
    u32 wmoRefCount;
    u16 holes;
    u16 padding;
    u8 lowQualityTextureMap[16];
    u8 noEffectDoodad[8];
    u32 ofsSoundEmitters;
    u32 soundEmitterCount;
    u32 ofsLiquid;
    u32 sizeLiquid;
    f32 position[3];
    u32 unused[3];
};
static_assert(sizeof(McnkHeader) == kMcnkHeaderSize, "MCNK header layout");

class McnkParser {
public:
    McnkParser(std::span<const u8> mcnk, AdtChunk& out) : m_mcnk(mcnk), m_out(out) {}

    Result<void> parse() {
        if (m_mcnk.size() < kChunkHeaderSize + kMcnkHeaderSize) {
            return Error{"MCNK too small"};
        }
        McnkHeader h;
        std::memcpy(&h, m_mcnk.data() + kChunkHeaderSize, sizeof(h));
        m_out.flags = h.flags;
        m_out.indexX = h.indexX;
        m_out.indexY = h.indexY;
        m_out.areaId = h.areaId;
        m_out.holes = h.holes;
        std::memcpy(m_out.cellLayers.data(), h.lowQualityTextureMap, sizeof(h.lowQualityTextureMap)); // phase 14
        std::memcpy(m_out.noEffectDoodad.data(), h.noEffectDoodad, sizeof(h.noEffectDoodad));
        std::memcpy(m_out.position, h.position, sizeof(m_out.position));

        Result<std::span<const u8>> heights = sub(h.ofsHeight, chunkId("MCVT"), kChunkVertices * 4);
        if (!heights) {
            return heights.error();
        }
        std::memcpy(m_out.heights.data(), heights.value().data(), kChunkVertices * 4);

        Result<std::span<const u8>> normals = sub(h.ofsNormal, chunkId("MCNR"), kChunkVertices * 3);
        if (!normals) {
            return normals.error();
        }
        std::memcpy(m_out.normals.data(), normals.value().data(), kChunkVertices * 3);

        if (h.layerCount > 4) {
            return Error{"MCNK with " + std::to_string(h.layerCount) + " layers"};
        }
        m_out.layerCount = h.layerCount;
        if (h.layerCount > 0) {
            Result<std::span<const u8>> layers = sub(h.ofsLayer, chunkId("MCLY"), h.layerCount * 16);
            if (!layers) {
                return layers.error();
            }
            for (u32 i = 0; i < h.layerCount; ++i) {
                const u8* at = layers.value().data() + i * 16;
                m_out.layers[i] = {load<u32>(at), load<u32>(at + 4), load<u32>(at + 8), load<u32>(at + 12)};
            }
        }

        const u32 refCount = h.doodadRefCount + h.wmoRefCount;
        if (refCount > 0) {
            Result<std::span<const u8>> refs = sub(h.ofsRefs, chunkId("MCRF"), refCount * 4);
            if (!refs) {
                return refs.error();
            }
            for (u32 i = 0; i < refCount; ++i) {
                const u32 ref = load<u32>(refs.value().data() + i * 4);
                (i < h.doodadRefCount ? m_out.doodadRefs : m_out.wmoRefs).push_back(ref);
            }
        }

        VD_TRY(parseAlpha(h));
        VD_TRY(parseShadow(h));
        return parseLiquids(h);
    }

private:
    // Payload of the sub-chunk at `offset` (relative to the MCNK magic), at least `minSize` bytes.
    Result<std::span<const u8>> sub(u32 offset, u32 id, usize minSize) const {
        Result<Chunk> chunk = chunkAt(m_mcnk, offset);
        if (!chunk) {
            return chunk.error();
        }
        if (chunk.value().id != id) {
            return Error{"Expected " + idToString(id) + " in MCNK, found " + idToString(chunk.value().id)};
        }
        if (chunk.value().data.size() < minSize) {
            return Error{idToString(id) + " too small"};
        }
        return chunk.value().data;
    }

    Result<void> parseAlpha(const McnkHeader& h) {
        bool needsAlpha = false;
        for (u32 i = 1; i < m_out.layerCount; ++i) {
            needsAlpha |= (m_out.layers[i].flags & kLayerUseAlpha) != 0;
        }
        if (!needsAlpha) {
            return {};
        }
        Result<std::span<const u8>> mcal = sub(h.ofsAlpha, chunkId("MCAL"), 0);
        if (!mcal) {
            return mcal.error();
        }
        constexpr u32 kLayerBytes = kAlphaSize * kAlphaSize / 2; // 4 bits per texel
        for (u32 i = 1; i < m_out.layerCount; ++i) {
            if (!(m_out.layers[i].flags & kLayerUseAlpha)) {
                continue; // no alpha map: the layer stays invisible
            }
            const u32 offset = m_out.layers[i].alphaOffset;
            if (offset + kLayerBytes > mcal.value().size()) {
                return Error{"MCAL layer out of bounds"};
            }
            const u8* src = mcal.value().data() + offset;
            std::array<u8, kAlphaSize * kAlphaSize>& dst = m_out.alpha[i - 1];
            for (u32 t = 0; t < kAlphaSize * kAlphaSize; t += 2) {
                const u8 packed = src[t / 2];
                dst[t] = static_cast<u8>((packed & 0x0F) * 17); // low nibble first
                dst[t + 1] = static_cast<u8>((packed >> 4) * 17);
            }
            fixEdges(dst);
        }
        return {};
    }

    Result<void> parseShadow(const McnkHeader& h) {
        if (!(h.flags & kChunkHasShadow) || h.ofsShadow == 0) {
            return {};
        }
        Result<std::span<const u8>> mcsh = sub(h.ofsShadow, chunkId("MCSH"), kAlphaSize * kAlphaSize / 8);
        if (!mcsh) {
            return mcsh.error();
        }
        for (u32 t = 0; t < kAlphaSize * kAlphaSize; ++t) {
            m_out.shadow[t] = (mcsh.value()[t / 8] >> (t % 8)) & 1u; // LSB first
        }
        std::memcpy(m_out.shadowBits.data(), mcsh.value().data(), m_out.shadowBits.size()); // phase 14: as stored
        fixEdges(m_out.shadow);
        m_out.hasShadow = true;
        return {};
    }

    Result<void> parseLiquids(const McnkHeader& h) {
        const u32 liquidFlags =
            h.flags & (kChunkLiquidRiver | kChunkLiquidOcean | kChunkLiquidMagma | kChunkLiquidSlime);
        if (liquidFlags == 0) {
            return {};
        }
        // The MCLQ header's size field is 0 in vanilla: the payload size comes from the MCNK header.
        const usize count = static_cast<usize>(std::popcount(liquidFlags));
        const usize begin = usize{h.ofsLiquid} + kChunkHeaderSize;
        if (h.ofsLiquid == 0 || begin + count * kLiquidInstanceSize > m_mcnk.size() ||
            load<u32>(m_mcnk.data() + h.ofsLiquid) != chunkId("MCLQ")) {
            return Error{"Bad MCLQ"};
        }
        const u8* at = m_mcnk.data() + begin;
        for (u32 bit = 0; bit < 4; ++bit) {
            if (!(liquidFlags & (kChunkLiquidRiver << bit))) {
                continue;
            }
            AdtLiquid& liquid = m_out.liquids.emplace_back();
            liquid.type = static_cast<LiquidType>(bit);
            liquid.minHeight = load<f32>(at);
            liquid.maxHeight = load<f32>(at + 4);
            for (u32 v = 0; v < 81; ++v) {
                const u8* vertex = at + 8 + v * 8;
                liquid.depth[v] = liquid.type <= LiquidType::Ocean ? vertex[0] : 0;
                liquid.heights[v] = load<f32>(vertex + 4);
            }
            std::memcpy(liquid.tiles.data(), at + 8 + 81 * 8, 64);
            at += kLiquidInstanceSize;
        }
        return {};
    }

    std::span<const u8> m_mcnk;
    AdtChunk& m_out;
};

Result<void> checkVersion(const Chunk& chunk) {
    if (chunk.id != chunkId("MVER") || chunk.data.size() < 4 || load<u32>(chunk.data.data()) != kAdtVersion) {
        return Error{"Not a version 18 terrain file"};
    }
    return {};
}

} // namespace

Result<Wdt> parseWdt(std::span<const u8> data) {
    Wdt wdt;
    usize offset = 0;
    bool first = true;
    bool hasMain = false;
    while (offset + kChunkHeaderSize <= data.size()) {
        Result<Chunk> chunk = chunkAt(data, offset);
        if (!chunk) {
            return chunk.error();
        }
        const Chunk& c = chunk.value();
        if (first) {
            VD_TRY(checkVersion(c));
            first = false;
        } else if (c.id == chunkId("MPHD") && c.data.size() >= 4) {
            wdt.flags = load<u32>(c.data.data());
        } else if (c.id == chunkId("MAIN")) {
            if (c.data.size() < kMapTiles * kMapTiles * 8) {
                return Error{"MAIN too small"};
            }
            for (u32 i = 0; i < kMapTiles * kMapTiles; ++i) {
                wdt.tiles[i] = (load<u32>(c.data.data() + i * 8) & 1u) ? 1 : 0;
                wdt.tileCount += wdt.tiles[i];
            }
            hasMain = true;
        } else if (c.id == chunkId("MWMO")) {
            const std::vector<std::string> names = splitStrings(c.data);
            wdt.wmoName = names.empty() ? std::string{} : names.front();
        } else if (c.id == chunkId("MODF") && c.data.size() >= 64) {
            const u8* at = c.data.data();
            WdtWmoPlacement& p = wdt.wmoPlacement;
            p.nameId = load<u32>(at);
            p.uniqueId = load<u32>(at + 4);
            readVec3(at + 8, p.position);
            readVec3(at + 20, p.rotation);
            readVec3(at + 32, p.extentsMin);
            readVec3(at + 44, p.extentsMax);
            p.flags = load<u16>(at + 56);
            p.doodadSet = load<u16>(at + 58);
            p.nameSet = load<u16>(at + 60); // phase 14
            // Phase 12: every 1.12 WDT places its WMO at (0, 0, 0) with its own extents, i.e. with the building's
            // origin at the map's center; vMaNGOS' vmap extractor moves such a placement there (wmo.cpp: position
            // x = z = 0 -> 533.33333 * 32), which makes the server's coordinates the building's own.
            if (p.position[0] == 0.0f && p.position[2] == 0.0f) {
                for (u32 axis : {0u, 2u}) {
                    p.position[axis] = kMapHalfSize;
                    p.extentsMin[axis] += kMapHalfSize;
                    p.extentsMax[axis] += kMapHalfSize;
                }
            }
            wdt.hasWmoPlacement = true;
        }
        offset += kChunkHeaderSize + c.data.size();
    }
    if (!hasMain) {
        return Error{"WDT without MAIN"};
    }
    return wdt;
}

Result<Wdl> parseWdl(std::span<const u8> data) {
    Wdl wdl;
    std::span<const u8> maof;
    usize offset = 0;
    bool first = true;
    while (offset + kChunkHeaderSize <= data.size()) {
        Result<Chunk> chunk = chunkAt(data, offset);
        if (!chunk) {
            return chunk.error();
        }
        if (first) {
            VD_TRY(checkVersion(chunk.value()));
            first = false;
        }
        if (chunk.value().id == chunkId("MAOF")) {
            maof = chunk.value().data;
            break;
        }
        offset += kChunkHeaderSize + chunk.value().data.size();
    }
    if (maof.size() < kMapTiles * kMapTiles * 4) {
        return Error{"WDL without MAOF"};
    }
    for (u32 i = 0; i < kMapTiles * kMapTiles; ++i) {
        const u32 at = load<u32>(maof.data() + i * 4); // absolute offset of the tile's MARE, 0 = none
        if (at == 0) {
            continue;
        }
        Result<Chunk> mare = chunkAt(data, at);
        if (!mare || mare.value().id != chunkId("MARE") || mare.value().data.size() < kWdlSamples * 2) {
            return Error{"Bad MARE for tile " + std::to_string(i)};
        }
        wdl.tiles[i].resize(kWdlSamples);
        std::memcpy(wdl.tiles[i].data(), mare.value().data.data(), kWdlSamples * 2);
        ++wdl.tileCount;
    }
    return wdl;
}

Result<Adt> parseAdt(std::span<const u8> data) {
    Adt adt;
    std::span<const u8> mcin;
    std::span<const u8> mmdx, mmid, mwmo, mwid;
    std::vector<usize> mcnkOffsets;

    usize offset = 0;
    bool first = true;
    while (offset + kChunkHeaderSize <= data.size()) {
        Result<Chunk> chunk = chunkAt(data, offset);
        if (!chunk) {
            return chunk.error();
        }
        const Chunk& c = chunk.value();
        if (first) {
            VD_TRY(checkVersion(c));
            first = false;
        }
        switch (c.id) {
        case chunkId("MCIN"): mcin = c.data; break;
        case chunkId("MTEX"): adt.textures = splitStrings(c.data); break;
        case chunkId("MMDX"): mmdx = c.data; break;
        case chunkId("MMID"): mmid = c.data; break;
        case chunkId("MWMO"): mwmo = c.data; break;
        case chunkId("MWID"): mwid = c.data; break;
        case chunkId("MDDF"):
            for (usize i = 0; i + 36 <= c.data.size(); i += 36) {
                const u8* at = c.data.data() + i;
                AdtDoodadPlacement& p = adt.doodads.emplace_back();
                p.nameId = load<u32>(at);
                p.uniqueId = load<u32>(at + 4);
                readVec3(at + 8, p.position);
                readVec3(at + 20, p.rotation);
                p.scale = load<u16>(at + 32);
                p.flags = load<u16>(at + 34);
            }
            break;
        case chunkId("MODF"):
            for (usize i = 0; i + 64 <= c.data.size(); i += 64) {
                const u8* at = c.data.data() + i;
                AdtWmoPlacement& p = adt.wmoPlacements.emplace_back();
                p.nameId = load<u32>(at);
                p.uniqueId = load<u32>(at + 4);
                readVec3(at + 8, p.position);
                readVec3(at + 20, p.rotation);
                readVec3(at + 32, p.extentsMin);
                readVec3(at + 44, p.extentsMax);
                p.flags = load<u16>(at + 56);
                p.doodadSet = load<u16>(at + 58);
                p.nameSet = load<u16>(at + 60);
            }
            break;
        case chunkId("MCNK"): mcnkOffsets.push_back(offset); break;
        default: break;
        }
        offset += kChunkHeaderSize + c.data.size();
    }

    // MCIN is what the client follows; the sequential scan is the fallback.
    if (mcin.size() >= kChunksPerTile * 16) {
        mcnkOffsets.clear();
        for (u32 i = 0; i < kChunksPerTile; ++i) {
            mcnkOffsets.push_back(load<u32>(mcin.data() + i * 16));
        }
    }
    if (mcnkOffsets.size() != kChunksPerTile) {
        return Error{"ADT has " + std::to_string(mcnkOffsets.size()) + " MCNK chunks"};
    }

    Result<std::vector<std::string>> models = stringsByOffset(mmdx, mmid);
    Result<std::vector<std::string>> wmos = stringsByOffset(mwmo, mwid);
    if (!models || !wmos) {
        return Error{"Bad MMID/MWID offsets"};
    }
    adt.models = std::move(models.value());
    adt.wmos = std::move(wmos.value());

    adt.chunks.resize(kChunksPerTile);
    std::array<bool, kChunksPerTile> seen{};
    for (u32 i = 0; i < kChunksPerTile; ++i) {
        Result<Chunk> chunk = chunkAt(data, mcnkOffsets[i]);
        if (!chunk || chunk.value().id != chunkId("MCNK")) {
            return Error{"MCNK " + std::to_string(i) + " missing"};
        }
        const std::span<const u8> mcnk = data.subspan(mcnkOffsets[i], kChunkHeaderSize + chunk.value().data.size());
        AdtChunk parsed;
        if (Result<void> ok = McnkParser(mcnk, parsed).parse(); !ok) {
            return Error{"MCNK " + std::to_string(i) + ": " + ok.error().message};
        }
        if (parsed.indexX >= 16 || parsed.indexY >= 16) {
            return Error{"MCNK " + std::to_string(i) + " has bad indices"};
        }
        for (u32 l = 0; l < parsed.layerCount; ++l) {
            if (parsed.layers[l].textureId >= adt.textures.size()) {
                return Error{"MCNK " + std::to_string(i) + " references a missing texture"};
            }
        }
        const u32 slot = parsed.indexY * 16 + parsed.indexX;
        if (seen[slot]) {
            return Error{"Two MCNK chunks for slot " + std::to_string(slot)};
        }
        seen[slot] = true;
        adt.chunks[slot] = std::move(parsed);
    }
    return adt;
}

std::string wdtPath(std::string_view mapDirectory) {
    const std::string dir(mapDirectory);
    return "World\\Maps\\" + dir + "\\" + dir + ".wdt";
}

std::string wdlPath(std::string_view mapDirectory) {
    const std::string dir(mapDirectory);
    return "World\\Maps\\" + dir + "\\" + dir + ".wdl";
}

std::string adtPath(std::string_view mapDirectory, u32 tx, u32 ty) {
    const std::string dir(mapDirectory);
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "_%u_%u.adt", tx, ty);
    return "World\\Maps\\" + dir + "\\" + dir + suffix;
}

} // namespace vd::formats
