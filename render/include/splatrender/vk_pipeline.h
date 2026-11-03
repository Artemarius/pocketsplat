#pragma once

#include <volk.h>

#include <cstdint>
#include <span>

#include "splatcore/result.h"

namespace splatrender {

struct GraphicsPipelineCreateInfo {
    std::span<const std::uint32_t> vertex_spirv;
    std::span<const std::uint32_t> fragment_spirv;
    VkFormat color_format = VK_FORMAT_UNDEFINED;  // single color attachment, dynamic rendering
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    bool alpha_blend = false;  // premultiplied-style "over" blend when true
    std::uint32_t push_constant_bytes = 0;
    VkShaderStageFlags push_constant_stages = 0;
    std::span<const VkDescriptorSetLayout> set_layouts;
    std::span<const std::uint32_t> vertex_constants;    // specializes vertex constant_id 0, 1, ... in order
    std::span<const std::uint32_t> fragment_constants;  // specializes fragment constant_id 0, 1, ... in order
};

struct ComputePipelineCreateInfo {
    std::span<const std::uint32_t> spirv;
    std::uint32_t push_constant_bytes = 0;
    std::span<const VkDescriptorSetLayout> set_layouts;
    std::uint32_t workgroup_size = 0;          // 0 = shader default; otherwise specializes constant_id 0
    std::span<const std::uint32_t> constants;  // specializes constant_id 1, 2, ... in order
    // 0 = the driver's choice. Otherwise the pipeline runs with exactly this many lanes per
    // subgroup and full subgroups (needs Context::compute_subgroup_size_control() and a workgroup
    // size that is a multiple of it).
    std::uint32_t required_subgroup_size = 0;
};

// Pipeline + its layout. Dynamic viewport/scissor, no vertex input (buffers are read as SSBOs or
// generated from gl_VertexIndex), dynamic rendering only - no render-pass objects (CLAUDE.md §6).
class Pipeline {
public:
    Pipeline() = default;
    ~Pipeline() { destroy(); }
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    splatcore::Status init_graphics(VkDevice device, const GraphicsPipelineCreateInfo& info);
    splatcore::Status init_compute(VkDevice device, const ComputePipelineCreateInfo& info);
    void destroy();

    VkPipeline handle() const { return m_pipeline; }
    VkPipelineLayout layout() const { return m_layout; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
};

}  // namespace splatrender
