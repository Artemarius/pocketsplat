#pragma once

#include <volk.h>

#include <cstdint>

#include "splatcore/result.h"
#include "splatrender/vk_context.h"

namespace splatrender {

inline constexpr std::uint32_t kFramesInFlight = 2;

// Frame pacing: kFramesInFlight command pools/buffers cycled per frame, one timeline semaphore
// pacing slot reuse, one binary image-available semaphore per slot for swapchain acquire.
// Flow per frame: begin() -> Swapchain::acquire(image_available()) -> record -> submit(render
// finished semaphore) -> Swapchain::present().
class FrameRing {
public:
    FrameRing() = default;
    ~FrameRing() { destroy(); }
    FrameRing(const FrameRing&) = delete;
    FrameRing& operator=(const FrameRing&) = delete;

    splatcore::Status init(Context& context);
    void destroy();

    // Waits until this slot's previous submission finished, then resets and begins its command
    // buffer. Returns VK_NULL_HANDLE on error.
    VkCommandBuffer begin();
    VkSemaphore image_available() const { return m_frames[m_slot].image_available; }
    std::uint32_t slot() const { return m_slot; }

    // Ends the command buffer and submits: waits image_available at color-attachment-output,
    // signals render_finished (for present) and the timeline (for slot reuse). VK_NULL_HANDLE =
    // offscreen frame: nothing was acquired and nothing is presented, only the timeline signals.
    splatcore::Status submit(VkSemaphore render_finished);

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        std::uint64_t submitted_value = 0;
    };

    Context* m_context = nullptr;
    VkSemaphore m_timeline = VK_NULL_HANDLE;
    Frame m_frames[kFramesInFlight];
    std::uint32_t m_slot = 0;
    std::uint64_t m_next_value = 1;
    bool m_begun = false;
};

}  // namespace splatrender
