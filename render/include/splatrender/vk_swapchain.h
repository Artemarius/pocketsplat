#pragma once

#include <volk.h>

#include <cstdint>
#include <vector>

#include "splatcore/result.h"
#include "splatrender/vk_context.h"

namespace splatrender {

// Swapchain plus per-image "render finished" binary semaphores (per image, not per frame, so a
// semaphore is never re-signaled while an earlier present still waits on it).
class Swapchain {
public:
    Swapchain() = default;
    ~Swapchain() { destroy(); }
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    splatcore::Status init(Context& context, VkSurfaceKHR surface, std::uint32_t width, std::uint32_t height,
                           bool vsync = true);  // vsync=false requests IMMEDIATE, then MAILBOX
    splatcore::Status recreate(std::uint32_t width, std::uint32_t height);  // waits device idle
    void destroy();

    // Takes effect on the next recreate() (benchmarks toggle vsync on a live swapchain).
    void set_vsync(bool vsync) { m_vsync = vsync; }

    VkFormat format() const { return m_format; }
    VkExtent2D extent() const { return m_extent; }
    VkPresentModeKHR present_mode() const { return m_present_mode; }  // the mode actually in use
    // The rotation the presented images must already carry (pre-rotation): IDENTITY on desktop; a
    // quarter or half turn on a rotated phone, where extent() is in the panel's native orientation
    // and the host renders the turned view (roll the camera, map touch coordinates). Present
    // returns SUBOPTIMAL once the display turns again; recreate() picks up the new transform.
    VkSurfaceTransformFlagBitsKHR pre_transform() const { return m_pre_transform; }
    std::uint32_t image_count() const { return static_cast<std::uint32_t>(m_images.size()); }
    VkImage image(std::uint32_t i) const { return m_images[i]; }
    VkImageView view(std::uint32_t i) const { return m_views[i]; }
    VkSemaphore render_finished(std::uint32_t i) const { return m_render_finished[i]; }

    VkResult acquire(VkSemaphore signal, std::uint32_t* image_index);
    VkResult present(VkQueue queue, std::uint32_t image_index);  // waits render_finished(index)

private:
    void destroy_resources();

    Context* m_context = nullptr;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;  // owned by the host window, not destroyed here
    bool m_vsync = true;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkExtent2D m_extent{};
    VkPresentModeKHR m_present_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkSurfaceTransformFlagBitsKHR m_pre_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_views;
    std::vector<VkSemaphore> m_render_finished;
};

}  // namespace splatrender
