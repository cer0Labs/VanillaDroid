#include "formats/AdtSound.h"

#include "formats/ChunkReader.h"

#include <cstring>
#include <string>

namespace vd::formats {
namespace {

// MCNK header fields (formats/Adt.cpp's McnkHeader): the chunk indices and the emitters' offset (relative to the
// MCNK magic) and count.
constexpr usize kMcnkIndexX = 4;
constexpr usize kMcnkIndexY = 8;
constexpr usize kMcnkSoundOffset = 0x58;
constexpr usize kMcnkSoundCount = 0x5C;
constexpr usize kMcnkHeaderSize = 128;

AdtSoundEmitter readEmitter(const u8* at) {
    AdtSoundEmitter e;
    e.id = load<u32>(at);
    e.sound = load<u32>(at + 4);
    for (u32 a = 0; a < 3; ++a) {
        e.position[a] = load<f32>(at + 8 + a * 4);
    }
    e.minDistance = load<f32>(at + 20);
    e.maxDistance = load<f32>(at + 24);
    e.cutoffDistance = load<f32>(at + 28);
    e.startMinute = load<u16>(at + 32);
    e.endMinute = load<u16>(at + 34);
    e.mode = load<u16>(at + 36);
    e.loopCountMin = at[38];
    e.loopCountMax = at[39];
    e.silenceMinMs = load<u16>(at + 40);
    e.silenceMaxMs = load<u16>(at + 42);
    e.instancesMin = load<u16>(at + 44);
    e.instancesMax = load<u16>(at + 46);
    e.gapMinMs = load<u16>(at + 48);
    e.gapMaxMs = load<u16>(at + 50);
    return e;
}

} // namespace

Result<std::vector<AdtSoundEmitter>> parseAdtSoundEmitters(std::span<const u8> data) {
    std::vector<AdtSoundEmitter> out;
    usize offset = 0;
    while (offset + kChunkHeaderSize <= data.size()) {
        Result<Chunk> chunk = chunkAt(data, offset);
        if (!chunk) {
            return chunk.error();
        }
        const Chunk& c = chunk.value();
        if (c.id == chunkId("MCNK") && c.data.size() >= kMcnkHeaderSize) {
            const u32 count = load<u32>(c.data.data() + kMcnkSoundCount);
            const u32 at = load<u32>(c.data.data() + kMcnkSoundOffset);
            if (count > 0) {
                const std::span<const u8> mcnk = data.subspan(offset, kChunkHeaderSize + c.data.size());
                Result<Chunk> mcse = chunkAt(mcnk, at);
                if (!mcse || mcse.value().id != chunkId("MCSE") ||
                    mcse.value().data.size() < usize{count} * kAdtSoundEmitterSize) {
                    return Error{"Bad MCSE in the MCNK at " + std::to_string(offset)};
                }
                for (u32 i = 0; i < count; ++i) {
                    AdtSoundEmitter& e =
                        out.emplace_back(readEmitter(mcse.value().data.data() + usize{i} * kAdtSoundEmitterSize));
                    e.chunkX = load<u32>(c.data.data() + kMcnkIndexX);
                    e.chunkY = load<u32>(c.data.data() + kMcnkIndexY);
                }
            }
        }
        offset += kChunkHeaderSize + c.data.size();
    }
    return out;
}

Result<AdtPlacements> parseAdtPlacements(std::span<const u8> data) {
    AdtPlacements out;
    std::span<const u8> mmdx, mmid, mwmo, mwid;
    usize offset = 0;
    while (offset + kChunkHeaderSize <= data.size()) {
        Result<Chunk> chunk = chunkAt(data, offset);
        if (!chunk) {
            return chunk.error();
        }
        const Chunk& c = chunk.value();
        if (c.id == chunkId("MMDX")) {
            mmdx = c.data;
        } else if (c.id == chunkId("MMID")) {
            mmid = c.data;
        } else if (c.id == chunkId("MDDF")) {
            for (usize i = 0; i + 36 <= c.data.size(); i += 36) {
                const u8* at = c.data.data() + i;
                AdtDoodadPlacement& p = out.doodads.emplace_back();
                p.nameId = load<u32>(at);
                p.uniqueId = load<u32>(at + 4);
                std::memcpy(p.position, at + 8, sizeof(p.position));
                std::memcpy(p.rotation, at + 20, sizeof(p.rotation));
                p.scale = load<u16>(at + 32);
                p.flags = load<u16>(at + 34);
            }
        } else if (c.id == chunkId("MWMO")) {
            mwmo = c.data;
        } else if (c.id == chunkId("MWID")) {
            mwid = c.data;
        } else if (c.id == chunkId("MODF")) {
            for (usize i = 0; i + 64 <= c.data.size(); i += 64) {
                const u8* at = c.data.data() + i;
                AdtWmoPlacement& p = out.placements.emplace_back();
                p.nameId = load<u32>(at);
                p.uniqueId = load<u32>(at + 4);
                std::memcpy(p.position, at + 8, sizeof(p.position));
                std::memcpy(p.rotation, at + 20, sizeof(p.rotation));
                std::memcpy(p.extentsMin, at + 32, sizeof(p.extentsMin));
                std::memcpy(p.extentsMax, at + 44, sizeof(p.extentsMax));
                p.flags = load<u16>(at + 56);
                p.doodadSet = load<u16>(at + 58);
                p.nameSet = load<u16>(at + 60);
            }
        } else if (c.id == chunkId("MCNK")) {
            break; // the object chunks come before the terrain's
        }
        offset += kChunkHeaderSize + c.data.size();
    }
    for (usize i = 0; i + 4 <= mmid.size(); i += 4) {
        out.models.push_back(stringAt(mmdx, load<u32>(mmid.data() + i)));
    }
    for (usize i = 0; i + 4 <= mwid.size(); i += 4) {
        out.wmos.push_back(stringAt(mwmo, load<u32>(mwid.data() + i)));
    }
    return out;
}

} // namespace vd::formats
