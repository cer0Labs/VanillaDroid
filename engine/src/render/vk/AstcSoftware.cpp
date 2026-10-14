#include "render/vk/AstcSoftware.h"

#if defined(VD_ASTC_SOFTWARE_DECODE)
#include <astcenc.h>

#include <array>
#endif

namespace vd::vk {

#if defined(VD_ASTC_SOFTWARE_DECODE)

Result<std::vector<u8>> decodeAstcSoftware(const u8* data, usize size, u32 width, u32 height, u32 blockSize) {
    // One decompress-only context per block size, reused (context creation is slow). Per thread: a context decodes
    // one image at a time, and phase 18 (world) decodes the terrain's arrays on the map loader's thread while the game
    // thread decodes streamed textures.
    struct Contexts {
        std::array<astcenc_context*, 9> byBlock{};
        ~Contexts() {
            for (astcenc_context* context : byBlock) {
                if (context) {
                    astcenc_context_free(context);
                }
            }
        }
    };
    static thread_local Contexts contexts;
    if (blockSize >= contexts.byBlock.size()) {
        return Error{"Unsupported ASTC block size"};
    }
    astcenc_context*& context = contexts.byBlock[blockSize];
    if (!context) {
        astcenc_config config{};
        if (astcenc_config_init(ASTCENC_PRF_LDR, blockSize, blockSize, 1, ASTCENC_PRE_FASTEST,
                                ASTCENC_FLG_DECOMPRESS_ONLY, &config) != ASTCENC_SUCCESS ||
            astcenc_context_alloc(&config, 1, &context, nullptr) != ASTCENC_SUCCESS) {
            context = nullptr;
            return Error{"Could not create ASTC decoder"};
        }
    }
    std::vector<u8> rgba(usize{width} * height * 4);
    void* slice = rgba.data();
    astcenc_image image{};
    image.dim_x = width;
    image.dim_y = height;
    image.dim_z = 1;
    image.data_type = ASTCENC_TYPE_U8;
    image.data = &slice;
    const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    const astcenc_error result = astcenc_decompress_image(context, data, size, &image, &swizzle, 0);
    astcenc_decompress_reset(context);
    if (result != ASTCENC_SUCCESS) {
        return Error{std::string("ASTC decode failed: ") + astcenc_get_error_string(result)};
    }
    return rgba;
}

#else

Result<std::vector<u8>> decodeAstcSoftware(const u8*, usize, u32, u32, u32) {
    return Error{"ASTC software decode is not available in this build"};
}

#endif

} // namespace vd::vk
