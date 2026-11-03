#include "splatrender/vk_frame.h"

namespace splatrender {

splatcore::Status FrameRing::init(Context& context) {
    using splatcore::Status;
    m_context = &context;

    VkSemaphoreTypeCreateInfo type_info{};
    type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type_info.initialValue = 0;
    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphore_info.pNext = &type_info;
    if (vkCreateSemaphore(context.device(), &semaphore_info, nullptr, &m_timeline) != VK_SUCCESS) {
        return Status::failure("timeline semaphore creation failed");
    }

    for (Frame& frame : m_frames) {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = context.queue_family();
        if (vkCreateCommandPool(context.device(), &pool_info, nullptr, &frame.pool) != VK_SUCCESS) {
            return Status::failure("command pool creation failed");
        }
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = frame.pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(context.device(), &alloc_info, &frame.cmd) != VK_SUCCESS) {
            return Status::failure("command buffer allocation failed");
        }
        VkSemaphoreCreateInfo binary_info{};
        binary_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(context.device(), &binary_info, nullptr, &frame.image_available) !=
            VK_SUCCESS) {
            return Status::failure("semaphore creation failed");
        }
    }
    return Status::success();
}

VkCommandBuffer FrameRing::begin() {
    Frame& frame = m_frames[m_slot];
    if (frame.submitted_value != 0) {
        VkSemaphoreWaitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &m_timeline;
        wait.pValues = &frame.submitted_value;
        if (vkWaitSemaphores(m_context->device(), &wait, UINT64_MAX) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
    }
    if (vkResetCommandPool(m_context->device(), frame.pool, 0) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(frame.cmd, &begin_info) != VK_SUCCESS) return VK_NULL_HANDLE;
    m_begun = true;
    return frame.cmd;
}

splatcore::Status FrameRing::submit(VkSemaphore render_finished) {
    using splatcore::Status;
    Frame& frame = m_frames[m_slot];
    if (!m_begun) return Status::failure("submit without begin");
    if (vkEndCommandBuffer(frame.cmd) != VK_SUCCESS) return Status::failure("vkEndCommandBuffer failed");
    m_begun = false;

    VkSemaphoreSubmitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait.semaphore = frame.image_available;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signals[2]{};
    signals[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[0].semaphore = render_finished;
    signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[1].semaphore = m_timeline;
    signals[1].value = m_next_value;
    signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = frame.cmd;

    const bool offscreen = render_finished == VK_NULL_HANDLE;
    VkSubmitInfo2 submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit_info.waitSemaphoreInfoCount = offscreen ? 0 : 1;
    submit_info.pWaitSemaphoreInfos = offscreen ? nullptr : &wait;
    submit_info.commandBufferInfoCount = 1;
    submit_info.pCommandBufferInfos = &cmd_info;
    submit_info.signalSemaphoreInfoCount = offscreen ? 1 : 2;
    submit_info.pSignalSemaphoreInfos = offscreen ? &signals[1] : signals;
    if (vkQueueSubmit2(m_context->queue(), 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
        return Status::failure("vkQueueSubmit2 failed");
    }
    frame.submitted_value = m_next_value++;
    m_slot = (m_slot + 1) % kFramesInFlight;
    return Status::success();
}

void FrameRing::destroy() {
    if (m_context == nullptr) return;
    m_context->wait_idle();
    for (Frame& frame : m_frames) {
        if (frame.image_available != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_context->device(), frame.image_available, nullptr);
        }
        if (frame.pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_context->device(), frame.pool, nullptr);
        }
        frame = Frame{};
    }
    if (m_timeline != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_context->device(), m_timeline, nullptr);
        m_timeline = VK_NULL_HANDLE;
    }
    m_context = nullptr;
}

}  // namespace splatrender
