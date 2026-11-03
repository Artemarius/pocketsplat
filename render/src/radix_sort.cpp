#include "splatrender/radix_sort.h"

#include <span>

#include "radix_downsweep_comp.spv.h"
#include "radix_downsweep_mask_comp.spv.h"
#include "radix_downsweep_staged_comp.spv.h"
#include "radix_spine_comp.spv.h"
#include "radix_upsweep_comp.spv.h"

namespace splatrender {
namespace {

constexpr std::uint32_t kWorkgroupSize = 256;  // the spine/downsweep digit scans assume 256 lanes
constexpr std::uint32_t kDigits = 256;
constexpr std::uint32_t kDefaultCountRows = 16;

struct SortPush {
    std::uint32_t shift;
    std::uint32_t elements_per_block;
    std::uint32_t write_keys;  // downsweep only
};

void compute_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

}  // namespace

splatcore::Status RadixSort::init(Context& context, std::uint32_t max_elements, std::uint32_t count_rows,
                                  RadixRanking ranking, RadixScatter scatter) {
    using splatcore::Status;
    m_context = &context;
    m_max_elements = max_elements;
    // The downsweep keeps one shared count row per possible subgroup (workgroup / minimum size).
    // A GPU whose subgroup size varies below 16 lanes (Intel: 8-32) runs the downsweep with a
    // required 16-lane subgroup (1.3 core subgroup size control); fixed sizes are used as they are.
    std::uint32_t min_subgroup = context.min_subgroup_size();
    std::uint32_t required_subgroup = 0;
    if (min_subgroup < 16 && context.max_subgroup_size() >= 16 && context.compute_subgroup_size_control()) {
        required_subgroup = 16;
        min_subgroup = 16;
    }
    if (min_subgroup < 16 || kWorkgroupSize % min_subgroup != 0) {
        return Status::failure("radix sort needs a minimum subgroup size of >= 16 lanes");
    }
    // Default 16 rows (enough for any >= 16-lane subgroup): on the Adreno 750 12-16 rows (14-18 KB
    // of shared memory) sort fastest; 4 rows run 25-30 % slower (more resident workgroups, more
    // open scatter streams), 20-28 rows 15-20 % slower (sweep in docs/sorting.md section 7).
    if (count_rows == 0) count_rows = kDefaultCountRows;
    // 28 rows + the two 256-entry arrays = 30 KB, under Adreno's 32 KB per-workgroup limit. The
    // mask ranking needs 8 rows of bitmasks; the rest is padding.
    const std::uint32_t min_rows = ranking == RadixRanking::mask ? 8 : kWorkgroupSize / min_subgroup;
    if (count_rows < min_rows || count_rows > 28) {
        return Status::failure("radix sort count rows out of range for this device");
    }
    // The staged scatter keeps its count rows inside the 4096-entry staging tile.
    if (scatter == RadixScatter::staged &&
        (ranking != RadixRanking::ballot || count_rows * kDigits > kSortElementsPerBlock)) {
        return Status::failure("staged radix scatter needs ballot ranking and at most 16 count rows");
    }
    m_max_blocks = (max_elements + kSortElementsPerBlock - 1) / kSortElementsPerBlock;

    const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    Status s = m_histograms.init(context.allocator(),
                                 VkDeviceSize{256} * m_max_blocks * sizeof(std::uint32_t), storage);
    if (s.ok()) {
        s = m_args.init(context.allocator(), 2 * sizeof(std::uint32_t),
                        storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }
    if (s.ok()) {
        s = m_digit_totals.init(context.allocator(), VkDeviceSize{kDigits} * sizeof(std::uint32_t), storage);
    }
    if (s.ok()) {
        s = m_keys_tmp.init(context.allocator(), VkDeviceSize{max_elements} * sizeof(std::uint32_t), storage);
    }
    if (s.ok()) {
        s = m_values_tmp.init(context.allocator(), VkDeviceSize{max_elements} * sizeof(std::uint32_t),
                              storage);
    }
    if (!s.ok()) return s;

    const VkDescriptorSetLayoutBinding upsweep_bindings[] = {storage_binding(0, VK_SHADER_STAGE_COMPUTE_BIT),
                                                             storage_binding(1, VK_SHADER_STAGE_COMPUTE_BIT),
                                                             storage_binding(2, VK_SHADER_STAGE_COMPUTE_BIT)};
    const VkDescriptorSetLayoutBinding spine_bindings[] = {storage_binding(0, VK_SHADER_STAGE_COMPUTE_BIT),
                                                           storage_binding(1, VK_SHADER_STAGE_COMPUTE_BIT),
                                                           storage_binding(2, VK_SHADER_STAGE_COMPUTE_BIT)};
    const VkDescriptorSetLayoutBinding downsweep_bindings[] = {
        storage_binding(0, VK_SHADER_STAGE_COMPUTE_BIT), storage_binding(1, VK_SHADER_STAGE_COMPUTE_BIT),
        storage_binding(2, VK_SHADER_STAGE_COMPUTE_BIT), storage_binding(3, VK_SHADER_STAGE_COMPUTE_BIT),
        storage_binding(4, VK_SHADER_STAGE_COMPUTE_BIT), storage_binding(5, VK_SHADER_STAGE_COMPUTE_BIT),
        storage_binding(6, VK_SHADER_STAGE_COMPUTE_BIT)};
    s = m_upsweep_layout.init(context.device(), upsweep_bindings);
    if (s.ok()) s = m_spine_layout.init(context.device(), spine_bindings);
    if (s.ok()) s = m_downsweep_layout.init(context.device(), downsweep_bindings);
    if (!s.ok()) return s;

    const VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * 3 + 3 + 2 * 7}};
    s = m_pool.init(context.device(), pool_sizes, 5);
    if (!s.ok()) return s;
    for (int dir = 0; dir < 2; ++dir) {
        splatcore::Result<VkDescriptorSet> up = m_pool.allocate(m_upsweep_layout.handle());
        splatcore::Result<VkDescriptorSet> down = m_pool.allocate(m_downsweep_layout.handle());
        if (!up || !down) return Status::failure("sort descriptor allocation failed");
        m_upsweep_sets[dir] = up.value();
        m_downsweep_sets[dir] = down.value();
    }
    splatcore::Result<VkDescriptorSet> spine = m_pool.allocate(m_spine_layout.handle());
    if (!spine) return Status::failure("sort descriptor allocation failed");
    m_spine_set = spine.value();
    write_buffer_descriptor(context.device(), m_spine_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            m_histograms.handle());
    write_buffer_descriptor(context.device(), m_spine_set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            m_args.handle());
    write_buffer_descriptor(context.device(), m_spine_set, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            m_digit_totals.handle());

