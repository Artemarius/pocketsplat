#pragma once

#include <volk.h>

#include <cstdint>
#include <span>

#include "splatcore/result.h"

namespace splatrender {

// RAII descriptor set layout built from a binding list.
class DescriptorSetLayout {
public:
    DescriptorSetLayout() = default;
    ~DescriptorSetLayout() { destroy(); }
    DescriptorSetLayout(const DescriptorSetLayout&) = delete;
    DescriptorSetLayout& operator=(const DescriptorSetLayout&) = delete;

    splatcore::Status init(VkDevice device, std::span<const VkDescriptorSetLayoutBinding> bindings);
    void destroy();
    VkDescriptorSetLayout handle() const { return m_layout; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
};

// Fixed-size descriptor pool; sets live as long as the pool (no per-set free).
class DescriptorPool {
public:
    DescriptorPool() = default;
    ~DescriptorPool() { destroy(); }
    DescriptorPool(const DescriptorPool&) = delete;
    DescriptorPool& operator=(const DescriptorPool&) = delete;

    splatcore::Status init(VkDevice device, std::span<const VkDescriptorPoolSize> sizes,
                           std::uint32_t max_sets);
    void destroy();
    splatcore::Result<VkDescriptorSet> allocate(VkDescriptorSetLayout layout);

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
};

// Points `binding` of `set` at a whole buffer (VK_WHOLE_SIZE).
void write_buffer_descriptor(VkDevice device, VkDescriptorSet set, std::uint32_t binding,
                             VkDescriptorType type, VkBuffer buffer);

// Convenience for the common binding shapes.
VkDescriptorSetLayoutBinding storage_binding(std::uint32_t binding, VkShaderStageFlags stages);
VkDescriptorSetLayoutBinding uniform_binding(std::uint32_t binding, VkShaderStageFlags stages);

}  // namespace splatrender
