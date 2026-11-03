#include "splatrender/vk_timer.h"

namespace splatrender {

splatcore::Status GpuTimer::init(Context& context) {
    m_context = &context;
    for (VkQueryPool& pool : m_pools) {
        VkQueryPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = kTimerStamps;
        if (vkCreateQueryPool(context.device(), &info, nullptr, &pool) != VK_SUCCESS) {
            return splatcore::Status::failure("query pool creation failed");
        }
    }
    return splatcore::Status::success();
}

void GpuTimer::begin_frame(VkCommandBuffer cmd, std::uint32_t slot) {
    // The slot's previous submission has completed (FrameRing::begin waited on the timeline), so
    // its results are available without blocking.
    const std::uint32_t written = m_stamp_count[slot];
    if (written >= 2) {
        std::uint64_t stamps[kTimerStamps] = {};
        if (vkGetQueryPoolResults(m_context->device(), m_pools[slot], 0, written, sizeof(stamps), stamps,
                                  sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const double period_ms = static_cast<double>(m_context->timestamp_period_ns()) / 1e6;
            m_segment_count = written - 1;
            for (std::size_t i = 0; i < m_segment_count; ++i) {
                m_segments[i] = static_cast<double>(stamps[i + 1] - stamps[i]) * period_ms;
            }
            m_last_gpu_ms = static_cast<double>(stamps[written - 1] - stamps[0]) * period_ms;
        }
    }
    vkCmdResetQueryPool(cmd, m_pools[slot], 0, kTimerStamps);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, m_pools[slot], 0);
    m_stamp_count[slot] = 1;
}

void GpuTimer::stamp(VkCommandBuffer cmd, std::uint32_t slot) {
    if (m_stamp_count[slot] >= kTimerStamps - 1) return;  // keep the last query for end_frame
    // BOTTOM_OF_PIPE, not ALL_COMMANDS: the Adreno 830 driver writes ALL_COMMANDS (and TOP_OF_PIPE)
    // stamps before the preceding dispatch has finished, which moved whole passes into the next
    // segment; BOTTOM_OF_PIPE reads the same as ALL_COMMANDS on the 750 and the RTX 3060
    // (docs/sorting.md section 8).
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, m_pools[slot], m_stamp_count[slot]);
    ++m_stamp_count[slot];
}

void GpuTimer::end_frame(VkCommandBuffer cmd, std::uint32_t slot) {
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, m_pools[slot], m_stamp_count[slot]);
    ++m_stamp_count[slot];
}

void GpuTimer::destroy() {
    if (m_context == nullptr) return;
    m_context->wait_idle();
    for (VkQueryPool& pool : m_pools) {
        if (pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(m_context->device(), pool, nullptr);
            pool = VK_NULL_HANDLE;
        }
    }
    m_context = nullptr;
}

}  // namespace splatrender
