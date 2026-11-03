#include "splatrender/vk_swapchain.h"

#include <algorithm>
#include <utility>

namespace splatrender {
namespace {

bool is_rotated_quarter(VkSurfaceTransformFlagBitsKHR transform) {
    return transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
           transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
}

}  // namespace

splatcore::Status Swapchain::init(Context& context, VkSurfaceKHR surface, std::uint32_t width,
                                  std::uint32_t height, bool vsync) {
    m_context = &context;
    m_surface = surface;
    m_vsync = vsync;
    return recreate(width, height);
}

// Built directly on the surface queries rather than vk-bootstrap's SwapchainBuilder: the builder
// always takes the surface's currentExtent as the image size, which is wrong for pre-rotation
// (below), and it offers no override.
splatcore::Status Swapchain::recreate(std::uint32_t width, std::uint32_t height) {
    using splatcore::Status;
    m_context->wait_idle();
    VkPhysicalDevice physical = m_context->physical_device();

    VkBool32 present_supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physical, m_context->queue_family(), m_surface, &present_supported);
    if (present_supported != VK_TRUE)
        return Status::failure("the graphics queue cannot present to this surface");

    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, m_surface, &caps) != VK_SUCCESS) {
        return Status::failure("surface capability query failed");
    }

    // Pre-rotation (Android developer guide "Handle device orientation with Vulkan pre-rotation";
    // Vulkan spec, VkSurfaceTransformFlagBitsKHR). With the phone turned, currentTransform says how
    // the image must be rotated relative to the panel's native orientation. We render that rotation
    // ourselves (the host rolls the camera, see pre_transform()) so the compositor never has to
    // rotate a full-screen layer, and the images are therefore in the panel's native orientation:
    // Android reports currentExtent in the current orientation, so a quarter turn swaps it. A
    // transform the surface does not support falls back to identity (the compositor rotates).
    VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
    if ((caps.supportedTransforms & transform) == 0) transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkExtent2D extent{width, height};
    if (caps.currentExtent.width != UINT32_MAX) extent = caps.currentExtent;
    if (is_rotated_quarter(transform)) std::swap(extent.width, extent.height);
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    if (extent.width == 0 || extent.height == 0) return Status::failure("surface has zero size");

    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, m_surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, m_surface, &count, formats.data());
    if (formats.empty()) return Status::failure("surface reports no formats");
    VkSurfaceFormatKHR format = formats[0];
    for (const VkSurfaceFormatKHR& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            format = f;
            break;
        }
    }

    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, m_surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, m_surface, &count, modes.data());
    // FIFO (vsync) for honest interactive pacing; IMMEDIATE (or MAILBOX where IMMEDIATE is absent -
    // Android) for benchmarks so present never paces the GPU. FIFO is always supported.
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!m_vsync) {
        for (VkPresentModeKHR wanted : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR}) {
            if (std::find(modes.begin(), modes.end(), wanted) != modes.end()) {
                present_mode = wanted;
                break;
            }
        }
    }

    // minImageCount + 1 (what vk-bootstrap did, and what every recorded benchmark ran with).
    std::uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) image_count = std::min(image_count, caps.maxImageCount);

        // vk-bootstrap's defaults, kept: INHERIT on Android (the window decides), OPAQUE elsewhere.
#if defined(__ANDROID__)
    VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
#else
    VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
#endif
    if ((caps.supportedCompositeAlpha & composite) == 0) {
        return Status::failure("surface does not support the composite alpha mode");
    }

    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if ((caps.supportedUsageFlags & usage) != usage)
        return Status::failure("surface lacks color+transfer-src usage");

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = m_surface;
    info.minImageCount = image_count;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = usage;  // transfer-src: screenshots
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = transform;
    info.compositeAlpha = composite;
    info.presentMode = present_mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = m_swapchain;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(m_context->device(), &info, nullptr, &swapchain) != VK_SUCCESS) {
        return Status::failure("swapchain creation failed");
    }

    const VkSwapchainKHR old_swapchain = m_swapchain;
    destroy_resources();  // views/semaphores of the old chain; images belong to the swapchain
    if (old_swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(m_context->device(), old_swapchain, nullptr);

    m_swapchain = swapchain;
    m_format = format.format;
    m_extent = extent;
    m_present_mode = present_mode;
    m_pre_transform = transform;
    vkGetSwapchainImagesKHR(m_context->device(), m_swapchain, &count, nullptr);
    m_images.resize(count);
    vkGetSwapchainImagesKHR(m_context->device(), m_swapchain, &count, m_images.data());

    m_views.resize(m_images.size(), VK_NULL_HANDLE);
    for (std::size_t i = 0; i < m_images.size(); ++i) {
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = m_images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = m_format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(m_context->device(), &view, nullptr, &m_views[i]) != VK_SUCCESS) {
            return Status::failure("swapchain image view creation failed");
        }
    }

    m_render_finished.resize(m_images.size(), VK_NULL_HANDLE);
    for (VkSemaphore& semaphore : m_render_finished) {
        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(m_context->device(), &semaphore_info, nullptr, &semaphore) != VK_SUCCESS) {
            return Status::failure("semaphore creation failed");
        }
    }
    return Status::success();
}

VkResult Swapchain::acquire(VkSemaphore signal, std::uint32_t* image_index) {
    return vkAcquireNextImageKHR(m_context->device(), m_swapchain, UINT64_MAX, signal, VK_NULL_HANDLE,
                                 image_index);
}

VkResult Swapchain::present(VkQueue queue, std::uint32_t image_index) {
    VkSemaphore wait = m_render_finished[image_index];
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &wait;
    info.swapchainCount = 1;
    info.pSwapchains = &m_swapchain;
    info.pImageIndices = &image_index;
    return vkQueuePresentKHR(queue, &info);
}

void Swapchain::destroy_resources() {
    for (VkImageView view : m_views) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(m_context->device(), view, nullptr);
    }
    m_views.clear();
    for (VkSemaphore semaphore : m_render_finished) {
        if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(m_context->device(), semaphore, nullptr);
    }
    m_render_finished.clear();
    m_images.clear();
}

void Swapchain::destroy() {
    if (m_context == nullptr) return;
    m_context->wait_idle();
    destroy_resources();
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_context->device(), m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
    m_context = nullptr;
}

}  // namespace splatrender
