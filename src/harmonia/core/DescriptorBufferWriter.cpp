#include "harmonia/core/DescriptorBufferWriter.hpp"

#include "harmonia/core/Logger.hpp"

namespace harmonia {

bool DescriptorBufferWriter::init(const DeviceContext& ctx, VkDescriptorSetLayout setLayout,
                                  std::uint32_t numBindings, const char* name) {
    // Query descriptor buffer properties.
    VkPhysicalDeviceDescriptorBufferPropertiesEXT props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &props;
    vkGetPhysicalDeviceProperties2(ctx.physicalDevice, &props2);

    m_storageBufDescSize = props.storageBufferDescriptorSize;
    m_uniformBufDescSize = props.uniformBufferDescriptorSize;
    m_storageImgDescSize = props.storageImageDescriptorSize;
    m_sampledImgDescSize = props.sampledImageDescriptorSize;
    m_samplerDescSize = props.samplerDescriptorSize;
    m_combinedImgSamplerDescSize = props.combinedImageSamplerDescriptorSize;
    m_accelStructDescSize = props.accelerationStructureDescriptorSize;

    // Query layout size and per-binding offsets.
    vkGetDescriptorSetLayoutSizeEXT(ctx.device, setLayout, &m_layoutSize);
    m_bindingOffsets.resize(numBindings);
    for (std::uint32_t i = 0; i < numBindings; ++i) {
        vkGetDescriptorSetLayoutBindingOffsetEXT(ctx.device, setLayout, i, &m_bindingOffsets[i]);
    }

    // Create a host-visible descriptor buffer.
    auto buf = Buffer::create(ctx, std::max<VkDeviceSize>(m_layoutSize, 256),
                              VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                              VMA_MEMORY_USAGE_AUTO_PREFER_HOST, name);
    if (!buf) {
        Logger::error("DescriptorBufferWriter: failed to create buffer '{}': VkResult {}", name,
                      static_cast<int>(buf.error()));
        return false;
    }
    m_buffer = std::move(*buf);
    return true;
}

void DescriptorBufferWriter::shutdown() {
    m_buffer = {};
    m_bindingOffsets.clear();
    m_layoutSize = 0;
}

VkDeviceSize DescriptorBufferWriter::bindingOffset(std::uint32_t binding) const {
    if (binding >= m_bindingOffsets.size()) {
        Logger::error("DescriptorBufferWriter: binding {} out of range ({})", binding, m_bindingOffsets.size());
        return 0;
    }
    return m_bindingOffsets[binding];
}

void DescriptorBufferWriter::writeRaw(VkDeviceSize offset, const void* data, std::size_t size) {
    if (!m_buffer.mappedData()) {
        Logger::error("DescriptorBufferWriter: buffer not mapped");
        return;
    }
    std::memcpy(static_cast<std::uint8_t*>(m_buffer.mappedData()) + offset, data, size);
}

void DescriptorBufferWriter::writeDescriptor(const DeviceContext& ctx, const VkDescriptorGetInfoEXT& info,
                                             std::uint32_t type, VkDeviceSize offset) {
    VkDeviceSize descSize = 0;
    switch (type) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            descSize = m_storageBufDescSize;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            descSize = m_uniformBufDescSize;
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            descSize = m_storageImgDescSize;
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            descSize = m_sampledImgDescSize;
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            descSize = m_samplerDescSize;
            break;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            descSize = m_combinedImgSamplerDescSize;
            break;
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            descSize = m_accelStructDescSize;
            break;
        default:
            Logger::error("DescriptorBufferWriter: unsupported descriptor type {}", type);
            return;
    }
    alignas(16) std::uint8_t data[256] = {};
    vkGetDescriptorEXT(ctx.device, &info, descSize, data);
    writeRaw(offset, data, descSize);
}

void DescriptorBufferWriter::writeStorageBuffer(const DeviceContext& ctx, std::uint32_t binding,
                                                VkDeviceAddress address, VkDeviceSize range) {
    const VkDescriptorAddressInfoEXT addrInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,
        .pNext = nullptr,
        .address = address,
        .range = range,
        .format = VK_FORMAT_UNDEFINED,
    };
    const VkDescriptorGetInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
        .pNext = nullptr,
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .data = {.pStorageBuffer = &addrInfo},
    };
    writeDescriptor(ctx, info, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bindingOffset(binding));
}

void DescriptorBufferWriter::writeUniformBuffer(const DeviceContext& ctx, std::uint32_t binding,
                                                VkDeviceAddress address, VkDeviceSize range) {
    const VkDescriptorAddressInfoEXT addrInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,
        .pNext = nullptr,
        .address = address,
        .range = range,
        .format = VK_FORMAT_UNDEFINED,
    };
    const VkDescriptorGetInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
        .pNext = nullptr,
        .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .data = {.pUniformBuffer = &addrInfo},
    };
    writeDescriptor(ctx, info, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, bindingOffset(binding));
}

void DescriptorBufferWriter::writeUniformBufferHandle(const DeviceContext& ctx, std::uint32_t binding,
                                                      VkBuffer buffer) {
    if (buffer == VK_NULL_HANDLE) {
        writeUniformBuffer(ctx, binding, 0, VK_WHOLE_SIZE);
        return;
    }
    const VkBufferDeviceAddressInfo addrInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .pNext = nullptr, .buffer = buffer};
    VkMemoryRequirements memReq{};
    vkGetBufferMemoryRequirements(ctx.device, buffer, &memReq);
    writeUniformBuffer(ctx, binding, vkGetBufferDeviceAddress(ctx.device, &addrInfo), memReq.size);
}

