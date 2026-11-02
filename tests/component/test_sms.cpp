// GPU tests of the production Slang solver. Oracle: analytic virtual-light geometry.
#include <volk/volk.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <slang-math/slang-math.hpp>
#include <span>
#include <vector>

#include "fixtures/VulkanTestFixture.hpp"
#include "harmonia/core/Buffer.hpp"
#include "harmonia/core/ShaderModule.hpp"
#include "harmonia/core/VulkanHandle.hpp"

namespace {

struct SmsInput {
    sm::float4 receiver, light, seed, normal, tangent, bitangent;
};
struct SmsOutput {
    sm::float4 root, constraint;
};
struct SmsPush {
    VkDeviceAddress inputs, outputs;
    std::uint32_t count, pad;
};
static_assert(sizeof(SmsInput) == 96 && sizeof(SmsOutput) == 32 && sizeof(SmsPush) == 24);

struct ReadbackStorage {
    VmaAllocator allocator{};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    ReadbackStorage() = default;
    ReadbackStorage(const ReadbackStorage&) = delete;
    ReadbackStorage& operator=(const ReadbackStorage&) = delete;
    ~ReadbackStorage() {
        if (buffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(allocator, buffer, allocation);
    }
};

[[nodiscard]] sm::float3 xyz(sm::float4 p) {
    return {p.x, p.y, p.z};
}

class SmsFixture : public VulkanFixture {
  public:
    void run(std::span<const SmsInput> inputs, std::vector<SmsOutput>& results) {
        const auto& ctx = deviceCtx();
        auto source = harmonia::core::Buffer::create(ctx,
                                                     inputs.size_bytes(),
                                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                     VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                                     "test.sms.input");
        ASSERT_TRUE(source.has_value());
        source->uploadData(inputs.data(), inputs.size_bytes());
        ReadbackStorage target;
        target.allocator = ctx.allocator;
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = inputs.size() * sizeof(SmsOutput);
        bufferInfo.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        allocationInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        VmaAllocationInfo mapped{};
        ASSERT_EQ(
            vmaCreateBuffer(ctx.allocator, &bufferInfo, &allocationInfo, &target.buffer, &target.allocation, &mapped),
            VK_SUCCESS);
        ASSERT_NE(mapped.pMappedData, nullptr);
        // Poison readback so an absent dispatch cannot look successful.
        results.assign(inputs.size(), SmsOutput{sm::float4(-99.0F), sm::float4(-99.0F)});
        std::memcpy(mapped.pMappedData, results.data(), results.size() * sizeof(SmsOutput));
        ASSERT_EQ(vmaFlushAllocation(ctx.allocator, target.allocation, 0, VK_WHOLE_SIZE), VK_SUCCESS);

        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SmsPush)};
        const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                    .pNext = nullptr,
                                                    .flags = 0,
                                                    .setLayoutCount = 0,
                                                    .pSetLayouts = nullptr,
                                                    .pushConstantRangeCount = 1,
                                                    .pPushConstantRanges = &range};
        VkPipelineLayout rawLayout{};
        ASSERT_EQ(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &rawLayout), VK_SUCCESS);
        const harmonia::core::UniquePipelineLayout layout{ctx.device, rawLayout};
        auto rawModule = harmonia::core::createShaderModule(
            ctx.device, std::filesystem::path(HARMONIA_TEST_SHADER_DIR) / "sms_test.spv");
        ASSERT_TRUE(rawModule.has_value());
        const harmonia::core::UniqueShaderModule module{ctx.device, *rawModule};
        const VkComputePipelineCreateInfo pipelineInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .pNext = nullptr,
                      .flags = 0,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = module,
                      .pName = "main",
                      .pSpecializationInfo = nullptr},
            .layout = layout,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = 0};
        VkPipeline rawPipeline{};
        ASSERT_EQ(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &rawPipeline),
                  VK_SUCCESS);
        const harmonia::core::UniquePipeline pipeline{ctx.device, rawPipeline};
        auto cmd = commandPool().beginOneShot();
        ASSERT_TRUE(cmd.has_value());
        vkCmdBindPipeline(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        const VkBufferDeviceAddressInfo addressInfo{
            VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, target.buffer};
        const SmsPush push{source->deviceAddress(),
                           vkGetBufferDeviceAddress(ctx.device, &addressInfo),
                           static_cast<std::uint32_t>(inputs.size()),
                           0};
        vkCmdPushConstants(*cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(*cmd, (push.count + 63U) / 64U, 1, 1);
        const VkMemoryBarrier2 memory{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                      .pNext = nullptr,
                                      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
                                      .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                      .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .pNext = nullptr,
                                          .dependencyFlags = 0,
                                          .memoryBarrierCount = 1,
                                          .pMemoryBarriers = &memory,
                                          .bufferMemoryBarrierCount = 0,
                                          .pBufferMemoryBarriers = nullptr,
                                          .imageMemoryBarrierCount = 0,
                                          .pImageMemoryBarriers = nullptr};
        vkCmdPipelineBarrier2(*cmd, &dependency);
        ASSERT_EQ(commandPool().endOneShot(*cmd), VK_SUCCESS);
        ASSERT_EQ(vmaInvalidateAllocation(ctx.allocator, target.allocation, 0, VK_WHOLE_SIZE), VK_SUCCESS);
        const auto* output = static_cast<const SmsOutput*>(mapped.pMappedData);
        results.assign(output, output + inputs.size());
    }
};

