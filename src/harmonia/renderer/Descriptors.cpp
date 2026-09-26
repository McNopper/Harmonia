#include "harmonia/renderer/Descriptors.hpp"

#include <volk/volk.h>

#include <array>
#include <cstdint>
#include <utility>

namespace harmonia {

std::expected<Descriptors, VkResult> Descriptors::create(const DeviceContext& ctx) {
    constexpr std::uint32_t kBindlessTextureArraySize = 1024U;

    constexpr std::array set0Bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_ALL, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_ALL, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
        VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_ALL, nullptr},
        VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_ALL, nullptr},
        VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_ALL, nullptr},
    };
    const VkDescriptorSetLayoutCreateInfo set0Info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        // MOD1: descriptor buffer (was PUSH_DESCRIPTOR — can't mix with descriptor buffer sets).
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .bindingCount = static_cast<std::uint32_t>(set0Bindings.size()),
        .pBindings = set0Bindings.data(),
    };

    constexpr std::array set1Bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
        VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
        VkDescriptorSetLayoutBinding{
            4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kBindlessTextureArraySize, VK_SHADER_STAGE_ALL, nullptr},
        VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
        VkDescriptorSetLayoutBinding{6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, nullptr},
        VkDescriptorSetLayoutBinding{7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
        VkDescriptorSetLayoutBinding{8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
        VkDescriptorSetLayoutBinding{9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
        VkDescriptorSetLayoutBinding{10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
    };
    constexpr std::array bindingFlags{
        VkDescriptorBindingFlags{},
        VkDescriptorBindingFlags{},
        VkDescriptorBindingFlags{},
        VkDescriptorBindingFlags{},
        VkDescriptorBindingFlags(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT), // MOD1: no UPDATE_AFTER_BIND
        VkDescriptorBindingFlags{},
        VkDescriptorBindingFlags(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT),
        VkDescriptorBindingFlags{},
        VkDescriptorBindingFlags(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT), // env marginal CDF
        VkDescriptorBindingFlags(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT), // env conditional CDF
        VkDescriptorBindingFlags{},                                          // emissive power CDF
    };
    const VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .pNext = nullptr,
        .bindingCount = static_cast<std::uint32_t>(bindingFlags.size()),
        .pBindingFlags = bindingFlags.data(),
    };
    const VkDescriptorSetLayoutCreateInfo set1Info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &bindingFlagsInfo,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT, // MOD1
        .bindingCount = static_cast<std::uint32_t>(set1Bindings.size()),
        .pBindings = set1Bindings.data(),
    };

    Descriptors descriptors;

    {
        VkDescriptorSetLayout set0Layout{};
        if (const VkResult result = vkCreateDescriptorSetLayout(ctx.device, &set0Info, nullptr, &set0Layout);
            result != VK_SUCCESS) {
            return std::unexpected(result);
        }
        descriptors.m_set0Layout = harmonia::UniqueDescriptorSetLayout{ctx.device, set0Layout};
    }
    {
        VkDescriptorSetLayout set1Layout{};
        if (const VkResult result = vkCreateDescriptorSetLayout(ctx.device, &set1Info, nullptr, &set1Layout);
            result != VK_SUCCESS) {
            return std::unexpected(result);
        }
        descriptors.m_set1Layout = harmonia::UniqueDescriptorSetLayout{ctx.device, set1Layout};
    }

    // MOD1: descriptor buffers replace the pool + sets.
    if (!descriptors.m_frameWriter.init(ctx, descriptors.m_set0Layout.get(), 6, "harmonia.set0.frame.descBuf") ||
        !descriptors.m_sceneWriter.init(ctx, descriptors.m_set1Layout.get(), 11, "harmonia.set1.scene.descBuf")) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    constexpr VkPushConstantRange pushConstantRange{
        .stageFlags = VK_SHADER_STAGE_ALL,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    const std::array<VkDescriptorSetLayout, 2> layouts{descriptors.m_set0Layout, descriptors.m_set1Layout};
    const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = static_cast<std::uint32_t>(layouts.size()),
        .pSetLayouts = layouts.data(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange,
    };
    {
        VkPipelineLayout pipelineLayout{};
        if (const VkResult result = vkCreatePipelineLayout(ctx.device, &pipelineLayoutInfo, nullptr, &pipelineLayout);
            result != VK_SUCCESS) {
            return std::unexpected(result);
        }
        descriptors.m_pipelineLayout = harmonia::UniquePipelineLayout{ctx.device, pipelineLayout};
    }

    ctx.setDebugName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, descriptors.m_set0Layout.get(), "harmonia.set0.push");
    ctx.setDebugName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, descriptors.m_set1Layout.get(), "harmonia.set1.scene");
    ctx.setDebugName(VK_OBJECT_TYPE_PIPELINE_LAYOUT, descriptors.m_pipelineLayout.get(), "harmonia.pipelineLayout");

    return descriptors;
}

