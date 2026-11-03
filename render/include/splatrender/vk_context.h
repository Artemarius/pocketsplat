#pragma once

#include <vk_mem_alloc.h>
#include <volk.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "splatcore/result.h"

namespace splatrender {

struct ContextCreateInfo {
    const char* app_name = "pocketsplat";
    bool enable_validation = false;
    // Enables VK_KHR_dynamic_rendering as a device extension even though the feature is 1.3 core.
    // Only needed because ImGui's Vulkan backend checks the extension string; our own code uses
    // the core entry points either way. Not set on Android.
    bool request_dynamic_rendering_extension = false;
    // Case-insensitive substring of the GPU name to use (e.g. "intel", "rtx"); null or empty =
    // vk-bootstrap's choice (a discrete GPU first). Init fails when no suitable GPU matches.
    const char* preferred_device = nullptr;
};

// Owns instance, device, graphics queue and the VMA allocator. Function pointers load via volk.
// Vulkan 1.3 core only: dynamic rendering, synchronization2, timeline semaphores; no vendor
// extensions (ADR-0001). Two-phase init; destruction is idempotent. Not copyable or movable.
class Context {
public:
    Context() = default;
    ~Context() { destroy(); }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    splatcore::Status init(const ContextCreateInfo& info);
    void destroy();

    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physical_device() const { return m_physical_device; }
    VkDevice device() const { return m_device; }
    VkQueue queue() const { return m_queue; }
    std::uint32_t queue_family() const { return m_queue_family; }
    VmaAllocator allocator() const { return m_allocator; }
    float timestamp_period_ns() const { return m_timestamp_period_ns; }
    const char* device_name() const { return m_device_name; }
    // Largest range a single storage-buffer descriptor may cover (128 MB on Adreno 750).
    std::uint32_t max_storage_buffer_range() const { return m_max_storage_buffer_range; }
    // Smallest subgroup any compute dispatch may run with (1.3 core; Adreno 64, NVIDIA 32, Intel 8).
    std::uint32_t min_subgroup_size() const { return m_min_subgroup_size; }
    std::uint32_t max_subgroup_size() const { return m_max_subgroup_size; }
    // True when a compute pipeline may require its subgroup size (1.3 core subgroup size control,
    // enabled on every device, and VK_SHADER_STAGE_COMPUTE_BIT in requiredSubgroupSizeStages):
    // ComputePipelineCreateInfo::required_subgroup_size then applies.
    bool compute_subgroup_size_control() const { return m_compute_subgroup_size_control; }

    // Validation errors/warnings counted by the debug messenger; the desktop host reports this at
    // exit so "validation clean" is a checkable claim, not a feeling.
    std::uint64_t validation_message_count() const { return m_validation_messages.load(); }
    void count_validation_message() { m_validation_messages.fetch_add(1); }

    void wait_idle() const;

    // Every GPU the instance reported, one "name (Vulkan a.b.c, type)" line each, filled during
    // init even when it fails, so a host can tell the user what was found.
    const std::string& devices_found() const { return m_devices_found; }

private:
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    std::uint32_t m_queue_family = 0;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    float m_timestamp_period_ns = 0.0f;
    std::uint32_t m_max_storage_buffer_range = 0;
    std::uint32_t m_min_subgroup_size = 0;
    std::uint32_t m_max_subgroup_size = 0;
    bool m_compute_subgroup_size_control = false;
    char m_device_name[256] = {};
    std::string m_devices_found;
    std::atomic<std::uint64_t> m_validation_messages{0};
};

}  // namespace splatrender
