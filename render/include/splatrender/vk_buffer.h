#pragma once

#include <vk_mem_alloc.h>
#include <volk.h>

#include <cstdint>

#include "splatcore/result.h"

namespace splatrender {

class Context;

// VMA-backed buffer. Pass VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
// VMA_ALLOCATION_CREATE_MAPPED_BIT in `alloc_flags` for persistently mapped upload buffers;
// device-local buffers are filled via staging copies (renderer-side, later).
class Buffer {
public:
    Buffer() = default;
    ~Buffer() { destroy(); }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept { *this = static_cast<Buffer&&>(other); }
    Buffer& operator=(Buffer&& other) noexcept;

    splatcore::Status init(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                           VmaAllocationCreateFlags alloc_flags = 0);
    void destroy();

    VkBuffer handle() const { return m_buffer; }
    // Unique per allocation for the process lifetime (0 = none). Drivers recycle VkBuffer handle
    // values after vkDestroyBuffer, so caches keyed on handle() can silently go stale.
    std::uint64_t id() const { return m_id; }
    VkDeviceSize size() const { return m_size; }
    void* mapped() const { return m_mapped; }  // non-null only with the MAPPED create flag
    // Makes device writes visible to host reads of mapped() (no-op on host-coherent memory).
    void invalidate();

private:
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    VkDeviceSize m_size = 0;
    void* m_mapped = nullptr;
    std::uint64_t m_id = 0;
};

// Creates a device-local buffer with `usage | TRANSFER_DST` and fills it from `data` through a
// one-shot staging copy (blocks on the queue; load-time only, never per frame).
splatcore::Status create_device_buffer(Context& context, const void* data, VkDeviceSize size,
                                       VkBufferUsageFlags usage, Buffer& out);

}  // namespace splatrender
