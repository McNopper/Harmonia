#include <volk/volk.h>

#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <slang-math/slang-math.hpp>

#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/core/ShaderModule.hpp"
#include "harmonia/core/VulkanHandle.hpp"

TEST_F(VulkanFixture, BsdfMeasure_DiscreteEventsHaveNoContinuousDensity) {
    const auto& ctx = deviceCtx();
    struct Output {
        VmaAllocator allocator{};
        VkBuffer buffer{};
        VmaAllocation allocation{};
        ~Output() {
            if (buffer)
                vmaDestroyBuffer(allocator, buffer, allocation);
        }
    } output;
    output.allocator = ctx.allocator;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = 12 * sizeof(sm::float4);
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    allocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    VmaAllocationInfo mapped{};
    ASSERT_EQ(vmaCreateBuffer(ctx.allocator, &info, &allocation, &output.buffer, &output.allocation, &mapped),
              VK_SUCCESS);
    ASSERT_NE(mapped.pMappedData, nullptr);
    std::memset(mapped.pMappedData, 0xCD, static_cast<std::size_t>(info.size));
    ASSERT_EQ(vmaFlushAllocation(ctx.allocator, output.allocation, 0, VK_WHOLE_SIZE), VK_SUCCESS);
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkDeviceAddress)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    VkPipelineLayout rawLayout{};
    ASSERT_EQ(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &rawLayout), VK_SUCCESS);
    const harmonia::core::UniquePipelineLayout layout{ctx.device, rawLayout};
    const auto loaded = harmonia::core::createShaderModule(
        ctx.device, std::filesystem::path(HARMONIA_TEST_SHADER_DIR) / "bsdf_measure_test.spv");
    ASSERT_TRUE(loaded.has_value());
    const harmonia::core::UniqueShaderModule module{ctx.device, *loaded};
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.layout = layout;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    VkPipeline rawPipeline{};
    ASSERT_EQ(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &rawPipeline),
              VK_SUCCESS);
    const harmonia::core::UniquePipeline pipeline{ctx.device, rawPipeline};
    const VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, output.buffer};
    const auto address = vkGetBufferDeviceAddress(ctx.device, &addressInfo);
    const auto cmd = commandPool().beginOneShot();
    ASSERT_TRUE(cmd.has_value());
    vkCmdBindPipeline(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdPushConstants(*cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(address), &address);
    vkCmdDispatch(*cmd, 1, 1, 1);
    VkMemoryBarrier2 memory{};
    memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    memory.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    memory.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    memory.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    memory.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &memory;
    vkCmdPipelineBarrier2(*cmd, &dependency);
    ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);
    ASSERT_EQ(vmaInvalidateAllocation(ctx.allocator, output.allocation, 0, VK_WHOLE_SIZE), VK_SUCCESS);
    const auto* values = static_cast<const sm::float4*>(mapped.pMappedData);
    for (auto i : {0U, 2U, 3U, 6U, 10U, 11U}) {
        SCOPED_TRACE(i);
        EXPECT_EQ(values[i].x, 0.0F);
        EXPECT_EQ(values[i].y, 0.0F);
        EXPECT_EQ(values[i].z, 1.0F);
        EXPECT_GT(values[i].w, 0.0F);
    }
    for (auto i : {1U, 4U, 5U, 7U, 8U, 9U}) {
        SCOPED_TRACE(i);
        EXPECT_GT(values[i].x, 0.0F);
        EXPECT_GT(values[i].y, 0.0F);
    }
}
