#include "splatrender/vk_buffer.h"

#include <atomic>
#include <cstring>

#include "splatrender/vk_context.h"

namespace splatrender {

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        destroy();
        m_allocator = other.m_allocator;
        m_buffer = other.m_buffer;
        m_allocation = other.m_allocation;
        m_size = other.m_size;
        m_mapped = other.m_mapped;
        m_id = other.m_id;
        other.m_allocator = VK_NULL_HANDLE;
        other.m_buffer = VK_NULL_HANDLE;
        other.m_allocation = VK_NULL_HANDLE;
        other.m_size = 0;
        other.m_mapped = nullptr;
        other.m_id = 0;
    }
    return *this;
}

splatcore::Status Buffer::init(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                               VmaAllocationCreateFlags alloc_flags) {
    using splatcore::Status;
    m_allocator = allocator;
    m_size = size;

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_info.flags = alloc_flags;

    VmaAllocationInfo result_info{};
    if (vmaCreateBuffer(allocator, &buffer_info, &alloc_info, &m_buffer, &m_allocation, &result_info) !=
        VK_SUCCESS) {
        return Status::failure("buffer allocation failed (" + std::to_string(size) + " bytes)");
    }
    m_mapped = result_info.pMappedData;
    static std::atomic<std::uint64_t> next_id{1};
    m_id = next_id.fetch_add(1, std::memory_order_relaxed);
    return Status::success();
}

void Buffer::invalidate() {
    if (m_allocation != VK_NULL_HANDLE) vmaInvalidateAllocation(m_allocator, m_allocation, 0, VK_WHOLE_SIZE);
}

void Buffer::destroy() {
    if (m_buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
        m_buffer = VK_NULL_HANDLE;
        m_allocation = VK_NULL_HANDLE;
        m_mapped = nullptr;
        m_size = 0;
        m_id = 0;
    }
}

splatcore::Status create_device_buffer(Context& context, const void* data, VkDeviceSize size,
                                       VkBufferUsageFlags usage, Buffer& out) {
    using splatcore::Status;
    if (Status s = out.init(context.allocator(), size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT); !s.ok()) {
        return s;
    }
    Buffer staging;
    if (Status s = staging.init(
            context.allocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        !s.ok()) {
        return s;
    }
    std::memcpy(staging.mapped(), data, static_cast<std::size_t>(size));

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = context.queue_family();
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(context.device(), &pool_info, nullptr, &pool) != VK_SUCCESS) {
        return Status::failure("staging command pool creation failed");
    }
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(context.device(), &alloc_info, &cmd);
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);
    VkBufferCopy region{0, 0, size};
    vkCmdCopyBuffer(cmd, staging.handle(), out.handle(), 1, &region);
    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit_info.commandBufferInfoCount = 1;
    submit_info.pCommandBufferInfos = &cmd_info;
    const VkResult result = vkQueueSubmit2(context.queue(), 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(context.queue());
    vkDestroyCommandPool(context.device(), pool, nullptr);
    if (result != VK_SUCCESS) return Status::failure("staging copy submit failed");
    return Status::success();
}

}  // namespace splatrender