VkResult Descriptors::updateSceneSet(const DeviceContext& ctx,
                                     VkBuffer instanceBuffer,
                                     VkBuffer materialBuffer,
                                     VkBuffer vertexBuffer,
                                     VkBuffer indexBuffer,
                                     VkBuffer lightBuffer,
                                     VkBuffer emissiveTriangleBuffer,
                                     VkBuffer emissiveCdfBuffer,
                                     std::span<const Texture> textures) {
    // MOD1: typed descriptor buffer writes.
    m_sceneWriter.writeStorageBufferHandle(ctx, 0, instanceBuffer);
    m_sceneWriter.writeStorageBufferHandle(ctx, 1, materialBuffer);
    m_sceneWriter.writeStorageBufferHandle(ctx, 2, vertexBuffer);
    m_sceneWriter.writeStorageBufferHandle(ctx, 3, indexBuffer);
    m_sceneWriter.writeStorageBufferHandle(ctx, 5, lightBuffer);
    m_sceneWriter.writeStorageBufferHandle(ctx, 7, emissiveTriangleBuffer);
    m_sceneWriter.writeStorageBufferHandle(ctx, 10, emissiveCdfBuffer);

    // Bind each scene texture to binding 4 (bindless combined image sampler array).
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(textures.size()); ++i) {
        m_sceneWriter.writeCombinedImageSampler(ctx, 4, i, textures[i].sampler(),
                                                 textures[i].image().view(),
                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return VK_SUCCESS;
}

VkResult Descriptors::updateEnvMap(const DeviceContext& ctx, VkImageView view, VkSampler sampler) {
    // MOD1: typed descriptor buffer write.
    m_sceneWriter.writeCombinedImageSampler(ctx, 6, 0, sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return VK_SUCCESS;
}

VkResult Descriptors::updateEnvImportance(const DeviceContext& ctx, VkBuffer marginalCdf, VkBuffer conditionalCdf) {
    // MOD1: typed descriptor buffer writes.
    m_sceneWriter.writeStorageBufferHandle(ctx, 8, marginalCdf);
    m_sceneWriter.writeStorageBufferHandle(ctx, 9, conditionalCdf);
    return VK_SUCCESS;
}

VkResult Descriptors::updateFrameSet(const DeviceContext& ctx,
                                     VkAccelerationStructureKHR tlas,
                                     VkImageView hdrView,
                                     VkBuffer cameraBuffer,
                                     VkImageView gNormalView,
                                     VkImageView gDepthView) {
    // MOD1: set 0 per-frame writes via descriptor buffer.
    // Bindings match the set0 layout: 0=AS, 1=hdr storage image, 2=camera UBO,
    // 4=gNormal storage image, 5=gDepth storage image.
    m_frameWriter.writeAccelerationStructure(ctx, 0, tlas);
    m_frameWriter.writeStorageImage(ctx, 1, hdrView, VK_IMAGE_LAYOUT_GENERAL);
    m_frameWriter.writeUniformBufferHandle(ctx, 2, cameraBuffer);
    m_frameWriter.writeStorageImage(ctx, 3, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL); // unused (nullDescriptor)
    m_frameWriter.writeStorageImage(ctx, 4, gNormalView, VK_IMAGE_LAYOUT_GENERAL);
    m_frameWriter.writeStorageImage(ctx, 5, gDepthView, VK_IMAGE_LAYOUT_GENERAL);
    return VK_SUCCESS;
}

void Descriptors::bindSceneSet(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint) const {
    // MOD1: bind both descriptor buffers (set 0 + set 1).
    const std::array<const DescriptorBufferWriter*, 2> writers{&m_frameWriter, &m_sceneWriter};
    DescriptorBufferWriter::bindSets(cmd, bindPoint, m_pipelineLayout, 0, writers);
}

} // namespace harmonia
