#include "render/TextureStaging.h"

#include "render/vk/AstcSoftware.h"

#include <algorithm>
#include <cstring>

namespace vd {

pack::TextureHeader stagedTextureLayout(const pack::TextureHeader& blob, bool nativeAstc, u64& bytes) {
    const u32 block = pack::astcBlockSize(static_cast<pack::TextureFormat>(blob.format));
    const u32 mips = std::min<u32>(blob.mipCount, pack::kMaxMips);
    if (block == 0 || nativeAstc) {
        bytes = 0;
        for (u32 level = 0; level < mips; ++level) {
            bytes = std::max<u64>(bytes, u64{blob.mipOffsets[level]} + blob.mipSizes[level]);
        }
        return blob;
    }
    pack::TextureHeader staged = blob;
    staged.format = static_cast<u16>(pack::TextureFormat::Rgba8);
    const u32 layers = std::max<u32>(1, blob.layers);
    u64 offset = 0;
    for (u32 level = 0; level < mips; ++level) {
        const u64 w = std::max(1u, u32{blob.width} >> level);
        const u64 h = std::max(1u, u32{blob.height} >> level);
        staged.mipOffsets[level] = static_cast<u32>(offset);
        staged.mipSizes[level] = static_cast<u32>(w * h * 4 * layers);
        offset = (offset + staged.mipSizes[level] + 15) & ~u64{15};
    }
    bytes = offset;
    return staged;
}

bool stageTexture(const u8* blob, usize size, const pack::TextureHeader& staged, u8* out,
                  const std::atomic<bool>* cancel, std::atomic<u32>* units) {
    if (size < sizeof(pack::TextureHeader)) {
        return false;
    }
    pack::TextureHeader source;
    std::memcpy(&source, blob, sizeof(source));
    const u32 mips = std::min<u32>(source.mipCount, pack::kMaxMips);
    const u32 layers = std::max<u32>(1, source.layers);
    const u32 block = pack::astcBlockSize(static_cast<pack::TextureFormat>(source.format));
    const bool decode = block != 0 && staged.format == static_cast<u16>(pack::TextureFormat::Rgba8);
    if (!decode) {
        std::memcpy(out, blob, sizeof(source)); // the header too: offsets stay those of the blob
    }
    for (u32 level = 0; level < mips; ++level) {
        if (u64{source.mipOffsets[level]} + source.mipSizes[level] > size) {
            return false;
        }
        const u32 layerBytes = source.mipSizes[level] / layers;
        const u32 stagedLayerBytes = staged.mipSizes[level] / layers;
        const u32 w = std::max(1u, u32{source.width} >> level);
        const u32 h = std::max(1u, u32{source.height} >> level);
        for (u32 layer = 0; layer < layers; ++layer) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                return false;
            }
            const u8* from = blob + source.mipOffsets[level] + usize{layer} * layerBytes;
            u8* to = out + staged.mipOffsets[level] + usize{layer} * stagedLayerBytes;
            if (decode) {
                Result<std::vector<u8>> rgba = vk::decodeAstcSoftware(from, layerBytes, w, h, block);
                if (!rgba || rgba.value().size() != stagedLayerBytes) {
                    return false;
                }
                std::memcpy(to, rgba.value().data(), stagedLayerBytes); // sequential writes: write-combined memory
            } else {
                std::memcpy(to, from, layerBytes);
            }
            if (units) {
                units->fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    return true;
}

} // namespace vd
