#include "harmonia/renderer/Descriptors.hpp"

#include <volk/volk.h>

#include <array>
#include <cstdint>
#include <utility>

namespace harmonia {

std::expected<Descriptors, VkResult> Descriptors::create(const DeviceContext& ctx) {
    constexpr std::uint32_t kBindlessTextureArraySize = 1024U;
    constexpr std::uint32_t kCombinedImageSamplerDescriptorCount = kBindlessTextureArraySize + 1U;

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
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
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
        VkDescriptorBindingFlags(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                 VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT),
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
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
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

    constexpr std::array poolSizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 9},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kCombinedImageSamplerDescriptorCount},
    };
    const VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets = 1,
        .poolSizeCount = static_cast<std::uint32_t>(poolSizes.size()),
        .pPoolSizes = poolSizes.data(),
    };
    {
        VkDescriptorPool pool{};
        if (const VkResult result = vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &pool);
            result != VK_SUCCESS) {
            return std::unexpected(result);
        }
        descriptors.m_pool = harmonia::UniqueDescriptorPool{ctx.device, pool};
    }

    const VkDescriptorSetAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = descriptors.m_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = descriptors.m_set1Layout.ptr(),
    };
    if (const VkResult result = vkAllocateDescriptorSets(ctx.device, &allocInfo, &descriptors.m_set1);
        result != VK_SUCCESS) {
        return std::unexpected(result);
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
        if (const VkResult result =
                vkCreatePipelineLayout(ctx.device, &pipelineLayoutInfo, nullptr, &pipelineLayout);
            result != VK_SUCCESS) {
            return std::unexpected(result);
        }
        descriptors.m_pipelineLayout = harmonia::UniquePipelineLayout{ctx.device, pipelineLayout};
    }

    ctx.setDebugName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, descriptors.m_set0Layout.get(), "harmonia.set0.push");
    ctx.setDebugName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, descriptors.m_set1Layout.get(), "harmonia.set1.scene");
    ctx.setDebugName(VK_OBJECT_TYPE_DESCRIPTOR_POOL, descriptors.m_pool.get(), "harmonia.scene.pool");
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
    const std::array bufferInfos{
        VkDescriptorBufferInfo{instanceBuffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{materialBuffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{vertexBuffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{indexBuffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{lightBuffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{emissiveTriangleBuffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{emissiveCdfBuffer, 0, VK_WHOLE_SIZE},
    };
    const std::array writes{
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             nullptr,
                             m_set1,
                             0,
                             0,
                             1,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             nullptr,
                             &bufferInfos[0],
                             nullptr},
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             nullptr,
                             m_set1,
                             1,
                             0,
                             1,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             nullptr,
                             &bufferInfos[1],
                             nullptr},
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             nullptr,
                             m_set1,
                             2,
                             0,
                             1,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             nullptr,
                             &bufferInfos[2],
                             nullptr},
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             nullptr,
                             m_set1,
                             3,
                             0,
                             1,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             nullptr,
                             &bufferInfos[3],
                             nullptr},
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             nullptr,
                             m_set1,
                             5,
                             0,
                             1,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             nullptr,
                             &bufferInfos[4],
                             nullptr},
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             nullptr,
                             m_set1,
                             7,
                             0,
                             1,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             nullptr,
                             &bufferInfos[5],
                             nullptr},
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             nullptr,
                             m_set1,
                             10,
                             0,
                             1,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             nullptr,
                             &bufferInfos[6],
                             nullptr},
    };
    vkUpdateDescriptorSets(ctx.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    // Bind each scene texture to binding 4 (bindless combined image sampler array).
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(textures.size()); ++i) {
        const VkDescriptorImageInfo imageInfo{
            .sampler = textures[i].sampler(),
            .imageView = textures[i].image().view(),
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = m_set1,
            .dstBinding = 4,
            .dstArrayElement = i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &imageInfo,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        };
        vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
    }

    return VK_SUCCESS;
}

VkResult Descriptors::updateEnvMap(const DeviceContext& ctx, VkImageView view, VkSampler sampler) {
    const VkDescriptorImageInfo imageInfo{
        .sampler = sampler,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = m_set1,
        .dstBinding = 6,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &imageInfo,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };
    vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
    return VK_SUCCESS;
}

VkResult Descriptors::updateEnvImportance(const DeviceContext& ctx, VkBuffer marginalCdf, VkBuffer conditionalCdf) {
    const VkDescriptorBufferInfo marginalInfo{.buffer = marginalCdf, .offset = 0, .range = VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo conditionalInfo{.buffer = conditionalCdf, .offset = 0, .range = VK_WHOLE_SIZE};
    const std::array writes{
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             nullptr,
                             m_set1,
                             8,
                             0,
                             1,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             nullptr,
                             &marginalInfo,
                             nullptr},
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             nullptr,
                             m_set1,
                             9,
                             0,
                             1,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             nullptr,
                             &conditionalInfo,
                             nullptr},
    };
    vkUpdateDescriptorSets(ctx.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return VK_SUCCESS;
}

} // namespace harmonia