void DescriptorBufferWriter::writeStorageBufferHandle(const DeviceContext& ctx, std::uint32_t binding,
                                                      VkBuffer buffer) {
    // VK14 nullDescriptor: a null handle produces a null descriptor (reads return zeros).
    if (buffer == VK_NULL_HANDLE) {
        writeStorageBuffer(ctx, binding, 0, VK_WHOLE_SIZE);
        return;
    }
    const VkBufferDeviceAddressInfo addrInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .pNext = nullptr, .buffer = buffer};
    VkMemoryRequirements memReq{};
    vkGetBufferMemoryRequirements(ctx.device, buffer, &memReq);
    writeStorageBuffer(ctx, binding, vkGetBufferDeviceAddress(ctx.device, &addrInfo), memReq.size);
}

void DescriptorBufferWriter::writeStorageImage(const DeviceContext& ctx, std::uint32_t binding,
                                               VkImageView imageView, VkImageLayout layout) {
    const VkDescriptorImageInfo imgInfo{VK_NULL_HANDLE, imageView, layout};
    const VkDescriptorGetInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
        .pNext = nullptr,
        .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .data = {.pStorageImage = &imgInfo},
    };
    writeDescriptor(ctx, info, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, bindingOffset(binding));
}

void DescriptorBufferWriter::writeSampledImage(const DeviceContext& ctx, std::uint32_t binding,
                                               VkImageView imageView, VkImageLayout layout) {
    const VkDescriptorImageInfo imgInfo{VK_NULL_HANDLE, imageView, layout};
    const VkDescriptorGetInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
        .pNext = nullptr,
        .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .data = {.pSampledImage = &imgInfo},
    };
    writeDescriptor(ctx, info, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, bindingOffset(binding));
}

void DescriptorBufferWriter::writeSampler(const DeviceContext& ctx, std::uint32_t binding, VkSampler sampler) {
    const VkDescriptorGetInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
        .pNext = nullptr,
        .type = VK_DESCRIPTOR_TYPE_SAMPLER,
        .data = {.pSampler = &sampler},
    };
    writeDescriptor(ctx, info, VK_DESCRIPTOR_TYPE_SAMPLER, bindingOffset(binding));
}

void DescriptorBufferWriter::writeCombinedImageSampler(const DeviceContext& ctx, std::uint32_t binding,
                                                       std::uint32_t arrayElement, VkSampler sampler,
                                                       VkImageView imageView, VkImageLayout layout) {
    const VkDescriptorImageInfo imgInfo{sampler, imageView, layout};
    const VkDescriptorGetInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
        .pNext = nullptr,
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .data = {.pCombinedImageSampler = &imgInfo},
    };
    // Array elements are consecutive: offset = bindingOffset + arrayElement * descriptorSize.
    writeDescriptor(ctx, info, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    bindingOffset(binding) + arrayElement * m_combinedImgSamplerDescSize);
}

void DescriptorBufferWriter::writeAccelerationStructure(const DeviceContext& ctx, std::uint32_t binding,
                                                        VkAccelerationStructureKHR as) {
    // VkDescriptorDataEXT::accelerationStructure is a VkDeviceAddress, not a handle.
    const VkAccelerationStructureDeviceAddressInfoKHR addrInfo{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
        .pNext = nullptr,
        .accelerationStructure = as,
    };
    const VkDescriptorGetInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
        .pNext = nullptr,
        .type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        .data = {.accelerationStructure = vkGetAccelerationStructureDeviceAddressKHR(ctx.device, &addrInfo)},
    };
    writeDescriptor(ctx, info, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, bindingOffset(binding));
}

void DescriptorBufferWriter::bind(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, VkPipelineLayout layout,
                                  std::uint32_t firstSet) const {
    const VkDescriptorBufferBindingInfoEXT bindingInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
        .pNext = nullptr,
        .address = m_buffer.deviceAddress(),
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT,
    };
    vkCmdBindDescriptorBuffersEXT(cmd, 1, &bindingInfo);
    constexpr std::uint32_t bufferIndex = 0;
    constexpr VkDeviceSize offset = 0;
    vkCmdSetDescriptorBufferOffsetsEXT(cmd, bindPoint, layout, firstSet, 1, &bufferIndex, &offset);
}

void DescriptorBufferWriter::bindSets(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, VkPipelineLayout layout,
                                       std::uint32_t firstSet,
                                       std::span<const DescriptorBufferWriter* const> writers) {
    if (writers.empty()) return;
    // Bind all descriptor buffers.
    std::vector<VkDescriptorBufferBindingInfoEXT> bindingInfos;
    bindingInfos.reserve(writers.size());
    for (const auto* w : writers) {
        bindingInfos.push_back(VkDescriptorBufferBindingInfoEXT{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
            .pNext = nullptr,
            .address = w->deviceAddress(),
            .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT,
        });
    }
    vkCmdBindDescriptorBuffersEXT(cmd, static_cast<std::uint32_t>(bindingInfos.size()), bindingInfos.data());
    // Set offsets: each writer is one set at offset 0 in its own buffer.
    std::vector<std::uint32_t> bufferIndices(writers.size());
    std::vector<VkDeviceSize> offsets(writers.size(), 0);
    for (std::size_t i = 0; i < writers.size(); ++i) bufferIndices[i] = static_cast<std::uint32_t>(i);
    vkCmdSetDescriptorBufferOffsetsEXT(cmd, bindPoint, layout, firstSet,
                                       static_cast<std::uint32_t>(writers.size()), bufferIndices.data(),
                                       offsets.data());
}

} // namespace harmonia