    ComputePipelineCreateInfo info;
    info.push_constant_bytes = sizeof(SortPush);
    info.workgroup_size = kWorkgroupSize;
    const VkDescriptorSetLayout up_layouts[] = {m_upsweep_layout.handle()};
    info.spirv = std::span(k_radix_upsweep_comp_spv);
    info.set_layouts = up_layouts;
    s = m_upsweep.init_compute(context.device(), info);
    if (!s.ok()) return s;
    const VkDescriptorSetLayout spine_layouts[] = {m_spine_layout.handle()};
    info.spirv = std::span(k_radix_spine_comp_spv);
    info.set_layouts = spine_layouts;
    info.push_constant_bytes = 0;
    s = m_spine.init_compute(context.device(), info);
    if (!s.ok()) return s;
    const VkDescriptorSetLayout down_layouts[] = {m_downsweep_layout.handle()};
    info.spirv =
        scatter == RadixScatter::staged ? std::span<const std::uint32_t>(k_radix_downsweep_staged_comp_spv)
        : ranking == RadixRanking::mask ? std::span<const std::uint32_t>(k_radix_downsweep_mask_comp_spv)
                                        : std::span<const std::uint32_t>(k_radix_downsweep_comp_spv);
    info.set_layouts = down_layouts;
    info.push_constant_bytes = sizeof(SortPush);
    const std::uint32_t downsweep_constants[] = {ranking == RadixRanking::mask ? count_rows - 8 : count_rows};
    info.constants = downsweep_constants;
    info.required_subgroup_size = required_subgroup;
    return m_downsweep.init_compute(context.device(), info);
}

void RadixSort::bind_downsweep(VkDescriptorSet set, VkBuffer keys_in, VkBuffer values_in, VkBuffer keys_out,
                               VkBuffer values_out) {
    VkDevice device = m_context->device();
    write_buffer_descriptor(device, set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, keys_in);
    write_buffer_descriptor(device, set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, values_in);
    write_buffer_descriptor(device, set, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, keys_out);
    write_buffer_descriptor(device, set, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, values_out);
    write_buffer_descriptor(device, set, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, m_histograms.handle());
    write_buffer_descriptor(device, set, 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, m_args.handle());
    write_buffer_descriptor(device, set, 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, m_digit_totals.handle());
}