[[nodiscard]] sm::float3 rotate(sm::float3 p) {
    // Rigid rotation around X, with exact 3/5 and 4/5 sine/cosine.
    return {p.x, 0.8F * p.y - 0.6F * p.z, 0.6F * p.y + 0.8F * p.z};
}

void expectMirror(const SmsInput& input, const SmsOutput& output) {
    const sm::float3 x = xyz(input.receiver);
    const sm::float3 y = xyz(input.light);
    const sm::float3 origin = xyz(input.seed);
    const sm::float3 n = xyz(input.normal);
    // Reflect the emitter in the plane, then intersect receiver -> virtual emitter.
    const auto virtualLight = y - n * (2.0F * sm::dot(y - origin, n));
    const auto delta = virtualLight - x;
    const float t = sm::dot(origin - x, n) / sm::dot(delta, n);
    const auto expected = x + delta * t;
    const auto root = xyz(output.root);
    ASSERT_EQ(output.root.w, 1.0F);
    EXPECT_LE(sm::length(root - expected), 3.0e-4F);
    const auto wi = sm::normalize(x - root);
    const auto wo = sm::normalize(y - root);
    EXPECT_LE(sm::length((n * (2.0F * sm::dot(wi, n)) - wi) - wo), 3.0e-4F);
    EXPECT_LE(std::hypot(output.constraint.x, output.constraint.y), 2.0e-5F);
}

} // namespace

// Select with SmsFixture.Sms*; an all-skipped run is not acceptance.
TEST_F(SmsFixture, SmsMirrorMatchesVirtualLightFromMultipleSeedsAndRotations) {
    std::vector<SmsInput> inputs;
    const std::array seeds{
        sm::float3(0, 0, 0), sm::float3(0.8F, 0.2F, 0), sm::float3(1.2F, 0.5F, 0), sm::float3(-0.2F, -0.3F, 0)};
    for (bool rotated : {false, true}) {
        const auto transform = [rotated](sm::float3 p) { return rotated ? rotate(p) : p; };
        for (auto seed : seeds) {
            inputs.push_back({sm::float4(transform({-1, 0, 2}), 0),
                              sm::float4(transform({2, 0.5F, 1}), 0),
                              sm::float4(transform(seed), 0),
                              sm::float4(transform({0, 0, 1}), 0),
                              sm::float4(transform({1, 0, 0}), 0),
                              sm::float4(transform({0, 1, 0}), 0)});
        }
    }
    std::vector<SmsOutput> outputs;
    ASSERT_NO_FATAL_FAILURE(run(inputs, outputs));
    ASSERT_EQ(outputs.size(), inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        SCOPED_TRACE(i);
        expectMirror(inputs[i], outputs[i]);
    }
}

TEST_F(SmsFixture, SmsGrazingMirrorMatchesVirtualLight) {
    const std::array inputs{
        SmsInput{{-1, 0, 0.1F, 0}, {2, 0.5F, 0.05F, 0}, {0.9F, 0.3F, 0, 0}, {0, 0, 1, 0}, {1, 0, 0, 0}, {0, 1, 0, 0}}};
    std::vector<SmsOutput> outputs;
    ASSERT_NO_FATAL_FAILURE(run(inputs, outputs));
    ASSERT_EQ(outputs.size(), inputs.size());
    expectMirror(inputs[0], outputs[0]);
}

TEST_F(SmsFixture, SmsDegenerateGeometryFailsWithoutNonFinitePosition) {
    const std::array inputs{
        SmsInput{{0, 0, 0, 0}, {1, 1, 1, 0}, {0, 0, 0, 0}, {0, 0, 1, 0}, {1, 0, 0, 0}, {0, 1, 0, 0}},
        SmsInput{{-1, 0, 2, 0}, {2, 0.5F, 1, 0}, {0, 0, 0, 0}, {0, 0, 1, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}}};
    std::vector<SmsOutput> outputs;
    ASSERT_NO_FATAL_FAILURE(run(inputs, outputs));
    ASSERT_EQ(outputs.size(), inputs.size());
    for (const auto& output : outputs) {
        EXPECT_EQ(output.root.w, 0.0F);
        EXPECT_TRUE(std::isfinite(output.root.x) && std::isfinite(output.root.y) && std::isfinite(output.root.z));
    }
}
