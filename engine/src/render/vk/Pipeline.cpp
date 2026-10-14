#include "render/vk/Pipeline.h"

#include "render/vk/Device.h"

#include <array>

namespace vd::vk {

Result<VkPipeline> createGraphicsPipeline(const Device& device, const GraphicsPipelineDesc& desc) {
    VkDevice dev = device.device();
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = desc.samples != 0 ? desc.samples : device.mainPassSamples();
    // Phase 18 (world): alpha-tested parts' edges as coverage (vegetation), with the shader's variant for it.
    const bool coverage = desc.alphaToCoverage && !desc.fragmentCoverageSpv.empty() &&
                          multisample.rasterizationSamples != VK_SAMPLE_COUNT_1_BIT;
    multisample.alphaToCoverageEnable = coverage ? VK_TRUE : VK_FALSE;
    const std::span<const u32> fragmentSpv = coverage ? desc.fragmentCoverageSpv : desc.fragmentSpv;
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    moduleInfo.codeSize = desc.vertexSpv.size_bytes();
    moduleInfo.pCode = desc.vertexSpv.data();
    if (VkResult r = vkCreateShaderModule(dev, &moduleInfo, nullptr, &vert); r != VK_SUCCESS) {
        return makeError(desc.name, r);
    }
    moduleInfo.codeSize = fragmentSpv.size_bytes();
    moduleInfo.pCode = fragmentSpv.data();
    if (VkResult r = vkCreateShaderModule(dev, &moduleInfo, nullptr, &frag); r != VK_SUCCESS) {
        vkDestroyShaderModule(dev, vert, nullptr);
        return makeError(desc.name, r);
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = static_cast<u32>(desc.bindings.size());
    vertexInput.pVertexBindingDescriptions = desc.bindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<u32>(desc.attributes.size());
    vertexInput.pVertexAttributeDescriptions = desc.attributes.data();
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = desc.cullMode;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = desc.depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = desc.depthCompare;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (desc.blend != BlendMode::Opaque) {
        blend.blendEnable = VK_TRUE;
        switch (desc.blend) {
        case BlendMode::Alpha:
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BlendMode::Additive:
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case BlendMode::Modulate:
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            break;
        case BlendMode::Modulate2x:
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
            break;
        case BlendMode::Premultiplied:
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        default: break;
        }
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blend;
    const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = static_cast<u32>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = static_cast<u32>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamicState;
    info.layout = desc.layout;
    info.renderPass = desc.renderPass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(dev, device.pipelineCache(), 1, &info, nullptr, &pipeline);
    vkDestroyShaderModule(dev, vert, nullptr);
    vkDestroyShaderModule(dev, frag, nullptr);
    if (result != VK_SUCCESS) {
        return makeError(desc.name, result);
    }
    return pipeline;
}

} // namespace vd::vk