void RadixSort::record(VkCommandBuffer cmd, Buffer& keys, Buffer& values, std::uint32_t count, int key_bits,
                       bool keep_sorted_keys) {
    if (count == 0 || count > m_max_elements) return;
    const std::uint32_t blocks = (count + kSortElementsPerBlock - 1) / kSortElementsPerBlock;
    const std::uint32_t args[2] = {count, blocks};
    vkCmdUpdateBuffer(cmd, m_args.handle(), 0, sizeof(args), args);
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
    record_passes(cmd, keys, values, key_bits, keep_sorted_keys, blocks, nullptr);
}

void RadixSort::record_indirect(VkCommandBuffer cmd, Buffer& keys, Buffer& values, Buffer& dispatch,
                                int key_bits, bool keep_sorted_keys) {
    record_passes(cmd, keys, values, key_bits, keep_sorted_keys, 0, &dispatch);
}

void RadixSort::record_passes(VkCommandBuffer cmd, Buffer& keys, Buffer& values, int key_bits,
                              bool keep_sorted_keys, std::uint32_t direct_blocks, Buffer* dispatch) {
    // (Re)bind descriptor sets when the target buffers change. The caller must not have the sort
    // in flight while switching buffers (descriptor updates require the sets to be unused).
    if (m_bound_keys != keys.id() || m_bound_values != values.id()) {
        VkDevice device = m_context->device();
        write_buffer_descriptor(device, m_upsweep_sets[0], 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                keys.handle());
        write_buffer_descriptor(device, m_upsweep_sets[0], 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_histograms.handle());
        write_buffer_descriptor(device, m_upsweep_sets[0], 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_args.handle());
        write_buffer_descriptor(device, m_upsweep_sets[1], 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_keys_tmp.handle());
        write_buffer_descriptor(device, m_upsweep_sets[1], 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_histograms.handle());
        write_buffer_descriptor(device, m_upsweep_sets[1], 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                m_args.handle());
        bind_downsweep(m_downsweep_sets[0], keys.handle(), values.handle(), m_keys_tmp.handle(),
                       m_values_tmp.handle());
        bind_downsweep(m_downsweep_sets[1], m_keys_tmp.handle(), m_values_tmp.handle(), keys.handle(),
                       values.handle());
        m_bound_keys = keys.id();
        m_bound_values = values.id();
    }

    const int passes = key_bits == 16 ? 2 : 4;
    for (int pass = 0; pass < passes; ++pass) {
        const int dir = pass & 1;
        const bool last = pass + 1 == passes;
        const SortPush push{static_cast<std::uint32_t>(8 * pass), kSortElementsPerBlock,
                            !last || keep_sorted_keys ? 1u : 0u};

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_upsweep.handle());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_upsweep.layout(), 0, 1,
                                &m_upsweep_sets[dir], 0, nullptr);
        vkCmdPushConstants(cmd, m_upsweep.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        if (dispatch != nullptr) {
            vkCmdDispatchIndirect(cmd, dispatch->handle(), 0);
        } else {
            vkCmdDispatch(cmd, direct_blocks, 1, 1);
        }
        compute_barrier(cmd);
        if (m_stage_hook != nullptr) m_stage_hook(m_stage_user, cmd, "upsweep");

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_spine.handle());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_spine.layout(), 0, 1, &m_spine_set, 0,
                                nullptr);
        vkCmdDispatch(cmd, kDigits, 1, 1);  // one workgroup per digit row
        compute_barrier(cmd);
        if (m_stage_hook != nullptr) m_stage_hook(m_stage_user, cmd, "spine");

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_downsweep.handle());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_downsweep.layout(), 0, 1,
                                &m_downsweep_sets[dir], 0, nullptr);
        vkCmdPushConstants(cmd, m_downsweep.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        if (dispatch != nullptr) {
            vkCmdDispatchIndirect(cmd, dispatch->handle(), 0);
        } else {
            vkCmdDispatch(cmd, direct_blocks, 1, 1);
        }
        compute_barrier(cmd);
        if (m_stage_hook != nullptr) m_stage_hook(m_stage_user, cmd, "downsweep");
    }
}

void RadixSort::destroy() {
    if (m_context == nullptr) return;
    m_context->wait_idle();
    m_upsweep.destroy();
    m_spine.destroy();
    m_downsweep.destroy();
    m_pool.destroy();
    m_upsweep_layout.destroy();
    m_spine_layout.destroy();
    m_downsweep_layout.destroy();
    m_histograms.destroy();
    m_digit_totals.destroy();
    m_args.destroy();
    m_keys_tmp.destroy();
    m_values_tmp.destroy();
    m_bound_keys = 0;
    m_bound_values = 0;
    m_context = nullptr;
}

}  // namespace splatrender
