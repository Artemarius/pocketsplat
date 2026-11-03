#pragma once

#include <volk.h>

#include <cstdint>

#include "splatcore/result.h"
#include "splatrender/vk_buffer.h"
#include "splatrender/vk_context.h"
#include "splatrender/vk_descriptors.h"
#include "splatrender/vk_pipeline.h"

namespace splatrender {

inline constexpr std::uint32_t kSortElementsPerBlock = 4096;  // workgroup 256 x 16 per thread

// Reduce-then-scan LSD radix sort for uint32 key/value pairs (ADR-0004, docs/sorting.md §4):
// per 8-bit digit an upsweep (per-block histograms), a spine that scans each digit's row in its
// own workgroup, and a stable ballot-ranked downsweep. No inter-workgroup waits, no fixed
// subgroup size (a minimum of >= 16 lanes is checked at init - all target GPUs qualify). 32-bit
// keys = 4 digit passes; key_bits 16 = 2 passes for depth keys quantized by the preprocess.
// Downsweep ranking: subgroup ballot match (default) or shared-memory digit bitmasks.
enum class RadixRanking { ballot, mask };
// Downsweep scatter: each 256-key batch straight to its global slots (default), or the whole block
// ranked, staged in shared memory in digit order and written in contiguous runs (ballot only;
// about 2x faster on an RTX 3060, neutral on the Adreno 750 - docs/sorting.md section 8).
enum class RadixScatter { direct, staged };

class RadixSort {
public:
    RadixSort() = default;
    ~RadixSort() { destroy(); }
    RadixSort(const RadixSort&) = delete;
    RadixSort& operator=(const RadixSort&) = delete;

    // count_rows: the downsweep's shared footprint in 1 KB rows on top of 2 KB (0 = default 16).
    // ballot ranking: per-subgroup count rows, at least workgroup / minimum subgroup size; mask
    // ranking: 8 rows of digit bitmasks + padding (>= 8). Shared memory bounds how many workgroups a
    // core keeps resident, so the same value gives both variants the same occupancy (a tuning knob
    // - see docs/sorting.md section 7). The staged scatter always uses 16 KB (the staging tile) and
    // holds at most 16 count rows in it.
    splatcore::Status init(Context& context, std::uint32_t max_elements, std::uint32_t count_rows = 0,
                           RadixRanking ranking = RadixRanking::ballot,
                           RadixScatter scatter = RadixScatter::direct);
    void destroy();

    // Records the sort of keys[0..count) with matching values (splat indices). After execution
    // the sorted values are back in `values` (an even number of ping-pong passes), and the sorted
    // keys in `keys` when keep_sorted_keys is set - otherwise the last pass skips the key writes
    // and `keys` holds an intermediate pass. The caller owns the surrounding barriers for
    // whatever produced the keys / consumes the order.
    void record(VkCommandBuffer cmd, Buffer& keys, Buffer& values, std::uint32_t count, int key_bits = 32,
                bool keep_sorted_keys = true);

    // GPU-driven variant: the element count lives in args_buffer() {count, num_blocks} and the
    // dispatch size in `dispatch` (VkDispatchIndirectCommand), both written on the GPU (e.g. by
    // the prepare_indirect pass). The caller barriers those writes before calling.
    void record_indirect(VkCommandBuffer cmd, Buffer& keys, Buffer& values, Buffer& dispatch,
                         int key_bits = 32, bool keep_sorted_keys = true);

    Buffer& args_buffer() { return m_args; }
    std::uint32_t max_elements() const { return m_max_elements; }

    // Profiling hook (sortbench --profile): called after every sort dispatch and its trailing
    // barrier with the stage name ("upsweep", "spine", "downsweep"), so the caller can write a
    // timestamp there. Null = off (the renderer never sets it).
    using StageHook = void (*)(void* user, VkCommandBuffer cmd, const char* stage);
    void set_stage_hook(StageHook hook, void* user) {
        m_stage_hook = hook;
        m_stage_user = user;
    }

private:
    void bind_downsweep(VkDescriptorSet set, VkBuffer keys_in, VkBuffer values_in, VkBuffer keys_out,
                        VkBuffer values_out);

    Context* m_context = nullptr;
    std::uint32_t m_max_elements = 0;
    std::uint32_t m_max_blocks = 0;

    void record_passes(VkCommandBuffer cmd, Buffer& keys, Buffer& values, int key_bits, bool keep_sorted_keys,
                       std::uint32_t direct_blocks, Buffer* dispatch);

    Buffer m_histograms;
    Buffer m_digit_totals;  // per-digit element counts from the spine (256 x u32)
    Buffer m_args;          // {count, num_blocks}
    Buffer m_keys_tmp, m_values_tmp;
    DescriptorSetLayout m_upsweep_layout, m_spine_layout, m_downsweep_layout;
    DescriptorPool m_pool;
    // Two directions per resource pairing: [0] app buffers -> tmp, [1] tmp -> app buffers.
    VkDescriptorSet m_upsweep_sets[2] = {};
    VkDescriptorSet m_downsweep_sets[2] = {};
    VkDescriptorSet m_spine_set = VK_NULL_HANDLE;
    std::uint64_t m_bound_keys = 0;  // descriptor cache key: Buffer::id(), never the VkBuffer value
    std::uint64_t m_bound_values = 0;
    Pipeline m_upsweep, m_spine, m_downsweep;
    StageHook m_stage_hook = nullptr;
    void* m_stage_user = nullptr;
};

}  // namespace splatrender
