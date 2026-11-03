#include "splatrender/vk_descriptors.h"

namespace splatrender {

splatcore::Status DescriptorSetLayout::init(VkDevice device,
                                            std::span<const VkDescriptorSetLayoutBinding> bindings) {
    m_device = device;
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &info, nullptr, &m_layout) != VK_SUCCESS) {
        return splatcore::Status::failure("descriptor set layout creation failed");
    }
    return splatcore::Status::success();
}

void DescriptorSetLayout::destroy() {
    if (m_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }
    m_device = VK_NULL_HANDLE;
}

splatcore::Status DescriptorPool::init(VkDevice device, std::span<const VkDescriptorPoolSize> sizes,
                                       std::uint32_t max_sets) {
    m_device = device;
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = max_sets;
    info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    info.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(device, &info, nullptr, &m_pool) != VK_SUCCESS) {
        return splatcore::Status::failure("descriptor pool creation failed");
    }
    return splatcore::Status::success();
}

void DescriptorPool::destroy() {
    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
    m_device = VK_NULL_HANDLE;
}

splatcore::Result<VkDescriptorSet> DescriptorPool::allocate(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = m_pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_device, &info, &set) != VK_SUCCESS) {
        return splatcore::Result<VkDescriptorSet>::failure("descriptor set allocation failed");
    }
    return splatcore::Result<VkDescriptorSet>::success(set);
}

void write_buffer_descriptor(VkDevice device, VkDescriptorSet set, std::uint32_t binding,
                             VkDescriptorType type, VkBuffer buffer) {
    VkDescriptorBufferInfo buffer_info{buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

VkDescriptorSetLayoutBinding storage_binding(std::uint32_t binding, VkShaderStageFlags stages) {
    return {binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr};
}

VkDescriptorSetLayoutBinding uniform_binding(std::uint32_t binding, VkShaderStageFlags stages) {
    return {binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr};
}

}  // namespace splatrender
