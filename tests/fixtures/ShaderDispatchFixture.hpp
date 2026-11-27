#ifndef TESTS_FIXTURES_SHADERDISPATCHFIXTURE_HPP
#define TESTS_FIXTURES_SHADERDISPATCHFIXTURE_HPP

// Runs a compute test shader of tests/shaders that takes one device address in its push
// constants (a float4 array) and writes one float4 per thread.
//
// Usage: derive a test from ShaderDispatchFixture and call runShader().

#include <volk/volk.h>

#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <slang-math/slang-math.hpp>
#include <vector>

#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/core/ShaderModule.hpp"
#include "harmonia/core/VulkanHandle.hpp"

class ShaderDispatchFixture : public VulkanFixture {
  public:
    /// One dispatch of @p shaderName from the test shader directory: a single workgroup of
    /// 64 threads, a thread per mode. The @p outputCount float4 are read back; where the
    /// shader wrote nothing, a float4 is still the 0xCD pattern the buffer was filled with.
    [[nodiscard]] std::vector<sm::float4> runShader(const char* shaderName, std::size_t outputCount) {
        const auto& ctx = deviceCtx();
        struct Output {
            VmaAllocator allocator{};
            VkBuffer buffer{};
            VmaAllocation allocation{};
            void* mapped{};
            ~Output() {
                if (buffer) {
                    vmaDestroyBuffer(allocator, buffer, allocation);
                }
            }
        } output;
        output.allocator = ctx.allocator;
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = outputCount * sizeof(sm::float4);
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        allocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        VmaAllocationInfo mapped{};
        EXPECT_EQ(vmaCreateBuffer(ctx.allocator, &info, &allocation, &output.buffer, &output.allocation, &mapped),
                  VK_SUCCESS);
        if (mapped.pMappedData == nullptr) {
            return {};
        }
        output.mapped = mapped.pMappedData;
        std::memset(output.mapped, 0xCD, static_cast<std::size_t>(info.size));
        EXPECT_EQ(vmaFlushAllocation(ctx.allocator, output.allocation, 0, VK_WHOLE_SIZE), VK_SUCCESS);
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkDeviceAddress)};
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &range;
        VkPipelineLayout rawLayout{};
        EXPECT_EQ(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &rawLayout), VK_SUCCESS);
        const harmonia::core::UniquePipelineLayout layout{ctx.device, rawLayout};
        const auto loaded = harmonia::core::createShaderModule(
            ctx.device, std::filesystem::path(HARMONIA_TEST_SHADER_DIR) / shaderName);
        EXPECT_TRUE(loaded.has_value());
        if (!loaded.has_value()) {
            return {};
        }
        const harmonia::core::UniqueShaderModule module{ctx.device, *loaded};
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.layout = layout;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = "main";
        VkPipeline rawPipeline{};
        EXPECT_EQ(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &rawPipeline),
                  VK_SUCCESS);
        const harmonia::core::UniquePipeline pipeline{ctx.device, rawPipeline};
        const VkBufferDeviceAddressInfo addressInfo{
            VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, output.buffer};
        const auto address = vkGetBufferDeviceAddress(ctx.device, &addressInfo);
        const auto cmd = commandPool().beginOneShot();
        EXPECT_TRUE(cmd.has_value());
        if (!cmd.has_value()) {
            return {};
        }
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
        EXPECT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);
        EXPECT_EQ(vmaInvalidateAllocation(ctx.allocator, output.allocation, 0, VK_WHOLE_SIZE), VK_SUCCESS);
        const auto* values = static_cast<const sm::float4*>(output.mapped);
        return {values, values + outputCount};
    }
};

#endif // TESTS_FIXTURES_SHADERDISPATCHFIXTURE_HPP
