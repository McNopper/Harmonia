#ifndef HARMONIA_CORE_DESCRIPTORBUFFERWRITER_HPP
#define HARMONIA_CORE_DESCRIPTORBUFFERWRITER_HPP

#include <volk/volk.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "harmonia/core/Buffer.hpp"
#include "harmonia/DeviceContext.hpp"

namespace harmonia {

/// MOD1: Descriptor buffer writer — wraps VK_EXT_descriptor_buffer descriptor
/// generation and layout so passes write typed descriptors into a flat GPU buffer
/// instead of using descriptor pools/sets.
///
/// Usage:
///   DescriptorBufferWriter writer;
///   writer.init(ctx, setLayout, numBindings, "myPass.descriptorBuffer");
///   writer.writeStorageBuffer(ctx, bindingIdx, buffer.deviceAddress(), buffer.size());
///   writer.writeSampledImage(ctx, bindingIdx, imageView, imageLayout);
///   writer.bind(cmd, pipelineBindPoint, pipelineLayout, firstSet);
///
/// The set layout must have VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT.
/// The pipeline must have VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT.
class DescriptorBufferWriter {
public:
    /// Initialize: query descriptor sizes + binding offsets, create the descriptor buffer.
    /// @param ctx Device context (for device + physicalDevice + allocator)
    /// @param setLayout The descriptor set layout (must have DESCRIPTOR_BUFFER_BIT)
    /// @param numBindings Number of bindings in the set layout (max binding index + 1)
    /// @param name Buffer debug name
    /// @return true on success
    bool init(const DeviceContext& ctx, VkDescriptorSetLayout setLayout, std::uint32_t numBindings,
              const char* name);

    /// Release the descriptor buffer.
    void shutdown();

    /// Write a uniform buffer descriptor at the given binding.
    void writeUniformBuffer(const DeviceContext& ctx, std::uint32_t binding, VkDeviceAddress address,
                            VkDeviceSize range);

    /// Write a uniform buffer descriptor at the given binding (from a VkBuffer handle).
    void writeUniformBufferHandle(const DeviceContext& ctx, std::uint32_t binding, VkBuffer buffer);

    /// Write a storage buffer descriptor at the given binding (from a VkBuffer handle).
    /// Queries device address + memory size internally.
    void writeStorageBufferHandle(const DeviceContext& ctx, std::uint32_t binding, VkBuffer buffer);
    void writeStorageBuffer(const DeviceContext& ctx, std::uint32_t binding, VkDeviceAddress address,
                            VkDeviceSize range);

    /// Write a storage image descriptor at the given binding.
    void writeStorageImage(const DeviceContext& ctx, std::uint32_t binding, VkImageView imageView,
                           VkImageLayout layout);

    /// Write a sampled image descriptor at the given binding.
    void writeSampledImage(const DeviceContext& ctx, std::uint32_t binding, VkImageView imageView,
                           VkImageLayout layout);

    /// Write a sampler descriptor at the given binding.
    void writeSampler(const DeviceContext& ctx, std::uint32_t binding, VkSampler sampler);

    /// Write a combined image sampler descriptor at the given binding + array element.
    void writeCombinedImageSampler(const DeviceContext& ctx, std::uint32_t binding, std::uint32_t arrayElement,
                                   VkSampler sampler, VkImageView imageView, VkImageLayout layout);

    /// Write an acceleration structure descriptor at the given binding.
    void writeAccelerationStructure(const DeviceContext& ctx, std::uint32_t binding, VkAccelerationStructureKHR as);

    /// Bind the descriptor buffer for the given set range.
    /// @param firstSet First set index in the pipeline layout
    /// @param setCount Number of sets (always 1 for a single-set writer)
    void bind(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, VkPipelineLayout layout,
              std::uint32_t firstSet = 0) const;

    /// Bind multiple descriptor buffer writers as consecutive sets.
    /// @param writers Array of writer pointers (each provides one set)
    /// @param firstSet First set index in the pipeline layout
    static void bindSets(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, VkPipelineLayout layout,
                         std::uint32_t firstSet, std::span<const DescriptorBufferWriter* const> writers);

    /// Get the device address of the descriptor buffer (for advanced use).
    [[nodiscard]] VkDeviceAddress deviceAddress() const { return m_buffer.deviceAddress(); }

    /// Get the raw buffer handle.
    [[nodiscard]] VkBuffer handle() const { return m_buffer.handle(); }

    /// Get the total layout size (bytes) of the descriptor buffer.
    [[nodiscard]] VkDeviceSize layoutSize() const { return m_layoutSize; }

private:
    /// Get the offset for a binding.
    [[nodiscard]] VkDeviceSize bindingOffset(std::uint32_t binding) const;

    /// Write raw descriptor data at a byte offset in the mapped buffer.
    void writeRaw(VkDeviceSize offset, const void* data, std::size_t size);

    /// Generate a descriptor via vkGetDescriptorEXT and write it at the given offset.
    void writeDescriptor(const DeviceContext& ctx, const VkDescriptorGetInfoEXT& info, std::uint32_t type,
                         VkDeviceSize offset);

    Buffer m_buffer;
    VkDeviceSize m_layoutSize = 0;
    std::vector<VkDeviceSize> m_bindingOffsets; ///< per-binding byte offsets in the descriptor buffer
    VkDeviceSize m_storageBufDescSize = 0;
    VkDeviceSize m_uniformBufDescSize = 0;
    VkDeviceSize m_storageImgDescSize = 0;
    VkDeviceSize m_sampledImgDescSize = 0;
    VkDeviceSize m_samplerDescSize = 0;
    VkDeviceSize m_combinedImgSamplerDescSize = 0;
    VkDeviceSize m_accelStructDescSize = 0;
};

} // namespace harmonia

#endif // HARMONIA_CORE_DESCRIPTORBUFFERWRITER_HPP
