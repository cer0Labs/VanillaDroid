#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "render/vk/VkCommon.h"

#include <span>

namespace vd::vk {

class Device;

enum class BlendMode : u8 {
    Opaque,
    Alpha,      // src * a + dst * (1 - a)
    Additive,   // src * a + dst
    Modulate,   // src * dst
    Modulate2x, // 2 * src * dst
    Premultiplied, // src + dst * (1 - a): src carries color * alpha (alpha 0 adds light)
};

// Everything the world pipelines differ in; the rest (dynamic viewport/scissor, 1 sample, triangle
// lists, one color attachment) is shared.
struct GraphicsPipelineDesc {
    std::span<const u32> vertexSpv;
    std::span<const u32> fragmentSpv;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    std::span<const VkVertexInputBindingDescription> bindings;
    std::span<const VkVertexInputAttributeDescription> attributes;
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT; // front faces are counter-clockwise
    bool depthTest = true;
    bool depthWrite = true;
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
    BlendMode blend = BlendMode::Opaque;
    const char* name = "pipeline"; // for error messages
    // Phase 18 (world): the render pass's samples (0: the main pass's, Device::mainPassSamples()), and alpha to
    // coverage for alpha-tested parts when there are several: the fragment shader is then `fragmentCoverageSpv` (its
    // alpha test as a coverage ramp a pixel wide), a compiled variant rather than a specialization constant (an
    // Adreno 650 driver lost the plain variant's discard).
    VkSampleCountFlagBits samples = static_cast<VkSampleCountFlagBits>(0);
    bool alphaToCoverage = false;
    std::span<const u32> fragmentCoverageSpv;
};

// Built with the device's pipeline cache (persisted between runs).
Result<VkPipeline> createGraphicsPipeline(const Device& device, const GraphicsPipelineDesc& desc);

} // namespace vd::vk
