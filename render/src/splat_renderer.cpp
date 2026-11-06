#include "splatrender/splat_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "splat_chunk_cull_comp.spv.h"
#include "splat_frag.spv.h"
#include "splat_pick_comp.spv.h"
#include "splat_prepare_indirect_comp.spv.h"
#include "splat_preprocess_comp.spv.h"
#include "splat_sorted_copy_comp.spv.h"
#include "splat_vert.spv.h"

namespace splatrender {
namespace {

constexpr std::uint32_t kWorkgroupSize = 256;
constexpr float kNearClip = 0.2f;  // preprocess near cull, view-space depth
// The chunk-culled preprocess maps one workgroup to one chunk (shader hardcodes the stride).
static_assert(kWorkgroupSize == splatcore::kPackChunkSize);

struct CameraUbo {
    float view[16];
    float proj_view[16];
    float cam_pos[4];
    float focal_viewport[4];
};
static_assert(sizeof(CameraUbo) == 160);

struct PreprocessPush {
    std::uint32_t count;
    std::uint32_t sh_degree;
    float sh1_scale;
    float near_clip;
    std::uint32_t key_bits;
    float log_depth_min;
    float log_depth_inv_range;
    std::uint32_t use_chunk_cull;
    float min_contribution;
    float coverage_strength;  // 0 = no coverage fade
    float coverage_cos_full;
    float coverage_cos_zero;
    std::uint32_t capacity;  // record / key slots: appends at or beyond it are counted, not written
};

struct ChunkCullPush {
    std::uint32_t chunk_count;
    float min_contribution;  // the preprocess's viewport-scaled threshold; 0 = no chunk bound test
    float near_clip;
};

struct PreparePush {
    std::uint32_t elements_per_block;
    std::uint32_t draw_mode;
    std::uint32_t quads_per_batch;
    std::uint32_t capacity;  // the sort and draw count is min(visible, capacity)
};

struct DrawPush {
    float viewport[2];
};

struct SortedCopyPush {
    std::uint32_t elements_per_block;
};

struct PickPush {
    float pixel[2];
    std::uint32_t key_bits;
    float log_depth_min;
    float log_depth_inv_range;
    std::uint32_t capacity;
};

// Splats covering one pixel: a few hundred in dense views; more are dropped (the composite then
// reaches its half point on the ones kept, which the GPU appends in no particular order).
constexpr std::uint32_t kPickCapacity = 1u << 16;
constexpr VkDeviceSize kPickBytes =
    2 * sizeof(std::uint32_t) + VkDeviceSize{kPickCapacity} * 2 * sizeof(float);

void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

}  // namespace

splatcore::Status SplatRenderer::init(Context& context, const splatcore::GpuPack& pack, VkFormat color_format,
                                      const std::vector<std::uint32_t>* coverage,
                                      std::uint32_t visible_capacity) {
    using splatcore::Status;
    m_context = &context;
    if (coverage != nullptr && coverage->size() != pack.header.count) {
        return Status::failure("coverage masks do not match the scene's splat count");
    }
    m_has_coverage = coverage != nullptr && !coverage->empty();
    m_coverage_strength = 0.0f;
    set_coverage_angles(kDefaultCoverageFullDeg, kDefaultCoverageZeroDeg);
    m_count = pack.header.count;
    m_clip_floor = 0.0f;
    m_clip_floor_calm = 0;
    m_clip_floor_pending = false;
    m_clip_floor_raised_from = 0;
    m_clip_floor_ratio = kClipFloorDefaultRatio;
    m_chunk_count = static_cast<std::uint32_t>(pack.chunk_count());
    m_sh_degree = pack.header.sh_degree;
    m_sh1_scale = pack.header.sh1_scale;
    if (m_count == 0) return Status::failure("cannot render an empty pack");
    // Each per-splat array lives in one storage buffer bound whole, so the widest (12 B per splat)
    // caps the scene by the device's descriptor range (Adreno 750: 128 MB -> 11,184,810 splats);
    // the 32 B draw records hold only a frame's visible splats, up to the visible capacity.
    const std::uint64_t max_splats = SplatRenderer::max_splats(context);
    if (m_count > max_splats) {
        return Status::failure("the scene has " + std::to_string(m_count) +
                               " splats, this GPU draws at most " + std::to_string(max_splats) +
                               " (maxStorageBufferRange): import it again with a smaller budget");
    }
    m_capacity = static_cast<std::uint32_t>(std::min<std::uint64_t>(m_count, max_visible(context)));
    if (visible_capacity > 0) m_capacity = std::min(m_capacity, visible_capacity);
    if (pack.header.chunk_size != splatcore::kPackChunkSize) {
        return Status::failure("pack chunk size does not match the renderer's workgroup mapping");
    }
    std::memcpy(m_aabb_min, pack.header.aabb_min, sizeof(m_aabb_min));
    std::memcpy(m_aabb_max, pack.header.aabb_max, sizeof(m_aabb_max));

    const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    Status s = create_device_buffer(context, pack.positions.data(), pack.positions.size() * sizeof(float),
                                    storage, m_positions);
    if (s.ok()) {
        s = create_device_buffer(context, pack.scale_rot.data(),
                                 pack.scale_rot.size() * sizeof(std::uint32_t), storage, m_scale_rot);
    }
    if (s.ok()) {
        s = create_device_buffer(context, pack.color.data(), pack.color.size() * sizeof(std::uint32_t),
                                 storage, m_color);
    }
    if (s.ok()) {
        // Degree 0 has no SH data; bind a 4-byte dummy so the descriptor stays valid.
        static const std::uint32_t kDummy = 0;
        const void* sh_data = pack.sh1.empty() ? &kDummy : static_cast<const void*>(pack.sh1.data());
        const VkDeviceSize sh_size =
            pack.sh1.empty() ? sizeof(std::uint32_t) : pack.sh1.size() * sizeof(std::uint32_t);
        s = create_device_buffer(context, sh_data, sh_size, storage, m_sh1);
    }
    if (s.ok()) {
        static const std::uint32_t kNoCoverage = 0;
        const void* data = m_has_coverage ? static_cast<const void*>(coverage->data()) : &kNoCoverage;
        const VkDeviceSize size =
            m_has_coverage ? coverage->size() * sizeof(std::uint32_t) : sizeof(std::uint32_t);
        s = create_device_buffer(context, data, size, storage, m_coverage);
    }
    if (s.ok()) {
        s = m_records.init(context.allocator(), VkDeviceSize{m_capacity} * kRecordBytes, storage);
    }
    if (s.ok()) {
        s = m_keys.init(context.allocator(), VkDeviceSize{m_capacity} * sizeof(std::uint32_t), storage);
    }
    if (s.ok()) {
        s = m_values.init(context.allocator(), VkDeviceSize{m_capacity} * sizeof(std::uint32_t), storage);
    }
    if (s.ok()) {
        s = create_device_buffer(context, pack.chunks.data(), pack.chunks.size() * sizeof(float), storage,
                                 m_chunks);
    }
    if (s.ok()) {
        s = m_chunk_list.init(context.allocator(), VkDeviceSize{m_chunk_count} * sizeof(std::uint32_t),
                              storage);
    }
    if (s.ok()) {
        s = m_cull_dispatch.init(context.allocator(), 3 * sizeof(std::uint32_t),
                                 storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    }
    if (s.ok()) {
        s = m_counter.init(context.allocator(), 2 * sizeof(std::uint32_t),
                           storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    }
    for (std::uint32_t f = 0; f < kFramesInFlight && s.ok(); ++f) {
        s = m_counter_readback[f].init(
            context.allocator(), 2 * sizeof(std::uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        m_readback_pending[f] = false;
    }
    if (s.ok()) {
        s = m_sort_dispatch.init(context.allocator(), 3 * sizeof(std::uint32_t),
                                 storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    }
    if (s.ok()) {
        s = m_draw_cmd.init(context.allocator(), 5 * sizeof(std::uint32_t),
                            storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    }
    const VmaAllocationCreateFlags host_flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    if (s.ok()) {
        // Indexed-batch mode: quad q of a batch is vertices 4q..4q+3, triangles {0,1,2} and {0,2,3}.
        // 1024 quads = 4096 vertices, so 16-bit indices suffice; written once from the host.
        s = m_quad_indices.init(context.allocator(), 6 * kQuadsPerBatch * sizeof(std::uint16_t),
                                VK_BUFFER_USAGE_INDEX_BUFFER_BIT, host_flags);
        if (s.ok()) {
            auto* indices = static_cast<std::uint16_t*>(m_quad_indices.mapped());
            constexpr std::uint16_t kQuad[6] = {0, 1, 2, 0, 2, 3};
            for (std::uint32_t q = 0; q < kQuadsPerBatch; ++q) {
                for (std::uint32_t i = 0; i < 6; ++i) {
                    indices[6 * q + i] = static_cast<std::uint16_t>(4 * q + kQuad[i]);
                }
            }
        }
    }
    for (std::uint32_t f = 0; f < kFramesInFlight && s.ok(); ++f) {
        s = m_camera[f].init(context.allocator(), sizeof(CameraUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             host_flags);
    }
    if (s.ok()) {
        s = m_pick.init(context.allocator(), kPickBytes,
                        storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    }
    for (std::uint32_t f = 0; f < kFramesInFlight && s.ok(); ++f) {
        s = m_pick_readback[f].init(
            context.allocator(), kPickBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    }
    if (s.ok()) s = m_sort.init(context, m_capacity);
    if (!s.ok()) return s;

    // One layout shared by the chunk-cull and preprocess pipelines; each shader declares only the
    // subset of bindings it touches.
    const VkDescriptorSetLayoutBinding compute_bindings[] = {
        storage_binding(0, VK_SHADER_STAGE_COMPUTE_BIT),  storage_binding(1, VK_SHADER_STAGE_COMPUTE_BIT),
        storage_binding(2, VK_SHADER_STAGE_COMPUTE_BIT),  storage_binding(3, VK_SHADER_STAGE_COMPUTE_BIT),
        storage_binding(4, VK_SHADER_STAGE_COMPUTE_BIT),  storage_binding(5, VK_SHADER_STAGE_COMPUTE_BIT),
        storage_binding(6, VK_SHADER_STAGE_COMPUTE_BIT),  storage_binding(7, VK_SHADER_STAGE_COMPUTE_BIT),
        uniform_binding(8, VK_SHADER_STAGE_COMPUTE_BIT),  storage_binding(9, VK_SHADER_STAGE_COMPUTE_BIT),
        storage_binding(10, VK_SHADER_STAGE_COMPUTE_BIT), storage_binding(11, VK_SHADER_STAGE_COMPUTE_BIT),
        storage_binding(12, VK_SHADER_STAGE_COMPUTE_BIT)};
    const VkDescriptorSetLayoutBinding prepare_bindings[] = {
        storage_binding(0, VK_SHADER_STAGE_COMPUTE_BIT), storage_binding(1, VK_SHADER_STAGE_COMPUTE_BIT),
        storage_binding(2, VK_SHADER_STAGE_COMPUTE_BIT), storage_binding(3, VK_SHADER_STAGE_COMPUTE_BIT)};
    const VkDescriptorSetLayoutBinding draw_bindings[] = {storage_binding(0, VK_SHADER_STAGE_VERTEX_BIT),
                                                          storage_binding(1, VK_SHADER_STAGE_VERTEX_BIT),
                                                          storage_binding(2, VK_SHADER_STAGE_VERTEX_BIT)};
    const VkDescriptorSetLayoutBinding pick_bindings[] = {
        storage_binding(0, VK_SHADER_STAGE_COMPUTE_BIT), storage_binding(1, VK_SHADER_STAGE_COMPUTE_BIT),
        storage_binding(2, VK_SHADER_STAGE_COMPUTE_BIT), storage_binding(3, VK_SHADER_STAGE_COMPUTE_BIT),
        storage_binding(4, VK_SHADER_STAGE_COMPUTE_BIT)};
    const VkDescriptorSetLayoutBinding copy_bindings[] = {
        storage_binding(0, VK_SHADER_STAGE_COMPUTE_BIT), storage_binding(1, VK_SHADER_STAGE_COMPUTE_BIT),
        storage_binding(2, VK_SHADER_STAGE_COMPUTE_BIT), storage_binding(3, VK_SHADER_STAGE_COMPUTE_BIT)};
    s = m_compute_layout.init(context.device(), compute_bindings);
    if (s.ok()) s = m_prepare_layout.init(context.device(), prepare_bindings);
    if (s.ok()) s = m_draw_layout.init(context.device(), draw_bindings);
    if (s.ok()) s = m_pick_layout.init(context.device(), pick_bindings);
    if (s.ok()) s = m_copy_layout.init(context.device(), copy_bindings);
    if (!s.ok()) return s;

    // Sets: compute per slot, prepare, draw, pick, and DrawProbe::kSortedCopy's copy + draw sets
    // (allocated and written when the probe is first asked for).
    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 12 * kFramesInFlight + 4 + 3 + 5 + 4 + 3},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight}};
    s = m_pool.init(context.device(), pool_sizes, kFramesInFlight + 5);
    if (!s.ok()) return s;
    for (std::uint32_t f = 0; f < kFramesInFlight; ++f) {
        splatcore::Result<VkDescriptorSet> set = m_pool.allocate(m_compute_layout.handle());
        if (!set) return Status::failure(set.error());
        m_compute_sets[f] = set.value();
        write_buffer_descriptor(context.device(), m_compute_sets[f], 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_positions.handle());
        write_buffer_descriptor(context.device(), m_compute_sets[f], 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_scale_rot.handle());
        write_buffer_descriptor(context.device(), m_compute_sets[f], 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_color.handle());
        write_buffer_descriptor(context.device(), m_compute_sets[f], 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_sh1.handle());
        write_buffer_descriptor(context.device(), m_compute_sets[f], 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_records.handle());
        write_buffer_descriptor(context.device(), m_compute_sets[f], 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_keys.handle());
        write_buffer_descriptor(context.device(), m_compute_sets[f], 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_values.handle());
        write_buffer_descriptor(context.device(), m_compute_sets[f], 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_counter.handle());
        write_buffer_descriptor(context.device(), m_compute_sets[f], 8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                m_camera[f].handle());
        write_buffer_descriptor(context.device(), m_compute_sets[f], 9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_chunks.handle());
        write_buffer_descriptor(context.device(), m_compute_sets[f], 10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_chunk_list.handle());
        write_buffer_descriptor(context.device(), m_compute_sets[f], 11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_cull_dispatch.handle());
        write_buffer_descriptor(context.device(), m_compute_sets[f], 12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_coverage.handle());
    }
    {
        splatcore::Result<VkDescriptorSet> set = m_pool.allocate(m_prepare_layout.handle());
        if (!set) return Status::failure(set.error());
        m_prepare_set = set.value();
        write_buffer_descriptor(context.device(), m_prepare_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_counter.handle());
        write_buffer_descriptor(context.device(), m_prepare_set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_sort.args_buffer().handle());
        write_buffer_descriptor(context.device(), m_prepare_set, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_sort_dispatch.handle());
        write_buffer_descriptor(context.device(), m_prepare_set, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_draw_cmd.handle());
    }
    {
        splatcore::Result<VkDescriptorSet> set = m_pool.allocate(m_draw_layout.handle());
        if (!set) return Status::failure(set.error());
        m_draw_set = set.value();
        write_buffer_descriptor(context.device(), m_draw_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_records.handle());
        write_buffer_descriptor(context.device(), m_draw_set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_values.handle());
        write_buffer_descriptor(context.device(), m_draw_set, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_sort.args_buffer().handle());
    }
    {
        splatcore::Result<VkDescriptorSet> set = m_pool.allocate(m_pick_layout.handle());
        if (!set) return Status::failure(set.error());
        m_pick_set = set.value();
        // The pick reads the sorted count (sort args: min(visible, capacity)), not the raw counter.
        const VkBuffer pick_buffers[] = {m_records.handle(), m_keys.handle(), m_values.handle(),
                                         m_sort.args_buffer().handle(), m_pick.handle()};
        for (std::uint32_t b = 0; b < 5; ++b) {
            write_buffer_descriptor(context.device(), m_pick_set, b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                    pick_buffers[b]);
        }
    }

    ComputePipelineCreateInfo preprocess_info;
    preprocess_info.spirv = std::span(k_splat_preprocess_comp_spv);
    preprocess_info.push_constant_bytes = sizeof(PreprocessPush);
    const VkDescriptorSetLayout compute_layouts[] = {m_compute_layout.handle()};
    preprocess_info.set_layouts = compute_layouts;
    preprocess_info.workgroup_size = kWorkgroupSize;
    s = m_preprocess.init_compute(context.device(), preprocess_info);
    if (!s.ok()) return s;

    ComputePipelineCreateInfo cull_info;
    cull_info.spirv = std::span(k_splat_chunk_cull_comp_spv);
    cull_info.push_constant_bytes = sizeof(ChunkCullPush);
    cull_info.set_layouts = compute_layouts;
    s = m_chunk_cull_pipe.init_compute(context.device(), cull_info);
    if (!s.ok()) return s;

    ComputePipelineCreateInfo prepare_info;
    prepare_info.spirv = std::span(k_splat_prepare_indirect_comp_spv);
    prepare_info.push_constant_bytes = sizeof(PreparePush);
    const VkDescriptorSetLayout prepare_layouts[] = {m_prepare_layout.handle()};
    prepare_info.set_layouts = prepare_layouts;
    s = m_prepare.init_compute(context.device(), prepare_info);
    if (!s.ok()) return s;

    ComputePipelineCreateInfo pick_info;
    pick_info.spirv = std::span(k_splat_pick_comp_spv);
    pick_info.push_constant_bytes = sizeof(PickPush);
    const VkDescriptorSetLayout pick_layouts[] = {m_pick_layout.handle()};
    pick_info.set_layouts = pick_layouts;
    pick_info.workgroup_size = kWorkgroupSize;
    s = m_pick_pipe.init_compute(context.device(), pick_info);
    if (!s.ok()) return s;

    ComputePipelineCreateInfo copy_info;
    copy_info.spirv = std::span(k_splat_sorted_copy_comp_spv);
    copy_info.push_constant_bytes = sizeof(SortedCopyPush);
    const VkDescriptorSetLayout copy_layouts[] = {m_copy_layout.handle()};
    copy_info.set_layouts = copy_layouts;
    copy_info.workgroup_size = kWorkgroupSize;
    s = m_copy_pipe.init_compute(context.device(), copy_info);
    if (!s.ok()) return s;

    GraphicsPipelineCreateInfo draw_info;
    draw_info.vertex_spirv = std::span(k_splat_vert_spv);
    draw_info.fragment_spirv = std::span(k_splat_frag_spv);
    draw_info.color_format = color_format;
    draw_info.alpha_blend = true;
    draw_info.push_constant_bytes = sizeof(DrawPush);
    draw_info.push_constant_stages = VK_SHADER_STAGE_VERTEX_BIT;
    const VkDescriptorSetLayout draw_layouts[] = {m_draw_layout.handle()};
    draw_info.set_layouts = draw_layouts;
    for (std::uint32_t mode = 0; mode < kDrawModeCount && s.ok(); ++mode) {
        const std::uint32_t constants[] = {mode, kQuadsPerBatch, 0u};
        draw_info.vertex_constants = constants;
        draw_info.topology = mode == static_cast<std::uint32_t>(DrawMode::kInstanced4)
                                 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                                 : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        s = m_draw[mode].init_graphics(context.device(), draw_info);
    }
    draw_info.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    for (std::uint32_t probe = 1; probe < kDrawProbeCount && s.ok(); ++probe) {
        const std::uint32_t constants[] = {static_cast<std::uint32_t>(DrawMode::kTriangle), kQuadsPerBatch,
                                           probe};
        const std::uint32_t fragment_constants[] = {probe};
        draw_info.vertex_constants = constants;
        draw_info.fragment_constants = fragment_constants;
        draw_info.alpha_blend = probe != static_cast<std::uint32_t>(DrawProbe::kNoBlend);
        s = m_draw_probes[probe].init_graphics(context.device(), draw_info);
    }
    return s;
}

void SplatRenderer::prepare_frame(std::uint32_t slot, const SplatCameraParams& camera) {
    CameraUbo ubo{};
    std::memcpy(ubo.view, camera.view, sizeof(ubo.view));
    std::memcpy(ubo.proj_view, camera.proj_view, sizeof(ubo.proj_view));
    std::memcpy(ubo.cam_pos, camera.cam_pos, sizeof(camera.cam_pos));
    ubo.focal_viewport[0] = camera.focal_x;
    ubo.focal_viewport[1] = camera.focal_y;
    ubo.focal_viewport[2] = camera.viewport_w;
    ubo.focal_viewport[3] = camera.viewport_h;
    std::memcpy(m_camera[slot].mapped(), &ubo, sizeof(ubo));
    if (!m_has_last_camera || std::memcmp(m_last_camera, &ubo, sizeof(ubo)) != 0) {
        std::memcpy(m_last_camera, &ubo, sizeof(ubo));
        m_has_last_camera = true;
        m_compute_dirty = true;
    }
    m_last_viewport[0] = camera.viewport_w;
    m_last_viewport[1] = camera.viewport_h;
    {
        // Depth range every splat center can occupy this frame: the pack AABB's corners in view
        // space, clamped to the near cull. The 16-bit keys spread log depth over it.
        float near_depth = 3.4e38f, far_depth = 0.0f;
        for (int corner = 0; corner < 8; ++corner) {
            const float x = (corner & 1) ? m_aabb_max[0] : m_aabb_min[0];
            const float y = (corner & 2) ? m_aabb_max[1] : m_aabb_min[1];
            const float z = (corner & 4) ? m_aabb_max[2] : m_aabb_min[2];
            const float depth =
                -(camera.view[2] * x + camera.view[6] * y + camera.view[10] * z + camera.view[14]);
            near_depth = std::min(near_depth, depth);
            far_depth = std::max(far_depth, depth);
        }
        near_depth = std::max(near_depth, kNearClip);
        far_depth = std::max(far_depth, near_depth * 1.001f);
        m_log_depth_min = std::log(near_depth);
        m_log_depth_inv_range = 1.0f / (std::log(far_depth) - m_log_depth_min);
    }
    if (m_readback_pending[slot]) {
        // This slot's previous submission has completed, so its snapshot holds the exact counts
        // of that frame. Frames that skipped the compute chain keep the last values.
        m_counter_readback[slot].invalidate();
        std::uint32_t counts[2];
        std::memcpy(counts, m_counter_readback[slot].mapped(), sizeof(counts));
        m_last_visible = counts[0];
        m_last_visible_chunks = counts[1];
        m_readback_pending[slot] = false;
        update_clip_floor(m_slot_floor[slot], counts[0]);
    }
    if (m_pick_slot == static_cast<int>(slot)) resolve_pick(slot);  // that submission has completed
}

void SplatRenderer::update_clip_floor(float frame_floor, std::uint32_t visible) {
    // Counts computed at an older floor say nothing about the current one (frames in flight).
    if (!m_clip_floor_enabled || frame_floor != m_clip_floor) return;
    if (m_clip_floor_pending && m_clip_floor_raised_from > 0) {
        // First count at a raised floor: how much the raise removed predicts what lowering adds.
        m_clip_floor_ratio = std::max(
            1.0f, static_cast<float>(m_clip_floor_raised_from) / static_cast<float>(std::max(visible, 1u)));
        m_clip_floor_raised_from = 0;
    }
    m_clip_floor_pending = false;
    if (visible > m_capacity) {
        const float factor = visible - m_capacity > m_capacity / 4 ? 4.0f : 2.0f;
        m_clip_floor = std::min(std::max(kClipFloorStart, effective_min_contribution() * factor), 1.0e4f);
        m_clip_floor_raised_from = visible;
        m_clip_floor_calm = 0;
        m_clip_floor_pending = true;
        m_compute_dirty = true;
    } else if (m_clip_floor > 0.0f &&
               static_cast<float>(visible) * m_clip_floor_ratio < 0.9f * static_cast<float>(m_capacity)) {
        if (++m_clip_floor_calm >= kClipFloorCalmFrames) {
            m_clip_floor /= 1.5f;
            if (m_clip_floor < kClipFloorStart || m_clip_floor <= m_min_contribution) m_clip_floor = 0.0f;
            m_clip_floor_calm = 0;
            m_clip_floor_pending = true;
            m_compute_dirty = true;
        }
    } else {
        m_clip_floor_calm = 0;
    }
}

bool SplatRenderer::record_compute(VkCommandBuffer cmd, std::uint32_t slot, GpuTimer* timer) {
    if (!m_compute_dirty) {
        // Camera and settings unchanged since the last computed frame: records, sorted
        // keys/values and the indirect commands are still valid, and no prior-frame consumer
        // hazard exists because nothing is written. Keep the timer's segment layout stable.
        if (timer != nullptr) {
            timer->stamp(cmd, slot);
            timer->stamp(cmd, slot);
        }
        return false;
    }
    m_compute_dirty = false;

    // Previous frame consumed records/values (vertex), draw_cmd/sort_dispatch (indirect), copied
    // the counter (copy) and the sort touched keys/values (compute); wait for all of it before
    // this frame's writes.
    memory_barrier(cmd,
                   VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                       VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                       VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
                   VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    vkCmdFillBuffer(cmd, m_counter.handle(), 0, 2 * sizeof(std::uint32_t), 0);
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    const float scaled_min_contribution =
        effective_min_contribution() * m_last_viewport[0] * m_last_viewport[1] / kContributionReferencePixels;
    m_slot_floor[slot] = m_clip_floor;
    if (m_chunk_cull) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_chunk_cull_pipe.handle());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_chunk_cull_pipe.layout(), 0, 1,
                                &m_compute_sets[slot], 0, nullptr);
        const ChunkCullPush cull_push{m_chunk_count,
                                      m_chunk_contribution_cull ? scaled_min_contribution : 0.0f, kNearClip};
        vkCmdPushConstants(cmd, m_chunk_cull_pipe.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cull_push),
                           &cull_push);
        vkCmdDispatch(cmd, 1, 1, 1);
        memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    }
    if (timer != nullptr) timer->stamp(cmd, slot);  // boundary: chunk cull / preprocess

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_preprocess.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_preprocess.layout(), 0, 1,
                            &m_compute_sets[slot], 0, nullptr);
    const PreprocessPush push{
        m_count,
        m_sh_degree,
        m_sh1_scale,
        kNearClip,
        static_cast<std::uint32_t>(m_key_bits),
        m_log_depth_min,
        m_log_depth_inv_range,
        m_chunk_cull ? 1u : 0u,
        scaled_min_contribution,
        m_has_coverage ? 1.0f - (1.0f - m_coverage_strength) * (1.0f - m_coverage_strength) : 0.0f,
        m_coverage_cos_full,
        m_coverage_cos_zero,
        m_capacity};
    vkCmdPushConstants(cmd, m_preprocess.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    if (m_chunk_cull) {
        vkCmdDispatchIndirect(cmd, m_cull_dispatch.handle(), 0);
    } else {
        vkCmdDispatch(cmd, (m_count + kWorkgroupSize - 1) / kWorkgroupSize, 1, 1);
    }
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                       VK_ACCESS_2_TRANSFER_READ_BIT);
    if (timer != nullptr) timer->stamp(cmd, slot);  // boundary: preprocess / prepare+sort

    // Snapshot the final counts for the host (read in prepare_frame once this slot completes).
    const VkBufferCopy counter_region{0, 0, 2 * sizeof(std::uint32_t)};
    vkCmdCopyBuffer(cmd, m_counter.handle(), m_counter_readback[slot].handle(), 1, &counter_region);
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    m_readback_pending[slot] = true;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_prepare.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_prepare.layout(), 0, 1, &m_prepare_set, 0,
                            nullptr);
    const PreparePush prepare_push{kSortElementsPerBlock, static_cast<std::uint32_t>(m_draw_mode),
                                   kQuadsPerBatch, m_capacity};
    vkCmdPushConstants(cmd, m_prepare.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(prepare_push),
                       &prepare_push);
    vkCmdDispatch(cmd, 1, 1, 1);
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);

    // Only the order (values) is consumed downstream, so the last pass skips the key writes -
    // except on a pick frame, whose pick pass reads each draw position's depth key.
    const bool pick = m_pick_requested;
    m_sort.record_indirect(cmd, m_keys, m_values, m_sort_dispatch, m_key_bits, pick);

    memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    if (m_draw_mode == DrawMode::kTriangle && m_draw_probe == DrawProbe::kSortedCopy) record_sorted_copy(cmd);
    if (pick) record_pick(cmd, slot);
    return true;
}

splatcore::Status SplatRenderer::set_draw_probe(DrawProbe probe) {
    if (probe == DrawProbe::kSortedCopy && m_context == nullptr) {
        return splatcore::Status::failure("the sorted-copy draw probe needs an initialized renderer");
    }
    if (probe == DrawProbe::kSortedCopy && m_sorted_records.handle() == VK_NULL_HANDLE) {
        // First use: the draw-ordered record buffer and the two sets that bind it.
        const VkDevice device = m_context->device();
        splatcore::Status s =
            m_sorted_records.init(m_context->allocator(), VkDeviceSize{m_capacity} * kRecordBytes,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!s.ok()) return s;
        splatcore::Result<VkDescriptorSet> copy_set = m_pool.allocate(m_copy_layout.handle());
        splatcore::Result<VkDescriptorSet> draw_set = m_pool.allocate(m_draw_layout.handle());
        if (!copy_set || !draw_set) {
            m_sorted_records.destroy();
            return splatcore::Status::failure(!copy_set ? copy_set.error() : draw_set.error());
        }
        m_copy_set = copy_set.value();
        m_draw_copy_set = draw_set.value();
        const VkBuffer copy_buffers[] = {m_records.handle(), m_values.handle(), m_sort.args_buffer().handle(),
                                         m_sorted_records.handle()};
        for (std::uint32_t b = 0; b < 4; ++b) {
            write_buffer_descriptor(device, m_copy_set, b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                    copy_buffers[b]);
        }
        const VkBuffer draw_buffers[] = {m_sorted_records.handle(), m_values.handle(),
                                         m_sort.args_buffer().handle()};
        for (std::uint32_t b = 0; b < 3; ++b) {
            write_buffer_descriptor(device, m_draw_copy_set, b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                    draw_buffers[b]);
        }
    }
    m_draw_probe = probe;
    m_compute_dirty = true;  // kSortedCopy's buffer is filled by the compute chain
    return splatcore::Status::success();
}

void SplatRenderer::record_sorted_copy(VkCommandBuffer cmd) {
    // The sort's writes are visible to compute (barrier above). One workgroup per sort block.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_copy_pipe.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_copy_pipe.layout(), 0, 1, &m_copy_set, 0,
                            nullptr);
    const SortedCopyPush push{kSortElementsPerBlock};
    vkCmdPushConstants(cmd, m_copy_pipe.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatchIndirect(cmd, m_sort_dispatch.handle(), 0);
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

void SplatRenderer::request_pick(float x_px, float y_px) {
    m_pick_px[0] = x_px;
    m_pick_px[1] = y_px;
    m_pick_requested = true;
    m_pick_result = PickResult::kNone;
    m_compute_dirty = true;  // the pick needs a sort that keeps its keys
}

SplatRenderer::PickResult SplatRenderer::take_pick(float world[3]) {
    if (m_pick_result == PickResult::kHit || m_pick_result == PickResult::kMiss) {
        const PickResult result = m_pick_result;
        if (result == PickResult::kHit) std::memcpy(world, m_pick_world, sizeof(m_pick_world));
        m_pick_result = PickResult::kNone;
        return result;
    }
    return m_pick_requested || m_pick_slot >= 0 ? PickResult::kPending : PickResult::kNone;
}

void SplatRenderer::record_pick(VkCommandBuffer cmd, std::uint32_t slot) {
    // The sort's writes are visible to compute (barrier above); zero the count, append, copy out.
    vkCmdFillBuffer(cmd, m_pick.handle(), 0, sizeof(std::uint32_t), 0);
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pick_pipe.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pick_pipe.layout(), 0, 1, &m_pick_set, 0,
                            nullptr);
    const PickPush push{{m_pick_px[0], m_pick_px[1]},
                        static_cast<std::uint32_t>(m_key_bits),
                        m_log_depth_min,
                        m_log_depth_inv_range,
                        kPickCapacity};
    vkCmdPushConstants(cmd, m_pick_pipe.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (m_capacity + kWorkgroupSize - 1) / kWorkgroupSize, 1, 1);  // draw positions
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    const VkBufferCopy region{0, 0, kPickBytes};
    vkCmdCopyBuffer(cmd, m_pick.handle(), m_pick_readback[slot].handle(), 1, &region);
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    std::memcpy(m_pick_camera, m_last_camera, sizeof(m_pick_camera));
    m_pick_slot = static_cast<int>(slot);
    m_pick_requested = false;
}

void SplatRenderer::resolve_pick(std::uint32_t slot) {
    m_pick_slot = -1;
    m_pick_readback[slot].invalidate();
    const auto* data = static_cast<const unsigned char*>(m_pick_readback[slot].mapped());
    std::uint32_t count = 0;
    std::memcpy(&count, data, sizeof(count));
    count = std::min(count, kPickCapacity);
    std::vector<std::pair<float, float>> hits(count);  // (depth, alpha)
    std::memcpy(hits.data(), data + 2 * sizeof(std::uint32_t), count * sizeof(hits[0]));
    std::sort(hits.begin(), hits.end());  // front to back
    float transmittance = 1.0f;
    for (const auto& [depth, alpha] : hits) transmittance *= 1.0f - alpha;
    const float covered = 1.0f - transmittance;
    m_pick_result = PickResult::kMiss;
    if (covered < 0.1f) return;
    // The depth at which the composite reaches half of its total opacity.
    float depth = hits.back().first;
    transmittance = 1.0f;
    for (const auto& [d, alpha] : hits) {
        transmittance *= 1.0f - alpha;
        if (1.0f - transmittance >= 0.5f * covered) {
            depth = d;
            break;
        }
    }
    // Back through that frame's camera: view space (RUB, looking down -z, y up), then world.
    CameraUbo camera;
    std::memcpy(&camera, m_pick_camera, sizeof(camera));
    const float fx = camera.focal_viewport[0], fy = camera.focal_viewport[1];
    const float w = camera.focal_viewport[2], h = camera.focal_viewport[3];
    const float view_pos[3] = {(m_pick_px[0] - 0.5f * w) / fx * depth, (0.5f * h - m_pick_px[1]) / fy * depth,
                               -depth};
    const float* m = camera.view;  // column-major world -> view: rotation R, translation t
    for (int c = 0; c < 3; ++c) {
        m_pick_world[c] = 0.0f;
        for (int r = 0; r < 3; ++r) m_pick_world[c] += m[4 * c + r] * (view_pos[r] - m[12 + r]);
    }
    m_pick_result = PickResult::kHit;
}

void SplatRenderer::record_draw(VkCommandBuffer cmd, std::uint32_t slot) {
    (void)slot;  // draw resources are shared; ordering comes from the sorted values buffer
    // The draw arguments were written for the mode of the last computed frame; a mode change marks
    // the compute chain dirty, so the two agree.
    const bool probe = m_draw_mode == DrawMode::kTriangle && m_draw_probe != DrawProbe::kNone;
    const Pipeline& draw = probe ? m_draw_probes[static_cast<std::uint32_t>(m_draw_probe)]
                                 : m_draw[static_cast<std::uint32_t>(m_draw_mode)];
    const VkDescriptorSet set =
        probe && m_draw_probe == DrawProbe::kSortedCopy ? m_draw_copy_set : m_draw_set;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.layout(), 0, 1, &set, 0, nullptr);
    const DrawPush push{{m_last_viewport[0], m_last_viewport[1]}};
    vkCmdPushConstants(cmd, draw.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    if (m_draw_mode == DrawMode::kIndexedBatch) {
        vkCmdBindIndexBuffer(cmd, m_quad_indices.handle(), 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexedIndirect(cmd, m_draw_cmd.handle(), 0, 1, 5 * sizeof(std::uint32_t));
    } else {
        vkCmdDrawIndirect(cmd, m_draw_cmd.handle(), 0, 1, 4 * sizeof(std::uint32_t));
    }
}

void SplatRenderer::set_coverage_angles(float full_deg, float zero_deg) {
    // smoothstep needs edge0 < edge1: keep at least 1 deg between the angles, both in [0, 180].
    const float full = std::clamp(full_deg, 0.0f, 179.0f);
    const float zero = std::clamp(zero_deg, full + 1.0f, 180.0f);
    constexpr float kDegToRad = 0.017453292519943295f;
    m_coverage_cos_full = std::cos(full * kDegToRad);
    m_coverage_cos_zero = std::cos(zero * kDegToRad);
    m_compute_dirty = true;
}

void SplatRenderer::destroy() {
    if (m_context == nullptr) return;
    m_context->wait_idle();
    m_chunk_cull_pipe.destroy();
    m_preprocess.destroy();
    m_prepare.destroy();
    for (Pipeline& draw : m_draw) draw.destroy();
    for (Pipeline& draw : m_draw_probes) draw.destroy();
    m_copy_pipe.destroy();
    m_sort.destroy();
    m_pool.destroy();
    m_copy_set = VK_NULL_HANDLE;
    m_draw_copy_set = VK_NULL_HANDLE;
    m_copy_layout.destroy();
    m_sorted_records.destroy();
    m_draw_probe = DrawProbe::kNone;
    m_compute_layout.destroy();
    m_prepare_layout.destroy();
    m_draw_layout.destroy();
    m_positions.destroy();
    m_scale_rot.destroy();
    m_color.destroy();
    m_sh1.destroy();
    m_coverage.destroy();
    m_records.destroy();
    m_keys.destroy();
    m_values.destroy();
    m_chunks.destroy();
    m_chunk_list.destroy();
    m_cull_dispatch.destroy();
    m_counter.destroy();
    for (std::uint32_t f = 0; f < kFramesInFlight; ++f) m_counter_readback[f].destroy();
    m_sort_dispatch.destroy();
    m_draw_cmd.destroy();
    m_quad_indices.destroy();
    for (std::uint32_t f = 0; f < kFramesInFlight; ++f) m_camera[f].destroy();
    m_pick_pipe.destroy();
    m_pick_layout.destroy();
    m_pick.destroy();
    for (std::uint32_t f = 0; f < kFramesInFlight; ++f) m_pick_readback[f].destroy();
    m_pick_slot = -1;
    m_pick_requested = false;
    m_pick_result = PickResult::kNone;
    m_context = nullptr;
}

}  // namespace splatrender
