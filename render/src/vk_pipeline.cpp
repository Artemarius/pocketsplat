#include "splatrender/vk_pipeline.h"

#include <vector>

namespace splatrender {
namespace {

VkShaderModule make_module(VkDevice device, std::span<const std::uint32_t> spirv) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size_bytes();
    info.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &info, nullptr, &module);
    return module;
}

}  // namespace

splatcore::Status Pipeline::init_graphics(VkDevice device, const GraphicsPipelineCreateInfo& info) {
    using splatcore::Status;
    m_device = device;

    VkPushConstantRange push_range{};
    push_range.stageFlags = info.push_constant_stages;
    push_range.size = info.push_constant_bytes;
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<std::uint32_t>(info.set_layouts.size());
    layout_info.pSetLayouts = info.set_layouts.empty() ? nullptr : info.set_layouts.data();
    layout_info.pushConstantRangeCount = info.push_constant_bytes > 0 ? 1 : 0;
    layout_info.pPushConstantRanges = info.push_constant_bytes > 0 ? &push_range : nullptr;
    if (vkCreatePipelineLayout(device, &layout_info, nullptr, &m_layout) != VK_SUCCESS) {
        return Status::failure("pipeline layout creation failed");
    }

    VkShaderModule vertex = make_module(device, info.vertex_spirv);
    VkShaderModule fragment = make_module(device, info.fragment_spirv);
    if (vertex == VK_NULL_HANDLE || fragment == VK_NULL_HANDLE) {
        if (vertex != VK_NULL_HANDLE) vkDestroyShaderModule(device, vertex, nullptr);
        if (fragment != VK_NULL_HANDLE) vkDestroyShaderModule(device, fragment, nullptr);
        return Status::failure("shader module creation failed");
    }

    std::vector<VkSpecializationMapEntry> vertex_entries;
    for (std::size_t i = 0; i < info.vertex_constants.size(); ++i) {
        vertex_entries.push_back({static_cast<std::uint32_t>(i),
                                  static_cast<std::uint32_t>(i * sizeof(std::uint32_t)),
                                  sizeof(std::uint32_t)});
    }
    VkSpecializationInfo vertex_spec{};
    vertex_spec.mapEntryCount = static_cast<std::uint32_t>(vertex_entries.size());
    vertex_spec.pMapEntries = vertex_entries.data();
    vertex_spec.dataSize = info.vertex_constants.size_bytes();
    vertex_spec.pData = info.vertex_constants.data();
    std::vector<VkSpecializationMapEntry> fragment_entries;
    for (std::size_t i = 0; i < info.fragment_constants.size(); ++i) {
        fragment_entries.push_back({static_cast<std::uint32_t>(i),
                                    static_cast<std::uint32_t>(i * sizeof(std::uint32_t)),
                                    sizeof(std::uint32_t)});
    }
    VkSpecializationInfo fragment_spec{};
    fragment_spec.mapEntryCount = static_cast<std::uint32_t>(fragment_entries.size());
    fragment_spec.pMapEntries = fragment_entries.data();
    fragment_spec.dataSize = info.fragment_constants.size_bytes();
    fragment_spec.pData = info.fragment_constants.data();

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[0].pSpecializationInfo = vertex_entries.empty() ? nullptr : &vertex_spec;
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = fragment_entries.empty() ? nullptr : &fragment_spec;

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = info.topology;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (info.alpha_blend) {
        // Premultiplied "over": expected by the back-to-front splat composite.
        blend_attachment.blendEnable = VK_TRUE;
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkFormat color_format = info.color_format;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pNext = &rendering;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = m_layout;

    const VkResult result =
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &m_pipeline);
    vkDestroyShaderModule(device, vertex, nullptr);
    vkDestroyShaderModule(device, fragment, nullptr);
    if (result != VK_SUCCESS) return Status::failure("graphics pipeline creation failed");
    return Status::success();
}

splatcore::Status Pipeline::init_compute(VkDevice device, const ComputePipelineCreateInfo& info) {
    using splatcore::Status;
    m_device = device;

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.size = info.push_constant_bytes;
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<std::uint32_t>(info.set_layouts.size());
    layout_info.pSetLayouts = info.set_layouts.empty() ? nullptr : info.set_layouts.data();
    layout_info.pushConstantRangeCount = info.push_constant_bytes > 0 ? 1 : 0;
    layout_info.pPushConstantRanges = info.push_constant_bytes > 0 ? &push_range : nullptr;
    if (vkCreatePipelineLayout(device, &layout_info, nullptr, &m_layout) != VK_SUCCESS) {
        return Status::failure("compute pipeline layout creation failed");
    }

    VkShaderModule shader = make_module(device, info.spirv);
    if (shader == VK_NULL_HANDLE) return Status::failure("compute shader module creation failed");

    // constant_id 0 = workgroup size (when set), then constant_id 1.. = info.constants.
    std::vector<std::uint32_t> spec_data;
    std::vector<VkSpecializationMapEntry> spec_entries;
    if (info.workgroup_size > 0) {
        spec_entries.push_back({0, 0, sizeof(std::uint32_t)});
        spec_data.push_back(info.workgroup_size);
    }
    for (std::size_t i = 0; i < info.constants.size(); ++i) {
        const auto offset = static_cast<std::uint32_t>(spec_data.size() * sizeof(std::uint32_t));
        spec_entries.push_back({static_cast<std::uint32_t>(i + 1), offset, sizeof(std::uint32_t)});
        spec_data.push_back(info.constants[i]);
    }
    VkSpecializationInfo spec_info{};
    spec_info.mapEntryCount = static_cast<std::uint32_t>(spec_entries.size());
    spec_info.pMapEntries = spec_entries.data();
    spec_info.dataSize = spec_data.size() * sizeof(std::uint32_t);
    spec_info.pData = spec_data.data();

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = shader;
    pipeline_info.stage.pName = "main";
    pipeline_info.stage.pSpecializationInfo = spec_entries.empty() ? nullptr : &spec_info;
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_info{};
    if (info.required_subgroup_size > 0) {
        subgroup_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO;
        subgroup_info.requiredSubgroupSize = info.required_subgroup_size;
        pipeline_info.stage.pNext = &subgroup_info;
        pipeline_info.stage.flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
    }
    pipeline_info.layout = m_layout;
    const VkResult result =
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &m_pipeline);
    vkDestroyShaderModule(device, shader, nullptr);
    if (result != VK_SUCCESS) return Status::failure("compute pipeline creation failed");
    return Status::success();
}

void Pipeline::destroy() {
    if (m_device == VK_NULL_HANDLE) return;
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }
    m_device = VK_NULL_HANDLE;
}

}  // namespace splatrender
