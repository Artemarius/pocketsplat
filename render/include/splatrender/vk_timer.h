#pragma once

#include <volk.h>

#include <cstdint>

#include "splatcore/result.h"
#include "splatrender/vk_context.h"
#include "splatrender/vk_frame.h"

namespace splatrender {

inline constexpr std::uint32_t kTimerStamps = 6;  // frame start + up to 5 pass boundaries

// GPU timing via timestamp queries, double-buffered to match the frame ring. begin_frame stamps
// the frame start; each stamp() marks the end of a pass; end_frame stamps the frame end. The
// resolved segment times of the previous submission on the same slot are exposed via
// segment_ms(i) (segment i = time between stamp i and stamp i+1).
class GpuTimer {
public:
    GpuTimer() = default;
    ~GpuTimer() { destroy(); }
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;

    splatcore::Status init(Context& context);
    void destroy();

    // Call with the slot's fresh command buffer: resolves the slot's previous results (its
    // submission has completed - FrameRing::begin waited for it), then resets and stamps "start".
    void begin_frame(VkCommandBuffer cmd, std::uint32_t slot);
    void stamp(VkCommandBuffer cmd, std::uint32_t slot);      // marks a pass boundary
    void end_frame(VkCommandBuffer cmd, std::uint32_t slot);  // stamps "end"

    double last_gpu_ms() const { return m_last_gpu_ms; }  // whole frame, most recently resolved
    std::size_t segment_count() const { return m_segment_count; }
    double segment_ms(std::size_t i) const { return i < m_segment_count ? m_segments[i] : 0.0; }

private:
    Context* m_context = nullptr;
    VkQueryPool m_pools[kFramesInFlight] = {};
    std::uint32_t m_stamp_count[kFramesInFlight] = {};  // stamps recorded this submission
    double m_segments[kTimerStamps - 1] = {};
    std::size_t m_segment_count = 0;
    double m_last_gpu_ms = 0.0;
};

}  // namespace splatrender
